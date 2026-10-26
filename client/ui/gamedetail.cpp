#include "gamedetail.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <algorithm>

#include "core_options.h"
#include "firmware_materializer.h"
#include "libretro_backend.h"
#include "semver.h"
#include "version.h"

namespace uzume::ui {

namespace {

QVariantMap checkItem(const QString& label, const QString& state, const QString& meta = QString()) {
  return {{QStringLiteral("label"), label}, {QStringLiteral("state"), state}, {QStringLiteral("meta"), meta}};
}

}  // namespace

GameDetail::GameDetail(const Deps& d, QObject* parent)
    : QObject(parent),
      model_(d.model),
      selectedId_(d.selectedId),
      saves_(d.saves),
      cache_(d.cache),
      conn_(d.conn),
      systems_(d.systems),
      fwCache_(d.fwCache),
      catalog_(d.catalog),
      coreVersions_(d.coreVersions),
      fwProblems_(d.fwProblems),
      phase_(d.phase),
      saveReady_(d.saveReady),
      saveNoteStart_(d.saveNoteStart),
      startError_(d.startError),
      hubLinkDown_(d.hubLinkDown) {}

QVariantMap GameDetail::selectedGame() const {
  QVariantMap m;
  const auto game = model_.game(selectedId_);
  if (!game) {
    return m;
  }
  const RomStatus st = model_.status(selectedId_).value_or(RomStatus{});
  const QString kind = LibraryModel::stateKind(st.state);
  const emu::SystemManifest* man = catalog_->manifestFor(*game);
  const QString ext = RomCache::extensionFromFilename(game->romFilename);

  m.insert(QStringLiteral("id"), game->id);
  m.insert(QStringLiteral("saveSlot"), saves_->slotFor(game->id));
  m.insert(QStringLiteral("personal"), model_.personalFor(game->id));  // play time, last played, favourite, collections (3c-1)
  m.insert(QStringLiteral("title"), game->title);
  m.insert(QStringLiteral("system"), game->system);
  m.insert(QStringLiteral("monogram"), LibraryModel::monogram(game->title));
  m.insert(QStringLiteral("systemName"), man ? man->displayName : game->system.toUpper());
  m.insert(QStringLiteral("stateKind"), kind);
  m.insert(QStringLiteral("sha"), game->romSha256);
  m.insert(QStringLiteral("romFilename"), game->romFilename);  // admin details of the Library
  m.insert(QStringLiteral("uploadedBy"), game->uploadedBy);
  m.insert(QStringLiteral("sizeText"), tr("%1 (%2 bytes)").arg(LibraryModel::formatSize(game->romSize)).arg(game->romSize));
  m.insert(QStringLiteral("cachePath"), cache_->finalPath(game->romSha256, ext));
  const qint64 total = st.totalBytes > 0 ? st.totalBytes : game->romSize;
  m.insert(QStringLiteral("progress"), total > 0 ? static_cast<double>(st.receivedBytes) / static_cast<double>(total) : 0.0);

  // ROM row
  QString romText;
  QString romTone = QStringLiteral("neutral");
  const bool fromLibraryFolder = kind == QLatin1String("ready") && !st.localPath.isEmpty() && !st.localPath.startsWith(cache_->dir());
  m.insert(QStringLiteral("onHub"), game->onHub);
  m.insert(QStringLiteral("localOnly"), !game->onHub);
  m.insert(QStringLiteral("localFile"), fromLibraryFolder);
  m.insert(QStringLiteral("localPath"), fromLibraryFolder ? st.localPath : QString());
  if (kind == QLatin1String("ready") && fromLibraryFolder) {
    romText = game->onHub ? tr("Local file · starts from your library folder") : tr("Local file · not on the server");
    romTone = QStringLiteral("ok");
  } else if (kind == QLatin1String("ready")) {
    romText = tr("Cached locally · verified");
    romTone = QStringLiteral("ok");
  } else if (kind == QLatin1String("missing")) {
    romText = tr("File missing on this device");
    romTone = QStringLiteral("amber");
  } else if (kind == QLatin1String("download")) {
    romText = tr("Download needed · %1").arg(LibraryModel::formatSize(game->romSize));
  } else if (kind == QLatin1String("downloading")) {
    romText = tr("Downloading %1 %").arg(static_cast<int>(m.value(QStringLiteral("progress")).toDouble() * 100));
  } else if (kind == QLatin1String("mismatch")) {
    romText = tr("Hash mismatch · reload");
    romTone = QStringLiteral("error");
  } else if (kind == QLatin1String("failed")) {
    romText = tr("Download failed");
    romTone = QStringLiteral("error");
  } else {
    romText = tr("Verifying…");
  }
  m.insert(QStringLiteral("romText"), romText);
  m.insert(QStringLiteral("romTone"), romTone);

  const bool hubUp = conn_->state() == HubConnection::State::Connected && !hubLinkDown_;
  const QString hubLabel = conn_->hubInfo().name.isEmpty() ? tr("the server") : conn_->hubInfo().name;
  // Core / firmware
  bool coreOk = false;
  bool coreProvisionable = false;  // missing locally, but the Hub offers it: provisioned at game start
  QString coreTone = QStringLiteral("ok");
  bool fwOk = true;
  bool fwChecking = false;
  bool fwBlocked = false;
  QString fwTone = QStringLiteral("neutral");
  QString coreText;
  QString coreStatusText;  // "Core missing", "Core download failed", ...: the START step reuses it as its title
  QString coreHint;
  QString fwText = tr("Not required");
  QString fwHint;
  if (man == nullptr) {
    coreText = tr("no system manifest for .%1").arg(ext);
  } else {
    const emu::CoreLocation loc = catalog_->locateCore(*man);
    const QString label = catalog_->coreLabel(*man, loc);
    coreOk = catalog_->coreUsable(*man);
    if (coreOk) {
      coreText = tr("%1 · ready").arg(label);
    } else {
      QString st;
      catalog_->coreStatus(*man, &st, &coreTone, &coreHint);
      coreStatusText = st;
      coreText = tr("%1 · %2").arg(label, st);
      coreProvisionable = hubUp && catalog_->hubOffersCore(*man) && phase_ == PlayPhase::None;
    }
    SystemInfo sys;
    if (systems_->supported() && systems_->state() == HubSystems::State::Loading && !systems_->system(man->systemId)) {
      fwText = tr("Checking…");
      fwChecking = true;
    } else if (systems_->supported() && systems_->state() == HubSystems::State::Failed) {
      fwText = tr("Status unavailable");
      fwTone = QStringLiteral("warn");
      fwHint = tr("The firmware status could not be read from the server; the built-in firmware is used.");
    } else if (catalog_->nativeFirmware(*man, &sys)) {
      const QStringList wanted = catalog_->wantedFirmwareIds(*man);
      QList<FirmwareProblem> problems = FirmwareProvisioner::missingOnHub(sys, wanted);
      for (const FirmwareProblem& p : fwProblems_) {
        if (p.reason == QLatin1String("invalid_hash") || p.reason == QLatin1String("invalid_size")) {
          problems.append(p);
        }
      }
      if (!problems.isEmpty()) {
        fwOk = false;
        fwBlocked = true;
        fwText = tr("Firmware required/missing");
        QStringList parts;
        for (const FirmwareProblem& p : problems) {
          const QString name = p.displayName.isEmpty() ? p.fileId : p.displayName;
          const QString why = p.reason == QLatin1String("missing_on_hub") ? tr("missing on the server")
                              : p.reason == QLatin1String("invalid_hash") ? tr("does not match the server's SHA-256")
                                                                          : tr("has the wrong size");
          parts.append(QStringLiteral("%1 (%2)").arg(name, why));
        }
        fwHint = tr("%1. The server admin can provide the files on the Systems page; then check again.").arg(parts.join(QStringLiteral(", ")));
      } else {
        bool allCached = true;
        for (const FirmwareFileInfo& f : sys.firmware) {
          if (f.present && wanted.contains(f.id) && !fwCache_->probe(sys.id, f.sha256, f.size)) {
            allCached = false;
          }
        }
        fwText = allCached ? tr("From server · cached") : tr("From server · download at start");
        fwTone = allCached ? QStringLiteral("ok") : QStringLiteral("neutral");
      }
    } else if (systems_->supported()) {
      fwText = tr("Not required");
    }
  }
  m.insert(QStringLiteral("hubOffline"), !hubUp);
  m.insert(QStringLiteral("hubLabel"), hubLabel);
  m.insert(QStringLiteral("coreText"), coreText);
  m.insert(QStringLiteral("coreTone"), coreOk ? QStringLiteral("ok") : coreTone);
  m.insert(QStringLiteral("coreHint"), coreHint);
  m.insert(QStringLiteral("firmwareText"), fwText);
  m.insert(QStringLiteral("firmwareTone"), fwOk ? fwTone : QStringLiteral("error"));
  m.insert(QStringLiteral("firmwareHint"), fwHint);
  m.insert(QStringLiteral("firmwareBlocked"), fwBlocked);

  // Save row (sync state)
  {
    const QString sk = model_.syncKind(selectedId_);
    QString saveText;
    QString saveTone = QStringLiteral("neutral");
    QString saveHint;
    if (man != nullptr && man->saveSource == QLatin1String("none")) {
      saveText = tr("Local only");
      saveHint = SaveSync::localOnlyNote();
    } else if (conn_->state() == HubConnection::State::Connected && !saves_->hubSupportsSaves()) {
      saveText = SaveSync::unsupportedNote();
    } else if (sk == QLatin1String("synced")) {
      saveText = tr("Synced");
      saveTone = QStringLiteral("ok");
    } else if (sk == QLatin1String("pending")) {
      saveText = tr("Sync pending");
      saveTone = QStringLiteral("warn");
      saveHint = tr("Local changes are uploaded to this server as soon as it is reachable.");
    } else if (sk == QLatin1String("conflict")) {
      saveText = tr("Conflict");
      saveTone = QStringLiteral("warn");
      saveHint = tr("Decide when starting the game or on the Saves page in the server.");
    } else {
      saveText = tr("No save yet");
    }
    m.insert(QStringLiteral("saveText"), saveText);
    m.insert(QStringLiteral("saveTone"), saveTone);
    m.insert(QStringLiteral("saveHint"), saveHint);
    m.insert(QStringLiteral("syncKind"), sk);
  }

  // Start checklist
  const bool busy = phase_ != PlayPhase::None;
  const bool romReady = kind == QLatin1String("ready");
  QVariantList list;
  const QString coreLabelForList = man != nullptr ? catalog_->coreLabel(*man, catalog_->locateCore(*man)) : QString();
  // Core step: what happens to the core at the start, in words (never a path).
  QString coreMeta;
  QString coreState = coreOk ? QStringLiteral("done")
                             : (phase_ == PlayPhase::Core ? QStringLiteral("active")
                                                          : (coreProvisionable ? QStringLiteral("pending") : QStringLiteral("error")));
  if (!coreOk && man != nullptr) {
    if (coreProvisionable) coreMeta = tr("%1 downloads from %2 at start").arg(coreLabelForList, hubLabel);
    else if (!hubUp && catalog_->hubOffersCore(*man)) coreMeta = tr("%1 must download from %2").arg(coreLabelForList, hubLabel);
    else coreMeta = coreHint.isEmpty() ? coreLabelForList : coreHint;  // the status text is the step title
  }
  // Step title follows the state (R-P13): "Core ready" is only shown while the core is usable or can still be provisioned.
  const bool coreWaitsForHub = !coreOk && !coreProvisionable && man != nullptr && !hubUp && catalog_->hubOffersCore(*man);
  const QString coreStepLabel = coreOk || coreProvisionable || phase_ == PlayPhase::Core ? tr("Core ready")
                                : coreWaitsForHub ? tr("Core needs the server")
                                                  : (coreStatusText.isEmpty() ? tr("Core missing") : coreStatusText);
  const bool saveLocal = man != nullptr && man->saveSource == QLatin1String("none");
  const QString saveState = phase_ == PlayPhase::Launching ? (saveReady_ ? QStringLiteral("done") : QStringLiteral("active")) : QStringLiteral("pending");
  const QString saveLabel = saveLocal ? tr("Local save folder prepared") : tr("Save checked with server");
  if (fromLibraryFolder) {
    // A game that starts from a library folder (3c-7): the file itself is the first step, no ROM download.
    list.append(checkItem(tr("Local file found"), QStringLiteral("done"),
                          game->onHub ? tr("same hash as the server") : tr("%1 · hash checked").arg(LibraryModel::formatSize(game->romSize))));
    list.append(checkItem(coreStepLabel, coreState, coreMeta));
    list.append(checkItem(saveLabel, saveState, saveNoteStart_));
  } else {
    list.append(checkItem(tr("Game data from server"), QStringLiteral("done")));
    if (man != nullptr && catalog_->nativeFirmware(*man)) {
      const bool fwDone = phase_ == PlayPhase::Rom || phase_ == PlayPhase::Launching;
      list.append(checkItem(tr("Firmware from server verified"),
                            fwDone ? QStringLiteral("done") : (phase_ == PlayPhase::Firmware ? QStringLiteral("active")
                                                               : (fwOk ? QStringLiteral("pending") : QStringLiteral("error"))),
                            QString()));
    }
    list.append(checkItem(romReady ? tr("ROM verified from cache") : tr("Download and check ROM"),
                          romReady ? QStringLiteral("done") : (phase_ == PlayPhase::Rom ? QStringLiteral("active") : QStringLiteral("pending")),
                          romReady ? QString() : romText));
    list.append(checkItem(saveLabel, saveState, saveNoteStart_));
    list.append(checkItem(coreStepLabel, coreState, coreMeta));
  }
  list.append(checkItem(tr("Emulator starting"), (phase_ == PlayPhase::Launching && saveReady_) ? QStringLiteral("active") : QStringLiteral("pending")));
  m.insert(QStringLiteral("checklist"), list);

  QString error = startError_;
  m.insert(QStringLiteral("error"), error);
  m.insert(QStringLiteral("busy"), busy);
  // The ROM still has to come from the Hub but the Hub cannot be reached (R-P3): no Play, with a reason.
  const bool romNeedsHub = !hubUp && !fromLibraryFolder && (kind == QLatin1String("download") || kind == QLatin1String("failed") || kind == QLatin1String("mismatch"));
  m.insert(QStringLiteral("canPlay"), !busy && man != nullptr && !romNeedsHub && (coreOk || coreProvisionable) && fwOk && kind != QLatin1String("validating") &&
                                          kind != QLatin1String("downloading") && kind != QLatin1String("unknown") && kind != QLatin1String("missing") && !fwChecking);
  // Why Play is disabled (3c-7): shown under the button, never a silent grey button.
  QString whyNot;
  if (!busy) {
    if (man == nullptr) {
      whyNot = tr("No emulator is known for .%1 files.").arg(ext);
    } else if (!coreOk && !coreProvisionable) {
      if (!hubUp && catalog_->hubOffersCore(*man))
        whyNot = tr("%1 is offline, so %2 cannot download. Try again when the server is back.").arg(hubLabel, coreLabelForList);
      else
        whyNot = tr("%1 is not on this device and %2 does not offer it.").arg(coreLabelForList, hubLabel);
    } else if (romNeedsHub) {
      whyNot = tr("%1 is offline, so the game cannot download. Try again when the server is back.").arg(hubLabel);
    } else if (fwBlocked) {
      whyNot = tr("Firmware is missing. Ask the server admin to provide it, then check again.");
    } else if (kind == QLatin1String("missing")) {
      whyNot = tr("The game file is missing on this device.");
    }
  }
  m.insert(QStringLiteral("playDisabledReason"), whyNot);
  // The one problem sentence of the detail pane and its fix (readable pass 5). fix: "" (Play as usual) | "installCore" (Play
  // installs the core first) | "retry" (the server cannot be reached) | "admin" (only an admin can fix it).
  QString problem = whyNot;
  QString fix;
  if (!busy && man != nullptr) {
    if (!coreOk && coreProvisionable) {
      problem = tr("No %1 core on this PC yet. %2 has %3 ready.").arg(game->system.toUpper(), hubLabel, coreLabelForList);
      fix = QStringLiteral("installCore");
    } else if (!coreOk) {
      fix = !hubUp && catalog_->hubOffersCore(*man) ? QStringLiteral("retry") : QStringLiteral("admin");
    } else if (romNeedsHub) {
      fix = QStringLiteral("retry");
    } else if (fwBlocked) {
      fix = QStringLiteral("admin");
    }
  }
  m.insert(QStringLiteral("problemText"), problem);
  m.insert(QStringLiteral("fixKind"), fix);
  m.insert(QStringLiteral("fileSizeText"), LibraryModel::formatSize(game->romSize));
  m.insert(QStringLiteral("hubMatchText"), game->romSha256.size() > 12 ? game->romSha256.left(4) + QStringLiteral("…") + game->romSha256.right(4) : game->romSha256);
  QString label = tr("Play");
  if (busy) {
    label = phase_ == PlayPhase::Launching ? tr("Starting…")
            : phase_ == PlayPhase::Core    ? tr("Loading core…")
            : phase_ == PlayPhase::Firmware ? tr("Checking firmware…")
                                            : tr("Downloading…");
  } else if (fix == QLatin1String("installCore")) {
    label = tr("Install core and play");
  } else if (kind == QLatin1String("download")) {
    label = tr("Download and play");
  } else if (kind == QLatin1String("mismatch") || kind == QLatin1String("failed")) {
    label = tr("Reload and play");
  }
  m.insert(QStringLiteral("playLabel"), label);

  // Status pill of the detail pane (3c): same priority as the tile status line.
  {
    QString pill = tr("Ready to play");
    QString tone = QStringLiteral("ok");
    const QString attention = model_.attentionText(game->id);
    if (kind == QLatin1String("mismatch")) {
      pill = tr("Hash mismatch");
      tone = QStringLiteral("error");
    } else if (!attention.isEmpty()) {
      pill = attention;
      tone = QStringLiteral("amber");
    } else if (kind == QLatin1String("failed")) {
      pill = tr("Download failed");
      tone = QStringLiteral("error");
    } else if (kind == QLatin1String("download")) {
      pill = tr("Download required");
      tone = QStringLiteral("neutral");
    } else if (kind == QLatin1String("downloading")) {
      pill = tr("Downloading");
      tone = QStringLiteral("neutral");
    } else if (kind != QLatin1String("ready")) {
      pill = tr("Verifying");
      tone = QStringLiteral("neutral");
    } else if (!coreOk && !coreProvisionable) {
      pill = hubUp ? tr("Core missing") : tr("Needs attention");
      tone = QStringLiteral("amber");
    } else if (fromLibraryFolder) {
      pill = game->onHub ? tr("Local · on server") : tr("Local");
      tone = game->onHub ? QStringLiteral("info") : QStringLiteral("neutral");
    }
    m.insert(QStringLiteral("pillText"), pill);
    m.insert(QStringLiteral("pillTone"), tone);
    m.insert(QStringLiteral("coreLabelText"), man != nullptr ? catalog_->coreLabel(*man, catalog_->locateCore(*man)) : QString());
    m.insert(QStringLiteral("coreVersionText"), man != nullptr ? coreVersions_.value(man->coreId) : QString());
    m.insert(QStringLiteral("coreExperimental"), man != nullptr && man->experimental);
    m.insert(QStringLiteral("coreNotice"), man != nullptr ? catalog_->coreNoticeFor(man->systemId, game->id) : QString());
  }
  return m;
}

}  // namespace uzume::ui
