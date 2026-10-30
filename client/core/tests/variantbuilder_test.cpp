// EngineRunner + VariantBuilder against the test engine (testutil/test_engine.cpp): happy path, determinism, cancel, failure with
// filtered output, time limit, memory and disk checks, base file checks, 3DS mod folder. Synthetic data only.
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "engine_pack.h"
#include "variantbuilder.h"

using namespace uzume;
using namespace uzume::testutil;

namespace {
const QString kPlatform = QStringLiteral("linux-x64");

QString sha256File(const QString& path) {
  QFile f(path);
  f.open(QIODevice::ReadOnly);
  QCryptographicHash h(QCryptographicHash::Sha256);
  h.addData(&f);
  return QString::fromLatin1(h.result().toHex());
}
}  // namespace

class VariantBuilderTest : public QObject {
  Q_OBJECT
  QTemporaryDir dir_;
  std::unique_ptr<ToolCache> tools_;
  std::unique_ptr<VariantCache> variants_;
  QString base_;
  QString baseSha_;

  VariantRecipe recipe(const QString& id, const QString& seed = QStringLiteral("42"), const QString& settings = QStringLiteral("428SETTINGS")) const {
    VariantRecipe r;
    r.variantId = id;
    r.gameId = QStringLiteral("g1");
    r.name = QStringLiteral("Randomizer");
    r.slot = QStringLiteral("var-aaaaaaaa");
    r.system = QStringLiteral("nds");
    r.toolVersion = QStringLiteral("1.6.2");
    r.settings = settings;
    r.seed = seed;
    r.baseSha256 = baseSha_;
    return r;
  }

  BuildOutcome run(VariantBuilder& b, const VariantRecipe& r, const QString& base, bool skipCheck = false, int waitMs = 15000) {
    QSignalSpy spy(&b, &VariantBuilder::finished);
    b.start(r, base, skipCheck);
    if (spy.isEmpty() && !spy.wait(waitMs)) {
      QTest::qFail("no result", __FILE__, __LINE__);
      return {};
    }
    return spy.takeFirst().at(0).value<BuildOutcome>();
  }

 private slots:
  void initTestCase() {
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "");
    tools_ = std::make_unique<ToolCache>(dir_.filePath(QStringLiteral("tools")));
    variants_ = std::make_unique<VariantCache>(dir_.filePath(QStringLiteral("variants")));
    QByteArray bytes;
    const QString z = writeEngineZip(dir_.path(), QStringLiteral(FB_TEST_ENGINE_PATH), QStringLiteral("UPR_FVX-v1_6_2-Test"), false, &bytes);
    QVERIFY(!z.isEmpty());
    QCOMPARE(tools_->installZip(QStringLiteral("randomizer"), QStringLiteral("1.6.2"), kPlatform, z, sha256Hex(bytes)), ToolCache::InstallResult::Ok);
    base_ = dir_.filePath(QStringLiteral("base.nds"));
    QByteArray data;
    for (int i = 0; i < 200000; ++i) data.append(static_cast<char>((i * 31 + 7) % 256));
    QFile f(base_);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
    f.close();
    baseSha_ = sha256Hex(data);
  }

  void init() { qputenv("UZUME_TEST_ENGINE_DELAY_MS", ""); }

  void happyPathAndDeterminism() {
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    const BuildOutcome o1 = run(b, recipe(QStringLiteral("v1")), base_);
    QVERIFY2(o1.ok, qPrintable(o1.error));
    QVERIFY(variants_->isBuilt(QStringLiteral("v1")));
    const QString rom1 = variants_->romPath(QStringLiteral("v1"));
    QVERIFY(QFileInfo(rom1).isFile());
    QCOMPARE(QFileInfo(rom1).size(), QFileInfo(base_).size());
    QVERIFY(sha256File(rom1) != baseSha_);  // the base file itself is untouched
    QCOMPARE(sha256File(base_), baseSha_);
    QVERIFY(!QFileInfo::exists(variants_->tempDir(QStringLiteral("v1"))));
    QVERIFY(tools_->isProtected(QStringLiteral("randomizer"), QStringLiteral("1.6.2")) == false);

    const BuildOutcome o2 = run(b, recipe(QStringLiteral("v2")), base_);
    QVERIFY(o2.ok);
    QCOMPARE(sha256File(variants_->romPath(QStringLiteral("v2"))), sha256File(rom1));  // same recipe, same bytes
    QVERIFY(run(b, recipe(QStringLiteral("v3"), QStringLiteral("43")), base_).ok);
    QVERIFY(sha256File(variants_->romPath(QStringLiteral("v3"))) != sha256File(rom1));  // another seed, another world
    QVERIFY(run(b, recipe(QStringLiteral("v4"), QStringLiteral("42"), QStringLiteral("428OTHER")), base_).ok);
    QVERIFY(sha256File(variants_->romPath(QStringLiteral("v4"))) != sha256File(rom1));
  }

  void progressIsReportedAndToolIsProtectedWhileRunning() {
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "700");
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    QSignalSpy prog(&b, &VariantBuilder::progress);
    QSignalSpy done(&b, &VariantBuilder::finished);
    b.start(recipe(QStringLiteral("vp")), base_);
    QVERIFY(tools_->isProtected(QStringLiteral("randomizer"), QStringLiteral("1.6.2")));
    QVERIFY(done.wait(15000));
    QVERIFY(prog.count() >= 2);
    QStringList steps;
    for (const auto& a : prog) steps << a.at(0).value<BuildProgress>().step;
    QVERIFY(steps.contains(QStringLiteral("reading")));
    QVERIFY(steps.contains(QStringLiteral("randomizing")));
    QVERIFY(!tools_->isProtected(QStringLiteral("randomizer"), QStringLiteral("1.6.2")));
  }

  void cancelKillsAndKeepsNothing() {
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "20000");
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    QSignalSpy done(&b, &VariantBuilder::finished);
    b.start(recipe(QStringLiteral("vc")), base_);
    QTest::qWait(500);
    b.cancel();
    QVERIFY(done.wait(8000));
    const BuildOutcome o = done.takeFirst().at(0).value<BuildOutcome>();
    QCOMPARE(o.error, QStringLiteral("cancelled"));
    QVERIFY(!variants_->isBuilt(QStringLiteral("vc")));
    QVERIFY(!QFileInfo::exists(variants_->tempDir(QStringLiteral("vc"))));
    QVERIFY(!b.running());
  }

  void failureKeepsOnlyErrorAndWarningLines() {
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    const BuildOutcome o = run(b, recipe(QStringLiteral("vf"), QStringLiteral("1"), QStringLiteral("999FAIL")), base_);
    QVERIFY(!o.ok);
    QCOMPARE(o.error, QStringLiteral("engine_failed"));
    QCOMPARE(o.exitCode, 3);
    const QString all = o.log.join(QLatin1Char('\n'));
    QVERIFY(all.contains(QStringLiteral("ERROR: synthetic failure")));
    QVERIFY(all.contains(QStringLiteral("WARNING: synthetic warning")));
    QVERIFY(all.contains(QStringLiteral("exit code 3")));
    QVERIFY(!all.contains(QStringLiteral("SYNTHETICMON")));  // every other engine line is dropped
    // The first ERROR line reaches the outcome; absolute paths shrink to the file name (they can carry the user name).
    QCOMPARE(o.engineError, QStringLiteral("synthetic failure while writing"));
    QVERIFY(all.contains(QStringLiteral("ERROR: Could not load abc.3ds; SYNTHETIC_REASON")));
    QVERIFY(!all.contains(QStringLiteral("Users")));
    QVERIFY(!variants_->isBuilt(QStringLiteral("vf")));
    QVERIFY(!QFileInfo::exists(variants_->tempDir(QStringLiteral("vf"))));
  }

  void timeLimit() {
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "20000");
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    b.setTimeLimitMs(400);
    const BuildOutcome o = run(b, recipe(QStringLiteral("vt")), base_);
    QCOMPARE(o.error, QStringLiteral("timed_out"));
    QVERIFY(!QFileInfo::exists(variants_->tempDir(QStringLiteral("vt"))));
  }

  void baseFileChecks() {
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    QCOMPARE(run(b, recipe(QStringLiteral("vb")), dir_.filePath(QStringLiteral("missing.nds"))).error, QStringLiteral("base_missing"));
    // A different dump: the recipe pins another SHA-256.
    VariantRecipe r = recipe(QStringLiteral("vb"));
    r.baseSha256 = QString(64, QLatin1Char('1'));
    const BuildOutcome o = run(b, r, base_);
    QCOMPARE(o.error, QStringLiteral("base_mismatch"));
    QCOMPARE(o.expectedBaseSha, r.baseSha256);
    QCOMPARE(o.actualBaseSha, baseSha_);
    QVERIFY(!variants_->isBuilt(QStringLiteral("vb")));
    // "Build anyway" and an unpinned recipe both build.
    QVERIFY(run(b, r, base_, true).ok);
    r.baseSha256.clear();
    QVERIFY(run(b, r, base_).ok);
  }

  void engineMissing() {
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    VariantRecipe r = recipe(QStringLiteral("ve"));
    r.toolVersion = QStringLiteral("9.9.9");
    QCOMPARE(run(b, r, base_).error, QStringLiteral("engine_missing"));
  }

  void memoryAndDiskChecks() {
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    b.runner()->setMemoryProbe([]() { return qint64(1200) * 1024 * 1024; });
    VariantRecipe r = recipe(QStringLiteral("vm"));
    r.system = QStringLiteral("3ds");  // 4 GB heap: wants 3 GB free
    BuildOutcome o = run(b, r, base_);
    QCOMPARE(o.error, QStringLiteral("not_enough_memory"));
    QCOMPARE(o.neededBytes, qint64(3) * 1024 * 1024 * 1024);
    QCOMPARE(o.freeBytes, qint64(1200) * 1024 * 1024);
    b.runner()->setMemoryProbe([]() { return qint64(64) * 1024 * 1024 * 1024; });
    b.runner()->setDiskProbe([](const QString&) { return qint64(1000); });
    o = run(b, recipe(QStringLiteral("vd")), base_);
    QCOMPARE(o.error, QStringLiteral("disk_full"));
    QVERIFY(o.neededBytes > o.freeBytes);
    QVERIFY(!QFileInfo::exists(variants_->tempDir(QStringLiteral("vd"))));
  }

  void modBuildFindsTheLayoutAndKeepsSpoilerLogOnRequest() {
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    VariantRecipe r = recipe(QStringLiteral("vm3"));
    r.system = QStringLiteral("3ds");
    r.titleId = QStringLiteral("00040000000FB000");
    r.baseSha256.clear();
    r.spoilerLog = true;
    const BuildOutcome o = run(b, r, base_);
    QVERIFY2(o.ok, qPrintable(o.error));
    QVERIFY(o.layoutNote.contains(QStringLiteral("romfs")));
    QVERIFY(QFileInfo(variants_->modDir(QStringLiteral("vm3")) + QStringLiteral("/romfs/a/0/0/0")).isFile());
    QVERIFY(QFileInfo(variants_->modDir(QStringLiteral("vm3")) + QStringLiteral("/code.bin")).isFile());
    QVERIFY(QFileInfo(variants_->spoilerLogPath(QStringLiteral("vm3"))).isFile());
    VariantRecipe r2 = r;
    r2.variantId = QStringLiteral("vm4");
    r2.spoilerLog = false;
    QVERIFY(run(b, r2, base_).ok);
    QVERIFY(variants_->spoilerLogPath(QStringLiteral("vm4")).isEmpty());
  }

  // FVX appends the default extension to -o (red.gb -> red.gb.gbc) and the log follows the real name: the builder takes the one
  // produced file and stores it under the base file's own name.
  void engineAppendedExtensionIsHandled() {
    const QString gb = dir_.filePath(QStringLiteral("red.gb"));
    QVERIFY(QFile::copy(base_, gb));
    VariantBuilder b(tools_.get(), variants_.get(), kPlatform);
    VariantRecipe r = recipe(QStringLiteral("vgb1"));
    r.system = QStringLiteral("gb");
    r.baseSha256.clear();
    r.spoilerLog = true;
    const BuildOutcome o = run(b, r, gb);
    QVERIFY2(o.ok, qPrintable(o.error));
    const QString rom = variants_->romPath(QStringLiteral("vgb1"));
    QCOMPARE(QFileInfo(rom).fileName(), QStringLiteral("red.gb"));
    QCOMPARE(QFileInfo(rom).size(), QFileInfo(gb).size());
    QVERIFY(sha256File(rom) != sha256File(gb));
    QVERIFY(QFileInfo(variants_->spoilerLogPath(QStringLiteral("vgb1"))).isFile());
  }

  void estimateGrowsWithSize() {
    QVERIFY(EngineRunner::estimateMs(true, 1800ll << 20) > EngineRunner::estimateMs(false, 128ll << 20));
    QCOMPARE(EngineRunner::memoryNeededBytes(EngineRunner::kHeapMb3ds), qint64(3) * 1024 * 1024 * 1024);
  }
};

QTEST_GUILESS_MAIN(VariantBuilderTest)
#include "variantbuilder_test.moc"
