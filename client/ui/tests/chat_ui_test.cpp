// Chat and the panel tabs (0.23, ADR 0033 D4) in the UI against FakeHub: Session · People · Chat tabs and Run · Chat in a run, the Chat tab with
// history, day separators, quick replies, the composer (Enter sends, the echo confirms), the unread badge and a dot with many tabs,
// mentions as toasts, and the People tab. Offscreen; synthetic data only.
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

class ChatUiTest : public QObject {
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
  static QJsonObject chatJson(int seq, const QString& user, const QString& body, const QString& sentAt, const QStringList& mentions = {}) {
    QJsonArray m;
    for (const QString& id : mentions) m.append(id);
    return {{"seq", seq}, {"user_id", user}, {"text", body}, {"sent_at", sentAt}, {"mentions", m}, {"client_id", QStringLiteral("c%1").arg(seq)}};
  }
  static QJsonObject presence(const QString& user, const QString& name) {
    return {{"user_id", user}, {"device_id", user + "d"}, {"state", "online"}, {"display_name", name}};
  }
  // The Chat tab of a Session scope with two earlier messages (one yesterday)
  static void sessionScope(Harness& h, FakeHub& hub) {
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QString old = QDateTime::currentDateTimeUtc().addDays(-1).toString(Qt::ISODate);
    hub.chatLog.insert(QStringLiteral("session:s1"), QJsonArray{chatJson(1, "u_lena", "old news", old), chatJson(2, "u_lena", "hello :wave:", now)});
    hub.sendWs(QStringLiteral("presence_update"), presence("u_lena", "Lena"));
    QTRY_COMPARE(h.controller->onlinePeople().size(), 1);
    h.controller->together()->setScopeProvider([]() { return TogetherController::Scope{QStringLiteral("session"), QStringLiteral("s1"), true}; });
    h.controller->chat()->setScopeProvider([]() { return TogetherController::Scope{QStringLiteral("session"), QStringLiteral("s1"), true}; });
    h.controller->together()->refresh();
  }

 private slots:
  void initTestCase() { uitest::installWarningCounter(); }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // Outside a run the tabs are Session · People · Chat; the Session tab keeps Now, the buttons, Sharing and Quit.
  void sessionTabsAndPeople() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    QVERIFY(shown(h.item("panelTab_game")) && shown(h.item("panelTab_people")) && shown(h.item("panelTab_chat")));
    QVERIFY(h.item("panelTab_game")->property("open").toBool());
    QCOMPARE(text(h.item("panelTab_game")->findChild<QQuickItem*>("panelTabLabel")), QStringLiteral("Session"));
    QVERIFY(shown(h.item("endGameButton")) && shown(h.item("sharingBlock")));
    QVERIFY(!shown(h.item("runInfoBlock")));
    uitest::saveShot(h.window, QStringLiteral("chat-session-tab"));
    // People: nobody else without a shared Session; the two actions
    QVERIFY(h.click("panelTab_people"));
    QTRY_VERIFY(shown(h.item("gamePanelPeople")));
    QVERIFY(!shown(h.item("sharingBlock")) && !shown(h.item("endGameButton")));
    QVERIFY(shown(h.item("peopleReadyCheck")) && shown(h.item("peopleInvite")));
    QVERIFY(!h.item("peopleReadyCheck")->isEnabled());  // no scope
    h.controller->together()->setScopeProvider([]() { return TogetherController::Scope{QStringLiteral("session"), QStringLiteral("s1"), true}; });
    h.controller->chat()->setScopeProvider([]() { return TogetherController::Scope{QStringLiteral("session"), QStringLiteral("s1"), true}; });
    h.controller->together()->refresh();
    QTRY_VERIFY(h.item("peopleReadyCheck")->isEnabled());
    QVERIFY(h.click("peopleReadyCheck"));
    QTRY_VERIFY(std::any_of(hub.wsReceived.begin(), hub.wsReceived.end(), [](const QJsonObject& e) { return e.value("type").toString() == QLatin1String("ready_check_start"); }));
    // "Invite someone…" leads to the Sharing section of the Session tab
    QVERIFY(h.click("peopleInvite"));
    QTRY_VERIFY(h.item("panelTab_game")->property("open").toBool());
    QTRY_VERIFY(shown(h.item("sharingBlock")));
    uitest::saveShot(h.window, QStringLiteral("chat-people-tab"));
  }

  void chatTabShowsHistorySendsWithEnterAndConfirmsTheEcho() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    sessionScope(h, hub);
    ChatController* chat = h.controller->chat();
    const QString me = h.controller->connection()->hubUserId();
    hub.chatEchoUser = me;
    QVERIFY(h.click("panelTab_chat"));
    QTRY_VERIFY(shown(h.item("gamePanelChat")));
    QTRY_COMPARE(chat->messageCount(), 2);
    QVERIFY(chat->panelVisible());
    QTRY_VERIFY(h.item("chatComposer")->property("activeFocus").toBool());  // opening the tab focuses the composer
    QVERIFY(text(h.item("chatHeader")).startsWith(QStringLiteral("SESSION CHAT")));
    QCOMPARE(h.items("chatDay").size(), 2);                // yesterday and today
    QVERIFY(text(h.items("chatText").first()).contains(QString::fromUtf8("\xF0\x9F\x91\x8B")));  // :wave: rendered
    QCOMPARE(h.items("chatQuick_1").size(), 1);
    QCOMPARE(text(h.item("chatQuick_1")), QStringLiteral("Down!"));
    QCOMPARE(text(h.item("chatQuick_2")), QStringLiteral("Gym next"));
    QCOMPARE(text(h.item("chatQuick_3")), QStringLiteral("Wait"));
    QVERIFY(shown(h.item("panelChatHintRow")));
    // Type and press Enter: the text stays as typed, the composer empties, the echo confirms the pending row
    for (const QChar c : QStringLiteral("gg :gg:")) QTest::keyClick(h.window, c.toLatin1());
    QCOMPARE(text(h.item("chatComposer")), QStringLiteral("gg :gg:"));
    QTest::keyClick(h.window, Qt::Key_Return);
    QTRY_COMPARE(text(h.item("chatComposer")), QString());
    QTRY_COMPARE(chat->messageCount(), 3);
    QJsonObject sent;
    for (const QJsonObject& e : hub.wsReceived)
      if (e.value("type").toString() == QLatin1String("chat_send")) sent = e.value("payload").toObject();
    QCOMPARE(sent.value("scope").toString(), QStringLiteral("session"));
    QCOMPARE(sent.value("id").toString(), QStringLiteral("s1"));
    QCOMPARE(sent.value("text").toString(), QStringLiteral("gg :gg:"));
    QTRY_VERIFY(!chat->isPending(sent.value("client_id").toString()));
    QCOMPARE(chat->messageCount(), 3);
    QVERIFY(text(h.items("chatText").first()).contains(QString::fromUtf8("\xF0\x9F\x8E\xAE")));
    // A quick reply sends its text
    QVERIFY(h.click("chatQuick_3"));
    QTRY_COMPARE(chat->messageCount(), 4);
    QCOMPARE(chat->history(1).first().toMap().value("text").toString(), QStringLiteral("Wait"));
    // The counter shows near the limit only
    QVERIFY(!shown(h.item("chatCounter")));
    h.item("chatComposer")->setProperty("text", QString(470, QLatin1Char('x')));
    QTRY_VERIFY(shown(h.item("chatCounter")));
    QCOMPARE(text(h.item("chatCounter")), QStringLiteral("470/500"));
    h.item("chatComposer")->setProperty("text", QString());
    uitest::saveShot(h.window, QStringLiteral("chat-session-chat"));
    // Esc gives the keys back to the game
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!h.item("chatComposer")->property("activeFocus").toBool());
  }

  // The unread count behind the label (four tabs or fewer; with more only a dot, see the live test). A mention toasts, a plain message does not.
  void unreadBadgeDotAndMentionToast() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    sessionScope(h, hub);
    ChatController* chat = h.controller->chat();
    auto* n = h.controller->notifications();
    const QString me = h.controller->connection()->hubUserId();
    QTRY_COMPARE(chat->messageCount(), 2);
    QVERIFY(h.item("panelTab_game")->property("open").toBool());  // the Chat tab is closed
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    auto push = [&](int seq, const QString& body, const QStringList& mentions) {
      QJsonObject o = chatJson(seq, "u_lena", body, now, mentions);
      o.insert("scope", "session");
      o.insert("id", "s1");
      hub.sendWs(QStringLiteral("chat_message"), o);
    };
    push(3, QStringLiteral("anyone there?"), {});
    QTRY_COMPARE(chat->unread(), 1);
    QTRY_VERIFY(shown(h.item("tabBadge")));
    QCOMPARE(text(h.item("tabBadge")), QStringLiteral("1"));
    QVERIFY(!shown(h.item("tabDot")));
    QCOMPARE(n->entries().size(), 0);  // plain messages stay out of the bell
    QCOMPARE(n->toasts().size(), 0);
    const QString myName = QStringLiteral("Max");  // the fake server has no display name for the test device
    chat->setSelf([me]() { return me; }, [myName]() { return myName; });
    push(4, QStringLiteral("@%1 gym next?").arg(myName), {me});
    QTRY_COMPARE(n->toasts().size(), 1);  // a mention always toasts
    QVERIFY(n->toasts().first().toMap().value("text").toString().contains(QStringLiteral("Lena: @%1 gym next?").arg(myName)));
    QCOMPARE(n->entries().first().toMap().value("kind").toString(), QStringLiteral("chat"));
    QVERIFY(n->entries().first().toMap().value("mention").toBool());
    QCOMPARE(n->entries().first().toMap().value("actionLabel").toString(), QStringLiteral("OPEN CHAT"));
    QTRY_COMPARE(text(h.item("tabBadge")), QStringLiteral("2"));
    QVERIFY(h.click("panelTab_chat"));
    QTRY_COMPARE(chat->unread(), 0);
    QTRY_VERIFY(!shown(h.item("tabBadge")));
    int rules = 0;  // the mention row has its rule and a bold name
    for (QQuickItem* r : h.items("chatMentionRule")) rules += r->isVisible() ? 1 : 0;
    QCOMPARE(rules, 1);
    bool bold = false;
    for (QQuickItem* t : h.items("chatText")) bold = bold || text(t).contains(QStringLiteral("<b>@%1</b>").arg(myName));
    QVERIFY(bold);
    // OPEN CHAT: the panel shows the Chat tab (here from the Session tab)
    QVERIFY(h.click("panelTab_game"));
    QTRY_VERIFY(!chat->panelVisible());
    QVERIFY(n->act(0));
    QTRY_VERIFY(chat->panelVisible());
    // The mention while the Chat tab is open does not toast
    n->reset();
    push(5, QStringLiteral("@%1 now").arg(myName), {me});
    QTRY_COMPARE(chat->messageCount(), 5);
    QCOMPARE(n->toasts().size(), 0);
  }

  // In a run the tabs are Run · Chat; the Run tab names the run, the Chat header says RUN CHAT, run events sit between the messages.
  void runTabsAndRunChat() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    startPreviewGame(h);
    h.controller->sessions()->gameStarted(QStringLiteral("g1"), QStringLiteral("Preview"), QStringLiteral("11111111-2222-3333-4444-555555555555"));
    QTRY_VERIFY(h.controller->gameRun().contains("id"));
    const QString runId = h.controller->gameRun().value("id").toString();
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    hub.chatLog.insert(QStringLiteral("run:") + runId, QJsonArray{chatJson(1, "u_lena", "ready when you are", now)});
    hub.sendWs(QStringLiteral("presence_update"), presence("u_lena", "Lena"));
    h.controller->together()->setScopeProvider([runId]() { return TogetherController::Scope{QStringLiteral("run"), runId, true}; });
    h.controller->chat()->setScopeProvider([runId]() { return TogetherController::Scope{QStringLiteral("run"), runId, true}; });
    h.controller->together()->refresh();
    h.controller->chat()->setRunEvents([](const QString&) {
      return QList<ChatController::EventLine>{{QDateTime::currentMSecsSinceEpoch(), 1, QStringLiteral("Lena joined")}};
    });
    QTRY_VERIFY(shown(h.item("panelTab_game")) && shown(h.item("panelTab_chat")));
    QVERIFY(!shown(h.item("panelTab_people")));
    QCOMPARE(text(h.item("panelTab_game")->findChild<QQuickItem*>("panelTabLabel")), QStringLiteral("Run"));
    QVERIFY(shown(h.item("runInfoBlock")));
    QVERIFY(shown(h.item("endGameButton")) && shown(h.item("nowBlock")));
    uitest::saveShot(h.window, QStringLiteral("chat-run-tab"));
    QVERIFY(h.click("panelTab_chat"));
    QTRY_VERIFY(shown(h.item("gamePanelChat")));
    QTRY_COMPARE(h.controller->chat()->messageCount(), 1);
    QVERIFY(text(h.item("chatHeader")).startsWith(QStringLiteral("RUN CHAT")));
    QTRY_COMPARE(h.items("chatEvent").size(), 1);
    QVERIFY(text(h.items("chatEventText").first()).startsWith(QStringLiteral("Lena joined")));
    uitest::saveShot(h.window, QStringLiteral("chat-run-chat"));
  }
};

QTEST_MAIN(ChatUiTest)
#include "chat_ui_test.moc"
