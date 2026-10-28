#include "runscontroller.h"
#include "timeformat.h"
#include "librarymodel.h"
#include "runrules.h"
#include "runpluginupdate.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QLoggingCategory>
#include <QMap>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <memory>

#include "manifest.h"
#include "run_session_bridge.h"
#include "semver.h"

namespace uzume::ui {

Q_LOGGING_CATEGORY(lcRunsUi, "uzume.runs.ui")

namespace {
QString statusText(const QString& s) {
  if (s == QLatin1String("starting")) return QObject::tr("Starting");
  if (s == QLatin1String("active")) return QObject::tr("Active");
  if (s == QLatin1String("paused")) return QObject::tr("❚❚ Paused");
  if (s == QLatin1String("finished")) return QObject::tr("Finished");
  if (s == QLatin1String("abandoned")) return QObject::tr("Abandoned");
  return s;
}
QString statusTone(const QString& s) {
  if (s == QLatin1String("active") || s == QLatin1String("starting")) return QStringLiteral("info");  // cyan, no warning glyph (P25)
  if (s == QLatin1String("finished")) return QStringLiteral("ok");
  return QStringLiteral("neutral");
}
}  // namespace

RunsController::RunsController(RunClient* client, PluginsController* plugins, HubLibrary* library, HubConnection* connection, QObject* parent)
    : QObject(parent), client_(client), plugins_(plugins), library_(library), conn_(connection) {
  recap_ = new RunRecapController(client, plugins, connection, this);
  // A game that runs when its run's plugins change restarts them only when the person agreed to the new permissions.
  // Decided from the pins alone against what this person accepted (never from run events or the run cache), so an unknown run, an unknown
  // dependency or a missing record all fail closed: the plugins stay off until the sheet is agreed to.
  plugins->setRunPinsGate([this](const QString& runId, const QList<plugins::RunPluginPin>& pins, bool raise) {
    QList<RunPluginRef> refs;
    for (const plugins::RunPluginPin& p : pins) refs.append({p.id, p.version, p.sha256, {}});
    const QVariantList need = consentNeededFor(runId, refs);
    if (need.isEmpty()) return true;
    if (raise && pluginConsent_.isEmpty()) {
      pluginConsent_ = {{QStringLiteral("runId"), runId.toLower()}, {QStringLiteral("items"), need}};
      emit pluginConsentChanged();
    }
    return false;
  });
  // The open consent sheet follows the catalog: permissions that were unknown when it opened show up in it before anyone agrees.
  connect(plugins, &PluginsController::hubPluginsChanged, this, [this] {
    const QString runId = pluginConsent_.value(QStringLiteral("runId")).toString();
    const auto run = runId.isEmpty() ? std::nullopt : client_->run(runId.toLower());
    if (!run) return;
    QList<RunPluginRef> refs;
    for (const QVariant& v : pluginConsent_.value(QStringLiteral("items")).toList())
      for (const RunPluginRef& p : run->plugins)
        if (p.pluginId == v.toMap().value(QStringLiteral("pluginId")).toString() && p.version == v.toMap().value(QStringLiteral("version")).toString()) refs.append(p);
    const QVariantList items = consentNeededFor(run->runId, refs);
    if (items == pluginConsent_.value(QStringLiteral("items")).toList()) return;
    pluginConsent_ = items.isEmpty() ? QVariantMap() : QVariantMap{{QStringLiteral("runId"), run->runId}, {QStringLiteral("items"), items}};
    emit pluginConsentChanged();
  });
  connect(client, &RunClient::runsChanged, this, &RunsController::scheduleRebuild);
  connect(client, &RunClient::runChanged, this, &RunsController::scheduleRebuild);
  connect(client, &RunClient::eventAdded, this, [this](const QString& id, const RunEvent& e) {
    if (id != openRunId_) return;
    rebuildEvents();
    if (e.type == QLatin1String("rules.changed")) scheduleRebuild();  // "set by" in the run header
  });
  connect(client, &RunClient::writeFailed, this, [this](const QString&, const QString& kind, const QString&, const QString& msg) {
    setMessage(tr("A run %1 could not be sent: %2").arg(kind == QLatin1String("event") ? tr("event") : kind, msg), true);
  });
  connect(connection, &HubConnection::stateChanged, this, &RunsController::scheduleRebuild);
  connect(plugins, &PluginsController::pluginsChanged, this, &RunsController::scheduleRebuild);
  connect(plugins, &PluginsController::hubPluginsChanged, this, &RunsController::scheduleRebuild);
  connect(plugins, &PluginsController::runSectionsChanged, this, &RunsController::rebuildRunTabs);
  connect(library, &HubLibrary::loaded, this, &RunsController::scheduleRebuild);
  connect(plugins, &PluginsController::inGameChanged, this, [this] { syncRunView(); });  // a game starts / ends: stop / restart the run view
  // The game picker follows late arrivals: the library or the Hub's plugin list may load after the dialog opened.
  connect(library, &HubLibrary::loaded, this, &RunsController::refreshGames);
  connect(plugins, &PluginsController::hubPluginsChanged, this, &RunsController::refreshGames);
  rebuild();
}

bool RunsController::available() const {
  // Runs are for everyone on the server and need no plugin: the entry follows the Hub feature alone (ADR 0030 D7).
  return client_ && client_->supported();
}

QString RunsController::saveSlotFor(const QString& runId) const {
  if (const auto run = client_->run(runId.toLower())) return run->saveSlot.isEmpty() ? runSaveSlot(run->runId) : run->saveSlot;
  return runSaveSlot(runId);
}

QString RunsController::slotDisplayName(const QString& slot) const {
  if (slot.isEmpty() || slot == QLatin1String("default")) return {};
  for (const RunInfo& run : client_->runs()) {
    const QString runSlot = run.saveSlot.isEmpty() ? runSaveSlot(run.runId) : run.saveSlot;
    if (runSlot == slot) return tr("Run: %1").arg(run.name);
  }
  return {};
}

QString RunsController::runIdForSlot(const QString& slot) const {
  if (slot.isEmpty() || slot == QLatin1String("default")) return {};
  for (const RunInfo& run : client_->runs()) {
    const QString runSlot = run.saveSlot.isEmpty() ? runSaveSlot(run.runId) : run.saveSlot;
    if (runSlot == slot) return run.runId;
  }
  return {};
}

QString RunsController::unverifiedWarning() const {
  return tr("Uzume has not checked this plugin. Turn it on only if you trust the admin of your server and where the file came from.");
}

bool RunsController::startUnverified() const {
  for (const QVariantMap& row : startPlugins_.rows())
    if (row.value(QStringLiteral("checked")).toBool() && row.value(QStringLiteral("origin")).toString() == QLatin1String("unverified")) return true;
  return false;
}

void RunsController::refresh() {
  if (client_->supported()) {
    client_->refresh();
    plugins_->refreshHubPlugins();  // a plugin removed from the Hub hides its run tab
  }
}

void RunsController::scheduleRebuild() {
  if (rebuildQueued_) return;
  rebuildQueued_ = true;
  QTimer::singleShot(0, this, [this]() {
    rebuildQueued_ = false;
    rebuild();
  });
}

// ------------------------------------------------------------------ names and texts

QString RunsController::pluginName(const QString& pluginId) const {
  if (const PluginHubClient* hub = plugins_->hubClient()) {
    for (const HubPluginInfo& p : hub->plugins())
      if (p.id == pluginId) return p.name;
  }
  if (const auto info = plugins_->manager()->info(pluginId)) return info->manifest.name;
  for (const plugins::PluginInfo& p : plugins_->manager()->hubCopies())
    if (p.manifest.id == pluginId) return p.manifest.name;
  return pluginId;
}

QString RunsController::pluginsText(const RunInfo& run) const {
  QStringList names;
  for (const RunPluginRef& p : run.plugins) names << pluginName(p.pluginId) + QLatin1Char(' ') + p.version;
  return names.join(QStringLiteral(" & "));
}

QString RunsController::gameTitle(const QString& gameId) const {
  if (gameId.isEmpty()) return {};
  for (const GameEntry& g : library_->games())
    if (g.id == gameId) return g.title;
  return tr("Game not in your library");
}

QString RunsController::userName(const RunInfo& run, const QString& userId) const {
  if (const RunParticipant* p = run.participant(userId)) return p->userName;
  return userId;
}

QString RunsController::timeText(const QString& iso) const {
  return TimeFormat::whenIso(iso);
}

QString RunsController::errorText(const RunResult& r, const QString& what) const {
  if (r.networkError) return tr("%1: the server is not reachable.").arg(what);
  if (r.status == 403) return tr("%1: you are not allowed to do that.").arg(what);
  if (r.status == 404) return tr("%1: this run does not exist (any more).").arg(what);
  if (r.errorCode == QLatin1String("own_variants_off")) return tr("%1: this run builds everyone's variant. Own variants are off.").arg(what);
  if (r.errorCode == QLatin1String("own_variant_mismatch")) return tr("%1: that variant doesn't use the run's engine version.").arg(what);
  if (r.status == 409) return tr("%1: the run changed in the meantime. Its current state is shown.").arg(what);
  if (r.status == 429) return tr("%1: too many requests, try again in a moment.").arg(what);
  return r.message.isEmpty() ? tr("%1 failed.").arg(what) : tr("%1: %2").arg(what, r.message);
}

namespace {
QString capitalizedFirst(QString s) {
  s = s.trimmed();
  if (!s.isEmpty()) s[0] = s.at(0).toUpper();
  return s;
}

QString invitedText(const QString& who, int n) {
  // No translator is installed in the Player, so a "%n player(s)" plural would render literally: branch instead.
  return n == 1 ? QCoreApplication::translate("RunsController", "%1 invited 1 player").arg(who)
                : QCoreApplication::translate("RunsController", "%1 invited %2 players").arg(who).arg(n);
}

// Events the Player does not know (or whose payload is incomplete): readable text, never the raw lowercase type.
QString fallbackEventText(const RunEvent& e) {
  if (!e.text.trimmed().isEmpty()) return capitalizedFirst(e.text);
  QString t = e.type;
  const qsizetype dot = t.lastIndexOf(QLatin1Char('.'));
  if (dot >= 0) t = t.mid(dot + 1);
  t.replace(QLatin1Char('_'), QLatin1Char(' '));
  return t.trimmed().isEmpty() ? QCoreApplication::translate("RunsController", "Run event") : capitalizedFirst(t);
}
}  // namespace

QString RunsController::hubEventText(const RunEvent& e) const {
  // The actor is always named, like the Hub's run board ("Lena joined"), also for events without a user_name in the payload.
  QString who = e.payload.value(QStringLiteral("user_name")).toString();
  if (who.isEmpty()) who = e.userName;
  if (who.isEmpty()) who = tr("Someone");
  if (e.type == QLatin1String("run.created")) {
    const QString name = e.payload.value(QStringLiteral("name")).toString().trimmed();
    return name.isEmpty() ? tr("%1 created the run").arg(who) : tr("%1 created the run %2").arg(who, QString(QChar(0x201C)) + name + QChar(0x201D));
  }
  if (e.type == QLatin1String("run.joined")) return tr("%1 joined").arg(who);
  if (e.type == QLatin1String("run.declined")) return tr("%1 declined the invite").arg(who);
  if (e.type == QLatin1String("run.left")) return tr("%1 left the run").arg(who);
  if (e.type == QLatin1String("run.invited")) return invitedText(who, static_cast<int>(e.payload.value(QStringLiteral("invited")).toArray().size()));
  if (e.type == QLatin1String("rules.changed")) {
    QString by = e.payload.value(QStringLiteral("by_user_name")).toString();
    if (by.isEmpty()) by = who;
    return runrules::changeEventText(by, e.payload);
  }
  if (e.type == QLatin1String("plugins.updated")) {
    QString by = e.payload.value(QStringLiteral("by_user_name")).toString();
    if (by.isEmpty()) by = who;
    return runplugins::eventText(by, e.payload, [this](const QString& id) { return pluginName(id); });
  }
  if (e.type == QLatin1String("run.status")) {
    const QString to = e.payload.value(QStringLiteral("status")).toString();
    QString by = e.payload.value(QStringLiteral("by_user_name")).toString();
    if (by.isEmpty()) by = who;
    if (to == QLatin1String("paused")) return tr("Run paused by %1").arg(by);
    if (to == QLatin1String("active")) return e.payload.value(QStringLiteral("previous")).toString() == QLatin1String("paused") ? tr("Run resumed by %1").arg(by) : tr("Run started by %1").arg(by);
    if (to == QLatin1String("finished")) return tr("Run finished by %1").arg(by);
    if (to == QLatin1String("abandoned")) return tr("Run abandoned by %1").arg(by);
  }
  return fallbackEventText(e);
}

// ------------------------------------------------------------------ rows

QVariantMap RunsController::rowFor(const RunInfo& run) const {
  const QString me = client_->myUserId();
  const RunParticipant* mine = run.participant(me);
  QVariantList parts;
  int joined = 0;
  for (const RunParticipant& p : run.participants) {
    if (p.status != QLatin1String("joined")) continue;
    ++joined;
    parts.append(QVariantMap{{QStringLiteral("name"), p.userName}, {QStringLiteral("color"), p.color}});
  }
  QString sub = run.plugins.isEmpty() ? tr("No rules") : pluginsText(run);
  if (mine != nullptr && !mine->gameId.isEmpty()) sub += QStringLiteral(" · ") + gameTitle(mine->gameId);
  QStringList ruleNames;
  for (const RunPluginRef& p : run.plugins) ruleNames << pluginName(p.pluginId);
  const QString ruleVersion = run.plugins.size() == 1 ? run.plugins.first().version : QString();
  const QString game = mine != nullptr && !mine->gameId.isEmpty() ? gameTitle(mine->gameId) : QString();
  QString status = run.status;
  QString text = statusText(run.status);
  QString tone = statusTone(run.status);
  if (mine != nullptr && mine->status == QLatin1String("left")) {
    text = tr("Left");
    tone = QStringLiteral("neutral");
  }
  const bool canOpen = !run.isOver() && mine != nullptr && mine->status == QLatin1String("joined");
  return {{QStringLiteral("runId"), run.runId},
          {QStringLiteral("name"), run.name},
          {QStringLiteral("status"), status},
          {QStringLiteral("statusText"), text},
          {QStringLiteral("tone"), tone},
          {QStringLiteral("subText"), sub},
          {QStringLiteral("timeText"), timeText(run.updatedAt)},
          {QStringLiteral("action"), canOpen ? tr("Open →") : tr("View")},
          {QStringLiteral("participants"), parts},
          {QStringLiteral("game"), game},
          {QStringLiteral("rulesName"), ruleNames.join(QStringLiteral(" & "))},
          {QStringLiteral("rulesVersion"), ruleVersion},
          {QStringLiteral("playTimeText"), LibraryModel::formatPlayTime(run.playSeconds)},
          {QStringLiteral("randomized"), run.randomizerMode() != QLatin1String("off")}};
}

void RunsController::rebuild() {
  const bool avail = available();
  const QString me = client_->myUserId();
  QList<QVariantMap> invites, active, finished;
  int activeN = 0, pausedN = 0;
  for (const RunInfo& run : client_->runs()) {
    const RunParticipant* mine = run.participant(me);
    if (mine == nullptr || mine->status == QLatin1String("declined")) continue;
    if (mine->status == QLatin1String("invited")) {
      if (run.isOver()) continue;
      int joined = 0;
      for (const RunParticipant& p : run.participants)
        if (p.status == QLatin1String("joined")) ++joined;
      invites.append({{QStringLiteral("runId"), run.runId},
                      {QStringLiteral("headline"), tr("%1 invites you to \u201C%2\u201D").arg(userName(run, run.createdBy), run.name)},
                      {QStringLiteral("subText"), run.plugins.isEmpty() ? tr("No rules") : pluginsText(run)},
                      {QStringLiteral("name"), run.name.isEmpty() ? tr("Untitled run") : run.name},
                      {QStringLiteral("inviteLine"), tr("%1 invited you · %2 · %3 of %4 joined")
                                                         .arg(userName(run, run.createdBy), timeText(run.createdAt)).arg(joined).arg(run.participants.size())},
                      {QStringLiteral("joinedText"), tr("%1 of %2 joined").arg(joined).arg(run.participants.size())}});
      continue;
    }
    if (run.isOver() || mine->status == QLatin1String("left")) {
      finished.append(rowFor(run));
    } else {
      active.append(rowFor(run));
      if (run.status == QLatin1String("paused")) ++pausedN; else ++activeN;
    }
  }
  invites_.setRows(invites);
  active_.setRows(active);
  finished_.setRows(finished);
  QString title;
  if (!active.isEmpty()) {
    title = tr("ACTIVE · %1").arg(activeN);
    if (pausedN > 0) title += tr(" · PAUSED · %1").arg(pausedN);
  }
  activeTitle_ = title;
  rebuildDetail();
  rebuildStartPlugins();
  if (!users_.isEmpty()) rebuildStartUsers();
  if (avail != lastAvailable_) {
    lastAvailable_ = avail;
    emit availableChanged();
  }
  emit runsChanged();
}

void RunsController::rebuildDetail() {
  QVariantMap d;
  QList<QVariantMap> parts;
  const auto run = openRunId_.isEmpty() ? std::nullopt : client_->run(openRunId_);
  if (run) {
    const QString me = client_->myUserId();
    const RunParticipant* mine = run->participant(me);
    const bool joined = mine != nullptr && mine->status == QLatin1String("joined");
    const bool creator = run->createdBy == me;
    const bool over = run->isOver();
    QString line = tr("Created by %1 · %2").arg(userName(*run, run->createdBy), timeText(run->createdAt));
    if (run->status == QLatin1String("paused"))
      line = tr("Paused by %1 · %2 · events still visible, no new ones recorded").arg(userName(*run, run->statusBy), timeText(run->updatedAt));
    else if (over)
      line = tr("%1 by %2 · %3").arg(statusText(run->status), userName(*run, run->statusBy), timeText(run->updatedAt));
    QString note;
    if (!joined && mine != nullptr && mine->status == QLatin1String("invited")) note = tr("You are invited. Join from the Runs list.");
    // run_plugin_updates_v1: creator or admin, run not over; what is offered comes from the server's plugin list.
    const bool canUpdatePlugins = client_->supportsPluginUpdates() && !over && plugins_->hubClient() != nullptr && (creator || conn_->userRole() == QLatin1String("admin"));
    QVariantList pluginChanges;
    if (canUpdatePlugins) {
      for (const runplugins::Change& ch : runplugins::changes(*run, plugins_->hubClient()->plugins())) {
        const QStringList added = additionsFor(*run, ch.request);  // against what the person accepted, restores and bundled plugins included
        QString text = ch.restore ? tr("Restore %1 %2").arg(ch.name, ch.to) : tr("Update available: %1 %2 → %3").arg(ch.name, ch.from, ch.to);
        if (!ch.bundledNames.isEmpty()) text += QStringLiteral(" · ") + tr("with %1").arg(ch.bundledNames.join(QStringLiteral(", ")));
        pluginChanges.append(QVariantMap{{QStringLiteral("pluginId"), ch.pluginId},
                                         {QStringLiteral("restore"), ch.restore},
                                         {QStringLiteral("text"), text},
                                         {QStringLiteral("buttonText"), ch.restore ? tr("Restore %1").arg(ch.to) : tr("Update")},
                                         {QStringLiteral("permissionsText"), added.isEmpty() ? QString() : tr("New permissions: %1").arg(PluginsController::permissionLabels(added))}});
      }
    }
    d = {{QStringLiteral("runId"), run->runId},
         {QStringLiteral("name"), run->name},
         {QStringLiteral("status"), run->status},
         {QStringLiteral("statusText"), statusText(run->status)},
         {QStringLiteral("tone"), statusTone(run->status)},
         {QStringLiteral("line"), line},
         {QStringLiteral("pluginsText"), run->plugins.isEmpty() ? tr("No rules") : pluginsText(*run)},
         {QStringLiteral("plain"), run->plugins.isEmpty()},
         {QStringLiteral("saveSlotText"), tr("Run: %1").arg(run->name)},
         {QStringLiteral("canPlay"), joined && !over},
         {QStringLiteral("playLabel"), mine != nullptr && !mine->gameId.isEmpty() ? tr("Play %1").arg(gameTitle(mine->gameId)) : tr("Play run")},
         {QStringLiteral("canStart"), joined && creator && run->status == QLatin1String("starting")},
         {QStringLiteral("canPause"), joined && run->status == QLatin1String("active")},
         {QStringLiteral("canResume"), joined && run->status == QLatin1String("paused")},
         {QStringLiteral("canFinish"), joined && creator && !over},
         {QStringLiteral("canAbandon"), joined && creator && !over},
         {QStringLiteral("canLeave"), joined && !creator && !over},
         {QStringLiteral("canInvite"), joined && !over && run->participants.size() < kRunMaxParticipants},
         {QStringLiteral("over"), over},
         {QStringLiteral("rules"), runrules::toMap(run->rules)},
         {QStringLiteral("rulesLine"), runrules::summaryLine(run->rules)},
         {QStringLiteral("rulesSetBy"), rulesSetBy(*run)},
         {QStringLiteral("rulesAvailable"), client_->supportsRules()},
         {QStringLiteral("canEditRules"), client_->supportsRules() && !over && (creator || conn_->userRole() == QLatin1String("admin"))},
         {QStringLiteral("canUpdatePlugins"), canUpdatePlugins},
         {QStringLiteral("pluginChanges"), pluginChanges},
         {QStringLiteral("note"), note},
         {QStringLiteral("randomized"), run->randomizerMode() != QLatin1String("off")},
         {QStringLiteral("randomizer"), randomizerOf(run->runId)}};
    const bool rnd = run->randomizerMode() != QLatin1String("off");
    for (const RunParticipant& p : run->participants) {
      QString st;
      if (p.status == QLatin1String("joined")) st = (rnd && p.variantState == QLatin1String("building")) ? tr("Building") : tr("Joined");
      else if (p.status == QLatin1String("invited")) st = tr("Invited");
      else if (p.status == QLatin1String("declined")) st = tr("Declined");
      else st = tr("Left");
      parts.append({{QStringLiteral("userId"), p.userId},
                    {QStringLiteral("name"), p.userName},
                    {QStringLiteral("color"), p.color},
                    // A randomized run's board shows only which kind of variant a person plays, never the world (10f-4).
                    {QStringLiteral("gameTitle"), rnd ? (p.status == QLatin1String("joined") && p.variantKind == QLatin1String("own") ? tr("Own variant")
                                                         : p.status == QLatin1String("joined") && p.variantKind == QLatin1String("run") ? tr("Run variant") : QStringLiteral("—"))
                                                      : (p.gameId.isEmpty() ? QStringLiteral("—") : gameTitle(p.gameId))},
                    {QStringLiteral("variantState"), p.variantState},
                    {QStringLiteral("statusText"), st},
                    {QStringLiteral("slotText"), p.status == QLatin1String("joined") ? tr("Run: %1").arg(run->name) : QStringLiteral("—")},
                    {QStringLiteral("me"), p.userId == me},
                    {QStringLiteral("playTimeText"), LibraryModel::formatPlayTime(p.playSeconds)},
                    {QStringLiteral("lastPlayedText"), LibraryModel::formatLastPlayed(
                         QDateTime::fromString(p.lastPlayedAt, Qt::ISODate).isValid() ? QDateTime::fromString(p.lastPlayedAt, Qt::ISODate).toMSecsSinceEpoch() : 0)}});
    }
  }
  participants_.setRows(parts);
  if (d != detail_) {
    detail_ = d;
    emit detailChanged();
  }
  rebuildEvents();
  syncRunView();
  rebuildRunTabs();
}

// Run view: while a run is shown in detail, the user takes part and no game runs, the plugins of the run that declare ui.run or ui.screen run
// in the shell context (read-only uz.run). The exact pinned copies are fetched first when they are missing (joining was the consent).
void RunsController::setRunViewState(const QString& state, const QString& text) {
  if (runViewState_ == state && runViewText_ == text) return;
  runViewState_ = state;
  runViewText_ = text;
  emit runViewChanged();
}

void RunsController::syncRunView() {
  const auto run = openRunId_.isEmpty() ? std::nullopt : client_->run(openRunId_);
  const QString me = client_->myUserId();
  const RunParticipant* mine = run ? run->participant(me) : nullptr;
  const bool takesPart = run && (run->createdBy == me || (mine != nullptr && mine->status == QLatin1String("joined")));
  if (!takesPart || run->plugins.isEmpty() || plugins_->inGame() || plugins_->delivery() == nullptr) {
    ++viewGen_;
    viewKey_.clear();
    viewFetching_ = false;
    plugins_->stopRunView();
    setRunViewState({}, {});
    return;
  }
  if (!consentNeeded(*run).isEmpty()) {  // updated plugins ask for new permissions: they start after "Play run" collected the agreement
    ++viewGen_;
    viewKey_.clear();
    viewFetching_ = false;
    plugins_->stopRunView();
    setRunViewState(QStringLiteral("error"), tr("The plugins of this run were updated and ask for new permissions. Press Play run to review them."));
    return;
  }
  QStringList sig;
  for (const RunPluginRef& p : run->plugins) sig << p.pluginId + QLatin1Char('@') + p.version + QLatin1Char('@') + p.sha256;
  const QString key = run->runId + QLatin1Char('|') + sig.join(QLatin1Char(','));
  if (key == viewKey_ && (viewFetching_ || plugins_->runViewId() == run->runId.toLower())) return;  // same run again: nothing to restart
  viewKey_ = key;
  const quint64 gen = ++viewGen_;
  bool missing = false;
  for (const RunPluginRef& p : run->plugins)
    if (plugins_->manager()->hubCopyDir(p.pluginId, p.version, p.sha256).isEmpty()) missing = true;
  const auto start = [this, runId = run->runId, gen]() {
    if (gen != viewGen_) return;
    viewFetching_ = false;
    plugins_->startRunView(launchFor(runId), runId);
    setRunViewState({}, {});
  };
  if (!missing) {
    start();
    return;
  }
  viewFetching_ = true;
  setRunViewState(QStringLiteral("loading"), tr("Getting the plugins of this run from the server…"));
  ensureCopies(*run, [this, gen, start](bool ok, const QString& error) {
    setBusy(false);  // also for a superseded fetch: ensureCopies raised the busy flag and nobody else would lower it
    if (gen != viewGen_) return;
    if (!ok) {
      viewFetching_ = false;
      setRunViewState(QStringLiteral("error"), error);
      return;
    }
    start();
  });
}

void RunsController::rebuildEvents() {
  QList<QVariantMap> rows;
  const auto run = openRunId_.isEmpty() ? std::nullopt : client_->run(openRunId_);
  if (run) {
    const QList<RunEvent> all = client_->events(openRunId_, 0, 500);
    for (auto it = all.crbegin(); it != all.crend() && rows.size() < 300; ++it) {  // newest first
      const RunEvent& e = *it;
      QString author;
      QString source;
      QString text = e.text;
      if (e.source == QLatin1String("hub")) {
        author = tr("server");
        source = tr("server");
        text = hubEventText(e);
      } else {
        author = e.userName.isEmpty() ? tr("Someone") : e.userName;
        source = (e.source == QLatin1String("automatic") ? tr("automatic · %1's app") : tr("manual · %1")).arg(author);
        if (text.isEmpty()) text = fallbackEventText(e);
      }
      rows.append({{QStringLiteral("seq"), e.seq},
                   {QStringLiteral("timeText"), timeText(e.createdAt)},
                   {QStringLiteral("fullTimeText"), TimeFormat::fullIso(e.createdAt)},
                   {QStringLiteral("author"), author},
                   {QStringLiteral("text"), text},
                   {QStringLiteral("sourceText"), source}});
    }
  }
  events_.setRows(rows);
}

// ------------------------------------------------------------------ messages and busy state

void RunsController::setMessage(const QString& text, bool error) {
  if (message_ == text && messageIsError_ == error) return;
  message_ = text;
  messageIsError_ = error;
  emit messageChanged();
}

void RunsController::dismissMessage() { setMessage({}, false); }

void RunsController::setBusy(bool busy, const QString& text) {
  if (busy_ == busy && busyText_ == text) return;
  busy_ = busy;
  busyText_ = text;
  emit busyChanged();
  scheduleRebuild();  // the entry follows busy_ (available())
}

// ------------------------------------------------------------------ opening a run

void RunsController::openRun(const QString& runId) {
  if (openRunId_ != runId.toLower()) {
    setRunTab(QStringLiteral("board"));
    recap_->clear();
    if (!pluginUpdateError_.isEmpty()) {
      pluginUpdateError_.clear();
      emit pluginUpdateChanged();
    }
  }
  openRunId_ = runId.toLower();
  client_->catchUp(openRunId_);
  rebuildDetail();
}

void RunsController::setRunTab(const QString& tab) {
  if (runTab_ == tab) return;
  runTab_ = tab;
  if (tab == QLatin1String("recap") && !openRunId_.isEmpty()) recap_->load(openRunId_);
  emit runTabChanged();
}

bool RunsController::selectPluginTab(const QString& pluginId) {
  if (openRunId_.isEmpty()) return false;
  for (const QVariant& t : std::as_const(runTabs_)) {
    if (t.toMap().value(QStringLiteral("pluginId")).toString() != pluginId) continue;
    setRunTab(QStringLiteral("plugin:") + pluginId);
    return true;
  }
  return false;
}

void RunsController::retryRunView() {
  viewKey_.clear();
  viewFetching_ = false;
  syncRunView();
}

void RunsController::rebuildRunTabs() {
  QVariantList tabs;
  const auto run = openRunId_.isEmpty() ? std::nullopt : client_->run(openRunId_);
  if (run) {
    // A plugin the Hub does not offer any more has no tab (the board and the log keep working); an unknown list shows all.
    QSet<QString> offered;
    const bool known = plugins_->hubClient() != nullptr && plugins_->hubStatus() == QLatin1String("ready");
    if (known)
      for (const HubPluginInfo& p : plugins_->hubClient()->plugins()) offered.insert(p.id);
    const QVariantList sections = plugins_->runSections();
    for (const RunPluginRef& ref : run->plugins) {
      if (known && !offered.contains(ref.pluginId)) continue;
      for (const QVariant& v : sections) {
        const QVariantMap sec = v.toMap();
        if (sec.value(QStringLiteral("pluginId")).toString() != ref.pluginId) continue;
        tabs.append(QVariantMap{{QStringLiteral("pluginId"), ref.pluginId},
                                {QStringLiteral("title"), sec.value(QStringLiteral("title"))},
                                {QStringLiteral("name"), sec.value(QStringLiteral("name"))},
                                {QStringLiteral("badge"), sec.value(QStringLiteral("badge"))}});
        break;
      }
    }
  }
  if (tabs != runTabs_) {
    runTabs_ = tabs;
    emit runViewChanged();
  }
  if (runTab_.startsWith(QLatin1String("plugin:"))) {
    const QString id = runTab_.mid(7);
    const bool present = std::any_of(runTabs_.begin(), runTabs_.end(), [&](const QVariant& t) { return t.toMap().value(QStringLiteral("pluginId")).toString() == id; });
    if (!present) setRunTab(QStringLiteral("board"));
  }
}

void RunsController::closeRun() {
  openRunId_.clear();
  recap_->clear();
  setRunTab(QStringLiteral("board"));
  rebuildDetail();
}

// ------------------------------------------------------------------ games

QList<QJsonArray> RunsController::gamePatternsFor(const RunInfo* run) const {
  QList<QJsonArray> out;
  if (run == nullptr || plugins_->hubClient() == nullptr) return out;
  for (const RunPluginRef& ref : run->plugins) {
    for (const HubPluginInfo& p : plugins_->hubClient()->plugins()) {
      if (p.id != ref.pluginId || p.version != ref.version) continue;
      if (!p.games.isEmpty()) out.append(p.games);
    }
  }
  return out;
}

QList<QJsonArray> RunsController::startGamePatterns() const {
  const StartResolution res = currentStartResolution();
  QList<QJsonArray> out;
  for (const HubPluginInfo& p : res.picked)
    if (!p.games.isEmpty()) out.append(p.games);
  for (const StartIncluded& i : res.included)
    if (!i.info.games.isEmpty()) out.append(i.info.games);
  return out;
}

namespace {
enum class Fit { No, Unknown, Yes };

// One plugin's `games` patterns against one library game: a known code must match one of the globs of a pattern of the
// game's system; an unknown code (empty) stays possible when only the system matches.
Fit fitOf(const QJsonArray& patterns, const GameEntry& g) {
  Fit best = Fit::No;
  for (const QJsonValue& v : patterns) {
    const QJsonObject o = v.toObject();
    if (o.value(QStringLiteral("system")).toString().compare(g.system, Qt::CaseInsensitive) != 0) continue;
    const QJsonArray codes = o.value(QStringLiteral("codes")).toArray();
    if (codes.isEmpty()) return Fit::Yes;
    if (g.gameCode.isEmpty()) {
      best = Fit::Unknown;
      continue;
    }
    for (const QJsonValue& c : codes)
      if (plugins::globMatch(c.toString(), g.gameCode)) return Fit::Yes;
  }
  return best;
}
}  // namespace

int RunsController::fillGames(const QList<QJsonArray>& patterns) {
  struct Row {
    QVariantMap map;
    bool unknown;
  };
  QList<Row> rows;
  for (const GameEntry& g : library_->games()) {
    bool unknown = false, fits = true;
    for (const QJsonArray& p : patterns) {
      const Fit f = fitOf(p, g);
      if (f == Fit::No) {
        fits = false;
        break;
      }
      if (f == Fit::Unknown) unknown = true;
    }
    if (!fits) continue;
    rows.append({{{QStringLiteral("id"), g.id},
                  {QStringLiteral("title"), g.title},
                  {QStringLiteral("system"), g.system},
                  {QStringLiteral("hint"), unknown ? tr("Game not recognized") : QString()},
                  {QStringLiteral("sha"), g.romSha256}},
                 unknown});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.unknown != b.unknown) return !a.unknown;
    return a.map.value(QStringLiteral("title")).toString().localeAwareCompare(b.map.value(QStringLiteral("title")).toString()) < 0;
  });
  QList<QVariantMap> maps;
  for (const Row& r : std::as_const(rows)) maps.append(r.map);
  games_.setRows(maps);
  return static_cast<int>(maps.size());
}

void RunsController::refreshGames() {
  if (gamesPrepared_) prepareGames(gamesFor_);
}

int RunsController::prepareGames(const QString& runId) {
  gamesPrepared_ = true;
  gamesFor_ = runId;
  const auto run = runId.isEmpty() ? std::nullopt : client_->run(runId.toLower());
  QList<QVariantMap> pluginRows;
  bool unverified = false;
  if (run) {
    for (const RunPluginRef& ref : run->plugins) {
      QVariantMap row = plugins_->hubOffer(ref.pluginId, ref.version);
      if (row.isEmpty()) {  // not listed by the Hub any more: shown as unchecked
        row = {{QStringLiteral("id"), ref.pluginId},
               {QStringLiteral("name"), pluginName(ref.pluginId)},
               {QStringLiteral("version"), ref.version},
               {QStringLiteral("origin"), QStringLiteral("unverified")},
               {QStringLiteral("originText"), tr("Server upload · unverified")},
               {QStringLiteral("permissionsText"), tr("Share run progress with your server")}};
      }
      if (row.value(QStringLiteral("origin")).toString() == QLatin1String("unverified")) unverified = true;
      pluginRows.append(row);
    }
  }
  joinPlugins_.setRows(pluginRows);
  if (run) {  // what the dialog shows is what joining accepts: exact version and permissions, nothing else
    QMap<QString, QPair<QString, QStringList>> reviewed;
    for (const RunPluginRef& ref : run->plugins) {
      QStringList perms;
      if (!plugins_->hubOffer(ref.pluginId, ref.version).isEmpty() && permissionsOf(ref.pluginId, ref.version, &perms)) reviewed.insert(ref.pluginId, {ref.version, perms});
    }
    joinReviewed_.insert(run->runId.toLower(), reviewed);
  }
  if (joinUnverified_ != unverified) joinUnverified_ = unverified;
  emit joinPluginsChanged();
  return fillGames(run ? gamePatternsFor(&*run) : startGamePatterns());
}

// ------------------------------------------------------------------ actions

QVariantMap RunsController::randomizerOf(const QString& runId) const {
  const auto run = client_->run(runId.toLower());
  if (!run || run->randomizerMode() == QLatin1String("off")) return {};
  const QJsonObject& o = run->randomizer;
  return {{QStringLiteral("mode"), run->randomizerMode()},
          {QStringLiteral("presetId"), o.value(QStringLiteral("preset_id")).toString()},
          {QStringLiteral("toolVersion"), o.value(QStringLiteral("tool_version")).toString()},
          {QStringLiteral("allowOwn"), o.value(QStringLiteral("allow_own")).toBool()},
          {QStringLiteral("baseSha256"), o.value(QStringLiteral("base_sha256")).toString()},
          {QStringLiteral("titleId"), o.value(QStringLiteral("title_id")).toString()},
          {QStringLiteral("creatorName"), userName(*run, run->createdBy)},
          {QStringLiteral("over"), run->isOver()}};
}

void RunsController::joinRun(const QString& runId, const QString& gameId) {
  joinRun(runId, gameId, QString(), QString());
}

void RunsController::joinRun(const QString& runId, const QString& gameId, const QString& variantKind, const QString& variantId) {
  if (gameId.isEmpty()) {
    setMessage(tr("Pick the game you play in this run."), true);
    return;
  }
  setBusy(true, tr("Joining…"));
  QJsonObject variant;
  if (!variantKind.isEmpty()) {
    variant.insert(QStringLiteral("kind"), variantKind);
    if (variantKind == QLatin1String("own") && !variantId.isEmpty()) variant.insert(QStringLiteral("variant_id"), variantId);
  }
  client_->joinRun(runId, gameId, [this, runId, variantKind, variantId](const RunResult& r) {
    setBusy(false);
    if (!r.ok) {
      setMessage(errorText(r, tr("Joining")), true);
      return;
    }
    setMessage({}, false);
    if (variantKind == QLatin1String("own") && !variantId.isEmpty()) emit ownVariantJoined(runId.toLower(), variantId);
    openRun(runId);
    if (r.run) {
      // Joining accepts only the pins (and permissions) the join dialog showed; a pin that changed since stays unaccepted for the gate to ask.
      const auto reviewed = joinReviewed_.value(r.run->runId.toLower());
      for (const RunPluginRef& p : r.run->plugins) {
        const auto it = reviewed.constFind(p.pluginId);
        if (it != reviewed.constEnd() && it->first == p.version) plugins_->acceptRunPermissions(r.run->runId, p.pluginId, it->second);
      }
      ensureCopies(*r.run, [this](bool, const QString&) { setBusy(false); });  // get the plugins ready in the background
    }
  }, variant);
}

void RunsController::declineRun(const QString& runId) {
  setBusy(true, tr("Declining…"));
  client_->declineRun(runId, [this](const RunResult& r) {
    setBusy(false);
    setMessage(r.ok ? QString() : errorText(r, tr("Declining")), !r.ok);
  });
}

void RunsController::leaveRun(const QString& runId) {
  setBusy(true, tr("Leaving…"));
  client_->leaveRun(runId, [this](const RunResult& r) {
    setBusy(false);
    setMessage(r.ok ? QString() : errorText(r, tr("Leaving")), !r.ok);
  });
}

void RunsController::setRunStatus(const QString& runId, const QString& status) {
  setBusy(true);
  client_->setStatus(runId, status, [this, status](const RunResult& r) {
    setBusy(false);
    setMessage(r.ok ? QString() : errorText(r, tr("Changing the run")), !r.ok);
  });
}

void RunsController::ensureCopies(const RunInfo& run, std::function<void(bool, const QString&)> done) {
  if (run.plugins.isEmpty()) {  // a run without plugins has nothing to fetch
    done(true, {});
    return;
  }
  if (plugins_->delivery() == nullptr) {
    done(false, tr("This server does not offer plugins."));
    return;
  }
  auto pending = std::make_shared<QList<RunPluginRef>>(run.plugins);
  auto step = std::make_shared<std::function<void()>>();
  *step = [this, pending, step, done = std::move(done)]() {
    if (pending->isEmpty()) {
      done(true, {});
      return;
    }
    const RunPluginRef ref = pending->takeFirst();
    setBusy(true, tr("Getting %1 %2 from the server…").arg(pluginName(ref.pluginId), ref.version));
    plugins_->delivery()->ensureHubCopy(ref.pluginId, ref.version, ref.sha256, [this, pending, step, done, ref](const plugins::DeliveryResult& res) {
      if (!res.ok) {
        setBusy(false);
        done(false, tr("%1 %2 could not be prepared: %3").arg(pluginName(ref.pluginId), ref.version, res.error));
        return;
      }
      (*step)();
    });
  };
  (*step)();
}

QVariantList RunsController::consentNeeded(const RunInfo& run) const { return consentNeededFor(run.runId, run.plugins); }

// Permissions of a plugin version: the server's list, else the manifest of the fetched Hub copy. false = unknown (stale catalog, no copy).
bool RunsController::permissionsOf(const QString& pluginId, const QString& version, QStringList* out) const {
  if (const PluginHubClient* hub = plugins_->hubClient()) {
    for (const HubPluginInfo& h : hub->plugins())
      if (h.id == pluginId && h.version == version) {
        *out = h.permissions;
        return true;
      }
  }
  for (const plugins::PluginInfo& c : plugins_->manager()->hubCopies())
    if (c.manifest.id == pluginId && c.manifest.version.toString() == version) {
      *out = c.manifest.permissions;
      return true;
    }
  return false;
}

// What the person accepted for this plugin in this run, and nothing else: the stored permissions, else the permissions of the versions a legacy
// consent file proves they accepted. No record means nothing accepted (fail closed): never inferred from the previous pin or the run events.
QStringList RunsController::acceptedBaseline(const QString& runId, const QString& pluginId) const {
  if (const auto stored = plugins_->runAcceptedPermissions(runId, pluginId)) return *stored;
  QStringList baseline;
  for (const QString& ver : plugins_->runLegacyVersions(runId, pluginId)) {
    QStringList lp;
    if (permissionsOf(pluginId, ver, &lp)) baseline += lp;
  }
  return baseline;
}

// The trigger for a stale catalog, outside the predicate: one refresh per run and version, none while one is in flight, none for runs that are
// over (their packages may be pruned for good), queued so the evaluation itself never starts a request.
void RunsController::requestCatalogRefresh(const QString& runId, const RunPluginRef& p) const {
  const auto run = client_->run(runId);
  const PluginHubClient* hub = plugins_->hubClient();
  if (!run || run->isOver() || hub == nullptr || hub->state() == PluginHubClient::State::Loading) return;
  const QString key = runId + QLatin1Char('|') + p.pluginId + QLatin1Char('@') + p.version;
  if (catalogTried_.contains(key)) return;
  catalogTried_.insert(key);
  QTimer::singleShot(0, plugins_, [pl = plugins_] { pl->refreshHubPlugins(); });
}

// Permissions a switch to `request` adds to what the person accepted, over the whole bundle (dependencies included), run.state exempt.
QStringList RunsController::additionsFor(const RunInfo& run, const QList<RunPluginRef>& request) const {
  QStringList out;
  for (const RunPluginRef& r : request) {
    QStringList now;
    if (!permissionsOf(r.pluginId, r.version, &now)) continue;
    QString current;
    for (const RunPluginRef& p : run.plugins)
      if (p.pluginId == r.pluginId) current = p.version;
    const QStringList baseline = current.isEmpty() ? QStringList() : acceptedBaseline(run.runId, r.pluginId);  // a plugin new to the run has accepted nothing
    for (const QString& perm : now)
      if (perm != QLatin1String("run.state") && !baseline.contains(perm) && !out.contains(perm)) out << perm;
  }
  return out;
}

QVariantList RunsController::consentNeededFor(const QString& runId, const QList<RunPluginRef>& pins) const {
  QVariantList out;
  const auto item = [&](const RunPluginRef& p, const QString& text, const QStringList& perms) {
    return QVariantMap{{QStringLiteral("pluginId"), p.pluginId}, {QStringLiteral("name"), pluginName(p.pluginId)},
                       {QStringLiteral("version"), p.version}, {QStringLiteral("permissions"), perms}, {QStringLiteral("text"), text}};
  };
  for (const RunPluginRef& p : pins) {
    QStringList now;
    if (!permissionsOf(p.pluginId, p.version, &now)) {
      requestCatalogRefresh(runId, p);
      out.append(item(p, tr("%1 %2 may ask for new permissions that are not known yet").arg(pluginName(p.pluginId), p.version), {}));
      continue;
    }
    const QStringList baseline = acceptedBaseline(runId, p.pluginId);
    QStringList added;
    for (const QString& perm : now)
      if (perm != QLatin1String("run.state") && !baseline.contains(perm) && !added.contains(perm)) added << perm;
    if (!added.isEmpty())
      out.append(item(p, tr("%1 %2 asks for: %3").arg(pluginName(p.pluginId), p.version, PluginsController::permissionLabels(added)), added));
  }
  return out;
}

void RunsController::dismissPluginConsent(const QString& why) {
  if (pluginConsent_.isEmpty()) return;
  pluginConsent_ = {};
  emit pluginConsentChanged();
  setMessage(why, true);
}

void RunsController::answerPluginConsent(bool agree) {
  const QVariantMap ask = std::exchange(pluginConsent_, {});
  emit pluginConsentChanged();
  const QString runId = ask.value(QStringLiteral("runId")).toString();
  if (runId.isEmpty()) return;
  if (!agree) {
    setMessage(tr("You did not agree to the new permissions, so the run does not start. Press Play run to review them again."), true);
    return;
  }
  for (const QVariant& v : ask.value(QStringLiteral("items")).toList()) {
    const QVariantMap it = v.toMap();
    // Exactly what the sheet displayed. An unresolved item showed no permissions, so nothing is recorded for it: once they are known the gate asks again.
    const QStringList perms = it.value(QStringLiteral("permissions")).toStringList();
    if (perms.isEmpty()) continue;
    plugins_->acceptRunPermissions(runId, it.value(QStringLiteral("pluginId")).toString(), perms);
  }
  syncRunView();
  if (plugins_->inGame()) plugins_->retryRunSet();  // the game runs without the plugins: start them now
  else playRun(runId);
}

void RunsController::applyPluginChange(const QString& runId, const QString& pluginId, bool restore) {
  if (pluginUpdateBusy_ || plugins_->hubClient() == nullptr) return;
  const auto run = client_->run(runId.toLower());
  if (!run) return;
  QList<runplugins::Change> found;
  for (const runplugins::Change& ch : runplugins::changes(*run, plugins_->hubClient()->plugins()))
    if (ch.pluginId == pluginId && ch.restore == restore) found.append(ch);
  if (found.isEmpty()) return;
  const runplugins::Change ch = found.first();
  pluginUpdateBusy_ = true;
  pluginUpdateError_.clear();
  emit pluginUpdateChanged();
  const QString id = run->runId;
  // The person who switches saw the permissions on the row: that is their agreement for these versions. Stored before the request, because
  // run_updated can arrive before the answer.
  for (const RunPluginRef& p : ch.request) {
    QStringList perms;
    if (permissionsOf(p.pluginId, p.version, &perms)) plugins_->acceptRunPermissions(id, p.pluginId, perms);
  }
  client_->setPlugins(id, ch.request, [this, id, ch](const RunResult& r) {
    pluginUpdateBusy_ = false;
    if (!r.ok) {
      // plugin_update_incompatible, plugin_requires_missing and "only the creator ...": the server says why
      pluginUpdateError_ = (r.status == 403 || r.status == 409) && !r.message.isEmpty() ? r.message : errorText(r, tr("Updating the plugins"));
    }
    emit pluginUpdateChanged();
    if (r.ok) scheduleRebuild();
  });
}

void RunsController::playRun(const QString& runId) {
  if (busy_) return;
  const auto run = client_->run(runId.toLower());
  const RunParticipant* mine = run ? run->participant(client_->myUserId()) : nullptr;
  if (!run || mine == nullptr || mine->status != QLatin1String("joined") || run->isOver()) {
    setMessage(tr("This run cannot be played any more."), true);
    return;
  }
  if (mine->gameId.isEmpty()) {
    setMessage(tr("You have not picked a game for this run."), true);
    return;
  }
  bool inLibrary = false;
  for (const GameEntry& g : library_->games())
    if (g.id == mine->gameId) inLibrary = true;
  if (!inLibrary) {
    setMessage(tr("The game of this run is not in your library."), true);
    return;
  }
  if (!run->plugins.isEmpty() && (plugins_->delivery() == nullptr || !plugins_->hubSupported())) {
    setMessage(tr("The server does not offer the plugins of this run."), true);
    return;
  }
  if (const QVariantList need = consentNeeded(*run); !need.isEmpty()) {
    pluginConsent_ = {{QStringLiteral("runId"), run->runId}, {QStringLiteral("items"), need}};
    emit pluginConsentChanged();
    return;
  }
  const quint64 gen = ++playGen_;
  const QString game = mine->gameId;
  ensureCopies(*run, [this, runId, game, gen](bool ok, const QString& error) {
    if (gen != playGen_) return;
    setBusy(false);
    if (!ok) {
      setMessage(error, true);
      return;
    }
    setMessage({}, false);
    emit launchRequested(runId.toLower(), game);
  });
}

RunLaunch RunsController::launchFor(const QString& runId) {
  RunLaunch l;
  const auto run = client_->run(runId.toLower());
  if (!run) return l;
  for (const RunPluginRef& p : run->plugins) l.pins.append({p.pluginId, p.version, p.sha256});
  l.bridge = plugins::RunSessionBridge::create(client_, run->runId);
  return l;
}

// ------------------------------------------------------------------ Start a run

namespace {
}  // namespace

StartResolution resolveStartPlugins(const QList<HubPluginInfo>& offered, const QStringList& pickedIds) {
  using plugins::VersionRange;
  StartResolution res;
  QMap<QString, QList<const HubPluginInfo*>> byId;  // newest first
  for (const HubPluginInfo& p : offered) byId[p.id].append(&p);
  for (auto it = byId.begin(); it != byId.end(); ++it)
    std::stable_sort(it->begin(), it->end(), [](const HubPluginInfo* a, const HubPluginInfo* b) { return update::compareVersions(a->version, b->version).value_or(0) > 0; });

  QMap<QString, const HubPluginInfo*> chosen, autoP;
  QStringList pickedOrder;
  for (const QString& id : pickedIds)
    if (byId.contains(id) && !chosen.contains(id)) {
      chosen.insert(id, byId.value(id).first());
      pickedOrder << id;
    }
  const auto nameOf = [&](const QString& id) {
    if (chosen.contains(id)) return chosen.value(id)->name;
    if (autoP.contains(id)) return autoP.value(id)->name;
    return byId.contains(id) ? byId.value(id).first()->name : id;
  };
  const auto fail = [&](const QString& e) {
    res.error = e;
    return res;
  };

  struct Need {
    QString rangeText;
    VersionRange range;
    QString byId;
  };
  QMap<QString, QList<Need>> needs;
  for (int iter = 0; iter < 32; ++iter) {
    needs.clear();
    const auto collect = [&](const HubPluginInfo* p) -> QString {
      for (auto it = p->requires_.begin(); it != p->requires_.end(); ++it) {
        const auto r = VersionRange::parse(it.value().toString());
        if (!r) return QObject::tr("%1 needs %2 with the version range \"%3\", which is not valid.").arg(p->name, nameOf(it.key()), it.value().toString());
        needs[it.key()].append({it.value().toString(), *r, p->id});
      }
      return {};
    };
    for (const HubPluginInfo* p : std::as_const(chosen))
      if (const QString e = collect(p); !e.isEmpty()) return fail(e);
    for (const HubPluginInfo* p : std::as_const(autoP))
      if (const QString e = collect(p); !e.isEmpty()) return fail(e);

    QMap<QString, const HubPluginInfo*> next;
    for (auto it = needs.constBegin(); it != needs.constEnd(); ++it) {
      const QString& dep = it.key();
      const QList<Need>& list = it.value();
      const auto fits = [&](const HubPluginInfo* c) {
        const auto v = update::SemVer::parse(c->version);
        return v && std::all_of(list.begin(), list.end(), [&](const Need& n) { return n.range.matches(*v); });
      };
      const HubPluginInfo* found = nullptr;
      for (const HubPluginInfo* c : byId.value(dep)) {
        if (chosen.contains(dep) && c != chosen.value(dep)) continue;  // a picked plugin runs in the picked version
        if (fits(c)) {
          found = c;
          break;
        }
      }
      if (found == nullptr) {
        const Need* conflict = nullptr;
        for (const Need& n : list)
          if (n.rangeText != list.first().rangeText) {
            conflict = &n;
            break;
          }
        if (conflict != nullptr)
          return fail(QObject::tr("%1 needs %2 %3, but %4 needs %2 %5. No version satisfies both.")
                          .arg(nameOf(list.first().byId), nameOf(dep), list.first().rangeText, nameOf(conflict->byId), conflict->rangeText));
        return fail(QObject::tr("%1 needs %2 %3, which this server does not offer.").arg(nameOf(list.first().byId), nameOf(dep), list.first().rangeText));
      }
      if (!chosen.contains(dep)) next.insert(dep, found);
    }
    if (next == autoP) break;
    autoP = next;
  }

  for (const QString& id : pickedOrder) res.picked.append(*chosen.value(id));
  for (auto it = autoP.constBegin(); it != autoP.constEnd(); ++it) {
    StartIncluded inc;
    inc.info = *it.value();
    for (const Need& n : needs.value(it.key())) {
      const QString by = nameOf(n.byId);
      if (!inc.neededBy.contains(by)) inc.neededBy << by;
    }
    inc.neededBy.sort();
    res.included.append(inc);
  }
  return res;
}

StartResolution RunsController::currentStartResolution() const {
  const PluginHubClient* hub = plugins_->hubClient();
  if (hub == nullptr) return {};
  QStringList ids = QStringList(startPluginIds_.begin(), startPluginIds_.end());
  ids.sort();
  return resolveStartPlugins(hub->plugins(), ids);
}

void RunsController::rebuildStartPlugins() {
  QList<QVariantMap> rows;
  const StartResolution res = currentStartResolution();
  QSet<QString> includedIds;
  for (const StartIncluded& i : res.included) includedIds.insert(i.info.id);
  if (const PluginHubClient* hub = plugins_->hubClient()) {
    QMap<QString, const HubPluginInfo*> best;
    for (const HubPluginInfo& p : hub->plugins()) {
      if (!p.permissions.contains(QStringLiteral("run.state"))) continue;
      if (includedIds.contains(p.id)) continue;  // shown as "Included", not as a row
      const auto it = best.constFind(p.id);
      if (it == best.constEnd() || update::compareVersions(p.version, (*it)->version).value_or(0) > 0) best.insert(p.id, &p);
    }
    for (auto it = best.constBegin(); it != best.constEnd(); ++it)
      rows.append({{QStringLiteral("id"), it.key()},
                   {QStringLiteral("name"), (*it)->name},
                   {QStringLiteral("version"), (*it)->version},
                   {QStringLiteral("description"), (*it)->description},
                   {QStringLiteral("origin"), plugins_->hubOffer(it.key(), (*it)->version).value(QStringLiteral("origin"))},
                   {QStringLiteral("originText"), plugins_->hubOffer(it.key(), (*it)->version).value(QStringLiteral("originText"))},
                   {QStringLiteral("permissionsText"), plugins_->hubOffer(it.key(), (*it)->version).value(QStringLiteral("permissionsText"))},
                   {QStringLiteral("checked"), startPluginIds_.contains(it.key())}});
  }
  startPlugins_.setRows(rows);

  // The "Rules" select: None first (only when the Hub accepts runs without a plugin), then the offered run plugins.
  QVariantList opts;
  if (pluginlessRuns()) opts.append(QVariantMap{{QStringLiteral("value"), QString()}, {QStringLiteral("label"), tr("None")}});
  for (const QVariantMap& r : std::as_const(rows))
    opts.append(QVariantMap{{QStringLiteral("value"), r.value(QStringLiteral("id"))},
                            {QStringLiteral("label"), r.value(QStringLiteral("name")).toString() + QLatin1Char(' ') + r.value(QStringLiteral("version")).toString()}});
  startRulesOptions_ = opts;
  startRulesCurrent_ = res.picked.isEmpty() ? QString() : res.picked.first().id;

  QList<QVariantMap> inc;
  for (const StartIncluded& i : res.included)
    inc.append({{QStringLiteral("id"), i.info.id},
                {QStringLiteral("name"), i.info.name},
                {QStringLiteral("version"), i.info.version},
                {QStringLiteral("text"), tr("Included: %1 %2 · needed by %3").arg(i.info.name, i.info.version, i.neededBy.join(QStringLiteral(", ")))}});
  startIncluded_.setRows(inc);

  // Rules: one group per plugin of the run that declares run settings.
  QList<QVariantMap> rules;
  QList<const HubPluginInfo*> all;
  for (const HubPluginInfo& p : res.picked) all.append(&p);
  for (const StartIncluded& i : res.included) all.append(&i.info);
  // Run setting strings are keyed by base language ("de"), not by region.
  const QString lang = plugins_->resolveLanguage().section(QLatin1Char('-'), 0, 0);
  for (const HubPluginInfo* p : std::as_const(all)) {
    if (p->runSettings.isEmpty()) continue;
    rules.append({{QStringLiteral("rowId"), QStringLiteral("h:") + p->id}, {QStringLiteral("kind"), QStringLiteral("header")}, {QStringLiteral("pluginId"), p->id},
                  {QStringLiteral("title"), p->name}, {QStringLiteral("key"), QString()}, {QStringLiteral("widget"), QString()}, {QStringLiteral("label"), QString()},
                  {QStringLiteral("hint"), QString()}, {QStringLiteral("value"), QVariant()}, {QStringLiteral("options"), QVariantList()}});
    const QVariantMap changed = startRuleValues_.value(p->id);
    for (const RunSetting& rs : p->runSettings) {
      QVariantList opts;
      for (const RunSettingOption& o : rs.options)
        opts.append(QVariantMap{{QStringLiteral("value"), o.value}, {QStringLiteral("label"), runSettingText(p->runSettingsStrings, lang, o.label)}});
      const QString widget = rs.type == QLatin1String("boolean") ? QStringLiteral("toggle") : (rs.options.size() <= 3 ? QStringLiteral("segments") : QStringLiteral("select"));
      rules.append({{QStringLiteral("rowId"), QStringLiteral("s:") + p->id + QLatin1Char(':') + rs.key}, {QStringLiteral("kind"), QStringLiteral("setting")},
                    {QStringLiteral("pluginId"), p->id}, {QStringLiteral("title"), QString()}, {QStringLiteral("key"), rs.key}, {QStringLiteral("widget"), widget},
                    {QStringLiteral("label"), runSettingText(p->runSettingsStrings, lang, rs.label)},
                    {QStringLiteral("hint"), rs.hint.isEmpty() ? QString() : runSettingText(p->runSettingsStrings, lang, rs.hint)},
                    {QStringLiteral("value"), changed.contains(rs.key) ? changed.value(rs.key) : rs.def}, {QStringLiteral("options"), opts}});
    }
  }
  startRules_.setRows(rules);

  if (!res.error.isEmpty()) {
    startError_ = res.error;
    startResolveError_ = true;
  } else if (startResolveError_) {
    startError_.clear();
    startResolveError_ = false;
  }
  emit startStateChanged();
}

void RunsController::setStartRule(const QString& pluginId, const QString& key, const QVariant& value) {
  const StartResolution res = currentStartResolution();
  const HubPluginInfo* plugin = nullptr;
  for (const HubPluginInfo& p : res.picked)
    if (p.id == pluginId) plugin = &p;
  for (const StartIncluded& i : res.included)
    if (i.info.id == pluginId) plugin = &i.info;
  if (plugin == nullptr) return;
  for (const RunSetting& rs : plugin->runSettings) {
    if (rs.key != key) continue;
    if (rs.type == QLatin1String("boolean")) {
      startRuleValues_[pluginId].insert(key, value.toBool());
    } else {
      const QString v = value.toString();
      const bool known = std::any_of(rs.options.begin(), rs.options.end(), [&](const RunSettingOption& o) { return o.value == v; });
      if (!known) return;
      startRuleValues_[pluginId].insert(key, v);
    }
    rebuildStartPlugins();
    return;
  }
}

void RunsController::rebuildStartUsers() {
  QList<QVariantMap> rows;
  const QString me = client_->myUserId();
  // Players that are already in the open run cannot be invited again.
  QSet<QString> skip{me};
  if (const auto run = openRunId_.isEmpty() ? std::nullopt : client_->run(openRunId_))
    for (const RunParticipant& p : run->participants)
      if (p.status == QLatin1String("joined") || p.status == QLatin1String("invited")) skip.insert(p.userId);
  for (const HubUser& u : users_) {
    if (skip.contains(u.id)) continue;
    rows.append({{QStringLiteral("id"), u.id}, {QStringLiteral("name"), u.displayName}, {QStringLiteral("online"), u.online}, {QStringLiteral("checked"), startUserIds_.contains(u.id)}});
  }
  startUsers_.setRows(rows);
}

QVariantMap RunsController::startCoreRules() const { return runrules::toMap(startCore_); }

void RunsController::setStartCoreRule(const QString& key, const QVariant& value) {
  if (!runrules::setRule(startCore_, key, value)) return;
  emit startStateChanged();
}

void RunsController::clearRulesError() {
  if (rulesError_.isEmpty()) return;
  rulesError_.clear();
  emit rulesStateChanged();
}

QString RunsController::rulesDraftWarning(const QVariantMap& draft) const {
  const auto run = openRunId_.isEmpty() ? std::nullopt : client_->run(openRunId_);
  if (!run) return {};
  return runrules::draftWarning(run->rules, runrules::fromMap(draft));
}

// "Lena" / "Lena · today 21:30": who set the rules last (the newest rules.changed event), else the creator who set them at the start.
QString RunsController::rulesSetBy(const RunInfo& run) const {
  const QList<RunEvent> all = client_->events(run.runId, 0, 500);
  for (auto it = all.crbegin(); it != all.crend(); ++it) {
    if (it->type != QLatin1String("rules.changed")) continue;
    QString by = it->payload.value(QStringLiteral("by_user_name")).toString();
    if (by.isEmpty()) by = it->userName;
    if (!by.isEmpty()) return tr("%1 · %2").arg(by, timeText(it->createdAt));
  }
  return userName(run, run.createdBy);
}

void RunsController::saveRunRules(const QString& runId, const QVariantMap& rules) {
  if (rulesBusy_) return;
  rulesBusy_ = true;
  rulesError_.clear();
  emit rulesStateChanged();
  const QString id = runId.toLower();
  client_->setRules(id, runrules::fromMap(rules), [this, id](const RunResult& r) {
    rulesBusy_ = false;
    rulesError_ = r.ok ? QString() : (r.errorCode == QLatin1String("forbidden") ? tr("Only the person who started the run or an admin can change its rules.") : errorText(r, tr("Changing the rules")));
    emit rulesStateChanged();
    if (r.ok) {
      scheduleRebuild();
      emit rulesSaved(id);
    }
  });
}

void RunsController::beginStart() {
  startCore_ = RunRules();
  startError_.clear();
  startBusy_ = false;
  startPluginIds_.clear();
  startRuleValues_.clear();
  startResolveError_ = false;
  startUserIds_.clear();
  users_.clear();
  startUsersLoading_ = true;
  if (plugins_->hubClient() != nullptr) plugins_->refreshHubPlugins();
  rebuildStartPlugins();
  rebuildStartUsers();
  emit startStateChanged();
  client_->loadUsers([this](bool ok, const QList<HubUser>& users, const QString& error) {
    startUsersLoading_ = false;
    if (ok) {
      users_ = users;
      rebuildStartUsers();
    } else {
      startError_ = tr("The player list could not be loaded: %1").arg(error);
    }
    emit startStateChanged();
  });
  prepareGames(QString());
}

void RunsController::toggleStartPlugin(const QString& pluginId) {
  if (!startPluginIds_.remove(pluginId)) startPluginIds_.insert(pluginId);
  rebuildStartPlugins();
  prepareGames(QString());
}

void RunsController::selectStartRules(const QString& pluginId) {
  if (pluginId.isEmpty() && !pluginlessRuns()) return;  // without runs_v2 a plugin stays required
  startPluginIds_.clear();
  if (!pluginId.isEmpty()) startPluginIds_.insert(pluginId);
  rebuildStartPlugins();
  prepareGames(QString());
}

void RunsController::toggleStartUser(const QString& userId) {
  if (!startUserIds_.remove(userId)) startUserIds_.insert(userId);
  rebuildStartUsers();
}

void RunsController::cancelStart() {
  startError_.clear();
  startBusy_ = false;
  emit startStateChanged();
}

void RunsController::startRun(const QString& name, const QString& gameId) {
  startRun(name, gameId, QVariantMap());
}

void RunsController::startRun(const QString& name, const QString& gameId, const QVariantMap& randomizer) {
  const QString trimmed = name.trimmed();
  const StartResolution resolved = currentStartResolution();
  QString problem;
  if (trimmed.isEmpty() || trimmed.size() > 64) problem = tr("Give the run a name (1 to 64 characters).");
  else if (startPluginIds_.isEmpty() && !pluginlessRuns()) problem = tr("Pick the rules plugin for the run.");
  else if (!resolved.error.isEmpty()) problem = resolved.error;
  else if (!startPluginIds_.isEmpty() && resolved.picked.isEmpty()) problem = tr("The picked plugin is not offered by your server any more. Pick another.");
  else if (resolved.total() > 8) problem = tr("A run can use up to 8 plugins, including the ones they need.");
  else if (gameId.isEmpty()) problem = tr("Pick the game you play in this run.");
  else if (std::none_of(games_.rows().begin(), games_.rows().end(), [&](const QVariantMap& r) { return r.value(QStringLiteral("id")).toString() == gameId; })) problem = tr("The picked game does not fit the plugins of this run. Pick another game.");
  else if (startUserIds_.size() + 1 > kRunMaxParticipants) problem = tr("A run has room for %1 players.").arg(kRunMaxParticipants);
  const QString rzMode = randomizer.value(QStringLiteral("mode")).toString();
  QJsonObject rz;
  if (!rzMode.isEmpty() && rzMode != QLatin1String("off")) {
    const QString settings = randomizer.value(QStringLiteral("settings")).toString().trimmed();
    const QString toolVersion = randomizer.value(QStringLiteral("tool_version")).toString();
    if (problem.isEmpty() && settings.isEmpty()) problem = tr("Pick a preset or paste a settings string for the randomizer.");
    else if (problem.isEmpty() && toolVersion.isEmpty()) problem = tr("The server offers no randomizer. Admins: Server › Tools.");
    rz = {{QStringLiteral("mode"), rzMode},
          {QStringLiteral("settings"), settings},
          {QStringLiteral("tool_id"), QStringLiteral("randomizer")},
          {QStringLiteral("tool_version"), toolVersion},
          {QStringLiteral("allow_own"), randomizer.value(QStringLiteral("allow_own")).toBool()}};
    const QString presetId = randomizer.value(QStringLiteral("preset_id")).toString();
    if (!presetId.isEmpty() && presetId != QLatin1String("custom")) rz.insert(QStringLiteral("preset_id"), presetId);
  }
  if (!problem.isEmpty()) {
    startError_ = problem;
    emit startStateChanged();
    return;
  }
  QList<RunPluginRef> pins;
  QJsonObject settings;
  const auto add = [&](const HubPluginInfo& p) {
    pins.append({p.id, p.version, {}});
    if (!p.runSettings.isEmpty()) settings.insert(p.id, buildRunSettings(p.runSettings, startRuleValues_.value(p.id)));
  };
  for (const HubPluginInfo& p : resolved.picked) add(p);
  for (const StartIncluded& i : resolved.included) add(i.info);
  startBusy_ = true;
  startError_.clear();
  emit startStateChanged();
  // The creator accepts exactly the versions and permissions the start dialog offered, like joining does; a pin that differs stays unaccepted.
  QMap<QString, QPair<QString, QStringList>> reviewed;
  for (const HubPluginInfo& p : resolved.picked) reviewed.insert(p.id, {p.version, p.permissions});
  for (const StartIncluded& i : resolved.included) reviewed.insert(i.info.id, {i.info.version, i.info.permissions});
  client_->createRun(trimmed, pins, gameId, QStringList(startUserIds_.begin(), startUserIds_.end()), settings, startCore_, [this, reviewed](const RunResult& r) {
    startBusy_ = false;
    if (!r.ok || !r.run) {
      startError_ = errorText(r, tr("Starting the run"));
      emit startStateChanged();
      return;
    }
    for (const RunPluginRef& p : r.run->plugins) {
      const auto it = reviewed.constFind(p.pluginId);
      if (it != reviewed.constEnd() && it->first == p.version) plugins_->acceptRunPermissions(r.run->runId, p.pluginId, it->second);
    }
    emit startStateChanged();
    openRun(r.run->runId);
    ensureCopies(*r.run, [this](bool, const QString&) { setBusy(false); });
    emit runCreated(r.run->runId);
  }, rz);
}

void RunsController::inviteSelected(const QString& runId) {
  if (startUserIds_.isEmpty()) {
    startError_ = tr("Pick at least one player.");
    emit startStateChanged();
    return;
  }
  startBusy_ = true;
  startError_.clear();
  emit startStateChanged();
  client_->inviteUsers(runId, QStringList(startUserIds_.begin(), startUserIds_.end()), [this](const RunResult& r) {
    startBusy_ = false;
    startError_ = r.ok ? QString() : errorText(r, tr("Inviting"));
    emit startStateChanged();
    if (r.ok) startUserIds_.clear();
  });
}

}  // namespace uzume::ui
