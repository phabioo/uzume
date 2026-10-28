// 0.18 package 10 "On this PC and sharing": LocalHubController in child mode ("Play on this PC": the server runs as a child
// process of the app, a hidden admin), "Share with others" (prepare the server, pending record, MSI properties, the next
// start finishes or rolls back), "Stop sharing" and "Share again" with a fake elevated runner. The child server is a shell
// script (UZUME_LOCAL_SERVER_EXE) that reports the port of a FakeHub; no QML window. Skipped on Windows.
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "fakehub.h"
#include "localhub.h"
#include "playercontroller.h"
#include "playersettings.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using S = HubConnection::State;

namespace {
const char* kScript = R"SH(#!/bin/sh
while [ $# -gt 0 ]; do
  case "$1" in
    -data-dir) DATA="$2"; shift 2;;
    *) shift;;
  esac
done
PORT=$(cat "$DATA/fake_port")
echo "$PORT" >> "$DATA/starts"
echo "UZUME_READY $PORT"
cat >/dev/null
echo stopped >> "$DATA/stops"
)SH";

// Memory store with switchable failures (locked or dismissed Secret Service).
class FaultyStore final : public MemoryCredentialStore {
 public:
  bool failWrites = false;
  bool failReads = false;
  bool write(const QString& t, const QString& s) override { return !failWrites && MemoryCredentialStore::write(t, s); }
  std::optional<QString> read(const QString& t) const override { return failReads ? std::nullopt : MemoryCredentialStore::read(t); }
  CredentialRead readChecked(const QString& t) const override {
    if (failReads) return {CredentialRead::Status::Error, {}};
    return MemoryCredentialStore::readChecked(t);
  }
};

int freePort() {
  QTcpServer s;
  return s.listen(QHostAddress::Any, 0) ? s.serverPort() : 0;
}
int portOf(const FakeHub& h) { return QUrl(h.address()).port(); }
int countLines(const QString& file) {
  QFile f(file);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts).size() : 0;
}
}  // namespace

class LocalServerUiTest : public QObject {
  Q_OBJECT

  QTemporaryDir tools_;
  QString exe_;

  // One app data directory with its (memory) credential store, across restarts of the controller.
  struct Rig {
    QTemporaryDir dir;
    std::shared_ptr<CredentialStore> creds = std::make_shared<MemoryCredentialStore>();
    std::unique_ptr<PlayerController> pc;
    QString serverDir() const { return dir.filePath(QStringLiteral("server")); }
    void writePort(int port, const QString& d = QString()) const {
      const QString where = d.isEmpty() ? serverDir() : d;
      QDir().mkpath(where);
      QFile f(where + QStringLiteral("/fake_port"));
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(QByteArray::number(port));
    }
    void open() {
      PlayerController::Options o;
      o.dataDir = dir.path();
      o.credentialStore = creds;
      o.probeCoreVersions = false;
      o.enableGamepads = false;
      o.allowHttp = true;
      pc = std::make_unique<PlayerController>(o);
      pc->connection()->setPollIntervalMs(80);
      pc->localHub()->setShareSupportedForTests(true);
    }
    void close() { pc.reset(); }
    LocalHubController* lh() { return pc->localHub(); }
    PlayerSettings settings() const { return PlayerSettings(dir.path()); }  // read back what was saved
  };

  static void waitConnected(Rig& r) { QTRY_COMPARE_WITH_TIMEOUT(r.pc->connection()->state(), S::Connected, 15000); }

 private slots:
  void initTestCase() {
#ifdef Q_OS_WIN
    QSKIP("the fake server is a shell script");
#endif
    uitest::installWarningCounter();
    QVERIFY(tools_.isValid());
    exe_ = tools_.filePath(QStringLiteral("fake-uzume-server.sh"));
    QFile f(exe_);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(kScript);
    f.close();
    f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
  }
  void init() {
    qputenv("UZUME_LOCAL_SERVER_EXE", exe_.toUtf8());
    qunsetenv("UZUME_LOCAL_SERVER_PORT");
    qunsetenv("UZUME_LOCAL_SERVER_DIR");
    qunsetenv("UZUME_LOCAL_SERVICE_DATA_DIR");
  }
  void cleanup() {
    qunsetenv("UZUME_LOCAL_SERVER_EXE");
    qunsetenv("UZUME_LOCAL_SERVER_PORT");
    qunsetenv("UZUME_LOCAL_SERVER_DIR");
    qunsetenv("UZUME_LOCAL_SERVICE_DATA_DIR");
  }

  void usernameSanitizingFollowsTheServerRule() {
    const QRegularExpression rule(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
    for (const QString& in : {QString(), QStringLiteral("Max Muster"), QStringLiteral("Jörg"), QStringLiteral("日本語"), QStringLiteral("a/b\\c:d"),
                              QString(100, QLatin1Char('x')), QStringLiteral("DOM\\user")}) {
      const QString out = LocalHubController::sanitizeUsername(in);
      QVERIFY2(rule.match(out).hasMatch(), qPrintable(in + " -> " + out));
    }
    QCOMPARE(LocalHubController::sanitizeUsername(QStringLiteral("Max Muster")), QStringLiteral("Max_Muster"));
    QCOMPARE(LocalHubController::sanitizeUsername(QStringLiteral("日本語")), QStringLiteral("player"));
  }

  // ------------------------------------------------------------ Play on this PC

  void playOnThisPcStartsTheChildAndPairsWithTheHiddenAdmin() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Rig r;
    r.writePort(portOf(hub));
    r.open();
    auto* lh = r.lh();
    QVERIFY(!lh->onThisPcOnly());
    QCOMPARE(lh->mode(), QString());
    lh->startChild();
    waitConnected(r);
    QTRY_COMPARE_WITH_TIMEOUT(lh->phase(), QStringLiteral("idle"), 8000);
    QCOMPARE(lh->childState(), QStringLiteral("running"));
    QCOMPARE(hub.localSetupCalls, 1);
    QCOMPARE(hub.lastLocalBody.value("password").toString().size(), 32);  // random, never shown
    QVERIFY(!hub.lastLocalBody.value("username").toString().isEmpty());
    QCOMPARE(lh->mode(), QStringLiteral("child"));
    QVERIFY(lh->onThisPcOnly());
    QCOMPARE(lh->autoConfirmPassword(), hub.lastLocalBody.value("password").toString());
    // The certificate was trusted for exactly the loopback address and port of the child.
    const auto p = r.pc->profileStore()->profile(hub.hubId);
    QVERIFY(p.has_value());
    QVERIFY(!p->pinnedFingerprint.isEmpty());
    QVERIFY(localhub::isLocalHubAddress(p->address, portOf(hub)));
    const PlayerSettings saved = r.settings();
    QCOMPARE(saved.localServerMode(), QStringLiteral("child"));
    QCOMPARE(saved.localServerHubId(), hub.hubId);
    QCOMPARE(saved.localServerPort(), portOf(hub));
    // The password is in the credential store only: not in any file below the data directory.
    QDirIterator it(r.dir.path(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      QFile f(it.next());
      if (f.open(QIODevice::ReadOnly)) QVERIFY2(!f.readAll().contains(hub.lastLocalBody.value("password").toString().toUtf8()), qPrintable(f.fileName()));
    }
    // Leaving the connection: no longer "on this PC only".
    r.pc->connection()->disconnectFromHub();
    QVERIFY(!lh->onThisPcOnly());
    r.close();
    QCOMPARE(countLines(r.serverDir() + "/stops"), 1);  // quitting closed the child's stdin
  }

  void anUnstoredAdminPasswordAbortsTheSetup() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Rig r;
    const auto faulty = std::make_shared<FaultyStore>();
    faulty->failWrites = true;
    r.creds = faulty;
    r.writePort(portOf(hub));
    r.open();
    r.lh()->startChild();
    QTRY_COMPARE_WITH_TIMEOUT(r.lh()->phase(), QStringLiteral("error"), 8000);
    QCOMPARE(hub.localSetupCalls, 0);  // the server never got a password nobody could read back
    QVERIFY(!r.lh()->error().isEmpty());
  }

  void anUnreadableCredentialStoreIsNotTreatedAsAbsent() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Rig r;
    const auto faulty = std::make_shared<FaultyStore>();
    r.creds = faulty;
    r.writePort(portOf(hub));
    r.open();
    r.lh()->startChild();
    waitConnected(r);
    r.close();
    // Restart with a credential store that does not answer: no pairing, no new admin, the reference stays.
    faulty->failReads = true;
    r.open();
    r.pc->startup();
    QTRY_COMPARE_WITH_TIMEOUT(r.pc->connection()->state(), S::Unreachable, 15000);
    QCOMPARE(r.pc->connection()->errorCode(), QStringLiteral("credential_store_unavailable"));
    QCOMPARE(hub.localSetupCalls, 1);
    QCOMPARE(hub.localPairCalls, 0);
    const auto p = r.pc->profileStore()->profile(hub.hubId);
    QVERIFY(p.has_value());
    QVERIFY(!p->credentialRef.isEmpty());
  }

  void anExistingAdminIsPairedWithTheStoredPassword() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Rig r;
    r.writePort(portOf(hub));
    r.open();
    r.lh()->startChild();
    waitConnected(r);
    QString password = hub.lastLocalBody.value("password").toString();
    r.close();

    // App restart: the stored device is reused, nothing is set up or paired again.
    r.open();
    QCOMPARE(r.lh()->mode(), QStringLiteral("child"));
    r.pc->startup();
    waitConnected(r);
    QTRY_VERIFY_WITH_TIMEOUT(r.lh()->onThisPcOnly(), 8000);
    QCOMPARE(hub.localSetupCalls, 1);
    QCOMPARE(hub.localPairCalls, 0);
    QCOMPARE(countLines(r.serverDir() + "/starts"), 2);
    QCOMPARE(r.lh()->autoConfirmPassword(), password);
    r.close();

    // The device credential is gone: the stored hidden password pairs again.
    r.open();
    const auto p = r.pc->profileStore()->profile(hub.hubId);
    QVERIFY(p.has_value());
    QVERIFY(r.creds->remove(p->credentialRef));
    r.pc->startup();
    waitConnected(r);
    QTRY_COMPARE_WITH_TIMEOUT(hub.localPairCalls, 1, 8000);
    QCOMPARE(hub.localSetupCalls, 1);
    QCOMPARE(hub.lastLocalBody.value("password").toString(), password);
  }

  void autoConfirmAnswersConfirmItsYouWithTheHiddenPassword() {
    FakeHub hub(QStringLiteral("a"));
    hub.adminUser = true;
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/users/u_2/disable")};
    QVERIFY(hub.start());
    Rig r;
    r.writePort(portOf(hub));
    r.open();
    r.lh()->startChild();
    waitConnected(r);
    QTRY_VERIFY_WITH_TIMEOUT(r.lh()->onThisPcOnly(), 8000);
    hub.accountPassword = hub.lastLocalBody.value("password").toString();  // the server knows the hidden password
    auto* admin = r.pc->admin();
    QSignalSpy replied(admin, &AdminController::replied);
    admin->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"));
    QTRY_COMPARE_WITH_TIMEOUT(replied.count(), 1, 8000);
    QCOMPARE(replied.at(0).at(1).toBool(), true);  // ok after the automatic confirmation
    QCOMPARE(hub.confirmPosts, 1);
    QVERIFY(!admin->confirmNeeded());
    QVERIFY(!admin->confirmAutomatic());
  }

  // ------------------------------------------------------------ Share with others

  void beginShareValidatesBeforeTouchingAnything() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Rig r;
    r.open();
    auto* lh = r.lh();
    lh->beginShare("Home", "Max", "long-enough-password", "home", 8443);
    QCOMPARE(lh->sharePhase(), QStringLiteral("failed"));
    QCOMPARE(lh->shareErrorCode(), QStringLiteral("not_connected"));
    r.writePort(portOf(hub));
    lh->startChild();
    waitConnected(r);
    QTRY_VERIFY_WITH_TIMEOUT(lh->onThisPcOnly(), 8000);
    lh->beginShare("Home", "Max", "short", "home", 8443);
    QCOMPARE(lh->shareErrorCode(), QStringLiteral("password_too_short"));
    lh->beginShare("", "Max", "long-enough-password", "home", 8443);
    QCOMPARE(lh->shareErrorCode(), QStringLiteral("invalid_input"));
    QTcpServer busy;
    QVERIFY(busy.listen(QHostAddress::Any, 0));
    lh->beginShare("Home", "Max", "long-enough-password", "home", busy.serverPort());
    QCOMPARE(lh->shareErrorCode(), QStringLiteral("port_in_use"));
    QVERIFY(lh->shareSuggestedPort() > 0);
    QVERIFY(!lh->shareError().isEmpty());
    lh->dismissShareError();
    QCOMPARE(lh->sharePhase(), QStringLiteral("idle"));
    QCOMPARE(lh->childState(), QStringLiteral("running"));  // nothing was stopped
  }

  void shareHappyPathPreparesTheServerAndLaunchesTheInstaller() {
    FakeHub hub(QStringLiteral("a"));
    hub.adminUser = true;
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/settings")};
    QVERIFY(hub.start());
    Rig r;
    r.writePort(portOf(hub));
    r.open();
    auto* lh = r.lh();
    lh->startChild();
    waitConnected(r);
    QTRY_VERIFY_WITH_TIMEOUT(lh->onThisPcOnly(), 8000);
    const QString hidden = hub.lastLocalBody.value("password").toString();
    hub.accountPassword = hidden;
    QStringList props;
    lh->setShareStarter([&](const QStringList& p) {
      props = p;
      return true;
    });
    const int port = freePort();
    lh->beginShare(QStringLiteral("Home server"), QStringLiteral("Max"), QStringLiteral("a-long-passphrase"), QStringLiteral("internet"), port);
    QCOMPARE(lh->sharePhase(), QStringLiteral("preparing"));
    QTRY_COMPARE_WITH_TIMEOUT(lh->sharePhase(), QStringLiteral("installing"), 15000);
    // The server was prepared with the hidden admin's confirmation, then the hidden password was replaced.
    QCOMPARE(hub.confirmPosts, 1);
    QVERIFY(hub.adminBodies.value(QStringLiteral("PATCH /api/v1/admin/settings")).contains("Home server"));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("PATCH /api/v1/admin/settings")), 1);  // the refused try is not counted
    QCOMPARE(hub.accountsByUser.value(QStringLiteral("u_admin_1")).displayName, QStringLiteral("Max"));
    QCOMPARE(hub.accountsByUser.value(QStringLiteral("u_admin_1")).password, QStringLiteral("a-long-passphrase"));
    QCOMPARE(lh->autoConfirmPassword(), QString());  // the hidden password is gone
    // The child is stopped, the pending record and the MSI properties are right.
    QCOMPARE(lh->childState(), QStringLiteral("stopped"));
    QCOMPARE(countLines(r.serverDir() + "/stops"), 1);
    QCOMPARE(props, (QStringList{QStringLiteral("HUB_PORT=%1").arg(port), QStringLiteral("NETWORK_SHARING=1"),
                                 QStringLiteral("IMPORT_SERVER_DATA=") + QDir::toNativeSeparators(r.serverDir())}));
    const PlayerSettings saved = r.settings();
    QVERIFY(saved.pendingShare().valid());
    QCOMPARE(saved.pendingShare().port, port);
    QCOMPARE(saved.pendingShare().join, QStringLiteral("internet"));
    QCOMPARE(saved.pendingShare().dataDir, r.serverDir());
    QCOMPARE(saved.localServerMode(), QStringLiteral("child"));  // still child until the service is there
    QCOMPARE(saved.displayName(), QStringLiteral("Max"));
  }

  void shareFollowUpStartMovesToTheServiceAndKeepsThePairing() {
    FakeHub child(QStringLiteral("a"));
    QVERIFY(child.start());
    Rig r;
    r.writePort(portOf(child));
    r.open();
    r.lh()->startChild();
    waitConnected(r);
    const QString hubId = child.hubId;
    const QString credRef = r.pc->profileStore()->profile(hubId)->credentialRef;
    r.close();
    // The installer ran: the service is another listener of the same server (same ID and certificate), moved.json exists.
    PlayerSettings::PendingShare ps{freePort(), QStringLiteral("home"), r.serverDir()};
    {
      PlayerSettings s(r.dir.path());
      QVERIFY(s.setPendingShare(ps));
    }
    FakeHub service(QStringLiteral("a"));
    QVERIFY(service.start());
    QFile moved(r.serverDir() + "/moved.json");
    QVERIFY(moved.open(QIODevice::WriteOnly));
    moved.write("{\"to\":\"C:\\\\ProgramData\\\\Uzume\\\\Server\",\"at\":\"2026-10-23T10:00:00Z\"}");
    moved.close();
    qputenv("UZUME_LOCAL_SERVER_PORT", QByteArray::number(portOf(service)));

    r.open();
    r.pc->startup();
    auto* lh = r.lh();
    QCOMPARE(lh->mode(), QStringLiteral("service"));
    QCOMPARE(lh->sharePhase(), QStringLiteral("done"));
    QVERIFY(lh->shareDone());
    waitConnected(r);
    QCOMPARE(r.pc->profileStore()->profile(hubId)->credentialRef, credRef);  // the same pairing
    QVERIFY(localhub::isLocalHubAddress(r.pc->profileStore()->profile(hubId)->address, portOf(service)));
    QCOMPARE(lh->childState(), QStringLiteral("stopped"));
    QVERIFY(!r.settings().pendingShare().valid());
    QCOMPARE(r.settings().localServerPort(), portOf(service));
    QVERIFY(lh->canStopSharing());
    lh->acknowledgeShareDone();
    QVERIFY(!lh->shareDone());
    QCOMPARE(lh->sharePhase(), QStringLiteral("idle"));
  }

  void shareFollowUpStartWithoutServiceFallsBackToTheChild() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Rig r;
    r.writePort(portOf(hub));
    r.open();
    r.lh()->startChild();
    waitConnected(r);
    r.close();
    {
      PlayerSettings s(r.dir.path());
      QVERIFY(s.setPendingShare({freePort(), QStringLiteral("home"), r.serverDir()}));
    }
    // No service registered, no moved.json: the person declined the administrator prompt.
    r.open();
    r.pc->startup();
    auto* lh = r.lh();
    QCOMPARE(lh->sharePhase(), QStringLiteral("failed"));
    QCOMPARE(lh->shareErrorCode(), QStringLiteral("admin_rights_refused"));
    QVERIFY(lh->shareError().contains(QStringLiteral("Nothing was changed")));
    QCOMPARE(lh->mode(), QStringLiteral("child"));
    QVERIFY(!r.settings().pendingShare().valid());
    waitConnected(r);
    QTRY_VERIFY_WITH_TIMEOUT(lh->onThisPcOnly(), 8000);
    QCOMPARE(countLines(r.serverDir() + "/starts"), 2);
  }

  // ------------------------------------------------------------ Stop sharing / Share again

  // Service mode on `service` (a second listener of the same server); the child's data folder is the former service folder.
  void toServiceMode(Rig& r, FakeHub& child, FakeHub& service) {
    r.writePort(portOf(child));
    r.open();
    r.lh()->startChild();
    waitConnected(r);
    r.close();
    {
      PlayerSettings s(r.dir.path());
      QVERIFY(s.setLocalServerMode(QStringLiteral("service")));
      QVERIFY(s.setLocalServerPort(portOf(service)));
      QVERIFY(s.setLocalServerDataDir(QString()));
    }
    {
      ProfileStore ps(r.dir.path());
      QVERIFY(ps.updateHubAddress(child.hubId, QStringLiteral("127.0.0.1"), portOf(service)));
    }
    qputenv("UZUME_LOCAL_SERVER_PORT", QByteArray::number(portOf(service)));
    qputenv("UZUME_LOCAL_SERVER_DIR", tools_.path().toUtf8());
    qputenv("UZUME_LOCAL_SERVICE_DATA_DIR", r.serverDir().toUtf8());
    r.open();
    r.pc->startup();
    waitConnected(r);
    QCOMPARE(r.lh()->mode(), QStringLiteral("service"));
  }

  void stopSharingReturnsToTheChildOnTheServiceData() {
    FakeHub child(QStringLiteral("a")), service(QStringLiteral("a"));
    QVERIFY(child.start());
    QVERIFY(service.start());
    Rig r;
    toServiceMode(r, child, service);
    auto* lh = r.lh();
    QStringList seen;
    QString program;
    lh->setSharingRunner([&](const QString& prog, const QStringList& args, QString*) {
      program = prog;
      seen = args;
      const int i = static_cast<int>(args.indexOf(QStringLiteral("--result")));
      QFile f(args.at(i + 1));
      if (!f.open(QIODevice::WriteOnly)) return 1;
      f.write(QJsonDocument(QJsonObject{{"ok", true}, {"error", ""}, {"data_dir", r.serverDir()}, {"port", portOf(child)}}).toJson());
      return 0;
    });
    QVERIFY(lh->canStopSharing());
    lh->stopSharing();
    QCOMPARE(lh->sharePhase(), QStringLiteral("stopping"));
    QTRY_COMPARE_WITH_TIMEOUT(lh->mode(), QStringLiteral("child"), 10000);
    QCOMPARE(program, localhub::hubExe(tools_.path()));
    QCOMPARE(seen.mid(0, 3), (QStringList{"sharing", "stop", "--grant-user"}));
    QVERIFY(!seen.at(3).isEmpty());
    QCOMPARE(seen.at(4), QStringLiteral("--result"));
    waitConnected(r);
    QTRY_COMPARE_WITH_TIMEOUT(lh->sharePhase(), QStringLiteral("idle"), 10000);
    QCOMPARE(lh->childState(), QStringLiteral("running"));
    QCOMPARE(r.settings().localServerDataDir(), r.serverDir());
    QCOMPARE(r.settings().localServerPort(), portOf(child));
    QVERIFY(localhub::isLocalHubAddress(r.pc->profileStore()->profile(child.hubId)->address, portOf(child)));
    QVERIFY(lh->onThisPcOnly());
    QVERIFY(lh->shareAgainAvailable());
  }

  void stopSharingErrorsKeepTheService() {
    FakeHub child(QStringLiteral("a")), service(QStringLiteral("a"));
    QVERIFY(child.start());
    QVERIFY(service.start());
    Rig r;
    toServiceMode(r, child, service);
    auto* lh = r.lh();
    lh->setSharingRunner([](const QString&, const QStringList&, QString* err) {
      *err = QStringLiteral("declined");
      return -1;
    });
    lh->stopSharing();
    QTRY_COMPARE_WITH_TIMEOUT(lh->sharePhase(), QStringLiteral("failed"), 10000);
    QCOMPARE(lh->shareErrorCode(), QStringLiteral("admin_rights_refused"));
    QCOMPARE(lh->mode(), QStringLiteral("service"));
    lh->dismissShareError();
    lh->setSharingRunner([](const QString&, const QStringList& args, QString*) {
      QFile f(args.at(static_cast<int>(args.indexOf(QStringLiteral("--result"))) + 1));
      if (f.open(QIODevice::WriteOnly)) f.write("{\"ok\":false,\"error\":\"service is busy\"}");
      return 0;
    });
    lh->stopSharing();
    QTRY_COMPARE_WITH_TIMEOUT(lh->sharePhase(), QStringLiteral("failed"), 10000);
    QCOMPARE(lh->shareErrorCode(), QStringLiteral("stop_failed"));
    QVERIFY(lh->shareError().contains(QStringLiteral("service is busy")));
    QCOMPARE(lh->mode(), QStringLiteral("service"));
    QCOMPARE(r.pc->connection()->state(), S::Connected);
  }

  void shareAgainStartsTheServiceOnceMore() {
    FakeHub child(QStringLiteral("a")), service(QStringLiteral("a"));
    QVERIFY(child.start());
    QVERIFY(service.start());
    Rig r;
    toServiceMode(r, child, service);
    auto* lh = r.lh();
    // Stop sharing first (service -> child on the service data), then share again.
    lh->setSharingRunner([&](const QString&, const QStringList& args, QString*) {
      const int i = static_cast<int>(args.indexOf(QStringLiteral("--result")));
      QFile f(args.at(i + 1));
      if (!f.open(QIODevice::WriteOnly)) return 1;
      const bool stop = args.contains(QStringLiteral("stop"));
      f.write(QJsonDocument(stop ? QJsonObject{{"ok", true}, {"data_dir", r.serverDir()}, {"port", portOf(child)}}
                                 : QJsonObject{{"ok", true}, {"port", portOf(service)}})
                  .toJson());
      return 0;
    });
    lh->stopSharing();
    QTRY_COMPARE_WITH_TIMEOUT(lh->mode(), QStringLiteral("child"), 10000);
    waitConnected(r);
    QTRY_COMPARE_WITH_TIMEOUT(lh->sharePhase(), QStringLiteral("idle"), 10000);
    QVERIFY(lh->shareAgainAvailable());
    lh->shareAgain();
    QTRY_COMPARE_WITH_TIMEOUT(lh->mode(), QStringLiteral("service"), 10000);
    waitConnected(r);
    QCOMPARE(lh->childState(), QStringLiteral("stopped"));
    QVERIFY(localhub::isLocalHubAddress(r.pc->profileStore()->profile(child.hubId)->address, portOf(service)));
    QVERIFY(!lh->shareAgainAvailable());
  }
};

UITEST_MAIN(LocalServerUiTest)
#include "localserver_ui_test.moc"
