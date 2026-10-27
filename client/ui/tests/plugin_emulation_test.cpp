// PluginEmulationService against the Uzume test core: run rule -> consent -> safety copy -> emulation queue -> run log, for
// fb.cheats and fb.memory.write, plus the plugin snapshot rate limit (0.20 package 5). The plugin thread side of the API is
// covered by plugins/tests/emulation_test.cpp with a fake bridge.
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "gamesession.h"
#include "pluginemulation.h"
#include "run_bridge.h"
#include "test_core_layout.h"

using namespace uzume;
using namespace uzume::ui;
using namespace uzume::plugins;
namespace mem = uzume::testcore::mem;
using Kind = PluginEmulationService::Kind;
using Consent = PluginEmulationService::Consent;

namespace {

// A run whose core rules the test can change; records the events the app writes.
class FakeRun : public RunBridge {
 public:
  QJsonObject rules;
  mutable QList<QPair<QString, QJsonObject>> logged;
  QJsonObject current() const override { return QJsonObject{{"id", "r1"}, {"status", "active"}, {"rules", rules}}; }
  QJsonObject stateOf(const QString&, const QString&, bool* found) const override { if (found) *found = false; return {}; }
  QJsonObject states(const QString&) const override { return {}; }
  QJsonArray events(qint64, int) const override { return {}; }
  QString setState(const QString&, const QJsonObject&) override { return {}; }
  QString postEvent(const QString&, const QString&, const QString&, const QString&, const QJsonObject&, QString*) override { return {}; }
  QString setBoard(const QString&, const QJsonObject&) override { return {}; }
  QString postCoreEvent(const QString& type, const QString&, const QJsonObject& payload) override {
    logged.append({type, payload});
    return {};
  }
  QList<RunNotification> takeNotifications() override { return {}; }
  int count(const QString& type) const {
    int n = 0;
    for (const auto& e : logged) n += e.first == type;
    return n;
  }
};

CheatItem raw(const QString& id, const QString& code, bool enabled = true) { return CheatItem{id, id, QStringLiteral("raw"), code, enabled}; }

}  // namespace

class PluginEmulationServiceTest : public QObject {
  Q_OBJECT
  QTemporaryDir dir_;
  std::unique_ptr<GameSession> gs_;
  std::unique_ptr<PluginEmulationService> svc_;
  std::shared_ptr<FakeRun> run_;
  QStringList snapshots_;
  int markers_ = 0;
  QList<QPair<QString, QVariant>> appEvents_;
  QList<PluginEmulationService::Notice> notices_;

  int appCount(const QString& name) const {
    int n = 0;
    for (const auto& e : appEvents_) n += e.first == name;
    return n;
  }
  void startGame(const std::shared_ptr<FakeRun>& run) {
    QDir(dir_.filePath("data")).removeRecursively();
    gs_ = std::make_unique<GameSession>();
    GameSession::LaunchConfig c;
    c.title = QStringLiteral("Test");
    c.corePath = qEnvironmentVariable("UZUME_MELONDS_DS_CORE");
    c.gamePath = QStringLiteral(FB_TEST_ROM_PATH);
    c.systemDir = dir_.filePath(QStringLiteral("system"));
    c.saveDir = dir_.filePath(QStringLiteral("save"));
    QDir().mkpath(c.systemDir);
    QDir().mkpath(c.saveDir);
    gs_->start(c);
    QTRY_COMPARE_WITH_TIMEOUT(gs_->state(), GameSession::Running, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!gs_->memoryRegions().empty(), 10000);
    svc_ = std::make_unique<PluginEmulationService>(gs_.get(), dir_.filePath("data"));
    snapshots_.clear();
    markers_ = 0;
    appEvents_.clear();
    notices_.clear();
    svc_->setSnapshotFn([this](const QString& label, PluginEmulationService::SnapshotDone done) {
      snapshots_ << label;
      done(PluginEmulationService::SnapshotOutcome::Ok, QString());
    });
    svc_->setCheatsUsedMarker([this]() { ++markers_; });
    svc_->setPluginNameFn([](const QString& id) { return QStringLiteral("Name of ") + id; });
    svc_->setAppEventSink([this](const QString& n, const QVariant& v) { appEvents_.append({n, v}); });
    connect(svc_.get(), &PluginEmulationService::noticeRaised, this, [this](const PluginEmulationService::Notice& n) { notices_.append(n); });
    run_ = run;
    PluginEmulationService::GameInfo gi;
    gi.id = QStringLiteral("game-1");
    gi.title = QStringLiteral("Test Game");
    gi.system = QStringLiteral("nds");
    gi.coreName = QStringLiteral("Test core");
    gi.cheatFormats = {QStringLiteral("raw")};
    svc_->gameStarted(gi, run);
  }
  void stopGame() {
    if (svc_) svc_->gameStopped();
    svc_.reset();
    if (gs_) gs_->stop();
    gs_.reset();
  }
  static CheatItem poke(const QString& id = QStringLiteral("c1")) { return raw(id, QStringLiteral("08000200 AB")); }

 private slots:
  void cleanup() { stopGame(); }

  void cheatsNeedTheRunRuleAndConsent() {
    auto run = std::make_shared<FakeRun>();
    run->rules = {{"cheats", "off"}};
    startGame(run);
    // Rule off: refused before anything else, the plugin hears cheats.blocked.
    CheatApplyResult r = svc_->applyCheats("p.one", {poke()});
    QCOMPARE(r.error, QStringLiteral("blocked_by_run_rule"));
    QCOMPARE(appCount("cheats.blocked"), 1);
    QVERIFY(snapshots_.isEmpty());
    QCOMPARE(svc_->cheatsOn(), 0);
    // Turning the rule off for a list with nothing enabled is still fine (clearing never needs permission).
    QVERIFY(svc_->clearCheats("p.one").isEmpty());

    // Rule logged: the first apply asks for the game's consent.
    run->rules = {{"cheats", "logged"}};
    r = svc_->applyCheats("p.one", {poke()});
    QCOMPARE(r.error, QStringLiteral("consent_required"));
    // 0.20 package 6 (F14): nothing pops up at game start; the question waits for the first action of the person in the game.
    QTest::qWait(100);
    QVERIFY(svc_->consentRequest().isEmpty());
    svc_->noteUserAction();
    QTRY_VERIFY(!svc_->consentRequest().isEmpty());
    QCOMPARE(svc_->consentRequest().value("kind").toString(), QStringLiteral("cheats"));
    QVERIFY(svc_->consentRequest().value("title").toString().contains(QStringLiteral("Test Game")));
    QCOMPARE(svc_->consentRequest().value("confirmLabel").toString(), QStringLiteral("Turn on cheats"));
    QVERIFY(snapshots_.isEmpty());  // nothing before the person agrees
    svc_->answerConsent(true);
    QTRY_COMPARE(svc_->cheatsOn(), 1);
    QTRY_COMPARE(snapshots_, QStringList{QStringLiteral("Before cheats")});
    QTRY_VERIFY(markers_ >= 1);
    QTRY_COMPARE(run->count("cheat.enabled"), 1);
    QCOMPARE(svc_->consent(Kind::Cheats), Consent::Allowed);
    // Remembered in the data dir: a new service for the same game does not ask again.
    QFile f(dir_.filePath("data/plugins/consent.json"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(f.readAll()).object().contains(QStringLiteral("game:game-1")));

    // Only one safety copy per play session; a second list does not take another.
    r = svc_->applyCheats("p.one", {poke(), raw("c2", "08000201 CD")});
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.applied, 2);
    QTRY_COMPARE(svc_->cheatsOn(), 2);
    QCOMPARE(snapshots_.size(), 1);

    // The rule switches off live: everything is cleared at once and the plugin hears about it.
    run->rules = {{"cheats", "off"}};
    QTRY_COMPARE_WITH_TIMEOUT(svc_->cheatsOn(), 0, 3000);
    QVERIFY(appCount("cheats.blocked") >= 2);
    QVERIFY(run->count("cheat.disabled") >= 2);
    // ... and back on.
    run->rules = {{"cheats", "logged"}};
    QTRY_VERIFY_WITH_TIMEOUT(appCount("cheats.allowed") >= 1, 3000);
  }

  // 0.20 final live check: the answer "Turn on cheats" tells the plugin (cheats.allowed), so its "Waiting for your answer" ends.
  void consentAllowTellsThePlugin() {
    startGame(nullptr);
    QCOMPARE(svc_->applyCheats("p.one", {poke()}).error, QStringLiteral("consent_required"));
    svc_->noteUserAction();
    QTRY_VERIFY(!svc_->consentRequest().isEmpty());
    QCOMPARE(appCount("cheats.allowed"), 0);
    svc_->answerConsent(true);
    QTRY_COMPARE(appCount("cheats.allowed"), 1);
    QCOMPARE(appCount("cheats.blocked"), 0);
    QTRY_COMPARE(svc_->cheatsOn(), 1);
  }

  // 0.20 final live check: "off" is the database being switched off; on without lists is another code.
  void cheatDatabaseStatusDistinguishesOffFromUnavailable() {
    startGame(nullptr);
    const QString path = QDir(dir_.filePath("data")).filePath(QStringLiteral("cheatdb//game-1.json"));
    QDir().mkpath(dir_.filePath("data/cheatdb"));
    const auto write = [&](const char* json) {
      QFile f(path);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
      f.write(json);
    };
    write(R"({"source":"none","cheats":[]})");
    QCOMPARE(svc_->cheatDatabase().error, QStringLiteral("off"));
    write(R"({"source":"unavailable","cheats":[]})");
    QCOMPARE(svc_->cheatDatabase().error, QStringLiteral("unavailable"));
    write(R"({"source":"libretro-database","cheats":[]})");
    const plugins::CheatDatabaseResult ok = svc_->cheatDatabase();
    QVERIFY(ok.error.isEmpty());
    QCOMPARE(ok.data.value("source").toString(), QStringLiteral("libretro-database"));
  }

  void cheatsConsentDeniedIsNotRemembered() {
    auto run = std::make_shared<FakeRun>();
    run->rules = {{"cheats", "logged"}};
    startGame(run);
    svc_->noteUserAction();  // the person is acting in the game: the question is asked right away
    QCOMPARE(svc_->applyCheats("p.one", {poke()}).error, QStringLiteral("consent_required"));
    QTRY_VERIFY(!svc_->consentRequest().isEmpty());
    svc_->answerConsent(false);
    QVERIFY(svc_->consentRequest().isEmpty());
    QVERIFY(appCount("cheats.blocked") >= 1);
    QCOMPARE(svc_->applyCheats("p.one", {poke()}).error, QStringLiteral("consent_denied"));
    QCOMPARE(svc_->cheatsOn(), 0);
    QVERIFY(snapshots_.isEmpty());
    QCOMPARE(svc_->storedConsent(svc_->gameKey(), Kind::Cheats), Consent::Ask);  // Cancel is session-only
  }

  void formatsAndCodesAreChecked() {
    auto run = std::make_shared<FakeRun>();
    run->rules = {{"cheats", "logged"}};
    startGame(run);
    svc_->setConsent(Kind::Cheats, Consent::Allowed);
    CheatItem bad = poke("bad");
    bad.format = QStringLiteral("gateway");  // the core does not take it
    CheatItem empty = raw("empty", "   ");
    const CheatApplyResult r = svc_->applyCheats("p.one", {poke(), bad, empty});
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.applied, 1);
    QCOMPARE(r.rejected.size(), 2);
    QCOMPARE(svc_->cheatFormats(), QStringList{QStringLiteral("raw")});
    QCOMPARE(svc_->coreName(), QStringLiteral("Test core"));
  }

  void bypassConsentSkipsOnlyTheQuestion() {
    auto run = std::make_shared<FakeRun>();
    run->rules = {{"cheats", "off"}};
    startGame(run);
    QCOMPARE(svc_->applyCheatsAs("mcp", {poke()}, true).error, QStringLiteral("blocked_by_run_rule"));  // the rule still applies
    run->rules = {{"cheats", "logged"}};
    const CheatApplyResult r = svc_->applyCheatsAs("mcp", {poke()}, true);
    QVERIFY(r.error.isEmpty());
    QTRY_COMPARE(svc_->cheatsOn(), 1);
    QVERIFY(svc_->consentRequest().isEmpty());
    QTRY_COMPARE(snapshots_, QStringList{QStringLiteral("Before cheats")});  // the safety copy still happens
  }

  void aSnapshotFailureStopsTheCheats() {
    auto run = std::make_shared<FakeRun>();
    run->rules = {{"cheats", "logged"}};
    startGame(run);
    svc_->setSnapshotFn([this](const QString& label, PluginEmulationService::SnapshotDone done) {
      snapshots_ << label;
      done(PluginEmulationService::SnapshotOutcome::Failed, QStringLiteral("server error"));
    });
    svc_->setConsent(Kind::Cheats, Consent::Allowed);
    QVERIFY(svc_->applyCheats("p.one", {poke()}).error.isEmpty());
    QTRY_VERIFY(!notices_.isEmpty());
    QCOMPARE(svc_->cheatsOn(), 0);
    QVERIFY(appCount("cheats.blocked") >= 1);
  }

  void memoryWritesFollowTheSameOrder() {
    auto run = std::make_shared<FakeRun>();
    run->rules = {{"memory_write", "off"}};
    startGame(run);
    const QByteArray bytes("\x01\x02\x03\x04");
    QCOMPARE(svc_->writeMemory("p.one", mem::kHeapAddr + 0x400, bytes), QStringLiteral("blocked_by_run_rule"));
    run->rules = {{"memory_write", "logged"}};
    QCOMPARE(svc_->writeMemory("p.one", 0x00000010, bytes), QStringLiteral("unmapped"));
    QCOMPARE(svc_->writeMemory("p.one", mem::kHeapAddr + 0x400, QByteArray()), QStringLiteral("invalid"));
    QCOMPARE(svc_->writeMemory("p.one", mem::kHeapAddr + 0x400, bytes), QStringLiteral("consent_required"));
    QTRY_VERIFY(!svc_->consentRequest().isEmpty());
    QCOMPARE(svc_->consentRequest().value("kind").toString(), QStringLiteral("memory_write"));
    QVERIFY(snapshots_.isEmpty());
    svc_->answerConsent(true);
    QVERIFY(svc_->writeMemory("p.one", mem::kHeapAddr + 0x400, bytes).isEmpty());
    QTRY_COMPARE(snapshots_, QStringList{QStringLiteral("Before memory changes")});
    // At most one run event per 10 s and plugin, however many writes.
    for (int i = 0; i < 5; ++i) QVERIFY(svc_->writeMemory("p.one", mem::kHeapAddr + 0x400, bytes).isEmpty());
    QCOMPARE(run->count("memory.write"), 1);
    QCOMPARE(run->logged.last().second.value("plugin_id").toString(), QStringLiteral("p.one"));
    QVERIFY(markers_ >= 1);
  }

  void twoPluginsOnOneAddressAreReported() {
    auto run = std::make_shared<FakeRun>();
    run->rules = {{"memory_write", "logged"}};
    startGame(run);
    svc_->setConsent(Kind::MemoryWrite, Consent::Allowed);
    QSignalSpy spy(svc_.get(), &PluginEmulationService::writeConflict);
    const QByteArray b("\x07");
    for (int i = 0; i < 8; ++i) QVERIFY(svc_->writeMemory(i % 2 ? "p.one" : "p.two", mem::kHeapAddr + 0x600, b).isEmpty());
    QCOMPARE(spy.count(), 1);  // reported once, not on every write
    QTRY_VERIFY(!notices_.isEmpty());
    QCOMPARE(notices_.last().tone, QStringLiteral("warning"));
  }

  void memoryRuleOffDropsQueuedWrites() {
    auto run = std::make_shared<FakeRun>();
    run->rules = {{"memory_write", "logged"}};
    startGame(run);
    svc_->setConsent(Kind::MemoryWrite, Consent::Allowed);
    run->rules = {{"memory_write", "off"}};
    QCOMPARE(svc_->writeMemory("p.one", mem::kHeapAddr + 0x400, QByteArray(1, 1)), QStringLiteral("blocked_by_run_rule"));
  }

  void noRunMeansEverythingIsAllowed() {
    startGame(nullptr);
    QVERIFY(svc_->cheatsBlockedReason().isEmpty());
    svc_->setConsent(Kind::Cheats, Consent::Allowed);
    QVERIFY(svc_->applyCheats("p.one", {poke()}).error.isEmpty());
    QTRY_COMPARE(svc_->cheatsOn(), 1);
  }

  void snapshotsFromPluginsAreLabelledAndAnnounced() {
    startGame(nullptr);
    QVERIFY(svc_->snapshot("p.one", "Some Plugin", "before the boss").isEmpty());
    QTRY_VERIFY(!snapshots_.isEmpty());
    QCOMPARE(snapshots_.last(), QStringLiteral("Some Plugin: before the boss"));
    QTRY_VERIFY(!notices_.isEmpty());
    QCOMPARE(notices_.first().pluginName, QStringLiteral("Some Plugin"));
    QCOMPARE(notices_.first().title, QStringLiteral("saved a snapshot · before the boss"));
    QCOMPARE(notices_.first().tone, QStringLiteral("done"));
  }
};

QTEST_MAIN(PluginEmulationServiceTest)
#include "plugin_emulation_test.moc"
