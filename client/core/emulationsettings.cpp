#include "emulationsettings.h"
#include "fsutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace uzume {

namespace {
const QString kGlobal = QStringLiteral("global");
const QString kSystems = QStringLiteral("systems");
const QString kGames = QStringLiteral("games");
const QString kOptions = QStringLiteral("options");

QMap<QString, QString> toMap(const QJsonObject& o) {
  QMap<QString, QString> m;
  for (auto it = o.begin(); it != o.end(); ++it) {
    if (it.value().isString()) {
      m.insert(it.key(), it.value().toString());
    }
  }
  return m;
}
}  // namespace

EmulationSettings::EmulationSettings(const QString& baseDir)
    : path_(QDir(baseDir).filePath(QStringLiteral("settings/emulation.json"))) {
  raw_ = fsutil::readJsonObject(path_);  // a corrupt file is moved aside before anything can overwrite it
}

// The object that holds the key/value pairs of one level.
const QJsonObject EmulationSettings::levelValue(Level level, const QString& scope) const {
  switch (level) {
    case Level::Global: return raw_.value(kGlobal).toObject();
    case Level::System: return raw_.value(kSystems).toObject().value(scope).toObject().value(kOptions).toObject();
    case Level::Game: return raw_.value(kGames).toObject().value(scope).toObject();
  }
  return {};
}

bool EmulationSettings::hasValue(Level level, const QString& scope, const QString& key) const {
  return levelValue(level, scope).value(key).isString();
}

QString EmulationSettings::value(Level level, const QString& scope, const QString& key) const {
  const QJsonValue v = levelValue(level, scope).value(key);
  return v.isString() ? v.toString() : QString();
}

QMap<QString, QString> EmulationSettings::values(Level level, const QString& scope) const {
  return toMap(levelValue(level, scope));
}

bool EmulationSettings::setValue(Level level, const QString& scope, const QString& key, const QString& value) {
  QJsonObject obj = levelValue(level, scope);
  obj.insert(key, value);
  switch (level) {
    case Level::Global: raw_.insert(kGlobal, obj); break;
    case Level::System: {
      QJsonObject systems = raw_.value(kSystems).toObject();
      QJsonObject sys = systems.value(scope).toObject();
      sys.insert(kOptions, obj);
      systems.insert(scope, sys);
      raw_.insert(kSystems, systems);
      break;
    }
    case Level::Game: {
      QJsonObject games = raw_.value(kGames).toObject();
      games.insert(scope, obj);
      raw_.insert(kGames, games);
      break;
    }
  }
  return save();
}

bool EmulationSettings::removeValue(Level level, const QString& scope, const QString& key) {
  QJsonObject obj = levelValue(level, scope);
  if (!obj.contains(key)) {
    return true;
  }
  obj.remove(key);
  switch (level) {
    case Level::Global:
      if (obj.isEmpty()) raw_.remove(kGlobal); else raw_.insert(kGlobal, obj);
      break;
    case Level::System: {
      QJsonObject systems = raw_.value(kSystems).toObject();
      QJsonObject sys = systems.value(scope).toObject();
      if (obj.isEmpty()) sys.remove(kOptions); else sys.insert(kOptions, obj);
      if (sys.isEmpty()) systems.remove(scope); else systems.insert(scope, sys);
      if (systems.isEmpty()) raw_.remove(kSystems); else raw_.insert(kSystems, systems);
      break;
    }
    case Level::Game: {
      QJsonObject games = raw_.value(kGames).toObject();
      if (obj.isEmpty()) games.remove(scope); else games.insert(scope, obj);
      if (games.isEmpty()) raw_.remove(kGames); else raw_.insert(kGames, games);
      break;
    }
  }
  return save();
}

EmulationSettings::Resolved EmulationSettings::resolve(const QString& key, const QString& systemId, const QString& gameId,
                                                       const QString& manifestDefault, const QString& coreDefault) const {
  if (!gameId.isEmpty() && hasValue(Level::Game, gameId, key)) {
    return {value(Level::Game, gameId, key), Source::Game};
  }
  if (!systemId.isEmpty() && hasValue(Level::System, systemId, key)) {
    return {value(Level::System, systemId, key), Source::System};
  }
  if (hasValue(Level::Global, QString(), key)) {
    return {value(Level::Global, QString(), key), Source::Global};
  }
  if (!manifestDefault.isNull()) {
    return {manifestDefault, Source::Manifest};
  }
  return {coreDefault, Source::Core};
}

QMap<QString, QString> EmulationSettings::mergedOverrides(const QString& systemId, const QString& gameId) const {
  QMap<QString, QString> m = values(Level::Global, QString());
  if (!systemId.isEmpty()) {
    const auto s = values(Level::System, systemId);
    for (auto it = s.cbegin(); it != s.cend(); ++it) m.insert(it.key(), it.value());
  }
  if (!gameId.isEmpty()) {
    const auto g = values(Level::Game, gameId);
    for (auto it = g.cbegin(); it != g.cend(); ++it) m.insert(it.key(), it.value());
  }
  return m;
}

QString EmulationSettings::normalizeRenderer(const QString& value) {
  return value.trimmed().compare(QLatin1String("vulkan"), Qt::CaseInsensitive) == 0 ? QStringLiteral("vulkan") : QStringLiteral("opengl");
}

QString EmulationSettings::renderer(const QString& systemId, const QString& gameId) const {
  return normalizeRenderer(resolve(QString::fromLatin1(kRendererKey), systemId, gameId, QString(), QStringLiteral("opengl")).value);
}

QString EmulationSettings::sourceName(Source s) {
  switch (s) {
    case Source::Game: return QStringLiteral("game");
    case Source::System: return QStringLiteral("system");
    case Source::Global: return QStringLiteral("global");
    case Source::Manifest: return QStringLiteral("manifest");
    case Source::Core: break;
  }
  return QStringLiteral("core");
}

bool EmulationSettings::save() const {
  if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
    return false;
  }
  QSaveFile f(path_);
  if (!f.open(QIODevice::WriteOnly)) {
    return false;
  }
  f.write(QJsonDocument(raw_).toJson(QJsonDocument::Indented));
  return f.commit();
}

}  // namespace uzume
