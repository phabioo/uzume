#include "core_options.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDateTime>
#include <QDebug>
#include <QStringList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "libretro_backend.h"

namespace uzume::emu {

CoreProbe probeCore(const QString& corePath, const QString& systemDir, const QString& saveDir) {
  CoreProbe p;
  LibretroBackend be;
  be.setSystemDirectory(systemDir);
  be.setSaveDirectory(saveDir);
  if (!be.loadCore(corePath, &p.error)) {
    return p;
  }
  p.info = be.coreInfo();
  // Cores like melonDS DS register their options only in retro_load_game: start the core once in no-game mode, but
  // only if it declared SET_SUPPORT_NO_GAME (otherwise retro_load_game(NULL) may crash, e.g. Azahar). Options then
  // come from what the core registered in retro_set_environment/retro_init. A core that refuses no-game mode has
  // still reported what it had before (options may then be empty).
  if (be.supportsNoGame()) {
    QString ignored;
    be.loadGame(QString(), &ignored);
  }
  p.options = be.coreOptions();
  p.categories = be.coreOptionCategories();
  p.ok = true;
  be.unloadCore();
  return p;
}

namespace {
QString libraryKey(const QString& corePath) {
  const QFileInfo fi(corePath);
  return QStringLiteral("%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
}
QStringList readKeyList(const QString& file) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QStringList out;
  for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).array()) out.append(v.toString());
  return out;
}
void writeKeyList(const QString& file, const QStringList& keys) {
  QSaveFile f(file);
  if (!f.open(QIODevice::WriteOnly)) return;
  QJsonArray a;
  for (const QString& k : keys) a.append(k);
  f.write(QJsonDocument(a).toJson(QJsonDocument::Compact));
  f.commit();
}
}  // namespace

CoreProbe probeCoreGuarded(const QString& corePath, const QString& systemDir, const QString& probeDir) {
  QDir().mkpath(probeDir);
  const QString markerFile = QDir(probeDir).filePath(QStringLiteral("probe_running.json"));
  const QString unsafeFile = QDir(probeDir).filePath(QStringLiteral("unsafe_cores.json"));
  QStringList unsafe = readKeyList(unsafeFile);

  // A marker that is still there means the previous probe never returned (crash): remember that library.
  if (QFile::exists(markerFile)) {
    const QStringList stale = readKeyList(markerFile);
    for (const QString& k : stale) {
      if (!unsafe.contains(k)) unsafe.append(k);
      qWarning().noquote() << "Core probe crashed earlier, library will not be probed again:" << k.section(QLatin1Char('|'), 0, 0);
    }
    writeKeyList(unsafeFile, unsafe);
    QFile::remove(markerFile);
  }

  const QString key = libraryKey(corePath);
  if (unsafe.contains(key)) {
    CoreProbe p;
    p.error = QStringLiteral("Core crashed during an earlier probe; skipped");
    return p;
  }

  writeKeyList(markerFile, {key});
  const CoreProbe p = probeCore(corePath, systemDir, probeDir);
  QFile::remove(markerFile);
  return p;
}

QByteArray coreProbeToJson(const CoreProbe& probe) {
  QJsonArray cats;
  for (const CoreOptionCategory& c : probe.categories) {
    cats.append(QJsonObject{{QStringLiteral("key"), c.key}, {QStringLiteral("description"), c.description}, {QStringLiteral("info"), c.info}});
  }
  QJsonArray opts;
  for (const CoreOption& o : probe.options) {
    QJsonArray vals;
    for (const CoreOptionValue& v : o.values) {
      vals.append(QJsonObject{{QStringLiteral("value"), v.value}, {QStringLiteral("label"), v.label}});
    }
    opts.append(QJsonObject{{QStringLiteral("key"), o.key},
                            {QStringLiteral("description"), o.description},
                            {QStringLiteral("info"), o.info},
                            {QStringLiteral("category"), o.categoryKey},
                            {QStringLiteral("default"), o.defaultValue},
                            {QStringLiteral("visible"), o.visible},
                            {QStringLiteral("values"), vals}});
  }
  const QJsonObject root{{QStringLiteral("name"), probe.info.name},
                         {QStringLiteral("version"), probe.info.version},
                         {QStringLiteral("categories"), cats},
                         {QStringLiteral("options"), opts}};
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

CoreProbe coreProbeFromJson(const QByteArray& json) {
  CoreProbe p;
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    p.error = QStringLiteral("cache unreadable");
    return p;
  }
  const QJsonObject root = doc.object();
  p.info.name = root.value(QStringLiteral("name")).toString();
  p.info.version = root.value(QStringLiteral("version")).toString();
  for (const QJsonValue& v : root.value(QStringLiteral("categories")).toArray()) {
    const QJsonObject o = v.toObject();
    p.categories.append({o.value(QStringLiteral("key")).toString(), o.value(QStringLiteral("description")).toString(),
                         o.value(QStringLiteral("info")).toString()});
  }
  for (const QJsonValue& v : root.value(QStringLiteral("options")).toArray()) {
    const QJsonObject o = v.toObject();
    CoreOption opt;
    opt.key = o.value(QStringLiteral("key")).toString();
    if (opt.key.isEmpty()) continue;
    opt.description = o.value(QStringLiteral("description")).toString();
    opt.info = o.value(QStringLiteral("info")).toString();
    opt.categoryKey = o.value(QStringLiteral("category")).toString();
    opt.defaultValue = o.value(QStringLiteral("default")).toString();
    opt.currentValue = opt.defaultValue;
    opt.visible = o.value(QStringLiteral("visible")).toBool(true);
    for (const QJsonValue& vv : o.value(QStringLiteral("values")).toArray()) {
      const QJsonObject vo = vv.toObject();
      opt.values.append({vo.value(QStringLiteral("value")).toString(), vo.value(QStringLiteral("label")).toString()});
    }
    p.options.append(opt);
  }
  p.ok = true;
  return p;
}

bool isLockedCoreOption(const SystemManifest& manifest, const QString& key) {
  if (manifest.lockedCoreOptions.contains(key)) return true;
  if (!manifest.firmware.sysfileOption.isEmpty() && manifest.firmware.sysfileOption == key) return true;
  for (const FirmwareFile& f : manifest.firmware.files) {
    if (!f.coreOption.isEmpty() && f.coreOption == key) return true;
  }
  return false;
}

QMap<QString, QString> launchCoreOptions(const SystemManifest& manifest, const QMap<QString, QString>& userOverrides,
                                         const QMap<QString, QString>& firmwareOptions, const QString& renderer) {
  QMap<QString, QString> out = manifest.coreOptions;
  for (auto it = userOverrides.cbegin(); it != userOverrides.cend(); ++it) {
    if (it.key().startsWith(QLatin1String("uzume.")) || isLockedCoreOption(manifest, it.key())) continue;
    out.insert(it.key(), it.value());
  }
  for (auto it = firmwareOptions.cbegin(); it != firmwareOptions.cend(); ++it) {
    out.insert(it.key(), it.value());
  }
  for (const QString& k : manifest.lockedCoreOptions) {
    if (manifest.coreOptions.contains(k)) out.insert(k, manifest.coreOptions.value(k));
  }
  // The renderer choice is Uzume's (it may sit among the locked keys): it wins over the locked default.
  if (!renderer.isEmpty() && !manifest.rendererOption.isEmpty() && manifest.rendererValues.contains(renderer)) {
    out.insert(manifest.rendererOption, manifest.rendererValues.value(renderer));
  }
  return out;
}

}  // namespace uzume::emu
