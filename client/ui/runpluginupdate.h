#pragma once
// Updating the pinned plugins of a running run (run_plugin_updates_v1): which update or restore the app offers to the creator or an admin,
// which required plugins it bundles, which permissions a switch adds (needs a new local consent), and the log row of plugins.updated.
// Pure helpers (no I/O), see tests/run_plugin_update_test.cpp.
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

#include "pluginhub.h"
#include "runtypes.h"

namespace uzume::ui::runplugins {

struct Change {
  QString pluginId;
  QString name;
  QString from;  // pinned now
  QString to;    // offered version
  bool restore = false;  // `to` is the previous version
  QList<RunPluginRef> request;  // body of PUT /runs/{id}/plugins: this plugin first, then the bundled required plugins
  QStringList bundledNames;     // display names of the bundled required plugins ("" entries never)
  QStringList addedPermissions; // permissions of the target version(s) the pinned version(s) do not declare
};

// Updates (newest offered version, semver-greater than the pinned one) and restores (previous_version, still offered) for a run.
// An update whose requirements no offered version can satisfy is left out.
QList<Change> changes(const RunInfo& run, const QList<HubPluginInfo>& offered);

// Permissions of `pluginId` at `version` that `previousVersion` does not declare; the offered list is the source. An unknown previous
// version counts as having none (so everything but run.state asks); an unknown version has none. Empty `previousVersion` = nothing added.
QStringList addedPermissions(const QList<HubPluginInfo>& offered, const QString& pluginId, const QString& version, const QString& previousVersion);

// Permissions of `pluginId` at `version` for a plugin the run did not pin before (appended by an update): everything but run.state.
// `known` is false when the version is not in the offered list (stale catalog); the permissions are then unknown.
QStringList newPermissions(const QList<HubPluginInfo>& offered, const QString& pluginId, const QString& version, bool* known = nullptr);

// Pins whose version or sha256 differs from `before` (new ids count as changed). Used to decide a restart: unchanged pins restart nothing.
bool pinsDiffer(const QList<RunPluginRef>& before, const QList<RunPluginRef>& after);

// "Lena updated Cheats 1.0.0 → 1.1.0"; several changes are joined with "; ". nameOf maps a plugin id to its display name.
QString eventText(const QString& who, const QJsonObject& payload, const std::function<QString(const QString&)>& nameOf);

}  // namespace uzume::ui::runplugins
