// Plugin API of 0.20 package 5 against fake bridges: uz.memory.write*, uz.cheats.*, uz.save.snapshot, uz.store.*, the
// textarea block and uz.ui.pick_file (permissions, results, limits). The app-side policy (consent, snapshots, run rules)
// is tested in ui/tests/plugin_emulation_test.cpp.
#include <QTemporaryDir>
#include <QtTest>

#include "plugin_host.h"
#include "plugin_pick.h"
#include "plugin_store.h"
#include "plugin_test_util.h"
#include "plugin_ui.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

namespace {

class FakeEmulation : public EmulationBridge {
 public:
  struct Write {
    QString plugin;
    quint64 addr;
    QByteArray bytes;
  };
  QString writeMemory(const QString& pluginId, quint64 addr, const QByteArray& bytes) override {
    std::lock_guard<std::mutex> l(m);
    writes.append({pluginId, addr, bytes});
    return writeError;
  }
  QStringList cheatFormats() const override { return formats; }
  QString coreName() const override { return QStringLiteral("mGBA"); }
  QString cheatsBlockedReason() const override { return blockedReason; }
  CheatApplyResult applyCheats(const QString&, const QList<CheatItem>& list) override {
    std::lock_guard<std::mutex> l(m);
    applied = list;
    CheatApplyResult r;
    r.error = applyError;
    for (const CheatItem& c : list) {
      if (!formats.contains(c.format)) r.rejected.append({c.id, QStringLiteral("unsupported_format")});
      else if (c.enabled) ++r.applied;
    }
    return r;
  }
  QString clearCheats(const QString&) override {
    std::lock_guard<std::mutex> l(m);
    ++clears;
    return QString();
  }
  CheatDatabaseResult cheatDatabase() override {
    CheatDatabaseResult r;
    r.error = dbError;
    r.data = QJsonObject{{"source", "libretro-database"},
                         {"updated_at", "2026-10-21T00:00:00Z"},
                         {"cheats", QJsonArray{QJsonObject{{"name", "Infinite money"}, {"code", "0212A4C8 000F423F"}, {"format_guess", "gsa"}}}}};
    return r;
  }
  QString snapshot(const QString& pluginId, const QString& pluginName, const QString& reason) override {
    std::lock_guard<std::mutex> l(m);
    snapshots.append(pluginId + "|" + pluginName + "|" + reason);
    return snapshotError;
  }

  mutable std::mutex m;
  QList<Write> writes;
  QStringList formats{"gsa", "raw"};
  QList<CheatItem> applied;
  int clears = 0;
  QStringList snapshots;
  QString blockedReason;
  QString writeError, applyError, dbError, snapshotError;
};

bool hasLog(const ConsoleSnapshot& c, const QString& needle) {
  for (const LogLine& l : c.log)
    if (l.text.contains(needle)) return true;
  return false;
}

}  // namespace

class EmulationApiTest : public QObject {
  Q_OBJECT

  QTemporaryDir m_dir;
  std::shared_ptr<FakeEmulation> m_emu;
  std::shared_ptr<PluginStore> m_store;
  PluginHost* m_host = nullptr;
  GameContext m_ctx{"gba", "ABCD", "abc123", "Test Game", "", "en"};

  void startWith(const QStringList& perms, const QString& lua, const QJsonObject& ui = {}) {
    PluginSpec s;
    s.permissions = perms;
    s.lua = lua;
    s.name = "Test Plugin";
    s.ui = ui;
    if (perms.contains("ui") && ui.isEmpty()) s.ui = QJsonObject{{"panel", QJsonObject{{"title", "T"}}}};
    m_host->start(m_ctx, nullptr, {infoFor(writePlugin(m_dir.path() + "/src", s), m_dir.path() + "/cache")});
    QTRY_COMPARE_WITH_TIMEOUT(m_host->consoleSnapshot("net.test.plugin").status, PluginStatus::Running, 10000);
  }
  // QTRY_* evaluates its condition a second time at the end, which would consume a take-style call: poll by hand.
  QList<PickRequest> waitPick() {
    QList<PickRequest> r;
    for (int i = 0; i < 200 && r.isEmpty(); ++i) {
      r = m_host->takePickRequests();
      if (r.isEmpty()) QTest::qWait(25);
    }
    return r;
  }
  QList<QPair<QString, QString>> waitClose() {
    QList<QPair<QString, QString>> r;
    for (int i = 0; i < 200 && r.isEmpty(); ++i) {
      r = m_host->takeCloseRequests();
      if (r.isEmpty()) QTest::qWait(25);
    }
    return r;
  }
  ConsoleSnapshot con() const { return m_host->consoleSnapshot("net.test.plugin"); }

 private slots:
  void init() {
    m_emu = std::make_shared<FakeEmulation>();
    m_store = std::make_shared<PluginStore>(m_dir.path() + "/store");
    m_ctx.emulation = m_emu;
    m_ctx.store = m_store;
    m_host = new PluginHost;
    m_host->setTickHz(200);
  }
  void cleanup() {
    delete m_host;
    m_host = nullptr;
    QDir(m_dir.path() + "/src").removeRecursively();
    QDir(m_dir.path() + "/store").removeRecursively();
  }

  void permissionsAreRequired() {
    startWith({}, R"LUA(
      local function denied(f, perm, ...)
        local ok, e = pcall(f, ...)
        assert(not ok and e:find("permission " .. perm .. " not granted", 1, true), tostring(e))
      end
      denied(uz.memory.write, "memory.write", 0, "x")
      denied(uz.memory.write_u8, "memory.write", 0, 1)
      denied(uz.memory.write_u16, "memory.write", 0, 1)
      denied(uz.memory.write_u32, "memory.write", 0, 1)
      denied(uz.cheats.formats, "cheats")
      denied(uz.cheats.apply, "cheats", {})
      denied(uz.cheats.clear, "cheats")
      denied(uz.cheats.allowed, "cheats")
      denied(uz.ui.close, "ui", "panel")
      denied(uz.cheats.database, "cheats")
      denied(uz.store.get, "store", "k")
      denied(uz.store.put, "store", "k", 1)
      denied(uz.store.conflicts, "store", "k")
      denied(uz.ui.pick_file, "ui", {}, function() end)
      uz.log.info("denied ok")
    )LUA");
    QVERIFY2(hasLog(con(), "denied ok"), qPrintable(con().error));
    QVERIFY(m_emu->writes.isEmpty());
  }

  void memoryWrite() {
    startWith({"memory.write"}, R"LUA(
      assert(uz.memory.write(0x02000000, "\1\2\3") == true)
      assert(uz.memory.write_u8(0x10, 255) == true)
      assert(uz.memory.write_u16(0x20, 0x1234) == true)
      assert(uz.memory.write_u32(0x30, 0xDEADBEEF) == true)
      assert(uz.memory.write_u8(0x40, -1) == true)
      assert(not pcall(uz.memory.write_u8, 0, 256))
      assert(not pcall(uz.memory.write_u16, 0, 65536))
      assert(not pcall(uz.memory.write_u32, 0, 1 << 32))
      assert(not pcall(uz.memory.write, 0, ""))
      assert(not pcall(uz.memory.write, 0, string.rep("x", 65537)))
      assert(not pcall(uz.memory.write, 0, {}))
      uz.log.info("writes ok")
    )LUA");
    QVERIFY2(hasLog(con(), "writes ok"), qPrintable(con().error));
    QCOMPARE(m_emu->writes.size(), 5);
    QCOMPARE(m_emu->writes[0].addr, quint64(0x02000000));
    QCOMPARE(m_emu->writes[0].bytes, QByteArray("\1\2\3"));
    QCOMPARE(m_emu->writes[0].plugin, QString("net.test.plugin"));
    QCOMPARE(m_emu->writes[1].bytes, QByteArray("\xFF"));
    QCOMPARE(m_emu->writes[2].bytes, QByteArray("\x34\x12"));  // little endian
    QCOMPARE(m_emu->writes[3].bytes, QByteArray("\xEF\xBE\xAD\xDE"));
    QCOMPARE(m_emu->writes[4].bytes, QByteArray("\xFF"));  // -1 fits a byte
  }

  void memoryWriteRefusalsComeBackAsNilAndMessage() {
    m_emu->writeError = "consent_required";
    startWith({"memory.write"}, R"LUA(
      local ok, msg = uz.memory.write_u8(0, 1)
      assert(ok == nil and msg == "consent_required", tostring(msg))
      uz.log.info("refused ok")
    )LUA");
    QVERIFY2(hasLog(con(), "refused ok"), qPrintable(con().error));
  }

  void withoutAGameBridgeEverythingIsNoGame() {
    m_ctx.emulation.reset();
    startWith({"memory.write", "cheats"}, R"LUA(
      local ok, msg = uz.memory.write_u8(0, 1)
      assert(ok == nil and msg == "no_game")
      assert(#uz.cheats.formats() == 0)
      local a, m2 = uz.cheats.apply({})
      assert(a == nil and m2 == "no_game")
      local s, m3 = uz.save.snapshot("x")
      assert(s == nil and m3 == "no_game")
      uz.log.info("nogame ok")
    )LUA");
    QVERIFY2(hasLog(con(), "nogame ok"), qPrintable(con().error));
  }

  void cheats() {
    startWith({"cheats"}, R"LUA(
      local f, core = uz.cheats.formats()
      assert(#f == 2 and f[1] == "gsa" and f[2] == "raw" and core == "mGBA")
      local al, why = uz.cheats.allowed()
      assert(al == true and why == nil)
      local r = uz.cheats.apply({
        {id = "a", name = "Money", format = "gsa", code = "0212A4C8 000F423F", enabled = true},
        {id = "b", name = "Off", format = "gsa", code = "00000000 00000000", enabled = false},
        {id = "c", name = "Foreign", format = "cb", code = "1 2"},
      })
      assert(r.applied == 1, tostring(r.applied))
      assert(#r.rejected == 1 and r.rejected[1].id == "c" and r.rejected[1].reason == "unsupported_format")
      assert(uz.cheats.clear() == true)
      assert(not pcall(uz.cheats.apply, {{id = "a"}}))        -- name/format/code missing
      assert(not pcall(uz.cheats.apply, {5}))
      assert(not pcall(uz.cheats.apply, "x"))
      local db = uz.cheats.database()
      assert(db.source == "libretro-database" and db.cheats[1].name == "Infinite money" and db.cheats[1].format_guess == "gsa")
      assert(uz.game.hash() == "abc123")
      uz.log.info("cheats ok")
    )LUA");
    QVERIFY2(hasLog(con(), "cheats ok"), qPrintable(con().error));
    QCOMPARE(m_emu->applied.size(), 3);
    QVERIFY(m_emu->applied[0].enabled);
    QVERIFY(!m_emu->applied[1].enabled);
    QVERIFY(m_emu->applied[2].enabled);  // enabled defaults to true
    QCOMPARE(m_emu->clears, 1);
  }

  void cheatsAllowedFollowsTheRunRule() {
    m_emu->blockedReason = "run_rule";
    startWith({"cheats"}, R"LUA(
      local ok, why = uz.cheats.allowed()
      assert(ok == false and why == "run_rule")
      uz.log.info("allowed ok")
    )LUA");
    QVERIFY2(hasLog(con(), "allowed ok"), qPrintable(con().error));
  }

  void uiCloseQueuesARequest() {
    startWith({"ui"}, R"LUA(
      assert(not pcall(uz.ui.close, "hud"))
      uz.ui.close("panel")
    )LUA");
    QList<QPair<QString, QString>> reqs;
    reqs = waitClose();
    QCOMPARE(reqs[0].second, QString("panel"));
  }

  void cheatsRefusalsAndDatabaseErrors() {
    m_emu->applyError = "blocked_by_run_rule";
    m_emu->dbError = "off";
    startWith({"cheats"}, R"LUA(
      local a, m = uz.cheats.apply({})
      assert(a == nil and m == "blocked_by_run_rule")
      local d, m2 = uz.cheats.database()
      assert(d == nil and m2 == "off")
      uz.log.info("errors ok")
    )LUA");
    QVERIFY2(hasLog(con(), "errors ok"), qPrintable(con().error));
  }

  void snapshotChecksAndRateLimit() {
    startWith({}, R"LUA(
      local ok = uz.save.snapshot("before Gym 3")
      assert(ok == true)
      local a, m = uz.save.snapshot("again")
      assert(a == nil and m == "rate_limited", tostring(m))
      assert(not pcall(uz.save.snapshot))
      uz.log.info("snapshot ok")
    )LUA");
    QVERIFY2(hasLog(con(), "snapshot ok"), qPrintable(con().error));
    QCOMPARE(m_emu->snapshots, QStringList{"net.test.plugin|Test Plugin|before Gym 3"});
  }

  void snapshotReasonLimits() {
    startWith({}, R"LUA(
      local a, m = uz.save.snapshot("")
      assert(a == nil and m:find("empty"))
      local b, m2 = uz.save.snapshot(string.rep("x", 81))
      assert(b == nil and m2:find("80"))
      assert(uz.save.snapshot(string.rep("x", 80)) == true)
      uz.log.info("limits ok")
    )LUA");
    QVERIFY2(hasLog(con(), "limits ok"), qPrintable(con().error));
    QCOMPARE(m_emu->snapshots.size(), 1);
  }

  void snapshotRefusedByTheAppDoesNotUseTheMinute() {
    m_emu->snapshotError = "unsupported";
    startWith({}, R"LUA(
      local a, m = uz.save.snapshot("one")
      assert(a == nil and m == "unsupported")
      local b, m2 = uz.save.snapshot("two")
      assert(b == nil and m2 == "unsupported")
      uz.log.info("retry ok")
    )LUA");
    QVERIFY2(hasLog(con(), "retry ok"), qPrintable(con().error));
    QCOMPARE(m_emu->snapshots.size(), 2);
  }

  void storeGetPutAndRevisions() {
    startWith({"store"}, R"LUA(
      local v, rev = uz.store.get("lists")
      assert(v == nil and rev == 0)
      local ok, r1 = uz.store.put("lists", {a = 1, names = {"x", "y"}}, 0)
      assert(ok == true and r1 == 1)
      local v2, rev2 = uz.store.get("lists")
      assert(rev2 == 1 and v2.a == 1 and v2.names[2] == "y")
      -- a stale revision is a conflict and does not change the value
      local c, msg = uz.store.put("lists", {a = 2}, 0)
      assert(c == nil and msg == "conflict", tostring(msg))
      local v3, rev3 = uz.store.get("lists")
      assert(rev3 == 1 and v3.a == 1)
      local cf = uz.store.conflicts("lists")
      assert(#cf == 1 and cf[1].source == "local" and cf[1].value.a == 2 and cf[1].revision == 0)
      -- a put after the conflict resolves it
      assert(uz.store.put("lists", {a = 3}, 1) == true)
      assert(#uz.store.conflicts("lists") == 0)
      -- scalars and strings
      assert(uz.store.put("n", 42) and uz.store.put("s", "text") and uz.store.put("b", false))
      assert(uz.store.get("n") == 42 and uz.store.get("s") == "text" and uz.store.get("b") == false)
      -- invalid keys and values raise, too large values do not
      assert(not pcall(uz.store.get, "Bad Key"))
      assert(not pcall(uz.store.put, "UPPER", 1))
      assert(not pcall(uz.store.put, string.rep("a", 65), 1))
      assert(not pcall(uz.store.put, "f", function() end))
      assert(not pcall(uz.store.put, "nilv", nil))
      local big, bmsg = uz.store.put("big", string.rep("x", 256 * 1024 + 1))
      assert(big == nil and bmsg == "too_large")
      assert(uz.store.put("ok", string.rep("x", 256 * 1024 - 100)))
      uz.log.info("store ok")
    )LUA");
    QVERIFY2(hasLog(con(), "store ok"), qPrintable(con().error));
    QVERIFY(m_store->get("net.test.plugin", "lists").found);
    QVERIFY(m_store->isDirty("net.test.plugin", "lists"));
  }

  void storeWithoutAStoreIsUnavailable() {
    m_ctx.store.reset();
    startWith({"store"}, R"LUA(
      local v, m = uz.store.get("k")
      assert(v == nil and m == "unavailable")
      local p, m2 = uz.store.put("k", 1)
      assert(p == nil and m2 == "unavailable")
      assert(#uz.store.conflicts("k") == 0)
      uz.log.info("unavailable ok")
    )LUA");
    QVERIFY2(hasLog(con(), "unavailable ok"), qPrintable(con().error));
  }

  void storeIsPerPlugin() {
    QVERIFY(m_store->put("net.a.one", "k", 1).kind == PluginStore::PutResult::Kind::Ok);
    QVERIFY(!m_store->get("net.a.two", "k").found);
    startWith({"store"}, R"LUA(
      local v = uz.store.get("k")
      assert(v == nil)
      uz.log.info("isolated ok")
    )LUA");
    QVERIFY2(hasLog(con(), "isolated ok"), qPrintable(con().error));
  }

  void textareaBlockValidation() {
    auto convert = [](const QString& lua) {
      Q_UNUSED(lua);
      return 0;
    };
    Q_UNUSED(convert);
    startWith({"ui"}, R"LUA(
      local ok = pcall(uz.ui.set, "panel", {type = "textarea", id = "code", value = "line1\nline2", lines = 8, mono = true, action = "code_changed"})
      assert(ok)
      assert(pcall(uz.ui.set, "panel", {type = "textarea", id = "t"}))
      local function bad(tbl)
        local ok2, e = pcall(uz.ui.set, "panel", tbl)
        assert(not ok2, "expected an error")
        return e
      end
      assert(bad({type = "textarea", lines = 0}):find("between 1 and 40"))
      assert(bad({type = "textarea", lines = 41}):find("between 1 and 40"))
      assert(bad({type = "textarea", mono = "yes"}):find("true or false"))
      assert(bad({type = "textarea", value = 5}):find("expected a string"))
      assert(bad({type = "textarea", value = string.rep("x", 65537)}):find("longer than"))
      assert(bad({type = "textarea", rows = 3}):find("unknown property"))
      assert(pcall(uz.ui.set, "panel", {type = "textarea", id = "big", value = string.rep("x", 60000)}))
      uz.log.info("textarea ok")
    )LUA", QJsonObject{{"panel", QJsonObject{{"title", "T"}}}});
    QVERIFY2(hasLog(con(), "textarea ok"), qPrintable(con().error));
  }

  void pickFileRoundTripAndArguments() {
    startWith({"ui"}, R"LUA(
      assert(not pcall(uz.ui.pick_file, {title = "x"}))              -- callback missing
      assert(not pcall(uz.ui.pick_file, {filters = {"cht"}}, function() end))
      assert(not pcall(uz.ui.pick_file, {filters = {"*.c/h"}}, function() end))
      assert(not pcall(uz.ui.pick_file, {max_bytes = 0}, function() end))
      assert(not pcall(uz.ui.pick_file, {max_bytes = 9 * 1024 * 1024}, function() end))
      local ok = uz.ui.pick_file({title = "Import cheats", filters = {"*.cht", "*.txt"}, max_bytes = 4096}, function(content, name)
        uz.log.info("picked " .. tostring(content) .. " " .. tostring(name))
      end)
      assert(ok == true)
      local busy, why = uz.ui.pick_file({}, function() end)
      assert(busy == nil and why == "busy")
      uz.log.info("requested")
    )LUA");
    QVERIFY2(hasLog(con(), "requested"), qPrintable(con().error));
    QVERIFY(!waitPick().isEmpty());
  }

  void pickFileRequestAndDelivery() {
    startWith({"ui"}, R"LUA(
      uz.ui.pick_file({title = "Import cheats", filters = {"*.cht", "*.txt"}, max_bytes = 4096}, function(content, name)
        uz.log.info("picked " .. tostring(content) .. " " .. tostring(name.name) .. " " .. tostring(name.size))
      end)
    )LUA");
    QList<PickRequest> reqs;
    reqs = waitPick();
    QCOMPARE(reqs.size(), 1);
    QCOMPARE(reqs[0].pluginId, QString("net.test.plugin"));
    QCOMPARE(reqs[0].title, QString("Import cheats"));
    QCOMPARE(reqs[0].filters, (QStringList{"*.cht", "*.txt"}));
    QCOMPARE(reqs[0].maxBytes, qint64(4096));
    m_host->deliverFile("net.test.plugin", reqs[0].id, "CODE 1\nCODE 2", "LUMEN.CHT");
    QTRY_VERIFY_WITH_TIMEOUT(hasLog(con(), "picked CODE 1\nCODE 2 LUMEN.CHT 13"), 5000);
    // a second delivery for the same request is ignored (the callback is called once)
    m_host->deliverFile("net.test.plugin", reqs[0].id, "again", "x");
    QTest::qWait(100);
    QVERIFY(!hasLog(con(), "picked again"));
  }

  void pickFileErrorsReachTheCallback() {
    startWith({"ui"}, R"LUA(
      uz.ui.pick_file({}, function(content, err)
        uz.log.info("result " .. tostring(content) .. " " .. tostring(err))
      end)
    )LUA");
    QList<PickRequest> reqs;
    reqs = waitPick();
    QCOMPARE(reqs[0].maxBytes, qint64(1024 * 1024));  // default 1 MiB
    m_host->deliverFile("net.test.plugin", reqs[0].id, QByteArray(), QString(), "cancelled");
    QTRY_VERIFY_WITH_TIMEOUT(hasLog(con(), "result nil nil"), 5000);  // cancel = callback(nil)
    m_host->takePickRequests();
    m_host->sendAction("net.test.plugin", "panel", "again", QVariant());
    QVERIFY(true);
  }

  void pickFileErrorMessage() {
    startWith({"ui"}, R"LUA(
      uz.ui.pick_file({max_bytes = 10}, function(content, err)
        uz.log.info("err " .. tostring(content) .. " " .. tostring(err))
      end)
    )LUA");
    QList<PickRequest> reqs;
    reqs = waitPick();
    m_host->deliverFile("net.test.plugin", reqs[0].id, QByteArray(), QString(), "too_large");
    QTRY_VERIFY_WITH_TIMEOUT(hasLog(con(), "err nil too_large"), 5000);
  }

  void readPickedFileLimits() {
    QTemporaryDir d;
    QFile f(d.filePath("a.cht"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(100, 'x'));
    f.close();
    PickedFile ok = readPickedFile(f.fileName(), 100);
    QVERIFY(ok.error.isEmpty());
    QCOMPARE(ok.content.size(), 100);
    QCOMPARE(ok.name, QString("a.cht"));  // base name, never the path
    PickedFile big = readPickedFile(f.fileName(), 99);
    QCOMPARE(big.error, QString("too_large"));
    QVERIFY(big.content.isEmpty());
    QCOMPARE(readPickedFile(d.filePath("missing"), 100).error, QString("read_failed"));
    QCOMPARE(readPickedFile(d.path(), 100).error, QString("read_failed"));  // a folder
  }

  void appEventsReachSubscribers() {
    startWith({"cheats"}, R"LUA(
      uz.events.on("app", "cheats.blocked", function(v, from, name)
        uz.log.info("blocked " .. v.reason .. " " .. from .. " " .. name)
      end)
    )LUA");
    m_host->postAppEvent("cheats.blocked", QVariantMap{{"reason", "run_rule"}});
    QTRY_VERIFY_WITH_TIMEOUT(hasLog(con(), "blocked run_rule app cheats.blocked"), 5000);
  }
};

// The local copy behind uz.store, without Lua: persistence, conflicts and the bookkeeping the server sync uses.
class PluginStoreTest : public QObject {
  Q_OBJECT
 private slots:
  void persistsAcrossInstances() {
    QTemporaryDir d;
    {
      PluginStore s(d.path());
      QVERIFY(s.put("net.a.b", "k", QJsonObject{{"x", 1}}).kind == PluginStore::PutResult::Kind::Ok);
    }
    PluginStore s2(d.path());
    const auto e = s2.get("net.a.b", "k");
    QVERIFY(e.found);
    QCOMPARE(e.revision, qint64(1));
    QCOMPARE(e.value.toObject().value("x").toInt(), 1);
    QVERIFY(s2.isDirty("net.a.b", "k"));
    QCOMPARE(s2.dirtyKeys().size(), 1);
  }
  void serverConflictKeepsBothVersions() {
    PluginStore s(QString{});
    s.put("net.a.b", "k", QString("mine"));
    s.markServerConflict("net.a.b", "k", 7, QString("theirs"));
    QCOMPARE(s.get("net.a.b", "k").value.toString(), QString("mine"));
    const auto c = s.conflicts("net.a.b", "k");
    QCOMPARE(c.size(), 1);
    QCOMPARE(c[0].source, QString("server"));
    QCOMPARE(c[0].value.toString(), QString("theirs"));
    QCOMPARE(c[0].revision, qint64(7));
    QVERIFY(s.dirtyKeys().isEmpty());  // held back while there are conflicts
    QCOMPARE(s.serverRevision("net.a.b", "k"), qint64(7));
    // resolving = a new put: pushed against the server's revision
    s.put("net.a.b", "k", QString("merged"));
    QVERIFY(s.conflicts("net.a.b", "k").isEmpty());
    const auto dirty = s.dirtyKeys();
    QCOMPARE(dirty.size(), 1);
    QCOMPARE(dirty[0].serverRev, qint64(7));
  }
  void equalServerValueIsNoConflict() {
    PluginStore s(QString{});
    s.put("net.a.b", "k", 5);
    s.markServerConflict("net.a.b", "k", 3, 5);
    QVERIFY(s.conflicts("net.a.b", "k").isEmpty());
    QVERIFY(!s.isDirty("net.a.b", "k"));
  }
  void adoptAndPushed() {
    PluginStore s(QString{});
    s.adoptServer("net.a.b", "new", 4, QString("remote"));
    QCOMPARE(s.get("net.a.b", "new").value.toString(), QString("remote"));
    QVERIFY(!s.isDirty("net.a.b", "new"));
    s.put("net.a.b", "k", 1);
    s.adoptServer("net.a.b", "k", 9, 2);  // dirty: not replaced
    QCOMPARE(s.get("net.a.b", "k").value.toInt(), 1);
    const auto d = s.dirtyKeys();
    QCOMPARE(d.size(), 1);
    s.put("net.a.b", "k", 3);  // written again while the push was in flight
    s.markPushed("net.a.b", "k", d[0].localRev, 5);
    QVERIFY(s.isDirty("net.a.b", "k"));  // newer local write stays dirty
    QCOMPARE(s.serverRevision("net.a.b", "k"), qint64(5));
  }
  void limits() {
    PluginStore s(QString{});
    using K = PluginStore::PutResult::Kind;
    QVERIFY(s.put("bad", "k", 1).kind == K::Invalid);
    QVERIFY(s.put("net.a.b", "Bad", 1).kind == K::Invalid);
    QVERIFY(s.put("net.a.b", "big", QString(kStoreMaxValueBytes + 1, 'x')).kind == K::TooLarge);
    for (int i = 0; i < kStoreMaxKeys; ++i) QVERIFY(s.put("net.a.b", QString("k%1").arg(i), 1).kind == K::Ok);
    QVERIFY(s.put("net.a.b", "one-too-many", 1).kind == K::Quota);
    QVERIFY(s.put("net.a.b", "k1", 2).kind == K::Ok);  // existing keys can still change
  }
  void changedCallback() {
    PluginStore s(QString{});
    QStringList seen;
    s.setChangedCallback([&](const QString& id) { seen << id; });
    s.put("net.a.b", "k", 1);
    QCOMPARE(seen, QStringList{"net.a.b"});
  }
};

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  int rc = 0;
  EmulationApiTest a;
  rc |= QTest::qExec(&a, argc, argv);
  PluginStoreTest b;
  rc |= QTest::qExec(&b, argc, argv);
  return rc;
}
#include "emulation_test.moc"
