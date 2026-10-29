#pragma once
// PluginsController: the plugin runtime inside the Player. Owns the PluginManager (<data>/plugins), the PluginHost and
// the memory adapter; starts the plugins that match a game when it runs, and serves Settings -> Plugins and the
// Plugin console tab of the game panel (docs/design/legacy/plugins.md 4a, 4d). Plugin failures never block or slow the game:
// they only show in the console and the Player log.
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <optional>

#include "devconsole.h"
#include "plugin_delivery.h"
#include "plugin_host.h"
#include "plugin_store.h"
#include "plugin_store_sync.h"
#include "pluginemulation.h"
#include "plugin_manager.h"
#include "run_bridge.h"
#include "pluginhub.h"
#include "pluginimageprovider.h"
#include "pluginruntime.h"

namespace uzume {
class HubConnection;
class PlayerSettings;
namespace emu {
class ManifestRegistry;
}
}

namespace uzume::ui {

class GameSession;

// A game started in a run: exactly these Hub copies run, with uz.run backed by `bridge`.
struct RunLaunch {
  std::shared_ptr<plugins::RunBridge> bridge;
  QList<plugins::RunPluginPin> pins;
};

class PluginsController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  // Developer mode (persisted in PlayerSettings, applied to the manager).
  Q_PROPERTY(bool developerMode READ developerMode WRITE setDeveloperMode NOTIFY developerModeChanged)
  // Plugin UI language (Settings > Plugins): "auto" | a tag of pluginLanguageOptions. Applies when the next game starts.
  Q_PROPERTY(QString pluginLanguage READ pluginLanguage WRITE setPluginLanguage NOTIFY pluginLanguageChanged)
  // [{value, label}]: Auto first, then English, Deutsch (plugins/plugin_language.h).
  Q_PROPERTY(QVariantList pluginLanguageOptions READ pluginLanguageOptions CONSTANT)
  // Installed plugins: [{id, name, version, origin ("official"|"unverified"|"local"), originText ("✓ Official" | "Hub upload ·
  //  unverified" | "Local file"), problemsText, hasProblems, enabled, devLinked, hubCopy, hasScreen (declares ui.screen), canToggle, needsConfirm (unverified: the
  //  warning comes before turning it on), permissionsText, updateVersion ("" when none)}]
  Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged)
  // Run view (Runs screen, outside a game): one entry per plugin of the shown run that declares ui.run or ui.screen, in start order:
  // [{pluginId, name, title (tab name), slot ("screen" | "run"), badge, origin, originText, tree ({} until the plugin sets that slot), paused, loading}].
  Q_PROPERTY(QVariantList runSections READ runSections NOTIFY runSectionsChanged)
  // Why plugins of the run view did not start ("" = fine), e.g. "Run Notes: needs x, which is not part of this run ...".
  Q_PROPERTY(QString runViewProblems READ runViewProblems NOTIFY runSectionsChanged)
  Q_PROPERTY(QString summary READ summary NOTIFY pluginsChanged)  // settings column line: "None installed" | "2 on · 1 update" | "None on"
  // Plugins the Hub offers (feature plugins_v1): the newest offered version of every plugin, [{id, name, version,
  // description, origin, originText, permissionsText, sizeText, state ("install"|"update"|"installed"|"busy"), installedVersion}].
  Q_PROPERTY(bool hubSupported READ hubSupported NOTIFY hubPluginsChanged)
  Q_PROPERTY(QVariantList hubPlugins READ hubPlugins NOTIFY hubPluginsChanged)
  Q_PROPERTY(QString hubStatus READ hubStatus NOTIFY hubPluginsChanged)  // idle | loading | ready | failed
  Q_PROPERTY(QString hubError READ hubError NOTIFY hubPluginsChanged)
  Q_PROPERTY(QString message READ message NOTIFY messageChanged)  // result of the last install / link
  Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)
  // Plugin console (in-game tab): only while developer mode is on and plugins run for the game.
  Q_PROPERTY(bool consoleAvailable READ consoleAvailable NOTIFY consoleAvailableChanged)
  // {plugins: [{id, name, status, statusText}], selected, status, statusText, error, watched, events, log, regions,
  //  regionsText}; refreshed at most about 10 times per second.
  Q_PROPERTY(QVariantMap console READ console NOTIFY consoleChanged)
  // The developer console (0.22, boards 4d-1 and 4d-2): the one model behind the Settings page, the game panel tab and the debug
  // MCP tools. `console` above is its model.
  Q_PROPERTY(uzume::ui::DevConsole* devConsole READ devConsole CONSTANT)
  // Result of the last "Load unpacked...": {state ("ok" | "error"), title ("Item Helper didn't load"), detail ("manifest.json: ..."),
  //  path}; empty before the first attempt or after dismissUnpackedResult(). An error installs nothing (board 4d-2B).
  Q_PROPERTY(QVariantMap unpackedResult READ unpackedResult NOTIFY unpackedResultChanged)

  // Plugin UI v1 (uz.ui). Models are rebuilt on the UI thread when the plugin thread delivers changes (coalesced to
  // about 30 updates per second and slot); the trees themselves are fetched with slotTree() on slotChanged().
  // [{pluginId, name, title, icon, origin, dot, paused}]: in-game panel tabs of the running plugins that declare ui.panel.
  Q_PROPERTY(QVariantList panelTabs READ panelTabs NOTIFY panelTabsChanged)
  // [{pluginId, name, icon, corner, hidden, tree, origin}]: HUD widgets (ui.hud) that have a tree; corner = top-left |
  // bottom-left | bottom-right (default bottom-left; the top-right corner belongs to the diagnostics overlay).
  Q_PROPERTY(QVariantList hudWidgets READ hudWidgets NOTIFY hudWidgetsChanged)
  // "Hide all HUD widgets" (persisted). The F4 / Ctrl+H keys that toggle it are bound elsewhere.
  Q_PROPERTY(bool hudAllHidden READ hudAllHidden WRITE setHudAllHidden NOTIFY hudAllHiddenChanged)
  // [{toastId, pluginId, pluginName, tone, title, text, actions: [{label, action, value}], origin, host}], oldest first, at
  // most 10 (the UI shows the newest 3 and a pill for the rest). host = true: a notice of the Player itself ("Uzume
  // plugin paused", toastId < 0, actions "restart" / "details").
  Q_PROPERTY(QVariantList toasts READ toasts NOTIFY toastsChanged)
  // [{pluginId, name, title, icon, badge, origin}]: the plugins running in the shell (or game) that declare ui.screen; no sidebar rows,
  // the entry feeds the plugin screen sheet and the badge of the Settings > Plugins row.
  Q_PROPERTY(QVariantList sidebarEntries READ sidebarEntries NOTIFY sidebarEntriesChanged)
  // [{pluginId, name, tree}]: sections of the game detail (ui.detail) that have a tree.
  Q_PROPERTY(QVariantList detailSections READ detailSections NOTIFY detailSectionsChanged)
  // [{pluginId, id, label, key}]: hotkeys declared by the running plugins.
  Q_PROPERTY(QVariantList hotkeys READ hotkeys NOTIFY hotkeysChanged)
  // Controllers > Hotkeys: one group per enabled plugin that declares hotkeys (list order):
  // [{pluginId, name, origin, originText, count, conflicts, rows: [{pluginId, id, label, key, keyText, set, active, changed,
  // listening, note}]}]. key = effective key ("" = unassigned); keyText = what the field shows ("Ctrl+H", "Ctrl+H · not
  // active", "not set"); note = "Already used by <owner>. Unassigned until you choose." on a conflict row.
  Q_PROPERTY(QVariantList hotkeyGroups READ hotkeyGroups NOTIFY hotkeysChanged)
  Q_PROPERTY(QString hotkeyListening READ hotkeyListening NOTIFY hotkeysChanged)  // "<pluginId>/<hotkeyId>" waiting for a key
  Q_PROPERTY(QString hotkeyNote READ hotkeyNote NOTIFY hotkeysChanged)            // "Already used by <owner>" after a rejected key
  // 0.20 plugin API emulation: the open consent question (cheats / memory writes, see PluginEmulationService) and the number of
  // cheats turned on (game header "N cheats on").
  Q_PROPERTY(QVariantMap consentRequest READ consentRequest NOTIFY consentRequestChanged)
  Q_PROPERTY(int cheatsOn READ cheatsOn NOTIFY cheatsOnChanged)
 public:
  // dataDir = ProfileStore::baseDir(); the plugins live in <dataDir>/plugins.
  // `connection` = the active Hub (plugin list and downloads); nullptr = no Hub features (tests).
  PluginsController(PlayerSettings* settings, const QString& dataDir, GameSession* session, HubConnection* connection = nullptr,
                    QObject* parent = nullptr);
  ~PluginsController() override;

  // System manifests (the game identity reader is the manifest's "identity"); without them no game code is read.
  void setManifests(const emu::ManifestRegistry* manifests) { manifests_ = manifests; }

  bool developerMode() const;
  void setDeveloperMode(bool on);
  QString pluginLanguage() const;
  void setPluginLanguage(const QString& tag);
  QVariantList pluginLanguageOptions() const;
  // The tag plugins get (GameContext.language) for a game of `systemId` started with these explicit core option
  // overrides; both empty = outside a game (Auto then is the OS language).
  QString resolveLanguage(const QString& systemId = QString(), const QMap<QString, QString>& coreOptions = {}) const;
  QVariantList plugins() const { return plugins_; }
  QString summary() const { return summary_; }
  QString message() const { return message_; }
  bool messageIsError() const { return messageIsError_; }
  bool consoleAvailable() const { return consoleAvailable_; }
  QVariantMap console() const { return devConsole_->model(); }
  DevConsole* devConsole() { return devConsole_.get(); }
  QVariantMap unpackedResult() const { return unpackedResult_; }

  QVariantList panelTabs() const { return panelTabs_; }
  QVariantList hudWidgets() const { return hudWidgets_; }
  bool hudAllHidden() const { return hudAllHidden_; }
  void setHudAllHidden(bool hidden);
  QVariantList toasts() const { return toasts_; }
  QVariantList toastLog() const { return toastLog_; }  // the last 20 toasts, oldest first (debug MCP toasts_list)
  QVariantList sidebarEntries() const { return sidebarEntries_; }
  QVariantList detailSections() const { return detailSections_; }
  QVariantList hotkeys() const { return hotkeys_; }
  QVariantList hotkeyGroups() const { return hotkeyGroups_; }
  QString hotkeyListening() const { return hotkeyListening_; }
  QString hotkeyNote() const { return hotkeyNote_; }
  // Key combinations (Qt::Key | modifiers) of the plugin hotkeys that fire now (running, not in conflict).
  QSet<int> activeHotkeyCombos() const { return activeCombos_; }
  // Uzume keys that always win: combination (Qt::Key | modifiers) -> label of the owner. Set by the PlayerController.
  void setReservedKeys(const QMap<int, QString>& reserved);
  // Registry behind image://uzplugin (see pluginimageprovider.h).
  std::shared_ptr<PluginImageSources> imageSources() const { return imageSources_; }
  // Plugins running in the shell context (no game), in start order.
  QStringList shellPluginIds() const { return shellIds_; }
  bool shellRunning() const { return shellRunning_; }
  // Run view: while the Runs screen shows a run (and no game runs) the plugins of the run that declare ui.run or ui.screen run next to the
  // shell set, as the exact pinned Hub copies (resolveForRun), in the shell context with a read-only uz.run bound to
  // `run.bridge` (current().view == true; writes are refused). Calling it again for the same run and pins keeps the running
  // plugins; another run replaces them. A plugin that is in the shell set too runs once, as the run view instance.
  // gameStarted() ends the run view (inGameChanged() tells the Runs screen to start it again after the game).
  void startRunView(const RunLaunch& run, const QString& runId);
  void stopRunView();
  QString runViewId() const { return runViewId_; }
  QStringList runViewPluginIds() const { return runViewIds_; }
  QVariantList runSections() const { return runSections_; }
  QString runViewProblems() const { return runViewProblems_; }
  bool inGame() const { return inGame_; }
  // run_plugin_updates_v1: the run behind the running game got other pinned versions (run_updated). Fetches the new Hub copies and restarts the
  // game's plugins once; the same pins restart nothing. When the gate (setRunPinsGate: false = the person has not agreed to the new
  // permissions yet) says no, the run's plugins stay stopped for this game and a notice says so.
  void runPinsChanged(const QList<plugins::RunPluginPin>& pins);
  // The one gate at the start boundary: every start of run plugins (game start, restart, rule change, run view) asks it with the exact pins it is about
  // to start; false = the person has not accepted their permissions (the gate raises the consent request when `raise`), nothing starts.
  using RunPinsGate = std::function<bool(const QString& runId, const QList<plugins::RunPluginPin>& pins, bool raise)>;
  void setRunPinsGate(RunPinsGate gate) { runPinsGate_ = std::move(gate); }
  // Starts the game's run plugins again after the person accepted (no-op when they run already).
  void retryRunSet() { refreshRunExtras(); }
  // Counter that every game start, game end and pin change advances; pending pin downloads of an older value are dropped.
  quint64 pinsGeneration() const { return pinsGen_; }
  // Local consent to the permissions of one plugin version in one run ("" = never asked). Stored per server and user under <data>/runs/<hub>/<user>/plugin_consent/; without a server or user nothing is read or stored.
  // Accepted-permissions baseline per run and plugin id: what this person actually agreed to (recorded at join and on each consent).
  // nullopt = nothing recorded. Legacy files (versions only) are migrated on write; runLegacyVersions reads them.
  std::optional<QStringList> runAcceptedPermissions(const QString& runId, const QString& pluginId) const;
  QStringList runLegacyVersions(const QString& runId, const QString& pluginId) const;
  void acceptRunPermissions(const QString& runId, const QString& pluginId, const QStringList& perms);
  // The same for an explicit server and user (the ones above use the connected account); consent never crosses servers or users.
  std::optional<QStringList> runAcceptedPermissionsFor(const QString& hubId, const QString& userId, const QString& runId, const QString& pluginId) const;
  void acceptRunPermissionsFor(const QString& hubId, const QString& userId, const QString& runId, const QString& pluginId, const QStringList& perms);
  // Permission labels as the lists show them ("Share run progress · ..."), for a list of permission ids.
  static QString permissionLabels(const QStringList& perms);

  plugins::PluginManager* manager() { return manager_.get(); }
  plugins::PluginHost* host() { return host_.get(); }
  // Hub delivery: ensureHubCopy(id, version, sha256, cb) gives the runs the exact Hub copy of a plugin version.
  plugins::PluginDelivery* delivery() { return delivery_.get(); }
  PluginHubClient* hubClient() { return hubClient_.get(); }
  const PluginHubClient* hubClient() const { return hubClient_.get(); }
  bool hubSupported() const { return hubClient_ != nullptr && hubClient_->supported(); }
  // The Hub offer of one plugin version as a row {id, name, version, origin, originText, permissionsText}; empty when the Hub does not list it.
  QVariantMap hubOffer(const QString& pluginId, const QString& version) const;
  QVariantList hubPlugins() const { return hubPlugins_; }
  QString hubStatus() const;
  QString hubError() const { return hubClient_ != nullptr ? hubClient_->errorMessage() : QString(); }

  // 0.20 plugin API emulation (uz.memory.write, uz.cheats, uz.save.snapshot). The PlayerController wires the snapshot function,
  // the cheats-used marker and the connection once, and sets the game info before gameStarted().
  PluginEmulationService* emulation() { return emulation_.get(); }
  void setSoundSink(std::shared_ptr<plugins::SoundSink> sink) { sound_ = std::move(sink); }  // uz.sound (0.23)
  void setGameInfo(const PluginEmulationService::GameInfo& info) { gameInfo_ = info; }
  QVariantMap consentRequest() const { return emulation_->consentRequest(); }
  int cheatsOn() const { return emulation_->cheatsOn(); }
  Q_INVOKABLE void answerConsent(bool allow) { emulation_->answerConsent(allow); }
  // uz.ui.pick_file: the file dialog answered (path or file:// URL) or was cancelled (empty path).
  Q_INVOKABLE void completePick(const QString& pluginId, const QString& requestId, const QString& pathOrUrl);
  // uz.store: the local copy of this person's plugin data (per server and user); null before the first start.
  std::shared_ptr<plugins::PluginStore> store() {
    ensureStore();
    return store_;
  }
  plugins::PluginStoreSync* storeSync() { return storeSync_.get(); }

  // A game runs (GameSession::started): resolves the plugins for it and starts them. `language` = the resolved plugin
  // language (resolveLanguage); empty = resolved here from the setting without core options.
  void gameStarted(const QString& system, const QString& romPath, const QString& sha256, const QString& title,
                   const QString& language = QString(), const RunLaunch& run = {});
  // The game ended or failed: stops all plugins.
  void gameStopped();

  // Uzume 0.21 (uz.variants, permission variants.build): the app side of the game variants. Set once; the shell plugins restart so a
  // plugin that registers its presets there sees it. launchContext = library id of the game about to start and the 3DS mod folder
  // of a started variant (uz.variants.list, the uz.rom RomFS overlay); call it before gameStarted().
  void setVariantsProvider(std::shared_ptr<plugins::VariantsProvider> provider);
  void setLaunchContext(const QString& gameId, const QString& modDir) {
    launchGameId_ = gameId;
    launchModDir_ = modDir;
  }
  bool pluginsRunning() const { return running_; }

  // Settings -> Plugins. Paths may be file:// URLs (file or folder dialog).
  Q_INVOKABLE void installFromFolder(const QString& pathOrUrl);
  // A .uzplugin file or a plugin folder, whichever the path is. Signed (valid Uzume key) = Official, otherwise a local
  // plugin that needs Developer mode.
  Q_INVOKABLE void installFromPath(const QString& pathOrUrl);
  // Same, but returns the result (debug MCP server).
  plugins::InstallResult installFromPathResult(const QString& pathOrUrl);
  // Asks the Hub for its plugin list (no-op without plugins_v1).
  Q_INVOKABLE void refreshHubPlugins();
  // Downloads, checks (SHA-256 pin, zip rules, signature) and installs an offered version; it stays off until turned on.
  Q_INVOKABLE void installHubPlugin(const QString& id, const QString& version);
  // Installs the missing required plugins the Hub offers first, then the plugin itself ("Install both").
  Q_INVOKABLE void installHubPluginWithDependencies(const QString& id, const QString& version);
  Q_INVOKABLE void linkDevFolder(const QString& pathOrUrl);
  // Drops only the link of a linked developer folder (the folder is kept); the installed or server copy of the id is active afterwards.
  // Does nothing for a plugin that is not dev-linked.
  Q_INVOKABLE void unlinkDevFolder(const QString& id);
  // "Load unpacked...": checks manifest.json of the folder, links it as a development plugin and turns it on; on a manifest problem
  // nothing is installed and unpackedResult says why. Returns the result (debug MCP).
  Q_INVOKABLE QVariantMap loadUnpacked(const QString& pathOrUrl);
  Q_INVOKABLE void dismissUnpackedResult();
  // Developer mode only: the game identity {system, code} that plugin matching uses for the next game start instead of the detected
  // one (debug MCP game_start identity). Refused (false) with developer mode off; single use, expires after 30 s.
  bool setIdentityOverride(const QString& system, const QString& code);
  void clearIdentityOverride();
  QVariantMap identityOverride() const;
  // Open uz.ui.pick_file requests as [{pluginId, requestId}], oldest first (debug MCP: file_dialog_pick).
  Q_INVOKABLE QVariantList pendingPicks() const;
  Q_INVOKABLE void uninstall(const QString& id);
  Q_INVOKABLE void setPluginEnabled(const QString& id, bool enabled);
  Q_INVOKABLE void dismissMessage();
  // Console tab.
  Q_INVOKABLE void selectConsolePlugin(const QString& id);
  Q_INVOKABLE void reloadPlugin(const QString& id);   // "↻ Reload" and "Restart plugin"
  Q_INVOKABLE void reloadSelected();                   // Ctrl+Shift+R
  Q_INVOKABLE void clearLog(const QString& id);
  Q_INVOKABLE void copyLog(const QString& id);

  // Plugin UI.
  Q_INVOKABLE QVariantMap slotTree(const QString& pluginId, const QString& slot) const;
  Q_INVOKABLE QVariantList tileStrips(const QString& tileId) const;  // [{pluginId, name, icon, origin, tree}]
  // Forwards a UI action (button, toggle, select, list row, map marker, toast action slot "toast", hotkey slot "hotkey")
  // to the plugin's uz.ui.on_action handler.
  Q_INVOKABLE void sendAction(const QString& pluginId, const QString& slot, const QString& action, const QVariant& value = QVariant());
  // The game detail pane shows this game (empty system = none): clears the "detail" slots and sends fb/detail.game.
  Q_INVOKABLE void setDetailGame(const QString& system, const QString& code, const QString& sha256, const QString& title);
  // Game detail "Plugins · n on" row: [{id, name, origin}] of the enabled, usable plugins that match this game; gameCode reads
  // the game code from a cached ROM (empty when the ROM is not there).
  Q_INVOKABLE QVariantList pluginsForGame(const QString& system, const QString& code) const;
  Q_INVOKABLE QString gameCode(const QString& system, const QString& romPath) const;
  // Visible remote Multiview tiles [{id, player, game}] -> fb/tiles.
  Q_INVOKABLE void setTiles(const QVariantList& tiles);
  Q_INVOKABLE void setHudCorner(const QString& pluginId, const QString& corner);
  Q_INVOKABLE void hideHud(const QString& pluginId);
  Q_INVOKABLE void showHud(const QString& pluginId);
  Q_INVOKABLE void dismissToast(int toastId);
  // mode: "session" (until the game ends), "always" and "important" (only important notices pass; both persisted) or "off".
  Q_INVOKABLE void muteToasts(const QString& pluginId, const QString& mode);
  Q_INVOKABLE QString toastMuteMode(const QString& pluginId) const;  // "" | "session" | "always" | "important"
  // Small in-game marker for non-official plugins: "" (official), "Unverified" or "Local".
  Q_INVOKABLE QString originMarker(const QString& origin) const;
  Q_INVOKABLE void triggerHotkey(const QString& pluginId, const QString& hotkeyId);
  // A key press in the game: runs the active plugin hotkey with this key (action slot "hotkey"); false = none.
  // Key text of the plugin's hotkey when it is assigned and not in conflict, else empty.
  Q_INVOKABLE QString activeHotkeyKey(const QString& pluginId, const QString& hotkeyId) const;
  Q_INVOKABLE bool triggerHotkeyKey(int qtKey, int modifiers);
  // Controllers > Hotkeys of the plugin groups (keyboard only). The captured key replaces the declared one (persisted).
  Q_INVOKABLE void beginHotkeyCapture(const QString& pluginId, const QString& hotkeyId);
  Q_INVOKABLE void cancelHotkeyCapture();
  Q_INVOKABLE bool captureHotkey(int qtKey, int modifiers);  // true = the key was consumed (capture active)
  Q_INVOKABLE void clearHotkey(const QString& pluginId, const QString& hotkeyId);  // unassigned
  Q_INVOKABLE void resetHotkey(const QString& pluginId, const QString& hotkeyId);  // back to the declared key

  // Tests and screenshots: shows `console` in the Plugin console without a running game; empty map removes it.
  void setPreviewConsole(const QVariantMap& console);
  // Tests and screenshots: replaces the plugin UI models without running a plugin. Keys: panelTabs, hudWidgets, toasts,
  // sidebarEntries (lists, as the properties), trees ({"<pluginId>|<slot>": tree}), tileStrips ({tileId: [{pluginId,
  // tree}]}). An empty map leaves the preview. Actions are still reported by actionSent().
  void setPreviewUi(const QVariantMap& ui);

 signals:
  // A plugin was paused by an error, the watchdog or a quota (first line of the Lua error / budget message); ADR 0035 error events.
  void pluginFailed(const QString& id, const QString& error);
  void developerModeChanged();
  void pluginLanguageChanged();
  void pluginsChanged();
  void hubPluginsChanged();
  void messageChanged();
  void consoleAvailableChanged();
  void consoleChanged();
  void panelTabsChanged();
  void hudWidgetsChanged();
  void toastsChanged();
  void sidebarEntriesChanged();
  void detailSectionsChanged();
  void runSectionsChanged();
  // A game started or ended (plugins or not).
  void inGameChanged();
  void hotkeysChanged();
  void activeHotkeyCombosChanged();
  void tileStripsChanged();
  void hudAllHiddenChanged();
  // reloadPlugin() was called ("Restart plugin"); tests observe it.
  void reloadRequested(const QString& pluginId);
  // setTiles() with a changed list (tests observe the Multiview -> plugins hand-over).
  void tilesSet(const QVariantList& tiles);
  // Every sendAction() call (also without a running plugin): tests observe the UI -> plugin round trip with it.
  void actionSent(const QString& pluginId, const QString& slot, const QString& action, const QVariant& value);
  // The tree of a slot was set, patched or cleared (fetch it with slotTree()).
  void slotChanged(const QString& pluginId, const QString& slot);
  // uz.ui.open(slot) of a running plugin ("screen" or "panel"); the game view and Main.qml decide what to show.
  void openRequested(const QString& pluginId, const QString& slot);
  // uz.ui.close(slot) of a running plugin: close the sheet or panel tab uz.ui.open showed.
  void closeRequested(const QString& pluginId, const QString& slot);
  // uz.ui.pick_file of a running plugin: show a file dialog (title, filters like "*.cht", max_bytes) and answer with completePick().
  void pickFileRequested(const QString& pluginId, const QString& requestId, const QString& title, const QStringList& filters, double maxBytes);
  // The request was answered (a file, or cancel): dialogs that are still open for it close (also when the MCP tool answered it).
  void pickCompleted(const QString& pluginId, const QString& requestId);
  void consentRequestChanged();
  void cheatsOnChanged();
  void unpackedResultChanged();

 private:
  void updateShell();
  void stopGame(bool restartShell);
  plugins::GameResolution resolveGameSet() const;
  bool startGameSet(const plugins::GameResolution& res);  // false: the consent gate stopped it
  void stopGameSet();
  void refreshRunExtras();
  void restartRunSet();
  void logToast(const QVariantMap& t);
  void postInitialEvents();
  void onUiChanged(const QString& id, const QString& what);
  void rebuildUiModels();
  void rebuildPanelTabs();
  void rebuildHud();
  void rebuildSidebar();
  void rebuildDetail();
  void rebuildRunSections();
  std::optional<plugins::PluginInfo> infoFor(const QString& id) const;  // the run view instance first, then the installed plugin
  void rebuildHotkeys();
  QVariantList declaredHotkeyPlugins() const;  // [{pluginId, name, origin, hotkeys: [{id, label, key}]}]
  void setHotkeyOverride(const QString& pluginId, const QString& hotkeyId, const QString& key);
  void collectToasts();
  void addAppNotice(const PluginEmulationService::Notice& n);
  void ensureStore();
  void addHostNotice(const QString& pluginId);
  void addReloadNotice(const QString& pluginId, const QString& name, qint64 ms);
  void applyPreviewHud();
  QString pluginIconUrl(const QString& id) const;
  QStringList activeIds() const;
  void updateImageSources();
  void refreshPlugins();
  void rebuildHubPlugins();
  void onDelivered(const plugins::DeliveryResult& r);
  void reportInstall(const plugins::InstallResult& r, bool fromFolder);
  const HubPluginInfo* newestOffer(const QString& id) const;
  void scheduleConsole();
  QStringList consoleIds() const;     // plugins the developer console lists: the game's set, else the shell's
  QVariantMap inspectOf(const QString& pluginId) const;
  void onStatus(const QString& id);
  void setMessage(const QString& text, bool isError);
  void setConsoleAvailable(bool on);
  static QString localPath(const QString& pathOrUrl);

  PlayerSettings* settings_;
  GameSession* session_;
  const emu::ManifestRegistry* manifests_ = nullptr;
  QString identityReader(const QString& system) const;
  // Declared before host_: the plugin thread holds these until host_->stop() has returned.
  std::shared_ptr<PluginEmulationService> emulation_;
  std::shared_ptr<plugins::PluginStore> store_;
  std::shared_ptr<plugins::SoundSink> sound_;
  std::unique_ptr<plugins::PluginStoreSync> storeSync_;
  QString storeKey_;
  QList<QPair<QString, QString>> pendingPicks_;  // (plugin id, request id) asked and not answered yet, oldest first
  QHash<QString, qint64> pickLimits_;  // "<plugin>/<request id>" -> max_bytes
  QString dataDir_;
  RunPinsGate runPinsGate_;
  quint64 pinsGen_ = 0;
  HubConnection* conn_ = nullptr;
  PluginEmulationService::GameInfo gameInfo_;
  std::unique_ptr<plugins::PluginManager> manager_;
  std::unique_ptr<plugins::PluginHost> host_;
  std::unique_ptr<PluginHubClient> hubClient_;
  std::unique_ptr<plugins::PluginDelivery> delivery_;
  QVariantList hubPlugins_;
  QString hubBusyId_;
  QList<QPair<QString, QString>> installQueue_;  // (id, version) still to install after the current one
  std::shared_ptr<MemoryWatchAdapter> adapter_;
  std::shared_ptr<plugins::VariantsProvider> variants_;  // uz.variants of every context (shell and game)
  plugins::GameContext gameCtx_;  // of the running game (restarts when the rules of a run change)
  QString gameSystem_, gameCode_;
  RunLaunch runLaunch_;
  QString launchGameId_;
  QString launchModDir_;
  std::shared_ptr<VideoCaptureAdapter> video_;  // uz.video of the running game; released on the GUI thread after host_->stop()
  std::unique_ptr<DevConsole> devConsole_;
  QVariantMap unpackedResult_;
  struct IdentityOverride {
    QString system, code;
    qint64 setAtMs = 0;
  };
  std::optional<IdentityOverride> identityOverride_;
  std::shared_ptr<plugins::RunBridge> consoleRun_;  // uz.run bridge of the running game (Inspect: run state)
  bool running_ = false;                      // plugins of the running game are started
  bool inGame_ = false;                       // a game runs (with or without plugins)
  bool shellRunning_ = false;
  QStringList shellIds_;
  QString shellSignature_;
  // Run view (see startRunView).
  bool runViewActive_ = false;
  QString runViewId_;
  QString runViewKey_;                                  // run id + pins: the same key keeps the running plugins
  QList<plugins::RunPluginPin> runViewPins_;
  std::shared_ptr<plugins::RunBridge> runViewBridge_;   // read-only wrapper
  QStringList runViewIds_;                              // started for the run view, start order
  QMap<QString, plugins::PluginInfo> runViewInfos_;
  QString runViewProblems_;
  QVariantList runSections_;
  QString lastRunProblems_;
  QString language_ = QStringLiteral("en");
  bool hudAllHidden_ = false;
  QSet<QString> mutedImportant_;
  int hostToastId_ = 0;                       // host notices count down from -1
  bool previewUi_ = false;
  QVariantMap previewUiData_;
  QVariantList toastLog_;
  QVariantList panelTabs_, hudWidgets_, toasts_, sidebarEntries_, detailSections_, hotkeys_, hotkeyGroups_;
  QSet<int> activeCombos_;
  QHash<int, QStringList> takenBy_;          // combo -> ["<pluginId>/<hotkeyId>", plugin name] (no conflict)
  QHash<int, QStringList> comboTarget_;      // combo -> [pluginId, hotkeyId] of the running plugins
  QMap<int, QString> reservedKeys_;
  QHash<QString, QString> hotkeyOverrides_;   // "<pluginId>/<hotkeyId>" -> key | "none"
  QString hotkeyListening_, hotkeyNote_;
  QHash<QString, QString> hudCorner_;
  QSet<QString> hudHidden_;
  QSet<QString> mutedSession_, mutedAlways_;
  QVariantMap detailGame_;                    // empty = none
  QVariantList tiles_;
  std::shared_ptr<PluginImageSources> imageSources_ = std::make_shared<PluginImageSources>();
  bool consoleAvailable_ = false;
  QStringList runningIds_;                    // start order of the running plugins
  QString selected_;
  QHash<QString, qint64> clearedAt_;          // plugin id -> log lines up to this time are hidden
  QHash<QString, int> loggedStatus_;          // plugin id -> last status written to the Player log
  QVariantList plugins_;
  QString summary_;
  QString message_;
  bool messageIsError_ = false;
  bool hasPreview_ = false;
};

}  // namespace uzume::ui
