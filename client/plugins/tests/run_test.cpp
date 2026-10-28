// uz.run (permission run.state): the Lua API against the real RunClient + RunSessionBridge + fake Hub, plus the cases
// without a run and without the permission. Synthetic plugins only.
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "fakehub.h"
#include "hubconnection.h"
#include "hubsocket.h"
#include "plugin_host.h"
#include "plugin_manager.h"
#include "plugin_test_util.h"
#include "profilestore.h"
#include "run_session_bridge.h"
#include "runclient.h"

using namespace uzume;
using namespace uzume::plugins;
using namespace uzume::plugins::testutil;
using State = HubConnection::State;

namespace {
const QString kId = QStringLiteral("net.test.plugin");
const QString kSecond = QStringLiteral("net.test.second");  // another plugin of the run (not started in these tests)
const QString kOther = QStringLiteral("u_other_1");
bool hasLog(const ConsoleSnapshot& c, const QString& needle) {
  for (const LogLine& l : c.log)
    if (l.text.contains(needle)) return true;
  return false;
}
}  // namespace

class RunApiTest : public QObject {
  Q_OBJECT
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;
  std::unique_ptr<HubSocket> socket_;
  std::unique_ptr<RunClient> client_;
  std::unique_ptr<PluginHost> host_;
  std::shared_ptr<FakeMemoryProvider> mem_;
  QString runId_;

  void connectHub(const QString& status = QStringLiteral("active")) {
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

    QJsonObject r = hub_->makeRun(QStringLiteral("Test run"), {hub_->runsMe, kOther}, hub_->runsMe,
                                  QJsonArray{QJsonObject{{"plugin_id", kId}, {"version", "1.0.0"}, {"sha256", QString(64, QLatin1Char('a'))}},
                                             QJsonObject{{"plugin_id", kSecond}, {"version", "1.0.0"}, {"sha256", QString(64, QLatin1Char('b'))}}});
    runId_ = r.value("run_id").toString();
    r.insert("status", status);
    r.insert("settings", QJsonObject{{"hardcore", true}});
    hub_->runs.insert(runId_, r);

    socket_ = std::make_unique<HubSocket>(conn_.get());
    client_ = std::make_unique<RunClient>(conn_.get(), socket_.get(), dir_->filePath(QStringLiteral("runs-data")));
    client_->setStateIntervalMs(60);
    client_->setBoardIntervalMs(100);
    client_->setRetryBaseMs(40);
    client_->setCatchUpDelayMs(60);
    socket_->start();
    QTRY_VERIFY_WITH_TIMEOUT(client_->run(runId_).has_value() && client_->lastSeq(runId_) >= 1, 8000);
    host_ = std::make_unique<PluginHost>();
    mem_ = std::make_shared<FakeMemoryProvider>();
  }

  void startPlugin(const QString& lua, const QStringList& perms = {QStringLiteral("run.state")}, bool inRun = true) {
    PluginSpec s;
    s.permissions = perms;
    s.lua = lua;
    const QString dir = writePlugin(dir_->filePath(QStringLiteral("src")), s);
    GameContext ctx{"nds", "ABCD", "abc123", "Test Game", "", "en"};
    if (inRun) ctx.run = RunSessionBridge::create(client_.get(), runId_);
    host_->start(ctx, mem_, {infoFor(dir, dir_->filePath(QStringLiteral("cache")))});
    QTRY_VERIFY_WITH_TIMEOUT(host_->consoleSnapshot(kId).status != PluginStatus::Loading, 10000);
  }

  ConsoleSnapshot con() const { return host_->consoleSnapshot(kId); }

 private slots:
  void cleanup() {
    host_.reset();
    client_.reset();
    socket_.reset();
    conn_.reset();
    hub_.reset();
    creds_.reset();
    profiles_.reset();
    dir_.reset();
  }

  void absentOutsideRuns() {
    connectHub();
    startPlugin(R"LUA( assert(uz.run == nil) uz.log.info("no run") )LUA", {QStringLiteral("run.state")}, false);
    QCOMPARE(con().status, PluginStatus::Running);
    QVERIFY(hasLog(con(), "no run"));
  }

  void needsPermission() {
    connectHub();
    startPlugin(R"LUA(
      local ok, err = pcall(uz.run.current)
      assert(not ok and tostring(err):find("permission run.state"), tostring(err))
      uz.log.info("denied")
    )LUA",
                {});
    QCOMPARE(con().status, PluginStatus::Running);
    QVERIFY(hasLog(con(), "denied"));
  }

  void currentStateAndEvents() {
    connectHub();
    hub_->addRunEvent(runId_, QStringLiteral("test.old"), QStringLiteral("old"), kOther, kId, {}, false);
    QJsonObject r = hub_->runs.value(runId_);
    hub_->runs.insert(runId_, r);
    hub_->pushRun(runId_);
    QTRY_VERIFY_WITH_TIMEOUT(client_->lastSeq(runId_) == 2, 8000);
    startPlugin(R"LUA(
      local c = uz.run.current()
      assert(c.name == "Test run" and c.status == "active", c.status)
      assert(c.me == "u_test_1")
      assert(c.settings.hardcore == true)
      assert(#c.plugins == 2 and c.plugins[1].id == "net.test.plugin" and c.plugins[1].version == "1.0.0")
      assert(#c.participants == 2 and c.participants[1].user_id == "u_test_1" and c.participants[1].status == "joined")
      assert(c.participants[1].color == 0 and c.participants[2].color == nil)
      assert(c.randomizer == nil and c.participants[1].variant_kind == nil)
      assert(next(uz.run.state()) == nil)
      assert(uz.run.state("nobody") == nil)
      assert(next(uz.run.state("u_test_1", "net.test.second")) == nil)
      assert(uz.run.state("u_test_1", "net.not.in.run") == nil)
      local evs = uz.run.events(0, 10)
      assert(#evs == 2 and evs[1].type == "run.created" and evs[2].type == "test.old" and evs[2].user_id == "u_other_1", #evs)
      assert(#uz.run.events(1, 10) == 1 and #uz.run.events(0, 1) == 1)
      uz.log.info("read ok")
    )LUA");
    QCOMPARE(con().status, PluginStatus::Running);
    QVERIFY2(hasLog(con(), "read ok"), con().error.toUtf8().constData());
  }

  void currentShowsOnlyPublicRandomizerFacts() {
    connectHub();
    QJsonObject r = hub_->runs.value(runId_);
    r.insert("randomizer", QJsonObject{{"mode", "each"},
                                       {"preset_id", "classic"},
                                       {"tool_version", "1.6.2"},
                                       {"allow_own", true},
                                       {"base_sha256", QString(64, QLatin1Char('c'))},
                                       {"seed", "12345"},
                                       {"settings", "SECRETSETTINGS"}});
    QJsonArray ps = r.value("participants").toArray();
    QJsonObject p0 = ps.at(0).toObject();
    p0.insert("variant_kind", "run");
    ps.replace(0, p0);
    r.insert("participants", ps);
    hub_->runs.insert(runId_, r);
    hub_->pushRun(runId_);
    QTRY_VERIFY_WITH_TIMEOUT(client_->run(runId_)->randomizerMode() == QLatin1String("each"), 8000);
    startPlugin(R"LUA(
      local c = uz.run.current()
      local z = c.randomizer
      assert(z.mode == "each" and z.preset_id == "classic" and z.tool_version == "1.6.2" and z.allow_own == true)
      assert(z.preset_label == nil)
      local n = 0
      for k in pairs(z) do n = n + 1 end
      assert(n == 4, n)
      assert(z.seed == nil and z.settings == nil and z.base_sha256 == nil)
      assert(c.participants[1].variant_kind == "run" and c.participants[2].variant_kind == nil)
      uz.log.info("rand ok")
    )LUA");
    QCOMPARE(con().status, PluginStatus::Running);
    QVERIFY2(hasLog(con(), "rand ok"), con().error.toUtf8().constData());
  }

  void writesReachTheHub() {
    connectHub();
    startPlugin(R"LUA(
      assert(uz.run.setState({hp = 5, party = {"a", "b"}}) == true)
      assert(uz.run.state().hp == 5, "own write is visible at once")
      assert(uz.run.states()["u_test_1"].party[2] == "b")
      local id = uz.run.event{type = "test.caught", text = "Caught it", payload = {species = 25, shiny = false}, source = "manual"}
      assert(type(id) == "string" and #id == 36, tostring(id))
      assert(uz.run.setBoard({type = "text", text = "Board"}) == true)
      uz.log.info("wrote")
    )LUA");
    QVERIFY2(hasLog(con(), "wrote"), con().error.toUtf8().constData());
    QTRY_VERIFY_WITH_TIMEOUT(!hub_->runStatePuts.isEmpty(), 8000);
    const QJsonObject st = hub_->runStatePuts.last().value("state").toObject();
    QCOMPARE(hub_->runStatePuts.last().value("plugin_id").toString(), kId);
    QCOMPARE(st.value("hp").toInt(), 5);
    QCOMPARE(st.value("party").toArray().size(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!hub_->runEventPosts.isEmpty(), 8000);
    const QJsonObject ev = hub_->runEventPosts.last();
    QCOMPARE(ev.value("plugin_id").toString(), kId);
    QCOMPARE(ev.value("source").toString(), QStringLiteral("manual"));
    QCOMPARE(ev.value("type").toString(), QStringLiteral("test.caught"));
    QCOMPARE(ev.value("payload").toObject().value("species").toInt(), 25);
    QTRY_VERIFY_WITH_TIMEOUT(hub_->runBoards.contains(runId_ + "/" + kId), 8000);
    QCOMPARE(hub_->runBoards.value(runId_ + "/" + kId).value("type").toString(), QStringLiteral("text"));
  }

  void callbacksRunOnThePluginThread() {
    connectHub();
    startPlugin(R"LUA(
      local got = {events = 0, states = 0, status = 0, parts = 0}
      uz.run.on("event", function(e) got.events = got.events + 1 got.lastEvent = e.type got.lastText = e.text end)
      uz.run.on("state", function(user, st, plugin) got.states = got.states + 1 got.user = user got.hp = st.hp got.plugin = plugin end)
      uz.run.on("status", function(s, prev) got.status = got.status + 1 got.s = s got.prev = prev end)
      uz.run.on("participants", function(list) got.parts = got.parts + 1 got.n = #list end)
      uz.on_tick(function()
        if got.events > 0 and got.states > 0 and got.status > 0 and got.parts > 0 and not got.done then
          got.done = true
          uz.log.info(string.format("callbacks e=%s:%s st=%s:%d@%s s=%s<-%s n=%d", got.lastEvent, got.lastText, got.user, got.hp, got.plugin, got.s, got.prev, got.n))
        end
      end)
    )LUA");
    QCOMPARE(con().status, PluginStatus::Running);
    hub_->addRunEvent(runId_, QStringLiteral("test.hit"), QStringLiteral("Ouch"), kOther, kId);
    hub_->sendWs(QStringLiteral("run_state"), {{"run_id", runId_}, {"user_id", kOther}, {"plugin_id", kSecond}, {"state_rev", 1}, {"state", QJsonObject{{"hp", 9}}}});
    QJsonObject r = hub_->runs.value(runId_);
    r.insert("status", "paused");
    QJsonArray ps = r.value("participants").toArray();
    QJsonObject p2 = ps.at(1).toObject();
    p2.insert("status", "joined");
    p2.insert("color", 1);
    ps.replace(1, p2);
    r.insert("participants", ps);
    hub_->runs.insert(runId_, r);
    hub_->pushRun(runId_);
    for (int i = 0; i < 40 && !hasLog(con(), "callbacks"); ++i) {
      mem_->snapshot();
      QTest::qWait(50);
    }
    QString all;
    for (const LogLine& l : con().log) all += l.text + "\n";
    QVERIFY2(hasLog(con(), "callbacks e=test.hit:Ouch st=u_other_1:9@net.test.second s=paused<-active n=2"), qPrintable(con().error + "\n" + all));
  }

  void readsOtherPluginsStateAndWritesOnlyItsOwn() {
    connectHub();
    startPlugin(R"LUA(
      assert(uz.run.setState({mine = 1}) == true)
      uz.on_tick(function()
        local other = uz.run.state("u_other_1", "net.test.second")
        if other and other.hp == 9 and not uz.done then
          uz.done = true
          local all = uz.run.states("net.test.second")
          local own = uz.run.states()
          assert(all["u_other_1"].hp == 9 and own["u_test_1"].mine == 1 and next(own["u_other_1"]) == nil)
          assert(uz.run.state().mine == 1 and uz.run.state(nil, "net.test.second").hp == nil)
          uz.run.setState({mine = 2})
          uz.log.info("read other " .. other.hp)
        end
      end)
    )LUA");
    QCOMPARE(con().status, PluginStatus::Running);
    hub_->sendWs(QStringLiteral("run_state"), {{"run_id", runId_}, {"user_id", kOther}, {"plugin_id", kSecond}, {"state_rev", 4}, {"state", QJsonObject{{"hp", 9}}}});
    for (int i = 0; i < 40 && !hasLog(con(), "read other"); ++i) {
      mem_->snapshot();
      QTest::qWait(50);
    }
    QVERIFY2(hasLog(con(), "read other 9"), qPrintable(con().error));
    QTRY_VERIFY_WITH_TIMEOUT(!hub_->runStatePuts.isEmpty() && hub_->runStatePuts.last().value("state").toObject().value("mine").toInt() == 2, 8000);
    for (const QJsonObject& put : std::as_const(hub_->runStatePuts)) QCOMPARE(put.value("plugin_id").toString(), kId);  // never the other plugin's
  }

  void refusedWhenPausedAndAfterwardsFine() {
    connectHub(QStringLiteral("paused"));
    startPlugin(R"LUA(
      local ok, err = uz.run.setState({a = 1})
      assert(ok == nil and err:find("paused"), tostring(err))
      local id, err2 = uz.run.event{type = "test.x"}
      assert(id == nil and err2:find("paused"), tostring(err2))
      local ok3, err3 = uz.run.setBoard({type = "text", text = "x"})
      assert(ok3 == nil and err3:find("paused"), tostring(err3))
      uz.log.info("refused")
    )LUA");
    QCOMPARE(con().status, PluginStatus::Running);
    QVERIFY2(hasLog(con(), "refused"), con().error.toUtf8().constData());
    QTest::qWait(200);
    QVERIFY(hub_->runStatePuts.isEmpty() && hub_->runEventPosts.isEmpty() && hub_->runBoardPuts == 0);
  }

  void badArgumentsRaiseButWritesNeverPauseOnRefusal() {
    connectHub();
    startPlugin(R"LUA(
      assert(not pcall(uz.run.event, {text = "no type"}))
      assert(not pcall(uz.run.event, {type = 5}))
      assert(not pcall(uz.run.setState, {1, 2, 3}))          -- a list is not a state object
      assert(not pcall(uz.run.setState, "text"))
      assert(not pcall(uz.run.setBoard, {type = "nonsense"}))
      assert(not pcall(uz.run.on, "bogus", function() end))
      local id, err = uz.run.event{type = "Bad Type"}         -- refused by the client: no crash
      uz.log.info("args ok")
    )LUA");
    QCOMPARE(con().status, PluginStatus::Running);
    QVERIFY2(hasLog(con(), "args ok"), con().error.toUtf8().constData());
  }

  // Run view: shell context (no game) with the read-only wrapper: reads and callbacks work, writes return nil + message.
  void runViewIsReadOnly() {
    connectHub();
    PluginSpec s;
    s.permissions = {QStringLiteral("run.state"), QStringLiteral("ui")};
    s.ui = QJsonObject{{"run", QJsonObject{{"title", "Board"}}}};
    s.lua = R"LUA(
      local c = uz.run.current()
      assert(c.view == true and c.name == "Test run" and c.me == "u_test_1")
      assert(next(uz.game) == nil or uz.game.title == nil)
      local ok, e1 = uz.run.setState({hp = 1})
      local id, e2 = uz.run.event{type = "test.x", text = "no"}
      local b, e3 = uz.run.setBoard({type = "text", text = "no"})
      assert(ok == nil and e1 == "read only outside the game", tostring(e1))
      assert(id == nil and e2 == "read only outside the game", tostring(e2))
      assert(b == nil and e3 == "read only outside the game", tostring(e3))
      assert(next(uz.run.state()) == nil and #uz.run.events(0, 10) >= 1)
      local seen = 0
      uz.run.on("event", function(e) seen = seen + 1 uz.ui.set("run", {type = "text", text = "events:" .. seen .. ":" .. e.type}) end)
      uz.ui.set("run", {type = "text", text = "view:" .. tostring(c.view)})
    )LUA";
    const QString dir = writePlugin(dir_->filePath(QStringLiteral("src")), s);
    GameContext ctx;
    ctx.shell = true;
    ctx.run = std::make_shared<ReadOnlyRunBridge>(RunSessionBridge::create(client_.get(), runId_));
    host_->start(ctx, nullptr, {infoFor(dir, dir_->filePath(QStringLiteral("cache")))});
    QTRY_VERIFY_WITH_TIMEOUT(host_->consoleSnapshot(kId).status == PluginStatus::Running, 10000);
    QCOMPARE(host_->uiTree(kId, "run").value("text").toString(), QStringLiteral("view:true"));
    hub_->addRunEvent(runId_, QStringLiteral("test.hit"), QStringLiteral("Ouch"), kOther, kId);
    QTRY_COMPARE_WITH_TIMEOUT(host_->uiTree(kId, "run").value("text").toString(), QStringLiteral("events:1:test.hit"), 8000);
    QVERIFY(hub_->runStatePuts.isEmpty() && hub_->runEventPosts.isEmpty() && !hub_->runBoards.contains(runId_ + "/" + kId));
    QCOMPARE(con().status, PluginStatus::Running);
  }

  void boardIsLimitedToOnePerSecond() {
    connectHub();
    startPlugin(R"LUA(
      for i = 1, 10 do assert(uz.run.setBoard({type = "text", text = "v" .. i})) end
      uz.log.info("sent")
    )LUA");
    QVERIFY2(hasLog(con(), "sent"), con().error.toUtf8().constData());
    // the first goes out at once, the last one follows once the second is over
    QTRY_VERIFY_WITH_TIMEOUT(hub_->runBoards.contains(runId_ + "/" + kId), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(hub_->runBoards.value(runId_ + "/" + kId).value("text").toString(), QStringLiteral("v10"), 8000);
    QVERIFY(hub_->runBoardPuts <= 3);
  }
};

QTEST_MAIN(RunApiTest)
#include "run_test.moc"
