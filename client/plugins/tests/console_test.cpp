// Developer console data of the plugin runtime (0.22, boards 4d-1 and 4d-2): the pause rule (three handler errors within ten
// seconds, budget overruns at once), log sequence numbers, denied permissions, reload timing and the "reloaded" host event.
// Homebrew plugins written to a temporary directory only.
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "plugin_host.h"
#include "plugin_test_util.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

namespace {
const QString kId = QStringLiteral("net.test.plugin");
using Clock = std::chrono::steady_clock;
}  // namespace

class ConsoleTest : public QObject {
  Q_OBJECT

  QTemporaryDir m_dir;
  PluginHost* m_host = nullptr;

  void startLua(const QString& lua, const QStringList& perms = {"ui"}) {
    PluginSpec s;
    s.permissions = perms;
    s.ui = QJsonObject{{"panel", QJsonObject{{"title", "T"}}}};
    s.lua = lua;
    const PluginInfo info = infoFor(writePlugin(m_dir.path() + "/src", s), m_dir.path() + "/cache");
    m_host->start(GameContext{"nds", "ABCD", "abc", "Game", "", "en", false}, std::make_shared<FakeMemoryProvider>(), {info});
    QTRY_COMPARE_WITH_TIMEOUT(m_host->consoleSnapshot(kId).status, PluginStatus::Running, 10000);
  }
  ConsoleSnapshot con() const { return m_host->consoleSnapshot(kId); }

 private slots:
  void init() { m_host = new PluginHost; }
  void cleanup() {
    delete m_host;
    m_host = nullptr;
    QDir(m_dir.path() + "/src").removeRecursively();
  }

  // The window logic on its own, with explicit times.
  void errorBurstWindow() {
    ErrorBurst b;
    const auto t0 = Clock::now();
    QVERIFY(!b.record(t0));
    QVERIFY(!b.record(t0 + std::chrono::seconds(2)));
    QVERIFY(b.record(t0 + std::chrono::seconds(4)));  // the third within ten seconds
    ErrorBurst slow;
    QVERIFY(!slow.record(t0));
    QVERIFY(!slow.record(t0 + std::chrono::seconds(6)));
    QVERIFY(!slow.record(t0 + std::chrono::seconds(12)));  // the first one is older than ten seconds: two in the window
    QCOMPARE(slow.count(), 2);
    QVERIFY(slow.record(t0 + std::chrono::seconds(13)));
    slow.reset();
    QCOMPARE(slow.count(), 0);
  }

  void singleErrorIsLoggedNotPaused() {
    startLua("fb.ui.on_action(function() error('boom') end)");
    m_host->sendAction(kId, "panel", "x");
    QTRY_COMPARE_WITH_TIMEOUT(con().errorCount, 1, 5000);
    QCOMPARE(con().status, PluginStatus::Running);
    QVERIFY(con().error.isEmpty());
    bool logged = false;
    for (const LogLine& l : con().log) logged = logged || (l.level == LogLevel::Error && l.text.contains(QStringLiteral("boom")));
    QVERIFY(logged);
  }

  void threeErrorsWithinTenSecondsPause() {
    startLua("fb.ui.on_action(function() error('boom') end)");
    for (int i = 0; i < 3; ++i) m_host->sendAction(kId, "panel", "x");
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Paused, 5000);
    QCOMPARE(con().errorCount, 3);
    QVERIFY(con().error.contains(QStringLiteral("boom")));
    QVERIFY(con().error.contains(QStringLiteral("3 errors within 10 s")));
    bool pausedLine = false;
    for (const LogLine& l : con().log) pausedLine = pausedLine || l.text.startsWith(QStringLiteral("Paused:"));
    QVERIFY(pausedLine);
  }

  void budgetOverrunPausesAtOnce() {
    startLua("fb.ui.on_action(function() while true do end end)");
    m_host->sendAction(kId, "panel", "x");
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Paused, 10000);
    QVERIFY(con().error.contains(QStringLiteral("budget exceeded")));
  }

  void logLinesCarryIncreasingSeqAcrossReload() {
    startLua("fb.log.info('one') fb.log.warn('two')");
    const ConsoleSnapshot a = con();
    QVERIFY(a.log.size() >= 3);  // one, two, Loaded
    for (qsizetype i = 1; i < a.log.size(); ++i) QCOMPARE(a.log[i].seq, a.log[i - 1].seq + 1);
    QCOMPARE(a.lastSeq, a.log.last().seq);
    QCOMPARE(a.reloadCount, 0);
    QCOMPARE(a.reloadMs, qint64(-1));
    m_host->reload(kId);
    QTRY_COMPARE_WITH_TIMEOUT(con().reloadCount, 1, 5000);
    QCOMPARE(con().status, PluginStatus::Running);
    QVERIFY(con().reloadMs >= 0);
    QVERIFY(con().log.last().seq > a.lastSeq);  // the sequence goes on, so since_seq polling works over a reload
  }

  void reloadResetsTheErrorWindowAndResumes() {
    startLua("fb.ui.on_action(function() error('boom') end)");
    m_host->sendAction(kId, "panel", "x");
    m_host->sendAction(kId, "panel", "x");
    QTRY_COMPARE_WITH_TIMEOUT(con().errorCount, 2, 5000);
    m_host->reload(kId);
    QTRY_COMPARE_WITH_TIMEOUT(con().reloadCount, 1, 5000);
    m_host->sendAction(kId, "panel", "x");  // the third error overall, but the first after the reload
    QTRY_COMPARE_WITH_TIMEOUT(con().errorCount, 3, 5000);
    QCOMPARE(con().status, PluginStatus::Running);
  }

  void pausedPluginResumesWithReload() {
    startLua("fb.ui.on_action(function() while true do end end)");
    m_host->sendAction(kId, "panel", "x");
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Paused, 10000);
    m_host->reload(kId);
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Running, 5000);
    QCOMPARE(con().reloadCount, 1);
    QVERIFY(con().error.isEmpty());
  }

  void reloadedEventReachesThePlugin() {
    startLua("fb.events.on('fb', 'reloaded', function(v) fb.log.info('RELOADED ' .. tostring(v.ms >= 0)) end)");
    m_host->reload(kId);
    QTRY_VERIFY_WITH_TIMEOUT(hasLog(con(), "RELOADED true"), 5000);
  }

  void deniedPermissionIsListedOnce() {
    startLua("fb.ui.on_action(function() pcall(function() return fb.memory.write(0, 'x') end) pcall(function() return fb.memory.write(0, 'x') end) end)", {"ui"});
    m_host->sendAction(kId, "panel", "x");
    QTRY_VERIFY_WITH_TIMEOUT(!con().notGranted.isEmpty(), 5000);
    QCOMPARE(con().notGranted, QStringList{"memory.write"});
    QCOMPARE(con().status, PluginStatus::Running);
  }

 private:
  static bool hasLog(const ConsoleSnapshot& c, const QString& needle) {
    for (const LogLine& l : c.log)
      if (l.text.contains(needle)) return true;
    return false;
  }
};

QTEST_GUILESS_MAIN(ConsoleTest)
#include "console_test.moc"
