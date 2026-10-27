// StatesController against the Uzume test core (real retro_serialize) and the fake server: slots, Auto kept before a load, the
// load dialog and Undo, run-rule gating, the server upload and the download of a state that is not cached. Never needs a real core.
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "fakehub.h"
#include "gamesession.h"
#include "hubconnection.h"
#include "playersettings.h"
#include "profilestore.h"
#include "statescontroller.h"

using namespace uzume;
using namespace uzume::ui;
using State = HubConnection::State;
using Result = StatesController::Result;

class StatesControllerTest : public QObject {
  Q_OBJECT
  QTemporaryDir dir_;
  std::unique_ptr<GameSession> gs_;
  std::unique_ptr<PlayerSettings> settings_;
  std::unique_ptr<StatesController> st_;
  std::optional<RunRules> rules_;  // nullopt = no run
  QString owner_ = QStringLiteral("Alex");
  QList<QString> runEvents_;
  // server
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;
  std::unique_ptr<StateApi> api_;

  QString statesDir() const { return dir_.filePath(QStringLiteral("states")); }
  void startGame(const QString& coreVersion = QStringLiteral("1.0")) {
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
    settings_ = std::make_unique<PlayerSettings>(dir_.filePath(QStringLiteral("settings")));
    st_ = std::make_unique<StatesController>(gs_.get(), settings_.get());
    st_->setLocation(statesDir());
    st_->setRulesFn([this]() { return rules_; });
    st_->setRuleOwnerFn([this]() { return owner_; });
    st_->setRunEventFn([this](const QString& type, const QString&, const QJsonObject&) { runEvents_ << type; });
    if (api_) st_->setApi(api_.get());
    StatesController::GameContext ctx;
    ctx.gameId = QStringLiteral("g1");
    ctx.title = QStringLiteral("Test");
    ctx.systemId = QStringLiteral("nds");
    ctx.coreId = QStringLiteral("testcore");
    ctx.coreVersion = coreVersion;
    ctx.states = true;
    st_->gameStarted(ctx);
    st_->view(QStringLiteral("g1"));
  }
  void stopGame() {
    st_.reset();
    settings_.reset();
    if (gs_) gs_->stop();
    gs_.reset();
  }
  Result save(int n) {
    Result out;
    bool done = false;
    st_->save(n, [&](const Result& r) { out = r; done = true; });
    [&] { QTRY_VERIFY_WITH_TIMEOUT(done, 10000); }();
    return out;
  }
  Result load(int n, bool force = true) {
    Result out;
    bool done = false;
    st_->load(n, force, [&](const Result& r) { out = r; done = true; });
    [&] { QTRY_VERIFY_WITH_TIMEOUT(done, 10000); }();
    return out;
  }
  QVariantMap row(int n) const { return st_->slotRows().value(n).toMap(); }
  void connectHub() {
    profiles_ = std::make_unique<ProfileStore>(dir_.filePath(QStringLiteral("data")));
    creds_ = std::make_unique<MemoryCredentialStore>();
    hub_ = std::make_unique<FakeHub>(QStringLiteral("a"));
    hub_->features = {QStringLiteral("saves_v1"), QStringLiteral("states_v1")};
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
    api_ = std::make_unique<StateApi>(conn_.get());
  }

 private slots:
  void init() {
    rules_.reset();
    runEvents_.clear();
  }
  void cleanup() {
    stopGame();
    api_.reset();
    conn_.reset();
    hub_.reset();
    QDir(dir_.filePath(QStringLiteral("states"))).removeRecursively();
  }

  void fiveRowsAutoFirstAllEmptyAtStart() {
    startGame();
    QVERIFY(st_->supported());
    QVERIFY(st_->playing());
    QCOMPARE(st_->slotRows().size(), 5);
    QCOMPARE(row(0).value("label").toString(), QStringLiteral("A"));
    QCOMPARE(row(4).value("label").toString(), QStringLiteral("4"));
    for (int n = 0; n <= 4; ++n) QVERIFY(row(n).value("empty").toBool());
    QCOMPARE(st_->blockedText(), QString());
  }

  void saveFillsTheSlotAndLoadKeepsAuto() {
    startGame();
    const Result s = save(2);
    QVERIFY2(s.ok, qPrintable(s.error));
    QVERIFY(!row(2).value("empty").toBool());
    QVERIFY(row(1).value("empty").toBool());
    QVERIFY(row(0).value("empty").toBool());
    const Result l = load(2);
    QVERIFY2(l.ok, qPrintable(l.error));
    QTRY_VERIFY_WITH_TIMEOUT(!row(0).value("empty").toBool(), 5000);  // Auto kept before the load
    QVERIFY(!row(2).value("empty").toBool());
    QCOMPARE(st_->toast().value("action").toString(), QStringLiteral("Undo"));
  }

  void loadingAnEmptySlotSaysSo() {
    startGame();
    const Result l = load(3);
    QVERIFY(!l.ok);
    QCOMPARE(l.error, QStringLiteral("empty"));
  }

  void loadDialogConfirmUndoAndDontAskAgain() {
    startGame();
    QVERIFY(save(1).ok);
    QVERIFY(st_->askBeforeLoad());
    QSignalSpy loaded(st_.get(), &StatesController::stateLoaded);
    st_->requestLoad(1);
    QVERIFY(!st_->loadRequest().isEmpty());
    QCOMPARE(st_->loadRequest().value("n").toInt(), 1);
    QCOMPARE(loaded.count(), 0);  // nothing happens until the person confirms
    st_->cancelLoad();
    QVERIFY(st_->loadRequest().isEmpty());
    QCOMPARE(loaded.count(), 0);
    st_->requestLoad(1);
    st_->confirmLoad();
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
    QVERIFY(st_->loadRequest().isEmpty());
    // Undo goes back to the Auto state
    QTRY_VERIFY_WITH_TIMEOUT(!row(0).value("empty").toBool(), 5000);
    st_->undoLoad();
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 10000);
    QCOMPARE(loaded.last().at(0).toInt(), 0);
    // "Don't ask again": the next load goes straight through
    st_->setAskBeforeLoad(false);
    QVERIFY(!settings_->stateLoadAsk());
    st_->requestLoad(1);
    QVERIFY(st_->loadRequest().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 3, 10000);
  }

  void deleteEmptiesTheSlot() {
    startGame();
    QVERIFY(save(4).ok);
    Result r;
    bool done = false;
    st_->remove(4, [&](const Result& x) { r = x; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(r.ok);
    QVERIFY(row(4).value("empty").toBool());
    done = false;
    st_->remove(4, [&](const Result& x) { r = x; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(!r.ok);
  }

  void slotChoiceAndNextSlot() {
    startGame();
    st_->setSlot(3);
    QCOMPARE(st_->slot(), 3);
    QCOMPARE(st_->nextSlot(), 4);
    QCOMPARE(st_->nextSlot(), 1);
    st_->setSlot(2);
    st_->saveToSlot();
    QTRY_VERIFY_WITH_TIMEOUT(!row(2).value("empty").toBool(), 10000);
  }

  void versionTags() {
    QCOMPARE(StatesController::versionTag(QStringLiteral("1.0.0"), QStringLiteral("1.0.0")), QString());
    QCOMPARE(StatesController::versionTag(QStringLiteral("1.0.0"), QStringLiteral("1.2.0")), QStringLiteral("Older core"));
    QCOMPARE(StatesController::versionTag(QStringLiteral("2.0.0"), QStringLiteral("1.2.0")), QStringLiteral("Newer core"));
    QCOMPARE(StatesController::versionTag(QString(), QStringLiteral("1.2.0")), QString());
    QVERIFY(!StatesController::rewindAllowedFor(QStringLiteral("3ds")));
    QVERIFY(StatesController::rewindAllowedFor(QStringLiteral("nds")));
  }

  void runRulesGateStatesAndRewind() {
    startGame();
    RunRules rr;
    rr.states = false;
    rr.rewind = false;
    rules_ = rr;
    st_->rulesChanged();
    QCOMPARE(st_->blockedText(), QStringLiteral("Save states are off in this run · rule set by Alex"));
    const Result s = save(1);
    QVERIFY(!s.ok);
    QCOMPARE(s.error, QStringLiteral("rule_states"));
    QVERIFY(s.message.contains(QStringLiteral("Save states are off in this run")));
    QVERIFY(row(1).value("empty").toBool());
    QVERIFY(!load(1).ok);
    st_->rewindKey(true);
    QVERIFY(st_->toast().value("text").toString().startsWith(QStringLiteral("Rewind is off in this run")));
    st_->rewindKey(false);
    // Live: the rule changes while the game runs (run_updated) and everything works at once.
    rr.states = true;
    rules_ = rr;
    st_->rulesChanged();
    QCOMPARE(st_->blockedText(), QString());
    QVERIFY(save(1).ok);
    // no run at all: everything allowed
    rules_.reset();
    st_->rulesChanged();
    QVERIFY(save(2).ok);
  }

  void loadingInARunWritesTheRunEvent() {
    startGame();
    RunRules rr;
    rr.states = true;
    rules_ = rr;
    st_->rulesChanged();
    QVERIFY(save(1).ok);
    QVERIFY(load(1).ok);
    QVERIFY(runEvents_.contains(QStringLiteral("state.loaded")));
    runEvents_.clear();
    rules_.reset();
    QVERIFY(load(1).ok);
    QVERIFY(runEvents_.isEmpty());  // outside a run nothing is logged
  }

  void statesNeverTouchTheSave() {
    startGame();
    const QString saveDir = dir_.filePath(QStringLiteral("save"));
    const auto listing = [&]() {
      QStringList l;
      QDirIterator it(saveDir, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext()) l << it.next() + QLatin1Char(':') + QString::number(it.fileInfo().size());
      l.sort();
      return l;
    };
    const QStringList before = listing();
    QVERIFY(save(1).ok);
    QVERIFY(save(2).ok);
    QCOMPARE(listing(), before);  // states live in the states folder only
    QVERIFY(QDir(statesDir()).exists());
  }

  void coreWithoutStatesIsUnsupported() {
    startGame();
    StatesController::GameContext ctx = st_->context();
    ctx.states = false;
    st_->gameStarted(ctx);
    QVERIFY(!st_->supported());
    const Result s = save(1);
    QVERIFY(!s.ok);
    QCOMPARE(s.error, QStringLiteral("unsupported"));
  }

  void uploadsToTheServerAndDownloadsWhenNotCached() {
    connectHub();
    startGame();
    QVERIFY(save(1).ok);
    QTRY_COMPARE_WITH_TIMEOUT(hub_->states.size(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(row(1).value("onServer").toBool(), 10000);
    // Drop the local copy of the blob: the record stays (list from the server), the load downloads it again.
    const QString slotDir = statesDir() + QStringLiteral("/g1/default");
    QVERIFY(QFile::remove(slotDir + QStringLiteral("/1.state")));
    const int gets = hub_->stateGets;
    QVERIFY(load(1).ok);
    QVERIFY(hub_->stateGets > gets);
    QVERIFY(QFile::exists(slotDir + QStringLiteral("/1.state")));  // cached again
    // Delete goes to the server as well.
    Result r;
    bool done = false;
    st_->remove(1, [&](const Result& x) { r = x; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(hub_->states.isEmpty(), 10000);
  }

  void worksWithoutAServer() {
    startGame();  // no api at all: "Only on this PC"
    QVERIFY(save(3).ok);
    QVERIFY(!row(3).value("onServer").toBool());
    QVERIFY(load(3).ok);
  }
};

QTEST_MAIN(StatesControllerTest)
#include "states_controller_test.moc"
