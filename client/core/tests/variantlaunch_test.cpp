// 3DS variants: the mod folder is placed only in the variant's own slot; Main never sees it; save sync never packs it.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "sdtree.h"
#include "variantlaunch.h"

using namespace uzume;

namespace {
void put(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  f.open(QIODevice::WriteOnly);
  f.write(data);
}
}  // namespace

class VariantLaunchTest : public QObject {
  Q_OBJECT
  const QString tid_ = QStringLiteral("00040000000FB000");

 private slots:
  void titleIds() {
    QVERIFY(variantlaunch::isValidTitleId(tid_));
    QVERIFY(!variantlaunch::isValidTitleId(QStringLiteral("0004")));
    QVERIFY(!variantlaunch::isValidTitleId(QStringLiteral("../../00040000000FB0")));
  }

  void placesOnlyTheVariantsModAndRemovesOthers() {
    QTemporaryDir t;
    const QString src = t.filePath(QStringLiteral("cache/out"));
    put(src + QStringLiteral("/romfs/a/0/0/0"), "world");
    put(src + QStringLiteral("/code.bin"), "code");
    const QString variantSlot = t.filePath(QStringLiteral("c/aaaa/Azahar"));
    const QString mainSlot = t.filePath(QStringLiteral("c/bbbb/Azahar"));
    put(mainSlot + QStringLiteral("/sdmc/Nintendo 3DS/x"), "m");
    put(variantSlot + QStringLiteral("/load/mods/00040000DEADBEEF/romfs/stale"), "old");  // another mod in the variant's slot
    QString err;
    QVERIFY2(variantlaunch::placeMod(variantSlot, tid_, src, QStringLiteral("v1@1"), &err), qPrintable(err));
    QCOMPARE(QFileInfo(variantSlot + QStringLiteral("/load/mods/") + tid_ + QStringLiteral("/romfs/a/0/0/0")).size(), qint64(5));
    QVERIFY(!QFileInfo::exists(variantSlot + QStringLiteral("/load/mods/00040000DEADBEEF")));
    // The slot of Main never gets a mod.
    QVERIFY(!QFileInfo::exists(mainSlot + QStringLiteral("/load/mods")));
    QVERIFY(variantlaunch::ensureNoMod(mainSlot, tid_));
    // Same marker: not copied again; a changed marker replaces the content exactly.
    put(variantSlot + QStringLiteral("/load/mods/") + tid_ + QStringLiteral("/leftover"), "x");
    QVERIFY(variantlaunch::placeMod(variantSlot, tid_, src, QStringLiteral("v1@1")));
    QVERIFY(QFileInfo::exists(variantSlot + QStringLiteral("/load/mods/") + tid_ + QStringLiteral("/leftover")));
    QVERIFY(variantlaunch::placeMod(variantSlot, tid_, src, QStringLiteral("v1@2")));
    QVERIFY(!QFileInfo::exists(variantSlot + QStringLiteral("/load/mods/") + tid_ + QStringLiteral("/leftover")));
    // Back to a non-variant start in this very slot: the placed mod goes.
    QVERIFY(variantlaunch::ensureNoMod(variantSlot, tid_));
    QVERIFY(!QFileInfo::exists(variantSlot + QStringLiteral("/load/mods/") + tid_));
  }

  void saveSyncNeverPacksTheMod() {
    QTemporaryDir t;
    const QString root = t.filePath(QStringLiteral("Azahar"));
    const QString save = root + QStringLiteral("/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/000fb000/data/00000001.sav");
    put(save, "save");
    const QByteArray before = sdtree::pack(root).value();
    QVERIFY(!before.isEmpty());
    put(root + QStringLiteral("/load/mods/") + tid_ + QStringLiteral("/romfs/a/0/0/0"), "mod");
    put(root + QStringLiteral("/load/mods/") + tid_ + QStringLiteral("/code.bin"), "mod");
    QVERIFY(!sdtree::isManagedPath(QStringLiteral("load/mods/") + tid_ + QStringLiteral("/romfs/a/0/0/0")));
    QCOMPARE(sdtree::pack(root).value(), before);  // the archive is byte-identical with and without the mod
    QVERIFY(sdtree::fingerprint(root) == sdtree::fingerprint(root));
  }
};

QTEST_APPLESS_MAIN(VariantLaunchTest)
#include "variantlaunch_test.moc"
