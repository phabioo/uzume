#pragma once
// VariantsController (Uzume 0.21 Randomizer, ADR 0031, design sheet 5): the app side of game variants. It gets the engine from the
// server (tools catalog), builds a variant from its recipe in the background (one build at a time), keeps the built files in the
// variant cache, tells the game start what to launch, and serves the plugin API (fb.variants) and the debug MCP server.
// The QML screens of the later UI package use the properties and methods below; this class draws nothing.
//
// Variant state strings: building (also queued), ready (built on this PC), needs_rebuild (files removed or built on another PC; the
// recipe rebuilds the same world), failed (the last build of this session; error / details tell why).
// Error codes: engine_missing, engine_not_offered, engine_download_failed, base_missing, base_mismatch, not_enough_memory, disk_full,
// engine_failed, timed_out, cancelled, output_missing, io_error, name_taken, server_unreachable, unsupported.
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <QCryptographicHash>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "hublibrary.h"
#include "hubprotocol.h"
#include "playersettings.h"
#include "plugin_variants.h"
#include "romdownloader.h"
#include "romsource.h"
#include "toolcache.h"
#include "toolsclient.h"
#include "variantbuilder.h"
#include "variantcache.h"
#include "variantlaunch.h"
#include "variantsclient.h"

namespace uzume::plugins {
class PluginHost;
}

namespace uzume::ui {

class VariantsController;

// The provider the plugin runtime calls from the plugin thread (thread-safe snapshots; builds are queued to the GUI thread).
class VariantsPluginBridge : public plugins::VariantsProvider {
 public:
  std::shared_ptr<plugins::VariantRegistry> registry() override { return registry_; }
  plugins::VariantEngineInfo engine() const override;
  QList<plugins::VariantRow> list(const QString& gameId) const override;
  quint64 build(const QString& pluginId, const plugins::VariantBuildRequest& req) override;

  // GUI thread: publish the current state.
  void setEngine(const plugins::VariantEngineInfo& e);
  void setRows(const QHash<QString, QList<plugins::VariantRow>>& rows);
  void attach(VariantsController* owner) { owner_ = owner; }
  quint64 nextRequestId() { return ++lastId_; }

 private:
  std::shared_ptr<plugins::VariantRegistry> registry_ = std::make_shared<plugins::VariantRegistry>();
  mutable QMutex mutex_;
  plugins::VariantEngineInfo engine_;
  QHash<QString, QList<plugins::VariantRow>> rows_;
  std::atomic<VariantsController*> owner_{nullptr};
  std::atomic<quint64> lastId_{0};
};

class VariantsController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")
  // The server offers variants (variants_v1) and the tools catalog (tools_v1) and a plugin registered games: the UI shows Randomize.
  Q_PROPERTY(bool serverSupport READ serverSupport NOTIFY engineChanged)
  // {state: no_server | not_offered | not_on_pc | downloading | ready | failed, version, sizeBytes, sizeText, license, sourceUrl,
  //  received, total, percent, error}
  Q_PROPERTY(QVariantMap engine READ engine NOTIFY engineChanged)
  // A build runs (or is queued).
  Q_PROPERTY(bool building READ building NOTIFY progressChanged)
  // The running build: {variantId, name, gameId, slot, step: reading|randomizing|writing, fraction, percent, etaMs, etaText, elapsedMs,
  //  small (a quick build: the UI shows no dialog)}; empty when none.
  Q_PROPERTY(QVariantMap current READ current NOTIFY progressChanged)
  // Variants of the game set with setGame(), newest first (see variantMap()).
  Q_PROPERTY(QVariantList variants READ variants NOTIFY variantsChanged)
  Q_PROPERTY(QString gameId READ gameId NOTIFY variantsChanged)
  // The variant cache: {bytes, text, limitBytes, limitText, unplayedDays}.
  Q_PROPERTY(QVariantMap cache READ cache NOTIFY cacheChanged)
  // Counts up whenever the plugins' variants registrations change; bind to it to re-ask randomizeState() / presets().
  Q_PROPERTY(int registryRevision READ registryRevision NOTIFY registryChanged)
  // Settings > This PC > Storage (10g-3): one row per variant known on this PC (recipes stay when the files are removed), most
  // recently played first: [{id, name, subtitle, isRun, removed, lastPlayedMs, lastPlayedText, bytes, sizeText}].
  Q_PROPERTY(QVariantList storage READ storage NOTIFY cacheChanged)
  // Library (10g-1): gameId -> {count, buildPercent (-1 none), buildName, lastId, lastName, lastPlayedMs}; own variants only.
  Q_PROPERTY(QVariantMap summary READ summary NOTIFY variantsChanged)
 public:
  struct Deps {
    HubConnection* conn = nullptr;
    HubLibrary* library = nullptr;
    PlayerSettings* settings = nullptr;
    ToolsClient* tools = nullptr;
    ToolCache* toolCache = nullptr;
    VariantsClient* client = nullptr;
    VariantCache* cache = nullptr;
    RomDownloader* downloader = nullptr;
    std::function<RomSourceChoice(const GameEntry&)> romSource;          // library folder / cache / download / missing
    std::function<QString(const QString& runId)> runSlot;                 // the run's save slot
    std::function<QString(const QString& runId)> runStatus;               // starting | active | paused | finished | abandoned | "" unknown
    std::function<QString(const QString& runId)> runRandomizerMode;       // off | each | same | "" (no randomizer / unknown)
    std::function<QString(const QString& runId)> runVariantKind;          // run | own | ""
    std::function<QSet<QString>()> protectedVariantIds;                   // the running game's variant
    std::function<void(const QString& pluginId, const QString& event, const QVariantMap& value)> postPluginEvent;  // host event to a plugin
    std::function<QString(const GameEntry&)> titleIdOf;                   // 3DS program id of a game (header), empty = unknown
    QString platform;                                                      // default CoreCache::currentPlatform()
  };

  VariantsController(Deps deps, std::shared_ptr<VariantsPluginBridge> bridge, QObject* parent = nullptr);
  ~VariantsController() override;

  bool serverSupport() const;
  QVariantMap engine() const { return engine_; }
  bool building() const { return active_.has_value() || !queue_.isEmpty(); }
  QVariantMap current() const { return current_; }
  QVariantList variants() const { return variantList_; }
  QString gameId() const { return gameId_; }
  QVariantMap cache() const;
  int registryRevision() const { return registryRevision_; }
  QVariantList storage() const;
  QVariantMap summary() const;
  std::shared_ptr<VariantsPluginBridge> bridge() const { return bridge_; }
  plugins::VariantRegistry* registry() { return bridge_->registry().get(); }

  // ---- the game the UI shows
  Q_INVOKABLE void setGame(const QString& gameId);  // loads the variants of the game from the server
  Q_INVOKABLE void refresh();
  // A plugin registered this game (RANDOMIZER Supported).
  Q_INVOKABLE bool supported(const QString& gameId) const;
  // [{id, label, hint, custom}] of the registered presets for the game plus Uzume's own "custom" entry last; empty when unsupported.
  Q_INVOKABLE QVariantList presets(const QString& gameId) const;
  Q_INVOKABLE QVariantMap variantMap(const QString& variantId) const;
  Q_INVOKABLE QVariantList variantsOf(const QString& gameId) const;

  // Randomize on the game page (10a-1, 10a-3a/c): "ok" (a plugin registered this game), "unsupported" (a plugin handles this system
  // but not this game: the button is off), "none" (no plugin: the button is hidden).
  Q_INVOKABLE QString randomizeState(const QString& gameId) const;
  // Any plugin registered variants support (the Variants section shows the "plugin off" line when variants exist but this is false).
  Q_INVOKABLE bool pluginAvailable() const;
  // The game file on this PC: {onPc, sizeText} (10d-1c).
  Q_INVOKABLE QVariantMap gameFile(const QString& gameId) const;
  // "Randomizer #n" for the next variant of the game, and whether a name is taken by another variant of the game (case-insensitive).
  Q_INVOKABLE QString defaultName(const QString& gameId) const;
  Q_INVOKABLE bool nameTaken(const QString& gameId, const QString& name, const QString& exceptVariantId = QString()) const;
  // A build of this game finishes in seconds (GB, GBC, GBA): the UI shows no dialog for it (10c-3a).
  Q_INVOKABLE bool smallGame(const QString& gameId) const;
  // Settings string check of the Custom preset (10b-2, 10b-3b): {state: empty | invalid | ok | other_version, valid, prefix, text}.
  Q_INVOKABLE QVariantMap checkSettings(const QString& gameId, const QString& text) const;
  // Name of the variant that owns a save slot ("var-xxxxxxxx"), empty if none (10g-2); the SaveHistory slot list uses it.
  Q_INVOKABLE QString slotDisplayName(const QString& slot) const;
  // The variants of every game, newest first (used by the Library status and the Storage page).
  Q_INVOKABLE void reloadAll();

  // ---- the engine
  Q_INVOKABLE void prepareEngine();  // downloads the offered engine (10d-2); a pending build continues after it
  // ---- variants
  // Creates the variant on the server and builds it. Returns {ok, requestId} or {ok:false, error}. seed: digits, used when useSeed.
  // Server answers that come later (name_taken, ...) arrive as createFailed().
  Q_INVOKABLE QVariantMap build(const QString& gameId, const QString& name, const QString& presetId, const QString& settings, const QString& seed,
                                bool useSeed, bool spoilerLog);
  Q_INVOKABLE void cancel(const QString& variantId);  // stops a build (running or queued); nothing is kept
  Q_INVOKABLE void rebuild(const QString& variantId, bool buildAnyway = false);  // the recipe builds it again (same world)
  Q_INVOKABLE void rename(const QString& variantId, const QString& name);
  Q_INVOKABLE void remove(const QString& variantId, bool keepSaves);
  Q_INVOKABLE void play(const QString& variantId);  // starts it; builds first when its files are gone (needsBuild)
  // The spoiler log text. Only after `confirmed`; for run variants only when the run is finished or abandoned. {ok, text | error}.
  Q_INVOKABLE QVariantMap spoilerLog(const QString& variantId, bool confirmed) const;
  // The seed of an own variant, for "Copy seed" (never for run variants). {ok, seed | error}
  Q_INVOKABLE QVariantMap ownSeed(const QString& variantId) const;
  Q_INVOKABLE QString failureDetails(const QString& variantId) const;  // text for "Copy details" (no game data)
  Q_INVOKABLE void dismissFailure(const QString& variantId);
  // ---- storage (Settings > This PC > Storage)
  Q_INVOKABLE void setCacheLimit(double bytes);
  Q_INVOKABLE void setUnplayedDays(int days);
  // "Free X…" of the Storage page: drops the built files of every variant that is not building or running; recipes and saves stay.
  // {freedBytes, removed}
  Q_INVOKABLE QVariantMap freeCache();
  Q_INVOKABLE QVariantMap trimCache();  // applies the limit and the age rule now: {freedBytes, removed}
  // ---- runs
  // Fetches the participant's recipe of the run (GET /runs/{id}/variant) and builds the run variant "run-<id>" into the run slot; reports
  // building / ready to the server. runVariantReady / runVariantFailed follow. With playWhenReady the game of the run starts then.
  Q_INVOKABLE void ensureRunVariant(const QString& runId, const QString& gameId);
  Q_INVOKABLE QVariantMap runVariantState(const QString& runId) const;  // {state, fraction, error}
  Q_INVOKABLE void setRunOwnVariant(const QString& runId, const QString& variantId);  // the own variant this person joined the run with
  bool runUsesVariant(const QString& runId) const;                // the run has a randomizer (mode not off)
  QString runVariantId(const QString& runId) const;               // "run-<id>" or the own variant joined with; empty if none
  bool runVariantBuilt(const QString& runId) const;

  // ---- for the game start and the plugins
  VariantLaunch launchInfo(const QString& variantId) const;
  void touch(const QString& variantId) { if (deps_.cache) deps_.cache->touch(variantId); }
  Q_INVOKABLE QString gameOf(const QString& variantId) const;
  std::optional<VariantInfo> info(const QString& variantId) const;
  // Used by the plugin bridge and the MCP server (GUI thread).
  void buildForPlugin(const QString& pluginId, const plugins::VariantBuildRequest& req, quint64 requestId);
  QVariantMap status(const QString& variantId) const;  // variantMap() of any known variant

 signals:
  void engineChanged();
  void registryChanged();  // a plugin registered or dropped variants support (Randomize… shows or hides)
  void progressChanged();
  void variantsChanged();
  void cacheChanged();
  void toast(const QString& text, const QString& variantId);  // "<name> is ready"
  void buildFinished(const QString& variantId, bool ok, const QString& error);
  void variantCreated(quint64 requestId, const QString& variantId);  // the server accepted the recipe
  void createFailed(quint64 requestId, const QString& error);
  void actionFailed(const QString& action, const QString& variantId, const QString& error);  // rename / remove
  void needsBuild(const QString& variantId);      // play() found no files and started a build
  void launchRequested(const QString& variantId);  // start exactly this variant (PlayerController)
  void runVariantChanged(const QString& runId);
  void runVariantReady(const QString& runId);
  void runVariantFailed(const QString& runId, const QString& error);

 private:
  struct Job {
    quint64 requestId = 0;
    QString pluginId;
    QString variantId;
    VariantRecipe recipe;
    QString gameId;
    bool playWhenReady = false;
    bool skipBaseCheck = false;
    QString runId;
    bool engineWaiting = false;
  };
  struct VarState {
    QString error;
    QStringList log;
    qint64 needed = 0, free = 0;
    QString expectedSha, actualSha;
    double fraction = 0;
    QString step;
    qint64 elapsedMs = 0, etaMs = 0;
    bool queued = false;
  };

  struct BuildArgs {
    QString gameId, name, presetId, settings, seed;
    bool useSeed = false, spoilerLog = false;
    quint64 requestId = 0;
    QString pluginId;
  };
  QVariantMap startBuild(const BuildArgs& a);
  void onToolPrepared(const ToolPrepareResult& r);
  void onRomReady(const QString& sha, const QString& path);
  void pump();
  void startJob(Job job);
  void continueJob();
  void prepareBase(const GameEntry& game);
  void startBuilder(const QString& basePath);
  void onProgress(const BuildProgress& p);
  void onFinished(const BuildOutcome& o);
  void failActive(const QString& error, const QString& detail = QString());
  void enqueue(Job job);
  void reloadList();
  void rebuildLists();
  void publishPluginState();
  void postPlugin(const Job& j, const QString& state, double progress, const QString& error = QString());
  void refreshEngine();
  void applyLocalRecipe(Job& j, const VariantInfo& v) const;
  VariantRecipe recipeFor(const VariantInfo& v, const GameEntry& game) const;
  QString stateOf(const QString& variantId) const;
  QVariantMap toMap(const VariantInfo& v) const;
  QString platformKey() const;
  QString seedOrRandom(const QString& seed, bool use) const;
  QString runVariantKey(const QString& runId) const { return QStringLiteral("run-") + runId.toLower(); }
  void reportRunState(const QString& runId, const QString& state);
  QString errorText(const QString& code) const;
  void loadRunChoices();
  void saveRunChoices() const;

  Deps deps_;
  std::shared_ptr<VariantsPluginBridge> bridge_;
  VariantBuilder builder_;
  QString gameId_;
  QHash<QString, QList<VariantInfo>> byGame_;
  QHash<QString, bool> loadedOk_;
  QHash<QString, VarState> states_;
  QList<Job> queue_;
  std::optional<Job> active_;
  QVariantMap engine_;
  QVariantMap current_;
  QVariantList variantList_;
  QElapsedTimer activeClock_;
  QHash<QString, RunVariantRecipe> runRecipes_;
  QHash<QString, QString> runChoices_;  // run id -> own variant id
  QHash<QString, QString> runStates_;   // run id -> not_built | building | ready | failed
  QHash<QString, QString> runErrors_;
  double engineReceived_ = 0, engineTotal_ = 0;
  QString engineError_;
  bool engineBusy_ = false;
  quint64 listGen_ = 0;
  bool activeWaitingEngine_ = false;
  bool activeWaitingBase_ = false;
  QString activeBaseSha_;
  int lastPosted_ = -1;
  int registryRevision_ = 0;
};

}  // namespace uzume::ui
