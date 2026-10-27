// RewindBuffer (ring buffer limits) and rewind / run-ahead in EmulationRunner against the Uzume test core (real
// retro_serialize). Never skips.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "emulation_runner.h"
#include "libretro_backend.h"
#include "rewind_buffer.h"
#include "test_core_layout.h"

using namespace uzume::emu;
namespace mem = uzume::testcore::mem;
using Cmd = EmulationRunner::Command;

class RewindTest : public QObject {
  Q_OBJECT
  QTemporaryDir m_dir;
  QString m_core = qEnvironmentVariable("UZUME_MELONDS_DS_CORE");
  EmulationRunner::StartRequest request() const {
    EmulationRunner::StartRequest req;
    req.corePath = m_core;
    req.gamePath = QStringLiteral(FB_TEST_ROM_PATH);
    req.systemDir = m_dir.filePath(QStringLiteral("system"));
    req.saveDir = m_dir.filePath(QStringLiteral("save"));
    return req;
  }
  static uint32_t frameOf(EmulationRunner& runner) {
    uint32_t v = 0;
    runner.runOnEmuThread([&](EmulatorBackend& be) {
      const MemoryMap* mm = be.memoryMap();
      const auto sp = mm ? mm->resolve(mem::kHeapAddr + mem::kFrameOff) : MemoryMap::Span{};
      if (sp.ptr) for (int i = 3; i >= 0; --i) v = (v << 8) | sp.ptr[i];
    });
    return v;
  }

 private slots:
  void initTestCase() { QVERIFY2(!m_core.isEmpty(), "UZUME_MELONDS_DS_CORE must point to the test core"); }

  void bufferDropsOldestByCount() {
    RewindBuffer b(3, 1000);
    for (int i = 1; i <= 5; ++i) QVERIFY(b.push(QByteArray(10, static_cast<char>('a' + i)), static_cast<quint64>(i * 4)));
    QCOMPARE(b.count(), 3);
    QCOMPARE(b.bytes(), qint64{30});
    QCOMPARE(b.oldestFrame(), quint64{12});
    QCOMPARE(b.newestFrame(), quint64{20});
    QByteArray s;
    quint64 f = 0;
    QVERIFY(b.pop(&s, &f));
    QCOMPARE(f, quint64{20});
    QCOMPARE(s, QByteArray(10, 'f'));
    QCOMPARE(b.count(), 2);
  }

  void bufferDropsOldestByBytesAndRefusesOversize() {
    RewindBuffer b(100, 25);
    QVERIFY(b.push(QByteArray(10, 'a'), 1));
    QVERIFY(b.push(QByteArray(10, 'b'), 2));
    QVERIFY(b.push(QByteArray(10, 'c'), 3));  // 30 > 25: the oldest goes
    QCOMPARE(b.count(), 2);
    QCOMPARE(b.oldestFrame(), quint64{2});
    QVERIFY(!b.push(QByteArray(26, 'x'), 4));
    QCOMPARE(b.count(), 2);
    QVERIFY(!b.push(QByteArray(), 5));
    b.setLimits(1, 25);  // shrinking drops the oldest
    QCOMPARE(b.count(), 1);
    QCOMPARE(b.newestFrame(), quint64{3});
    b.setLimits(0, 0);
    QVERIFY(!b.enabled() && b.empty());
    QVERIFY(!b.push(QByteArray(1, 'a'), 1));
    QByteArray s;
    QVERIFY(!b.pop(&s));
  }

  void lengthMath() {
    QCOMPARE(RewindBuffer::snapshotsFor(0, 60), 0);
    QCOMPARE(RewindBuffer::snapshotsFor(1, 60), 15);
    QCOMPARE(RewindBuffer::snapshotsFor(30, 60), 450);
    QVERIFY(qFuzzyCompare(RewindBuffer::secondsFor(15, 60), 1.0));
    QCOMPARE(RewindBuffer::estimateBytes(10, 60, 1000), qint64{150} * 1000);
    QCOMPARE(RewindBuffer::estimateBytes(0, 60, 1000), qint64{0});
    // a big state caps the count so the memory stays bounded
    const auto lim = RewindBuffer::limitsFor(60, 60, 200ll * 1024 * 1024);
    QVERIFY(lim.first > 0 && lim.first < 900);
    QVERIFY(static_cast<qint64>(lim.first) * 200ll * 1024 * 1024 <= kRewindMaxBytes);
    QCOMPARE(RewindBuffer::limitsFor(0, 60, 1000), (std::pair<int, qint64>{0, 0}));
  }

  void runnerRewindOffByDefault() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QSignalSpy started(&runner, &EmulationRunner::started);
    runner.start(request());
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
    QTest::qWait(100);
    QVERIFY(!runner.rewindInfo().active);
    const auto r = runner.execute(Cmd::rewind(1));
    QVERIFY(!r.ok);
    QCOMPARE(r.error, QStringLiteral("empty"));
    runner.stop();
  }

  void runnerKeepsStatesAndStepsBack() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    runner.setRewindSeconds(5);
    QSignalSpy started(&runner, &EmulationRunner::started);
    runner.start(request());
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(runner.rewindInfo().snapshots >= 8, 20000);
    const auto info = runner.rewindInfo();
    QVERIFY(info.active);
    QVERIFY(info.capacity >= 75);  // 5 s at 60 fps, one state per 4 frames
    QVERIFY(info.stateBytes > 0);
    QVERIFY(info.bytes >= info.stateBytes * info.snapshots);
    runner.pause();
    QTRY_COMPARE_WITH_TIMEOUT(runner.state(), EmulationRunner::State::Paused, 5000);
    const uint32_t before = frameOf(runner);
    QVERIFY(before > 8);
    const int have = runner.rewindInfo().snapshots;
    const auto r = runner.execute(Cmd::rewind(3));
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.applied, 3);
    const uint32_t after = frameOf(runner);
    QVERIFY2(after < before, "the game must be earlier after stepping back");
    QCOMPARE(runner.rewindInfo().snapshots, have - 3);
    // asking for more than is kept steps back as far as possible
    const auto all = runner.execute(Cmd::rewind(100000));
    QVERIFY(all.ok);
    QCOMPARE(all.applied, have - 3);
    QCOMPARE(runner.execute(Cmd::rewind(1)).error, QStringLiteral("empty"));
    runner.stop();
  }

  void runnerHoldRewindPlaysBackwardsAndFinishes() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    runner.setRewindSeconds(5);
    QSignalSpy started(&runner, &EmulationRunner::started);
    QSignalSpy progress(&runner, &EmulationRunner::rewindProgress);
    QSignalSpy finished(&runner, &EmulationRunner::rewindFinished);
    runner.start(request());
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(runner.rewindInfo().snapshots >= 10, 20000);
    const int peak = runner.rewindInfo().snapshots;
    runner.setRewinding(true);
    QTRY_VERIFY_WITH_TIMEOUT(progress.count() > 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(runner.rewindInfo().snapshots < peak, 5000);
    runner.setRewinding(false);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 5000);
    QCOMPARE(finished.last().at(0).toBool(), false);  // released by the person, not run dry
    QCOMPARE(runner.state(), EmulationRunner::State::Running);
    runner.stop();
  }

  void runnerSettingOffClearsBuffer() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    runner.setRewindSeconds(5);
    QSignalSpy started(&runner, &EmulationRunner::started);
    runner.start(request());
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(runner.rewindInfo().snapshots >= 4, 20000);
    runner.setRewindSeconds(0);
    QTRY_COMPARE_WITH_TIMEOUT(runner.rewindInfo().snapshots, 0, 5000);
    runner.stop();
  }

  void runnerRunAheadKeepsRunning() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QSignalSpy started(&runner, &EmulationRunner::started);
    runner.start(request());
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
    runner.setRunAhead(2);
    QCOMPARE(runner.runAhead(), 2);
    runner.setRunAhead(9);
    QCOMPARE(runner.runAhead(), 2);  // at most two frames
    runner.setRunAhead(1);
    const uint32_t f0 = frameOf(runner);
    QTest::qWait(300);
    const uint32_t f1 = frameOf(runner);
    QVERIFY2(f1 > f0 + 5, "the game advances at about normal speed with run-ahead");
    // the state saved with run-ahead on is the real timeline: loading it gives the same game again
    runner.pause();
    QTRY_COMPARE_WITH_TIMEOUT(runner.state(), EmulationRunner::State::Paused, 5000);
    const auto saved = runner.execute(Cmd::saveState());
    QVERIFY(saved.ok);
    QVERIFY(runner.execute(Cmd::loadState(saved.state)).ok);
    runner.setRunAhead(0);
    QCOMPARE(runner.runAhead(), 0);
    runner.stop();
  }
};

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  RewindTest t;
  return QTest::qExec(&t, argc, argv);
}
#include "rewind_test.moc"
