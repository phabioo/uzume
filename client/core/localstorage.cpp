#include "localstorage.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStorageInfo>

#include "statestore.h"

namespace uzume {

qint64 LocalStorage::folderBytes(const QString& dir, int* files) {
  qint64 n = 0;
  int count = 0;
  if (!dir.isEmpty() && QFileInfo(dir).isDir()) {
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      n += it.fileInfo().size();
      ++count;
    }
  }
  if (files != nullptr) *files = count;
  return n;
}

QString LocalStorage::screenshotsPendingDir(const QString& dataDir) { return QDir(dataDir).filePath(QStringLiteral("screenshots/pending")); }

qint64 LocalStorage::unusedGamesCutoffMs(qint64 nowMs) {
  if (nowMs <= 0) nowMs = QDateTime::currentMSecsSinceEpoch();
  return nowMs - static_cast<qint64>(RomCache::kUnusedGameDays) * 24 * 60 * 60 * 1000;
}

std::function<QString(const QString&)> LocalStorage::inUseResolver(const LocalStorageInput& in) {
  const QString id = in.inUseCoreId;
  const QString version = in.inUseCoreVersion;
  return [id, version](const QString& coreId) { return (!id.isEmpty() && coreId == id) ? version : QString(); };
}

LocalStorageReport LocalStorage::measure(const LocalStorageInput& in) {
  LocalStorageReport r;
  r.dataPath = QDir::toNativeSeparators(QDir(in.dataDir).absolutePath());
  r.measuredAtMs = QDateTime::currentMSecsSinceEpoch();
  const QStorageInfo vol(in.dataDir);
  if (vol.isValid() && vol.isReady()) {
    r.totalBytes = vol.bytesTotal();
    r.freeBytes = vol.bytesAvailable();
  }
  r.gamesBytes = in.roms.totalSize();
  r.gamesCount = static_cast<int>(in.roms.entries().size());
  // Save cache: every server's local save folders.
  const QDir hubs(QDir(in.dataDir).filePath(QStringLiteral("hubs")));
  for (const QString& hub : hubs.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    const QDir hubDir(hubs.filePath(hub));
    r.savesBytes += folderBytes(hubDir.filePath(QStringLiteral("saves")));
    const QDir users(hubDir.filePath(QStringLiteral("users")));
    for (const QString& user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      r.savesBytes += folderBytes(users.filePath(user + QStringLiteral("/saves")));
    }
  }
  r.savesBytes += folderBytes(QDir(in.dataDir).filePath(QStringLiteral("c")));
  r.statesBytes = StateStore::measureAll(in.dataDir);
  if (in.libraryKnown && !in.statesDir.isEmpty()) {
    const StateStore::Orphans o = StateStore(in.statesDir).orphans(in.libraryGameIds);
    r.orphanStatesBytes = o.bytes;
    r.orphanStatesGames = o.games;
  }
  r.screenshotsBytes = folderBytes(screenshotsPendingDir(in.dataDir), &r.screenshotsCount);
  r.coresBytes = folderBytes(in.cores.root()) + folderBytes(in.firmwareDir);
  for (const RomCache::Entry& e : in.roms.unusedSince(unusedGamesCutoffMs(in.nowMs), in.protectedHashes)) {
    r.oldGamesBytes += e.size;
    ++r.oldGamesCount;
  }
  for (const CoreCache::StaleVersion& s : in.cores.staleVersions(in.platform, inUseResolver(in))) {
    r.staleCoresBytes += s.bytes;
    ++r.staleCoresCount;
  }
  return r;
}

}  // namespace uzume
