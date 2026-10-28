#include <QTemporaryDir>
#include <QtTest>

#include "lauxlib.h"
#include "lua.h"
#include "lua_sandbox.h"
#include "plugin_host.h"
#include "plugin_test_util.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

class SandboxTest : public QObject {
  Q_OBJECT

  static bool hasLog(const ConsoleSnapshot& c, const QString& needle) {
    for (const LogLine& l : c.log)
      if (l.text.contains(needle)) return true;
    return false;
  }

 private slots:
  void removedGlobals() {
    LuaSandbox sb;
    std::string err;
    const bool ok = sb.runChunk(R"(
      for _, n in ipairs{"io","os","package","debug","load","loadstring","dofile","loadfile","require"} do
        assert(_G[n] == nil, n .. " must be nil")
      end
      assert(string.dump == nil)
      assert(type(string.format) == "function" and type(table.concat) == "function" and type(math.floor) == "function")
      assert(type(utf8.char) == "function" and type(coroutine.wrap) == "function")
      assert(type(collectgarbage("count")) == "number")
      assert(collectgarbage("collect") == 0)
      assert(getmetatable("").__index == string)
    )", "@t.lua", err);
    QVERIFY2(ok, err.c_str());
  }

  void printGoesToSink() {
    LuaSandbox sb;
    std::vector<std::string> lines;
    sb.setPrintSink([&](LogLevel lv, const std::string& t) {
      QCOMPARE(lv, LogLevel::Info);
      lines.push_back(t);
    });
    std::string err;
    QVERIFY2(sb.runChunk("print('a', 1, nil, true)", "@t.lua", err), err.c_str());
    QCOMPARE(lines.size(), size_t(1));
    QCOMPARE(QString::fromStdString(lines[0]), QString("a\t1\tnil\ttrue"));
  }

  // PI-1: native pattern matching must hit a budget instead of running for seconds inside one C call (stock Lua 5.4: 6.9 s).
  void quadraticPatternsAbortQuickly() {
    const char* cases[] = {
        R"LUA(return string.rep("a", 40000):find(".-b"))LUA",
        R"LUA(return string.rep("a", 40000):match(".-b"))LUA",
        R"LUA(for _ in string.gmatch(string.rep("a", 40000), ".-b") do end)LUA",
        R"LUA(return string.gsub(string.rep("a", 40000), ".-b", "x"))LUA",
        R"LUA(return string.rep("a", 3000):find(".-.-.-.-b"))LUA",  // cubic and worse with several lazy quantifiers
    };
    for (const char* code : cases) {
      LuaSandbox sb;
      std::string err;
      QElapsedTimer t;
      t.start();
      const bool ok = sb.runChunk(code, "@t.lua", err);
      QVERIFY2(!ok, code);
      QVERIFY2(err.find("budget exceeded") != std::string::npos, (std::string(code) + ": " + err).c_str());
      QVERIFY2(t.elapsed() < 600, qPrintable(QString("%1 took %2 ms").arg(code).arg(t.elapsed())));
      QVERIFY(sb.trip() == LuaSandbox::Trip::Time);
    }
  }

  void patternBudgetCannotBeSwallowedByPcall() {
    LuaSandbox sb;
    std::string err;
    const bool ok = sb.runChunk(R"LUA(
      local s = string.rep("a", 40000)
      pcall(string.find, s, ".-b")
      local n = 0
      while true do n = n + 1 end  -- the tripped budget raises again at the next instruction
    )LUA", "@t.lua", err);
    QVERIFY(!ok);
    QVERIFY(err.find("budget exceeded") != std::string::npos);
  }

  void normalPatternsStillWork() {
    LuaSandbox sb;
    std::string err;
    QVERIFY2(sb.runChunk(R"LUA(
      assert(("hello world"):find("o w") == 5)
      assert(("key=value"):match("(%w+)=(%w+)") == "key")
      assert(select(2, ("a,b,c"):gsub(",", ";")) == 2)
      local n = 0 for w in ("one two three"):gmatch("%a+") do n = n + 1 end assert(n == 3)
      local big = string.rep("ab", 50000)
      assert(#big:gsub("a", "") == 50000)           -- linear work on a long subject is fine
      assert(big:find("ba", 1, true) == 2)
    )LUA", "@t.lua", err), err.c_str());
  }

  void stringRepIsCapped() {
    LuaSandbox sb;
    std::string err;
    QVERIFY2(sb.runChunk(R"LUA(
      assert(#string.rep("x", 1000, ",") == 1999)
      local ok, e = pcall(string.rep, "x", 32 * 1024 * 1024)
      assert(not ok and e:find("larger than"))
      assert(string.rep("", 1e9) == "")
    )LUA", "@t.lua", err), err.c_str());
  }

  // PI-2: a finalizer would run in lua_close with no hook; setmetatable refuses __gc, so closing is quick.
  void gcMetamethodIsRejectedAndCloseIsQuick() {
    QElapsedTimer t;
    t.start();
    {
      LuaSandbox sb;
      std::string err;
      QVERIFY2(sb.runChunk(R"LUA(
        local ok, e = pcall(setmetatable, {}, {__gc = function() while true do end end})
        assert(not ok and e:find("__gc"), tostring(e))
        local mt = {}
        local obj = setmetatable({}, mt)
        mt.__gc = function() while true do end end   -- too late for Lua: not marked for finalization
        local t2 = setmetatable({}, {__index = function() return 1 end})
        assert(t2.x == 1 and getmetatable(t2) ~= nil)
        assert(getmetatable(setmetatable({}, nil)) == nil)
      )LUA", "@t.lua", err), err.c_str());
    }  // ~LuaSandbox -> lua_close
    QVERIFY2(t.elapsed() < 500, qPrintable(QString("close took %1 ms").arg(t.elapsed())));
  }

  // PI-9: no host addresses in tostring / print / string.format.
  void noAddressesInTostring() {
    LuaSandbox sb;
    std::vector<std::string> lines;
    sb.setPrintSink([&](LogLevel, const std::string& t) { lines.push_back(t); });
    std::string err;
    QVERIFY2(sb.runChunk(R"LUA(
      local function noaddr(s) assert(not s:find("0x"), s) assert(not s:find("%x%x%x%x%x%x%x%x%x%x")) end
      local t = {}
      local a, b = tostring(t), tostring(print)
      noaddr(a) noaddr(b)
      assert(a == tostring(t) and a ~= tostring({}))       -- stable per object, distinct between objects
      assert(a:find("^table: #%d+$") and b:find("^function: #%d+$"))
      noaddr(string.format("%s %s", t, print))
      assert(not pcall(string.format, "%p", t))
      assert(string.format("100%% %s %d", "x", 5) == "100% x 5")
      assert(tostring(setmetatable({}, {__tostring = function() return "mine" end})) == "mine")
      assert(string.format("%s", setmetatable({}, {__tostring = function() return "mine" end})) == "mine")
      assert(tostring(1) == "1" and tostring(nil) == "nil" and tostring("s") == "s" and tostring(true) == "true")
      print(t, print, coroutine.create(print))
    )LUA", "@t.lua", err), err.c_str());
    QCOMPARE(lines.size(), size_t(1));
    QVERIFY(!QString::fromStdString(lines[0]).contains("0x"));
  }

  void bytecodeRejected() {
    LuaSandbox sb;
    std::string err;
    // Real bytecode produced by a throwaway state, then fed to the text-only loader.
    std::string dumped;
    {
      lua_State* L = luaL_newstate();
      QVERIFY(luaL_loadstring(L, "return 1") == LUA_OK);
      lua_dump(L, [](lua_State*, const void* p, size_t sz, void* ud) -> int {
        static_cast<std::string*>(ud)->append(static_cast<const char*>(p), sz);
        return 0;
      }, &dumped, 0);
      lua_close(L);
    }
    QVERIFY(dumped.size() > 4 && dumped[0] == '\x1b');
    QVERIFY(!sb.runChunk(dumped, "=bin", err));
    QVERIFY2(err.find("binary") != std::string::npos, err.c_str());
    QVERIFY(!sb.runChunk("\x1bLua", "=bin", err));
  }

  void errorHasTraceback() {
    LuaSandbox sb;
    std::string err;
    QVERIFY(!sb.runChunk("local function boom() error('kaputt') end\nboom()", "@main.lua", err));
    QVERIFY2(err.find("main.lua:1: kaputt") != std::string::npos, err.c_str());
    QVERIFY2(err.find("stack traceback") != std::string::npos, err.c_str());
    QVERIFY(!sb.runChunk("error({})", "@t.lua", err));
    QVERIFY(!sb.runChunk("this is not lua", "@t.lua", err));
    QVERIFY(err.find("syntax error") != std::string::npos || err.find("near") != std::string::npos);
  }

  void instructionBudget() {
    SandboxLimits lim;
    lim.instructions = 200000;
    LuaSandbox sb(lim);
    std::string err;
    QVERIFY(!sb.runChunk("while true do end", "@t.lua", err));
    QCOMPARE(sb.trip(), LuaSandbox::Trip::Instructions);
    QVERIFY2(err.find("instruction budget exceeded") != std::string::npos, err.c_str());
    // pcall must not be able to swallow the budget error
    QVERIFY(!sb.runChunk("while true do pcall(function() while true do end end) end", "@t.lua", err));
    QCOMPARE(sb.trip(), LuaSandbox::Trip::Instructions);
    // inside a coroutine too
    QVERIFY(!sb.runChunk("local co = coroutine.wrap(function() while true do end end) while true do pcall(co) end", "@t.lua", err));
    QCOMPARE(sb.trip(), LuaSandbox::Trip::Instructions);
    // the budget is per call: the state is usable again
    QVERIFY2(sb.runChunk("local n = 0 for i = 1, 1000 do n = n + i end", "@t.lua", err), err.c_str());
    QCOMPARE(sb.trip(), LuaSandbox::Trip::None);
  }

  void timeBudget() {
    SandboxLimits lim;
    lim.instructions = 4'000'000'000ll;
    lim.wallMs = 50;
    LuaSandbox sb(lim);
    std::string err;
    QElapsedTimer t;
    t.start();
    QVERIFY(!sb.runChunk("while true do end", "@t.lua", err));
    QCOMPARE(sb.trip(), LuaSandbox::Trip::Time);
    QVERIFY2(err.find("time budget exceeded") != std::string::npos, err.c_str());
    QVERIFY(t.elapsed() < 3000);
  }

  void memoryLimit() {
    SandboxLimits lim;
    lim.memoryBytes = 8u * 1024 * 1024;
    LuaSandbox sb(lim);
    std::string err;
    QVERIFY(!sb.runChunk("local t = {} for i = 1, 1e9 do t[i] = string.rep('x', 1000) .. i end", "@t.lua", err));
    QCOMPARE(sb.trip(), LuaSandbox::Trip::Memory);
    QVERIFY2(err.find("memory limit exceeded") != std::string::npos, err.c_str());
    QVERIFY(sb.memoryUsed() < lim.memoryBytes + 1024);
    LuaSandbox sb2(lim);
    QVERIFY(!sb2.runChunk("local s = string.rep('x', 1e10)", "@t.lua", err));
  }

  void hostPausesOnlyTheBadPlugin() {
    QTemporaryDir t;
    PluginSpec bad;
    bad.id = "net.test.bad";
    bad.lua = "uz.on_tick(function() while true do end end)";
    PluginSpec good;
    good.id = "net.test.good";
    good.lua = "local n = 0 uz.on_tick(function() n = n + 1 uz.log.info('tick ' .. n) end)";
    PluginSpec bomb;
    bomb.id = "net.test.bomb";
    bomb.lua = "local t = {} for i = 1, 1e9 do t[i] = string.rep('y', 1000) .. i end";
    PluginSpec loop;
    loop.id = "net.test.loop";
    loop.lua = "while true do end";
    const QString cache = t.path() + "/cache";
    QList<PluginInfo> infos;
    for (const PluginSpec* s : {&bad, &good, &bomb, &loop}) infos << infoFor(writePlugin(t.path(), *s), cache);

    auto mem = std::make_shared<FakeMemoryProvider>();
    PluginHost host;
    HostLimits lim;
    lim.instructions = 300000;
    lim.memoryBytes = 8u * 1024 * 1024;
    host.setLimits(lim);
    host.setTickHz(240);
    QSignalSpy status(&host, &PluginHost::statusChanged);
    host.start(GameContext{"nds", "ABCD", "", "", "", "en"}, mem, infos);
    QTRY_COMPARE_WITH_TIMEOUT(host.consoleSnapshot("net.test.loop").status, PluginStatus::Paused, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(host.consoleSnapshot("net.test.bomb").status, PluginStatus::Paused, 10000);
    QVERIFY(host.consoleSnapshot("net.test.loop").error.contains("budget exceeded"));
    QVERIFY(host.consoleSnapshot("net.test.loop").error.contains("traceback"));
    QVERIFY(host.consoleSnapshot("net.test.bomb").error.contains("memory limit exceeded"));
    QVERIFY(hasLog(host.consoleSnapshot("net.test.bomb"), "Paused"));
    QCOMPARE(host.consoleSnapshot("net.test.good").status, PluginStatus::Running);
    QTRY_VERIFY_WITH_TIMEOUT(host.consoleSnapshot("net.test.bad").status == PluginStatus::Running, 5000);

    // snapshots drive on_tick: the bad one gets paused in its tick, the good one keeps ticking
    for (int i = 0; i < 40; ++i) {
      mem->snapshot();
      QTest::qWait(10);
    }
    QTRY_COMPARE_WITH_TIMEOUT(host.consoleSnapshot("net.test.bad").status, PluginStatus::Paused, 10000);
    const int before = host.consoleSnapshot("net.test.good").log.size();
    for (int i = 0; i < 40; ++i) {
      mem->snapshot();
      QTest::qWait(10);
    }
    QCOMPARE(host.consoleSnapshot("net.test.good").status, PluginStatus::Running);
    QVERIFY(host.consoleSnapshot("net.test.good").log.size() > before);
    QTRY_VERIFY_WITH_TIMEOUT(!status.isEmpty(), 5000);
    host.stop();
    QCOMPARE(host.consoleSnapshot("net.test.good").status, PluginStatus::Stopped);
  }
};

QTEST_MAIN(SandboxTest)
#include "sandbox_test.moc"
