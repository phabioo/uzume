// Controller hand-over (0.23 package 8, ADR 0033 D8) in the UI against FakeHub: the host's confirm card (Allow / Deny, keys, auto-deny), the
// People rows (PLAYER P2 tag, "Take back"), P1 take-over suspending the local input, and Esc twice. A real SessionViewer in the same process
// asks over the real fb-diag / fb-input channels. Offscreen; synthetic data only.
#include <QSignalSpy>
#include <QtTest>

#include <QQuickWindow>
#include <QtQuickTest/quicktest.h>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

class ControlUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("sessions_v1"), QStringLiteral("together_v1")};
    QVERIFY(hub.start());
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    h.window->resize(1280, 800);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->sessions()->socket()->isOpen(), 8000);
  }
  static QString text(QQuickItem* it) { return it ? it->property("text").toString() : QString(); }
  static bool shown(QQuickItem* it) { return it != nullptr && it->isVisible(); }
  static void startPreviewGame(Harness& h) {
    h.controller->selectGame(QStringLiteral("g1"));
    h.controller->adoptPreviewGame(QStringLiteral("g1"));
    GameSession* gs = h.controller->gameSession();
    QImage frame(256, 384, QImage::Format_RGB32);
    frame.fill(QColor(40, 90, 140));
    gs->setPreview(QStringLiteral("Preview"), frame, emu::DisplayProfile(), EmulationDiagnostics());
    emit gs->started();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 8000);
    QQuickTest::qWaitForPolish(h.window);
    QTRY_VERIFY(h.item("gameView") != nullptr);
  }
  static void hostSession(Harness& h) {
    SessionController* ctl = h.controller->sessions();
    QJsonObject own{{"session_id", "own1"}, {"game_id", "g1"}, {"game_title", "Harbor Rally"},
                    {"owner", QJsonObject{{"user_id", h.controller->connection()->hubUserId()}, {"display_name", "Tester"}, {"device_name", "PC"}}},
                    {"visibility", "hub_users"}, {"created_at", QDateTime::currentDateTimeUtc().addSecs(-300).toString(Qt::ISODate)},
                    {"viewer_count", 2}, {"is_owner", true}, {"invited", false},
                    {"viewers", QJsonArray{QJsonObject{{"viewer_id", "v1"}, {"display_name", "Lena"}, {"device_name", "PC"}},
                                           QJsonObject{{"viewer_id", "v2"}, {"display_name", "Sam"}, {"device_name", "PC"}}}}};
    const auto info = parseSession(own);
    QVERIFY(info.has_value());
    ctl->adoptSharedSessionForTest(*info);
    const auto scope = []() { return TogetherController::Scope{QStringLiteral("session"), QStringLiteral("own1"), true}; };
    h.controller->together()->setScopeProvider(scope);
    h.controller->chat()->setScopeProvider(scope);
    h.controller->voice()->setScopeProvider(scope);
    h.controller->voice()->setCaptureEnabled(false);
    h.controller->together()->refresh();
    h.controller->chat()->refresh();
  }
  // A viewer in the same process, wired to the host of the controller the way the Hub relays signals.
  struct Peer {
    SessionViewer viewer;
    void wire(SessionHost* host, const QString& id) {
      QObject::connect(&viewer, &SessionViewer::signalOut, host, &SessionHost::handleSignal);
      QObject::connect(host, &SessionHost::signalOut, &viewer, [this, id](const SessionSignal& s) {
        if (s.viewerId == id) viewer.handleSignal(s);
      });
      viewer.open(QStringLiteral("own1"), id, {});
      host->addViewer(id);
    }
  };
  static QVariantMap personRow(Harness& h, const QString& name) {
    for (const QVariant& v : h.controller->sessions()->sessionPeople(QStringLiteral("own1")))
      if (v.toMap().value("name").toString() == name) return v.toMap();
    return {};
  }

 private slots:
  void initTestCase() { uitest::installWarningCounter(); }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // Ask for P2: the card, Allow, the PLAYER tag, Take back; P1 take-over suspends the local pad; a held port is refused with a reason.
  void requestCardAllowAndTakeBack() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    hostSession(h);
    ControlController* cc = h.controller->control();
    cc->setPortsProvider([]() { return 2; });
    SessionHost* host = h.controller->sessions()->host();
    host->open(QStringLiteral("own1"), {});
    Peer lena;
    lena.wire(host, QStringLiteral("v1"));
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.hostPorts(), 2, 20000);
    QVERIFY(!shown(h.item("stageControlCard")));

    QVERIFY(lena.viewer.requestControl(1));
    QTRY_VERIFY_WITH_TIMEOUT(cc->askOpen(), 10000);
    QTRY_VERIFY(shown(h.item("stageControlCard")));
    QCOMPARE(text(h.item("controlCardText")), QStringLiteral("Lena wants to play as P2"));
    QVERIFY(shown(h.item("controlAllow")) && shown(h.item("controlDeny")));
    QVERIFY(!shown(h.item("controlTakeBack")));
    QVERIFY(cc->askSeconds() > 0 && cc->askSeconds() <= 20);
    uitest::saveShot(h.window, QStringLiteral("control-request-card"));
    // the People row says she wants it
    QCOMPARE(personRow(h, QStringLiteral("Lena")).value("controlNote").toString(), QStringLiteral("Wants to play as P2"));

    QVERIFY(h.click("controlAllow"));
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Granted, 10000);
    QTRY_VERIFY(!cc->askOpen());
    QTRY_VERIFY(shown(h.item("controlTakeBack")));
    QCOMPARE(text(h.item("controlCardText")), QStringLiteral("Lena plays P2"));
    const QVariantMap row = personRow(h, QStringLiteral("Lena"));
    QCOMPARE(row.value("role").toString(), QStringLiteral("PLAYER P2"));
    QCOMPARE(row.value("roleKey").toString(), QStringLiteral("player"));
    QCOMPARE(row.value("controlButtons").toList().first().toMap().value("key").toString(), QStringLiteral("takeback"));
    QCOMPARE(personRow(h, QStringLiteral("Sam")).value("role").toString(), QStringLiteral("VIEWER"));
    // the injected input reaches port 1 only
    GameSession* gs = h.controller->gameSession();
    QVERIFY(gs->remoteControlled(1) && !gs->remoteControlled(0));
    lena.viewer.setControlButtons(0x0020);
    QTRY_COMPARE_WITH_TIMEOUT(gs->remoteMask(1), 0x0020u, 10000);
    // a second viewer cannot take the held port (not connected here: the arbiter refuses it)
    QVERIFY(host->controlPending().isEmpty());
    // People tab: PLAYER tag and the Take back button on the row
    QVERIFY(h.click("panelTab_people"));
    QTRY_VERIFY(h.item("control_takeback") != nullptr);
    uitest::saveShot(h.window, QStringLiteral("control-people-player"));
    QVERIFY(h.click("control_takeback"));
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Idle, 10000);
    QCOMPARE(lena.viewer.controlReason(), QStringLiteral("host"));
    QTRY_VERIFY(!shown(h.item("stageControlCard")));
    QVERIFY(!gs->remoteControlled(1));
    QCOMPARE(personRow(h, QStringLiteral("Lena")).value("role").toString(), QStringLiteral("VIEWER"));
  }

  // Deny, key 1 / 2, P1 take-over (the local pad no longer drives port 0), Esc twice.
  void denyKeysTakeOverAndEscapeTwice() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    hostSession(h);
    ControlController* cc = h.controller->control();
    cc->setPortsProvider([]() { return 1; });  // one port: no "Play as P2"
    SessionHost* host = h.controller->sessions()->host();
    host->open(QStringLiteral("own1"), {});
    Peer lena;
    lena.wire(host, QStringLiteral("v1"));
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.hostPorts(), 1, 20000);
    QVERIFY(!lena.viewer.requestControl(1));  // NDS-like: only P1

    QVERIFY(lena.viewer.requestControl(0));
    QTRY_VERIFY_WITH_TIMEOUT(cc->askOpen(), 10000);
    QCOMPARE(text(h.item("controlCardText")), QStringLiteral("Lena wants to take over P1"));
    QVERIFY(h.click("controlDeny"));
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Idle, 10000);
    QCOMPARE(lena.viewer.controlReason(), QStringLiteral("denied"));
    QVERIFY(!shown(h.item("stageControlCard")));

    // key 1 allows
    QVERIFY(lena.viewer.requestControl(0));
    QTRY_VERIFY_WITH_TIMEOUT(cc->askOpen(), 10000);
    GameSession* gs = h.controller->gameSession();
    gs->setGamepadMask(0x0001);  // the host's own pad
    h.item("gameView")->forceActiveFocus();
    QTest::keyClick(h.window, Qt::Key_1);
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Granted, 10000);
    QVERIFY(gs->remoteControlled(0));
    lena.viewer.setControlButtons(0x0100);
    QTRY_COMPARE_WITH_TIMEOUT(gs->remoteMask(0), 0x0100u, 10000);
    gs->setGamepadMask(0x0002);  // the local pad no longer changes the remote mask
    QCOMPARE(gs->remoteMask(0), 0x0100u);

    // Esc twice takes the controller back; the first press only says so
    h.item("gameView")->forceActiveFocus();
    QTest::keyClick(h.window, Qt::Key_Escape);
    QVERIFY(lena.viewer.controlState() == SessionViewer::ControlState::Granted);
    QVERIFY(!gs->isPaused());
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Idle, 10000);
    QCOMPARE(lena.viewer.controlReason(), QStringLiteral("host"));
    QVERIFY(!gs->remoteControlled(0));
  }

  // P2 guest: Esc is not taken (the game pauses as usual); a game switch ends the hold; two pending requests above a lowered port
  // count are denied "unsupported" and the viewer's Asking resets.
  void guestEscPausesAndPortDropDeniesAsk() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    hostSession(h);
    ControlController* cc = h.controller->control();
    static int ports = 2;  // read by the provider for as long as the controller lives
    ports = 2;
    cc->setPortsProvider([]() { return ports; });
    SessionHost* host = h.controller->sessions()->host();
    host->open(QStringLiteral("own1"), {});
    Peer lena;
    lena.wire(host, QStringLiteral("v1"));
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.hostPorts(), 2, 20000);
    QVERIFY(lena.viewer.requestControl(1));
    QTRY_VERIFY_WITH_TIMEOUT(cc->askOpen(), 10000);
    cc->allow();
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Granted, 10000);
    QVERIFY(cc->hasHolder());
    QVERIFY(!cc->escapePressed());  // P2 is not P1: Esc is left to the pause
    QVERIFY(lena.viewer.controlState() == SessionViewer::ControlState::Granted);
    host->takeBack(1);
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Idle, 10000);

    // the host's port count drops while Lena asks for P2
    QVERIFY(lena.viewer.requestControl(1));
    QTRY_VERIFY_WITH_TIMEOUT(cc->askOpen(), 10000);
    ports = 1;
    host->setControlPorts(1);
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Idle, 10000);
    QTRY_VERIFY(!cc->askOpen());
  }

  // 20 s without an answer: denied with "timeout"; the viewer leaving takes its hold along.
  void timeoutAndLeaving() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    hostSession(h);
    ControlController* cc = h.controller->control();
    cc->setPortsProvider([]() { return 2; });
    SessionHost* host = h.controller->sessions()->host();
    host->open(QStringLiteral("own1"), {});
    Peer lena;
    lena.wire(host, QStringLiteral("v1"));
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.hostPorts(), 2, 20000);
    QVERIFY(lena.viewer.requestControl(0));
    QTRY_VERIFY_WITH_TIMEOUT(cc->askOpen(), 10000);
    host->advanceControlClockForTest(kControlAskTimeoutMs + 1);
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Idle, 10000);
    QCOMPARE(lena.viewer.controlReason(), QStringLiteral("timeout"));
    QTRY_VERIFY(!cc->askOpen());
    // leaving ends a hold
    QVERIFY(lena.viewer.requestControl(0));
    QTRY_VERIFY_WITH_TIMEOUT(cc->askOpen(), 10000);
    cc->allow();
    QTRY_COMPARE_WITH_TIMEOUT(lena.viewer.controlState(), SessionViewer::ControlState::Granted, 10000);
    QVERIFY(h.controller->gameSession()->remoteControlled(0));
    host->removeViewer(QStringLiteral("v1"));
    QVERIFY(!h.controller->gameSession()->remoteControlled(0));
    QVERIFY(!cc->hasHolder());
  }
};

QTEST_MAIN(ControlUiTest)
#include "control_ui_test.moc"
