// Together (0.23, ADR 0033 D1 and D2): the online roster (people, not devices) and the notification center (kinds, settings, cap, quiet in
// fullscreen, toast queue) without QML.
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "notificationcenter.h"
#include "onlineroster.h"
#include "playersettings.h"

using namespace uzume;
using namespace uzume::ui;

namespace {
PresenceUpdate presence(const QString& user, const QString& device, const QString& state, const QString& name = {}, const QString& title = {},
                        const QString& session = {}) {
  PresenceUpdate u;
  u.userId = user;
  u.deviceId = device;
  u.state = state;
  u.displayName = name;
  u.gameTitle = title;
  u.sessionId = session;
  return u;
}
}  // namespace

class TogetherTest : public QObject {
  Q_OBJECT

 private slots:
  // ---------------------------------------------------------------- roster
  void rosterListsPeopleNotDevices() {
    OnlineRoster r;
    QSignalSpy changed(&r, &OnlineRoster::changed);
    r.apply(presence("u1", "d1", "online", "Lena"));
    r.apply(presence("u1", "d2", "online", "Lena"));
    r.apply(presence("u2", "d3", "online", "Max"));
    const auto people = r.people();
    QCOMPARE(people.size(), 2);
    QCOMPARE(people[0].name, QStringLiteral("Lena"));
    QCOMPARE(people[0].devices, 2);
    QCOMPARE(people[0].state, QStringLiteral("online"));
    QCOMPARE(changed.size(), 3);  // the second device of Lena changed the device count, so it counts
    // Excluding myself
    QCOMPARE(r.people(QStringLiteral("u1")).size(), 1);
    QCOMPARE(r.people(QStringLiteral("u1"))[0].name, QStringLiteral("Max"));
  }

  void inGameWinsAndSortsFirst() {
    OnlineRoster r;
    r.apply(presence("u1", "d1", "online", "Lena"));
    r.apply(presence("u1", "d2", "in_game", "Lena", "Example Homebrew"));
    r.apply(presence("u2", "d3", "online", "Anna"));
    const auto people = r.people();
    QCOMPARE(people[0].userId, QStringLiteral("u1"));
    QCOMPARE(people[0].state, QStringLiteral("in_game"));
    QCOMPARE(people[0].gameTitle, QStringLiteral("Example Homebrew"));
    QCOMPARE(people[1].userId, QStringLiteral("u2"));
    const QVariantMap m = r.peopleVariant()[0].toMap();
    QCOMPARE(m.value("sub").toString(), QStringLiteral("Playing Example Homebrew"));
    QCOMPARE(r.peopleVariant()[1].toMap().value("sub").toString(), QStringLiteral("Online"));
    QVERIFY(m.value("color").toInt() >= 1 && m.value("color").toInt() <= 8);
  }

  void offlineRemovesDeviceThenPerson() {
    OnlineRoster r;
    r.apply(presence("u1", "d1", "online", "Lena"));
    r.apply(presence("u1", "d2", "in_game", "Lena", "Game"));
    r.apply(presence("u1", "d2", "offline"));
    QCOMPARE(r.people().size(), 1);
    QCOMPARE(r.people()[0].state, QStringLiteral("online"));
    r.apply(presence("u1", "d1", "offline"));
    QVERIFY(r.people().isEmpty());
    QVERIFY(r.isEmpty());
    QSignalSpy changed(&r, &OnlineRoster::changed);
    r.apply(presence("u1", "d1", "offline"));  // already gone: no change, no signal
    QCOMPARE(changed.size(), 0);
  }

  void sessionIdAppearsAndDisappears() {
    OnlineRoster r;
    r.apply(presence("u1", "d1", "in_game", "Lena", "Game"));
    QVERIFY(r.people()[0].sessionId.isEmpty());
    QSignalSpy changed(&r, &OnlineRoster::changed);
    r.apply(presence("u1", "d1", "in_game", "Lena", "Game", "s-1"));
    QCOMPARE(r.people()[0].sessionId, QStringLiteral("s-1"));
    QCOMPARE(changed.size(), 1);
    r.apply(presence("u1", "d1", "in_game", "Lena", "Game", "s-1"));  // same again: nothing
    QCOMPARE(changed.size(), 1);
    r.apply(presence("u1", "d1", "in_game", "Lena", "Game"));  // no longer visible
    QVERIFY(r.people()[0].sessionId.isEmpty());
    QCOMPARE(changed.size(), 2);
  }

  void sessionOnAnotherDeviceStillWatchable() {
    OnlineRoster r;
    r.apply(presence("u1", "d1", "in_game", "Lena", "Game A", "s-1"));
    r.apply(presence("u1", "d2", "in_game", "Lena", "Game B"));
    QCOMPARE(r.people()[0].sessionId, QStringLiteral("s-1"));
    QCOMPARE(r.people()[0].gameTitle, QStringLiteral("Game A"));  // the device with the Session is the one shown
  }

  void clearEmptiesTheSnapshot() {
    OnlineRoster r;
    r.apply(presence("u1", "d1", "online", "Lena"));
    QSignalSpy changed(&r, &OnlineRoster::changed);
    r.clear();
    QVERIFY(r.people().isEmpty());
    QCOMPARE(changed.size(), 1);
    r.clear();
    QCOMPARE(changed.size(), 1);
    r.apply(presence("", "d1", "online", "Nobody"));  // no user id: ignored
    r.apply(presence("u9", "d1", "away", "X"));        // unknown state: ignored
    QVERIFY(r.isEmpty());
  }

  // ---------------------------------------------------------------- notification center
  void settingsRouteKindsToToastOrList() {
    QTemporaryDir dir;
    PlayerSettings s(dir.path());
    NotificationCenter n(&s);
    qint64 t = 1000;
    n.setClock([&t]() { return t; });
    n.notify("invite", "Lena invited you", "WATCH", "watch:s-1");
    QCOMPARE(n.toasts().size(), 1);  // default: Toast
    n.setMode("session_started", "list");
    n.notify("session_started", "Lena started a Session", "WATCH", "watch:s-2");
    QCOMPARE(n.toasts().size(), 1);  // list only: bell, no toast
    QCOMPARE(n.entries().size(), 2);
    QCOMPARE(n.unread(), 2);
    QCOMPARE(n.entries()[0].toMap().value("kind").toString(), QStringLiteral("session_started"));  // newest first
    QVERIFY(!n.modes().isEmpty());
    QCOMPARE(n.modes().value("session_started").toString(), QStringLiteral("list"));
    // Persisted per this PC
    PlayerSettings again(dir.path());
    QCOMPARE(again.notificationMode("session_started"), QStringLiteral("list"));
    QCOMPARE(again.notificationMode("invite"), QStringLiteral("toast"));
    QVERIFY(!s.setNotificationMode("bogus", "toast"));
    QVERIFY(!s.setNotificationMode("invite", "loud"));
  }

  void quietInFullscreenOnlyLetsMentionsToast() {
    QTemporaryDir dir;
    PlayerSettings s(dir.path());
    NotificationCenter n(&s);
    QVERIFY(n.quietFullscreen());  // default on
    n.setFullscreen(true);
    n.notify("invite", "Lena invited you", "WATCH", "watch:s-1");
    QCOMPARE(n.toasts().size(), 0);
    QCOMPARE(n.entries().size(), 1);  // still in the bell
    n.notify("chat", "Max: @you hello", "OPEN CHAT", "chat:1", true);
    QCOMPARE(n.toasts().size(), 1);
    n.setQuietFullscreen(false);
    n.notify("invite", "Anna invited you", "WATCH", "watch:s-3");
    QCOMPARE(n.toasts().size(), 2);
    n.setFullscreen(false);
    n.setQuietFullscreen(true);
    n.notify("run_event", "Run paused", "OPEN RUN", "run:1");
    QCOMPARE(n.toasts().size(), 3);
  }

  void listIsCappedAtFifty() {
    QTemporaryDir dir;
    PlayerSettings s(dir.path());
    NotificationCenter n(&s);
    for (const char* k : {"invite", "session_started", "run_event", "update", "server_problem"}) n.setMode(QLatin1String(k), "list");
    for (int i = 0; i < 60; ++i) n.notify("invite", QStringLiteral("Entry %1").arg(i));
    const QVariantList e = n.entries();
    QCOMPARE(e.size(), NotificationCenter::kMaxEntries);
    QCOMPARE(e[0].toMap().value("text").toString(), QStringLiteral("Entry 59"));
    QCOMPARE(e.last().toMap().value("text").toString(), QStringLiteral("Entry 10"));
    QCOMPARE(n.unread(), 50);
    n.markAllRead();
    QCOMPARE(n.unread(), 0);
    n.reset();
    QVERIFY(n.entries().isEmpty());
  }

  void toastQueueShowsThreeForSixSeconds() {
    QTemporaryDir dir;
    PlayerSettings s(dir.path());
    NotificationCenter n(&s);
    qint64 t = 10000;
    n.setClock([&t]() { return t; });
    for (int i = 0; i < 5; ++i) n.notify("invite", QStringLiteral("Invite %1").arg(i));
    QCOMPARE(n.toasts().size(), 3);
    QCOMPARE(n.toasts()[0].toMap().value("text").toString(), QStringLiteral("Invite 0"));
    t += 5900;
    n.tick();
    QCOMPARE(n.toasts().size(), 3);
    t += 200;  // six seconds passed: the first three go, the other two come
    n.tick();
    QCOMPARE(n.toasts().size(), 2);
    QCOMPARE(n.toasts()[0].toMap().value("text").toString(), QStringLiteral("Invite 3"));
    n.dismissToast(n.toasts()[0].toMap().value("id").toInt());
    QCOMPARE(n.toasts().size(), 1);
    QCOMPARE(n.entries().size(), 5);  // dismissing a toast keeps the entry
  }

  void actRunsTheActionAndMarksRead() {
    QTemporaryDir dir;
    PlayerSettings s(dir.path());
    NotificationCenter n(&s);
    QSignalSpy action(&n, &NotificationCenter::actionRequested);
    n.notify("invite", "first", "WATCH", "watch:s-1");
    n.notify("update", "second", "WHAT'S NEW", "settings:updates");
    QVERIFY(!n.act(2));
    QVERIFY(!n.act(-1));
    QVERIFY(n.act(1));  // index 1 = the older one
    QCOMPARE(action.size(), 1);
    QCOMPARE(action[0][0].toString(), QStringLiteral("invite"));
    QCOMPARE(action[0][1].toString(), QStringLiteral("watch:s-1"));
    QCOMPARE(n.unread(), 1);
    QCOMPARE(n.toasts().size(), 1);  // the toast of the acted entry is gone
    n.notify("   ", "ignored");       // blank kind is fine, blank text is not
    n.notify("invite", "  ");
    QCOMPARE(n.entries().size(), 3);
  }

  void soundPlaysOncePerShownToast() {
    QTemporaryDir dir;
    PlayerSettings s(dir.path());
    NotificationCenter n(&s);
    int tones = 0;
    n.setSoundHook([&tones]() { ++tones; });
    n.notify("invite", "a");
    QCOMPARE(tones, 0);  // sound is off by default
    n.setSound(true);
    n.notify("invite", "b");
    QCOMPARE(tones, 1);
    n.setMode("invite", "list");
    n.notify("invite", "c");  // no toast, no tone
    QCOMPARE(tones, 1);
    QVERIFY(PlayerSettings(dir.path()).notifySound());
  }
};

QTEST_GUILESS_MAIN(TogetherTest)
#include "together_test.moc"
