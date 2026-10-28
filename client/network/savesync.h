#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

#include "playersettings.h"
#include "profilestore.h"
#include "saveapi.h"
#include "savestore.h"

namespace uzume {

struct SaveRestoreResult {
  enum class Outcome { Ok, Blocked, Stale, NotFound, Offline, Failed };
  Outcome kind = Outcome::Failed;
  QString message;  // user-facing; on Ok only a warning (e.g. the local file was not updated), otherwise empty
  int revision = 0;
  bool localUpdated = false;  // the local save file of the slot was replaced by the new checkpoint
  bool ok() const { return kind == Outcome::Ok; }
};
struct SaveSnapshotResult {
  enum class Outcome { Ok, Blocked, NotFound, Offline, Failed };
  Outcome kind = Outcome::Failed;
  QString message;
  SaveHistoryVersion version;
  bool ok() const { return kind == Outcome::Ok; }
};

// Result of renaming or deleting a slot (saves_v5). `message` is user-facing; on Ok only a warning (empty = none).
struct SaveSlotChangeResult {
  enum class Outcome { Ok, Blocked, Stale, NotFound, Exists, Protected, SessionRunning, Conflict, Offline, Failed };
  Outcome kind = Outcome::Failed;
  QString message;
  bool ok() const { return kind == Outcome::Ok; }
};

// Save sync state machine of the Player (docs: ADR 0005 / D4). One active game at a time.
// Flow: prepareStart() -> startReady() | startConflict() -> (resolveConflict() -> startReady()) ->
// beginSession() (auto checkpoints) -> finalSync() on pause/stop/exit.
// Local data only below hubs/<hub_id>/users/<user_id>/; uploads and retries only to the Hub of that hub_id.
class SaveSync : public QObject {
  Q_OBJECT
 public:
  enum class Kind { None, Synced, Pending, Conflict };
  Q_ENUM(Kind)
  enum class Resolution { UseHub, UseLocal, DecideLater };
  Q_ENUM(Resolution)

  struct Timing {
    int pollMs = 2000;           // file watcher
    int debounceMs = 12000;      // quiet time after the last change
    int minIntervalMs = 60000;   // minimum distance between two checkpoint uploads
    int retryBaseMs = 5000;      // backoff after a failed upload (doubles)
    int retryMaxMs = 300000;
    int finalTimeoutMs = 10000;  // wait at app exit
  };

  // Both sides of a conflict for the dialog.
  struct ConflictView {
    SaveConflictInfo conflict;
    QString gameId;
    QString localDeviceName;
    QDateTime localModified;
    QString localSha256;
    int localBaseRevision = 0;
  };

  SaveSync(HubConnection* connection, ProfileStore* profiles, QObject* parent = nullptr);

  using RestoreResult = SaveRestoreResult;
  using SnapshotResult = SaveSnapshotResult;
  using RestoreCallback = std::function<void(const RestoreResult&)>;
  using SnapshotCallback = std::function<void(const SnapshotResult&)>;
  using SlotChangeResult = SaveSlotChangeResult;
  using SlotChangeCallback = std::function<void(const SlotChangeResult&)>;

  // Slot choice per game and Hub profile (ADR 0012 D7), kept in the Player settings (without settings: in memory).
  void setSettings(PlayerSettings* settings) { settings_ = settings; }
  QString slotFor(const QString& gameId) const;
  bool setSlot(const QString& gameId, const QString& slot);  // false: invalid name; refreshes the library state of the game
  QString slotDirFor(const QString& gameId, const QString& slot) const;  // empty without Hub/user or for an invalid slot
  SaveApi* api() { return &api_; }
  // Cheats or memory writes were active in this play session (0.20): the uploads of the session carry X-Uzume-Cheats-Used.
  // Cleared by beginSession().
  void setCheatsUsed(bool used) { api_.setCheatsUsed(used); }
  bool cheatsUsed() { return api_.cheatsUsed(); }
  // Hub advertises saves_v2 (restore, snapshots, save_updated).
  bool hubSupportsSavesV2() const;
  // Hub advertises saves_v3 (deleting manual snapshots).
  bool hubSupportsSavesV3() const;
  // Hub advertises saves_v4 (upload of a local save file).
  bool hubSupportsSavesV4() const;
  // Hub advertises saves_v5 (rename and delete slots, upload lock during a Session).
  bool hubSupportsSavesV5() const;
  // Hub advertises saves_v6 (delete the main slot and any history version).
  bool hubSupportsSavesV6() const;

  // The game runs (or its Session just ended) with this slot on this Player.
  bool isRunning(const QString& gameId, const QString& slot = QString()) const {
    return session_ && a_.gameId == gameId && (slot.isEmpty() || a_.slot == slot);
  }
  QString activeSlot() const { return a_.slot; }
  // Pending upload, open conflict or unsynced local changes in this slot (restore is blocked then). Empty = none.
  QString pendingReason(const QString& gameId, const QString& slot) const;

  // Restore a history version: refused while the game runs here or while the slot has pending changes. Afterwards the
  // local save of the slot becomes the new checkpoint (download); `localFileName` names the file if none exists yet.
  void restoreVersion(const QString& gameId, const QString& slot, int version, int expectedRevision, const QString& localFileName,
                      RestoreCallback cb);
  // saves_v5: rename / delete a slot (never "default"; refused while the game runs on this Player). Rename moves the
  // local slot (save folder, core folder, sync.json, selection) after the Hub confirmed; unsynced local data moves along.
  // Delete is refused when the slot has unsynced local changes; the local copy is removed only after the Hub confirmed
  // (204), and a selected slot falls back to "default". `confirmRevision` is the checkpoint revision the user saw
  // (0 = the slot is not on the Hub: only then the local-only slot is removed).
  void renameSlot(const QString& gameId, const QString& slot, const QString& newName, SlotChangeCallback cb);
  void deleteSlot(const QString& gameId, const QString& slot, int confirmRevision, SlotChangeCallback cb);
  // Why rename / delete are off for this slot on this Player (default slot, game running); empty = allowed.
  QString slotChangeBlockReason(const QString& gameId, const QString& slot, bool deleting = false) const;
  // saves_v4: make a local file the Hub's current save of the slot (and the local save). Same block rules as restore;
  // for a running game the core loads the file live; refused while the game is shared as a Session (setSharedCheck);
  // the file must be non-empty and at most kMaxSaveBytes. `expectedRevision` 0 = no save on the Hub yet.
  void uploadSaveFile(const QString& gameId, const QString& slot, const QString& filePath, int expectedRevision,
                      const QString& localFileName, RestoreCallback cb);
  // ---- Live save: the running core loads a restored / uploaded / chosen save (saves view inside the game) ----
  // Hooks set by the Player (GameSession). All block the calling (GUI) thread until the emulation thread did it.
  struct LiveHooks {
    std::function<bool()> ready;                          // a core is loaded and running or paused (optional)
    std::function<void()> flush;                          // the core's battery save is written to the save file
    std::function<bool(qint64)> accepts;                  // the core's battery save memory has exactly this size
    std::function<bool(const QByteArray&)> apply;         // replaces the battery save by reloading the game from the new file
    // Stops the game, runs the writer (false = the game restarts with the old data), starts it again ("core_tree": the
    // save is a tree, no single file or size to hand over).
    std::function<bool(std::function<bool()>)> restartWith;
  };
  void setLiveHooks(LiveHooks hooks) { live_ = std::move(hooks); }
  // Set by the Player: this game is currently shared as a Session (upload of a save file is blocked then; the Hub
  // would refuse with session_running as well).
  void setSharedCheck(std::function<bool(const QString&)> check) { shared_ = std::move(check); }
  // Why a save file cannot be uploaded to this game now (shared as a Session); empty = allowed.
  QString uploadBlockReason(const QString& gameId) const;
  // This game runs here with this slot and the core can take a new save.
  bool liveApplyAvailable(const QString& gameId, const QString& slot) const {
    // "core_tree" (Azahar) needs restartWith (no size check); everything else apply + accepts.
    const bool hooks = a_.coreTree.isEmpty() ? (live_.apply && live_.accepts) : static_cast<bool>(live_.restartWith);
    return isRunning(gameId, slot) && hooks && (!live_.ready || live_.ready());
  }
  // Why restore / upload are off for a running game even though the core could take the save; empty = allowed.
  QString liveBlockReason(const QString& gameId, const QString& slot) const;

  // ---- Save conflict outside the play flow (Library / in-game saves view) ----
  struct SlotConflict {
    enum class Outcome { Found, None, Offline, Failed };
    Outcome kind = Outcome::Failed;
    QString message;
    ConflictView view;        // both sides: view.conflict (Hub), local device name / modification time
    bool localExists = false;
    qint64 localSize = 0;
    qint64 hubSize = 0;       // size of the Hub's current checkpoint
  };
  using ConflictCallback = std::function<void(const SlotConflict&)>;
  // Reads the open conflict of this device in the slot from the Hub (the game does not start).
  void loadSlotConflict(const QString& gameId, const QString& slot, const QString& localFileName, ConflictCallback cb);
  // Resolves it with the same code as the start dialog (UseHub: local backup first; UseLocal: upload as current). For a
  // running game the local save is replaced live (hooks). The game never starts. `localFileName` names the file if none exists.
  void resolveSlotConflict(const QString& gameId, const QString& slot, Resolution r, const QString& localFileName, RestoreCallback cb);

  // Snapshot of the Hub's current checkpoint (game not running here).
  void createSnapshot(const QString& gameId, const QString& slot, const QString& label, SnapshotCallback cb);
  // In game: first uploads a changed save through the final-sync path, then creates the snapshot.
  void snapshotActive(const QString& label, SnapshotCallback cb);
  // WSS save_updated from the Hub (other device changed a checkpoint).
  void handleSaveUpdate(const SaveUpdate& update);

  static QString localOnlyNote() {
    return QObject::tr("Save sync is not available for this system yet. Saves stay on this device.");
  }
  static QString unsupportedNote() { return QObject::tr("Server does not support save sync"); }
  static QString kindName(Kind k);

  // Test hook: replaces the local backup before overwriting (returns the backup path, empty = failed).
  void setBackupHook(std::function<QString(const QString&, const QString&)> hook) { backupHook_ = std::move(hook); }
  void setTiming(const Timing& t);
  const Timing& timing() const { return timing_; }

  // Connected, Hub advertises saves_v1 and the Hub user ID is known.
  bool available() const;
  bool hubSupportsSaves() const;
  QString note() const;  // UI note when sync is unavailable (empty if available or not connected)

  // Library state of a game (cached from sync.json; call refreshKinds after the library loaded).
  Kind kind(const QString& gameId) const { return kinds_.value(gameId, Kind::None); }
  void refreshKinds(const QStringList& gameIds);
  // Same as refreshKinds with the file access on a worker; emits kindsRefreshed(changed ids) once, no kindChanged per game.
  void refreshKindsAsync(const QStringList& gameIds);

  // Core (id + version) a game is about to start with (ADR 0020 D7).
  struct CoreRef {
    QString id;
    QString version;
    // Save handling of the core's profile (SystemManifest::saveSource/saveExtension/saveFormat). "auto" (experimental
    // core): sync the newest candidate file as before. "none" (3DS: per-title trees in the core's virtual SD/NAND, no
    // single save blob): local-only, the game's save directory is handed to the core and never touched, synced,
    // deleted or overwritten; startReady() carries a note that sync is not available for this system yet. "save_ram" / "core_file": exactly <rom>.sav is synced, never
    // another file. "core_file" with format "desmume_dsv": the core's own <rom><extension> is converted around the
    // core (raw .sav <-> .dsv); the Hub only ever holds the raw save. "core_tree" with format "azahar_sdmc" (3DS): the
    // game's save data in the core's virtual SD tree (core/sdtree.h) is packed into one deterministic tar archive that is
    // the raw <rom>.sav the Hub holds; nand, sysdata, shaders, cheats stay local. "none" is still honored as above.
    QString saveSource = QStringLiteral("auto");
    QString fileExtension;
    QString fileFormat;
    // Core profile save.short_dir: the core gets SaveStore::shortCoreDir (Windows MAX_PATH); a core-created file
    // (.dsv, or the "core_tree" tree) is read and written there, the raw save and sync.json stay in the regular slot directory.
    bool shortDir = false;
    QString subfolder;  // "core_tree": directory below the core dir that holds the tree (profile save.short_dir_subfolder); also
                        // adopted once from the slot directory into the short one
    bool valid() const { return !id.isEmpty() && !version.isEmpty(); }
  };
  // Start sync before the core loads. The core's save dir comes with startReady().
  // With a valid `core`: when the save dir was last written by another core id or version, a manual snapshot on the Hub
  // ("Before core change: <old id> <old ver> → <new id> <new ver>") is created before the start sync and the change is
  // reported in the startReady() note; a failed snapshot (or an unreachable Hub with a local save) ends in startFailed()
  // and the record stays unchanged. No record yet (first start): the core is only recorded.
  void prepareStart(const QString& gameId, const QString& romPath, const QStringList& romBasenames, const CoreRef& core);
  void prepareStart(const QString& gameId, const QString& romPath, const QStringList& romBasenames) { prepareStart(gameId, romPath, romBasenames, CoreRef()); }
  static QString coreChangeLabel(const CoreRef& from, const CoreRef& to);  // at most 64 bytes (Hub limit)
  void resolveConflict(Resolution r);
  bool hasPendingConflictDialog() const { return dialogOpen_; }

  // While the game runs.
  void beginSession();
  // Immediate upload of a changed save (pause: sessionEnd=false; stop/exit: true, ends the session after the upload).
  void finalSync(bool sessionEnd);
  // For app exit: waits up to timing().finalTimeoutMs; remaining changes stay pending. True if clean or uploaded.
  bool finalSyncBlocking(bool sessionEnd);
  bool sessionActive() const { return session_; }
  QString activeGameId() const { return a_.gameId; }
  QString activeSaveFile() const { return a_.file; }
  QString activeGameDir() const { return a_.dir; }

  // Retries all games with pending changes of the current hub/user (not the active game).
  void retryPending();

 signals:
  void startReady(const QString& gameId, const QString& saveDir, const QString& note);
  void startConflict(const uzume::SaveSync::ConflictView& view);
  void resolveFailed(const QString& message);  // dialog stays open
  void startFailed(const QString& gameId, const QString& message);
  void kindChanged(const QString& gameId, uzume::SaveSync::Kind kind);
  void kindsRefreshed(const QStringList& changedIds);
  void finalSyncFinished(const QString& gameId, bool ok);
  // The core's own save file could not be converted; it stays untouched locally and is not uploaded.
  void coreSaveProblem(const QString& gameId, const QString& message);
  void uploaded(const QString& gameId, int revision);  // after every accepted upload (tests/diagnostics)
  // The first failed upload of a row (not offline, not a conflict): `errorCode` is the API error code or "http_<status>". Repeated
  // retries of the same failure stay silent until an upload worked (ADR 0035 error events).
  void uploadFailed(const QString& gameId, const QString& errorCode);
  // save_updated from another device; runningHere = this game runs on this Player with that slot (next upload conflicts).
  void saveChangedElsewhere(const uzume::SaveUpdate& update, bool runningHere);
  // save_updated "renamed" / "deleted" from another device, after the local slot was handled (moved, removed, or kept
  // because it is running or has unsynced changes). `message` is a user-facing notice (empty = nothing to say).
  void slotChangedElsewhere(const uzume::SaveUpdate& update, const QString& message, bool warning);

 private:
  enum class UploadOutcome { Clean, Uploaded, Conflict, Offline, Failed };
  struct Active {
    QString gameId, slot, hubId, userId, dir, file, expectedName, romPath;
    SyncState st;
    std::optional<SaveConflictInfo> conflict;
    QString lastHash;      // hash of the file at the last observation
    qint64 lastSize = -1;
    QDateTime lastMtime;
    bool dirty = false;
    bool uploading = false;
    bool pausedByConflict = false;
    int backoffMs = 0;
    QElapsedTimer sinceChange, sinceUpload, sinceFail;
    bool uploadedOnce = false, failedOnce = false;
    int finalRequested = 0;  // 0 none, 1 final, 2 final_session_end
    QString coreNote;        // one-time notice about a core change, prepended to the start note
    bool localOnly = false;  // saveSource "none": saves stay on this device, nothing is imported, uploaded or replaced
    bool exactSave = false;  // profiled core: only <rom>.sav is the save, never another file in the directory
    QString coreFileExt;     // core-managed save file next to it (".dsv"); empty = none
    QString coreDir;         // directory the core gets when CoreRef::shortDir (the .dsv lives there); empty = a_.dir
    QString coreFileFormat;
    QString coreTree;        // "core_tree": root of the core's tree (core dir + subfolder); empty = no tree conversion
    QString coreTreeFp;      // sdtree::fingerprint at the last observation
    qint64 coreFileSize = -1;
    QDateTime coreFileMtime;
  };

  QString findLocalSave(QStringList* warnings = nullptr) const;
  QString coreFilePath() const;  // below a_.coreDir (the directory the core gets), else a_.dir
  bool importCoreFile(bool backup, QString* error);  // core file / tree -> raw .sav when it changed since the last export/import
  bool exportCoreFile(QString* error);               // raw .sav -> core file / tree before the core starts
  bool importCoreTree(bool backup, QString* error);
  // Live size/content check before anything changes: size via hook (files) or archive validation (tree).
  bool liveAccepts(qint64 size, const QByteArray* data) const;
  bool liveApplyData(const QByteArray& data);  // the running core takes the new raw save (file hook or tree restart)
  bool exportCoreTree(QString* error);
  bool pullCoreFile(bool force);                     // session: import, error reported once on a forced pull
  void emitStartReady(const QString& note);

  void continueStart(quint64 gen);
  void guardCoreChange(quint64 gen, const CoreRef& core);
  void recordCore(const CoreRef& core);
  void failStart(quint64 gen, const QString& message);

  bool sameHub(const QString& hubId) const;
  QString noteForOffline() const;
  void setKind(const QString& gameId, const SyncState& st, bool hasFile);
  void persist();
  void readyToPlay(const QString& note);
  void startSync();  // (re)run reconciliation with the Hub
  void onSlot(quint64 gen, const SaveApiResult& r);
  void onHubSlot(quint64 gen, const SaveSlotInfo& slot);
  void downloadAndApply(quint64 gen, bool backupLocal, const QString& note);
  void uploadAtStart(quint64 gen, const QString& sha);
  void showConflict(const SaveConflictInfo& c);
  std::optional<SaveConflictInfo> ownConflict(const SaveSlotInfo& slot, const QString& preferredId) const;
  ConflictView makeConflictView(const SaveConflictInfo& c, const QString& gameId, const QString& file) const;
  // End of resolveConflict(): the play flow starts the game; a resolution from the saves view calls its callback instead.
  void resolvedOk(const QString& note);
  void resolveError(const QString& message, SaveRestoreResult::Outcome kind = SaveRestoreResult::Outcome::Failed);
  bool writeResolvedSave(const QByteArray& data);
  void resyncActiveFile();
  void restoreVersionLive(const QString& gameId, const QString& slot, int version, int expectedRevision, RestoreCallback cb);
  void uploadSaveFileLive(const QString& gameId, const QString& slot, const QString& filePath, int expectedRevision, RestoreCallback cb);
  QString moveLocalSlot(const QString& gameId, const QString& from, const QString& to);  // empty = ok, else a warning
  void removeLocalSlot(const QString& gameId, const QString& slot);
  // Copies the slot's save file to `<file>.deleted-<stamp>.bak` (beside it for the main slot, in the game directory for
  // another slot, so it survives removeLocalSlot). Empty = ok (also when there is no save file), else why it failed.
  QString backupLocalSlot(const QString& gameId, const QString& slot);
  void handleSlotRenamedElsewhere(const SaveUpdate& u);
  void handleSlotDeletedElsewhere(const SaveUpdate& u);
  // Flush, upload a changed save (final-sync path), then `next(ok, revisionToExpect, message)`; liveOp_ stays set on ok.
  void livePrelude(const QString& gameId, const QString& slot, int expectedRevision,
                   std::function<void(bool, int, const QString&)> next);
  RestoreResult applyLiveContent(const QByteArray& data, int newRevision, const QString& backupTag);
  void poll();
  void pump();
  void uploadCurrent(const QString& reason, std::function<void(UploadOutcome)> done);
  void finishFinal(bool ok);
  void scheduleRetry();
  struct RetryItem {
    QString gameId, slot, dir;
  };
  void retryNext(QList<RetryItem> items);

  HubConnection* conn_;
  ProfileStore* profiles_;
  PlayerSettings* settings_ = nullptr;
  QHash<QString, QString> memorySlots_;  // without settings: hub_id/game_id -> slot
  SaveApi api_;
  Timing timing_;
  std::function<QString(const QString&, const QString&)> backupHook_;
  QHash<QString, Kind> kinds_;
  quint64 kindsGen_ = 0;
  QSet<QString> kindsTouched_;  // set by setKind while an async refresh runs
  std::unique_ptr<QFutureWatcher<QList<QPair<QString, int>>>> kindsWatcher_;
  Active a_;
  quint64 gen_ = 0;
  bool dialogOpen_ = false;
  bool session_ = false;
  bool starting_ = false;  // prepareStart() ran, beginSession() has not (or the start failed before it)
  LiveHooks live_;
  std::function<bool(const QString&)> shared_;
  bool liveOp_ = false;                     // a live restore / upload runs: no checkpoint uploads meanwhile
  RestoreCallback resolveCb_;               // set while a resolution from the saves view runs
  bool resolveLive_ = false;                // ... for the running game (a_ is its session state)
  QTimer pollTimer_;
  QTimer retryTimer_;
  int retryBackoffMs_ = 0;
  bool retryRunning_ = false;
};

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::SaveSync::ConflictView)
Q_DECLARE_METATYPE(uzume::SaveSync::Kind)
