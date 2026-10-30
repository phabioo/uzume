#include "updatescontroller.h"
#include "timeformat.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QLocale>
#include <QUrl>

#include "semver.h"
#include "version.h"

namespace uzume::ui {

using update::UpdateManager;

namespace {

UpdateManager::Config makeConfig(const UpdatesController::Options& o) {
  UpdateManager::Config c;
  c.baseDir = o.baseDir;
  c.indexUrl = o.indexUrl;
  c.currentVersion = QString::fromUtf8(playerVersion().data(), static_cast<qsizetype>(playerVersion().size()));
  c.compiledChannel = QString::fromUtf8(playerChannel().data(), static_cast<qsizetype>(playerChannel().size()));
  c.installRoot = o.installRoot;
  c.forceApplySupport = o.forceApplySupport;
  c.protocolVersion = o.protocolVersion;
  return c;
}

}  // namespace

UpdatesController::UpdatesController(const Options& options, PlayerSettings* settings, QObject* parent)
    : QObject(parent), manager_(makeConfig(options), settings), settings_(settings) {
  connect(&manager_, &UpdateManager::changed, this, &UpdatesController::changed);
  connect(&manager_, &UpdateManager::changed, this, [this]() {
    if (!whatsNewDecided_ && manager_.rollbackListLoaded()) {
      whatsNewDecided_ = true;
      applyWhatsNew(manager_.installedNotes());
    }
  });
  connect(&manager_, &UpdateManager::quitRequested, this, &UpdatesController::quitRequested);
}

void UpdatesController::setProviders(std::function<std::optional<update::HubProtocol>()> hub, std::function<bool()> busy) {
  manager_.setHubProtocolProvider(std::move(hub));
  manager_.setBusyProvider(std::move(busy));
}

QString UpdatesController::shown(const QString& version) const {
  return QString::fromStdString(displayVersion(version.toStdString()));
}

QString UpdatesController::compiledChannel() const {
  return QString::fromUtf8(playerChannel().data(), static_cast<qsizetype>(playerChannel().size()));
}

QString UpdatesController::channelSetting() const {
  const QString c = settings_->updateChannel();
  return c.isEmpty() ? QStringLiteral("default") : c;
}

QString UpdatesController::effectiveChannel() const { return update::channelName(manager_.effectiveChannel()); }

QString UpdatesController::state() const {
  switch (manager_.state()) {
    case UpdateManager::State::Idle: return QStringLiteral("idle");
    case UpdateManager::State::Disabled: return QStringLiteral("disabled");
    case UpdateManager::State::Checking: return QStringLiteral("checking");
    case UpdateManager::State::UpToDate: return QStringLiteral("up_to_date");
    case UpdateManager::State::Available: return QStringLiteral("available");
    case UpdateManager::State::Incompatible: return QStringLiteral("incompatible");
    case UpdateManager::State::Downloading: return QStringLiteral("downloading");
    case UpdateManager::State::Ready: return QStringLiteral("ready");
    case UpdateManager::State::Applying: return QStringLiteral("applying");
    case UpdateManager::State::Error: return QStringLiteral("error");
  }
  return QStringLiteral("idle");
}

QString UpdatesController::statusText() const {
  if (manager_.state() == UpdateManager::State::Idle) {
    return manager_.effectiveChannel() == update::Channel::Off
               ? tr("Updates are off for this development build. Choose a channel to check for updates.")
               : tr("Not checked yet.");
  }
  return manager_.statusText();
}

QString UpdatesController::lastCheckText() const {
  const QDateTime t = manager_.lastCheck();
  return t.isValid() ? TimeFormat::when(t) : tr("never");
}

bool UpdatesController::updateAvailable() const {
  if (manager_.availableVersion().isEmpty() || manager_.selectionSkipped()) return false;  // skipped: listed, not nagged
  switch (manager_.state()) {
    case UpdateManager::State::Available:
    case UpdateManager::State::Downloading:
    case UpdateManager::State::Ready:
    case UpdateManager::State::Applying: return true;
    default: return false;
  }
}

// Cadence of the periodic check (core/updater.cpp: timer 1 h on Beta, 24 h on Stable).
QString UpdatesController::cadenceText() const {
  switch (manager_.effectiveChannel()) {
    case update::Channel::Beta: return tr("checks every hour while Uzume runs");
    case update::Channel::Stable: return tr("checks every 24 hours while Uzume runs");
    default: return {};
  }
}

bool UpdatesController::canInstall() const {
  const auto s = manager_.state();
  return manager_.applySupported() && (s == UpdateManager::State::Available || s == UpdateManager::State::Ready) &&
         !manager_.busy();
}

QString UpdatesController::bannerAction() const {
  const auto s = manager_.state();
  if (s == UpdateManager::State::Ready && manager_.applySupported()) {
    return manager_.autoInstallEffective() ? QStringLiteral("restart") : QStringLiteral("install");
  }
  if (s == UpdateManager::State::Available) {
    if (manager_.applySupported()) return QStringLiteral("install");
    return manager_.notesUrl().isEmpty() ? QStringLiteral("none") : QStringLiteral("link");
  }
  return QStringLiteral("none");
}

QString UpdatesController::bannerActionLabel() const {
  const QString a = bannerAction();
  if (a == QLatin1String("install")) return tr("Install and restart");
  if (a == QLatin1String("restart")) return tr("Restart to update");
  if (a == QLatin1String("link")) return tr("View release");
  return {};
}

QString UpdatesController::bannerText() const {
  const auto s = manager_.state();
  const QString v = manager_.availableVersion();
  if (v.isEmpty() || v == dismissedVersion_ || manager_.selectionSkipped()) return {};
  switch (s) {
    case UpdateManager::State::Available: return tr("Uzume %1 is available").arg(v);
    case UpdateManager::State::Ready: return tr("Uzume %1 is ready to install").arg(v);
    case UpdateManager::State::Downloading: return tr("Downloading Uzume %1…").arg(v);
    case UpdateManager::State::Incompatible: return tr("Uzume %1 needs a newer server").arg(v);
    default: break;
  }
  return {};
}

namespace {

// "today" or "2 Oct": the day part of an ISO timestamp as the list shows it.
QString dayText(const QString& iso) {
  const QDateTime t = QDateTime::fromString(iso, Qt::ISODate).toLocalTime();
  if (!t.isValid()) return {};
  if (t.date() == QDate::currentDate()) return QObject::tr("today");
  return QLocale().toString(t.date(), QStringLiteral("d MMM"));
}

}  // namespace

QVariantList UpdatesController::earlierVersions() const {
  QVariantList out;
  const QList<update::RollbackCandidate> options = manager_.rollbackOptions();
  if (!manager_.rollbackListLoaded()) return out;
  if (manager_.selectionSkipped()) {
    out.append(QVariantMap{{"version", manager_.availableVersion()}, {"kind", "skipped"},
                           {"text", tr("skipped · available")}, {"notesUrl", manager_.notesUrl()}});
  }
  const QList<update::HistoryEntry> history = update::parseHistory(settings_->updateHistory());
  QString runText;
  for (const update::HistoryEntry& h : history) {
    if (h.version == manager_.currentVersion() && !h.first.isEmpty()) runText = tr("installed %1").arg(dayText(h.first));
  }
  out.append(QVariantMap{{"version", manager_.currentVersion()}, {"kind", "running"}, {"text", runText}, {"notesUrl", QString()}});
  for (const update::RollbackCandidate& c : options) {
    QString text = tr("not used on this device");
    if (c.ran) {
      const QString from = dayText(c.first);
      const QString to = dayText(c.last);
      text = from == to || to.isEmpty() ? tr("used %1").arg(from) : tr("used %1 – %2").arg(from, to);
    }
    out.append(QVariantMap{{"version", c.release.version}, {"kind", "earlier"}, {"text", text}, {"notesUrl", c.release.notesUrl}});
  }
  return out;
}

QString UpdatesController::goBackUnavailableText() const {
  if (manager_.applySupported()) return {};
  return tr("This copy cannot install an older version itself. Open the release and install it from there.");
}

QString UpdatesController::pausedTitle() const {
  return tr("Running %1 · automatic updates paused until you resume them").arg(manager_.currentVersion());
}

QString UpdatesController::pausedDetail() const {
  const update::Pause p = manager_.pause();
  const QDateTime at = QDateTime::fromString(p.at, Qt::ISODate).toLocalTime();
  const QString when = at.isValid() ? TimeFormat::when(at) : QString();
  return when.isEmpty() ? tr("You went back from %1. %1 stays available.").arg(p.from)
                        : tr("You went back from %1 %2. %1 stays available.").arg(p.from, when);
}

QVariantMap UpdatesController::rollbackInfo(const QString& version) const {
  QVariantMap m;
  m.insert(QStringLiteral("version"), version);
  m.insert(QStringLiteral("canGoBack"), manager_.applySupported());
  const bool busy = manager_.busy();
  m.insert(QStringLiteral("busy"), busy);
  m.insert(QStringLiteral("busyText"), busy ? tr("A game or Session is running. Quit it first; it saves and syncs.") : QString());
  QStringList lines;
  if (pluginNeeds_) {
    for (const auto& n : pluginNeeds_(version)) lines << tr("%1 needs %2 and is turned off until you update.").arg(n.first, n.second);
  }
  m.insert(QStringLiteral("pluginLines"), lines);
  return m;
}

void UpdatesController::openRelease(const QString& url) {
  const QUrl u(url);
  if (u.isValid() && u.scheme() == QLatin1String("https")) QDesktopServices::openUrl(u);
}

void UpdatesController::setChannel(const QString& channel) {
  settings_->setUpdateChannel(channel == QLatin1String("default") ? QString() : channel);
  dismissedVersion_.clear();
  manager_.settingsChanged();
}

void UpdatesController::setAutoInstall(bool on) {
  settings_->setUpdateAutoInstall(on);
  manager_.settingsChanged();
}

void UpdatesController::openNotes() {
  const QUrl u(manager_.notesUrl());
  if (u.isValid() && u.scheme() == QLatin1String("https")) QDesktopServices::openUrl(u);
}

void UpdatesController::openWhatsNewNotes() {
  const QUrl u(manager_.installedNotesUrl());
  if (u.isValid() && u.scheme() == QLatin1String("https")) QDesktopServices::openUrl(u);
}

void UpdatesController::dismissBanner() {
  dismissedVersion_ = manager_.availableVersion();
  emit changed();
}

void UpdatesController::applyWhatsNew(const QString& notes) {
  const QString cur = manager_.currentVersion();
  if (cur.isEmpty()) return;
  const QString last = settings_->whatsNewVersion();
  if (last == cur) return;
  const std::optional<int> order = last.isEmpty() ? std::nullopt : update::compareVersions(last, cur);
  if (last.isEmpty() || (order.has_value() && *order > 0) || notes.trimmed().isEmpty()) {
    settings_->setWhatsNewVersion(cur);  // fresh install, went back, or nothing to say: nothing to show for this version
    return;
  }
  whatsNewNotes_ = notes;
  whatsNewPending_ = true;
  emit whatsNewChanged();
}

void UpdatesController::dismissWhatsNew() {
  settings_->setWhatsNewVersion(manager_.currentVersion());
  if (!whatsNewPending_) return;
  whatsNewPending_ = false;
  emit whatsNewChanged();
}

void UpdatesController::rememberWhatsNew(const QString& version) { settings_->setWhatsNewVersion(version); }

}  // namespace uzume::ui
