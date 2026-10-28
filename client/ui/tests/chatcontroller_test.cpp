// ChatController (0.23, ADR 0033 D4) without QML: dedupe by seq and client_id, pending -> confirmed, history merge, unread, mention detection,
// emote rendering and the rows (day separators, run events, grouping). The Hub socket is never opened; messages are emitted directly.
#include <QSignalSpy>
#include <QUuid>
#include <ctime>
#include <QtTest>

#include "chatcontroller.h"

using namespace uzume;
using namespace uzume::ui;

namespace {
const QString kMe = QStringLiteral("u_max");
const QString kLena = QStringLiteral("u_lena");

ChatMessage msg(qint64 seq, const QString& user, const QString& text, const QString& cid = {}, const QStringList& mentions = {}, const QString& scope = QStringLiteral("run"),
                const QString& id = QStringLiteral("r1"), const QString& sentAt = QStringLiteral("2026-05-01T12:00:00Z")) {
  ChatMessage m;
  m.scope = scope;
  m.id = id;
  m.seq = seq;
  m.userId = user;
  m.text = text;
  m.sentAt = sentAt;
  m.mentions = mentions;
  m.clientId = cid.isEmpty() ? QStringLiteral("c%1").arg(seq) : cid;
  return m;
}
}  // namespace

class ChatControllerTest : public QObject {
  Q_OBJECT

  struct Sent {
    QString scope, id, clientId, text;
  };
  struct Fixture {
    HubSocket socket{nullptr};
    TogetherController tc{&socket};
    ChatController chat{&tc, &socket};
    TogetherController::Scope scope{QStringLiteral("run"), QStringLiteral("r1"), true};
    QList<Sent> sent;
    bool linkOpen = true;
    qint64 clock = 1777636800000;  // 2026-05-01T12:00:00Z
    struct Fetch {
      QString scope, id;
      qint64 after;
      int limit;
      std::function<void(const ChatPage&)> cb;
    };
    QList<Fetch> fetches;
    QList<ChatController::EventLine> events;
    Fixture() {
      tc.setFeatureCheck([] { return true; });
      tc.setSelf([] { return kMe; });
      tc.setScopeProvider([this] { return scope; });
      chat.setSelf([] { return kMe; }, [] { return QStringLiteral("Max"); });
      chat.setNameResolver([](const QString& id) { return id == kLena ? QStringLiteral("Lena") : id == kMe ? QStringLiteral("Max") : QString(); });
      chat.setScopeInfo([](const QString&, const QString&) { return QStringLiteral("Ember Route"); },
                        [](const QString&, const QString&) { return QVariantList{QVariantMap{}, QVariantMap{}, QVariantMap{}, QVariantMap{}}; });
      chat.setSender([this](const QString& s, const QString& i, const QString& c, const QString& t) {
        if (!linkOpen) return false;
        sent.append({s, i, c, t});
        return true;
      });
      chat.setFetch([this](const QString& s, const QString& i, qint64 a, int l, std::function<void(const ChatPage&)> cb) { fetches.append({s, i, a, l, std::move(cb)}); });
      chat.setRunEvents([this](const QString&) { return events; });
      chat.setClock([this] { return clock; });
      tc.refresh();
      chat.refresh();
    }
    void live(const ChatMessage& m) { emit socket.chatReceived(m); }
    int rowsOfType(const QString& type) const {
      int n = 0;
      for (const QVariant& v : chat.rows())
        if (v.toMap().value(QStringLiteral("type")).toString() == type) ++n;
      return n;
    }
  };

 private slots:
  void initTestCase() {
    qputenv("TZ", "UTC");  // the day separators follow the local time zone
#ifndef Q_OS_WIN
    tzset();
#endif
  }

  void liveMessagesAreOrderedAndDeduped() {
    Fixture f;
    f.live(msg(2, kLena, "second"));
    f.live(msg(1, kLena, "first"));
    f.live(msg(2, kLena, "second again"));                // same seq
    QCOMPARE(f.chat.messageCount(), 2);
    const QVariantList hist = f.chat.history(10);
    QCOMPARE(hist.at(0).toMap().value("text").toString(), QStringLiteral("first"));
    QCOMPARE(hist.at(1).toMap().value("text").toString(), QStringLiteral("second"));
  }

  void pendingBecomesConfirmedByTheEcho() {
    Fixture f;
    QVERIFY(f.chat.send(QStringLiteral("  hello :gg:  ")));
    QCOMPARE(f.sent.size(), 1);
    QCOMPARE(f.sent[0].scope, QStringLiteral("run"));
    QCOMPARE(f.sent[0].id, QStringLiteral("r1"));
    QCOMPARE(f.sent[0].text, QStringLiteral("hello :gg:"));  // trimmed, shortcodes stay
    const QString cid = f.sent[0].clientId;
    QVERIFY(!QUuid(cid).isNull());
    QVERIFY(f.chat.isPending(cid));
    QCOMPARE(f.chat.messageCount(), 1);
    QVariantMap row = f.chat.rows().first().toMap();
    QVERIFY(row.value("pending").toBool());
    QVERIFY(row.value("mine").toBool());
    QCOMPARE(row.value("plain").toString(), QStringLiteral("hello :gg:"));
    QVERIFY(row.value("html").toString().contains(QString::fromUtf8("\xF0\x9F\x8E\xAE")));
    // The Hub's echo carries the same client_id: no second message, no unread, no mention
    QSignalSpy mention(&f.chat, &ChatController::mentioned);
    f.live(msg(7, kMe, "hello :gg:", cid));
    QCOMPARE(f.chat.messageCount(), 1);
    QVERIFY(!f.chat.isPending(cid));
    row = f.chat.rows().first().toMap();
    QVERIFY(!row.value("pending").toBool());
    QCOMPARE(row.value("seq").toLongLong(), 7LL);
    QCOMPARE(f.chat.unread(), 0);
    QCOMPARE(mention.count(), 0);
    // The echo again (a resend answered by the Hub) changes nothing
    f.live(msg(7, kMe, "hello :gg:", cid));
    QCOMPARE(f.chat.messageCount(), 1);
  }

  void sendRules() {
    Fixture f;
    QVERIFY(!f.chat.send(QStringLiteral("   ")));
    QVERIFY(!f.chat.send(QString(501, QLatin1Char('x'))));
    QVERIFY(f.chat.send(QString(500, QLatin1Char('x'))));
    QCOMPARE(f.chat.charCount(QString::fromUtf8("a\xF0\x9F\x8E\xAE")), 2);  // code points as the Hub counts them
    f.linkOpen = false;
    QVERIFY(!f.chat.send(QStringLiteral("no link")));  // nothing is queued without a link
    QCOMPARE(f.chat.messageCount(), 1);
    f.scope = {};
    f.tc.refresh();
    f.chat.refresh();
    QVERIFY(!f.chat.available());
    f.linkOpen = true;
    QVERIFY(!f.chat.send(QStringLiteral("no scope")));
  }

  void anUnechoedMessageFailsAndCanBeResent() {
    Fixture f;
    QVERIFY(f.chat.send(QStringLiteral("hi")));
    const QString cid = f.sent[0].clientId;
    f.clock += ChatController::kPendingMs + 1;
    f.chat.tick();
    QVERIFY(!f.chat.isPending(cid));
    QVERIFY(f.chat.rows().first().toMap().value("failed").toBool());
    f.chat.retry(cid);
    QCOMPARE(f.sent.size(), 2);
    QCOMPARE(f.sent[1].clientId, cid);  // the same client_id: the Hub answers a known one with the stored message
    QVERIFY(f.chat.isPending(cid));
    f.live(msg(3, kMe, "hi", cid));
    QCOMPARE(f.chat.messageCount(), 1);
    QVERIFY(!f.chat.rows().first().toMap().value("failed").toBool());
  }

  void historyIsMergedWithLiveMessages() {
    Fixture f;
    QCOMPARE(f.fetches.size(), 1);  // the scope appeared: the run history is fetched
    QCOMPARE(f.fetches[0].scope, QStringLiteral("run"));
    QCOMPARE(f.fetches[0].after, 0LL);
    f.live(msg(5, kLena, "live"));  // arrives before the history
    QVERIFY(f.chat.send(QStringLiteral("mine")));
    const QString cid = f.sent[0].clientId;
    ChatPage page;
    page.ok = true;
    for (int i = 1; i <= 5; ++i) page.messages.append(msg(i, kLena, QStringLiteral("old %1").arg(i)));
    page.messages.append(msg(6, kMe, "mine", cid));  // my own message came with the history
    f.fetches[0].cb(page);
    QCOMPARE(f.chat.messageCount(), 6);  // seq 1..5 (the live 5 wins over the history's 5) and my own 6, no second copy of it
    const QVariantList h = f.chat.history(50);
    QCOMPARE(h.at(0).toMap().value("seq").toLongLong(), 1LL);
    QCOMPARE(h.last().toMap().value("seq").toLongLong(), 6LL);
    QVERIFY(!f.chat.isPending(cid));
    QCOMPARE(f.chat.unread(), 1);  // only the live message counts; the history is never unread
    QVERIFY(!f.chat.loading());
  }

  void longRunHistoryKeepsTheLastHundred() {
    Fixture f;
    ChatPage p1;
    p1.ok = true;
    for (int i = 1; i <= 150; ++i) p1.messages.append(msg(i, kLena, QStringLiteral("m%1").arg(i)));
    p1.nextAfterSeq = 150;
    f.fetches[0].cb(p1);
    QCOMPARE(f.fetches.size(), 2);  // paged on
    QCOMPARE(f.fetches[1].after, 150LL);
    ChatPage p2;
    p2.ok = true;
    for (int i = 151; i <= 230; ++i) p2.messages.append(msg(i, kLena, QStringLiteral("m%1").arg(i)));
    f.fetches[1].cb(p2);
    QCOMPARE(f.chat.messageCount(), ChatController::kHistoryRun);
    QCOMPARE(f.chat.history(1).first().toMap().value("seq").toLongLong(), 230LL);
    QCOMPARE(f.chat.history(500).first().toMap().value("seq").toLongLong(), 131LL);
  }

  void aReconnectFetchesWhatFollowsTheLastSeq() {
    Fixture f;
    ChatPage page;
    page.ok = true;
    page.messages.append(msg(3, kLena, "x"));
    f.fetches[0].cb(page);
    f.fetches.clear();
    emit f.socket.stateChanged(HubSocket::State::Open);
    QCOMPARE(f.fetches.size(), 1);
    QCOMPARE(f.fetches[0].after, 3LL);
    // Messages found this way are not news
    ChatPage gap;
    gap.ok = true;
    gap.messages.append(msg(4, kLena, "while away"));
    f.fetches[0].cb(gap);
    QCOMPARE(f.chat.messageCount(), 2);
    QCOMPARE(f.chat.unread(), 0);
  }

  void unreadCountsOnlyWhileTheTabIsClosed() {
    Fixture f;
    QSignalSpy unread(&f.chat, &ChatController::unreadChanged);
    f.live(msg(1, kLena, "a"));
    f.live(msg(2, kLena, "b"));
    f.live(msg(3, kMe, "mine", QStringLiteral("zz")));  // my own message from another device
    QCOMPARE(f.chat.unread(), 2);
    f.chat.setPanelVisible(true);  // opening the tab clears it
    QCOMPARE(f.chat.unread(), 0);
    f.live(msg(4, kLena, "c"));
    QCOMPARE(f.chat.unread(), 0);  // visible: read as it arrives
    f.chat.setPanelVisible(false);
    f.live(msg(5, kLena, "d"));
    QCOMPARE(f.chat.unread(), 1);
    QVERIFY(unread.count() >= 2);
  }

  void unreadIsPerScope() {
    Fixture f;
    f.live(msg(1, kLena, "in the session", {}, {}, QStringLiteral("session"), QStringLiteral("s9")));  // not my current scope
    QCOMPARE(f.chat.unread(), 0);
    f.scope = {QStringLiteral("session"), QStringLiteral("s9"), false};
    f.tc.refresh();
    f.chat.refresh();
    QCOMPARE(f.chat.scopeKind(), QStringLiteral("session"));
    QCOMPARE(f.chat.unread(), 1);
    QCOMPARE(f.chat.messageCount(), 1);
    QCOMPARE(f.fetches.last().scope, QStringLiteral("session"));  // the ring is fetched on the first visit
  }

  void mentionOfMeRaisesOneSignalWhileTheTabIsClosed() {
    Fixture f;
    QSignalSpy mention(&f.chat, &ChatController::mentioned);
    f.live(msg(1, kLena, "hello @Max", {}, {kMe}));
    QCOMPARE(mention.count(), 1);
    QCOMPARE(mention.at(0).at(0).toString(), QStringLiteral("run"));
    QCOMPARE(mention.at(0).at(2).toString(), QStringLiteral("Lena"));
    QCOMPARE(mention.at(0).at(3).toString(), QStringLiteral("hello @Max"));
    f.live(msg(2, kLena, "@someone else", {}, {QStringLiteral("u_sam")}));  // names somebody else
    f.live(msg(3, kLena, "no mention"));
    QCOMPARE(mention.count(), 1);
    f.chat.setPanelVisible(true);
    f.live(msg(4, kLena, "@max again", {}, {kMe}));  // on screen: no notification
    QCOMPARE(mention.count(), 1);
    // History never notifies
    f.chat.setPanelVisible(false);
    ChatPage page;
    page.ok = true;
    page.messages.append(msg(10, kLena, "@max old", {}, {kMe}));
    f.fetches[0].cb(page);
    QCOMPARE(mention.count(), 1);
    // The row of a message that names me is marked
    QVERIFY(f.chat.rows().first().toMap().value("mention").toBool() || f.chat.rows().last().toMap().value("mention").toBool());
  }

  void emotesAreRenderedInTheDisplayOnly() {
    QCOMPARE(ChatController::renderEmotes(QStringLiteral(":ok:")), QString::fromUtf8("\xF0\x9F\x91\x8D"));
    QCOMPARE(ChatController::renderEmotes(QStringLiteral(":wave: hi")), QString::fromUtf8("\xF0\x9F\x91\x8B hi"));
    QCOMPARE(ChatController::renderEmotes(QStringLiteral(":lol::hype:")), QString::fromUtf8("\xF0\x9F\x98\x82\xF0\x9F\x94\xA5"));
    QCOMPARE(ChatController::renderEmotes(QStringLiteral(":sad: :GG:")), QString::fromUtf8("\xF0\x9F\x98\xA2 \xF0\x9F\x8E\xAE"));
    QCOMPARE(ChatController::renderEmotes(QStringLiteral(":nope: 10:30")), QStringLiteral(":nope: 10:30"));  // unknown codes stay
    // HTML in a message is text
    QCOMPARE(ChatController::renderHtml(QStringLiteral("<b>x</b> & y"), QStringLiteral("Max"), false), QStringLiteral("&lt;b&gt;x&lt;/b&gt; &amp; y"));
    // The mention of me is bold, with or without the spaces of a display name, and not a longer name
    QCOMPARE(ChatController::renderHtml(QStringLiteral("hey @max!"), QStringLiteral("Max"), true), QStringLiteral("hey <b>@max</b>!"));
    QCOMPARE(ChatController::renderHtml(QStringLiteral("hey @maxine"), QStringLiteral("Max"), true), QStringLiteral("hey @maxine"));
    QCOMPARE(ChatController::renderHtml(QStringLiteral("@Max Power and @MaxPower"), QStringLiteral("Max Power"), true),
             QStringLiteral("<b>@Max Power</b> and <b>@MaxPower</b>"));
    QCOMPARE(ChatController::renderHtml(QStringLiteral("hey @max"), QStringLiteral("Max"), false), QStringLiteral("hey @max"));
  }

  void rowsHaveDaySeparatorsGroupingEventsAndTheHeader() {
    Fixture f;
    f.live(msg(1, kLena, "yesterday", {}, {}, "run", "r1", "2026-04-30T20:00:00Z"));
    f.live(msg(2, kLena, "one", {}, {}, "run", "r1", "2026-05-01T09:00:00Z"));
    f.live(msg(3, kLena, "two", {}, {}, "run", "r1", "2026-05-01T09:01:00Z"));
    f.live(msg(4, kMe, "three", {}, {}, "run", "r1", "2026-05-01T09:02:00Z"));
    f.events = {{QDateTime::fromString("2026-05-01T09:01:30Z", Qt::ISODate).toMSecsSinceEpoch(), 1, QStringLiteral("Lena joined")}};
    f.chat.noteRunEvent(QStringLiteral("r1"));
    QCOMPARE(f.rowsOfType("day"), 2);
    QCOMPARE(f.rowsOfType("event"), 1);
    QCOMPARE(f.rowsOfType("msg"), 4);
    QCOMPARE(f.chat.header(), QStringLiteral("RUN CHAT · EMBER ROUTE · 4 PEOPLE"));
    // Newest first: three, event, two, one, TODAY, yesterday, day
    const QVariantList r = f.chat.rows();
    QCOMPARE(r.at(0).toMap().value("plain").toString(), QStringLiteral("three"));
    QCOMPARE(r.at(1).toMap().value("type").toString(), QStringLiteral("event"));
    QCOMPARE(r.at(2).toMap().value("plain").toString(), QStringLiteral("two"));
    QVERIFY(!r.at(2).toMap().value("head").toBool());  // "two" follows "one" of the same person within 5 minutes: no second head
    QVERIFY(r.at(0).toMap().value("head").toBool());     // "three" is another person (after the event)
  }

  void sessionHeaderAndNoRunEvents() {
    Fixture f;
    f.scope = {QStringLiteral("session"), QStringLiteral("s1"), true};
    f.tc.refresh();
    f.chat.refresh();
    f.events = {{1777636800000, 1, QStringLiteral("must not show")}};
    f.chat.noteRunEvent(QStringLiteral("r1"));
    QCOMPARE(f.chat.header(), QStringLiteral("SESSION CHAT · 4 PEOPLE"));
    QCOMPARE(f.rowsOfType("event"), 0);
  }

  void resetForgetsEverything() {
    Fixture f;
    f.live(msg(1, kLena, "x"));
    f.chat.reset();
    QCOMPARE(f.chat.messageCount(), 0);
    QCOMPARE(f.chat.unread(), 0);
  }
};

QTEST_MAIN(ChatControllerTest)
#include "chatcontroller_test.moc"
