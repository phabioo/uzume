#include "runpluginupdate.h"

#include <QJsonArray>
#include <QMap>
#include <QObject>
#include <QSet>
#include <algorithm>

#include "manifest.h"
#include "semver.h"

namespace uzume::ui::runplugins {

namespace {
int cmp(const QString& a, const QString& b) { return update::compareVersions(a, b).value_or(0); }

const HubPluginInfo* find(const QList<HubPluginInfo>& offered, const QString& id, const QString& version) {
  for (const HubPluginInfo& p : offered)
    if (p.id == id && p.version == version) return &p;
  return nullptr;
}

const HubPluginInfo* newest(const QList<HubPluginInfo>& offered, const QString& id) {
  const HubPluginInfo* best = nullptr;
  for (const HubPluginInfo& p : offered)
    if (p.id == id && (best == nullptr || cmp(p.version, best->version) > 0)) best = &p;
  return best;
}

// Newest offered version of `id` that satisfies `range`.
const HubPluginInfo* newestFitting(const QList<HubPluginInfo>& offered, const QString& id, const plugins::VersionRange& range) {
  const HubPluginInfo* best = nullptr;
  for (const HubPluginInfo& p : offered) {
    if (p.id != id) continue;
    const auto v = update::SemVer::parse(p.version);
    if (!v || !range.matches(*v)) continue;
    if (best == nullptr || cmp(p.version, best->version) > 0) best = &p;
  }
  return best;
}

QString nameOfOffered(const QList<HubPluginInfo>& offered, const QString& id) {
  const HubPluginInfo* p = newest(offered, id);
  return p != nullptr && !p->name.isEmpty() ? p->name : id;
}

// Fills `c.request` with `target` and the plugins its requirements need: a required plugin that the run pins in a version outside the
// range (or does not have at all) is switched to the newest offered version that fits. false when some requirement cannot be met.
bool bundle(const RunInfo& run, const QList<HubPluginInfo>& offered, const HubPluginInfo& target, Change& c) {
  QMap<QString, QString> resulting;  // plugin id -> version after the call
  for (const RunPluginRef& p : run.plugins) resulting.insert(p.pluginId, p.version);
  resulting.insert(target.id, target.version);
  c.request = {{target.id, target.version, {}, {}}};
  QList<const HubPluginInfo*> queue{&target};
  QSet<QString> seen{target.id};
  for (int guard = 0; !queue.isEmpty() && guard < 64; ++guard) {
    const HubPluginInfo* cur = queue.takeFirst();
    for (auto it = cur->requires_.begin(); it != cur->requires_.end(); ++it) {
      const auto range = plugins::VersionRange::parse(it.value().toString());
      if (!range) return false;
      const QString dep = it.key();
      const auto have = resulting.constFind(dep);
      if (have != resulting.constEnd()) {
        const auto v = update::SemVer::parse(have.value());
        if (v && range->matches(*v)) continue;
      }
      const HubPluginInfo* fit = newestFitting(offered, dep, *range);
      if (fit == nullptr) return false;
      resulting.insert(dep, fit->version);
      if (seen.contains(dep)) return false;  // a second switch of the same plugin would contradict the first
      seen.insert(dep);
      c.request.append({dep, fit->version, {}, {}});
      c.bundledNames << (fit->name.isEmpty() ? dep : fit->name);
      queue.append(fit);
    }
  }
  return true;
}
}  // namespace

QStringList addedPermissions(const QList<HubPluginInfo>& offered, const QString& pluginId, const QString& version, const QString& previousVersion) {
  if (previousVersion.isEmpty()) return {};
  const HubPluginInfo* now = find(offered, pluginId, version);
  if (now == nullptr) return {};
  const HubPluginInfo* before = find(offered, pluginId, previousVersion);
  QStringList out;
  for (const QString& p : now->permissions)
    if ((before == nullptr || !before->permissions.contains(p)) && p != QLatin1String("run.state")) out << p;
  return out;
}

QList<Change> changes(const RunInfo& run, const QList<HubPluginInfo>& offered) {
  QList<Change> out;
  for (const RunPluginRef& pin : run.plugins) {
    const HubPluginInfo* top = newest(offered, pin.pluginId);
    if (top != nullptr && cmp(top->version, pin.version) > 0) {
      Change c;
      c.pluginId = pin.pluginId;
      c.name = top->name.isEmpty() ? pin.pluginId : top->name;
      c.from = pin.version;
      c.to = top->version;
      if (bundle(run, offered, *top, c)) {
        for (const RunPluginRef& r : c.request) {
          QString prev;  // the version the run pins now, "" = the plugin is new to the run
          for (const RunPluginRef& p : run.plugins)
            if (p.pluginId == r.pluginId) prev = p.version;
          const HubPluginInfo* now = find(offered, r.pluginId, r.version);
          const HubPluginInfo* before = prev.isEmpty() ? nullptr : find(offered, r.pluginId, prev);
          if (now != nullptr)
            for (const QString& perm : now->permissions)
              if ((before == nullptr || !before->permissions.contains(perm)) && perm != QLatin1String("run.state") && !c.addedPermissions.contains(perm))
                c.addedPermissions << perm;
        }
        out.append(c);
      }
    }
    if (!pin.previousVersion.isEmpty() && find(offered, pin.pluginId, pin.previousVersion) != nullptr) {
      Change c;
      c.pluginId = pin.pluginId;
      c.name = nameOfOffered(offered, pin.pluginId);
      c.from = pin.version;
      c.to = pin.previousVersion;
      c.restore = true;
      c.request = {{pin.pluginId, pin.previousVersion, {}, {}}};
      out.append(c);
    }
  }
  return out;
}

bool pinsDiffer(const QList<RunPluginRef>& before, const QList<RunPluginRef>& after) {
  if (before.size() != after.size()) return true;
  QMap<QString, QPair<QString, QString>> b;
  for (const RunPluginRef& p : before) b.insert(p.pluginId, {p.version, p.sha256});
  for (const RunPluginRef& p : after) {
    const auto it = b.constFind(p.pluginId);
    if (it == b.constEnd() || it->first != p.version || it->second != p.sha256) return true;
  }
  return false;
}

QString eventText(const QString& who, const QJsonObject& payload, const std::function<QString(const QString&)>& nameOf) {
  QStringList parts;
  for (const QJsonValue& v : payload.value(QStringLiteral("changes")).toArray()) {
    const QJsonObject o = v.toObject();
    const QString id = o.value(QStringLiteral("plugin_id")).toString();
    if (id.isEmpty()) continue;
    parts << QObject::tr("%1 %2 → %3").arg(nameOf ? nameOf(id) : id, o.value(QStringLiteral("from")).toString(), o.value(QStringLiteral("to")).toString());
  }
  if (parts.isEmpty()) return QObject::tr("%1 updated the plugins").arg(who);
  return QObject::tr("%1 updated %2").arg(who, parts.join(QStringLiteral("; ")));
}

}  // namespace uzume::ui::runplugins
