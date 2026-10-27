// Together (0.23) in the UI against FakeHub: the ONLINE list of the AppNav from presence updates (Watch / Invite), the bell with its
// popover, the toast host and Settings > This PC > Notifications. Offscreen; synthetic data only.
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

class TogetherUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("sessions_v1")};
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
  static QJsonObject presence(const QString& user, const QString& device, const QString& state, const QString& name, const QString& title = {},
                              const QString& session = {}) {
    QJsonObject p{{"user_id", user}, {"device_id", device}, {"state", state}, {"display_name", name}};
    if (!title.isEmpty()) p.insert("game_title", title);
    if (!session.isEmpty()) p.insert("session_id", session);
    return p;
  }

 private slots:
  void initTestCase() { uitest::installWarningCounter(); }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void onlineListShowsPeopleWithWatch() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1280, 1000);  // not compact: name and "Playing <game>" on two lines
    QVERIFY(!shown(h.item("navOnlineHead")));
    hub.sendWs(QStringLiteral("presence_update"), presence("u_lena", "d1", "online", "Lena"));
    hub.sendWs(QStringLiteral("presence_update"), presence("u_lena", "d2", "in_game", "Lena", "Example Homebrew", "5d3f6a7b"));
    hub.sendWs(QStringLiteral("presence_update"), presence("u_max", "d3", "online", "Max"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h.item("navOnlineHead")), 8000);
    QTRY_COMPARE(h.controller->onlinePeople().size(), 2);  // people, not devices
    QTRY_COMPARE(text(h.item("navOnlineName")), QStringLiteral("Lena"));
    QTRY_COMPARE(text(h.item("navOnlineSub")), QStringLiteral("Playing Example Homebrew"));
    QTRY_VERIFY(shown(h.item("navOnlineWatch")));
    QVERIFY(!shown(h.item("navOnlineInvite")));  // I share nothing and play no run
    // The Session disappears: no Watch; offline removes the person
    hub.sendWs(QStringLiteral("presence_update"), presence("u_lena", "d2", "in_game", "Lena", "Example Homebrew"));
    QTRY_VERIFY(!shown(h.item("navOnlineWatch")));
    hub.sendWs(QStringLiteral("presence_update"), presence("u_lena", "d2", "offline", "Lena"));
    hub.sendWs(QStringLiteral("presence_update"), presence("u_lena", "d1", "offline", "Lena"));
    QTRY_COMPARE(h.controller->onlinePeople().size(), 1);
    // A lost link empties the list; the server sends a snapshot after the next hello_ack
    h.controller->sessions()->socket()->stop();
    QTRY_COMPARE(h.controller->onlinePeople().size(), 0);
  }

  void bellCountsOpensAndActs() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    auto* n = h.controller->notifications();
    n->notify(QStringLiteral("update"), QStringLiteral("Uzume 9.9.9 is available"), QStringLiteral("WHAT'S NEW"), QStringLiteral("settings:updates"));
    n->notify(QStringLiteral("invite"), QStringLiteral("Lena invited you to watch Example Homebrew"), QStringLiteral("WATCH"), QStringLiteral("watch:none"));
    QCOMPARE(n->unread(), 2);
    QTRY_VERIFY(shown(h.item("navBell")));
    QVERIFY(h.click("navBell"));
    QTRY_VERIFY(shown(h.item("notificationsPopover")) || shown(h.item("notificationEntry")));
    QTRY_VERIFY(shown(h.item("notificationEntry")));
    QCOMPARE(n->unread(), 0);  // opening marks read
    QVERIFY(text(h.item("notificationText")).contains(QStringLiteral("Lena")));  // newest first
    QVERIFY(text(h.item("notificationMeta")).endsWith(QStringLiteral("WATCH")));
    QSignalSpy acted(n, &NotificationCenter::actionRequested);
    QVERIFY(h.click("notificationEntry"));
    QCOMPARE(acted.size(), 1);
    QCOMPARE(acted[0][1].toString(), QStringLiteral("watch:none"));
    QTRY_VERIFY(!shown(h.item("notificationEntry")));  // clicking an entry closes the popover
  }

  void toastsShowAndListOnlyKindsStayInTheBell() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    h.notificationToasts = true;
    QVERIFY(h.start());
    pair(h, hub);
    auto* n = h.controller->notifications();
    n->setMode(QStringLiteral("update"), QStringLiteral("list"));
    n->notify(QStringLiteral("update"), QStringLiteral("quiet one"));
    n->notify(QStringLiteral("invite"), QStringLiteral("Lena invited you"), QStringLiteral("WATCH"), QStringLiteral("watch:none"));
    QTRY_VERIFY(shown(h.item("notificationToast")));
    QCOMPARE(text(h.item("notificationToast")->findChild<QQuickItem*>("toastText")), QStringLiteral("Lena invited you"));
    QCOMPARE(n->toasts().size(), 1);
    QCOMPARE(n->entries().size(), 2);
    QVERIFY(h.click("toastClose"));
    QTRY_COMPARE(n->toasts().size(), 0);
  }

  void notificationsSettingsPage() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->showSettingsSection(QStringLiteral("notifications"));
    QTRY_VERIFY(shown(h.item("notificationsSection")));
    QVERIFY(shown(h.item("notifyInviteRow")));
    QVERIFY(shown(h.item("notifyRunEventRow")));
    QVERIFY(!shown(h.item("notifyServerProblemRow")));  // admins only
    QVERIFY(shown(h.item("notifyQuietRow")));
    QVERIFY(shown(h.item("notifySoundRow")));
    auto* n = h.controller->notifications();
    QCOMPARE(n->modes().value("invite").toString(), QStringLiteral("list"));  // the harness keeps toasts off
    QVERIFY(h.click("notifyInviteToast"));
    QTRY_COMPARE(n->modes().value("invite").toString(), QStringLiteral("toast"));
    QVERIFY(h.click("notifyInviteList"));
    QTRY_COMPARE(n->modes().value("invite").toString(), QStringLiteral("list"));
    QVERIFY(n->quietFullscreen());
    QVERIFY(h.click("notifyQuietToggle"));
    QTRY_VERIFY(!n->quietFullscreen());
    QVERIFY(h.click("notifySoundToggle"));
    QTRY_VERIFY(n->sound());
  }
};

QTEST_MAIN(TogetherUiTest)
#include "together_ui_test.moc"
