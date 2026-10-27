#pragma once
// PluginStore: the local copy of the plugin user store (fb.store, permission `store`). One instance per person and server,
// kept in a directory of the app's data dir (one JSON file per plugin id). Reads and writes never wait for the network;
// PluginStoreSync (network/) pushes the dirty keys to the server and pulls newer ones. Thread-safe.
//
// The revision a plugin sees is a local counter (+1 per write, 0 = the key does not exist). The server's own revision of a
// key is kept next to it (serverRev) and only used for the sync. A conflict keeps both versions: the local value stays the
// current one, the other one (a stale write of the plugin, or the server's version) is listed in conflicts(); the sync holds
// a key back while it has conflicts, and the next successful put of the plugin resolves them.
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <functional>
#include <mutex>
#include <optional>

namespace uzume::plugins {

inline constexpr int kStoreMaxValueBytes = 256 * 1024;
inline constexpr int kStoreMaxKeys = 1024;
inline constexpr qint64 kStoreMaxBytes = 16ll * 1024 * 1024;
inline constexpr int kStoreMaxConflicts = 8;

class PluginStore {
 public:
  struct Conflict {
    QJsonValue value;
    qint64 revision = 0;  // local: the base revision the plugin gave; server: the server's revision
    QString source;       // "local" | "server"
  };
  struct Entry {
    bool found = false;
    QJsonValue value;
    qint64 revision = 0;
  };
  struct PutResult {
    enum class Kind { Ok, Conflict, TooLarge, Quota, Invalid };
    Kind kind = Kind::Ok;
    qint64 revision = 0;
  };
  struct Dirty {
    QString pluginId;
    QString key;
    QJsonValue value;
    qint64 localRev = 0;
    qint64 serverRev = 0;  // base revision for the server (0 = the key is new there)
  };

  // `dir` is created on demand; an empty dir keeps everything in memory (tests).
  explicit PluginStore(const QString& dir);

  static bool validKey(const QString& key);
  static bool validPluginId(const QString& id);

  Entry get(const QString& pluginId, const QString& key) const;
  QStringList keys(const QString& pluginId) const;  // sorted (developer console "Inspect")
  // baseRevision: the revision the plugin read; a different current revision is a Conflict (the attempted value is kept in
  // conflicts(), the current value stays).
  PutResult put(const QString& pluginId, const QString& key, const QJsonValue& value, std::optional<qint64> baseRevision = std::nullopt);
  QList<Conflict> conflicts(const QString& pluginId, const QString& key) const;

  // --- sync (called by PluginStoreSync on the UI thread) ---
  QList<Dirty> dirtyKeys() const;                       // dirty and without conflicts
  QStringList knownPlugins() const;
  QList<std::pair<QString, qint64>> serverRevisions(const QString& pluginId) const;  // (key, serverRev) of the keys with one
  void markPushed(const QString& pluginId, const QString& key, qint64 localRev, qint64 serverRev);
  // The server refused a push with 409: adopt its revision as the new base and keep its value as a conflict (unless equal).
  void markServerConflict(const QString& pluginId, const QString& key, qint64 serverRev, const QJsonValue& serverValue);
  // A newer server value for a key that is not dirty (or does not exist here): becomes the current value.
  void adoptServer(const QString& pluginId, const QString& key, qint64 serverRev, const QJsonValue& value);
  bool isDirty(const QString& pluginId, const QString& key) const;
  qint64 serverRevision(const QString& pluginId, const QString& key) const;

  // Called after every successful put (any thread); the sync schedules a push from it.
  void setChangedCallback(std::function<void(const QString& pluginId)> cb);

 private:
  struct Rec {
    QJsonValue value;
    qint64 rev = 0;
    qint64 serverRev = 0;
    bool dirty = false;
    QList<Conflict> conflicts;
  };
  struct Plugin {
    QHash<QString, Rec> keys;
    bool loaded = false;
  };
  Plugin& plugin(const QString& id) const;  // caller holds the lock
  void save(const QString& id, const Plugin& p) const;
  QString filePath(const QString& id) const;

  QString dir_;
  mutable std::mutex m_;
  mutable QHash<QString, Plugin> plugins_;
  std::function<void(const QString&)> changed_;
};

}  // namespace uzume::plugins
