// End to end: the Uzume test core runs with EmulationRunner, a PluginHost with the real memory adapter runs a plugin
// (homebrew Lua in a temp dir) that checks the magic, follows the party pointer, decrypts slot 1 with uz.codec.pk6_decrypt
// and emits the species. Also: the manager resolves plugins for the game, developer mode gates local plugins, and a
// plugin written to a linked folder reloads. Only synthetic data (test core party built with testutil/synth_formats.h).
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "emulation_runner.h"
#include "libretro_backend.h"
#include "plugin_host.h"
#include "plugin_manager.h"
#include "pluginruntime.h"
#include "synth_formats.h"
#include "test_core_layout.h"

using namespace uzume;
using namespace uzume::emu;
using namespace uzume::plugins;
namespace mem = uzume::testcore::mem;

namespace {

const char* kMain = R"LUA(
local HDR = 0x08000000
uz.memory.watch("hdr", HDR, 0x20)
uz.dev.watch("magic", HDR, "u32")
uz.dev.watch("party ptr", HDR + 0x10, "u32")
local followed, emitted = false, false
uz.on_tick(function()
  local magic = uz.memory.u32(HDR)
  if magic ~= 0x4D4D4246 then return end
  local ptr = uz.memory.u32(HDR + 0x10)
  if not ptr or ptr == 0 then return end
  if not followed then
    uz.memory.watch("party", ptr, 4 + 6 * 260)
    followed = true
    return
  end
  local count = uz.memory.u32(ptr)
  local raw = uz.memory.bytes(ptr + 4, 260)
  if not count or not raw then return end
  local plain, ok = uz.codec.pk6_decrypt(raw)
  if not plain then uz.log.error("decrypt failed", ok) return end
  if not emitted then
    emitted = true
    uz.log.info("party", count, "checksum", tostring(ok))
    uz.events.emit("species", string.unpack("<I2", plain, 9))
  end
end)
)LUA";

void writeFile(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(data);
}

QString writePlugin(const QString& parent, const QString& id, const QByteArray& lua, const QJsonArray& games = {}) {
  const QString dir = QDir(parent).filePath(id);
  QJsonObject m{{"id", id}, {"name", id}, {"version", "1.0.0"}, {"api", 1}, {"type", "script"}, {"entry", "main.lua"},
                {"permissions", QJsonArray{"memory.read"}}};
  if (!games.isEmpty()) m["games"] = games;
  writeFile(dir + "/manifest.json", QJsonDocument(m).toJson());
  writeFile(dir + "/main.lua", lua);
  return dir;
}

bool hasEvent(const ConsoleSnapshot& c, const QString& name, const QString& text) {
  for (const EventLine& e : c.events)
    if (e.name == name && e.text == text) return true;
  return false;
}

}  // namespace

class PluginsE2eTest : public QObject {
  Q_OBJECT
  QTemporaryDir m_dir;

  EmulationRunner::StartRequest request() const {
    EmulationRunner::StartRequest req;
    req.corePath = qEnvironmentVariable("UZUME_MELONDS_DS_CORE");
    req.gamePath = QStringLiteral(FB_TEST_ROM_PATH);
    req.systemDir = m_dir.filePath(QStringLiteral("system"));
    req.saveDir = m_dir.filePath(QStringLiteral("save"));
    return req;
  }

 private slots:
  void partySpeciesReachesThePlugin() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QSignalSpy started(&runner, &EmulationRunner::started);
    runner.start(request());
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
    QVERIFY(runner.memoryRegions().size() >= 2);  // 3 after the test core re-sends its map at frame 30

    auto adapter = std::make_shared<ui::MemoryWatchAdapter>(runner.memoryWatch());
    PluginInfo info;
    info.path = writePlugin(m_dir.filePath("src"), "net.test.party", kMain);
    const auto lm = loadManifest(info.path);
    QVERIFY2(lm.ok, qPrintable(lm.error));
    info.manifest = lm.manifest;
    info.cacheDir = m_dir.filePath("cache/net.test.party");
    info.enabled = true;

    PluginHost host;
    host.setTickHz(120);
    GameContext ctx{"3ds", "0004000000000000", "abc", "Test", QStringLiteral(FB_TEST_ROM_PATH), "en"};
    host.start(ctx, adapter, {info});
    const QString id = QStringLiteral("net.test.party");
    QTRY_COMPARE_WITH_TIMEOUT(host.consoleSnapshot(id).status, PluginStatus::Running, 10000);
    // Species 1 (slot 1, kSpeciesBase) arrives as the event value.
    QTRY_VERIFY_WITH_TIMEOUT(hasEvent(host.consoleSnapshot(id), "species", QString::number(mem::kSpeciesBase)), 15000);
    const ConsoleSnapshot c = host.consoleSnapshot(id);
    QCOMPARE(c.status, PluginStatus::Running);
    QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
    bool logged = false;
    for (const LogLine& l : c.log) logged |= l.text.contains("checksum\ttrue");
    QVERIFY(logged);  // the stored checksum of the synthetic party member is valid
    // The WATCHED list has the developer watches with live values.
    QTRY_VERIFY_WITH_TIMEOUT(host.consoleSnapshot(id).watched.size() == 2, 5000);
    bool magic = false;
    for (const WatchedRow& w : host.consoleSnapshot(id).watched)
      if (w.label == "magic") {
        QCOMPARE(w.address, QStringLiteral("0x08000000"));
        QTRY_VERIFY_WITH_TIMEOUT(host.consoleSnapshot(id).watched.value(0).value != "-" || host.consoleSnapshot(id).watched.value(1).value != "-", 5000);
        magic = true;
      }
    QVERIFY(magic);

    host.stop();
    adapter->clear();
    QVERIFY(runner.memoryWatch()->empty());
    runner.stop();
  }

  void adapterRebindsToANewWatchSet() {
    auto a = std::make_shared<MemoryWatchSet>();
    ui::MemoryWatchAdapter ad(a);
    ad.setWatches("p", {{0x1000, 16}});
    QVERIFY(!a->empty());
    std::vector<uint8_t> ram(0x2000, 7);
    a->capture(MemoryMap::fromBuffer(ram.data(), ram.size()), 5);
    uint8_t b = 0;
    QVERIFY(ad.read(0x1000, &b, 1));
    QCOMPARE(b, uint8_t{7});
    QCOMPARE(ad.frame().value_or(0), uint64_t{5});
    QCOMPARE(ad.generation(), uint64_t{1});
    QVERIFY(!ad.read(0x1800, &b, 1));
    auto n = std::make_shared<MemoryWatchSet>();
    ad.bind(n);  // live-save restart: ranges move to the new set
    QVERIFY(a->empty());
    QVERIFY(!n->empty());
    QVERIFY(!ad.frame().has_value());
    QVERIFY(!ad.read(0x1000, &b, 1));
    ad.setWatches("p", {});
    QVERIFY(n->empty());
  }

  void managerGatesLocalPluginsByDeveloperModeAndGame() {
    PluginManager mgr(m_dir.filePath("mgr"));
    const QString src = writePlugin(m_dir.filePath("src2"), "net.test.only3ds", "uz.log.info('x')",
                                    QJsonArray{QJsonObject{{"system", "3ds"}}});
    const auto r = mgr.installFromFolder(src);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(mgr.setEnabled(r.id, true));
    QVERIFY(mgr.resolveForGame("3ds", "0004000000000000").start.isEmpty());  // Developer mode off
    QVERIFY(mgr.resolveForGame("3ds", "0004000000000000").problems.contains(r.id));
    mgr.setDeveloperMode(true);
    QCOMPARE(mgr.resolveForGame("3ds", "0004000000000000").start.size(), 1);
    QVERIFY(mgr.resolveForGame("nds", "ABCD").start.isEmpty());  // other system
  }

  void linkedPluginReloadsOnChange() {
    PluginManager mgr(m_dir.filePath("mgr2"));
    mgr.setDeveloperMode(true);
    const QString src = writePlugin(m_dir.filePath("src3"), "net.test.linked", "uz.log.info('v1')");
    const auto r = mgr.linkDevFolder(src);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(mgr.setEnabled(r.id, true));
    const auto res = mgr.resolveForGame("nds", "ABCD");
    QCOMPARE(res.start.size(), 1);
    auto mem = std::make_shared<ui::MemoryWatchAdapter>();
    PluginHost host;
    GameContext ctx{"nds", "ABCD", "", "T", "", "en"};
    host.start(ctx, mem, res.start);
    QTRY_COMPARE_WITH_TIMEOUT(host.consoleSnapshot(r.id).status, PluginStatus::Running, 10000);
    QSignalSpy changed(&mgr, &PluginManager::pluginChanged);
    QObject::connect(&mgr, &PluginManager::pluginChanged, &host, [&host](const QString& id) { host.reload(id); });
    writeFile(src + "/main.lua", "uz.log.info('v2')");
    QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
          for (const LogLine& l : host.consoleSnapshot(r.id).log)
            if (l.text == "v2") return true;
          return false;
        }(),
        10000);
  }
};

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  PluginsE2eTest t;
  return QTest::qExec(&t, argc, argv);
}
#include "plugins_e2e_test.moc"
