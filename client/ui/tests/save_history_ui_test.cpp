// Save history, restore, snapshots and slot picker in the Library detail pane (ADR 0012 D7) against the fake Hub.
// No emulator core needed; dummy save bytes only.
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "fakehub.h"
#include "savestore.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
const QString kHubId = QStringLiteral("hub-hist");
QByteArray readFile(const QString& p) {
  QFile f(p);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
}  // namespace

class SaveHistoryUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    QVERIFY(h.openGame());   // every test here works on the game page of the one game
  }
  // Play is off while a restore / delete / upload confirmation is open (decision af): the game page's `confirming`, and the
  // button is disabled in any case (these tests run without a core, so Play is never enabled).
  static bool playBlockedByConfirmation(Harness& h) {
    QQuickItem* pane = h.item("gamePage");
    return pane != nullptr && pane->property("confirming").toBool() && h.item("playButton") != nullptr && !h.item("playButton")->isEnabled();
  }
  static bool confirming(Harness& h) { return h.item("gamePage") != nullptr && h.item("gamePage")->property("confirming").toBool(); }
  // Game page overview -> saves view ("All saves and versions").
  static void openSavesView(Harness& h) {
    QTRY_VERIFY_WITH_TIMEOUT(h.item("manageSavesLink") != nullptr && h.item("manageSavesLink")->isVisible(), 8000);
    QVERIFY(h.click("manageSavesLink"));
    QTRY_VERIFY_WITH_TIMEOUT(h.item("savesView") != nullptr && h.item("savesView")->isVisible(), 4000);
  }
  static void setGames(FakeHub& hub, const QByteArray& rom) {
    hub.hubId = kHubId;
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    hub.games = QJsonObject{{QStringLiteral("games"), QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"),
                                                                                    uitest::sha256Hex(rom), rom.size())}}};
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void historyRestoreSnapshotAndSlots() {
    const QByteArray rom = "homebrew-dummy-rom-hist";
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("saves_v2"), QStringLiteral("sessions_v1")};
    setGames(hub, rom);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "old-content", QStringLiteral("session_end"), QStringLiteral("Boss"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->selectedGameId(), QStringLiteral("g1"), 8000);
    QVERIFY(hist->available());
    QTRY_COMPARE_WITH_TIMEOUT(hist->history().size(), 1, 8000);
    QCOMPARE(hist->history().first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Boss"));
    QVERIFY(hist->restoreBlockReason().isEmpty());
    // Summary block in the overview, then the saves view
    QTRY_VERIFY(h.item("saveSummary") != nullptr && h.item("saveSummary")->isVisible());
    uitest::saveShot(h.window, QStringLiteral("save-summary"));
    openSavesView(h);
    QTRY_VERIFY(h.item("historyRow") != nullptr && h.item("historyRow")->isVisible());
    QVERIFY(h.item("slotTabs") != nullptr && h.item("snapshotButton") != nullptr);
    uitest::saveShot(h.window, QStringLiteral("save-history"));
    for (const char* name : {"slotTabs", "newSlotButton", "historyRow", "restoreButton", "snapshotButton", "historyRefresh", "currentBar"}) {
      QQuickItem* it = h.item(name);
      QVERIFY2(it != nullptr, name);
      const QPointF right = it->mapToItem(h.window->contentItem(), QPointF(it->width(), 0));
      QVERIFY2(right.x() <= h.window->width(), name);
      QVERIFY2(it->width() <= h.item("gamePage")->width() + 0.5, name);  // never wider than the game page
    }

    // Restore needs a confirmation; cancel changes nothing
    QVERIFY(h.click("restoreButton"));
    QTRY_VERIFY(h.item("restoreConfirmBox") != nullptr && h.item("restoreConfirmBox")->isVisible());
    QCOMPARE(hist->restoreRequest().value(QStringLiteral("version")).toInt(), 1);
    QVERIFY(playBlockedByConfirmation(h));  // decision af
    uitest::saveShot(h.window, QStringLiteral("save-restore-confirm"));
    QVERIFY(h.click("restoreCancel"));
    QTRY_VERIFY(hist->restoreRequest().isEmpty());
    QTRY_VERIFY(!confirming(h));
    QCOMPARE(hub.restoreCount, 0);

    // Confirm: Hub restores, the local save becomes the new checkpoint
    QVERIFY(h.click("restoreButton"));
    QTRY_VERIFY(h.item("restoreConfirm") != nullptr && h.item("restoreConfirm")->isVisible());
    QVERIFY(h.click("restoreConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.restoreCount, 1, 8000);
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).content, QByteArray("old-content"));
    const QString dir = SaveStore::gameDir(*h.controller->profileStore(), kHubId, QStringLiteral("u_test_1"), QStringLiteral("g1"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir + QLatin1Char('/') + uitest::sha256Hex(rom) + QStringLiteral(".sav")), 8000);
    QCOMPARE(readFile(dir + QLatin1Char('/') + uitest::sha256Hex(rom) + QStringLiteral(".sav")), QByteArray("old-content"));
    QTRY_VERIFY(hist->message().contains(QStringLiteral("restored")));
    QTRY_COMPARE_WITH_TIMEOUT(hist->history().size(), 2, 8000);  // + "Before restore"
    QCOMPARE(hist->history().first().toMap().value(QStringLiteral("reason")).toString(), QStringLiteral("before_restore"));

    // Stale: the Hub changed behind our back -> 409, list refreshed, user told
    hub.setHubSave(QStringLiteral("g1"), "changed-elsewhere");
    QVERIFY(hist->history().size() == 2);
    hist->requestRestore(1);
    hist->confirmRestore();
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().contains(QStringLiteral("changed on the server")), 8000);
    QVERIFY(hist->messageIsError());
    QCOMPARE(hub.restoreCount, 1);
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).content, QByteArray("changed-elsewhere"));

    // Pending local changes block the restore (button disabled), nothing is overwritten
    QFile f(dir + QLatin1Char('/') + uitest::sha256Hex(rom) + QStringLiteral(".sav"));
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("unsynced");
    f.close();
    hist->refresh();
    QTRY_VERIFY(!hist->restoreBlockReason().isEmpty());
    QTRY_VERIFY(h.item("restoreButton") != nullptr && !h.item("restoreButton")->isEnabled());
    hist->requestRestore(1);
    QVERIFY(hist->restoreRequest().isEmpty());
    QCOMPARE(readFile(f.fileName()), QByteArray("unsynced"));
    f.remove();

    // Snapshot with label from the saves view (inline form)
    hist->refresh();
    QTRY_VERIFY(hist->canSnapshot());
    QVERIFY(h.click("snapshotButton"));
    QTRY_VERIFY(h.item("snapshotLabel") != nullptr && h.item("snapshotLabel")->isVisible());
    QVERIFY(h.item("snapshotLabel")->setProperty("text", QStringLiteral("Before final boss")));
    QVERIFY(h.click("snapshotCreate"));
    QTRY_VERIFY_WITH_TIMEOUT(hub.saves.value(QStringLiteral("g1")).history.last().reason == QLatin1String("manual_snapshot"), 8000);
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).history.last().label, QStringLiteral("Before final boss"));
    QTRY_VERIFY(hist->message().contains(QStringLiteral("Before final boss")));

    // Slot picker: validation, new slot, persisted per game and Hub profile
    QVERIFY(!hist->createSlot(QStringLiteral("Bad Name")).isEmpty());
    QVERIFY(!hist->createSlot(QString()).isEmpty());
    QVERIFY(!hist->createSlot(QString(33, QLatin1Char('a'))).isEmpty());
    QVERIFY(hist->createSlot(QStringLiteral("boss")).isEmpty());
    QCOMPARE(hist->slot(), QStringLiteral("boss"));
    QCOMPARE(h.controller->playerSettings()->saveSlot(kHubId, QStringLiteral("g1")), QStringLiteral("boss"));
    QCOMPARE(h.controller->selectedGame().value(QStringLiteral("saveSlot")).toString(), QStringLiteral("boss"));
    bool hasBoss = false;
    for (const QVariant& o : hist->slotOptions()) {
      hasBoss = hasBoss || o.toMap().value(QStringLiteral("value")).toString() == QLatin1String("boss");
    }
    QVERIFY(hasBoss);
    QTRY_VERIFY(hist->history().isEmpty());  // the boss slot has no versions on the Hub
    QTRY_VERIFY(h.item("historyEmpty") != nullptr && h.item("historyEmpty")->isVisible());
    QTRY_VERIFY(h.item("slotTab_boss") != nullptr && h.item("slotTab_boss")->isVisible());
    // Back to default: existing Hub slots are offered
    hub.setHubSave(QStringLiteral("g1/extra"), "x");
    hist->selectSlot(QStringLiteral("default"));
    QCOMPARE(h.controller->playerSettings()->saveSlot(kHubId, QStringLiteral("g1")), QStringLiteral("default"));
    QTRY_VERIFY_WITH_TIMEOUT(hist->history().size() >= 3, 8000);
  }

  // Regression: a created slot without any version must stay listed after switching to another slot.
  void createdEmptySlotSurvivesSlotSwitch() {
    const QByteArray rom = "homebrew-dummy-rom-newslot";
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("saves_v2"), QStringLiteral("sessions_v1")};
    setGames(hub, rom);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
    const auto listed = [hist](const QString& name) {
      for (const QVariant& o : hist->slotOptions()) {
        if (o.toMap().value(QStringLiteral("value")).toString() == name) {
          return true;
        }
      }
      return false;
    };
    QVERIFY(hist->createSlot(QStringLiteral("boss")).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!hist->loading(), 8000);
    QVERIFY(listed(QStringLiteral("boss")));
    hist->selectSlot(QStringLiteral("default"));
    QTRY_VERIFY_WITH_TIMEOUT(!hist->loading(), 8000);
    QCOMPARE(hist->slot(), QStringLiteral("default"));
    QVERIFY(listed(QStringLiteral("boss")));
    hist->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!hist->loading(), 8000);
    QVERIFY(listed(QStringLiteral("boss")));
  }

  void saveUpdatedPushRefreshesHistory() {
    const QByteArray rom = "homebrew-dummy-rom-push";
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("saves_v2"), QStringLiteral("sessions_v1")};
    setGames(hub, rom);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
    QVERIFY(hist->history().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(hub.wsOpen() > 0, 8000);
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "v", QStringLiteral("session_end"));
    // server admin: nil device id, unknown reason string -> accepted, history refreshed, no notice (game not running)
    hub.sendWs(QStringLiteral("save_updated"),
               QJsonObject{{QStringLiteral("game_id"), QStringLiteral("g1")},
                           {QStringLiteral("slot"), QStringLiteral("default")},
                           {QStringLiteral("revision"), 2},
                           {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
                           {QStringLiteral("device_id"), QStringLiteral("00000000-0000-0000-0000-000000000000")},
                           {QStringLiteral("device_name"), QStringLiteral("Server admin")},
                           {QStringLiteral("reason"), QStringLiteral("conflict_resolution")}});
    QTRY_COMPARE_WITH_TIMEOUT(hist->history().size(), 1, 8000);
    QVERIFY(hist->notice().isEmpty());
  }

  void uploadSaveFileNeedsSavesV4AndConfirms() {
    const QByteArray rom = "homebrew-dummy-rom-up";
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString picked = tmp.filePath(QStringLiteral("picked.srm"));
    QFile pf(picked);
    QVERIFY(pf.open(QIODevice::WriteOnly));
    pf.write("uploaded-bytes");
    pf.close();
    const QString emptyFile = tmp.filePath(QStringLiteral("empty.sav"));
    QFile ef(emptyFile);
    QVERIFY(ef.open(QIODevice::WriteOnly));
    ef.close();

    // saves_v2 only: no upload option
    {
      FakeHub hub(QStringLiteral("a"));
      hub.features = {QStringLiteral("saves_v1"), QStringLiteral("saves_v2"), QStringLiteral("sessions_v1")};
      setGames(hub, rom);
      hub.setHubSave(QStringLiteral("g1"), "cp-1");
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      SaveHistoryController* hist = h.controller->saveHistory();
      QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
      QVERIFY(!hist->canUploadFile());
      openSavesView(h);
      QVERIFY(h.item("uploadSaveButton") == nullptr || !h.item("uploadSaveButton")->isVisible());
      hist->requestUploadFile(picked);
      QVERIFY(hist->uploadRequest().isEmpty());
    }

    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("saves_v2"), QStringLiteral("saves_v4"), QStringLiteral("sessions_v1")};
    setGames(hub, rom);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->selectedGameId(), QStringLiteral("g1"), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(hist->canUploadFile() && !hist->loading(), 8000);
    openSavesView(h);
    QTRY_VERIFY(h.item("uploadSaveButton") != nullptr && h.item("uploadSaveButton")->isVisible());
    QVERIFY(h.item("uploadSaveButton")->isEnabled());

    // Empty file is refused locally
    hist->requestUploadFile(emptyFile);
    QVERIFY(hist->uploadRequest().isEmpty());
    QVERIFY(hist->messageIsError());

    // Request -> cancel changes nothing
    hist->requestUploadFile(QUrl::fromLocalFile(picked).toString());
    QTRY_VERIFY(h.item("uploadConfirmBox") != nullptr && h.item("uploadConfirmBox")->isVisible());
    QCOMPARE(hist->uploadRequest().value(QStringLiteral("fileName")).toString(), QStringLiteral("picked.srm"));
    QCOMPARE(hist->uploadRequest().value(QStringLiteral("gameTitle")).toString(), QStringLiteral("Lumen Drift"));
    QVERIFY(playBlockedByConfirmation(h));  // decision af
    uitest::saveShot(h.window, QStringLiteral("save-upload-confirm"));
    QVERIFY(h.click("uploadCancel"));
    QTRY_VERIFY(hist->uploadRequest().isEmpty());
    QTRY_VERIFY(!confirming(h));
    QCOMPARE(hub.uploadCount, 0);

    // Confirm: Hub takes the file, local save follows
    hist->requestUploadFile(picked);
    QTRY_VERIFY(h.item("uploadConfirm") != nullptr && h.item("uploadConfirm")->isVisible());
    QVERIFY(h.click("uploadConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.uploadCount, 1, 8000);
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).content, QByteArray("uploaded-bytes"));
    const QString dir = SaveStore::gameDir(*h.controller->profileStore(), kHubId, QStringLiteral("u_test_1"), QStringLiteral("g1"));
    QTRY_VERIFY_WITH_TIMEOUT(readFile(dir + QLatin1Char('/') + uitest::sha256Hex(rom) + QStringLiteral(".sav")) == QByteArray("uploaded-bytes"), 8000);
    QTRY_VERIFY(hist->message().contains(QStringLiteral("uploaded")));
    QVERIFY(!hist->messageIsError());
    QTRY_VERIFY_WITH_TIMEOUT(!hist->history().isEmpty(), 8000);
    QCOMPARE(hist->history().first().toMap().value(QStringLiteral("reason")).toString(), QStringLiteral("before_upload"));

    // Stale: the Hub changed behind our back -> error, nothing uploaded
    hub.setHubSave(QStringLiteral("g1"), "changed-elsewhere");
    hist->requestUploadFile(picked);
    hist->confirmUploadFile();
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().contains(QStringLiteral("changed on the server")), 8000);
    QVERIFY(hist->messageIsError());
    QCOMPARE(hub.uploadCount, 1);
  }

  void noSavesV2HidesHistoryButKeepsSlots() {
    const QByteArray rom = "homebrew-dummy-rom-v1";
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1")};
    setGames(hub, rom);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->selectedGameId(), QStringLiteral("g1"), 8000);
    QVERIFY(hist->slotsAvailable());
    QVERIFY(!hist->available());
    QVERIFY(!hist->canSnapshot());
    openSavesView(h);
    QTRY_VERIFY(h.item("slotTabs") != nullptr && h.item("slotTabs")->isVisible());
    for (const char* name : {"timeline", "historyHeader", "saveActions"}) {
      for (QQuickItem* it : h.items(name)) {
        QVERIFY2(!it->isVisible(), name);
      }
    }
    QVERIFY(h.item("historyUnsupported") != nullptr && h.item("historyUnsupported")->isVisible());
    hist->requestRestore(1);  // no-op without saves_v2
    QVERIFY(hist->restoreRequest().isEmpty());
    QCOMPARE(hub.count(QStringLiteral("/api/v1/games/g1/saves/default/history")), 0);
  }
  // #407 / #406: the slot meta (current checkpoint, history) belongs to the slot that is selected. A slot switched by the run start
  // (SaveSync::setSlot, not the picker) must not keep showing the previous slot's meta, and slots keep their own checkpoints.
  void slotMetaFollowsSelectedSlot() {
    const QByteArray rom = "homebrew-dummy-rom-slotmeta";
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("saves_v2"), QStringLiteral("sessions_v1")};
    setGames(hub, rom);
    hub.setHubSave(QStringLiteral("g1"), "cp-main");
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "old-main", QStringLiteral("session_end"));
    hub.setHubSave(QStringLiteral("g1/run-b"), "cp-b");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    SaveSync* sync = h.controller->saveSync();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->history().size() == 1, 8000);
    const QString mainSha = hist->current().value(QStringLiteral("sha256")).toString();
    QVERIFY(!mainSha.isEmpty());

    // Empty run slot selected like a run start does: nothing of Main may remain visible, not even for a moment.
    QVERIFY(sync->setSlot(QStringLiteral("g1"), QStringLiteral("run-new")));
    QVERIFY(hist->history().isEmpty());
    QVERIFY(hist->current().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!hist->loading(), 8000);
    QVERIFY(hist->history().isEmpty());
    QVERIFY(hist->current().isEmpty());

    // Another slot with its own save shows its own checkpoint, not the one of the slot before.
    QVERIFY(sync->setSlot(QStringLiteral("g1"), QStringLiteral("run-b")));
    QVERIFY(hist->current().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!hist->current().isEmpty(), 8000);
    const QString bSha = hist->current().value(QStringLiteral("sha256")).toString();
    QVERIFY(bSha != mainSha);

    // Back to Main: Main's own checkpoint again.
    QVERIFY(sync->setSlot(QStringLiteral("g1"), QStringLiteral("default")));
    QTRY_COMPARE_WITH_TIMEOUT(hist->current().value(QStringLiteral("sha256")).toString(), mainSha, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(hist->history().size(), 1, 8000);  // the history arrives in its own answer, after the checkpoint
  }
};

UITEST_MAIN(SaveHistoryUiTest)
#include "save_history_ui_test.moc"
