// Voice chat (0.23 package 7, ADR 0033 D7) in the UI against FakeHub: the header mic control with its states, Join / Leave voice in the People
// tab, the voice marks, the volume slider and the host's mute on the rows, and Settings > This PC > Voice. No microphone: the capture is off and
// the engine's events are emitted by hand. Offscreen; synthetic data only.
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

class VoiceUiTest : public QObject {
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
  static QJsonObject lastWs(FakeHub& hub, const QString& type) {
    for (int i = hub.wsReceived.size() - 1; i >= 0; --i)
      if (hub.wsReceived[i].value("type").toString() == type) return hub.wsReceived[i].value("payload").toObject();
    return {};
  }
  static int countWs(FakeHub& hub, const QString& type) {
    int n = 0;
    for (const QJsonObject& e : hub.wsReceived) n += e.value("type").toString() == type;
    return n;
  }
  static QJsonObject vmember(const QString& user, const QString& device, const QString& mic = QStringLiteral("ptt"), bool muted = false) {
    return {{"user_id", user}, {"device_id", device}, {"mic", mic}, {"muted_by_host", muted}};
  }
  static QJsonObject presence(const QString& user, const QString& name) {
    return {{"user_id", user}, {"device_id", user + "d"}, {"state", "online"}, {"display_name", name}};
  }
  static void clickIn(Harness& h, QQuickItem* it, double fx = 0.5, double fy = 0.5) {
    QQuickTest::qWaitForPolish(h.window);
    QTest::mouseClick(h.window, Qt::LeftButton, {}, it->mapToScene(QPointF(it->width() * fx, it->height() * fy)).toPoint());
  }
  static QQuickItem* row(Harness& h, const QString& person) { return h.item(qPrintable(QStringLiteral("person_") + person)); }

  // A hosted Session "own1" with the viewers Lena and Sam, as the scope of everything together.
  static void hostSession(Harness& h, FakeHub& hub) {
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
    hub.sendWs(QStringLiteral("presence_update"), presence("u_lena", "Lena"));
    hub.sendWs(QStringLiteral("presence_update"), presence("u_sam", "Sam"));
    QTRY_COMPARE(h.controller->onlinePeople().size(), 2);
    const auto scope = []() { return TogetherController::Scope{QStringLiteral("session"), QStringLiteral("own1"), true}; };
    h.controller->together()->setScopeProvider(scope);
    h.controller->chat()->setScopeProvider(scope);
    h.controller->voice()->setScopeProvider(scope);
    h.controller->voice()->setCaptureEnabled(false);
    h.controller->together()->refresh();
    h.controller->voice()->refresh();
  }

 private slots:
  void initTestCase() { uitest::installWarningCounter(); }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // One click joins from the header; the six mic states read as words; the People tab joins and leaves too.
  void headerMicStatesAndJoinLeave() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    hostSession(h, hub);
    VoiceController* v = h.controller->voice();
    const QString me = h.controller->connection()->hubUserId();
    const QString dev = h.controller->connection()->deviceId();
    QTRY_VERIFY(shown(h.item("micButton")));
    QCOMPARE(text(h.item("micButton")), QStringLiteral("Join voice"));
    QVERIFY(h.click("micButton"));
    QTRY_VERIFY(countWs(hub, "voice_join") == 1);
    QCOMPARE(lastWs(hub, "voice_join").value("scope").toString(), QStringLiteral("session"));
    QCOMPARE(lastWs(hub, "voice_join").value("id").toString(), QStringLiteral("own1"));
    QCOMPARE(lastWs(hub, "voice_join").value("mic").toString(), QStringLiteral("ptt"));
    hub.sendWs(QStringLiteral("voice_room"), {{"scope", "session"}, {"id", "own1"}, {"members", QJsonArray{vmember(me, dev)}}});
    QTRY_VERIFY(v->joined());
    // 1 push-to-talk: "Hold V"
    QTRY_COMPARE(h.item("micButton")->property("value").toString(), QStringLiteral("Hold V"));
    uitest::saveShot(h.window, QStringLiteral("voice-header-ptt"));
    // 2 talking: Live with the speaking mark
    QVERIFY(v->injectTone(440, 3));
    QTRY_COMPARE(h.item("micButton")->property("value").toString(), QStringLiteral("Live"));
    for (int i = 0; i < 3; ++i) v->tickForTests();
    QTRY_VERIFY(shown(h.item("micSpeakingMark")));
    uitest::saveShot(h.window, QStringLiteral("voice-header-live"));
    // 3 mic off (one click on the control; the Hub learns "off")
    v->setMicMode(QStringLiteral("ptt"));
    for (int i = 0; i < 160; ++i) v->tickForTests();  // the rest of the tone
    QVERIFY(!v->injecting());
    QVERIFY(h.click("micButton"));
    QTRY_COMPARE(h.item("micButton")->property("value").toString(), QStringLiteral("Mic off"));
    QTRY_COMPARE(lastWs(hub, "voice_state").value("mic").toString(), QStringLiteral("off"));
    uitest::saveShot(h.window, QStringLiteral("voice-header-off"));
    QVERIFY(h.click("micButton"));
    QTRY_COMPARE(h.item("micButton")->property("value").toString(), QStringLiteral("Hold V"));
    QTRY_COMPARE(lastWs(hub, "voice_state").value("mic").toString(), QStringLiteral("ptt"));
    // 4 muted by host
    hub.sendWs(QStringLiteral("voice_room"), {{"scope", "session"}, {"id", "own1"}, {"members", QJsonArray{vmember(me, dev, "ptt", true)}}});
    QTRY_COMPARE(h.item("micButton")->property("value").toString(), QStringLiteral("Muted by host"));
    QVERIFY(h.click("micButton"));  // nothing to do for the person
    QCOMPARE(h.item("micButton")->property("value").toString(), QStringLiteral("Muted by host"));
    uitest::saveShot(h.window, QStringLiteral("voice-header-muted-by-host"));
    hub.sendWs(QStringLiteral("voice_room"), {{"scope", "session"}, {"id", "own1"}, {"members", QJsonArray{vmember(me, dev)}}});
    QTRY_COMPARE(h.item("micButton")->property("value").toString(), QStringLiteral("Hold V"));
    // 5 no microphone, 6 blocked by the system
    v->setCaptureProblemForTests(true, false);
    QTRY_COMPARE(h.item("micButton")->property("value").toString(), QStringLiteral("No microphone"));
    v->setCaptureProblemForTests(false, true);
    QTRY_COMPARE(h.item("micButton")->property("value").toString(), QStringLiteral("Blocked"));
    QVERIFY(v->micDetail().contains(QStringLiteral("microphone")));
    uitest::saveShot(h.window, QStringLiteral("voice-header-blocked"));
    v->setCaptureProblemForTests(false, false);
    // Leave from the People tab
    QVERIFY(h.click("panelTab_people"));
    QTRY_VERIFY(shown(h.item("gamePanelPeople")));
    QCOMPARE(text(h.item("peopleVoiceJoin")), QStringLiteral("Leave voice"));
    QVERIFY(h.click("peopleVoiceJoin"));
    QTRY_COMPARE(countWs(hub, "voice_leave"), 1);
    QTRY_COMPARE(text(h.item("peopleVoiceJoin")), QStringLiteral("Join voice"));
    QTRY_COMPARE(text(h.item("micButton")), QStringLiteral("Join voice"));
  }

  // The People tab: marks next to the names (speaking bars, mic off, muted by host), the Volume slider of somebody else and the host's Mute.
  void peopleRowsShowMarksVolumeAndMute() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    hostSession(h, hub);
    VoiceController* v = h.controller->voice();
    const QString me = h.controller->connection()->hubUserId();
    const QString dev = h.controller->connection()->deviceId();
    QVERIFY(h.click("panelTab_people"));
    QTRY_VERIFY(shown(h.item("gamePanelPeople")));
    QVERIFY(h.click("peopleVoiceJoin"));
    QTRY_COMPARE(countWs(hub, "voice_join"), 1);
    hub.sendWs(QStringLiteral("voice_room"), {{"scope", "session"}, {"id", "own1"},
               {"members", QJsonArray{vmember(me, dev), vmember("u_lena", "dev-lena"), vmember("u_sam", "dev-sam", "off")}}});
    QTRY_VERIFY(row(h, QStringLiteral("u_lena")) != nullptr && row(h, QStringLiteral("u_sam")) != nullptr);
    const auto mark = [&](const char* user) { return row(h, QString::fromLatin1(user))->findChild<QQuickItem*>("personVoice")->property("voice").toString(); };
    QCOMPARE(mark("u_sam"), QStringLiteral("micOff"));
    QCOMPARE(mark("u_lena"), QString());
    emit v->engine()->speakingChanged(QStringLiteral("dev-lena"), true);
    QTRY_COMPARE(mark("u_lena"), QStringLiteral("speaking"));
    // Volume and Mute are on the rows of the others, not on mine
    QVERIFY(shown(row(h, QStringLiteral("u_lena"))->findChild<QQuickItem*>("personVolume")));
    QVERIFY(shown(row(h, QStringLiteral("u_lena"))->findChild<QQuickItem*>("personMute")));
    QQuickItem* mine = row(h, me);
    QVERIFY(mine != nullptr);
    QVERIFY(!shown(mine->findChild<QQuickItem*>("personVolume")));
    QVERIFY(!shown(mine->findChild<QQuickItem*>("personMute")));
    uitest::saveShot(h.window, QStringLiteral("voice-people-marks"));
    // The slider remembers the person's volume by user id
    QQuickItem* slider = row(h, QStringLiteral("u_lena"))->findChild<QQuickItem*>("personVolume");
    slider->setProperty("value", 30);
    QMetaObject::invokeMethod(slider, "moved");
    QTRY_COMPARE(v->personGain(QStringLiteral("u_lena")), 30);
    QCOMPARE(PlayerSettings(h.dir.path()).voicePersonGain(QStringLiteral("u_lena")), 30);
    // The host mutes Sam for everyone
    QQuickItem* mute = row(h, QStringLiteral("u_sam"))->findChild<QQuickItem*>("personMute");
    QCOMPARE(text(mute), QStringLiteral("Mute"));
    clickIn(h, mute);
    QTRY_COMPARE(countWs(hub, "voice_mute"), 1);
    QCOMPARE(lastWs(hub, "voice_mute").value("user_id").toString(), QStringLiteral("u_sam"));
    QCOMPARE(lastWs(hub, "voice_mute").value("id").toString(), QStringLiteral("own1"));
    QVERIFY(lastWs(hub, "voice_mute").value("muted").toBool());
    hub.sendWs(QStringLiteral("voice_room"), {{"scope", "session"}, {"id", "own1"},
               {"members", QJsonArray{vmember(me, dev), vmember("u_lena", "dev-lena"), vmember("u_sam", "dev-sam", "off", true)}}});
    QTRY_COMPARE(mark("u_sam"), QStringLiteral("mutedByHost"));
    QTRY_COMPARE(text(row(h, QStringLiteral("u_sam"))->findChild<QQuickItem*>("personMute")), QStringLiteral("Unmute"));
    uitest::saveShot(h.window, QStringLiteral("voice-people-muted"));
    // A person joins: the toast says so
    hub.sendWs(QStringLiteral("presence_update"), presence("u_jonas", "Jonas"));
    hub.sendWs(QStringLiteral("voice_room"), {{"scope", "session"}, {"id", "own1"},
               {"members", QJsonArray{vmember(me, dev), vmember("u_lena", "dev-lena"), vmember("u_sam", "dev-sam", "off", true), vmember("u_jonas", "dev-jonas")}}});
    // entries() builds a new list on each call: take begin and end from one copy
    const auto toasted = [&](const QString& text) {
      const QVariantList entries = h.controller->notifications()->entries();
      return std::any_of(entries.begin(), entries.end(), [&](const QVariant& e) { return e.toMap().value("text").toString() == text; });
    };
    QTRY_VERIFY(toasted(QStringLiteral("Jonas joined voice")));
  }

  // Settings > This PC > Voice (board 6w-1)
  void voiceSettingsPage() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    VoiceController* v = h.controller->voice();
    v->setCaptureEnabled(false);
    h.controller->showSettingsSection(QStringLiteral("voice"));
    QTRY_VERIFY(shown(h.item("voiceSection")));
    QTRY_VERIFY(shown(h.item("voiceContent")));
    QVERIFY(v->monitoring());  // the level moves while the page is open
    QCOMPARE(text(h.item("voiceThrough")), QStringLiteral("Through Test server"));
    for (const char* n : {"voiceMicRow", "voiceTalkRow", "voiceKeyRow", "voiceNoiseRow", "voiceDuckRow", "voiceAutoRow", "voiceMeter", "voiceLevel", "voiceThresholdLine"})
      QVERIFY2(shown(h.item(n)), n);
    QCOMPARE(h.item("voiceTalkSegment")->property("current").toString(), QStringLiteral("ptt"));
    QVERIFY(h.item("voiceNoiseToggle")->property("checked").toBool());
    QVERIFY(h.item("voiceDuckToggle")->property("checked").toBool());
    QVERIFY(h.item("voiceAutoToggle")->property("checked").toBool());
    QVERIFY(!shown(h.item("voiceMicProblem")));
    uitest::saveShot(h.window, QStringLiteral("voice-settings"));
    // Talk: Voice activation; the key row keeps saying which keys do what
    QVERIFY(h.click("voiceTalkVad"));
    QTRY_COMPARE(v->talkMode(), QStringLiteral("vad"));
    QVERIFY(text(h.item("voiceKeyValue")).startsWith(QStringLiteral("V")));
    // The threshold line moves with a click on the level bar
    QQuickItem* meter = h.item("voiceMeter");
    clickIn(h, meter, 0.8);
    QTRY_VERIFY(v->thresholdDb() > -20 && v->thresholdDb() < -8);
    // A level shows in the bar
    v->injectTone(440, 0.5);
    for (int i = 0; i < 4; ++i) v->tickForTests();
    QTRY_VERIFY(h.item("voiceLevel")->property("value").toDouble() > 0.5);
    // The toggles write to the settings of this PC
    QVERIFY(h.click("voiceNoiseToggle"));
    QVERIFY(h.click("voiceDuckToggle"));
    QVERIFY(h.click("voiceAutoToggle"));
    QTRY_VERIFY(!v->noiseSuppression() && !v->ducking() && !v->autoJoin());
    PlayerSettings again(h.dir.path());
    QVERIFY(!again.voiceNoiseSuppression() && !again.voiceDucking() && !again.voiceAutoJoin());
    QCOMPARE(again.voiceTalkMode(), QStringLiteral("vad"));
    // A microphone that cannot be used says so on this page
    v->setCaptureProblemForTests(false, true);
    QTRY_VERIFY(shown(h.item("voiceMicProblem")));
    QVERIFY(shown(h.item("voiceMicFix")));
    uitest::saveShot(h.window, QStringLiteral("voice-settings-blocked"));
    v->setCaptureProblemForTests(true, false);
    QTRY_VERIFY(shown(h.item("voiceMicRetry")) && !shown(h.item("voiceMicFix")));
    // Leaving the page closes the microphone again
    h.controller->showSettingsSection(QStringLiteral("notifications"));
    QTRY_VERIFY(!v->monitoring());
  }
};

QTEST_MAIN(VoiceUiTest)
#include "voice_ui_test.moc"
