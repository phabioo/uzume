#include "localstoragecontroller.h"

#include <QDateTime>
#include <algorithm>
#include <QLocale>
#include <QVariantList>
#include <QtConcurrent>

namespace uzume::ui {

LocalStorageController::LocalStorageController(InputSource source, QObject* parent) : QObject(parent), source_(std::move(source)) {
  connect(&watcher_, &QFutureWatcher<LocalStorageReport>::finished, this, [this]() {
    report_ = toMap(watcher_.result());
    measuring_ = false;
    emit reportChanged();
    emit measuringChanged();
    if (again_) {
      again_ = false;
      measure();
    }
  });
}

QString LocalStorageController::formatBytes(qint64 bytes) {
  constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
  constexpr double kMiB = 1024.0 * 1024.0;
  const QLocale c = QLocale::c();
  if (bytes >= static_cast<qint64>(kGiB)) return QStringLiteral("%1 GB").arg(c.toString(static_cast<double>(bytes) / kGiB, 'f', 1));
  if (bytes >= static_cast<qint64>(kMiB)) return QStringLiteral("%1 MB").arg(c.toString(static_cast<double>(bytes) / kMiB, 'f', 0));
  return QStringLiteral("%1 KB").arg(c.toString(static_cast<double>(bytes) / 1024.0, 'f', 0));
}

void LocalStorageController::measure() {
  if (!source_) return;
  if (measuring_) {
    again_ = true;
    return;
  }
  const LocalStorageInput in = source_();
  measuring_ = true;
  emit measuringChanged();
  watcher_.setFuture(QtConcurrent::run([in]() { return LocalStorage::measure(in); }));
}

QVariantMap LocalStorageController::cleanUp(const QString& id) {
  QVariantMap out{{QStringLiteral("ok"), false}, {QStringLiteral("freedBytes"), 0.0}, {QStringLiteral("freedText"), formatBytes(0)}, {QStringLiteral("removed"), 0}};
  if (!source_) return out;
  const LocalStorageInput in = source_();
  qint64 freed = 0;
  int removed = 0;
  if (id == QLatin1String("old_games")) {
    const RomCache::TrimResult r = in.roms.removeUnusedSince(LocalStorage::unusedGamesCutoffMs(in.nowMs), in.protectedHashes);
    freed = r.freedBytes;
    removed = r.removedFiles;
    if (removed > 0) emit gamesRemoved();
  } else if (id == QLatin1String("unused_cores")) {
    const CoreCache::PruneResult r = in.cores.removeStaleVersions(in.platform, LocalStorage::inUseResolver(in));
    freed = r.freedBytes;
    removed = r.removed;
  } else {
    return out;  // nothing else is ever offered, saves least of all
  }
  out[QStringLiteral("ok")] = true;
  out[QStringLiteral("freedBytes")] = static_cast<double>(freed);
  out[QStringLiteral("freedText")] = formatBytes(freed);
  out[QStringLiteral("removed")] = removed;
  measure();
  return out;
}

QVariantMap LocalStorageController::toMap(const LocalStorageReport& r) {
  const double total = r.totalBytes > 0 ? static_cast<double>(r.totalBytes) : 0.0;
  const auto frac = [total](qint64 b) { return total > 0 ? std::min(1.0, static_cast<double>(b) / total) : 0.0; };
  const auto kind = [&](const char* id, qint64 bytes) {
    return QVariantMap{{QStringLiteral("id"), QLatin1String(id)}, {QStringLiteral("bytes"), static_cast<double>(bytes)},
                       {QStringLiteral("text"), formatBytes(bytes)}, {QStringLiteral("fraction"), frac(bytes)}};
  };
  QVariantList kinds{kind("games", r.gamesBytes), kind("variants", r.variantsBytes), kind("saves", r.savesBytes), kind("states", r.statesBytes),
                     kind("screenshots", r.screenshotsBytes), kind("cores", r.coresBytes)};
  QVariantList cleanup;
  const auto suggestion = [&](const char* id, qint64 bytes, int count) {
    if (bytes > 0 || count > 0) {
      cleanup.append(QVariantMap{{QStringLiteral("id"), QLatin1String(id)}, {QStringLiteral("bytes"), static_cast<double>(bytes)},
                                 {QStringLiteral("text"), formatBytes(bytes)}, {QStringLiteral("count"), count}});
    }
  };
  suggestion("old_games", r.oldGamesBytes, r.oldGamesCount);
  suggestion("unused_cores", r.staleCoresBytes, r.staleCoresCount);
  return {{QStringLiteral("dataPath"), r.dataPath},
          {QStringLiteral("measuredAt"), static_cast<double>(r.measuredAtMs)},
          {QStringLiteral("measuredText"), QDateTime::fromMSecsSinceEpoch(r.measuredAtMs).toString(QStringLiteral("HH:mm"))},
          {QStringLiteral("totalBytes"), static_cast<double>(r.totalBytes)},
          {QStringLiteral("totalText"), r.totalBytes >= 0 ? formatBytes(r.totalBytes) : QString()},
          {QStringLiteral("freeBytes"), static_cast<double>(r.freeBytes)},
          {QStringLiteral("freeText"), r.freeBytes >= 0 ? formatBytes(r.freeBytes) : QString()},
          {QStringLiteral("usedBytes"), static_cast<double>(r.usedBytes())},
          {QStringLiteral("usedText"), formatBytes(r.usedBytes())},
          {QStringLiteral("kinds"), kinds},
          {QStringLiteral("cleanup"), cleanup}};
}

}  // namespace uzume::ui
