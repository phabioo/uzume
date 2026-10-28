#pragma once
// RunsController: the Runs screen of the Player (docs/design/legacy/plugins.md 4j-2). Owns the RunClient of the active Hub and
// presents it to QML: invite cards, runs by group, one run in detail (participants, status actions, event log), the
// "Start a run" dialog and "Play run", which first makes sure the exact Hub copy of every plugin of the run is on this
// device and then asks the PlayerController to start the participant's game with the run context.
#include <QAbstractItemModel>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <memory>

#include "hubconnection.h"
#include "hublibrary.h"
#include "keyedrowsmodel.h"
#include "pluginscontroller.h"
#include "runclient.h"
#include "runrecapcontroller.h"
#include "variantscontroller.h"

namespace uzume::ui {

// The plugins of a run that is being started: the picked ones (newest version the Hub offers) plus the plugins they
// require, transitively (newest offered version that satisfies every range). `error` is set when a requirement cannot be met.
struct StartIncluded {
  HubPluginInfo info;
  QStringList neededBy;  // display names, sorted
};
struct StartResolution {
  QList<HubPluginInfo> picked;
  QList<StartIncluded> included;
  QString error;
  int total() const { return static_cast<int>(picked.size() + included.size()); }
};
StartResolution resolveStartPlugins(const QList<HubPluginInfo>& offered, const QStringList& pickedIds);

class RunsController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  // The Runs entry is shown whenever the connected Hub offers runs (runs_v1); runs need no plugin (runs_v2, ADR 0030 D7).
  Q_PROPERTY(bool available READ available NOTIFY availableChanged)
  Q_PROPERTY(bool pluginlessRuns READ pluginlessRuns NOTIFY availableChanged)  // the Hub accepts runs with no plugin (runs_v2)
  Q_PROPERTY(int pendingInvites READ pendingInvites NOTIFY runsChanged)  // sidebar badge
  Q_PROPERTY(bool loaded READ loaded NOTIFY runsChanged)                 // the list from the Hub arrived
  // Rows {runId, headline, subText, joinedText} / {runId, name, status, statusText, tone, subText, timeText, action,
  // participants: [{name, color}], mine}. "active" holds Starting, Active and Paused runs.
  Q_PROPERTY(QAbstractItemModel* invites READ invites CONSTANT)
  Q_PROPERTY(QAbstractItemModel* active READ active CONSTANT)
  Q_PROPERTY(QAbstractItemModel* finished READ finished CONSTANT)
  Q_PROPERTY(QString activeTitle READ activeTitle NOTIFY runsChanged)  // "ACTIVE · 1 · PAUSED · 1"
  Q_PROPERTY(int finishedCount READ finishedCount NOTIFY runsChanged)
  // One run in detail. detail: {runId, name, status, statusText, tone, line, pluginsText, canPlay, playLabel, canStart,
  // canPause, canResume, canFinish, canAbandon, canLeave, canInvite, mine: bool, over: bool, note, saveSlotText}.
  Q_PROPERTY(QString openRunId READ openRunId NOTIFY detailChanged)
  Q_PROPERTY(QVariantMap detail READ detail NOTIFY detailChanged)
  // Plugin content of the opened run (slot "run", PluginsController::runSections): "" | "loading" (copies are fetched or the
  // plugins start) | "error"; runViewText says what is going on or wrong.
  Q_PROPERTY(QString runViewState READ runViewState NOTIFY runViewChanged)
  Q_PROPERTY(QString runViewText READ runViewText NOTIFY runViewChanged)
  // Tabs of the opened run view: "board" | "log" | "plugin:<pluginId>". runTabs = the plugin tabs, last in the tab bar:
  // [{pluginId, title, name, badge}] of the run's plugins that declare ui.screen or ui.run, are known to the Hub and run here.
  Q_PROPERTY(QString runTab READ runTab WRITE setRunTab NOTIFY runTabChanged)
  Q_PROPERTY(QVariantList runTabs READ runTabs NOTIFY runViewChanged)
  Q_PROPERTY(QAbstractItemModel* participants READ participants CONSTANT)  // {userId, name, color, gameTitle, statusText, slotText, me}
  Q_PROPERTY(QAbstractItemModel* events READ events CONSTANT)              // {seq, timeText, author, text, sourceText}
  Q_PROPERTY(QString message READ message NOTIFY messageChanged)
  Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)  // a request or the plugin preparation runs
  Q_PROPERTY(QString busyText READ busyText NOTIFY busyChanged)
  // Games of the library the participant may bring: {id, title, system}; refilled by prepareGames().
  Q_PROPERTY(QAbstractItemModel* games READ games CONSTANT)
  // Plugins of the run to join, {id, name, version, origin, originText, permissionsText}; joinUnverified = one of them is
  // unverified (the dialog then shows unverifiedWarning; joining counts as the consent to run it).
  Q_PROPERTY(QAbstractItemModel* joinPlugins READ joinPlugins CONSTANT)
  Q_PROPERTY(bool joinUnverified READ joinUnverified NOTIFY joinPluginsChanged)
  Q_PROPERTY(bool startUnverified READ startUnverified NOTIFY startStateChanged)  // a picked plugin of the start dialog is unverified
  Q_PROPERTY(QString unverifiedWarning READ unverifiedWarning CONSTANT)
  // Start a run dialog. startRulesOptions = the "Rules" select, [{value, label}] ("" = None first, only with runs_v2) and the offered
  // run plugins; startRulesCurrent = the picked value.
  Q_PROPERTY(QVariantList startRulesOptions READ startRulesOptions NOTIFY startStateChanged)
  Q_PROPERTY(QString startRulesCurrent READ startRulesCurrent NOTIFY startStateChanged)
  Q_PROPERTY(QAbstractItemModel* startPlugins READ startPlugins CONSTANT)  // {id, name, version, description, origin, originText, permissionsText, checked}
  Q_PROPERTY(QAbstractItemModel* startUsers READ startUsers CONSTANT)      // {id, name, online, checked}
  // Plugins the picked ones need, included automatically: {id, name, version, text}.
  Q_PROPERTY(QAbstractItemModel* startIncluded READ startIncluded CONSTANT)
  // The "Rules" section (empty = hidden): one header row per plugin with run settings, then one row per setting:
  // {rowId, kind: header|setting, pluginId, title, key, widget: toggle|segments|select, label, hint, value, options: [{value, label}]}.
  Q_PROPERTY(QAbstractItemModel* startRules READ startRules CONSTANT)
  // Core run rules (run_rules_v1, design 11e): the draft of the Start dialog {states, rewind, cheats, memoryWrite}; false on an older server.
  Q_PROPERTY(QVariantMap startCoreRules READ startCoreRules NOTIFY startStateChanged)
  Q_PROPERTY(bool coreRulesAvailable READ coreRulesAvailable NOTIFY startStateChanged)
  // The Rules dialog of the open run (design 11f): saving, and why it failed.
  Q_PROPERTY(bool rulesBusy READ rulesBusy NOTIFY rulesStateChanged)
  Q_PROPERTY(QString rulesError READ rulesError NOTIFY rulesStateChanged)
  // Updating the pinned plugins (run_plugin_updates_v1): the busy state and error of the Update / Restore action, and the consent sheet that
  // "Play run" opens when a plugin version asks for permissions the person has not agreed to in this run:
  // pluginConsent = {runId, items: [{pluginId, name, version, text}]} (empty = nothing to ask).
  Q_PROPERTY(bool pluginUpdateBusy READ pluginUpdateBusy NOTIFY pluginUpdateChanged)
  Q_PROPERTY(QString pluginUpdateError READ pluginUpdateError NOTIFY pluginUpdateChanged)
  Q_PROPERTY(QVariantMap pluginConsent READ pluginConsent NOTIFY pluginConsentChanged)
  Q_PROPERTY(QString startError READ startError NOTIFY startStateChanged)
  Q_PROPERTY(bool startBusy READ startBusy NOTIFY startStateChanged)
  Q_PROPERTY(bool startUsersLoading READ startUsersLoading NOTIFY startStateChanged)
  // The variants controller (0.21): the Randomizer section of Start a run and the join dialog read presets and own variants from it.
  Q_PROPERTY(uzume::ui::VariantsController* variants READ variants CONSTANT)
  // The Recap tab of the opened run (0.22, board 4jh-app-2).
  Q_PROPERTY(uzume::ui::RunRecapController* recap READ recap CONSTANT)

 public:
  RunsController(RunClient* client, PluginsController* plugins, HubLibrary* library, HubConnection* connection, QObject* parent = nullptr);

  bool available() const;
  bool pluginlessRuns() const { return client_ && client_->supportsPluginless(); }
  QString runTab() const { return runTab_; }
  void setRunTab(const QString& tab);
  QVariantList runTabs() const { return runTabs_; }
  QVariantList startRulesOptions() const { return startRulesOptions_; }
  QString startRulesCurrent() const { return startRulesCurrent_; }
  // "Run: <run name>" for the save slot of a known run, else empty (the raw slot name is then shown).
  QString slotDisplayName(const QString& slot) const;
  // Id of the run whose save slot this is ("" = a plain slot): the game page's "Open run" link.
  Q_INVOKABLE QString runIdForSlot(const QString& slot) const;
  // Save slot of a run: the Hub's save_slot, else "run-" + 8 hex.
  QString saveSlotFor(const QString& runId) const;
  QAbstractItemModel* joinPlugins() { return &joinPlugins_; }
  bool joinUnverified() const { return joinUnverified_; }
  bool startUnverified() const;
  QString unverifiedWarning() const;
  int pendingInvites() const { return client_->pendingInviteCount(); }
  bool loaded() const { return client_->loaded(); }
  QAbstractItemModel* invites() { return &invites_; }
  QAbstractItemModel* active() { return &active_; }
  QAbstractItemModel* finished() { return &finished_; }
  QString activeTitle() const { return activeTitle_; }
  int finishedCount() const { return finished_.count(); }
  QString openRunId() const { return openRunId_; }
  QString runViewState() const { return runViewState_; }
  QString runViewText() const { return runViewText_; }
  QVariantMap detail() const { return detail_; }
  QAbstractItemModel* participants() { return &participants_; }
  QAbstractItemModel* events() { return &events_; }
  QString message() const { return message_; }
  bool messageIsError() const { return messageIsError_; }
  bool busy() const { return busy_; }
  QString busyText() const { return busyText_; }
  QAbstractItemModel* games() { return &games_; }
  QAbstractItemModel* startPlugins() { return &startPlugins_; }
  QAbstractItemModel* startIncluded() { return &startIncluded_; }
  QAbstractItemModel* startRules() { return &startRules_; }
  QAbstractItemModel* startUsers() { return &startUsers_; }
  QString startError() const { return startError_; }
  QVariantMap startCoreRules() const;
  bool coreRulesAvailable() const { return client_ && client_->supportsRules(); }
  bool rulesBusy() const { return rulesBusy_; }
  QString rulesError() const { return rulesError_; }
  bool startBusy() const { return startBusy_; }
  bool startUsersLoading() const { return startUsersLoading_; }

  RunClient* client() { return client_; }
  VariantsController* variants() const { return variants_; }
  RunRecapController* recap() const { return recap_; }
  void setVariants(VariantsController* v) { variants_ = v; }
  // One line for a Hub run event ("Lena paused the run"), as the run board shows it.
  QString eventSummary(const RunEvent& e) const { return hubEventText(e); }

  // The run context for a game start: the bridge of uz.run and the pins of the plugins (null bridge: unknown run).
  RunLaunch launchFor(const QString& runId);

  Q_INVOKABLE void refresh();
  Q_INVOKABLE void openRun(const QString& runId);
  // Selects the run tab of a plugin (uz.ui.open("screen")); false when no run is open or the plugin has no tab there.
  Q_INVOKABLE bool selectPluginTab(const QString& pluginId);
  // The plugins of the opened run could not be fetched: try again.
  Q_INVOKABLE void retryRunView();
  Q_INVOKABLE void closeRun();
  // Fills `games` with the library games that fit the run's plugins (empty runId: the plugins ticked in the Start dialog, or
  // every game when none is ticked); returns the count. A game fits when every plugin accepts its system and game code;
  // games without a known code stay in the list after the matching ones with the hint "Game not recognized".
  Q_INVOKABLE int prepareGames(const QString& runId);
  Q_INVOKABLE void joinRun(const QString& runId, const QString& gameId);
  // Join a randomized run (variants_v1): variantKind "run" (the run's own variant) or "own" with the id of an own variant. A 409
  // own_variants_off / own_variant_mismatch is shown as its own sentence in `message`.
  Q_INVOKABLE void joinRun(const QString& runId, const QString& gameId, const QString& variantKind, const QString& variantId);
  // The randomizer block of the run to join (10f-1): {mode, presetId, toolVersion, allowOwn, baseSha256} or empty for an ordinary run.
  Q_INVOKABLE QVariantMap randomizerOf(const QString& runId) const;
  Q_INVOKABLE void declineRun(const QString& runId);
  Q_INVOKABLE void leaveRun(const QString& runId);
  // status: "active" (start / resume), "paused", "finished", "abandoned".
  Q_INVOKABLE void setRunStatus(const QString& runId, const QString& status);
  Q_INVOKABLE void playRun(const QString& runId);
  // Update or restore one plugin of the open run (creator or admin; detail.pluginChanges lists what is offered). The server message is shown on failure.
  Q_INVOKABLE void applyPluginChange(const QString& runId, const QString& pluginId, bool restore);
  // Answer of the consent sheet: true remembers the agreement (run + plugin + version) and goes on with Play run, false drops it (the game does not start).
  Q_INVOKABLE void answerPluginConsent(bool agree);
  bool pluginUpdateBusy() const { return pluginUpdateBusy_; }
  QString pluginUpdateError() const { return pluginUpdateError_; }
  QVariantMap pluginConsent() const { return pluginConsent_; }
  // Pins of the run that ask for permissions the person has not agreed to yet: [{pluginId, name, version, text}].
  QVariantList consentNeeded(const RunInfo& run) const;
  QVariantList consentNeededFor(const QString& runId, const QList<RunPluginRef>& pins) const;
  bool permissionsOf(const QString& pluginId, const QString& version, QStringList* out) const;
  QSet<QString> appendedPlugins(const QString& runId) const;
  QStringList acceptedBaseline(const QString& runId, const QString& pluginId, const QString& previousVersion, bool appended, const QStringList& now) const;
  QStringList additionsFor(const RunInfo& run, const QList<RunPluginRef>& request) const;
  void requestCatalogRefresh(const QString& runId, const RunPluginRef& p) const;
  Q_INVOKABLE void dismissMessage();
  // The app could not start the game of a run: the reason shows on the run screen (the same text as the game page).
  void showLaunchProblem(const QString& text) { setMessage(text, true); }

  // Start a run (and invite more players into an open run).
  Q_INVOKABLE void beginStart();  // loads the users and the offered plugins
  Q_INVOKABLE void toggleStartPlugin(const QString& pluginId);
  // The "Rules" select: the plugin id, "" = None (only offered with runs_v2).
  Q_INVOKABLE void selectStartRules(const QString& pluginId);
  // Sets one run rule of the Start dialog (bool for a toggle, the option value for an enum); unknown keys and values are ignored.
  Q_INVOKABLE void setStartRule(const QString& pluginId, const QString& key, const QVariant& value);
  // Sets one core rule of the Start dialog: states / rewind (bool), cheats / memoryWrite ("off" | "logged"); unknown keys and levels are ignored.
  Q_INVOKABLE void setStartCoreRule(const QString& key, const QVariant& value);
  // PUT /runs/{id}/rules with the whole object {states, rewind, cheats, memoryWrite} (creator or admin). rulesSaved() on success.
  Q_INVOKABLE void saveRunRules(const QString& runId, const QVariantMap& rules);
  Q_INVOKABLE void clearRulesError();
  // The warning line of the Rules dialog for a draft against the open run's saved rules ("" = none).
  Q_INVOKABLE QString rulesDraftWarning(const QVariantMap& draft) const;
  Q_INVOKABLE void toggleStartUser(const QString& userId);
  Q_INVOKABLE void startRun(const QString& name, const QString& gameId);
  // With the Randomizer section of the dialog (10e): {mode: off|each|same, preset_id, settings, tool_version, allow_own}. Mode off or an
  // empty map starts an ordinary run.
  Q_INVOKABLE void startRun(const QString& name, const QString& gameId, const QVariantMap& randomizer);
  Q_INVOKABLE void inviteSelected(const QString& runId);
  Q_INVOKABLE void cancelStart();

 signals:
  void availableChanged();
  void runsChanged();
  void detailChanged();
  void runViewChanged();
  void runTabChanged();
  void messageChanged();
  void busyChanged();
  void startStateChanged();
  void rulesStateChanged();
  void rulesSaved(const QString& runId);
  void pluginUpdateChanged();
  void pluginConsentChanged();
  void joinPluginsChanged();
  // Everything for the run is ready: start `gameId` with the run context (PlayerController).
  void launchRequested(const QString& runId, const QString& gameId);
  // A run was created (the Start dialog closes and the detail opens).
  void runCreated(const QString& runId);
  // Joined a randomized run with an own variant: the variants controller remembers the choice (it plays in the run slot).
  void ownVariantJoined(const QString& runId, const QString& variantId);

 private:
  void scheduleRebuild();
  void rebuild();
  void rebuildDetail();
  void rebuildEvents();
  void syncRunView();  // starts/stops the run view plugins for the opened run (PluginsController::startRunView)
  void setRunViewState(const QString& state, const QString& text);
  void rebuildRunTabs();
  void rebuildStartPlugins();
  void rebuildStartUsers();
  void setMessage(const QString& text, bool error);
  void setBusy(bool busy, const QString& text = {});
  QString pluginName(const QString& pluginId) const;
  QString pluginsText(const RunInfo& run) const;
  QString gameTitle(const QString& gameId) const;
  QString userName(const RunInfo& run, const QString& userId) const;
  QString rulesSetBy(const RunInfo& run) const;
  QString errorText(const RunResult& r, const QString& what) const;
  QString hubEventText(const RunEvent& e) const;
  QString timeText(const QString& iso) const;
  QVariantMap rowFor(const RunInfo& run) const;
  void ensureCopies(const RunInfo& run, std::function<void(bool ok, const QString& error)> done);
  // The `games` patterns of every plugin of the run ({system, codes?}); a plugin without patterns is no constraint.
  QList<QJsonArray> gamePatternsFor(const RunInfo* run) const;
  StartResolution currentStartResolution() const;
  QList<QJsonArray> startGamePatterns() const;  // same for the plugins ticked in the Start dialog (newest offered version)
  int fillGames(const QList<QJsonArray>& patterns);
  void refreshGames();  // re-fills `games` for the last prepareGames() call (library or plugin list arrived later)
  bool gamesPrepared_ = false;
  QString gamesFor_;

  VariantsController* variants_ = nullptr;
  RunRecapController* recap_ = nullptr;
  QPointer<RunClient> client_;
  PluginsController* plugins_;
  HubLibrary* library_;
  QPointer<HubConnection> conn_;
  KeyedRowsModel invites_{{QStringLiteral("runId"), QStringLiteral("headline"), QStringLiteral("subText"), QStringLiteral("joinedText"), QStringLiteral("name"), QStringLiteral("inviteLine")}, QStringLiteral("runId")};
  static QStringList runRoles() {
    return {QStringLiteral("runId"),  QStringLiteral("name"),     QStringLiteral("status"),  QStringLiteral("statusText"), QStringLiteral("tone"),
            QStringLiteral("subText"), QStringLiteral("timeText"), QStringLiteral("action"), QStringLiteral("participants"),
            QStringLiteral("game"), QStringLiteral("rulesName"), QStringLiteral("rulesVersion"), QStringLiteral("playTimeText"), QStringLiteral("randomized")};
  }
  KeyedRowsModel active_{runRoles(), QStringLiteral("runId")};
  KeyedRowsModel finished_{runRoles(), QStringLiteral("runId")};
  KeyedRowsModel participants_{{QStringLiteral("userId"), QStringLiteral("name"), QStringLiteral("color"), QStringLiteral("gameTitle"), QStringLiteral("statusText"),
                                QStringLiteral("slotText"), QStringLiteral("me"), QStringLiteral("playTimeText"),
                                QStringLiteral("lastPlayedText"), QStringLiteral("variantState")},
                               QStringLiteral("userId")};
  KeyedRowsModel events_{{QStringLiteral("seq"), QStringLiteral("timeText"), QStringLiteral("fullTimeText"), QStringLiteral("author"), QStringLiteral("text"), QStringLiteral("sourceText")},
                         QStringLiteral("seq")};
  KeyedRowsModel games_{{QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("system"), QStringLiteral("hint"), QStringLiteral("sha")}, QStringLiteral("id")};
  KeyedRowsModel startPlugins_{{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("version"), QStringLiteral("description"), QStringLiteral("origin"), QStringLiteral("originText"), QStringLiteral("permissionsText"), QStringLiteral("checked")},
                               QStringLiteral("id")};
  KeyedRowsModel startIncluded_{{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("version"), QStringLiteral("text")}, QStringLiteral("id")};
  KeyedRowsModel startRules_{{QStringLiteral("rowId"), QStringLiteral("kind"), QStringLiteral("pluginId"), QStringLiteral("title"), QStringLiteral("key"), QStringLiteral("widget"),
                              QStringLiteral("label"), QStringLiteral("hint"), QStringLiteral("value"), QStringLiteral("options")},
                             QStringLiteral("rowId")};
  KeyedRowsModel startUsers_{{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("online"), QStringLiteral("checked")}, QStringLiteral("id")};

  QString activeTitle_;
  QString openRunId_;
  QVariantMap detail_;
  QString message_;
  bool messageIsError_ = false;
  bool busy_ = false;
  QString busyText_;
  QString startError_;
  RunRules startCore_;
  mutable QSet<QString> catalogTried_;  // run|plugin@version a catalog refresh was already tried for
  QMap<QString, QMap<QString, QPair<QString, QStringList>>> joinReviewed_;  // run -> plugin -> {version, permissions} the join dialog showed
  bool pluginUpdateBusy_ = false;
  QString pluginUpdateError_;
  QVariantMap pluginConsent_;
  bool rulesBusy_ = false;
  QString rulesError_;
  bool startBusy_ = false;
  bool startUsersLoading_ = false;
  QSet<QString> startPluginIds_;
  QMap<QString, QVariantMap> startRuleValues_;  // plugin id -> {setting key -> value} the creator changed
  bool startResolveError_ = false;              // startError_ comes from the requires resolution
  QSet<QString> startUserIds_;
  QList<HubUser> users_;
  bool rebuildQueued_ = false;
  bool lastAvailable_ = false;
  bool joinUnverified_ = false;
  KeyedRowsModel joinPlugins_{{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("version"), QStringLiteral("origin"), QStringLiteral("originText"), QStringLiteral("permissionsText")}, QStringLiteral("id")};
  quint64 playGen_ = 0;
  quint64 viewGen_ = 0;
  QString viewKey_;  // run id + pins of the run view being fetched or running
  bool viewFetching_ = false;
  QString runViewState_;
  QString runViewText_;
  QString runTab_ = QStringLiteral("board");
  QVariantList runTabs_;
  QVariantList startRulesOptions_;
  QString startRulesCurrent_;
};

}  // namespace uzume::ui
