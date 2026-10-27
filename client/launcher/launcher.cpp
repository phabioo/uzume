// Uzume launcher (Windows): {app}\Uzume.exe starts {app}\bin\Uzume.exe with the
// same arguments, passes its std handles on, waits and returns the exit code of the child.
// Deliberately tiny: no Qt, no C++ runtime features beyond the static CRT.
#include <windows.h>
#include <shellapi.h>  // not part of windows.h under WIN32_LEAN_AND_MEAN
#include <shobjidl.h>
#include <bcrypt.h>

#include <cwchar>
#include <string>
#include <vector>

#include "cmdline.h"
#include "msiupdate.h"

namespace {

// SHA-256 of the whole file behind `file` (Windows CNG, no extra dependency); lowercase hex, empty on any failure.
std::wstring sha256HexOfFile(HANDLE file) {
  std::wstring hex;
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return hex;
  DWORD objLen = 0, got = 0;
  if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &got, 0) >= 0) {
    std::vector<UCHAR> obj(objLen);
    LARGE_INTEGER zero = {};
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) >= 0 && SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
      std::vector<UCHAR> buf(1 << 20);
      bool ok = true;
      for (;;) {
        DWORD n = 0;
        if (!ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
          ok = false;
          break;
        }
        if (n == 0) break;
        if (BCryptHashData(hash, buf.data(), n, 0) < 0) {
          ok = false;
          break;
        }
      }
      UCHAR digest[32] = {};
      if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
        static const wchar_t* kHex = L"0123456789abcdef";
        for (UCHAR b : digest) {
          hex += kHex[b >> 4];
          hex += kHex[b & 15];
        }
      }
    }
  }
  if (hash != nullptr) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

// PC-3: re-verifies the MSI the Player verified earlier (the file lies in a folder the user can write to, and an
// elevated msiexec follows). Opens it read-only with FILE_SHARE_READ only, so nobody can write, rename or delete it
// while we hold the handle (kept in *lock until msiexec ended). False = do not install.
bool verifyAndLockMsi(const uzume::launcher::MsiUpdateRequest& req, HANDLE* lock) {
  using namespace uzume::launcher;
  if (!expectationComplete(req)) return false;
  HANDLE h = CreateFileW(req.msi.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz = {};
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
    CloseHandle(h);
    return false;
  }
  const std::wstring sha = sha256HexOfFile(h);
  if (!msiMatches(req, static_cast<unsigned long long>(sz.QuadPart), sha)) {
    CloseHandle(h);
    return false;
  }
  *lock = h;
  return true;
}

// Waits while another Windows installation holds the installer (execute mutex), polling every 2 s, until the
// budget counted from `startedMs` (GetTickCount64) is used up; msiexec then reports 1618 itself.
// A non-elevated open can fail with ERROR_ACCESS_DENIED while the mutex exists: that counts as busy.
void waitWhileInstallerBusy(ULONGLONG startedMs) {
  using namespace uzume::launcher;
  while (GetTickCount64() - startedMs < kMsiBusyBudgetMs) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\_MSIExecute");
    if (m == nullptr) {
      if (GetLastError() != ERROR_ACCESS_DENIED) return;
    } else {
      CloseHandle(m);
    }
    Sleep(kMsiBusyPollMs);
  }
}

// --apply-msi-update (temporary copy of the launcher, see msiupdate.h): wait until no Player instance runs, run
// msiexec (elevated unless per-user), start the Player again, delete this copy.
int applyMsiUpdate(const uzume::launcher::MsiUpdateRequest& req) {
  using namespace uzume::launcher;
  // Every Player instance holds the named mutex (app/main.cpp); max. 60 s.
  bool stillRunning = false;
  for (int i = 0; i < 120; ++i) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"UzumeApp");
    if (m == nullptr) break;
    CloseHandle(m);
    stillRunning = i == 119;
    Sleep(500);
  }
  // A Player that is still running after the wait holds files msiexec needs (error 1603 / reboot): do not start it (PE-17).
  DWORD code = 1;
  bool refused = false;
  HANDLE lock = INVALID_HANDLE_VALUE;
  if (!stillRunning && verificationRequired(req) && !verifyAndLockMsi(req, &lock)) {
    refused = true;  // size/SHA-256 differ from what the Player verified, or were not passed: install nothing
  }
  const std::wstring params = (refused || stillRunning) ? std::wstring() : msiexecParameters(req.msi, req.scope, req.properties);
  wchar_t sys[MAX_PATH] = {};
  if (!params.empty() && GetSystemDirectoryW(sys, MAX_PATH) > 0) {
    const std::wstring msiexec = std::wstring(sys) + L"\\msiexec.exe";
    const ULONGLONG started = GetTickCount64();
    for (;;) {
      waitWhileInstallerBusy(started);
      code = 1;
      HANDLE proc = nullptr;
      if (scopeNeedsElevation(req.scope)) {
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = msiexec.c_str();
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei)) proc = sei.hProcess;
      } else {
        std::wstring cmd = L"\"" + msiexec + L"\" " + params;
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
          CloseHandle(pi.hThread);
          proc = pi.hProcess;
        }
      }
      if (proc != nullptr) {
        WaitForSingleObject(proc, INFINITE);
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
      }
      // Another installation was running (1618): wait and try again within the budget. The MSI stays locked.
      if (!shouldRetryMsi(code, GetTickCount64() - started)) break;
      Sleep(kMsiRetryPauseMs);
    }
  }
  if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
  if (stillRunning) {
    MessageBoxW(nullptr, L"Uzume is still running, so the update was not installed. Close the Player and start the update again.",
                L"Uzume", MB_OK | MB_ICONWARNING);
  } else if (refused) {
    MessageBoxW(nullptr, L"The installer file does not match the verified update (it was changed or could not be checked). Nothing was installed.",
                L"Uzume", MB_OK | MB_ICONERROR);
  } else if (code != 0 && code != 3010) {  // 3010 = success, restart required
    MessageBoxW(nullptr, msiFailureMessage(code).c_str(), L"Uzume", MB_OK | MB_ICONERROR);
  }
  // Start the Player again (the installed one after success, otherwise the previous one).
  std::vector<std::wstring> relaunch;
  relaunch.push_back(req.relaunchExe);
  relaunch.insert(relaunch.end(), req.relaunchArgs.begin(), req.relaunchArgs.end());
  std::wstring cmd = quoteArgs(relaunch);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (CreateProcessW(req.relaunchExe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  // Delete this temporary copy and its folder a moment after exit.
  wchar_t self[MAX_PATH * 4] = {};
  const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
  if (n > 0 && n < sizeof(self) / sizeof(self[0])) {
    std::wstring dir(self, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    std::wstring clean = L"cmd.exe /c ping -n 4 127.0.0.1 >nul & del /f /q \"" + std::wstring(self, n) + L"\" & rmdir \"" + dir + L"\"";
    const std::wstring setupDir = setupDirToRemove(req);  // downloaded setup MSI of "Set up a server on this PC"
    if (!setupDir.empty()) clean += L" & rd /s /q \"" + setupDir + L"\"";
    STARTUPINFOW si2 = {};
    si2.cb = sizeof(si2);
    PROCESS_INFORMATION pi2 = {};
    if (CreateProcessW(nullptr, &clean[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2)) {
      CloseHandle(pi2.hThread);
      CloseHandle(pi2.hProcess);
    }
  }
  return static_cast<int>(code == 3010 ? 0 : code);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetCurrentProcessExplicitAppUserModelID(L"Uzume.App");

  {
    int argc = 0;
    LPWSTR* argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvw != nullptr) {
      uzume::launcher::MsiUpdateRequest req;
      const bool msi = uzume::launcher::parseMsiUpdate(std::vector<std::wstring>(argvw, argvw + argc), &req);
      LocalFree(argvw);
      if (msi) return applyMsiUpdate(req);
    }
  }

  wchar_t self[MAX_PATH * 4] = {};
  const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
  if (n == 0 || n >= sizeof(self) / sizeof(self[0])) return 1;
  std::wstring dir(self, n);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return 1;
  dir.resize(slash);

  // The real app is bin\Uzume.exe, independent of this launcher's own file name.
  const std::wstring child = dir + L"\\bin\\Uzume.exe";
  std::wstring cmd = uzume::launcher::childCommandLine(child, GetCommandLineW());

  // GUI subsystem (no console window, like the Player itself): hand our std handles to the child explicitly,
  // so a redirected stdout (e.g. `--version-json` captured by CI) reaches the caller. Only valid handles.
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  const auto usable = [](HANDLE h) { return h != nullptr && h != INVALID_HANDLE_VALUE; };
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (!usable(out) && !usable(err) && AttachConsole(ATTACH_PARENT_PROCESS)) {
    // Started from a terminal without redirection: borrow its console (no new window) for the child's output.
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
    err = out;
  }
  if (usable(in) || usable(out) || usable(err)) {
    for (HANDLE h : {in, out, err}) {
      if (usable(h)) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = usable(in) ? in : nullptr;
    si.hStdOutput = usable(out) ? out : nullptr;
    si.hStdError = usable(err) ? err : nullptr;
  }
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(child.c_str(), &cmd[0], nullptr, nullptr, TRUE /* inherit handles */, 0, nullptr,
                      nullptr /* keep the caller's working directory */, &si, &pi)) {
    const DWORD code = GetLastError();
    wchar_t msg[512];
    _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"Uzume could not be started (%lu):\n%ls", code, child.c_str());
    MessageBoxW(nullptr, msg, L"Uzume", MB_OK | MB_ICONERROR);
    return 1;
  }
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}
