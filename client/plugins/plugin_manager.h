#pragma once

#include <QFileSystemWatcher>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

#include "fbplugin.h"
#include "manifest.h"

namespace uzume::plugins {

struct InstallResult {
  bool ok = false;
  QString id;
  QString error;                  // clear English message when !ok
  QString version;                // installed version (when ok)
  QString origin;                 // "official" | "unverified" | "local" (when ok)
  QString signatureNote;          // a present but invalid signature: why (the plugin is then never "official")
  QStringList addedPermissions;   // an update asks for permissions the replaced copy did not have: the plugin was turned off
};

// A plugin version of a run (the Hub's pin: id, version, SHA-256 of the .fbplugin).
struct RunPluginPin {
  QString id;
  QString version;
  QString sha256;
};

// Plugins to start for one game, in start order (dependencies first), and why other plugins were left out.
struct GameResolution {
  QList<PluginInfo> start;
  QMap<QString, QStringList> problems;  // plugin id -> reasons it is not started
};

// Installed plugins on disk: <root>/installed/<id>/ (installed from a folder or a .fbplugin file on this device),
// <root>/hub/<id>/<version>/ (copies delivered by the Hub, several versions side by side), <root>/cache/<id>/,
// <root>/state.json (enabled flags, linked dev folders). Every installed folder carries an origin.json written by the
// Player after the checks ("official": valid Uzume signature, wherever it came from; "unverified": unsigned copy
// offered by the Hub; "local": unsigned file or folder of this device). Only "local" plugins need developer mode.
// Per plugin id one copy is active outside runs: linked dev folder > installed/ > newest hub copy. A run uses exactly
// its Hub version through hubCopyDir() and a local copy never replaces it (docs/architecture/12-plugins.md 16.10).
// Single-threaded (UI thread).
class PluginManager : public QObject {
  Q_OBJECT
 public:
  explicit PluginManager(const QString& rootDir, QObject* parent = nullptr);

  QString rootDir() const { return m_root; }
  void scan();                                   // re-reads installed/ and linked folders
  QList<PluginInfo> list() const;                // sorted by id
  std::optional<PluginInfo> info(const QString& id) const;

  // Validates the manifest in `srcDir`, then copies the folder to installed/<id> (replacing a local copy of that id).
  // A new install is turned off. A folder is always "local" (a SIGNATURE.json inside it is not looked at).
  InstallResult installFromFolder(const QString& srcDir);
  // Installs a .fbplugin file from this device to installed/<id>: signed by a trusted key -> "official", otherwise
  // "local". A new install is turned off.
  InstallResult installFromFile(const QString& fbpluginPath);
  // Folder or .fbplugin file, by what `path` is.
  InstallResult installFromPath(const QString& path);
  // Installs the bytes of a .fbplugin downloaded from the Hub as hub/<id>/<version>/. Rejected (nothing changes) when
  // the SHA-256 of `fbplugin` differs from `sha256` or the file is not plugin `pluginId` at `version`. Valid
  // signature -> "official", otherwise "unverified". An existing copy of that version is replaced only after success.
  InstallResult installHubCopy(const QByteArray& fbplugin, const QString& pluginId, const QString& version, const QString& sha256);
  // Directory of the Hub copy (id, version) that was installed from exactly the file with `sha256`; empty if absent.
  QString hubCopyDir(const QString& pluginId, const QString& version, const QString& sha256) const;
  // Every Hub copy on disk (all versions), sorted by id and version.
  QList<PluginInfo> hubCopies() const;
  // Trusted Ed25519 keys (raw 32 bytes) for plugin signatures; default = compiled-in release key (+ env keys).
  void setTrustedKeys(const QList<QByteArray>& keys) { m_keys = keys; }
  // Runs straight from `srcDir` and reloads on file changes (~300 ms debounce). Does not copy.
  InstallResult linkDevFolder(const QString& srcDir);
  // "Reload on file change" (developer console): whether linked folders are watched. On by default; off keeps the links and
  // stops the watching (a debug log line "watching <folder> for changes" is written for every folder when it is on).
  void setWatchEnabled(bool on);
  bool watchEnabled() const { return m_watchEnabled; }
  // Removes the installed copy and its cache, or only the link of a dev-linked folder (the folder is never deleted).
  bool uninstall(const QString& id);
  bool setEnabled(const QString& id, bool enabled);
  void setDeveloperMode(bool on);
  bool developerMode() const { return m_devMode; }
  // The version of the running Player, compared with the manifest field `min_player` (default empty: not checked, e.g. development builds). A plugin that
  // needs a newer Player has the problem "Needs Player <version>": it is off (kept enabled) until the Player is updated.
  void setPlayerVersion(const QString& version);
  QString playerVersion() const { return m_playerVersion; }
  // Display names of the installed plugins that are turned on and need a Player newer than `version` (the rollback
  // dialog: "X needs 0.16.4 and is turned off until you update."), as {name, min_player} pairs sorted by name.
  QList<QPair<QString, QString>> pluginsNeedingNewerPlayer(const QString& version) const;

  // Enabled plugins whose games match, plus (transitively) the plugins they require, in start order. Plugins with
  // problems are listed in `problems` and not started, nor are plugins that depend on them.
  GameResolution resolveForGame(const QString& system, const QString& code) const;
  // A game started in a run: exactly the Hub copies the run pins (id + version + SHA-256), in start order, whatever is
  // enabled, linked or installed locally. A pin whose Hub copy is not on this device, or whose dependency is not part of
  // the run, is listed in `problems` and not started.
  GameResolution resolveForRun(const QList<RunPluginPin>& pins) const;
  // Shell context (no game runs): enabled plugins that declare ui.screen or ui.detail, plus what they require.
  GameResolution resolveForShell() const;

 signals:
  void pluginsChanged();                       // installed/linked/enabled set changed
  void pluginChanged(const QString& id);       // files of a dev-linked plugin changed: reload it

 private:
  struct Entry {
    PluginInfo info;
    bool valid = false;
  };
  void loadState();
  void saveState() const;
  void refreshWatcher();
  void onWatchTriggered(const QString& path);
  void flushChanged();
  Entry makeEntry(const QString& dir, const QString& fallbackId, bool devLinked) const;
  InstallResult installPackage(const FbPluginPackage& pkg, const QString& dest, const QString& tmp, const QString& origin,
                               const QString& sourceSha, const QString& signatureNote);
  void scanHub();
  GameResolution resolveWhere(const std::function<bool(const Manifest&)>& wanted) const;
  void computeDependencyProblems(QMap<QString, QStringList>& out) const;

  QString m_root;
  bool m_devMode = false;
  bool m_watchEnabled = true;
  QString m_playerVersion;
  QMap<QString, bool> m_enabled;
  QMap<QString, QString> m_linked;   // plugin id -> folder
  QMap<QString, Entry> m_entries;                    // the active copy per plugin id
  QMap<QString, QMap<QString, Entry>> m_hub;         // hub copies: id -> normalized version -> entry
  QList<QByteArray> m_keys;
  QFileSystemWatcher m_watcher;
  QTimer m_debounce;
  QSet<QString> m_dirtyIds;
};

}  // namespace uzume::plugins
