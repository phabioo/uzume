#pragma once
// SaveHistoryController: save slot picker, save history with restore, manual snapshots and the "save changed on
// another device" notice for the QML UI (ADR 0012 D7). Slot choice, restore, snapshots and the sync logic live in
// network/ (SaveSync, SaveApi); this class only holds the view state of the selected game.

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>

#include "hubconnection.h"
#include "savesync.h"

namespace uzume::ui {

class SaveHistoryController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  // Saves view (3c-3): summary block and timeline of the selected game.
  // Current checkpoint of the slot: {} (none yet) or {revision, revisionText, device, when, reason, reasonText, sha256, shaShort,
  // size, sizeText, localPath}. The Hub numbers the current checkpoint by revision, history versions by their own counter.
  Q_PROPERTY(QVariantMap current READ current NOTIFY changed)
  // Versions of the slot: history entries plus the current checkpoint.
  Q_PROPERTY(int versionCount READ versionCount NOTIFY changed)
  Q_PROPERTY(int slotCount READ slotCount NOTIFY changed)
  Q_PROPERTY(QString slotLabel READ slotLabel NOTIFY changed)  // "Main" for the default slot
  // The Hub is reachable (saves available and the last refresh did not fail with a network error). When false the view
  // is read-only and shows the cached state.
  Q_PROPERTY(bool online READ online NOTIFY changed)
  // There is something to show (also from the cache while the Hub is offline).
  Q_PROPERTY(bool hasSaveData READ hasSaveData NOTIFY changed)
  Q_PROPERTY(QString lastUpdated READ lastUpdated NOTIFY changed)  // "22:06" of the last successful refresh; empty = never
  Q_PROPERTY(QString lastSynced READ lastSynced NOTIFY changed)    // "today 18:42" of the same moment
  // Snapshots can be deleted (saves_v3 and the Hub is reachable).
  Q_PROPERTY(bool canDeleteSnapshot READ canDeleteSnapshot NOTIFY changed)
  // saves_v6: every history version can be deleted, not only manual snapshots.
  Q_PROPERTY(bool canDeleteAnyVersion READ canDeleteAnyVersion NOTIFY changed)
  // Pending delete confirmation: {} = none, else {version, versionText, label, slot}.
  Q_PROPERTY(QVariantMap deleteRequest READ deleteRequest NOTIFY changed)
  // A restore, delete or upload confirmation is open or an upload runs: "Play" is disabled (decision af).
  // In the saves view inside the game: restore and upload may load the save into the running core (see SaveSync live hooks).
  Q_PROPERTY(bool liveMode READ liveMode WRITE setLiveMode NOTIFY changed)
  // liveMode and the running core can take a new save: restore / upload restart the game from that save.
  Q_PROPERTY(bool liveApply READ liveApply NOTIFY changed)
  // Inline conflict resolution (no game start): open, loading the Hub side, resolving, and both sides once loaded:
  // {slot, local: {device, when, size, sizeText, exists}, hub: {device, when, size, sizeText, revisionText}}.
  Q_PROPERTY(bool conflictOpen READ conflictOpen NOTIFY changed)
  Q_PROPERTY(bool conflictLoading READ conflictLoading NOTIFY changed)
  Q_PROPERTY(bool conflictBusy READ conflictBusy NOTIFY changed)
  Q_PROPERTY(QVariantMap conflictInfo READ conflictInfo NOTIFY changed)
  Q_PROPERTY(bool confirmationOpen READ confirmationOpen NOTIFY changed)
  Q_PROPERTY(bool uploading READ uploading NOTIFY changed)
  // Last failed upload (retry possible): {} or {path, fileName}.
  Q_PROPERTY(QVariantMap uploadFailure READ uploadFailure NOTIFY changed)
  // Hub advertises saves_v1 (slot picker) / saves_v2 (history, restore, snapshots).
  Q_PROPERTY(bool slotsAvailable READ slotsAvailable NOTIFY changed)
  Q_PROPERTY(bool available READ available NOTIFY changed)
  Q_PROPERTY(QString gameId READ gameId NOTIFY changed)
  Q_PROPERTY(QString slot READ slot NOTIFY changed)
  Q_PROPERTY(QVariantList slotOptions READ slotOptions NOTIFY changed)  // [{value, label, count}] count -1 = unknown
  // [{version, versionText, revision, label, reason, reasonText, device, when, isSnapshot}] newest first
  Q_PROPERTY(QVariantList history READ history NOTIFY changed)
  Q_PROPERTY(bool loading READ loading NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  // The selected game runs (or is starting) on this Player: no slot change, no restore.
  Q_PROPERTY(bool gameRunning READ gameRunning NOTIFY changed)
  // Why restoring is blocked right now (empty = possible).
  Q_PROPERTY(QString restoreBlockReason READ restoreBlockReason NOTIFY changed)
  Q_PROPERTY(bool canSnapshot READ canSnapshot NOTIFY changed)
  // Hub advertises saves_v4: "Upload save file..." is offered (enabled when restoreBlockReason is empty and not busy).
  Q_PROPERTY(bool canUploadFile READ canUploadFile NOTIFY changed)
  Q_PROPERTY(QStringList saveFileFilters READ saveFileFilters CONSTANT)
  // Pending upload confirmation: {} = none, else {path, fileName, size, sizeText, slot, gameTitle}.
  Q_PROPERTY(QVariantMap uploadRequest READ uploadRequest NOTIFY changed)
  // saves_v5: rename / delete the selected slot (never "default"). `slotManageAvailable`: the actions exist (Hub supports
  // them, signed in, not the main slot); `slotManageBlockReason`: why they are disabled right now (empty = allowed).
  Q_PROPERTY(QString uploadBlockReason READ uploadBlockReason NOTIFY changed)  // upload off although supported (shared Session)
  Q_PROPERTY(bool slotManageAvailable READ slotManageAvailable NOTIFY changed)
  // Delete exists for the selected slot: any slot with saves_v5, the main slot only with saves_v6 (rename never).
  Q_PROPERTY(bool slotDeleteAvailable READ slotDeleteAvailable NOTIFY changed)
  Q_PROPERTY(QString slotManageBlockReason READ slotManageBlockReason NOTIFY changed)
  Q_PROPERTY(QVariantMap slotDeleteRequest READ slotDeleteRequest NOTIFY changed)  // {slot, slotLabel, revisionText, when, device, onHub}
  Q_PROPERTY(QString message READ message NOTIFY changed)
  Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY changed)
  // Pending restore confirmation: {} = none, else {version, versionText, label, reasonText, device, when, slot, gameTitle}.
  Q_PROPERTY(QVariantMap restoreRequest READ restoreRequest NOTIFY changed)
  // In game: "the save changed on <device>" (empty when none or no game runs).
  Q_PROPERTY(QString notice READ notice NOTIFY changed)

 public:
  struct Env {
    SaveSync* saves = nullptr;
    HubConnection* connection = nullptr;
    std::function<bool(const QString&)> gameBusy;         // game is starting/running on this Player
    std::function<QString(const QString&)> gameTitle;      // library title of a game (may be empty)
    std::function<QString(const QString&)> localFileName;  // expected local save file name of a game (may be empty)
    std::function<QString(const QString&)> slotDisplayName;  // "Run: <name>" for the save slot of a known run, the name of a variant's slot, else empty
    std::function<QString(const QString&)> slotKind;         // "run" | "variant" | "" for a slot (slotOptions: kind, hint)
  };
  explicit SaveHistoryController(const Env& env, QObject* parent = nullptr);

  bool slotsAvailable() const;
  bool available() const;
  QString gameId() const { return gameId_; }
  QString slot() const;
  QVariantList slotOptions() const;
  QVariantList history() const { return history_; }
  bool loading() const { return pending_ > 0; }
  bool busy() const { return busy_; }
  bool gameRunning() const;
  QString restoreBlockReason() const;
  bool canSnapshot() const;
  bool canUploadFile() const;
  QString uploadBlockReason() const { return gameId_.isEmpty() ? QString() : env_.saves->uploadBlockReason(gameId_); }
  bool slotManageAvailable() const;
  bool slotDeleteAvailable() const;
  QString slotManageBlockReason() const;
  QVariantMap slotDeleteRequest() const { return slotDeleteRequest_; }
  static QStringList saveFileFilters();
  QVariantMap uploadRequest() const { return uploadRequest_; }
  void showError(const QString& text) { setMessage(text, true); }  // non-blocking notice (game toast / Saves view)
  QString message() const { return message_; }
  bool messageIsError() const { return messageIsError_; }
  QString notice() const;
  QVariantMap restoreRequest() const { return restoreRequest_; }
  QVariantMap current() const { return current_; }
  int versionCount() const { return static_cast<int>(history_.size()) + (current_.isEmpty() ? 0 : 1); }
  int slotCount() const { return static_cast<int>(slotOptions().size()); }
  QString slotLabel() const;
  QString slotName(const QString& slot) const;  // label of a slot: Main, "Run: <name>" for a run's slot, else the raw name
  void onSlotNamesChanged() { emit changed(); }
  bool online() const { return slotsAvailable() && !offline_; }
  bool hasSaveData() const { return slotsAvailable() || !current_.isEmpty() || !history_.isEmpty(); }
  QString lastUpdated() const;
  QString lastSynced() const;
  bool canDeleteSnapshot() const;
  bool canDeleteAnyVersion() const;
  QVariantMap deleteRequest() const { return deleteRequest_; }
  bool confirmationOpen() const {
    return !restoreRequest_.isEmpty() || !deleteRequest_.isEmpty() || !uploadRequest_.isEmpty() || uploading_ || conflictOpen_ ||
           !slotDeleteRequest_.isEmpty();
  }
  bool liveMode() const { return liveMode_; }
  void setLiveMode(bool on);
  bool liveApply() const;
  bool conflictOpen() const { return conflictOpen_; }
  bool conflictLoading() const { return conflictLoading_; }
  bool conflictBusy() const { return conflictBusy_; }
  QVariantMap conflictInfo() const { return conflictInfo_; }
  bool uploading() const { return uploading_; }
  QVariantMap uploadFailure() const { return uploadFailure_; }

  // The selected game of the library changed (empty: none).
  void setGame(const QString& gameId);
  // The game phase (starting/running/ended) may have changed: update the restore block and clear stale messages.
  void onGameStateChanged();
  // save_updated from the Hub (see SaveSync::saveChangedElsewhere).
  void onSaveUpdated(const SaveUpdate& update, bool runningHere);

  Q_INVOKABLE void refresh();
  Q_INVOKABLE void selectSlot(const QString& slot);
  // "New slot": empty string = created and selected, otherwise the validation error.
  Q_INVOKABLE QString createSlot(const QString& name);
  Q_INVOKABLE QString validateSlotName(const QString& name) const;  // empty = valid
  // Rename the selected slot: empty string = started (the result arrives as `message`), otherwise the validation error.
  Q_INVOKABLE QString renameSlot(const QString& name);
  // Delete the selected slot (hard delete on the Hub, no undo) with confirmation data: the latest checkpoint.
  Q_INVOKABLE void requestDeleteSlot();
  Q_INVOKABLE void confirmDeleteSlot();
  Q_INVOKABLE void cancelDeleteSlot();
  // Restore with confirmation: requestRestore() opens the dialog data, confirmRestore() runs it, cancelRestore() closes it.
  Q_INVOKABLE void requestRestore(int version);
  Q_INVOKABLE void confirmRestore();
  Q_INVOKABLE void cancelRestore();
  Q_INVOKABLE void restore(int version);
  // Upload a local save file as the current save of the slot: requestUploadFile() (path or file: URL) validates and opens
  // the confirmation data, confirmUploadFile() runs it, cancelUploadFile() closes it.
  Q_INVOKABLE void requestUploadFile(const QString& source);
  Q_INVOKABLE void confirmUploadFile();
  Q_INVOKABLE void cancelUploadFile();
  // Delete a manual snapshot (saves_v3) with confirmation, like restore.
  Q_INVOKABLE void requestDelete(int version);
  Q_INVOKABLE void confirmDelete();
  Q_INVOKABLE void cancelDelete();
  Q_INVOKABLE void retryUpload();
  Q_INVOKABLE void dismissUploadFailure();
  // File details of the current checkpoint.
  Q_INVOKABLE void copyText(const QString& text);
  Q_INVOKABLE void openSaveFolder();
  Q_INVOKABLE void createSnapshot(const QString& label);        // detail pane
  Q_INVOKABLE void createSnapshotInGame(const QString& label);  // running game: uploads a changed save first
  // Save conflict of the slot, resolved here without starting the game: openConflict() reads both sides, then
  // resolveConflict("use_local" | "use_hub"); "use_hub" makes a local backup of this device's save first.
  Q_INVOKABLE void openConflict();
  Q_INVOKABLE void closeConflict();
  Q_INVOKABLE void resolveConflict(const QString& action);
  Q_INVOKABLE void dismissMessage();
  Q_INVOKABLE void dismissNotice();

 signals:
  void changed();

 private:
  void setMessage(const QString& text, bool isError);
  void recomputeBlock();
  static QString reasonText(const QString& reason);
  static QString formatWhen(const QString& iso);
  static QString formatWhen(const QDateTime& when);
  QVariantMap currentMap(const SaveCheckpoint& c) const;
  void fetchOtherSlotCounts(const QString& game, quint64 gen, const QStringList& names);

  Env env_;
  QString gameId_;
  QVariantList history_;
  QStringList remoteSlots_;  // slots of this game on the Hub
  int currentRevision_ = 0;
  int pending_ = 0;
  quint64 gen_ = 0;
  bool busy_ = false;
  QString message_;
  bool messageIsError_ = false;
  QString notice_;
  QString blockReason_;
  QVariantMap restoreRequest_;
  QVariantMap uploadRequest_;
  QVariantMap deleteRequest_;
  QVariantMap slotDeleteRequest_;
  int slotDeleteRevision_ = 0;
  QVariantMap uploadFailure_;
  QVariantMap current_;
  QHash<QString, int> slotVersionCounts_;  // slot -> versions (history + current) of the other slots
  QDateTime lastRefresh_;
  bool offline_ = false;
  bool uploading_ = false;
  bool lastBusy_ = false;
  bool liveMode_ = false;
  bool conflictOpen_ = false;
  bool conflictLoading_ = false;
  bool conflictBusy_ = false;
  QVariantMap conflictInfo_;
};

}  // namespace uzume::ui
