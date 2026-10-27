#include "pluginscontroller.h"

#include <QRegularExpression>

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGuiApplication>
#include <QKeyCombination>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QUrl>
#include <algorithm>

#include "game_identity.h"
#include "gamesession.h"
#include "hubconnection.h"
#include "playersettings.h"
#include "runtypes.h"
#include "system_manifest.h"
#include "semver.h"
#include "plugin_language.h"
#include "plugin_pick.h"
#include "plugin_ui.h"

namespace uzume::ui {

Q_LOGGING_CATEGORY(lcPlugins, "uzume.plugins")

namespace {

using plugins::LogLevel;
using plugins::PluginStatus;

constexpr Qt::KeyboardModifiers kComboMods = Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;

// Portable key text ("Ctrl+H") -> Qt::Key | modifiers; 0 = empty or not a single combination.
int comboFromText(const QString& text) {
  if (text.isEmpty() || text == QLatin1String("none")) return 0;
  const QKeySequence seq = QKeySequence::fromString(text, QKeySequence::PortableText);
  if (seq.count() != 1) return 0;
  const QKeyCombination kc = seq[0];
  if (kc.key() == Qt::Key_unknown || kc.key() == Qt::Key(0)) return 0;
  return int(kc.key()) | int(kc.keyboardModifiers() & kComboMods);
}

QString textFromCombo(int combo) {
  return QKeySequence(QKeyCombination::fromCombined(combo)).toString(QKeySequence::PortableText);
}

QString originText(const QString& origin) {
  if (origin == QLatin1String("official")) return PluginsController::tr("✓ Official");
  if (origin == QLatin1String("unverified")) return PluginsController::tr("Server upload · unverified");
  return PluginsController::tr("Local file");
}

// Plain-language permission (docs/design/legacy/plugins.md 4b), the manifest names stay in the plugin API reference.
QString permissionLabel(const QString& p) {
  if (p == QLatin1String("memory.read")) return PluginsController::tr("Read game memory");
  if (p == QLatin1String("rom.read")) return PluginsController::tr("Read your game file");
  if (p == QLatin1String("ui")) return PluginsController::tr("Show panels in game");
  if (p == QLatin1String("run.state")) return PluginsController::tr("Share run progress with your server");
  if (p == QLatin1String("video.capture")) return PluginsController::tr("Capture the game picture");
  if (p == QLatin1String("memory.write")) return PluginsController::tr("Change game memory");
  if (p == QLatin1String("cheats")) return PluginsController::tr("Turn cheats on and off");
  if (p == QLatin1String("store")) return PluginsController::tr("Keep its data with your account");
  if (p == QLatin1String("sound")) return PluginsController::tr("Play sounds");
  return p;
}

// "^0.3" / ">=0.3" -> "0.3 or newer", "*" -> "", an exact or "~" range stays as written.
QString rangeLabel(const QString& r) {
  if (r == QLatin1String("*")) return {};
  if (r.startsWith(QLatin1Char('^'))) return PluginsController::tr("%1 or newer").arg(r.mid(1));
  if (r.startsWith(QLatin1String(">="))) return PluginsController::tr("%1 or newer").arg(r.mid(2));
  return r;
}

QStringList permissionLabels(const QStringList& perms) {
  QStringList out;
  for (const QString& p : perms) out << permissionLabel(p);
  return out;
}

// cheats and memory.write change the running game and its saves: the lists mark them and say what Uzume does about it (F15).
bool changesGame(const QStringList& perms) { return perms.contains(QStringLiteral("cheats")) || perms.contains(QStringLiteral("memory.write")); }

QString changesGameNote() {
  return PluginsController::tr("It can change your game. Uzume saves a copy first and asks you for each game.");
}

QVariantList permissionRows(const QStringList& perms) {
  QVariantList out;
  for (const QString& p : perms)
    out.append(QVariantMap{{QStringLiteral("text"), permissionLabel(p)}, {QStringLiteral("warn"), p == QLatin1String("cheats") || p == QLatin1String("memory.write")}});
  return out;
}

QString permissionsText(const QStringList& perms) {
  QStringList out;
  for (const QString& p : perms) out << permissionLabel(p);
  return out.isEmpty() ? PluginsController::tr("No special permissions") : out.join(QStringLiteral(" · "));
}

QString sizeText(qint64 bytes) {
  if (bytes <= 0) return {};
  if (bytes < 1024 * 1024) return PluginsController::tr("%1 KB").arg((bytes / 1024 > 0 ? bytes / 1024 : qint64(1)));
  return PluginsController::tr("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

int compareVersionText(const QString& a, const QString& b) {
  const auto sa = update::SemVer::parse(a);
  const auto sb = update::SemVer::parse(b);
  if (!sa || !sb) return 0;
  return update::SemVer::compare(*sa, *sb);
}

}  // namespace

PluginsController::PluginsController(PlayerSettings* settings, const QString& dataDir, GameSession* session, HubConnection* connection,
                                     QObject* parent)
    : QObject(parent), settings_(settings), session_(session), dataDir_(dataDir), conn_(connection) {
  emulation_ = std::make_shared<PluginEmulationService>(session, dataDir);
  emulation_->setConnection(connection);
  emulation_->setPluginNameFn([this](const QString& id) {
    const auto info = infoFor(id);
    return info ? info->manifest.name : id;
  });
  connect(emulation_.get(), &PluginEmulationService::consentRequestChanged, this, &PluginsController::consentRequestChanged);
  connect(emulation_.get(), &PluginEmulationService::cheatsOnChanged, this, &PluginsController::cheatsOnChanged);
  connect(emulation_.get(), &PluginEmulationService::runRulesChanged, this, &PluginsController::refreshRunExtras);
  connect(emulation_.get(), &PluginEmulationService::noticeRaised, this, &PluginsController::addAppNotice);
  manager_ = std::make_unique<plugins::PluginManager>(QDir(dataDir).filePath(QStringLiteral("plugins")));
  manager_->setDeveloperMode(settings_ != nullptr && settings_->developerMode());
  if (connection != nullptr) {
    hubClient_ = std::make_unique<PluginHubClient>(connection);
    delivery_ = std::make_unique<plugins::PluginDelivery>(manager_.get(), hubClient_.get());
    connect(delivery_.get(), &plugins::PluginDelivery::finished, this, &PluginsController::onDelivered);
    connect(hubClient_.get(), &PluginHubClient::stateChanged, this, [this]() {
      rebuildHubPlugins();
      refreshPlugins();  // "update available" chips
    });
    connect(connection, &HubConnection::stateChanged, this, [this](HubConnection::State s) {
      if (s == HubConnection::State::Connected) refreshHubPlugins();
    });
  }
  host_ = std::make_unique<plugins::PluginHost>();
  emulation_->setAppEventSink([this](const QString& name, const QVariant& v) {
    if (host_) host_->postAppEvent(name, v);
  });
  adapter_ = std::make_shared<MemoryWatchAdapter>();
  if (settings_ != nullptr) {  // HUD placement and notice muting survive restarts
    hudCorner_ = settings_->pluginHudCorners();
    const QStringList hidden = settings_->pluginHudHidden();
    hudHidden_ = QSet<QString>(hidden.begin(), hidden.end());
    hudAllHidden_ = settings_->pluginHudAllHidden();
    hotkeyOverrides_ = settings_->pluginHotkeyOverrides();
    const QHash<QString, QString> mutes = settings_->pluginToastMutes();
    for (auto it = mutes.begin(); it != mutes.end(); ++it) (it.value() == QLatin1String("important") ? mutedImportant_ : mutedAlways_).insert(it.key());
  }

  DevConsole::Sources src;
  src.developerMode = [this]() { return developerMode(); };
  src.activeIds = [this]() { return consoleIds(); };
  src.info = [this](const QString& id) { return infoFor(id); };
  src.snapshot = [this](const QString& id) { return host_->consoleSnapshot(id); };
  src.unpacked = [this]() {
    QList<plugins::PluginInfo> out;
    for (const plugins::PluginInfo& p : manager_->list())
      if (p.devLinked) out.append(p);
    return out;
  };
  src.inspect = [this](const QString& id) { return inspectOf(id); };
  src.permissionLabel = [](const QString& perm) { return permissionLabel(perm); };
  src.reload = [this](const QString& id) { reloadPlugin(id); };
  src.setWatching = [this](bool on) {
    manager_->setWatchEnabled(on);
    if (settings_ != nullptr) settings_->setPluginReloadOnChange(on);
    return true;
  };
  devConsole_ = std::make_unique<DevConsole>(std::move(src), this);
  if (settings_ != nullptr) {
    manager_->setWatchEnabled(settings_->pluginReloadOnChange());
    devConsole_->setReloadOnChange(settings_->pluginReloadOnChange());
  }
  if (session_ != nullptr) devConsole_->setMemorySource(std::shared_ptr<MemorySource>(makeGameMemorySource(session_).release()));
  connect(devConsole_.get(), &DevConsole::modelChanged, this, &PluginsController::consoleChanged);
  connect(devConsole_.get(), &DevConsole::pausedAfterError, this, [this](const QString& id, const QString&) { addHostNotice(id); });
  connect(devConsole_.get(), &DevConsole::reloaded, this, [this](const QString& id, const QString& name, qint64 ms) { addReloadNotice(id, name, ms); });
  connect(host_.get(), &plugins::PluginHost::consoleChanged, this, [this](const QString& id) { devConsole_->noteChanged(id); });
  connect(host_.get(), &plugins::PluginHost::statusChanged, this, [this](const QString& id) {
    onStatus(id);
    devConsole_->noteChanged(id);
    rebuildPanelTabs();
  });
  connect(host_.get(), &plugins::PluginHost::uiChanged, this, &PluginsController::onUiChanged);
  connect(manager_.get(), &plugins::PluginManager::pluginsChanged, this, &PluginsController::refreshPlugins);
  // A linked developer folder changed: reload that plugin while the game runs.
  connect(manager_.get(), &plugins::PluginManager::pluginChanged, this, [this](const QString& id) {
    refreshPlugins();
    if ((running_ && runningIds_.contains(id)) || (shellRunning_ && shellIds_.contains(id))) {
      qCInfo(lcPlugins) << "plugin" << id << "reloading (files changed)";
      host_->reload(id);
    }
  });
  if (session_ != nullptr) {
    // Live-save restart: the new runner has a new memory watch set.
    connect(session_, &GameSession::memoryWatchReplaced, this, [this]() {
      if (running_) adapter_->bind(session_->memoryWatch());
    });
    connect(session_, &GameSession::memoryRegionsChanged, this, [this]() {
      devConsole_->schedule();
      devConsole_->memoryRefresh();
    });
    connect(session_, &GameSession::stateChanged, this, [this]() {
      if (inGame_ && !session_->isActive()) gameStopped();
    });
  }
  refreshPlugins();
}

PluginsController::~PluginsController() {
  // Plugins first, then the adapter (unregisters its ranges), then the manager.
  if (host_) host_->stop();
  if (adapter_) adapter_->clear();
}

bool PluginsController::developerMode() const { return manager_->developerMode(); }

void PluginsController::setDeveloperMode(bool on) {
  if (manager_->developerMode() == on) return;
  if (settings_ != nullptr) settings_->setDeveloperMode(on);
  manager_->setDeveloperMode(on);
  refreshPlugins();
  emit developerModeChanged();
  setConsoleAvailable(on && running_);
  devConsole_->schedule();
}

QString PluginsController::pluginLanguage() const {
  return settings_ != nullptr ? settings_->pluginLanguage() : QString::fromLatin1(plugins::kPluginLanguageAuto);
}

void PluginsController::setPluginLanguage(const QString& tag) {
  if (settings_ == nullptr || settings_->pluginLanguage() == tag) return;
  if (!settings_->setPluginLanguage(tag)) return;
  emit pluginLanguageChanged();
}

QVariantList PluginsController::pluginLanguageOptions() const {
  QVariantList out;
  out.append(QVariantMap{{QStringLiteral("value"), QString::fromLatin1(plugins::kPluginLanguageAuto)}, {QStringLiteral("label"), tr("Auto")}});
  for (const plugins::PluginLanguageChoice& c : plugins::pluginLanguageChoices())
    out.append(QVariantMap{{QStringLiteral("value"), c.tag}, {QStringLiteral("label"), c.label}});
  return out;
}

QString PluginsController::resolveLanguage(const QString& systemId, const QMap<QString, QString>& coreOptions) const {
  return plugins::resolvePluginLanguage(pluginLanguage(), systemId, coreOptions);
}

QString PluginsController::localPath(const QString& pathOrUrl) {
  QString p = pathOrUrl.trimmed();
  if (p.startsWith(QLatin1String("file:"))) p = QUrl(p).toLocalFile();
  return p;
}

void PluginsController::setMessage(const QString& text, bool isError) {
  message_ = text;
  messageIsError_ = isError;
  emit messageChanged();
}

void PluginsController::dismissMessage() {
  if (!message_.isEmpty()) setMessage(QString(), false);
}

void PluginsController::reportInstall(const plugins::InstallResult& r, bool fromFolder) {
  if (!r.ok) {
    setMessage(tr("Could not install: %1").arg(r.error), true);
    return;
  }
  const auto info = manager_->info(r.id);
  const QString name = info ? info->manifest.name : r.id;
  QString text;
  if (fromFolder) {
    text = tr("Installed %1. It is off until you turn it on.").arg(name);
  } else if (r.origin == QLatin1String("official")) {
    text = tr("Installed %1 (Official, signed by Uzume). It is off until you turn it on.").arg(name);
  } else {
    text = tr("Installed %1 as a local file. It is off until you turn it on.").arg(name);
  }
  if (r.origin == QLatin1String("local") && !manager_->developerMode()) text += QLatin1Char(' ') + tr("It runs only while Developer mode is on.");
  if (!r.signatureNote.isEmpty()) text += QLatin1Char(' ') + tr("The signature of this file is not valid, so it is not treated as Official.");
  if (!r.addedPermissions.isEmpty())
    text += QLatin1Char(' ') + tr("It asks for new permissions (%1) and was turned off.").arg(permissionsText(r.addedPermissions));
  setMessage(text, false);
}

void PluginsController::installFromFolder(const QString& pathOrUrl) {
  const QString path = localPath(pathOrUrl);
  if (path.isEmpty()) return;
  reportInstall(manager_->installFromFolder(path), true);
  refreshPlugins();
}

void PluginsController::installFromPath(const QString& pathOrUrl) {
  const QString path = localPath(pathOrUrl);
  if (path.isEmpty()) return;
  const bool isFolder = QFileInfo(path).isDir();
  reportInstall(manager_->installFromPath(path), isFolder);
  refreshPlugins();
}

plugins::InstallResult PluginsController::installFromPathResult(const QString& pathOrUrl) {
  const QString path = localPath(pathOrUrl);
  plugins::InstallResult r;
  if (path.isEmpty()) {
    r.error = QStringLiteral("path required");
    return r;
  }
  r = manager_->installFromPath(path);
  reportInstall(r, QFileInfo(path).isDir());
  refreshPlugins();
  return r;
}

const HubPluginInfo* PluginsController::newestOffer(const QString& id) const {
  const HubPluginInfo* best = nullptr;
  if (hubClient_ == nullptr) return nullptr;
  for (const HubPluginInfo& p : hubClient_->plugins())
    if (p.id == id && (best == nullptr || compareVersionText(p.version, best->version) > 0)) best = &p;
  return best;
}

QString PluginsController::hubStatus() const {
  if (hubClient_ == nullptr) return QStringLiteral("idle");
  switch (hubClient_->state()) {
    case PluginHubClient::State::Idle: return QStringLiteral("idle");
    case PluginHubClient::State::Loading: return QStringLiteral("loading");
    case PluginHubClient::State::Ready: return QStringLiteral("ready");
    case PluginHubClient::State::Failed: return QStringLiteral("failed");
  }
  return QStringLiteral("idle");
}

void PluginsController::refreshHubPlugins() {
  if (hubClient_ != nullptr) hubClient_->refresh();
}

void PluginsController::installHubPlugin(const QString& id, const QString& version) {
  if (delivery_ == nullptr || !hubBusyId_.isEmpty()) return;
  hubBusyId_ = id;
  rebuildHubPlugins();
  delivery_->install(id, version);
}

void PluginsController::installHubPluginWithDependencies(const QString& id, const QString& version) {
  if (delivery_ == nullptr || !hubBusyId_.isEmpty()) return;
  const HubPluginInfo* offer = newestOffer(id);
  QList<QPair<QString, QString>> queue;
  if (offer != nullptr) {
    for (auto it = offer->requires_.begin(); it != offer->requires_.end(); ++it) {
      if (manager_->info(it.key()).has_value()) continue;
      if (const HubPluginInfo* dep = newestOffer(it.key()); dep != nullptr) queue.append({dep->id, dep->version});
    }
  }
  queue.append({id, version});
  const auto first = queue.takeFirst();
  installQueue_ = queue;
  installHubPlugin(first.first, first.second);
}

void PluginsController::onDelivered(const plugins::DeliveryResult& r) {
  hubBusyId_.clear();
  if (!r.ok) installQueue_.clear();
  if (r.ok && !installQueue_.isEmpty()) {
    const auto next = installQueue_.takeFirst();
    refreshPlugins();
    installHubPlugin(next.first, next.second);
    return;
  }
  if (r.ok) {
    const auto info = manager_->info(r.id);
    QString text = tr("Installed %1 %2 from your server. It is off until you turn it on.").arg(info ? info->manifest.name : r.id, r.version);
    if (r.origin == QLatin1String("unverified")) text += QLatin1Char(' ') + tr("Uzume has not checked this plugin.");
    if (!r.signatureNote.isEmpty()) text += QLatin1Char(' ') + tr("Its signature is not valid, so it is not treated as Official.");
    if (!r.addedPermissions.isEmpty())
      text += QLatin1Char(' ') + tr("It asks for new permissions (%1) and was turned off.").arg(permissionsText(r.addedPermissions));
    setMessage(text, false);
  } else {
    setMessage(r.error, true);
  }
  refreshPlugins();
  rebuildHubPlugins();
}

void PluginsController::rebuildHubPlugins() {
  QVariantList out;
  if (hubClient_ != nullptr && hubClient_->supported()) {
    QStringList seen;
    for (const HubPluginInfo& p : hubClient_->plugins()) {
      if (seen.contains(p.id)) continue;
      seen << p.id;
      const HubPluginInfo* best = newestOffer(p.id);
      if (best == nullptr) continue;
      QString state = QStringLiteral("install");
      QString installedVersion;
      for (const plugins::PluginInfo& c : manager_->hubCopies()) {
        if (c.manifest.id != best->id) continue;
        if (installedVersion.isEmpty() || compareVersionText(c.manifest.version.toString(), installedVersion) > 0) installedVersion = c.manifest.version.toString();
      }
      // A plugin installed another way (a file) or linked from a folder on this PC counts as installed: it is not offered
      // again under "Available on the server". A newer server version of an installed copy shows as an update; a linked
      // developer folder is never replaced by a server copy, so it stays "installed".
      const std::optional<plugins::PluginInfo> local = manager_->info(best->id);
      if (installedVersion.isEmpty() && local.has_value()) installedVersion = local->manifest.version.toString();
      if (!manager_->hubCopyDir(best->id, best->version, best->sha256).isEmpty() || (local.has_value() && local->devLinked)) state = QStringLiteral("installed");
      else if (!installedVersion.isEmpty()) state = QStringLiteral("update");
      if (hubBusyId_ == best->id) state = QStringLiteral("busy");
      // Required plugins that are not installed yet and that the Hub offers: "Also install SoulLink Core 0.3.1".
      QVariantList needsInstall;
      for (auto rit = best->requires_.begin(); rit != best->requires_.end(); ++rit) {
        if (manager_->info(rit.key()).has_value()) continue;
        if (const HubPluginInfo* dep = newestOffer(rit.key()); dep != nullptr)
          needsInstall.append(QVariantMap{{QStringLiteral("id"), dep->id}, {QStringLiteral("name"), dep->name}, {QStringLiteral("version"), dep->version}});
      }
      out.append(QVariantMap{
          {QStringLiteral("id"), best->id},
          {QStringLiteral("name"), best->name},
          {QStringLiteral("version"), best->version},
          {QStringLiteral("description"), best->description},
          {QStringLiteral("origin"), best->origin},
          {QStringLiteral("originText"), originText(best->origin)},
          {QStringLiteral("permissionsText"), permissionsText(best->permissions)},
          {QStringLiteral("writesGame"), changesGame(best->permissions)},
          {QStringLiteral("writesNote"), changesGame(best->permissions) ? changesGameNote() : QString()},
          {QStringLiteral("sizeText"), sizeText(best->size)},
          {QStringLiteral("state"), state},
          {QStringLiteral("installedVersion"), installedVersion},
          {QStringLiteral("needsInstall"), needsInstall},
      });
    }
  }
  hubPlugins_ = out;
  emit hubPluginsChanged();
}

void PluginsController::linkDevFolder(const QString& pathOrUrl) {
  if (!developerMode()) return;
  const QString path = localPath(pathOrUrl);
  if (path.isEmpty()) return;
  const auto r = manager_->linkDevFolder(path);
  if (r.ok) {
    const auto info = manager_->info(r.id);
    setMessage(tr("Linked %1. It reloads when its files change.").arg(info ? info->manifest.name : r.id), false);
  } else {
    setMessage(tr("Could not link: %1").arg(r.error), true);
  }
  refreshPlugins();
}

// "Load unpacked..." (boards 4d-2A and 4d-2B). The manifest is checked first; a problem installs nothing.
QVariantMap PluginsController::loadUnpacked(const QString& pathOrUrl) {
  QVariantMap res;
  const QString path = localPath(pathOrUrl);
  res.insert(QStringLiteral("path"), path);
  const auto fail = [&](const QString& name, const QString& detail) {
    res.insert(QStringLiteral("state"), QStringLiteral("error"));
    res.insert(QStringLiteral("title"), tr("%1 didn't load").arg(name));
    res.insert(QStringLiteral("detail"), detail);
    res.insert(QStringLiteral("note"), tr("Nothing was installed."));
    unpackedResult_ = res;
    emit unpackedResultChanged();
    return res;
  };
  if (!developerMode()) return fail(QFileInfo(path).fileName(), tr("Turn on Developer mode first."));
  if (path.isEmpty() || !QFileInfo(path).isDir()) return fail(QFileInfo(path).fileName(), tr("The folder was not found."));
  const plugins::ManifestResult mr = plugins::loadManifest(path);
  if (!mr.ok) {
    QString name = QFileInfo(path).fileName();
    QFile mf(QDir(path).filePath(QStringLiteral("manifest.json")));
    if (mf.open(QIODevice::ReadOnly)) {
      const QString n = QJsonDocument::fromJson(mf.readAll()).object().value(QLatin1String("name")).toString();
      if (!n.isEmpty()) name = n.left(64);
    }
    qCWarning(lcPlugins) << "load unpacked failed:" << mr.error.left(200);
    return fail(name, QStringLiteral("manifest.json: ") + mr.error);
  }
  const plugins::InstallResult r = manager_->linkDevFolder(path);
  if (!r.ok) return fail(mr.manifest.name, r.error);
  setPluginEnabled(r.id, true);
  refreshPlugins();
  devConsole_->selectPlugin(r.id);  // the console shows what was just loaded, not the plugin that was selected before
  res.insert(QStringLiteral("state"), QStringLiteral("ok"));
  res.insert(QStringLiteral("id"), r.id);
  res.insert(QStringLiteral("title"), tr("%1 loaded").arg(mr.manifest.name));
  res.insert(QStringLiteral("detail"), tr("manifest.json · %1 %2").arg(mr.manifest.name, mr.manifest.version.toString()));
  unpackedResult_ = res;
  emit unpackedResultChanged();
  setMessage(tr("Loaded %1. It reloads when its files change.").arg(mr.manifest.name), false);
  return res;
}

void PluginsController::dismissUnpackedResult() {
  if (unpackedResult_.isEmpty()) return;
  unpackedResult_.clear();
  emit unpackedResultChanged();
}

bool PluginsController::setIdentityOverride(const QString& system, const QString& code) {
  if (!developerMode()) return false;
  static const QRegularExpression sysRe(QStringLiteral("^[a-z0-9_-]{1,32}$"));
  static const QRegularExpression codeRe(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
  if (!sysRe.match(system).hasMatch() || !codeRe.match(code).hasMatch()) return false;
  identityOverride_ = IdentityOverride{system, code, QDateTime::currentMSecsSinceEpoch()};
  return true;
}

void PluginsController::clearIdentityOverride() { identityOverride_.reset(); }

QVariantMap PluginsController::identityOverride() const {
  if (!identityOverride_) return {};
  return {{QStringLiteral("system"), identityOverride_->system}, {QStringLiteral("code"), identityOverride_->code}};
}

void PluginsController::uninstall(const QString& id) {
  manager_->uninstall(id);
  refreshPlugins();
}

void PluginsController::setPluginEnabled(const QString& id, bool enabled) {
  // A plugin and the plugins it needs turn on together, so the toggles and the log agree (P20): without this the
  // dependency was started for the dependent while its own toggle still showed off.
  if (enabled) {
    QStringList todo{id};
    QStringList seen;
    while (!todo.isEmpty()) {
      const QString cur = todo.takeFirst();
      if (seen.contains(cur)) continue;
      seen << cur;
      const auto info = manager_->info(cur);
      if (!info) continue;
      for (const plugins::Dependency& d : info->manifest.dependencies) {
        const auto dep = manager_->info(d.id);
        if (dep && !dep->enabled) manager_->setEnabled(d.id, true);
        todo << d.id;
      }
    }
  }
  if (!manager_->setEnabled(id, enabled)) {
    setMessage(tr("This plugin cannot be turned on until its problems are fixed."), true);
  }
  refreshPlugins();
}

void PluginsController::refreshPlugins() {
  QVariantList out;
  int on = 0;
  int updates = 0;
  const auto list = manager_->list();
  for (const plugins::PluginInfo& p : list) {
    QStringList problems = p.problems;
    const bool hasProblems = !problems.isEmpty();
    if (!hasProblems && p.enabled) ++on;
    const bool isLocal = p.origin == QLatin1String("local");
    if (!hasProblems && isLocal && !manager_->developerMode()) problems << tr("Runs only while Developer mode is on");
    // "needs net.uzume.pokemon-core, not installed" -> "needs SoulLink Core, not installed" (the Hub's name when it offers it).
    for (QString& pr : problems) {
      // "Needs Player 0.16.4" (manifest field min_player, e.g. after going back to an older Player).
      static const QRegularExpression needsPlayer(QStringLiteral("^Needs Player (\\S+)$"));
      if (const auto np = needsPlayer.match(pr); np.hasMatch()) {
        pr = p.enabled ? tr("Off · Needs Uzume %1. Turns on again after you update.").arg(np.captured(1))
                       : tr("Needs Uzume %1. Cannot be turned on until you update.").arg(np.captured(1));
        continue;
      }
      static const QRegularExpression notInstalled(QStringLiteral("^needs (\\S+), not installed$"));
      const auto m = notInstalled.match(pr);
      if (!m.hasMatch()) continue;
      const HubPluginInfo* offer = newestOffer(m.captured(1));
      pr = offer != nullptr ? tr("needs %1, which is not installed").arg(offer->name) : tr("needs another plugin (%1) that is not installed").arg(m.captured(1));
    }
    QString problemsText = problems.join(QStringLiteral(" · "));
    if (!problemsText.isEmpty()) problemsText[0] = problemsText[0].toUpper();
    // Relations for the row subtitle and the detail sheet.
    QStringList needsNames;
    QStringList needsLines;
    QStringList depsOff;
    for (const plugins::Dependency& d : p.manifest.dependencies) {
      const auto dep = manager_->info(d.id);
      const QString depName = dep ? dep->manifest.name : (newestOffer(d.id) != nullptr ? newestOffer(d.id)->name : d.id);
      needsNames << depName;
      const QString range = rangeLabel(d.range.text);
      const QString need = range.isEmpty() ? depName : depName + QLatin1Char(' ') + range;
      needsLines << (dep ? tr("Needs %1 · installed %2").arg(need, dep->manifest.version.toString()) : tr("Needs %1 · not installed").arg(need));
      if (dep && !dep->enabled) depsOff << depName;
    }
    QStringList usedBy;
    for (const plugins::PluginInfo& o : list)
      for (const plugins::Dependency& d : o.manifest.dependencies)
        if (d.id == p.manifest.id) usedBy << o.manifest.name;
    QString subtitle;
    if (!usedBy.isEmpty()) subtitle = tr("Used by %1").arg(usedBy.join(QStringLiteral(", ")));
    else if (!needsNames.isEmpty()) subtitle = tr("Needs %1").arg(needsNames.join(QStringLiteral(", ")));
    {
      const QString perms = permissionsText(p.manifest.permissions);
      subtitle = subtitle.isEmpty() ? perms : subtitle + QStringLiteral(" · ") + perms;
    }
    QString updateVersion;
    if (hubClient_ != nullptr && hubClient_->supported()) {
      if (const HubPluginInfo* offer = newestOffer(p.manifest.id); offer != nullptr && compareVersionText(offer->version, p.manifest.version.toString()) > 0) {
        updateVersion = offer->version;
        ++updates;
      }
    }
    out.append(QVariantMap{
        {QStringLiteral("id"), p.manifest.id},
        {QStringLiteral("name"), p.manifest.name},
        {QStringLiteral("version"), p.manifest.version.toString()},
        {QStringLiteral("origin"), p.origin},
        {QStringLiteral("originText"), originText(p.origin)},
        {QStringLiteral("devLinked"), p.devLinked},
        {QStringLiteral("hubCopy"), p.hubCopy},
        {QStringLiteral("enabled"), p.enabled},
        {QStringLiteral("hasProblems"), hasProblems},
        {QStringLiteral("problemsText"), problemsText},
        {QStringLiteral("canToggle"), !hasProblems},
        {QStringLiteral("needsConfirm"), p.origin == QLatin1String("unverified")},
        {QStringLiteral("permissionsText"), permissionsText(p.manifest.permissions)},
        {QStringLiteral("writesGame"), changesGame(p.manifest.permissions)},
        {QStringLiteral("writesNote"), changesGame(p.manifest.permissions) ? changesGameNote() : QString()},
        {QStringLiteral("permissionRows"), permissionRows(p.manifest.permissions)},
        {QStringLiteral("updateVersion"), updateVersion},
        {QStringLiteral("adds"), p.manifest.ui.addsText()},
        {QStringLiteral("hasScreen"), p.manifest.ui.screen},
        {QStringLiteral("subtitle"), subtitle},
        {QStringLiteral("needsLines"), needsLines},
        {QStringLiteral("depsOff"), depsOff},
        {QStringLiteral("usedBy"), usedBy},
        {QStringLiteral("permissionList"), permissionLabels(p.manifest.permissions)},
    });
  }
  plugins_ = out;
  if (list.isEmpty()) {
    summary_ = tr("None installed");
  } else {
    // "2 installed · 1 on", or "2 installed · 1 update" when updates exist (round 3 sheet).
    summary_ = tr("%1 installed").arg(list.size());
    if (updates > 0) summary_ += QStringLiteral(" · ") + (updates == 1 ? tr("1 update") : tr("%1 updates").arg(updates));
    else summary_ += QStringLiteral(" · ") + (on > 0 ? tr("%1 on").arg(on) : tr("none on"));
  }
  updateImageSources();
  if (variants_ != nullptr) {
    // What a plugin registered through fb.variants stays only while the plugin is on and without problems.
    QSet<QString> active;
    for (const plugins::PluginInfo& p : list)
      if (p.enabled && p.problems.isEmpty() && p.manifest.hasPermission(QStringLiteral("variants.build"))) active.insert(p.manifest.id);
    variants_->registry()->keepOnly(active);
  }
  emit pluginsChanged();
  rebuildHotkeys();
  updateShell();  // a toggle, install or Developer mode change can add or remove shell plugins
}

void PluginsController::setVariantsProvider(std::shared_ptr<plugins::VariantsProvider> provider) {
  variants_ = std::move(provider);
  shellSignature_.clear();  // the shell plugins start again with fb.variants
  updateShell();
}

// ---------------------------------------------------------------- game lifecycle

QVariantMap PluginsController::hubOffer(const QString& pluginId, const QString& version) const {
  if (hubClient_ == nullptr) return {};
  for (const HubPluginInfo& p : hubClient_->plugins()) {
    if (p.id != pluginId || p.version != version) continue;
    return {{QStringLiteral("id"), p.id},
            {QStringLiteral("name"), p.name},
            {QStringLiteral("version"), p.version},
            {QStringLiteral("origin"), p.origin},
            {QStringLiteral("originText"), originText(p.origin)},
            {QStringLiteral("permissionsText"), permissionsText(p.permissions)},
            {QStringLiteral("writesGame"), changesGame(p.permissions)},
            {QStringLiteral("writesNote"), changesGame(p.permissions) ? changesGameNote() : QString()}};
  }
  return {};
}

void PluginsController::gameStarted(const QString& system, const QString& romPath, const QString& sha256, const QString& title,
                                    const QString& language, const RunLaunch& run) {
  if (inGame_ || running_) stopGame(false);
  inGame_ = true;
  language_ = language.isEmpty() ? resolveLanguage(system) : language;
  if (shellRunning_) {  // the shell set (and the run view) gives way to the game set
    host_->stop();
    shellRunning_ = false;
    shellIds_.clear();
    shellSignature_.clear();
  }
  runViewActive_ = false;
  runViewId_.clear();
  runViewKey_.clear();
  runViewPins_.clear();
  runViewBridge_.reset();
  runViewIds_.clear();
  runViewInfos_.clear();
  runViewProblems_.clear();
  rebuildRunSections();
  emit inGameChanged();
  plugins::GameContext ctx;
  ctx.system = system;
  ctx.code = emu::readGameIdentity(romPath, identityReader(system)).code;
  if (identityOverride_) {  // debug MCP game_start identity: used once, only while developer mode is on and not older than 30 s
    const IdentityOverride o = *identityOverride_;
    identityOverride_.reset();
    if (developerMode() && QDateTime::currentMSecsSinceEpoch() - o.setAtMs <= 30000) {
      qCInfo(lcPlugins) << "game identity overridden for plugin matching:" << o.system << o.code;
      ctx.system = o.system;
      ctx.code = o.code;
    }
  }
  ctx.sha256 = sha256;
  ctx.title = title;
  ctx.romPath = romPath;
  ctx.language = language_;
  ctx.run = run.bridge;
  ctx.gameId = launchGameId_;
  ctx.modDir = launchModDir_;
  ctx.variants = variants_;
  ensureStore();
  ctx.store = store_;
  ctx.sound = sound_;
  ctx.emulation = emulation_;
  gameCtx_ = ctx;
  gameSystem_ = ctx.system;
  gameCode_ = ctx.code;
  runLaunch_ = run;
  const plugins::GameResolution res = resolveGameSet();
  for (auto it = res.problems.begin(); it != res.problems.end(); ++it)
    qCInfo(lcPlugins) << "plugin" << it.key() << "not started:" << it.value().join(QStringLiteral("; "));
  // The service of the app (run rules, consent, cheat list) is there for every game, with or without a plugin: the run rule lives in
  // the app, so a run game without the cheats plugin still has cheats_list / cheats_apply (MCP) and the live rule toasts.
  emulation_->gameStarted(gameInfo_, run.bridge);
  if (res.start.isEmpty()) {
    rebuildUiModels();
    return;
  }

  startGameSet(res);
  rebuildUiModels();
}

// The plugins of a game: without a run the enabled plugins of the game; in a run the plugins pinned to the run (run.state), plus the
// person's own enabled plugins that change the game (permission cheats / memory.write) while the run's rules allow that.
plugins::GameResolution PluginsController::resolveGameSet() const {
  if (!runLaunch_.bridge) return manager_->resolveForGame(gameSystem_, gameCode_);
  plugins::GameResolution res = manager_->resolveForRun(runLaunch_.pins);
  const RunRules rules = parseRunRules(runLaunch_.bridge->current().value(QStringLiteral("rules")).toObject());
  const bool cheats = rules.cheatsAllowed();
  const bool memory = rules.memoryWriteAllowed();
  if (!cheats && !memory) return res;
  const plugins::GameResolution own = manager_->resolveForGame(gameSystem_, gameCode_);
  QSet<QString> have;  // pinned server copies win over a local copy with the same id
  QMap<QString, plugins::PluginInfo> byId;
  for (const plugins::PluginInfo& p : res.start) have.insert(p.manifest.id);
  for (const plugins::PluginInfo& p : own.start) byId.insert(p.manifest.id, p);
  QSet<QString> want;
  QStringList queue;
  for (const plugins::PluginInfo& p : own.start) {
    if (have.contains(p.manifest.id)) continue;
    if ((cheats && p.manifest.permissions.contains(QStringLiteral("cheats"))) ||
        (memory && p.manifest.permissions.contains(QStringLiteral("memory.write"))))
      queue << p.manifest.id;
  }
  while (!queue.isEmpty()) {
    const QString id = queue.takeFirst();
    if (want.contains(id) || have.contains(id) || !byId.contains(id)) continue;
    want.insert(id);
    for (const plugins::Dependency& d : byId.value(id).manifest.dependencies) queue << d.id;
  }
  for (const plugins::PluginInfo& p : own.start)  // dependency order of the resolver
    if (want.contains(p.manifest.id)) res.start.append(p);
  return res;
}

void PluginsController::startGameSet(const plugins::GameResolution& res) {
  runningIds_.clear();
  for (const plugins::PluginInfo& p : res.start) runningIds_ << p.manifest.id;
  loggedStatus_.clear();
  devConsole_->resetSeen();
  consoleRun_ = runLaunch_.bridge;
  running_ = true;
  if (storeSync_) {
    for (const plugins::PluginInfo& p : res.start)
      if (p.manifest.permissions.contains(QStringLiteral("store"))) storeSync_->pullPlugin(p.manifest.id);
  }
  adapter_->bind(session_ != nullptr ? session_->memoryWatch() : nullptr);
  if (session_ != nullptr) {
    video_ = std::make_shared<VideoCaptureAdapter>(session_);
    gameCtx_.video = video_;
  }
  host_->start(gameCtx_, adapter_, res.start);  // asynchronous: the game does not wait for the plugins
  postInitialEvents();
  setConsoleAvailable(developerMode());
  devConsole_->schedule();
  devConsole_->memoryRefresh();
}

void PluginsController::stopGameSet() {
  host_->stop();
  video_.reset();
  adapter_->clear();
  adapter_->bind(nullptr);
  for (const QString& id : std::as_const(runningIds_)) qCInfo(lcPlugins) << "plugin" << id << "stopped";
  running_ = false;
  runningIds_.clear();
  setConsoleAvailable(false);
  consoleRun_.reset();
  devConsole_->resetSeen();
  devConsole_->memoryRefresh();
}

// The rules of the run changed while the game runs: the person's own cheats / memory plugins start or stop with them. The pinned run
// plugins keep running when the set stays the same.
void PluginsController::refreshRunExtras() {
  if (!inGame_ || !runLaunch_.bridge) return;
  const plugins::GameResolution res = resolveGameSet();
  QStringList ids;
  for (const plugins::PluginInfo& p : res.start) ids << p.manifest.id;
  if (ids == runningIds_) return;
  if (running_) stopGameSet();
  if (!res.start.isEmpty()) startGameSet(res);
  rebuildUiModels();
}

void PluginsController::gameStopped() { stopGame(true); }

void PluginsController::stopGame(bool restartShell) {
  if (!inGame_ && !running_) return;
  if (running_) {
    emulation_->gameStopped();
    stopGameSet();
    if (!previewUi_ && !toasts_.isEmpty()) {  // notices of this game ("paused after an error", "Undo death") do not carry over into the next one
      toasts_.clear();
      emit toastsChanged();
    }
  } else if (emulation_) {
    emulation_->gameStopped();  // a game without plugins still had the app's service running
  }
  inGame_ = false;
  mutedSession_.clear();
  rebuildUiModels();
  if (restartShell) updateShell();
  emit inGameChanged();
}

// Shell context: while no game runs, the enabled plugins with ui.screen or ui.detail run with an empty game. While the Runs
// screen shows a run (run view), the run's pinned plugins with ui.run or ui.screen (plus what they require) join them in the same host,
// with a read-only fb.run; a plugin that is in both sets runs once, as the run view instance.
void PluginsController::updateShell() {
  if (inGame_) return;
  const plugins::GameResolution res = manager_->resolveForShell();
  QList<plugins::PluginInfo> start;
  QStringList rvOrder;
  QMap<QString, plugins::PluginInfo> rvInfos;
  QString rvProblems;
  if (runViewActive_) {
    const plugins::GameResolution rv = manager_->resolveForRun(runViewPins_);
    QMap<QString, plugins::PluginInfo> byId;
    QStringList queue;
    for (const plugins::PluginInfo& p : rv.start) {
      byId.insert(p.manifest.id, p);
      if (p.manifest.ui.run || p.manifest.ui.screen) queue << p.manifest.id;
    }
    QSet<QString> want;
    while (!queue.isEmpty()) {
      const QString id = queue.takeFirst();
      if (want.contains(id)) continue;
      want.insert(id);
      for (const plugins::Dependency& d : byId.value(id).manifest.dependencies)
        if (byId.contains(d.id)) queue << d.id;
    }
    for (const plugins::PluginInfo& p : rv.start) {
      if (!want.contains(p.manifest.id)) continue;
      start.append(p);
      rvOrder << p.manifest.id;
      rvInfos.insert(p.manifest.id, p);
    }
    QStringList lines;
    for (auto it = rv.problems.begin(); it != rv.problems.end(); ++it)
      for (const plugins::PluginInfo& c : manager_->hubCopies())
        if (c.manifest.id == it.key() && (c.manifest.ui.run || c.manifest.ui.screen)) {
          lines << QStringLiteral("%1: %2").arg(c.manifest.name, it.value().join(QStringLiteral("; ")));
          break;
        }
    rvProblems = lines.join(QLatin1Char('\n'));
  }
  QStringList ids;
  QStringList sig;
  for (const plugins::PluginInfo& p : res.start) {
    ids << p.manifest.id;
    if (!rvInfos.contains(p.manifest.id)) start.append(p);
  }
  for (const plugins::PluginInfo& p : std::as_const(start))
    sig << p.manifest.id + QLatin1Char('@') + p.manifest.version.toString() + QLatin1Char('@') + p.path;
  if (!rvOrder.isEmpty()) sig << QStringLiteral("run:") + runViewKey_;
  const QString signature = sig.join(QLatin1Char('|'));
  const bool rvChanged = rvOrder != runViewIds_ || rvProblems != runViewProblems_;
  runViewIds_ = rvOrder;
  runViewInfos_ = rvInfos;
  runViewProblems_ = rvProblems;
  if (signature == shellSignature_ && shellRunning_ == !start.isEmpty()) {
    if (rvChanged) rebuildRunSections();
    return;
  }
  shellSignature_ = signature;
  shellIds_ = ids;
  updateImageSources();
  if (start.isEmpty()) {
    if (shellRunning_) host_->stop();
    shellRunning_ = false;
    rebuildUiModels();
    return;
  }
  plugins::GameContext ctx;
  ctx.shell = true;
  ctx.variants = variants_;
  ensureStore();
  ctx.store = store_;
  ctx.sound = sound_;
  ctx.language = resolveLanguage();
  if (!rvOrder.isEmpty()) ctx.run = runViewBridge_;
  shellRunning_ = true;
  host_->start(ctx, nullptr, start);
  postInitialEvents();
  rebuildUiModels();
}

void PluginsController::startRunView(const RunLaunch& run, const QString& runId) {
  if (inGame_ || !run.bridge) return;
  QStringList sig;
  for (const plugins::RunPluginPin& p : run.pins) sig << p.id + QLatin1Char('@') + p.version + QLatin1Char('@') + p.sha256;
  const QString key = runId.toLower() + QLatin1Char('|') + sig.join(QLatin1Char(','));
  if (runViewActive_ && key == runViewKey_) return;  // the same run again: keep the running plugins
  runViewActive_ = true;
  runViewId_ = runId.toLower();
  runViewKey_ = key;
  runViewPins_ = run.pins;
  runViewBridge_ = std::make_shared<plugins::ReadOnlyRunBridge>(run.bridge);
  updateShell();
}

void PluginsController::stopRunView() {
  if (!runViewActive_) return;
  runViewActive_ = false;
  runViewId_.clear();
  runViewKey_.clear();
  runViewPins_.clear();
  runViewBridge_.reset();
  updateShell();  // restores the normal shell set
  runViewIds_.clear();
  runViewInfos_.clear();
  runViewProblems_.clear();
  rebuildRunSections();
}

std::optional<plugins::PluginInfo> PluginsController::infoFor(const QString& id) const {
  const auto it = runViewInfos_.constFind(id);
  if (it != runViewInfos_.constEnd()) return it.value();
  return manager_->info(id);
}

// Host events carry state, so a restarted plugin set gets the current values again.
void PluginsController::postInitialEvents() {
  if (!detailGame_.isEmpty()) host_->postHostEvent(QStringLiteral("detail.game"), detailGame_);
  if (!tiles_.isEmpty()) host_->postHostEvent(QStringLiteral("tiles"), tiles_);
  for (const QString& id : std::as_const(hudHidden_)) host_->postHostEvent(QStringLiteral("hud.visible"), false, id);
}

void PluginsController::onStatus(const QString& id) {
  const auto snap = host_->consoleSnapshot(id);
  const int st = static_cast<int>(snap.status);
  if (loggedStatus_.value(id, -1) == st) return;
  loggedStatus_.insert(id, st);
  if (snap.status == PluginStatus::Running) {
    qCInfo(lcPlugins) << "plugin" << id << "started";
  } else if (snap.status == PluginStatus::Paused) {
    qCWarning(lcPlugins) << "plugin" << id << "paused:" << snap.error.section(QLatin1Char('\n'), 0, 0);  // the notice comes from the DevConsole
  }
}

// ---------------------------------------------------------------- console

void PluginsController::setConsoleAvailable(bool on) {
  if (hasPreview_) on = true;
  if (consoleAvailable_ == on) return;
  consoleAvailable_ = on;
  emit consoleAvailableChanged();
}

// Plugins the developer console lists: the game's set while a game runs, else the shell set and the run view.
QStringList PluginsController::consoleIds() const {
  QStringList ids = activeIds();
  if (!inGame_)
    for (const QString& id : runViewIds_)
      if (!ids.contains(id)) ids << id;
  return ids;
}

// Inspect area of the console: the run state this plugin wrote (fb.run) and its keys in the plugin store (fb.store).
QVariantMap PluginsController::inspectOf(const QString& id) const {
  QVariantMap out;
  std::shared_ptr<plugins::RunBridge> run = consoleRun_ ? consoleRun_ : runViewBridge_;
  if (run) {
    const QJsonObject cur = run->current();
    if (!cur.isEmpty()) {
      out.insert(QStringLiteral("runName"), cur.value(QLatin1String("name")).toString());
      bool found = false;
      const QJsonObject st = run->stateOf(cur.value(QLatin1String("me")).toString(), id, &found);
      if (found) out.insert(QStringLiteral("runState"), QString::fromUtf8(QJsonDocument(st).toJson(QJsonDocument::Compact)).left(4000));
    }
  }
  QVariantList keys;
  if (store_) {
    for (const QString& k : store_->keys(id)) {
      const auto e = store_->get(id, k);
      if (!e.found) continue;
      QString text;
      const QJsonValue v = e.value;
      if (v.isObject()) text = QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
      else if (v.isArray()) text = QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
      else text = v.toVariant().toString();
      keys.append(QVariantMap{{QStringLiteral("key"), k}, {QStringLiteral("bytes"), text.toUtf8().size()}, {QStringLiteral("revision"), e.revision},
                              {QStringLiteral("preview"), text.left(120)}});
      if (keys.size() >= 100) break;
    }
  }
  out.insert(QStringLiteral("store"), keys);
  return out;
}

void PluginsController::selectConsolePlugin(const QString& id) { devConsole_->selectPlugin(id); }

void PluginsController::reloadPlugin(const QString& id) {
  emit reloadRequested(id);
  if (!((running_ && runningIds_.contains(id)) || (shellRunning_ && (shellIds_.contains(id) || runViewIds_.contains(id))))) return;
  qCInfo(lcPlugins) << "plugin" << id << "reloading";
  loggedStatus_.remove(id);
  host_->reload(id);
}

void PluginsController::reloadSelected() { reloadPlugin(devConsole_->selectedId()); }

void PluginsController::clearLog(const QString& id) { devConsole_->clearLog(id); }

void PluginsController::copyLog(const QString& id) {
  if (QGuiApplication::clipboard() != nullptr) QGuiApplication::clipboard()->setText(devConsole_->logText(id));
}

void PluginsController::setPreviewConsole(const QVariantMap& console) {
  hasPreview_ = !console.isEmpty();
  devConsole_->setPreview(console);
  consoleAvailable_ = !hasPreview_;  // force the signal below
  setConsoleAvailable(hasPreview_);
  emit consoleChanged();
}

// ---------------------------------------------------------------- plugin UI (fb.ui)

QStringList PluginsController::activeIds() const { return inGame_ ? runningIds_ : shellIds_; }

void PluginsController::updateImageSources() {
  QHash<QString, PluginImageSource> src;
  for (const plugins::PluginInfo& p : manager_->list()) src.insert(p.manifest.id, PluginImageSource{p.cacheDir, p.path, p.manifest.assets});
  for (const plugins::PluginInfo& p : std::as_const(runViewInfos_)) src.insert(p.manifest.id, PluginImageSource{p.cacheDir, p.path, p.manifest.assets});
  imageSources_->set(src);
}

QVariantMap PluginsController::slotTree(const QString& pluginId, const QString& slot) const {
  if (previewUi_) return previewUiData_.value(QStringLiteral("trees")).toMap().value(pluginId + QLatin1Char('|') + slot).toMap();
  return host_->uiTree(pluginId, slot);
}

QString PluginsController::pluginIconUrl(const QString& id) const {
  const auto info = infoFor(id);
  if (info && info->manifest.assets.contains(QStringLiteral("icon.png"))) return QStringLiteral("image://uzplugin/%1/asset/icon.png").arg(id);
  return {};
}

QString PluginsController::originMarker(const QString& origin) const {
  if (origin == QLatin1String("official")) return {};
  return origin == QLatin1String("unverified") ? tr("Unverified") : tr("Local");
}

void PluginsController::setPreviewUi(const QVariantMap& ui) {
  previewUi_ = !ui.isEmpty();
  previewUiData_ = ui;
  if (previewUi_) {
    panelTabs_ = ui.value(QStringLiteral("panelTabs")).toList();
    hudWidgets_ = ui.value(QStringLiteral("hudWidgets")).toList();
    toasts_ = ui.value(QStringLiteral("toasts")).toList();
    sidebarEntries_ = ui.value(QStringLiteral("sidebarEntries")).toList();
    emit panelTabsChanged();
    emit hudWidgetsChanged();
    emit toastsChanged();
    emit sidebarEntriesChanged();
    emit tileStripsChanged();
    rebuildHotkeys();
  } else {
    toasts_.clear();
    emit toastsChanged();
    rebuildUiModels();
  }
}

void PluginsController::onUiChanged(const QString& id, const QString& what) {
  if (previewUi_) return;
  if (what == QLatin1String("@toast")) {
    collectToasts();
    return;
  }
  if (what == QLatin1String("@close")) {
    for (const auto& r : host_->takeCloseRequests())
      if (activeIds().contains(r.first) || runViewIds_.contains(r.first)) emit closeRequested(r.first, r.second);
    return;
  }
  if (what == QLatin1String("@pick")) {
    for (const auto& r : host_->takePickRequests())
      if (activeIds().contains(r.pluginId) || runViewIds_.contains(r.pluginId)) {
        pickLimits_.insert(r.pluginId + QLatin1Char('/') + QString::number(r.id), r.maxBytes);
        pendingPicks_.append({r.pluginId, QString::number(r.id)});
        emit pickFileRequested(r.pluginId, QString::number(r.id), r.title, r.filters, static_cast<double>(r.maxBytes));
      } else
        host_->deliverFile(r.pluginId, r.id, {}, {}, QStringLiteral("cancelled"));
    return;
  }
  if (what == QLatin1String("@open")) {
    for (const auto& r : host_->takeOpenRequests())
      if (activeIds().contains(r.first) || runViewIds_.contains(r.first)) emit openRequested(r.first, r.second);
    return;
  }
  if (!activeIds().contains(id) && !runViewIds_.contains(id)) return;
  if (what == QLatin1String("@meta")) {
    rebuildPanelTabs();
    rebuildSidebar();
    rebuildHud();
    rebuildRunSections();
    return;
  }
  emit slotChanged(id, what);
  if (what == QLatin1String("run") || what == QLatin1String("screen")) rebuildRunSections();
  if (what == QLatin1String("hud")) rebuildHud();
  else if (what == QLatin1String("detail")) rebuildDetail();
  else if (what.startsWith(QLatin1String("tile:"))) emit tileStripsChanged();
  else if (what == QLatin1String("panel") || what == QLatin1String("screen")) {
    rebuildPanelTabs();
    rebuildSidebar();
  }
}

void PluginsController::rebuildUiModels() {
  if (previewUi_) return;
  rebuildPanelTabs();
  rebuildHud();
  rebuildSidebar();
  rebuildDetail();
  rebuildRunSections();
  rebuildHotkeys();
  emit tileStripsChanged();
}

void PluginsController::rebuildPanelTabs() {
  if (previewUi_) return;
  QVariantList out;
  for (const QString& id : activeIds()) {
    const auto info = infoFor(id);
    if (!info || !info->manifest.ui.panel) continue;
    const plugins::UiPluginState st = host_->uiState(id);
    const bool paused = st.paused || host_->consoleSnapshot(id).status == PluginStatus::Paused;
    out.append(QVariantMap{{QStringLiteral("pluginId"), id},
                           {QStringLiteral("name"), info->manifest.name},
                           {QStringLiteral("title"), st.titles.value(QStringLiteral("panel"), info->manifest.ui.panelTitle)},
                           {QStringLiteral("icon"), pluginIconUrl(id)},
                           {QStringLiteral("origin"), info->origin}, {QStringLiteral("originText"), originText(info->origin)},
                           {QStringLiteral("dot"), paused ? QStringLiteral("paused") : st.dot},
                           {QStringLiteral("paused"), paused}});
  }
  if (out == panelTabs_) return;
  panelTabs_ = out;
  emit panelTabsChanged();
}

void PluginsController::rebuildHud() {
  if (previewUi_) return;
  QVariantList out;
  for (const QString& id : activeIds()) {
    const auto info = infoFor(id);
    if (!info || !info->manifest.ui.hud) continue;
    const QVariantMap tree = host_->uiTree(id, QStringLiteral("hud"));
    if (tree.isEmpty()) continue;
    out.append(QVariantMap{{QStringLiteral("pluginId"), id},
                           {QStringLiteral("name"), info->manifest.name},
                           {QStringLiteral("icon"), pluginIconUrl(id)},
                           {QStringLiteral("corner"), hudCorner_.value(id, QStringLiteral("bottom-left"))},
                           {QStringLiteral("hidden"), hudHidden_.contains(id)},
                           {QStringLiteral("tree"), tree},
                           {QStringLiteral("origin"), info->origin}, {QStringLiteral("originText"), originText(info->origin)}});
  }
  hudWidgets_ = out;
  emit hudWidgetsChanged();
}

void PluginsController::rebuildSidebar() {
  if (previewUi_) return;
  QVariantList out;
  for (const QString& id : activeIds()) {
    const auto info = infoFor(id);
    if (!info || !info->manifest.ui.screen) continue;
    const plugins::UiPluginState st = host_->uiState(id);
    // A title that collides with a built-in sidebar entry ("Runs", "Library", ...) is shown as the plugin name (decision ba, 4f-3).
    QString title = st.titles.value(QStringLiteral("screen"), info->manifest.ui.screenTitle);
    static const QStringList builtIn{QStringLiteral("library"), QStringLiteral("runs"), QStringLiteral("settings")};
    if (builtIn.contains(title.trimmed().toLower())) title = info->manifest.name;
    out.append(QVariantMap{{QStringLiteral("pluginId"), id},
                           {QStringLiteral("name"), info->manifest.name},
                           {QStringLiteral("title"), title},
                           {QStringLiteral("icon"), pluginIconUrl(id)},
                           {QStringLiteral("badge"), st.badge},
                           {QStringLiteral("origin"), info->origin}, {QStringLiteral("originText"), originText(info->origin)}});
  }
  if (out == sidebarEntries_) return;
  sidebarEntries_ = out;
  emit sidebarEntriesChanged();
}

void PluginsController::rebuildDetail() {
  if (previewUi_) return;
  QVariantList out;
  for (const QString& id : activeIds()) {
    const auto info = infoFor(id);
    if (!info || !info->manifest.ui.detail) continue;
    const QVariantMap tree = host_->uiTree(id, QStringLiteral("detail"));
    if (tree.isEmpty()) continue;
    out.append(QVariantMap{{QStringLiteral("pluginId"), id}, {QStringLiteral("name"), info->manifest.name}, {QStringLiteral("origin"), info->origin}, {QStringLiteral("originText"), originText(info->origin)}, {QStringLiteral("tree"), tree}});
  }
  detailSections_ = out;
  emit detailSectionsChanged();
}

void PluginsController::rebuildRunSections() {
  if (previewUi_) return;
  QVariantList out;
  static const QStringList reservedTabs{QStringLiteral("board"), QStringLiteral("log")};
  for (const QString& id : std::as_const(runViewIds_)) {
    const auto info = infoFor(id);
    if (!info || !(info->manifest.ui.run || info->manifest.ui.screen)) continue;
    // The run tab shows the plugin's "screen" tree when it declares ui.screen, otherwise its "run" tree.
    const bool useScreen = info->manifest.ui.screen;
    const QString slot = useScreen ? QStringLiteral("screen") : QStringLiteral("run");
    const plugins::UiPluginState st = host_->uiState(id);
    const QVariantMap tree = host_->uiTree(id, slot);
    const PluginStatus status = host_->consoleSnapshot(id).status;
    const bool paused = st.paused || status == PluginStatus::Paused;
    // Tab name: ui.screen title, then ui.run title, then the plugin name ("Board" and "Log" belong to Uzume).
    QString title = useScreen ? st.titles.value(QStringLiteral("screen"), info->manifest.ui.screenTitle) : QString();
    if (title.trimmed().isEmpty() && info->manifest.ui.run) title = info->manifest.ui.runTitle;
    if (title.trimmed().isEmpty() || reservedTabs.contains(title.trimmed().toLower())) title = info->manifest.name;
    out.append(QVariantMap{{QStringLiteral("pluginId"), id},
                           {QStringLiteral("name"), info->manifest.name},
                           {QStringLiteral("title"), title},
                           {QStringLiteral("slot"), slot},
                           {QStringLiteral("badge"), st.badge},
                           {QStringLiteral("origin"), info->origin}, {QStringLiteral("originText"), originText(info->origin)},
                           {QStringLiteral("tree"), tree},
                           {QStringLiteral("paused"), paused},
                           {QStringLiteral("loading"), tree.isEmpty() && !paused && status == PluginStatus::Loading}});
  }
  if (out == runSections_ && runViewProblems_ == lastRunProblems_) return;
  runSections_ = out;
  lastRunProblems_ = runViewProblems_;
  emit runSectionsChanged();
}

QVariantList PluginsController::declaredHotkeyPlugins() const {
  if (previewUi_) return previewUiData_.value(QStringLiteral("hotkeyPlugins")).toList();
  QVariantList out;
  if (!manager_->developerMode()) return out;
  for (const plugins::PluginInfo& p : manager_->list()) {
    if (!p.enabled || !p.problems.isEmpty() || p.manifest.ui.hotkeys.isEmpty()) continue;
    QVariantList hks;
    for (const plugins::UiHotkey& hk : p.manifest.ui.hotkeys)
      hks.append(QVariantMap{{QStringLiteral("id"), hk.id}, {QStringLiteral("label"), hk.label}, {QStringLiteral("key"), hk.key}});
    out.append(QVariantMap{{QStringLiteral("pluginId"), p.manifest.id}, {QStringLiteral("name"), p.manifest.name},
                           {QStringLiteral("origin"), p.origin}, {QStringLiteral("hotkeys"), hks}});
  }
  return out;
}

void PluginsController::rebuildHotkeys() {
  bool changed = false;
  if (!previewUi_) {  // hotkeys of the running plugins
    QVariantList out;
    for (const QString& id : activeIds()) {
      const auto info = infoFor(id);
      if (!info) continue;
      for (const plugins::UiHotkey& hk : info->manifest.ui.hotkeys)
        out.append(QVariantMap{{QStringLiteral("pluginId"), id}, {QStringLiteral("id"), hk.id}, {QStringLiteral("label"), hk.label}, {QStringLiteral("key"), hk.key}});
    }
    if (out != hotkeys_) {
      hotkeys_ = out;
      changed = true;
    }
  }

  // Groups for Controllers > Hotkeys. Uzume keys always win, then the earlier plugin; a key in conflict is not active
  // until the user chooses another one.
  QVariantList groups;
  QSet<int> active;
  QHash<int, QStringList> taken, target;
  const QStringList running = activeIds();
  for (const QVariant& pv : declaredHotkeyPlugins()) {
    const QVariantMap plugin = pv.toMap();
    const QString pid = plugin.value(QStringLiteral("pluginId")).toString();
    const QString name = plugin.value(QStringLiteral("name")).toString();
    QVariantList rows;
    int conflicts = 0;
    for (const QVariant& hv : plugin.value(QStringLiteral("hotkeys")).toList()) {
      const QVariantMap hk = hv.toMap();
      const QString id = hk.value(QStringLiteral("id")).toString();
      const QString ov = hotkeyOverrides_.value(pid + QLatin1Char('/') + id);
      const QString eff = ov.isEmpty() ? hk.value(QStringLiteral("key")).toString() : (ov == QLatin1String("none") ? QString() : ov);
      const int combo = comboFromText(eff);
      QString owner;
      if (combo != 0) {
        if (reservedKeys_.contains(combo)) owner = reservedKeys_.value(combo);
        else if (taken.contains(combo)) owner = taken.value(combo).at(1);
      }
      const bool ok = combo != 0 && owner.isEmpty();
      if (ok) {
        taken.insert(combo, {pid + QLatin1Char('/') + id, name});
        if (previewUi_ || running.contains(pid)) {
          active.insert(combo);
          target.insert(combo, {pid, id});
        }
      }
      if (!owner.isEmpty()) ++conflicts;
      const QString text = combo != 0 ? textFromCombo(combo) : QString();
      rows.append(QVariantMap{
          {QStringLiteral("pluginId"), pid},
          {QStringLiteral("id"), id},
          {QStringLiteral("label"), hk.value(QStringLiteral("label")).toString()},
          {QStringLiteral("key"), text},
          {QStringLiteral("keyText"), text.isEmpty() ? tr("not set") : (owner.isEmpty() ? text : tr("%1 · not active").arg(text))},
          {QStringLiteral("set"), !text.isEmpty()},
          {QStringLiteral("active"), ok},
          {QStringLiteral("conflict"), !owner.isEmpty()},
          {QStringLiteral("changed"), !ov.isEmpty()},
          {QStringLiteral("listening"), hotkeyListening_ == pid + QLatin1Char('/') + id},
          {QStringLiteral("note"), owner.isEmpty() ? QString()
                                   : reservedKeys_.contains(combo) ? tr("Already used by %1. Uzume keys always win.").arg(owner)
                                                                    : tr("Already used by %1. Unassigned until you choose.").arg(owner)}});
    }
    const QString origin = plugin.value(QStringLiteral("origin")).toString();
    groups.append(QVariantMap{{QStringLiteral("pluginId"), pid},
                              {QStringLiteral("name"), name},
                              {QStringLiteral("origin"), origin},
                              {QStringLiteral("marker"), originMarker(origin)},
                              {QStringLiteral("count"), rows.size()},
                              {QStringLiteral("conflicts"), conflicts},
                              {QStringLiteral("rows"), rows}});
  }
  if (groups != hotkeyGroups_) {
    hotkeyGroups_ = groups;
    changed = true;
  }
  takenBy_ = taken;
  comboTarget_ = target;
  if (changed) emit hotkeysChanged();
  if (active != activeCombos_) {
    activeCombos_ = active;
    emit activeHotkeyCombosChanged();
  }
}

void PluginsController::setReservedKeys(const QMap<int, QString>& reserved) {
  if (reserved == reservedKeys_) return;
  reservedKeys_ = reserved;
  rebuildHotkeys();
}

void PluginsController::setHotkeyOverride(const QString& pluginId, const QString& hotkeyId, const QString& key) {
  QString declared;
  for (const QVariant& pv : declaredHotkeyPlugins()) {
    const QVariantMap p = pv.toMap();
    if (p.value(QStringLiteral("pluginId")).toString() != pluginId) continue;
    for (const QVariant& hv : p.value(QStringLiteral("hotkeys")).toList())
      if (hv.toMap().value(QStringLiteral("id")).toString() == hotkeyId) declared = hv.toMap().value(QStringLiteral("key")).toString();
  }
  QString stored = key;
  if (key == QLatin1String("none") ? comboFromText(declared) == 0 : (!key.isEmpty() && comboFromText(key) == comboFromText(declared))) stored.clear();
  const QString k = pluginId + QLatin1Char('/') + hotkeyId;
  if (stored.isEmpty()) hotkeyOverrides_.remove(k);
  else hotkeyOverrides_.insert(k, stored);
  if (settings_ != nullptr) settings_->setPluginHotkeyOverride(pluginId, hotkeyId, stored);
  rebuildHotkeys();
}

void PluginsController::beginHotkeyCapture(const QString& pluginId, const QString& hotkeyId) {
  const QString k = pluginId + QLatin1Char('/') + hotkeyId;
  bool known = false;
  for (const QVariant& gv : std::as_const(hotkeyGroups_))
    for (const QVariant& rv : gv.toMap().value(QStringLiteral("rows")).toList())
      if (rv.toMap().value(QStringLiteral("pluginId")).toString() == pluginId && rv.toMap().value(QStringLiteral("id")).toString() == hotkeyId) known = true;
  if (!known) return;
  hotkeyListening_ = k;
  hotkeyNote_.clear();
  rebuildHotkeys();
  emit hotkeysChanged();
}

void PluginsController::cancelHotkeyCapture() {
  if (hotkeyListening_.isEmpty() && hotkeyNote_.isEmpty()) return;
  hotkeyListening_.clear();
  hotkeyNote_.clear();
  rebuildHotkeys();
  emit hotkeysChanged();
}

bool PluginsController::captureHotkey(int qtKey, int modifiers) {
  if (hotkeyListening_.isEmpty()) return false;
  if (qtKey == Qt::Key_Escape) {
    cancelHotkeyCapture();
    return true;
  }
  if (qtKey == 0 || qtKey == Qt::Key_unknown || qtKey == Qt::Key_Shift || qtKey == Qt::Key_Control || qtKey == Qt::Key_Alt ||
      qtKey == Qt::Key_Meta || qtKey == Qt::Key_AltGr)
    return true;
  const int combo = qtKey | int(Qt::KeyboardModifiers::fromInt(modifiers) & kComboMods);
  QString owner = reservedKeys_.value(combo);
  if (owner.isEmpty() && takenBy_.contains(combo) && takenBy_.value(combo).at(0) != hotkeyListening_) owner = takenBy_.value(combo).at(1);
  if (!owner.isEmpty()) {
    hotkeyNote_ = tr("Already used by %1").arg(owner);  // the field keeps listening
    emit hotkeysChanged();
    return true;
  }
  const int slash = hotkeyListening_.indexOf(QLatin1Char('/'));
  const QString pid = hotkeyListening_.left(slash), id = hotkeyListening_.mid(slash + 1);
  hotkeyListening_.clear();
  hotkeyNote_.clear();
  setHotkeyOverride(pid, id, textFromCombo(combo));
  emit hotkeysChanged();
  return true;
}

void PluginsController::clearHotkey(const QString& pluginId, const QString& hotkeyId) {
  hotkeyListening_.clear();
  hotkeyNote_.clear();
  setHotkeyOverride(pluginId, hotkeyId, QStringLiteral("none"));
  emit hotkeysChanged();
}

void PluginsController::resetHotkey(const QString& pluginId, const QString& hotkeyId) {
  hotkeyListening_.clear();
  hotkeyNote_.clear();
  setHotkeyOverride(pluginId, hotkeyId, QString());
  emit hotkeysChanged();
}

QVariantList PluginsController::tileStrips(const QString& tileId) const {
  if (previewUi_) return previewUiData_.value(QStringLiteral("tileStrips")).toMap().value(tileId).toList();
  QVariantList out;
  const QString slot = QStringLiteral("tile:") + tileId;
  for (const QString& id : activeIds()) {
    const auto info = infoFor(id);
    if (!info || !info->manifest.ui.tile) continue;
    const QVariantMap tree = host_->uiTree(id, slot);
    if (!tree.isEmpty())
      out.append(QVariantMap{{QStringLiteral("pluginId"), id}, {QStringLiteral("name"), info->manifest.name}, {QStringLiteral("icon"), pluginIconUrl(id)},
                             {QStringLiteral("origin"), info->origin}, {QStringLiteral("originText"), originText(info->origin)}, {QStringLiteral("tree"), tree}});
  }
  return out;
}

void PluginsController::collectToasts() {
  if (previewUi_) return;
  bool changed = false;
  for (const QVariantMap& t : host_->takeToasts()) {
    const QString id = t.value(QStringLiteral("pluginId")).toString();
    if (mutedAlways_.contains(id) || mutedSession_.contains(id)) continue;
    if (mutedImportant_.contains(id) && t.value(QStringLiteral("tone")).toString() != QLatin1String("important")) continue;
    const auto info = infoFor(id);
    QVariantMap toast = t;
    toast.insert(QStringLiteral("pluginName"), info ? info->manifest.name : id);
    toast.insert(QStringLiteral("origin"), info ? info->origin : QStringLiteral("local"));
    toasts_.append(toast);
    logToast(toast);
    while (toasts_.size() > 10) toasts_.removeFirst();
    changed = true;
  }
  if (changed) emit toastsChanged();
}

QVariantList PluginsController::pendingPicks() const {
  QVariantList out;
  for (const auto& p : pendingPicks_) out.append(QVariantMap{{QStringLiteral("pluginId"), p.first}, {QStringLiteral("requestId"), p.second}});
  return out;
}

void PluginsController::completePick(const QString& pluginId, const QString& requestId, const QString& pathOrUrl) {
  bool ok = false;
  const quint64 id = requestId.toULongLong(&ok);
  if (!ok) return;
  pendingPicks_.removeAll({pluginId, requestId});
  emit pickCompleted(pluginId, requestId);
  if (pathOrUrl.isEmpty()) {
    host_->deliverFile(pluginId, id, {}, {}, QStringLiteral("cancelled"));
    return;
  }
  const qint64 maxBytes = pickLimits_.value(pluginId + QLatin1Char('/') + requestId, 1024 * 1024);
  pickLimits_.remove(pluginId + QLatin1Char('/') + requestId);
  const plugins::PickedFile f = plugins::readPickedFile(localPath(pathOrUrl), maxBytes);
  host_->deliverFile(pluginId, id, f.content, f.name, f.error);
}

void PluginsController::addAppNotice(const PluginEmulationService::Notice& n) {
  if (previewUi_) return;
  if (!n.pluginId.isEmpty()) {
    if (mutedAlways_.contains(n.pluginId) || mutedSession_.contains(n.pluginId)) return;
    if (mutedImportant_.contains(n.pluginId) && n.tone != QLatin1String("important")) return;
  }
  const QVariantMap toast{{QStringLiteral("toastId"), --hostToastId_},
                          {QStringLiteral("host"), true},
                          {QStringLiteral("pluginId"), n.pluginId},
                          {QStringLiteral("pluginName"), n.pluginName.isEmpty() ? tr("Uzume") : n.pluginName},
                          {QStringLiteral("tone"), n.tone},
                          {QStringLiteral("title"), n.title},
                          {QStringLiteral("text"), n.text},
                          {QStringLiteral("actions"), QVariantList()},
                          {QStringLiteral("origin"), QStringLiteral("official")}};
  toasts_.append(toast);
  logToast(toast);
  while (toasts_.size() > 10) toasts_.removeFirst();
  emit toastsChanged();
}

void PluginsController::logToast(const QVariantMap& t) {
  toastLog_.append(QVariantMap{{QStringLiteral("source"), t.value(QStringLiteral("pluginId")).toString().isEmpty() ? QStringLiteral("app") : QStringLiteral("plugin")},
                               {QStringLiteral("pluginId"), t.value(QStringLiteral("pluginId")).toString()},
                               {QStringLiteral("pluginName"), t.value(QStringLiteral("pluginName")).toString()},
                               {QStringLiteral("tone"), t.value(QStringLiteral("tone")).toString()},
                               {QStringLiteral("title"), t.value(QStringLiteral("title")).toString()},
                               {QStringLiteral("text"), t.value(QStringLiteral("text")).toString()},
                               {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
  while (toastLog_.size() > 20) toastLog_.removeFirst();
}

// The local copy of the plugin data follows the connected server and user; without a profile it is the "local" one.
void PluginsController::ensureStore() {
  QString hub = conn_ != nullptr ? conn_->hubId() : QString();
  QString user = conn_ != nullptr ? conn_->hubUserId() : QString();
  if (hub.isEmpty() || user.isEmpty()) {
    if (store_) return;  // keep the last one
    hub = QStringLiteral("local");
    user = QStringLiteral("local");
  }
  const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9._-]"));
  hub.replace(bad, QStringLiteral("_"));
  user.replace(bad, QStringLiteral("_"));
  const QString key = hub + QLatin1Char('/') + user;
  if (store_ && key == storeKey_) return;
  storeKey_ = key;
  store_ = std::make_shared<plugins::PluginStore>(QDir(dataDir_).filePath(QStringLiteral("plugins/store/") + key));
  storeSync_.reset();
  if (conn_ != nullptr) {
    storeSync_ = std::make_unique<plugins::PluginStoreSync>(store_, conn_);
    store_->setChangedCallback([this](const QString&) { QMetaObject::invokeMethod(this, [this]() { if (storeSync_) storeSync_->pushSoon(); }, Qt::QueuedConnection); });
    connect(storeSync_.get(), &plugins::PluginStoreSync::keyAdopted, this,
            [this](const QString& pid, const QString& k) { if (host_) host_->postAppEvent(QStringLiteral("store.changed"), k, pid); });
    connect(storeSync_.get(), &plugins::PluginStoreSync::keyConflicted, this,
            [this](const QString& pid, const QString& k) { if (host_) host_->postAppEvent(QStringLiteral("store.conflict"), k, pid); });
  }
}

// Host notice "Uzume · plugin paused" (not mutable: it is about the Player's own plugin handling).
// "<Plugin> paused after an error" (board 4d-2D): Restart plugin, and in Developer mode Open console. The game keeps running.
void PluginsController::addHostNotice(const QString& pluginId) {
  if (previewUi_ || !activeIds().contains(pluginId)) return;
  const auto info = infoFor(pluginId);
  if (!info) return;
  QVariantList actions{QVariantMap{{QStringLiteral("label"), tr("Restart plugin")}, {QStringLiteral("action"), QStringLiteral("restart")}, {QStringLiteral("value"), pluginId}}};
  if (developerMode())
    actions.append(QVariantMap{{QStringLiteral("label"), tr("Open console")}, {QStringLiteral("action"), QStringLiteral("details")}, {QStringLiteral("value"), pluginId}});
  toasts_.append(QVariantMap{{QStringLiteral("toastId"), --hostToastId_},
                             {QStringLiteral("host"), true},
                             {QStringLiteral("pluginId"), pluginId},
                             {QStringLiteral("pluginName"), tr("Uzume")},
                             {QStringLiteral("tone"), QStringLiteral("warning")},
                             {QStringLiteral("title"), tr("%1 paused after an error").arg(info->manifest.name)},
                             {QStringLiteral("text"), tr("Your game keeps running.")},
                             {QStringLiteral("actions"), actions},
                             {QStringLiteral("origin"), QStringLiteral("official")}});
  while (toasts_.size() > 10) toasts_.removeFirst();
  emit toastsChanged();
}

// "<Plugin> reloaded · 38 ms" (board 4d-2E), Developer mode only.
void PluginsController::addReloadNotice(const QString& pluginId, const QString& name, qint64 ms) {
  if (previewUi_ || !developerMode()) return;
  toasts_.append(QVariantMap{{QStringLiteral("toastId"), --hostToastId_},
                             {QStringLiteral("host"), true},
                             {QStringLiteral("pluginId"), pluginId},
                             {QStringLiteral("pluginName"), tr("Uzume")},
                             {QStringLiteral("tone"), QStringLiteral("info")},
                             {QStringLiteral("title"), tr("%1 reloaded · %2 ms").arg(name).arg(ms)},
                             {QStringLiteral("text"), QString()},
                             {QStringLiteral("actions"), QVariantList()},
                             {QStringLiteral("origin"), QStringLiteral("official")}});
  while (toasts_.size() > 10) toasts_.removeFirst();
  emit toastsChanged();
}

void PluginsController::dismissToast(int toastId) {
  for (int i = 0; i < toasts_.size(); ++i)
    if (toasts_[i].toMap().value(QStringLiteral("toastId")).toInt() == toastId) {
      toasts_.removeAt(i);
      emit toastsChanged();
      return;
    }
}

void PluginsController::muteToasts(const QString& pluginId, const QString& mode) {
  mutedSession_.remove(pluginId);
  mutedAlways_.remove(pluginId);
  mutedImportant_.remove(pluginId);
  QString persisted;
  if (mode == QLatin1String("session")) mutedSession_.insert(pluginId);
  else if (mode == QLatin1String("always")) {
    mutedAlways_.insert(pluginId);
    persisted = mode;
  } else if (mode == QLatin1String("important")) {
    mutedImportant_.insert(pluginId);
    persisted = mode;
  } else if (mode != QLatin1String("off")) return;
  if (settings_ != nullptr) settings_->setPluginToastMute(pluginId, persisted);
  QVariantList kept;
  for (const QVariant& v : std::as_const(toasts_)) {
    const QVariantMap t = v.toMap();
    if (t.value(QStringLiteral("host")).toBool() || t.value(QStringLiteral("pluginId")).toString() != pluginId) {
      kept.append(v);
    } else if (mode == QLatin1String("important") && t.value(QStringLiteral("tone")).toString() == QLatin1String("important")) {
      kept.append(v);
    }
  }
  if (kept.size() != toasts_.size()) {
    toasts_ = kept;
    emit toastsChanged();
  }
}

QString PluginsController::toastMuteMode(const QString& pluginId) const {
  if (mutedAlways_.contains(pluginId)) return QStringLiteral("always");
  if (mutedImportant_.contains(pluginId)) return QStringLiteral("important");
  if (mutedSession_.contains(pluginId)) return QStringLiteral("session");
  return {};
}

void PluginsController::sendAction(const QString& pluginId, const QString& slot, const QString& action, const QVariant& value) {
  emit actionSent(pluginId, slot, action, value);
  if (emulation_) emulation_->noteUserAction();  // a click in a plugin's UI: a waiting consent question may be asked now
  if ((!activeIds().contains(pluginId) && !runViewIds_.contains(pluginId)) || action.isEmpty()) return;
  host_->sendAction(pluginId, slot, action, value);
}

void PluginsController::triggerHotkey(const QString& pluginId, const QString& hotkeyId) {
  bool known = false;
  if (previewUi_) {
    for (const QVariant& pv : declaredHotkeyPlugins()) {
      const QVariantMap p = pv.toMap();
      if (p.value(QStringLiteral("pluginId")).toString() != pluginId) continue;
      for (const QVariant& hv : p.value(QStringLiteral("hotkeys")).toList())
        if (hv.toMap().value(QStringLiteral("id")).toString() == hotkeyId) known = true;
    }
  } else if (activeIds().contains(pluginId)) {
    if (const auto info = infoFor(pluginId))
      for (const plugins::UiHotkey& hk : info->manifest.ui.hotkeys)
        if (hk.id == hotkeyId) known = true;
  }
  if (known) sendAction(pluginId, QStringLiteral("hotkey"), hotkeyId);  // on_action(id, nil, "hotkey")
}

QString PluginsController::activeHotkeyKey(const QString& pluginId, const QString& hotkeyId) const {
  for (const QVariant& gv : hotkeyGroups_)
    for (const QVariant& rv : gv.toMap().value(QStringLiteral("rows")).toList()) {
      const QVariantMap r = rv.toMap();
      if (r.value(QStringLiteral("pluginId")).toString() == pluginId && r.value(QStringLiteral("id")).toString() == hotkeyId && r.value(QStringLiteral("active")).toBool())
        return r.value(QStringLiteral("key")).toString();
    }
  return {};
}

bool PluginsController::triggerHotkeyKey(int qtKey, int modifiers) {
  const int combo = qtKey | int(Qt::KeyboardModifiers::fromInt(modifiers) & kComboMods);
  const QStringList t = comboTarget_.value(combo);
  if (t.size() != 2) return false;
  triggerHotkey(t.at(0), t.at(1));
  return true;
}

void PluginsController::setDetailGame(const QString& system, const QString& code, const QString& sha256, const QString& title) {
  QVariantMap g;
  if (!system.isEmpty())
    g = QVariantMap{{QStringLiteral("system"), system}, {QStringLiteral("code"), code}, {QStringLiteral("sha256"), sha256}, {QStringLiteral("title"), title}};
  if (g == detailGame_) return;
  detailGame_ = g;
  host_->clearSlot(QStringLiteral("detail"));  // the host clears the section; the plugin sets a new one on the event
  host_->postHostEvent(QStringLiteral("detail.game"), g.isEmpty() ? QVariant() : QVariant(g));
}

QVariantList PluginsController::pluginsForGame(const QString& system, const QString& code) const {
  QVariantList out;
  if (system.isEmpty()) return out;
  for (const plugins::PluginInfo& p : manager_->list())
    if (p.enabled && p.problems.isEmpty() && manager_->developerMode() && p.manifest.matchesGame(system, code))
      out.append(QVariantMap{{QStringLiteral("id"), p.manifest.id}, {QStringLiteral("name"), p.manifest.name}, {QStringLiteral("origin"), p.origin}});
  return out;
}

QString PluginsController::identityReader(const QString& system) const {
  const emu::SystemManifest* m = manifests_ ? manifests_->find(system) : nullptr;
  return m ? m->identity : QString();
}

QString PluginsController::gameCode(const QString& system, const QString& romPath) const {
  return romPath.isEmpty() || !QFileInfo::exists(romPath) ? QString() : emu::readGameIdentity(romPath, identityReader(system)).code;
}

void PluginsController::setTiles(const QVariantList& tiles) {
  if (tiles == tiles_) return;
  tiles_ = tiles;
  emit tilesSet(tiles_);
  host_->postHostEvent(QStringLiteral("tiles"), tiles_);
}

void PluginsController::setHudCorner(const QString& pluginId, const QString& corner) {
  // The top-right corner belongs to the diagnostics overlay.
  static const QStringList ok = {QStringLiteral("top-left"), QStringLiteral("bottom-left"), QStringLiteral("bottom-right")};
  if (!ok.contains(corner) || hudCorner_.value(pluginId, QStringLiteral("bottom-left")) == corner) return;
  hudCorner_.insert(pluginId, corner);
  if (settings_ != nullptr) settings_->setPluginHudCorner(pluginId, corner);
  rebuildHud();
  if (previewUi_) applyPreviewHud();
}

void PluginsController::hideHud(const QString& pluginId) {
  if (hudHidden_.contains(pluginId)) return;
  hudHidden_.insert(pluginId);
  if (settings_ != nullptr) settings_->setPluginHudHidden(pluginId, true);
  host_->postHostEvent(QStringLiteral("hud.visible"), false, pluginId);
  rebuildHud();
  if (previewUi_) applyPreviewHud();
}

void PluginsController::showHud(const QString& pluginId) {
  if (!hudHidden_.remove(pluginId)) return;
  if (settings_ != nullptr) settings_->setPluginHudHidden(pluginId, false);
  host_->postHostEvent(QStringLiteral("hud.visible"), true, pluginId);
  rebuildHud();
  if (previewUi_) applyPreviewHud();
}

void PluginsController::setHudAllHidden(bool hidden) {
  if (hidden == hudAllHidden_) return;
  hudAllHidden_ = hidden;
  if (settings_ != nullptr) settings_->setPluginHudAllHidden(hidden);
  emit hudAllHiddenChanged();
}

// Preview models carry corner/hidden in the entries; keep them in step with the setters above.
void PluginsController::applyPreviewHud() {
  QVariantList out;
  for (const QVariant& v : std::as_const(hudWidgets_)) {
    QVariantMap w = v.toMap();
    const QString id = w.value(QStringLiteral("pluginId")).toString();
    if (hudCorner_.contains(id)) w.insert(QStringLiteral("corner"), hudCorner_.value(id));
    w.insert(QStringLiteral("hidden"), hudHidden_.contains(id));
    out.append(w);
  }
  hudWidgets_ = out;
  emit hudWidgetsChanged();
}

}  // namespace uzume::ui
