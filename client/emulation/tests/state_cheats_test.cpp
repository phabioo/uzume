// Save states, cheats and the emulation command queue against the Uzume test core (real retro_serialize*, cheat format
// "raw"): backend level, then EmulationRunner (commands applied between frames, results with request ids). Never skips.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "emulation_runner.h"
#include "libretro_backend.h"
#include "test_core_layout.h"

using namespace uzume::emu;
namespace tc = uzume::testcore;
namespace mem = uzume::testcore::mem;
using Cmd = EmulationRunner::Command;
using CheatEntry = EmulationRunner::CheatEntry;

namespace {
QByteArray hashOf(const QImage& img) {
  QCryptographicHash h(QCryptographicHash::Sha256);
  for (int y = 0; y < img.height(); ++y) h.addData(QByteArrayView(reinterpret_cast<const char*>(img.constScanLine(y)), img.width() * 4));
  return h.result().toHex();
}
uint8_t peek(EmulatorBackend& be, uint64_t addr) {
  const MemoryMap* mm = be.memoryMap();
  const auto sp = mm ? mm->resolve(addr) : MemoryMap::Span{};
  return sp.ptr ? *sp.ptr : 0xEE;
}
uint32_t peek32(EmulatorBackend& be, uint64_t addr) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | peek(be, addr + static_cast<uint64_t>(i));
  return v;
}
CheatEntry cheat(const QString& code, bool enabled = true) { return CheatEntry{QStringLiteral("c"), QStringLiteral("raw"), code, enabled}; }
}  // namespace

class StateCheatsTest : public QObject {
  Q_OBJECT
  QTemporaryDir m_dir;
  QString m_core = qEnvironmentVariable("UZUME_MELONDS_DS_CORE");
  QString game() const { return QStringLiteral(FB_TEST_ROM_PATH); }
  bool startBackend(LibretroBackend& be) {
    be.setSystemDirectory(m_dir.filePath(QStringLiteral("system")));
    be.setSaveDirectory(m_dir.filePath(QStringLiteral("save")));
    QString err;
    const bool ok = be.loadCore(m_core, &err) && be.loadGame(game(), &err);
    if (!ok) qWarning() << err;
    return ok;
  }
  static void run(LibretroBackend& be, int n) {
    for (int i = 0; i < n; ++i) QVERIFY(be.runFrame());
  }
  EmulationRunner::StartRequest request() const {
    EmulationRunner::StartRequest req;
    req.corePath = m_core;
    req.gamePath = game();
    req.systemDir = m_dir.filePath(QStringLiteral("system"));
    req.saveDir = m_dir.filePath(QStringLiteral("save"));
    return req;
  }
  // Starts the runner and leaves it paused.
  void startPaused(EmulationRunner& runner) {
    QSignalSpy started(&runner, &EmulationRunner::started);
    runner.start(request());
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
    runner.pause();
    QTRY_COMPARE_WITH_TIMEOUT(runner.state(), EmulationRunner::State::Paused, 5000);
  }
  // Frame image and a byte, read on the emulation thread.
  static QImage lastFrame(EmulationRunner& runner) {
    QImage img;
    runner.runOnEmuThread([&](EmulatorBackend& be) { img = be.videoFrame(); });
    return img;
  }
  static uint32_t read32(EmulationRunner& runner, uint64_t addr) {
    uint32_t v = 0;
    runner.runOnEmuThread([&](EmulatorBackend& be) { v = peek32(be, addr); });
    return v;
  }
  static uint8_t read8(EmulationRunner& runner, uint64_t addr) {
    uint8_t v = 0;
    runner.runOnEmuThread([&](EmulatorBackend& be) { v = peek(be, addr); });
    return v;
  }

 private slots:
  void initTestCase() {
    QVERIFY2(!m_core.isEmpty(), "UZUME_MELONDS_DS_CORE must point to the test core");
    qunsetenv("FB_TEST_CORE_LATE_MAP_FRAME");
  }

  // ---- backend
  void backendStateRoundTrip() {
    LibretroBackend be;
    QVERIFY(startBackend(be));
    QVERIFY(be.supportsStates());
    run(be, 20);
    QByteArray s1, s1b;
    QVERIFY(be.serialize(&s1));
    QVERIFY(be.serialize(&s1b));
    QCOMPARE(s1, s1b);  // serializing twice in a row yields the same bytes
    QCOMPARE(static_cast<qint64>(s1.size()), be.serializeSize());
    run(be, 10);
    const QByteArray h1 = hashOf(be.videoFrame());
    const quint64 f1 = be.frameCount();
    QVERIFY(be.unserialize(s1));
    run(be, 10);
    QCOMPARE(hashOf(be.videoFrame()), h1);  // deterministic: same frames after a load
    QVERIFY(be.frameCount() > f1);          // the app's frame counter keeps counting
    QByteArray s2;
    QVERIFY(be.serialize(&s2));
    QVERIFY(!be.unserialize(QByteArray("garbage")));
    QVERIFY(!be.unserialize(QByteArray()));
    be.unloadGame();
    be.unloadCore();
  }

  void backendCheats() {
    LibretroBackend be;
    QVERIFY(startBackend(be));
    QVERIFY(be.supportsCheats());
    const uint64_t a = mem::kHeapAddr + 0x100;
    run(be, 2);
    QCOMPARE(peek(be, a), uint8_t{0});
    be.cheatSet(0, true, QStringLiteral("08000100 5A+08000101 5B"));
    run(be, 1);
    QCOMPARE(peek(be, a), uint8_t{0x5A});
    QCOMPARE(peek(be, a + 1), uint8_t{0x5B});
    QCOMPARE(peek32(be, mem::kHeapAddr + mem::kCheatCountOff), 1u);
    be.cheatSet(1, true, QStringLiteral("14000010 77\n14000011 78"));  // newline separated, linear region
    run(be, 1);
    QCOMPARE(peek(be, mem::kLinearAddr + 0x11), uint8_t{0x78});
    be.cheatSet(0, false, QStringLiteral("08000100 5A"));
    *const_cast<uint8_t*>(be.memoryMap()->resolve(a).ptr) = 0;
    run(be, 1);
    QCOMPARE(peek(be, a), uint8_t{0});  // disabled entry writes nothing
    QCOMPARE(peek32(be, mem::kHeapAddr + mem::kCheatCountOff), 1u);
    be.cheatReset();
    run(be, 1);
    QCOMPARE(peek32(be, mem::kHeapAddr + mem::kCheatCountOff), 0u);
    be.unloadGame();
    be.unloadCore();
  }

  // Spike numbers (notes/0.20/spike-serialize-cheats.md): size and time of serialize/unserialize, determinism over 600 frames.
  void spikeMeasure() {
    LibretroBackend be;
    QVERIFY(startBackend(be));
    run(be, 30);
    constexpr int kRounds = 200;
    QByteArray s;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < kRounds; ++i) QVERIFY(be.serialize(&s));
    const double serMs = static_cast<double>(t.nsecsElapsed()) / 1e6 / kRounds;
    t.restart();
    for (int i = 0; i < kRounds; ++i) QVERIFY(be.unserialize(s));
    const double unserMs = static_cast<double>(t.nsecsElapsed()) / 1e6 / kRounds;
    run(be, 600);
    const QByteArray endHash = hashOf(be.videoFrame());
    QVERIFY(be.unserialize(s));
    run(be, 600);
    QCOMPARE(hashOf(be.videoFrame()), endHash);  // 600 frames after a load match the original run
    qInfo().noquote() << QStringLiteral("SPIKE test core: state %1 bytes, serialize %2 ms, unserialize %3 ms (avg of %4)")
                             .arg(s.size()).arg(serMs, 0, 'f', 3).arg(unserMs, 0, 'f', 3).arg(kRounds);
    be.unloadGame();
    be.unloadCore();
  }

  // ---- runner
  void runnerSaveLoadDeterministic() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    startPaused(runner);
    QVERIFY(runner.stepFrames(5) > 0);
    const auto saved = runner.execute(Cmd::saveState());
    QVERIFY2(saved.ok, qPrintable(saved.error));
    QVERIFY(saved.state.size() > 1000);
    QVERIFY(!saved.preview.isNull());
    QCOMPARE(saved.preview.format(), QImage::Format_RGB32);
    QVERIFY(saved.frame >= 5);
    QVERIFY(runner.stepFrames(12) > 0);
    const QByteArray h1 = hashOf(lastFrame(runner));
    const auto loaded = runner.execute(Cmd::loadState(saved.state));
    QVERIFY2(loaded.ok, qPrintable(loaded.error));
    QVERIFY(runner.stepFrames(12) > 0);
    QCOMPARE(hashOf(lastFrame(runner)), h1);
    QVERIFY(!runner.execute(Cmd::loadState(QByteArray("garbage"))).ok);
    QCOMPARE(runner.execute(Cmd::loadState(QByteArray())).error, QStringLiteral("invalid"));
    runner.stop();
  }

  void runnerCheatsAppliedAndReappliedAfterLoad() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    startPaused(runner);
    const uint64_t a = mem::kHeapAddr + 0x200;
    const uint64_t countAddr = mem::kHeapAddr + mem::kCheatCountOff;
    QVERIFY(runner.stepFrames(2) > 0);
    const auto before = runner.execute(Cmd::saveState());  // no cheat active yet
    QVERIFY(before.ok);
    const auto set = runner.execute(Cmd::setCheats({cheat(QStringLiteral("08000200 AB")), cheat(QStringLiteral("08000201 CD"), false),
                                                    cheat(QStringLiteral("  "))}));
    QVERIFY(set.ok);
    QCOMPARE(set.applied, 1);  // only the enabled one with a code
    QCOMPARE(runner.activeCheats().size(), 3);
    QVERIFY(runner.stepFrames(1) > 0);
    QCOMPARE(read8(runner, a), uint8_t{0xAB});
    QCOMPARE(read8(runner, a + 1), uint8_t{0});
    QCOMPARE(read32(runner, countAddr), 1u);
    // The test core drops its cheat list on unserialize (like a core that rebuilds its engine): the runner re-applies it.
    QVERIFY(runner.execute(Cmd::loadState(before.state)).ok);
    QCOMPARE(read8(runner, a), uint8_t{0});  // the loaded RAM, before the next frame
    QVERIFY(runner.stepFrames(1) > 0);
    QCOMPARE(read8(runner, a), uint8_t{0xAB});
    QCOMPARE(read32(runner, countAddr), 1u);
    // Clearing the list stops the writes.
    QVERIFY(runner.execute(Cmd::setCheats({})).ok);
    QVERIFY(runner.activeCheats().isEmpty());
    QVERIFY(runner.stepFrames(1) > 0);
    QCOMPARE(read32(runner, countAddr), 0u);
    runner.stop();
  }

  void runnerCheatsReappliedAfterReset() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    startPaused(runner);
    QVERIFY(runner.execute(Cmd::setCheats({cheat(QStringLiteral("08000300 11"))})).ok);
    QVERIFY(runner.stepFrames(2) > 0);
    QCOMPARE(read32(runner, mem::kHeapAddr + mem::kCheatCountOff), 1u);
    runner.reset();
    runner.resume();
    // The first frame after the reset shows resets == 1; the core dropped its cheats in retro_reset, so a count of 1 in that
    // frame proves the runner re-sent the list before running it.
    bool seen = false;
    for (int i = 0; i < 400 && !seen; ++i) {
      QTest::qWait(10);
      runner.runOnEmuThread([&](EmulatorBackend& be) {
        const QImage f = be.videoFrame();
        if (f.isNull() || (f.pixel(tc::DataResets, tc::kDataRow) & 0xFFFFFFu) != 1u) return;
        seen = true;
        QCOMPARE(peek32(be, mem::kHeapAddr + mem::kCheatCountOff), 1u);
        QCOMPARE(peek(be, mem::kHeapAddr + 0x300), uint8_t{0x11});
      });
    }
    QVERIFY(seen);
    runner.stop();
  }

  void runnerMemoryWrite() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    startPaused(runner);
    const uint64_t a = mem::kHeapAddr + 0x400;
    const auto ok = runner.execute(Cmd::writeMemory(a, QByteArray::fromHex("01020304")));
    QVERIFY2(ok.ok, qPrintable(ok.error));
    QCOMPARE(read32(runner, a), 0x04030201u);
    // Unmapped address and a range that runs off the end of a region: error, nothing written.
    QCOMPARE(runner.execute(Cmd::writeMemory(0x0F000000, QByteArray(4, 'x'))).error, QStringLiteral("unmapped"));
    const uint64_t edge = mem::kHeapAddr + mem::kRegionSize - 2;
    QVERIFY(runner.execute(Cmd::writeMemory(edge, QByteArray::fromHex("FFFF"))).ok);
    const auto over = runner.execute(Cmd::writeMemory(edge, QByteArray::fromHex("11223344")));
    QCOMPARE(over.error, QStringLiteral("unmapped"));
    QCOMPARE(read8(runner, edge), uint8_t{0xFF});  // untouched
    QCOMPARE(runner.execute(Cmd::writeMemory(a, QByteArray())).error, QStringLiteral("invalid"));
    runner.stop();
  }

  void runnerCommandOrderAndSignal() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QSignalSpy finished(&runner, &EmulationRunner::commandFinished);
    startPaused(runner);
    const uint64_t a = mem::kHeapAddr + 0x500;
    const quint64 id1 = runner.submit(Cmd::writeMemory(a, QByteArray::fromHex("AA")));
    const quint64 id2 = runner.submit(Cmd::saveState());
    const quint64 id3 = runner.submit(Cmd::writeMemory(a, QByteArray::fromHex("BB")));
    const quint64 id4 = runner.submit(Cmd::loadState(QByteArray("nope")));
    QVERIFY(id1 > 0 && id2 > id1 && id3 > id2 && id4 > id3);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 5000);
    QCOMPARE(finished.at(0).at(0).value<EmulationRunner::CommandResult>().requestId, id1);
    const auto r2 = finished.at(1).at(0).value<EmulationRunner::CommandResult>();
    QCOMPARE(r2.requestId, id2);
    QVERIFY(r2.ok);
    QCOMPARE(r2.kind, Cmd::Kind::SaveState);
    QVERIFY(!finished.at(3).at(0).value<EmulationRunner::CommandResult>().ok);
    // Submit order: the state saved by command 2 holds the first write, the live RAM the second.
    QCOMPARE(read8(runner, a), uint8_t{0xBB});
    QVERIFY(runner.execute(Cmd::loadState(r2.state)).ok);
    QCOMPARE(read8(runner, a), uint8_t{0xAA});
    runner.stop();
    // Not running: nothing is queued.
    QCOMPARE(runner.submit(Cmd::saveState()), quint64{0});
    QCOMPARE(runner.execute(Cmd::saveState()).error, QStringLiteral("stopped"));
  }

  void runnerCommandsWhileRunning() {
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QSignalSpy started(&runner, &EmulationRunner::started);
    runner.start(request());
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 20000);
    const auto saved = runner.execute(Cmd::saveState());
    QVERIFY2(saved.ok, qPrintable(saved.error));
    QTest::qWait(100);
    const auto loaded = runner.execute(Cmd::loadState(saved.state));
    QVERIFY2(loaded.ok, qPrintable(loaded.error));
    QVERIFY(runner.execute(Cmd::writeMemory(mem::kHeapAddr + 0x600, QByteArray::fromHex("42"))).ok);
    QCOMPARE(runner.state(), EmulationRunner::State::Running);
    runner.stop();
  }
};

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  StateCheatsTest t;
  return QTest::qExec(&t, argc, argv);
}
#include "state_cheats_test.moc"
