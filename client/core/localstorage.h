#pragma once

#include <QDateTime>
#include <QSet>
#include <QString>
#include <functional>

#include "corecache.h"
#include "romcache.h"

namespace uzume {

// What the app keeps on this PC (Settings > This PC > Storage, design 6l-1). Pure file logic, no GUI; measure() blocks and belongs
// on a worker thread. Every folder comes from the class that owns it:
//   Downloaded games     RomCache (<data>/cache/roms, finished files and unfinished downloads)
//   Variants             VariantCache (<data>/cache/variants, built randomizer variants and unfinished builds)
//   Save cache           SaveStore layout: <data>/hubs/<hub>/users/<user>/saves, legacy <data>/hubs/<hub>/saves and the short core
//                        folders <data>/c (the local copies of saves; they are never offered for clean-up)
//   Save states          StateStore layout: <data>/hubs/<hub>/users/<user>/states (the local cache of the states; the server keeps the originals)
//   Screenshots waiting  <data>/screenshots/pending (ScreenshotController queue)
//   Cores and firmware   CoreCache (<data>/cache/cores) + FirmwareCache (<data>/system/firmware)
struct LocalStorageInput {
  QString dataDir;      // ProfileStore::baseDir()
  QString firmwareDir;  // FirmwareCache::dir()
  QString variantsDir;  // VariantCache::root() (game variants built on this PC, 0.21)
  RomCache roms{QString()};
  CoreCache cores{QString()};
  QString platform;               // CoreCache::currentPlatform()
  QSet<QString> protectedHashes;  // running game, downloads in progress (PlayerController::protectedRomHashes)
  QString inUseCoreId;            // core of the running game, empty when none
  QString inUseCoreVersion;
  // Clean-up of save states of games that are no longer in the library: the states folder of the signed-in person on the current
  // server and the ids of the games the library lists. libraryKnown false (library not loaded yet) offers nothing.
  QString statesDir;
  QSet<QString> libraryGameIds;
  bool libraryKnown = false;
  qint64 nowMs = 0;  // 0 = now
};

struct LocalStorageReport {
  QString dataPath;
  qint64 measuredAtMs = 0;
  qint64 totalBytes = -1;  // volume of the data folder (QStorageInfo), -1 = unknown
  qint64 freeBytes = -1;
  qint64 gamesBytes = 0;
  int gamesCount = 0;
  qint64 variantsBytes = 0;  // built game variants (VariantCache)
  qint64 savesBytes = 0;
  qint64 statesBytes = 0;
  qint64 orphanStatesBytes = 0;  // states of games that are not in the library any more (current server and person only)
  int orphanStatesGames = 0;
  qint64 screenshotsBytes = 0;
  int screenshotsCount = 0;
  qint64 coresBytes = 0;  // cores + firmware
  // Clean-up suggestions: what would be removed right now.
  qint64 oldGamesBytes = 0;  // games not played for kUnusedGameDays
  int oldGamesCount = 0;
  qint64 staleCoresBytes = 0;  // cached core versions that are not the newest of their core and not in use
  int staleCoresCount = 0;
  qint64 usedBytes() const { return gamesBytes + variantsBytes + savesBytes + statesBytes + screenshotsBytes + coresBytes; }
};

class LocalStorage {
 public:
  static qint64 folderBytes(const QString& dir, int* files = nullptr);  // recursive, 0 for a missing folder
  static QString screenshotsPendingDir(const QString& dataDir);
  static LocalStorageReport measure(const LocalStorageInput& in);
  static qint64 unusedGamesCutoffMs(qint64 nowMs = 0);  // now - kUnusedGameDays
  static std::function<QString(const QString&)> inUseResolver(const LocalStorageInput& in);
};

}  // namespace uzume
