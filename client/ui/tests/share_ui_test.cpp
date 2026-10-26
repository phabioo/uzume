// 0.18 package 10 screens: the "On this PC" view (sidebar, Library, Saves, Users), the "Share with others" dialogs (8i), "Stop
// sharing" with "Confirm it's you" and the ConnectionScreen child states, driven through Main.qml against a fake server script
// (UZUME_LOCAL_SERVER_EXE) and the FakeHub. Skipped on Windows.
#include <QDir>
#include <QFile>
#include <QTcpServer>
#include <QtTest>

#include "fakehub.h"
#include "localhubcontroller.h"
#include "playercontroller.h"
#include "playersettings.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
const char* kScript = R"SH(#!/bin/sh
while [ $# -gt 0 ]; do
  case "$1" in
    -data-dir) DATA="$2"; shift 2;;
    *) shift;;
  esac
done
echo x >> "$DATA/starts"
if [ -f "$DATA/fail" ]; then
  echo "boom" >&2
  exit 1
fi
PORT=$(cat "$DATA/fake_port")
echo "UZUME_READY $PORT"
cat >/dev/null
)SH";

bool shown(const Harness& h, const char* name) {
  QQuickItem* it = h.item(name);
  return it != nullptr && it->isVisible() && it->window() == h.window;
}
QString textOf(const Harness& h, const char* name) {
  QQuickItem* it = h.item(name);
  return it != nullptr ? it->property("text").toString() : QString();
}
int portOf(const FakeHub& h) { return QUrl(h.address()).port(); }
int countLines(const QString& file) {
  QFile f(file);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts).size() : 0;
}
int freePort() {
  QTcpServer s;
  return s.listen(QHostAddress::Any, 0) ? s.serverPort() : 0;
}
}  // namespace

#define SHOWN(name) QTRY_VERIFY2_WITH_TIMEOUT(shown(h, name), name, 8000)
#define GONE(name) QTRY_VERIFY2_WITH_TIMEOUT(!shown(h, name), name, 8000)

class ShareUiTest : public QObject {
  Q_OBJECT

  QTemporaryDir tools_;
  QString exe_;

  static QString serverDir(const Harness& h) { return h.dir.filePath(QStringLiteral("server")); }
  static void writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
  }
  // A running child server: the controller is on "this PC only" against `hub`.
  void startChildOn(Harness& h, FakeHub& hub) {
    writeFile(serverDir(h) + "/fake_port", QByteArray::number(portOf(hub)));
    QVERIFY(h.start());
    h.controller->localHub()->setShareSupportedForTests(true);
    h.controller->localHub()->startChild();
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->localHub()->onThisPcOnly(), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
  }

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
    uitest::warningCount() = 0;
    qputenv("UZUME_LOCAL_SERVER_EXE", exe_.toUtf8());
  }
  void cleanup() {
    qunsetenv("UZUME_LOCAL_SERVER_EXE");
    QCOMPARE(uitest::warningCount().load(), 0);
  }

  void onThisPcShowsThisPcInTheSidebarAndTheLibraryLabel() {
    FakeHub hub(QStringLiteral("a"));
    hub.adminUser = true;
    QVERIFY(hub.start());
    Harness h;
    startChildOn(h, hub);
    SHOWN("navLibrary");
    SHOWN("navThisPcSystems");
    SHOWN("navThisPcPlugins");
    QVERIFY(!shown(h, "navServer"));
    QVERIFY(!shown(h, "navOnlineHead"));
    QVERIFY(!shown(h, "navServerHead") || !textOf(h, "navServerHead").contains(QStringLiteral("Server")));
    SHOWN("librarySectionLabel");
    QVERIFY2(textOf(h, "librarySectionLabel").toLower().startsWith(QStringLiteral("on this pc")), qPrintable(textOf(h, "librarySectionLabel")));
    SHOWN("libraryOnThisPcNote");
    QVERIFY(textOf(h, "libraryOnThisPcNote").contains(QStringLiteral("Share with others")));
  }

  void usersPageNeedsAServerOnThisPc() {
    FakeHub hub(QStringLiteral("a"));
    hub.adminUser = true;
    QVERIFY(hub.start());
    Harness h;
    startChildOn(h, hub);
    h.controller->showServerPage(QStringLiteral("users"));
    SHOWN("serverNeedsServer");
    QVERIFY(h.item("serverNeedsServerState")->property("title").toString().contains(QStringLiteral("People needs a server")));
  }

  // 8i-1 .. 8i-3: intro, setup form, then beginShare gets the form values.
  void theShareDialogsReachBeginShareWithTheFormValues() {
    FakeHub hub(QStringLiteral("a"));
    hub.adminUser = true;
    QVERIFY(hub.start());
    Harness h;
    startChildOn(h, hub);
    auto* lh = h.controller->localHub();
    QStringList props;
    lh->setShareStarter([&](const QStringList& p) {
      props = p;
      return true;
    });
    lh->openShareDialog(QStringLiteral("intro"));
    SHOWN("shareIntroDialog");
    QVERIFY(h.clickWhenReady("shareIntroContinue"));
    SHOWN("shareSetupDialog");
    // A short password is refused before anything starts.
    h.item("shareServerName")->setProperty("text", QStringLiteral("Home server"));
    h.item("shareYourName")->setProperty("text", QStringLiteral("Max"));
    h.item("shareYourPassword")->setProperty("text", QStringLiteral("short"));
    QVERIFY(h.click("shareSetupConfirm"));
    QCOMPARE(lh->sharePhase(), QStringLiteral("idle"));
    const int port = freePort();
    h.item("shareYourPassword")->setProperty("text", QStringLiteral("a-long-passphrase"));
    h.item("shareHostPort")->setProperty("text", QString::number(port));
    QVERIFY(h.click("shareSetupConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(lh->sharePhase() != QStringLiteral("idle"), 8000);
    SHOWN("shareProgressDialog");
    QTRY_COMPARE_WITH_TIMEOUT(lh->sharePhase(), QStringLiteral("installing"), 15000);
    QVERIFY(props.contains(QStringLiteral("HUB_PORT=%1").arg(port)));
    QVERIFY(props.contains(QStringLiteral("NETWORK_SHARING=1")));
    QCOMPARE(h.controller->playerSettings()->pendingShare().join, QStringLiteral("home"));  // the default of "Who can join"
    QCOMPARE(h.controller->playerSettings()->displayName(), QStringLiteral("Max"));
  }

  // 8i-6: an error code shows the matching dialog; "Use <port>" retries with the suggestion.
  void thePortInUseErrorOffersAnotherPort() {
    FakeHub hub(QStringLiteral("a"));
    hub.adminUser = true;
    QVERIFY(hub.start());
    Harness h;
    startChildOn(h, hub);
    auto* lh = h.controller->localHub();
    lh->failShareForTests(QStringLiteral("port_in_use"), 8444);
    QCOMPARE(lh->sharePhase(), QStringLiteral("failed"));
    SHOWN("shareErrorDialog");
    SHOWN("shareErrorPort");
    QVERIFY2(textOf(h, "shareErrorPort").contains(QStringLiteral("8445")), qPrintable(textOf(h, "shareErrorPort")));
    QVERIFY(h.click("shareErrorClose"));
    GONE("shareErrorDialog");
    QCOMPARE(lh->sharePhase(), QStringLiteral("idle"));
  }

  // 8i-5: Stop sharing → Confirm it's you → stopSharing().
  void stopSharingAsksToConfirmThenStops() {
    FakeHub hub(QStringLiteral("a"));
    hub.adminUser = true;
    QVERIFY(hub.start());
    Harness h;
    {
      PlayerSettings s(h.dir.path());
      QVERIFY(s.setLocalServerMode(QStringLiteral("service")));
    }
    QVERIFY(h.start());
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    auto* lh = h.controller->localHub();
    QVERIFY(lh->canStopSharing());
    lh->openShareDialog(QStringLiteral("stop"));
    SHOWN("stopSharingDialog");
    QVERIFY(h.clickWhenReady("stopSharingConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->admin()->confirmNeeded(), 8000);
    QCOMPARE(lh->sharePhase(), QStringLiteral("idle"));  // nothing before the person confirmed
    SHOWN("confirmPassword");
    h.item("confirmPassword")->setProperty("text", hub.accountPassword);
    QVERIFY(h.click("confirmSubmit"));
    // stopSharing() ran: this machine has no installed service, so it ends in the "stop failed" dialog (8i-6) with Try again.
    QTRY_COMPARE_WITH_TIMEOUT(lh->shareErrorCode(), QStringLiteral("stop_failed"), 8000);
    SHOWN("shareErrorDialog");
    SHOWN("shareErrorRetry");
  }

  // 8b: the app's own server did not start: the reason and "Start again".
  void connectionScreenShowsTheChildFailureAndStartsAgain() {
    Harness h;
    writeFile(serverDir(h) + "/fail", "1");
    {
      PlayerSettings s(h.dir.path());
      QVERIFY(s.setLocalServerMode(QStringLiteral("child")));
    }
    QVERIFY(h.start());
    h.controller->onboarding()->setStartScreenEnabled(false);
    auto* lh = h.controller->localHub();
    lh->setServerConfigPatch([](LocalServerProcess::Config& c) { c.maxRestarts = 0; });
    h.controller->startup();
    QTRY_COMPARE_WITH_TIMEOUT(lh->childState(), QStringLiteral("failed"), 15000);
    SHOWN("connectChildState");
    const int before = countLines(serverDir(h) + "/starts");
    QVERIFY(h.clickWhenReady("stateBlockAction"));
    QTRY_VERIFY_WITH_TIMEOUT(countLines(serverDir(h) + "/starts") > before, 15000);
    uitest::warningCount() = 0;  // the failing server logs its failure on purpose
  }
};

UITEST_MAIN(ShareUiTest)
#include "share_ui_test.moc"
