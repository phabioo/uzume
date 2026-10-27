#include "plugin_store.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace uzume::plugins {

namespace {

QByteArray compact(const QJsonValue& v) {
  // QJsonDocument only holds objects and arrays: wrap scalars.
  return QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
}

QJsonObject conflictJson(const PluginStore::Conflict& c) {
  return {{QStringLiteral("value"), c.value}, {QStringLiteral("revision"), static_cast<double>(c.revision)}, {QStringLiteral("source"), c.source}};
}

}  // namespace

PluginStore::PluginStore(const QString& dir) : dir_(dir) {}

bool PluginStore::validKey(const QString& key) {
  static const QRegularExpression re(QStringLiteral("^[a-z0-9_.-]{1,64}$"));
  return re.match(key).hasMatch();
}

bool PluginStore::validPluginId(const QString& id) {
  static const QRegularExpression re(QStringLiteral("^[a-z0-9-]+(\\.[a-z0-9-]+)+$"));
  return id.size() <= 128 && re.match(id).hasMatch();
}

QString PluginStore::filePath(const QString& id) const { return QDir(dir_).filePath(id + QStringLiteral(".json")); }

PluginStore::Plugin& PluginStore::plugin(const QString& id) const {
  Plugin& p = plugins_[id];
  if (p.loaded) return p;
  p.loaded = true;
  if (dir_.isEmpty() || !validPluginId(id)) return p;
  QFile f(filePath(id));
  if (!f.open(QIODevice::ReadOnly)) return p;
  const QJsonObject keys = QJsonDocument::fromJson(f.readAll()).object().value(QLatin1String("keys")).toObject();
  for (auto it = keys.begin(); it != keys.end(); ++it) {
    if (!validKey(it.key())) continue;
    const QJsonObject o = it.value().toObject();
    Rec r;
    r.value = o.value(QLatin1String("value"));
    r.rev = static_cast<qint64>(o.value(QLatin1String("rev")).toDouble(0));
    r.serverRev = static_cast<qint64>(o.value(QLatin1String("server_rev")).toDouble(0));
    r.dirty = o.value(QLatin1String("dirty")).toBool(false);
    for (const QJsonValue& cv : o.value(QLatin1String("conflicts")).toArray()) {
      const QJsonObject co = cv.toObject();
      r.conflicts.append({co.value(QLatin1String("value")), static_cast<qint64>(co.value(QLatin1String("revision")).toDouble(0)),
                          co.value(QLatin1String("source")).toString()});
    }
    p.keys.insert(it.key(), r);
  }
  return p;
}

void PluginStore::save(const QString& id, const Plugin& p) const {
  if (dir_.isEmpty() || !validPluginId(id)) return;
  QJsonObject keys;
  for (auto it = p.keys.begin(); it != p.keys.end(); ++it) {
    QJsonArray cs;
    for (const Conflict& c : it->conflicts) cs.append(conflictJson(c));
    keys.insert(it.key(), QJsonObject{{QStringLiteral("value"), it->value},
                                     {QStringLiteral("rev"), static_cast<double>(it->rev)},
                                     {QStringLiteral("server_rev"), static_cast<double>(it->serverRev)},
                                     {QStringLiteral("dirty"), it->dirty},
                                     {QStringLiteral("conflicts"), cs}});
  }
  QDir().mkpath(dir_);
  QSaveFile f(filePath(id));
  if (!f.open(QIODevice::WriteOnly)) return;
  f.write(QJsonDocument(QJsonObject{{QStringLiteral("keys"), keys}}).toJson(QJsonDocument::Compact));
  f.commit();
}

PluginStore::Entry PluginStore::get(const QString& pluginId, const QString& key) const {
  std::lock_guard<std::mutex> l(m_);
  const Plugin& p = plugin(pluginId);
  const auto it = p.keys.constFind(key);
  if (it == p.keys.constEnd()) return {};
  return {true, it->value, it->rev};
}

QStringList PluginStore::keys(const QString& pluginId) const {
  std::lock_guard<std::mutex> l(m_);
  QStringList out = plugin(pluginId).keys.keys();
  out.sort();
  return out;
}

PluginStore::PutResult PluginStore::put(const QString& pluginId, const QString& key, const QJsonValue& value, std::optional<qint64> baseRevision) {
  PutResult res;
  if (!validKey(key) || !validPluginId(pluginId)) {
    res.kind = PutResult::Kind::Invalid;
    return res;
  }
  const QByteArray bytes = compact(value);
  if (bytes.size() - 2 > kStoreMaxValueBytes) {
    res.kind = PutResult::Kind::TooLarge;
    return res;
  }
  std::function<void(const QString&)> cb;
  {
    std::lock_guard<std::mutex> l(m_);
    Plugin& p = plugin(pluginId);
    auto it = p.keys.find(key);
    const qint64 current = it == p.keys.end() ? 0 : it->rev;
    if (baseRevision && *baseRevision != current) {
      res.kind = PutResult::Kind::Conflict;
      res.revision = current;
      if (it != p.keys.end()) {  // the stale write is not lost
        it->conflicts.append({value, *baseRevision, QStringLiteral("local")});
        while (it->conflicts.size() > kStoreMaxConflicts) it->conflicts.removeFirst();
        save(pluginId, p);
      }
      return res;
    }
    if (it == p.keys.end()) {
      if (p.keys.size() >= kStoreMaxKeys) {
        res.kind = PutResult::Kind::Quota;
        return res;
      }
    }
    qint64 total = 0;
    for (auto k = p.keys.begin(); k != p.keys.end(); ++k) total += k.key() == key ? 0 : compact(k->value).size();
    if (total + bytes.size() > kStoreMaxBytes) {
      res.kind = PutResult::Kind::Quota;
      return res;
    }
    Rec& r = it == p.keys.end() ? p.keys[key] : *it;
    r.value = value;
    r.rev = current + 1;
    r.dirty = true;
    r.conflicts.clear();  // a put after reading the conflicts resolves them
    res.revision = r.rev;
    save(pluginId, p);
    cb = changed_;
  }
  if (cb) cb(pluginId);
  return res;
}

QList<PluginStore::Conflict> PluginStore::conflicts(const QString& pluginId, const QString& key) const {
  std::lock_guard<std::mutex> l(m_);
  const Plugin& p = plugin(pluginId);
  const auto it = p.keys.constFind(key);
  return it == p.keys.constEnd() ? QList<Conflict>() : it->conflicts;
}

QList<PluginStore::Dirty> PluginStore::dirtyKeys() const {
  std::lock_guard<std::mutex> l(m_);
  QList<Dirty> out;
  if (!dir_.isEmpty())
    for (const QString& f : QDir(dir_).entryList({QStringLiteral("*.json")}, QDir::Files)) plugin(f.left(f.size() - 5));  // load what is on disk
  for (auto pit = plugins_.begin(); pit != plugins_.end(); ++pit)
    for (auto it = pit->keys.begin(); it != pit->keys.end(); ++it)
      if (it->dirty && it->conflicts.isEmpty()) out.append({pit.key(), it.key(), it->value, it->rev, it->serverRev});
  return out;
}

QStringList PluginStore::knownPlugins() const {
  std::lock_guard<std::mutex> l(m_);
  QStringList out;
  for (auto it = plugins_.begin(); it != plugins_.end(); ++it) out << it.key();
  if (!dir_.isEmpty())
    for (const QString& f : QDir(dir_).entryList({QStringLiteral("*.json")}, QDir::Files)) {
      const QString id = f.left(f.size() - 5);
      if (validPluginId(id) && !out.contains(id)) out << id;
    }
  return out;
}

QList<std::pair<QString, qint64>> PluginStore::serverRevisions(const QString& pluginId) const {
  std::lock_guard<std::mutex> l(m_);
  const Plugin& p = plugin(pluginId);
  QList<std::pair<QString, qint64>> out;
  for (auto it = p.keys.begin(); it != p.keys.end(); ++it) out.append({it.key(), it->serverRev});
  return out;
}

void PluginStore::markPushed(const QString& pluginId, const QString& key, qint64 localRev, qint64 serverRev) {
  std::lock_guard<std::mutex> l(m_);
  Plugin& p = plugin(pluginId);
  auto it = p.keys.find(key);
  if (it == p.keys.end()) return;
  it->serverRev = serverRev;
  if (it->rev == localRev) it->dirty = false;  // not written again meanwhile
  save(pluginId, p);
}

void PluginStore::markServerConflict(const QString& pluginId, const QString& key, qint64 serverRev, const QJsonValue& serverValue) {
  std::lock_guard<std::mutex> l(m_);
  Plugin& p = plugin(pluginId);
  auto it = p.keys.find(key);
  if (it == p.keys.end()) return;
  it->serverRev = serverRev;
  if (compact(it->value) == compact(serverValue)) {
    it->dirty = false;  // the same value on both sides: nothing to resolve
  } else {
    it->conflicts.append({serverValue, serverRev, QStringLiteral("server")});
    while (it->conflicts.size() > kStoreMaxConflicts) it->conflicts.removeFirst();
  }
  save(pluginId, p);
}

void PluginStore::adoptServer(const QString& pluginId, const QString& key, qint64 serverRev, const QJsonValue& value) {
  std::lock_guard<std::mutex> l(m_);
  Plugin& p = plugin(pluginId);
  auto it = p.keys.find(key);
  if (it != p.keys.end() && it->dirty) return;
  Rec& r = it == p.keys.end() ? p.keys[key] : *it;
  r.value = value;
  r.rev += 1;
  r.serverRev = serverRev;
  r.dirty = false;
  r.conflicts.clear();
  save(pluginId, p);
}

bool PluginStore::isDirty(const QString& pluginId, const QString& key) const {
  std::lock_guard<std::mutex> l(m_);
  const Plugin& p = plugin(pluginId);
  const auto it = p.keys.constFind(key);
  return it != p.keys.constEnd() && it->dirty;
}

qint64 PluginStore::serverRevision(const QString& pluginId, const QString& key) const {
  std::lock_guard<std::mutex> l(m_);
  const Plugin& p = plugin(pluginId);
  const auto it = p.keys.constFind(key);
  return it == p.keys.constEnd() ? 0 : it->serverRev;
}

void PluginStore::setChangedCallback(std::function<void(const QString& pluginId)> cb) {
  std::lock_guard<std::mutex> l(m_);
  changed_ = std::move(cb);
}

}  // namespace uzume::plugins
