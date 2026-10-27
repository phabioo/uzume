// Save sync against the fake Hub: start sync cases (D4), conflicts, pending retry, checkpoints, final sync.
// Only dummy files; no real saves or ROMs.
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "dsvsave.h"
#include "fakehub.h"
#include "hubconnection.h"
#include "profilestore.h"
#include "savesync.h"
#include "sdtree.h"

using namespace uzume;
using State = HubConnection::State;

namespace {
const QString kGame = QStringLiteral("game-1");
const QString kRom = QStringLiteral("/cache/rom1.nds");  // -> save file rom1.sav
const QString kHubId = QStringLiteral("hub-test-1");
const QString kUser = QStringLiteral("u_test_1");

QByteArray readFile(const QString& p) {
  QFile f(p);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
void writeFile(const QString& p, const QByteArray& d) {
  QDir().mkpath(QFileInfo(p).absolutePath());
  QFile f(p);
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  f.write(d);
}
}  // namespace

class SaveSyncTest : public QObject {
  Q_OBJECT
 private:
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;
  std::unique_ptr<SaveSync> sync_;
  bool noFeature_ = false;
  bool noV4_ = false;
  bool noV5_ = false;
  bool v6_ = false;

  QString gdir() const { return SaveStore::gameDir(*profiles_, kHubId, kUser, kGame); }
  QString saveFile() const { return gdir() + QStringLiteral("/rom1.sav"); }
  int puts() const {
    int n = 0;
    for (const FakeRequest& r : hub_->requests) {
      n += r.method == "PUT" ? 1 : 0;
    }
    return n;
  }
  QByteArray lastPutReason() const {
    for (int i = hub_->requests.size() - 1; i >= 0; --i) {
      if (hub_->requests.at(i).method == "PUT") {
        return hub_->requests.at(i).headers.value(QStringLiteral("x-uzume-sync-reason"));
      }
    }
    return {};
  }

  void connectAll() {
    dir_ = std::make_unique<QTemporaryDir>();
    profiles_ = std::make_unique<ProfileStore>(dir_->path());
    creds_ = std::make_unique<MemoryCredentialStore>();
    hub_ = std::make_unique<FakeHub>(QStringLiteral("a"));
    hub_->features = {QStringLiteral("saves_v1"), QStringLiteral("saves_v2")};
    if (!noV4_) {
      hub_->features.append(QStringLiteral("saves_v4"));
    }
    if (!noV5_) {
      hub_->features.append(QStringLiteral("saves_v5"));
    }
    if (v6_) {
      hub_->features.append(QStringLiteral("saves_v6"));
    }
    if (noFeature_) {
      hub_->features.clear();
    }
    QVERIFY(hub_->start());
    conn_ = std::make_unique<HubConnection>(profiles_.get(), creds_.get());
    conn_->setPollIntervalMs(50);
    conn_->connectToAddress(hub_->address());
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsTrustConfirmation, 8000);
    conn_->confirmTrust();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsPairing, 8000);
    hub_->decision = FakeHub::Decision::Approve;
    conn_->requestPairing();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::Connected, 8000);
    sync_ = std::make_unique<SaveSync>(conn_.get(), profiles_.get());
    SaveSync::Timing t;
    t.pollMs = 20;
    t.debounceMs = 150;
    t.minIntervalMs = 500;
    t.retryBaseMs = 100;
    t.retryMaxMs = 400;
    t.finalTimeoutMs = 2000;
    sync_->setTiming(t);
  }

  // Runs prepareStart and waits for startReady or startConflict. Returns "ready" | "conflict".
  QString start() {
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    QSignalSpy conflict(sync_.get(), &SaveSync::startConflict);
    sync_->prepareStart(kGame, kRom, {QStringLiteral("rom1")});
    (void)QTest::qWaitFor([&]() { return ready.count() + conflict.count() > 0; }, 8000);
    return conflict.count() > 0 ? QStringLiteral("conflict") : (ready.count() > 0 ? QStringLiteral("ready") : QString());
  }
  // prepareStart with a core; waits for startReady / startFailed. Returns "ready" | "failed" | "conflict" (empty = timeout).
  QString startWithCore(const QString& coreId, const QString& version, QString* text = nullptr) {
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    QSignalSpy failed(sync_.get(), &SaveSync::startFailed);
    QSignalSpy conflict(sync_.get(), &SaveSync::startConflict);
    sync_->prepareStart(kGame, kRom, {QStringLiteral("rom1")}, SaveSync::CoreRef{coreId, version});
    (void)QTest::qWaitFor([&]() { return ready.count() + failed.count() + conflict.count() > 0; }, 8000);
    if (text != nullptr) {
      *text = failed.count() > 0 ? failed.at(0).at(1).toString() : (ready.count() > 0 ? ready.at(0).at(2).toString() : QString());
    }
    return failed.count() > 0 ? QStringLiteral("failed") : (conflict.count() > 0 ? QStringLiteral("conflict") : (ready.count() > 0 ? QStringLiteral("ready") : QString()));
  }

  // ---- saves_v5 helpers ----
  QString slotDir(const QString& slot) const { return sync_->slotDirFor(kGame, slot); }
  // A slot on the Hub (revision rev) and optionally its local copy; synced = the local file equals the Hub content.
  void makeSlot(const QString& slot, const QByteArray& hubContent, int rev, bool local, bool synced, const QByteArray& localContent = {}) {
    FakeSlot fs;
    fs.revision = rev;
    fs.content = hubContent;
    fs.deviceId = QStringLiteral("other-device");
    fs.deviceName = QStringLiteral("Laptop Office");
    hub_->saves.insert(FakeHub::slotKey(kGame, slot), fs);
    if (local) {
      const QByteArray content = localContent.isEmpty() ? hubContent : localContent;
      writeFile(slotDir(slot) + QStringLiteral("/rom1.sav"), content);
      SyncState st;
      st.slot = slot;
      st.baseRevision = rev;
      st.lastSyncedSha256 = SaveStore::sha256Of(hubContent);
      st.pending = !synced;
      QVERIFY(SaveStore::saveState(slotDir(slot), st));
    }
  }
  SaveSync::SlotChangeResult renameSlot(const QString& slot, const QString& name) {
    SaveSync::SlotChangeResult res;
    bool done = false;
    sync_->renameSlot(kGame, slot, name, [&](const SaveSync::SlotChangeResult& r) { res = r; done = true; });
    (void)QTest::qWaitFor([&]() { return done; }, 8000);
    return res;
  }
  SaveSync::SlotChangeResult deleteSlot(const QString& slot, int revision) {
    SaveSync::SlotChangeResult res;
    bool done = false;
    sync_->deleteSlot(kGame, slot, revision, [&](const SaveSync::SlotChangeResult& r) { res = r; done = true; });
    (void)QTest::qWaitFor([&]() { return done; }, 8000);
    return res;
  }
  static SaveUpdate slotUpdate(const QString& reason, const QString& slot, const QString& previous = QString()) {
    SaveUpdate u;
    u.gameId = kGame;
    u.slot = slot;
    u.previousSlot = previous;
    u.revision = 2;
    u.sha256 = QString(64, QLatin1Char('a'));
    u.deviceId = QStringLiteral("other-device");
    u.deviceName = QStringLiteral("Laptop");
    u.reason = reason;
    return u;
  }
  int snapshotsOnHub() const {
    int n = 0;
    for (const FakeVersion& v : hub_->saves.value(kGame).history) {
      n += v.reason == QLatin1String("manual_snapshot") ? 1 : 0;
    }
    return n;
  }

  // After a conflict dialog: resolve and wait for ready.
  QString resolve(SaveSync::Resolution r) {
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    QSignalSpy conflict(sync_.get(), &SaveSync::startConflict);
    QSignalSpy failed(sync_.get(), &SaveSync::resolveFailed);
    sync_->resolveConflict(r);
    (void)QTest::qWaitFor([&]() { return ready.count() + conflict.count() + failed.count() > 0; }, 8000);
    return failed.count() > 0 ? QStringLiteral("failed") : (conflict.count() > 0 ? QStringLiteral("conflict") : QStringLiteral("ready"));
  }

  // ---- Conflicts outside the play flow, live save --------------------------------------------------------------------

  // A conflict that is "decided later": local "local-offline" vs Hub "hub-2", the game is not running.
  void makeOpenConflict() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-offline");
    hub_->setHubSave(kGame, "hub-2");
    QCOMPARE(start(), QStringLiteral("conflict"));
    QCOMPARE(resolve(SaveSync::Resolution::DecideLater), QStringLiteral("ready"));
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Conflict);
  }
  SaveSync::SlotConflict loadConflict() {
    SaveSync::SlotConflict out;
    bool done = false;
    sync_->loadSlotConflict(kGame, QStringLiteral("default"), QStringLiteral("rom1.sav"), [&](const SaveSync::SlotConflict& c) { out = c; done = true; });
    (void)QTest::qWaitFor([&]() { return done; }, 8000);
    return out;
  }
  SaveSync::RestoreResult resolveHere(SaveSync::Resolution r) {
    SaveSync::RestoreResult out;
    bool done = false;
    sync_->resolveSlotConflict(kGame, QStringLiteral("default"), r, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& x) { out = x; done = true; });
    (void)QTest::qWaitFor([&]() { return done; }, 8000);
    return out;
  }

  struct LiveCore {  // fake running core: 12 bytes of save memory, `apply` writes the save file like the emulation thread would
    int flushes = 0, applies = 0, resets = 0;
    QByteArray applied;
    qint64 size = 12;
    bool applyOk = true;
    QString file;
  };
  void installLiveCore(LiveCore* core) {
    SaveSync::LiveHooks h;
    h.flush = [core]() { ++core->flushes; };
    h.accepts = [core](qint64 n) { return n == core->size; };
    h.apply = [core](const QByteArray& d) {
      ++core->applies;
      if (!core->applyOk) {
        return false;
      }
      core->applied = d;
      writeFile(core->file, d);
      ++core->resets;
      return true;
    };
    sync_->setLiveHooks(std::move(h));
  }
  // A running game with a synced save of 12 bytes.
  void startRunning(LiveCore* core) {
    hub_->setHubSave(kGame, "hub-cp-1-xxx");  // 12 bytes
    QCOMPARE(start(), QStringLiteral("ready"));
    core->file = saveFile();
    installLiveCore(core);
    sync_->beginSession();
  }

 private slots:
  void init() {
    noFeature_ = false;
    noV4_ = false;
    noV5_ = false;
    v6_ = false;
    qRegisterMetaType<uzume::SaveSync::Kind>();
    connectAll();
  }
  void cleanup() {
    sync_.reset();
    conn_.reset();
    hub_.reset();
    profiles_.reset();
    creds_.reset();
    dir_.reset();
  }

  void noHubSlotNoLocalFileStartsEmpty() {
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(puts(), 0);
    QVERIFY(!QFileInfo::exists(saveFile()));
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::None);
  }

  void noHubSlotLocalFileUploadsWithBase0() {
    writeFile(saveFile(), "local-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(puts(), 1);
    QCOMPARE(hub_->saves.value(kGame).revision, 1);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("local-1"));
    QCOMPARE(hub_->requests.last().headers.value(QStringLiteral("x-uzume-base-revision")), QByteArray("0"));
    const SyncState st = SaveStore::loadState(gdir());
    QCOMPARE(st.baseRevision, 1);
    QVERIFY(!st.pending);
    QCOMPARE(st.lastSyncedSha256, SaveStore::sha256Of("local-1"));
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Synced);
    // No secret in sync.json
    const QByteArray raw = readFile(SaveStore::stateFilePath(gdir()));
    QVERIFY(!raw.contains("fba_") && !raw.contains("fbd_") && !raw.toLower().contains("token"));
  }

  void hubSlotNoLocalFileDownloads() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(readFile(saveFile()), QByteArray("hub-1"));
    QCOMPARE(SaveStore::loadState(gdir()).baseRevision, 1);
    QCOMPARE(puts(), 0);
  }

  void localUnchangedHubNewerDownloadsWithBackup() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    hub_->setHubSave(kGame, "hub-2");
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(readFile(saveFile()), QByteArray("hub-2"));
    QCOMPARE(readFile(saveFile() + QStringLiteral(".bak")), QByteArray("hub-1"));
    QCOMPARE(SaveStore::loadState(gdir()).baseRevision, 2);
    QCOMPARE(puts(), 0);
  }

  void localChangedHubUnchangedUploads() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-2");
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(hub_->saves.value(kGame).revision, 2);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("local-2"));
    QCOMPARE(SaveStore::loadState(gdir()).baseRevision, 2);
  }

  void idempotentReuploadDoesNotCreateRevisionOrConflict() {
    hub_->setHubSave(kGame, "same");
    writeFile(saveFile(), "same");  // identical content, but no sync.json (e.g. lost state)
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(puts(), 1);
    QCOMPARE(hub_->saves.value(kGame).revision, 1);
    QVERIFY(hub_->saves.value(kGame).conflicts.isEmpty());
    QCOMPARE(SaveStore::loadState(gdir()).baseRevision, 1);
    // Repeating after the state is in sync does not upload again
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(puts(), 1);
  }

  void staleBaseLeadsToConflictAndKeepsEverything() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-offline");
    hub_->setHubSave(kGame, "hub-2");  // other device
    QSignalSpy conflict(sync_.get(), &SaveSync::startConflict);
    sync_->prepareStart(kGame, kRom, {QStringLiteral("rom1")});
    QTRY_COMPARE_WITH_TIMEOUT(conflict.count(), 1, 8000);
    const auto v = conflict.at(0).at(0).value<SaveSync::ConflictView>();
    QCOMPARE(v.conflict.hubRevision, 2);
    QCOMPARE(v.conflict.hubDeviceName, QStringLiteral("Laptop Office"));
    QCOMPARE(v.localBaseRevision, 1);
    QCOMPARE(v.localSha256, SaveStore::sha256Of("local-offline"));
    QVERIFY(!v.localDeviceName.isEmpty() && v.localModified.isValid());
    QVERIFY(sync_->hasPendingConflictDialog());
    // Hub current unchanged, upload secured, conflict recorded; local file untouched
    QCOMPARE(hub_->saves.value(kGame).revision, 2);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("hub-2"));
    QCOMPARE(hub_->saves.value(kGame).conflicts.size(), 1);
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));
    const SyncState st = SaveStore::loadState(gdir());
    QVERIFY(!st.conflictId.isEmpty());
    QVERIFY(st.pending);
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Conflict);
  }

  // 0.20: after cheats or memory writes the uploads of the play session carry X-Uzume-Cheats-Used; a new session starts clean.
  void uploadsAfterCheatsCarryTheMarker() {
    writeFile(saveFile(), "local-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    QVERIFY(!hub_->requests.last().headers.contains(QStringLiteral("x-uzume-cheats-used")));
    sync_->beginSession();
    sync_->setCheatsUsed(true);
    writeFile(saveFile(), "local-2");
    const int before = puts();
    sync_->finalSyncBlocking(true);
    QCOMPARE(puts(), before + 1);
    QCOMPARE(hub_->requests.last().headers.value(QStringLiteral("x-uzume-cheats-used")), QByteArray("true"));
    sync_->beginSession();
    QVERIFY(!sync_->cheatsUsed());
  }

  void conflictDecideLaterKeepsLocalAndPausesUploads() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-offline");
    hub_->setHubSave(kGame, "hub-2");
    QCOMPARE(start(), QStringLiteral("conflict"));
    QCOMPARE(resolve(SaveSync::Resolution::DecideLater), QStringLiteral("ready"));
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Conflict);
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));
    // Session: uploads paused while the conflict is open, even for changes
    const int putsBefore = puts();
    sync_->beginSession();
    writeFile(saveFile(), "local-offline-more");
    QTest::qWait(700);
    QCOMPARE(puts(), putsBefore);
    sync_->finalSyncBlocking(true);
    QCOMPARE(puts(), putsBefore);
    QVERIFY(SaveStore::loadState(gdir()).pending);
    // Next start: dialog again from the persisted/Hub conflict, without a new upload
    QCOMPARE(start(), QStringLiteral("conflict"));
    QCOMPARE(puts(), putsBefore);
    QCOMPARE(hub_->saves.value(kGame).conflicts.size(), 1);
  }

  void conflictUseHubReplacesLocalWithBackup() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-offline");
    hub_->setHubSave(kGame, "hub-2");
    QCOMPARE(start(), QStringLiteral("conflict"));
    QCOMPARE(resolve(SaveSync::Resolution::UseHub), QStringLiteral("ready"));
    QCOMPARE(readFile(saveFile()), QByteArray("hub-2"));
    QStringList backups = QDir(gdir()).entryList({QStringLiteral("rom1.sav.local-*.bak")});
    QCOMPARE(backups.size(), 1);
    QCOMPARE(readFile(gdir() + QLatin1Char('/') + backups.first()), QByteArray("local-offline"));
    const SyncState st = SaveStore::loadState(gdir());
    QVERIFY(!st.pending && st.conflictId.isEmpty());
    QCOMPARE(st.baseRevision, 2);
    QCOMPARE(hub_->saves.value(kGame).conflicts.first().status, QStringLiteral("resolved_hub"));
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Synced);
  }

  void useLocalWithMissingLocalFileDownloadsPromotedCheckpoint() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-offline");
    hub_->setHubSave(kGame, "hub-2");
    QCOMPARE(start(), QStringLiteral("conflict"));
    QVERIFY(QFile::remove(saveFile()));  // local file vanished while the dialog was open
    QCOMPARE(resolve(SaveSync::Resolution::UseLocal), QStringLiteral("ready"));
    QCOMPARE(hub_->saves.value(kGame).revision, 3);
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));  // the secured upload, now the Hub current
    const SyncState st = SaveStore::loadState(gdir());
    QVERIFY(!st.pending && st.conflictId.isEmpty());
    QCOMPARE(st.baseRevision, 3);
    QCOMPARE(st.lastSyncedSha256, SaveStore::sha256Of("local-offline"));
  }

  void useHubAbortsWhenBackupFails() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-offline");
    hub_->setHubSave(kGame, "hub-2");
    QCOMPARE(start(), QStringLiteral("conflict"));
    sync_->setBackupHook([](const QString&, const QString&) { return QString(); });  // backup fails
    QCOMPARE(resolve(SaveSync::Resolution::UseHub), QStringLiteral("failed"));
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));  // not replaced
    QCOMPARE(hub_->saves.value(kGame).conflicts.first().status, QStringLiteral("open"));  // resolve never called
    QCOMPARE(hub_->saves.value(kGame).revision, 2);
    QVERIFY(!SaveStore::loadState(gdir()).conflictId.isEmpty());
    QVERIFY(sync_->hasPendingConflictDialog());
    // After the problem is gone the same dialog can be resolved
    sync_->setBackupHook({});
    QCOMPARE(resolve(SaveSync::Resolution::UseHub), QStringLiteral("ready"));
    QCOMPARE(readFile(saveFile()), QByteArray("hub-2"));
  }

  void conflictUseLocalAdoptsLocal() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-offline");
    hub_->setHubSave(kGame, "hub-2");
    QCOMPARE(start(), QStringLiteral("conflict"));
    QCOMPARE(resolve(SaveSync::Resolution::UseLocal), QStringLiteral("ready"));
    QCOMPARE(hub_->saves.value(kGame).revision, 3);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("local-offline"));
    const SyncState st = SaveStore::loadState(gdir());
    QVERIFY(!st.pending && st.conflictId.isEmpty());
    QCOMPARE(st.baseRevision, 3);
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));
  }

  void staleResolveRerunsStartSyncAndShowsDialogAgain() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-offline");
    hub_->setHubSave(kGame, "hub-2");
    QCOMPARE(start(), QStringLiteral("conflict"));
    hub_->setHubSave(kGame, "hub-3");  // changes while the dialog is open
    QCOMPARE(resolve(SaveSync::Resolution::UseLocal), QStringLiteral("conflict"));
    QCOMPARE(hub_->saves.value(kGame).revision, 3);  // nothing was changed by the stale resolution
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("hub-3"));
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));
  }

  void hubUnreachableStartsLocalAndRetriesLater() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-2");
    hub_->failSaveRequests = 1;  // slot request fails (503)
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(readFile(saveFile()), QByteArray("local-2"));
    SyncState st = SaveStore::loadState(gdir());
    QVERIFY(st.pending);
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Pending);
    // Retry (pending sync, same Hub)
    QSignalSpy up(sync_.get(), &SaveSync::uploaded);
    sync_->retryPending();
    QTRY_COMPARE_WITH_TIMEOUT(up.count(), 1, 5000);
    st = SaveStore::loadState(gdir());
    QVERIFY(!st.pending);
    QCOMPARE(st.baseRevision, 2);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("local-2"));
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Synced);
  }

  void pendingRetriedWithBackoffWhileConnected() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-2");
    hub_->failSaveRequests = 2;  // slot GET and the first retry fail
    QCOMPARE(start(), QStringLiteral("ready"));
    QSignalSpy up(sync_.get(), &SaveSync::uploaded);
    sync_->retryPending();  // fails (offline) -> schedules a retry with backoff
    QTRY_COMPARE_WITH_TIMEOUT(up.count(), 1, 5000);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("local-2"));
  }

  void pendingOfAnotherHubIsNeverSentToThisHub() {
    const QString otherDir = SaveStore::gameDir(*profiles_, QStringLiteral("hub-other"), kUser, QStringLiteral("game-x"));
    writeFile(otherDir + QStringLiteral("/x.sav"), "other-hub-save");
    SyncState st;
    st.pending = true;
    QVERIFY(SaveStore::saveState(otherDir, st));
    // Same hub, other user: also separated
    const QString otherUser = SaveStore::gameDir(*profiles_, kHubId, QStringLiteral("u_other"), QStringLiteral("game-y"));
    writeFile(otherUser + QStringLiteral("/y.sav"), "other-user-save");
    QVERIFY(SaveStore::saveState(otherUser, st));
    sync_->retryPending();
    QTest::qWait(300);
    QCOMPARE(puts(), 0);
    QVERIFY(SaveStore::loadState(otherDir).pending);
    QVERIFY(SaveStore::loadState(otherUser).pending);
  }

  void legacySaveIsCopiedNotMoved() {
    const QString legacy = SaveStore::legacyDir(*profiles_, kHubId) + QStringLiteral("/rom1.sav");
    writeFile(legacy, "legacy-save");
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(readFile(saveFile()), QByteArray("legacy-save"));
    QVERIFY(QFileInfo::exists(legacy));  // old file stays
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("legacy-save"));
    QCOMPARE(hub_->requests.last().headers.value(QStringLiteral("x-uzume-base-revision")), QByteArray("0"));
  }

  void hubWithoutSavesFeatureDisablesSync() {
    cleanup();
    noFeature_ = true;
    connectAll();
    QVERIFY(!sync_->available());
    QCOMPARE(sync_->note(), QStringLiteral("Server does not support save sync"));
    writeFile(saveFile(), "local");
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    sync_->prepareStart(kGame, kRom, {});
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(ready.at(0).at(2).toString(), QStringLiteral("Server does not support save sync"));
    QCOMPARE(hub_->count(QStringLiteral("/api/v1/games/")), 0);
    QCOMPARE(hub_->count(QStringLiteral("/api/v1/saves")), 0);
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::None);
  }

  void autoCheckpointDebounceIntervalAndHashSkip() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    sync_->beginSession();
    writeFile(saveFile(), "play-1");
    QTest::qWait(20);
    QCOMPARE(puts(), 0);  // debounce not over yet
    QTRY_COMPARE_WITH_TIMEOUT(puts(), 1, 3000);
    QCOMPARE(lastPutReason(), QByteArray("checkpoint"));
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("play-1"));
    // Next change: only after the minimum interval (500 ms since the last upload). Measured on
    // the Hub's clock (request arrival times), not on the test's polling clock, so a slow
    // runner cannot make the check racy.
    writeFile(saveFile(), "play-22");
    QTRY_COMPARE_WITH_TIMEOUT(puts(), 2, 3000);
    QList<qint64> putArrivals;
    for (const FakeRequest& r : hub_->requests) {
      if (r.method == "PUT") {
        putArrivals.append(r.arrivalMs);
      }
    }
    QCOMPARE(putArrivals.size(), 2);
    // 500 ms minimum interval; the first request may arrive slightly after SaveSync took its
    // timestamp, so allow 50 ms of slack.
    QVERIFY2(putArrivals.at(1) - putArrivals.at(0) >= 450,
             qPrintable(QStringLiteral("gap between uploads was %1 ms").arg(putArrivals.at(1) - putArrivals.at(0))));
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("play-22"));
    // Same content again (new mtime, same hash): no upload
    writeFile(saveFile(), "play-22");
    QTest::qWait(900);
    QCOMPARE(puts(), 2);
    sync_->finalSyncBlocking(true);
    QCOMPARE(puts(), 2);  // nothing changed: no final upload either
  }

  void finalSyncUploadsImmediatelyOnPauseAndStop() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    sync_->beginSession();
    writeFile(saveFile(), "play-1");
    QSignalSpy fin(sync_.get(), &SaveSync::finalSyncFinished);
    sync_->finalSync(false);  // pause
    QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 3000);
    QVERIFY(fin.at(0).at(1).toBool());
    QCOMPARE(lastPutReason(), QByteArray("final"));
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("play-1"));
    QVERIFY(sync_->sessionActive());
    writeFile(saveFile(), "play-2");
    QVERIFY(sync_->finalSyncBlocking(true));  // stop/exit
    QCOMPARE(lastPutReason(), QByteArray("final_session_end"));
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("play-2"));
    QVERIFY(!sync_->sessionActive());
    QVERIFY(!SaveStore::loadState(gdir()).pending);
  }

  void finalSyncFailureLeavesPending() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    sync_->beginSession();
    writeFile(saveFile(), "play-1");
    hub_->failSaveRequests = 1;
    QVERIFY(!sync_->finalSyncBlocking(true));
    QVERIFY(SaveStore::loadState(gdir()).pending);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("hub-1"));
    // Retried at the next opportunity
    QSignalSpy up(sync_.get(), &SaveSync::uploaded);
    sync_->retryPending();
    QTRY_COMPARE_WITH_TIMEOUT(up.count(), 1, 5000);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("play-1"));
  }

  // ---------------------------------------------------------------- saves_v2: restore, snapshots, push, slots

  void restoreReplacesLocalSaveAndRespectsPending() {
    hub_->setHubSave(kGame, "cp-1");
    hub_->addHistory(kGame, QStringLiteral("default"), "old-content", QStringLiteral("session_end"), QStringLiteral("Boss"));
    QCOMPARE(start(), QStringLiteral("ready"));  // downloads cp-1
    QCOMPARE(readFile(saveFile()), QByteArray("cp-1"));
    QVERIFY(sync_->hubSupportsSavesV2());
    QVERIFY(sync_->pendingReason(kGame, QStringLiteral("default")).isEmpty());

    // Pending local changes block the restore; nothing is sent
    writeFile(saveFile(), "unsynced-local");
    QVERIFY(!sync_->pendingReason(kGame, QStringLiteral("default")).isEmpty());
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->restoreVersion(kGame, QStringLiteral("default"), 1, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY(done);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QCOMPARE(hub_->restoreCount, 0);
    QCOMPARE(readFile(saveFile()), QByteArray("unsynced-local"));

    // Stale expected revision: 409, nothing changes
    writeFile(saveFile(), "cp-1");  // back to the synced content
    done = false;
    sync_->restoreVersion(kGame, QStringLiteral("default"), 1, 7, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY(done);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Stale);
    QCOMPARE(hub_->restoreCount, 0);
    QCOMPARE(readFile(saveFile()), QByteArray("cp-1"));

    // Restore: Hub checkpoint + local save replaced (download), backup of the old local file kept
    done = false;
    sync_->restoreVersion(kGame, QStringLiteral("default"), 1, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY(done);
    QVERIFY2(res.ok(), qPrintable(res.message));
    QVERIFY(res.localUpdated);
    QCOMPARE(res.revision, 2);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("old-content"));
    QCOMPARE(readFile(saveFile()), QByteArray("old-content"));
    QCOMPARE(QDir(gdir()).entryList({QStringLiteral("*.restore-*.bak")}).size(), 1);
    const SyncState st = SaveStore::loadState(gdir());
    QCOMPARE(st.baseRevision, 2);
    QVERIFY(!st.pending);
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Synced);
    QCOMPARE(hub_->saves.value(kGame).history.last().reason, QStringLiteral("before_restore"));

    // Not while the game runs
    sync_->prepareStart(kGame, kRom, {QStringLiteral("rom1")});
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    QTRY_COMPARE(ready.count(), 1);
    sync_->beginSession();
    done = false;
    sync_->restoreVersion(kGame, QStringLiteral("default"), 1, 2, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY(done);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QCOMPARE(hub_->restoreCount, 1);
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void uploadSaveFileReplacesHubAndLocalSave() {
    hub_->setHubSave(kGame, "cp-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    QVERIFY(sync_->hubSupportsSavesV4());
    const QString dflt = QStringLiteral("default");
    const QString picked = QDir(dir_->path()).filePath(QStringLiteral("picked.srm"));
    writeFile(picked, "uploaded-bytes");
    const auto run = [&](const QString& file, int rev) {
      SaveSync::RestoreResult res;
      bool done = false;
      sync_->uploadSaveFile(kGame, dflt, file, rev, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
      (void)QTest::qWaitFor([&]() { return done; }, 8000);
      return res;
    };

    // Stale expected revision: error, local file untouched
    SaveSync::RestoreResult res = run(picked, 5);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Stale);
    QCOMPARE(hub_->uploadCount, 0);
    QCOMPARE(readFile(saveFile()), QByteArray("cp-1"));

    // Empty file is refused locally, nothing is sent
    const QString empty = QDir(dir_->path()).filePath(QStringLiteral("empty.sav"));
    writeFile(empty, "");
    res = run(empty, 1);
    QVERIFY(!res.ok());
    QVERIFY(!res.message.isEmpty());
    QCOMPARE(hub_->uploadCount, 0);

    // Too large for the Hub (413): error, local file untouched
    hub_->uploadLimit = 4;
    res = run(picked, 1);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Failed);
    QVERIFY(res.message.contains(QStringLiteral("too large")));
    QCOMPARE(readFile(saveFile()), QByteArray("cp-1"));
    hub_->uploadLimit = 64LL * 1024 * 1024;

    // Success: Hub checkpoint, local file replaced, backup, state
    res = run(picked, 1);
    QVERIFY2(res.ok(), qPrintable(res.message));
    QVERIFY(res.localUpdated);
    QCOMPARE(res.revision, 2);
    QCOMPARE(hub_->uploadCount, 1);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("uploaded-bytes"));
    QCOMPARE(hub_->saves.value(kGame).reason, QStringLiteral("upload"));
    QCOMPARE(hub_->saves.value(kGame).history.last().reason, QStringLiteral("before_upload"));
    QCOMPARE(readFile(saveFile()), QByteArray("uploaded-bytes"));
    QCOMPARE(QDir(gdir()).entryList({QStringLiteral("*.upload-*.bak")}).size(), 1);
    const SyncState st = SaveStore::loadState(gdir());
    QCOMPARE(st.baseRevision, 2);
    QVERIFY(!st.pending);
    QCOMPARE(st.lastSyncedSha256, SaveStore::sha256Of(QByteArray("uploaded-bytes")));
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Synced);

    // Pending local changes block it
    writeFile(saveFile(), "unsynced-local");
    res = run(picked, 2);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QCOMPARE(hub_->uploadCount, 1);
    QCOMPARE(readFile(saveFile()), QByteArray("unsynced-local"));
    writeFile(saveFile(), "uploaded-bytes");

    // Not while the game runs
    sync_->prepareStart(kGame, kRom, {QStringLiteral("rom1")});
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    QTRY_COMPARE(ready.count(), 1);
    sync_->beginSession();
    res = run(picked, 2);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QCOMPARE(hub_->uploadCount, 1);
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void uploadSaveFileNeedsSavesV4() {
    cleanup();
    noV4_ = true;
    connectAll();
    QVERIFY(!sync_->hubSupportsSavesV4());
    const QString picked = QDir(dir_->path()).filePath(QStringLiteral("picked.srm"));
    writeFile(picked, "x");
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->uploadSaveFile(kGame, QStringLiteral("default"), picked, 0, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY(done);
    QVERIFY(!res.ok());
    QCOMPARE(hub_->uploadCount, 0);
  }

  void snapshotInGameUploadsChangedSaveFirst() {
    hub_->setHubSave(kGame, "cp-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    sync_->beginSession();
    writeFile(saveFile(), "play-1");
    SaveSync::SnapshotResult res;
    bool done = false;
    sync_->snapshotActive(QStringLiteral("Before boss"), [&](const SaveSync::SnapshotResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY2(res.ok(), qPrintable(res.message));
    QCOMPARE(res.version.label, QStringLiteral("Before boss"));
    QCOMPARE(res.version.reason, QStringLiteral("manual_snapshot"));
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("play-1"));  // uploaded first
    QCOMPARE(hub_->saves.value(kGame).history.last().content, QByteArray("play-1"));
    QCOMPARE(lastPutReason(), QByteArray("final"));
    QVERIFY(sync_->sessionActive());
    QVERIFY(sync_->finalSyncBlocking(true));

    // Snapshot of an unknown slot: not found, clear message
    done = false;
    sync_->createSnapshot(kGame, QStringLiteral("nothing"), QString(), [&](const SaveSync::SnapshotResult& r) { res = r; done = true; });
    QTRY_VERIFY(done);
    QCOMPARE(res.kind, SaveSnapshotResult::Outcome::NotFound);
  }

  void conflictIsLoadedAndResolvedWithoutStartingTheGame_useHub() {
    makeOpenConflict();
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    QSignalSpy conflictSig(sync_.get(), &SaveSync::startConflict);
    const SaveSync::SlotConflict c = loadConflict();
    QCOMPARE(c.kind, SaveSync::SlotConflict::Outcome::Found);
    QCOMPARE(c.view.conflict.hubRevision, 2);
    QCOMPARE(c.view.conflict.hubDeviceName, QStringLiteral("Laptop Office"));
    QVERIFY(c.localExists);
    QCOMPARE(c.localSize, qint64(13));  // "local-offline"
    QCOMPARE(c.hubSize, qint64(5));     // "hub-2"
    QVERIFY(!c.view.localDeviceName.isEmpty() && c.view.localModified.isValid());
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));  // looking changes nothing

    const SaveSync::RestoreResult r = resolveHere(SaveSync::Resolution::UseHub);
    QVERIFY2(r.ok(), qPrintable(r.message));
    QCOMPARE(readFile(saveFile()), QByteArray("hub-2"));
    const QStringList backups = QDir(gdir()).entryList({QStringLiteral("rom1.sav.local-*.bak")});
    QCOMPARE(backups.size(), 1);
    QCOMPARE(readFile(gdir() + QLatin1Char('/') + backups.first()), QByteArray("local-offline"));
    QCOMPARE(hub_->saves.value(kGame).conflicts.first().status, QStringLiteral("resolved_hub"));
    const SyncState st = SaveStore::loadState(gdir());
    QVERIFY(!st.pending && st.conflictId.isEmpty());
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Synced);
    QCOMPARE(ready.count(), 0);  // the game start flow never ran
    QCOMPARE(conflictSig.count(), 0);
    QVERIFY(!sync_->sessionActive() && !sync_->hasPendingConflictDialog());
    // Resolved: loading again finds none
    QCOMPARE(loadConflict().kind, SaveSync::SlotConflict::Outcome::None);
  }

  void conflictIsResolvedWithoutStartingTheGame_useLocal() {
    makeOpenConflict();
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    const SaveSync::RestoreResult r = resolveHere(SaveSync::Resolution::UseLocal);
    QVERIFY2(r.ok(), qPrintable(r.message));
    QCOMPARE(hub_->saves.value(kGame).revision, 3);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("local-offline"));
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));
    const SyncState st = SaveStore::loadState(gdir());
    QVERIFY(!st.pending && st.conflictId.isEmpty());
    QCOMPARE(st.baseRevision, 3);
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Synced);
    QCOMPARE(ready.count(), 0);
  }

  void conflictResolutionAbortsOnBackupFailureAndStaleHub() {
    makeOpenConflict();
    sync_->setBackupHook([](const QString&, const QString&) { return QString(); });
    SaveSync::RestoreResult r = resolveHere(SaveSync::Resolution::UseHub);
    QVERIFY(!r.ok());
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));
    QCOMPARE(hub_->saves.value(kGame).conflicts.first().status, QStringLiteral("open"));
    QVERIFY(!SaveStore::loadState(gdir()).conflictId.isEmpty());
    sync_->setBackupHook({});
    // The Hub changes between "look" and "decide": stale, nothing changed, the caller loads the conflict again
    hub_->setHubSave(kGame, "hub-3");
    r = resolveHere(SaveSync::Resolution::UseLocal);
    QCOMPARE(r.kind, SaveRestoreResult::Outcome::Stale);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("hub-3"));
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));
  }

  void conflictResolvedElsewhereClearsTheLocalMarker() {
    makeOpenConflict();
    hub_->saves[kGame].conflicts[0].status = QStringLiteral("resolved_hub");  // e.g. in the server admin
    QCOMPARE(loadConflict().kind, SaveSync::SlotConflict::Outcome::None);
    QVERIFY(SaveStore::loadState(gdir()).conflictId.isEmpty());
    QVERIFY(sync_->kind(kGame) != SaveSync::Kind::Conflict);
    QCOMPARE(readFile(saveFile()), QByteArray("local-offline"));
  }

  void liveRestoreLoadsTheSaveIntoTheRunningCoreAfterBackup() {
    LiveCore core;
    startRunning(&core);
    hub_->addHistory(kGame, QStringLiteral("default"), "hub-old-1234", QStringLiteral("session_end"));
    QVERIFY(sync_->liveApplyAvailable(kGame, QStringLiteral("default")));
    QVERIFY(sync_->liveBlockReason(kGame, QStringLiteral("default")).isEmpty());
    // The game has progressed (unsynced): it is uploaded first and kept in the history, then the restore happens
    writeFile(saveFile(), "play-newer-xx");
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->restoreVersion(kGame, QStringLiteral("default"), 1, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QVERIFY2(res.ok() && res.message.isEmpty() && res.localUpdated, qPrintable(res.message));
    QVERIFY(core.flushes >= 1);
    QCOMPARE(core.applies, 1);
    QCOMPARE(core.applied, QByteArray("hub-old-1234"));
    QCOMPARE(readFile(saveFile()), QByteArray("hub-old-1234"));
    QCOMPARE(hub_->restoreCount, 1);
    // the progress of the game is on the Hub (final upload + before_restore) and in a local backup
    QCOMPARE(hub_->saves.value(kGame).history.last().reason, QStringLiteral("before_restore"));
    QCOMPARE(hub_->saves.value(kGame).history.last().content, QByteArray("play-newer-xx"));
    const QStringList backups = QDir(gdir()).entryList({QStringLiteral("*.restore-*.bak")});
    QCOMPARE(backups.size(), 1);
    QCOMPARE(readFile(gdir() + QLatin1Char('/') + backups.first()), QByteArray("play-newer-xx"));
    const SyncState st = SaveStore::loadState(gdir());
    QVERIFY(!st.pending);
    QCOMPARE(st.baseRevision, 3);
    QCOMPARE(st.lastSyncedSha256, SaveStore::sha256Of("hub-old-1234"));
    // The restored file is not uploaded again as a "change"
    const int putsBefore = puts();
    QTest::qWait(600);
    QCOMPARE(puts(), putsBefore);
    QVERIFY(sync_->sessionActive());
    QVERIFY(sync_->finalSyncBlocking(true));
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("hub-old-1234"));
  }

  void liveRestoreRefusesWhenTheCoreCannotTakeTheSave() {
    LiveCore core;
    startRunning(&core);
    hub_->addHistory(kGame, QStringLiteral("default"), "hub-old-other-size", QStringLiteral("session_end"));  // not 12 bytes
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->restoreVersion(kGame, QStringLiteral("default"), 1, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QVERIFY(res.message.contains(QStringLiteral("Quit the game")));
    QCOMPARE(hub_->restoreCount, 0);  // nothing changed on the Hub
    QCOMPARE(core.applies, 0);
    QCOMPARE(readFile(saveFile()), QByteArray("hub-cp-1-xxx"));
    // The core fails after the Hub restored: the local file stays, the message asks for a restart
    hub_->addHistory(kGame, QStringLiteral("default"), "hub-old-1234", QStringLiteral("session_end"));
    core.applyOk = false;
    done = false;
    sync_->restoreVersion(kGame, QStringLiteral("default"), 2, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QVERIFY(res.ok() && !res.localUpdated);
    QVERIFY(res.message.contains(QStringLiteral("Quit the game and start it again")));
    QCOMPARE(readFile(saveFile()), QByteArray("hub-cp-1-xxx"));
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void liveRestoreIsBlockedByAnOpenConflict() {
    LiveCore core;
    startRunning(&core);
    writeFile(saveFile(), "local-newer-x");
    hub_->setHubSave(kGame, "hub-newer-xx");
    QSignalSpy fin(sync_.get(), &SaveSync::finalSyncFinished);
    sync_->finalSync(false);  // the checkpoint upload runs into a conflict
    QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 8000);
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Conflict);
    QVERIFY(!sync_->liveBlockReason(kGame, QStringLiteral("default")).isEmpty());
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->restoreVersion(kGame, QStringLiteral("default"), 1, 2, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QCOMPARE(core.applies, 0);
    // ... and is resolved in the game: "Use the Hub save" backs up the local save and loads the Hub save into the core
    const SaveSync::SlotConflict c = loadConflict();
    QCOMPARE(c.kind, SaveSync::SlotConflict::Outcome::Found);
    QCOMPARE(c.localSize, qint64(13));
    const SaveSync::RestoreResult r = resolveHere(SaveSync::Resolution::UseHub);
    QVERIFY2(r.ok(), qPrintable(r.message));
    QCOMPARE(core.applied, QByteArray("hub-newer-xx"));
    QCOMPARE(readFile(saveFile()), QByteArray("hub-newer-xx"));
    QCOMPARE(QDir(gdir()).entryList({QStringLiteral("rom1.sav.local-*.bak")}).size(), 1);
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Synced);
    QVERIFY(sync_->sessionActive());
    // Uploads are free again: the next change is a normal checkpoint on top of the resolved revision
    const int putsBefore = puts();
    writeFile(saveFile(), "play-after-xxx");
    QVERIFY(sync_->finalSyncBlocking(true));
    QCOMPARE(puts(), putsBefore + 1);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("play-after-xxx"));
  }

  void liveConflictKeepLocalNeedsNoCoreChange() {
    LiveCore core;
    startRunning(&core);
    writeFile(saveFile(), "local-newer-x");
    hub_->setHubSave(kGame, "hub-newer-xx");
    QSignalSpy fin(sync_.get(), &SaveSync::finalSyncFinished);
    sync_->finalSync(false);
    QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 8000);
    const SaveSync::RestoreResult r = resolveHere(SaveSync::Resolution::UseLocal);
    QVERIFY2(r.ok(), qPrintable(r.message));
    QCOMPARE(core.applies, 0);  // the running game already has this save
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("local-newer-x"));
    QCOMPARE(readFile(saveFile()), QByteArray("local-newer-x"));
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Synced);
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void liveUploadLoadsTheFileIntoTheRunningCore() {
    LiveCore core;
    startRunning(&core);
    const QString picked = dir_->filePath(QStringLiteral("picked.sav"));
    writeFile(picked, "picked-12-byt");  // 13 bytes: the core takes 12 only
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->uploadSaveFile(kGame, QStringLiteral("default"), picked, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QCOMPARE(hub_->uploadCount, 0);
    writeFile(picked, "picked-12-by");
    done = false;
    sync_->uploadSaveFile(kGame, QStringLiteral("default"), picked, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QVERIFY2(res.ok() && res.message.isEmpty(), qPrintable(res.message));
    QCOMPARE(hub_->uploadCount, 1);
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("picked-12-by"));
    QCOMPARE(core.applied, QByteArray("picked-12-by"));
    QCOMPARE(QDir(gdir()).entryList({QStringLiteral("*.upload-*.bak")}).size(), 1);
    QVERIFY(!SaveStore::loadState(gdir()).pending);
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void liveUploadIsBlockedWhileTheGameIsShared() {
    LiveCore core;
    startRunning(&core);
    sync_->setSharedCheck([](const QString& id) { return id == kGame; });
    const QString picked = dir_->filePath(QStringLiteral("picked.sav"));
    writeFile(picked, "picked-12-by");
    QVERIFY(!sync_->uploadBlockReason(kGame).isEmpty());
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->uploadSaveFile(kGame, QStringLiteral("default"), picked, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QVERIFY(res.message.contains(QStringLiteral("Session")));
    QCOMPARE(hub_->uploadCount, 0);
    QVERIFY(core.applied.isEmpty());
    sync_->setSharedCheck({});  // not shared any more: the same upload works live
    QVERIFY(sync_->uploadBlockReason(kGame).isEmpty());
    done = false;
    sync_->uploadSaveFile(kGame, QStringLiteral("default"), picked, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QVERIFY2(res.ok(), qPrintable(res.message));
    QCOMPARE(core.applied, QByteArray("picked-12-by"));
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void snapshotInGameFlushesTheCoreFirst() {
    LiveCore core;
    startRunning(&core);
    writeFile(saveFile(), "play-1-xxxxxx");
    SaveSync::SnapshotResult res;
    bool done = false;
    sync_->snapshotActive(QString(), [&](const SaveSync::SnapshotResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QVERIFY(res.ok());
    QCOMPARE(core.flushes, 1);
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void saveUpdatedPushIsReportedAndOwnDeviceIgnored() {
    hub_->setHubSave(kGame, "cp-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    QSignalSpy spy(sync_.get(), &SaveSync::saveChangedElsewhere);
    SaveUpdate u;
    u.gameId = kGame;
    u.slot = QStringLiteral("default");
    u.revision = 5;
    u.sha256 = QString(64, QLatin1Char('a'));
    u.deviceId = QStringLiteral("other-device");
    u.deviceName = QStringLiteral("Laptop");
    u.reason = QStringLiteral("checkpoint");
    sync_->handleSaveUpdate(u);  // game not running here
    QCOMPARE(spy.count(), 1);
    QVERIFY(!spy.at(0).at(1).toBool());
    sync_->beginSession();
    u.deviceId = QStringLiteral("00000000-0000-0000-0000-000000000000");  // server admin, any reason string
    u.deviceName = QStringLiteral("Server admin");
    u.reason = QStringLiteral("conflict_resolution");
    sync_->handleSaveUpdate(u);
    QCOMPARE(spy.count(), 2);
    QVERIFY(spy.at(1).at(1).toBool());  // running here with this slot
    u.slot = QStringLiteral("boss");
    sync_->handleSaveUpdate(u);
    QVERIFY(!spy.at(2).at(1).toBool());  // other slot
    u.deviceId = conn_->deviceId();
    sync_->handleSaveUpdate(u);
    QCOMPARE(spy.count(), 3);  // own device ignored
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void renameSlotMovesTheLocalSlotWithUnsyncedData() {
    PlayerSettings settings(dir_->path());
    sync_->setSettings(&settings);
    makeSlot(QStringLiteral("boss"), "hub-boss", 2, true, false, "local-progress");
    QVERIFY(sync_->setSlot(kGame, QStringLiteral("boss")));
    QVERIFY(sync_->hubSupportsSavesV5());
    const SaveSync::SlotChangeResult res = renameSlot(QStringLiteral("boss"), QStringLiteral("final"));
    QVERIFY2(res.ok(), qPrintable(res.message));
    QVERIFY(res.message.isEmpty());
    QCOMPARE(hub_->renameCount, 1);
    QVERIFY(!hub_->saves.contains(FakeHub::slotKey(kGame, QStringLiteral("boss"))));
    QCOMPARE(hub_->saves.value(FakeHub::slotKey(kGame, QStringLiteral("final"))).content, QByteArray("hub-boss"));
    QVERIFY(!QFileInfo::exists(slotDir(QStringLiteral("boss"))));
    QCOMPARE(readFile(slotDir(QStringLiteral("final")) + QStringLiteral("/rom1.sav")), QByteArray("local-progress"));
    const SyncState st = SaveStore::loadState(slotDir(QStringLiteral("final")));
    QCOMPARE(st.slot, QStringLiteral("final"));
    QVERIFY(st.pending);                   // the unsynced change travels with the slot
    QCOMPARE(st.baseRevision, 2);
    QCOMPARE(sync_->slotFor(kGame), QStringLiteral("final"));
    QCOMPARE(PlayerSettings(dir_->path()).saveSlot(kHubId, kGame), QStringLiteral("final"));
    QCOMPARE(sync_->kind(kGame), SaveSync::Kind::Pending);
  }

  void renameSlotSetsAsideAStaleLocalFolderOfTheNewName() {
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    writeFile(slotDir(QStringLiteral("old")) + QStringLiteral("/rom1.sav"), "stale-old");  // local leftover, not on the Hub
    const SaveSync::SlotChangeResult res = renameSlot(QStringLiteral("boss"), QStringLiteral("old"));
    QVERIFY2(res.ok(), qPrintable(res.message));
    QCOMPARE(readFile(slotDir(QStringLiteral("old")) + QStringLiteral("/rom1.sav")), QByteArray("hub-boss"));
    const QStringList aside = QDir(QFileInfo(slotDir(QStringLiteral("old"))).absolutePath()).entryList({QStringLiteral("old.replaced-*")}, QDir::Dirs);
    QCOMPARE(aside.size(), 1);  // nothing was overwritten or deleted
    QCOMPARE(readFile(QFileInfo(slotDir(QStringLiteral("old"))).absolutePath() + QLatin1Char('/') + aside.first() + QStringLiteral("/rom1.sav")),
             QByteArray("stale-old"));
  }

  void renameSlotOfALocalOnlySlotRenamesLocally() {
    writeFile(slotDir(QStringLiteral("scratch")) + QStringLiteral("/rom1.sav"), "only-here");  // never uploaded: 404 on the Hub
    const SaveSync::SlotChangeResult res = renameSlot(QStringLiteral("scratch"), QStringLiteral("kept"));
    QVERIFY2(res.ok(), qPrintable(res.message));
    QCOMPARE(hub_->renameCount, 0);
    QCOMPARE(readFile(slotDir(QStringLiteral("kept")) + QStringLiteral("/rom1.sav")), QByteArray("only-here"));
    QVERIFY(!QFileInfo::exists(slotDir(QStringLiteral("scratch"))));
  }

  void renameSlotErrorsAreMappedAndNothingMoves() {
    using SO = SaveSync::SlotChangeResult::Outcome;
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    makeSlot(QStringLiteral("taken"), "hub-taken", 1, false, true);
    SaveSync::SlotChangeResult res = renameSlot(QStringLiteral("boss"), QStringLiteral("taken"));
    QCOMPARE(res.kind, SO::Exists);
    QVERIFY(res.message.contains(QStringLiteral("already exists")));
    res = renameSlot(QStringLiteral("default"), QStringLiteral("x"));
    QCOMPARE(res.kind, SO::Protected);
    res = renameSlot(QStringLiteral("boss"), QStringLiteral("Bad Name"));
    QCOMPARE(res.kind, SO::Failed);
    res = renameSlot(QStringLiteral("boss"), QStringLiteral("con"));  // Windows reserved name
    QCOMPARE(res.kind, SO::Failed);
    const int requestsBefore = hub_->count(QStringLiteral("/api/v1/games/"), "POST");
    hub_->sessionRunningGames.append(kGame);
    res = renameSlot(QStringLiteral("boss"), QStringLiteral("fresh"));
    QCOMPARE(res.kind, SO::SessionRunning);
    QVERIFY(res.message.contains(QStringLiteral("Session")));
    QCOMPARE(hub_->renameCount, 0);
    QVERIFY(QFileInfo::exists(slotDir(QStringLiteral("boss"))));
    QVERIFY(hub_->count(QStringLiteral("/api/v1/games/"), "POST") > requestsBefore);
    hub_->sessionRunningGames.clear();
    hub_->failSaveRequests = 1;
    res = renameSlot(QStringLiteral("boss"), QStringLiteral("fresh"));
    QCOMPARE(res.kind, SO::Offline);
    QVERIFY(QFileInfo::exists(slotDir(QStringLiteral("boss"))));
  }

  void renameAndDeleteAreBlockedWhileTheGameRunsHere() {
    using SO = SaveSync::SlotChangeResult::Outcome;
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    QCOMPARE(start(), QStringLiteral("ready"));
    sync_->beginSession();
    const int before = hub_->requests.size();
    QCOMPARE(renameSlot(QStringLiteral("boss"), QStringLiteral("fresh")).kind, SO::Blocked);
    QCOMPARE(deleteSlot(QStringLiteral("boss"), 1).kind, SO::Blocked);
    QCOMPARE(hub_->requests.size(), before);  // refused before any request
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void renameNeedsSavesV5() {
    cleanup();
    noV5_ = true;
    connectAll();
    QVERIFY(!sync_->hubSupportsSavesV5());
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    const int before = hub_->requests.size();
    QVERIFY(!renameSlot(QStringLiteral("boss"), QStringLiteral("fresh")).ok());
    QVERIFY(!deleteSlot(QStringLiteral("boss"), 1).ok());
    QCOMPARE(hub_->requests.size(), before);
    QVERIFY(QFileInfo::exists(slotDir(QStringLiteral("boss"))));
  }

  void deleteSlotRemovesTheLocalCopyAfterTheHubConfirmed() {
    PlayerSettings settings(dir_->path());
    sync_->setSettings(&settings);
    makeSlot(QStringLiteral("boss"), "hub-boss", 3, true, true);
    QVERIFY(sync_->setSlot(kGame, QStringLiteral("boss")));
    const SaveSync::SlotChangeResult res = deleteSlot(QStringLiteral("boss"), 3);
    QVERIFY2(res.ok(), qPrintable(res.message));
    QCOMPARE(hub_->slotDeleteCount, 1);
    QVERIFY(!hub_->saves.contains(FakeHub::slotKey(kGame, QStringLiteral("boss"))));
    QVERIFY(!QFileInfo::exists(slotDir(QStringLiteral("boss"))));
    QCOMPARE(sync_->slotFor(kGame), QStringLiteral("default"));
    QCOMPARE(PlayerSettings(dir_->path()).saveSlot(kHubId, kGame), QStringLiteral("default"));
    bool sawDelete = false;
    for (const FakeRequest& r : hub_->requests) {
      sawDelete = sawDelete || (r.method == "DELETE" && r.path.endsWith(QStringLiteral("/saves/boss?confirm_revision=3")));
    }
    QVERIFY(sawDelete);
  }

  void deleteSlotWithAStaleRevisionKeepsEverything() {
    using SO = SaveSync::SlotChangeResult::Outcome;
    makeSlot(QStringLiteral("boss"), "hub-boss", 4, true, true);
    const SaveSync::SlotChangeResult res = deleteSlot(QStringLiteral("boss"), 3);  // the user saw r3, the Hub is at r4
    QCOMPARE(res.kind, SO::Stale);
    QCOMPARE(hub_->slotDeleteCount, 0);
    QVERIFY(hub_->saves.contains(FakeHub::slotKey(kGame, QStringLiteral("boss"))));
    QCOMPARE(readFile(slotDir(QStringLiteral("boss")) + QStringLiteral("/rom1.sav")), QByteArray("hub-boss"));
  }

  void deleteSlotRefusalsKeepTheLocalCopy() {
    using SO = SaveSync::SlotChangeResult::Outcome;
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    hub_->sessionRunningGames.append(kGame);
    QCOMPARE(deleteSlot(QStringLiteral("boss"), 1).kind, SO::SessionRunning);
    hub_->sessionRunningGames.clear();
    FakeConflict c;
    c.id = QStringLiteral("c1");
    hub_->saves[FakeHub::slotKey(kGame, QStringLiteral("boss"))].conflicts.append(c);
    SaveSync::SlotChangeResult res = deleteSlot(QStringLiteral("boss"), 1);
    QVERIFY(res.kind == SO::Blocked || res.kind == SO::Conflict);  // the local conflict marker or the Hub refuses
    QCOMPARE(deleteSlot(QStringLiteral("default"), 1).kind, SO::Protected);
    QCOMPARE(hub_->slotDeleteCount, 0);
    QVERIFY(hub_->saves.contains(FakeHub::slotKey(kGame, QStringLiteral("boss"))));
    QVERIFY(QFileInfo::exists(slotDir(QStringLiteral("boss")) + QStringLiteral("/rom1.sav")));
  }

  void deleteSlotWithUnsyncedLocalChangesIsRefusedBeforeAnyRequest() {
    using SO = SaveSync::SlotChangeResult::Outcome;
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, false, "local-progress");
    const int before = hub_->requests.size();
    const SaveSync::SlotChangeResult res = deleteSlot(QStringLiteral("boss"), 1);
    QCOMPARE(res.kind, SO::Blocked);
    QVERIFY(res.message.contains(QStringLiteral("not uploaded")));
    QCOMPARE(hub_->requests.size(), before);
    QCOMPARE(readFile(slotDir(QStringLiteral("boss")) + QStringLiteral("/rom1.sav")), QByteArray("local-progress"));
  }

  void deleteSlotThatIsOnlyLocalRemovesItAndOneOnTheHubIsNotTouchedWithoutARevision() {
    using SO = SaveSync::SlotChangeResult::Outcome;
    writeFile(slotDir(QStringLiteral("scratch")) + QStringLiteral("/rom1.sav"), "only-here");
    SyncState st;
    st.slot = QStringLiteral("scratch");
    st.lastSyncedSha256 = SaveStore::sha256Of("only-here");
    QVERIFY(SaveStore::saveState(slotDir(QStringLiteral("scratch")), st));
    QVERIFY(deleteSlot(QStringLiteral("scratch"), 0).ok());
    QVERIFY(!QFileInfo::exists(slotDir(QStringLiteral("scratch"))));
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    QCOMPARE(deleteSlot(QStringLiteral("boss"), 0).kind, SO::Stale);  // it is on the Hub: a revision is required
    QCOMPARE(hub_->slotDeleteCount, 0);
    QVERIFY(QFileInfo::exists(slotDir(QStringLiteral("boss"))));
  }

  void renamedPushMovesACleanLocalSlotAndTheSelection() {
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    QVERIFY(sync_->setSlot(kGame, QStringLiteral("boss")));
    QSignalSpy changed(sync_.get(), &SaveSync::slotChangedElsewhere);
    QSignalSpy plain(sync_.get(), &SaveSync::saveChangedElsewhere);
    sync_->handleSaveUpdate(slotUpdate(QStringLiteral("renamed"), QStringLiteral("final"), QStringLiteral("boss")));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(plain.count(), 0);
    QVERIFY(!changed.at(0).at(2).toBool());
    QVERIFY(changed.at(0).at(1).toString().contains(QStringLiteral("renamed")));
    QVERIFY(!QFileInfo::exists(slotDir(QStringLiteral("boss"))));
    QCOMPARE(readFile(slotDir(QStringLiteral("final")) + QStringLiteral("/rom1.sav")), QByteArray("hub-boss"));
    QCOMPARE(SaveStore::loadState(slotDir(QStringLiteral("final"))).slot, QStringLiteral("final"));
    QCOMPARE(sync_->slotFor(kGame), QStringLiteral("final"));
  }

  void renamedPushKeepsUnsyncedLocalProgressAndSaysSo() {
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, false, "local-progress");
    QVERIFY(sync_->setSlot(kGame, QStringLiteral("boss")));
    QSignalSpy changed(sync_.get(), &SaveSync::slotChangedElsewhere);
    sync_->handleSaveUpdate(slotUpdate(QStringLiteral("renamed"), QStringLiteral("final"), QStringLiteral("boss")));
    QCOMPARE(changed.count(), 1);
    QVERIFY(changed.at(0).at(2).toBool());  // warning
    QVERIFY(changed.at(0).at(1).toString().contains(QStringLiteral("unsynced")));
    QCOMPARE(readFile(slotDir(QStringLiteral("boss")) + QStringLiteral("/rom1.sav")), QByteArray("local-progress"));
    QVERIFY(!QFileInfo::exists(slotDir(QStringLiteral("final"))));
    QCOMPARE(sync_->slotFor(kGame), QStringLiteral("boss"));
  }

  void renamedPushOfTheRunningSlotChangesNothingLocally() {
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    QVERIFY(sync_->setSlot(kGame, QStringLiteral("boss")));
    QCOMPARE(start(), QStringLiteral("ready"));
    sync_->beginSession();
    QSignalSpy changed(sync_.get(), &SaveSync::slotChangedElsewhere);
    sync_->handleSaveUpdate(slotUpdate(QStringLiteral("renamed"), QStringLiteral("final"), QStringLiteral("boss")));
    QCOMPARE(changed.count(), 1);
    QVERIFY(changed.at(0).at(2).toBool());
    QVERIFY(QFileInfo::exists(slotDir(QStringLiteral("boss"))));
    QVERIFY(!QFileInfo::exists(slotDir(QStringLiteral("final"))));
    QCOMPARE(sync_->slotFor(kGame), QStringLiteral("boss"));
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void deletedPushRemovesACleanLocalSlotAndFallsBackToDefault() {
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    QVERIFY(sync_->setSlot(kGame, QStringLiteral("boss")));
    QSignalSpy changed(sync_.get(), &SaveSync::slotChangedElsewhere);
    sync_->handleSaveUpdate(slotUpdate(QStringLiteral("deleted"), QStringLiteral("boss")));
    QCOMPARE(changed.count(), 1);
    QVERIFY(!changed.at(0).at(2).toBool());
    QVERIFY(!QFileInfo::exists(slotDir(QStringLiteral("boss"))));
    QCOMPARE(sync_->slotFor(kGame), QStringLiteral("default"));
  }

  void deletedPushKeepsUnsyncedLocalProgress() {
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, false, "local-progress");
    QVERIFY(sync_->setSlot(kGame, QStringLiteral("boss")));
    QSignalSpy changed(sync_.get(), &SaveSync::slotChangedElsewhere);
    sync_->handleSaveUpdate(slotUpdate(QStringLiteral("deleted"), QStringLiteral("boss")));
    QCOMPARE(changed.count(), 1);
    QVERIFY(changed.at(0).at(2).toBool());
    QVERIFY(changed.at(0).at(1).toString().contains(QStringLiteral("kept")));
    QCOMPARE(readFile(slotDir(QStringLiteral("boss")) + QStringLiteral("/rom1.sav")), QByteArray("local-progress"));
  }

  void deletedPushForMainSlotKeepsUnsyncedLocalProgress() {
    makeSlot(QStringLiteral("default"), "hub-main", 1, true, false, "local-progress");
    QSignalSpy changed(sync_.get(), &SaveSync::slotChangedElsewhere);
    sync_->handleSaveUpdate(slotUpdate(QStringLiteral("deleted"), QStringLiteral("default")));
    QCOMPARE(changed.count(), 1);
    QVERIFY(changed.at(0).at(2).toBool());
    QVERIFY(changed.at(0).at(1).toString().contains(QStringLiteral("main slot")));
    QCOMPARE(readFile(saveFile()), QByteArray("local-progress"));
    QCOMPARE(sync_->slotFor(kGame), QStringLiteral("default"));
  }

  void deletedPushForMainSlotRemovesCleanLocalCopyOnly() {
    makeSlot(QStringLiteral("default"), "hub-main", 1, true, true);
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true, "boss-local");
    QSignalSpy changed(sync_.get(), &SaveSync::slotChangedElsewhere);
    sync_->handleSaveUpdate(slotUpdate(QStringLiteral("deleted"), QStringLiteral("default")));
    QCOMPARE(changed.count(), 1);
    QVERIFY(!changed.at(0).at(2).toBool());
    QVERIFY(!QFileInfo::exists(saveFile()));
    QVERIFY(!QFileInfo::exists(SaveStore::stateFilePath(gdir())));  // no stale Hub base
    QCOMPARE(readFile(slotDir(QStringLiteral("boss")) + QStringLiteral("/rom1.sav")), QByteArray("boss-local"));
    QCOMPARE(sync_->slotFor(kGame), QStringLiteral("default"));
    // The next start begins fresh from the empty Hub slot, without a conflict
    hub_->saves.remove(kGame);
    writeFile(saveFile(), "fresh");
    QCOMPARE(start(), QStringLiteral("ready"));
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("fresh"));
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  // PN-7: a "deleted elsewhere" push keeps a backup of the clean local copy before removing it.
  void deletedPushKeepsABackupOfTheRemovedLocalSlot() {
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    QSignalSpy changed(sync_.get(), &SaveSync::slotChangedElsewhere);
    sync_->handleSaveUpdate(slotUpdate(QStringLiteral("deleted"), QStringLiteral("boss")));
    QCOMPARE(changed.count(), 1);
    QVERIFY(!QFileInfo::exists(slotDir(QStringLiteral("boss"))));
    const QStringList baks = QDir(gdir()).entryList({QStringLiteral("rom1.sav.boss-deleted-*.bak")}, QDir::Files);
    QCOMPARE(baks.size(), 1);
    QCOMPARE(readFile(gdir() + QLatin1Char('/') + baks.first()), QByteArray("hub-boss"));

    makeSlot(QStringLiteral("default"), "hub-main", 1, true, true);
    sync_->handleSaveUpdate(slotUpdate(QStringLiteral("deleted"), QStringLiteral("default")));
    QVERIFY(!QFileInfo::exists(saveFile()));
    const QStringList mainBaks = QDir(gdir()).entryList({QStringLiteral("rom1.sav.deleted-*.bak")}, QDir::Files);
    QCOMPARE(mainBaks.size(), 1);
    QCOMPARE(readFile(gdir() + QLatin1Char('/') + mainBaks.first()), QByteArray("hub-main"));
  }

  // The main slot can be deleted from the Player when the Hub offers saves_v6 (never without it).
  void mainSlotDeleteNeedsSavesV6() {
    using SO = SaveSync::SlotChangeResult::Outcome;
    makeSlot(QStringLiteral("default"), "hub-main", 2, true, true);
    QCOMPARE(deleteSlot(QStringLiteral("default"), 2).kind, SO::Protected);  // saves_v5 only
    QCOMPARE(hub_->slotDeleteCount, 0);
    QVERIFY(QFileInfo::exists(saveFile()));
    cleanup();
    v6_ = true;
    connectAll();
    QVERIFY(sync_->hubSupportsSavesV6());
    makeSlot(QStringLiteral("default"), "hub-main", 2, true, true);
    makeSlot(QStringLiteral("boss"), "hub-boss", 1, true, true);
    const SaveSync::SlotChangeResult res = deleteSlot(QStringLiteral("default"), 2);
    QVERIFY2(res.ok(), qPrintable(res.message));
    QCOMPARE(hub_->slotDeleteCount, 1);
    QVERIFY(!hub_->saves.contains(FakeHub::slotKey(kGame, QStringLiteral("default"))));
    QVERIFY(!QFileInfo::exists(saveFile()));
    QCOMPARE(readFile(slotDir(QStringLiteral("boss")) + QStringLiteral("/rom1.sav")), QByteArray("hub-boss"));  // other slots stay
    // Renaming the main slot stays refused
    QCOMPARE(renameSlot(QStringLiteral("default"), QStringLiteral("x")).kind, SO::Protected);
  }

  // PN-1: resolving a conflict of another slot of the running game must not take over the running session.
  void resolvingAnotherSlotOfTheRunningGameLeavesTheSessionAlone() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    sync_->beginSession();
    QVERIFY(sync_->isRunning(kGame, QStringLiteral("default")));
    makeSlot(QStringLiteral("foo"), "hub-foo", 2, true, false, "foo-local");
    SyncState st = SaveStore::loadState(slotDir(QStringLiteral("foo")));
    st.conflictId = QStringLiteral("c1");
    QVERIFY(SaveStore::saveState(slotDir(QStringLiteral("foo")), st));
    SaveSync::RestoreResult out;
    bool done = false;
    sync_->resolveSlotConflict(kGame, QStringLiteral("foo"), SaveSync::Resolution::UseHub, QStringLiteral("rom1.sav"),
                               [&](const SaveSync::RestoreResult& x) { out = x; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(out.kind, SaveSync::RestoreResult::Outcome::Blocked);
    QVERIFY(sync_->isRunning(kGame, QStringLiteral("default")));  // still the running session of the main slot
    QCOMPARE(readFile(slotDir(QStringLiteral("foo")) + QStringLiteral("/rom1.sav")), QByteArray("foo-local"));
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  // PN-8: retryPending must not upload the slot whose game is in its start flow; the start flow does it once.
  void retryPendingSkipsAGameInItsStartFlow() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    writeFile(saveFile(), "local-progress");
    SyncState st = SaveStore::loadState(gdir());
    st.pending = true;
    QVERIFY(SaveStore::saveState(gdir(), st));
    const int before = puts();
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    sync_->prepareStart(kGame, kRom, {QStringLiteral("rom1")});
    sync_->retryPending();  // reconnect-time retry right while the game starts
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 8000);
    QTest::qWait(300);
    QCOMPARE(puts() - before, 1);  // only the start flow uploaded it
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("local-progress"));
  }

  void uploadFileIsBlockedWhileTheGameRunsHere() {
    hub_->setHubSave(kGame, "cp-1");
    QCOMPARE(start(), QStringLiteral("ready"));
    sync_->beginSession();
    const QString picked = QDir(dir_->path()).filePath(QStringLiteral("picked.sav"));
    writeFile(picked, "picked");
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->uploadSaveFile(kGame, QStringLiteral("default"), picked, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QCOMPARE(hub_->uploadCount, 0);
    QVERIFY(sync_->finalSyncBlocking(true));
  }

  void uploadFileDuringASessionIsRefusedByTheHub() {
    hub_->setHubSave(kGame, "cp-1");
    hub_->sessionRunningGames.append(kGame);
    const QString picked = QDir(dir_->path()).filePath(QStringLiteral("picked.sav"));
    writeFile(picked, "picked");
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->uploadSaveFile(kGame, QStringLiteral("default"), picked, 1, QStringLiteral("rom1.sav"), [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QCOMPARE(res.kind, SaveRestoreResult::Outcome::Blocked);
    QVERIFY(res.message.contains(QStringLiteral("Session")));
    QCOMPARE(hub_->uploadCount, 0);
  }

  void slotSelectionIsPersistedAndUsedBySync() {
    PlayerSettings settings(dir_->path());
    sync_->setSettings(&settings);
    QCOMPARE(sync_->slotFor(kGame), QStringLiteral("default"));
    QVERIFY(!sync_->setSlot(kGame, QStringLiteral("Bad Slot")));
    QVERIFY(sync_->setSlot(kGame, QStringLiteral("boss")));
    QCOMPARE(PlayerSettings(dir_->path()).saveSlot(kHubId, kGame), QStringLiteral("boss"));

    const QString sdir = SaveStore::slotDir(*profiles_, kHubId, kUser, kGame, QStringLiteral("boss"));
    QVERIFY(sdir != gdir());
    writeFile(sdir + QStringLiteral("/rom1.sav"), "boss-local");
    writeFile(saveFile(), "default-local");  // other slot, must stay untouched
    QCOMPARE(start(), QStringLiteral("ready"));  // uploads the boss slot's file at start
    QCOMPARE(hub_->saves.value(FakeHub::slotKey(kGame, QStringLiteral("boss"))).content, QByteArray("boss-local"));
    QVERIFY(!hub_->saves.contains(kGame));
    QCOMPARE(sync_->activeSaveFile(), sdir + QStringLiteral("/rom1.sav"));
    QCOMPARE(sync_->activeSlot(), QStringLiteral("boss"));
    QCOMPARE(SaveStore::loadState(sdir).slot, QStringLiteral("boss"));

    // checkpoint + final sync go to the chosen slot
    sync_->beginSession();
    writeFile(sdir + QStringLiteral("/rom1.sav"), "boss-play");
    QVERIFY(sync_->finalSyncBlocking(true));
    QCOMPARE(hub_->saves.value(FakeHub::slotKey(kGame, QStringLiteral("boss"))).content, QByteArray("boss-play"));
    QVERIFY(!hub_->saves.contains(kGame));
    QCOMPARE(readFile(saveFile()), QByteArray("default-local"));

    // Pending retry also covers non-default slots
    writeFile(sdir + QStringLiteral("/rom1.sav"), "boss-offline");
    SyncState st = SaveStore::loadState(sdir);
    st.pending = true;
    SaveStore::saveState(sdir, st);
    QSignalSpy up(sync_.get(), &SaveSync::uploaded);
    sync_->retryPending();
    QTRY_COMPARE_WITH_TIMEOUT(up.count(), 1, 5000);
    QCOMPARE(hub_->saves.value(FakeHub::slotKey(kGame, QStringLiteral("boss"))).content, QByteArray("boss-offline"));
  }

  // ---------------------------------------------------------------- ADR 0020 D7: save snapshot before a core change

  void localOnlySaveSourceNeverTouchesTheHubOrTheFiles() {
    // 3DS (saveSource "none"): the core's save tree lives in the game's save directory; nothing is synced, nothing is
    // imported, replaced or deleted, even when the Hub holds a save for the same game id.
    hub_->setHubSave(kGame, "hub-1");
    const QString tree = gdir() + QStringLiteral("/default/sdmc/Nintendo 3DS/00000000/title.dat");
    writeFile(tree, "dummy-3ds-tree");
    writeFile(saveFile(), "stray-local-file");  // even a file named like a SAVE_RAM save stays untouched
    const int requestsBefore = hub_->requests.size();
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    SaveSync::CoreRef core{QStringLiteral("azahar"), QStringLiteral("2026.10.09")};
    core.saveSource = QStringLiteral("none");
    sync_->prepareStart(kGame, kRom, {QStringLiteral("rom1")}, core);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
    QVERIFY(ready.at(0).at(2).toString().contains(QStringLiteral("not available for this system yet")));
    QVERIFY(!ready.at(0).at(1).toString().isEmpty());  // the save directory is handed to the core
    sync_->beginSession();
    QVERIFY(!sync_->sessionActive());
    QSignalSpy finished(sync_.get(), &SaveSync::finalSyncFinished);
    sync_->finalSync(true);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 4000);
    QVERIFY(finished.at(0).at(1).toBool());
    QCOMPARE(hub_->requests.size(), requestsBefore);  // no request to the Hub at all
    QCOMPARE(readFile(tree), QByteArray("dummy-3ds-tree"));
    QCOMPARE(readFile(saveFile()), QByteArray("stray-local-file"));
    QCOMPARE(puts(), 0);
  }

  void firstStartWithACoreOnlyRecords() {
    hub_->setHubSave(kGame, "hub-1");
    QString text;
    QCOMPARE(startWithCore(QStringLiteral("melondsds"), QStringLiteral("2026.10.09"), &text), QStringLiteral("ready"));
    QCOMPARE(snapshotsOnHub(), 0);
    QVERIFY(!text.contains(QStringLiteral("Core changed")));
    const SyncState st = SaveStore::loadState(gdir());
    QCOMPARE(st.writerCoreId, QStringLiteral("melondsds"));
    QCOMPARE(st.writerCoreVersion, QStringLiteral("2026.10.09"));
    // Same core and version again: nothing happens.
    QCOMPARE(startWithCore(QStringLiteral("melondsds"), QStringLiteral("2026.10.09")), QStringLiteral("ready"));
    QCOMPARE(snapshotsOnHub(), 0);
  }

  void coreChangeSnapshotsTheHubSaveFirstAndNotifies() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(startWithCore(QStringLiteral("melondsds"), QStringLiteral("2026.10.09")), QStringLiteral("ready"));
    QString text;
    QCOMPARE(startWithCore(QStringLiteral("desmume"), QStringLiteral("2026.10.08"), &text), QStringLiteral("ready"));
    QCOMPARE(snapshotsOnHub(), 1);
    const FakeVersion snap = hub_->saves.value(kGame).history.last();
    QCOMPARE(snap.label, QString::fromUtf8("Before core change: melondsds 2026.10.09 \xe2\x86\x92 desmume 2026.10.08"));
    QCOMPARE(snap.content, QByteArray("hub-1"));
    QVERIFY2(text.contains(QStringLiteral("melondsds 2026.10.09")) && text.contains(QStringLiteral("desmume 2026.10.08")), qPrintable(text));
    QCOMPARE(SaveStore::loadState(gdir()).writerCoreId, QStringLiteral("desmume"));
    // A local copy of the save is kept as well.
    QVERIFY(!QDir(gdir()).entryList({QStringLiteral("rom1.sav.core-change-*.bak")}).isEmpty());
    // Back to the first core: another change, another snapshot.
    QCOMPARE(startWithCore(QStringLiteral("melondsds"), QStringLiteral("2026.10.09")), QStringLiteral("ready"));
    QCOMPARE(snapshotsOnHub(), 2);
  }

  void newBuildOfTheSameCoreIsAChange() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(startWithCore(QStringLiteral("my-core"), QStringLiteral("2026.10.09")), QStringLiteral("ready"));
    QCOMPARE(startWithCore(QStringLiteral("my-core"), QStringLiteral("2026.10.10")), QStringLiteral("ready"));
    QCOMPARE(snapshotsOnHub(), 1);
    QVERIFY(hub_->saves.value(kGame).history.last().label.contains(QStringLiteral("my-core 2026.10.09")));
  }

  void failedSnapshotBlocksTheStartAndKeepsTheRecord() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(startWithCore(QStringLiteral("melondsds"), QStringLiteral("2026.10.09")), QStringLiteral("ready"));
    hub_->failSaveRequests = 1;  // the snapshot request answers 503
    QString text;
    QCOMPARE(startWithCore(QStringLiteral("desmume"), QStringLiteral("2026.10.08"), &text), QStringLiteral("failed"));
    QVERIFY2(text.contains(QStringLiteral("not started")) && text.contains(QStringLiteral("melondsds")), qPrintable(text));
    QCOMPARE(snapshotsOnHub(), 0);
    QCOMPARE(SaveStore::loadState(gdir()).writerCoreId, QStringLiteral("melondsds"));  // unchanged: the old core stays the writer
    QCOMPARE(hub_->saves.value(kGame).content, QByteArray("hub-1"));
    // Next try works.
    QCOMPARE(startWithCore(QStringLiteral("desmume"), QStringLiteral("2026.10.08")), QStringLiteral("ready"));
    QCOMPARE(snapshotsOnHub(), 1);
  }

  void offlineHubBlocksACoreChangeWithALocalSave() {
    hub_->setHubSave(kGame, "hub-1");
    QCOMPARE(startWithCore(QStringLiteral("melondsds"), QStringLiteral("2026.10.09")), QStringLiteral("ready"));
    QVERIFY(QFileInfo::exists(saveFile()));
    hub_.reset();  // Hub gone
    QString text;
    QCOMPARE(startWithCore(QStringLiteral("desmume"), QStringLiteral("2026.10.08"), &text), QStringLiteral("failed"));
    QVERIFY2(text.contains(QStringLiteral("not reachable")) && text.contains(QStringLiteral("not started")), qPrintable(text));
    QCOMPARE(SaveStore::loadState(gdir()).writerCoreId, QStringLiteral("melondsds"));
    // Same core while offline is fine.
    QCOMPARE(startWithCore(QStringLiteral("melondsds"), QStringLiteral("2026.10.09")), QStringLiteral("ready"));
  }

  void noSaveOnTheHubDoesNotBlockACoreChange() {
    // Local save only (never uploaded): there is nothing to snapshot, the start sync uploads the local save first.
    writeFile(saveFile(), "local-1");
    hub_->failSaveRequests = 0;
    QCOMPARE(startWithCore(QStringLiteral("melondsds"), QStringLiteral("2026.10.09")), QStringLiteral("ready"));
    QCOMPARE(startWithCore(QStringLiteral("desmume"), QStringLiteral("2026.10.08")), QStringLiteral("ready"));
    QCOMPARE(SaveStore::loadState(gdir()).writerCoreId, QStringLiteral("desmume"));
    QVERIFY(hub_->saves.contains(kGame));  // uploaded by the start sync
  }

  void snapshotLabelStaysWithinTheHubLimit() {
    const SaveSync::CoreRef a{QStringLiteral("a-very-long-core-identifier-number-one"), QStringLiteral("2026.10.09.12")};
    const SaveSync::CoreRef b{QStringLiteral("another-very-long-core-identifier-two"), QStringLiteral("2026.10.10.3")};
    const QString label = SaveSync::coreChangeLabel(a, b);
    QVERIFY(label.toUtf8().size() <= 64);
    QVERIFY(label.startsWith(QStringLiteral("Before core change: a-very-long")));
    QCOMPARE(SaveSync::coreChangeLabel({QStringLiteral("x"), QStringLiteral("1")}, {QStringLiteral("y"), QStringLiteral("2")}),
             QString::fromUtf8("Before core change: x 1 \xe2\x86\x92 y 2"));
  }

  // ---------------------------------------------------------------- ADR 0020 D7: DeSmuME core file (.dsv) around the raw save

  static QByteArray dsRaw(char fill) { return QByteArray(0x2000, fill); }  // 64 kbit EEPROM sized dummy save
  static SaveSync::CoreRef desmumeRef() {
    SaveSync::CoreRef r{QStringLiteral("desmume"), QStringLiteral("2026.10.08")};
    r.saveSource = QStringLiteral("core_file");
    r.fileExtension = QStringLiteral(".dsv");
    r.fileFormat = QStringLiteral("desmume_dsv");
    return r;
  }
  static SaveSync::CoreRef melonRef() {
    SaveSync::CoreRef r{QStringLiteral("melondsds"), QStringLiteral("2026.10.09")};
    r.saveSource = QStringLiteral("save_ram");
    r.fileExtension = QStringLiteral(".sav");
    return r;
  }
  QString dsvFile() const { return gdir() + QStringLiteral("/rom1.dsv"); }
  QString startWith(const SaveSync::CoreRef& core, QString* text = nullptr) {
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    QSignalSpy failed(sync_.get(), &SaveSync::startFailed);
    sync_->prepareStart(kGame, kRom, {QStringLiteral("rom1")}, core);
    (void)QTest::qWaitFor([&]() { return ready.count() + failed.count() > 0; }, 8000);
    if (text != nullptr) *text = failed.count() > 0 ? failed.at(0).at(1).toString() : QString();
    return failed.count() > 0 ? QStringLiteral("failed") : (ready.count() > 0 ? QStringLiteral("ready") : QString());
  }

  void desmumeStartWritesTheHubSaveAsDsv() {
    hub_->setHubSave(kGame, dsRaw('a'));
    QCOMPARE(startWith(desmumeRef()), QStringLiteral("ready"));
    QCOMPARE(readFile(saveFile()), dsRaw('a'));  // canonical raw save
    const auto back = dsv::dsvToRaw(readFile(dsvFile()));
    QVERIFY(back.has_value());
    QCOMPARE(*back, dsRaw('a'));  // what DeSmuME loads
    QCOMPARE(puts(), 0);
  }

  void desmumeDsvChangesAreUploadedAsRawNeverAsDsv() {
    hub_->setHubSave(kGame, dsRaw('a'));
    QCOMPARE(startWith(desmumeRef()), QStringLiteral("ready"));
    sync_->beginSession();
    writeFile(dsvFile(), *dsv::rawToDsv(dsRaw('b')));  // the fake core saves
    QVERIFY(sync_->finalSyncBlocking(true));
    QCOMPARE(hub_->saves.value(kGame).content, dsRaw('b'));  // raw, no footer
    QCOMPARE(readFile(saveFile()), dsRaw('b'));
    QCOMPARE(SaveStore::loadState(gdir()).lastSyncedSha256, SaveStore::sha256Of(dsRaw('b')));
  }

  void desmumeShortDirHoldsTheDsvAndAdoptsTheOldOne() {
    auto ref = desmumeRef();
    ref.shortDir = true;
    const QString shortDir = SaveStore::shortCoreDir(*profiles_, kHubId, kUser, kGame, QStringLiteral("default"));
    QVERIFY(!shortDir.isEmpty());
    // a .dsv the core wrote to the long directory before this version
    writeFile(dsvFile(), *dsv::rawToDsv(dsRaw('z')));
    QSignalSpy ready(sync_.get(), &SaveSync::startReady);
    sync_->prepareStart(kGame, kRom, {QStringLiteral("rom1")}, ref);
    QVERIFY(QTest::qWaitFor([&]() { return ready.count() > 0; }, 8000));
    QCOMPARE(ready.at(0).at(1).toString(), shortDir);  // the core gets the short dir
    QVERIFY(!QFileInfo::exists(dsvFile()));            // adopted once
    QVERIFY(QFileInfo::exists(shortDir + QStringLiteral("/rom1.dsv")));
    QCOMPARE(readFile(saveFile()), dsRaw('z'));  // the progress of the old .dsv was imported, not lost
    sync_->beginSession();
    writeFile(shortDir + QStringLiteral("/rom1.dsv"), *dsv::rawToDsv(dsRaw('b')));  // the fake core saves
    QVERIFY(sync_->finalSyncBlocking(true));
    QCOMPARE(hub_->saves.value(kGame).content, dsRaw('b'));
  }

  void changesFromACrashedSessionAreImportedAtTheNextStart() {
    hub_->setHubSave(kGame, dsRaw('a'));
    QCOMPARE(startWith(desmumeRef()), QStringLiteral("ready"));
    writeFile(dsvFile(), *dsv::rawToDsv(dsRaw('c')));  // progress that was never synced (app crashed)
    QCOMPARE(startWith(desmumeRef()), QStringLiteral("ready"));
    QCOMPARE(hub_->saves.value(kGame).content, dsRaw('c'));
    QVERIFY(!QDir(gdir()).entryList({QStringLiteral("rom1.sav.core-file-*.bak")}).isEmpty());  // the old raw save is kept
  }

  void unparsableDsvIsNeverUploadedAndNeverLost() {
    hub_->setHubSave(kGame, dsRaw('a'));
    QCOMPARE(startWith(desmumeRef()), QStringLiteral("ready"));
    sync_->beginSession();
    QSignalSpy problem(sync_.get(), &SaveSync::coreSaveProblem);
    const QByteArray junk = "this is not a DeSmuME save";
    writeFile(dsvFile(), junk);
    sync_->finalSyncBlocking(true);
    QCOMPARE(problem.count(), 1);
    QCOMPARE(hub_->saves.value(kGame).content, dsRaw('a'));  // Hub unchanged
    QCOMPARE(readFile(dsvFile()), junk);                       // kept locally untouched
    QCOMPARE(readFile(saveFile()), dsRaw('a'));
    // The next start refuses to run over it.
    QString text;
    QCOMPARE(startWith(desmumeRef(), &text), QStringLiteral("failed"));
    QVERIFY2(text.contains(QStringLiteral("untouched")), qPrintable(text));
    QCOMPARE(readFile(dsvFile()), junk);
  }

  void bothFormatsInOneDirectoryNeverFlipTheHubSlot() {
    hub_->setHubSave(kGame, dsRaw('a'));
    QCOMPARE(startWith(desmumeRef()), QStringLiteral("ready"));
    sync_->beginSession();
    writeFile(dsvFile(), *dsv::rawToDsv(dsRaw('b')));
    QVERIFY(sync_->finalSyncBlocking(true));
    // Core change to a SAVE_RAM core: .sav (raw) is the save, the stale .dsv next to it is ignored.
    QCOMPARE(startWith(melonRef()), QStringLiteral("ready"));
    QVERIFY(QFileInfo::exists(dsvFile()));
    QCOMPARE(hub_->saves.value(kGame).content, dsRaw('b'));
    sync_->beginSession();
    writeFile(saveFile(), dsRaw('m'));  // melonDS DS persists its SAVE_RAM
    QVERIFY(sync_->finalSyncBlocking(true));
    QCOMPARE(hub_->saves.value(kGame).content, dsRaw('m'));
    // Back to DeSmuME: the .dsv is rebuilt from the raw save, the older .dsv content does not come back.
    QCOMPARE(startWith(desmumeRef()), QStringLiteral("ready"));
    QCOMPARE(*dsv::dsvToRaw(readFile(dsvFile())), dsRaw('m'));
    QCOMPARE(hub_->saves.value(kGame).content, dsRaw('m'));
    QCOMPARE(snapshotsOnHub(), 2);  // each core change was snapshotted
  }

  void profiledCoreSyncsExactlyItsFileNotTheNewest() {
    // A foreign newer file in the directory is not picked up by a profiled core.
    writeFile(gdir() + QStringLiteral("/other.bin"), "foreign");
    hub_->setHubSave(kGame, dsRaw('a'));
    QCOMPARE(startWith(melonRef()), QStringLiteral("ready"));
    QCOMPARE(readFile(saveFile()), dsRaw('a'));
    QCOMPARE(puts(), 0);
  }

  // ---------------------------------------------------------------- ADR 0020 D7: Azahar SD tree (core_tree) <-> tar archive as raw save

  static SaveSync::CoreRef azaharRef() {
    SaveSync::CoreRef r{QStringLiteral("azahar"), QStringLiteral("2026.10.01")};
    r.saveSource = QStringLiteral("core_tree");
    r.fileExtension = QStringLiteral(".sav");
    r.fileFormat = QStringLiteral("azahar_sdmc");
    r.shortDir = true;
    r.subfolder = QStringLiteral("Azahar");
    return r;
  }
  QString treeRoot() const {
    return SaveStore::shortCoreDir(*profiles_, kHubId, kUser, kGame, QStringLiteral("default")) + QStringLiteral("/Azahar");
  }
  static QString titleData(const QString& file) {
    return QStringLiteral("/sdmc/Nintendo 3DS/0123456789ABCDEF0123456789ABCDEF/FEDCBA9876543210FEDCBA9876543210/title/00040000/00123400/data/00000001/") + file;
  }
  static void writeTree(const QString& root, const QByteArray& main) {
    writeFile(root + titleData(QStringLiteral("main")), main);
    writeFile(root + titleData(QStringLiteral(".metadata")), "meta");
    writeFile(root + QStringLiteral("/nand/data/system.bin"), "nand-local");
  }

  void azaharExistingTreeIsUploadedAtStart() {
    writeTree(treeRoot(), "tree-a");
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));
    const QByteArray tar = *sdtree::pack(treeRoot());
    QVERIFY(!tar.isEmpty());
    QCOMPARE(readFile(saveFile()), tar);
    QCOMPARE(hub_->saves.value(kGame).content, tar);  // Hub bytes == sdtree::pack
    QCOMPARE(readFile(treeRoot() + titleData(QStringLiteral("main"))), QByteArray("tree-a"));
    QCOMPARE(readFile(treeRoot() + QStringLiteral("/nand/data/system.bin")), QByteArray("nand-local"));
  }

  void azaharTreeFromTheLongDirectoryIsAdopted() {
    writeTree(gdir() + QStringLiteral("/Azahar"), "old-tree");  // written by the local-only mode
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));
    QVERIFY(!QFileInfo::exists(gdir() + QStringLiteral("/Azahar")));
    QCOMPARE(readFile(treeRoot() + titleData(QStringLiteral("main"))), QByteArray("old-tree"));
    QCOMPARE(hub_->saves.value(kGame).content, *sdtree::pack(treeRoot()));
  }

  void azaharNewerHubSaveReplacesTheTreeAfterABackup() {
    writeTree(treeRoot(), "tree-a");
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));  // uploaded: tree and Hub in sync
    QTemporaryDir other;
    writeTree(other.path(), "from-the-hub");
    const QByteArray hubTar = *sdtree::pack(other.path());
    const QByteArray localTar = *sdtree::pack(treeRoot());
    hub_->setHubSave(kGame, hubTar);
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));
    QCOMPARE(readFile(treeRoot() + titleData(QStringLiteral("main"))), QByteArray("from-the-hub"));
    QCOMPARE(readFile(treeRoot() + QStringLiteral("/nand/data/system.bin")), QByteArray("nand-local"));  // never touched
    const QStringList baks = QDir(gdir()).entryList({QStringLiteral("rom1.sav.tree-*.bak")});
    QCOMPARE(baks.size(), 1);
    QCOMPARE(readFile(gdir() + QLatin1Char('/') + baks.at(0)), localTar);  // what the tree held before
    QCOMPARE(readFile(saveFile()), hubTar);
    // restarting changes nothing: no upload, no second backup
    const int before = puts();
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));
    QCOMPARE(puts(), before);
    QCOMPARE(QDir(gdir()).entryList({QStringLiteral("rom1.sav.tree-*.bak")}).size(), 1);
  }

  void azaharTreeChangesDuringTheSessionAreUploaded() {
    writeTree(treeRoot(), "tree-a");
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));
    sync_->beginSession();
    QVERIFY(!sync_->liveApplyAvailable(kGame, QStringLiteral("default")));
    writeFile(treeRoot() + titleData(QStringLiteral("main")), "tree-b-longer");  // the core saves
    QTRY_COMPARE_WITH_TIMEOUT(hub_->saves.value(kGame).content, *sdtree::pack(treeRoot()), 8000);  // found by the fingerprint poll
    writeFile(treeRoot() + titleData(QStringLiteral("extra")), "more");
    QVERIFY(sync_->finalSyncBlocking(true));
    QCOMPARE(hub_->saves.value(kGame).content, *sdtree::pack(treeRoot()));
    QCOMPARE(SaveStore::loadState(gdir()).lastSyncedSha256, SaveStore::sha256Of(hub_->saves.value(kGame).content));
  }

  void azaharInvalidHubBlobFailsTheStartAndLeavesTheTree() {
    writeTree(treeRoot(), "tree-a");
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));
    const QByteArray before = *sdtree::pack(treeRoot());
    hub_->setHubSave(kGame, QByteArray("this is not a tar archive"));
    QString text;
    QCOMPARE(startWith(azaharRef(), &text), QStringLiteral("failed"));
    QVERIFY2(text.contains(QStringLiteral("cannot be prepared")), qPrintable(text));
    QCOMPARE(*sdtree::pack(treeRoot()), before);
    QCOMPARE(readFile(treeRoot() + QStringLiteral("/nand/data/system.bin")), QByteArray("nand-local"));
  }

  void azaharWithoutAnySaveStartsFresh() {
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));
    QVERIFY(!QFileInfo::exists(saveFile()));
    QCOMPARE(puts(), 0);
  }

 private:
  struct TreeHooks {
    int restarts = 0, flushes = 0;
  };
  void installTreeHooks(TreeHooks* th) {
    SaveSync::LiveHooks h;
    h.flush = [th]() { ++th->flushes; };
    h.restartWith = [th](std::function<bool()> writer) {  // stop, write, start: the fake core owns no files
      ++th->restarts;
      return writer();
    };
    sync_->setLiveHooks(std::move(h));
  }
  SaveSync::RestoreResult uploadLive(const QString& file) {
    SaveSync::RestoreResult res;
    bool done = false;
    sync_->uploadSaveFile(kGame, QStringLiteral("default"), file, SaveStore::loadState(gdir()).baseRevision, QStringLiteral("rom1.sav"),
                          [&](const SaveSync::RestoreResult& r) { res = r; done = true; });
    (void)QTest::qWaitFor([&]() { return done; }, 8000);
    return res;
  }

 private slots:
  void azaharLiveUploadRestartsTheGameWithTheNewTree() {
    writeTree(treeRoot(), "tree-a");
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));
    sync_->beginSession();
    TreeHooks th;
    installTreeHooks(&th);
    QVERIFY(sync_->liveApplyAvailable(kGame, QStringLiteral("default")));
    writeFile(treeRoot() + titleData(QStringLiteral("main")), "tree-a-played");  // progress since the last upload
    QTemporaryDir other;
    writeTree(other.path(), "picked-tree");
    const QByteArray pickedTar = *sdtree::pack(other.path());
    const QString picked = QDir(dir_->path()).filePath(QStringLiteral("picked.sav"));
    writeFile(picked, pickedTar);
    const SaveSync::RestoreResult res = uploadLive(picked);
    QVERIFY2(res.ok(), qPrintable(res.message));
    QVERIFY(res.localUpdated);
    QCOMPARE(th.restarts, 1);
    QCOMPARE(readFile(treeRoot() + titleData(QStringLiteral("main"))), QByteArray("picked-tree"));
    QCOMPARE(readFile(treeRoot() + QStringLiteral("/nand/data/system.bin")), QByteArray("nand-local"));
    QCOMPARE(QDir(gdir()).entryList({QStringLiteral("rom1.sav.tree-*.bak")}).size(), 1);
    QCOMPARE(readFile(saveFile()), pickedTar);
    QCOMPARE(hub_->saves.value(kGame).content, pickedTar);
    const int before = puts();
    QTest::qWait(400);  // several polls: the restored save is not a new change
    QCOMPARE(puts(), before);
    QVERIFY(sync_->finalSyncBlocking(true));
    QCOMPARE(puts(), before);
    QCOMPARE(hub_->saves.value(kGame).content, pickedTar);
  }

  void azaharLiveUploadOfAnInvalidArchiveIsRefused() {
    writeTree(treeRoot(), "tree-a");
    QCOMPARE(startWith(azaharRef()), QStringLiteral("ready"));
    sync_->beginSession();
    TreeHooks th;
    installTreeHooks(&th);
    const QByteArray before = *sdtree::pack(treeRoot());
    const QString junk = QDir(dir_->path()).filePath(QStringLiteral("junk.sav"));
    writeFile(junk, "not a tar archive, just some bytes");
    const SaveSync::RestoreResult res = uploadLive(junk);
    QVERIFY(!res.ok());
    QCOMPARE(th.restarts, 0);
    QCOMPARE(*sdtree::pack(treeRoot()), before);
    QCOMPARE(hub_->saves.value(kGame).content, before);
    QVERIFY(QDir(gdir()).entryList({QStringLiteral("rom1.sav.tree-*.bak")}).isEmpty());
  }
};

QTEST_GUILESS_MAIN(SaveSyncTest)
#include "savesync_test.moc"
