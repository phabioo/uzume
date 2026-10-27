#include <QtTest>
#include <string>
#include <vector>

#include "msiupdate.h"

using namespace uzume::launcher;

class MsiUpdateTest : public QObject {
  Q_OBJECT
 private slots:
  void parsesRequest() {
    MsiUpdateRequest r;
    const std::vector<std::wstring> argv{L"l.exe", L"--apply-msi-update", L"C:\\t\\fb.msi", L"user", L"C:\\p\\Uzume.exe",
                                         L"--setup-local-hub"};
    QVERIFY(parseMsiUpdate(argv, &r));
    QVERIFY(r.msi == L"C:\\t\\fb.msi");
    QVERIFY(r.scope == L"user");
    QVERIFY(r.relaunchExe == L"C:\\p\\Uzume.exe");
    QCOMPARE(r.relaunchArgs.size(), size_t(1));
    QVERIFY(r.relaunchArgs[0] == L"--setup-local-hub");
  }
  void rejectsOtherCalls() {
    MsiUpdateRequest r;
    QVERIFY(!parseMsiUpdate({L"l.exe", L"--smoke-test"}, &r));
    QVERIFY(!parseMsiUpdate({L"l.exe", L"--apply-msi-update", L"a.msi", L"user"}, &r));  // incomplete
    QVERIFY(!parseMsiUpdate({L"l.exe"}, &r));
  }
  void msiexecParametersPerScope() {
    QVERIFY(msiexecParameters(L"a b.msi", L"user") == L"/i \"a b.msi\" /qn /norestart MSIINSTALLPERUSER=1 ALLUSERS=2");
    QVERIFY(msiexecParameters(L"a.msi", L"machine") == L"/i \"a.msi\" /qn /norestart ALLUSERS=1");
    QVERIFY(msiexecParameters(L"a.msi", L"machine-hub") == L"/i \"a.msi\" /qn /norestart ALLUSERS=1 INSTALL_HUB=1");
    QVERIFY(msiexecParameters(L"a.msi", L"setup-hub") ==
            L"/i \"a.msi\" /qn /norestart ALLUSERS=1 INSTALL_HUB=1 NETWORK_SHARING=0");
    QVERIFY(msiexecParameters(L"a.msi", L"bogus").empty());
    QVERIFY(msiexecParameters(L"C:/u/a b/x.msi", L"machine") == L"/i \"C:\\u\\a b\\x.msi\" /qn /norestart ALLUSERS=1");
  }
  void shareHubScopePassesWhitelistedProperties() {
    MsiUpdateRequest r;
    QVERIFY(parseMsiUpdate({L"l.exe", L"--apply-msi-update", L"--msi-property", L"HUB_PORT=8443", L"--msi-property",
                            L"IMPORT_SERVER_DATA=C:\\d a\\server", L"C:\\t\\uzume-server-setup-1\\a.msi", L"share-hub", L"C:\\p\\Uzume.exe"},
                           &r));
    QCOMPARE(r.properties.size(), size_t(2));
    QVERIFY(msiexecParameters(r.msi, r.scope, r.properties) ==
            L"/i \"C:\\t\\uzume-server-setup-1\\a.msi\" /qn /norestart ALLUSERS=1 INSTALL_HUB=1 HUB_PORT=\"8443\" IMPORT_SERVER_DATA=\"C:\\d a\\server\"");
    QVERIFY(!setupDirToRemove(r).empty());
    QVERIFY(msiexecParameters(L"a.msi", L"share-hub", {L"EVIL=1"}).empty());
    QVERIFY(msiexecParameters(L"a.msi", L"share-hub", {L"HUB_PORT=1\" EVIL=1"}).empty());
    QVERIFY(msiexecParameters(L"a.msi", L"machine", {L"HUB_PORT=1"}) == L"/i \"a.msi\" /qn /norestart ALLUSERS=1");
  }
  void parsesVerificationOptions() {
    const std::wstring sha(64, L'a');
    MsiUpdateRequest r;
    QVERIFY(parseMsiUpdate({L"l.exe", L"--apply-msi-update", L"--sha256", sha, L"--size", L"123456", L"C:\\t\\fb.msi", L"machine",
                            L"C:\\p\\Uzume.exe", L"--x"},
                           &r));
    QVERIFY(r.sha256 == sha);
    QVERIFY(r.hasSize && r.size == 123456ULL);
    QVERIFY(r.msi == L"C:\\t\\fb.msi");
    QVERIFY(r.scope == L"machine");
    QVERIFY(r.relaunchExe == L"C:\\p\\Uzume.exe");
    QCOMPARE(r.relaunchArgs.size(), size_t(1));
    QVERIFY(!parseMsiUpdate({L"l.exe", L"--apply-msi-update", L"--size", L"12x", L"a.msi", L"user", L"p.exe"}, &r));
    QVERIFY(!parseMsiUpdate({L"l.exe", L"--apply-msi-update", L"--sha256", sha, L"a.msi", L"user"}, &r));  // incomplete
  }
  void reverificationDecision() {
    const std::wstring sha = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    MsiUpdateRequest r;
    r.msi = L"a.msi";
    r.scope = L"machine";
    // elevated scope without an expectation: the launcher must refuse
    QVERIFY(verificationRequired(r));
    QVERIFY(!msiMatches(r, 10, sha));
    r.sha256 = sha;
    r.size = 10;
    r.hasSize = true;
    QVERIFY(msiMatches(r, 10, sha));
    std::wstring upper = sha;
    for (wchar_t& c : upper) {
      if (c >= L'a' && c <= L'f') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    QVERIFY(msiMatches(r, 10, upper));                       // case-insensitive digest
    QVERIFY(!msiMatches(r, 11, sha));                        // size differs
    QVERIFY(!msiMatches(r, 10, std::wstring(64, L'0')));     // swapped file
    QVERIFY(!msiMatches(r, 10, L""));                        // unreadable
    r.sha256 = L"short";
    QVERIFY(!msiMatches(r, 10, L"short"));                   // malformed expectation never matches
    // per-user scope without any expectation is not verified (old callers); a given hash always is
    MsiUpdateRequest u;
    u.scope = L"user";
    QVERIFY(!verificationRequired(u));
    u.sha256 = sha;
    QVERIFY(verificationRequired(u));
  }
  void setupFolderCleanup() {
    MsiUpdateRequest r;
    r.scope = L"setup-hub";
    r.msi = L"C:\\Users\\a\\AppData\\Local\\Temp\\uzume-server-setup-0123abcd\\Uzume.msi";
    QVERIFY(setupDirToRemove(r) == L"C:\\Users\\a\\AppData\\Local\\Temp\\uzume-server-setup-0123abcd");
    r.scope = L"machine";  // a staged update MSI stays in the Player's cache
    QVERIFY(setupDirToRemove(r).empty());
    r.scope = L"setup-hub";
    r.msi = L"C:\\Users\\a\\Downloads\\Uzume.msi";  // not our folder: never removed
    QVERIFY(setupDirToRemove(r).empty());
  }
  void elevation() {
    QVERIFY(!scopeNeedsElevation(L"user"));
    QVERIFY(scopeNeedsElevation(L"machine"));
    QVERIFY(scopeNeedsElevation(L"setup-hub"));
  }
  void quoting() { QVERIFY(quoteArgs({L"C:\\a b\\p.exe", L"--x"}) == L"\"C:\\a b\\p.exe\" \"--x\""); }
  void retryOnlyOnBusyWithinBudget() {
    QVERIFY(shouldRetryMsi(1618, 0));
    QVERIFY(shouldRetryMsi(1618, kMsiBusyBudgetMs - 1));
    QVERIFY(!shouldRetryMsi(1618, kMsiBusyBudgetMs));
    QVERIFY(!shouldRetryMsi(0, 0));
    QVERIFY(!shouldRetryMsi(1603, 0));
    QVERIFY(!shouldRetryMsi(3010, 0));
  }
  void failureMessage() {
    QVERIFY(msiFailureMessage(1618) ==
            L"Another program was being installed, so the Uzume update could not be installed (code 1618). "
            L"Start the update again from Settings when that installation has finished.");
    QVERIFY(msiFailureMessage(1603) == L"The Uzume update could not be installed (code 1603).");
  }
};

QTEST_GUILESS_MAIN(MsiUpdateTest)
#include "msiupdate_test.moc"
