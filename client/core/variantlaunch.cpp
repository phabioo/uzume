#include "variantlaunch.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace uzume::variantlaunch {

namespace {
constexpr const char* kMarker = ".uzume-variant";

bool copyTree(const QString& src, const QString& dst, QString* error) {
  if (!QDir().mkpath(dst)) {
    if (error) *error = QStringLiteral("cannot create a folder");
    return false;
  }
  const QDir s(src);
  for (const QFileInfo& e : s.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name)) {
    const QString to = dst + QLatin1Char('/') + e.fileName();
    if (e.isSymLink()) continue;  // the engine writes plain files only
    if (e.isDir()) {
      if (!copyTree(e.absoluteFilePath(), to, error)) return false;
    } else if (!QFile::copy(e.absoluteFilePath(), to)) {
      if (error) *error = QStringLiteral("cannot copy a file");
      return false;
    }
  }
  return true;
}
}  // namespace

bool isValidTitleId(const QString& titleId) {
  static const QRegularExpression re(QStringLiteral("^[0-9A-Fa-f]{16}$"));
  return re.match(titleId).hasMatch();
}

QString modsDir(const QString& azaharDir) { return QDir::cleanPath(azaharDir) + QStringLiteral("/load/mods"); }

bool placeMod(const QString& azaharDir, const QString& titleId, const QString& modSource, const QString& markerValue, QString* error) {
  if (!isValidTitleId(titleId) || azaharDir.isEmpty() || !QFileInfo(modSource).isDir()) {
    if (error) *error = QStringLiteral("invalid mod or title id");
    return false;
  }
  const QString mods = modsDir(azaharDir);
  const QString tid = titleId.toUpper();
  const QString target = mods + QLatin1Char('/') + tid;
  // Anything else in this slot's mods folder goes: a variant plays its own world only.
  if (QFileInfo(mods).isDir()) {
    for (const QFileInfo& e : QDir(mods).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
      if (e.fileName() == tid && e.isDir() && !e.isSymLink()) continue;
      if (e.isDir() && !e.isSymLink()) QDir(e.absoluteFilePath()).removeRecursively();
      else QFile::remove(e.absoluteFilePath());
    }
  }
  const QString markerPath = target + QLatin1Char('/') + QLatin1String(kMarker);
  QFile m(markerPath);
  if (m.open(QIODevice::ReadOnly) && QString::fromUtf8(m.readAll()) == markerValue) return true;
  m.close();
  QDir(target).removeRecursively();
  if (!QDir().mkpath(mods) || !copyTree(modSource, target, error)) {
    QDir(target).removeRecursively();
    return false;
  }
  QFile w(markerPath);
  if (!w.open(QIODevice::WriteOnly) || w.write(markerValue.toUtf8()) < 0) {
    QDir(target).removeRecursively();
    if (error) *error = QStringLiteral("cannot write the marker");
    return false;
  }
  return true;
}

bool ensureNoMod(const QString& azaharDir, const QString& titleId, QString* error) {
  if (!isValidTitleId(titleId) || azaharDir.isEmpty()) return true;  // nothing to clear
  const QString target = modsDir(azaharDir) + QLatin1Char('/') + titleId.toUpper();
  if (!QFileInfo::exists(target)) return true;
  // Only a mod placed by a variant (it carries the marker) is removed; a folder somebody put there by hand is left alone ... and
  // reported, because Main must not play a variant.
  if (QFileInfo(target + QLatin1Char('/') + QLatin1String(kMarker)).isFile()) {
    if (QDir(target).removeRecursively()) return true;
    if (error) *error = QStringLiteral("cannot remove the mod folder of a variant");
    return false;
  }
  return true;
}

}  // namespace uzume::variantlaunch
