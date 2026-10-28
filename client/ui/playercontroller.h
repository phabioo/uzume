#pragma once
// PlayerController: connects HubConnection, HubLibrary, RomDownloader, emulation and GameSession to the
// QML UI. The QML screens only read properties (screen, hubs, pairing, selectedGame, ...) and
// call actions; hub logic stays in network/ and core/.

#include <QElapsedTimer>
#include <QEvent>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <memory>

#include "audiocontroller.h"
#include "audiomixer.h"
#include "pluginsoundbus.h"
#include "connectioncheckcontroller.h"
#include "controllerscontroller.h"
#include "core_locator.h"
#include "emulationcontroller.h"
#include "firmwarecache.h"
#include "corecache.h"
#include "coreprovisioner.h"
#include "coreupdater.h"
#include "firmwareprovisioner.h"
#include "corecatalog.h"
#include "gamedetail.h"
#include "gamesession.h"
#include "gamestarter.h"
#include "gameuploader.h"
#include "uploadcontroller.h"
#include "localromindex.h"
#include "romregistrar.h"
#include "romsource.h"
#include "hubconnection.h"
#include "hublibrary.h"
#include "hubpresenter.h"
#include "hubsystems.h"
#include "librarymodel.h"
#include "localstoragecontroller.h"
#include "playphase.h"
#include "pluginscontroller.h"
#include "personalcontroller.h"
#include "screenshotcontroller.h"
#include "runscontroller.h"
#include "variantscontroller.h"
#include "playersettings.h"
#include "romcache.h"
#include "stateapi.h"
#include "statescontroller.h"
#include "romdownloader.h"
#include "savehistorycontroller.h"
#include "savesync.h"
#include "sessioncontroller.h"
#include "system_manifest.h"
#include "localhubcontroller.h"
#include "onboardingcontroller.h"
#include "accountscontroller.h"
#include "updatescontroller.h"
#include "admincontroller.h"
#include "reportcontroller.h"
#include "errorscontroller.h"
#include <QSet>
#include "mcpcontroller.h"
#include "discordpresence.h"
#include "notificationcenter.h"
#include "chatcontroller.h"
#include "controlcontroller.h"
#include "togethercontroller.h"
#include "voicecontroller.h"

namespace uzume::ui {

class PlayerController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Created in main.cpp")

  // Navigation: "start" (first start, join, set up, add a person) | "connection" | "pairing" | "library" | "game" | "settings" | "runs" | "server" (Emulation, Controls and Hotkeys are sections of "settings": showEmulation(), showControllers() and showSettingsSection())
  Q_PROPERTY(QString screen READ screen NOTIFY screenChanged)

  // 3a
  Q_PROPERTY(QVariantList hubs READ hubs NOTIFY hubsChanged)
  Q_PROPERTY(QString connectionNotice READ connectionNotice NOTIFY hubsChanged)
  Q_PROPERTY(bool autoConnect READ autoConnect WRITE setAutoConnect NOTIFY autoConnectChanged)
  Q_PROPERTY(QString deviceFooter READ deviceFooter CONSTANT)
  // 3b
  Q_PROPERTY(QVariantMap pairing READ pairing NOTIFY pairingChanged)
  // 3c
  Q_PROPERTY(QString hubName READ hubName NOTIFY hubChanged)
  Q_PROPERTY(QString hubAddress READ hubAddress NOTIFY hubChanged)
  // Link to the Hub: online | connecting | offline (handshake state plus the Sessions socket when the Hub offers Sessions).
  Q_PROPERTY(QString hubLink READ hubLink NOTIFY hubLinkChanged)
  Q_PROPERTY(uzume::ui::LibraryModel* library READ library CONSTANT)
  Q_PROPERTY(QString libraryState READ libraryState NOTIFY libraryStateChanged)  // loading | ready | error
  Q_PROPERTY(QString libraryError READ libraryError NOTIFY libraryStateChanged)
  Q_PROPERTY(QString selectedGameId READ selectedGameId NOTIFY selectedGameChanged)
  Q_PROPERTY(QVariantMap selectedGame READ selectedGame NOTIFY selectedGameChanged)
  // Game paused and kept loaded while the Library is shown: {id, title}; empty map when there is none (0.7.x).
  Q_PROPERTY(QVariantMap backgroundGame READ backgroundGame NOTIFY backgroundGameChanged)
  Q_PROPERTY(QString runningGameId READ runningGameId NOTIFY selectedGameChanged)  // library id of the running game, "" = none
  // "Quit game" was asked for and is running ("Saving and syncing…" in the game panel and the Now running strip).
  Q_PROPERTY(bool quitting READ quitting NOTIFY quittingChanged)
  // Confirmation "Quit {running} and start {new}?": {active, runningTitle, newTitle}
  Q_PROPERTY(QVariantMap startConfirm READ startConfirm NOTIFY startConfirmChanged)
  // Save sync (3d): conflict dialog data; empty map = no dialog
  Q_PROPERTY(QVariantMap saveConflict READ saveConflict NOTIFY saveConflictChanged)
  Q_PROPERTY(QString saveNote READ saveNote NOTIFY hubChanged)
  // Game view
  Q_PROPERTY(uzume::ui::GameSession* gameSession READ gameSession CONSTANT)
  // Appearance (Settings page), upload, core warnings
  Q_PROPERTY(QString appearance READ appearance WRITE setAppearance NOTIFY appearanceChanged)  // dark | light | system
  // Text size "100" | "125" | "tv" and Reduce motion (effective value; follows the system until the user changes it).
  Q_PROPERTY(QString textSize READ textSize WRITE setTextSize NOTIFY textSizeChanged)
  Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY reduceMotionChanged)
  Q_PROPERTY(bool discordPresence READ discordPresence WRITE setDiscordPresence NOTIFY discordPresenceChanged)  // Discord Rich Presence, off by default
  // ROM cache (Settings): limitBytes (0 = unlimited), limitText, usedBytes, usedText, clearableBytes, clearableText, files,
  // limitOptions [{value (bytes), label}].
  Q_PROPERTY(QVariantMap romCache READ romCache NOTIFY romCacheChanged)
  Q_PROPERTY(bool darkMode READ darkMode NOTIFY appearanceChanged)  // effective palette (System resolved)
  Q_PROPERTY(bool canUpload READ canUpload NOTIFY hubChanged)       // handshake feature uploads_v1
  // The final save write at "stop game" failed (disk full, file locked): shown as a banner in the Library until dismissed.
  Q_PROPERTY(QString stopSaveError READ stopSaveError NOTIFY stopSaveErrorChanged)
  Q_PROPERTY(QVariantMap upload READ upload NOTIFY uploadChanged)   // active, fileName, progress, message, isError
  Q_PROPERTY(QStringList uploadFilters READ uploadFilters CONSTANT)
  // Local ROMs (0.12): library folders of this device and the state of the folder scan: {active, done, total, text, isError}.
  Q_PROPERTY(QStringList libraryFolders READ libraryFolders NOTIFY libraryFoldersChanged)
  Q_PROPERTY(QVariantMap libraryScan READ libraryScan NOTIFY libraryScanChanged)
  // Library view "list" | "grid" (saved per device) and the last "Remove from library" ({title} while its Undo is offered, else empty).
  Q_PROPERTY(QString libraryView READ libraryView WRITE setLibraryView NOTIFY libraryViewChanged)
  Q_PROPERTY(QVariantMap removedFromLibrary READ removedFromLibrary NOTIFY removedFromLibraryChanged)
  // The game page (usability pass 5): the Library column shows the selected game instead of the list. The screen stays "library".
  // gamePageTab: "overview" | "saves" | "settings" | "manage" (the tab to show when the page opens or is asked for).
  Q_PROPERTY(bool gamePageOpen READ gamePageOpen NOTIFY gamePageChanged)
  Q_PROPERTY(QString gamePageTab READ gamePageTab NOTIFY gamePageChanged)
  // One row per library folder (Settings): [{path, exists, count (game files found in it), scannedText ("5 min ago", "" = not
  // scanned yet), scannedFull, scanning}] and the number of game files over all folders.
  Q_PROPERTY(QVariantList libraryFolderInfo READ libraryFolderInfo NOTIFY libraryFolderInfoChanged)
  Q_PROPERTY(int libraryGameCount READ libraryGameCount NOTIFY libraryFolderInfoChanged)
  Q_PROPERTY(bool canRegisterLocalRoms READ canRegisterLocalRoms NOTIFY hubChanged)  // handshake feature local_roms_v1
  // Background core updates (quiet indicator on the Emulation page): active, text ("Updating X to V"), lastText ("Updated X to V")
  Q_PROPERTY(QVariantMap coreUpdate READ coreUpdate NOTIFY coreUpdateChanged)
  Q_PROPERTY(QVariantList coreWarnings READ coreWarnings NOTIFY coreWarningsChanged)  // core_missing / core_version_mismatch
  // Emulation page (3e), Controllers page (3f), applied settings
  Q_PROPERTY(uzume::ui::EmulationController* emulation READ emulation CONSTANT)
  Q_PROPERTY(uzume::ui::ControllersController* controllers READ controllers CONSTANT)
  Q_PROPERTY(uzume::ui::AudioController* audio READ audio CONSTANT)
  Q_PROPERTY(bool fullscreenOnStart READ fullscreenOnStart NOTIFY emulationSettingsChanged)  // effective Uzume option
  // Sessions (3c list, 3g panel, multiview, diagnostics)
  Q_PROPERTY(uzume::ui::SessionController* sessions READ sessions CONSTANT)
  // Updates (Settings section + banner)
  Q_PROPERTY(uzume::ui::UpdatesController* updates READ updates CONSTANT)
  // "Report a problem" dialog (Settings > Diagnostics, game "⋯" menu)
  Q_PROPERTY(uzume::ui::ReportController* report READ report CONSTANT)
  // Error events and the opt-in "Send errors to the server automatically", the GitHub lookup of Server > Reports > Errors (ADR 0035)
  Q_PROPERTY(uzume::ui::ErrorsController* errors READ errors CONSTANT)
  // Admin area (0.18 package 8): generic admin requests, "Confirm it's you", server status
  Q_PROPERTY(uzume::ui::AdminController* admin READ admin CONSTANT)
  // Page (tab) of the "server" screen: overview | people | games | systems | plugins | reports | settings
  Q_PROPERTY(QString serverPage READ serverPage NOTIFY serverPageChanged)
  // Page of Server > Settings: general | network | signin | updates | backups | storage | diagnostics
  Q_PROPERTY(QString serverSettingsPage READ serverSettingsPage NOTIFY serverSettingsPageChanged)
  // Versions (accounts-and-server.md 8k-2): the server's version from the handshake and how it relates to this app:
  // "same" | "server_older" | "app_older" | "unknown" (a version that is not SemVer, or no connection).
  Q_PROPERTY(QString hubVersion READ hubVersion NOTIFY hubChanged)
  Q_PROPERTY(QString versionRelation READ versionRelation NOTIFY hubChanged)
  // Setup assistant (7a): shown over the current screen; showSetupAssistant() is the entry point for the first start
  // and "Run setup again". Admins of a server with the admin pages only.
  Q_PROPERTY(bool setupAssistantOpen READ setupAssistantOpen NOTIFY setupAssistantChanged)
  // "Set up a server on this PC" (connection screen, pairing form, Settings > Hubs)
  Q_PROPERTY(uzume::ui::LocalHubController* localHub READ localHub CONSTANT)
  // First start, joining and setting up servers (screen "start"); the people on this PC and the account (0.18 package 7)
  Q_PROPERTY(uzume::ui::OnboardingController* onboarding READ onboarding CONSTANT)
  Q_PROPERTY(uzume::ui::AccountsController* accounts READ accounts CONSTANT)
  // Plugins (Settings > Plugins, Plugin console tab of the game panel)
  Q_PROPERTY(uzume::ui::PluginsController* plugins READ plugins CONSTANT)
  Q_PROPERTY(uzume::ui::RunsController* runs READ runs CONSTANT)
  // Together (0.23): the notifications of the bell and the toasts (ADR 0033 D2), and the online list (D1): people other than me
  // [{userId, name, state, gameTitle, system, sessionId, sub, color, inviteKind}]; inviteKind "session" | "run" | "" decides the
  // "Invite" button. Empty without a server.
  Q_PROPERTY(uzume::ui::NotificationCenter* notifications READ notifications CONSTANT)
  Q_PROPERTY(uzume::ui::ControlController* control READ control CONSTANT)  // controller hand-over (ADR 0033 D8)
  Q_PROPERTY(uzume::ui::TogetherController* together READ together CONSTANT)  // ready check and shared pause (ADR 0033 D3)
  Q_PROPERTY(QVariantMap gameRun READ gameRun NOTIFY gameRunChanged)  // run of the running game {id, name, status}; empty = not a run game
  Q_PROPERTY(uzume::ui::VoiceController* voice READ voice CONSTANT)  // voice chat of the run or Session of the game (ADR 0033 D7)
  Q_PROPERTY(uzume::ui::ChatController* chat READ chat CONSTANT)  // chat of the run or Session of the game (ADR 0033 D4)
  Q_PROPERTY(QVariantList onlinePeople READ onlinePeople NOTIFY onlineChanged)
  Q_PROPERTY(uzume::ui::VariantsController* variants READ variants CONSTANT)  // game variants (0.21)
  // Play run of a randomized run that has to build the person's variant first (10f-3): {runId, runName, gameTitle, state: building |
  // failed, step: reading|randomizing|writing|"", fraction, percent, etaText, error, startWhenReady, background}; empty when none.
  Q_PROPERTY(QVariantMap runBuild READ runBuild NOTIFY runBuildChanged)
  // Favourites and collections of the signed-in person (3c-0, 3c-1, 3c-4)
  Q_PROPERTY(uzume::ui::PersonalController* personal READ personal CONSTANT)
  Q_PROPERTY(uzume::ui::ScreenshotController* screenshots READ screenshots CONSTANT)
  Q_PROPERTY(uzume::ui::StatesController* states READ states CONSTANT)  // save states, rewind, run-ahead (0.20)
  Q_PROPERTY(uzume::ui::LocalStorageController* localStorage READ localStorage CONSTANT)  // Settings > This PC > Storage (0.19)
  // Connection check dialog (design 6e): Hub reachable, STUN, TURN, NAT type, test connection
  Q_PROPERTY(uzume::ui::ConnectionCheckController* connectionCheck READ connectionCheck CONSTANT)
  // Save slot picker, history/restore, snapshots, "save changed elsewhere" notice (ADR 0012 D7)
  Q_PROPERTY(uzume::ui::SaveHistoryController* saveHistory READ saveHistory CONSTANT)
  // Claude debugging (ADR 0032): switch, port, band; the MCP server itself lives in McpController.
  Q_PROPERTY(uzume::ui::McpController* mcp READ mcp CONSTANT)
  // Log file path (empty when file logging is not active) and "open folder" (Settings)
  Q_PROPERTY(QString logFile READ logFile CONSTANT)
  // Shell (sidebar user block, Settings footer)
  Q_PROPERTY(QString deviceName READ deviceName CONSTANT)
  // Admin attention (readable pass 7): what needs an admin, one entry per open problem: {id, severity warn|error, text,
  // actionLabel, target}. Server problems of /admin/status, core problems, a server update on offer, games in save conflict.
  // Empty for anyone who is not an admin of a server with the admin area. The counts feed the badges of AppNav.
  Q_PROPERTY(QVariantList attention READ attention NOTIFY attentionChanged)
  Q_PROPERTY(int attentionCount READ attentionCount NOTIFY attentionChanged)
  Q_PROPERTY(int coreProblemCount READ coreProblemCount NOTIFY attentionChanged)
  Q_PROPERTY(bool serverUpdatePending READ serverUpdatePending NOTIFY attentionChanged)
  Q_PROPERTY(bool cheatDbSupported READ cheatDbSupported NOTIFY hubChanged)  // the server advertises cheat_db_v1
  Q_PROPERTY(bool isAdmin READ isAdmin NOTIFY hubChanged)  // handshake `user.role` is "admin"; false for older servers
  Q_PROPERTY(QString userName READ userName NOTIFY hubChanged)  // Hub user (handshake `user`); empty for older Hubs
  Q_PROPERTY(QString playerVersion READ playerVersion CONSTANT)
  Q_PROPERTY(QString platformText READ platformText CONSTANT)  // "Windows x86-64 · Protocol v1"

 public:
  struct Options {
    QString dataDir;                // empty: ProfileStore::defaultBaseDir()
    bool allowHttp = false;         // --dev-allow-http
    bool memoryCredentials = false; // tests: no OS credential store
    std::shared_ptr<CredentialStore> credentialStore;  // tests: this store (also across a restart of the controller); wins over memoryCredentials
    bool probeCoreVersions = true;  // determine core version for the handshake (briefly loads the core)
    bool enableGamepads = true;     // SDL3 gamepads (tests without hardware use SDL virtual joysticks)
    bool enableUpdates = false;     // schedule update checks (the app enables it; tests never touch the network)
    QString updateIndexUrl;         // tests/CI: instead of env UZUME_APP_UPDATE_INDEX_URL / the default
    int gamepadPollMs = 8;          // <= 0: no poll timer (tests call controllers()->gamepads()->poll())
    std::function<std::unique_ptr<ConnectionProbes>()> connectionProbes;  // tests: fake probes for the connection check; empty = real
    std::function<void(ConnectionTarget&)> connectionTargetPatch;         // tests: adds the STUN/TURN servers a fake Hub does not hand out
  };

  explicit PlayerController(const Options& options, QObject* parent = nullptr);
  ~PlayerController() override;

  // Auto-connect to the last used hub, if enabled.
  void startup();

  QString screen() const;
  QVariantList hubs() const;
  QString connectionNotice() const { return notice_; }
  bool autoConnect() const;
  void setAutoConnect(bool on);
  QString deviceFooter() const;
  QVariantMap pairing() const;
  QString hubName() const;
  QString hubLink() const;
  QString hubAddress() const;
  LibraryModel* library() { return &model_; }
  QString libraryState() const { return libraryState_; }
  QString libraryError() const { return libraryError_; }
  QString selectedGameId() const { return selectedId_; }
  QVariantMap selectedGame() const;
  QVariantMap backgroundGame() const;
  QString runningGameId() const { return gameActive_ ? launchGame_.id : QString(); }
  // Called when a game starts (msecs 0 = now); tests pass fixed times.
  void recordLastPlayed(const QString& gameId, qint64 msecs = 0);
  // Tests and screenshots (GameSession::setPreview): declare the library game that the preview game stands for.
  void adoptPreviewGame(const QString& gameId);
  QVariantMap startConfirm() const;
  bool gameInBackground() const { return background_; }
  bool quitting() const { return quitting_; }
  GameSession* gameSession() { return &session_; }
  SessionController* sessions() { return sessions_.get(); }
  UpdatesController* updates() { return updates_.get(); }
  ReportController* report() { return report_.get(); }
  ErrorsController* errors() { return errors_.get(); }
  AdminController* admin() { return admin_.get(); }
  QString serverPage() const { return serverPage_; }
  QString serverSettingsPage() const { return serverSettingsPage_; }
  LocalHubController* localHub() { return localHub_.get(); }
  OnboardingController* onboarding() { return onboarding_.get(); }
  AccountsController* accounts() { return accounts_.get(); }
  PluginsController* plugins() { return plugins_.get(); }
  RunsController* runs() { return runs_.get(); }
  NotificationCenter* notifications() { return notifications_.get(); }
  TogetherController* together() { return together_.get(); }
  ControlController* control() { return control_.get(); }
  ChatController* chat() { return chat_.get(); }
  VoiceController* voice() { return voice_.get(); }
  QVariantMap gameRun() const;
  // People of the chat scope (kind "run" | "session", id): [{userId, name, role: "HOST" | "VIEWER" | "", me, color, connection, tone}]
  Q_INVOKABLE QVariantList scopePeople(const QString& kind, const QString& id) const;
  QVariantList onlinePeople() const;
  Q_INVOKABLE void watchOnline(const QString& sessionId);   // "Watch" in the online list
  Q_INVOKABLE void inviteOnline(const QString& userId);     // "Invite": to my shared Session, else to my run
  // The action of a notification entry (NotificationCenter::actionRequested): "watch:<session id>", "run:<run id>",
  // "settings:<section>", "attention:<target>"; anything else is ignored.
  void runNotificationAction(const QString& kind, const QString& actionData);
  VariantsController* variants() { return variants_.get(); }
  QVariantMap runBuild() const { return runBuild_; }
  Q_INVOKABLE void setRunBuildStartWhenReady(bool on);  // "Start <run> when it's ready"
  Q_INVOKABLE void runBuildInBackground();              // hides the dialog, the build goes on
  Q_INVOKABLE void cancelRunBuild();                    // stops the build; nothing is kept
  Q_INVOKABLE void dismissRunBuild();                   // closes the dialog after a failure
  ToolsClient* toolsClient() { return toolsClient_.get(); }  // debug MCP server
  ToolCache* toolCache() { return toolCache_.get(); }
  VariantCache* variantCache() { return variantCache_.get(); }
  PersonalController* personal() { return personal_.get(); }
  ScreenshotController* screenshots() { return screenshots_.get(); }
  LocalStorageController* localStorage() { return localStorage_.get(); }
  StatesController* states() { return states_.get(); }
  // image://screenshots/<id>: for QQmlEngine::addImageProvider (the engine takes ownership); the cache belongs to the controller.
  ScreenshotImageProvider* createScreenshotImageProvider() { return new ScreenshotImageProvider(screenshots_->cache()); }
  SaveHistoryController* saveHistory() { return history_.get(); }
  ConnectionCheckController* connectionCheck() { return connectionCheck_.get(); }
  McpController* mcp() { return mcp_.get(); }
  // Debug access (MCP server): the running game as {id, title, system, core, romPath, inputProfile}, empty map when none.
  QVariantMap runningGameInfo() const;
  // The player-info.json content of "Collect logs" (live values, no tokens or passwords).
  QJsonObject diagnosticsSnapshot() const { return diagnosticsInfo(); }
  QString logFile() const;
  QString deviceName() const;
  QString userName() const;
  bool isAdmin() const;
  bool cheatDbSupported() const;
  QString hubVersion() const;
  QString versionRelation() const;
  bool setupAssistantOpen() const { return setupAssistantOpen_; }
  QString playerVersion() const;
  QString platformText() const;
  // A game or a watched Session is running (updates are never applied then).
  bool sessionBusy() const;
  bool collectingDiagnostics() const { return collectingDiagnostics_; }
  EmulationController* emulation() { return emulation_.get(); }
  ControllersController* controllers() { return controllers_.get(); }
  AudioController* audio() { return audio_.get(); }
  AudioMixer* audioMixer() { return &mixer_; }
  bool fullscreenOnStart() const;
  QString currentSystemId() const;
  QString appearance() const;
  void setAppearance(const QString& name);
  bool darkMode() const;
  QString textSize() const;
  void setTextSize(const QString& size);
  bool reduceMotion() const;
  bool discordPresence() const;
  void setDiscordPresence(bool on);
  const DiscordPresence* discord() const { return discord_.get(); }
  void setReduceMotion(bool on);
  bool eventFilter(QObject* watched, QEvent* event) override;  // Qt < 6.5: palette changes drive the System appearance
  QVariantMap romCache() const;
  // Size limit in bytes (0 = unlimited); trims at once (never the running game or a file being downloaded).
  Q_INVOKABLE void setRomCacheLimit(double bytes);
  // Removes all cached ROM files except the running game and files being downloaded. Returns freedBytes, freedText, removedFiles.
  Q_INVOKABLE QVariantMap clearRomCache();
  Q_INVOKABLE void refreshRomCache() { emit romCacheChanged(); }
  // Hashes that must stay in the cache: the game that is starting/running/in the background and active downloads.
  QSet<QString> protectedRomHashes() const;
  // Version of `coreId` the running game has loaded (empty when no game runs or it uses another core).
  QString inUseCoreVersion(const QString& coreId) const;
  bool canUpload() const;
  QVariantMap upload() const { return uploads_->upload(); }
  QStringList libraryFolders() const;
  QVariantMap libraryScan() const { return libraryScan_; }
  QVariantList libraryFolderInfo() const;
  int libraryGameCount() const;
  bool canRegisterLocalRoms() const;
  LocalRomIndex* localRomIndex() { return localRoms_.get(); }
  RomRegistrar* romRegistrar() { return registrar_.get(); }
  // Where the ROM of a game comes from right now (library folder, cache, download, missing).
  RomSourceChoice romSourceOf(const GameEntry& game) const;
  QVariantMap coreUpdate() const;
  QStringList uploadFilters() const;
  QVariantList coreWarnings() const;
  QVariantList attention() const;
  int attentionCount() const { return attention().size(); }
  int coreProblemCount() const;
  bool serverUpdatePending() const;
  // Runs the target of an attention entry: "page:<server page>", "settings:<section>" or "library:attention".
  Q_INVOKABLE void openAttention(const QString& target);
  QVariantMap saveConflict() const { return conflict_; }
  QString saveNote() const { return saves_ ? saves_->note() : QString(); }
  SaveSync* saveSync() { return saves_.get(); }
  // Ends a running game and waits (max. about 10 s) for the final save upload; also called on destruction.
  void shutdown();

  // Access for tests.
  HubConnection* connection() { return conn_.get(); }
  HubLibrary* hubLibrary() { return library_.get(); }
  RomDownloader* downloader() { return downloader_.get(); }
  ProfileStore* profileStore() { return profiles_.get(); }
  const QList<CoreInfo>& handshakeCores() const { return coreList_; }
  PlayerSettings* playerSettings() { return settings_.get(); }
  HubSystems* hubSystems() { return systems_.get(); }
  GameUploader* gameUploader() { return uploads_->uploader(); }
  FirmwareCache* firmwareCache() { return fwCache_.get(); }

  static QString formatFingerprint(const QString& fp);
  // Date/time helper of every list (timeformat.h): "5 min ago", "today 18:52", "16 Oct"; the full text is for tooltips.
  // Window of the bottom screen (0.19): {x, y, width, height, screen}, empty map = never placed; saved per PC.
  Q_INVOKABLE QVariantMap bottomWindowGeometry() const;
  Q_INVOKABLE void saveBottomWindowGeometry(int x, int y, int width, int height, const QString& screenName);
  Q_INVOKABLE QString whenText(const QString& iso) const;
  Q_INVOKABLE QString fullTimeText(const QString& iso) const;
  static QString platformLabel(const QString& platform, const QString& arch);

  // 3a / 3b
  Q_INVOKABLE void addHub(const QString& address);
  Q_INVOKABLE void connectProfile(const QString& hubId);
  Q_INVOKABLE void retryConnection();
  Q_INVOKABLE void removeHub(const QString& hubId);  // empty: discard the running attempt
  // Settings > Hubs > Edit (D3): only host and port of a saved Hub change; hubId, pinned fingerprint, credential and
  // Hub user stay. validateHubAddress returns the messages of docs/design/legacy/player.md 3p (empty = valid).
  // editHub reconnects when it is the active Hub; a different certificate at the new address then goes through
  // the "certificate changed" flow and is never accepted silently.
  Q_INVOKABLE QStringList validateHubAddress(const QString& host, const QString& port) const;
  Q_INVOKABLE bool editHub(const QString& hubId, const QString& host, const QString& port);
  // Re-evaluates the core / firmware state of all games, the detail pane and the Emulation page (after a core
  // download or whenever the core cache may have changed) without a restart.
  Q_INVOKABLE void refreshCoreState();
  Q_INVOKABLE void confirmTrust();
  Q_INVOKABLE void rejectTrust();
  // Certificate changed (blocked): re-pins exactly `observedFingerprint` (raw value from the hub card) and reconnects;
  // cancel drops the blocked attempt without touching the profile.
  Q_INVOKABLE void trustChangedCertificate(const QString& observedFingerprint);
  Q_INVOKABLE void cancelCertificateChange();
  Q_INVOKABLE void requestPairing();
  Q_INVOKABLE void cancelPairing();
  // Onboarding invite (3b "Redeem invite"): code + display name.
  Q_INVOKABLE void redeemInvite(const QString& code, const QString& displayName);
  Q_INVOKABLE void leavePairing();
  // 3c
  Q_INVOKABLE void switchHub();
  Q_INVOKABLE void switchToHub(const QString& hubId);  // Settings -> Hubs: switchHub() + connectProfile()
  Q_INVOKABLE void reloadLibrary();
  Q_INVOKABLE void selectGame(const QString& gameId);
  // Opens the game page of a library game (selects it, shows the Library) on a tab; closeGamePage() returns to the list.
  Q_INVOKABLE void openGamePage(const QString& gameId, const QString& tab = QString());
  Q_INVOKABLE void closeGamePage();
  bool gamePageOpen() const { return gamePageOpen_; }
  QString gamePageTab() const { return gamePageTab_; }
  Q_INVOKABLE void playSelected();
  // "Play and share Session": starts the game and publishes it with the last chosen visibility.
  Q_INVOKABLE void playAndShareSelected();
  // "← Library" in the game view: pauses the running game and keeps it loaded in the background (the Session stays
  // shared), or leaves the watched Session. Quitting is quitGame().
  Q_INVOKABLE void leaveGameView();
  // Library "Resume": back to the game view; the game continues (resumed, not left paused).
  Q_INVOKABLE void resumeGame();
  // Starting another game while one is in the background asks first; confirm = quitGame() + start.
  Q_INVOKABLE void confirmQuitAndStart();
  Q_INVOKABLE void cancelQuitAndStart();
  // Library header / Sidebar
  Q_INVOKABLE void showLibrary();
  Q_INVOKABLE void showSettings();
  // Settings on one section: "account" | "appearance" | "audio" | "controllers" | "hotkeys" | "emulation" | "storage" | "hubs" |
  // "plugins" | "updates" | "diagnostics" ("library" is an alias of "storage"), or "server:<page>": Server > Settings at that page
  // for admins of a server with admin_v1 (anyone else gets the This PC section of the same name).
  Q_INVOKABLE void showSettingsSection(const QString& section);
  Q_INVOKABLE void openLogFolder();
  // "Collect logs" (Settings -> Diagnostics): diagnostics bundle zip (core/diagbundle.h). The info is gathered here, the
  // zip is built off the GUI thread; diagnosticsCollected() reports the result.
  Q_INVOKABLE QString diagnosticsFileName() const;     // uzume-diagnostics-<UTC timestamp>.zip
  Q_INVOKABLE QString diagnosticsFolderUrl() const;    // Desktop or Downloads (file URL) for the save dialog
  Q_INVOKABLE void collectDiagnostics(const QString& pathOrUrl);
  Q_INVOKABLE void showInFolder(const QString& pathOrUrl);
  Q_PROPERTY(bool collectingDiagnostics READ collectingDiagnostics NOTIFY collectingDiagnosticsChanged)
  Q_INVOKABLE void showEmulation();
  Q_INVOKABLE void showControllers();
  // uz.ui.open("screen") / "Open" in Settings > Plugins outside a game: selects the plugin's tab when it runs as the opened run's
  // view, else asks the window to show the plugin screen sheet (pluginScreenRequested). Plugins have no sidebar entry.
  Q_INVOKABLE void openPluginScreen(const QString& pluginId);
  Q_INVOKABLE void showRuns();  // Runs screen (docs/design/legacy/plugins.md 4j-2)
  // `uzume://run/<server>/<run id>/recap` (from the command line or the single-instance socket): opens the recap of that run when
  // the app is connected to that server. True when `link` is a run link (handled, also when it needs a different server).
  bool openRunLink(const QString& link);
  // Admin area: screen "server" on a tab (overview | people | games | systems | plugins | reports | settings; the old ids
  // status and users are aliases of overview and people; unknown -> overview). Only for admins of a server with admin_v1
  // (otherwise the screen shows the member block); reads the server status again.
  Q_INVOKABLE void showServerPage(const QString& page);
  // Server > Settings page (unknown -> general); does not leave the screen.
  Q_INVOKABLE void setServerSettingsPage(const QString& page);
  Q_INVOKABLE void showSetupAssistant();
  Q_INVOKABLE void hideSetupAssistant();
  // "Plugin settings" / "Notice settings…": Settings -> Plugins (pluginId highlights its row; empty = the section only).
  Q_INVOKABLE void showPluginSettings(const QString& pluginId = QString());
  // Ctrl+Shift+D (Developer mode): in a game the Plugin console tab of the panel, otherwise Settings > Plugins scrolled to the
  // Developer console.
  Q_INVOKABLE void showDeveloperConsole();
  // "Upload ROM": source = local path or file:// URL (from the file dialog). Streamed; progress in upload.
  Q_INVOKABLE void dismissCoreNotice(const QString& code);
  Q_INVOKABLE void uploadRom(const QString& source);
  Q_INVOKABLE void dismissUploadMessage();
  // Library folders (Settings): source = folder path or file:// URL. Adding/removing rescans; files are registered with the Hub.
  Q_INVOKABLE void addLibraryFolder(const QString& source);
  Q_INVOKABLE void removeLibraryFolder(const QString& path);
  Q_INVOKABLE void rescanLibrary();
  Q_INVOKABLE void stopLibraryScan();
  // DetailPane: uploads the local file of a local-only game (resumable); the game id stays the same.
  Q_INVOKABLE void uploadGameToHub(const QString& gameId);
  // DetailPane: removes the local registration of a local-only game (the game and its saves stay on the Hub).
  Q_INVOKABLE void removeFromLibrary(const QString& gameId);
  // Undo of the last removal: the files are registered again; the files themselves were never touched.
  Q_INVOKABLE void undoRemoveFromLibrary();
  Q_INVOKABLE void dismissRemovedFromLibrary();
  QVariantMap removedFromLibrary() const { return removedInfo_; }
  QString libraryView() const;
  void setLibraryView(const QString& view);
  QString stopSaveError() const { return stopSaveError_; }
  Q_INVOKABLE void dismissStopSaveError();
  // Detail pane "Check again": reloads the systems registry (firmware status) after the admin changed something.
  Q_INVOKABLE void recheckFirmware();
  // Game view
  Q_INVOKABLE void quitGame();
  // "Quit game" of the panel and the sidebar strip: shows the "Saving and syncing…" state for a moment, then quitGame().
  Q_INVOKABLE void requestQuit();
  // Conflict dialog: "use_hub" | "use_local" | "later" (Keep both, decide later)
  Q_INVOKABLE void resolveSaveConflict(const QString& action);

 signals:
  void runBuildChanged();
  void screenChanged();
  void pluginScreenRequested(const QString& pluginId);
  void serverPageChanged();
  void serverSettingsPageChanged();
  void setupAssistantChanged();
  void settingsSectionRequested(const QString& section);
  // Settings screen: scroll to a section ("plugins") and highlight a plugin row.
  void pluginSettingsRequested(const QString& pluginId);
  void developerConsoleRequested();   // Settings shows its Developer console
  void gameConsoleRequested();        // the game view opens the Plugin console tab
  void hubsChanged();
  void autoConnectChanged();
  void pairingChanged();
  void hubChanged();
  void coreWarningsChanged();  // hub handshake, the server's systems/cores or the local cores changed
  void attentionChanged();
  void onlineChanged();
  void gameRunChanged();
  void openChatRequested();  // the mention toast's OPEN CHAT: the game panel shows the Chat tab
  void hubLinkChanged();
  void libraryStateChanged();
  void selectedGameChanged();
  void backgroundGameChanged();
  void quittingChanged();
  void startConfirmChanged();
  void saveConflictChanged();
  void appearanceChanged();
  void textSizeChanged();
  void reduceMotionChanged();
  void discordPresenceChanged();
  void romCacheChanged();
  void uploadChanged();
  void libraryFoldersChanged();
  void libraryScanChanged();
  void libraryViewChanged();
  void gamePageChanged();
  void removedFromLibraryChanged();
  void libraryFolderInfoChanged();
  void stopSaveErrorChanged();
  void coreUpdateChanged();
  void emulationSettingsChanged();
  void collectingDiagnosticsChanged();
  void diagnosticsCollected(bool ok, const QString& path, const QString& message);

 private:
  void onConnectionState(HubConnection::State s);
  void refreshHubLink();
  void onLibraryLoaded();
  void applyCachedSyncKinds();
  void onRomStatus(const QString& sha, const RomStatus& st);
  void updateScreen();
  void prepareGameCore(const QString& gameId);
  void refreshEmulationPage();
  void refreshAttention();
  void adoptUploadedRom(const QString& srcPath, const QString& sha256, const QString& romFilename);  // off-thread copy into the cache
  void trimRomCache();  // applies the size limit (LRU); called at start, after a ROM became ready, when a game ends
  void applyUzumeOptions();
  void onUploadFinished(const UploadResult& result, const QString& srcPath);
  void startLibraryScan();
  void registerLocalFiles();      // files of the folders the Hub does not know yet
  void refreshRomStatuses();      // after the local index changed
  RomStatus romStatusOf(const GameEntry& game);
  QString localRomStatePath() const;
  void loadIgnoredRoms();
  void saveIgnoredRoms() const;
  QString friendlyError(const QString& code, const QString& message) const;
  void endRunningWork();
  QJsonObject diagnosticsInfo() const;

  Options options_;
  std::unique_ptr<ProfileStore> profiles_;
  std::shared_ptr<CredentialStore> credentials_;
  std::unique_ptr<HubConnection> conn_;
  std::unique_ptr<HubLibrary> library_;
  std::unique_ptr<RomCache> cache_;
  std::unique_ptr<RomDownloader> downloader_;
  std::unique_ptr<SaveSync> saves_;
  std::unique_ptr<PlayerSettings> settings_;
  std::unique_ptr<DiscordPresence> discord_;  // Rich Presence; fed by updateDiscord()
  std::unique_ptr<FirmwareCache> fwCache_;
  std::unique_ptr<HubSystems> systems_;
  std::unique_ptr<FirmwareProvisioner> provisioner_;
  std::unique_ptr<CoreCache> coreCache_;
  std::unique_ptr<CoreProvisioner> coreProv_;
  std::unique_ptr<CoreUpdater> coreUpdater_;  // background updates of cores that are already cached
  QTimer* systemsTimer_ = nullptr;           // re-reads the Hub's systems / cores info (at most every 30 min)
  QTimer* coreUpdateHideTimer_ = nullptr;    // hides the "Updated X to V" result line a while after the update
  bool coreUpdateShown_ = false;
  QSet<QString> coreReprobe_;                 // updated cores whose version is reported to the Hub once no game is running
  bool probeCoreVersions_ = true;
  void reprobeUpdatedCores();
  void cancelBackgroundUpdateForSelected();
  QMap<QString, QString> coreProblems_;  // core_id -> last provisioning problem reason (cleared with a fresh registry)
  HandshakeInfo handshake_;
  emu::ManifestRegistry manifests_;
  emu::CoreLocator locator_;
  QList<CoreInfo> coreList_;
  QMap<QString, QString> coreVersions_;  // core_id -> version from the core info
  QMap<QString, QString> coreNames_;  // core_id -> name from the core info (only if probed)
  AudioMixer mixer_;                          // the one output sink of the app (0.23); before session_ and sessions_
  std::shared_ptr<PluginSoundBus> soundBus_;  // uz.sound; before plugins_
  LibraryModel model_;
  GameSession session_;
  std::unique_ptr<SessionController> sessions_;
  std::unique_ptr<UpdatesController> updates_;
  std::unique_ptr<ReportController> report_;  // "Report a problem"
  std::unique_ptr<ErrorsController> errors_;  // error events (after updates_, plugins_, saves_)
  void noteCoreFailure(const QString& message);
  std::unique_ptr<AdminController> admin_;    // admin area: requests, Confirm it's you, status
  std::unique_ptr<LocalHubController> localHub_;
  std::unique_ptr<OnboardingController> onboarding_;  // after localHub_ and conn_
  std::unique_ptr<AccountsController> accounts_;
  std::unique_ptr<PersonalController> personal_;
  std::unique_ptr<LocalStorageController> localStorage_;  // measuring and clean-up of this PC's data (0.19)
  std::unique_ptr<ScreenshotController> screenshots_;  // screenshot hotkey and the game page strip (0.19)
  std::unique_ptr<PluginsController> plugins_;  // after settings_ and session_: destroyed before them
  std::unique_ptr<RunClient> runClient_;        // after sessions_ (the Hub socket) and plugins_
  std::unique_ptr<RunsController> runs_;
  std::unique_ptr<StateApi> stateApi_;          // save states on the server (states_v1)
  std::unique_ptr<StatesController> states_;    // after session_, settings_, runClient_
  QString stateRunId_;                          // run the running game was started in ("" = none): its core rules apply to states
  std::unique_ptr<TogetherController> together_;       // after sessions_, runClient_ and session_
  std::unique_ptr<ControlController> control_;         // after sessions_, session_ and controllers_
  std::unique_ptr<NotificationCenter> notifications_;  // after runs_ and updates_ (connected to both)
  std::unique_ptr<ChatApi> chatApi_;                   // after notifications_ (mentions)
  std::unique_ptr<ChatController> chat_;
  std::unique_ptr<VoiceController> voice_;             // after chat_, together_ and mixer_
  QString notifyHubId_;                                // the server the notifications belong to
  QSet<QString> notifiedRunInvites_;
  QSet<QString> notifiedProblems_;
  QString notifiedUpdate_;
  void wireNotifications();
  void wireTogether();
  void wireControl();
  void wireChat();
  void wireVoice();
  QVariantList withVoice(const QString& kind, const QString& id, QVariantList rows) const;  // voice marks and controls on the People rows
  TogetherController::Scope currentScope(bool chat) const;
  QString chatHubId_;  // the server and person the chat belongs to
  QString hubUserName(const QString& userId) const;  // display name of a person I know from the roster, my runs or my own Session; "" = unknown
  std::unique_ptr<ToolCache> toolCache_;
  std::unique_ptr<VariantCache> variantCache_;
  std::unique_ptr<ToolsClient> toolsClient_;
  std::unique_ptr<VariantsClient> variantsClient_;
  std::unique_ptr<VariantsController> variants_;  // after runs_ and plugins_ (references the Hub socket through conn_)
  std::unique_ptr<ConnectionCheckController> connectionCheck_;  // reads the Hub's ICE servers from sessions_ and the profile from conn_
  // "Play run": the game starts with the run context and the run's save slot; the chosen slot is restored afterwards.
  struct RunStart {
    QString runId;
    QString gameId;
    QString previousSlot;
    bool pending() const { return !runId.isEmpty(); }
  };
  RunStart runStart_;
  void startRunGame(const QString& runId, const QString& gameId);
  // "Play" of one variant (0.21): the game starts with the variant's own save slot; the chosen slot is restored afterwards.
  struct VariantStart {
    QString variantId;
    QString gameId;
    QString previousSlot;
    bool pending() const { return !variantId.isEmpty(); }
  };
  VariantStart variantStart_;
  struct PendingRunPlay {
    QString runId;
    QString gameId;
    bool startWhenReady = true;
    bool background = false;
  };
  QVariantMap runBuild_;
  void refreshRunBuild();
  PendingRunPlay pendingRunPlay_;  // a run start waits for its run variant
  void startVariantGame(const QString& variantId);
  VariantLaunch variantLaunchFor(const GameEntry& game);
  QString titleIdOf(const GameEntry& game);
  bool startPending() const { return runStart_.pending() || variantStart_.pending(); }
  bool routeRunSlotStart();  // a normal start from a run's slot: Play run, or back to Main (#355)
  void finishRunStart();  // the run's game is over (or never started): the game's own slot is selected again
  std::unique_ptr<SaveHistoryController> history_;  // after saves_/conn_: destroyed first
  std::unique_ptr<EmulationController> emulation_;
  std::unique_ptr<ControllersController> controllers_;
  std::unique_ptr<AudioController> audio_;  // master volume, mute, background mute (0.19)
  // Helpers (after everything they reference): they read/write the state below through references.
  std::unique_ptr<CoreCatalog> catalog_;
  std::unique_ptr<GameDetail> detail_;
  std::unique_ptr<HubPresenter> hubPresenter_;
  std::unique_ptr<GameStarter> starter_;
  bool collectingDiagnostics_ = false;
  bool shareOnStart_ = false;
  QTimer* liveTimer_ = nullptr;     // periodic quiet refresh of the Library and the core state while it is shown
  bool quietRefresh_ = false;       // the running library reload is the periodic one (no "loading" state, errors ignored)
  QString resumePage_;              // page to return to after the reconnect of an edited active Hub
  void endGameContext();
  void setupStates();
  void updateStatesLocation();
  QString statesDir() const;
  StatesController::GameContext stateContext(const GameEntry& game, bool running) const;
  void applyStateOptions();

  QString screen_ = QStringLiteral("connection");
  QString page_ = QStringLiteral("library");  // page shown while connected: library | settings | emulation | controllers | plugin
  QString serverPage_ = QStringLiteral("overview");
  QString serverSettingsPage_ = QStringLiteral("general");
  bool setupAssistantOpen_ = false;
  bool inviteBusy_ = false;
  std::unique_ptr<LocalRomIndex> localRoms_;
  std::unique_ptr<UploadController> uploads_;  // ROM upload state (declared after localRoms_, model_, manifests_)
  std::unique_ptr<RomRegistrar> registrar_;
  QVariantMap libraryScan_{{QStringLiteral("active"), false}, {QStringLiteral("done"), 0}, {QStringLiteral("total"), 0},
                           {QStringLiteral("text"), QString()}, {QStringLiteral("isError"), false}};
  QHash<QString, QDateTime> folderScanned_;  // library folder -> end of its last complete scan (saved in cache/library-folders.json)
  void loadFolderScans();
  void saveFolderScans() const;
  QSet<QString> dismissedCoreNotices_;  // "core_download" dismissed for this session
  QVariantMap removedInfo_;          // {title} of the last removed game while Undo is offered
  QString removedTitle_;             // title of the game being removed (the answer comes asynchronously)
  QStringList removedShas_;          // hashes put on the ignore list by that removal
  QSet<QString> ignoredRoms_;        // sha256 of files the user removed from the library (until the file is gone)
  QSet<QString> failedRegistrations_;  // not retried within this connection
  bool scanRequested_ = false;       // a scan was started for the current connection
  QString pendingSelectId_;             // select this game once the library reloaded (after an upload)
  QList<FirmwareProblem> fwProblems_;   // last validation problems (cleared by recheckFirmware/new registry)
  QMap<QString, QString> fwOptions_;    // firmware core options of the launch in progress
  bool pairingFlow_ = false;
  bool lastAttemptPairing_ = false;  // last attempt came from addHub (true) or connectProfile (false)
  QString notice_;
  QString lastError_;
  QString libraryState_ = QStringLiteral("loading");
  QString libraryError_;
  QString selectedId_;
  QString startError_;
  QString stopSaveError_;
  QString pendingSha_;
  QElapsedTimer progressClock_;           // ROM progress throttle
  QHash<QString, qint64> lastProgressMs_;  // sha -> last accepted progress (clock ms + 1)
  QHash<QString, int> shownPercent_;       // sha -> last percent that reached selectedGame
  PlayPhase phase_ = PlayPhase::None;
  bool gamePageOpen_ = false;
  QString gamePageTab_ = QStringLiteral("overview");
  bool gameActive_ = false;
  bool background_ = false;      // gameActive_ and paused behind the Library (input blocked)
  QString backgroundId_;
  QString backgroundTitle_;
  qint64 gamePausedMs_ = 0;      // epoch ms when the game went to the background (3c-5 detail column: "paused 22:31")
  void updateDiscord();  // game / Session / run changed: the activity for Discord (nothing private)
  qint64 gameStartedMs_ = 0;     // epoch ms of the start of the running game ("12 min" in the Now running strip)
  bool quitting_ = false;
  bool linkSeenUp_ = false;   // the Sessions socket was open since the last handshake
  bool hubLinkDown_ = false;  // see hubLink(); read by GameDetail
  bool confirmActive_ = false;   // "Quit X and start Y?" is open
  bool confirmShare_ = false;
  void setBackground(bool on);
  void askQuitAndStart(bool share);
  // Start of a game: after the ROM and the save sync
  GameEntry launchGame_;
  QString launchRom_;
  bool saveReady_ = false;
  QString saveNoteStart_;  // note of the last start sync (e.g. Hub not reachable)
  QVariantMap conflict_;
  std::unique_ptr<McpController> mcp_;  // last: destroyed first, its tools reach into everything above
};

}  // namespace uzume::ui
