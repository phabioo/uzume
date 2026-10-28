#include "joinlink.h"
#include "playercontroller.h"
#include "envvar.h"
#include "timeformat.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QPalette>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStyleHints>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>
#include <algorithm>
#include <utility>

#include "core_options.h"
#include "diagbundle.h"
#include "videoencoder.h"
#include "filelog.h"
#include "firmware_materializer.h"
#include "installroot.h"
#include "libretro_backend.h"
#include "game_identity.h"
#include "mediacaps.h"
#include "connectiontest.h"
#include "rtcutil.h"
#include "uploadcontroller.h"
#include "semver.h"
#include "version.h"

namespace uzume::ui {

namespace {
// How long the Emulation page keeps the "Updated X to V" line (the test override keeps tests short).
int coreUpdateNoticeMs() {
  bool ok = false;
  const int v = uzume::env::intValue("UZUME_CORE_NOTICE_MS", &ok);
  return ok && v > 0 ? v : 120000;
}
}  // namespace

PlayerController::PlayerController(const Options& options, QObject* parent)
    : QObject(parent), options_(options), locator_() {
  qRegisterMetaType<uzume::RomStatus>("uzume::RomStatus");
  session_.audioOutput().setMixer(&mixer_, AudioMixer::Game);  // the local game is the game channel of the one mixer
  soundBus_ = std::make_shared<PluginSoundBus>(&mixer_);

  profiles_ = std::make_unique<ProfileStore>(options.dataDir);
  if (options.credentialStore) {
    credentials_ = options.credentialStore;
  } else if (options.memoryCredentials) {
    credentials_ = std::make_shared<MemoryCredentialStore>();
  } else {
    credentials_ = createDefaultCredentialStore();
  }
  conn_ = std::make_unique<HubConnection>(profiles_.get(), credentials_.get());
  library_ = std::make_unique<HubLibrary>(conn_.get());
  cache_ = std::make_unique<RomCache>(profiles_->romCacheDir());
  downloader_ = std::make_unique<RomDownloader>(conn_.get(), cache_.get());
  saves_ = std::make_unique<SaveSync>(conn_.get(), profiles_.get());
  settings_ = std::make_unique<PlayerSettings>(profiles_->baseDir());
  discord_ = std::make_unique<DiscordPresence>();
  discord_->setEnabled(settings_->discordPresence());
  mcp_ = std::make_unique<McpController>(settings_.get(), credentials_);
  localRoms_ = std::make_unique<LocalRomIndex>(QDir(profiles_->baseDir()).filePath(QStringLiteral("cache/local-roms.json")));
  registrar_ = std::make_unique<RomRegistrar>(conn_.get());
  connect(localRoms_.get(), &LocalRomIndex::scanProgress, this, [this](int done, int total) {
    libraryScan_.insert(QStringLiteral("done"), done);
    libraryScan_.insert(QStringLiteral("total"), total);
    emit libraryScanChanged();
  });
  connect(localRoms_.get(), &LocalRomIndex::scanFinished, this, [this](int count, int hashed, bool cancelled) {
    Q_UNUSED(hashed)
    libraryScan_ = {{QStringLiteral("active"), false},
                    {QStringLiteral("done"), count},
                    {QStringLiteral("total"), count},
                    {QStringLiteral("isError"), false},
                    {QStringLiteral("text"), cancelled ? tr("Scan cancelled.")
                                             : count == 0 ? tr("No game files found in the library folders.")
                                                          : (count == 1 ? tr("1 game file found.") : tr("%1 game files found.").arg(count))}};
    if (!cancelled) {
      const QDateTime now = QDateTime::currentDateTime();
      for (const QString& f : settings_->libraryFolders())
        if (QFileInfo(f).isDir()) folderScanned_.insert(f, now);   // an unplugged drive keeps its old time
      saveFolderScans();
    }
    emit libraryScanChanged();
    emit libraryFolderInfoChanged();
    if (cancelled) return;
    ignoredRoms_.intersect([this]() {
      QSet<QString> present;
      for (const LocalRomFile& f : localRoms_->files()) present.insert(f.sha256);
      return present;
    }());
    saveIgnoredRoms();
    refreshRomStatuses();
    if (libraryState_ == QLatin1String("ready")) registerLocalFiles();
  });
  connect(registrar_.get(), &RomRegistrar::registered, this, [this](const RegisterOutcome& o) {
    if (!o.ok) failedRegistrations_.insert(o.sha256);
  });
  connect(registrar_.get(), &RomRegistrar::batchFinished, this, [this](int ok, int failed) {
    if (failed > 0) {
      libraryScan_.insert(QStringLiteral("text"), (failed == 1 ? tr("1 file could not be registered with the server.") : tr("%1 files could not be registered with the server.").arg(failed)));
      libraryScan_.insert(QStringLiteral("isError"), true);
      emit libraryScanChanged();
    }
    if (ok > 0) {
      quietRefresh_ = true;
      library_->reload();
    }
  });
  connect(registrar_.get(), &RomRegistrar::unregistered, this, [this](const QString& gameId, bool ok, const QString& message) {
    if (!ok) {
      startError_ = tr("The game could not be removed from the library: %1").arg(message);
      emit selectedGameChanged();
      return;
    }
    removedShas_.clear();
    if (const auto g = library_->gameById(gameId)) {
      for (const RomFileEntry& r : g->roms) {
        if (!localRoms_->filesFor(r.sha256).isEmpty()) {
          ignoredRoms_.insert(r.sha256);
          removedShas_.append(r.sha256);
        }
      }
      saveIgnoredRoms();
    }
    // The Undo is offered when the files can be registered again (they are on this PC).
    if (!removedShas_.isEmpty()) {
      removedInfo_ = {{QStringLiteral("title"), removedTitle_}, {QStringLiteral("gameId"), gameId}};
      emit removedFromLibraryChanged();
    }
    reloadLibrary();
  });
  plugins_ = std::make_unique<PluginsController>(settings_.get(), profiles_->baseDir(), &session_, conn_.get());
  plugins_->setSoundSink(soundBus_);
  // Plugin API emulation (0.20): the safety copy goes through the save sync, later uploads carry the cheats-used marker.
  plugins_->emulation()->setSnapshotFn([this](const QString& label, PluginEmulationService::SnapshotDone done) {
    if (!saves_ || !gameActive_) {
      done(PluginEmulationService::SnapshotOutcome::NoSave, QString());
      return;
    }
    saves_->snapshotActive(label, [done](const SaveSync::SnapshotResult& r) {
      using O = SaveSnapshotResult::Outcome;
      const auto outcome = r.kind == O::Ok ? PluginEmulationService::SnapshotOutcome::Ok
                           : (r.kind == O::Blocked || r.kind == O::NotFound || r.kind == O::Offline) ? PluginEmulationService::SnapshotOutcome::NoSave
                                                                                                    : PluginEmulationService::SnapshotOutcome::Failed;
      done(outcome, r.message);
    });
  });
  plugins_->emulation()->setCheatsUsedMarker([this]() {
    if (saves_) saves_->setCheatsUsed(true);
  });
  loadFolderScans();
  trimRomCache();  // limit lowered while the Player was closed
  // Library sort and "Ready first" are saved per device (3c-2).
  model_.setSortKey(settings_->librarySort());
  model_.setReadyFirst(settings_->libraryReadyFirst());
  connect(&model_, &LibraryModel::sortChanged, this, [this]() {
    settings_->setLibrarySort(model_.sortKey());
    settings_->setLibraryReadyFirst(model_.readyFirst());
  });
  fwCache_ = std::make_unique<FirmwareCache>(QDir(CoreCatalog::systemDirIn(profiles_->baseDir())).filePath(QStringLiteral("firmware")));
  systems_ = std::make_unique<HubSystems>(conn_.get());
  provisioner_ = std::make_unique<FirmwareProvisioner>(conn_.get(), fwCache_.get());
  coreCache_ = std::make_unique<CoreCache>(profiles_->coreCacheDir());
  coreProv_ = std::make_unique<CoreProvisioner>(conn_.get(), coreCache_.get());
  locator_.setCache(coreCache_->root(), CoreCache::currentPlatform());
  probeCoreVersions_ = options.probeCoreVersions;
  coreUpdater_ = std::make_unique<CoreUpdater>(conn_.get(), coreCache_.get());
  // A running game keeps its loaded core: pruning never removes the version it uses while a newer one is staged.
  const auto inUseVersion = [this](const QString& coreId) { return inUseCoreVersion(coreId); };
  coreUpdater_->setInUseProvider(inUseVersion);
  connect(coreUpdater_.get(), &CoreUpdater::statusChanged, this, &PlayerController::coreUpdateChanged);
  connect(coreUpdater_.get(), &CoreUpdater::updated, this, [this](const CoreUpdater::Candidate& c, const CoreResult&) {
    coreProblems_.remove(c.coreId);
    coreReprobe_.insert(c.coreId);
    reprobeUpdatedCores();
    refreshCoreState();
    coreUpdateShown_ = true;
    coreUpdateHideTimer_->start();
    emit coreUpdateChanged();
  });
  // The result line is a quiet notice, not a state: it goes away after a while instead of staying until the next start.
  coreUpdateHideTimer_ = new QTimer(this);
  coreUpdateHideTimer_->setSingleShot(true);
  coreUpdateHideTimer_->setInterval(coreUpdateNoticeMs());
  connect(coreUpdateHideTimer_, &QTimer::timeout, this, [this]() {
    coreUpdateShown_ = false;
    emit coreUpdateChanged();
  });
  // The Hub's systems / cores info changes when it updates its cores (after each catalog refresh): re-read it at most
  // every 30 min (no push event for it) and compare the cached cores with the Hub's versions.
  systemsTimer_ = new QTimer(this);
  systemsTimer_->setInterval(30 * 60 * 1000);
  connect(systemsTimer_, &QTimer::timeout, this, [this]() {
    if (conn_->state() == HubConnection::State::Connected && systems_->state() != HubSystems::State::Loading) {
      systems_->reload();
    }
  });
  systemsTimer_->start();
  uploads_ = std::make_unique<UploadController>(
      UploadController::Deps{conn_.get(), profiles_.get(), model_, localRoms_, manifests_});
  connect(uploads_.get(), &UploadController::changed, this, &PlayerController::uploadChanged);
  connect(uploads_.get(), &UploadController::finished, this, &PlayerController::onUploadFinished);
  report_ = std::make_unique<ReportController>(
      ReportController::Deps{conn_.get(), profiles_->baseDir(),
                             [this]() {
                               ReportController::Deps::CheckInfo c;
                               if (!connectionCheck_ || connectionCheck_->lastResultAt().isEmpty()) return c;
                               c.json = connectionCheck_->lastResultJson().toUtf8();
                               c.at = QDateTime::fromString(connectionCheck_->lastResultAt(), Qt::ISODate);
                               c.marked = connectionCheck_->markedForReport();
                               return c;
                             },
                             [this]() {
                               if (connectionCheck_) connectionCheck_->clearReportMark();
                             },
                             [this]() { return diagnosticsInfo(); }});
  admin_ = std::make_unique<AdminController>(
      AdminController::Deps{conn_.get(), [this]() { return isAdmin(); }, [this]() { return screen_ == QLatin1String("game"); },
                            [this]() { return localHub_ ? localHub_->autoConfirmPassword() : QString(); }});
  connect(admin_.get(), &AdminController::statusChanged, this, &PlayerController::attentionChanged);
  connect(admin_.get(), &AdminController::countsChanged, this, &PlayerController::attentionChanged);
  connect(admin_.get(), &AdminController::availableChanged, this, &PlayerController::attentionChanged);
  connect(this, &PlayerController::hubChanged, this, &PlayerController::attentionChanged);
  connect(&model_, &LibraryModel::countChanged, this, &PlayerController::attentionChanged);
  connect(systems_.get(), &HubSystems::stateChanged, this, [this]() {
    if (systems_->state() == HubSystems::State::Ready) {
      fwProblems_.clear();  // fresh registry from the Hub: validation problems are re-evaluated
      coreProblems_.clear();
      coreUpdater_->check(systems_->systems());
    }
    refreshAttention();
    emit coreWarningsChanged();  // the banner reads the same systems/cores as the game detail
    emit selectedGameChanged();
  });
  connect(this, &PlayerController::hubChanged, this, &PlayerController::coreWarningsChanged);
  connect(&model_, &LibraryModel::countChanged, this, &PlayerController::coreWarningsChanged);  // the notice counts games
  connect(provisioner_.get(), &FirmwareProvisioner::finished, this, [this](const FirmwareResult& r) { starter_->onFirmwareFinished(r); });
  connect(coreProv_.get(), &CoreProvisioner::finished, this, [this](const CoreResult& r) {
    starter_->onCoreFinished(r);
    emit coreWarningsChanged();
  });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  if (QGuiApplication::styleHints() != nullptr) {
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() {
      if (settings_->appearance() == PlayerSettings::Appearance::System) {
        emit appearanceChanged();
      }
    });
  }
#else
  // Qt 6.4 has no color scheme API: "System" follows the application palette (window lightness).
  if (QCoreApplication::instance() != nullptr) {
    QCoreApplication::instance()->installEventFilter(this);
  }
#endif
  progressClock_.start();
  connect(saves_.get(), &SaveSync::startReady, this,
          [this](const QString& gameId, const QString& saveDir, const QString& note) { starter_->onSaveReady(gameId, saveDir, note); });
  connect(saves_.get(), &SaveSync::startConflict, this, [this](const SaveSync::ConflictView& v) { starter_->onSaveConflict(v); });
  connect(saves_.get(), &SaveSync::resolveFailed, this, [this](const QString& msg) {
    conflict_.insert(QStringLiteral("busy"), false);
    conflict_.insert(QStringLiteral("error"), msg);
    emit saveConflictChanged();
  });
  connect(saves_.get(), &SaveSync::startFailed, this, [this](const QString&, const QString& msg) {
    phase_ = PlayPhase::None;
    startError_ = msg;
    emit selectedGameChanged();
  });
  connect(saves_.get(), &SaveSync::coreSaveProblem, this, [this](const QString&, const QString& msg) {
    qWarning().noquote() << msg;
    startError_ = msg;  // shown with the game (detail pane); the save itself stays untouched
    emit selectedGameChanged();
  });
  connect(saves_.get(), &SaveSync::kindsRefreshed, this, [this](const QStringList& changed) {
    applyCachedSyncKinds();
    if (changed.contains(selectedId_)) {
      emit selectedGameChanged();
    }
  });
  connect(saves_.get(), &SaveSync::kindChanged, this, [this](const QString& gameId, SaveSync::Kind k) {
    model_.setSyncKind(gameId, SaveSync::kindName(k));
    if (gameId == selectedId_) {
      emit selectedGameChanged();
    }
  });

  QString err;
  if (!manifests_.loadBuiltin(&err)) {
    qWarning().noquote() << "Manifests not loaded:" << err;
  }
  emulation_ = std::make_unique<EmulationController>(profiles_->baseDir(), &manifests_);
  controllers_ = std::make_unique<ControllersController>(profiles_->baseDir());
  controllers_->start(options.enableGamepads, options.gamepadPollMs);
  plugins_->setManifests(&manifests_);
  if (const QList<emu::SystemManifest> all = manifests_.all(); !all.isEmpty()) {
    // Default until a game starts: the first system in listing order.
    const emu::SystemManifest& first = all.first();
    QVariantList sys;
    for (const emu::SystemManifest& m : all) {
      sys.append(QVariantMap{{QStringLiteral("id"), m.systemId}, {QStringLiteral("label"), m.inputLabel.isEmpty() ? m.displayName : m.inputLabel},
                             {QStringLiteral("inputLabel"), m.inputLabel}, {QStringLiteral("profile"), m.inputProfile}});
    }
    controllers_->setSystems(sys);
    controllers_->setSystemLabels(first.inputLabel, first.display.touchScreenIndex() >= 0 ? first.touchLabel : QString());
    controllers_->setInputProfile(all.first().inputProfile);
  }
  {
    CoreCatalog::Deps cd{conn_.get(), profiles_.get(), systems_.get(), coreCache_.get(), emulation_.get(), manifests_, locator_,
                         coreList_, coreVersions_, coreNames_, coreProblems_, fwProblems_, phase_, launchGame_};
    catalog_ = std::make_unique<CoreCatalog>(cd);
    GameDetail::Deps dd{model_, selectedId_, saves_.get(), cache_.get(), conn_.get(), systems_.get(), fwCache_.get(), catalog_.get(),
                        coreVersions_, fwProblems_, phase_, saveReady_, saveNoteStart_, startError_, hubLinkDown_};
    detail_ = std::make_unique<GameDetail>(dd);
    hubPresenter_ = std::make_unique<HubPresenter>(HubPresenter::Deps{conn_.get(), profiles_.get(), lastError_, inviteBusy_});
    GameStarter::Deps sd{conn_.get(), profiles_.get(), saves_.get(), coreProv_.get(), provisioner_.get(), downloader_.get(),
                         library_.get(), emulation_.get(), catalog_.get(), model_, session_, selectedId_, phase_, launchGame_,
                         launchRom_, saveReady_, saveNoteStart_, startError_, pendingSha_, fwOptions_, shareOnStart_, fwProblems_,
                         coreProblems_, coreList_, handshake_, conflict_, options.probeCoreVersions,
                         [this](const GameEntry& g) { return romSourceOf(g); },
                         [this](const GameEntry& g) { return variantLaunchFor(g); }};
    starter_ = std::make_unique<GameStarter>(sd);
    connect(starter_.get(), &GameStarter::selectedGameChanged, this, &PlayerController::selectedGameChanged);
    connect(starter_.get(), &GameStarter::saveConflictChanged, this, &PlayerController::saveConflictChanged);
    connect(starter_.get(), &GameStarter::coreStateRefreshRequested, this, &PlayerController::refreshCoreState);
    connect(starter_.get(), &GameStarter::gameLaunched, this, [this](const QString& gameId) { recordLastPlayed(gameId); });
    connect(starter_.get(), &GameStarter::systemInputSelected, this,
            [this](const QString& profile, const QString& inputLabel, const QString& touchLabel) {
              controllers_->setSystemLabels(inputLabel, touchLabel);
              controllers_->setInputProfile(profile);
              // Per-game controller profiles for P1: a temporary override, the device assignments stay as they are.
              controllers_->setGameProfiles(
                  emulation_->uzumeValue(QString::fromLatin1(EmulationController::kPadProfileKey), launchGame_.system, launchGame_.id),
                  emulation_->uzumeValue(QString::fromLatin1(EmulationController::kKeyboardProfileKey), launchGame_.system, launchGame_.id));
            });
  }
  if (options.probeCoreVersions) {
    catalog_->probeCores();
  }
  HandshakeInfo info = HandshakeInfo::detect();
  info.cores = coreList_;
  info.gamepad = controllers_->gamepadAvailable();  // input.gamepad: SDL gamepads usable on this Player
  handshake_ = info;
  conn_->setHandshakeInfo(info);
  {
    // h264_encode/h264_decode/encoders as libavcodec can really open them: opening the encoders (GPU drivers) can take
    // long, so it runs on a worker and the handshake is completed and resent when it is done (the result is cached).
    auto* w = new QFutureWatcher<bool>(this);
    connect(w, &QFutureWatcher<bool>::finished, this, [this, w]() {
      w->deleteLater();
      applyMediaCapabilities(&handshake_);  // cached by now
      conn_->setHandshakeInfo(handshake_);
      conn_->resendHandshake();
    });
    w->setFuture(QtConcurrent::run([]() {
      detectMediaCapabilities();
      return true;
    }));
  }
  // The core probes run on a worker: the handshake carries the core list once they are done (a changed list is resent).
  connect(catalog_.get(), &CoreCatalog::coresProbed, this, [this]() {
    const auto same = [](const QList<CoreInfo>& a, const QList<CoreInfo>& b) {
      if (a.size() != b.size()) return false;
      for (int i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].version != b[i].version) return false;
      }
      return true;
    };
    if (!same(handshake_.cores, coreList_)) {
      handshake_.cores = coreList_;
      conn_->setHandshakeInfo(handshake_);
      conn_->resendHandshake();  // the Hub recomputes core_missing / core_version_mismatch
    }
  });
  connect(catalog_.get(), &CoreCatalog::coreProbed, this, [this]() {
    if (!session_.isActive()) {
      refreshEmulationPage();
    }
    refreshAttention();
    emit selectedGameChanged();
  });

  connect(conn_.get(), &HubConnection::handshakeProblemsChanged, this, &PlayerController::hubChanged);

  sessions_ = std::make_unique<SessionController>(conn_.get(), profiles_.get(), &session_);
  // Master volume and mute: one software gain for the local game and one for watched sessions (the share is unaffected).
  audio_ = std::make_unique<AudioController>(settings_.get());
  const auto syncAudio = [this]() {
    // One mixer: master volume and mute for everything, the background mute for the local game only, Voice and Plugin sounds
    // as channel gains (the Game and Tiles channels stay at 1; watched tiles have their own per-person gains).
    mixer_.setMaster(audio_->volume() / 100.0);
    mixer_.setMuted(audio_->muted());
    mixer_.setGameSilenced(audio_->gameSilenced());
    mixer_.setChannelGain(AudioMixer::Voice, audio_->voiceGain() / 100.0);
    mixer_.setChannelGain(AudioMixer::Plugin, audio_->pluginGain() / 100.0);
  };
  const auto syncAudioDevice = [this]() { mixer_.setDeviceId(audio_->deviceIdBytes()); };
  connect(audio_.get(), &AudioController::gainsChanged, this, syncAudio);
  connect(audio_.get(), &AudioController::changed, this, syncAudioDevice);
  syncAudio();
  syncAudioDevice();
  sessions_->setPlayerSettings(settings_.get());  // diagnostics overlay states live in settings/player.json (0.6 D5)
  runClient_ = std::make_unique<RunClient>(conn_.get(), sessions_->socket(), profiles_->baseDir());
  runs_ = std::make_unique<RunsController>(runClient_.get(), plugins_.get(), library_.get(), conn_.get());
  setupStates();
  {
    toolCache_ = std::make_unique<ToolCache>(profiles_->toolCacheDir());
    variantCache_ = std::make_unique<VariantCache>(profiles_->variantCacheDir());
    toolsClient_ = std::make_unique<ToolsClient>(conn_.get(), toolCache_.get());
    connect(admin_.get(), &AdminController::toolsChanged, toolsClient_.get(), &ToolsClient::refresh);  // Server > Tools changed what is offered
    variantsClient_ = std::make_unique<VariantsClient>(conn_.get());
    auto bridge = std::make_shared<VariantsPluginBridge>();
    VariantsController::Deps vd;
    vd.conn = conn_.get();
    vd.library = library_.get();
    vd.settings = settings_.get();
    vd.tools = toolsClient_.get();
    vd.toolCache = toolCache_.get();
    vd.client = variantsClient_.get();
    vd.cache = variantCache_.get();
    vd.downloader = downloader_.get();
    vd.romSource = [this](const GameEntry& g) { return romSourceOf(g); };
    vd.runSlot = [this](const QString& runId) { return runs_->saveSlotFor(runId); };
    vd.runStatus = [this](const QString& runId) {
      const auto r = runClient_->run(runId);
      return r ? r->status : QString();
    };
    vd.runRandomizerMode = [this](const QString& runId) {
      const auto r = runClient_->run(runId);
      return r ? r->randomizerMode() : QString();
    };
    vd.runVariantKind = [this](const QString& runId) {
      const auto r = runClient_->run(runId);
      if (!r) return QString();
      for (const RunParticipant& p : r->participants) {
        if (p.userId == conn_->hubUserId()) return p.variantKind;
      }
      return QString();
    };
    vd.protectedVariantIds = [this]() {
      QSet<QString> ids;
      if (variantStart_.pending() && (gameActive_ || phase_ != PlayPhase::None)) ids.insert(variantStart_.variantId);
      if (runStart_.pending() && (gameActive_ || phase_ != PlayPhase::None)) {
        const QString id = variants_ ? variants_->runVariantId(runStart_.runId) : QString();
        if (!id.isEmpty()) ids.insert(id);
      }
      return ids;
    };
    vd.postPluginEvent = [this](const QString& pluginId, const QString& event, const QVariantMap& value) {
      if (plugins_ && plugins_->host()) plugins_->host()->postHostEvent(event, value, pluginId);
    };
    vd.titleIdOf = [this](const GameEntry& g) { return titleIdOf(g); };
    variants_ = std::make_unique<VariantsController>(vd, bridge);
    runs_->setVariants(variants_.get());
    plugins_->setVariantsProvider(bridge);
    connect(variants_.get(), &VariantsController::launchRequested, this, &PlayerController::startVariantGame);
    // Library status "N variants" / "Building n %" and Continue (10g-1).
    const auto syncBadges = [this]() { model_.setVariantBadges(variants_->summary()); };
    connect(variants_.get(), &VariantsController::variantsChanged, this, syncBadges);
    connect(variants_.get(), &VariantsController::progressChanged, this, syncBadges);
    connect(variants_.get(), &VariantsController::runVariantReady, this, [this](const QString& runId) {
      // A run start waited for its variant: the game of the run starts now (unless "Start when it's ready" was turned off).
      if (pendingRunPlay_.runId == runId) {
        const QString game = std::exchange(pendingRunPlay_.gameId, {});
        const bool start = pendingRunPlay_.startWhenReady;
        pendingRunPlay_ = {};
        refreshRunBuild();
        if (start) startRunGame(runId, game);
      }
    });
    connect(variants_.get(), &VariantsController::runVariantFailed, this, [this](const QString& runId, const QString&) {
      if (pendingRunPlay_.runId == runId) {
        pendingRunPlay_ = {};
        runBuild_ = {};
        if (const auto r = runClient_->run(runId)) {
          runBuild_ = {{QStringLiteral("runId"), runId}, {QStringLiteral("runName"), r->name}, {QStringLiteral("state"), QStringLiteral("failed")},
                       {QStringLiteral("error"), variants_->runVariantState(runId).value(QStringLiteral("error"))}, {QStringLiteral("startWhenReady"), false},
                       {QStringLiteral("background"), false}};
        }
        emit runBuildChanged();
      }
    });
    connect(variants_.get(), &VariantsController::runVariantChanged, this, [this](const QString&) { refreshRunBuild(); });
    connect(variants_.get(), &VariantsController::progressChanged, this, [this]() { refreshRunBuild(); });
  }
  personal_ = std::make_unique<PersonalController>(conn_.get(), &model_);
  screenshots_ = std::make_unique<ScreenshotController>(conn_.get(), profiles_.get());
  screenshots_->setGameSource(
      [this]() { return session_.isActive() ? launchGame_.id : QString(); },
      [this]() { return runStart_.pending() && runStart_.gameId == launchGame_.id ? runStart_.runId : QString(); },
      [this](ScreenshotController::FrameCallback done) { session_.requestNativeFrame(std::move(done)); });
  screenshots_->setRunNameLookup([this](const QString& runId) {
    const auto run = runClient_ ? runClient_->run(runId) : std::nullopt;
    return run ? run->name : QString();
  });
  localStorage_ = std::make_unique<LocalStorageController>([this]() {
    LocalStorageInput in;
    in.dataDir = profiles_->baseDir();
    in.firmwareDir = fwCache_->dir();
    in.variantsDir = variantCache_ ? variantCache_->root() : QString();
    in.roms = *cache_;
    in.cores = *coreCache_;
    in.platform = CoreCache::currentPlatform();
    in.protectedHashes = protectedRomHashes();
    in.statesDir = statesDir();
    in.libraryKnown = conn_->state() == HubConnection::State::Connected && libraryState_ == QLatin1String("ready") && !library_->isLoading();
    if (in.libraryKnown) {
      for (const GameEntry& g : library_->games()) in.libraryGameIds.insert(g.id);
    }
    const emu::SystemManifest* man = session_.isActive() && catalog_ ? catalog_->manifestFor(launchGame_) : nullptr;
    if (man != nullptr) {
      in.inUseCoreId = man->coreId;
      in.inUseCoreVersion = inUseCoreVersion(man->coreId);
    }
    return in;
  });
  connect(localStorage_.get(), &LocalStorageController::gamesRemoved, this, [this]() {
    emit romCacheChanged();
    emit selectedGameChanged();  // a removed ROM turns "ready" back into "download needed"
  });
  connect(&model_, &LibraryModel::collectionsChanged, this, &PlayerController::selectedGameChanged);
  connect(runs_.get(), &RunsController::launchRequested, this, &PlayerController::startRunGame);
  connect(runs_.get(), &RunsController::ownVariantJoined, this, [this](const QString& runId, const QString& variantId) {
    if (variants_) variants_->setRunOwnVariant(runId, variantId);
  });
  connect(runs_.get(), &RunsController::availableChanged, this, [this]() {
    if (page_ == QLatin1String("runs") && !runs_->available()) showLibrary();  // the entry went away
  });
  saves_->setSettings(settings_.get());
  {
    // Saves view inside the game: restore / upload / "use the Hub save" load the save into the running core.
    SaveSync::LiveHooks hooks;
    hooks.ready = [this]() { return session_.liveSaveReady(); };
    hooks.flush = [this]() { session_.flushLiveSave(); };
    hooks.accepts = [this](qint64 size) { return session_.liveSaveAccepts(size); };
    hooks.apply = [this](const QByteArray& data) { return session_.restartWithSave(saves_->activeSaveFile(), data); };
    hooks.restartWith = [this](std::function<bool()> writer) { return session_.restartWith(writer); };
    saves_->setLiveHooks(std::move(hooks));
  }
  {
    saves_->setSharedCheck([this](const QString& id) {
      return sessions_ && sessions_->shared() && (launchGame_.id.isEmpty() || launchGame_.id == id);
    });
    SaveHistoryController::Env env;
    env.saves = saves_.get();
    env.connection = conn_.get();
    env.gameBusy = [this](const QString& id) {
      return (phase_ != PlayPhase::None || gameActive_ || session_.isActive()) && (launchGame_.id.isEmpty() || launchGame_.id == id);
    };
    env.gameTitle = [this](const QString& id) {
      const auto g = model_.game(id);
      return g ? g->title : QString();
    };
    env.localFileName = [this](const QString& id) {
      const auto g = model_.game(id);
      return g ? SaveStore::expectedSaveName(g->romSha256 + QLatin1Char('.') + RomCache::extensionFromFilename(g->romFilename)) : QString();
    };
    env.slotDisplayName = [this](const QString& slot) {
      const QString run = runs_ ? runs_->slotDisplayName(slot) : QString();
      return run.isEmpty() && variants_ ? variants_->slotDisplayName(slot) : run;
    };
    env.slotKind = [this](const QString& slot) {
      if (runs_ && !runs_->slotDisplayName(slot).isEmpty()) return QStringLiteral("run");
      return variants_ && !variants_->slotDisplayName(slot).isEmpty() ? QStringLiteral("variant") : QString();
    };
    history_ = std::make_unique<SaveHistoryController>(env);
    connect(runs_.get(), &RunsController::runsChanged, history_.get(), &SaveHistoryController::onSlotNamesChanged);
    if (variants_) connect(variants_.get(), &VariantsController::cacheChanged, history_.get(), &SaveHistoryController::onSlotNamesChanged);
    connect(this, &PlayerController::selectedGameChanged, history_.get(), [this]() {
      history_->setGame(selectedId_);
      history_->onGameStateChanged();
    });
    connect(sessions_->socket(), &HubSocket::saveUpdated, this, [this](const SaveUpdate& u) { saves_->handleSaveUpdate(u); });
    connect(saves_.get(), &SaveSync::saveChangedElsewhere, history_.get(), &SaveHistoryController::onSaveUpdated);
  }
  connectionCheck_ = std::make_unique<ConnectionCheckController>(
      [this, patch = options.connectionTargetPatch]() {
        ConnectionTarget t;
        const auto profile = conn_->profile();
        if (!profile) return t;
        t.hubName = profile->name;
        t.hubAddress = profile->address;
        t.pinnedFingerprint = profile->pinnedFingerprint;
        t.allowHttp = profile->allowHttp;
        t.deviceName = profiles_->deviceName();
        t.forceRelay = forceRelayFromEnv();
        // The Hub hands out STUN/TURN with hello_ack and ice_update; they are known while the Hub socket is open.
        t.iceKnown = sessions_ && sessions_->socket()->isOpen();
        if (t.iceKnown) {
          t.stunUrls = sessions_->socket()->helloAck().iceServers;
          t.turnServers = sessions_->socket()->helloAck().turnServers;
        }
        if (patch) patch(t);
        return t;
      },
      options.connectionProbes ? options.connectionProbes : std::function<std::unique_ptr<ConnectionProbes>()>([]() { return makeConnectionProbes(); }));
  // The entry points in Settings appear when a Hub profile becomes active or goes away.
  connect(this, &PlayerController::hubsChanged, connectionCheck_.get(), &ConnectionCheckController::changed);
  {
    UpdatesController::Options uo;
    uo.baseDir = profiles_->baseDir();
    uo.indexUrl = options.updateIndexUrl;
    uo.installRoot = update::installRootFor(QCoreApplication::applicationDirPath());
    uo.protocolVersion = kProtocolVersion;
    updates_ = std::make_unique<UpdatesController>(uo, settings_.get());
    // Plugins that need a newer Player than this one are off (manifest field min_player); the rollback dialog warns about them.
    if (updates_->compiledChannel() != QLatin1String("dev")) plugins_->manager()->setPlayerVersion(updates_->currentVersion());
    updates_->setPluginNeedsProvider([this](const QString& v) { return plugins_->manager()->pluginsNeedingNewerPlayer(v); });
    updates_->setProviders(
        [this]() -> std::optional<update::HubProtocol> {
          if (conn_->state() != HubConnection::State::Connected || conn_->hubInfo().protocolVersion <= 0) {
            return std::nullopt;
          }
          return update::HubProtocol{conn_->hubInfo().protocolVersion, conn_->hubInfo().minProtocolVersion};
        },
        [this]() { return sessionBusy(); });
    connect(updates_.get(), &UpdatesController::quitRequested, this, [this]() {
      shutdown();
      QCoreApplication::quit();
    });
  }
  {
    // Error events (ADR 0035): one call from each existing error path; the opt-in decides whether they ever leave this PC.
    ErrorsController::Deps ed;
    ed.settings = settings_.get();
    ed.conn = conn_.get();
    ed.dataDir = profiles_->baseDir();
    ed.source.appVersion = updates_->currentVersion();
    ed.source.os = QSysInfo::prettyProductName();
    ed.serverVersion = [this]() { return conn_->hubInfo().hubVersion; };
    errors_ = std::make_unique<ErrorsController>(ed);
    connect(plugins_.get(), &PluginsController::pluginFailed, errors_.get(), &ErrorsController::recordPluginError);
    connect(saves_.get(), &SaveSync::uploadFailed, errors_.get(),
            [this](const QString&, const QString& code) { errors_->recordSyncFailure(code.isEmpty() ? QStringLiteral("upload failed") : QStringLiteral("upload failed: ") + code); });
    connect(updates_->manager(), &update::UpdateManager::failed, errors_.get(), &ErrorsController::recordUpdateFailure);
  }
  {
    LocalHubController::Deps ld;
    ld.conn = conn_.get();
    ld.addHub = [this](const QString& address) { addHub(address); };
    ld.installer.indexUrl = options.updateIndexUrl;
    ld.installer.currentVersion = updates_->currentVersion();
    ld.installer.installRoot = update::installRootFor(QCoreApplication::applicationDirPath());
    ld.profiles = profiles_.get();
    ld.credentials = credentials_.get();
    ld.settings = settings_.get();
    ld.connectProfile = [this](const QString& hubId) { connectProfile(hubId); };
    // Before the server of this PC is stopped for sharing: end the game and upload the saves while it still answers.
    ld.beforeServerStop = [this]() {
      endRunningWork();
      if (saves_) saves_->finalSyncBlocking(true);
    };
    localHub_ = std::make_unique<LocalHubController>(ld);
    connect(localHub_.get(), &LocalHubController::quitRequested, this, [this]() {
      shutdown();
      QCoreApplication::quit();
    });
    // Paired with the Hub on this PC: show Settings, where its card (network sharing, ROM folder) is.
    connect(localHub_.get(), &LocalHubController::setupCompleted, this, &PlayerController::showSettings);
  }
  {
    OnboardingController::Deps od;
    od.conn = conn_.get();
    od.profiles = profiles_.get();
    od.credentials = credentials_.get();
    od.settings = settings_.get();
    od.allowHttp = options_.allowHttp;
    od.gameRunning = [this]() { return sessionBusy(); };
    od.endRunningWork = [this]() { endRunningWork(); };
    od.addLibraryFolder = [this](const QString& path) { addLibraryFolder(path); };
    od.startLocalHub = [this]() { localHub_->startChild(); };
    onboarding_ = std::make_unique<OnboardingController>(od);
    AccountsController::Deps ad;
    ad.conn = conn_.get();
    ad.profiles = profiles_.get();
    ad.credentials = credentials_.get();
    ad.gameRunning = od.gameRunning;
    ad.endRunningWork = od.endRunningWork;
    accounts_ = std::make_unique<AccountsController>(ad);
    connect(onboarding_.get(), &OnboardingController::changed, this, &PlayerController::updateScreen);
    connect(accounts_.get(), &AccountsController::changed, this, &PlayerController::updateScreen);
  }
  connect(sessions_.get(), &SessionController::watchChanged, this, [this]() {
    // Watching a Session from the Library while a game is in the background shows the game view again (multiview with
    // the paused game); the game stays paused until the user resumes it.
    if (background_ && sessions_->watching()) {
      setBackground(false);
    }
    updateScreen();
  });
  connect(conn_.get(), &HubConnection::stateChanged, this, &PlayerController::onConnectionState);
  connect(conn_.get(), &HubConnection::stateChanged, this, [this]() { refreshHubLink(); });
  connect(sessions_.get(), &SessionController::linkChanged, this, [this]() { refreshHubLink(); });
  connect(sessions_.get(), &SessionController::shareChanged, this, [this]() { updateDiscord(); });
  connect(conn_.get(), &HubConnection::errorOccurred, this, [this](const QString& code, const QString& message) {
    lastError_ = friendlyError(code, message);
    inviteBusy_ = false;
    emit hubsChanged();
    emit pairingChanged();
  });
  connect(library_.get(), &HubLibrary::loaded, this, &PlayerController::onLibraryLoaded);
  connect(library_.get(), &HubLibrary::loadFailed, this, [this](const QString&, const QString& message) {
    if (quietRefresh_) {
      quietRefresh_ = false;  // periodic refresh failed: keep showing the last Library
      return;
    }
    libraryState_ = QStringLiteral("error");
    libraryError_ = message;
    emit libraryStateChanged();
  });
  // Live updates (0.6): while the Library is shown it is refreshed quietly (new games, ROM and sync state) and the
  // core / firmware state is re-evaluated, e.g. after the core cache changed outside the Player.
  liveTimer_ = new QTimer(this);
  liveTimer_->setInterval(30000);
  connect(liveTimer_, &QTimer::timeout, this, [this]() {
    if (conn_->state() != HubConnection::State::Connected || screen_ == QLatin1String("game") || phase_ != PlayPhase::None) {
      return;
    }
    refreshCoreState();
    if (screen_ == QLatin1String("library") && libraryState_ == QLatin1String("ready") && !library_->isLoading()) {
      quietRefresh_ = true;
      library_->reload();
    }
  });
  liveTimer_->start();
  connect(library_.get(), &HubLibrary::cleared, this, [this]() {
    if (personal_) personal_->clear();
    if (screenshots_) screenshots_->clear();
    model_.clear();
    emit selectedGameChanged();
  });
  connect(downloader_.get(), &RomDownloader::statusChanged, this,
          [this](const QString& sha, const RomStatus& st) { onRomStatus(sha, st); });
  connect(downloader_.get(), &RomDownloader::progress, this, [this](const QString& sha, qint64 got, qint64 total) {
    // One signal per network chunk: coalesce to ~10 Hz per ROM (the last chunk always passes).
    const qint64 now = progressClock_.elapsed();
    qint64& last = lastProgressMs_[sha];
    if (got != total && last != 0 && now - last < 100) {
      return;
    }
    last = now + 1;  // never 0, which marks "no update yet"
    RomStatus st;
    st.state = RomState::Downloading;
    st.receivedBytes = got;
    st.totalBytes = total;
    onRomStatus(sha, st);
  });
  connect(downloader_.get(), &RomDownloader::romReady, this,
          [this](const QString& sha, const QString& path) {
            starter_->onRomReady(sha, path);
            trimRomCache();  // the starting game is protected through its phase
          });

  // Input: gamepad P1 and the keyboard profile are merged into the joypad mask of the game.
  session_.setKeyboardMap(controllers_->keyboardMap());
  connect(controllers_.get(), &ControllersController::keyboardMapChanged, this,
          [this]() { session_.setKeyboardMap(controllers_->keyboardMap()); });
  // F4 (hide all HUD widgets) is a fixed Player key like Esc.
  session_.setHotkeyKeys(controllers_->hotkeyKeys() | QSet<int>{Qt::Key_F4});
  connect(controllers_.get(), &ControllersController::hotkeysChanged, this, [this]() {
    session_.setHotkeyKeys(controllers_->hotkeyKeys() | QSet<int>{Qt::Key_F4});
    plugins_->setReservedKeys(controllers_->reservedKeyOwners());
    if (!controllers_->hotkeyListening().isEmpty() || !controllers_->padHotkeyListening().isEmpty()) plugins_->cancelHotkeyCapture();
  });
  // Plugin hotkeys that fire and Alt+1 ... Alt+9 (panel tabs) never reach the core.
  const auto syncCombos = [this]() {
    QSet<int> combos = plugins_->activeHotkeyCombos();
    for (int n = 1; n <= 9; ++n) combos.insert(int(Qt::Key_0) + n + int(Qt::AltModifier));
    session_.setReservedCombos(combos);
  };
  connect(plugins_.get(), &PluginsController::activeHotkeyCombosChanged, this, syncCombos);
  plugins_->setReservedKeys(controllers_->reservedKeyOwners());
  syncCombos();
  connect(controllers_.get(), &ControllersController::libretroMaskChanged, this,
          [this](quint32 mask) { session_.setGamepadMask(mask); });
  session_.setGamepadMask(controllers_->gamepads()->libretroMask());
  // The pad drives the shell (arrows, A, B) unless a game runs unpaused: then the pad belongs to the core.
  controllers_->setPadNavGate([this]() { return screen() != QLatin1String("game") || !session_.isActive() || session_.isPaused(); });
  connect(emulation_.get(), &EmulationController::uzumeOptionsChanged, this, &PlayerController::applyUzumeOptions);
  // The core choice of a system changed: the new core's options, status and the game detail follow at once.
  connect(emulation_.get(), &EmulationController::gameSelectionChanged, this, [this]() { refreshEmulationPage(); });
  connect(emulation_.get(), &EmulationController::coreChoiceChanged, this, [this]() {
    refreshEmulationPage();
    refreshAttention();
    emit selectedGameChanged();
  });
  applyUzumeOptions();
  connect(&session_, &GameSession::stateChanged, this, [this]() {
    emulation_->setGameRunning(session_.isActive());
    emulation_->setRunningGame(session_.isActive() ? launchGame_.id : QString());
    if (!session_.isActive()) controllers_->clearGameProfiles();  // the per-game profiles end with the game
  });
  // Per-game controller profiles (uzume.pad_profile / uzume.keyboard_profile) and the profile lists of the Emulation page.
  emulation_->setControllerProfiles(controllers_->profileStore());
  connect(controllers_.get(), &ControllersController::profilesChanged, emulation_.get(), &EmulationController::profilesChanged);
  connect(emulation_.get(), &EmulationController::gameCoreWanted, this, [this](const QString& gameId) { prepareGameCore(gameId); });

  connect(&session_, &GameSession::started, this, [this]() {
    applyUzumeOptions();
    saves_->beginSession();
    phase_ = PlayPhase::None;
    gameActive_ = true;
    gameStartedMs_ = QDateTime::currentMSecsSinceEpoch();
    sessions_->gameStarted(launchGame_.id, launchGame_.title,
                           runStart_.pending() && runStart_.gameId == launchGame_.id ? runStart_.runId : QString());
    // Screen layout default of the system (Emulation > Defaults / system), the in-game switch overrides it for this game only.
    sessions_->setScreenLayout(emulation_->uzumeValue(QString::fromLatin1(EmulationController::kScreenLayoutKey), launchGame_.system, launchGame_.id));
    if (session_.screenCount() >= 2) {  // swap and the bottom-screen window exist for two-screen systems only
      sessions_->setScreenSwap(emulation_->uzumeValue(QString::fromLatin1(EmulationController::kScreenSwapKey), launchGame_.system, launchGame_.id) == QLatin1String("on"));
      sessions_->setBottomWindow(emulation_->uzumeValue(QString::fromLatin1(EmulationController::kBottomWindowKey), launchGame_.system, launchGame_.id) == QLatin1String("on"));
    }
    applyUzumeOptions();  // picture options of the game (gameActive_ is set now)
    {  // identity, cheat formats and core of the running game for fb.cheats / fb.memory.write
      PluginEmulationService::GameInfo gi;
      gi.id = launchGame_.id;
      gi.title = launchGame_.title;
      gi.system = launchGame_.system;
      gi.sha256 = launchGame_.romSha256;
      const QString coreId = emulation_->launchCoreId(launchGame_.system, launchGame_.id);
      if (const emu::CoreProfile* prof = manifests_.profile(coreId)) {
        gi.cheatFormats = prof->cheatFormatsFor(launchGame_.system);
        gi.coreName = coreNames_.value(coreId, prof->displayName);
      }
      plugins_->setGameInfo(gi);
    }
    stateRunId_ = runStart_.pending() && runStart_.gameId == launchGame_.id ? runStart_.runId : QString();
    updateDiscord();
    states_->gameStarted(stateContext(launchGame_, true));
    applyStateOptions();
    // Plugins start asynchronously after the game: they never delay it, failures only show in the console and the log.
    plugins_->gameStarted(launchGame_.system, launchRom_, launchGame_.romSha256, launchGame_.title,
                          plugins_->resolveLanguage(launchGame_.system, emulation_->launchOverrides(launchGame_.system, launchGame_.id)),
                          runStart_.pending() && runStart_.gameId == launchGame_.id ? runs_->launchFor(runStart_.runId) : RunLaunch());
    if (shareOnStart_) {
      shareOnStart_ = false;
      sessions_->shareSession();
    }
    updateScreen();
    emit selectedGameChanged();
  });
  connect(&session_, &GameSession::stateChanged, this, [this]() {
    if (gameActive_ && session_.state() == GameSession::Paused) {
      saves_->finalSync(false);  // pause = immediate sync of a changed save
    }
  });
  connect(&session_, &GameSession::saveProblem, this, [this](const QString& reason) {
    if (history_) history_->showError(tr("This game's save file cannot be read, so your progress will not be saved over it (%1). Check the file, then restart the game.").arg(reason));
  });
  connect(&session_, &GameSession::saveWriteFailed, this, [this]() {
    if (history_) history_->showError(tr("The save file could not be written. Your latest progress may not be saved."));
  });
  connect(&session_, &GameSession::stopSaveFailed, this, [this](const QString& reason) {
    const QString title = launchGame_.title;
    stopSaveError_ = title.isEmpty() ? tr("The save file could not be written when the game stopped (%1). Your latest progress may not be saved.").arg(reason)
                                     : tr("The save file of %1 could not be written when the game stopped (%2). Your latest progress may not be saved.")
                                           .arg(title, reason);
    emit stopSaveErrorChanged();
  });
  connect(&session_, &GameSession::startFailed, this, [this](const QString& msg) {
    noteCoreFailure(msg);
    saves_->finalSync(true);
    phase_ = PlayPhase::None;
    shareOnStart_ = false;
    const bool fromRun = runStart_.pending();  // endGameContext() ends the run start
    endGameContext();
    startError_ = tr("The emulator could not start: %1").arg(msg);
    if (fromRun) runs_->showLaunchProblem(startError_);  // "Play" on the run page: say why there, not only on the game page
    updateScreen();
    emit selectedGameChanged();
  });
  connect(&session_, &GameSession::errorChanged, this, [this]() {
    // Runtime error in the game: back to the Library, error shown in the detail pane.
    if (gameActive_ && session_.state() == GameSession::Failed) {
      noteCoreFailure(session_.errorText());
      saves_->finalSync(true);
      const bool fromRun = runStart_.pending();
      endGameContext();
      startError_ = tr("The emulator was terminated: %1").arg(session_.errorText());
      if (fromRun) runs_->showLaunchProblem(startError_);
      updateScreen();
      emit selectedGameChanged();
    }
  });
  wireNotifications();
  wireTogether();
  wireControl();
  wireChat();
  wireVoice();
}

// Scope of the running game or the open run board. `chat`: a run that is starting or paused counts too (the Hub allows chat in any run that
// is not over); the ready check and the shared pause keep to active runs.
TogetherController::Scope PlayerController::currentScope(bool chat) const {
  TogetherController::ScopeInputs in;
  const auto usable = [this, chat](const QString& runId, bool needJoined) -> bool {
    const auto run = runClient_->run(runId);
    if (!run || (chat ? run->isOver() : run->status != QLatin1String("active"))) return false;
    const RunParticipant* me = run->participant(conn_->hubUserId());
    return !needJoined || (me && me->status == QLatin1String("joined"));
  };
  // The board of a run I have joined (Runs screen): the same ready check, without a game
  if (screen() == QLatin1String("runs") && !runs_->openRunId().isEmpty() && usable(runs_->openRunId(), true)) in.boardRunId = runs_->openRunId();
  in.gameActive = session_.isActive();
  const QString runId = sessions_->localRunId();
  if (!runId.isEmpty() && in.gameActive && usable(runId, chat ? true : false)) in.gameRunId = runId;
  in.ownSessionId = sessions_->ownSessionId();
  in.watched = sessions_->shownSessionIds();
  in.focus = sessions_->audioFocus();
  return TogetherController::pickScope(in);
}

QString PlayerController::hubUserName(const QString& userId) const {
  if (userId == conn_->hubUserId()) return conn_->userDisplayName();
  for (const OnlineRoster::Person& p : sessions_->roster()->people()) {
    if (p.userId == userId && !p.name.isEmpty()) return p.name;
  }
  const QString runId = sessions_->localRunId();
  if (!runId.isEmpty()) {
    if (const auto run = runClient_->run(runId)) {
      if (const RunParticipant* part = run->participant(userId)) return part->userName;
    }
  }
  return {};
}

// ---------------------------------------------------------------- Together: chat (0.23, ADR 0033 D4)

void PlayerController::wireChat() {
  chatApi_ = std::make_unique<ChatApi>(conn_.get());
  chat_ = std::make_unique<ChatController>(together_.get(), sessions_->socket());
  chat_->setScopeProvider([this]() { return currentScope(true); });
  const auto refreshChat = [this]() { chat_->refresh(); };
  connect(sessions_.get(), &SessionController::shareChanged, chat_.get(), refreshChat);
  connect(sessions_.get(), &SessionController::gameChanged, chat_.get(), refreshChat);
  connect(sessions_.get(), &SessionController::watchChanged, chat_.get(), refreshChat);
  connect(sessions_.get(), &SessionController::surfacesChanged, chat_.get(), refreshChat);
  connect(runClient_.get(), &RunClient::runsChanged, chat_.get(), refreshChat);
  connect(runs_.get(), &RunsController::detailChanged, chat_.get(), refreshChat);
  connect(this, &PlayerController::screenChanged, chat_.get(), refreshChat);
  connect(&session_, &GameSession::stateChanged, chat_.get(), refreshChat);
  chat_->setSelf([this]() { return conn_->hubUserId(); }, [this]() { return conn_->userDisplayName(); });
  chat_->setNameResolver([this](const QString& userId) {
    const QString n = hubUserName(userId);
    if (!n.isEmpty()) return n;
    // A run member that is not online and not in my run: the participants of the open scope are the best source left.
    const QString rid = together_->scopeKind() == QLatin1String("run") ? together_->scopeId() : QString();
    if (!rid.isEmpty()) {
      if (const auto run = runClient_->run(rid)) {
        if (const RunParticipant* part = run->participant(userId)) return part->userName;
      }
    }
    return QString();
  });
  chat_->setScopeInfo(
      [this](const QString& kind, const QString& id) -> QString {
        if (kind != QLatin1String("run")) return {};
        const auto run = runClient_->run(id);
        return run ? run->name : QString();
      },
      [this](const QString& kind, const QString& id) { return scopePeople(kind, id); });
  chat_->setRunEvents([this](const QString& runId) {
    QList<ChatController::EventLine> out;
    const qint64 last = runClient_->lastSeq(runId);
    for (const RunEvent& e : runClient_->events(runId, std::max<qint64>(0, last - 60), 60)) {
      const QString text = e.pluginId.isEmpty() ? runs_->eventSummary(e) : e.text;
      if (text.trimmed().isEmpty()) continue;
      const QDateTime at = QDateTime::fromString(e.createdAt, Qt::ISODate);
      if (!at.isValid()) continue;
      out.append({at.toMSecsSinceEpoch(), e.seq, text});
    }
    return out;
  });
  chat_->setFetch([this](const QString& scope, const QString& id, qint64 afterSeq, int limit, std::function<void(const ChatPage&)> cb) {
    chatApi_->fetch(scope, id, afterSeq, limit, std::move(cb));
  });
  connect(runClient_.get(), &RunClient::eventAdded, chat_.get(), [this](const QString& runId, const RunEvent&) { chat_->noteRunEvent(runId); });
  connect(sessions_.get(), &SessionController::shareChanged, chat_.get(), &ChatController::rowsChanged);
  connect(sessions_->roster(), &OnlineRoster::changed, chat_.get(), &ChatController::rowsChanged);
  for (const auto& sig : {&SessionController::shareChanged, &SessionController::surfacesChanged, &SessionController::surfaceLinksChanged, &SessionController::controlChanged})
    connect(sessions_.get(), sig, chat_.get(), &ChatController::peopleChanged);
  connect(runClient_.get(), &RunClient::runsChanged, chat_.get(), &ChatController::peopleChanged);
  connect(sessions_.get(), &SessionController::gameChanged, this, &PlayerController::gameRunChanged);
  connect(runClient_.get(), &RunClient::runsChanged, this, &PlayerController::gameRunChanged);
  connect(sessions_->roster(), &OnlineRoster::changed, chat_.get(), &ChatController::peopleChanged);
  connect(chat_.get(), &ChatController::scopeChanged, chat_.get(), &ChatController::peopleChanged);
  connect(runClient_.get(), &RunClient::runsChanged, chat_.get(), &ChatController::rowsChanged);
  connect(this, &PlayerController::hubChanged, chat_.get(), [this]() {
    const QString id = conn_->hubId().isEmpty() ? QString() : conn_->hubId() + QLatin1Char('/') + conn_->hubUserId();
    if (id != chatHubId_) {
      chatHubId_ = id;
      chat_->reset();  // another server or person: nothing of the old chat stays
    }
  });
  // A message that names me while its chat is not on screen: the bell, always as a toast (the one chat entry; plain messages stay quiet).
  connect(chat_.get(), &ChatController::mentioned, notifications_.get(), [this](const QString& kind, const QString& id, const QString& from, const QString& text) {
    notifications_->notify(QStringLiteral("chat"), tr("%1: %2").arg(from, ChatController::renderEmotes(text)), tr("OPEN CHAT"),
                           QStringLiteral("chat:") + kind + QLatin1Char(':') + id, true);
  });
}

// ---------------------------------------------------------------- Together: voice chat (0.23 package 7, ADR 0033 D7)

void PlayerController::wireVoice() {
  voice_ = std::make_unique<VoiceController>(sessions_->socket(), &mixer_, settings_.get());
  voice_->setScopeProvider([this]() { return currentScope(true); });
  voice_->setSelf([this]() { return conn_->hubUserId(); }, [this]() { return conn_->deviceId(); });
  voice_->setNameResolver([this](const QString& userId) {
    const QString n = hubUserName(userId);
    if (!n.isEmpty()) return n;
    const QString rid = together_->scopeKind() == QLatin1String("run") ? together_->scopeId() : QString();
    if (!rid.isEmpty()) {
      if (const auto run = runClient_->run(rid)) {
        if (const RunParticipant* part = run->participant(userId)) return part->userName;
      }
    }
    return QString();
  });
  voice_->setServerName([this]() { return hubName(); });
  voice_->setOwnerCheck([this](const QString& sessionId) { return !sessionId.isEmpty() && sessions_->ownSessionId() == sessionId; });
  // Runs join by themselves while I play them; Sessions only when invite-only (a small circle that expects to talk).
  voice_->setAutoJoinCheck([this](const TogetherController::Scope& s) {
    if (s.kind == QLatin1String("run")) return s.hostsGame;
    return s.kind == QLatin1String("session") && sessions_->sessionVisibility(s.id) == QLatin1String("invite_only");
  });
  const auto refresh = [this]() { voice_->refresh(); };
  connect(sessions_.get(), &SessionController::shareChanged, voice_.get(), refresh);
  connect(sessions_.get(), &SessionController::gameChanged, voice_.get(), refresh);
  connect(sessions_.get(), &SessionController::watchChanged, voice_.get(), refresh);
  connect(sessions_.get(), &SessionController::surfacesChanged, voice_.get(), refresh);
  connect(runClient_.get(), &RunClient::runsChanged, voice_.get(), refresh);
  connect(runs_.get(), &RunsController::detailChanged, voice_.get(), refresh);
  connect(this, &PlayerController::screenChanged, voice_.get(), refresh);
  connect(this, &PlayerController::hubChanged, voice_.get(), refresh);
  connect(&session_, &GameSession::stateChanged, voice_.get(), refresh);
  // The People tab shows the voice marks of the rows.
  connect(voice_.get(), &VoiceController::membersChanged, chat_.get(), &ChatController::peopleChanged);
  connect(voice_.get(), &VoiceController::stateChanged, chat_.get(), &ChatController::peopleChanged);
  connect(voice_.get(), &VoiceController::memberJoined, notifications_.get(), [this](const QString& name) {
    notifications_->notify(QStringLiteral("voice"), tr("%1 joined voice").arg(name), {}, {}, true);
  });
  // The hint of the push-to-talk key follows the Hotkeys settings (keyboard or gamepad).
  const auto pttLabel = [this]() {
    const QString l = controllers_->hotkeyLabels().value(QStringLiteral("talk")).toString();
    voice_->setPttKeyLabel(l.isEmpty() ? QStringLiteral("V") : l);
  };
  connect(controllers_.get(), &ControllersController::hotkeysChanged, voice_.get(), pttLabel);
  pttLabel();
}

// The rows of the People tab get the voice marks, the volume of the person and the host's mute; people in the voice room that the list does
// not show yet (other viewers of a Session are only counted) take the place of the "Another viewer" rows or are added.
QVariantList PlayerController::withVoice(const QString& kind, const QString& id, QVariantList rows) const {
  if (!voice_ || !voice_->joined() || voice_->roomScope() != kind || voice_->roomId() != id) return rows;
  const QVariantMap info = voice_->memberInfo();
  const bool canMute = voice_->canMuteOthers();
  QSet<QString> used;
  const auto attach = [&](QVariantMap& row, const QVariantMap& m) {
    const QString uid = m.value(QStringLiteral("userId")).toString();
    used.insert(uid);
    row.insert(QStringLiteral("userId"), uid);
    row.insert(QStringLiteral("inVoice"), true);
    row.insert(QStringLiteral("voice"), m.value(QStringLiteral("voice")));
    row.insert(QStringLiteral("voiceGain"), m.value(QStringLiteral("gain")));
    row.insert(QStringLiteral("mutedByHost"), m.value(QStringLiteral("mutedByHost")));
    row.insert(QStringLiteral("canMute"), canMute && !row.value(QStringLiteral("me")).toBool());
  };
  for (int i = 0; i < rows.size(); ++i) {  // by person
    QVariantMap row = rows[i].toMap();
    const QString uid = row.value(QStringLiteral("userId")).toString();
    if (!uid.isEmpty() && info.contains(uid)) {
      attach(row, info.value(uid).toMap());
      rows[i] = row;
    }
  }
  for (int i = 0; i < rows.size(); ++i) {  // rows without a user id (the viewers a host sees): by name
    QVariantMap row = rows[i].toMap();
    if (!row.value(QStringLiteral("userId")).toString().isEmpty() || row.value(QStringLiteral("placeholder")).toBool()) continue;
    for (auto it = info.cbegin(); it != info.cend(); ++it) {
      const QVariantMap m = it.value().toMap();
      if (used.contains(it.key()) || m.value(QStringLiteral("name")).toString() != row.value(QStringLiteral("name")).toString()) continue;
      attach(row, m);
      rows[i] = row;
      break;
    }
  }
  for (auto it = info.cbegin(); it != info.cend(); ++it) {  // everyone else in the room
    if (used.contains(it.key())) continue;
    const QVariantMap m = it.value().toMap();
    int at = -1;
    for (int i = 0; i < rows.size() && at < 0; ++i) {
      if (rows[i].toMap().value(QStringLiteral("placeholder")).toBool()) at = i;
    }
    QVariantMap row = at >= 0 ? rows[at].toMap() : QVariantMap{{QStringLiteral("role"), kind == QLatin1String("session") ? QStringLiteral("VIEWER") : QString()},
                                                               {QStringLiteral("me"), false}, {QStringLiteral("color"), OnlineRoster::colorOf(it.key())}};
    row.insert(QStringLiteral("name"), m.value(QStringLiteral("name")));
    row.remove(QStringLiteral("placeholder"));
    attach(row, m);
    if (at >= 0) rows[at] = row;
    else rows.append(row);
  }
  return rows;
}

QVariantMap PlayerController::gameRun() const {
  if (!sessions_ || !runClient_) return {};
  const QString id = sessions_->localRunId();
  if (id.isEmpty()) return {};
  const auto run = runClient_->run(id);
  QString status = run ? run->status : QString();
  if (!status.isEmpty()) status[0] = status[0].toUpper();
  return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), run ? run->name : QString()}, {QStringLiteral("status"), status},
                     {QStringLiteral("people"), scopePeople(QStringLiteral("run"), id)}};
}

QVariantList PlayerController::scopePeople(const QString& kind, const QString& id) const {
  if (kind == QLatin1String("run")) {
    QVariantList out;
    const auto run = runClient_->run(id);
    if (!run) return out;
    for (const RunParticipant& p : run->participants) {
      if (p.status != QLatin1String("joined")) continue;
      out.append(QVariantMap{{QStringLiteral("userId"), p.userId}, {QStringLiteral("name"), p.userName}, {QStringLiteral("role"), QString()},
                             {QStringLiteral("me"), p.userId == conn_->hubUserId()}, {QStringLiteral("color"), OnlineRoster::colorOf(p.userId)}});
    }
    return withVoice(kind, id, out);
  }
  return withVoice(kind, id, sessions_->sessionPeople(id));
}

// ---------------------------------------------------------------- Together: ready check and shared pause (0.23, ADR 0033 D3)

void PlayerController::wireTogether() {
  together_ = std::make_unique<TogetherController>(sessions_->socket());
  together_->setSelf([this]() { return conn_->hubUserId(); });
  together_->setNameResolver([this](const QString& userId) { return hubUserName(userId); });
  together_->setScopeProvider([this]() { return currentScope(false); });
  together_->setGameHooks({[this]() { return session_.isPaused(); }, [this]() { session_.pause(); }, [this]() { session_.resume(); }});
  connect(together_.get(), &TogetherController::pauseChanged, this, [this]() { sessions_->setHostPauseBy(together_->hostPauseName()); });
  const auto refresh = [this]() { together_->refresh(); };
  connect(sessions_.get(), &SessionController::shareChanged, this, refresh);
  connect(sessions_.get(), &SessionController::gameChanged, this, refresh);
  connect(sessions_.get(), &SessionController::watchChanged, this, refresh);
  connect(sessions_.get(), &SessionController::surfacesChanged, this, refresh);
  connect(runClient_.get(), &RunClient::runsChanged, this, refresh);
  connect(this, &PlayerController::hubChanged, this, refresh);
  connect(this, &PlayerController::screenChanged, this, refresh);
  connect(runs_.get(), &RunsController::detailChanged, this, refresh);
  connect(&session_, &GameSession::stateChanged, this, [this]() {
    together_->refresh();
    together_->noteLocalPauseChanged();
  });
  controllers_->setPadTogetherGate([this]() { return screen() == QLatin1String("game") && together_->awaitsAnswer(); });
  connect(controllers_.get(), &ControllersController::padCommand, this, [this](const QString& command) {
    if (command == QLatin1String("togetherReady")) together_->answer(true);
    else if (command == QLatin1String("togetherNotYet")) together_->answer(false);
  });
}

// ---------------------------------------------------------------- Controller hand-over (0.23, ADR 0033 D8)

void PlayerController::wireControl() {
  control_ = std::make_unique<ControlController>(sessions_.get(), &session_);
  control_->setPortsProvider([this]() {
    const emu::SystemManifest* man = catalog_ ? catalog_->manifestFor(launchGame_) : nullptr;
    return man != nullptr ? man->inputPorts : 1;
  });
  control_->setKeyboardMap(controllers_->keyboardMap());
  connect(controllers_.get(), &ControllersController::keyboardMapChanged, control_.get(), [this]() { control_->setKeyboardMap(controllers_->keyboardMap()); });
  control_->setPadMask(controllers_->gamepads()->libretroMask());
  connect(controllers_.get(), &ControllersController::libretroMaskChanged, control_.get(), [this](quint32 mask) { control_->setPadMask(mask); });
  const auto reserved = [this]() { control_->setReservedKeys(controllers_->hotkeyKeys() | QSet<int>{Qt::Key_F4}); };
  reserved();
  connect(controllers_.get(), &ControllersController::hotkeysChanged, control_.get(), reserved);
}

// ---------------------------------------------------------------- Together: notifications and the online list (0.23, ADR 0033)

void PlayerController::wireNotifications() {
  notifications_ = std::make_unique<NotificationCenter>(settings_.get());
  connect(notifications_.get(), &NotificationCenter::actionRequested, this, &PlayerController::runNotificationAction);
  // The list belongs to one server and person: another one starts empty.
  connect(this, &PlayerController::hubChanged, this, [this]() {
    const QString id = conn_->hubId().isEmpty() ? QString() : conn_->hubId() + QLatin1Char('/') + conn_->hubUserId();
    if (id.isEmpty() || id == notifyHubId_) return;
    notifyHubId_ = id;
    notifications_->reset();
    notifiedRunInvites_.clear();
    notifiedProblems_.clear();
    notifiedUpdate_.clear();
  });
  connect(sessions_.get(), &SessionController::notice, notifications_.get(), [this](const QString& kind, const QString& text, const QString& label, const QString& data) {
    notifications_->notify(kind, text, label, data);
  });
  // App update available.
  connect(updates_.get(), &UpdatesController::changed, this, [this]() {
    if (!updates_->updateAvailable()) return;
    const QString v = updates_->availableVersion();
    if (v.isEmpty() || v == notifiedUpdate_) return;
    notifiedUpdate_ = v;
    notifications_->notify(QStringLiteral("update"), tr("Uzume %1 is available").arg(v), tr("WHAT'S NEW"), QStringLiteral("settings:updates"));
  });
  // Server problems (admins only: attention() is empty for everyone else); a problem that went away may be news again later.
  connect(this, &PlayerController::attentionChanged, this, [this]() {
    QSet<QString> now;
    for (const QVariant& v : attention()) {
      const QVariantMap m = v.toMap();
      const QString id = m.value(QStringLiteral("id")).toString();
      if (!id.startsWith(QLatin1String("server:"))) continue;
      now.insert(id);
      if (notifiedProblems_.contains(id)) continue;
      notifiedProblems_.insert(id);
      notifications_->notify(QStringLiteral("server_problem"), m.value(QStringLiteral("text")).toString(), tr("OPEN STATUS"),
                             QStringLiteral("attention:") + m.value(QStringLiteral("target")).toString());
    }
    notifiedProblems_.intersect(now);
  });
  // Runs: an invite for me, and status changes by others. What arrives with the first list after a connect is not news.
  connect(runClient_.get(), &RunClient::participantsChanged, this, [this](const QString& runId) {
    const auto run = runClient_->run(runId);
    const RunParticipant* mine = run ? run->participant(conn_->hubUserId()) : nullptr;
    if (mine == nullptr || mine->status != QLatin1String("invited") || run->isOver()) {
      notifiedRunInvites_.remove(runId);
      return;
    }
    if (notifiedRunInvites_.contains(runId)) return;
    notifiedRunInvites_.insert(runId);
    if (runClient_->loaded())
      notifications_->notify(QStringLiteral("run_event"), tr("You are invited to the run \u201C%1\u201D").arg(run->name), tr("OPEN RUN"), QStringLiteral("run:") + runId);
  });
  connect(runClient_.get(), &RunClient::eventAdded, this, [this](const QString& runId, const RunEvent& e) {
    if (!runClient_->loaded() || e.type != QLatin1String("run.status")) return;
    const QDateTime at = QDateTime::fromString(e.createdAt, Qt::ISODate);
    if (!at.isValid() || at.secsTo(QDateTime::currentDateTimeUtc()) > 120) return;  // catch-up of an older event
    const QString by = e.payload.value(QStringLiteral("by_user_name")).toString();
    if (e.userId == conn_->hubUserId() || (!by.isEmpty() && by == conn_->userDisplayName())) return;  // my own change
    const auto run = runClient_->run(runId);
    notifications_->notify(QStringLiteral("run_event"), run ? tr("%1: %2").arg(run->name, runs_->eventSummary(e)) : runs_->eventSummary(e), tr("OPEN RUN"),
                           QStringLiteral("run:") + runId);
  });
  // The online list.
  connect(sessions_->roster(), &OnlineRoster::changed, this, &PlayerController::onlineChanged);
  connect(sessions_.get(), &SessionController::shareChanged, this, &PlayerController::onlineChanged);
  connect(sessions_.get(), &SessionController::gameChanged, this, &PlayerController::onlineChanged);
  connect(runClient_.get(), &RunClient::runsChanged, this, &PlayerController::onlineChanged);
  connect(this, &PlayerController::hubChanged, this, &PlayerController::onlineChanged);
}

QVariantList PlayerController::onlinePeople() const {
  if (!sessions_ || !runClient_) return {};
  QVariantList out = sessions_->roster()->peopleVariant(conn_->hubUserId());
  const QString runId = sessions_->localRunId();
  const auto run = runId.isEmpty() ? std::nullopt : runClient_->run(runId);
  const bool runOpen = run && !run->isOver() && run->participants.size() < kRunMaxParticipants;
  for (QVariant& v : out) {
    QVariantMap m = v.toMap();
    const QString uid = m.value(QStringLiteral("userId")).toString();
    QString kind;
    if (sessions_->canInvite(uid)) kind = QStringLiteral("session");
    else if (runOpen && run->participant(uid) == nullptr) kind = QStringLiteral("run");
    m.insert(QStringLiteral("inviteKind"), kind);
    v = m;
  }
  return out;
}

void PlayerController::watchOnline(const QString& sessionId) { sessions_->watchFromRoster(sessionId); }

void PlayerController::inviteOnline(const QString& userId) {
  if (sessions_->canInvite(userId)) {
    sessions_->invite(userId);
    return;
  }
  const QString runId = sessions_->localRunId();
  if (runId.isEmpty() || userId.isEmpty()) return;
  const QString name = [&]() {
    for (const QVariant& v : sessions_->roster()->peopleVariant())
      if (v.toMap().value(QStringLiteral("userId")).toString() == userId) return v.toMap().value(QStringLiteral("name")).toString();
    return QString();
  }();
  runClient_->inviteUsers(runId, {userId}, [this, name](const RunResult& r) {
    if (!r.ok) notifications_->notify(QStringLiteral("run_event"), tr("Could not invite %1 to the run.").arg(name.isEmpty() ? tr("this person") : name));
  });
}

void PlayerController::runNotificationAction(const QString& kind, const QString& data) {
  Q_UNUSED(kind);
  const int colon = static_cast<int>(data.indexOf(QLatin1Char(':')));
  if (colon <= 0) return;
  const QString what = data.left(colon);
  const QString arg = data.mid(colon + 1);
  if (what == QLatin1String("watch")) {
    sessions_->watchFromRoster(arg);
  } else if (what == QLatin1String("run")) {
    showRuns();
    if (page_ == QLatin1String("runs")) runs_->openRun(arg);
  } else if (what == QLatin1String("chat")) {
    // In the game (or watching): the panel opens on the Chat tab; else the board of the run
    if (gameActive_ || screen() == QLatin1String("game")) {
      if (gameActive_ && background_) resumeGame();
      emit openChatRequested();
    } else if (arg.startsWith(QLatin1String("run:"))) {
      showRuns();
      if (page_ == QLatin1String("runs")) runs_->openRun(arg.mid(4));
    }
  } else if (what == QLatin1String("settings")) {
    showSettingsSection(arg);
  } else if (what == QLatin1String("attention")) {
    openAttention(arg);
  }
}

PlayerController::~PlayerController() {
  mcp_.reset();  // no tool call may reach a half-destroyed controller
  shutdown();
}

QVariantMap PlayerController::runningGameInfo() const {
  if (!gameActive_ && !session_.isActive()) return {};
  const emu::SystemManifest* man = catalog_ ? catalog_->manifestFor(launchGame_) : nullptr;
  return QVariantMap{{QStringLiteral("id"), launchGame_.id},
                     {QStringLiteral("title"), launchGame_.title},
                     {QStringLiteral("system"), launchGame_.system},
                     {QStringLiteral("core"), man != nullptr ? man->coreId : QString()},
                     {QStringLiteral("romPath"), launchRom_},
                     {QStringLiteral("inputProfile"), man != nullptr ? man->inputProfile : QString()}};
}

void PlayerController::endGameContext() {
  setBackground(false);
  const bool wasActive = gameActive_;
  gameActive_ = false;
  if (states_) states_->gameStopped();
  stateRunId_.clear();
  updateDiscord();
  if (wasActive) {
    // The server adds the play time when the app reports that the game ended (presence): read it back a moment later.
    QTimer::singleShot(1500, this, [this]() {
      if (conn_->state() == HubConnection::State::Connected && libraryState_ == QLatin1String("ready") && !library_->isLoading()) {
        quietRefresh_ = true;
        library_->reload();
      }
    });
  }
  finishRunStart();
  if (cache_ && !launchGame_.romSha256.isEmpty()) {
    cache_->touch(launchGame_.romSha256, RomCache::extensionFromFilename(launchGame_.romFilename));  // LRU: last use = end of play
  }
  QTimer::singleShot(0, this, [this]() {
    trimRomCache();
    reprobeUpdatedCores();
  });  // after the session state settled
  if (sessions_) {
    sessions_->gameEnded();  // ends a shared Session
  }
}

// ---- save states, rewind, run-ahead (0.20 package 4)

QString PlayerController::statesDir() const {
  if (!conn_ || !profiles_) return {};
  return StateStore::userStatesDir(*profiles_, conn_->hubId(), conn_->hubUserId());
}

void PlayerController::updateStatesLocation() {
  if (states_) states_->setLocation(statesDir());
}

StatesController::GameContext PlayerController::stateContext(const GameEntry& game, bool running) const {
  StatesController::GameContext sc;
  sc.gameId = game.id;
  sc.title = game.title;
  sc.systemId = game.system;
  QString slot = running ? saves_->activeSlot() : QString();
  if (slot.isEmpty()) slot = saves_->slotFor(game.id);
  sc.saveSlot = slot.isEmpty() ? QStringLiteral("default") : slot;
  const emu::SystemManifest* man = catalog_ ? (running ? catalog_->manifestFor(game) : catalog_->manifestForSystem(game.system, game.id)) : nullptr;
  if (man != nullptr) {
    sc.coreId = man->coreId;
    if (const emu::CoreProfile* prof = manifests_.profile(man->coreId)) {
      sc.states = prof->statesSupported;
      sc.runAhead = prof->statesRunAhead;
    }
    sc.coreVersion = catalog_->locateCore(*man).version;
  }
  if (running) sc.runId = stateRunId_;
  return sc;
}

// Rewind length and run-ahead of the running game: game > system > global (Emulation settings), applied by StatesController when
// the core and the run rules allow them.
void PlayerController::applyStateOptions() {
  if (!states_ || !gameActive_) return;
  bool ok = false;
  int secs = emulation_->uzumeValue(QString::fromLatin1(EmulationController::kRewindKey), launchGame_.system, launchGame_.id).toInt(&ok);
  if (!ok) secs = 0;
  int ahead = emulation_->uzumeValue(QString::fromLatin1(EmulationController::kRunAheadKey), launchGame_.system, launchGame_.id).toInt(&ok);
  if (!ok) ahead = 0;
  states_->applyOptions(secs, ahead);
}

void PlayerController::setupStates() {
  stateApi_ = std::make_unique<StateApi>(conn_.get());
  states_ = std::make_unique<StatesController>(&session_, settings_.get());
  states_->setApi(stateApi_.get());
  states_->setDeviceName(QSysInfo::machineHostName());
  states_->setViewResolver([this](const QString& gameId) {
    const auto g = library_->gameById(gameId);
    return g ? stateContext(*g, false) : StatesController::GameContext();
  });
  states_->setRulesFn([this]() -> std::optional<RunRules> {
    if (stateRunId_.isEmpty() || !runClient_) return std::nullopt;
    const auto run = runClient_->run(stateRunId_);
    if (!run) return std::nullopt;
    return run->rules;
  });
  states_->setRuleOwnerFn([this]() -> QString {
    if (stateRunId_.isEmpty() || !runClient_) return {};
    const auto run = runClient_->run(stateRunId_);
    if (!run) return {};
    for (const RunParticipant& p : run->participants) {
      if (p.userId == run->createdBy) return p.userName;
    }
    return {};
  });
  states_->setRunEventFn([this](const QString& type, const QString& text, const QJsonObject& payload) {
    if (stateRunId_.isEmpty() || !runClient_) return;
    const QString err = runClient_->postEvent(stateRunId_, QString(), QStringLiteral("automatic"), type, text, payload);
    if (!err.isEmpty()) qInfo() << "run log" << type << "not written:" << err;
  });
  connect(runClient_.get(), &RunClient::runChanged, this, [this](const QString& runId) {
    if (runId == stateRunId_) updateDiscord();
  });
  connect(runClient_.get(), &RunClient::runChanged, states_.get(), [this](const QString& runId) {
    if (runId == stateRunId_) states_->rulesChanged();
  });
  connect(conn_.get(), &HubConnection::stateChanged, this, [this]() {
    updateStatesLocation();
    states_->connectionChanged();
  });
  updateStatesLocation();
}

void PlayerController::shutdown() {
  background_ = false;  // app close with a game in the background: same path as a game in the foreground
  if (sessions_) {
    sessions_->gameEnded();
  }
  session_.stop();  // unloads the core first so it flushes its save
  if (liveTimer_) liveTimer_->stop();  // the blocking sync spins a nested event loop: no library reload meanwhile (PE-19)
  if (saves_) {
    saves_->finalSyncBlocking(true);
  }
  // The server of "Play on this PC" goes last: the final sync above needs it.
  if (localHub_) localHub_->stopChild();
}

// ---------------------------------------------------------------- Cores / Handshake

void PlayerController::refreshAttention() {
  QHash<QString, QString> bySystem;
  QHash<QString, QString> byGame;
  for (const GameEntry& g : library_->games()) {
    if (!bySystem.contains(g.system)) {
      bySystem.insert(g.system, catalog_->attentionFor(g));
    }
    const QString a = bySystem.value(g.system);
    if (!a.isEmpty()) {
      byGame.insert(g.id, a);
    }
  }
  model_.setAttention(byGame);
}

QVariantMap PlayerController::coreUpdate() const {
  QVariantMap m;
  const bool active = coreUpdater_ && coreUpdater_->busy();
  m.insert(QStringLiteral("active"), active);
  m.insert(QStringLiteral("text"), active ? tr("Updating %1 to %2").arg(coreUpdater_->current().name, coreUpdater_->current().version) : QString());
  const bool has = coreUpdateShown_ && coreUpdater_ && !coreUpdater_->lastUpdate().coreId.isEmpty();
  m.insert(QStringLiteral("lastText"), has ? tr("Updated %1 to %2").arg(coreUpdater_->lastUpdate().name, coreUpdater_->lastUpdate().version) : QString());
  return m;
}

// Reports the version of updated cores to the Hub (handshake) once no game is running: probing loads the core briefly.
void PlayerController::reprobeUpdatedCores() {
  if (coreReprobe_.isEmpty() || session_.isActive() || phase_ != PlayPhase::None) {
    return;
  }
  const QSet<QString> ids = std::exchange(coreReprobe_, {});
  if (!probeCoreVersions_) {
    return;
  }
  coreList_.removeIf([&](const CoreInfo& c) { return ids.contains(c.id); });
  catalog_->probeCores();  // async (CoreProber); coresProbed resends the handshake with the new versions
}

// The on-demand download at game start takes over for the core of the selected game.
void PlayerController::cancelBackgroundUpdateForSelected() {
  const auto game = model_.game(selectedId_);
  const emu::SystemManifest* man = game ? catalog_->manifestFor(*game) : nullptr;
  if (man != nullptr) {
    coreUpdater_->cancelFor(man->coreId);
  }
}

void PlayerController::refreshCoreState() {
  reprobeUpdatedCores();
  refreshAttention();
  if (!session_.isActive()) {
    refreshEmulationPage();
  }
  emit selectedGameChanged();
}

// Emulation page: system cards (core, readiness, firmware) and the core options (loaded once without a game or from
// the cache of the last capture; not possible while a game runs because only one core can be loaded per process).
// The per-game settings dialog needs the options of the game's core: read them like the Emulation page does (worker thread,
// else the cache of the last capture; not while a game runs).
void PlayerController::prepareGameCore(const QString& gameId) {
  const QString systemId = [&]() {
    for (const QVariant& v : emulation_->games()) {
      if (v.toMap().value(QStringLiteral("id")).toString() == gameId) return v.toMap().value(QStringLiteral("systemId")).toString();
    }
    return QString();
  }();
  const emu::SystemManifest* mp = systemId.isEmpty() ? nullptr : catalog_->manifestForSystem(systemId, gameId);
  if (mp == nullptr || mp->coreId.isEmpty() || emulation_->hasCoreProbe(mp->coreId)) return;
  if (!session_.isActive() && (catalog_->probing(mp->coreId) || catalog_->probeCoreOptions(*mp))) {
    emulation_->setCoreProbing(catalog_->probeBusy());
    return;
  }
  emulation_->loadCoreCache(mp->coreId);
}

void PlayerController::refreshEmulationPage() {
  // The probe runs on a worker thread (CoreProber); the page shows a loading state and refreshes when the result arrives.
  const auto probeCore = [this](const emu::SystemManifest* mp) {
    if (mp == nullptr || mp->coreId.isEmpty() || emulation_->hasCoreProbe(mp->coreId)) {
      return;
    }
    if (!session_.isActive() && (catalog_->probing(mp->coreId) || catalog_->probeCoreOptions(*mp))) {
      return;
    }
    emulation_->loadCoreCache(mp->coreId);
  };
  for (const emu::SystemManifest& sysManifest : manifests_.all()) {
    probeCore(catalog_->manifestForSystem(sysManifest.systemId));
  }
  // The core chosen for the game on the Emulation page (per-game override) may differ from the system's core.
  if (const QString gameId = emulation_->selectedGame(); !gameId.isEmpty()) {
    probeCore(catalog_->manifestForSystem(emulation_->game().value(QStringLiteral("systemId")).toString(), gameId));
  }
  emulation_->setSystems(catalog_->systemCards());
  emulation_->setGameRunning(session_.isActive());
  emulation_->setCoreProbing(catalog_->probeBusy());
}

// ---------------------------------------------------------------- Navigation

QString PlayerController::logFile() const { return QDir::toNativeSeparators(filelog::path()); }

QString PlayerController::userName() const { return conn_->userDisplayName(); }
bool PlayerController::cheatDbSupported() const { return conn_->hubHasFeature(QStringLiteral("cheat_db_v1")); }
bool PlayerController::isAdmin() const { return conn_->userRole() == QLatin1String("admin"); }
// The core of the running launch failed to load or stopped (ADR 0035 error event core_failure).
void PlayerController::noteCoreFailure(const QString& message) {
  if (!errors_) return;
  const QString coreId = emulation_ ? emulation_->launchCoreId(launchGame_.system, launchGame_.id) : QString();
  errors_->recordCoreFailure(coreId, tr("Core failed"), message);
}

QString PlayerController::hubVersion() const { return conn_->hubInfo().hubVersion; }
QString PlayerController::versionRelation() const {
  if (conn_->state() != HubConnection::State::Connected) return QStringLiteral("unknown");
  const auto order = update::compareVersions(conn_->hubInfo().hubVersion, updates_->currentVersion());
  if (!order.has_value()) return QStringLiteral("unknown");
  return *order == 0 ? QStringLiteral("same") : *order < 0 ? QStringLiteral("server_older") : QStringLiteral("app_older");
}

void PlayerController::showSetupAssistant() {
  if (!admin_->available() || setupAssistantOpen_) return;
  setupAssistantOpen_ = true;
  emit setupAssistantChanged();
}

void PlayerController::hideSetupAssistant() {
  if (!setupAssistantOpen_) return;
  setupAssistantOpen_ = false;
  emit setupAssistantChanged();
}

QString PlayerController::deviceName() const { return profiles_->deviceName(); }
QString PlayerController::playerVersion() const { return HandshakeInfo::detect().playerVersion; }
QString PlayerController::platformText() const {
  const HandshakeInfo h = HandshakeInfo::detect();
  return tr("%1 · Protocol v%2").arg(platformLabel(h.platform, h.arch)).arg(kProtocolVersion);
}

void PlayerController::openLogFolder() {
  const QString p = filelog::path();
  if (!p.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).absolutePath()));
}

// ---- Collect logs ----

namespace {
QString localPathOf(const QString& pathOrUrl) {
  const QUrl u(pathOrUrl);
  return u.isLocalFile() ? u.toLocalFile() : pathOrUrl;
}
}  // namespace

QString PlayerController::diagnosticsFileName() const { return diag::fileName(QDateTime::currentDateTimeUtc()); }

QString PlayerController::diagnosticsFolderUrl() const {
  for (const auto loc : {QStandardPaths::DesktopLocation, QStandardPaths::DownloadLocation, QStandardPaths::HomeLocation}) {
    const QString dir = QStandardPaths::writableLocation(loc);
    if (!dir.isEmpty() && QFileInfo(dir).isDir()) return QUrl::fromLocalFile(dir).toString();
  }
  return {};
}

QJsonObject PlayerController::diagnosticsInfo() const {
  QJsonObject info = diag::offlineInfo(profiles_->baseDir());
  info.insert(QStringLiteral("mode"), QStringLiteral("running"));

  // Hubs: saved profiles; the connected one with its handshake data. No tokens, no credential references.
  QJsonArray hubs;
  const bool connected = conn_->state() == HubConnection::State::Connected;
  for (const HubProfile& p : profiles_->profiles()) {
    const bool current = connected && conn_->hubId() == p.hubId;
    QJsonArray features;
    if (current) {
      for (const QString& f : conn_->hubFeatures()) features.append(f);
    }
    hubs.append(QJsonObject{{QStringLiteral("name"), p.name},
                            {QStringLiteral("address"), p.address},
                            {QStringLiteral("hub_version"), current ? conn_->hubInfo().hubVersion : QString()},
                            {QStringLiteral("connected"), current},
                            {QStringLiteral("last_connected"), p.lastConnected.isValid() ? p.lastConnected.toUTC().toString(Qt::ISODate) : QString()},
                            {QStringLiteral("features"), features}});
  }
  info.insert(QStringLiteral("hubs"), hubs);

  QJsonObject emu = info.value(QStringLiteral("emulation")).toObject();
  QJsonArray cores;
  for (const CoreInfo& c : coreList_) cores.append(QJsonObject{{QStringLiteral("core"), c.id}, {QStringLiteral("version"), c.version}});
  if (!cores.isEmpty()) emu.insert(QStringLiteral("cores"), cores);
  info.insert(QStringLiteral("emulation"), emu);

  QJsonObject media = info.value(QStringLiteral("media")).toObject();
  QJsonArray enc;
  for (const QString& e : VideoEncoder::availableEncoders()) enc.append(e);
  media.insert(QStringLiteral("encoders_available"), enc);
  media.insert(QStringLiteral("encoder_last_chosen"), sessions_ ? sessions_->host()->encoderName() : QString());
  info.insert(QStringLiteral("media"), media);

  QJsonArray pads;
  if (controllers_) {
    for (const QVariant& v : controllers_->devices()) {
      const QVariantMap m = v.toMap();
      pads.append(QJsonObject{{QStringLiteral("name"), m.value(QStringLiteral("name")).toString()},
                              {QStringLiteral("kind"), m.value(QStringLiteral("kind")).toString()}});
    }
  }
  info.insert(QStringLiteral("controllers"), QJsonObject{{QStringLiteral("connected"), pads}});
  return info;
}

void PlayerController::collectDiagnostics(const QString& pathOrUrl) {
  const QString out = localPathOf(pathOrUrl);
  if (collectingDiagnostics_ || out.isEmpty()) return;
  collectingDiagnostics_ = true;
  emit collectingDiagnosticsChanged();
  const QJsonObject info = diagnosticsInfo();
  const QString dataDir = profiles_->baseDir();
  // "Add to problem report" of the connection check: the result goes into this bundle, once.
  diag::Options options;
  options.connectionCheckJson = connectionCheck_ ? connectionCheck_->attachmentJson() : QByteArray();
  auto* watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, out, attached = !options.connectionCheckJson.isEmpty()]() {
    const QString err = watcher->result();
    watcher->deleteLater();
    collectingDiagnostics_ = false;
    if (err.isEmpty() && attached && connectionCheck_) connectionCheck_->clearAttachment();
    emit collectingDiagnosticsChanged();
    emit diagnosticsCollected(err.isEmpty(), out, err);
  });
  watcher->setFuture(QtConcurrent::run([info, dataDir, out, options]() {
    QString err;
    return diag::writeBundle(out, info, dataDir, options, &err) ? QString() : (err.isEmpty() ? QStringLiteral("unknown error") : err);
  }));
}

void PlayerController::showInFolder(const QString& pathOrUrl) {
  const QString p = localPathOf(pathOrUrl);
  if (p.isEmpty()) return;
#ifdef Q_OS_WIN
  QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(p)});
#else
  QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(p).absolutePath()));
#endif
}

bool PlayerController::sessionBusy() const {
  return gameActive_ || session_.isActive() || (sessions_ && sessions_->watching());
}

void PlayerController::startup() {
  if (mcp_) mcp_->start();
  if (options_.enableUpdates && updates_) {
    updates_->manager()->start();
  }
  // "Play on this PC": the server of this PC starts first; it is connected when it is the last server (or none is) so the
  // person lands in the library without a click. A pending "Share with others" is finished or rolled back here.
  if (localHub_) {
    const QString childId = settings_->localServerHubId();
    const QString last = profiles_->lastHubId();
    const bool connectChild = !childId.isEmpty() && (last.isEmpty() || last == childId);
    if (localHub_->resumeAtStartup(connectChild)) {
      if (!connectChild && profiles_->autoConnect() && !last.isEmpty() && profiles_->profile(last)) connectProfile(last);
      return;
    }
    // "Share with others": the service of this PC is the person's server, connected the same way.
    if (connectChild && settings_->localServerMode() == QLatin1String("service") && profiles_->profile(childId)) {
      connectProfile(childId);
      return;
    }
  }
  if (!profiles_->autoConnect() || profiles_->lastHubId().isEmpty()) {
    return;
  }
  if (profiles_->profile(profiles_->lastHubId())) {
    connectProfile(profiles_->lastHubId());
  }
}

QString PlayerController::screen() const { return screen_; }

void PlayerController::updateScreen() {
  using S = HubConnection::State;
  QString next;
  const S s = conn_->state();
  if ((gameActive_ && !background_) || (sessions_ && sessions_->watching())) {
    next = QStringLiteral("game");
  } else if ((onboarding_ && onboarding_->takesScreen()) || (accounts_ && accounts_->takesScreen())) {
    next = QStringLiteral("start");
  } else if (s == S::Connected) {
    next = page_;
  } else if (s == S::NeedsTrustConfirmation || s == S::NeedsPairing || s == S::AwaitingApproval || s == S::Denied ||
             s == S::Expired) {
    next = QStringLiteral("pairing");
  } else if ((s == S::Identifying || s == S::Authenticating) && pairingFlow_) {
    next = QStringLiteral("pairing");
  } else {
    next = QStringLiteral("connection");
  }
  if (next != screen_) {
    screen_ = next;
    emit screenChanged();
  }
  if (admin_) admin_->refreshAvailability();  // status polling pauses while a game is shown
}

void PlayerController::onConnectionState(HubConnection::State s) {
  using S = HubConnection::State;
  lastError_.clear();
  inviteBusy_ = false;
  if (s != S::Connected) {
    page_ = QStringLiteral("library");
    fwProblems_.clear();
    scanRequested_ = false;
    failedRegistrations_.clear();
    ignoredRoms_.clear();
    dismissRemovedFromLibrary();
  }

  if (s == S::NeedsTrustConfirmation || s == S::NeedsPairing || s == S::AwaitingApproval || s == S::Denied ||
      s == S::Expired) {
    pairingFlow_ = true;
  } else if (s == S::Disconnected || s == S::Connected || s == S::Unreachable || s == S::Incompatible ||
             s == S::CertificateChanged || s == S::UserDisabled) {
    pairingFlow_ = false;
  }
  if (s == S::Connected) {
    // The first successful connection turns the automatic connection on by default (an explicit choice stays).
    const bool autoBefore = profiles_->autoConnect();
    profiles_->markConnectedOnce();
    if (profiles_->autoConnect() != autoBefore) emit autoConnectChanged();
    if (!resumePage_.isEmpty()) {
      page_ = resumePage_;  // reconnect after "Edit Hub": back where the user was
      resumePage_.clear();
    }
    quietRefresh_ = false;
    libraryState_ = QStringLiteral("loading");
    libraryError_.clear();
    emit libraryStateChanged();
    emit hubChanged();
    library_->reload();
    systems_->reload();  // only with firmware_v1
  } else if (s == S::Disconnected) {
    selectedId_.clear();
    startError_.clear();
    emit selectedGameChanged();
    emit hubChanged();
  }
  updateScreen();
  emit hubsChanged();
  emit pairingChanged();
}

// ---------------------------------------------------------------- 3a Connection

bool PlayerController::autoConnect() const { return profiles_->autoConnect(); }

void PlayerController::setAutoConnect(bool on) {
  if (profiles_->autoConnect() == on) {
    return;
  }
  profiles_->setAutoConnect(on);
  emit autoConnectChanged();
}

QString PlayerController::deviceFooter() const {
  const HandshakeInfo h = HandshakeInfo::detect();
  return tr("This device: %1 · Uzume %2 · %3")
      .arg(profiles_->deviceName(), h.playerVersion, platformLabel(h.platform, h.arch));
}

void PlayerController::addHub(const QString& address) {
  notice_.clear();
  resumePage_.clear();
  if (address.trimmed().isEmpty()) {
    notice_ = tr("Please enter a Server address, e.g. hub.local:8443.");
    emit hubsChanged();
    return;
  }
  conn_->connectToAddress(address, options_.allowHttp);
  // Set after the call: disconnecting the old connection (e.g. after Unreachable) would otherwise reset the flag.
  lastAttemptPairing_ = true;
  pairingFlow_ = conn_->state() == HubConnection::State::Identifying;
  updateScreen();
  emit hubsChanged();
  emit pairingChanged();
}

void PlayerController::connectProfile(const QString& hubId) {
  notice_.clear();
  lastAttemptPairing_ = false;
  pairingFlow_ = false;
  conn_->connectToProfile(hubId);
  updateScreen();
  emit hubsChanged();
}

void PlayerController::retryConnection() {
  conn_->retry();
  using S = HubConnection::State;
  // Paired profile (credential present): never show the pairing status screen.
  const std::optional<HubProfile> prof = conn_->profile();
  const bool paired = prof.has_value() && !prof->credentialRef.isEmpty();
  if (lastAttemptPairing_ && !paired && conn_->state() == S::Identifying) {
    pairingFlow_ = true;  // like addHub: show the status screen again (retry disconnects internally and resets the flag)
    updateScreen();
    emit hubsChanged();
    emit pairingChanged();
  }
}

void PlayerController::removeHub(const QString& hubId) {
  resumePage_.clear();
  if (hubId.isEmpty()) {
    conn_->disconnectFromHub();
  } else {
    if (conn_->profile() && conn_->profile()->hubId == hubId) {
      endRunningWork();  // the current Hub: end Sessions and secure pending saves before the connection goes
    }
    conn_->removeProfile(hubId);
  }
  notice_.clear();
  updateScreen();
  emit hubsChanged();
}

QStringList PlayerController::validateHubAddress(const QString& host, const QString& port) const {
  return ProfileStore::validateHubAddress(host, port);
}

bool PlayerController::editHub(const QString& hubId, const QString& host, const QString& port) {
  if (!ProfileStore::validateHubAddress(host, port).isEmpty() || !profiles_->profile(hubId)) {
    return false;
  }
  const bool active = conn_->state() != HubConnection::State::Disconnected && conn_->profile() && conn_->profile()->hubId == hubId;
  if (active) {
    endRunningWork();  // Sessions end and pending saves are secured before the connection goes
  }
  if (!profiles_->updateHubAddress(hubId, host, port.trimmed().toInt())) {
    return false;
  }
  if (active) {
    resumePage_ = page_;
    conn_->disconnectFromHub();
    connectProfile(hubId);  // pinned fingerprint of the profile still applies: another certificate is never accepted silently
  } else {
    emit hubsChanged();
  }
  return true;
}

// ---------------------------------------------------------------- 3b Pairing

void PlayerController::redeemInvite(const QString& code, const QString& displayName) {
  const QString name = displayName.trimmed();
  QString problem;
  if (code.trimmed().isEmpty()) {
    problem = tr("Please enter the invite code.");
  } else if (name.isEmpty()) {
    problem = tr("Please enter a display name.");
  } else if (name.size() > 32) {
    problem = tr("The display name can have at most 32 characters.");
  }
  if (!problem.isEmpty()) {
    lastError_ = problem;
    emit pairingChanged();
    return;
  }
  lastError_.clear();
  inviteBusy_ = true;
  emit pairingChanged();
  conn_->redeemInvite(code, name);
}

QString PlayerController::friendlyError(const QString& code, const QString& message) const {
  if (code == QLatin1String("invite_invalid")) {
    return tr("This invite code is not valid. It may have expired, been used already or been revoked.");
  }
  if (code == QLatin1String("display_name_taken")) {
    return tr("This display name is already taken on the server. Please choose another one.");
  }
  if (code == QLatin1String("rate_limited")) {
    return tr("Too many attempts. Please wait a moment and try again.");
  }
  if (code == QLatin1String("not_found") && inviteBusy_) {
    return tr("This server does not support invite codes.");
  }
  if (code == QLatin1String("invalid_input")) {
    return tr("Please enter the invite code and a display name of 1 to 32 characters.");
  }
  return message;
}

void PlayerController::confirmTrust() { conn_->confirmTrust(); }
void PlayerController::rejectTrust() { conn_->rejectTrust(); }
void PlayerController::trustChangedCertificate(const QString& observedFingerprint) {
  conn_->confirmCertificateChange(observedFingerprint);
}
void PlayerController::cancelCertificateChange() {
  if (conn_->state() == HubConnection::State::CertificateChanged) {
    conn_->disconnectFromHub();
  }
}
void PlayerController::requestPairing() { conn_->requestPairing(); }
void PlayerController::cancelPairing() { conn_->cancelPairing(); }
void PlayerController::leavePairing() { conn_->disconnectFromHub(); }

// ---------------------------------------------------------------- Settings, upload, warnings

namespace {
QString formatBytes(qint64 bytes) {
  constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
  if (bytes >= static_cast<qint64>(kGiB)) {
    return QStringLiteral("%1 GB").arg(QLocale::c().toString(static_cast<double>(bytes) / kGiB, 'f', 1));
  }
  return LibraryModel::formatSize(bytes);
}
}  // namespace

// A running game keeps its loaded core: clean-up and pruning never remove the version it uses.
QString PlayerController::inUseCoreVersion(const QString& coreId) const {
  if (!session_.isActive()) {
    return {};
  }
  const emu::SystemManifest* man = catalog_ ? catalog_->manifestFor(launchGame_) : nullptr;
  if (man == nullptr || man->coreId != coreId) {
    return {};
  }
  return catalog_->locateCore(*man).version;
}

QSet<QString> PlayerController::protectedRomHashes() const {
  QSet<QString> keep = downloader_ ? downloader_->activeHashes() : QSet<QString>();
  if (!pendingSha_.isEmpty()) keep.insert(pendingSha_);
  if (!launchGame_.romSha256.isEmpty() && (phase_ != PlayPhase::None || gameActive_ || session_.isActive())) keep.insert(launchGame_.romSha256);
  return keep;
}

void PlayerController::trimRomCache() {
  if (!cache_ || !settings_) return;
  const RomCache::TrimResult r = cache_->trimToLimit(settings_->romCacheLimitBytes(), protectedRomHashes());
  if (r.removedFiles > 0) {
    qInfo() << "ROM cache trimmed:" << r.removedFiles << "file(s)," << r.freedBytes << "bytes freed";
    emit romCacheChanged();
    emit selectedGameChanged();  // a removed ROM turns "ready" back into "download needed"
  }
}

QVariantMap PlayerController::romCache() const {
  const qint64 limit = settings_->romCacheLimitBytes();
  const qint64 used = cache_->totalSize();
  const qint64 clearable = cache_->clearableSize(protectedRomHashes());
  QVariantList options;
  QList<qint64> values{0, 5, 10, 20, 50, 100, 200};
  for (qint64& v : values) v = v * 1024 * 1024 * 1024;
  if (!values.contains(limit)) {
    values.append(limit);
    std::sort(values.begin(), values.end());
  }
  for (qint64 v : std::as_const(values)) {
    options.append(QVariantMap{{QStringLiteral("value"), QString::number(v)},
                               {QStringLiteral("label"), v == 0 ? tr("Unlimited") : formatBytes(v)}});
  }
  return {{QStringLiteral("limitBytes"), static_cast<double>(limit)},
          {QStringLiteral("limitText"), limit == 0 ? tr("Unlimited") : formatBytes(limit)},
          {QStringLiteral("usedBytes"), static_cast<double>(used)},
          {QStringLiteral("usedText"), formatBytes(used)},
          {QStringLiteral("clearableBytes"), static_cast<double>(clearable)},
          {QStringLiteral("clearableText"), formatBytes(clearable)},
          {QStringLiteral("files"), static_cast<int>(cache_->entries().size())},
          {QStringLiteral("limitOptions"), options}};
}

void PlayerController::setRomCacheLimit(double bytes) {
  const qint64 b = bytes < 0 ? 0 : static_cast<qint64>(bytes);
  if (!settings_->setRomCacheLimitBytes(b)) {
    qWarning() << "Player settings could not be written";
  }
  trimRomCache();
  emit romCacheChanged();
}

QVariantMap PlayerController::clearRomCache() {
  const RomCache::TrimResult r = cache_->clear(protectedRomHashes());
  emit romCacheChanged();
  emit selectedGameChanged();
  return {{QStringLiteral("freedBytes"), static_cast<double>(r.freedBytes)},
          {QStringLiteral("freedText"), formatBytes(r.freedBytes)},
          {QStringLiteral("removedFiles"), r.removedFiles}};
}

QString PlayerController::appearance() const { return PlayerSettings::appearanceName(settings_->appearance()); }

void PlayerController::setAppearance(const QString& name) {
  const PlayerSettings::Appearance a = PlayerSettings::parseAppearance(name, settings_->appearance());
  if (a == settings_->appearance()) {
    return;
  }
  if (!settings_->setAppearance(a)) {
    qWarning() << "Player settings could not be written";
  }
  emit appearanceChanged();
}

QString PlayerController::textSize() const { return settings_->textSize(); }

void PlayerController::setTextSize(const QString& size) {
  if (size == settings_->textSize()) {
    return;
  }
  if (!settings_->setTextSize(size)) {
    if (size != QLatin1String("100") && size != QLatin1String("125") && size != QLatin1String("tv")) {
      return;  // unknown value: ignored
    }
    qWarning() << "Player settings could not be written";
  }
  emit textSizeChanged();
}

// Qt exposes no "reduce motion" system hint (QStyleHints has none), so the system value is false.
bool PlayerController::reduceMotion() const { return settings_->reduceMotion().value_or(false); }

void PlayerController::setReduceMotion(bool on) {
  if (settings_->reduceMotion().has_value() && *settings_->reduceMotion() == on) {
    return;
  }
  if (!settings_->setReduceMotion(on)) {
    qWarning() << "Player settings could not be written";
  }
  emit reduceMotionChanged();
}

bool PlayerController::discordPresence() const { return settings_->discordPresence(); }

void PlayerController::setDiscordPresence(bool on) {
  if (settings_->discordPresence() == on) return;
  if (!settings_->setDiscordPresence(on)) qWarning() << "Player settings could not be written";
  discord_->setEnabled(on);
  updateDiscord();
  emit discordPresenceChanged();
}

// Game title, system name and "· Session" or the run name: nothing else leaves the app (no server address, people, paths).
void PlayerController::updateDiscord() {
  if (!discord_) return;
  if (!gameActive_ || launchGame_.id.isEmpty()) {
    discord_->setActivity(std::nullopt);
    return;
  }
  const emu::SystemManifest* man = catalog_ ? catalog_->manifestFor(launchGame_) : nullptr;
  QString state = man != nullptr && !man->displayName.isEmpty() ? man->displayName : launchGame_.system;
  const auto run = !stateRunId_.isEmpty() && runClient_ ? runClient_->run(stateRunId_) : std::nullopt;
  if (run && !run->name.isEmpty()) state += QStringLiteral(" · ") + run->name;
  else if (sessions_ && sessions_->shared()) state += QStringLiteral(" · Session");
  discord_->setActivity(DiscordActivity{launchGame_.title, state, gameStartedMs_});
}

bool PlayerController::darkMode() const {
  switch (settings_->appearance()) {
    case PlayerSettings::Appearance::Dark: return true;
    case PlayerSettings::Appearance::Light: return false;
    case PlayerSettings::Appearance::System: break;
  }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  if (const QStyleHints* hints = QGuiApplication::styleHints()) {
    return hints->colorScheme() != Qt::ColorScheme::Light;  // Unknown -> dark
  }
#endif
  return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;  // Qt 6.4: derive from the palette
}

bool PlayerController::eventFilter(QObject* watched, QEvent* event) {
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
  if (event->type() == QEvent::ApplicationPaletteChange && watched == QCoreApplication::instance() &&
      settings_->appearance() == PlayerSettings::Appearance::System) {
    emit appearanceChanged();
  }
#endif
  return QObject::eventFilter(watched, event);
}

void PlayerController::showLibrary() {
  // The Library entry again while the game page is open leads to the list; coming back from another screen keeps the page.
  if (screen_ == QLatin1String("library")) closeGamePage();
  page_ = QStringLiteral("library");
  updateScreen();
}

void PlayerController::openGamePage(const QString& gameId, const QString& tab) {
  if (!gameId.isEmpty()) selectGame(gameId);
  if (selectedId_.isEmpty()) return;
  static const QStringList tabs{QStringLiteral("overview"), QStringLiteral("saves"), QStringLiteral("settings"), QStringLiteral("manage")};
  const QString want = tabs.contains(tab) ? tab : QStringLiteral("overview");
  gamePageOpen_ = true;
  gamePageTab_ = want;
  if (page_ != QLatin1String("library")) showLibrary();
  emit gamePageChanged();
}

void PlayerController::closeGamePage() {
  if (!gamePageOpen_) return;
  gamePageOpen_ = false;
  gamePageTab_ = QStringLiteral("overview");
  emit gamePageChanged();
}

void PlayerController::showSettings() {
  if (conn_->state() != HubConnection::State::Connected || (gameActive_ && !background_)) {
    return;
  }
  page_ = QStringLiteral("settings");
  updateScreen();
}

void PlayerController::showSettingsSection(const QString& section) {
  if (section.startsWith(QLatin1String("server:"))) {
    const QString page = section.mid(7);
    if (isAdmin() && admin_->available()) {
      setServerSettingsPage(page);
      showServerPage(QStringLiteral("settings"));
      return;
    }
    showSettings();
    emit settingsSectionRequested(page);  // no admin area: the This PC section of the same name
    return;
  }
  showSettings();
  emit settingsSectionRequested(section);
}

void PlayerController::showEmulation() {
  if (conn_->state() != HubConnection::State::Connected || (gameActive_ && !background_)) {
    return;
  }
  refreshEmulationPage();
  showSettingsSection(QStringLiteral("emulation"));  // Emulation is a section of Settings
}

void PlayerController::showServerPage(const QString& page) {
  if (conn_->state() != HubConnection::State::Connected || (gameActive_ && !background_)) {
    return;
  }
  static const QStringList pages{QStringLiteral("overview"), QStringLiteral("people"), QStringLiteral("games"),   QStringLiteral("systems"),
                                 QStringLiteral("tools"),    QStringLiteral("plugins"),  QStringLiteral("reports"), QStringLiteral("settings")};
  QString next = page;
  if (next == QLatin1String("status")) next = QStringLiteral("overview");  // the pages before the tabs
  else if (next == QLatin1String("users")) next = QStringLiteral("people");
  if (!pages.contains(next)) next = QStringLiteral("overview");
  if (next != serverPage_) {
    serverPage_ = next;
    emit serverPageChanged();
  }
  page_ = QStringLiteral("server");
  updateScreen();
  admin_->refreshStatus();
}

void PlayerController::setServerSettingsPage(const QString& page) {
  static const QStringList pages{QStringLiteral("general"), QStringLiteral("network"), QStringLiteral("signin"), QStringLiteral("updates"),
                                 QStringLiteral("cheatdb"), QStringLiteral("backups"), QStringLiteral("storage"), QStringLiteral("diagnostics")};
  const QString next = pages.contains(page) ? page : QStringLiteral("general");
  if (next == serverSettingsPage_) return;
  serverSettingsPage_ = next;
  emit serverSettingsPageChanged();
}

void PlayerController::showControllers() {
  if (conn_->state() != HubConnection::State::Connected || (gameActive_ && !background_)) {
    return;
  }
  showSettingsSection(QStringLiteral("controllers"));  // Controls is a section of Settings
}

void PlayerController::openPluginScreen(const QString& pluginId) {
  if (conn_->state() != HubConnection::State::Connected || (gameActive_ && !background_)) {
    return;  // in a game the game view shows the sheet itself
  }
  if (page_ == QLatin1String("runs") && runs_->selectPluginTab(pluginId)) {
    return;  // the plugin is the opened run's view: its tab shows the screen
  }
  for (const QVariant& e : plugins_->sidebarEntries()) {  // the plugin runs here and declares a screen
    if (e.toMap().value(QStringLiteral("pluginId")).toString() == pluginId) {
      emit pluginScreenRequested(pluginId);
      return;
    }
  }
}

void PlayerController::showPluginSettings(const QString& pluginId) {
  if (conn_->state() != HubConnection::State::Connected) {
    return;
  }
  if (gameActive_ && !background_) leaveGameView();
  showSettings();
  emit pluginSettingsRequested(pluginId);
}

void PlayerController::showDeveloperConsole() {
  if (!plugins_->developerMode()) return;
  if (gameActive_ && !background_) {
    emit gameConsoleRequested();
    return;
  }
  if (conn_->state() != HubConnection::State::Connected) return;
  showSettingsSection(QStringLiteral("plugins"));
  emit developerConsoleRequested();
}

// System of the game being launched, else of the selected game, else of the first known system.
QString PlayerController::currentSystemId() const {
  if (!launchGame_.system.isEmpty()) {
    return launchGame_.system;
  }
  if (const auto g = model_.game(selectedId_); g && !g->system.isEmpty()) {
    return g->system;
  }
  const QList<emu::SystemManifest> all = manifests_.all();
  return all.isEmpty() ? QString() : all.first().systemId;
}

bool PlayerController::fullscreenOnStart() const {
  const QString sys = currentSystemId();
  return emulation_->uzumeValue(QString::fromLatin1(EmulationController::kFullscreenKey), sys, launchGame_.id) ==
         QLatin1String("on");
}

void PlayerController::applyUzumeOptions() {
  // Default multiview: applied immediately (not while the user changes it in a running Session view).
  if (sessions_ && !gameActive_) {
    const QString sys = currentSystemId();
    sessions_->setMultiviewMode(emulation_->uzumeValue(QString::fromLatin1(EmulationController::kMultiviewKey), sys));
  }
  // Picture options apply live to the running game (game > system > global).
  if (sessions_ && gameActive_) {
    const QString sys = launchGame_.system;
    sessions_->setPictureOptions(
        emulation_->uzumeValue(QString::fromLatin1(EmulationController::kIntegerScaleKey), sys, launchGame_.id) == QLatin1String("on"),
        emulation_->uzumeValue(QString::fromLatin1(EmulationController::kAspectRatioKey), sys, launchGame_.id));
  }
  if (gameActive_) applyStateOptions();
  emit emulationSettingsChanged();
}

bool PlayerController::canUpload() const { return uploads_->canUpload(); }

QStringList PlayerController::uploadFilters() const { return uploads_->filters(); }

QVariantList PlayerController::hubs() const { return hubPresenter_->hubs(); }
QVariantMap PlayerController::pairing() const { return hubPresenter_->pairing(); }
QVariantMap PlayerController::bottomWindowGeometry() const {
  if (!settings_) return {};
  const PlayerSettings::WindowGeometry g = settings_->bottomWindowGeometry();
  if (!g.valid()) return {};
  return {{QStringLiteral("x"), g.x}, {QStringLiteral("y"), g.y}, {QStringLiteral("width"), g.width}, {QStringLiteral("height"), g.height}, {QStringLiteral("screen"), g.screen}};
}

void PlayerController::saveBottomWindowGeometry(int x, int y, int width, int height, const QString& screenName) {
  if (!settings_) return;
  PlayerSettings::WindowGeometry g;
  g.x = x;
  g.y = y;
  g.width = width;
  g.height = height;
  g.screen = screenName;
  settings_->setBottomWindowGeometry(g);
}

QString PlayerController::whenText(const QString& iso) const { return TimeFormat::whenIso(iso); }
QString PlayerController::fullTimeText(const QString& iso) const { return TimeFormat::fullIso(iso); }
QString PlayerController::formatFingerprint(const QString& fp) { return HubPresenter::formatFingerprint(fp); }
QString PlayerController::platformLabel(const QString& platform, const QString& arch) {
  return HubPresenter::platformLabel(platform, arch);
}

QVariantList PlayerController::coreWarnings() const {
  // One entry per kind, not per system and core (round 3, P10): cores the Hub delivers at the first start are one
  // dismissible notice, grouped by core ("mGBA (GB, GBC, GBA) · Azahar (3DS)"); a core the Hub cannot deliver and that
  // is not on this device is a real problem (grouped as well); a version mismatch is listed once per core.
  struct CoreRef {
    QString id;
    QString label;
    QStringList systems;
    int games = 0;  // library games of this core's systems
  };
  QList<CoreRef> download;
  QList<CoreRef> unavailable;
  QVariantList mismatches;
  QSet<QString> seen;
  const auto refFor = [this](const HandshakeProblem& p) {
    CoreRef r;
    r.id = p.coreId;
    r.label = p.coreId;
    if (const emu::CoreProfile* prof = manifests_.profile(p.coreId)) {
      r.label = coreNames_.value(p.coreId, prof->displayName);
      for (const QString& sys : prof->systemIds) {
        r.systems << sys.toUpper();
        r.games += model_.gamesForSystem(sys);
      }
    }
    if (r.label.isEmpty()) r.label = tr("Core");
    return r;
  };
  for (const HandshakeProblem& p : conn_->handshakeProblems()) {
    const bool missing = p.code == QLatin1String("core_missing");
    if (!missing && p.code != QLatin1String("core_version_mismatch")) {
      continue;
    }
    // The handshake report is a snapshot from connect time (before a core was downloaded, or under an id the Hub
    // does not alias): a core that is usable on this device is not "missing" here.
    if (missing && catalog_->coreInstalledLocally(p.coreId)) {
      continue;
    }
    // The server's systems list is being reloaded: whether it offers the core is not known yet, so no verdict (the banner
    // would contradict the game detail); coreWarningsChanged fires when the list is Ready. A failed load is no verdict either
    // (the game detail says "Status unavailable" then).
    if (missing && systems_->supported() && (systems_->state() == HubSystems::State::Loading || systems_->state() == HubSystems::State::Failed)) {
      continue;
    }
    const QString key = p.code + QLatin1Char('|') + p.coreId;
    if (seen.contains(key)) continue;
    seen.insert(key);
    const CoreRef ref = refFor(p);
    if (!missing) {
      mismatches.append(QVariantMap{{QStringLiteral("code"), p.code},
                                    {QStringLiteral("coreId"), p.coreId},
                                    {QStringLiteral("text"), tr("%1 has a different version than this server expects. Games can still start, "
                                                                "but may behave differently.").arg(ref.label)},
                                    {QStringLiteral("detail"), p.detail}});
      continue;
    }
    bool offered = false;
    if (const emu::CoreProfile* prof = manifests_.profile(p.coreId)) {
      for (const QString& sys : prof->systemIds) {
        if (const emu::SystemManifest* man = catalog_->manifestForSystem(sys)) offered = offered || catalog_->hubOffersCore(*man);
      }
    }
    (offered ? download : unavailable).append(ref);
  }
  const auto describe = [](const QList<CoreRef>& cores) {
    QStringList parts;
    for (const CoreRef& c : cores) parts << (c.systems.isEmpty() ? c.label : QStringLiteral("%1 (%2)").arg(c.label, c.systems.join(QStringLiteral(", "))));
    return parts.join(QStringLiteral(" · "));
  };
  // Games per kind (a system shared by two cores counts once).
  const auto gamesOf = [this](const QList<CoreRef>& cores) {
    QSet<QString> systems;
    for (const CoreRef& c : cores) {
      for (const QString& sys : c.systems) systems.insert(sys);
    }
    int n = 0;
    for (const QString& sys : std::as_const(systems)) n += model_.gamesForSystem(sys);
    return n;
  };
  QVariantList list;
  // Wording follows the core state per game, like the game detail: a core the server delivers installs on first play (info,
  // only when a game needs it); only a core that is neither on this device nor offered is a problem.
  if (const int games = gamesOf(download); games > 0 && !dismissedCoreNotices_.contains(QStringLiteral("core_download"))) {
    list.append(QVariantMap{{QStringLiteral("code"), QStringLiteral("core_download")},
                            {QStringLiteral("coreId"), download.first().id},
                            {QStringLiteral("tone"), QStringLiteral("info")},
                            {QStringLiteral("games"), games},
                            {QStringLiteral("text"), games == 1 ? tr("1 game needs a core that is not installed yet. It installs on first play.")
                                                                : tr("%1 games need a core that is not installed yet. They install on first play.").arg(games)},
                            {QStringLiteral("detail"), describe(download)}});
  }
  if (!unavailable.isEmpty()) {
    const int games = gamesOf(unavailable);
    const QString cores = describe(unavailable);
    list.append(QVariantMap{{QStringLiteral("code"), QStringLiteral("core_missing")},
                            {QStringLiteral("coreId"), unavailable.first().id},
                            {QStringLiteral("tone"), QStringLiteral("warn")},
                            {QStringLiteral("games"), games},
                            {QStringLiteral("text"), games == 0 ? tr("The core %1 is not installed on this device and the server does not provide it.").arg(cores)
                                                     : games == 1 ? tr("1 game cannot start: the core %1 is not installed on this device and the server does not provide it.").arg(cores)
                                                                  : tr("%1 games cannot start: the core %2 is not installed on this device and the server does not provide it.").arg(games).arg(cores)},
                            {QStringLiteral("detail"), QString()}});
  }
  for (const QVariant& m : mismatches) list.append(m);
  return list;
}

int PlayerController::coreProblemCount() const {
  if (!isAdmin() || !admin_->available()) return 0;
  int n = 0;
  for (const QVariant& w : coreWarnings()) n += w.toMap().value(QStringLiteral("tone")).toString() == QLatin1String("info") ? 0 : 1;
  return n;
}

bool PlayerController::serverUpdatePending() const {
  return isAdmin() && admin_->available() && !admin_->serverUpdateVersion().isEmpty();
}

QVariantList PlayerController::attention() const {
  QVariantList list;
  if (!isAdmin() || !admin_->available()) return list;
  const auto add = [&list](const QString& id, const QString& severity, const QString& text, const QString& label, const QString& target) {
    list.append(QVariantMap{{QStringLiteral("id"), id},
                            {QStringLiteral("severity"), severity},
                            {QStringLiteral("text"), text},
                            {QStringLiteral("actionLabel"), label},
                            {QStringLiteral("target"), target}});
  };
  // Problems the server reports, most urgent first. The action opens the page that fixes it.
  for (const QVariant& v : admin_->status().value(QStringLiteral("problems")).toList()) {
    const QVariantMap p = v.toMap();
    const QString id = p.value(QStringLiteral("id")).toString();
    const QString detail = p.value(QStringLiteral("detail")).toString();
    QString target = QStringLiteral("settings:server:general");
    if (id == QLatin1String("turn")) target = QStringLiteral("settings:server:network");
    else if (id == QLatin1String("update") || id == QLatin1String("update-paused")) target = QStringLiteral("settings:server:updates");
    else if (id == QLatin1String("backup")) target = QStringLiteral("settings:server:backups");
    else if (id == QLatin1String("disk")) target = QStringLiteral("settings:server:storage");
    QString label = p.value(QStringLiteral("action_label")).toString();
    if (label.isEmpty()) label = tr("Open server settings");
    add(QStringLiteral("server:") + id, p.value(QStringLiteral("severity")).toString() == QLatin1String("error") ? QStringLiteral("error") : QStringLiteral("warn"),
        detail.isEmpty() ? p.value(QStringLiteral("title")).toString() : detail, label, target);
  }
  for (const QVariant& w : coreWarnings()) {
    const QVariantMap m = w.toMap();
    if (m.value(QStringLiteral("tone")).toString() == QLatin1String("info")) continue;
    add(QStringLiteral("core:") + m.value(QStringLiteral("code")).toString() + QLatin1Char(':') + m.value(QStringLiteral("coreId")).toString(),
        QStringLiteral("warn"), m.value(QStringLiteral("text")).toString(), tr("Open Systems & cores"), QStringLiteral("page:systems"));
  }
  if (!admin_->serverUpdateVersion().isEmpty()) {
    add(QStringLiteral("update-available"), QStringLiteral("warn"), tr("Server update %1 is ready to install.").arg(admin_->serverUpdateVersion()),
        tr("Install update"), QStringLiteral("settings:server:updates"));
  }
  if (const int conflicts = model_.conflictCount(); conflicts > 0) {
    add(QStringLiteral("save-conflicts"), QStringLiteral("warn"),
        conflicts == 1 ? tr("1 game has a save conflict. Neither copy is overwritten until someone decides.")
                       : tr("%1 games have a save conflict. Neither copy is overwritten until someone decides.").arg(conflicts),
        tr("Review conflicts"), QStringLiteral("library:attention"));
  }
  return list;
}

void PlayerController::openAttention(const QString& target) {
  if (target.startsWith(QLatin1String("page:"))) {
    showServerPage(target.mid(5));
  } else if (target.startsWith(QLatin1String("settings:"))) {
    showSettingsSection(target.mid(9));
  } else if (target == QLatin1String("library:attention")) {
    model_.setFilter(QStringLiteral("attention"));
    showLibrary();
  }
}

void PlayerController::dismissCoreNotice(const QString& code) {
  if (code != QLatin1String("core_download") || dismissedCoreNotices_.contains(code)) return;
  dismissedCoreNotices_.insert(code);
  emit hubChanged();
}

void PlayerController::uploadRom(const QString& source) { uploads_->uploadRom(source); }

void PlayerController::dismissStopSaveError() {
  if (stopSaveError_.isEmpty()) {
    return;
  }
  stopSaveError_.clear();
  emit stopSaveErrorChanged();
}

void PlayerController::dismissUploadMessage() { uploads_->dismissMessage(); }

void PlayerController::adoptUploadedRom(const QString& srcPath, const QString& sha256, const QString& romFilename) {
  if (!cache_ || srcPath.isEmpty() || !RomCache::isValidSha256(sha256)) return;
  const QString ext = RomCache::extensionFromFilename(romFilename);
  const RomCache cache = *cache_;  // copy: the worker must not touch the controller
  auto* watcher = new QFutureWatcher<RomCache::CommitResult>(this);
  connect(watcher, &QFutureWatcher<RomCache::CommitResult>::finished, this, [this, watcher, sha256, ext]() {
    const RomCache::CommitResult res = watcher->result();
    watcher->deleteLater();
    if (res != RomCache::CommitResult::Ok) {
      qInfo() << "Uploaded ROM was not adopted into the ROM cache (it downloads on first start)";
      return;
    }
    if (!cache_) return;
    cache_->touch(sha256, ext);  // newest entry, so the size limit does not evict it right away
    trimRomCache();
    emit romCacheChanged();
    emit selectedGameChanged();  // "download needed" turns into "ready"
  });
  watcher->setFuture(QtConcurrent::run([cache, srcPath, sha256, ext]() { return cache.adoptFile(srcPath, sha256, ext); }));
}


// ---------------------------------------------------------------- Local ROMs (0.12)

QStringList PlayerController::libraryFolders() const { return settings_->libraryFolders(); }

namespace {
bool pathInFolder(const QString& file, const QString& folder) {
#ifdef Q_OS_WIN
  const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
  const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
  QString f = QDir::fromNativeSeparators(folder);
  if (!f.endsWith(QLatin1Char('/'))) f += QLatin1Char('/');
  return QDir::fromNativeSeparators(file).startsWith(f, cs);
}
}  // namespace

QVariantList PlayerController::libraryFolderInfo() const {
  QVariantList list;
  const bool scanning = localRoms_ && localRoms_->scanning();
  for (const QString& f : settings_->libraryFolders()) {
    int count = 0;
    for (const LocalRomFile& file : localRoms_->files())
      if (pathInFolder(file.path, f)) ++count;
    const QDateTime when = folderScanned_.value(f);
    const bool exists = QFileInfo(f).isDir();
    list.append(QVariantMap{{QStringLiteral("path"), f},
                            {QStringLiteral("exists"), exists},
                            {QStringLiteral("count"), count},
                            {QStringLiteral("scanning"), scanning && exists},
                            {QStringLiteral("scannedText"), when.isValid() ? TimeFormat::when(when) : QString()},
                            {QStringLiteral("scannedFull"), TimeFormat::full(when)}});
  }
  return list;
}

int PlayerController::libraryGameCount() const {
  int n = 0;
  for (const QString& f : settings_->libraryFolders())
    for (const LocalRomFile& file : localRoms_->files())
      if (pathInFolder(file.path, f)) ++n;
  return n;
}

void PlayerController::loadFolderScans() {
  QFile f(QDir(profiles_->baseDir()).filePath(QStringLiteral("cache/library-folders.json")));
  if (!f.open(QIODevice::ReadOnly)) return;
  const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
  for (auto it = o.begin(); it != o.end(); ++it) {
    const QDateTime t = QDateTime::fromString(it.value().toString(), Qt::ISODate);
    if (t.isValid()) folderScanned_.insert(it.key(), t);
  }
}

void PlayerController::saveFolderScans() const {
  QJsonObject o;
  for (auto it = folderScanned_.begin(); it != folderScanned_.end(); ++it) o.insert(it.key(), it.value().toUTC().toString(Qt::ISODate));
  QDir().mkpath(QDir(profiles_->baseDir()).filePath(QStringLiteral("cache")));
  QSaveFile f(QDir(profiles_->baseDir()).filePath(QStringLiteral("cache/library-folders.json")));
  if (!f.open(QIODevice::WriteOnly)) return;
  f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
  f.commit();
}

bool PlayerController::canRegisterLocalRoms() const { return registrar_ && registrar_->supported(); }

RomSourceChoice PlayerController::romSourceOf(const GameEntry& game) const {
  const QString ext = RomCache::extensionFromFilename(game.romFilename);
  const bool cacheHas = cache_ && cache_->probe(game.romSha256, ext, game.romSize) != RomCache::Probe::Missing;
  return chooseRomSource(game, [this](const QString& sha) { return localRoms_->pathForSha(sha); }, cacheHas);
}

RomStatus PlayerController::romStatusOf(const GameEntry& game) {
  const RomSourceChoice src = romSourceOf(game);
  RomStatus st;
  switch (src.source) {
    case RomSource::Local:
      st.state = RomState::Ready;
      st.localPath = src.localPath;
      st.fromLibraryFolder = true;
      st.totalBytes = game.romSize;
      return st;
    case RomSource::Missing:
      st.state = RomState::Missing;
      st.totalBytes = game.romSize;
      return st;
    default:
      return downloader_->status(game);
  }
}

void PlayerController::refreshRomStatuses() {
  model_.setGames(library_->games(), [this](const GameEntry& g) { return romStatusOf(g); });
  refreshAttention();
  emit selectedGameChanged();
}

void PlayerController::startLibraryScan() {
  if (localRoms_->scanning()) {
    // Folders changed during a scan: cancel it and start over once it ended.
    localRoms_->cancelScan();
    connect(localRoms_.get(), &LocalRomIndex::scanFinished, this, [this]() { startLibraryScan(); }, Qt::SingleShotConnection);
    return;
  }
  const QStringList folders = settings_->libraryFolders();
  if (folders.isEmpty()) {
    localRoms_->clear();
    libraryScan_ = {{QStringLiteral("active"), false}, {QStringLiteral("text"), QString()}, {QStringLiteral("isError"), false}};
    emit libraryScanChanged();
    refreshRomStatuses();
    return;
  }
  LocalScanOptions opt;
  opt.folders = folders;
  QHash<QString, QString> readers;  // system id -> identity reader of its manifest (the scan runs on a worker thread)
  for (const emu::SystemManifest& m : manifests_.all()) {
    for (const QString& e : m.extensions) opt.extensionToSystem.insert(e.toLower(), m.systemId);
    readers.insert(m.systemId, m.identity);
  }
  opt.identity = [readers](const QString& path, const QString& system) {
    const emu::GameIdentity id = emu::readGameIdentity(path, readers.value(system));
    return LocalRomIdentity{id.code, id.version};
  };
  if (!localRoms_->rescan(opt)) return;  // a scan runs: it picks up the current folders on the next rescan
  libraryScan_ = {{QStringLiteral("active"), true},
                  {QStringLiteral("done"), 0},
                  {QStringLiteral("total"), 0},
                  {QStringLiteral("isError"), false},
                  {QStringLiteral("text"), tr("Scanning…")}};
  emit libraryScanChanged();
  emit libraryFolderInfoChanged();
}

void PlayerController::registerLocalFiles() {
  if (!registrar_->supported() || registrar_->busy()) return;
  QList<LocalRomFile> todo = RomRegistrar::filesToRegister(localRoms_->files(), library_->games(), ignoredRoms_);
  todo.removeIf([this](const LocalRomFile& f) { return failedRegistrations_.contains(f.sha256); });
  if (!todo.isEmpty()) registrar_->registerFiles(todo);
}

void PlayerController::addLibraryFolder(const QString& source) {
  const QString path = QDir::cleanPath(source.startsWith(QLatin1String("file:")) ? QUrl(source).toLocalFile() : source);
  if (path.isEmpty() || !QFileInfo(path).isDir()) return;
  QStringList folders = settings_->libraryFolders();
  if (folders.contains(path)) return;
  folders.append(path);
  if (!settings_->setLibraryFolders(folders)) return;
  emit libraryFoldersChanged();
  emit libraryFolderInfoChanged();
  startLibraryScan();
}

void PlayerController::removeLibraryFolder(const QString& path) {
  QStringList folders = settings_->libraryFolders();
  if (!folders.removeOne(QDir::cleanPath(path))) return;
  if (!settings_->setLibraryFolders(folders)) return;
  folderScanned_.remove(QDir::cleanPath(path));
  saveFolderScans();
  emit libraryFoldersChanged();
  emit libraryFolderInfoChanged();
  startLibraryScan();
}

void PlayerController::stopLibraryScan() {
  if (localRoms_ && localRoms_->scanning()) localRoms_->cancelScan();
}

void PlayerController::rescanLibrary() {
  failedRegistrations_.clear();
  startLibraryScan();
}

QString PlayerController::localRomStatePath() const {
  const QString dir = profiles_->hubDir(conn_->hubInfo().hubId);
  return dir.isEmpty() ? QString() : QDir(dir).filePath(QStringLiteral("local_roms.json"));
}

void PlayerController::loadIgnoredRoms() {
  ignoredRoms_.clear();
  QFile f(localRomStatePath());
  if (localRomStatePath().isEmpty() || !f.open(QIODevice::ReadOnly)) return;
  for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("ignored")).toArray()) {
    ignoredRoms_.insert(v.toString());
  }
}

void PlayerController::saveIgnoredRoms() const {
  const QString path = localRomStatePath();
  if (path.isEmpty()) return;
  QJsonArray arr;
  for (const QString& s : ignoredRoms_) arr.append(s);
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return;
  f.write(QJsonDocument(QJsonObject{{QStringLiteral("ignored"), arr}}).toJson(QJsonDocument::Compact));
  f.commit();
}

void PlayerController::uploadGameToHub(const QString& gameId) {
  const QString err = uploads_->uploadLocalGame(gameId);
  if (!err.isEmpty()) {
    startError_ = err;
    emit selectedGameChanged();
  }
}

void PlayerController::removeFromLibrary(const QString& gameId) {
  const auto game = model_.game(gameId);
  if (!game || game->onHub || !registrar_) return;
  removedTitle_ = game->title;
  registrar_->unregisterGame(gameId);
}

void PlayerController::undoRemoveFromLibrary() {
  if (removedInfo_.isEmpty()) return;
  for (const QString& sha : std::as_const(removedShas_)) ignoredRoms_.remove(sha);
  removedShas_.clear();
  removedInfo_.clear();
  saveIgnoredRoms();
  emit removedFromLibraryChanged();
  registerLocalFiles();
}

void PlayerController::dismissRemovedFromLibrary() {
  if (removedInfo_.isEmpty()) return;
  removedInfo_.clear();
  removedShas_.clear();
  emit removedFromLibraryChanged();
}

QString PlayerController::libraryView() const { return settings_->libraryView(); }

void PlayerController::setLibraryView(const QString& view) {
  if (view == settings_->libraryView() || !settings_->setLibraryView(view)) return;
  emit libraryViewChanged();
}

// Library side effects of a finished upload; the message and the `upload` map are UploadController's.
void PlayerController::onUploadFinished(const UploadResult& r, const QString& srcPath) {
  using K = UploadResult::Kind;
  if (r.kind == K::Created) {
    pendingSelectId_ = r.game.id;
    adoptUploadedRom(srcPath, r.game.romSha256, r.game.romFilename);
    reloadLibrary();
  } else if (r.kind == K::Duplicate && !r.existingGameId.isEmpty()) {
    if (const auto existing = model_.game(r.existingGameId)) {
      adoptUploadedRom(srcPath, existing->romSha256, existing->romFilename);  // hash is verified against the Hub's value
    }
    if (model_.rowOfGame(r.existingGameId) >= 0) {
      selectGame(r.existingGameId);
    } else {
      pendingSelectId_ = r.existingGameId;
      reloadLibrary();
    }
  }
}

void PlayerController::recheckFirmware() {
  fwProblems_.clear();
  startError_.clear();
  systems_->reload();
  emit selectedGameChanged();
}

// ---------------------------------------------------------------- 3c Library

QString PlayerController::hubName() const { return conn_->hubInfo().name; }
QString PlayerController::hubAddress() const { return trimmedScheme(conn_->address()); }

// Ends the running game/Session and secures pending save uploads; needs the connection, so call before disconnecting.
void PlayerController::endRunningWork() {
  if (gameActive_) {
    setBackground(false);
    sessions_->gameEnded();  // same order as quitGame(): Sessions end before the core goes
    session_.stop();
    const bool liveWasOn = liveTimer_ && liveTimer_->isActive();
    if (liveTimer_) liveTimer_->stop();  // nested event loop in the final sync: no library reload while the game is half torn down (PE-19)
    saves_->finalSyncBlocking(true);
    if (liveWasOn) liveTimer_->start();
    gameActive_ = false;
    updateDiscord();
    phase_ = PlayPhase::None;
    finishRunStart();
    if (quitting_) {
      quitting_ = false;
      emit quittingChanged();
    }
    emit selectedGameChanged();
  }
}

void PlayerController::switchHub() {
  resumePage_.clear();
  endRunningWork();
  conn_->disconnectFromHub();
  updateScreen();
}

// Settings -> Hubs: same path as the connection screen (disconnect, then connect to the stored profile).
void PlayerController::switchToHub(const QString& hubId) {
  if (hubId.isEmpty() || (conn_->state() == HubConnection::State::Connected && conn_->profile() && conn_->profile()->hubId == hubId)) {
    return;
  }
  switchHub();
  connectProfile(hubId);
}

// Last played is local and kept per Hub (a game id on another Hub is a different game).
void PlayerController::recordLastPlayed(const QString& gameId, qint64 msecs) {
  const qint64 now = msecs > 0 ? msecs : QDateTime::currentMSecsSinceEpoch();
  settings_->setLastPlayed(conn_->hubInfo().hubId, gameId, now);
  model_.noteLastPlayed(gameId, now);
}

void PlayerController::reloadLibrary() {
  libraryState_ = QStringLiteral("loading");
  emit libraryStateChanged();
  library_->reload();
}

void PlayerController::applyCachedSyncKinds() {
  QHash<QString, QString> kinds;
  for (const GameEntry& g : library_->games()) {
    kinds.insert(g.id, SaveSync::kindName(saves_->kind(g.id)));
  }
  model_.setSyncKinds(kinds);
}

void PlayerController::onLibraryLoaded() {
  quietRefresh_ = false;
  if (!scanRequested_) {
    scanRequested_ = true;
    loadIgnoredRoms();
    startLibraryScan();
  } else if (!localRoms_->scanning()) {
    registerLocalFiles();  // e.g. the Hub connection came back after the scan
  }
  model_.setLastPlayed(settings_->lastPlayedAll(conn_->hubInfo().hubId));
  model_.setGames(library_->games(), [this](const GameEntry& g) { return romStatusOf(g); });
  if (personal_) personal_->refresh();  // collections of the person (3c-0)
  {
    // "42 games · Nintendo DS": the system name shows only when all games share one system.
    QString label;
    bool single = true;
    for (const GameEntry& g : library_->games()) {
      const emu::SystemManifest* man = catalog_->manifestFor(g);
      const QString name = man != nullptr ? man->displayName : g.system.toUpper();
      if (label.isEmpty()) {
        label = name;
      } else if (label != name) {
        single = false;
      }
    }
    model_.setSystemLabel(single ? label : QString());
  }
  refreshAttention();
  {
    // Games for the per-game section of the Emulation page.
    QVariantList list;
    for (const GameEntry& g : library_->games()) {
      const emu::SystemManifest* man = catalog_->manifestFor(g);
      if (man == nullptr) continue;
      list.append(QVariantMap{{QStringLiteral("id"), g.id}, {QStringLiteral("title"), g.title}, {QStringLiteral("systemId"), man->systemId}});
    }
    std::sort(list.begin(), list.end(), [](const QVariant& a, const QVariant& b) {
      return a.toMap().value(QStringLiteral("title")).toString().compare(b.toMap().value(QStringLiteral("title")).toString(), Qt::CaseInsensitive) < 0;
    });
    emulation_->setGames(list);
  }
  QStringList ids;
  for (const GameEntry& g : library_->games()) {
    ids.append(g.id);
  }
  // Known kinds first (cheap, no file access); the sync state on disk is read on a worker and reaches the model as one
  // batch (kindsRefreshed).
  applyCachedSyncKinds();
  saves_->refreshKindsAsync(ids);
  libraryState_ = QStringLiteral("ready");
  libraryError_.clear();
  emit libraryStateChanged();
  if (!pendingSelectId_.isEmpty()) {
    if (model_.rowOfGame(pendingSelectId_) >= 0) {
      selectedId_ = pendingSelectId_;
      startError_.clear();
    }
    pendingSelectId_.clear();
  }
  if (selectedId_.isEmpty() || model_.rowOfGame(selectedId_) < 0) {
    selectedId_ = model_.rowCount() > 0 ? model_.data(model_.index(0), LibraryModel::GameIdRole).toString() : QString();
  }
  emit selectedGameChanged();
}

void PlayerController::selectGame(const QString& gameId) {
  if (selectedId_ == gameId) {
    return;
  }
  selectedId_ = gameId;
  startError_.clear();
  emit selectedGameChanged();
}

void PlayerController::onRomStatus(const QString& sha, const RomStatus& st) {
  model_.setStatus(sha, st);
  bool progressOnly = false;
  if (st.state == RomState::Downloading) {
    // The detail pane shows "Downloading n %": re-evaluate selectedGame only when the whole percent moved.
    const int pct = st.totalBytes > 0 ? static_cast<int>(st.receivedBytes * 100 / st.totalBytes) : -1;
    int& shown = shownPercent_[sha];
    progressOnly = pct == shown;
    shown = pct;
  } else {
    shownPercent_.remove(sha);
    lastProgressMs_.remove(sha);
  }
  if (progressOnly) {
    return;
  }
  if (!pendingSha_.isEmpty() && pendingSha_ == sha && (st.state == RomState::HashMismatch || st.state == RomState::Failed)) {
    pendingSha_.clear();
    phase_ = PlayPhase::None;
    startError_ = st.state == RomState::HashMismatch
                      ? tr("The download does not match the expected SHA-256 and was discarded. Please download again.")
                      : tr("Download failed: %1").arg(st.errorMessage.isEmpty() ? st.errorCode : st.errorMessage);
  }
  const auto g = model_.game(selectedId_);
  if (g && g->romSha256 == sha) {
    emit selectedGameChanged();
  }
}

void PlayerController::playSelected() {
  if (startPending() && !gameActive_ && phase_ == PlayPhase::None) finishRunStart();  // a run start that never got going
  if (background_) {
    if (selectedId_ == backgroundId_) {
      resumeGame();
    } else {
      askQuitAndStart(false);
    }
    return;
  }
  cancelBackgroundUpdateForSelected();
  starter_->startSelected(false);
}

void PlayerController::resolveSaveConflict(const QString& action) { starter_->resolveSaveConflict(action); }

QString PlayerController::hubLink() const {
  using S = HubConnection::State;
  const S s = conn_->state();
  if (s == S::Connected) {
    // The handshake state stays Connected when the Hub process stops; the Sessions socket is the live signal.
    if (sessions_ && sessions_->available()) {
      const QString link = sessions_->hubLink();
      if (link == QLatin1String("online")) return link;
      // Never opened since the handshake: still starting. Opened before: the link dropped, a reconnect attempt is still "lost".
      return linkSeenUp_ ? QStringLiteral("offline") : QStringLiteral("connecting");
    }
    return QStringLiteral("online");
  }
  if (s == S::Identifying || s == S::Disconnected) return QStringLiteral("connecting");
  return QStringLiteral("offline");
}

void PlayerController::refreshHubLink() {
  if (conn_->state() != HubConnection::State::Connected) {
    linkSeenUp_ = false;
  } else if (sessions_ && sessions_->available() && sessions_->hubLink() == QLatin1String("online")) {
    linkSeenUp_ = true;
  }
  const bool down = hubLink() == QLatin1String("offline");
  emit hubLinkChanged();
  if (down != hubLinkDown_) {
    hubLinkDown_ = down;
    emit selectedGameChanged();
  }
}

QVariantMap PlayerController::selectedGame() const {
  QVariantMap m = detail_->selectedGame();
  if (background_ && !m.isEmpty() && m.value(QStringLiteral("id")).toString() == backgroundId_) {
    // The game runs (paused) in the background: the primary button resumes it instead of starting it again.
    m.insert(QStringLiteral("running"), true);
    m.insert(QStringLiteral("playLabel"), tr("Resume"));
    m.insert(QStringLiteral("canPlay"), true);
    m.insert(QStringLiteral("busy"), false);
    m.insert(QStringLiteral("pillText"), tr("❚❚ Running · paused"));
    m.insert(QStringLiteral("pillTone"), QStringLiteral("warn"));  // the accent chip (3c-5)
  }
  return m;
}

void PlayerController::adoptPreviewGame(const QString& gameId) {
  if (const auto g = model_.game(gameId)) {
    launchGame_ = *g;
  }
}

QVariantMap PlayerController::backgroundGame() const {
  if (!background_) {
    return {};
  }
  return {{QStringLiteral("id"), backgroundId_},
          {QStringLiteral("title"), backgroundTitle_},
          {QStringLiteral("startedMs"), gameStartedMs_},
          {QStringLiteral("pausedMs"), gamePausedMs_}};
}

QVariantMap PlayerController::startConfirm() const {
  QVariantMap m{{QStringLiteral("active"), confirmActive_}};
  if (confirmActive_) {
    const auto g = model_.game(selectedId_);
    m.insert(QStringLiteral("runningTitle"), backgroundTitle_);
    m.insert(QStringLiteral("newTitle"), g ? g->title : QString());
  }
  return m;
}

void PlayerController::setBackground(bool on) {
  const bool wasConfirm = confirmActive_;
  confirmActive_ = false;
  if (on == background_) {
    if (wasConfirm) emit startConfirmChanged();
    return;
  }
  background_ = on;
  if (on) {
    backgroundId_ = launchGame_.id;
    backgroundTitle_ = launchGame_.title;
    gamePausedMs_ = QDateTime::currentMSecsSinceEpoch();
  } else {
    backgroundId_.clear();
    backgroundTitle_.clear();
  }
  session_.setInputBlocked(on);
  emit backgroundGameChanged();
  if (wasConfirm) emit startConfirmChanged();
  emit selectedGameChanged();
}

void PlayerController::askQuitAndStart(bool share) {
  if (!background_ || !model_.game(selectedId_) || phase_ != PlayPhase::None) {
    return;
  }
  confirmActive_ = true;
  confirmShare_ = share;
  emit startConfirmChanged();
}

void PlayerController::cancelQuitAndStart() {
  if (confirmActive_) {
    confirmActive_ = false;
    emit startConfirmChanged();
  }
}

void PlayerController::confirmQuitAndStart() {
  if (!confirmActive_) {
    return;
  }
  const bool share = confirmShare_;
  quitGame();  // saved and synced first (core unloaded, final upload), also clears the confirmation
  cancelBackgroundUpdateForSelected();
  starter_->startSelected(share);
}

void PlayerController::playAndShareSelected() {
  if (startPending() && !gameActive_ && phase_ == PlayPhase::None) finishRunStart();
  if (background_) {
    if (selectedId_ == backgroundId_) {
      resumeGame();
    } else {
      askQuitAndStart(true);
    }
    return;
  }
  if (phase_ != PlayPhase::None || session_.isActive()) {
    return;
  }
  cancelBackgroundUpdateForSelected();
  starter_->startSelected(true);
}

void PlayerController::leaveGameView() {
  if (gameActive_ && !background_) {
    // Pause and keep the game loaded (the pause triggers the immediate save sync); a shared Session stays shared.
    page_ = QStringLiteral("library");
    session_.pause();
    setBackground(true);
    if (sessions_->watching()) {
      sessions_->leaveWatch();  // remote surfaces end with the game view
    }
    updateScreen();
  } else if (sessions_->watching()) {
    sessions_->leaveWatch();  // a game in the background stays there
    updateScreen();
  }
}

void PlayerController::resumeGame() {
  if (!gameActive_ || !background_) {
    return;
  }
  setBackground(false);
  session_.resume();
  updateScreen();
}

void PlayerController::requestQuit() {
  if (quitting_ || !gameActive_) {
    return;
  }
  quitting_ = true;
  emit quittingChanged();
  // quitGame() blocks until the core is unloaded: let the "Saving and syncing…" state paint first.
  QTimer::singleShot(80, this, [this]() { quitGame(); });
}

void PlayerController::quitGame() {
  if (quitting_) {
    quitting_ = false;
    emit quittingChanged();
  }
  setBackground(false);
  sessions_->gameEnded();
  session_.stop();  // core unloaded (save flushed), then the final upload
  saves_->finalSync(true);
  gameActive_ = false;
  if (states_) states_->gameStopped();  // the game page must not offer Save/Load for a stopped game
  stateRunId_.clear();
  updateDiscord();
  phase_ = PlayPhase::None;
  finishRunStart();
  updateScreen();
  emit selectedGameChanged();
  if (onboarding_) onboarding_->gameEnded();
}

// ---------------------------------------------------------------- Runs

void PlayerController::showRuns() {
  if (conn_->state() != HubConnection::State::Connected || (gameActive_ && !background_) || !runs_->available()) {
    return;
  }
  runs_->refresh();
  page_ = QStringLiteral("runs");
  updateScreen();
}

bool PlayerController::openRunLink(const QString& link) {
  const RunLink parsed = parseRunLink(link);
  if (!parsed.ok) return false;
  if (conn_->state() != HubConnection::State::Connected || makeRunLink(conn_->address(), parsed.runId) != makeRunLink(parsed.address, parsed.runId)) {
    return true;  // another server than the one connected: nothing to open (the run is not visible here)
  }
  showRuns();
  if (page_ != QLatin1String("runs")) return true;
  runs_->openRun(parsed.runId);
  runs_->setRunTab(QStringLiteral("recap"));
  return true;
}

void PlayerController::startRunGame(const QString& runId, const QString& gameId) {
  if (background_ || gameActive_ || session_.isActive() || phase_ != PlayPhase::None) {
    return;  // a game is running or starting: the Runs screen is not reachable then, nothing to do
  }
  finishRunStart();
  // A run with a randomizer builds the person's run variant first; the game of the run starts when it is ready (0.21).
  if (variants_ && variants_->runUsesVariant(runId) && !variants_->runVariantBuilt(runId)) {
    pendingRunPlay_ = {runId, gameId};
    runBuild_ = {};
    variants_->ensureRunVariant(runId, gameId);
    refreshRunBuild();
    return;
  }
  runStart_ = {runId, gameId, saves_->slotFor(gameId)};
  // The run keeps its own saves: the run's save slot (run-<first 8 hex of the run id>), never the game's default slot.
  if (!saves_->setSlot(gameId, runs_->saveSlotFor(runId))) {
    runStart_ = {};
    return;
  }
  selectGame(gameId);
  // A game that cannot start (core neither installed nor offered, file missing, ...) says why on the run screen; it never fails silently.
  if (const QVariantMap sel = selectedGame(); !sel.value(QStringLiteral("canPlay")).toBool()) {
    QString why = sel.value(QStringLiteral("playDisabledReason")).toString();
    if (why.isEmpty()) why = startError_;
    if (why.isEmpty()) why = tr("The game of this run cannot start right now.");
    finishRunStart();
    runs_->showLaunchProblem(why);
    return;
  }
  cancelBackgroundUpdateForSelected();
  starter_->startSelected(false);
  if (phase_ == PlayPhase::None && !gameActive_ && !session_.isActive() && !startError_.isEmpty()) {
    const QString why = startError_;
    finishRunStart();
    runs_->showLaunchProblem(why);
  }
}

// The state of the "Building your variant" dialog (10f-3) from the pending run start and the build that is running for it.
void PlayerController::refreshRunBuild() {
  QVariantMap m;
  if (pendingRunPlay_.runId.isEmpty() || variants_ == nullptr) {
    if (runBuild_.value(QStringLiteral("state")).toString() == QLatin1String("failed")) return;  // the failure stays until it is dismissed
  } else {
    const QString runId = pendingRunPlay_.runId;
    const auto run = runClient_->run(runId);
    const QVariantMap vs = variants_->runVariantState(runId);
    const QVariantMap cur = variants_->current();
    const bool mine = cur.value(QStringLiteral("variantId")).toString() == variants_->runVariantId(runId);
    const auto game = library_->gameById(pendingRunPlay_.gameId);
    m = {{QStringLiteral("runId"), runId},
         {QStringLiteral("runName"), run ? run->name : QString()},
         {QStringLiteral("gameTitle"), game ? game->title : QString()},
         {QStringLiteral("state"), QStringLiteral("building")},
         {QStringLiteral("step"), mine ? cur.value(QStringLiteral("step")).toString() : QString()},
         {QStringLiteral("fraction"), mine ? cur.value(QStringLiteral("fraction")).toDouble() : vs.value(QStringLiteral("fraction")).toDouble()},
         {QStringLiteral("percent"), mine ? cur.value(QStringLiteral("percent")).toInt() : 0},
         {QStringLiteral("etaText"), mine ? cur.value(QStringLiteral("etaText")).toString() : QString()},
         {QStringLiteral("error"), QString()},
         {QStringLiteral("startWhenReady"), pendingRunPlay_.startWhenReady},
         {QStringLiteral("background"), pendingRunPlay_.background}};
  }
  if (m != runBuild_) {
    runBuild_ = m;
    emit runBuildChanged();
  }
}

void PlayerController::setRunBuildStartWhenReady(bool on) {
  if (pendingRunPlay_.runId.isEmpty()) return;
  pendingRunPlay_.startWhenReady = on;
  refreshRunBuild();
}

void PlayerController::runBuildInBackground() {
  if (pendingRunPlay_.runId.isEmpty()) return;
  pendingRunPlay_.background = true;
  refreshRunBuild();
}

void PlayerController::cancelRunBuild() {
  if (pendingRunPlay_.runId.isEmpty() || variants_ == nullptr) return;
  const QString runId = std::exchange(pendingRunPlay_, {}).runId;
  const QString id = variants_->runVariantId(runId);
  if (!id.isEmpty()) variants_->cancel(id);
  runBuild_ = {};
  emit runBuildChanged();
}

void PlayerController::dismissRunBuild() {
  if (runBuild_.isEmpty()) return;
  runBuild_ = {};
  emit runBuildChanged();
}

void PlayerController::finishRunStart() {
  if (variantStart_.pending()) {
    const VariantStart vs = std::exchange(variantStart_, {});
    if (!saves_->isRunning(vs.gameId)) saves_->setSlot(vs.gameId, vs.previousSlot);
  }
  if (!runStart_.pending()) return;
  const RunStart rs = std::exchange(runStart_, {});
  if (!saves_->isRunning(rs.gameId)) saves_->setSlot(rs.gameId, rs.previousSlot);
}

void PlayerController::startVariantGame(const QString& variantId) {
  if (background_ || gameActive_ || session_.isActive() || phase_ != PlayPhase::None) {
    return;
  }
  const VariantLaunch vl = variants_->launchInfo(variantId);
  if (!vl.active || !vl.error.isEmpty() || vl.slot.isEmpty()) {
    startError_ = tr("The variant is not built on this device.");
    emit selectedGameChanged();
    return;
  }
  finishRunStart();
  variantStart_ = {variantId, vl.gameId, saves_->slotFor(vl.gameId)};
  // A variant plays in its own save slot (var-<8 hex>); Main is never touched.
  if (!saves_->setSlot(vl.gameId, vl.slot)) {
    variantStart_ = {};
    return;
  }
  variants_->touch(variantId);
  selectGame(vl.gameId);
  cancelBackgroundUpdateForSelected();
  starter_->startSelected(false);
}

QString PlayerController::titleIdOf(const GameEntry& game) {
  if (game.system != QLatin1String("3ds")) return {};
  const emu::SystemManifest* man = catalog_ ? catalog_->manifestFor(game) : nullptr;
  const RomStatus st = romStatusOf(game);
  QString code;
  if (man != nullptr && !st.localPath.isEmpty() && QFileInfo::exists(st.localPath)) {
    code = emu::readGameIdentity(st.localPath, man->identity).code;
  }
  if (!variantlaunch::isValidTitleId(code)) code = game.gameCode.toUpper();
  return variantlaunch::isValidTitleId(code) ? code : QString();
}

VariantLaunch PlayerController::variantLaunchFor(const GameEntry& game) {
  VariantLaunch vl;
  QString variantId;
  if (variantStart_.pending() && variantStart_.gameId == game.id) {
    variantId = variantStart_.variantId;
  } else if (runStart_.pending() && runStart_.gameId == game.id && variants_ && variants_->runUsesVariant(runStart_.runId)) {
    variantId = variants_->runVariantId(runStart_.runId);
  }
  if (!variantId.isEmpty() && variants_) {
    vl = variants_->launchInfo(variantId);
    vl.active = true;
    if (vl.titleId.isEmpty()) vl.titleId = titleIdOf(game);
    return vl;
  }
  if (game.system == QLatin1String("3ds")) vl.titleId = titleIdOf(game);
  return vl;
}

}  // namespace uzume::ui
