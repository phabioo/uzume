// SAVE_RAM persistence of LibretroBackend with a tiny fake core (no ROM, no melonDS).
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "emulation_runner.h"
#include "libretro_backend.h"

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

using namespace uzume::emu;

namespace {
QByteArray readFile(const QString& p) {
  QFile f(p);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
void writeFile(const QString& p, const QByteArray& d) {
  QFile f(p);
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  f.write(d);
}
}  // namespace

class SaveRamTest : public QObject {
  Q_OBJECT
  QTemporaryDir dir_;
  QString saveDir() const { return dir_.filePath(QStringLiteral("save")); }
  QString save() const { return saveDir() + QStringLiteral("/game.sav"); }
  QString game() const { return dir_.filePath(QStringLiteral("game.bin")); }
  QStringList baks(const QString& pattern) const { return QDir(saveDir()).entryList({pattern}); }

  // Starts the fake core, runs n frames, unloads (which flushes).
  void play(int frames, const char* size, const char* delay, const char* write) {
    qputenv("FB_FAKE_SRAM_SIZE", size);
    qputenv("FB_FAKE_SRAM_DELAY_FRAMES", delay);
    if (write != nullptr) {
      qputenv("FB_FAKE_SRAM_WRITE", write);
    } else {
      qunsetenv("FB_FAKE_SRAM_WRITE");
    }
    LibretroBackend be;
    be.setSaveDirectory(saveDir());
    QString err;
    QVERIFY2(be.loadCore(QStringLiteral(FB_FAKE_CORE_PATH), &err), qPrintable(err));
    QVERIFY2(be.loadGame(game(), &err), qPrintable(err));
    for (int i = 0; i < frames; ++i) {
      QVERIFY(be.runFrame());
    }
    be.unloadCore();
  }

 private slots:
  void init() {
    QDir(saveDir()).removeRecursively();
    QVERIFY(QDir().mkpath(saveDir()));
    writeFile(game(), "dummy");
  }

  void writesAndReloadsSave() {
    play(3, "8", "0", "7");
    QCOMPARE(readFile(save()), QByteArray("\x07\0\0\0\0\0\0\0", 8));
    writeFile(save(), QByteArray("ABCDEFGH"));
    play(3, "8", "0", nullptr);  // existing file is loaded and written back unchanged
    QCOMPARE(readFile(save()), QByteArray("ABCDEFGH"));
  }

  void delayedSaveMemoryNeverOverwritesExistingFile() {
    writeFile(save(), QByteArray("ABCDEFGH"));
    // Save memory is missing for the first 5 frames. Unloading before it appears must not flush;
    // after it appears the file is applied first, then the game's change is written on top of it.
    play(2, "8", "5", "9");
    QCOMPARE(readFile(save()), QByteArray("ABCDEFGH"));
    play(8, "8", "5", "9");
    QCOMPARE(readFile(save()), QByteArray("\x09" "BCDEFGH", 8));
  }

  void sizeMismatchIsBackedUpBeforeWriting() {
    writeFile(save(), QByteArray("ABCDEFGHIJ"));  // 10 bytes vs 8
    play(3, "8", "0", "1");
    const QStringList b = baks(QStringLiteral("game.sav.size-mismatch-*.bak"));
    QCOMPARE(b.size(), 1);
    QCOMPARE(readFile(saveDir() + QLatin1Char('/') + b.first()), QByteArray("ABCDEFGHIJ"));
    QCOMPARE(readFile(save()).size(), 8);
  }

  // A live save is a full restart (GameSession::restartWithSave): stop flushes the old memory, the new file is written
  // afterwards, a fresh runner and backend start the game and load the new file. The old memory must not come back.
  void liveSaveIsAStopWriteStartRestart() {
    writeFile(save(), QByteArray("ABCDEFGH"));
    qputenv("FB_FAKE_SRAM_SIZE", "8");
    qputenv("FB_FAKE_SRAM_DELAY_FRAMES", "0");
    qputenv("FB_FAKE_SRAM_WRITE", "9");  // the "game" keeps writing 9 into byte 0 every frame
    EmulationRunner::StartRequest req;
    req.corePath = QStringLiteral(FB_FAKE_CORE_PATH);
    req.gamePath = game();
    req.systemDir = dir_.filePath(QStringLiteral("sys"));
    req.saveDir = saveDir();
    {
      EmulationRunner runner(std::make_unique<LibretroBackend>());
      QImage last;
      connect(&runner, &EmulationRunner::frameReady, &runner, [&last](const QImage& img, quint64) { last = img; });
      runner.start(req);
      QTRY_VERIFY_WITH_TIMEOUT(runner.state() == EmulationRunner::State::Running && !last.isNull(), 5000);
      QVERIFY(runner.saveMemoryAccepts(8));
      QVERIFY(!runner.saveMemoryAccepts(9));
      runner.stop();  // flushes the old memory ("\tBCDEFGH")
    }
    QCOMPARE(readFile(save()), QByteArray("\tBCDEFGH"));
    writeFile(save(), QByteArray("12345678"));  // written only after the stop
    {
      EmulationRunner runner(std::make_unique<LibretroBackend>());
      QImage last;
      connect(&runner, &EmulationRunner::frameReady, &runner, [&last](const QImage& img, quint64) { last = img; });
      runner.start(req);
      QTRY_VERIFY_WITH_TIMEOUT(runner.state() == EmulationRunner::State::Running && !last.isNull(), 5000);
            runner.stop();
    }
    QCOMPARE(readFile(save()), QByteArray("\t2345678"));  // only the game's own write on top of the new save
  }

  // The final write at stop can fail (disk full, file locked): the runner reports it after stop(), a good stop reports nothing.
  void failedFinalWriteIsReportedAfterStop() {
    qputenv("FB_FAKE_SRAM_SIZE", "8");
    qputenv("FB_FAKE_SRAM_DELAY_FRAMES", "0");
    qputenv("FB_FAKE_SRAM_WRITE", "9");
    EmulationRunner::StartRequest req;
    req.corePath = QStringLiteral(FB_FAKE_CORE_PATH);
    req.gamePath = game();
    req.systemDir = dir_.filePath(QStringLiteral("sys"));
    req.saveDir = saveDir();
    {
      EmulationRunner runner(std::make_unique<LibretroBackend>());
      QImage last;
      connect(&runner, &EmulationRunner::frameReady, &runner, [&last](const QImage& img, quint64) { last = img; });
      runner.start(req);
      QTRY_VERIFY_WITH_TIMEOUT(runner.state() == EmulationRunner::State::Running && !last.isNull(), 5000);
      runner.stop();
      QVERIFY(runner.finalSaveError().isEmpty());
      QVERIFY(QFile::exists(save()));
    }
    QVERIFY(QFile::remove(save()));
    {
      EmulationRunner runner(std::make_unique<LibretroBackend>());
      QImage last;
      connect(&runner, &EmulationRunner::frameReady, &runner, [&last](const QImage& img, quint64) { last = img; });
      runner.start(req);
      QTRY_VERIFY_WITH_TIMEOUT(runner.state() == EmulationRunner::State::Running && !last.isNull(), 5000);
      QDir(saveDir()).removeRecursively();  // the target directory is gone: the final write cannot succeed
      runner.stop();
      QVERIFY(runner.finalSaveError().contains(QStringLiteral("game.sav")));
    }
  }

  // PE-11: the same mismatching file found at every start is backed up once, a different content gets its own backup.
  void sizeMismatchBackupIsWrittenOncePerContent() {
    writeFile(save(), QByteArray("ABCDEFGHIJ"));
    play(3, "8", "0", "1");
    writeFile(save(), QByteArray("ABCDEFGHIJ"));  // the same 10 bytes again
    play(3, "8", "0", "1");
    QCOMPARE(baks(QStringLiteral("game.sav.size-mismatch-*.bak")).size(), 1);
    writeFile(save(), QByteArray("KLMNOPQRSTU"));  // other content
    play(3, "8", "0", "1");
    QCOMPARE(baks(QStringLiteral("game.sav.size-mismatch-*.bak")).size(), 2);
  }

  EmulationRunner::StartRequest request() const {
    EmulationRunner::StartRequest req;
    req.corePath = QStringLiteral(FB_FAKE_CORE_PATH);
    req.gamePath = game();
    req.systemDir = dir_.filePath(QStringLiteral("sys"));
    req.saveDir = saveDir();
    return req;
  }

  // PE-1: a save file that cannot be opened (here: a directory of that name) blocks saving, but the problem is reported
  // at once while the game runs, not only at stop; nothing is written over it.
  void unreadableFileIsReportedAtStart() {
    qputenv("FB_FAKE_SRAM_SIZE", "8");
    qputenv("FB_FAKE_SRAM_DELAY_FRAMES", "0");
    qputenv("FB_FAKE_SRAM_WRITE", "9");
    QVERIFY(QDir().mkpath(save()));  // game.sav is a directory
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QStringList problems;
    connect(&runner, &EmulationRunner::saveProblem, &runner, [&problems](const QString& m) { problems << m; });
    runner.start(request());
    QTRY_VERIFY_WITH_TIMEOUT(!problems.isEmpty(), 8000);  // after the retries (about 1.2 s), while still running
    QVERIFY(runner.state() == EmulationRunner::State::Running || runner.state() == EmulationRunner::State::Starting);
    QVERIFY(problems.first().contains(QStringLiteral("game.sav")));
    runner.stop();
    QVERIFY(QFileInfo(save()).isDir());  // never replaced
    QVERIFY(runner.finalSaveError().contains(QStringLiteral("game.sav")));
  }

  // PE-1: a file that is locked only for a moment is retried and then loaded normally.
  void briefLockIsRetried() {
    qputenv("FB_FAKE_SRAM_SIZE", "8");
    qputenv("FB_FAKE_SRAM_DELAY_FRAMES", "0");
    qputenv("FB_FAKE_SRAM_WRITE", "9");
    QVERIFY(QDir().mkpath(save()));
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QStringList problems;
    connect(&runner, &EmulationRunner::saveProblem, &runner, [&problems](const QString& m) { problems << m; });
    runner.start(request());
    QTest::qWait(450);  // the first open attempts failed by now
    QVERIFY(QDir().rmdir(save()));
    writeFile(save(), QByteArray("ABCDEFGH"));
    QTRY_VERIFY_WITH_TIMEOUT(runner.state() == EmulationRunner::State::Running, 8000);
    QTest::qWait(200);
    runner.stop();
    QVERIFY(problems.isEmpty());
    QVERIFY(runner.finalSaveError().isEmpty());
    QCOMPARE(readFile(save()), QByteArray("\tBCDEFGH"));  // loaded, then the game's own write on top
  }

  // PE-3: when the final write fails even after the retries, the SRAM goes to a sibling file and the error says so.
  void failedFinalWriteKeepsProgressInEmergencyFile() {
    qputenv("FB_FAKE_SRAM_SIZE", "8");
    qputenv("FB_FAKE_SRAM_DELAY_FRAMES", "0");
    qputenv("FB_FAKE_SRAM_WRITE", "9");
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QImage last;
    connect(&runner, &EmulationRunner::frameReady, &runner, [&last](const QImage& img, quint64) { last = img; });
    runner.start(request());
    QTRY_VERIFY_WITH_TIMEOUT(runner.state() == EmulationRunner::State::Running && !last.isNull(), 5000);
    QFile::remove(save());
    QVERIFY(QDir().mkpath(save()));  // the target cannot be replaced by a file any more, a sibling name still works
    runner.stop();
    const QStringList e = baks(QStringLiteral("game.sav.unsaved-*"));
    QCOMPARE(e.size(), 1);
    QCOMPARE(readFile(saveDir() + QLatin1Char('/') + e.first()), QByteArray("\x09\0\0\0\0\0\0\0", 8));
    QVERIFY(runner.finalSaveError().contains(e.first()));
    QVERIFY(QFileInfo(save()).isDir());
  }

  // PE-7: stop() does not wait for a core that hangs while unloading; the save was flushed before that.
  void stopIsBoundedWhenTheCoreHangsOnUnload() {
    qputenv("FB_FAKE_SRAM_SIZE", "8");
    qputenv("FB_FAKE_SRAM_DELAY_FRAMES", "0");
    qputenv("FB_FAKE_SRAM_WRITE", "9");
    qputenv("FB_FAKE_UNLOAD_HANG_MS", "2500");
    EmulationRunner::stopTimeoutMsForTest = 400;
    {
      EmulationRunner runner(std::make_unique<LibretroBackend>());
      QImage last;
      connect(&runner, &EmulationRunner::frameReady, &runner, [&last](const QImage& img, quint64) { last = img; });
      QSignalSpy stopped(&runner, &EmulationRunner::stopped);
      runner.start(request());
      QTRY_VERIFY_WITH_TIMEOUT(runner.state() == EmulationRunner::State::Running && !last.isNull(), 5000);
      QElapsedTimer t;
      t.start();
      runner.stop();
      QVERIFY2(t.elapsed() < 2000, "stop() waited for the hanging core");
      QCOMPARE(stopped.count(), 1);
      QVERIFY(runner.state() == EmulationRunner::State::Idle);
      QCOMPARE(readFile(save()), QByteArray("\x09\0\0\0\0\0\0\0", 8));  // flushed before the unload hung
    }
    EmulationRunner::stopTimeoutMsForTest = 0;
    qunsetenv("FB_FAKE_UNLOAD_HANG_MS");
    QTest::qWait(2600);  // let the abandoned thread finish
  }

  // #342: a core that blocks inside retro_run is abandoned by stop(); until its thread ends no core can be loaded, and the next
  // start says so with the stable code instead of the raw "already loaded" error. When the thread gets through, starting works again.
  void hungCoreBlocksTheNextStartWithAClearCode() {
    qputenv("FB_FAKE_SRAM_SIZE", "8");
    qputenv("FB_FAKE_RUN_HANG_MS", "2500");
    EmulationRunner::stopTimeoutMsForTest = 300;
    {
      EmulationRunner runner(std::make_unique<LibretroBackend>());
      QSignalSpy stopped(&runner, &EmulationRunner::stopped);
      QVERIFY(!LibretroBackend::previousCoreHung());
      runner.start(request());
      QTRY_VERIFY_WITH_TIMEOUT(runner.state() == EmulationRunner::State::Running, 5000);
      QTest::qWait(300);  // the core is inside the blocking frame now
      runner.stop();
      QCOMPARE(stopped.count(), 1);
      QVERIFY(LibretroBackend::previousCoreHung());
    }
    EmulationRunner::stopTimeoutMsForTest = 0;
    qunsetenv("FB_FAKE_RUN_HANG_MS");
    {
      EmulationRunner second(std::make_unique<LibretroBackend>());
      QSignalSpy failed(&second, &EmulationRunner::startFailed);
      second.start(request());
      QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
      QCOMPARE(failed.first().first().toString(), LibretroBackend::kPreviousGameHungCode);
      QVERIFY(LibretroBackend::previousCoreHung());
    }
    // The hung frame returns, the abandoned thread unloads the core and the process is free again.
    QTRY_VERIFY_WITH_TIMEOUT(!LibretroBackend::previousCoreHung(), 8000);
    {
      EmulationRunner third(std::make_unique<LibretroBackend>());
      QImage last;
      connect(&third, &EmulationRunner::frameReady, &third, [&last](const QImage& img, quint64) { last = img; });
      third.start(request());
      QTRY_VERIFY_WITH_TIMEOUT(third.state() == EmulationRunner::State::Running && !last.isNull(), 5000);
      third.stop();
    }
  }

  void unreadableFileIsNeverOverwritten() {
#ifdef Q_OS_WIN
    QSKIP("permissions test is POSIX only");
#else
    if (geteuid() == 0) {
      QSKIP("permissions are not enforced for root");
    }
    writeFile(save(), QByteArray("ABCDEFGH"));
    QFile::setPermissions(save(), QFileDevice::Permissions());
    play(3, "8", "0", "1");
    QFile::setPermissions(save(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QCOMPARE(readFile(save()), QByteArray("ABCDEFGH"));
#endif
  }
};

QTEST_GUILESS_MAIN(SaveRamTest)
#include "save_ram_test.moc"
