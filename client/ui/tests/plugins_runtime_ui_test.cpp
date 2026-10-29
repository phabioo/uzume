// Plugin UI runtime in the PluginsController (shell context, UI models, actions, toasts, host events) and the
// image://uzplugin provider safety rules. Homebrew plugin folders and generated PNGs only; no QML.
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "uzplugin_testutil.h"
#include "pluginimageprovider.h"
#include "pluginscontroller.h"

using namespace uzume::ui;

namespace {

QString writePlugin(const QString& parent, const QString& id, const QJsonObject& extra, const QString& lua, const QStringList& perms = {"ui"}) {
  const QString dir = QDir(parent).filePath(id);
  QDir().mkpath(dir);
  QJsonObject m{{"id", id}, {"name", id.section('.', -1)}, {"version", "1.0.0"}, {"api", 1}, {"type", "script"}, {"entry", "main.lua"},
                {"permissions", QJsonArray::fromStringList(perms)}};
  for (auto it = extra.begin(); it != extra.end(); ++it) m[it.key()] = it.value();
  QFile mf(dir + "/manifest.json");
  if (!mf.open(QIODevice::WriteOnly)) return {};
  mf.write(QJsonDocument(m).toJson());
  QFile lf(dir + "/main.lua");
  if (!lf.open(QIODevice::WriteOnly)) return {};
  lf.write(lua.toUtf8());
  return dir;
}

const char* kShellLua = R"LUA(
  uz.ui.set('screen', {type='text', text='ready'})
  uz.ui.badge(3)
  uz.ui.set('hud', {type='text', text='hud'})
  uz.ui.on_action(function(a, v, slot) uz.ui.set('screen', {type='text', text=a .. ':' .. tostring(v) .. ':' .. slot}) end)
  uz.events.on('fb', 'detail.game', function(g)
    if g then uz.ui.set('detail', {type='text', text=g.title .. ':' .. g.id}) else uz.ui.clear('detail') end
  end)
  uz.events.on('fb', 'hud.visible', function(v) uz.ui.set('hud', {type='text', text='visible=' .. tostring(v)}) end)
  uz.events.on('fb', 'tiles', function(t) uz.ui.set('tile:' .. t[1].id, {type='text', text=t[1].player}) end)
  uz.ui.notify{ text='hello', actions={ {label='Go', action='go'} } }
)LUA";

// uz.run of the run view: reads from a fixed run, every write attempt is counted (the controller must wrap it read-only).
class FakeRunBridge : public uzume::plugins::RunBridge {
 public:
  QJsonObject current() const override {
    return {{"id", "run-1"}, {"name", "Tide Hardcore"}, {"status", "active"}, {"me", "u1"}, {"plugins", QJsonArray()}, {"participants", QJsonArray()}};
  }
  QJsonObject stateOf(const QString&, const QString&, bool* found) const override {
    *found = true;
    return {{"hp", 4}};
  }
  QJsonObject states(const QString&) const override { return {{"u1", QJsonObject{{"hp", 4}}}}; }
  QJsonArray events(qint64, int) const override { return {}; }
  QString setState(const QString&, const QJsonObject&) override { ++writes; return {}; }
  QString postEvent(const QString&, const QString&, const QString&, const QString&, const QJsonObject&, QString*) override { ++writes; return {}; }
  QString setBoard(const QString&, const QJsonObject&) override { ++writes; return {}; }
  QList<uzume::plugins::RunNotification> takeNotifications() override { return {}; }
  std::atomic<int> writes{0};
};

// A run game: the rules of the run can change while the game runs (run_updated).
class RulesRunBridge : public FakeRunBridge {
 public:
  QJsonObject current() const override {
    QJsonObject o = FakeRunBridge::current();
    o.insert("rules", QJsonObject{{"states", "on"}, {"rewind", "off"}, {"cheats", cheats.load() ? "logged" : "off"}, {"memory_write", memory.load() ? "logged" : "off"}});
    return o;
  }
  std::atomic<bool> cheats{false};
  std::atomic<bool> memory{false};
};

const char* kRunViewLua = R"LUA(
  uz.log.info('boot')
  local c = uz.run.current()
  local ok, err = uz.run.setState({hp = 1})
  local id, err2 = uz.run.event{type = 'x.y'}
  local hp = uz.run.state().hp
  uz.ui.set('run', {type = 'text', text = 'view=' .. tostring(c.view) .. ' hp=' .. tostring(hp) .. ' ' .. tostring(err) .. ' ' .. tostring(err2) .. ' game=' .. tostring(uz.game.title)})
  uz.ui.on_action(function(a, v, slot) uz.ui.set('run', {type = 'text', text = 'act:' .. a .. ':' .. slot}) end)
)LUA";

}  // namespace

class PluginsRuntimeUiTest : public QObject {
  Q_OBJECT

  QTemporaryDir m_data, m_src;
  std::unique_ptr<PluginsController> m_pc;

  void install(const QString& id) {
    const auto r = m_pc->manager()->installFromFolder(m_src.path() + "/" + id);
    QVERIFY2(r.ok, qPrintable(r.error));
  }

 private slots:
  void init() {
    m_pc = std::make_unique<PluginsController>(nullptr, m_data.path(), nullptr);
    QDir(m_data.path() + "/plugins").removeRecursively();
    QDir(m_src.path()).removeRecursively();
    m_pc.reset();
    m_pc = std::make_unique<PluginsController>(nullptr, m_data.path(), nullptr);
  }
  void cleanup() { m_pc.reset(); }

  // 0.20 package 6: the app's emulation service (run rules, cheats_list / cheats_apply) is there for a game without any plugin.
  void emulationServiceWithoutPlugins() {
    QVERIFY(!m_pc->emulation()->active());
    m_pc->gameStarted("nds", m_src.path() + "/missing.nds", "abc", "Homebrew");
    QVERIFY(!m_pc->pluginsRunning() || m_pc->emulation()->active());
    QVERIFY(m_pc->emulation()->active());
    m_pc->gameStopped();
    QVERIFY(!m_pc->emulation()->active());
  }

  void shellContextAndModels() {
    writePlugin(m_src.path(), "net.test.dep", {{"provides", QJsonArray{"dep.api"}}}, "uz.log.info('dep')", {});
    writePlugin(m_src.path(), "net.test.shell",
                {{"requires", QJsonObject{{"net.test.dep", "*"}}},
                 {"ui", QJsonObject{{"screen", QJsonObject{{"title", "Runs"}}}, {"detail", true}, {"hud", true}, {"tile", true},
                                    {"hotkeys", QJsonArray{QJsonObject{{"id", "toggle-hud"}, {"label", "Toggle"}}}}}}},
                kShellLua);
    writePlugin(m_src.path(), "net.test.panel", {{"ui", QJsonObject{{"panel", QJsonObject{{"title", "Panel"}}}}}},
                "uz.ui.set('panel', {type='text', text='in game'}) uz.ui.dot('news')");
    install("net.test.dep");
    install("net.test.shell");
    install("net.test.panel");

    // Nothing runs while the plugin is off or Developer mode is off.
    QVERIFY(m_pc->shellPluginIds().isEmpty());
    m_pc->setPluginEnabled("net.test.shell", true);
    QVERIFY(!m_pc->shellRunning());
    m_pc->setDeveloperMode(true);
    QVERIFY(m_pc->shellRunning());
    QCOMPARE(m_pc->shellPluginIds(), (QStringList{"net.test.dep", "net.test.shell"}));  // required plugin first

    QTRY_COMPARE_WITH_TIMEOUT(m_pc->sidebarEntries().size(), 1, 10000);
    QTRY_COMPARE(m_pc->sidebarEntries().at(0).toMap().value("badge").toInt(), 3);
    QCOMPARE(m_pc->sidebarEntries().at(0).toMap().value("title").toString(), QStringLiteral("shell"));  // title collides with Runs: the plugin name is used
    QCOMPARE(m_pc->sidebarEntries().at(0).toMap().value("pluginId").toString(), QStringLiteral("net.test.shell"));
    QTRY_COMPARE(m_pc->slotTree("net.test.shell", "screen").value("text").toString(), QStringLiteral("ready"));
    QTRY_COMPARE(m_pc->hudWidgets().size(), 1);
    QCOMPARE(m_pc->hotkeys().size(), 1);
    QVERIFY(m_pc->panelTabs().isEmpty());  // net.test.panel is not part of the shell set

    // Toast of the plugin, with its name; dismiss and mute.
    QTRY_COMPARE(m_pc->toasts().size(), 1);
    const QVariantMap toast = m_pc->toasts().at(0).toMap();
    QCOMPARE(toast.value("pluginName").toString(), QStringLiteral("shell"));
    QCOMPARE(toast.value("text").toString(), QStringLiteral("hello"));
    m_pc->dismissToast(toast.value("toastId").toInt());
    QCOMPARE(m_pc->toasts().size(), 0);

    // Action round trip UI -> plugin thread -> on_action -> new tree.
    QSignalSpy slotSpy(m_pc.get(), &PluginsController::slotChanged);
    m_pc->sendAction("net.test.shell", "screen", "pick", 7);
    QTRY_COMPARE(m_pc->slotTree("net.test.shell", "screen").value("text").toString(), QStringLiteral("pick:7:screen"));
    QTRY_VERIFY(slotSpy.count() >= 1);
    m_pc->triggerHotkey("net.test.shell", "toggle-hud");
    QTRY_COMPARE(m_pc->slotTree("net.test.shell", "screen").value("text").toString(), QStringLiteral("toggle-hud:nil:hotkey"));
    m_pc->triggerHotkey("net.test.shell", "unknown");  // not declared: ignored

    // HUD hide event and corner.
    m_pc->hideHud("net.test.shell");
    QTRY_COMPARE(m_pc->slotTree("net.test.shell", "hud").value("text").toString(), QStringLiteral("visible=false"));
    QTRY_VERIFY(m_pc->hudWidgets().at(0).toMap().value("hidden").toBool());
    m_pc->showHud("net.test.shell");
    QTRY_COMPARE(m_pc->slotTree("net.test.shell", "hud").value("text").toString(), QStringLiteral("visible=true"));
    m_pc->setHudCorner("net.test.shell", "bottom-left");
    QCOMPARE(m_pc->hudWidgets().at(0).toMap().value("corner").toString(), QStringLiteral("bottom-left"));

    // Detail game: event in, section out; changing the game clears the section first.
    m_pc->setDetailGame("nds", "ABCD", "ff", "Homebrew", "g1");
    QTRY_COMPARE(m_pc->detailSections().size(), 1);
    QCOMPARE(m_pc->slotTree("net.test.shell", "detail").value("text").toString(), QStringLiteral("Homebrew:g1"));
    m_pc->setDetailGame(QString(), QString(), QString(), QString());
    QTRY_COMPARE(m_pc->detailSections().size(), 0);

    // Multiview tiles.
    m_pc->setTiles({QVariantMap{{"id", "t1"}, {"player", "Ann"}, {"game", "G"}}});
    QTRY_COMPARE(m_pc->tileStrips("t1").size(), 1);
    QCOMPARE(m_pc->tileStrips("t1").at(0).toMap().value("tree").toMap().value("text").toString(), QStringLiteral("Ann"));
    QCOMPARE(m_pc->tileStrips("other").size(), 0);

    // Game start: the shell set stops, the game set runs; game end restarts the shell set.
    m_pc->setPluginEnabled("net.test.panel", true);
    m_pc->gameStarted("nds", m_src.path() + "/missing.nds", "abc", "Homebrew");
    QVERIFY(!m_pc->shellRunning());
    QVERIFY(m_pc->pluginsRunning());
    QTRY_COMPARE_WITH_TIMEOUT(m_pc->panelTabs().size(), 1, 10000);
    QCOMPARE(m_pc->panelTabs().at(0).toMap().value("title").toString(), QStringLiteral("Panel"));
    QTRY_COMPARE(m_pc->panelTabs().at(0).toMap().value("dot").toString(), QStringLiteral("news"));
    QTRY_COMPARE(m_pc->slotTree("net.test.panel", "panel").value("text").toString(), QStringLiteral("in game"));
    m_pc->gameStopped();
    QVERIFY(!m_pc->pluginsRunning());
    QVERIFY(m_pc->shellRunning());
    QCOMPARE(m_pc->shellPluginIds(), (QStringList{"net.test.dep", "net.test.shell"}));
    QTRY_COMPARE_WITH_TIMEOUT(m_pc->panelTabs().size(), 0, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(m_pc->sidebarEntries().size(), 1, 10000);

    // Toggling re-evaluates the shell set; so does Developer mode.
    m_pc->setPluginEnabled("net.test.shell", false);
    QVERIFY(!m_pc->shellRunning());
    QVERIFY(m_pc->shellPluginIds().isEmpty());
    QTRY_COMPARE(m_pc->sidebarEntries().size(), 0);
    m_pc->setPluginEnabled("net.test.shell", true);
    QVERIFY(m_pc->shellRunning());
    m_pc->setDeveloperMode(false);
    QVERIFY(!m_pc->shellRunning());
    QTRY_COMPARE(m_pc->sidebarEntries().size(), 0);
  }

  // ---------------------------------------------------------------- run view

  void installHubCopy(const QString& id, const QString& version, const QJsonObject& ui, const QStringList& perms, const QString& lua,
                      QString* sha, const QJsonObject& requires_ = {}) {
    uzume::plugins::testutil::FbSpec spec;
    spec.id = id;
    spec.version = version;
    spec.permissions = perms;
    spec.ui = ui;
    spec.lua = lua;
    const QByteArray file = uzume::plugins::testutil::makeUzPlugin(spec);
    *sha = uzume::plugins::testutil::sha256Hex(file);
    const auto r = m_pc->manager()->installHubCopy(file, id, version, *sha);
    QVERIFY2(r.ok, qPrintable(r.error));
  }

  void runViewSet() {
    // A local shell plugin, and a plugin that is in both sets (local screen plugin + pinned Hub copy with ui.run).
    writePlugin(m_src.path(), "net.test.shell", {{"ui", QJsonObject{{"screen", QJsonObject{{"title", "Runs"}}}}}}, "uz.ui.set('screen', {type='text', text='shell'})");
    writePlugin(m_src.path(), "net.test.both", {{"ui", QJsonObject{{"screen", QJsonObject{{"title", "Both"}}}}}},
                "uz.log.info('local') uz.ui.set('screen', {type='text', text='local both'})");
    install("net.test.shell");
    install("net.test.both");
    m_pc->setDeveloperMode(true);
    m_pc->setPluginEnabled("net.test.shell", true);
    m_pc->setPluginEnabled("net.test.both", true);
    QCOMPARE(m_pc->shellPluginIds(), (QStringList{"net.test.both", "net.test.shell"}));

    QString shaRun, shaBoth, shaPlain;
    installHubCopy("net.test.runonly", "1.2.0", QJsonObject{{"run", QJsonObject{{"title", "Board"}}}}, {"ui", "run.state"}, kRunViewLua, &shaRun);
    installHubCopy("net.test.both", "2.0.0", QJsonObject{{"run", QJsonObject{{"title", "Both run"}}}, {"screen", QJsonObject{{"title", "Both"}}}},
                   {"ui", "run.state"}, kRunViewLua, &shaBoth);
    installHubCopy("net.test.plain", "1.0.0", QJsonObject{}, {"run.state"}, "uz.log.info('plain')", &shaPlain);

    auto bridge = std::make_shared<FakeRunBridge>();
    RunLaunch launch;
    launch.bridge = bridge;
    launch.pins = {{"net.test.runonly", "1.2.0", shaRun}, {"net.test.both", "2.0.0", shaBoth}, {"net.test.plain", "1.0.0", shaPlain}};

    QSignalSpy sections(m_pc.get(), &PluginsController::runSectionsChanged);
    m_pc->startRunView(launch, "RUN-1");
    QCOMPARE(m_pc->runViewId(), QStringLiteral("run-1"));
    // Only the run's ui.run or ui.screen plugins start; the one that is in both sets runs once (as the run view instance).
    QCOMPARE(m_pc->runViewPluginIds(), (QStringList{"net.test.both", "net.test.runonly"}));
    QCOMPARE(m_pc->shellPluginIds(), (QStringList{"net.test.both", "net.test.shell"}));
    QTRY_COMPARE_WITH_TIMEOUT(m_pc->runSections().size(), 2, 10000);
    QVERIFY(!sections.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(m_pc->slotTree("net.test.runonly", "run").value("text").toString().startsWith("view=true"), 10000);
    const QString text = m_pc->slotTree("net.test.runonly", "run").value("text").toString();
    QCOMPARE(text, QStringLiteral("view=true hp=4 read only outside the game read only outside the game game=nil"));
    QCOMPARE(bridge->writes.load(), 0);
    const QVariantMap sec = m_pc->runSections().at(1).toMap();
    QCOMPARE(sec.value("pluginId").toString(), QStringLiteral("net.test.runonly"));
    // Tab names: the ui.screen title, then the ui.run title, then the plugin name ("Board" belongs to Uzume's own tab).
    QCOMPARE(sec.value("title").toString(), QStringLiteral("net.test.runonly"));
    QCOMPARE(sec.value("slot").toString(), QStringLiteral("run"));
    QCOMPARE(m_pc->runSections().at(0).toMap().value("title").toString(), QStringLiteral("Both"));
    QCOMPARE(m_pc->runSections().at(0).toMap().value("slot").toString(), QStringLiteral("screen"));
    // Not part of the run view: the plugin without ui.run; the local copy of the plugin that is in both sets.
    QTRY_VERIFY_WITH_TIMEOUT(!m_pc->host()->consoleSnapshot("net.test.both").log.isEmpty(), 10000);
    for (const auto& l : m_pc->host()->consoleSnapshot("net.test.both").log) QVERIFY(l.text != QLatin1String("local"));
    QVERIFY(m_pc->host()->consoleSnapshot("net.test.plain").log.isEmpty());
    // The sidebar still lists the screen plugins of the shell set (the run-only plugin has no screen).
    QTRY_COMPARE_WITH_TIMEOUT(m_pc->sidebarEntries().size(), 2, 10000);

    // The "run" slot takes UI actions.
    m_pc->sendAction("net.test.runonly", "run", "go", 1);
    QTRY_COMPARE(m_pc->slotTree("net.test.runonly", "run").value("text").toString(), QStringLiteral("act:go:run"));

    // The same run again keeps the running plugins (no restart).
    auto bootCount = [&] {
      int n = 0;
      for (const auto& l : m_pc->host()->consoleSnapshot("net.test.runonly").log) n += l.text == QLatin1String("boot");
      return n;
    };
    QCOMPARE(bootCount(), 1);
    m_pc->startRunView(launch, "run-1");
    QTest::qWait(200);
    QCOMPARE(bootCount(), 1);
    QCOMPARE(m_pc->slotTree("net.test.runonly", "run").value("text").toString(), QStringLiteral("act:go:run"));

    // Closing the detail stops the set and restores the normal shell set (the local copy of the shared plugin again).
    m_pc->stopRunView();
    QVERIFY(m_pc->runViewPluginIds().isEmpty());
    QVERIFY(m_pc->runSections().isEmpty());
    QVERIFY(m_pc->shellRunning());
    QCOMPARE(m_pc->shellPluginIds(), (QStringList{"net.test.both", "net.test.shell"}));
    QTRY_VERIFY_WITH_TIMEOUT(m_pc->slotTree("net.test.runonly", "run").isEmpty(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(m_pc->slotTree("net.test.both", "screen").value("text").toString(), QStringLiteral("local both"), 10000);

    // A game start ends the run view; it does not come back by itself, and the run view cannot start during a game.
    m_pc->startRunView(launch, "run-1");
    QTRY_COMPARE_WITH_TIMEOUT(m_pc->runSections().size(), 2, 10000);
    QSignalSpy inGame(m_pc.get(), &PluginsController::inGameChanged);
    m_pc->gameStarted("nds", m_src.path() + "/missing.nds", "abc", "Homebrew");
    QCOMPARE(inGame.size(), 1);
    QVERIFY(m_pc->inGame());
    QVERIFY(m_pc->runViewPluginIds().isEmpty());
    QVERIFY(m_pc->runSections().isEmpty());
    QVERIFY(m_pc->runViewId().isEmpty());
    m_pc->startRunView(launch, "run-1");
    QVERIFY(m_pc->runViewId().isEmpty());
    m_pc->gameStopped();
    QVERIFY(m_pc->runSections().isEmpty());
    QCOMPARE(m_pc->shellPluginIds(), (QStringList{"net.test.both", "net.test.shell"}));
  }

  // One consent gate at the start boundary: a run launch whose pins the person has not accepted starts no plugin and raises the request; once
  // accepted the plugins start. Game start and end advance the pin generation, so a pending pin download of an earlier run is dropped.
  void runPinsGateBlocksStartUntilAccepted() {
    QString sha;
    installHubCopy("net.test.gated", "1.0.0", QJsonObject{}, {"run.state"}, "uz.log.info('gated')", &sha);
    auto bridge = std::make_shared<RulesRunBridge>();
    RunLaunch launch;
    launch.bridge = bridge;
    launch.pins = {{"net.test.gated", "1.0.0", sha}};
    bool accepted = false;
    int raised = 0;
    QList<uzume::plugins::RunPluginPin> seen;
    m_pc->setRunPinsGate([&](const QString&, const QList<uzume::plugins::RunPluginPin>& pins, bool raise) {
      seen = pins;
      if (!accepted && raise) ++raised;
      return accepted;
    });
    const quint64 g0 = m_pc->pinsGeneration();
    m_pc->gameStarted("nds", m_src.path() + "/missing.nds", "abc", "Homebrew", QString(), launch);
    QVERIFY(m_pc->pinsGeneration() > g0);
    QTest::qWait(500);
    QVERIFY(m_pc->host()->pluginIds().isEmpty());  // not started without the agreement
    QCOMPARE(raised, 1);
    QCOMPARE(seen.size(), 1);  // the gate saw the exact pins about to start
    accepted = true;
    m_pc->retryRunSet();
    QTRY_VERIFY_WITH_TIMEOUT(m_pc->host()->pluginIds().contains("net.test.gated"), 10000);
    const quint64 g1 = m_pc->pinsGeneration();
    m_pc->gameStopped();
    QVERIFY(m_pc->pinsGeneration() > g1);
    m_pc->setRunPinsGate({});
  }

  // 0.20 final live check, P1: in a run the plugins pinned to the run start, plus the person's own enabled plugins that change the
  // game (cheats / memory.write) while the run's rules allow that; they stop again when the rule goes back to off.
  void runGameStartsCheatsPluginByRules() {
    writePlugin(m_src.path(), "net.test.cheater", {}, "uz.log.info('cheater boot')", {"cheats"});
    writePlugin(m_src.path(), "net.test.editor", {{"requires", QJsonObject{{"net.test.core", "*"}}}}, "uz.log.info('editor boot')", {"memory.write"});
    writePlugin(m_src.path(), "net.test.core", {{"provides", QJsonArray{"core.api"}}}, "uz.log.info('core boot')", {});
    writePlugin(m_src.path(), "net.test.harmless", {}, "uz.log.info('harmless')", {"ui"});
    for (const char* id : {"net.test.cheater", "net.test.editor", "net.test.core", "net.test.harmless"}) install(id);
    m_pc->setDeveloperMode(true);
    for (const char* id : {"net.test.cheater", "net.test.editor", "net.test.core", "net.test.harmless"}) m_pc->setPluginEnabled(id, true);
    QString shaPlain;
    installHubCopy("net.test.pinned", "1.0.0", QJsonObject{}, {"run.state"}, "uz.log.info('pinned')", &shaPlain);

    auto bridge = std::make_shared<RulesRunBridge>();
    RunLaunch launch;
    launch.bridge = bridge;
    launch.pins = {{"net.test.pinned", "1.0.0", shaPlain}};
    m_pc->gameStarted("nds", m_src.path() + "/missing.nds", "abc", "Homebrew", QString(), launch);
    // Rules off: only the pinned plugin runs (run.state pinning unchanged).
    QTRY_VERIFY_WITH_TIMEOUT(m_pc->host()->pluginIds().contains("net.test.pinned"), 10000);
    QCOMPARE(m_pc->host()->pluginIds(), QStringList{"net.test.pinned"});

    // Cheats allowed: the cheats plugin joins; the editor needs memory changes, the harmless one is never part of a run.
    bridge->cheats = true;
    QTRY_VERIFY_WITH_TIMEOUT(m_pc->host()->pluginIds().contains("net.test.cheater"), 10000);
    QVERIFY(m_pc->host()->pluginIds().contains("net.test.pinned"));
    QVERIFY(!m_pc->host()->pluginIds().contains("net.test.editor"));
    QVERIFY(!m_pc->host()->pluginIds().contains("net.test.harmless"));

    // Memory changes allowed too: the editor joins with what it needs.
    bridge->memory = true;
    QTRY_VERIFY_WITH_TIMEOUT(m_pc->host()->pluginIds().contains("net.test.editor"), 10000);
    QVERIFY(m_pc->host()->pluginIds().contains("net.test.core"));

    // Back to off: both stop, the pinned plugin stays.
    bridge->cheats = false;
    bridge->memory = false;
    QTRY_COMPARE_WITH_TIMEOUT(m_pc->host()->pluginIds(), QStringList{"net.test.pinned"}, 10000);
    m_pc->gameStopped();

    // Without a run nothing of this applies: the game's own plugins start by their game match.
    m_pc->gameStarted("nds", m_src.path() + "/missing.nds", "abc", "Homebrew");
    QTRY_VERIFY_WITH_TIMEOUT(m_pc->host()->pluginIds().contains("net.test.cheater"), 10000);
    m_pc->gameStopped();
  }

  void toastMuting() {
    writePlugin(m_src.path(), "net.test.shell", {{"ui", QJsonObject{{"screen", QJsonObject{{"title", "Runs"}}}}}}, "uz.ui.notify{ text='a' }");
    install("net.test.shell");
    m_pc->setDeveloperMode(true);
    m_pc->muteToasts("net.test.shell", "always");
    m_pc->setPluginEnabled("net.test.shell", true);
    QTRY_VERIFY(m_pc->shellRunning());
    QTest::qWait(300);
    QCOMPARE(m_pc->toasts().size(), 0);  // muted
    m_pc->muteToasts("net.test.shell", "off");
    m_pc->setPluginEnabled("net.test.shell", false);
    m_pc->setPluginEnabled("net.test.shell", true);
    QTRY_COMPARE_WITH_TIMEOUT(m_pc->toasts().size(), 1, 10000);
  }

  // ---------------------------------------------------------------- developer console (0.22)

  QVariantMap toastWithTitle(const QString& prefix) const {
    for (const QVariant& t : m_pc->toasts())
      if (t.toMap().value("title").toString().startsWith(prefix)) return t.toMap();
    return {};
  }

  void reloadToastAndConsoleModel() {
    writePlugin(m_src.path(), "net.test.shell", {{"name", "Item Helper"}, {"ui", QJsonObject{{"screen", QJsonObject{{"title", "Runs"}}}}}}, "uz.log.info('hi')");
    install("net.test.shell");
    m_pc->setDeveloperMode(true);
    m_pc->setPluginEnabled("net.test.shell", true);
    QTRY_VERIFY(m_pc->shellRunning());
    DevConsole* dc = m_pc->devConsole();
    QTRY_COMPARE_WITH_TIMEOUT(dc->buildModel().value("status").toString(), QStringLiteral("running"), 10000);
    QCOMPARE(dc->buildModel().value("selected").toString(), QStringLiteral("net.test.shell"));
    QCOMPARE(dc->buildModel().value("reload").toMap().value("count").toInt(), 0);
    QSignalSpy reloaded(dc, &DevConsole::reloaded);
    m_pc->reloadPlugin("net.test.shell");
    QTRY_COMPARE_WITH_TIMEOUT(reloaded.size(), 1, 10000);
    const qint64 ms = reloaded.at(0).at(2).toLongLong();
    QVERIFY(ms >= 0);
    const QVariantMap toast = toastWithTitle("Item Helper reloaded");
    QVERIFY(!toast.isEmpty());
    QCOMPARE(toast.value("title").toString(), QStringLiteral("Item Helper reloaded · %1 ms").arg(ms));
    QVERIFY(toast.value("host").toBool());
    QCOMPARE(dc->buildModel().value("reload").toMap().value("ms").toLongLong(), ms);
    // Only a person in Developer mode gets the toast (a plain "Restart plugin" stays quiet).
    m_pc->dismissToast(toast.value("toastId").toInt());
    m_pc->devConsole()->resetSeen();
    m_pc->setDeveloperMode(false);
    m_pc->setDeveloperMode(true);
    QCOMPARE(m_pc->toasts().size(), 0);
  }

  void threeErrorsPauseWithToast() {
    writePlugin(m_src.path(), "net.test.shell", {{"name", "Item Helper"}, {"ui", QJsonObject{{"screen", QJsonObject{{"title", "Runs"}}}}}},
                "uz.ui.on_action(function() error('boom') end)");
    install("net.test.shell");
    m_pc->setDeveloperMode(true);
    m_pc->setPluginEnabled("net.test.shell", true);
    QTRY_VERIFY(m_pc->shellRunning());
    DevConsole* dc = m_pc->devConsole();
    QTRY_COMPARE_WITH_TIMEOUT(dc->buildModel().value("status").toString(), QStringLiteral("running"), 10000);
    QSignalSpy paused(dc, &DevConsole::pausedAfterError);
    m_pc->sendAction("net.test.shell", "screen", "x");
    QTRY_COMPARE_WITH_TIMEOUT(dc->buildModel().value("counts").toMap().value("error").toInt(), 1, 10000);
    QCOMPARE(dc->buildModel().value("status").toString(), QStringLiteral("running"));  // one error is only logged
    QVERIFY(toastWithTitle("Item Helper paused").isEmpty());
    m_pc->sendAction("net.test.shell", "screen", "x");
    m_pc->sendAction("net.test.shell", "screen", "x");
    QTRY_COMPARE_WITH_TIMEOUT(paused.size(), 1, 10000);
    const QVariantMap toast = toastWithTitle("Item Helper paused after an error");
    QVERIFY(!toast.isEmpty());
    const QVariantList acts = toast.value("actions").toList();
    QCOMPARE(acts.size(), 2);
    QCOMPARE(acts.at(0).toMap().value("label").toString(), QStringLiteral("Restart plugin"));
    QCOMPARE(acts.at(1).toMap().value("label").toString(), QStringLiteral("Open console"));
    const QVariantMap m = dc->buildModel();
    QVERIFY(m.value("paused").toBool());
    QCOMPARE(m.value("errorFirst").toString().contains("boom"), true);
    QCOMPARE(m.value("counts").toMap().value("error").toInt(), 4);  // three handler errors and the "Paused:" line
    // Reload and resume: the plugin runs again; the toast for a reload follows.
    m_pc->reloadPlugin("net.test.shell");
    QTRY_COMPARE_WITH_TIMEOUT(dc->buildModel().value("status").toString(), QStringLiteral("running"), 10000);
  }

  void loadUnpackedChecksTheManifest() {
    m_pc->setDeveloperMode(true);
    // 4d-2B: a manifest problem installs nothing.
    const QString bad = writePlugin(m_src.path(), "net.test.bad", {{"name", "Item Helper"}, {"permissions", "oops"}}, "uz.log.info('x')", {});
    const QVariantMap r = m_pc->loadUnpacked(bad);
    QCOMPARE(r.value("state").toString(), QStringLiteral("error"));
    QCOMPARE(r.value("title").toString(), QStringLiteral("Item Helper didn't load"));
    QVERIFY(r.value("detail").toString().startsWith(QStringLiteral("manifest.json: ")));
    QCOMPARE(r.value("note").toString(), QStringLiteral("Nothing was installed."));
    QCOMPARE(m_pc->plugins().size(), 0);
    QCOMPARE(m_pc->unpackedResult().value("state").toString(), QStringLiteral("error"));
    m_pc->dismissUnpackedResult();
    QVERIFY(m_pc->unpackedResult().isEmpty());
    QVERIFY(!m_pc->loadUnpacked(m_src.path() + "/missing").value("detail").toString().isEmpty());
    QCOMPARE(m_pc->plugins().size(), 0);
    // A good folder is linked and turned on; the console lists it with the Unpacked flag and its folder.
    const QString good = writePlugin(m_src.path(), "net.test.good", {{"name", "Item Helper"}, {"ui", QJsonObject{{"screen", QJsonObject{{"title", "Runs"}}}}}}, "uz.log.info('ok')");
    const QVariantMap ok = m_pc->loadUnpacked(good);
    QCOMPARE(ok.value("state").toString(), QStringLiteral("ok"));
    QCOMPARE(m_pc->plugins().size(), 1);
    QVERIFY(m_pc->plugins().at(0).toMap().value("devLinked").toBool());
    QVERIFY(m_pc->plugins().at(0).toMap().value("enabled").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(m_pc->shellRunning(), 10000);
    const QVariantMap model = m_pc->devConsole()->buildModel();
    QVERIFY(model.value("unpacked").toBool());
    QVERIFY(QDir(model.value("folder").toString()) == QDir(good));
    // A second folder: the console switches to the plugin that was just loaded (not the first one it listed).
    const QString second = writePlugin(m_src.path(), "net.test.second", {{"name", "Second Helper"}}, "uz.log.info('ok')");
    QCOMPARE(m_pc->loadUnpacked(second).value("state").toString(), QStringLiteral("ok"));
    QCOMPARE(m_pc->devConsole()->selectedId(), QStringLiteral("net.test.second"));
    m_pc->selectConsolePlugin(QStringLiteral("net.test.good"));
    QCOMPARE(m_pc->devConsole()->selectedId(), QStringLiteral("net.test.good"));
    // Developer mode off: nothing loads.
    m_pc->setDeveloperMode(false);
    QCOMPARE(m_pc->loadUnpacked(good).value("state").toString(), QStringLiteral("error"));
  }

  void reloadOnFileChangeSwitch() {
    QVERIFY(m_pc->manager()->watchEnabled());
    QVERIFY(m_pc->devConsole()->reloadOnChange());
    m_pc->devConsole()->setReloadOnChange(false);
    QVERIFY(!m_pc->manager()->watchEnabled());
    m_pc->devConsole()->setReloadOnChange(true);
    QVERIFY(m_pc->manager()->watchEnabled());
  }

  // The debug MCP game_start identity: only with Developer mode, once, and plugin matching sees it.
  void identityOverrideGuard() {
    writePlugin(m_src.path(), "net.test.gba", {{"games", QJsonArray{QJsonObject{{"system", "gba"}, {"codes", QJsonArray{"AXVE"}}}}}}, "uz.log.info('matched')", {});
    install("net.test.gba");
    m_pc->setPluginEnabled("net.test.gba", true);
    QVERIFY(!m_pc->setIdentityOverride("gba", "AXVE"));  // Developer mode is off
    QVERIFY(m_pc->identityOverride().isEmpty());
    m_pc->setDeveloperMode(true);
    QVERIFY(!m_pc->setIdentityOverride("GBA!", "AXVE"));
    QVERIFY(!m_pc->setIdentityOverride("gba", ""));
    QVERIFY(m_pc->setIdentityOverride("gba", "AXVE"));
    QCOMPARE(m_pc->identityOverride().value("code").toString(), QStringLiteral("AXVE"));
    m_pc->gameStarted("nds", QString(), QString(), "Test");
    QVERIFY(m_pc->pluginsRunning());
    QCOMPARE(m_pc->devConsole()->buildModel().value("selected").toString(), QStringLiteral("net.test.gba"));
    QVERIFY(m_pc->identityOverride().isEmpty());  // used once
    // A "paused after an error" notice of this game does not carry over into the next game.
    emit m_pc->devConsole()->pausedAfterError(QStringLiteral("net.test.gba"), QStringLiteral("boom"));
    QCOMPARE(m_pc->toasts().size(), 1);
    m_pc->gameStopped();
    QCOMPARE(m_pc->toasts().size(), 0);
    m_pc->gameStarted("nds", QString(), QString(), "Test");  // the real identity matches no plugin
    QVERIFY(!m_pc->pluginsRunning());
    m_pc->gameStopped();
    // Switched off between the call and the start: ignored.
    QVERIFY(m_pc->setIdentityOverride("gba", "AXVE"));
    m_pc->setDeveloperMode(false);
    m_pc->setDeveloperMode(true);
    m_pc->clearIdentityOverride();
    QVERIFY(m_pc->identityOverride().isEmpty());
    QVERIFY(m_pc->setIdentityOverride("gba", "AXVE"));
    m_pc->setDeveloperMode(false);
    m_pc->gameStarted("nds", QString(), QString(), "Test");
    QVERIFY(!m_pc->pluginsRunning());
    m_pc->gameStopped();
  }

  // ---------------------------------------------------------------- image provider

  void imageProviderRules() {
    QTemporaryDir root;
    const QString cache = root.path() + "/cache";
    const QString plug = root.path() + "/plugin";
    QDir().mkpath(cache + "/icons");
    QDir().mkpath(plug + "/assets");
    QImage ok(8, 6, QImage::Format_ARGB32);
    ok.fill(Qt::red);
    QVERIFY(ok.save(cache + "/icons/25.png", "PNG"));
    QVERIFY(ok.save(plug + "/assets/a.png", "PNG"));
    QVERIFY(ok.save(plug + "/secret.png", "PNG"));
    QVERIFY(ok.save(root.path() + "/outside.png", "PNG"));
    QImage wide(5000, 1, QImage::Format_ARGB32);
    wide.fill(Qt::blue);
    QVERIFY(wide.save(cache + "/wide.png", "PNG"));
    {
      QFile big(cache + "/big.png");
      QVERIFY(big.open(QIODevice::WriteOnly));
      big.write(QByteArray(9 * 1024 * 1024, 'x'));
    }
    const bool linked = QFile::link(root.path() + "/outside.png", cache + "/link.png");

    PluginImageSource src{cache, plug, {"assets"}};
    QString err;
    QVERIFY(!PluginImageProvider::resolve(src, "p/cache/icons/25.png", &err).isEmpty());
    QVERIFY(!PluginImageProvider::resolve(src, "p/asset/assets/a.png", &err).isEmpty());
    QVERIFY(PluginImageProvider::resolve(src, "p/cache/../outside.png", &err).isEmpty());
    QVERIFY2(err.contains("invalid path"), qPrintable(err));
    QVERIFY(PluginImageProvider::resolve(src, "p/cache/icons/../../outside.png", &err).isEmpty());
    QVERIFY(PluginImageProvider::resolve(src, "p/cache//icons/25.png", &err).isEmpty());
    QVERIFY(PluginImageProvider::resolve(src, "p/cache/%2e%2e/outside.png", &err).isEmpty());
    QVERIFY(PluginImageProvider::resolve(src, "p/asset/secret.png", &err).isEmpty());  // not declared
    QVERIFY2(err.contains("not declared"), qPrintable(err));
    QVERIFY(PluginImageProvider::resolve(src, "p/cache/icons/25.jpg", &err).isEmpty());
    QVERIFY(PluginImageProvider::resolve(src, "p/other/icons/25.png", &err).isEmpty());
    QVERIFY(PluginImageProvider::resolve(src, "p/cache/missing.png", &err).isEmpty());
#ifndef Q_OS_WIN  // QFile::link makes a .lnk shortcut on Windows, not a symlink; the provider never follows .lnk files
    if (linked) {
      QVERIFY(PluginImageProvider::resolve(src, "p/cache/link.png", &err).isEmpty());
      QVERIFY2(err.contains("leaves the plugin folder"), qPrintable(err));
    }
#else
    // A .lnk shortcut is just a file of non-PNG bytes: it is never followed and loading it fails as "not a PNG".
    {
      const QString lnk = cache + "/shortcut.png";
      QFile f(lnk);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(QByteArray("L\0\0\0\x01\x14\x02\0", 8));  // .lnk header bytes
      f.close();
      QVERIFY(PluginImageProvider::loadPng(lnk, &err).isNull());
      QVERIFY(!err.isEmpty());
    }
#endif

    const QImage img = PluginImageProvider::loadPng(cache + "/icons/25.png", &err);
    QCOMPARE(img.size(), QSize(8, 6));
    QVERIFY(PluginImageProvider::loadPng(cache + "/big.png", &err).isNull());
    QVERIFY2(err.contains("8 MiB"), qPrintable(err));
    QVERIFY(PluginImageProvider::loadPng(cache + "/wide.png", &err).isNull());
    QVERIFY2(err.contains("4096"), qPrintable(err));

    auto sources = std::make_shared<PluginImageSources>();
    sources->set({{"p", src}});
    PluginImageProvider provider(sources);
    QSize size;
    QCOMPARE(provider.requestImage("p/cache/icons/25.png", &size, QSize()).size(), QSize(8, 6));
    QCOMPARE(size, QSize(8, 6));
    QVERIFY(provider.requestImage("q/cache/icons/25.png", &size, QSize()).isNull());  // unknown plugin
    QVERIFY(provider.requestImage("p/cache/../outside.png", &size, QSize()).isNull());
  }

  // PI-6: requestedSize is honored, large images are scaled down while decoding, and decoding is budgeted per plugin.
  void imagesAreScaledAndBudgeted() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage big(3000, 2000, QImage::Format_ARGB32);
    big.fill(QColor(10, 120, 200));
    const QString path = dir.filePath("big.png");
    QVERIFY(big.save(path, "PNG"));
    QString err;
    QImage full = PluginImageProvider::loadPng(path, &err);  // no requestedSize: bounded by kMaxDeliveredImageDim
    QVERIFY2(!full.isNull(), qPrintable(err));
    QVERIFY(full.width() <= kMaxDeliveredImageDim && full.height() <= kMaxDeliveredImageDim);
    QVERIFY(qAbs(double(full.width()) / full.height() - 1.5) < 0.01);
    const QImage small = PluginImageProvider::loadPng(path, &err, QSize(48, 36));
    QVERIFY2(!small.isNull(), qPrintable(err));
    QVERIFY(small.width() <= 48 && small.height() <= 36);
    QVERIFY(qAbs(double(small.width()) / small.height() - 1.5) < 0.1);
    QImage tiny(8, 6, QImage::Format_ARGB32);
    tiny.fill(Qt::red);
    QVERIFY(tiny.save(dir.filePath("tiny.png"), "PNG"));
    QCOMPARE(PluginImageProvider::loadPng(dir.filePath("tiny.png"), &err, QSize(200, 200)).size(), QSize(8, 6));  // never upscaled

    PluginPixelBudget budget;
    const auto t0 = PluginPixelBudget::Clock::now();
    const qint64 each = 4096ll * 4096;
    int allowed = 0;
    while (budget.take("p", each, t0)) ++allowed;
    QCOMPARE(allowed, int(kPluginDecodePixelsPerWindow / each));
    QVERIFY(budget.take("q", each, t0));  // other plugins are not affected
    QVERIFY(budget.take("p", each, t0 + std::chrono::milliseconds(kPluginDecodeWindowMs + 1)));  // the window moved on
    QVERIFY(PluginImageProvider::loadPng(path, &err, QSize(), &budget, "z").width() > 0);
    PluginPixelBudget tight;
    for (int i = 0; i < 200; ++i) (void)tight.take("z", 1'000'000);
    QVERIFY(PluginImageProvider::loadPng(path, &err, QSize(), &tight, "z").isNull());
    QVERIFY2(err.contains("too many image pixels"), qPrintable(err));
  }
};

QTEST_MAIN(PluginsRuntimeUiTest)
#include "plugins_runtime_ui_test.moc"
