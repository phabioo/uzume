#include "savesync.h"

#include "dsvsave.h"
#include "sdtree.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>
#include <QLoggingCategory>
#include <algorithm>
#include <memory>

namespace uzume {

Q_LOGGING_CATEGORY(lcSaveSync, "uzume.savesync")

SaveSync::SaveSync(HubConnection* connection, ProfileStore* profiles, QObject* parent)
    : QObject(parent), conn_(connection), profiles_(profiles), api_(connection, this) {
  qRegisterMetaType<uzume::SaveSync::ConflictView>();
  qRegisterMetaType<uzume::SaveSync::Kind>();
  pollTimer_.setInterval(timing_.pollMs);
  connect(&pollTimer_, &QTimer::timeout, this, &SaveSync::poll);
  retryTimer_.setSingleShot(true);
  connect(&retryTimer_, &QTimer::timeout, this, &SaveSync::retryPending);
  // The start flow ends with one of these (beginSession() follows startReady); until then it owns its slot's sync.json.
  connect(this, &SaveSync::startReady, this, [this]() { starting_ = false; });
  connect(this, &SaveSync::startFailed, this, [this]() { starting_ = false; });
  connect(this, &SaveSync::startConflict, this, [this]() { starting_ = false; });
  connect(conn_, &HubConnection::stateChanged, this, [this](HubConnection::State s) {
    if (s == HubConnection::State::Connected) {
      retryBackoffMs_ = 0;
      retryPending();
    } else {
      retryTimer_.stop();
    }
  });
}

QString SaveSync::kindName(Kind k) {
  switch (k) {
    case Kind::Synced: return QStringLiteral("synced");
    case Kind::Pending: return QStringLiteral("pending");
    case Kind::Conflict: return QStringLiteral("conflict");
    default: return QStringLiteral("none");
  }
}

void SaveSync::setTiming(const Timing& t) {
  timing_ = t;
  pollTimer_.setInterval(std::max(5, t.pollMs));
}

bool SaveSync::hubSupportsSaves() const {
  return conn_->state() == HubConnection::State::Connected && conn_->hubHasFeature(QString::fromLatin1(kSavesFeature));
}

bool SaveSync::available() const {
  return hubSupportsSaves() && SaveStore::isSafeId(conn_->hubUserId());
}

QString SaveSync::note() const {
  if (conn_->state() != HubConnection::State::Connected) {
    return {};
  }
  if (!hubSupportsSaves()) {
    return unsupportedNote();
  }
  return available() ? QString() : tr("Save sync is off: server user unknown");
}

bool SaveSync::hubSupportsSavesV2() const {
  return hubSupportsSaves() && conn_->hubHasFeature(QString::fromLatin1(kSavesV2Feature));
}

bool SaveSync::hubSupportsSavesV3() const {
  return hubSupportsSavesV2() && conn_->hubHasFeature(QString::fromLatin1(kSavesV3Feature));
}

bool SaveSync::hubSupportsSavesV5() const {
  return hubSupportsSavesV2() && conn_->hubHasFeature(QString::fromLatin1(kSavesV5Feature));
}

bool SaveSync::hubSupportsSavesV6() const {
  return hubSupportsSavesV2() && conn_->hubHasFeature(QString::fromLatin1(kSavesV6Feature));
}

bool SaveSync::hubSupportsSavesV4() const {
  return hubSupportsSavesV2() && conn_->hubHasFeature(QString::fromLatin1(kSavesV4Feature));
}

QString SaveSync::slotFor(const QString& gameId) const {
  const QString hubId = conn_->hubId();
  if (settings_ != nullptr) {
    return settings_->saveSlot(hubId, gameId);
  }
  return memorySlots_.value(hubId + QLatin1Char('/') + gameId, QStringLiteral("default"));
}

bool SaveSync::setSlot(const QString& gameId, const QString& slot) {
  if (!SaveStore::isValidSlotName(slot) || gameId.isEmpty()) {
    return false;
  }
  const QString hubId = conn_->hubId();
  if (settings_ != nullptr) {
    if (!settings_->setSaveSlot(hubId, gameId, slot)) {
      return false;
    }
  } else {
    memorySlots_.insert(hubId + QLatin1Char('/') + gameId, slot);
  }
  refreshKinds({gameId});
  return true;
}

QString SaveSync::slotDirFor(const QString& gameId, const QString& slot) const {
  return SaveStore::slotDir(*profiles_, conn_->hubId(), conn_->hubUserId(), gameId, slot);
}

bool SaveSync::sameHub(const QString& hubId) const {
  return conn_->state() == HubConnection::State::Connected && !hubId.isEmpty() && conn_->hubId() == hubId;
}

QString SaveSync::noteForOffline() const { return tr("Server not reachable. Playing with the local save; sync is pending."); }

void SaveSync::setKind(const QString& gameId, const SyncState& st, bool hasFile) {
  Kind k = Kind::None;
  if (!st.conflictId.isEmpty()) {
    k = Kind::Conflict;
  } else if (st.pending) {
    k = Kind::Pending;
  } else if (!st.lastSyncedSha256.isEmpty() && hasFile) {
    k = Kind::Synced;
  }
  kindsTouched_.insert(gameId);
  if (kinds_.value(gameId, Kind::None) != k) {
    kinds_.insert(gameId, k);
    emit kindChanged(gameId, k);
  }
}

void SaveSync::persist() {
  if (a_.dir.isEmpty()) {
    return;
  }
  if (!SaveStore::saveState(a_.dir, a_.st)) {
    qCWarning(lcSaveSync) << "sync.json could not be written";
  }
  setKind(a_.gameId, a_.st, QFileInfo(a_.file).isFile());
}

namespace {
// Classification of a slot directory from its sync.json and save file. Read-only file access: safe on a worker.
SaveSync::Kind kindOfSlotDir(const QString& dir) {
  using Kind = SaveSync::Kind;
  if (dir.isEmpty() || !QFileInfo::exists(SaveStore::stateFilePath(dir))) {
    return Kind::None;
  }
  const SyncState st = SaveStore::loadState(dir);
  const bool hasFile = !SaveStore::findSaveFile(dir, QString()).isEmpty();
  if (!st.conflictId.isEmpty()) {
    return Kind::Conflict;
  }
  if (st.pending) {
    return Kind::Pending;
  }
  if (!st.lastSyncedSha256.isEmpty() && hasFile) {
    return Kind::Synced;
  }
  return Kind::None;
}
}  // namespace

void SaveSync::refreshKinds(const QStringList& gameIds) {
  const bool on = available();
  for (const QString& id : gameIds) {
    const Kind k = on ? kindOfSlotDir(slotDirFor(id, slotFor(id))) : Kind::None;
    kindsTouched_.insert(id);
    if (kinds_.value(id, Kind::None) != k) {
      kinds_.insert(id, k);
      emit kindChanged(id, k);
    }
  }
}

// Same classification as refreshKinds, but the file access runs on a worker and the changes arrive as ONE kindsRefreshed
// signal (no kindChanged per game). A kind that setKind changed while the worker ran is newer and is kept.
void SaveSync::refreshKindsAsync(const QStringList& gameIds) {
  const bool on = available();
  QList<QPair<QString, QString>> jobs;  // game id, slot directory (empty: no state)
  jobs.reserve(gameIds.size());
  for (const QString& id : gameIds) {
    jobs.append({id, on ? slotDirFor(id, slotFor(id)) : QString()});
  }
  kindsTouched_.clear();
  const quint64 gen = ++kindsGen_;
  if (!kindsWatcher_) {
    kindsWatcher_ = std::make_unique<QFutureWatcher<QList<QPair<QString, int>>>>();
  }
  kindsWatcher_->disconnect(this);
  connect(kindsWatcher_.get(), &QFutureWatcherBase::finished, this, [this, gen]() {
    if (gen != kindsGen_) {
      return;  // superseded by a newer refresh
    }
    QStringList changed;
    for (const auto& r : kindsWatcher_->result()) {
      if (kindsTouched_.contains(r.first)) continue;
      const Kind k = static_cast<Kind>(r.second);
      if (kinds_.value(r.first, Kind::None) != k) {
        kinds_.insert(r.first, k);
        changed.append(r.first);
      }
    }
    kindsTouched_.clear();
    emit kindsRefreshed(changed);
  });
  kindsWatcher_->setFuture(QtConcurrent::run([jobs]() {
    QList<QPair<QString, int>> out;
    out.reserve(jobs.size());
    for (const auto& j : jobs) {
      out.append({j.first, static_cast<int>(kindOfSlotDir(j.second))});
    }
    return out;
  }));
}

// ---------------------------------------------------------------- Start sync

namespace {
SaveSnapshotResult snapshotResultOf(const SaveApiResult& r);
}

void SaveSync::prepareStart(const QString& gameId, const QString& romPath, const QStringList& romBasenames, const CoreRef& core) {
  ++gen_;
  pollTimer_.stop();
  session_ = false;
  starting_ = true;  // until beginSession() or the failed start's finalSync()
  dialogOpen_ = false;
  a_ = Active{};
  a_.gameId = gameId;
  a_.romPath = romPath;
  a_.hubId = conn_->hubId();
  a_.userId = conn_->hubUserId();
  a_.slot = slotFor(gameId);
  a_.dir = SaveStore::slotDir(*profiles_, a_.hubId, a_.userId, gameId, a_.slot);
  const quint64 gen = gen_;
  if (a_.dir.isEmpty()) {
    QTimer::singleShot(0, this, [this, gen, gameId]() {
      if (gen == gen_) {
        emit startFailed(gameId, tr("The save directory for this game is unknown."));
      }
    });
    return;
  }
  QDir().mkpath(a_.dir);
  if (core.saveSource == QLatin1String("none")) {
    a_.localOnly = true;
    QTimer::singleShot(0, this, [this, gen, gameId]() {
      if (gen == gen_) {
        emit startReady(gameId, a_.dir, localOnlyNote());
      }
    });
    return;
  }
  a_.expectedName = SaveStore::expectedSaveName(romPath);
  a_.exactSave = core.valid() && core.saveSource != QLatin1String("auto");
  const bool dsvCore = core.valid() && core.saveSource == QLatin1String("core_file") && core.fileFormat == QLatin1String("desmume_dsv");
  const bool treeCore = core.valid() && core.saveSource == QLatin1String("core_tree") && core.fileFormat == QLatin1String("azahar_sdmc");
  if (dsvCore || treeCore) {
    if (dsvCore) {
      a_.coreFileExt = core.fileExtension;
    }
    a_.coreFileFormat = core.fileFormat;
    if (core.shortDir) {
      a_.coreDir = SaveStore::shortCoreDir(*profiles_, a_.hubId, a_.userId, gameId, a_.slot);
      if (a_.coreDir.isEmpty()) {
        failStart(gen, tr("The save directory for this game is unknown."));
        return;
      }
      QDir().mkpath(a_.coreDir);
      if (treeCore) {
        // One-time adoption of a tree the core (or the local-only mode) wrote to the long directory earlier; never overwrites.
        if (SaveStore::migrateCoreSubfolder(a_.dir, a_.coreDir, core.subfolder)) {
          qCInfo(lcSaveSync) << "Core save tree" << core.subfolder << "moved to the short core directory";
        }
      } else {
        // One-time adoption of a .dsv the core wrote to the long directory earlier; never overwrites a file in the short one.
        const QString name = SaveStore::expectedSaveName(romPath, a_.coreFileExt);
        switch (SaveStore::migrateCoreFile(a_.dir, a_.coreDir, name)) {
          case SaveStore::FileMove::Moved:
            qCInfo(lcSaveSync) << "Core save file" << name << "moved to the short core directory";
            break;
          case SaveStore::FileMove::KeptBoth:
            qCWarning(lcSaveSync) << "Core save file" << name << "exists in the old and the short core directory; the old one was kept untouched";
            break;
          case SaveStore::FileMove::Failed:
            qCWarning(lcSaveSync) << "Core save file" << name << "could not be moved to the short core directory";
            break;
          case SaveStore::FileMove::None:
            break;
        }
      }
    }
    if (treeCore) {
      const QString base = a_.coreDir.isEmpty() ? a_.dir : a_.coreDir;
      a_.coreTree = core.subfolder.isEmpty() ? base : QDir(base).filePath(core.subfolder);
    }
  }
  a_.st = SaveStore::loadState(a_.dir);
  a_.st.slot = a_.slot;
  if (!a_.coreFileExt.isEmpty() || !a_.coreTree.isEmpty()) {
    // The core's own save of the last session may hold progress the raw save does not have yet: bring it in first.
    QString err;
    if (!importCoreFile(true, &err)) {
      failStart(gen, tr("The save file %1 of %2 could not be read (%3). It was left untouched and the game was not started, so no progress is lost.")
                         .arg(a_.coreTree.isEmpty() ? QFileInfo(coreFilePath()).fileName() : tr("save data"), core.id, err));
      return;
    }
  }
  QStringList warnings;
  a_.file = findLocalSave(&warnings);
  for (const QString& w : warnings) {
    qCWarning(lcSaveSync) << w;
  }
  if (a_.file.isEmpty()) {
    if (a_.slot == QLatin1String("default") &&
        SaveStore::migrateLegacy(SaveStore::legacyDir(*profiles_, a_.hubId), romBasenames, a_.dir, a_.expectedName)) {
      qCInfo(lcSaveSync) << "Legacy save copied into the per-user save directory";
      const SyncState before = a_.st;
      a_.st = SyncState{};
      a_.st.writerCoreId = before.writerCoreId;
      a_.st.writerCoreVersion = before.writerCoreVersion;
      a_.st.coreFileSha256 = before.coreFileSha256;
      a_.st.pending = true;  // counts as an unsynced local change (base 0)
      a_.file = QDir(a_.dir).filePath(a_.expectedName);
      persist();
    } else {
      a_.file = QDir(a_.dir).filePath(a_.expectedName);
    }
  }
  if (core.valid()) {
    guardCoreChange(gen, core);
    return;
  }
  continueStart(gen);
}

void SaveSync::continueStart(quint64 gen) {
  if (!available()) {
    QTimer::singleShot(0, this, [this, gen]() {
      if (gen == gen_) {
        emitStartReady(note());
      }
    });
    return;
  }
  startSync();
}

QString SaveSync::findLocalSave(QStringList* warnings) const {
  return SaveStore::findSaveFile(a_.dir, a_.expectedName, warnings, a_.exactSave);
}

QString SaveSync::coreFilePath() const {
  return a_.coreFileExt.isEmpty() ? QString() : QDir(a_.coreDir.isEmpty() ? a_.dir : a_.coreDir).filePath(a_.gameId.isEmpty() ? QString() : SaveStore::expectedSaveName(a_.romPath, a_.coreFileExt));
}

static_assert(sdtree::kMaxTreeBytes == kMaxSaveBytes, "the save tree limit must equal the server's save size limit");

bool SaveSync::importCoreTree(bool backup, QString* error) {
  const auto tar = sdtree::pack(a_.coreTree, error);
  if (!tar) {
    return false;  // never upload a partial tree
  }
  if (tar->isEmpty()) {
    return true;  // no save data in the tree (yet)
  }
  const QString sha = SaveStore::sha256Of(*tar);
  if (sha == a_.st.coreFileSha256) {
    return true;  // nothing new since the last export / import
  }
  const QString rawFile = QDir(a_.dir).filePath(a_.expectedName);
  QFile cur(rawFile);
  const bool same = cur.open(QIODevice::ReadOnly) && cur.readAll() == *tar;
  cur.close();
  if (!same) {
    if (backup) {
      SaveStore::backupFile(rawFile, QStringLiteral("core-file"));
    }
    if (!SaveStore::atomicWrite(rawFile, *tar)) {
      *error = tr("the raw save could not be written");
      return false;
    }
  }
  a_.file = rawFile;
  a_.st.coreFileSha256 = sha;
  persist();
  return true;
}

bool SaveSync::exportCoreTree(QString* error) {
  const QString rawFile = QDir(a_.dir).filePath(a_.expectedName);
  QFile in(rawFile);
  if (!in.open(QIODevice::ReadOnly)) {
    return true;  // no save yet: the core starts fresh
  }
  const QByteArray raw = in.readAll();
  in.close();
  const auto cur = sdtree::pack(a_.coreTree, error);
  if (!cur) {
    return false;
  }
  const QString rawSha = SaveStore::sha256Of(raw);
  if (!cur->isEmpty() && SaveStore::sha256Of(*cur) == rawSha) {
    a_.st.coreFileSha256 = rawSha;
    persist();
    return true;
  }
  if (!cur->isEmpty()) {  // the tree is about to be replaced: keep what is there
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    if (!SaveStore::atomicWrite(QStringLiteral("%1.tree-%2.bak").arg(rawFile, stamp), *cur)) {
      *error = tr("the backup of the current save data could not be written");
      return false;
    }
  }
  if (!sdtree::unpack(raw, a_.coreTree, error)) {
    return false;
  }
  // The archive may not be canonical (e.g. uploaded by another tool): remember what the tree really packs to.
  const auto fresh = sdtree::pack(a_.coreTree, error);
  if (!fresh) {
    return false;
  }
  a_.st.coreFileSha256 = fresh->isEmpty() ? rawSha : SaveStore::sha256Of(*fresh);
  a_.coreTreeFp = sdtree::fingerprint(a_.coreTree);
  persist();
  return true;
}

bool SaveSync::importCoreFile(bool backup, QString* error) {
  if (!a_.coreTree.isEmpty()) {
    return importCoreTree(backup, error);
  }
  const QString path = coreFilePath();
  QFile in(path);
  if (path.isEmpty() || !in.exists()) {
    return true;
  }
  if (!in.open(QIODevice::ReadOnly)) {
    *error = in.errorString();
    return false;
  }
  const QByteArray data = in.readAll();
  in.close();
  const QString sha = SaveStore::sha256Of(data);
  if (sha == a_.st.coreFileSha256) {
    return true;  // nothing new since the last export / import
  }
  const auto raw = dsv::dsvToRaw(data, error);
  if (!raw) {
    return false;  // never convert or upload what cannot be parsed
  }
  const QString rawFile = QDir(a_.dir).filePath(a_.expectedName);
  QFile cur(rawFile);
  const bool same = cur.open(QIODevice::ReadOnly) && cur.readAll() == *raw;
  cur.close();
  if (!same) {
    if (backup) {
      SaveStore::backupFile(rawFile, QStringLiteral("core-file"));
    }
    if (!SaveStore::atomicWrite(rawFile, *raw)) {
      *error = tr("the raw save could not be written");
      return false;
    }
  }
  a_.file = rawFile;
  a_.st.coreFileSha256 = sha;
  persist();
  return true;
}

bool SaveSync::exportCoreFile(QString* error) {
  if (!a_.coreTree.isEmpty()) {
    return exportCoreTree(error);
  }
  const QString path = coreFilePath();
  const QString rawFile = QDir(a_.dir).filePath(a_.expectedName);
  QFile in(rawFile);
  if (path.isEmpty() || !in.open(QIODevice::ReadOnly)) {
    return true;  // no save yet: the core starts fresh
  }
  const QByteArray raw = in.readAll();
  in.close();
  const auto d = dsv::rawToDsv(raw, error);
  if (!d) {
    return false;
  }
  if (!SaveStore::atomicWrite(path, *d)) {
    *error = tr("the file could not be written");
    return false;
  }
  a_.st.coreFileSha256 = SaveStore::sha256Of(*d);
  persist();
  return true;
}

bool SaveSync::pullCoreFile(bool force) {
  if (!a_.coreTree.isEmpty()) {
    const QString fp = sdtree::fingerprint(a_.coreTree);
    if (fp.isEmpty() || (!force && fp == a_.coreTreeFp)) {
      return true;
    }
    a_.coreTreeFp = fp;
    QString err;
    if (importCoreTree(false, &err)) {
      return true;
    }
    if (force) {
      emit coreSaveProblem(a_.gameId, tr("The save data of the game could not be packed (%1). It stays on this device untouched and was not uploaded; your server save is unchanged.").arg(err));
    }
    return false;
  }
  const QString path = coreFilePath();
  if (path.isEmpty()) {
    return true;
  }
  const QFileInfo fi(path);
  if (!fi.exists()) {
    return true;
  }
  if (!force && fi.size() == a_.coreFileSize && fi.lastModified() == a_.coreFileMtime) {
    return true;
  }
  a_.coreFileSize = fi.size();
  a_.coreFileMtime = fi.lastModified();
  QString err;
  if (importCoreFile(false, &err)) {
    return true;
  }
  if (force) {  // a half-written file is retried silently; a forced pull is the last chance of the session
    emit coreSaveProblem(a_.gameId, tr("The save file %1 could not be converted (%2). It stays on this device untouched and was not uploaded; your server save is unchanged.")
                                        .arg(fi.fileName(), err));
  }
  return false;
}

void SaveSync::emitStartReady(const QString& note) {
  QString err;
  if ((!a_.coreFileExt.isEmpty() || !a_.coreTree.isEmpty()) && !exportCoreFile(&err)) {
    emit startFailed(a_.gameId, tr("The save cannot be prepared for the core (%1). The game was not started, so your save stays untouched.").arg(err));
    return;
  }
  emit startReady(a_.gameId, a_.coreDir.isEmpty() ? a_.dir : a_.coreDir, a_.coreNote.isEmpty() ? note : (note.isEmpty() ? a_.coreNote : a_.coreNote + QLatin1Char(' ') + note));
}

QString SaveSync::coreChangeLabel(const CoreRef& from, const CoreRef& to) {
  QString label = QStringLiteral("Before core change: %1 %2 \u2192 %3 %4").arg(from.id, from.version, to.id, to.version);
  QByteArray bytes = label.toUtf8();
  if (bytes.size() > 64) {  // Hub limit; cut on a character boundary
    int n = 64;
    while (n > 0 && (static_cast<unsigned char>(bytes.at(n)) & 0xC0) == 0x80) {
      --n;
    }
    label = QString::fromUtf8(bytes.left(n));
  }
  return label;
}

void SaveSync::recordCore(const CoreRef& core) {
  a_.st.writerCoreId = core.id;
  a_.st.writerCoreVersion = core.version;
  persist();
}

void SaveSync::failStart(quint64 gen, const QString& message) {
  QTimer::singleShot(0, this, [this, gen, message]() {
    if (gen == gen_) {
      emit startFailed(a_.gameId, message);
    }
  });
}

// ADR 0020 D7: a save written by another core (or another build of it) is snapshotted on the Hub before the new core
// touches it. Never risk a save silently: a failed snapshot blocks the start.
void SaveSync::guardCoreChange(quint64 gen, const CoreRef& core) {
  CoreRef old;
  old.id = a_.st.writerCoreId;
  old.version = a_.st.writerCoreVersion;
  if (old.id.isEmpty()) {  // first start with this save directory: just record
    recordCore(core);
    continueStart(gen);
    return;
  }
  if (old.id == core.id && old.version == core.version) {
    continueStart(gen);
    return;
  }
  const bool haveLocal = QFileInfo(a_.file).isFile();
  if (haveLocal) {
    SaveStore::backupFile(a_.file, QStringLiteral("core-change"));  // local copy, also covers unsynced changes
  }
  const QString label = coreChangeLabel(old, core);
  const QString change = tr("This game was last played with %1 %2 and now starts with %3 %4.").arg(old.id, old.version, core.id, core.version);
  if (available() && hubSupportsSavesV2()) {
    const QString slot = a_.slot;
    const QString gameId = a_.gameId;
    api_.createSnapshot(gameId, slot, label, [this, gen, core, change, label](const SaveApiResult& r) {
      if (gen != gen_) {
        return;
      }
      const SnapshotResult res = snapshotResultOf(r);
      using SK = SnapshotResult::Outcome;
      if (res.kind == SK::Ok || res.kind == SK::NotFound) {  // NotFound: the Hub has no save to protect (a local one is uploaded first)
        if (res.kind == SK::Ok) {
          a_.coreNote = tr("Core changed: %1 A snapshot \"%2\" of your save was created on the server first.").arg(change, label);
        }
        recordCore(core);
        continueStart(gen);
        return;
      }
      const QString why = res.kind == SK::Offline
                              ? tr("The server is not reachable, so the snapshot of your save that is required before a core change could not be created.")
                              : res.message;
      emit startFailed(a_.gameId, tr("%1 %2 The game was not started, so your save stays untouched. Switch back to the previous core or try again when the server is reachable.").arg(change, why));
    });
    return;
  }
  if (conn_->state() == HubConnection::State::Connected) {
    // The Hub has no snapshots or no save sync at all: nothing on the Hub to protect, the local copy above remains.
    a_.coreNote = tr("Core changed: %1 A backup copy of the local save was made.").arg(change);
    recordCore(core);
    continueStart(gen);
    return;
  }
  if (haveLocal) {
    failStart(gen, tr("%1 The server is not reachable, so the snapshot of your save that is required before a core change could not be created. "
                      "The game was not started, so your save stays untouched. Switch back to the previous core or connect to the server.")
                       .arg(change));
    return;
  }
  recordCore(core);
  continueStart(gen);
}

void SaveSync::startSync() {
  const quint64 gen = ++gen_;
  api_.getSlot(a_.gameId, a_.st.slot, [this, gen](const SaveApiResult& r) { onSlot(gen, r); });
}

void SaveSync::readyToPlay(const QString& note) {
  dialogOpen_ = false;
  setKind(a_.gameId, a_.st, QFileInfo(a_.file).isFile());
  emitStartReady(note);
}

void SaveSync::onSlot(quint64 gen, const SaveApiResult& r) {
  if (gen != gen_) {
    return;
  }
  using K = SaveApiResult::Kind;
  const bool hasFile = QFileInfo(a_.file).isFile();
  switch (r.kind) {
    case K::Ok:
      onHubSlot(gen, *r.slot);
      return;
    case K::NotFound: {
      if (!hasFile) {
        a_.st.baseRevision = 0;
        a_.st.lastSyncedSha256.clear();
        a_.st.pending = false;
        a_.st.conflictId.clear();
        readyToPlay(QString());  // start empty
        return;
      }
      a_.st.baseRevision = 0;
      a_.st.lastSyncedSha256.clear();
      uploadAtStart(gen, SaveStore::sha256OfFile(a_.file));
      return;
    }
    case K::Offline: {
      if (hasFile) {
        const QString l = SaveStore::sha256OfFile(a_.file);
        if (l != a_.st.lastSyncedSha256) {
          a_.st.pending = true;
          persist();
        }
      }
      readyToPlay(noteForOffline());
      return;
    }
    default:
      a_.st.lastError = r.errorCode;
      persist();
      readyToPlay(tr("Save sync is not available (%1). Playing with the local save.").arg(r.errorCode));
      return;
  }
}

void SaveSync::onHubSlot(quint64 gen, const SaveSlotInfo& slot) {
  const int h = slot.current.revision;
  const std::optional<SaveConflictInfo> own = ownConflict(slot, a_.st.conflictId);
  if (own) {
    showConflict(*own);
    return;
  }
  if (!a_.st.conflictId.isEmpty()) {
    a_.st.conflictId.clear();  // resolved elsewhere (e.g. by an admin through the admin API)
    persist();
  }
  const bool hasFile = QFileInfo(a_.file).isFile();
  if (!hasFile) {
    downloadAndApply(gen, false, QString());
    return;
  }
  const QString l = SaveStore::sha256OfFile(a_.file);
  if (l == a_.st.lastSyncedSha256) {
    if (h > a_.st.baseRevision) {
      downloadAndApply(gen, true, QString());
    } else {
      a_.st.pending = false;
      persist();
      readyToPlay(QString());
    }
    return;
  }
  uploadAtStart(gen, l);
}

void SaveSync::downloadAndApply(quint64 gen, bool backupLocal, const QString&) {
  api_.getContent(a_.gameId, a_.st.slot, [this, gen, backupLocal](const SaveApiResult& r) {
    if (gen != gen_) {
      return;
    }
    using K = SaveApiResult::Kind;
    if (r.kind == K::Ok) {
      if (backupLocal) {
        SaveStore::backupFile(a_.file);
      }
      if (!SaveStore::atomicWrite(a_.file, r.content)) {
        a_.st.lastError = QStringLiteral("write_failed");
        persist();
        readyToPlay(tr("The server save could not be written locally."));
        return;
      }
      a_.st.baseRevision = r.contentRevision;
      a_.st.lastSyncedSha256 = SaveStore::sha256Of(r.content);
      a_.st.pending = false;
      a_.st.conflictId.clear();
      a_.st.lastError.clear();
      persist();
      readyToPlay(QString());
      return;
    }
    if (r.kind == K::Offline) {
      if (QFileInfo(a_.file).isFile()) {
        a_.st.pending = true;
        persist();
      }
      readyToPlay(noteForOffline());
      return;
    }
    a_.st.lastError = r.errorCode;
    persist();
    readyToPlay(tr("Save sync is not available (%1). Playing with the local save.").arg(r.errorCode));
  });
}

void SaveSync::uploadAtStart(quint64 gen, const QString& sha) {
  QFile f(a_.file);
  if (!f.open(QIODevice::ReadOnly)) {
    readyToPlay(QString());
    return;
  }
  const QByteArray data = f.readAll();
  f.close();
  a_.st.pending = true;  // persisted before the attempt: survives a crash or timeout
  persist();
  api_.putSave(a_.gameId, a_.st.slot, data, a_.st.baseRevision, QStringLiteral("checkpoint"),
               [this, gen, sha](const SaveApiResult& r) {
                 if (gen != gen_) {
                   return;
                 }
                 using K = SaveApiResult::Kind;
                 switch (r.kind) {
                   case K::Ok:
                     a_.st.baseRevision = r.slot->current.revision;
                     a_.st.lastSyncedSha256 = sha;
                     a_.st.pending = false;
                     a_.st.conflictId.clear();
                     a_.st.lastError.clear();
                     persist();
                     emit uploaded(a_.gameId, a_.st.baseRevision);
                     readyToPlay(QString());
                     return;
                   case K::Conflict:
                     showConflict(*r.conflict);
                     return;
                   case K::Offline:
                     readyToPlay(noteForOffline());
                     return;
                   default:
                     a_.st.lastError = r.errorCode;
                     persist();
                     readyToPlay(tr("The save could not be uploaded (%1). Playing with the local save.").arg(r.errorCode));
                     return;
                 }
               });
}

std::optional<SaveConflictInfo> SaveSync::ownConflict(const SaveSlotInfo& slot, const QString& preferredId) const {
  std::optional<SaveConflictInfo> own;
  for (const SaveConflictInfo& c : slot.openConflicts) {
    const bool mine = conn_->deviceId().isEmpty() || c.securedDeviceId == conn_->deviceId();
    if (c.isOpen() && mine && (!own || c.id == preferredId)) {
      own = c;
    }
  }
  return own;
}

SaveSync::ConflictView SaveSync::makeConflictView(const SaveConflictInfo& c, const QString& gameId, const QString& file) const {
  ConflictView v;
  v.conflict = c;
  v.gameId = gameId;
  v.localDeviceName = profiles_->deviceName();
  const QFileInfo fi(file);
  v.localModified = fi.exists() ? fi.lastModified() : QDateTime();
  v.localSha256 = SaveStore::sha256OfFile(file);
  v.localBaseRevision = c.securedBaseRevision;
  return v;
}

void SaveSync::showConflict(const SaveConflictInfo& c) {
  a_.conflict = c;
  a_.st.conflictId = c.id;
  a_.st.pending = true;
  persist();
  dialogOpen_ = true;
  emit startConflict(makeConflictView(c, a_.gameId, a_.file));
}

void SaveSync::resolveConflict(Resolution res) {
  if (!dialogOpen_ || !a_.conflict) {
    return;
  }
  const SaveConflictInfo c = *a_.conflict;
  if (res == Resolution::DecideLater) {
    a_.st.conflictId = c.id;
    a_.st.pending = true;
    persist();
    a_.pausedByConflict = true;
    readyToPlay(tr("Conflict open: changes stay on this device until you decide."));
    return;
  }
  const quint64 gen = ++gen_;
  const auto fail = [this](const SaveApiResult& r) {
    using K = SaveApiResult::Kind;
    if (r.kind == K::Stale) {
      if (resolveCb_) {  // saves view: no reconcile (it would start the play flow); the view loads the conflict again
        resolveError(tr("The save changed on the server in the meantime. Nothing was changed; check the conflict again."), SaveRestoreResult::Outcome::Stale);
        return;
      }
      dialogOpen_ = false;
      startSync();  // the Hub changed meanwhile: reconcile again, dialog shows the new state
      return;
    }
    resolveError(r.kind == K::Offline ? tr("Server not reachable. Nothing was changed.")
                                      : tr("The conflict could not be resolved (%1). Nothing was changed.").arg(r.errorCode),
                 r.kind == K::Offline ? SaveRestoreResult::Outcome::Offline : SaveRestoreResult::Outcome::Failed);
  };
  if (res == Resolution::UseLocal) {
    api_.resolve(a_.gameId, a_.st.slot, c.id, QStringLiteral("use_local"), c.hubRevision, [this, gen, fail](const SaveApiResult& r) {
      if (gen != gen_) {
        return;
      }
      if (!r.ok()) {
        fail(r);
        return;
      }
      const SaveCheckpoint cur = r.slot->current;
      if (!QFileInfo(a_.file).isFile()) {
        // The local file is gone: the promoted checkpoint is now the Hub's current. Fetch it as the local save
        // instead of starting with an empty directory.
        a_.st.conflictId.clear();
        a_.st.baseRevision = cur.revision;
        a_.st.lastSyncedSha256.clear();
        a_.st.pending = false;
        persist();
        api_.getContent(a_.gameId, a_.st.slot, [this, gen, cur](const SaveApiResult& cr) {
          if (gen != gen_) {
            return;
          }
          if (cr.ok() && SaveStore::sha256Of(cr.content) == cur.sha256 && SaveStore::atomicWrite(a_.file, cr.content)) {
            a_.st.baseRevision = cr.contentRevision;
            a_.st.lastSyncedSha256 = cur.sha256;
            a_.st.pending = false;
            a_.st.lastError.clear();
            persist();
            resolvedOk(QString());
          } else if (resolveCb_) {
            resolvedOk(tr("Resolved on the server; the local save is fetched at the next start."));
          } else {
            dialogOpen_ = false;
            startSync();  // reconcile again: Hub slot without a local file -> download (or offline note)
          }
        });
        return;
      }
      const QString l = SaveStore::sha256OfFile(a_.file);
      a_.st.baseRevision = cur.revision;
      a_.st.lastSyncedSha256 = cur.sha256;
      a_.st.pending = l != cur.sha256;  // the local file kept changing: still unsynced
      a_.st.conflictId.clear();
      a_.st.lastError.clear();
      persist();
      resolvedOk(QString());
    });
    return;
  }
  // Use Hub version: download and verify first, then resolve against that revision, then replace the local file.
  api_.getContent(a_.gameId, a_.st.slot, [this, gen, fail, c](const SaveApiResult& cr) {
    if (gen != gen_) {
      return;
    }
    if (!cr.ok()) {
      fail(cr);
      return;
    }
    if (resolveLive_ && !liveAccepts(cr.content.size(), &cr.content)) {
      resolveError(tr("The running game cannot load this save. Nothing was changed; quit the game and resolve the conflict at the next start."));
      return;
    }
    // The local file must be backed up BEFORE anything changes; if that fails nothing is changed.
    if (QFileInfo(a_.file).isFile()) {
      const QString bak = backupHook_ ? backupHook_(a_.file, QStringLiteral("local")) : SaveStore::backupFile(a_.file, QStringLiteral("local"));
      if (bak.isEmpty()) {
        a_.st.lastError = QStringLiteral("backup_failed");
        persist();
        resolveError(tr("Could not back up the local save; nothing was changed."));
        return;
      }
    }
    const quint64 gen2 = ++gen_;
    api_.resolve(a_.gameId, a_.st.slot, c.id, QStringLiteral("use_hub"), cr.contentRevision,
                 [this, gen2, fail, cr](const SaveApiResult& r) {
                   if (gen2 != gen_) {
                     return;
                   }
                   if (!r.ok()) {
                     fail(r);
                     return;
                   }
                   if (!writeResolvedSave(cr.content)) {
                     a_.st.lastError = QStringLiteral("write_failed");
                     persist();
                     resolveError(resolveLive_ ? tr("The server save is now current, but the running game could not load it. Quit the game and start it again to use it.")
                                               : tr("The server save could not be written locally."));
                     return;
                   }
                   a_.st.baseRevision = r.slot->current.revision;
                   a_.st.lastSyncedSha256 = SaveStore::sha256Of(cr.content);
                   a_.st.pending = false;
                   a_.st.conflictId.clear();
                   a_.st.lastError.clear();
                   persist();
                   resolvedOk(QString());
                 });
  });
}

bool SaveSync::liveAccepts(qint64 size, const QByteArray* data) const {
  if (a_.coreTree.isEmpty()) {
    return live_.accepts && live_.accepts(size);
  }
  if (!live_.restartWith) {
    return false;
  }
  return data == nullptr || sdtree::validate(*data);  // history entries: validated again when the content arrives
}

bool SaveSync::liveApplyData(const QByteArray& data) {
  if (a_.coreTree.isEmpty()) {
    return live_.apply && live_.apply(data);
  }
  if (!live_.restartWith) {
    return false;
  }
  // Runs after the stop: the core has flushed the tree. Nothing may be lost: current tree -> raw save, new raw save,
  // backup of the tree + unpack (export).
  return live_.restartWith([this, data]() {
    QString err;
    if (!importCoreTree(false, &err)) {
      qCWarning(lcSaveSync) << "Live save: the current save data could not be packed:" << err;
      return false;
    }
    const QString rawFile = QDir(a_.dir).filePath(a_.expectedName);
    if (!SaveStore::atomicWrite(rawFile, data)) {
      return false;
    }
    a_.file = rawFile;
    if (!exportCoreTree(&err)) {
      qCWarning(lcSaveSync) << "Live save: the save data could not be written to the tree:" << err;
      return false;
    }
    return true;
  });
}

bool SaveSync::writeResolvedSave(const QByteArray& data) {
  if (resolveLive_) {
    return liveApplyData(data);  // the emulation thread writes memory and file, then resets the core
  }
  return SaveStore::atomicWrite(a_.file, data);
}

void SaveSync::resyncActiveFile() {
  const QFileInfo fi(a_.file);
  a_.lastMtime = fi.exists() ? fi.lastModified() : QDateTime();
  a_.lastSize = fi.exists() ? fi.size() : -1;
  a_.lastHash = fi.exists() ? SaveStore::sha256OfFile(a_.file) : QString();
  a_.dirty = a_.st.pending && fi.exists();
  a_.sinceChange.restart();
}

void SaveSync::resolvedOk(const QString& note) {
  if (!resolveCb_) {
    readyToPlay(note);
    return;
  }
  const RestoreCallback cb = std::move(resolveCb_);
  resolveCb_ = nullptr;
  dialogOpen_ = false;
  setKind(a_.gameId, a_.st, QFileInfo(a_.file).isFile());
  RestoreResult r;
  r.kind = SaveRestoreResult::Outcome::Ok;
  r.message = note;
  r.revision = a_.st.baseRevision;
  r.localUpdated = true;
  if (resolveLive_) {
    a_.pausedByConflict = false;
    a_.conflict.reset();
    resyncActiveFile();
  } else {
    a_ = Active{};
  }
  resolveLive_ = false;
  cb(r);
}

void SaveSync::resolveError(const QString& message, SaveRestoreResult::Outcome kind) {
  if (!resolveCb_) {
    emit resolveFailed(message);
    return;
  }
  const RestoreCallback cb = std::move(resolveCb_);
  resolveCb_ = nullptr;
  dialogOpen_ = false;
  if (!resolveLive_) {
    a_ = Active{};
  }
  resolveLive_ = false;
  RestoreResult r;
  r.kind = kind;
  r.message = message;
  cb(r);
}

// ---------------------------------------------------------------- Session: checkpoints and final sync

void SaveSync::beginSession() {
  api_.setCheatsUsed(false);  // a new play session starts without the marker
  if (a_.gameId.isEmpty() || a_.localOnly) {  // local-only: no sync session, no polling, no final upload
    return;
  }
  session_ = true;
  starting_ = false;
  a_.pausedByConflict = !a_.st.conflictId.isEmpty();
  const QString found = findLocalSave();
  if (!found.isEmpty()) {
    a_.file = found;
  }
  const QFileInfo fi(a_.file);
  a_.lastMtime = fi.exists() ? fi.lastModified() : QDateTime();
  a_.lastSize = fi.exists() ? fi.size() : -1;
  a_.lastHash = fi.exists() ? SaveStore::sha256OfFile(a_.file) : QString();
  a_.dirty = fi.exists() && a_.st.pending && !a_.pausedByConflict;
  a_.sinceChange.start();
  if (available()) {
    pollTimer_.start();
  }
}

void SaveSync::poll() {
  if (!session_ || a_.gameId.isEmpty() || liveOp_) {
    return;
  }
  pullCoreFile(false);
  if (!QFileInfo::exists(a_.file)) {
    const QString found = findLocalSave();
    if (!found.isEmpty()) {
      a_.file = found;
    }
  }
  const QFileInfo fi(a_.file);
  if (fi.exists() && (fi.lastModified() != a_.lastMtime || fi.size() != a_.lastSize)) {
    a_.lastMtime = fi.lastModified();
    a_.lastSize = fi.size();
    const QString h = SaveStore::sha256OfFile(a_.file);
    if (!h.isEmpty() && h != a_.lastHash) {
      a_.lastHash = h;
      if (h != a_.st.lastSyncedSha256) {
        a_.dirty = true;
        a_.sinceChange.restart();
      } else {
        a_.dirty = false;
      }
    }
  }
  pump();
}

void SaveSync::pump() {
  if (!session_ || a_.uploading) {
    return;
  }
  if (a_.finalRequested != 0) {
    const bool end = a_.finalRequested == 2;
    uploadCurrent(end ? QStringLiteral("final_session_end") : QStringLiteral("final"),
                  [this](UploadOutcome o) { finishFinal(o == UploadOutcome::Clean || o == UploadOutcome::Uploaded); });
    return;
  }
  if (!a_.dirty || a_.pausedByConflict || liveOp_ || !available() || !sameHub(a_.hubId)) {
    return;
  }
  if (a_.sinceChange.elapsed() < timing_.debounceMs) {
    return;
  }
  if (a_.uploadedOnce && a_.sinceUpload.elapsed() < timing_.minIntervalMs) {
    return;
  }
  if (a_.failedOnce && a_.sinceFail.elapsed() < a_.backoffMs) {
    return;
  }
  uploadCurrent(QStringLiteral("checkpoint"), [](UploadOutcome) {});
}

void SaveSync::uploadCurrent(const QString& reason, std::function<void(UploadOutcome)> done) {
  QFile f(a_.file);
  if (!f.open(QIODevice::ReadOnly)) {
    done(UploadOutcome::Clean);  // no save file (yet)
    return;
  }
  const QByteArray data = f.readAll();
  f.close();
  const QString sha = SaveStore::sha256Of(data);
  a_.lastHash = sha;  // this is what we observed
  if (sha == a_.st.lastSyncedSha256) {
    if (a_.st.pending) {
      a_.st.pending = false;
      persist();
    }
    a_.dirty = false;
    done(UploadOutcome::Clean);
    return;
  }
  if (!a_.st.pending) {
    a_.st.pending = true;  // persisted before the attempt
    persist();
  }
  if (a_.pausedByConflict) {
    done(UploadOutcome::Conflict);
    return;
  }
  if (!available() || !sameHub(a_.hubId)) {
    done(UploadOutcome::Offline);
    return;
  }
  a_.uploading = true;
  const quint64 gen = gen_;
  api_.putSave(a_.gameId, a_.st.slot, data, a_.st.baseRevision, reason, [this, gen, sha, done](const SaveApiResult& r) {
    if (gen != gen_) {
      return;
    }
    a_.uploading = false;
    using K = SaveApiResult::Kind;
    UploadOutcome out = UploadOutcome::Failed;
    if (r.kind == K::Ok) {
      a_.st.baseRevision = r.slot->current.revision;
      a_.st.lastSyncedSha256 = sha;
      a_.st.pending = a_.lastHash != sha && !a_.lastHash.isEmpty();  // changed again meanwhile
      a_.st.lastError.clear();
      a_.dirty = a_.st.pending;
      a_.backoffMs = 0;
      a_.failedOnce = false;
      a_.uploadedOnce = true;
      a_.sinceUpload.restart();
      persist();
      emit uploaded(a_.gameId, a_.st.baseRevision);
      out = UploadOutcome::Uploaded;
    } else if (r.kind == K::Conflict) {
      a_.st.conflictId = r.conflict->id;
      a_.st.pending = true;
      a_.pausedByConflict = true;
      persist();
      out = UploadOutcome::Conflict;
    } else {
      const bool firstFailure = !a_.failedOnce && r.kind != K::Offline;
      a_.failedOnce = true;
      a_.backoffMs = std::min(timing_.retryMaxMs, std::max(timing_.retryBaseMs, a_.backoffMs * 2));
      a_.sinceFail.restart();
      a_.st.lastError = r.errorCode;
      persist();
      out = r.kind == K::Offline ? UploadOutcome::Offline : UploadOutcome::Failed;
      if (firstFailure) emit uploadFailed(a_.gameId, r.errorCode);
    }
    done(out);
    if (a_.finalRequested != 0) {
      pump();
    }
  });
}

void SaveSync::finalSync(bool sessionEnd) {
  if (!session_ || a_.gameId.isEmpty()) {
    starting_ = false;  // a start that failed before beginSession() ends here
    QTimer::singleShot(0, this, [this]() { emit finalSyncFinished(QString(), true); });
    return;
  }
  // Pick up the latest state of the file (the core just flushed it).
  pullCoreFile(true);
  const QString found = findLocalSave();
  if (!found.isEmpty()) {
    a_.file = found;
  }
  a_.finalRequested = sessionEnd ? 2 : std::max(a_.finalRequested, 1);
  if (sessionEnd) {
    pollTimer_.stop();
  }
  pump();
}

void SaveSync::finishFinal(bool ok) {
  const bool end = a_.finalRequested == 2;
  const QString id = a_.gameId;
  a_.finalRequested = 0;
  if (end) {
    pollTimer_.stop();
    session_ = false;
    a_ = Active{};
  }
  emit finalSyncFinished(id, ok);
}

bool SaveSync::finalSyncBlocking(bool sessionEnd) {
  if (!session_) {
    return true;
  }
  QEventLoop loop;
  bool done = false;
  bool ok = true;
  const QMetaObject::Connection c = connect(this, &SaveSync::finalSyncFinished, &loop, [&](const QString&, bool o) {
    done = true;
    ok = o;
    loop.quit();
  });
  QTimer timeout;
  timeout.setSingleShot(true);
  connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(timing_.finalTimeoutMs);
  finalSync(sessionEnd);
  if (!done) {
    loop.exec();
  }
  disconnect(c);
  if (!done) {
    // Timeout: the change stays pending (already persisted before the attempt) and is retried later.
    ++gen_;
    a_.finalRequested = 0;
    a_.uploading = false;
    if (sessionEnd) {
      pollTimer_.stop();
      session_ = false;
      a_ = Active{};
    }
    return false;
  }
  return ok;
}

// ---------------------------------------------------------------- Pending retry (only the current Hub)

void SaveSync::scheduleRetry() {
  retryBackoffMs_ = std::min(timing_.retryMaxMs, std::max(timing_.retryBaseMs, retryBackoffMs_ * 2));
  retryTimer_.start(retryBackoffMs_);
}

void SaveSync::retryPending() {
  if (retryRunning_ || !available()) {
    return;
  }
  const QString base = SaveStore::userSavesDir(*profiles_, conn_->hubId(), conn_->hubUserId());
  QList<RetryItem> items;
  const auto consider = [&](const QString& gameId, const QString& slot, const QString& dir) {
    if ((session_ || starting_) && gameId == a_.gameId && slot == a_.slot) {
      return;  // the running game is handled by its own watcher; a game in its start flow owns sync.json of its slot
    }
    const SyncState st = SaveStore::loadState(dir);
    if (st.pending && st.conflictId.isEmpty()) {
      items.append({gameId, slot, dir});
    }
  };
  for (const QString& name : QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    const QString dir = QDir(base).filePath(name);
    consider(name, QStringLiteral("default"), dir);
    for (const QString& slot : SaveStore::localSlots(dir)) {
      if (slot != QLatin1String("default")) {
        consider(name, slot, SaveStore::slotDirIn(dir, slot));
      }
    }
  }
  if (items.isEmpty()) {
    retryBackoffMs_ = 0;
    return;
  }
  retryRunning_ = true;
  retryNext(items);
}

void SaveSync::retryNext(QList<RetryItem> items) {
  if (items.isEmpty() || !available()) {
    retryRunning_ = false;
    retryBackoffMs_ = 0;
    return;
  }
  const QString hubId = conn_->hubId();
  const RetryItem item = items.takeFirst();
  const QString dir = item.dir;
  const QString gameId = item.gameId;
  const QString slot = item.slot;
  const bool chosen = slot == slotFor(gameId);  // the library badge shows the chosen slot only
  SyncState st = SaveStore::loadState(dir);
  st.slot = slot;
  const QString file = SaveStore::findSaveFile(dir, QString());
  const QByteArray data = [&]() {
    QFile f(file);
    return (!file.isEmpty() && f.open(QIODevice::ReadOnly)) ? f.readAll() : QByteArray();
  }();
  if (file.isEmpty()) {
    st.pending = false;
    SaveStore::saveState(dir, st);
    if (chosen) {
      setKind(gameId, st, false);
    }
    retryNext(items);
    return;
  }
  const QString sha = SaveStore::sha256Of(data);
  if (sha == st.lastSyncedSha256) {
    st.pending = false;
    SaveStore::saveState(dir, st);
    if (chosen) {
      setKind(gameId, st, true);
    }
    retryNext(items);
    return;
  }
  api_.putSave(gameId, slot, data, st.baseRevision, QStringLiteral("checkpoint"),
               [this, hubId, dir, gameId, chosen, sha, st, items](const SaveApiResult& r) mutable {
                 using K = SaveApiResult::Kind;
                 if (!sameHub(hubId)) {
                   retryRunning_ = false;  // never continue against another Hub
                   return;
                 }
                 if (r.kind == K::Ok) {
                   st.baseRevision = r.slot->current.revision;
                   st.lastSyncedSha256 = sha;
                   st.pending = false;
                   st.lastError.clear();
                   SaveStore::saveState(dir, st);
                   if (chosen) {
                     setKind(gameId, st, true);
                   }
                   emit uploaded(gameId, st.baseRevision);
                 } else if (r.kind == K::Conflict) {
                   st.conflictId = r.conflict->id;
                   SaveStore::saveState(dir, st);
                   if (chosen) {
                     setKind(gameId, st, true);
                   }
                 } else if (r.kind == K::Offline) {
                   retryRunning_ = false;
                   scheduleRetry();
                   return;
                 } else {
                   st.lastError = r.errorCode;
                   SaveStore::saveState(dir, st);
                 }
                 retryNext(items);
               });
}

// ---------------------------------------------------------------- Save comfort (saves_v2): restore, snapshots, push

QString SaveSync::pendingReason(const QString& gameId, const QString& slot) const {
  if (isRunning(gameId, slot)) {
    return tr("This game is running on this PC.");
  }
  const QString dir = slotDirFor(gameId, slot);
  if (dir.isEmpty()) {
    return tr("The save directory for this game is unknown.");
  }
  const SyncState st = SaveStore::loadState(dir);
  if (!st.conflictId.isEmpty()) {
    return tr("This slot has an open save conflict. Resolve it first.");
  }
  if (st.pending) {
    return tr("This slot has local changes that are not uploaded yet.");
  }
  const QString file = SaveStore::findSaveFile(dir, QString());
  if (!file.isEmpty() && SaveStore::sha256OfFile(file) != st.lastSyncedSha256) {
    return tr("This slot has local changes that are not uploaded yet.");
  }
  return {};
}

void SaveSync::restoreVersion(const QString& gameId, const QString& slot, int version, int expectedRevision,
                              const QString& localFileName, RestoreCallback cb) {
  using RK = RestoreResult::Outcome;
  const auto finish = [this, cb](RK kind, const QString& message, int revision = 0, bool local = false) {
    RestoreResult r;
    r.kind = kind;
    r.message = message;
    r.revision = revision;
    r.localUpdated = local;
    QTimer::singleShot(0, this, [cb, r]() { cb(r); });
  };
  if (!hubSupportsSavesV2() || !available()) {
    finish(RK::Failed, tr("The server does not support restoring saves."));
    return;
  }
  if (liveApplyAvailable(gameId, slot)) {
    restoreVersionLive(gameId, slot, version, expectedRevision, cb);
    return;
  }
  if (const QString why = pendingReason(gameId, slot); !why.isEmpty()) {
    finish(RK::Blocked, why);
    return;
  }
  const QString hubId = conn_->hubId();
  api_.restore(gameId, slot, version, expectedRevision, [=, this](const SaveApiResult& r) {
    using K = SaveApiResult::Kind;
    if (!sameHub(hubId)) {
      cb({RK::Failed, tr("The server connection changed."), 0, false});
      return;
    }
    switch (r.kind) {
      case K::Ok:
        break;
      case K::Stale:
        cb({RK::Stale, tr("The save changed on the server in the meantime. Nothing was restored."), 0, false});
        return;
      case K::NotFound:
        cb({RK::NotFound, tr("That version no longer exists on the server."), 0, false});
        return;
      case K::Offline:
        cb({RK::Offline, tr("Server not reachable. Nothing was changed."), 0, false});
        return;
      default:
        cb({RK::Failed, tr("The version could not be restored (%1). Nothing was changed.").arg(r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode),
            0, false});
        return;
    }
    const int newRevision = r.slot->current.revision;
    // The Hub restored. Replace the local save of the slot by the new checkpoint (download); nothing unsynced exists here.
    const QString dir = slotDirFor(gameId, slot);
    if (dir.isEmpty() || (SaveStore::findSaveFile(dir, QString()).isEmpty() && localFileName.isEmpty())) {
      cb({RK::Ok, QString(), newRevision, false});  // no local file: the next start downloads the checkpoint
      return;
    }
    api_.getContent(gameId, slot, [=, this](const SaveApiResult& cr) {
      if (!sameHub(hubId) || pendingReason(gameId, slot).isEmpty() == false) {
        cb({RK::Ok, QString(), newRevision, false});  // something started meanwhile: leave the local save to the next sync
        return;
      }
      if (!cr.ok()) {
        cb({RK::Ok, tr("Restored on the server; the local save will be updated at the next start."), newRevision, false});
        return;
      }
      QDir().mkpath(dir);
      QString file = SaveStore::findSaveFile(dir, localFileName);
      if (file.isEmpty()) {
        file = QDir(dir).filePath(localFileName);
      }
      if (QFileInfo(file).isFile() && SaveStore::backupFile(file, QStringLiteral("restore")).isEmpty()) {
        cb({RK::Ok, tr("Restored on the server; the local save could not be backed up and stays unchanged until the next start."),
            newRevision, false});
        return;
      }
      if (!SaveStore::atomicWrite(file, cr.content)) {
        cb({RK::Ok, tr("Restored on the server; the local save could not be written."), newRevision, false});
        return;
      }
      SyncState st = SaveStore::loadState(dir);
      st.slot = slot;
      st.baseRevision = cr.contentRevision;
      st.lastSyncedSha256 = SaveStore::sha256Of(cr.content);
      st.pending = false;
      st.conflictId.clear();
      st.lastError.clear();
      SaveStore::saveState(dir, st);
      if (slot == slotFor(gameId)) {
        setKind(gameId, st, true);
      }
      cb({RK::Ok, QString(), cr.contentRevision, true});
    });
  });
}

void SaveSync::uploadSaveFile(const QString& gameId, const QString& slot, const QString& filePath, int expectedRevision,
                              const QString& localFileName, RestoreCallback cb) {
  using RK = RestoreResult::Outcome;
  const auto finish = [this, cb](RK kind, const QString& message) {
    RestoreResult r;
    r.kind = kind;
    r.message = message;
    QTimer::singleShot(0, this, [cb, r]() { cb(r); });
  };
  if (!hubSupportsSavesV4() || !available()) {
    finish(RK::Failed, tr("The server does not support uploading save files."));
    return;
  }
  if (const QString why = uploadBlockReason(gameId); !why.isEmpty()) {
    finish(RK::Blocked, why);
    return;
  }
  if (liveApplyAvailable(gameId, slot)) {
    uploadSaveFileLive(gameId, slot, filePath, expectedRevision, cb);
    return;
  }
  if (const QString why = pendingReason(gameId, slot); !why.isEmpty()) {
    finish(RK::Blocked, why);
    return;
  }
  const QFileInfo info(filePath);
  if (!info.isFile()) {
    finish(RK::Failed, tr("The file could not be read."));
    return;
  }
  if (info.size() == 0) {
    finish(RK::Failed, tr("The file is empty. Nothing was uploaded."));
    return;
  }
  if (info.size() > kMaxSaveBytes) {
    finish(RK::Failed, tr("The file is larger than %1 MiB. Nothing was uploaded.").arg(kMaxSaveBytes / (1024 * 1024)));
    return;
  }
  QFile in(filePath);
  if (!in.open(QIODevice::ReadOnly)) {
    finish(RK::Failed, tr("The file could not be read."));
    return;
  }
  const QByteArray data = in.readAll();
  in.close();
  if (data.isEmpty()) {
    finish(RK::Failed, tr("The file is empty. Nothing was uploaded."));
    return;
  }
  const QString hubId = conn_->hubId();
  api_.uploadFile(gameId, slot, data, expectedRevision, [=, this](const SaveApiResult& r) {
    using K = SaveApiResult::Kind;
    if (!sameHub(hubId)) {
      cb({RK::Failed, tr("The server connection changed."), 0, false});
      return;
    }
    switch (r.kind) {
      case K::Ok:
        break;
      case K::Stale:
        cb({RK::Stale, tr("The save changed on the server in the meantime. Nothing was uploaded."), 0, false});
        return;
      case K::NotFound:
        cb({RK::NotFound, tr("The game or slot was not found on the server."), 0, false});
        return;
      case K::Offline:
        cb({RK::Offline, tr("Server not reachable. Nothing was changed."), 0, false});
        return;
      default:
        if (r.errorCode == QLatin1String("session_running")) {
          cb({RK::Blocked, tr("A Session of this game is running. Upload the save file after it ends. Nothing was uploaded."), 0, false});
        } else if (r.errorCode == QLatin1String("payload_too_large")) {
          cb({RK::Failed, tr("The file is too large for the server (limit %1 MiB). Nothing was uploaded.").arg(kMaxSaveBytes / (1024 * 1024)), 0, false});
        } else {
          cb({RK::Failed, tr("The file could not be uploaded (%1). Nothing was changed.").arg(r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode),
              0, false});
        }
        return;
    }
    const int newRevision = r.slot->current.revision;
    // The Hub took the file. Make it the local save of the slot, too (backup first); nothing unsynced exists here.
    const QString dir = slotDirFor(gameId, slot);
    if (dir.isEmpty() || (SaveStore::findSaveFile(dir, QString()).isEmpty() && localFileName.isEmpty())) {
      cb({RK::Ok, QString(), newRevision, false});  // no local file: the next start downloads the checkpoint
      return;
    }
    if (!pendingReason(gameId, slot).isEmpty()) {
      cb({RK::Ok, tr("Uploaded to the server; the local save will be updated at the next start."), newRevision, false});
      return;
    }
    QDir().mkpath(dir);
    QString file = SaveStore::findSaveFile(dir, localFileName);
    if (file.isEmpty()) {
      file = QDir(dir).filePath(localFileName);
    }
    if (QFileInfo(file).isFile() && SaveStore::backupFile(file, QStringLiteral("upload")).isEmpty()) {
      cb({RK::Ok, tr("Uploaded to the server; the local save could not be backed up and stays unchanged until the next start."),
          newRevision, false});
      return;
    }
    if (!SaveStore::atomicWrite(file, data)) {
      cb({RK::Ok, tr("Uploaded to the server; the local save could not be written."), newRevision, false});
      return;
    }
    SyncState st = SaveStore::loadState(dir);
    st.slot = slot;
    st.baseRevision = newRevision;
    st.lastSyncedSha256 = SaveStore::sha256Of(data);
    st.pending = false;
    st.conflictId.clear();
    st.lastError.clear();
    SaveStore::saveState(dir, st);
    if (slot == slotFor(gameId)) {
      setKind(gameId, st, true);
    }
    cb({RK::Ok, QString(), newRevision, true});
  });
}

// ---------------------------------------------------------------- Live save and conflicts outside the play flow

QString SaveSync::liveBlockReason(const QString& gameId, const QString& slot) const {
  if (!liveApplyAvailable(gameId, slot)) {
    return tr("This game is running on this PC. Quit it to restore a version.");
  }
  if (!a_.st.conflictId.isEmpty()) {
    return tr("This slot has an open save conflict. Resolve it first.");
  }
  return {};
}

void SaveSync::livePrelude(const QString& gameId, const QString& slot, int expectedRevision,
                           std::function<void(bool, int, const QString&)> next) {
  if (!liveApplyAvailable(gameId, slot) || liveOp_) {
    QTimer::singleShot(0, this, [next]() { next(false, 0, tr("This game is running on this PC. Quit it to restore a version.")); });
    return;
  }
  if (const QString why = liveBlockReason(gameId, slot); !why.isEmpty()) {
    QTimer::singleShot(0, this, [next, why]() { next(false, 0, why); });
    return;
  }
  liveOp_ = true;
  if (live_.flush) {
    live_.flush();
  }
  const int before = a_.st.baseRevision;
  auto conn = std::make_shared<QMetaObject::Connection>();
  *conn = connect(this, &SaveSync::finalSyncFinished, this, [this, conn, gameId, before, expectedRevision, next](const QString& id, bool ok) {
    if (!id.isEmpty() && id != gameId) {
      return;
    }
    disconnect(*conn);
    if (id.isEmpty() || !ok) {
      liveOp_ = false;
      next(false, 0, tr("The current save could not be uploaded first. Nothing was changed."));
      return;
    }
    next(true, a_.st.baseRevision != before ? a_.st.baseRevision : expectedRevision, QString());
  });
  finalSync(false);  // existing final-sync path: a changed save becomes a Hub version first, so nothing is lost
}

// Backup of the local save, then the running core takes `data`; the sync state follows. Never leaves the file and the core apart.
SaveSync::RestoreResult SaveSync::applyLiveContent(const QByteArray& data, int newRevision, const QString& backupTag) {
  using RK = RestoreResult::Outcome;
  RestoreResult out;
  out.kind = RK::Ok;
  out.revision = newRevision;
  const QString quitHint = tr("Quit the game and start it again to use it.");
  if (!liveAccepts(data.size(), &data)) {
    out.message = tr("Done on the server, but the running game cannot load this save. %1").arg(quitHint);
    return out;
  }
  if (QFileInfo(a_.file).isFile()) {
    const QString bak = backupHook_ ? backupHook_(a_.file, backupTag) : SaveStore::backupFile(a_.file, backupTag);
    if (bak.isEmpty()) {
      out.message = tr("Done on the server; the local save could not be backed up, so the running game was not changed. %1").arg(quitHint);
      return out;
    }
  }
  if (!liveApplyData(data)) {
    out.message = tr("Done on the server, but the running game could not load the save. %1").arg(quitHint);
    return out;
  }
  a_.st.baseRevision = newRevision;
  a_.st.lastSyncedSha256 = SaveStore::sha256Of(data);
  a_.st.pending = false;
  a_.st.conflictId.clear();
  a_.st.lastError.clear();
  persist();
  resyncActiveFile();
  out.localUpdated = true;
  return out;
}

void SaveSync::restoreVersionLive(const QString& gameId, const QString& slot, int version, int expectedRevision, RestoreCallback cb) {
  using RK = RestoreResult::Outcome;
  const auto done = [this, cb](const RestoreResult& r) {
    liveOp_ = false;
    cb(r);
  };
  if (const QString why = liveBlockReason(gameId, slot); !why.isEmpty()) {
    QTimer::singleShot(0, this, [cb, why]() { cb({RK::Blocked, why, 0, false}); });
    return;
  }
  const QString hubId = conn_->hubId();
  api_.listHistory(gameId, slot, [=, this](const SaveApiResult& hr) {
    qint64 size = -1;
    for (const SaveHistoryVersion& v : hr.history) {
      if (v.version == version) {
        size = v.size;
      }
    }
    if (hr.ok() && size < 0) {
      cb({RK::NotFound, tr("That version no longer exists on the server."), 0, false});
      return;
    }
    if (hr.ok() && !liveAccepts(size, nullptr)) {
      cb({RK::Blocked, tr("The running game cannot load this version. Quit the game to restore it."), 0, false});
      return;
    }
    livePrelude(gameId, slot, expectedRevision, [=, this](bool ok, int revision, const QString& message) {
      if (!ok) {
        cb({RK::Blocked, message, 0, false});
        return;
      }
      api_.restore(gameId, slot, version, revision, [=, this](const SaveApiResult& r) {
        using K = SaveApiResult::Kind;
        if (!sameHub(hubId)) {
          done({RK::Failed, tr("The server connection changed."), 0, false});
          return;
        }
        switch (r.kind) {
          case K::Ok:
            break;
          case K::Stale:
            done({RK::Stale, tr("The save changed on the server in the meantime. Nothing was restored."), 0, false});
            return;
          case K::NotFound:
            done({RK::NotFound, tr("That version no longer exists on the server."), 0, false});
            return;
          case K::Offline:
            done({RK::Offline, tr("Server not reachable. Nothing was changed."), 0, false});
            return;
          default:
            done({RK::Failed, tr("The version could not be restored (%1). Nothing was changed.").arg(r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode),
                  0, false});
            return;
        }
        const int newRevision = r.slot->current.revision;
        api_.getContent(gameId, slot, [=, this](const SaveApiResult& cr) {
          if (!sameHub(hubId) || !isRunning(gameId, slot)) {
            done({RK::Ok, QString(), newRevision, false});
            return;
          }
          if (!cr.ok()) {
            done({RK::Ok, tr("Restored on the server; the running game keeps its save until the next start."), newRevision, false});
            return;
          }
          done(applyLiveContent(cr.content, cr.contentRevision, QStringLiteral("restore")));
        });
      });
    });
  });
}

void SaveSync::uploadSaveFileLive(const QString& gameId, const QString& slot, const QString& filePath, int expectedRevision, RestoreCallback cb) {
  using RK = RestoreResult::Outcome;
  const auto fail = [this, cb](RK kind, const QString& message) {
    RestoreResult r;
    r.kind = kind;
    r.message = message;
    QTimer::singleShot(0, this, [cb, r]() { cb(r); });
  };
  if (const QString why = liveBlockReason(gameId, slot); !why.isEmpty()) {
    fail(RK::Blocked, why);
    return;
  }
  const QFileInfo info(filePath);
  if (!info.isFile()) {
    fail(RK::Failed, tr("The file could not be read."));
    return;
  }
  if (info.size() == 0) {
    fail(RK::Failed, tr("The file is empty. Nothing was uploaded."));
    return;
  }
  if (info.size() > kMaxSaveBytes) {
    fail(RK::Failed, tr("The file is larger than %1 MiB. Nothing was uploaded.").arg(kMaxSaveBytes / (1024 * 1024)));
    return;
  }
  QFile in(filePath);
  if (!in.open(QIODevice::ReadOnly)) {
    fail(RK::Failed, tr("The file could not be read."));
    return;
  }
  const QByteArray data = in.readAll();
  in.close();
  if (!liveAccepts(data.size(), &data)) {
    fail(RK::Blocked, tr("The running game cannot load a save of this size. Quit the game to upload it."));
    return;
  }
  const QString hubId = conn_->hubId();
  const auto done = [this, cb](const RestoreResult& r) {
    liveOp_ = false;
    cb(r);
  };
  livePrelude(gameId, slot, expectedRevision, [=, this](bool ok, int revision, const QString& message) {
    if (!ok) {
      cb({RK::Blocked, message, 0, false});
      return;
    }
    api_.uploadFile(gameId, slot, data, revision, [=, this](const SaveApiResult& r) {
      using K = SaveApiResult::Kind;
      if (!sameHub(hubId)) {
        done({RK::Failed, tr("The server connection changed."), 0, false});
        return;
      }
      switch (r.kind) {
        case K::Ok:
          break;
        case K::Stale:
          done({RK::Stale, tr("The save changed on the server in the meantime. Nothing was uploaded."), 0, false});
          return;
        case K::NotFound:
          done({RK::NotFound, tr("The game or slot was not found on the server."), 0, false});
          return;
        case K::Offline:
          done({RK::Offline, tr("Server not reachable. Nothing was changed."), 0, false});
          return;
        default:
          if (r.errorCode == QLatin1String("session_running")) {
            done({RK::Blocked, tr("A Session of this game is running. Upload the save file after it ends. Nothing was uploaded."), 0, false});
            return;
          }
          done({RK::Failed, tr("The file could not be uploaded (%1). Nothing was changed.").arg(r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode),
                0, false});
          return;
      }
      done(applyLiveContent(data, r.slot->current.revision, QStringLiteral("upload")));
    });
  });
}

void SaveSync::loadSlotConflict(const QString& gameId, const QString& slot, const QString& localFileName, ConflictCallback cb) {
  using O = SlotConflict::Outcome;
  if (!available()) {
    QTimer::singleShot(0, this, [cb]() {
      SlotConflict c;
      c.kind = O::Offline;
      c.message = tr("Server not reachable. The conflict cannot be shown right now.");
      cb(c);
    });
    return;
  }
  const QString hubId = conn_->hubId();
  api_.getSlot(gameId, slot, [=, this](const SaveApiResult& r) {
    SlotConflict out;
    if (!sameHub(hubId) || r.kind == SaveApiResult::Kind::Offline) {
      out.kind = O::Offline;
      out.message = tr("Server not reachable. The conflict cannot be shown right now.");
      cb(out);
      return;
    }
    if (!r.ok()) {
      out.kind = r.kind == SaveApiResult::Kind::NotFound ? O::None : O::Failed;
      out.message = tr("The conflict could not be loaded (%1).").arg(r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode);
      cb(out);
      return;
    }
    const QString dir = slotDirFor(gameId, slot);
    const QString preferred = dir.isEmpty() ? QString() : SaveStore::loadState(dir).conflictId;
    const std::optional<SaveConflictInfo> own = ownConflict(*r.slot, preferred);
    if (!own) {
      out.kind = O::None;
      if (!dir.isEmpty() && !isRunning(gameId, slot) && !dialogOpen_) {
        // Resolved elsewhere (e.g. by an admin through the admin API): drop the local conflict marker; the next start reconciles.
        SyncState st = SaveStore::loadState(dir);
        if (!st.conflictId.isEmpty()) {
          st.conflictId.clear();
          st.slot = slot;
          SaveStore::saveState(dir, st);
          if (slot == slotFor(gameId)) {
            refreshKinds({gameId});
          }
        }
      }
      cb(out);
      return;
    }
    QString file = isRunning(gameId, slot) ? a_.file : (dir.isEmpty() ? QString() : SaveStore::findSaveFile(dir, localFileName));
    out.kind = O::Found;
    out.view = makeConflictView(*own, gameId, file);
    out.localExists = !file.isEmpty() && QFileInfo(file).isFile();
    out.localSize = out.localExists ? QFileInfo(file).size() : 0;
    out.hubSize = r.slot->current.size;
    cb(out);
  });
}

void SaveSync::resolveSlotConflict(const QString& gameId, const QString& slot, Resolution res, const QString& localFileName, RestoreCallback cb) {
  using RK = RestoreResult::Outcome;
  const auto fail = [this, cb](RK kind, const QString& message) {
    RestoreResult r;
    r.kind = kind;
    r.message = message;
    QTimer::singleShot(0, this, [cb, r]() { cb(r); });
  };
  if (res == Resolution::DecideLater) {
    fail(RK::Ok, QString());
    return;
  }
  if (!available()) {
    fail(RK::Offline, tr("Server not reachable. Nothing was changed."));
    return;
  }
  if (resolveCb_ || dialogOpen_ || liveOp_) {
    fail(RK::Blocked, tr("Another save action is in progress."));
    return;
  }
  const bool live = isRunning(gameId, slot);
  if (live) {
    if (!liveApplyAvailable(gameId, slot) && res == Resolution::UseHub) {
      fail(RK::Blocked, tr("This game is running on this PC. Quit it to use the server save."));
      return;
    }
    if (live_.flush) {
      live_.flush();  // the local side is the game's current save
    }
  } else {
    // The running session (any slot of the same game too) and a game in its start flow own `a_`: never replace it.
    if (session_) {
      fail(RK::Blocked, a_.gameId != gameId ? tr("Another game is running. Quit it first.")
                                            : tr("This game is running with another slot. Quit it first."));
      return;
    }
    if (starting_) {
      fail(RK::Blocked, tr("A game is starting. Try again in a moment."));
      return;
    }
    const QString dir = SaveStore::slotDir(*profiles_, conn_->hubId(), conn_->hubUserId(), gameId, slot);
    if (dir.isEmpty()) {
      fail(RK::Failed, tr("The save directory for this game is unknown."));
      return;
    }
    ++gen_;
    pollTimer_.stop();
    session_ = false;
    a_ = Active{};
    a_.gameId = gameId;
    a_.slot = slot;
    a_.hubId = conn_->hubId();
    a_.userId = conn_->hubUserId();
    a_.dir = dir;
    QDir().mkpath(dir);
    a_.expectedName = localFileName;
    a_.file = SaveStore::findSaveFile(dir, localFileName);
    if (a_.file.isEmpty()) {
      a_.file = QDir(dir).filePath(localFileName);
    }
    a_.st = SaveStore::loadState(dir);
    a_.st.slot = slot;
  }
  const QString hubId = conn_->hubId();
  const quint64 resolveGen = gen_;
  api_.getSlot(gameId, slot, [=, this](const SaveApiResult& r) {
    if (!live && resolveGen != gen_) {  // another game started or the session changed meanwhile: `a_` is not ours any more
      RestoreResult out;
      out.kind = RK::Blocked;
      out.message = tr("The game changed in the meantime. Nothing was changed.");
      cb(out);
      return;
    }
    const auto abort = [&](RK kind, const QString& message) {
      if (!live) {
        a_ = Active{};
      }
      RestoreResult out;
      out.kind = kind;
      out.message = message;
      cb(out);
    };
    if (!sameHub(hubId) || r.kind == SaveApiResult::Kind::Offline) {
      abort(RK::Offline, tr("Server not reachable. Nothing was changed."));
      return;
    }
    if (!r.ok()) {
      abort(RK::Failed, tr("The conflict could not be loaded (%1). Nothing was changed.").arg(r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode));
      return;
    }
    const std::optional<SaveConflictInfo> own = ownConflict(*r.slot, a_.st.conflictId);
    if (!own) {
      // Resolved elsewhere (e.g. by an admin through the admin API): drop the local marker so the slot is usable again.
      if (!a_.st.conflictId.isEmpty()) {
        a_.st.conflictId.clear();
        persist();
      }
      if (live) {
        a_.pausedByConflict = false;
      }
      abort(RK::NotFound, tr("The conflict was already resolved elsewhere."));
      return;
    }
    a_.conflict = *own;
    a_.st.conflictId = own->id;
    a_.st.pending = true;
    dialogOpen_ = true;
    resolveCb_ = cb;
    resolveLive_ = live;
    resolveConflict(res);
  });
}

namespace {
SaveSnapshotResult snapshotResultOf(const SaveApiResult& r) {
  using K = SaveApiResult::Kind;
  using SK = SaveSnapshotResult::Outcome;
  SaveSnapshotResult out;
  switch (r.kind) {
    case K::Ok:
      out.kind = SK::Ok;
      out.version = *r.snapshot;
      break;
    case K::NotFound:
      out.kind = SK::NotFound;
      out.message = QObject::tr("There is no save on the server yet, so there is nothing to snapshot.");
      break;
    case K::Offline:
      out.kind = SK::Offline;
      out.message = QObject::tr("Server not reachable. No snapshot was created.");
      break;
    default:
      out.kind = SK::Failed;
      out.message = QObject::tr("The snapshot could not be created (%1).").arg(r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode);
      break;
  }
  return out;
}
}  // namespace

void SaveSync::createSnapshot(const QString& gameId, const QString& slot, const QString& label, SnapshotCallback cb) {
  if (!hubSupportsSavesV2() || !available()) {
    QTimer::singleShot(0, this, [cb]() {
      SnapshotResult r;
      r.message = tr("The server does not support snapshots.");
      cb(r);
    });
    return;
  }
  api_.createSnapshot(gameId, slot, label, [cb](const SaveApiResult& r) { cb(snapshotResultOf(r)); });
}

void SaveSync::snapshotActive(const QString& label, SnapshotCallback cb) {
  if (!session_ || a_.gameId.isEmpty() || !hubSupportsSavesV2() || !available()) {
    QTimer::singleShot(0, this, [cb]() {
      SnapshotResult r;
      r.kind = SnapshotResult::Outcome::Blocked;
      r.message = tr("No snapshot is possible right now.");
      cb(r);
    });
    return;
  }
  const QString gameId = a_.gameId;
  const QString slot = a_.slot;
  auto conn = std::make_shared<QMetaObject::Connection>();
  *conn = connect(this, &SaveSync::finalSyncFinished, this, [this, conn, gameId, slot, label, cb](const QString& id, bool ok) {
    if (!id.isEmpty() && id != gameId) {
      return;
    }
    disconnect(*conn);
    if (id.isEmpty() || !ok) {
      SnapshotResult r;
      r.kind = SnapshotResult::Outcome::Failed;
      r.message = tr("The current save could not be uploaded, so no snapshot was created.");
      cb(r);
      return;
    }
    createSnapshot(gameId, slot, label, cb);
  });
  if (live_.flush) {
    live_.flush();  // the core writes its save RAM to the file first, so the snapshot includes the latest progress
  }
  finalSync(false);  // existing final-sync path: uploads a changed save immediately
}

// ---------------------------------------------------------------- Rename and delete slots (saves_v5)

namespace {
QString whoText(const SaveUpdate& u) {
  if (!u.deviceName.isEmpty()) {
    return u.deviceName;
  }
  return u.deviceId == QLatin1String("00000000-0000-0000-0000-000000000000") ? QObject::tr("the server admin")
                                                                              : QObject::tr("another device");
}

// Moves a directory out of the way (never overwrites or deletes it): <dir>.replaced-<timestamp>.
bool moveAside(const QString& dir) {
  const QString aside = dir + QStringLiteral(".replaced-") + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz"));
  return QDir().rename(dir, aside);
}

bool moveDir(const QString& src, const QString& dst) {
  if (!QFileInfo::exists(src)) {
    return true;
  }
  if (QFileInfo::exists(dst) && !moveAside(dst)) {
    return false;
  }
  QDir().mkpath(QFileInfo(dst).absolutePath());
  return QDir().rename(src, dst);
}
}  // namespace

QString SaveSync::moveLocalSlot(const QString& gameId, const QString& from, const QString& to) {
  const QString src = slotDirFor(gameId, from);
  const QString dst = slotDirFor(gameId, to);
  if (src.isEmpty() || dst.isEmpty() || src == dst) {
    return {};
  }
  QString error;
  if (!moveDir(src, dst)) {
    error = tr("The local folder of slot \"%1\" could not be moved to \"%2\". It stays under the old name.").arg(from, to);
  } else if (QFileInfo::exists(SaveStore::stateFilePath(dst))) {
    SyncState st = SaveStore::loadState(dst);
    if (st.slot != to) {
      st.slot = to;
      SaveStore::saveState(dst, st);
    }
  }
  if (error.isEmpty()) {
    // Cores with a short save directory (Windows MAX_PATH) key it by slot name: move that one, too.
    const QString shortSrc = SaveStore::shortCoreDir(*profiles_, conn_->hubId(), conn_->hubUserId(), gameId, from);
    const QString shortDst = SaveStore::shortCoreDir(*profiles_, conn_->hubId(), conn_->hubUserId(), gameId, to);
    if (!shortSrc.isEmpty() && !shortDst.isEmpty() && !moveDir(shortSrc, shortDst)) {
      error = tr("The core files of slot \"%1\" could not be moved to \"%2\".").arg(from, to);
    }
  }
  if (slotFor(gameId) == from && error.isEmpty()) {
    setSlot(gameId, to);
  } else {
    refreshKinds({gameId});
  }
  return error;
}

QString SaveSync::backupLocalSlot(const QString& gameId, const QString& slot) {
  const QString dir = slotDirFor(gameId, slot);
  if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
    return {};
  }
  const QString file = SaveStore::findSaveFile(dir, QString());
  if (file.isEmpty()) {
    return {};
  }
  const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
  if (slot == QLatin1String("default")) {
    return SaveStore::backupFile(file, QStringLiteral("deleted")).isEmpty() ? tr("The backup of the local save could not be written.") : QString();
  }
  const QString target = QDir(SaveStore::gameDir(*profiles_, conn_->hubId(), conn_->hubUserId(), gameId))
                             .filePath(QStringLiteral("%1.%2-deleted-%3.bak").arg(QFileInfo(file).fileName(), slot, stamp));
  return QFile::copy(file, target) ? QString() : tr("The backup of the local save could not be written.");
}

void SaveSync::removeLocalSlot(const QString& gameId, const QString& slot) {
  if (slot == QLatin1String("default")) {
    // The main slot directory is the game directory and also holds "slots/": remove only its own files (save file, sync.json), keep
    // the other slots and *.bak backups. Without sync.json the next start begins fresh from the (empty) Hub slot.
    const QString gameDir = slotDirFor(gameId, slot);
    if (!gameDir.isEmpty() && QFileInfo(gameDir).isDir() && !QFileInfo(gameDir).isSymLink()) {
      for (const QFileInfo& fi : QDir(gameDir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (fi.fileName() == QLatin1String("slots") || fi.fileName().endsWith(QLatin1String(".bak"))) {
          continue;
        }
        if (fi.isDir() && !fi.isSymLink()) {
          QDir(fi.absoluteFilePath()).removeRecursively();
        } else {
          QFile::remove(fi.absoluteFilePath());
        }
      }
    }
    const QString shortDir = SaveStore::shortCoreDir(*profiles_, conn_->hubId(), conn_->hubUserId(), gameId, slot);
    if (!shortDir.isEmpty() && QFileInfo(shortDir).isDir() && !QFileInfo(shortDir).isSymLink()) {
      QDir(shortDir).removeRecursively();
    }
    refreshKinds({gameId});
    return;
  }
  for (const QString& dir : {slotDirFor(gameId, slot), SaveStore::shortCoreDir(*profiles_, conn_->hubId(), conn_->hubUserId(), gameId, slot)}) {
    if (!dir.isEmpty() && QFileInfo(dir).isDir() && !QFileInfo(dir).isSymLink()) {
      QDir(dir).removeRecursively();
    }
  }
  if (slotFor(gameId) == slot) {
    setSlot(gameId, QStringLiteral("default"));
  } else {
    refreshKinds({gameId});
  }
}

QString SaveSync::uploadBlockReason(const QString& gameId) const {
  if (isRunning(gameId) && shared_ && shared_(gameId)) {
    return tr("This game is shared as a Session. Upload the save file after the Session ends.");
  }
  return {};
}

QString SaveSync::slotChangeBlockReason(const QString& gameId, const QString& slot, bool deleting) const {
  if (slot == QLatin1String("default") && !(deleting && hubSupportsSavesV6())) {  // saves_v6: the main slot can be deleted (never renamed)
    return deleting ? tr("This server cannot delete the main slot.") : tr("The main slot cannot be renamed.");
  }
  if (isRunning(gameId)) {
    return tr("This game is running on this PC. Quit it first.");
  }
  return {};
}

void SaveSync::renameSlot(const QString& gameId, const QString& slot, const QString& newName, SlotChangeCallback cb) {
  using SO = SlotChangeResult::Outcome;
  const auto finish = [this, cb](SO kind, const QString& message) {
    QTimer::singleShot(0, this, [cb, kind, message]() { cb({kind, message}); });
  };
  if (!hubSupportsSavesV5() || !available()) {
    finish(SO::Failed, tr("The server does not support renaming slots."));
    return;
  }
  if (const QString why = slotChangeBlockReason(gameId, slot); !why.isEmpty()) {
    finish(slot == QLatin1String("default") ? SO::Protected : SO::Blocked, why);
    return;
  }
  if (!SaveStore::isValidSlotName(newName)) {
    finish(SO::Failed, tr("Use lowercase letters, digits, - and _ only (at most 32 characters)."));
    return;
  }
  if (newName == slot) {
    finish(SO::Failed, tr("The slot already has this name."));
    return;
  }
  const QString hubId = conn_->hubId();
  api_.renameSlot(gameId, slot, newName, [=, this](const SaveApiResult& r) {
    using K = SaveApiResult::Kind;
    if (!sameHub(hubId)) {
      cb({SO::Failed, tr("The server connection changed. Nothing was renamed.")});
      return;
    }
    if (r.ok()) {
      if (isRunning(gameId)) {  // the game started while the request was in flight: the files stay where the core uses them
        cb({SO::Ok, tr("Renamed on the server. The local copy keeps the old name until the game is closed.")});
        return;
      }
      cb({SO::Ok, moveLocalSlot(gameId, slot, newName)});
      return;
    }
    if (r.kind == K::NotFound) {
      const QString dir = slotDirFor(gameId, slot);
      if (!dir.isEmpty() && QFileInfo::exists(dir)) {  // a slot that was never uploaded exists only on this device
        cb({SO::Ok, moveLocalSlot(gameId, slot, newName)});
        return;
      }
      cb({SO::NotFound, tr("The slot was not found on the server. The list was refreshed.")});
      return;
    }
    if (r.kind == K::Offline) {
      cb({SO::Offline, tr("Server not reachable. Nothing was renamed.")});
      return;
    }
    if (r.errorCode == QLatin1String("save_slot_exists")) {
      cb({SO::Exists, tr("A slot named \"%1\" already exists.").arg(newName)});
    } else if (r.errorCode == QLatin1String("save_slot_protected")) {
      cb({SO::Protected, tr("The main slot cannot be renamed or deleted.")});
    } else if (r.errorCode == QLatin1String("session_running")) {
      cb({SO::SessionRunning, tr("A Session of this game is running. Rename the slot after it ends. Nothing was changed.")});
    } else {
      cb({SO::Failed, tr("The server refused the new name (%1). Nothing was changed.").arg(r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode)});
    }
  });
}

void SaveSync::deleteSlot(const QString& gameId, const QString& slot, int confirmRevision, SlotChangeCallback cb) {
  using SO = SlotChangeResult::Outcome;
  const auto finish = [this, cb](SO kind, const QString& message) {
    QTimer::singleShot(0, this, [cb, kind, message]() { cb({kind, message}); });
  };
  if (!hubSupportsSavesV5() || !available()) {
    finish(SO::Failed, tr("The server does not support deleting slots."));
    return;
  }
  if (const QString why = slotChangeBlockReason(gameId, slot, true); !why.isEmpty()) {
    finish(slot == QLatin1String("default") ? SO::Protected : SO::Blocked, why);
    return;
  }
  // Unsynced progress on this device is never deleted along with the slot.
  if (const QString why = pendingReason(gameId, slot); !why.isEmpty()) {
    finish(SO::Blocked, why + QLatin1Char(' ') + tr("Nothing was deleted."));
    return;
  }
  const QString hubId = conn_->hubId();
  const auto onResult = [=, this](const SaveApiResult& r) {
    using K = SaveApiResult::Kind;
    if (!sameHub(hubId)) {
      cb({SO::Failed, tr("The server connection changed. Nothing was deleted.")});
      return;
    }
    if (isRunning(gameId, slot)) {  // started meanwhile: keep the files the core uses
      cb({SO::Ok, tr("Deleted on the server. The local copy was kept because the game is running.")});
      return;
    }
    if (r.ok()) {
      removeLocalSlot(gameId, slot);
      cb({SO::Ok, QString()});
    } else if (r.kind == K::NotFound) {
      removeLocalSlot(gameId, slot);
      cb({SO::Ok, tr("The slot was already gone on the server. The local copy was removed.")});
    } else if (r.kind == K::Stale) {
      cb({SO::Stale, tr("The save changed on the server in the meantime. Nothing was deleted. The list was refreshed; check it and decide again.")});
    } else if (r.kind == K::Offline) {
      cb({SO::Offline, tr("Server not reachable. Nothing was deleted.")});
    } else if (r.errorCode == QLatin1String("session_running")) {
      cb({SO::SessionRunning, tr("A Session of this game is running. Delete the slot after it ends. Nothing was deleted.")});
    } else if (r.errorCode == QLatin1String("conflict")) {
      cb({SO::Conflict, tr("This slot has an open save conflict. Resolve it first. Nothing was deleted.")});
    } else if (r.errorCode == QLatin1String("save_slot_protected")) {
      cb({SO::Protected, tr("This server cannot delete the main slot.")});
    } else {
      cb({SO::Failed, tr("The server refused to delete the slot (%1). Nothing was deleted.").arg(r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode)});
    }
  };
  if (confirmRevision > 0) {
    api_.deleteSlot(gameId, slot, confirmRevision, onResult);
    return;
  }
  // No revision seen: only a slot that does not exist on the Hub (created here, never uploaded) is removed, device only.
  api_.getSlot(gameId, slot, [=, this](const SaveApiResult& r) {
    using K = SaveApiResult::Kind;
    if (r.kind == K::NotFound && sameHub(hubId)) {
      onResult(r);
    } else if (r.ok()) {
      cb({SO::Stale, tr("The save changed on the server in the meantime. Nothing was deleted. The list was refreshed; check it and decide again.")});
    } else {
      onResult(r);
    }
  });
}

void SaveSync::handleSlotRenamedElsewhere(const SaveUpdate& u) {
  const QString from = u.previousSlot;
  const QString to = u.slot;
  if (from.isEmpty() || from == QLatin1String("default") || !SaveStore::isValidSlotName(from) || !SaveStore::isValidSlotName(to) || from == to) {
    emit slotChangedElsewhere(u, QString(), false);
    return;
  }
  QString msg = tr("Slot \"%1\" was renamed to \"%2\" on %3.").arg(from, to, whoText(u));
  bool warn = false;
  if (isRunning(u.gameId, from)) {
    msg += QLatin1Char(' ') + tr("This game is running with the old name. This device keeps its local copy until you quit.");
    warn = true;
  } else if (QFileInfo::exists(slotDirFor(u.gameId, from))) {
    if (!pendingReason(u.gameId, from).isEmpty()) {
      msg += QLatin1Char(' ') + tr("This device has unsynced changes in \"%1\". They were kept there and not uploaded.").arg(from);
      warn = true;
    } else if (const QString err = moveLocalSlot(u.gameId, from, to); !err.isEmpty()) {
      msg += QLatin1Char(' ') + err;
      warn = true;
    }
  } else if (slotFor(u.gameId) == from) {
    setSlot(u.gameId, to);  // nothing stored here: only the selection follows
  }
  emit slotChangedElsewhere(u, msg, warn);
}

void SaveSync::handleSlotDeletedElsewhere(const SaveUpdate& u) {
  const QString slot = u.slot;
  if (!SaveStore::isValidSlotName(slot)) {
    emit slotChangedElsewhere(u, QString(), false);
    return;
  }
  const bool isMain = slot == QLatin1String("default");
  QString msg = isMain ? tr("The main slot was deleted on %1.").arg(whoText(u)) : tr("Slot \"%1\" was deleted on %2.").arg(slot, whoText(u));
  bool warn = false;
  if (isRunning(u.gameId, slot)) {
    msg += QLatin1Char(' ') + tr("This game is running with it. This device keeps its local copy; the next upload creates the slot again.");
    warn = true;
  } else if (QFileInfo::exists(slotDirFor(u.gameId, slot))) {
    if (!pendingReason(u.gameId, slot).isEmpty()) {
      msg += QLatin1Char(' ') + tr("This device has unsynced changes in it, so the local copy was kept. Selecting the slot uploads it again.");
      warn = true;
    } else if (const QString err = backupLocalSlot(u.gameId, slot); !err.isEmpty()) {
      msg += QLatin1Char(' ') + err + QLatin1Char(' ') + tr("The local copy was kept.");
      warn = true;
    } else {
      removeLocalSlot(u.gameId, slot);
      msg += QLatin1Char(' ') + tr("A backup of the local copy was kept (*.bak).");
    }
  } else if (!isMain && slotFor(u.gameId) == slot) {
    setSlot(u.gameId, QStringLiteral("default"));
  }
  emit slotChangedElsewhere(u, msg, warn);
}

void SaveSync::handleSaveUpdate(const SaveUpdate& u) {
  // The server reports the nil UUID with device_name "server admin": never "this device".
  const bool nilDevice = u.deviceId == QLatin1String("00000000-0000-0000-0000-000000000000");
  if (!nilDevice && !conn_->deviceId().isEmpty() && u.deviceId == conn_->deviceId()) {
    return;  // our own change
  }
  if (u.reason == QLatin1String("renamed")) {
    handleSlotRenamedElsewhere(u);
    return;
  }
  if (u.reason == QLatin1String("deleted")) {
    handleSlotDeletedElsewhere(u);
    return;
  }
  emit saveChangedElsewhere(u, isRunning(u.gameId, u.slot));
}

}  // namespace uzume
