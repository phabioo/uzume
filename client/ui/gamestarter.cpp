#include "gamestarter.h"
#include "timeformat.h"
#include "savestore.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QScopeGuard>
#include <algorithm>

#include "core_options.h"
#include "firmware_materializer.h"
#include "libretro_backend.h"
#include "semver.h"
#include "version.h"

namespace uzume::ui {

Q_LOGGING_CATEGORY(lcGameStarter, "uzume.gamestarter")

GameStarter::GameStarter(const Deps& d, QObject* parent)
    : QObject(parent),
      conn_(d.conn),
      profiles_(d.profiles),
      saves_(d.saves),
      coreProv_(d.coreProv),
      provisioner_(d.provisioner),
      downloader_(d.downloader),
      library_(d.library),
      emulation_(d.emulation),
      catalog_(d.catalog),
      model_(d.model),
      session_(d.session),
      selectedId_(d.selectedId),
      phase_(d.phase),
      launchGame_(d.launchGame),
      launchRom_(d.launchRom),
      saveReady_(d.saveReady),
      saveNoteStart_(d.saveNoteStart),
      startError_(d.startError),
      pendingSha_(d.pendingSha),
      fwOptions_(d.fwOptions),
      shareOnStart_(d.shareOnStart),
      fwProblems_(d.fwProblems),
      coreProblems_(d.coreProblems),
      coreList_(d.coreList),
      handshake_(d.handshake),
      conflict_(d.conflict),
      probeCoreVersions_(d.probeCoreVersions),
      romSource_(d.romSource),
      variantLaunch_(d.variantLaunch) {}

void GameStarter::startSelected(bool share) {
  shareOnStart_ = false;
  const auto game = model_.game(selectedId_);
  if (!game || phase_ != PlayPhase::None || session_.isActive()) {
    return;
  }
  startError_.clear();
  const emu::SystemManifest* man = catalog_->manifestFor(*game);
  if (man == nullptr) {
    startError_ = tr("There is no system manifest for this game.");
    emit selectedGameChanged();
    return;
  }
  emu::CoreLocation loc;
  const bool usable = catalog_->coreUsable(*man, &loc);
  QString hubVersion;
  const bool offered = catalog_->hubOffersCore(*man, &hubVersion);
  // Provision when the core is missing (or a cached one is damaged), or when the Hub serves another version than the
  // cached one. Explicit/env/app-dir cores are the user's choice and are never replaced.
  const bool versionDiffers = usable && loc.source == QLatin1String("cache") && loc.version != hubVersion;
  if (offered && (!usable || versionDiffers)) {
    shareOnStart_ = share;
    launchGame_ = *game;
    phase_ = PlayPhase::Core;
    emit selectedGameChanged();
    coreProv_->prepare(man->coreId, hubVersion);
    return;
  }
  if (!usable) {
    emit selectedGameChanged();  // detail pane shows the core status and hint
    return;
  }
  shareOnStart_ = share;
  continueStartAfterCore(*game, *man);
}

void GameStarter::onCoreFinished(const CoreResult& result) {
  // D15: whatever happens next, Library tiles, detail pane and Emulation page see the new core state at once.
  const auto refresh = qScopeGuard([this]() { refreshCoreState(); });
  if (phase_ != PlayPhase::Core) {
    return;
  }
  const auto game = model_.game(launchGame_.id);  // the game being started, not the current selection (PU-9)
  const emu::SystemManifest* man = game ? catalog_->manifestFor(*game) : nullptr;
  if (!game || man == nullptr || game->id != launchGame_.id) {
    phase_ = PlayPhase::None;
    shareOnStart_ = false;
    emit selectedGameChanged();
    return;
  }
  if (result.ok) {
    coreProblems_.remove(man->coreId);
    // The core arrived after the handshake data was built: report it and fill the Emulation page on the next refresh.
    // A new or updated version: drop the old entry and report what the locator now picks (the Hub's version).
    if (probeCoreVersions_) {
      coreList_.removeIf([&](const CoreInfo& c) { return c.id == man->coreId; });
      catalog_->probeCores();  // async; PlayerController resends the handshake when the probe is done
    }
    continueStartAfterCore(*game, *man);
    return;
  }
  coreProblems_.insert(man->coreId, result.problem);
  emu::CoreLocation loc;
  QString blockedText;
  if (catalog_->coreUsable(*man, &loc)) {
    // Another version of the core is cached (or env/legacy): the game still starts with it, unless the cached core's
    // major version differs from the one the Hub expects (ADR 0017). Explicit/env/app-dir cores are never blocked.
    QString hubVersion;
    catalog_->hubOffersCore(*man, &hubVersion);
    if (loc.source != QLatin1String("cache") ||
        update::coreVersionVerdict(loc.version, hubVersion) != update::CoreVersionVerdict::Block) {
      continueStartAfterCore(*game, *man);
      return;
    }
    blockedText = tr("The cached core %1 %2 is not compatible with version %3 the server expects. The game was not started.")
                      .arg(man->coreId, loc.version, hubVersion);
  }
  phase_ = PlayPhase::None;
  shareOnStart_ = false;
  startError_ = blockedText.isEmpty() ? tr("%1. The game was not started.").arg(catalog_->coreProblemText(result.problem)) : blockedText;
  emit selectedGameChanged();
}

void GameStarter::continueStartAfterCore(const GameEntry& gameRef, const emu::SystemManifest& manRef) {
  const GameEntry* game = &gameRef;
  const emu::SystemManifest* man = &manRef;
  phase_ = PlayPhase::None;  // the core step (if any) is over; the firmware/ROM steps set their own phase
  fwOptions_.clear();
  SystemInfo sys;
  if (catalog_->nativeFirmware(*man, &sys)) {
    // Mode native: the files must be available and valid before anything else happens (also blocks the launch).
    const QStringList wanted = catalog_->wantedFirmwareIds(*man);
    const QList<FirmwareProblem> missing = FirmwareProvisioner::missingOnHub(sys, wanted);
    if (!missing.isEmpty()) {
      emit selectedGameChanged();  // the detail pane shows "Firmware required/missing"
      return;
    }
    launchGame_ = *game;
    phase_ = PlayPhase::Firmware;
    emit selectedGameChanged();
    provisioner_->prepare(sys, wanted);
    return;
  }
  // Built-in firmware (default, old hubs): nothing is downloaded, the core is told explicitly.
  fwOptions_ = emu::firmwareCoreOptions(man->firmware, false, {});
  beginRomPhase(*game);
}

void GameStarter::beginRomPhase(const GameEntry& game) {
  launchGame_ = game;  // identifies the game for the ROM handler even when the selection changes meanwhile
  pendingSha_ = game.romSha256;
  activeVariant_ = variantLaunch_ ? variantLaunch_(game) : VariantLaunch{};
  if (activeVariant_.active) {
    if (!activeVariant_.error.isEmpty() || (!activeVariant_.isMod && activeVariant_.romPath.isEmpty())) {
      pendingSha_.clear();
      phase_ = PlayPhase::None;
      shareOnStart_ = false;
      startError_ = activeVariant_.error.isEmpty() ? tr("The variant is not built on this device. The game was not started.") : activeVariant_.error;
      emit selectedGameChanged();
      return;
    }
    if (!activeVariant_.isMod) {
      // Gen 1 to 5: the built variant ROM is the game file; Main's ROM is not needed.
      pendingSha_.clear();
      launch(game, activeVariant_.romPath);
      return;
    }
  }
  const RomSourceChoice src = romSource_ ? romSource_(game) : RomSourceChoice{game.onHub ? RomSource::Download : RomSource::Missing, {}};
  if (src.source == RomSource::Local) {
    // Start directly from the library folder: no copy into the ROM cache.
    pendingSha_.clear();
    launch(game, src.localPath);
    return;
  }
  if (src.source == RomSource::Missing) {
    pendingSha_.clear();
    phase_ = PlayPhase::None;
    shareOnStart_ = false;
    startError_ = tr("File missing on this device. Add the folder with this game's ROM under Settings > Library folders, or upload it to the server from a device that has it. The game was not started.");
    emit selectedGameChanged();
    return;
  }
  phase_ = PlayPhase::Rom;
  emit selectedGameChanged();
  downloader_->ensureRom(game);
}

void GameStarter::onFirmwareFinished(const FirmwareResult& result) {
  if (phase_ != PlayPhase::Firmware) {
    return;
  }
  const auto game = model_.game(launchGame_.id);  // the game being started, not the current selection (PU-9)
  const emu::SystemManifest* man = game ? catalog_->manifestFor(*game) : nullptr;
  if (!game || man == nullptr || game->id != launchGame_.id) {
    phase_ = PlayPhase::None;
    emit selectedGameChanged();
    return;
  }
  if (!result.ok) {
    phase_ = PlayPhase::None;
    shareOnStart_ = false;
    QStringList names;
    for (const FirmwareProblem& p : result.problems) {
      names.append(p.displayName.isEmpty() ? p.fileId : p.displayName);
      if (p.reason == QLatin1String("invalid_hash") || p.reason == QLatin1String("invalid_size") || p.reason == QLatin1String("missing_on_hub")) {
        fwProblems_.append(p);  // blocks until the Hub reports other data (Check again)
      }
    }
    startError_ = tr("Firmware required/missing: %1. The game was not started.").arg(names.join(QStringLiteral(", ")));
    emit selectedGameChanged();
    return;
  }
  QStringList written;
  QString err;
  if (!emu::materializeFirmware(man->firmware, result.pathsById, catalog_->systemDir(), &written, &err)) {
    phase_ = PlayPhase::None;
    shareOnStart_ = false;
    startError_ = tr("Firmware could not be prepared: %1").arg(err);
    emit selectedGameChanged();
    return;
  }
  fwOptions_ = emu::firmwareCoreOptions(man->firmware, true, written);
  fwProblems_.clear();
  beginRomPhase(*game);
}

void GameStarter::onRomReady(const QString& sha, const QString& path) {
  if (pendingSha_.isEmpty() || pendingSha_ != sha) {
    return;
  }
  pendingSha_.clear();
  RomStatus st;
  st.state = RomState::Ready;
  st.localPath = path;
  model_.setStatus(sha, st);
  for (const GameEntry& g : library_->games()) {
    if (g.romSha256 == sha && g.id == launchGame_.id) {
      launch(g, path);
      return;
    }
  }
  phase_ = PlayPhase::None;
  emit selectedGameChanged();
}

void GameStarter::launch(const GameEntry& game, const QString& romPath) {
  // Only one core can be loaded per process: wait until a running core probe (worker thread) is done.
  if (catalog_->probeBusy()) {
    phase_ = PlayPhase::Launching;
    launchGame_ = game;
    emit selectedGameChanged();
    connect(catalog_, &CoreCatalog::coresProbed, this,
            [this, game, romPath]() {
              if (phase_ == PlayPhase::Launching && launchGame_.id == game.id && !catalog_->probeBusy()) {
                launch(game, romPath);
              }
            },
            Qt::SingleShotConnection);
    return;
  }
  const emu::SystemManifest* man = catalog_->manifestFor(game);
  if (man == nullptr || profiles_->hubDir(conn_->hubInfo().hubId).isEmpty()) {
    phase_ = PlayPhase::None;
    startError_ = tr("Cannot start the game (system or server directory unknown).");
    emit selectedGameChanged();
    return;
  }
  phase_ = PlayPhase::Launching;
  launchGame_ = game;
  launchRom_ = romPath;
  saveReady_ = false;
  saveNoteStart_.clear();
  emit selectedGameChanged();
  // Start sync with the Hub first (before the core loads); the game starts in onSaveReady().
  // The core id + version go along: a save last written by another core or build is snapshotted first (ADR 0020 D7).
  saves_->prepareStart(game.id, romPath, {game.romSha256, QFileInfo(game.romFilename).completeBaseName()}, coreRefFor(*man));
}

SaveSync::CoreRef GameStarter::coreRefFor(const emu::SystemManifest& man) const {
  const emu::CoreLocation loc = catalog_->locateCore(man);
  QString version = loc.source == QLatin1String("cache") ? loc.version : QString();
  if (version.isEmpty()) {
    // env / explicit / app-dir core (development): the version the core reports, else a fixed marker
    version = catalog_->reportedCoreVersion(man.coreId);
  }
  SaveSync::CoreRef ref{man.coreId, version.isEmpty() ? QStringLiteral("local") : version};
  ref.saveSource = man.saveSource;
  ref.fileExtension = man.saveExtension;
  ref.fileFormat = man.saveFormat;
  ref.shortDir = man.saveShortDir;
  ref.subfolder = man.saveShortDirSubfolder;
  return ref;
}

void GameStarter::onSaveReady(const QString& gameId, const QString& saveDir, const QString& note) {
  if (phase_ != PlayPhase::Launching || launchGame_.id != gameId) {
    return;
  }
  saveNoteStart_ = note;
  saveReady_ = true;
  if (!conflict_.isEmpty()) {
    conflict_.clear();
    emit saveConflictChanged();
  }
  const emu::SystemManifest* man = catalog_->manifestFor(launchGame_);
  if (man == nullptr) {
    phase_ = PlayPhase::None;
    emit selectedGameChanged();
    return;
  }
  const emu::CoreLocation loc = catalog_->locateCore(*man);
  GameSession::LaunchConfig cfg;
  cfg.title = launchGame_.title;
  cfg.corePath = loc.path;
  cfg.gamePath = launchRom_;
  cfg.systemDir = catalog_->systemDir();
  cfg.saveDir = saveDir;
  if (man->saveShortDir) {  // for core_file / core_tree cores SaveSync already hands out this very directory
    // Core profile save.short_dir (Windows MAX_PATH): a short dir; adopt a tree written to the long dir earlier.
    const QString shortDir = SaveStore::shortCoreDir(*profiles_, conn_->hubId(), conn_->hubUserId(), gameId, saves_->slotFor(gameId));
    if (!shortDir.isEmpty()) {
      if (!man->saveShortDirSubfolder.isEmpty()) {
        SaveStore::migrateCoreSubfolder(saveDir, shortDir, man->saveShortDirSubfolder);
      }
      cfg.saveDir = shortDir;
    }
  }
  // 3DS variants: the LayeredFS folder lives in the Azahar user folder of this slot only; every other start removes a
  // variant mod from its slot, so Main never sees one (ADR 0031 D2).
  if (man->saveSource == QLatin1String("core_tree") && !man->saveShortDirSubfolder.isEmpty()) {
    const QString azaharDir = QDir(cfg.saveDir).filePath(man->saveShortDirSubfolder);
    QString modError;
    bool ok = true;
    if (activeVariant_.active && activeVariant_.isMod) {
      ok = variantlaunch::placeMod(azaharDir, activeVariant_.titleId, activeVariant_.modDir, activeVariant_.marker, &modError);
    } else if (variantlaunch::isValidTitleId(activeVariant_.titleId)) {
      ok = variantlaunch::ensureNoMod(azaharDir, activeVariant_.titleId, &modError);
    }
    if (!ok) {
      phase_ = PlayPhase::None;
      shareOnStart_ = false;
      startError_ = tr("The variant could not be prepared (%1). The game was not started.").arg(modError);
      emit selectedGameChanged();
      return;
    }
  }
#ifdef Q_OS_WIN
  // Cores use narrow Windows APIs without the long-path prefix: warn before a file the core creates cannot be opened.
  {
    const int saveName = man->saveSource == QLatin1String("core_file") ? SaveStore::expectedSaveName(launchRom_, man->saveExtension).size() : 40;
    if (SaveStore::exceedsLegacyPathLimit(cfg.saveDir, saveName)) {
      qCWarning(lcGameStarter) << "Save directory plus core file name exceeds 259 characters; the core may not be able to persist saves:" << cfg.saveDir;
    }
    if (SaveStore::exceedsLegacyPathLimit(cfg.systemDir, 40)) {
      qCWarning(lcGameStarter) << "System directory plus the longest firmware file name exceeds 259 characters; the core may not find its BIOS/firmware:" << cfg.systemDir;
    }
  }
#endif
  // Manifest defaults < user overrides (game > system/core > global) < firmware mode of the Hub (builtin | native +
  // files) < manifest-locked options (screen layout, OSD off; the render mode is a user choice, default software): Uzume stays in control of those.
  cfg.coreOptions = emu::launchCoreOptions(*man, emulation_->launchOverrides(man->systemId, launchGame_.id), fwOptions_);
  cfg.display = man->display;
  cfg.requireHwRender = man->requiresHwRender;
  {
    const auto fb = [&](const char* key) { return emulation_->uzumeValue(QString::fromLatin1(key), man->systemId, launchGame_.id); };
    cfg.speedUpRatio = fb(EmulationController::kSpeedUpRatioKey).toDouble();
    cfg.speedUpOnStart = fb(EmulationController::kSpeedUpOnStartKey) == QLatin1String("true");
    cfg.speedUpAudio = fb(EmulationController::kSpeedUpAudioKey) != QLatin1String("false");
  }
  emit selectedGameChanged();
  emit gameLaunched(launchGame_.id);
  emit systemInputSelected(man->inputProfile, man->inputLabel, man->display.touchScreenIndex() >= 0 ? man->touchLabel : QString());
  session_.start(cfg);
}

QString GameStarter::formatWhen(const QDateTime& when) { return TimeFormat::when(when); }

void GameStarter::onSaveConflict(const SaveSync::ConflictView& v) {
  const SaveConflictInfo& c = v.conflict;
  const auto side = [](const QString& title, const QString& when, const QString& base) {
    return QVariantMap{{QStringLiteral("title"), title}, {QStringLiteral("when"), when}, {QStringLiteral("base"), base}};
  };
  QVariantMap m;
  m.insert(QStringLiteral("active"), true);
  m.insert(QStringLiteral("busy"), false);
  m.insert(QStringLiteral("error"), QString());
  m.insert(QStringLiteral("id"), c.id);
  m.insert(QStringLiteral("gameTitle"), launchGame_.title);
  m.insert(QStringLiteral("hub"),
           side(tr("Current checkpoint · %1").arg(c.hubDeviceName),
                tr("%1 · Rev %2").arg(formatWhen(QDateTime::fromString(c.hubCreatedAt, Qt::ISODate)), QString::number(c.hubRevision)),
                tr("Hash %1").arg(c.hubSha256.left(8))));
  m.insert(QStringLiteral("local"),
           side(tr("Local · %1").arg(v.localDeviceName), tr("%1 · sync pending").arg(formatWhen(v.localModified)),
                tr("Base: Rev %1 · hash %2").arg(v.localBaseRevision).arg(v.localSha256.left(8))));
  conflict_ = m;
  emit saveConflictChanged();
}

void GameStarter::resolveSaveConflict(const QString& action) {
  if (conflict_.isEmpty() || conflict_.value(QStringLiteral("busy")).toBool()) {
    return;
  }
  conflict_.insert(QStringLiteral("busy"), true);
  conflict_.insert(QStringLiteral("error"), QString());
  emit saveConflictChanged();
  saves_->resolveConflict(action == QLatin1String("use_hub")     ? SaveSync::Resolution::UseHub
                          : action == QLatin1String("use_local") ? SaveSync::Resolution::UseLocal
                                                                 : SaveSync::Resolution::DecideLater);
}

}  // namespace uzume::ui
