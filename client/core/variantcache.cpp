#include "variantcache.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace uzume {

namespace {
constexpr const char* kRecipe = "recipe.json";
constexpr const char* kBuilt = "built.json";
constexpr const char* kSpoiler = "spoiler.log";

qint64 nowOr(qint64 ms) { return ms > 0 ? ms : QDateTime::currentMSecsSinceEpoch(); }

bool writeJson(const QString& path, const QJsonObject& o) {
  QSaveFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) >= 0 && f.commit();
}
QJsonObject readJson(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}
}  // namespace

QJsonObject VariantRecipe::toJson() const {
  return QJsonObject{{QStringLiteral("variant_id"), variantId},
                     {QStringLiteral("game_id"), gameId},
                     {QStringLiteral("name"), name},
                     {QStringLiteral("slot"), slot},
                     {QStringLiteral("preset_id"), presetId},
                     {QStringLiteral("system"), system},
                     {QStringLiteral("tool_id"), toolId},
                     {QStringLiteral("tool_version"), toolVersion},
                     {QStringLiteral("settings"), settings},
                     {QStringLiteral("seed"), seed},
                     {QStringLiteral("base_sha256"), baseSha256},
                     {QStringLiteral("title_id"), titleId},
                     {QStringLiteral("update_version"), updateVersion},
                     {QStringLiteral("rom_file_name"), romFileName},
                     {QStringLiteral("spoiler_log"), spoilerLog}};
}

VariantRecipe VariantRecipe::fromJson(const QJsonObject& o) {
  VariantRecipe r;
  r.variantId = o.value(QStringLiteral("variant_id")).toString();
  r.gameId = o.value(QStringLiteral("game_id")).toString();
  r.name = o.value(QStringLiteral("name")).toString();
  r.slot = o.value(QStringLiteral("slot")).toString();
  r.presetId = o.value(QStringLiteral("preset_id")).toString();
  r.system = o.value(QStringLiteral("system")).toString();
  r.toolId = o.value(QStringLiteral("tool_id")).toString(QStringLiteral("randomizer"));
  r.toolVersion = o.value(QStringLiteral("tool_version")).toString();
  r.settings = o.value(QStringLiteral("settings")).toString();
  r.seed = o.value(QStringLiteral("seed")).toString();
  r.baseSha256 = o.value(QStringLiteral("base_sha256")).toString();
  r.titleId = o.value(QStringLiteral("title_id")).toString();
  r.updateVersion = o.value(QStringLiteral("update_version")).toString();
  r.romFileName = o.value(QStringLiteral("rom_file_name")).toString();
  r.spoilerLog = o.value(QStringLiteral("spoiler_log")).toBool();
  return r;
}

VariantCache::VariantCache(const QString& root) : root_(QDir::cleanPath(root)) {}

bool VariantCache::isValidId(const QString& id) {
  static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
  return re.match(id).hasMatch();
}

QString VariantCache::dir(const QString& id) const { return root_.isEmpty() || !isValidId(id) ? QString() : root_ + QLatin1Char('/') + id; }
QString VariantCache::outDir(const QString& id) const { return dir(id).isEmpty() ? QString() : dir(id) + QStringLiteral("/out"); }
QString VariantCache::tempDir(const QString& id) const { return root_.isEmpty() || !isValidId(id) ? QString() : root_ + QStringLiteral("/.tmp-") + id; }

bool VariantCache::isBuilt(const QString& id) const {
  const QString d = dir(id);
  return !d.isEmpty() && QFileInfo(d + QLatin1Char('/') + QLatin1String(kBuilt)).isFile() && QFileInfo(d + QStringLiteral("/out")).isDir();
}

std::optional<VariantRecipe> VariantCache::recipe(const QString& id) const {
  const QString d = dir(id);
  if (d.isEmpty()) return std::nullopt;
  const QJsonObject o = readJson(d + QLatin1Char('/') + QLatin1String(kRecipe));
  if (o.isEmpty()) return std::nullopt;
  return VariantRecipe::fromJson(o);
}

bool VariantCache::writeRecipe(const VariantRecipe& r) const {
  const QString d = dir(r.variantId);
  if (d.isEmpty() || !QDir().mkpath(d)) return false;
  return writeJson(d + QLatin1Char('/') + QLatin1String(kRecipe), r.toJson());
}

QString VariantCache::romPath(const QString& id) const {
  if (!isBuilt(id)) return {};
  const auto r = recipe(id);
  if (!r || r->isMod() || r->romFileName.isEmpty()) return {};
  const QString p = outDir(id) + QLatin1Char('/') + r->romFileName;
  return QFileInfo(p).isFile() ? p : QString();
}

QString VariantCache::modDir(const QString& id) const {
  if (!isBuilt(id)) return {};
  const auto r = recipe(id);
  return r && r->isMod() ? outDir(id) : QString();
}

QString VariantCache::spoilerLogPath(const QString& id) const {
  const QString d = dir(id);
  if (d.isEmpty() || !isBuilt(id)) return {};
  const QString p = d + QLatin1Char('/') + QLatin1String(kSpoiler);
  return QFileInfo(p).isFile() ? p : QString();
}

qint64 VariantCache::folderBytes(const QString& dir) {
  qint64 n = 0;
  QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    n += it.fileInfo().size();
  }
  return n;
}

bool VariantCache::commit(const VariantRecipe& r, qint64 nowMs) const {
  const QString tmp = tempDir(r.variantId);
  const QString d = dir(r.variantId);
  if (tmp.isEmpty() || d.isEmpty() || !QFileInfo(tmp + QStringLiteral("/out")).isDir()) {
    if (!tmp.isEmpty()) QDir(tmp).removeRecursively();
    return false;
  }
  const qint64 now = nowOr(nowMs);
  const qint64 bytes = folderBytes(tmp);
  const bool ok = writeJson(tmp + QLatin1Char('/') + QLatin1String(kRecipe), r.toJson()) &&
                  writeJson(tmp + QLatin1Char('/') + QLatin1String(kBuilt),
                            QJsonObject{{QStringLiteral("built_ms"), static_cast<double>(now)},
                                        {QStringLiteral("last_played_ms"), static_cast<double>(now)},
                                        {QStringLiteral("bytes"), static_cast<double>(bytes)}});
  if (!ok) {
    QDir(tmp).removeRecursively();
    return false;
  }
  QDir(d).removeRecursively();
  if (!QDir().rename(tmp, d)) {
    QDir(tmp).removeRecursively();
    return false;
  }
  return true;
}

void VariantCache::discardTemp(const QString& id) const {
  const QString tmp = tempDir(id);
  if (!tmp.isEmpty()) QDir(tmp).removeRecursively();
}

bool VariantCache::remove(const QString& id, bool keepRecipe) const {
  const QString d = dir(id);
  if (d.isEmpty() || !QFileInfo(d).isDir()) return false;
  if (!keepRecipe) return QDir(d).removeRecursively();
  QDir(d + QStringLiteral("/out")).removeRecursively();
  QFile::remove(d + QLatin1Char('/') + QLatin1String(kBuilt));
  QFile::remove(d + QLatin1Char('/') + QLatin1String(kSpoiler));
  return true;
}

void VariantCache::touch(const QString& id, qint64 nowMs) const {
  const QString d = dir(id);
  if (d.isEmpty() || !isBuilt(id)) return;
  const QString p = d + QLatin1Char('/') + QLatin1String(kBuilt);
  QJsonObject o = readJson(p);
  o.insert(QStringLiteral("last_played_ms"), static_cast<double>(nowOr(nowMs)));
  o.insert(QStringLiteral("played"), true);
  writeJson(p, o);
}

QList<VariantCache::Entry> VariantCache::entries() const {
  QList<Entry> out;
  if (root_.isEmpty()) return out;
  const QFileInfoList dirs = QDir(root_).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QFileInfo& di : dirs) {
    if (di.fileName().startsWith(QLatin1Char('.')) || !isValidId(di.fileName()) || !isBuilt(di.fileName())) continue;
    const QJsonObject b = readJson(di.absoluteFilePath() + QLatin1Char('/') + QLatin1String(kBuilt));
    Entry e;
    e.id = di.fileName();
    e.bytes = folderBytes(di.absoluteFilePath());
    e.builtMs = static_cast<qint64>(b.value(QStringLiteral("built_ms")).toDouble());
    e.lastPlayedMs = static_cast<qint64>(b.value(QStringLiteral("last_played_ms")).toDouble(static_cast<double>(e.builtMs)));
    e.played = b.value(QStringLiteral("played")).toBool(e.lastPlayedMs != e.builtMs);
    out.append(e);
  }
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.lastPlayedMs != b.lastPlayedMs ? a.lastPlayedMs < b.lastPlayedMs : a.id < b.id; });
  return out;
}

qint64 VariantCache::totalBytes() const {
  qint64 n = 0;
  for (const Entry& e : entries()) n += e.bytes;
  if (!root_.isEmpty()) {
    const QFileInfoList tmps = QDir(root_).entryInfoList(QStringList{QStringLiteral(".tmp-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo& t : tmps) n += folderBytes(t.absoluteFilePath());
  }
  return n;
}

VariantCache::TrimResult VariantCache::trimToLimit(qint64 limitBytes, const QSet<QString>& protectedIds, qint64 nowMs) const {
  TrimResult r;
  const qint64 now = nowOr(nowMs);
  // Stale unfinished builds.
  if (!root_.isEmpty()) {
    const QFileInfoList tmps = QDir(root_).entryInfoList(QStringList{QStringLiteral(".tmp-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo& t : tmps) {
      const QString id = t.fileName().mid(5);
      if (protectedIds.contains(id) || now - t.lastModified().toMSecsSinceEpoch() < 24ll * 3600 * 1000) continue;
      r.freedBytes += folderBytes(t.absoluteFilePath());
      QDir(t.absoluteFilePath()).removeRecursively();
    }
  }
  QList<Entry> es = entries();
  qint64 total = 0;
  for (const Entry& e : es) total += e.bytes;
  if (limitBytes > 0) {
    for (const Entry& e : es) {
      if (total <= limitBytes) break;
      if (protectedIds.contains(e.id)) continue;
      if (remove(e.id, true)) {
        total -= e.bytes;
        r.freedBytes += e.bytes;
        ++r.removed;
      }
    }
  }
  r.remainingBytes = total;
  return r;
}

VariantCache::TrimResult VariantCache::removeUnplayed(int days, const QSet<QString>& protectedIds, qint64 nowMs) const {
  TrimResult r;
  const qint64 cutoff = nowOr(nowMs) - static_cast<qint64>(days) * 24 * 3600 * 1000;
  qint64 total = 0;
  for (const Entry& e : entries()) {
    if (days > 0 && e.lastPlayedMs < cutoff && !protectedIds.contains(e.id) && remove(e.id, true)) {
      r.freedBytes += e.bytes;
      ++r.removed;
    } else {
      total += e.bytes;
    }
  }
  r.remainingBytes = total;
  return r;
}

}  // namespace uzume
