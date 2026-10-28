#include "system_manifest.h"

#include "inputmap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

// Explicitly initialize the Qt resource from a static library.
static void initManifestResources() { Q_INIT_RESOURCE(uzume_manifests); }

namespace uzume::emu {

namespace {
const char kBuiltinDir[] = ":/uzume/emulation/manifests";

QStringList stringList(const QJsonValue& v) {
  QStringList out;
  for (const QJsonValue& x : v.toArray())
    if (x.isString()) out.append(x.toString());
  return out;
}

QString normExt(QString e) {
  e = e.trimmed().toLower();
  if (!e.isEmpty() && !e.startsWith(QLatin1Char('.'))) e.prepend(QLatin1Char('.'));
  return e;
}
}  // namespace

QSize DisplayProfile::frameSize() const {
  int w = 0, h = 0;
  const bool vertical = layout == QLatin1String("vertical");
  for (int i = 0; i < screens.size(); ++i) {
    const ScreenSpec& s = screens.at(i);
    const int g = i > 0 ? gap : 0;
    if (vertical) {
      w = std::max(w, s.width);
      h += s.height + g;
    } else {
      w += s.width + g;
      h = std::max(h, s.height);
    }
  }
  return {w, h};
}

QRect DisplayProfile::screenRect(int index) const {
  if (index < 0 || index >= screens.size()) return {};
  const bool vertical = layout == QLatin1String("vertical");
  int x = 0, y = 0;
  for (int i = 0; i < index; ++i) {
    if (vertical) y += screens.at(i).height + gap;
    else x += screens.at(i).width + gap;
  }
  const ScreenSpec& s = screens.at(index);
  const QSize fs = frameSize();
  // Cross-axis alignment (screens of different width in a stack, different height side by side).
  const int free = vertical ? fs.width() - s.width : fs.height() - s.height;
  const int shift = s.align == QLatin1String("center") ? free / 2 : (s.align == QLatin1String("end") ? free : 0);
  if (vertical) x += shift;
  else y += shift;
  return {x, y, s.width, s.height};
}

int DisplayProfile::touchScreenIndex() const {
  for (int i = 0; i < screens.size(); ++i)
    if (screens.at(i).touch) return i;
  return -1;
}

QPointF DisplayProfile::toFrameNormalized(int screenIndex, QPointF n) const {
  const QSize fs = frameSize();
  const QRect r = screenRect(screenIndex);
  if (fs.isEmpty() || r.isEmpty()) return {};
  return {(r.x() + n.x() * r.width()) / fs.width(), (r.y() + n.y() * r.height()) / fs.height()};
}

const FirmwareFile* FirmwareSpec::fileById(const QString& id) const {
  for (const FirmwareFile& f : files)
    if (f.id == id) return &f;
  return nullptr;
}

bool SystemManifest::supportsExtension(const QString& ext) const { return extensions.contains(normExt(ext)); }

std::optional<SystemManifest> ManifestRegistry::parse(const QByteArray& json, QString* error) {
  auto fail = [&](const QString& m) -> std::optional<SystemManifest> {
    if (error) *error = m;
    return std::nullopt;
  };
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) return fail(QStringLiteral("Invalid JSON: ") + pe.errorString());
  const QJsonObject o = doc.object();

  SystemManifest m;
  struct Req { const char* key; QString* dst; };
  for (const Req& r : {Req{"system_id", &m.systemId}, Req{"display_name", &m.displayName}}) {
    *r.dst = o.value(QLatin1String(r.key)).toString().trimmed();
    if (r.dst->isEmpty()) return fail(QStringLiteral("Missing required field: ") + QLatin1String(r.key));
  }
  for (const QJsonValue& v : o.value(QLatin1String("extensions")).toArray()) {
    const QString e = normExt(v.toString());
    if (!e.isEmpty() && e != QLatin1String(".")) m.extensions.append(e);
  }
  if (m.extensions.isEmpty()) return fail(QStringLiteral("Missing required field: extensions"));

  const QJsonObject fw = o.value(QLatin1String("firmware")).toObject();
  m.firmware.required = fw.value(QLatin1String("required")).toBool(false);
  for (const QJsonValue& v : fw.value(QLatin1String("files")).toArray()) {
    const QJsonObject f = v.toObject();
    FirmwareFile ff;
    ff.name = f.value(QLatin1String("name")).toString();
    // The name ends up in the system directory: plain file name only.
    if (ff.name.isEmpty() || ff.name.contains(QLatin1Char('/')) || ff.name.contains(QLatin1Char('\\')) ||
        ff.name.startsWith(QLatin1Char('.'))) {
      return fail(QStringLiteral("firmware.files: invalid name"));
    }
    ff.id = f.value(QLatin1String("id")).toString(QFileInfo(ff.name).completeBaseName());
    ff.required = f.value(QLatin1String("required")).toBool(false);
    m.firmware.files.append(ff);
  }

  m.order = o.value(QLatin1String("order")).toInt(100);
  m.inputProfile = o.value(QLatin1String("input_profile")).toString();
  m.displayProfile = o.value(QLatin1String("display_profile")).toString();
  m.identity = o.value(QLatin1String("identity")).toString();
  const QJsonObject in = o.value(QLatin1String("input")).toObject();
  if (in.contains(QLatin1String("ports"))) {
    const int ports = in.value(QLatin1String("ports")).toInt(0);
    if (ports < 1 || ports > 2) return fail(QStringLiteral("input.ports must be 1 or 2"));
    m.inputPorts = ports;
  }
  for (const bool labels : {false, true}) {
    const QJsonObject obj = in.value(QLatin1String(labels ? "labels" : "map")).toObject();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
      if (!it.value().isString() || it.value().toString().isEmpty()) return fail(QStringLiteral("input: invalid value for ") + it.key());
      (labels ? m.inputLabels : m.inputMap).insert(it.key(), it.value().toString());
    }
  }
  const QJsonObject labels = o.value(QLatin1String("labels")).toObject();
  m.inputLabel = labels.value(QLatin1String("input")).toString(m.displayName);
  m.touchLabel = labels.value(QLatin1String("touch")).toString(QStringLiteral("Touch"));

  const QJsonObject d = o.value(QLatin1String("display")).toObject();
  m.display.layout = d.value(QLatin1String("layout")).toString(QStringLiteral("single"));
  m.display.gap = d.value(QLatin1String("gap")).toInt(0);
  for (const QJsonValue& v : d.value(QLatin1String("screens")).toArray()) {
    const QJsonObject s = v.toObject();
    ScreenSpec sp{s.value(QLatin1String("id")).toString(), s.value(QLatin1String("width")).toInt(),
                  s.value(QLatin1String("height")).toInt(), s.value(QLatin1String("touch")).toBool(false)};
    sp.align = s.value(QLatin1String("align")).toString(QStringLiteral("start"));
    if (sp.width <= 0 || sp.height <= 0) return fail(QStringLiteral("display.screens: invalid size"));
    if (sp.align != QLatin1String("start") && sp.align != QLatin1String("center") && sp.align != QLatin1String("end"))
      return fail(QStringLiteral("display.screens: unknown align: ") + sp.align);
    m.display.screens.append(sp);
  }
  if (m.display.screens.isEmpty()) return fail(QStringLiteral("Missing required field: display.screens"));
  if (m.display.layout != QLatin1String("single") && m.display.layout != QLatin1String("vertical") &&
      m.display.layout != QLatin1String("horizontal"))
    return fail(QStringLiteral("display.layout unknown: ") + m.display.layout);

  return m;
}

std::optional<CoreProfile> ManifestRegistry::parseProfile(const QByteArray& json, QString* error) {
  auto fail = [&](const QString& m) -> std::optional<CoreProfile> {
    if (error) *error = m;
    return std::nullopt;
  };
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) return fail(QStringLiteral("Invalid JSON: ") + pe.errorString());
  const QJsonObject o = doc.object();  // "_source" and other unknown keys are ignored
  CoreProfile p;
  p.coreId = o.value(QLatin1String("core_id")).toString().trimmed();
  p.libraryBasename = o.value(QLatin1String("library_basename")).toString().trimmed();
  if (p.coreId.isEmpty()) return fail(QStringLiteral("Missing required field: core_id"));
  if (p.libraryBasename.isEmpty()) return fail(QStringLiteral("Missing required field: library_basename"));
  p.aliases = stringList(o.value(QLatin1String("aliases")));
  p.displayName = o.value(QLatin1String("display_name")).toString(p.coreId);
  p.systemIds = stringList(o.value(QLatin1String("system_ids")));
  p.order = o.value(QLatin1String("order")).toInt(100);
  if (p.systemIds.isEmpty()) return fail(QStringLiteral("Missing required field: system_ids"));
  const QJsonObject co = o.value(QLatin1String("core_options")).toObject();
  for (auto it = co.begin(); it != co.end(); ++it) p.coreOptions.insert(it.key(), it.value().toString());
  p.lockedCoreOptions = stringList(o.value(QLatin1String("locked_core_options")));
  p.alwaysShownCoreOptions = stringList(o.value(QLatin1String("always_shown_core_options")));
  p.requiresHwRender = o.value(QLatin1String("requires_hw_render")).toBool(false);
  const QJsonObject rend = o.value(QLatin1String("renderer")).toObject();
  p.rendererOption = rend.value(QLatin1String("option")).toString().trimmed();
  const QJsonObject rv = rend.value(QLatin1String("values")).toObject();
  for (auto it = rv.begin(); it != rv.end(); ++it) p.rendererValues.insert(it.key(), it.value().toString());
  if (p.rendererOption.isEmpty()) p.rendererValues.clear();
  const QJsonObject save = o.value(QLatin1String("save")).toObject();
  p.saveSource = save.value(QLatin1String("source")).toString(p.saveSource);
  p.saveExtension = save.value(QLatin1String("extension")).toString(p.saveExtension);
  p.saveFormat = save.value(QLatin1String("format")).toString(p.saveFormat);
  p.saveShortDir = save.value(QLatin1String("short_dir")).toBool(false);
  p.saveShortDirSubfolder = save.value(QLatin1String("short_dir_subfolder")).toString();
  if (p.saveShortDirSubfolder.contains(QLatin1Char('/')) || p.saveShortDirSubfolder.contains(QLatin1Char('\\')) || p.saveShortDirSubfolder == QLatin1String("..")) {
    return fail(QStringLiteral("save: invalid short_dir_subfolder"));
  }
  if ((p.saveSource != QLatin1String("save_ram") && p.saveSource != QLatin1String("core_file") && p.saveSource != QLatin1String("core_tree") &&
       p.saveSource != QLatin1String("none")) || !p.saveExtension.startsWith(QLatin1Char('.')) ||
      p.saveExtension.contains(QLatin1Char('/')) || p.saveExtension.contains(QLatin1Char('\\')) || p.saveExtension.size() < 2) {
    return fail(QStringLiteral("save: invalid source or extension"));
  }
  if (p.saveSource == QLatin1String("core_tree") && p.saveFormat != QLatin1String("azahar_sdmc")) {
    return fail(QStringLiteral("save: core_tree needs format azahar_sdmc"));
  }
  const QJsonObject fw = o.value(QLatin1String("firmware")).toObject();
  p.sysfileOption = fw.value(QLatin1String("sysfile_option")).toString();
  p.sysfileNative = fw.value(QLatin1String("sysfile_native")).toString(p.sysfileNative);
  p.sysfileBuiltin = fw.value(QLatin1String("sysfile_builtin")).toString(p.sysfileBuiltin);
  const QJsonObject fo = fw.value(QLatin1String("file_options")).toObject();
  for (auto it = fo.begin(); it != fo.end(); ++it) p.fileOptions.insert(it.key(), it.value().toString());
  const QJsonObject states = o.value(QLatin1String("states")).toObject();
  p.statesSupported = states.value(QLatin1String("supported")).toBool(false);
  p.statesRunAhead = states.value(QLatin1String("run_ahead")).toBool(false);
  if (p.statesRunAhead && !p.statesSupported) return fail(QStringLiteral("states: run_ahead needs supported"));
  const QJsonValue fv = o.value(QLatin1String("cheats")).toObject().value(QLatin1String("formats"));
  auto addFormats = [&](const QStringList& list, QStringList* into) -> bool {
    for (const QString& f : list) {
      if (!CoreProfile::knownCheatFormats().contains(f)) return false;
      if (!into->contains(f)) into->append(f);
    }
    return true;
  };
  if (fv.isObject()) {
    const QJsonObject fo2 = fv.toObject();
    for (auto it = fo2.begin(); it != fo2.end(); ++it) {
      if (!p.systemIds.contains(it.key())) return fail(QStringLiteral("cheats: unknown system: ") + it.key());
      QStringList per;
      if (!addFormats(stringList(it.value()), &per)) return fail(QStringLiteral("cheats: unknown format for ") + it.key());
      if (per.isEmpty()) continue;
      p.cheatFormatsBySystem.insert(it.key(), per);
      addFormats(per, &p.cheatFormats);
    }
  } else {
    for (const QString& f : stringList(fv))
      if (!addFormats({f}, &p.cheatFormats)) return fail(QStringLiteral("cheats: unknown format: ") + f);
  }
  return p;
}

bool ManifestRegistry::addProfile(const CoreProfile& profile, QString* error) {
  for (const QString& id : QStringList(profile.coreId) + profile.aliases) {
    if (this->profile(id)) {
      if (error) *error = QStringLiteral("Duplicate core_id or alias: ") + id;
      return false;
    }
  }
  m_profiles.append(profile);
  return true;
}

const CoreProfile* ManifestRegistry::profile(const QString& id) const {
  for (const CoreProfile& p : m_profiles)
    if (p.matches(id)) return &p;
  return nullptr;
}

QList<CoreProfile> ManifestRegistry::profilesForSystem(const QString& systemId) const {
  QList<CoreProfile> out;
  for (const CoreProfile& p : m_profiles)
    if (p.systemIds.contains(systemId)) out.append(p);
  std::stable_sort(out.begin(), out.end(), [](const CoreProfile& a, const CoreProfile& b) {
    return a.order != b.order ? a.order < b.order : a.coreId < b.coreId;
  });
  return out;
}

QString ManifestRegistry::canonicalCoreId(const QString& id) const {
  const CoreProfile* p = profile(id);
  return p ? p->coreId : id;
}

std::optional<SystemManifest> ManifestRegistry::resolve(const QString& systemId, const QString& coreId) const {
  const SystemManifest* base = find(systemId);
  if (!base) return std::nullopt;
  SystemManifest m = *base;
  const CoreProfile* p = profile(coreId);
  if (p && p->systemIds.contains(systemId)) {
    m.coreId = coreId;  // the id the Hub serves (may be a legacy alias): cache directory and provisioning use it
    m.coreAliases = QStringList(p->coreId) + p->aliases;
    m.coreAliases.removeAll(coreId);
    m.coreDisplayName = p->displayName;
    m.coreLibraryBasename = p->libraryBasename;
    m.saveSource = p->saveSource;
    m.saveExtension = p->saveExtension;
    m.saveFormat = p->saveFormat;
    m.saveShortDir = p->saveShortDir;
    m.saveShortDirSubfolder = p->saveShortDirSubfolder;
    m.requiresHwRender = p->requiresHwRender;
    m.rendererOption = p->rendererOption;
    m.rendererValues = p->rendererValues;
    m.coreOptions = p->coreOptions;
    m.lockedCoreOptions = p->lockedCoreOptions;
    m.alwaysShownCoreOptions = p->alwaysShownCoreOptions;
    m.firmware.sysfileOption = p->sysfileOption;
    m.firmware.sysfileNative = p->sysfileNative;
    m.firmware.sysfileBuiltin = p->sysfileBuiltin;
    for (FirmwareFile& f : m.firmware.files) f.coreOption = p->fileOptions.value(f.id);
    return m;
  }
  // Experimental (ADR 0020 D6): no defaults, no locks, the raw framebuffer as one screen, touch off, firmware only
  // placed into the system directory.
  m.coreId = coreId;
  m.coreDisplayName = coreId;
  m.coreLibraryBasename = coreId + QStringLiteral("_libretro");
  m.experimental = true;
  m.display = DisplayProfile{QStringLiteral("single"), 0, {}};
  return m;
}

QList<SystemManifest> ManifestRegistry::all() const {
  QList<SystemManifest> out = m_manifests;
  std::stable_sort(out.begin(), out.end(), [](const SystemManifest& a, const SystemManifest& b) {
    return a.order != b.order ? a.order < b.order : a.systemId < b.systemId;
  });
  return out;
}

bool ManifestRegistry::add(const SystemManifest& manifest, QString* error) {
  if (find(manifest.systemId)) {
    if (error) *error = QStringLiteral("Duplicate system_id: ") + manifest.systemId;
    return false;
  }
  if (!manifest.inputProfile.isEmpty()) {
    for (const SystemManifest& o : m_manifests) {
      if (o.inputProfile == manifest.inputProfile && (o.inputMap != manifest.inputMap || o.inputLabels != manifest.inputLabels)) {
        if (error) *error = QStringLiteral("Conflicting input maps for input_profile ") + manifest.inputProfile + QStringLiteral(" (") + o.systemId + QStringLiteral(")");
        return false;
      }
    }
  }
  // Every path that loads manifests (builtin, directory, add) fills the input tables of the input library.
  if (!manifest.inputProfile.isEmpty() && !input::registerInputProfile(manifest.inputProfile, manifest.inputMap, manifest.inputLabels, error))
    return false;
  m_manifests.append(manifest);
  return true;
}

bool ManifestRegistry::loadDirectory(const QString& dir, QString* error) {
  for (const bool systems : {true, false}) {
    const QDir d(dir + (systems ? QStringLiteral("/systems") : QStringLiteral("/cores")));
    const QStringList files = d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& name : files) {
      QFile f(d.filePath(name));
      if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = name + QStringLiteral(": ") + f.errorString();
        return false;
      }
      QString err;
      bool ok = false;
      if (systems) {
        const auto m = parse(f.readAll(), &err);
        ok = m && add(*m, &err);
      } else {
        const auto p = parseProfile(f.readAll(), &err);
        ok = p && addProfile(*p, &err);
      }
      if (!ok) {
        if (error) *error = name + QStringLiteral(": ") + err;
        return false;
      }
    }
  }
  return true;
}

bool ManifestRegistry::loadBuiltin(QString* error) {
  initManifestResources();
  return loadDirectory(QString::fromLatin1(kBuiltinDir), error);
}

const SystemManifest* ManifestRegistry::find(const QString& systemId) const {
  for (const SystemManifest& m : m_manifests)
    if (m.systemId == systemId) return &m;
  return nullptr;
}

const SystemManifest* ManifestRegistry::forExtension(const QString& ext) const {
  for (const SystemManifest& m : m_manifests)
    if (m.supportsExtension(ext)) return &m;
  return nullptr;
}

}  // namespace uzume::emu
