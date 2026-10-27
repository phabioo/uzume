// VariantCache: commit, recipe round trip, size limit with LRU by last played, protection, age rule. Synthetic data only.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "variantcache.h"

using namespace uzume;

class VariantCacheTest : public QObject {
  Q_OBJECT
  static VariantRecipe recipe(const QString& id, const QString& system = QStringLiteral("nds")) {
    VariantRecipe r;
    r.variantId = id;
    r.gameId = QStringLiteral("g1");
    r.name = QStringLiteral("Randomizer #1");
    r.slot = QStringLiteral("var-12345678");
    r.system = system;
    r.toolVersion = QStringLiteral("1.6.2");
    r.settings = QStringLiteral("428abc");
    r.seed = QStringLiteral("9223372036854775807");
    r.baseSha256 = QString(64, QLatin1Char('a'));
    r.romFileName = QStringLiteral("game.nds");
    return r;
  }
  static bool build(const VariantCache& c, const VariantRecipe& r, int bytes, qint64 nowMs) {
    const QString out = c.tempDir(r.variantId) + QStringLiteral("/out");
    QDir().mkpath(out);
    QFile f(out + QLatin1Char('/') + (r.isMod() ? QStringLiteral("data.bin") : r.romFileName));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QByteArray(bytes, 'x'));
    f.close();
    return c.commit(r, nowMs);
  }

 private slots:
  void commitAndRecipe() {
    QTemporaryDir t;
    VariantCache c(t.filePath(QStringLiteral("v")));
    const VariantRecipe r = recipe(QStringLiteral("v1"));
    QVERIFY(!c.isBuilt(r.variantId));
    QVERIFY(build(c, r, 1000, 1000));
    QVERIFY(c.isBuilt(r.variantId));
    QVERIFY(QFileInfo(c.romPath(r.variantId)).isFile());
    QVERIFY(c.modDir(r.variantId).isEmpty());
    const auto back = c.recipe(r.variantId);
    QVERIFY(back.has_value());
    QCOMPARE(back->seed, r.seed);  // an int63 survives as text
    QCOMPARE(back->settings, r.settings);
    QVERIFY(!QFileInfo::exists(c.tempDir(r.variantId)));
    const VariantRecipe m = recipe(QStringLiteral("m1"), QStringLiteral("3ds"));
    QVERIFY(build(c, m, 10, 2000));
    QVERIFY(c.romPath(m.variantId).isEmpty());
    QVERIFY(QFileInfo(c.modDir(m.variantId)).isDir());
  }

  void invalidIds() {
    VariantCache c(QStringLiteral("/tmp/none"));
    QVERIFY(c.dir(QStringLiteral("../x")).isEmpty());
    QVERIFY(c.dir(QStringLiteral("a/b")).isEmpty());
    QVERIFY(!c.dir(QStringLiteral("run-abc123")).isEmpty());
  }

  void limitDropsLeastRecentlyPlayedFirstAndKeepsRecipes() {
    QTemporaryDir t;
    VariantCache c(t.filePath(QStringLiteral("v")));
    QVERIFY(build(c, recipe(QStringLiteral("a")), 4000, 1000));
    QVERIFY(build(c, recipe(QStringLiteral("b")), 4000, 2000));
    QVERIFY(build(c, recipe(QStringLiteral("c")), 4000, 3000));
    c.touch(QStringLiteral("a"), 5000);  // a is now the most recently played
    const auto res = c.trimToLimit(9000, {}, 10000);
    QCOMPARE(res.removed, 1);
    QVERIFY(c.isBuilt(QStringLiteral("a")));
    QVERIFY(!c.isBuilt(QStringLiteral("b")));  // oldest played goes first
    QVERIFY(c.isBuilt(QStringLiteral("c")));
    QVERIFY(c.recipe(QStringLiteral("b")).has_value());  // the recipe rebuilds it
    // A protected variant (running or building) is never dropped.
    const auto res2 = c.trimToLimit(1, {QStringLiteral("a"), QStringLiteral("c")}, 10000);
    QCOMPARE(res2.removed, 0);
    QVERIFY(c.isBuilt(QStringLiteral("a")));
    QCOMPARE(c.trimToLimit(0, {}, 10000).removed, 0);  // 0 = unlimited
  }

  void ageRule() {
    QTemporaryDir t;
    VariantCache c(t.filePath(QStringLiteral("v")));
    const qint64 day = 24ll * 3600 * 1000;
    QVERIFY(build(c, recipe(QStringLiteral("old")), 100, 1000));
    QVERIFY(build(c, recipe(QStringLiteral("new")), 100, 100 * day));
    QCOMPARE(c.removeUnplayed(0, {}, 120 * day).removed, 0);  // 0 = never
    const auto r = c.removeUnplayed(30, {}, 120 * day);
    QCOMPARE(r.removed, 1);
    QVERIFY(!c.isBuilt(QStringLiteral("old")));
    QVERIFY(c.isBuilt(QStringLiteral("new")));
  }

  void removeEverythingOrOnlyFiles() {
    QTemporaryDir t;
    VariantCache c(t.filePath(QStringLiteral("v")));
    QVERIFY(build(c, recipe(QStringLiteral("a")), 100, 1000));
    QVERIFY(c.remove(QStringLiteral("a"), true));
    QVERIFY(!c.isBuilt(QStringLiteral("a")));
    QVERIFY(c.recipe(QStringLiteral("a")).has_value());
    QVERIFY(c.remove(QStringLiteral("a")));
    QVERIFY(!c.recipe(QStringLiteral("a")).has_value());
  }
};

QTEST_APPLESS_MAIN(VariantCacheTest)
#include "variantcache_test.moc"
