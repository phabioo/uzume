// fb.variants (permission variants.build): register round trip into the registry, validation, engine / list / build with a fake
// provider, the "variants.state" host event, and the gate in the plugin manager (signed official plugins only).
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <mutex>

#include "fbplugin_testutil.h"
#include "plugin_host.h"
#include "plugin_manager.h"
#include "plugin_test_util.h"
#include "plugin_variants.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

namespace {

class FakeVariants : public VariantsProvider {
 public:
  std::shared_ptr<VariantRegistry> reg = std::make_shared<VariantRegistry>();
  std::mutex m;
  QList<VariantBuildRequest> requests;
  QString lastListGame;

  std::shared_ptr<VariantRegistry> registry() override { return reg; }
  VariantEngineInfo engine() const override { return {true, QStringLiteral("1.6.2"), false}; }
  QList<VariantRow> list(const QString& gameId) const override {
    const_cast<FakeVariants*>(this)->lastListGame = gameId;
    return {{QStringLiteral("v1"), QStringLiteral("Randomizer #1"), QStringLiteral("var-12345678"), QStringLiteral("classic"), QStringLiteral("ready"), QStringLiteral("1.6.2")}};
  }
  quint64 build(const QString&, const VariantBuildRequest& r) override {
    std::lock_guard<std::mutex> l(m);
    requests.append(r);
    return static_cast<quint64>(requests.size());
  }
};

QString writeFile(const QString& path, const QByteArray& bytes) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return {};
  f.write(bytes);
  return path;
}

bool hasLog(const ConsoleSnapshot& c, const QString& needle) {
  for (const LogLine& l : c.log)
    if (l.text.contains(needle)) return true;
  return false;
}

const char* kRegister = R"LUA(
  fb.variants.register{
    engine = "randomizer",
    presets = {
      { id = "classic", label = "Classic", hint = "Plain shuffle", settings = { ["427"] = "427old", ["428"] = "428new" } },
      { id = "chaos", label = "Chaos", settings = { ["428"] = "428chaos" } },
    },
    games = { { system = "nds", codes = { "IPKE", "ipkd", "POKEMON YEL*" } }, { system = "3ds" } },
  }
)LUA";

}  // namespace

class VariantsPluginTest : public QObject {
  Q_OBJECT

  QTemporaryDir m_dir;
  PluginHost* m_host = nullptr;
  std::shared_ptr<FakeVariants> m_prov;

  void startWith(const QStringList& perms, const QString& lua, bool shell = false) {
    PluginSpec s;
    s.permissions = perms;
    s.lua = lua;
    GameContext ctx{"nds", "ABCD", "abc123", "Test Game", "", "en"};
    ctx.shell = shell;
    ctx.gameId = shell ? QString() : QStringLiteral("game-1");
    ctx.variants = m_prov;
    m_host->start(ctx, nullptr, {infoFor(writePlugin(m_dir.path() + "/src", s), m_dir.path() + "/cache")});
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Running, 10000);
  }
  ConsoleSnapshot con() const { return m_host->consoleSnapshot("net.test.plugin"); }

 private slots:
  void init() {
    m_prov = std::make_shared<FakeVariants>();
    m_host = new PluginHost;
    m_host->setTickHz(200);
  }
  void cleanup() {
    delete m_host;
    m_host = nullptr;
    QDir(m_dir.path() + "/src").removeRecursively();
    QDir(m_dir.path() + "/cache").removeRecursively();
  }

  void withoutThePermissionThereIsNoApi() {
    startWith({"memory.read"}, "fb.log.info('variants=' .. tostring(fb.variants))");
    QTRY_VERIFY(hasLog(con(), "variants=nil"));
    QVERIFY(m_prov->reg->all().isEmpty());
  }

  void registerRoundTrip() {
    startWith({"variants.build"}, QString::fromLatin1(kRegister) + "fb.log.info('registered')");
    QTRY_VERIFY(hasLog(con(), "registered"));
    const auto all = m_prov->reg->all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.first().pluginId, QStringLiteral("net.test.plugin"));
    QCOMPARE(all.first().presets.size(), 2);
    QCOMPARE(all.first().presets.first().label, QStringLiteral("Classic"));
    QCOMPARE(all.first().presets.first().hint, QStringLiteral("Plain shuffle"));
    QCOMPARE(all.first().presets.first().bestSettings(), QStringLiteral("428new"));  // the highest settings version
    QVERIFY(m_prov->reg->supported(QStringLiteral("nds"), QStringLiteral("IPKE")));
    QVERIFY(m_prov->reg->supported(QStringLiteral("nds"), QStringLiteral("IPKD")));  // codes compare ignoring case
    QVERIFY(!m_prov->reg->supported(QStringLiteral("nds"), QStringLiteral("XXXX")));
    QVERIFY(!m_prov->reg->supported(QStringLiteral("nds"), QStringLiteral("IPK")));
    QVERIFY(m_prov->reg->supported(QStringLiteral("nds"), QStringLiteral("Pokemon Yellow")));  // codes are globs like manifest games
    QVERIFY(!m_prov->reg->supported(QStringLiteral("nds"), QStringLiteral("POKEMON RED")));
    QVERIFY(m_prov->reg->supported(QStringLiteral("3ds"), QStringLiteral("00040000000FB000")));  // no codes = the whole system
    QVERIFY(!m_prov->reg->supported(QStringLiteral("gba"), QStringLiteral("IPKE")));
    QCOMPARE(m_prov->reg->presetsFor(QStringLiteral("nds"), QStringLiteral("IPKE")).size(), 2);
    QCOMPARE(m_prov->reg->preset(QStringLiteral("chaos")).bestSettings(), QStringLiteral("428chaos"));
    // Gone with the plugin: disabled or removed plugins are pruned from the registry.
    m_prov->reg->keepOnly({});
    QVERIFY(m_prov->reg->all().isEmpty());
  }

  void invalidRegistrationsAreErrors() {
    startWith({"variants.build"}, R"LUA(
      local bad = {
        { engine = "other" },
        { presets = { { id = "custom", settings = { ["428"] = "x" } } } },   -- Uzume adds "custom" itself
        { presets = { { id = "ok", settings = {} } } },
        { presets = { { id = "ok", settings = { ["4"] = "x" } } } },
        { games = { { codes = { "A" } } } },
      }
      for i, t in ipairs(bad) do
        local ok, e = pcall(fb.variants.register, t)
        assert(not ok, "case " .. i .. " accepted")
      end
      fb.log.info("all refused")
    )LUA");
    QTRY_VERIFY(hasLog(con(), "all refused"));
    QVERIFY(m_prov->reg->all().isEmpty());
  }

  void engineListAndBuild() {
    startWith({"variants.build"}, R"LUA(
      local e = fb.variants.engine()
      fb.log.info(string.format("engine offered=%s version=%s on_pc=%s", tostring(e.offered), tostring(e.version), tostring(e.on_pc)))
      local l = fb.variants.list()
      fb.log.info(string.format("list n=%d id=%s slot=%s preset=%s state=%s seed=%s", #l, l[1].id, l[1].slot, l[1].preset_id, l[1].state, tostring(l[1].seed)))
      local id = fb.variants.build{ name = "Mine", preset_id = "classic", seed = 123, spoiler_log = true }
      fb.log.info("build id=" .. id)
      fb.events.on("fb", "variants.state", function(ev) fb.log.info(string.format("state id=%d %s %.2f %s", ev.id, ev.state, ev.progress, tostring(ev.error))) end)
      assert(not pcall(fb.variants.build, { name = "x" }))                                 -- neither preset nor settings
      assert(not pcall(fb.variants.build, { name = "x", preset_id = "a", settings = "b" })) -- both
      assert(not pcall(fb.variants.build, { name = "", preset_id = "a" }))
      assert(not pcall(fb.variants.build, { name = "x", preset_id = "a", seed = -5 }))
    )LUA");
    QTRY_VERIFY(hasLog(con(), "engine offered=true version=1.6.2 on_pc=false"));
    QTRY_VERIFY(hasLog(con(), "list n=1 id=v1 slot=var-12345678 preset=classic state=ready seed=nil"));  // never the seed
    QTRY_VERIFY(hasLog(con(), "build id=1"));
    QCOMPARE(m_prov->lastListGame, QStringLiteral("game-1"));
    QCOMPARE(m_prov->requests.size(), 1);
    QCOMPARE(m_prov->requests.first().gameId, QStringLiteral("game-1"));
    QCOMPARE(m_prov->requests.first().name, QStringLiteral("Mine"));
    QCOMPARE(m_prov->requests.first().presetId, QStringLiteral("classic"));
    QCOMPARE(m_prov->requests.first().seed, QStringLiteral("123"));
    QVERIFY(m_prov->requests.first().spoilerLog);
    m_host->postHostEvent(QStringLiteral("variants.state"), QVariantMap{{"id", 1}, {"state", "building"}, {"progress", 0.5}});
    QTRY_VERIFY(hasLog(con(), "state id=1 building 0.50 nil"));
    m_host->postHostEvent(QStringLiteral("variants.state"), QVariantMap{{"id", 1}, {"state", "failed"}, {"progress", 0.0}, {"error", "engine_failed"}});
    QTRY_VERIFY(hasLog(con(), "state id=1 failed 0.00 engine_failed"));
  }

  void shellContextNeedsAGameId() {
    startWith({"variants.build"}, R"LUA(
      assert(not pcall(fb.variants.build, { name = "x", preset_id = "a" }))
      local id = fb.variants.build{ name = "x", preset_id = "a", game_id = "g-9" }
      fb.variants.list("g-9")
      fb.log.info("shell ok " .. id)
    )LUA", true);
    QTRY_VERIFY(hasLog(con(), "shell ok 1"));
    QCOMPARE(m_prov->requests.first().gameId, QStringLiteral("g-9"));
    QCOMPARE(m_prov->lastListGame, QStringLiteral("g-9"));
  }

  void withoutProviderRegisterFails() {
    PluginSpec s;
    s.permissions = {"variants.build"};
    s.lua = "local ok = pcall(fb.variants.register, {}); local e = fb.variants.engine(); fb.log.info('noprov ok=' .. tostring(ok) .. ' offered=' .. tostring(e.offered))";
    GameContext ctx{"nds", "ABCD", "abc123", "Test Game", "", "en"};
    m_host->start(ctx, nullptr, {infoFor(writePlugin(m_dir.path() + "/src", s), m_dir.path() + "/cache")});
    QTRY_VERIFY(hasLog(con(), "noprov ok=false offered=false"));
  }

  // ---- the gate in the manager
  void unverifiedPluginsCannotUseTheBuildPermission() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    const TestKey key = fixedTestKey();
    m.setTrustedKeys({key.pub});
    FbSpec unsigned_;
    unsigned_.id = "net.test.unsigned";
    unsigned_.permissions = {"variants.build"};
    QVERIFY(m.installFromFile(writeFile(src.filePath("u.fbplugin"), makeFbPlugin(unsigned_))).ok);
    FbSpec official;
    official.id = "net.test.official";
    official.permissions = {"variants.build"};
    official.sign = true;
    QVERIFY(m.installFromFile(writeFile(src.filePath("o.fbplugin"), makeFbPlugin(official))).ok);
    m.setDeveloperMode(false);
    const QString msg = "permission variants.build needs a signed official plugin or Developer mode";
    QVERIFY(m.info("net.test.unsigned")->problems.contains(msg));
    QVERIFY(!m.setEnabled("net.test.unsigned", true));
    m.setDeveloperMode(true);  // an installed unsigned (local) copy may use it in Developer mode
    QVERIFY(m.info("net.test.unsigned")->problems.isEmpty());
    m.setDeveloperMode(false);
    QVERIFY(m.info("net.test.unsigned")->problems.contains(msg));
    QVERIFY(m.info("net.test.official")->problems.isEmpty());
    QVERIFY(m.setEnabled("net.test.official", true));
    // The shell context starts the official one (it registers presets while no game runs), never the unsigned one.
    QStringList started;
    for (const PluginInfo& i : m.resolveForShell().start) started << i.manifest.id;
    QCOMPARE(started, QStringList{"net.test.official"});
  }

  void developerLinkedPluginMayWithDeveloperMode() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    PluginSpec ps;
    ps.id = "net.test.linked";
    ps.permissions = {"variants.build"};
    QVERIFY(m.linkDevFolder(writePlugin(src.path(), ps)).ok);
    QVERIFY(m.info("net.test.linked")->problems.contains("permission variants.build needs a signed official plugin or Developer mode"));
    m.setDeveloperMode(true);
    QVERIFY(m.info("net.test.linked")->problems.isEmpty());
    QVERIFY(m.setEnabled("net.test.linked", true));
    m.setDeveloperMode(false);
    QVERIFY(!m.info("net.test.linked")->problems.isEmpty());
  }
};

QTEST_MAIN(VariantsPluginTest)
#include "variants_test.moc"
