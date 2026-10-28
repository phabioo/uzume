#include "plugin_manager.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <utility>
#include <functional>
#include <set>

#include "updatesig.h"

Q_LOGGING_CATEGORY(lcManager, "uzume.plugins.manager")

namespace uzume::plugins {

using update::SemVer;

namespace {

constexpr qint64 kMaxInstallBytes = 256ll * 1024 * 1024;
constexpr int kMaxInstallFiles = 20000;

// Copies src into dst (symlinks and ".git" are skipped). Returns an error text, empty on success.
QString copyTree(const QString& src, const QString& dst, qint64& bytes, int& files) {
  if (!QDir().mkpath(dst)) return QStringLiteral("cannot create %1").arg(dst);
  const QDir sd(src);
  const QFileInfoList entries = sd.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  for (const QFileInfo& e : entries) {
    if (e.isSymLink() || e.fileName() == QLatin1String(".git")) continue;
    const QString target = QDir(dst).filePath(e.fileName());
    if (e.isDir()) {
      const QString err = copyTree(e.absoluteFilePath(), target, bytes, files);
      if (!err.isEmpty()) return err;
    } else {
      bytes += e.size();
      if (bytes > kMaxInstallBytes || ++files > kMaxInstallFiles) return QStringLiteral("the plugin folder is too large (limit 256 MiB / 20000 files)");
      if (!QFile::copy(e.absoluteFilePath(), target)) return QStringLiteral("cannot copy %1").arg(e.fileName());
    }
  }
  return {};
}

const QString kOriginFile = QStringLiteral("origin.json");

struct OriginInfo {
  QString origin;  // "official" | "unverified" | "local" | empty when unreadable
  QString sha256;
};

OriginInfo readOrigin(const QString& dir) {
  QFile f(QDir(dir).filePath(kOriginFile));
  if (!f.open(QIODevice::ReadOnly)) return {};
  const QJsonObject o = QJsonDocument::fromJson(f.read(64 * 1024)).object();
  OriginInfo i;
  i.origin = o.value(QLatin1String("origin")).toString();
  i.sha256 = o.value(QLatin1String("sha256")).toString().toLower();
  if (i.origin != QLatin1String("official") && i.origin != QLatin1String("unverified") && i.origin != QLatin1String("local")) i.origin.clear();
  return i;
}

// Written by the Player after the checks (and over anything a plugin or folder brought along under that name).
bool writeOrigin(const QString& dir, const QString& origin, const QString& sha256, const QString& source) {
  QJsonObject o{{QStringLiteral("origin"), origin}, {QStringLiteral("source"), source}};
  if (!sha256.isEmpty()) o.insert(QStringLiteral("sha256"), sha256);
  QSaveFile f(QDir(dir).filePath(kOriginFile));
  if (!f.open(QIODevice::WriteOnly)) return false;
  f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
  return f.commit();
}

// SemVer text without build metadata; empty when it is not a valid version. Also the folder name below hub/<id>/.
QString normVersion(const QString& v) {
  const auto sv = SemVer::parse(v.trimmed());
  return sv ? sv->toString() : QString();
}

bool validPluginId(const QString& id) {
  static const QRegularExpression re(QStringLiteral("^[a-z0-9-]+(\\.[a-z0-9-]+)+$"));
  return re.match(id).hasMatch();
}

// Puts `tmp` in place of `dest`; the previous `dest` is restored when the move fails.
bool swapIn(const QString& tmp, const QString& dest) {
  const QFileInfo di(dest);
  const QString old = di.absolutePath() + QStringLiteral("/.old-") + di.fileName();
  QDir(old).removeRecursively();
  if (di.exists() && !QDir().rename(dest, old)) return false;
  if (!QDir().rename(tmp, dest)) {
    if (QFileInfo::exists(old)) QDir().rename(old, dest);
    return false;
  }
  QDir(old).removeRecursively();
  return true;
}

}  // namespace

PluginManager::PluginManager(const QString& rootDir, QObject* parent)
    : QObject(parent), m_root(QDir::cleanPath(rootDir)), m_keys(update::trustedKeysFromEnvironment()) {
  m_debounce.setSingleShot(true);
  m_debounce.setInterval(300);
  connect(&m_debounce, &QTimer::timeout, this, &PluginManager::flushChanged);
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &PluginManager::onWatchTriggered);
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &PluginManager::onWatchTriggered);
  loadState();
  scan();
}

void PluginManager::loadState() {
  QFile f(m_root + QStringLiteral("/state.json"));
  if (!f.open(QIODevice::ReadOnly)) return;
  const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
  const QJsonObject en = o.value(QLatin1String("enabled")).toObject();
  for (auto it = en.begin(); it != en.end(); ++it) m_enabled.insert(it.key(), it.value().toBool());
  const QJsonObject ln = o.value(QLatin1String("linked")).toObject();
  for (auto it = ln.begin(); it != ln.end(); ++it)
    if (it.value().isString()) m_linked.insert(it.key(), it.value().toString());
}

void PluginManager::saveState() const {
  QJsonObject en, ln;
  for (auto it = m_enabled.begin(); it != m_enabled.end(); ++it) en.insert(it.key(), it.value());
  for (auto it = m_linked.begin(); it != m_linked.end(); ++it) ln.insert(it.key(), it.value());
  QJsonObject o;
  o.insert(QStringLiteral("enabled"), en);
  o.insert(QStringLiteral("linked"), ln);
  QDir().mkpath(m_root);
  QSaveFile f(m_root + QStringLiteral("/state.json"));
  if (f.open(QIODevice::WriteOnly)) {
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.commit();
  }
}

PluginManager::Entry PluginManager::makeEntry(const QString& dir, const QString& fallbackId, bool devLinked) const {
  Entry e;
  const ManifestResult m = loadManifest(dir);
  e.valid = m.ok;
  if (m.ok) {
    e.info.manifest = m.manifest;
    for (const QString& w : m.manifest.warnings) qCWarning(lcManager) << "plugin" << m.manifest.id << "manifest:" << w;
  } else {
    e.info.manifest.id = fallbackId;
    e.info.manifest.name = fallbackId;
    e.info.problems << m.error;
  }
  e.info.path = dir;
  e.info.cacheDir = m_root + QStringLiteral("/cache/") + e.info.manifest.id;
  e.info.enabled = m_enabled.value(e.info.manifest.id, false);
  e.info.devLinked = devLinked;
  e.info.origin = QStringLiteral("local");
  return e;
}

void PluginManager::scan() {
  m_entries.clear();
  m_hub.clear();
  const QDir inst(m_root + QStringLiteral("/installed"));
  const QFileInfoList dirs = inst.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& d : dirs) {
    if (d.fileName().startsWith(QLatin1Char('.'))) continue;
    Entry e = makeEntry(d.absoluteFilePath(), d.fileName(), false);
    if (readOrigin(d.absoluteFilePath()).origin == QLatin1String("official")) e.info.origin = QStringLiteral("official");
    m_entries.insert(e.info.manifest.id, e);
  }
  scanHub();
  // Outside runs: an installed copy beats a Hub copy; of several Hub copies the newest version is active.
  for (auto it = m_hub.begin(); it != m_hub.end(); ++it) {
    if (m_entries.contains(it.key()) || it->isEmpty()) continue;
    const Entry* best = nullptr;
    for (auto v = it->begin(); v != it->end(); ++v)
      if (best == nullptr || SemVer::compare(v->info.manifest.version, best->info.manifest.version) > 0) best = &v.value();
    m_entries.insert(it.key(), *best);
  }
  for (auto it = m_linked.begin(); it != m_linked.end(); ++it) {
    Entry e = makeEntry(it.value(), it.key(), true);
    m_entries.insert(e.info.manifest.id, e);  // a linked folder overrides an installed copy of the same id
  }
  refreshWatcher();
}

void PluginManager::scanHub() {
  const QDir hub(m_root + QStringLiteral("/hub"));
  const QFileInfoList ids = hub.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& idDir : ids) {
    if (idDir.fileName().startsWith(QLatin1Char('.')) || !validPluginId(idDir.fileName())) continue;
    const QFileInfoList vers = QDir(idDir.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& vDir : vers) {
      if (vDir.fileName().startsWith(QLatin1Char('.'))) continue;
      Entry e = makeEntry(vDir.absoluteFilePath(), idDir.fileName(), false);
      const OriginInfo o = readOrigin(vDir.absoluteFilePath());
      // Anything that is not exactly what the Player installed (id/version of the folder, readable origin) is ignored.
      if (!e.valid || e.info.manifest.id != idDir.fileName() || normVersion(e.info.manifest.version.toString()) != vDir.fileName()) continue;
      if ((o.origin != QLatin1String("official") && o.origin != QLatin1String("unverified")) || o.sha256.size() != 64) continue;
      e.info.origin = o.origin;
      e.info.hubCopy = true;
      e.info.sha256 = o.sha256;
      m_hub[idDir.fileName()].insert(vDir.fileName(), e);
    }
  }
}

void PluginManager::setPlayerVersion(const QString& version) {
  if (m_playerVersion == version) return;
  m_playerVersion = version;
  emit pluginsChanged();
}

QList<QPair<QString, QString>> PluginManager::pluginsNeedingNewerPlayer(const QString& version) const {
  QList<QPair<QString, QString>> out;
  const auto target = SemVer::parse(version);
  if (!target) return out;
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    if (!it->valid || !it->info.enabled || !it->info.manifest.minPlayer) continue;
    if (SemVer::compare(*target, *it->info.manifest.minPlayer) < 0) {
      out.append({it->info.manifest.name, it->info.manifest.minPlayer->toString()});
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

void PluginManager::computeDependencyProblems(QMap<QString, QStringList>& out) const {
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    if (!it->valid) continue;
    const Manifest& m = it->info.manifest;
    if (m.minPlayer) {
      const auto player = SemVer::parse(m_playerVersion);
      if (player && SemVer::compare(*player, *m.minPlayer) < 0) out[m.id] << QStringLiteral("Needs Player %1").arg(m.minPlayer->toString());
    }
    // variants.build starts a program on this PC: signed official plugins only (ADR 0031 D5); a local plugin (installed from a folder or
    // an unsigned package, or linked from a developer folder) may use it while Developer mode is on.
    if (m.hasPermission(QStringLiteral("variants.build")) && it->info.origin != QLatin1String("official") &&
        !(m_devMode && (it->info.origin == QLatin1String("local") || it->info.devLinked)))
      out[m.id] << QStringLiteral("permission variants.build needs a signed official plugin or Developer mode");
    for (const Dependency& d : m.dependencies) {
      auto dep = m_entries.constFind(d.id);
      if (dep == m_entries.constEnd() || !dep->valid) {
        out[m.id] << QStringLiteral("needs %1, not installed").arg(d.id);
      } else if (!d.range.matches(dep->info.manifest.version)) {
        out[m.id] << QStringLiteral("needs %1 %2 · you have %3").arg(dep->info.manifest.name, d.range.text, dep->info.manifest.version.toString());
      }
    }
  }
  // Cycles: a node is on a cycle when it can reach itself.
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    if (!it->valid) continue;
    const QString start = it.key();
    QStringList path{start};
    std::set<QString> seen;
    std::function<bool(const QString&)> dfs = [&](const QString& cur) -> bool {
      auto e = m_entries.constFind(cur);
      if (e == m_entries.constEnd() || !e->valid) return false;
      QStringList deps;
      for (const Dependency& d : e->info.manifest.dependencies) deps << d.id;
      deps.sort();
      for (const QString& d : std::as_const(deps)) {
        if (d == start) {
          path << d;
          return true;
        }
        if (seen.count(d)) continue;
        seen.insert(d);
        path << d;
        if (dfs(d)) return true;
        path.removeLast();
      }
      return false;
    };
    seen.insert(start);
    if (dfs(start)) out[start] << QStringLiteral("dependency cycle: %1").arg(path.join(QStringLiteral(" -> ")));
  }
  // A plugin that needs a plugin with problems cannot work either.
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
      if (!it->valid) continue;
      for (const Dependency& d : it->info.manifest.dependencies) {
        auto dep = m_entries.constFind(d.id);
        if (dep == m_entries.constEnd() || !dep->valid) continue;
        const bool depBad = !out.value(d.id).isEmpty();
        const QString msg = QStringLiteral("needs %1, which has problems").arg(dep->info.manifest.name);
        if (depBad && !out.value(it.key()).contains(msg)) {
          out[it.key()] << msg;
          changed = true;
        }
      }
    }
  }
}

QList<PluginInfo> PluginManager::list() const {
  QMap<QString, QStringList> deps;
  computeDependencyProblems(deps);
  QList<PluginInfo> out;
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    PluginInfo i = it->info;
    i.problems << deps.value(it.key());
    out.append(i);
  }
  return out;
}

std::optional<PluginInfo> PluginManager::info(const QString& id) const {
  for (const PluginInfo& i : list())
    if (i.manifest.id == id) return i;
  return std::nullopt;
}

InstallResult PluginManager::installFromFolder(const QString& srcDir) {
  InstallResult r;
  const QFileInfo sfi(srcDir);
  if (!sfi.isDir()) {
    r.error = QStringLiteral("%1 is not a folder").arg(srcDir);
    return r;
  }
  const ManifestResult m = loadManifest(srcDir);
  if (!m.ok) {
    r.error = m.error;
    return r;
  }
  r.id = m.manifest.id;
  const QString dest = m_root + QStringLiteral("/installed/") + r.id;
  if (QFileInfo(dest).exists() && QFileInfo(dest).canonicalFilePath() == sfi.canonicalFilePath()) {
    r.error = QStringLiteral("this plugin is already installed from that folder");
    return r;
  }
  if (!QDir().mkpath(m_root + QStringLiteral("/installed"))) {
    r.error = QStringLiteral("cannot create the plugin folder");
    return r;
  }
  const QString tmp = m_root + QStringLiteral("/installed/.tmp-") + r.id;
  QDir(tmp).removeRecursively();
  qint64 bytes = 0;
  int files = 0;
  const QString err = copyTree(sfi.absoluteFilePath(), tmp, bytes, files);
  if (!err.isEmpty()) {
    QDir(tmp).removeRecursively();
    r.error = err;
    return r;
  }
  if (!loadManifest(tmp).ok) {  // the copy must be valid too (e.g. entry was a symlink)
    const QString e2 = loadManifest(tmp).error;
    QDir(tmp).removeRecursively();
    r.error = e2;
    return r;
  }
  if (!writeOrigin(tmp, QStringLiteral("local"), QString(), QStringLiteral("folder"))) {
    QDir(tmp).removeRecursively();
    r.error = QStringLiteral("cannot write the plugin's origin");
    return r;
  }
  if (!swapIn(tmp, dest)) {
    QDir(tmp).removeRecursively();
    r.error = QStringLiteral("cannot move the plugin into place");
    return r;
  }
  scan();
  saveState();
  r.ok = true;
  r.version = m.manifest.version.toString();
  r.origin = QStringLiteral("local");
  emit pluginsChanged();
  return r;
}

InstallResult PluginManager::installPackage(const UzPluginPackage& pkg, const QString& dest, const QString& tmp, const QString& origin,
                                            const QString& sourceSha, const QString& signatureNote) {
  InstallResult r;
  r.id = pkg.manifest.id;
  const bool hadPrev = m_entries.contains(r.id);
  const QStringList prevPerms = hadPrev ? m_entries.value(r.id).info.manifest.permissions : QStringList();
  QDir(tmp).removeRecursively();
  QDir().mkpath(QFileInfo(tmp).absolutePath());
  QDir().mkpath(QFileInfo(dest).absolutePath());
  QString err;
  if (!extractUzPlugin(pkg, tmp, &err)) {
    QDir(tmp).removeRecursively();
    r.error = err;
    return r;
  }
  if (const ManifestResult chk = loadManifest(tmp); !chk.ok) {  // the unpacked copy must be valid too
    QDir(tmp).removeRecursively();
    r.error = chk.error;
    return r;
  }
  const bool fromHub = !sourceSha.isEmpty();
  if (!writeOrigin(tmp, origin, sourceSha, fromHub ? QStringLiteral("hub") : QStringLiteral("file"))) {
    QDir(tmp).removeRecursively();
    r.error = QStringLiteral("cannot write the plugin's origin");
    return r;
  }
  if (!swapIn(tmp, dest)) {
    QDir(tmp).removeRecursively();
    r.error = QStringLiteral("cannot move the plugin into place");
    return r;
  }
  scan();
  // An update that asks for permissions the replaced copy did not have is turned off until the user turns it on again.
  if (hadPrev && m_enabled.value(r.id, false) && QDir::cleanPath(m_entries.value(r.id).info.path) == QDir::cleanPath(dest)) {
    for (const QString& p : pkg.manifest.permissions)
      if (!prevPerms.contains(p)) r.addedPermissions << p;
    if (!r.addedPermissions.isEmpty()) {
      m_enabled.insert(r.id, false);
      scan();
    }
  }
  // A renamed plugin (manifest `replaces`) takes over the person's choice for the old id once: turned on when the old id was
  // on and the new one asks for no permission the old one lacked (otherwise it stays off and the normal consent flow asks).
  // An old id that was on is always turned off, so the two never run side by side.
  if (!pkg.manifest.replaces.isEmpty()) {
    const bool firstTime = !m_enabled.contains(r.id);
    bool takeOver = false;
    // The active copy of the new id (an installed or linked copy outranks a Hub copy) is what gets turned on, so its
    // permissions decide, not those of the package just installed.
    const auto activeIt = m_entries.constFind(r.id);
    const QStringList newPerms = activeIt != m_entries.constEnd() ? activeIt->info.manifest.permissions : pkg.manifest.permissions;
    for (const QString& oldId : pkg.manifest.replaces) {
      if (oldId == r.id || !m_enabled.value(oldId, false)) continue;
      if (firstTime && m_entries.contains(oldId)) {
        const QStringList oldPerms = m_entries.value(oldId).info.manifest.permissions;
        bool covered = true;
        for (const QString& p : newPerms)
          if (!oldPerms.contains(p)) covered = false;
        if (covered) takeOver = true;
      }
      m_enabled.insert(oldId, false);
      auto oit = m_entries.find(oldId);
      if (oit != m_entries.end()) oit->info.enabled = false;
    }
    auto it = m_entries.find(r.id);
    if (takeOver && it != m_entries.end() && it->info.problems.isEmpty()) {
      m_enabled.insert(r.id, true);
      it->info.enabled = true;
    }
  }
  saveState();
  r.ok = true;
  r.version = pkg.manifest.version.toString();
  r.origin = origin;
  r.signatureNote = signatureNote;
  emit pluginsChanged();
  return r;
}

InstallResult PluginManager::installFromFile(const QString& packagePath) {
  InstallResult r;
  QFile f(packagePath);
  if (!f.exists()) {
    r.error = QStringLiteral("%1 does not exist").arg(packagePath);
    return r;
  }
  if (f.size() > kUzPluginMaxFileBytes) {
    r.error = QStringLiteral("the plugin file is larger than %1 MiB").arg(kUzPluginMaxFileBytes >> 20);
    return r;
  }
  if (!f.open(QIODevice::ReadOnly)) {
    r.error = QStringLiteral("cannot read %1").arg(packagePath);
    return r;
  }
  const UzPluginResult pr = readUzPlugin(f.readAll(), m_keys);
  if (!pr.ok) {
    r.error = pr.error;
    return r;
  }
  const QString id = pr.package.manifest.id;
  const QString origin = pr.package.official() ? QStringLiteral("official") : QStringLiteral("local");
  const QString note = pr.package.signature == SignatureState::Invalid ? pr.package.signatureError : QString();
  return installPackage(pr.package, m_root + QStringLiteral("/installed/") + id, m_root + QStringLiteral("/installed/.tmp-") + id, origin, QString(), note);
}

InstallResult PluginManager::installFromPath(const QString& path) {
  return QFileInfo(path).isDir() ? installFromFolder(path) : installFromFile(path);
}

InstallResult PluginManager::installHubCopy(const QByteArray& packageBytes, const QString& pluginId, const QString& version, const QString& sha256) {
  InstallResult r;
  r.id = pluginId;
  const QString nv = normVersion(version);
  const QString pin = sha256.toLower();
  if (!validPluginId(pluginId) || nv.isEmpty()) {
    r.error = QStringLiteral("invalid plugin id or version from the server");
    return r;
  }
  if (QString::fromLatin1(QCryptographicHash::hash(packageBytes, QCryptographicHash::Sha256).toHex()) != pin) {
    r.error = QStringLiteral("the downloaded file does not match the SHA-256 the server announced");
    return r;
  }
  const UzPluginResult pr = readUzPlugin(packageBytes, m_keys);
  if (!pr.ok) {
    r.error = pr.error;
    return r;
  }
  if (pr.package.manifest.id != pluginId || pr.package.manifest.version.toString() != nv) {
    r.error = QStringLiteral("the file is %1 %2, not %3 %4").arg(pr.package.manifest.id, pr.package.manifest.version.toString(), pluginId, nv);
    return r;
  }
  const QString origin = pr.package.official() ? QStringLiteral("official") : QStringLiteral("unverified");
  const QString note = pr.package.signature == SignatureState::Invalid ? pr.package.signatureError : QString();
  return installPackage(pr.package, m_root + QStringLiteral("/hub/") + pluginId + QLatin1Char('/') + nv,
                        m_root + QStringLiteral("/hub/.tmp-") + pluginId + QLatin1Char('-') + nv, origin, pin, note);
}

QString PluginManager::hubCopyDir(const QString& pluginId, const QString& version, const QString& sha256) const {
  const QString nv = normVersion(version);
  const auto idIt = m_hub.constFind(pluginId);
  if (nv.isEmpty() || idIt == m_hub.constEnd()) return {};
  const auto vIt = idIt->constFind(nv);
  if (vIt == idIt->constEnd() || !vIt->valid || vIt->info.sha256 != sha256.toLower() || !QFileInfo(vIt->info.path).isDir()) return {};
  return vIt->info.path;
}

QList<PluginInfo> PluginManager::hubCopies() const {
  QList<PluginInfo> out;
  for (auto it = m_hub.begin(); it != m_hub.end(); ++it)
    for (auto v = it->begin(); v != it->end(); ++v) out.append(v->info);
  return out;
}

InstallResult PluginManager::linkDevFolder(const QString& srcDir) {
  InstallResult r;
  const QFileInfo sfi(srcDir);
  if (!sfi.isDir()) {
    r.error = QStringLiteral("%1 is not a folder").arg(srcDir);
    return r;
  }
  const ManifestResult m = loadManifest(srcDir);
  if (!m.ok) {
    r.error = m.error;
    return r;
  }
  r.id = m.manifest.id;
  m_linked.insert(r.id, sfi.canonicalFilePath());
  scan();
  saveState();
  r.ok = true;
  emit pluginsChanged();
  return r;
}

bool PluginManager::uninstall(const QString& id) {
  if (m_linked.remove(id)) {
    scan();
    saveState();
    emit pluginsChanged();
    return true;
  }
  // Only plain ids below installed/ and hub/ (an id from the UI must never walk out of them).
  if (id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\')) || id.contains(QLatin1String(".."))) return false;
  bool any = false;
  for (const QString& sub : {QStringLiteral("/installed/"), QStringLiteral("/hub/")}) {
    const QString dest = m_root + sub + id;
    if (!QFileInfo(dest).isDir()) continue;
    QDir(dest).removeRecursively();
    if (QFileInfo::exists(dest)) {
      // Something is still open or locked (Windows): do not report a half-removed plugin as uninstalled.
      scan();
      return false;
    }
    any = true;
  }
  if (!any) return false;
  QDir(m_root + QStringLiteral("/cache/") + id).removeRecursively();  // a cache that cannot be removed does not block
  m_enabled.remove(id);
  scan();
  saveState();
  emit pluginsChanged();
  return true;
}

bool PluginManager::setEnabled(const QString& id, bool enabled) {
  const std::optional<PluginInfo> i = info(id);
  if (!i) return false;
  if (enabled && !i->problems.isEmpty()) return false;  // "can't be turned on until ..." (design 4d-2)
  m_enabled.insert(id, enabled);
  auto it = m_entries.find(id);
  if (it != m_entries.end()) it->info.enabled = enabled;
  saveState();
  emit pluginsChanged();
  return true;
}

void PluginManager::setDeveloperMode(bool on) {
  if (m_devMode == on) return;
  m_devMode = on;
  emit pluginsChanged();
}

GameResolution PluginManager::resolveForGame(const QString& system, const QString& code) const {
  return resolveWhere([&](const Manifest& m) { return m.matchesGame(system, code); });
}

GameResolution PluginManager::resolveForRun(const QList<RunPluginPin>& pins) const {
  GameResolution res;
  QMap<QString, PluginInfo> infos;
  for (const RunPluginPin& pin : pins) {
    const QString dir = hubCopyDir(pin.id, pin.version, pin.sha256);
    if (dir.isEmpty()) {
      res.problems[pin.id] << QStringLiteral("the server copy %1 of this run is not on this device").arg(pin.version);
      continue;
    }
    PluginInfo i = m_hub.value(pin.id).value(normVersion(pin.version)).info;
    i.enabled = true;  // the run decides, not the toggle of Settings -> Plugins
    infos.insert(pin.id, i);
  }
  QSet<QString> blocked;
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto it = infos.begin(); it != infos.end(); ++it) {
      if (blocked.contains(it.key())) continue;
      for (const Dependency& d : it->manifest.dependencies) {
        const auto dep = infos.constFind(d.id);
        QString why;
        if (dep == infos.constEnd() || blocked.contains(d.id)) {
          why = QStringLiteral("needs %1, which is not part of this run or cannot start").arg(d.id);
        } else if (!d.range.matches(dep->manifest.version)) {
          why = QStringLiteral("needs %1 %2 · the run has %3").arg(d.id, d.range.text, dep->manifest.version.toString());
        }
        if (!why.isEmpty()) {
          blocked.insert(it.key());
          res.problems[it.key()] << why;
          changed = true;
          break;
        }
      }
    }
  }
  QSet<QString> done;
  QSet<QString> visiting;
  std::function<void(const QString&)> visit = [&](const QString& id) {
    if (done.contains(id) || blocked.contains(id) || !infos.contains(id)) return;
    if (visiting.contains(id)) {  // dependency cycle
      blocked.insert(id);
      res.problems[id] << QStringLiteral("dependency cycle");
      return;
    }
    visiting.insert(id);
    QStringList deps;
    for (const Dependency& d : infos[id].manifest.dependencies) deps << d.id;
    deps.sort();
    for (const QString& d : std::as_const(deps)) visit(d);
    visiting.remove(id);
    if (blocked.contains(id) || done.contains(id)) return;
    done.insert(id);
    res.start.append(infos[id]);
  };
  for (auto it = infos.begin(); it != infos.end(); ++it) visit(it.key());
  return res;
}

GameResolution PluginManager::resolveForShell() const {
  // A plugin with variants.build (Randomizer) runs in the shell too: it registers its presets and games there, and the game page and
  // New run read them while no game is running.
  return resolveWhere([](const Manifest& m) { return m.ui.screen || m.ui.detail || m.hasPermission(QStringLiteral("variants.build")); });
}

GameResolution PluginManager::resolveWhere(const std::function<bool(const Manifest&)>& wanted) const {
  GameResolution res;
  QMap<QString, QStringList> problems;
  computeDependencyProblems(problems);
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
    if (!it->valid) problems[it.key()] << it->info.problems;

  // Candidates and their transitive requirements.
  QStringList members;
  QStringList queue;
  QStringList candidates;
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
    if (it->valid && it->info.enabled && wanted(it->info.manifest)) queue << it.key();
  candidates = queue;
  while (!queue.isEmpty()) {
    const QString id = queue.takeFirst();
    if (members.contains(id)) continue;
    members << id;
    const Entry& e = m_entries[id];
    for (const Dependency& d : e.info.manifest.dependencies)
      if (m_entries.contains(d.id) && m_entries[d.id].valid) queue << d.id;
  }
  members.sort();

  std::set<QString> blocked;
  for (const QString& id : std::as_const(members)) {
    QStringList reasons = problems.value(id);
    if (!m_devMode && m_entries[id].info.origin == QLatin1String("local")) reasons << QStringLiteral("unsigned local plugin: turn on Developer mode to run it");
    if (!reasons.isEmpty()) {
      blocked.insert(id);
      res.problems[id] = reasons;
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const QString& id : std::as_const(members)) {
      if (blocked.count(id)) continue;
      for (const Dependency& d : m_entries[id].info.manifest.dependencies) {
        if (blocked.count(d.id)) {
          blocked.insert(id);
          res.problems[id] << QStringLiteral("needs %1, which cannot start").arg(m_entries[d.id].info.manifest.name);
          changed = true;
          break;
        }
      }
    }
  }

  std::set<QString> done;
  std::function<void(const QString&)> visit = [&](const QString& id) {
    if (done.count(id) || blocked.count(id) || !m_entries.contains(id)) return;
    done.insert(id);
    QStringList deps;
    for (const Dependency& d : m_entries[id].info.manifest.dependencies) deps << d.id;
    deps.sort();
    for (const QString& d : std::as_const(deps)) visit(d);
    PluginInfo i = m_entries[id].info;
    res.start.append(i);
  };
  for (const QString& id : std::as_const(candidates)) visit(id);  // only what a startable candidate needs
  return res;
}

// ---- developer-folder watching

void PluginManager::setWatchEnabled(bool on) {
  if (m_watchEnabled == on) return;
  m_watchEnabled = on;
  m_dirtyIds.clear();
  m_debounce.stop();
  refreshWatcher();
}

void PluginManager::refreshWatcher() {
  const QStringList old = m_watcher.files() + m_watcher.directories();
  if (!old.isEmpty()) m_watcher.removePaths(old);
  if (!m_watchEnabled) return;
  int budget = 5000;
  for (auto it = m_linked.begin(); it != m_linked.end(); ++it) {
    if (!QFileInfo(it.value()).isDir()) continue;
    m_watcher.addPath(it.value());
    if (!old.contains(it.value())) qDebug().noquote() << "watching" << it.value() << "for changes";
    QDirIterator di(it.value(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (di.hasNext() && budget-- > 0) {
      di.next();
      if (di.fileInfo().isSymLink() || di.filePath().contains(QLatin1String("/.git"))) continue;
      m_watcher.addPath(di.filePath());
    }
  }
}

void PluginManager::onWatchTriggered(const QString& path) {
  for (auto it = m_linked.begin(); it != m_linked.end(); ++it)
    if (path == it.value() || path.startsWith(it.value() + QLatin1Char('/'))) m_dirtyIds.insert(it.key());
  m_debounce.start();  // restarts the 300 ms window
}

void PluginManager::flushChanged() {
  const QSet<QString> ids = std::exchange(m_dirtyIds, {});
  scan();  // re-reads manifests and re-registers the watched files (editors often replace files)
  for (const QString& id : ids) emit pluginChanged(id);
  emit pluginsChanged();
}

}  // namespace uzume::plugins
