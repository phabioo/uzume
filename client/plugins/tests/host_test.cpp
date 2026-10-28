#include <QTemporaryDir>
#include <QtTest>

#include "plugin_host.h"
#include "processguard.h"
#include "plugin_test_util.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

namespace {
QStringList logTexts(const ConsoleSnapshot& c) {
  QStringList out;
  for (const LogLine& l : c.log) out << l.text;
  return out;
}
bool hasLog(const ConsoleSnapshot& c, const QString& needle) {
  for (const LogLine& l : c.log)
    if (l.text.contains(needle)) return true;
  return false;
}
const QStringList kMem{"memory.read"};
}  // namespace

class HostTest : public QObject {
  Q_OBJECT

  QTemporaryDir m_dir;
  std::shared_ptr<FakeMemoryProvider> m_mem;
  PluginHost* m_host = nullptr;
  GameContext m_ctx{"nds", "ABCD", "abc123", "Test Game", "", "en"};

  QString cache() const { return m_dir.path() + "/cache"; }
  PluginInfo make(const PluginSpec& s) { return infoFor(writePlugin(m_dir.path() + "/src", s), cache()); }
  void startOne(const PluginSpec& s) { start({s}); }
  void start(const QList<PluginSpec>& specs) {
    QList<PluginInfo> infos;
    for (const PluginSpec& s : specs) infos << make(s);
    m_host->start(m_ctx, m_mem, infos);
  }
  void waitRunning(const QString& id) { QTRY_COMPARE_WITH_TIMEOUT(m_host->consoleSnapshot(id).status, PluginStatus::Running, 10000); }
  void snapshots(int n) {
    for (int i = 0; i < n; ++i) {
      m_mem->snapshot();
      QTest::qWait(25);
    }
  }
  ConsoleSnapshot con(const QString& id = "net.test.plugin") const { return m_host->consoleSnapshot(id); }

 private slots:
  void init() {
    m_mem = std::make_shared<FakeMemoryProvider>();
    m_host = new PluginHost;
    m_host->setTickHz(200);
  }
  void cleanup() {
    delete m_host;
    m_host = nullptr;
    QDir(m_dir.path() + "/src").removeRecursively();
    QDir(cache()).removeRecursively();
  }

  void apiTables() {
    PluginSpec s;
    s.lua = R"LUA(
      assert(uz.api == 1)
      assert(uz.plugin.id == "net.test.plugin" and uz.plugin.version == "1.0.0")
      assert(uz.game.system == "nds" and uz.game.code == "ABCD" and uz.game.sha256 == "abc123" and uz.game.title == "Test Game")
      assert(uz.game.language == "en")
      assert(math.type(uz.clock()) == "integer")
      local a = uz.clock() local b = uz.clock() assert(b >= a)
      uz.log.info("ok")
    )LUA";
    startOne(s);
    waitRunning("net.test.plugin");
    QVERIFY(hasLog(con(), "ok"));
  }

  void fbIsAliasOfUz() {
    // Uzume 0.22 renamed the namespace to `uz`; `fb` stays as the same table for plugins written for older apps.
    PluginSpec s;
    s.lua = R"LUA(
      assert(uz == fb)
      assert(fb.api == 1 and fb.plugin.id == "net.test.plugin")
      assert(fb.game.system == "nds" and fb.game.code == "ABCD")
      fb.log.info("alias ok")
    )LUA";
    startOne(s);
    waitRunning("net.test.plugin");
    QVERIFY(hasLog(con(), "alias ok"));
  }

  void hostEventSourceUzAndFbAlias() {
    // Host events come from source "uz"; subscribing with the old name "fb" still works and delivers exactly once.
    PluginSpec a, b;
    a.id = "net.test.a";
    b.id = "net.test.b";
    a.lua = R"LUA(
      uz.events.on("fb", "ping", function(v, from, name) uz.log.info("A got " .. from .. "/" .. name) end)
    )LUA";
    b.lua = R"LUA(
      uz.events.on("uz", "ping", function(v, from, name) uz.log.info("B got " .. from .. "/" .. name) end)
    )LUA";
    start({a, b});
    waitRunning("net.test.a");
    waitRunning("net.test.b");
    m_host->postHostEvent("ping", QVariant());
    QTRY_VERIFY(hasLog(con("net.test.a"), "A got uz/ping"));
    QTRY_VERIFY(hasLog(con("net.test.b"), "B got uz/ping"));
    QTest::qWait(150);
    for (const QString& id : {QString("net.test.a"), QString("net.test.b")}) {
      int n = 0;
      for (const LogLine& l : con(id).log)
        if (l.text.contains(" got uz/ping")) ++n;
      QCOMPARE(n, 1);
    }
  }

  void memoryApiAndPointerFollowing() {
    // pointer at 0x1000 -> 0x2000 -> payload
    m_mem->setU32(0x1000, 0x2000);
    m_mem->setRam(0x1004, {0xFE, 0xFF, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    m_mem->setU32(0x2000, 0xCAFEBABE);
    PluginSpec s;
    s.permissions = kMem;
    s.lua = R"LUA(
      assert(uz.memory.u8(0x1000) == nil)               -- nothing in a snapshot yet
      assert(uz.memory.frame() == nil)
      uz.memory.watch("head", 0x1000, 16)
      local followed = false
      uz.on_tick(function(frame)
        local ptr = uz.memory.u32(0x1000)
        if ptr == nil then return end
        uz.log.info(string.format("ptr=%x s16=%d s8=%d u16=%d u64=%d s32=%d frame=%s", ptr, uz.memory.s16(0x1004), uz.memory.s8(0x1005),
          uz.memory.u16(0x1004), uz.memory.u64(0x1004), uz.memory.s32(0x1006), tostring(uz.memory.frame() == frame)))
        local target = uz.memory.u32(ptr)
        if not followed then
          followed = true
          uz.log.info("target " .. tostring(target))   -- not yet in the snapshot
          uz.memory.watch("target", ptr, 4)
        else
          uz.log.info(string.format("target %s bytes=%d", tostring(target), #uz.memory.bytes(ptr, 4)))
          assert(uz.memory.u8(0x9999) == nil and uz.memory.bytes(0x9999, 4) == nil)
        end
      end)
    )LUA";
    startOne(s);
    waitRunning("net.test.plugin");
    QTRY_VERIFY(!m_mem->watchesOf("net.test.plugin").empty());
    QCOMPARE(m_mem->watchesOf("net.test.plugin").size(), size_t(1));
    QCOMPARE(m_mem->watchesOf("net.test.plugin")[0].addr, uint64_t(0x1000));
    snapshots(3);
    QTRY_VERIFY_WITH_TIMEOUT(hasLog(con(), "target 3405691582 bytes=4"), 5000);
    const QStringList logs = logTexts(con());
    QVERIFY2(logs.contains("target nil"), qPrintable(logs.join("\n")));
    QVERIFY2(hasLog(con(), "ptr=2000 s16=-2 s8=-1 u16=65534 u64=-8323074 s32=-128 frame=true"), qPrintable(logs.join("\n")));
    QCOMPARE(m_mem->watchesOf("net.test.plugin").size(), size_t(2));
    QCOMPARE(con().status, PluginStatus::Running);
    // stop unwatches everything
    m_host->stop();
    QVERIFY(m_mem->watchesOf("net.test.plugin").empty());
  }

  void watchLimits() {
    PluginSpec s;
    s.permissions = kMem;
    s.lua = R"LUA(
      local ok, e = pcall(uz.memory.watch, "big", 0, 4 * 1024 * 1024 + 1)
      assert(not ok and e:find("too large"), e)
      for i = 1, 4 do uz.memory.watch("r" .. i, i * 0x1000000, 4 * 1024 * 1024) end
      local ok2, e2 = pcall(uz.memory.watch, "over", 0x10000000, 1)
      assert(not ok2 and e2:find("16 MiB"), e2)
      uz.memory.unwatch("r4")
      uz.memory.watch("fits", 0x10000000, 1024)
      uz.log.info("limits ok")
    )LUA";
    startOne(s);
    waitRunning("net.test.plugin");
    QVERIFY(hasLog(con(), "limits ok"));
    QCOMPARE(m_mem->watchesOf("net.test.plugin").size(), size_t(4));
  }

  void tickRateLimited() {
    m_host->setTickHz(10);
    PluginSpec s;
    s.lua = "uz.on_tick(function() uz.log.info('tick') end)";
    startOne(s);
    waitRunning("net.test.plugin");
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 1000) {
      m_mem->snapshot();
      QTest::qWait(2);
    }
    int ticks = 0;
    for (const QString& l : logTexts(con())) ticks += l == "tick";
    // The upper bound is the point of the test (rate limit); the lower bound scales with the real elapsed time so a
    // stalled runner does not fail it.
    const qint64 ms = t.elapsed();
    QVERIFY2(ticks >= 1 && ticks <= ms / 100 + 2, qPrintable(QString("%1 ticks in %2 ms").arg(ticks).arg(ms)));
  }

  void noTickWithoutSnapshot() {
    PluginSpec s;
    s.lua = "uz.on_tick(function() uz.log.info('tick') end)";
    startOne(s);
    waitRunning("net.test.plugin");
    QTest::qWait(200);
    QVERIFY(!hasLog(con(), "tick"));
  }

  void eventsBetweenPlugins() {
    PluginSpec a;
    a.id = "net.test.a";
    a.lua = R"LUA(
      local payload = { n = 1, list = {1, 2, 3}, nested = { s = "x" }, flag = true, f = 1.5 }
      uz.events.on("*", "pong", function(v, from) uz.log.info("a got pong from " .. from) end)
      uz.on_tick(function()
        if sent then return end
        sent = true
        uz.events.emit("hello", payload)
        payload.list[1] = 99                               -- the receiver gets its own copy
        local ok, e = pcall(uz.events.emit, "bad", { fn = print })
        uz.log.info("fn rejected: " .. tostring(not ok and e:find("function") ~= nil))
        local deep = {} local t = deep for i = 1, 40 do t.x = {} t = t.x end
        local ok2, e2 = pcall(uz.events.emit, "deep", deep)
        uz.log.info("deep rejected: " .. tostring(not ok2 and e2:find("32") ~= nil))
        local ok3, e3 = pcall(uz.events.emit, "huge", string.rep("x", 1024 * 1024 + 1))
        uz.log.info("huge rejected: " .. tostring(not ok3))
        local c = {} c.self = c
        local ok4 = pcall(uz.events.emit, "cyclic", c)
        uz.log.info("cyclic rejected: " .. tostring(not ok4))
      end)
    )LUA";
    PluginSpec b;
    b.id = "net.test.b";
    b.lua = R"LUA(
      uz.events.on("net.test.a", "hello", function(v, from, name)
        v.list[2] = 77                                      -- must not affect anything else
        uz.log.info(string.format("b got %s from %s: n=%d list=%d,%d,%d s=%s flag=%s f=%s", name, from, v.n, v.list[1], v.list[2], v.list[3],
          v.nested.s, tostring(v.flag), tostring(v.f)))
        uz.events.emit("pong", 1)
      end)
      uz.events.on("net.test.zzz", "hello", function() uz.log.info("b must not see this") end)
    )LUA";
    start({a, b});
    waitRunning("net.test.a");
    waitRunning("net.test.b");
    snapshots(2);
    QTRY_VERIFY_WITH_TIMEOUT(hasLog(con("net.test.b"), "b got hello from net.test.a: n=1 list=1,77,3 s=x flag=true f=1.5"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(hasLog(con("net.test.a"), "a got pong from net.test.b"), 5000);
    QVERIFY(hasLog(con("net.test.a"), "fn rejected: true"));
    QVERIFY(hasLog(con("net.test.a"), "deep rejected: true"));
    QVERIFY(hasLog(con("net.test.a"), "huge rejected: true"));
    QVERIFY(hasLog(con("net.test.a"), "cyclic rejected: true"));
    QVERIFY(!hasLog(con("net.test.b"), "must not see"));
    const ConsoleSnapshot ca = con("net.test.a");
    QVERIFY(!ca.events.isEmpty());
    QCOMPARE(ca.events.first().name, QString("hello"));
    QCOMPARE(ca.events.first().plugin, QString("net.test.a"));
    QVERIFY2(ca.events.first().text.contains("n=1"), qPrintable(ca.events.first().text));
    QVERIFY(ca.events.first().timeMs > 0);
    QCOMPARE(con("net.test.b").events.first().name, QString("pong"));
  }

  void provideAndCall() {
    PluginSpec core;
    core.id = "net.test.core";
    core.provides = {"core.api"};
    core.lua = R"LUA(
      local state = { n = 0 }
      uz.provide("core.api", {
        add = function(a, b) return a + b end,
        touch = function(t) t.touched = true t.list[1] = "changed" state.n = state.n + 1 return t, state.n end,
        boom = function() error("callee exploded") end,
        spin = function() while true do end end,
        bad = function() return print end,
        multi = function() return 1, "two", { 3 } end,
      })
      local ok, e = pcall(uz.provide, "not.listed", {})
      assert(not ok and e:find("provides"), e)
    )LUA";
    PluginSpec user;
    user.id = "net.test.user";
    user.requires_ = QJsonObject{{"net.test.core", "^1.0"}};
    user.lua = R"LUA(
      uz.on_tick(function()
        if done then return end
        done = true
        assert(uz.call("core.api", "add", 2, 3) == 5)
        local mine = { list = { "orig" } }
        local t, n = uz.call("core.api", "touch", mine)
        assert(t.touched and t.list[1] == "changed" and n == 1)
        assert(mine.touched == nil and mine.list[1] == "orig", "argument was not copied")
        local a, b, c = uz.call("core.api", "multi")
        assert(a == 1 and b == "two" and c[1] == 3)
        local ok, e = pcall(uz.call, "core.api", "boom")
        uz.log.info("boom -> " .. tostring(ok) .. " " .. tostring(e))
        local ok2, e2 = pcall(uz.call, "core.api", "add", print, 1)
        uz.log.info("fn arg -> " .. tostring(ok2) .. " " .. tostring(e2))
        local ok3, e3 = pcall(uz.call, "core.api", "bad")
        uz.log.info("fn result -> " .. tostring(ok3))
        local ok4, e4 = pcall(uz.call, "core.api", "missing")
        uz.log.info("missing -> " .. tostring(e4))
        local ok5, e5 = pcall(uz.call, "other.api", "x")
        uz.log.info("unknown -> " .. tostring(e5))
        local ok6, e6 = pcall(uz.call, "core.api", "spin")
        uz.log.info("spin -> " .. tostring(ok6) .. " " .. tostring(e6))
        uz.log.info("finished")
      end)
    )LUA";
    PluginSpec stranger;  // does not list the core in requires
    stranger.id = "net.test.stranger";
    stranger.lua = R"LUA(
      uz.on_tick(function()
        if done then return end
        done = true
        local ok, e = pcall(uz.call, "core.api", "add", 1, 2)
        uz.log.info("stranger -> " .. tostring(ok) .. " " .. tostring(e))
      end)
    )LUA";
    HostLimits lim;
    lim.instructions = 500000;
    m_host->setLimits(lim);
    start({core, user, stranger});
    waitRunning("net.test.core");
    waitRunning("net.test.user");
    snapshots(2);
    QTRY_VERIFY_WITH_TIMEOUT(hasLog(con("net.test.user"), "finished"), 10000);
    const ConsoleSnapshot u = con("net.test.user");
    QVERIFY2(hasLog(u, "boom -> false"), qPrintable(logTexts(u).join("\n")));
    QVERIFY(hasLog(u, "callee exploded"));
    QVERIFY(hasLog(u, "fn arg -> false"));
    QVERIFY(hasLog(u, "fn result -> false"));
    QVERIFY(hasLog(u, "missing -> "));
    QVERIFY(hasLog(u, "is not a function"));
    QVERIFY(hasLog(u, "unknown -> "));
    QVERIFY(hasLog(u, "no required plugin provides 'other.api'"));
    QVERIFY(hasLog(u, "spin -> false"));
    QVERIFY(hasLog(u, "was paused"));
    QCOMPARE(u.status, PluginStatus::Running);              // the caller survives
    QCOMPARE(con("net.test.core").status, PluginStatus::Paused);  // the callee that blew its budget is paused
    QVERIFY(con("net.test.core").error.contains("budget exceeded"));
    QVERIFY(hasLog(con("net.test.stranger"), "stranger -> false"));
    QVERIFY(hasLog(con("net.test.stranger"), "no required plugin provides"));
    // a paused callee is reported clearly
    PluginSpec again = user;
    Q_UNUSED(again);
  }

  void permissionErrors() {
    PluginSpec s;  // no permissions at all
    s.lua = R"LUA(
      local function denied(f, perm, ...)
        local ok, e = pcall(f, ...)
        assert(not ok and e:find("permission " .. perm .. " not granted", 1, true), tostring(e))
      end
      denied(uz.memory.watch, "memory.read", "k", 0, 4)
      denied(uz.memory.u8, "memory.read", 0)
      denied(uz.memory.bytes, "memory.read", 0, 1)
      denied(uz.memory.frame, "memory.read")
      denied(uz.dev.watch, "memory.read", "x", 0, "u8")
      denied(uz.rom.size, "rom.read")
      denied(uz.rom.read, "rom.read", 0, 1)
      uz.log.info("denied ok")
    )LUA";
    startOne(s);
    waitRunning("net.test.plugin");
    QVERIFY2(hasLog(con(), "denied ok"), qPrintable(con().error));
    QVERIFY(m_mem->watchesOf("net.test.plugin").empty());
  }

  void romApi() {
    QFile rom(m_dir.path() + "/game.bin");
    QVERIFY(rom.open(QIODevice::WriteOnly));
    rom.write("0123456789ABCDEF");
    rom.close();
    m_ctx.romPath = rom.fileName();
    PluginSpec s;
    s.permissions = {"rom.read"};
    s.lua = R"LUA(
      assert(uz.rom.size() == 16)
      assert(uz.rom.read(4, 4) == "4567")
      assert(uz.rom.read(14, 100) == "EF")
      assert(uz.rom.read(16, 4) == nil)
      assert(not pcall(uz.rom.read, 0, 16 * 1024 * 1024 + 1))
      assert(not pcall(uz.rom.read, -1, 4))
      uz.log.info("rom ok")
    )LUA";
    startOne(s);
    waitRunning("net.test.plugin");
    QVERIFY2(hasLog(con(), "rom ok"), qPrintable(con().error));
  }

  void cacheApi() {
    PluginSpec s;
    s.lua = R"LUA(
      assert(uz.cache.exists("a.bin") == false and uz.cache.read("a.bin") == nil)
      assert(uz.cache.write("a.bin", "\0\1\2data"))
      assert(uz.cache.exists("a.bin") and uz.cache.read("a.bin") == "\0\1\2data")
      assert(uz.cache.write("icons/x/y.png", "png"))
      assert(uz.cache.read("icons/x/y.png") == "png")
      assert(uz.cache.write("a.bin", "short") and uz.cache.read("a.bin") == "short")
      for _, bad in ipairs{"../evil", "a/../../evil", "/abs/path", "..", "a/" .. "/b", "", "sp ace", "back\\slash", "dir/", "x/..", "C:/x", "nul", "CON.png", "icons/aux", "com1.txt", "Lpt9", "trail.", "a./b"} do
        for _, fn in ipairs{uz.cache.read, uz.cache.exists} do
          local ok, e = pcall(fn, bad)
          assert(not ok and e:find("invalid cache name"), bad .. ": " .. tostring(e))
        end
        local ok, e = pcall(uz.cache.write, bad, "x")
        assert(not ok and e:find("invalid cache name"), bad .. ": " .. tostring(e))
      end
      assert(uz.cache.write("console.txt", "ok") and uz.cache.write("com10", "ok") and uz.cache.write(".hidden", "ok"))  -- not device names
      uz.log.info("cache ok")
    )LUA";
    startOne(s);
    waitRunning("net.test.plugin");
    QVERIFY2(hasLog(con(), "cache ok"), qPrintable(con().error));
    QVERIFY(QFileInfo(cache() + "/net.test.plugin/a.bin").isFile());
    QVERIFY(QFileInfo(cache() + "/net.test.plugin/icons/x/y.png").isFile());
    QVERIFY(!QFileInfo(m_dir.path() + "/evil").exists());
    QVERIFY(!QFileInfo(cache() + "/evil").exists());
  }

  // PI-14: bytes alone do not bound the cache: the number of files is capped too (2000 per plugin).
  void cacheFileLimit() {
    PluginSpec s;
    s.lua = R"LUA(
      local n, err = 0, nil
      for i = 1, 2100 do
        local ok, e = pcall(uz.cache.write, "many/f" .. i, "x")
        if not ok then err = tostring(e) break end
        n = n + 1
      end
      uz.log.info("files " .. n .. " " .. tostring(err))
    )LUA";
    HostLimits lim;
    lim.wallMs = 30000;  // 2000 small files without fsync take well under 2 s; generous for slow CI disks
    lim.instructions = 200'000'000;
    m_host->setLimits(lim);
    startOne(s);
    QTRY_COMPARE_WITH_TIMEOUT(m_host->consoleSnapshot("net.test.plugin").status, PluginStatus::Running, 30000);
    QVERIFY2(hasLog(con(), "files 2000 "), qPrintable(con().error));
    QVERIFY2(hasLog(con(), "file limit exceeded"), qPrintable(con().error));
  }

  void cacheQuota() {
    // One 16 MiB write per tick: the wall-clock budget applies per entry call, so a slow disk (CI runner) cannot push
    // the whole 256 MiB series over it, which made the plugin pause with "time budget exceeded" before.
    PluginSpec s;
    s.lua = R"LUA(
      local chunk = string.rep("x", 16 * 1024 * 1024)
      local written = 0
      local done = false
      uz.on_tick(function()
        if done then return end
        local ok, e = pcall(uz.cache.write, "big" .. (written + 1), chunk)
        if ok then
          written = written + 1
          if written > 40 then done = true uz.log.info("written too many") end
        else
          done = true
          uz.log.info("written " .. written .. " " .. tostring(e))
        end
      end)
    )LUA";
    HostLimits lim;
    lim.wallMs = 20000;
    lim.instructions = 200'000'000;
    m_host->setLimits(lim);
    startOne(s);
    waitRunning("net.test.plugin");
    for (int i = 0; i < 400 && !hasLog(con(), "written "); ++i) {
      m_mem->snapshot();
      QTest::qWait(25);
    }
    QVERIFY2(hasLog(con(), "written 16 "), qPrintable(logTexts(con()).join("\n") + con().error));
    QVERIFY(hasLog(con(), "cache quota exceeded"));
  }

  void i18nFallback() {
    PluginSpec s;
    s.files["i18n/en.json"] = R"LUA({"hello":"Hello {name}!","only_en":"English only","nested":{"key":"Nested {a} {b}"}})LUA";
    s.files["i18n/de.json"] = R"LUA({"hello":"Hallo {name}!"})LUA";
    s.lua = R"LUA(
      uz.log.info("1:" .. uz.i18n.t("hello", { name = "Ash" }))
      uz.log.info("2:" .. uz.i18n.t("only_en"))
      uz.log.info("3:" .. uz.i18n.t("missing.key"))
      uz.log.info("4:" .. uz.i18n.t("nested.key", { a = 1, b = true }))
      uz.log.info("5:" .. uz.i18n.t("hello", { other = "x" }))
      uz.log.info("6:" .. uz.i18n.t("hello"))
    )LUA";
    m_ctx.language = "de-DE";
    startOne(s);
    waitRunning("net.test.plugin");
    const QStringList l = logTexts(con());
    QVERIFY2(l.contains("1:Hallo Ash!"), qPrintable(l.join("\n")));
    QVERIFY(l.contains("2:English only"));
    QVERIFY(l.contains("3:missing.key"));
    QVERIFY(l.contains("4:Nested 1 true"));
    QVERIFY(l.contains("5:Hallo {name}!"));
    QVERIFY(l.contains("6:Hallo {name}!"));
    m_host->stop();
    m_ctx.language = "fr";
    startOne(s);
    waitRunning("net.test.plugin");
    QVERIFY(logTexts(con()).contains("1:Hello Ash!"));
  }

  void consoleFeed() {
    m_mem->setRam(0x08001A10, {0x2A, 0, 0, 0});
    m_mem->setRam(0x08002000, {0xDE, 0xAD, 0xBE, 0xEF});
    m_mem->setRam(0x08003000, {0xFE, 0xFF, 0xFF, 0xFF});
    PluginSpec s;
    s.permissions = kMem;
    s.lua = R"LUA(
      uz.dev.watch("party count", 0x08001A10, "u8")
      uz.dev.watch("magic", 0x08002000, "hex:4")
      uz.dev.watch("signed", 0x08003000, "s32")
      uz.dev.watch("nowhere", 0x0F000000, "u16")
      assert(not pcall(uz.dev.watch, "x", 0, "float"))
      assert(not pcall(uz.dev.watch, "x", 0, "hex:0"))
      uz.dev.watch("temp", 0x08001A10, "u32")
      uz.dev.unwatch("temp")
      uz.log.debug("a debug line")
      uz.log.info("an info line", 42)
      uz.log.warn("a warn line")
      uz.log.error("an error line")
      print("printed", "line")
      uz.events.emit("team.changed", { slot = 1 })
    )LUA";
    startOne(s);
    waitRunning("net.test.plugin");
    snapshots(2);
    QTRY_VERIFY_WITH_TIMEOUT(con().watched.size() == 4 && con().watched[0].value == "42", 5000);
    const ConsoleSnapshot c = con();
    QCOMPARE(c.watched[0].label, QString("party count"));
    QCOMPARE(c.watched[0].address, QString("0x08001A10"));
    QCOMPARE(c.watched[1].value, QString("DE AD BE EF"));
    QCOMPARE(c.watched[2].value, QString("-2"));
    QCOMPARE(c.watched[3].value, QString("-"));
    QCOMPARE(m_mem->watchesOf("net.test.plugin").size(), size_t(4));
    QVERIFY(hasLog(c, "a debug line") && hasLog(c, "an info line\t42") && hasLog(c, "printed\tline"));
    int levels[4] = {0, 0, 0, 0};
    for (const LogLine& l : c.log) {
      if (l.text == "a debug line") QCOMPARE(l.level, LogLevel::Debug);
      if (l.text == "an info line\t42") QCOMPARE(l.level, LogLevel::Info);
      if (l.text == "printed\tline") QCOMPARE(l.level, LogLevel::Info);
      if (l.text == "a warn line") QCOMPARE(l.level, LogLevel::Warn);
      if (l.text == "an error line") QCOMPARE(l.level, LogLevel::Error);
      QVERIFY(l.timeMs > 0);
      ++levels[static_cast<int>(l.level)];
    }
    QVERIFY(levels[0] && levels[1] && levels[2] && levels[3]);
    QCOMPARE(c.events.size(), 1);
    QCOMPARE(c.events[0].name, QString("team.changed"));
    QCOMPARE(c.events[0].text, QString("{slot=1}"));
    // signals
    QSignalSpy changed(m_host, &PluginHost::consoleChanged);
    m_mem->setRam(0x08001A10, {0x07, 0, 0, 0});
    snapshots(3);
    QTRY_VERIFY_WITH_TIMEOUT(con().watched[0].value == "7", 5000);
    QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 1, 5000);
    QCOMPARE(changed.first().first().toString(), QString("net.test.plugin"));
    // unwatch everything when stopped
    m_host->stop();
    QVERIFY(m_mem->watchesOf("net.test.plugin").empty());
    QVERIFY(con().watched.isEmpty());
    QCOMPARE(con().status, PluginStatus::Stopped);
  }

  void logRing() {
    PluginSpec s;
    s.lua = "for i = 1, 700 do uz.log.info('line ' .. i) end for i = 1, 300 do uz.events.emit('e', i) end";
    startOne(s);
    waitRunning("net.test.plugin");
    const ConsoleSnapshot c = con();
    QVERIFY(c.log.size() <= 500 && c.log.size() >= 499);
    QVERIFY(hasLog(c, "line 700"));
    QVERIFY(!hasLog(c, "line 1\n") && !logTexts(c).contains("line 1"));
    QCOMPARE(c.events.size(), 200);
    QCOMPARE(c.events.last().text, QString("300"));
  }

  void errorsPauseAndReload() {
    PluginSpec s;
    s.lua = "local function broken() error('first version is broken') end\nbroken()";
    startOne(s);
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Paused, 10000);
    QVERIFY(con().error.contains("first version is broken"));
    QVERIFY(con().error.contains("stack traceback"));
    QVERIFY(hasLog(con(), "Paused: "));
    // fix the file, reload
    QSignalSpy status(m_host, &PluginHost::statusChanged);
    PluginSpec fixed = s;
    fixed.lua = "uz.log.info('fixed')";
    writePlugin(m_dir.path() + "/src", fixed);
    m_host->reload("net.test.plugin");
    waitRunning("net.test.plugin");
    QVERIFY(hasLog(con(), "fixed"));
    QVERIFY(con().error.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(status.count() >= 1, 5000);
    // a manifest that became invalid pauses the plugin with a clear message
    QFile::remove(m_dir.path() + "/src/net.test.plugin/manifest.json");
    m_host->reload("net.test.plugin");
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Paused, 5000);
    QVERIFY(con().error.contains("manifest.json"));
    m_host->reload("net.unknown.id");  // ignored
  }

  void runtimeErrorInTickPausesOnlyThatPlugin() {
    PluginSpec bad;
    bad.id = "net.test.bad";
    bad.lua = "uz.on_tick(function() local x = nil; return x.field end)";
    PluginSpec good;
    good.id = "net.test.good";
    good.lua = "uz.on_tick(function() uz.log.info('alive') end)";
    start({bad, good});
    waitRunning("net.test.good");
    snapshots(4);
    QTRY_COMPARE_WITH_TIMEOUT(con("net.test.bad").status, PluginStatus::Paused, 5000);
    QVERIFY(con("net.test.bad").error.contains("attempt to index a nil value"));
    QCOMPARE(con("net.test.good").status, PluginStatus::Running);
    QVERIFY(hasLog(con("net.test.good"), "alive"));
  }

  void restartReplacesSet() {
    PluginSpec a;
    a.id = "net.test.a";
    a.lua = "uz.log.info('a')";
    PluginSpec b;
    b.id = "net.test.b";
    b.lua = "uz.log.info('b')";
    start({a});
    waitRunning("net.test.a");
    start({b});
    waitRunning("net.test.b");
    QCOMPARE(m_host->pluginIds(), QStringList{"net.test.b"});
    m_host->stop();
    m_host->stop();
  }
};

FB_TEST_MAIN(HostTest)
#include "host_test.moc"
