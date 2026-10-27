#include "statestore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

#include "localstorage.h"
#include "profilestore.h"
#include "savestore.h"

namespace uzume {

QJsonObject StateRecord::toJson() const {
  return {{QStringLiteral("game_id"), gameId},
          {QStringLiteral("slot"), saveSlot},
          {QStringLiteral("n"), n},
          {QStringLiteral("core_id"), coreId},
          {QStringLiteral("core_version"), coreVersion},
          {QStringLiteral("size_bytes"), static_cast<double>(sizeBytes)},
          {QStringLiteral("sha256"), sha256},
          {QStringLiteral("created_at"), createdAt},
          {QStringLiteral("device_name"), deviceName},
          {QStringLiteral("has_preview"), hasPreview},
          {QStringLiteral("on_server"), onServer},
          {QStringLiteral("pending_upload"), pendingUpload},
          {QStringLiteral("delete_pending"), deletePending}};
}

std::optional<StateRecord> StateRecord::fromJson(const QJsonObject& o) {
  StateRecord r;
  r.gameId = o.value(QStringLiteral("game_id")).toString();
  r.saveSlot = o.value(QStringLiteral("slot")).toString();
  r.n = o.value(QStringLiteral("n")).toInt(-1);
  r.coreId = o.value(QStringLiteral("core_id")).toString();
  r.coreVersion = o.value(QStringLiteral("core_version")).toString();
  r.sizeBytes = o.value(QStringLiteral("size_bytes")).toVariant().toLongLong();
  r.sha256 = o.value(QStringLiteral("sha256")).toString();
  r.createdAt = o.value(QStringLiteral("created_at")).toString();
  r.deviceName = o.value(QStringLiteral("device_name")).toString();
  r.hasPreview = o.value(QStringLiteral("has_preview")).toBool();
  r.onServer = o.value(QStringLiteral("on_server")).toBool();
  r.pendingUpload = o.value(QStringLiteral("pending_upload")).toBool();
  r.deletePending = o.value(QStringLiteral("delete_pending")).toBool();
  if (!SaveStore::isSafeId(r.gameId) || !SaveStore::isValidSlotName(r.saveSlot) || r.n < 0 || r.n > 10) return std::nullopt;
  return r;
}

QString StateStore::userStatesDir(const ProfileStore& profiles, const QString& hubId, const QString& userId) {
  if (!SaveStore::isSafeId(hubId) || !SaveStore::isSafeId(userId)) return {};
  return QDir(profiles.hubDir(hubId)).filePath(QStringLiteral("users/%1/states").arg(userId));
}

QString StateStore::label(int n) { return n == kStateAuto ? QStringLiteral("A") : QString::number(n); }

int StateStore::nextSlot(int current) { return current >= 1 && current < kStateSlotCount ? current + 1 : 1; }

int StateStore::firstFreeSlot(const QList<StateRecord>& have) {
  for (int n = 1; n <= kStateSlotCount; ++n) {
    bool taken = false;
    for (const StateRecord& r : have) taken = taken || (r.n == n && !r.deletePending);
    if (!taken) return n;
  }
  return 0;
}

QString StateStore::dirFor(const QString& gameId, const QString& saveSlot) const {
  if (root_.isEmpty() || !SaveStore::isSafeId(gameId) || !SaveStore::isValidSlotName(saveSlot)) return {};
  return QDir(root_).filePath(gameId + QLatin1Char('/') + saveSlot);
}

QString StateStore::fileFor(const QString& gameId, const QString& saveSlot, int n, const char* ext) const {
  const QString d = dirFor(gameId, saveSlot);
  if (d.isEmpty() || n < 0 || n > 10) return {};
  return QDir(d).filePath(QString::number(n) + QLatin1Char('.') + QLatin1String(ext));
}

namespace {
bool writeFile(const QString& path, const QByteArray& data) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  if (f.write(data) != data.size()) {
    f.cancelWriting();
    return false;
  }
  return f.commit();
}
}  // namespace

bool StateStore::put(StateRecord rec, const QByteArray& state, const QByteArray& previewPng) {
  const QString dir = dirFor(rec.gameId, rec.saveSlot);
  if (dir.isEmpty() || rec.n < 0 || rec.n > 10 || state.isEmpty() || state.size() > kMaxStateBytes) return false;
  if (!QDir().mkpath(dir)) return false;
  rec.sizeBytes = state.size();
  rec.sha256 = QString::fromLatin1(QCryptographicHash::hash(state, QCryptographicHash::Sha256).toHex());
  if (rec.createdAt.isEmpty()) rec.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  const bool withPreview = !previewPng.isEmpty() && previewPng.size() <= kMaxStatePreviewBytes;
  rec.hasPreview = withPreview;
  rec.deletePending = false;
  if (!writeFile(fileFor(rec.gameId, rec.saveSlot, rec.n, "state"), state)) return false;
  if (withPreview) {
    if (!writeFile(fileFor(rec.gameId, rec.saveSlot, rec.n, "png"), previewPng)) rec.hasPreview = false;
  } else {
    QFile::remove(fileFor(rec.gameId, rec.saveSlot, rec.n, "png"));
  }
  return writeFile(fileFor(rec.gameId, rec.saveSlot, rec.n, "json"), QJsonDocument(rec.toJson()).toJson(QJsonDocument::Compact));
}

bool StateStore::updateRecord(const StateRecord& rec) {
  const QString p = fileFor(rec.gameId, rec.saveSlot, rec.n, "json");
  if (p.isEmpty() || !QFile::exists(p)) return false;
  return writeFile(p, QJsonDocument(rec.toJson()).toJson(QJsonDocument::Compact));
}

bool StateStore::putRecord(StateRecord rec) {
  const QString dir = dirFor(rec.gameId, rec.saveSlot);
  if (dir.isEmpty() || rec.n < 0 || rec.n > 10 || !QDir().mkpath(dir)) return false;
  const auto old = record(rec.gameId, rec.saveSlot, rec.n);
  if (!old || old->sha256 != rec.sha256) {
    QFile::remove(fileFor(rec.gameId, rec.saveSlot, rec.n, "state"));
    QFile::remove(fileFor(rec.gameId, rec.saveSlot, rec.n, "png"));
  }
  rec.hasPreview = rec.hasPreview && QFile::exists(fileFor(rec.gameId, rec.saveSlot, rec.n, "png"));
  return writeFile(fileFor(rec.gameId, rec.saveSlot, rec.n, "json"), QJsonDocument(rec.toJson()).toJson(QJsonDocument::Compact));
}

bool StateStore::putPreview(const QString& gameId, const QString& saveSlot, int n, const QByteArray& png) {
  auto rec = record(gameId, saveSlot, n);
  if (!rec || png.isEmpty() || png.size() > kMaxStatePreviewBytes) return false;
  if (!writeFile(fileFor(gameId, saveSlot, n, "png"), png)) return false;
  rec->hasPreview = true;
  return updateRecord(*rec);
}

bool StateStore::hasBlob(const QString& gameId, const QString& saveSlot, int n) const {
  const QString p = fileFor(gameId, saveSlot, n, "state");
  return !p.isEmpty() && QFile::exists(p);
}

QString StateStore::previewPath(const QString& gameId, const QString& saveSlot, int n) const {
  const QString p = fileFor(gameId, saveSlot, n, "png");
  return !p.isEmpty() && QFile::exists(p) ? p : QString();
}

std::optional<StateRecord> StateStore::record(const QString& gameId, const QString& saveSlot, int n) const {
  const QString p = fileFor(gameId, saveSlot, n, "json");
  if (p.isEmpty()) return std::nullopt;
  QFile f(p);
  if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
  const auto rec = StateRecord::fromJson(QJsonDocument::fromJson(f.readAll()).object());
  if (!rec || rec->gameId != gameId || rec->saveSlot != saveSlot || rec->n != n) return std::nullopt;
  return rec;
}

QList<StateRecord> StateStore::list(const QString& gameId, const QString& saveSlot) const {
  QList<StateRecord> out;
  for (int n = 0; n <= 10; ++n) {
    if (const auto r = record(gameId, saveSlot, n)) out.append(*r);
  }
  return out;
}

QByteArray StateStore::state(const QString& gameId, const QString& saveSlot, int n) const {
  const QString p = fileFor(gameId, saveSlot, n, "state");
  QFile f(p);
  return !p.isEmpty() && f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray StateStore::preview(const QString& gameId, const QString& saveSlot, int n) const {
  const QString p = fileFor(gameId, saveSlot, n, "png");
  QFile f(p);
  return !p.isEmpty() && f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool StateStore::remove(const QString& gameId, const QString& saveSlot, int n) {
  const QString j = fileFor(gameId, saveSlot, n, "json");
  if (j.isEmpty()) return false;
  const bool had = QFile::exists(j) || QFile::exists(fileFor(gameId, saveSlot, n, "state"));
  QFile::remove(fileFor(gameId, saveSlot, n, "state"));
  QFile::remove(fileFor(gameId, saveSlot, n, "png"));
  QFile::remove(j);
  return had;
}

QList<StateRecord> StateStore::pending() const {
  QList<StateRecord> out;
  if (root_.isEmpty()) return out;
  const QDir games(root_);
  for (const QString& g : games.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    const QDir gd(games.filePath(g));
    for (const QString& s : gd.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      for (const StateRecord& r : list(g, s)) {
        if (r.pendingUpload || r.deletePending) out.append(r);
      }
    }
  }
  return out;
}

qint64 StateStore::bytes() const { return root_.isEmpty() ? 0 : LocalStorage::folderBytes(root_); }

StateStore::Orphans StateStore::orphans(const QSet<QString>& libraryGameIds) const {
  Orphans o;
  if (root_.isEmpty()) return o;
  const QDir games(root_);
  for (const QString& g : games.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (libraryGameIds.contains(g)) continue;
    int files = 0;
    const qint64 b = LocalStorage::folderBytes(games.filePath(g), &files);
    o.bytes += b;
    ++o.games;
    QDirIterator it(games.filePath(g), {QStringLiteral("*.state")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      it.next();
      ++o.states;
    }
  }
  return o;
}

StateStore::Orphans StateStore::removeOrphans(const QSet<QString>& libraryGameIds) {
  const Orphans o = orphans(libraryGameIds);
  const QDir games(root_);
  for (const QString& g : games.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (!libraryGameIds.contains(g)) QDir(games.filePath(g)).removeRecursively();
  }
  return o;
}

qint64 StateStore::measureAll(const QString& dataDir) {
  qint64 total = 0;
  const QDir hubs(QDir(dataDir).filePath(QStringLiteral("hubs")));
  for (const QString& hub : hubs.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    const QDir users(hubs.filePath(hub + QStringLiteral("/users")));
    for (const QString& user : users.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      total += LocalStorage::folderBytes(users.filePath(user + QStringLiteral("/states")));
    }
  }
  return total;
}

}  // namespace uzume
