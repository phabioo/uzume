// 0.9 "One installer": MSI update helpers, artifact choice, portable data migration, local Hub detection.
// Dummy files only.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "installroot.h"
#include "hubsetup.h"
#include "localhub.h"
#include "profilestore.h"
#include "updateindex.h"

using namespace uzume;
using namespace uzume::update;

namespace {
bool put(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  f.write(data);
  return true;
}
QByteArray get(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QJsonObject art(const QString& kind, const QString& name) {
  return {{"platform", "windows-x64"}, {"kind", kind}, {"name", name}, {"size", 10},
          {"sha256", QString(64, QLatin1Char('a'))}, {"url", "https://example.invalid/" + name}};
}
QByteArray indexWith(const QJsonArray& artifacts, const QString& version = QStringLiteral("0.9.1")) {
  const QJsonObject rel{{"product", "player"}, {"channel", "stable"}, {"version", version},
                        {"commit", "abcdef1"}, {"published_at", "2026-01-01T00:00:00Z"},
                        {"notes_url", "https://example.invalid/n"}, {"protocol_version", 1},
                        {"min_protocol_version", 1}, {"artifacts", artifacts}};
  return QJsonDocument(QJsonObject{{"schema", 1}, {"generated_at", "2026-01-01T00:00:00Z"}, {"releases", QJsonArray{rel}}}).toJson();
}
}  // namespace

class LocalHubTest : public QObject {
  Q_OBJECT
 private slots:
  void msiexecArgumentsPerScope() {
    const QString m = QStringLiteral("C:\\t\\fb.msi");  // msiexec gets backslashes (forward slashes are converted)
    QCOMPARE(msiexecArguments(m, MsiScope::User),
             (QStringList{"/i", m, "/qn", "/norestart", "MSIINSTALLPERUSER=1", "ALLUSERS=2"}));
    QCOMPARE(msiexecArguments(m, MsiScope::Machine), (QStringList{"/i", m, "/qn", "/norestart", "ALLUSERS=1"}));
    QCOMPARE(msiexecArguments(m, MsiScope::MachineWithHub),
             (QStringList{"/i", m, "/qn", "/norestart", "ALLUSERS=1", "INSTALL_HUB=1"}));
    QCOMPARE(msiexecArguments(m, MsiScope::SetupHub),
             (QStringList{"/i", m, "/qn", "/norestart", "ALLUSERS=1", "INSTALL_HUB=1", "NETWORK_SHARING=0"}));
  }
  void scopeDetection() {
    QCOMPARE(detectMsiScope(true, false), MsiScope::User);
    QCOMPARE(detectMsiScope(false, false), MsiScope::Machine);
    QCOMPARE(detectMsiScope(false, true), MsiScope::MachineWithHub);
    QVERIFY(!msiScopeNeedsElevation(MsiScope::User));
    QVERIFY(msiScopeNeedsElevation(MsiScope::SetupHub));
    QCOMPARE(msiScopeName(MsiScope::MachineWithHub), QStringLiteral("machine-hub"));
  }
  void markersAndSelfUpdate() {
    QTemporaryDir t;
    const QString root = t.filePath("Uzume/App");
    QVERIFY(QDir().mkpath(root));
    QVERIFY(!canSelfUpdate(root));
    QVERIFY(put(root + "/unins000.exe", "x"));
    QVERIFY(!canSelfUpdate(root));  // only the MSI marker counts
    QVERIFY(QFile::remove(root + "/unins000.exe"));
    QVERIFY(put(root + "/uzume-app.msi-installed", ""));
    QVERIFY(hasMsiMarker(root));
    QVERIFY(canSelfUpdate(root));
    QVERIFY(!hubInstalledNear(root));
    QVERIFY(put(t.filePath("Uzume/Server/uzume-server.msi-installed"), ""));
    QVERIFY(hubInstalledNear(root));
    QVERIFY(isMsiFile("a/B.MSI"));
    QVERIFY(!isMsiFile("a/setup.exe"));
  }
  void launcherCopyAndRelaunchCommand() {
    QTemporaryDir t;
    QString why;
    QVERIFY(copyLauncherToTemp(t.filePath("none"), t.path(), &why).isEmpty());
    QVERIFY(!why.isEmpty());
    QVERIFY(put(t.filePath("root/Uzume.exe"), "launcher"));
    const QString copy = copyLauncherToTemp(t.filePath("root"), t.filePath("tmp"));
    QVERIFY(!copy.isEmpty());
    QCOMPARE(get(copy), QByteArray("launcher"));
    QVERIFY(!copy.startsWith(t.filePath("root")));
    const InstallerCommand c = msiRelaunchCommand(copy, "x.msi", MsiScope::SetupHub, "P/Uzume.exe", {"--setup-local-hub"});
    QCOMPARE(c.program, copy);
    QCOMPARE(c.args, (QStringList{"--apply-msi-update", "x.msi", "setup-hub", "P\\Uzume.exe", "--setup-local-hub"}));
    // size + SHA-256 for the launcher's re-check right before msiexec (PC-3)
    const QString sha(64, QLatin1Char('a'));
    const InstallerCommand v = msiRelaunchCommand(copy, "x.msi", MsiScope::Machine, "P/Uzume.exe", {}, sha, 1234);
    QCOMPARE(v.args, (QStringList{"--apply-msi-update", "--sha256", sha, "--size", "1234", "x.msi", "machine", "P\\Uzume.exe"}));
    // forward slashes would make msiexec fail with 1619
    const InstallerCommand w = msiRelaunchCommand(copy, "C:/Users/a/data/x.msi", MsiScope::User, "C:/Users/a/P/Uzume.exe");
    QCOMPARE(w.args[1], QStringLiteral("C:\\Users\\a\\data\\x.msi"));
    QCOMPARE(w.args[3], QStringLiteral("C:\\Users\\a\\P\\Uzume.exe"));
    QCOMPARE(msiexecArguments("C:/a/x.msi", MsiScope::Machine)[1], QStringLiteral("C:\\a\\x.msi"));
    QVERIFY(perMachinePlayerExe("C:/Program Files").endsWith("Uzume/App/Uzume.exe"));
  }
  void msiSelectionAndTolerantParsing() {
    // An index that still lists a legacy "installer" artifact next to the MSI parses; only the MSI is selected.
    const auto both = parseIndex(indexWith({art("installer", "setup.exe"), art("msi", "fb.msi")}));
    QVERIFY2(both.ok, qPrintable(both.error));
    SelectionInput in;
    in.platform = QStringLiteral("windows-x64");  // the Hub setup runs the Windows MSI whatever the host
    in.kind = QStringLiteral("msi");
    in.channel = Channel::Stable;
    in.currentVersion = "0.9.0";
    QCOMPARE(selectRelease(both.index, in).artifact.kind, QStringLiteral("msi"));
    const auto old = parseIndex(indexWith({art("installer", "setup.exe")}));
    QVERIFY(old.ok);
    QVERIFY(selectRelease(old.index, in).status != Selection::Status::Available);  // no MSI: nothing to install
    const auto msiOnly = parseIndex(indexWith({art("msi", "fb.msi")}));
    QCOMPARE(selectRelease(msiOnly.index, in).status, Selection::Status::Available);
  }
  void ownMsiLookup() {
    const auto pr = parseIndex(indexWith({art("installer", "setup.exe"), art("msi", "fb.msi")}, "0.9.1"));
    QVERIFY(pr.ok);
    QCOMPARE(findOwnMsi(pr.index, "0.9.1")->name, QStringLiteral("fb.msi"));
    QVERIFY(!findOwnMsi(pr.index, "0.9.2").has_value());  // only the Player's own version
    const auto noMsi = parseIndex(indexWith({art("installer", "setup.exe")}, "0.9.1"));
    QVERIFY(!findOwnMsi(noMsi.index, "0.9.1").has_value());
  }
  void detectionViaEnvironment() {
    qunsetenv("UZUME_LOCAL_SERVER_PORT");
    qunsetenv("UZUME_LOCAL_SERVER_DIR");
    QVERIFY(!localhub::detect().present());
    qputenv("UZUME_LOCAL_SERVER_PORT", "8555");
    qputenv("UZUME_LOCAL_SERVER_DIR", "/opt/hub");
    const auto i = localhub::detect();
    QVERIFY(i.present());
    QCOMPARE(i.port, 8555);
    QCOMPARE(i.installDir, QStringLiteral("/opt/hub"));
    qputenv("UZUME_LOCAL_SERVER_PORT", "banana");
    QVERIFY(!localhub::detect().present());
    qunsetenv("UZUME_LOCAL_SERVER_PORT");
    qunsetenv("UZUME_LOCAL_SERVER_DIR");
  }
  void portAndAddressRules() {
    QVERIFY(!localhub::parsePort("0").has_value());
    QVERIFY(!localhub::parsePort("70000").has_value());
    QCOMPARE(localhub::parsePort(" 8443 ").value_or(0), 8443);
    QCOMPARE(localhub::hubUrl(8443), QStringLiteral("https://127.0.0.1:8443"));
    QVERIFY(localhub::isLocalHubAddress("https://127.0.0.1:8443", 8443));
    QVERIFY(!localhub::isLocalHubAddress("https://127.0.0.1:8444", 8443));
    QVERIFY(!localhub::isLocalHubAddress("https://localhost:8443", 8443));
    QVERIFY(!localhub::isLocalHubAddress("https://192.168.1.5:8443", 8443));
    QVERIFY(!localhub::isLocalHubAddress("http://127.0.0.1:8443", 8443));
    QVERIFY(!localhub::isLocalHubAddress("https://127.0.0.1:8443", 0));
    const auto c = localhub::grantFolderCommand("C:/Hub", "D:/ROMs");
    QCOMPARE(c.args, (QStringList{"grant-folder", "D:/ROMs"}));
    QVERIFY(c.program.endsWith("uzume-server.exe"));
  }

  void staleSetupFoldersAreRemovedButFreshOnesStay() {
    QTemporaryDir t;
    QVERIFY(put(t.filePath("uzume-server-setup-a/Uzume.msi"), "dummy"));
    QVERIFY(put(t.filePath("uzume-server-setup-b/Uzume.msi"), "dummy"));
    QVERIFY(put(t.filePath("other-folder/keep.txt"), "dummy"));
    QCOMPARE(HubSetupInstaller::cleanStaleTempDirs(t.path(), 24 * 3600), 0);  // just created: not older than a day
    QVERIFY(QFileInfo::exists(t.filePath("uzume-server-setup-a/Uzume.msi")));
    QCOMPARE(HubSetupInstaller::cleanStaleTempDirs(t.path(), -60), 2);  // everything counts as stale
    QVERIFY(!QFileInfo::exists(t.filePath("uzume-server-setup-a")));
    QVERIFY(!QFileInfo::exists(t.filePath("uzume-server-setup-b")));
    QVERIFY(QFileInfo::exists(t.filePath("other-folder/keep.txt")));  // only our own folders
  }
};

QTEST_GUILESS_MAIN(LocalHubTest)
#include "localhub_test.moc"
