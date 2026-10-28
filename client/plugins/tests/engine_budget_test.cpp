// Aggregate budgets of the plugin engine: one iterate() delivers events for a bounded time (PI-3), the event queue is
// bounded in bytes per plugin and in total (PI-4). The engine is driven directly (no thread), so the tests are deterministic.
#include <QTemporaryDir>
#include <QtTest>

#include "plugin_engine.h"
#include "plugin_test_util.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

class EngineBudgetTest : public QObject {
  Q_OBJECT

  QTemporaryDir m_dir;

  PluginInfo make(const PluginSpec& s) { return infoFor(writePlugin(m_dir.path() + "/src", s), m_dir.path() + "/cache"); }
  static int countLogs(const ConsoleSnapshot& c, const QString& text) {
    int n = 0;
    for (const LogLine& l : c.log) n += l.text == text;
    return n;
  }

 private slots:
  void initTestCase() { QVERIFY(m_dir.isValid()); }

  // PI-3: handlers of all plugins together get kIterateBudgetMs per iterate(); the rest of the queue stays queued and is
  // delivered by the next iterations.
  void iterateStopsDeliveringWhenTheAggregateBudgetIsUsed() {
    PluginSpec emitter;
    emitter.id = "net.test.emitter";
    emitter.lua = "for i = 1, 24 do uz.events.emit('ping', i) end";
    PluginSpec slow;
    slow.id = "net.test.slow";
    slow.lua = R"LUA(
      uz.events.on("*", "ping", function()
        local x = 0
        for i = 1, 4000000 do x = x + i end   -- a few tens of ms: well below the per-call limits, far above the aggregate budget
        uz.log.info("h")
      end)
    )LUA";
    ConsoleStore store;
    std::atomic<int> hz{100};
    PluginEngine engine(store, hz, {});
    HostLimits lim;
    lim.instructions = 200'000'000;  // the per-call limits are not what is tested here
    lim.wallMs = 5000;
    engine.start(GameContext{"nds", "ABCD", "abc", "Game", "", "en"}, nullptr, {make(emitter), make(slow)}, lim);
    QCOMPARE(store.snapshot("net.test.slow").status, PluginStatus::Running);
    QCOMPARE(engine.events.size(), size_t(24));

    QElapsedTimer t;
    t.start();
    engine.iterate();
    const qint64 first = t.elapsed();
    const int handled = countLogs(store.snapshot("net.test.slow"), "h");
    QVERIFY2(handled >= 1, "at least one event is delivered per iteration");
    QVERIFY2(engine.events.size() > 0 && handled < 24, qPrintable(QString("%1 handled in %2 ms").arg(handled).arg(first)));
    QCOMPARE(int(engine.events.size()) + handled, 24);  // nothing lost: the rest stays queued
    QVERIFY2(first < 2000, qPrintable(QString("one iteration took %1 ms").arg(first)));
    for (int i = 0; i < 200 && !engine.events.empty(); ++i) engine.iterate();
    QVERIFY(engine.events.empty());
    QCOMPARE(countLogs(store.snapshot("net.test.slow"), "h"), 24);
    QCOMPARE(engine.queuedEventBytes(), size_t(0));
  }

  // PI-4: events are bounded by their real size per emitting plugin and in total, not only by their number.
  void eventQueueIsBoundedInBytes() {
    PluginSpec p;
    p.id = "net.test.flood";
    p.lua = R"LUA(
      local big = {}
      for i = 1, 10000 do big[i] = i end      -- about 1 MiB by the reader's accounting, a multiple of that as host objects
      local n = 0
      local last
      for i = 1, 100 do
        local ok, e = pcall(uz.events.emit, "flood", big)
        if not ok then last = e break end
        n = n + 1
      end
      uz.log.info("emitted " .. n)
      uz.log.info("stopped: " .. tostring(last))
    )LUA";
    ConsoleStore store;
    std::atomic<int> hz{100};
    PluginEngine engine(store, hz, {});
    engine.start(GameContext{"nds", "ABCD", "abc", "Game", "", "en"}, nullptr, {make(p)}, HostLimits{});
    QCOMPARE(store.snapshot("net.test.flood").status, PluginStatus::Running);
    const ConsoleSnapshot c = store.snapshot("net.test.flood");
    int emitted = -1;
    bool full = false;
    for (const LogLine& l : c.log) {
      if (l.text.startsWith("emitted ")) emitted = l.text.mid(8).toInt();
      full = full || l.text.contains("event queue is full");
    }
    QVERIFY2(emitted >= 1 && emitted <= 4, qPrintable(QString("emitted %1").arg(emitted)));
    QVERIFY(full);
    QVERIFY(engine.queuedEventBytes() <= kMaxQueuedEventBytesPerPlugin);
    QVERIFY(engine.queuedEventBytes() > 0);
    // Host events share the total but have their own source share: they are dropped silently when full, never crash.
    for (int i = 0; i < 100; ++i) {
      Value v;
      v.type = Value::Type::String;
      v.s.assign(512 * 1024, 'x');
      engine.postHostEvent(QStringLiteral("video.captured"), v, QString());
    }
    QVERIFY(engine.queuedEventBytes() <= kMaxQueuedEventBytes + kMaxQueuedEventBytesPerPlugin);
    // Draining returns the accounting to zero.
    for (int i = 0; i < 400 && !engine.events.empty(); ++i) engine.iterate();
    QVERIFY(engine.events.empty());
    QCOMPARE(engine.queuedEventBytes(), size_t(0));
  }
};

QTEST_APPLESS_MAIN(EngineBudgetTest)
#include "engine_budget_test.moc"
