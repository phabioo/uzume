// EngineRunner: the argument form of the engine call. A 3DS (mod) job goes through the Uzume launcher (-cp ... UzumeFvxLauncher,
// issue #414), a ROM job keeps -jar. The in-repo test engine records its arguments.
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "enginerunner.h"

using namespace uzume;

class EngineRunnerTest : public QObject {
  Q_OBJECT
  QTemporaryDir dir_;

  QStringList runAndGetArgs(bool isMod, EngineResult* res = nullptr) {
    const QString argsFile = dir_.filePath(QStringLiteral("args.txt"));
    QFile::remove(argsFile);
    qputenv("UZUME_TEST_ENGINE_ARGS_FILE", argsFile.toUtf8());
    const QString work = dir_.filePath(isMod ? QStringLiteral("work3ds") : QStringLiteral("workrom"));
    QDir().mkpath(work);
    QFile jar(work + QStringLiteral("/UPR-FVX.jar"));
    if (!jar.open(QIODevice::WriteOnly)) return {};
    jar.write("dummy");
    jar.close();
    const QString base = dir_.filePath(QStringLiteral("base.bin"));
    QFile b(base);
    if (!b.open(QIODevice::WriteOnly)) return {};
    b.write(QByteArray(1000, 'x'));
    b.close();
    EngineJob job;
    job.javaPath = QStringLiteral(FB_TEST_ENGINE_PATH);
    job.workDir = work;
    job.baseFile = base;
    job.settings = QStringLiteral("428ABC");
    job.seed = QStringLiteral("7");
    job.isMod = isMod;
    job.directoryOutput = isMod;
    job.baseBytes = 1000;
    job.heapMb = isMod ? EngineRunner::kHeapMb3ds : EngineRunner::kHeapMbOther;
    job.outPath = dir_.filePath(isMod ? QStringLiteral("out3ds") : QStringLiteral("outrom.nds"));
    EngineRunner r;
    r.setMemoryProbe([]() { return qint64(-1); });
    r.setDiskProbe([](const QString&) { return qint64(-1); });
    QSignalSpy spy(&r, &EngineRunner::finished);
    r.start(job);
    if (!spy.wait(20000)) return {};
    const EngineResult er = spy.takeFirst().at(0).value<EngineResult>();
    if (res) *res = er;
    QFile f(argsFile);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  }

 private slots:
  void initTestCase() { QVERIFY(dir_.isValid()); qRegisterMetaType<uzume::EngineResult>(); }

  void romJobKeepsDashJar() {
    EngineResult er;
    const QStringList a = runAndGetArgs(false, &er);
    QVERIFY(!a.isEmpty());
    QCOMPARE(a.at(0), QStringLiteral("-Xmx1024M"));
    QCOMPARE(a.at(1), QStringLiteral("-jar"));
    QCOMPARE(a.at(2), QStringLiteral("UPR-FVX.jar"));
    QCOMPARE(a.at(3), QStringLiteral("cli"));
    QVERIFY(!er.launcherUsed);
  }

  void modJobUsesTheLauncher() {
    EngineResult er;
    const QStringList a = runAndGetArgs(true, &er);
    QVERIFY(!a.isEmpty());
    QCOMPARE(a.at(0), QStringLiteral("-Xmx4096M"));
#ifdef UZUME_HAVE_FVX_LAUNCHER
    QVERIFY(er.launcherUsed);
    QCOMPARE(a.at(1), QStringLiteral("-cp"));
    QCOMPARE(a.at(2), QStringLiteral("UPR-FVX.jar") + QDir::listSeparator() + QStringLiteral("uzume-fvx-launcher.jar"));
    QCOMPARE(a.at(3), QStringLiteral("UzumeFvxLauncher"));
    QCOMPARE(a.at(4), QStringLiteral("cli"));
    QFile jar(dir_.filePath(QStringLiteral("work3ds/uzume-fvx-launcher.jar")));
    QVERIFY(jar.open(QIODevice::ReadOnly));
    QVERIFY(jar.readAll().startsWith("PK"));
#else
    QVERIFY(!er.launcherUsed);
    QCOMPARE(a.at(1), QStringLiteral("-jar"));
#endif
  }

  void extractionIsIdempotentAndReplacesChangedFiles() {
#ifdef UZUME_HAVE_FVX_LAUNCHER
    QTemporaryDir d;
    QVERIFY(EngineRunner::extractLauncher(d.path()));
    const QString p = d.filePath(QStringLiteral("uzume-fvx-launcher.jar"));
    QFile f(p);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray orig = f.readAll();
    f.close();
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("junk");
    f.close();
    QVERIFY(EngineRunner::extractLauncher(d.path()));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), orig);
    f.close();
    QVERIFY(!EngineRunner::extractLauncher(d.filePath(QStringLiteral("missing/dir"))));
#else
    QSKIP("launcher not compiled in");
#endif
  }
};

QTEST_GUILESS_MAIN(EngineRunnerTest)
#include "enginerunner_test.moc"
