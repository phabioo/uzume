#pragma once
// MSI update mode of the launcher (no Qt, header only, testable on any platform):
//   Uzume.exe --apply-msi-update [--sha256 <hex>] [--size <bytes>] [--msi-property NAME=VALUE]... <msi> <scope> <relaunchExe> [relaunchArgs...]
// --sha256/--size: what the Player verified (signed update index); the launcher checks them again right before msiexec
// (mandatory for elevated scopes) and keeps the file open read-only until msiexec ended, so it cannot be swapped.
// The Player starts a temporary copy of its launcher in this mode and quits; the copy installs the MSI and starts the
// Player again. --msi-property adds a property to the msiexec call (only the names of isAllowedMsiProperty, only for the
// scope share-hub: "Share with others" installs the server service and imports the data of "Play on this PC").
// The msiexec arguments mirror uzume::update::msiexecArguments (client/core/installroot.cpp).
#include <string>
#include <vector>

namespace uzume::launcher {

struct MsiUpdateRequest {
  std::wstring sha256;       // expected SHA-256 (hex), empty = not given
  unsigned long long size = 0;
  bool hasSize = false;
  std::wstring msi;
  std::wstring scope;  // user | machine | machine-hub | setup-hub | share-hub
  std::vector<std::wstring> properties;  // NAME=VALUE for share-hub
  std::wstring relaunchExe;
  std::vector<std::wstring> relaunchArgs;
};

// argv as CommandLineToArgvW delivers it (argv[0] = program). False when this is not an MSI update call or the
// call is incomplete.
inline bool parseMsiUpdate(const std::vector<std::wstring>& argv, MsiUpdateRequest* out) {
  if (argv.size() < 5 || argv[1] != L"--apply-msi-update") return false;
  size_t i = 2;
  while (i + 1 < argv.size() && (argv[i] == L"--sha256" || argv[i] == L"--size" || argv[i] == L"--msi-property")) {
    if (argv[i] == L"--sha256") {
      out->sha256 = argv[i + 1];
    } else if (argv[i] == L"--msi-property") {
      out->properties.push_back(argv[i + 1]);
    } else {
      if (argv[i + 1].empty() || argv[i + 1].size() > 18) return false;
      unsigned long long v = 0;
      for (wchar_t c : argv[i + 1]) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + static_cast<unsigned long long>(c - L'0');
      }
      out->size = v;
      out->hasSize = true;
    }
    i += 2;
  }
  if (argv.size() < i + 3) return false;
  out->msi = argv[i];
  out->scope = argv[i + 1];
  out->relaunchExe = argv[i + 2];
  out->relaunchArgs.assign(argv.begin() + static_cast<std::ptrdiff_t>(i) + 3, argv.end());
  return !out->msi.empty() && !out->relaunchExe.empty();
}

inline bool scopeNeedsElevation(const std::wstring& scope) { return scope != L"user"; }

inline bool isKnownScope(const std::wstring& scope) {
  return scope == L"user" || scope == L"machine" || scope == L"machine-hub" || scope == L"setup-hub" || scope == L"share-hub";
}

// The properties "Share with others" may pass to the MSI.
inline bool isAllowedMsiProperty(const std::wstring& nameValue) {
  const size_t eq = nameValue.find(L'=');
  if (eq == std::wstring::npos || eq + 1 >= nameValue.size()) return false;
  const std::wstring name = nameValue.substr(0, eq);
  if (name != L"HUB_PORT" && name != L"NETWORK_SHARING" && name != L"IMPORT_SERVER_DATA") return false;
  for (size_t k = eq + 1; k < nameValue.size(); ++k) {
    if (nameValue[k] == L'"') return false;  // the value is quoted below
  }
  return true;
}

// A 64-digit hex SHA-256 (any case).
inline bool isSha256Hex(const std::wstring& s) {
  if (s.size() != 64) return false;
  for (wchar_t c : s) {
    if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
  }
  return true;
}

// Case-insensitive compare of two hex digests.
inline bool sha256Equal(const std::wstring& a, const std::wstring& b) {
  if (a.size() != b.size() || a.empty()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    wchar_t x = a[i], y = b[i];
    if (x >= L'A' && x <= L'F') x = static_cast<wchar_t>(x - L'A' + L'a');
    if (y >= L'A' && y <= L'F') y = static_cast<wchar_t>(y - L'A' + L'a');
    if (x != y) return false;
  }
  return true;
}

// Elevated scopes must come with a valid SHA-256 (and size); the launcher refuses otherwise. A given hash is
// always checked, also for the per-user scope.
inline bool verificationRequired(const MsiUpdateRequest& r) { return scopeNeedsElevation(r.scope) || !r.sha256.empty() || r.hasSize; }
inline bool expectationComplete(const MsiUpdateRequest& r) { return isSha256Hex(r.sha256) && r.hasSize; }

// True if file size and digest are what the Player passed on.
inline bool msiMatches(const MsiUpdateRequest& r, unsigned long long actualSize, const std::wstring& actualSha256) {
  return expectationComplete(r) && r.size == actualSize && sha256Equal(r.sha256, actualSha256);
}

// "Set up a server on this PC" downloads the MSI into <temp>\uzume-server-setup-<uuid>;
// the launcher copy removes that folder after msiexec. Empty for every other call (a staged update MSI stays in the Player's cache).
inline std::wstring setupDirToRemove(const MsiUpdateRequest& r) {
  if (r.scope != L"setup-hub" && r.scope != L"share-hub") return {};
  const size_t slash = r.msi.find_last_of(L"\\/");
  if (slash == std::wstring::npos || slash == 0) return {};
  const std::wstring dir = r.msi.substr(0, slash);
  const size_t leafAt = dir.find_last_of(L"\\/");
  const std::wstring leaf = leafAt == std::wstring::npos ? dir : dir.substr(leafAt + 1);
  static const wchar_t kPrefix[] = L"uzume-server-setup-";
  const bool ours = leaf.compare(0, (sizeof(kPrefix) / sizeof(kPrefix[0])) - 1, kPrefix) == 0;
  return ours ? dir : std::wstring();
}

// Command line parameters for msiexec.exe (without the program name); empty for an unknown scope.
inline std::wstring msiexecParameters(const std::wstring& msi, const std::wstring& scope,
                                      const std::vector<std::wstring>& properties = {}) {
  if (!isKnownScope(scope)) return {};
  std::wstring path = msi;
  for (wchar_t& c : path) {
    if (c == L'/') c = L'\\';  // msiexec fails with 1619 on forward slashes
  }
  std::wstring p = L"/i \"" + path + L"\" /qn /norestart";
  if (scope == L"user") {
    p += L" MSIINSTALLPERUSER=1 ALLUSERS=2";
  } else {
    p += L" ALLUSERS=1";
    if (scope == L"machine-hub" || scope == L"setup-hub" || scope == L"share-hub") p += L" INSTALL_HUB=1";
    if (scope == L"setup-hub") p += L" NETWORK_SHARING=0";
    if (scope == L"share-hub") {
      for (const std::wstring& nv : properties) {
        if (!isAllowedMsiProperty(nv)) return {};  // refused as a whole
        const size_t eq = nv.find(L'=');
        p += L" " + nv.substr(0, eq + 1) + L"\"" + nv.substr(eq + 1) + L"\"";
      }
    }
  }
  return p;
}

// "a" "b c" ... (every argument quoted; the arguments of the Player contain no quotes).
inline std::wstring quoteArgs(const std::vector<std::wstring>& args) {
  std::wstring s;
  for (const std::wstring& a : args) {
    if (!s.empty()) s += L' ';
    s += L'"' + a + L'"';
  }
  return s;
}

// ERROR_INSTALL_ALREADY_RUNNING: another Windows installation holds the installer (e.g. an auto-update of another program).
constexpr unsigned long kMsiBusyCode = 1618;
constexpr unsigned long long kMsiBusyBudgetMs = 5ull * 60 * 1000;  // total wait for a busy installer
constexpr unsigned long kMsiBusyPollMs = 2000;                     // poll interval of the installer mutex
constexpr unsigned long kMsiRetryPauseMs = 5000;                   // pause before running msiexec again after 1618

// Run msiexec again? Only after 1618 and while the wait budget (counted from the first attempt) is not used up.
inline bool shouldRetryMsi(unsigned long exitCode, unsigned long long elapsedMs) {
  return exitCode == kMsiBusyCode && elapsedMs < kMsiBusyBudgetMs;
}

// Text of the error box for a failed msiexec run.
inline std::wstring msiFailureMessage(unsigned long code) {
  if (code == kMsiBusyCode) {
    return L"Another program was being installed, so the Uzume update could not be installed (code 1618). "
           L"Start the update again from Settings when that installation has finished.";
  }
  return L"The Uzume update could not be installed (code " + std::to_wstring(code) + L").";
}

}  // namespace uzume::launcher
