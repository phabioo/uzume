// Run recap model (counting, day bucketing, mappings, core events, screenshots, tiles), the HTML export, and the loader that pages the
// whole event log through the fake Hub. Dummy data only.
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "fakehub.h"
#include "hubconnection.h"
#include "profilestore.h"
#include "runrecap.h"

using namespace uzume;
using State = HubConnection::State;

namespace {

const QString kSoul = QStringLiteral("soullink");
const QString kU1 = QStringLiteral("u1");
const QString kU2 = QStringLiteral("u2");
const QString kU3 = QStringLiteral("u3");

RunInfo makeRun(const QString& status = QStringLiteral("finished")) {
  RunInfo r;
  r.runId = QStringLiteral("11111111-2222-3333-4444-555555555555");
  r.name = QStringLiteral("Ember Route");
  r.status = status;
  r.createdAt = QStringLiteral("2026-10-01T10:00:00Z");
  r.updatedAt = QStringLiteral("2026-10-03T12:00:00Z");
  r.plugins = {{kSoul, QStringLiteral("1.0.0"), QString()}};
  for (const auto& p : {std::tuple{kU1, QStringLiteral("Max"), QStringLiteral("joined"), 0}, std::tuple{kU2, QStringLiteral("Lena"), QStringLiteral("joined"), 1},
                        std::tuple{kU3, QStringLiteral("Jonas"), QStringLiteral("invited"), -1}}) {
    RunParticipant x;
    x.userId = std::get<0>(p);
    x.userName = std::get<1>(p);
    x.status = std::get<2>(p);
    x.color = std::get<3>(p);
    r.participants.append(x);
  }
  return r;
}

RunEvent ev(qint64 seq, const QString& when, const QString& user, const QString& plugin, const QString& type, const QString& text = {}) {
  RunEvent e;
  e.seq = seq;
  e.createdAt = when;
  e.userId = user;
  e.userName = user == kU1 ? QStringLiteral("Max") : user == kU2 ? QStringLiteral("Lena") : QString();
  e.pluginId = plugin;
  e.source = user.isEmpty() ? QStringLiteral("hub") : QStringLiteral("automatic");
  e.type = type;
  e.text = text;
  return e;
}

RecapMappings soulMap() {
  return {{kSoul, {{QStringLiteral("soullink.catch"), QStringLiteral("catch")},
                   {QStringLiteral("soullink.death"), QStringLiteral("lost")},
                   {QStringLiteral("soullink.badge"), QStringLiteral("badge")},
                   {QStringLiteral("soullink.correction"), QStringLiteral("note")}}}};
}

RecapInput baseInput() {
  RecapInput in;
  in.run = makeRun();
  in.mappings = soulMap();
  in.zone = QTimeZone::utc();
  in.now = QDateTime::fromString(QStringLiteral("2026-10-10T00:00:00Z"), Qt::ISODate);
  return in;
}

const RecapLane* laneOf(const RecapModel& m, const QString& id) {
  for (const RecapLane& l : m.lanes)
    if (l.userId == id) return &l;
  return nullptr;
}

}  // namespace

class RunRecapTest : public QObject {
  Q_OBJECT
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;

  void connectHub() {
    dir_ = std::make_unique<QTemporaryDir>();
    profiles_ = std::make_unique<ProfileStore>(dir_->filePath(QStringLiteral("data")));
    creds_ = std::make_unique<MemoryCredentialStore>();
    hub_ = std::make_unique<FakeHub>(QStringLiteral("a"));
    hub_->features = {QStringLiteral("saves_v1"), QStringLiteral("runs_v1")};
    QVERIFY(hub_->start());
    conn_ = std::make_unique<HubConnection>(profiles_.get(), creds_.get());
    conn_->setPollIntervalMs(50);
    conn_->connectToAddress(hub_->address());
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsTrustConfirmation, 8000);
    conn_->confirmTrust();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsPairing, 8000);
    hub_->decision = FakeHub::Decision::Approve;
    conn_->requestPairing();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::Connected, 8000);
  }

 private slots:
  void groupsOverlappingGlyphs() {
    auto e = [](double pos, const QString& glyph, const QString& text) {
      return QVariant(QVariantMap{{"pos", pos}, {"glyph", glyph}, {"glyphChar", "■"}, {"text", text}, {"day", 2}});
    };
    // Two at the same moment, one 5 px away (grouped on 500 px), one far away, one 100 px away.
    const QVariantList ev{e(0.2, "catch", "A"), e(0.2, "lost", "B"), e(0.21, "note", "C"), e(0.5, "badge", "D")};
    QVariantList g = groupRecapEvents(ev, 500, 16);
    QCOMPARE(g.size(), 2);
    QCOMPARE(g.at(0).toMap().value("count").toInt(), 3);
    QCOMPARE(g.at(0).toMap().value("extra").toInt(), 2);
    QCOMPARE(g.at(0).toMap().value("glyph").toString(), QString("catch"));
    QVERIFY(g.at(0).toMap().value("tip").toString().contains("lost: B"));
    QCOMPARE(g.at(1).toMap().value("count").toInt(), 1);
    QVERIFY(g.at(1).toMap().value("tip").toString().contains("day 2"));
    // A wider track separates them; deterministic for equal input.
    QCOMPARE(groupRecapEvents(ev, 5000, 16).size(), 3);
    QCOMPARE(groupRecapEvents(ev, 500, 16), g);
    // The list is capped: first 5, then "and N more".
    QVariantList many;
    for (int i = 0; i < 8; ++i) many.append(e(0.3, "catch", QString("E%1").arg(i)));
    g = groupRecapEvents(many, 500, 16);
    QCOMPARE(g.size(), 1);
    const QString tip = g.at(0).toMap().value("tip").toString();
    QVERIFY(tip.contains("E4") && !tip.contains("E5") && tip.contains("and 3 more"));
  }

  void countsLanesAndIgnoresUnmapped() {
    RecapInput in = baseInput();
    in.events = {
        ev(1, "2026-10-01T10:05:00Z", kU1, kSoul, "soullink.catch", "Caught Pidgey"),
        ev(2, "2026-10-01T11:00:00Z", kU1, kSoul, "soullink.catch"),
        ev(3, "2026-10-02T09:00:00Z", kU2, kSoul, "soullink.catch"),
        ev(4, "2026-10-02T10:00:00Z", kU1, kSoul, "soullink.death"),
        ev(5, "2026-10-03T08:00:00Z", kU2, kSoul, "soullink.badge"),
        ev(6, "2026-10-03T09:00:00Z", kU1, kSoul, "soullink.correction", "Fixed a route"),
        ev(7, "2026-10-03T09:30:00Z", kU1, kSoul, "soullink.unmapped"),           // no mapping: not shown
        ev(8, "2026-10-03T09:40:00Z", kU1, "other.plugin", "soullink.catch"),     // plugin not installed / no mapping: not shown
        ev(9, "2026-10-01T10:00:01Z", {}, {}, "run.joined"),                       // server event: not shown
        ev(10, "2026-10-01T10:00:02Z", kU1, {}, "chat.message"),                   // no plugin, not core: not shown
    };
    const RecapModel m = buildRecap(in);
    QCOMPARE(m.lanes.size(), 2);  // Jonas was only invited and has nothing on the board
    QCOMPARE(m.lanes.at(0).name, QString("Max"));
    QCOMPARE(m.lanes.at(1).name, QString("Lena"));
    QCOMPARE(m.catches, 3);
    QCOMPARE(m.lost, 1);
    QCOMPARE(m.badges, 1);
    const RecapLane* max = laneOf(m, kU1);
    QVERIFY(max);
    QCOMPARE(max->catches, 2);
    QCOMPARE(max->lost, 1);
    QCOMPARE(max->badges, 0);
    QCOMPARE(max->events.size(), 4);  // 2 catches, 1 lost, 1 note
    QCOMPARE(max->events.at(3).glyph, RecapGlyph::Note);
    QCOMPARE(max->events.at(0).text, QString("Caught Pidgey"));
    QCOMPARE(m.shownEvents, 6);
    QCOMPARE(m.days, 3);
    QVERIFY(!m.isEmpty());
    const QList<RecapTile> tiles = m.tiles();
    QCOMPARE(tiles.size(), 4);
    QCOMPARE(tiles.at(0).value, QString("3"));
    QCOMPARE(tiles.at(0).sub, QString("Max 2 · Lena 1"));
    QCOMPARE(tiles.at(1).sub, QString("Max 1"));
    QCOMPARE(tiles.at(2).sub, QString("Lena 1"));
  }

  void coreEventsAreNotesAndScreenshotsAreGlyphs() {
    RecapInput in = baseInput();
    RunEvent rules = ev(1, "2026-10-02T10:00:00Z", {}, {}, "rules.changed");
    rules.payload = QJsonObject{{"by_user_id", kU1}, {"by_user_name", "Max"}};
    in.events = {rules,
                 ev(2, "2026-10-02T11:00:00Z", kU2, {}, "cheat.enabled"),
                 ev(3, "2026-10-02T11:01:00Z", kU2, {}, "cheat.disabled"),
                 ev(4, "2026-10-02T11:02:00Z", kU2, kSoul, "memory.write"),  // a plugin may cause it, still a note
                 ev(5, "2026-10-02T11:03:00Z", kU2, {}, "state.loaded"),
                 ev(6, "2026-10-02T11:04:00Z", {}, {}, "memory.write")};      // no person: cannot be placed
    ScreenshotInfo s;
    s.id = "shot1";
    s.userId = kU1;
    s.userName = "Max";
    s.createdAt = "2026-10-01T12:00:00Z";
    ScreenshotInfo s2 = s;
    s2.id = "shot2";
    s2.userId = QString();  // unknown person: skipped
    in.screenshots = {s, s2};
    const RecapModel m = buildRecap(in);
    QCOMPARE(m.catches + m.lost + m.badges, 0);  // notes and screenshots are not counted
    const RecapLane* max = laneOf(m, kU1);
    const RecapLane* lena = laneOf(m, kU2);
    QVERIFY(max && lena);
    QCOMPARE(max->events.size(), 2);  // screenshot (day 1) then rules.changed (day 2)
    QCOMPARE(max->events.at(0).glyph, RecapGlyph::Screenshot);
    QCOMPARE(max->events.at(0).screenshotId, QString("shot1"));
    QCOMPARE(max->events.at(1).glyph, RecapGlyph::Note);
    QCOMPARE(max->events.at(1).type, QString("rules.changed"));
    QCOMPARE(max->events.at(1).text, QString("Rules changed"));
    QCOMPARE(lena->events.size(), 4);
    for (const RecapEvent& e : lena->events) QCOMPARE(e.glyph, RecapGlyph::Note);
    QCOMPARE(m.shownEvents, 6);
  }

  void dayBucketingAndPosition() {
    RecapInput in = baseInput();
    in.run.updatedAt = "2026-10-02T23:00:00Z";  // finished: the axis ends on day 2
    in.events = {ev(1, "2026-10-01T23:59:00Z", kU1, kSoul, "soullink.catch"), ev(2, "2026-10-02T00:00:00Z", kU1, kSoul, "soullink.catch"),
                 ev(3, "2026-10-01T12:00:00Z", kU2, kSoul, "soullink.catch")};
    RecapModel m = buildRecap(in);
    QCOMPARE(m.days, 2);
    QCOMPARE(m.startDate, QString("2026-10-01"));
    const RecapLane* max = laneOf(m, kU1);
    QCOMPARE(max->events.at(0).day, 1);
    QCOMPARE(max->events.at(1).day, 2);
    QCOMPARE(laneOf(m, kU2)->events.at(0).day, 1);
    QVERIFY(qAbs(laneOf(m, kU2)->events.at(0).pos - 0.25) < 1e-9);  // noon of day 1 of 2
    QVERIFY(qAbs(max->events.at(1).pos - 0.5) < 1e-9);

    // The viewer's time zone decides the day: 22:30 UTC is already day 2 at UTC+2.
    in.zone = QTimeZone(2 * 3600);
    in.events = {ev(1, "2026-10-01T22:30:00Z", kU1, kSoul, "soullink.catch")};
    m = buildRecap(in);
    QCOMPARE(laneOf(m, kU1)->events.at(0).day, 2);

    // A running run reaches today even without events; a paused one stops at its last update.
    in.zone = QTimeZone::utc();
    in.events.clear();
    in.run.status = "active";
    QCOMPARE(buildRecap(in).days, 10);
    in.run.status = "paused";
    QCOMPARE(buildRecap(in).days, 2);  // updated 2026-10-02
    // No events at all: empty.
    QVERIFY(buildRecap(in).isEmpty());
  }

  void playTimeTile() {
    RecapInput in = baseInput();
    in.run.playSeconds = 62 * 3600 + 600;
    RecapModel m = buildRecap(in);
    RecapTile t = m.tiles().at(3);
    QCOMPARE(t.id, QString("playtime"));
    QCOMPARE(t.label, QString("PLAY TIME"));
    QCOMPARE(t.value, QString("62 h"));
    QCOMPARE(t.sub, QString("over 3 days"));
    in.run.playSeconds = 2700;
    QCOMPARE(buildRecap(in).tiles().at(3).value, QString("45 min"));
    // No play time known: the elapsed days instead, no second line.
    in.run.playSeconds = 0;
    t = buildRecap(in).tiles().at(3);
    QCOMPARE(t.label, QString("ELAPSED"));
    QCOMPARE(t.value, QString("3 days"));
    QVERIFY(t.sub.isEmpty());
    QCOMPARE(recapDurationText(0), QString("0 min"));
    QCOMPARE(recapDurationText(30), QString("1 min"));
    QCOMPARE(recapDurationText(5400), QString("2 h"));
  }

  void participantsWhoLeftAndStrangersGetALane() {
    RecapInput in = baseInput();
    in.run.participants[1].status = "left";
    in.events = {ev(1, "2026-10-02T09:00:00Z", kU2, kSoul, "soullink.catch"), ev(2, "2026-10-02T09:00:00Z", "u9", kSoul, "soullink.catch")};
    in.events[1].userName = "Guest";
    const RecapModel m = buildRecap(in);
    QCOMPARE(m.lanes.size(), 3);  // Max, Lena (left), Guest (not a participant any more)
    QCOMPARE(laneOf(m, "u9")->name, QString("Guest"));
    QCOMPARE(m.catches, 2);
  }

  void variantAndJsonShape() {
    RecapInput in = baseInput();
    in.events = {ev(1, "2026-10-01T10:05:00Z", kU1, kSoul, "soullink.catch"), ev(2, "2026-10-03T10:05:00Z", kU1, kSoul, "soullink.badge")};
    const RecapModel m = buildRecap(in);
    const QJsonObject j = m.toJson();
    QCOMPARE(j.value("days").toInt(), 3);
    QCOMPARE(j.value("tiles").toArray().size(), 4);
    const QJsonObject lane = j.value("lanes").toArray().at(0).toObject();
    QCOMPARE(lane.value("name").toString(), QString("Max"));
    QCOMPARE(lane.value("per_day").toArray().size(), 2);
    QCOMPARE(lane.value("per_day").toArray().at(1).toObject().value("day").toInt(), 3);
    const QVariantMap v = m.toVariant();
    QCOMPARE(v.value("lanes").toList().size(), 2);
    QCOMPARE(v.value("axis").toList().first().toMap().value("label").toString(), QString("DAY 1"));
    QVERIFY(!v.value("empty").toBool());
    // Axis labels: 1 3 5 7 9 for nine days.
    in.run.updatedAt = "2026-10-09T10:00:00Z";
    QCOMPARE(buildRecap(in).toVariant().value("axis").toList().size(), 5);
  }

  void htmlIsSelfContained() {
    RecapInput in = baseInput();
    in.run.name = "Ember <b>Route</b> & Co";
    in.run.playSeconds = 3600 * 5;
    in.events = {ev(1, "2026-10-01T10:05:00Z", kU1, kSoul, "soullink.catch", "Caught <i>x</i>"), ev(2, "2026-10-02T10:05:00Z", kU2, kSoul, "soullink.death"),
                 ev(3, "2026-10-02T11:05:00Z", kU2, kSoul, "soullink.badge"), ev(4, "2026-10-02T11:06:00Z", kU2, kSoul, "soullink.correction")};
    ScreenshotInfo s;
    s.id = "shot1";
    s.userId = kU1;
    s.createdAt = "2026-10-02T12:00:00Z";
    in.screenshots = {s};
    const QString html = recapHtml(buildRecap(in), QStringLiteral("Home server"));
    QVERIFY(html.startsWith("<!doctype html>"));
    for (const char* bad : {"http://", "https://", "ftp://", "//cdn", "url(", "@import", "<script", "<link", " src=", "href=", "<img", "<iframe", "<object", "data:"})
      QVERIFY2(!html.contains(QLatin1String(bad), Qt::CaseInsensitive), bad);
    QVERIFY(!html.contains("<b>Route</b>"));  // escaped
    QVERIFY(html.contains("Ember &lt;b&gt;Route&lt;/b&gt; &amp; Co"));
    QVERIFY(html.contains("Caught &lt;i&gt;x&lt;/i&gt;"));
    for (const QString& g : {"■", "✕", "◆", "□", "▣"}) QVERIFY2(html.contains(g), qPrintable(g));
    QVERIFY(html.contains("prefers-color-scheme:dark"));
    QVERIFY(html.contains("Home server"));
    QVERIFY(html.contains("5 h"));
    // The empty recap is a valid file too.
    RecapInput empty = baseInput();
    const QString e = recapHtml(buildRecap(empty), QString());
    QVERIFY(e.contains("Nothing to show yet."));
    QVERIFY(!e.contains("http"));
  }

  void loaderPagesTheWholeLogAndScreenshots() {
    connectHub();
    QJsonObject r = hub_->makeRun(QStringLiteral("Big run"), {hub_->runsMe}, hub_->runsMe, {});
    const QString id = r.value("run_id").toString();
    hub_->runs.insert(id, r);
    for (int i = 0; i < 1200; ++i)
      hub_->addRunEvent(id, QStringLiteral("test.event"), QStringLiteral("e%1").arg(i), hub_->runsMe, QStringLiteral("net.test.plugin"), {}, false);
    hub_->screenshotsUnsupported = false;
    QJsonObject meta{{"id", "s1"}, {"game_id", "g"}, {"run_id", id}, {"user_id", hub_->runsMe}, {"user_name", "Tester"},
                     {"created_at", "2026-10-02T12:00:00Z"}, {"width", 16}, {"height", 20}, {"size_bytes", 100}};
    hub_->screenshots.append({meta, QByteArray("png")});

    RunRecapLoader loader(conn_.get());
    bool done = false;
    RecapFetch got;
    loader.load(id, [&](const RecapFetch& f) { got = f; done = true; });
    QVERIFY(!done);  // never synchronous
    QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
    QVERIFY2(got.ok, qPrintable(got.error));
    const int total = int(hub_->runEvents.value(id).size());  // the 1200 plus what the fake Hub wrote itself
    QVERIFY(total >= 1200);
    QCOMPARE(got.events.size(), total);
    QCOMPARE(got.events.first().seq, qint64(1));
    QCOMPARE(got.events.last().seq, qint64(total));
    QCOMPARE(got.screenshots.size(), 1);
    QCOMPARE(hub_->count(QStringLiteral("/api/v1/runs/") + id + QStringLiteral("/events")), 3);  // 500 + 500 + the rest

    // A server without screenshots only leaves the glyphs out.
    hub_->screenshotsUnsupported = true;
    done = false;
    loader.load(id, [&](const RecapFetch& f) { got = f; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
    QVERIFY(got.ok);
    QVERIFY(got.screenshots.isEmpty());
    QCOMPARE(got.events.size(), total);
  }

  void loaderReportsAFailure() {
    connectHub();
    hub_->runFailNext = 100;
    hub_->runFailStatus = 500;
    RunRecapLoader loader(conn_.get());
    bool done = false;
    RecapFetch got;
    loader.load(QStringLiteral("11111111-2222-3333-4444-555555555555"), [&](const RecapFetch& f) { got = f; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QVERIFY(!got.ok);
    QVERIFY(!got.error.isEmpty());
    QVERIFY(got.events.isEmpty());
  }
};

QTEST_MAIN(RunRecapTest)
#include "runrecap_test.moc"
