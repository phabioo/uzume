// LocalStorage: measurement of the app data folders and the clean-up selection, with dummy files in a temp dir (no real data).
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "localstorage.h"
#include "statestore.h"

using namespace uzume;

namespace {
constexpr qint64 kDay = 24ll * 60 * 60 * 1000;

void writeBytes(const QString& path, int size, char fill = 'x') {
  QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QByteArray(size, fill));
}
QString shaOf(const QByteArray& d) { return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex()); }

QString addRom(RomCache& c, char fill, int size, qint64 lastUsedMs) {
  const QByteArray d(size, fill);
  const QString sha = shaOf(d);
  writeBytes(c.partPath(sha, QStringLiteral("nds")), size, fill);
  if (c.verifyAndCommit(sha, QStringLiteral("nds")) != RomCache::CommitResult::Ok) return {};
  c.touch(sha, QStringLiteral("nds"), lastUsedMs);
  return sha;
}

void addCore(const CoreCache& c, const QString& core, const QString& version) {
  const QByteArray lib(300, 'l');
  CorePackageInfo p;
  p.coreId = core;
  p.version = version;
  p.platform = CoreCache::currentPlatform();
  p.files.append({QStringLiteral("dummy_libretro.so"), QStringLiteral("library"), lib.size(), shaOf(lib), true});
  QCOMPARE(c.store(p, p.files.at(0), lib), CoreCache::StoreResult::Ok);
  QVERIFY(c.writePackage(p));
}
}  // namespace

class LocalStorageTest : public QObject {
  Q_OBJECT
 private slots:
  void measuresEveryKindOfTheDataFolder() {
    QTemporaryDir dir;
    const QString data = dir.path();
    LocalStorageInput in;
    in.dataDir = data;
    in.firmwareDir = QDir(data).filePath(QStringLiteral("system/firmware"));
    in.roms = RomCache(QDir(data).filePath(QStringLiteral("cache/roms")));
    in.cores = CoreCache(QDir(data).filePath(QStringLiteral("cache/cores")));
    in.platform = CoreCache::currentPlatform();
    in.nowMs = 1000 * kDay;
    addRom(in.roms, 'a', 1000, in.nowMs);
    addRom(in.roms, 'b', 2000, in.nowMs);
    writeBytes(data + QStringLiteral("/hubs/h1/users/u1/saves/g1/game.sav"), 400);
    writeBytes(data + QStringLiteral("/hubs/h1/users/u1/saves/g1/slots/two/game.sav"), 100);
    writeBytes(data + QStringLiteral("/hubs/h1/saves/legacy.sav"), 50);
    writeBytes(data + QStringLiteral("/c/0123456789abcdef/core.sav"), 25);
    writeBytes(data + QStringLiteral("/screenshots/pending/a.png"), 300);
    writeBytes(data + QStringLiteral("/screenshots/pending/b.png"), 200);
    writeBytes(in.firmwareDir + QStringLiteral("/nds/abc"), 700);
    addCore(in.cores, QStringLiteral("melonds_ds"), QStringLiteral("2026.10.09"));

    const LocalStorageReport r = LocalStorage::measure(in);
    QCOMPARE(r.gamesBytes, qint64(3000));
    QCOMPARE(r.gamesCount, 2);
    QCOMPARE(r.savesBytes, qint64(400 + 100 + 50 + 25));
    QCOMPARE(r.statesBytes, qint64(0));
    QCOMPARE(r.screenshotsBytes, qint64(500));
    QCOMPARE(r.screenshotsCount, 2);
    QVERIFY(r.coresBytes >= 700 + 300);
    QVERIFY(r.totalBytes > 0);
    QVERIFY(r.freeBytes >= 0);
    QVERIFY(r.measuredAtMs > 0);
    QCOMPARE(r.oldGamesCount, 0);
    QCOMPARE(r.staleCoresCount, 0);
    QCOMPARE(r.usedBytes(), r.gamesBytes + r.savesBytes + r.screenshotsBytes + r.coresBytes);
    // An empty folder measures as zero and does not fail.
    LocalStorageInput empty;
    empty.dataDir = data + QStringLiteral("/nothing");
    empty.roms = RomCache(empty.dataDir + QStringLiteral("/r"));
    empty.cores = CoreCache(empty.dataDir + QStringLiteral("/c"));
    QCOMPARE(LocalStorage::measure(empty).usedBytes(), qint64(0));
  }

  void saveStatesAreMeasuredSeparatelyAndOrphansOfDeletedGamesFound() {
    QTemporaryDir dir;
    const QString data = dir.path();
    LocalStorageInput in;
    in.dataDir = data;
    in.firmwareDir = QDir(data).filePath(QStringLiteral("system/firmware"));
    in.roms = RomCache(QDir(data).filePath(QStringLiteral("cache/roms")));
    in.cores = CoreCache(QDir(data).filePath(QStringLiteral("cache/cores")));
    in.platform = CoreCache::currentPlatform();
    in.nowMs = 1000 * kDay;
    writeBytes(data + QStringLiteral("/hubs/h1/users/u1/saves/g1/game.sav"), 400);
    const QString states = data + QStringLiteral("/hubs/h1/users/u1/states");
    StateStore store(states);
    StateRecord a;
    a.gameId = QStringLiteral("g1");
    a.n = 1;
    StateRecord b;
    b.gameId = QStringLiteral("deleted");
    b.n = 2;
    QVERIFY(store.put(a, QByteArray(1000, 'a')));
    QVERIFY(store.put(b, QByteArray(3000, 'b')));
    in.statesDir = states;
    in.libraryGameIds = {QStringLiteral("g1")};
    in.libraryKnown = true;
    LocalStorageReport r = LocalStorage::measure(in);
    QVERIFY(r.statesBytes >= 4000);
    QCOMPARE(r.savesBytes, qint64(400));  // states are not saves
    QCOMPARE(r.orphanStatesGames, 1);
    QVERIFY(r.orphanStatesBytes >= 3000 && r.orphanStatesBytes < r.statesBytes);
    QVERIFY(r.usedBytes() >= r.savesBytes + r.statesBytes);
    // library not loaded yet: nothing is offered for clean-up
    in.libraryKnown = false;
    r = LocalStorage::measure(in);
    QCOMPARE(r.orphanStatesGames, 0);
    QCOMPARE(r.orphanStatesBytes, qint64(0));
    QVERIFY(r.statesBytes >= 4000);
  }

  void gamesNotPlayedForSixtyDaysAreSuggestedButNeverTheRunningOne() {
    QTemporaryDir dir;
    LocalStorageInput in;
    in.dataDir = dir.path();
    in.roms = RomCache(QDir(dir.path()).filePath(QStringLiteral("roms")));
    in.cores = CoreCache(QDir(dir.path()).filePath(QStringLiteral("cores")));
    in.nowMs = 1000 * kDay;
    addRom(in.roms, 'a', 1000, in.nowMs - 61 * kDay);
    const QString running = addRom(in.roms, 'b', 2000, in.nowMs - 120 * kDay);
    addRom(in.roms, 'c', 4000, in.nowMs - 10 * kDay);
    QCOMPARE(LocalStorage::measure(in).oldGamesCount, 2);
    QCOMPARE(LocalStorage::measure(in).oldGamesBytes, qint64(3000));
    in.protectedHashes = {running};
    const LocalStorageReport r = LocalStorage::measure(in);
    QCOMPARE(r.oldGamesCount, 1);
    QCOMPARE(r.oldGamesBytes, qint64(1000));
    // The clean-up removes exactly what was suggested and leaves saves alone.
    writeBytes(QDir(dir.path()).filePath(QStringLiteral("hubs/h/users/u/saves/g/a.sav")), 77);
    const auto res = in.roms.removeUnusedSince(LocalStorage::unusedGamesCutoffMs(in.nowMs), in.protectedHashes);
    QCOMPARE(res.removedFiles, 1);
    QVERIFY(QFile::exists(in.roms.finalPath(running, QStringLiteral("nds"))));
    const LocalStorageReport after = LocalStorage::measure(in);
    QCOMPARE(after.oldGamesCount, 0);
    QCOMPARE(after.savesBytes, qint64(77));
    QCOMPARE(LocalStorage::unusedGamesCutoffMs(1000 * kDay), (1000 - 60) * kDay);
  }

  void unusedCoresAreOlderVersionsButNotTheRunningGamesCore() {
    QTemporaryDir dir;
    LocalStorageInput in;
    in.dataDir = dir.path();
    in.roms = RomCache(QDir(dir.path()).filePath(QStringLiteral("roms")));
    in.cores = CoreCache(QDir(dir.path()).filePath(QStringLiteral("cores")));
    in.platform = CoreCache::currentPlatform();
    addCore(in.cores, QStringLiteral("melonds_ds"), QStringLiteral("2026.09.01"));
    addCore(in.cores, QStringLiteral("melonds_ds"), QStringLiteral("2026.10.09"));
    addCore(in.cores, QStringLiteral("mgba"), QStringLiteral("0.10.0"));
    LocalStorageReport r = LocalStorage::measure(in);
    QCOMPARE(r.staleCoresCount, 1);
    QVERIFY(r.staleCoresBytes >= 300);
    in.inUseCoreId = QStringLiteral("melonds_ds");
    in.inUseCoreVersion = QStringLiteral("2026.09.01");  // the running game still has the older build loaded
    r = LocalStorage::measure(in);
    QCOMPARE(r.staleCoresCount, 0);
    QCOMPARE(r.staleCoresBytes, qint64(0));
    in.inUseCoreVersion.clear();
    const auto removed = in.cores.removeStaleVersions(in.platform, LocalStorage::inUseResolver(in));
    QCOMPARE(removed.removed, 1);
    QCOMPARE(in.cores.versions(QStringLiteral("melonds_ds"), in.platform), (QStringList{"2026.10.09"}));
    QCOMPARE(LocalStorage::measure(in).staleCoresCount, 0);
  }
};

QTEST_GUILESS_MAIN(LocalStorageTest)
#include "localstorage_test.moc"
