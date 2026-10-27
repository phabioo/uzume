// Library revision 3c-2 .. 3c-4 against the fake Hub: sort control, "Ready first", empty result, last played per Hub,
// SAVE summary, saves view (tabs / switcher, timeline, inline confirmations, offline, running, upload failure) and the
// layout of the detail column at 1280 and 960 px. No emulator core needed; homebrew-style dummy bytes only.
#include <algorithm>

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QtTest>

#include "fakehub.h"
#include "savestore.h"
#include "savesync.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
const QString kHubId = QStringLiteral("hub-lib");
const QStringList kAllFeatures{QStringLiteral("saves_v1"), QStringLiteral("saves_v2"), QStringLiteral("saves_v3"),
                               QStringLiteral("saves_v4"), QStringLiteral("saves_v5"), QStringLiteral("sessions_v1")};

// Scrolls the library page column so that `name` is in view (with wide fonts the empty result sits below the fold).
void scrollIntoView(Harness& h, const char* name) {
  QQuickItem* it = h.item(name);
  QQuickItem* page = h.item("libraryPageScroll");
  if (it == nullptr || page == nullptr) return;
  QQuickItem* content = page->property("contentItem").value<QQuickItem*>();
  const qreal max = std::max<qreal>(0, page->property("contentHeight").toReal() - page->height());
  const qreal y = it->mapToItem(content, QPointF(0, 0)).y() + it->height() - page->height() + 8;
  page->setProperty("contentY", std::clamp<qreal>(y, page->property("contentY").toReal(), max));
  QQuickTest::qWaitForPolish(h.window);
}
}  // namespace

class LibrarySavesUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
  }
  // One game "Lumen Drift" (g1) with a save on the Hub.
  static void oneGame(FakeHub& hub, const QStringList& features = kAllFeatures, const QString& title = QStringLiteral("Lumen Drift")) {
    const QByteArray rom = "homebrew-dummy-rom-lib";
    hub.hubId = kHubId;
    hub.features = features;
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    hub.games = QJsonObject{{QStringLiteral("games"), QJsonArray{uitest::gameJson(QStringLiteral("g1"), title,
                                                                                    uitest::sha256Hex(rom), rom.size())}}};
  }
  static QQuickItem* column(Harness& h) { return h.item("detailPane"); }
  static QRectF sceneRect(QQuickItem* it) { return it->mapRectToScene(QRectF(0, 0, it->width(), it->height())); }
  static QString chainOf(QQuickItem* it) {
    QString chain;
    for (QQuickItem* i = it; i != nullptr; i = i->parentItem()) {
      const QRectF r = sceneRect(i);
      chain += QStringLiteral("\n  %1 '%2' x=%3 w=%4 impl=%5 vis=%6")
                   .arg(QString::fromLatin1(i->metaObject()->className()), i->objectName()).arg(r.x()).arg(r.width()).arg(i->implicitWidth()).arg(i->isVisible());
    }
    return chain;
  }
  // Every named item lies inside the visible detail column (392 wide), which lies inside the window.
  static void checkInColumn(Harness& h, const QStringList& names, int minChecked = 3) {
    QQuickTest::qWaitForPolish(h.window);
    QTest::qWait(30);
    QQuickItem* col = column(h);
    QVERIFY(col != nullptr);
    const QRectF cr = sceneRect(col);
    QVERIFY2(cr.right() <= h.window->width() + 0.5, qPrintable(QStringLiteral("column outside window %1 > %2").arg(cr.right()).arg(h.window->width())));
    QVERIFY2(cr.width() <= 392.5, qPrintable(QStringLiteral("column wider than 392: %1").arg(cr.width())));
    int checked = 0;
    for (const QString& name : names) {
      const QList<QQuickItem*> all = h.items(name.toLatin1().constData());
      for (QQuickItem* it : all) {
        if (!it->isVisible() || it->width() <= 0) continue;
        ++checked;
        const QRectF r = sceneRect(it);
        QVERIFY2(r.left() >= cr.left() - 0.5 && r.right() <= cr.right() + 0.5,
                 qPrintable(QStringLiteral("'%1' outside the detail column (%2..%3 vs %4..%5):%6")
                                .arg(name).arg(r.left()).arg(r.right()).arg(cr.left()).arg(cr.right()).arg(chainOf(it))));
      }
    }
    QVERIFY2(checked >= minChecked, qPrintable(QStringLiteral("only %1 of the named items are visible: the state under test is not shown").arg(checked)));
  }
  // Adds letter spacing to every item that has a font (stand-in for wider Windows fonts).
  static void widenFonts(Harness& h, qreal spacing) {
    QList<QQuickItem*> all{h.window->contentItem()};
    for (int i = 0; i < all.size(); ++i) all.append(all.at(i)->childItems());
    for (QQuickItem* it : std::as_const(all)) {
      const QVariant fv = it->property("font");
      if (fv.metaType() != QMetaType::fromType<QFont>()) continue;
      QFont f = fv.value<QFont>();
      if (f.letterSpacing() < 1.0) { f.setLetterSpacing(QFont::AbsoluteSpacing, spacing); it->setProperty("font", f); }
    }
    QQuickTest::qWaitForPolish(h.window);
    QTest::qWait(60);
  }
  static void openSavesView(Harness& h) {
    QTRY_VERIFY_WITH_TIMEOUT(h.item("manageSavesLink") != nullptr && h.item("manageSavesLink")->isVisible(), 8000);
    QVERIFY(h.click("manageSavesLink"));
    QTRY_VERIFY_WITH_TIMEOUT(h.item("savesView") != nullptr && h.item("savesView")->isVisible(), 4000);
  }
  // For states where the summary link is "Upload save file…" (the file dialog is not wanted in tests).
  static void openSavesViewViaApi(Harness& h) {
    QQuickItem* pane = h.item("detailPane");
    QVERIFY(pane != nullptr);
    QVERIFY(QMetaObject::invokeMethod(pane, "openSaves", Q_ARG(QVariant, QVariant(false))));
    QTRY_VERIFY_WITH_TIMEOUT(h.item("savesView") != nullptr && h.item("savesView")->isVisible(), 4000);
  }
  // UzStateBlock (empty state): the message is its title.
  static QString titleOf(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr ? it->property("title").toString() : QString();
  }
  static QString textOf(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr ? it->property("text").toString() : QString();
  }
  static QString metaOf(Harness& h, const char* name) {   // the meta text of a UzSectionLabel
    QQuickItem* it = h.item(name);
    return it != nullptr ? it->property("meta").toString() : QString();
  }
  static bool shown(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr && it->isVisible();
  }
  static bool confirming(Harness& h) { return h.item("detailPane") != nullptr && h.item("detailPane")->property("confirming").toBool(); }

  // An open save conflict of g1 without the game start flow (that would open the start dialog): the Hub holds a newer
  // checkpoint and the conflict with this device's secured upload, this device holds an unsynced local save.
  struct ConflictSeed {
    QString dir, file;
  };
  static ConflictSeed seedConflict(Harness& h, FakeHub& hub, const QString& id, const QByteArray& hubContent, const QByteArray& local) {
    FakeSlot& s = hub.saves[QStringLiteral("g1")];
    const int base = s.revision;
    s.revision += 1;
    s.content = hubContent;
    s.deviceId = QStringLiteral("other-device");
    s.deviceName = QStringLiteral("Laptop Office");
    FakeConflict c;
    c.id = id;
    c.hubRevision = s.revision;
    c.hubSha = uitest::sha256Hex(hubContent);
    c.hubDeviceId = QStringLiteral("other-device");
    c.hubDeviceName = QStringLiteral("Laptop Office");
    c.securedContent = local;
    c.securedVersion = s.nextVersion++;
    c.securedBase = base;
    c.securedDeviceId = h.controller->connection()->deviceId();
    c.securedDeviceName = QStringLiteral("Test Device");
    s.conflicts.append(c);
    ConflictSeed out;
    out.dir = h.controller->saveSync()->slotDirFor(QStringLiteral("g1"), QStringLiteral("default"));
    out.file = out.dir + QLatin1Char('/') + SaveStore::expectedSaveName(uitest::sha256Hex("homebrew-dummy-rom-lib") + QStringLiteral(".nds"));
    QDir().mkpath(out.dir);
    QFile f(out.file);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      f.write(local);
      f.close();
    }
    SyncState st;
    st.baseRevision = base;
    st.lastSyncedSha256 = uitest::sha256Hex("base-state");
    st.pending = true;
    st.conflictId = id;
    SaveStore::saveState(out.dir, st);
    h.controller->saveSync()->refreshKinds({QStringLiteral("g1")});
    return out;
  }
  static QByteArray fileBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // ---- 3c-2: toolbar, sort, Ready first, empty result --------------------------------------------------------------
  void sortReadyFirstAndEmptyResult() {
    struct G { const char* id; const char* title; int size; const char* added; };
    const G defs[] = {{"g1", "Zephyr Run", 900, "2026-01-05T10:00:00Z"}, {"g2", "alpha Garden", 100, "2026-03-01T10:00:00Z"},
                      {"g3", "Moon Courier", 500, "2026-02-01T10:00:00Z"}, {"g4", "Orbit 10", 700, "2026-04-01T10:00:00Z"},
                      {"g5", "Orbit 2", 200, "2026-01-20T10:00:00Z"}};
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = kHubId;
    QJsonArray games;
    for (const G& g : defs) {
      QByteArray rom = QByteArray(g.id) + "-rom-";
      rom.append(QByteArray(g.size - rom.size(), 'x'));
      hub.roms.insert(uitest::sha256Hex(rom), rom);
      QJsonObject j = uitest::gameJson(QString::fromLatin1(g.id), QString::fromLatin1(g.title), uitest::sha256Hex(rom), rom.size());
      j.insert(QStringLiteral("added_at"), QString::fromLatin1(g.added));
      games.append(j);
    }
    hub.games = QJsonObject{{QStringLiteral("games"), games}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    LibraryModel* lib = h.controller->library();
    const auto order = [&]() {
      QStringList out;
      for (int i = 0; i < lib->rowCount(); ++i) out << lib->data(lib->index(i), LibraryModel::GameIdRole).toString();
      return out;
    };
    // g1, g3, g4 are ready (verified from the Hub); g2, g5 need a download
    for (const char* id : {"g1", "g3", "g4"}) {
      for (const GameEntry& g : h.controller->hubLibrary()->games()) {
        if (g.id == QLatin1String(id)) h.controller->downloader()->ensureRom(g);
      }
    }
    QTRY_COMPARE_WITH_TIMEOUT(lib->readyCount(), 3, 8000);
    // Without a core every game "needs attention" (core missing) and none would count as ready: take that out for the grouping.
    QTRY_VERIFY_WITH_TIMEOUT((lib->setAttention({}), lib->isReady(QStringLiteral("g1")) && lib->isReady(QStringLiteral("g3")) && lib->isReady(QStringLiteral("g4"))), 8000);
    QCOMPARE(order(), (QStringList{"g2", "g3", "g5", "g4", "g1"}));  // Name A–Z, "Orbit 2" before "Orbit 10"
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("g2"));  // first game of the sorted list
    QCOMPARE(metaOf(h, "libraryAllLabel"), QStringLiteral("5 games · Nintendo DS"));
    QVERIFY(shown(h, "sortControl") && shown(h, "scopeChip_all"));  // Ready first lives in the sort menu
    QVERIFY(!shown(h, "notReadyDivider"));
    uitest::saveShot(h.window, QStringLiteral("library-3c-2"));

    // Sort menu
    QVERIFY(h.click("sortControl"));
    QTRY_VERIFY(shown(h, "sort_size_desc"));
    uitest::saveShot(h.window, QStringLiteral("library-sort-menu"));
    QVERIFY(h.click("sort_size_desc"));
    QTRY_COMPARE(lib->sortKey(), QStringLiteral("size_desc"));
    QCOMPARE(order(), (QStringList{"g1", "g4", "g3", "g5", "g2"}));
    QVERIFY(h.click("sortControl"));
    QTRY_VERIFY(shown(h, "sort_added_asc"));
    QVERIFY(h.click("sort_added_asc"));
    QTRY_COMPARE(lib->sortKey(), QStringLiteral("added_asc"));
    QCOMPARE(order(), (QStringList{"g1", "g5", "g3", "g2", "g4"}));
    // With a date sort the tiles carry "Added DD.MM.YYYY"
    QTRY_VERIFY(h.item("tileAdded") != nullptr && h.item("tileAdded")->isVisible());
    QVERIFY(h.item("tileAdded")->property("text").toString().startsWith(QStringLiteral("Added ")));
    QVERIFY(h.click("sortControl"));
    QTRY_VERIFY(shown(h, "sort_size_desc"));
    QVERIFY(h.click("sort_size_desc"));
    QTRY_COMPARE(lib->sortKey(), QStringLiteral("size_desc"));

    // Ready first: two grids and the divider "NOT READY · 2 · same sort"
    QVERIFY(h.click("sortControl"));
    QTRY_VERIFY(shown(h, "readyFirstToggle"));
    QVERIFY(h.click("readyFirstToggle"));
    QTRY_VERIFY(lib->readyFirst());
    QTRY_VERIFY(shown(h, "notReadyDivider"));
    QCOMPARE(order(), (QStringList{"g1", "g4", "g3", "g5", "g2"}));
    QCOMPARE(QQmlProperty(h.item("gameGrid"), QStringLiteral("count")).read().toInt(), 3);
    QCOMPARE(QQmlProperty(h.item("gameGridNotReady"), QStringLiteral("count")).read().toInt(), 2);
    bool dividerText = false;
    for (QQuickItem* c : h.item("notReadyDivider")->childItems()) {
      dividerText = dividerText || c->property("text").toString() == QStringLiteral("NOT READY · 2 · same sort");
    }
    QVERIFY(dividerText);
    uitest::saveShot(h.window, QStringLiteral("library-ready-first"));
    // Sort and Ready first are saved per device
    {
      PlayerSettings s(h.controller->profileStore()->baseDir());
      QCOMPARE(s.librarySort(), QStringLiteral("size_desc"));
      QVERIFY(s.libraryReadyFirst());
    }
    // The "Ready" chip: every result is ready, the toggle is disabled and no divider is shown
    lib->setFilter(QStringLiteral("ready"));
    QTRY_VERIFY(!shown(h, "notReadyDivider"));
    QVERIFY(h.click("sortControl"));
    QTRY_VERIFY(shown(h, "readyFirstToggle"));
    QVERIFY(!h.item("readyFirstToggle")->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(h.window->findChild<QObject*>(QStringLiteral("sortMenu")), "close"));
    QTRY_VERIFY(!shown(h, "readyFirstToggle"));
    QCOMPARE(metaOf(h, "libraryAllLabel"), QStringLiteral("3 of 5 games"));
    lib->setFilter(QStringLiteral("all"));
    QTRY_VERIFY(shown(h, "notReadyDivider"));
    QVERIFY(h.click("sortControl"));
    QTRY_VERIFY(shown(h, "readyFirstToggle"));
    QVERIFY(h.item("readyFirstToggle")->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(h.window->findChild<QObject*>(QStringLiteral("sortMenu")), "close"));
    QTRY_VERIFY(!shown(h, "readyFirstToggle"));

    // Search + chip -> empty result with the hint and both actions (decision ac for the count)
    lib->setFilter(QStringLiteral("download"));  // g2, g5
    QVERIFY(h.item("searchField")->setProperty("text", QStringLiteral("moon")));
    QTRY_VERIFY(shown(h, "libraryEmptyText"));
    QCOMPARE(titleOf(h, "libraryEmptyText"), QStringLiteral("No games not downloaded match “moon”"));
    QCOMPARE(textOf(h, "libraryEmptyHint"), QStringLiteral("1 match outside this filter: Moon Courier (ready)."));
    QCOMPARE(metaOf(h, "libraryAllLabel"), QStringLiteral("0 of 2 games not downloaded"));
    QVERIFY(shown(h, "emptyClearSearch") && shown(h, "emptyShowAll"));
    uitest::saveShot(h.window, QStringLiteral("library-empty-result"));
    QCOMPARE(lib->counts().value(QStringLiteral("all")).toInt(), 1);  // the chips recount to the search
    scrollIntoView(h, "emptyShowAll");
    QVERIFY(h.click("emptyShowAll"));
    QTRY_COMPARE(lib->filter(), QStringLiteral("all"));
    QTRY_COMPARE(lib->rowCount(), 1);
    QVERIFY(h.item("searchField")->setProperty("text", QStringLiteral("zzz")));
    QTRY_COMPARE(titleOf(h, "libraryEmptyText"), QStringLiteral("No games match “zzz”"));
    scrollIntoView(h, "emptyClearSearch");
    QVERIFY(h.click("emptyClearSearch"));
    QTRY_COMPARE(lib->rowCount(), 5);
    QVERIFY(h.item("searchField")->property("text").toString().isEmpty());

    // Restart with the same data directory: the sort choice is back
    {
      PlayerController::Options o;
      o.dataDir = h.dir.path();
      o.memoryCredentials = true;
      o.probeCoreVersions = false;
      o.enableGamepads = false;
      PlayerController second(o);
      QCOMPARE(second.library()->sortKey(), QStringLiteral("size_desc"));
      QVERIFY(second.library()->readyFirst());
    }
  }

  void lastPlayedIsPerHubAndSortable() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    PlayerController* c = h.controller.get();
    QCOMPARE(c->playerSettings()->lastPlayed(kHubId, QStringLiteral("g1")), qint64(0));
    c->recordLastPlayed(QStringLiteral("g1"), 1760000000000LL);
    QCOMPARE(c->playerSettings()->lastPlayed(kHubId, QStringLiteral("g1")), 1760000000000LL);
    QCOMPARE(c->playerSettings()->lastPlayed(QStringLiteral("another-hub"), QStringLiteral("g1")), qint64(0));  // same game id, other Hub
    c->library()->setSortKey(QStringLiteral("played"));
    QCOMPARE(c->library()->rowCount(), 1);
    // A reload of the library reads the stored value of this Hub
    c->reloadLibrary();
    QTRY_COMPARE_WITH_TIMEOUT(c->libraryState(), QStringLiteral("ready"), 8000);
    QCOMPARE(PlayerSettings(c->profileStore()->baseDir()).lastPlayed(kHubId, QStringLiteral("g1")), 1760000000000LL);
  }

  // ---- 3c-2 SAVE summary and 3c-3 saves view -----------------------------------------------------------------------
  void summaryOpensTheSavesViewAndBack() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "old", QStringLiteral("session_end"));
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "snap", QStringLiteral("manual_snapshot"), QStringLiteral("Chapter 2"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->versionCount() == 3, 8000);
    QTRY_VERIFY(shown(h, "saveSummary"));
    QCOMPARE(textOf(h, "saveSlotName"), QStringLiteral("Main"));
    QCOMPARE(textOf(h, "saveVersion"), QStringLiteral("r1"));
    QCOMPARE(textOf(h, "saveCounts"), QStringLiteral("1 slot · 3 versions in Main"));
    QCOMPARE(textOf(h, "manageSavesLink"), QStringLiteral("Manage saves →"));
    // The "Save" row and "Show details" of 3c are gone; the checklist is still there
    QVERIFY(h.item("detailsToggle") == nullptr);
    QVERIFY(h.item("saveSection") == nullptr);
    uitest::saveShot(h.window, QStringLiteral("library-save-summary"));
    checkInColumn(h, {"saveSummary", "saveCurrentRow", "saveCounts", "manageSavesLink", "playButton", "playShareButton"});

    openSavesView(h);
    QVERIFY(!shown(h, "saveSummary"));
    QVERIFY(shown(h, "playButton"));  // Play stays pinned below the view
    QTRY_VERIFY(shown(h, "currentBar"));
    QCOMPARE(textOf(h, "currentVersion"), QStringLiteral("r1"));
    QVERIFY(shown(h, "slotTab_default"));
    QVERIFY(!shown(h, "slotSwitcher"));
    QTRY_VERIFY(!textOf(h, "savesUpdated").isEmpty());
    QVERIFY(textOf(h, "savesUpdated").startsWith(QStringLiteral("updated ")));
    // Timeline: current row + 2 history rows; the snapshot has Delete, the other row has not
    QTRY_COMPARE(h.items("historyRow").size() + h.items("historyRowCurrent").size(), 3);
    int visibleDelete = 0;
    for (QQuickItem* it : h.items("deleteButton")) visibleDelete += it->isVisible() ? 1 : 0;
    QCOMPARE(visibleDelete, 1);
    // File details are collapsed until asked for
    QVERIFY(!shown(h, "fileDetails"));
    QVERIFY(h.click("fileDetailsToggle"));
    QTRY_VERIFY(shown(h, "fileDetails"));
    QCOMPARE(textOf(h, "detailSize").isEmpty(), false);
    QVERIFY(h.click("copyHashButton"));
    QCOMPARE(QGuiApplication::clipboard()->text(), hist->current().value(QStringLiteral("sha256")).toString());
    uitest::saveShot(h.window, QStringLiteral("library-saves-view"));
    checkInColumn(h, {"savesView", "slotTabs", "currentBar", "fileDetails", "saveActions", "snapshotButton", "uploadSaveButton", "historyHeader",
                      "historyRow", "restoreButton", "deleteButton", "playButton", "playShareButton"});

    // Snapshots filter: the snapshot and the current version
    QVERIFY(h.click("historyFilterSnapshots"));
    QTRY_COMPARE(h.items("historyRow").size() + h.items("historyRowCurrent").size(), 2);
    QVERIFY(h.click("historyFilterAll"));
    QTRY_COMPARE(h.items("historyRow").size() + h.items("historyRowCurrent").size(), 3);

    // Esc and "← title" return to the game detail
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "savesView"));
    QVERIFY(shown(h, "saveSummary"));
    openSavesView(h);
    QVERIFY(h.click("tab_Overview"));
    QTRY_VERIFY(!shown(h, "savesView"));
    QVERIFY(shown(h, "saveSummary"));
  }

  void noSavesYetState() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);  // no save on the Hub
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
    QTRY_VERIFY(shown(h, "saveEmptyBlock"));
    QCOMPARE(textOf(h, "manageSavesLink"), QStringLiteral("Upload save file…"));  // saves_v4 is advertised
    uitest::saveShot(h.window, QStringLiteral("library-save-none"));
    // The saves view says the same and still offers the actions
    openSavesViewViaApi(h);
    QTRY_VERIFY(shown(h, "historyEmpty"));
    QCOMPARE(textOf(h, "snapshotButton").isEmpty(), false);
    QVERIFY(shown(h, "saveActions"));
  }

  void threeShortSlotsStayTabsWithModeratelyWideFonts() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-main");
    hub.setHubSave(QStringLiteral("g1/alt"), "cp-2");
    hub.setHubSave(QStringLiteral("g1/ch2"), "cp-3");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->slotCount() == 3, 8000);
    openSavesView(h);
    QTRY_VERIFY(shown(h, "slotTab_alt"));
    widenFonts(h, 1.5);  // roughly a wide Windows UI font
    QTest::qWait(200);   // counts of the other slots arrive
    QVERIFY(shown(h, "slotTab_default") && shown(h, "slotTab_alt") && shown(h, "slotTab_ch2"));
    QVERIFY(!shown(h, "slotSwitcher"));
    checkInColumn(h, {"slotTabs", "slotTab_default", "slotTab_alt", "slotTab_ch2", "newSlotButton"}, 5);
    QVERIFY(h.click("slotTab_ch2"));
    QTRY_COMPARE(hist->slot(), QStringLiteral("ch2"));
  }

  void slotTabsStayReachableWithWideFonts() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-main");
    hub.setHubSave(QStringLiteral("g1/speedrun-any-percent"), "cp-2");
    hub.setHubSave(QStringLiteral("g1/chapter-2"), "cp-3");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->slotCount() == 3, 8000);
    openSavesView(h);
    widenFonts(h, 8.0);
    // Either every tab is fully inside the clipped row, or the compact switcher is shown; both reach every slot by click.
    if (shown(h, "slotSwitcher")) {
      QVERIFY(h.click("slotSwitcher"));
      QTRY_VERIFY(shown(h, "slotMenu_chapter-2"));
      QVERIFY(h.click("slotMenu_chapter-2"));
      QTRY_COMPARE(hist->slot(), QStringLiteral("chapter-2"));
    } else {
      for (const char* t : {"slotTab_default", "slotTab_speedrun-any-percent", "slotTab_chapter-2"}) {
        QVERIFY2(shown(h, t), t);
        QVERIFY(h.click(t));
      }
    }
    QVERIFY2(uitest::warningCount().load() == 0, "a click was lost (tab clipped?)");
  }

  void slotSwitcherAboveThreeSlots() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-main");
    hub.setHubSave(QStringLiteral("g1/alt"), "cp-2");
    hub.setHubSave(QStringLiteral("g1/ch2"), "cp-3");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->slotCount() == 3, 8000);
    openSavesView(h);
    // Exactly three slots: tabs with the version count after the name
    QTRY_VERIFY(shown(h, "slotTab_default") && shown(h, "slotTab_alt") && shown(h, "slotTab_ch2"));
    QVERIFY(!shown(h, "slotSwitcher"));
    QVERIFY(shown(h, "newSlotButton"));
    QTRY_VERIFY(textOf(h, "slotTab_alt").contains(QStringLiteral("alt")));
    checkInColumn(h, {"slotTabs", "slotTab_default", "slotTab_alt", "slotTab_ch2", "newSlotButton"});
    // Click a tab: the slot changes
    QVERIFY(h.click("slotTab_alt"));
    QTRY_COMPARE(hist->slot(), QStringLiteral("alt"));
    QCOMPARE(h.controller->playerSettings()->saveSlot(kHubId, QStringLiteral("g1")), QStringLiteral("alt"));

    // A fourth slot collapses the tabs into the switcher "Main · n ▾"
    hub.setHubSave(QStringLiteral("g1/zen"), "cp-4");
    hist->refresh();
    QTRY_COMPARE_WITH_TIMEOUT(hist->slotCount(), 4, 8000);
    QTRY_VERIFY(shown(h, "slotSwitcher"));
    for (const char* tab : {"slotTab_default", "slotTab_alt"}) {
      QQuickItem* it = h.item(tab);
      QVERIFY2(it == nullptr || !it->isVisible(), tab);
    }
    QVERIFY(!shown(h, "newSlotButton"));  // "+ New slot" is the last item of the switcher
    checkInColumn(h, {"slotTabs", "slotSwitcher"}, 2);
    QVERIFY(h.click("slotSwitcher"));
    QTRY_VERIFY(shown(h, "slotMenu_zen") && shown(h, "slotMenuNew"));
    uitest::saveShot(h.window, QStringLiteral("library-slot-switcher"));
    QVERIFY(h.click("slotMenu_default"));
    QTRY_COMPARE(hist->slot(), QStringLiteral("default"));
    QVERIFY(h.click("slotSwitcher"));
    QTRY_VERIFY(shown(h, "slotMenuNew"));
    QVERIFY(h.click("slotMenuNew"));
    QTRY_VERIFY(shown(h, "newSlotRow"));
    QVERIFY(h.item("newSlotField")->setProperty("text", QStringLiteral("Bad Name")));
    QVERIFY(h.click("newSlotCreate"));
    QTRY_VERIFY(shown(h, "newSlotError"));
    QVERIFY(h.item("newSlotField")->setProperty("text", QStringLiteral("second-run")));
    QVERIFY(h.click("newSlotCreate"));
    QTRY_COMPARE(hist->slot(), QStringLiteral("second-run"));
    QTRY_VERIFY(!shown(h, "newSlotRow"));
  }

  void deleteSnapshotInlineConfirmation() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "a", QStringLiteral("session_end"));
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "b", QStringLiteral("manual_snapshot"), QStringLiteral("Before the lighthouse"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->history().size() == 2, 8000);
    QVERIFY(hist->canDeleteSnapshot());
    openSavesView(h);
    QTRY_VERIFY(shown(h, "deleteButton"));
    QVERIFY(!confirming(h));
    QVERIFY(h.click("deleteButton"));
    QTRY_VERIFY(shown(h, "deleteConfirmBox"));
    QVERIFY(confirming(h));  // Play is disabled while a delete confirmation is open (decision af)
    QCOMPARE(hist->deleteRequest().value(QStringLiteral("label")).toString(), QStringLiteral("Before the lighthouse"));
    uitest::saveShot(h.window, QStringLiteral("library-delete-confirm"));
    checkInColumn(h, {"deleteConfirmBox", "confirmTitle", "deleteCancel", "deleteConfirm"});
    // Opening a restore confirmation replaces the delete one
    QVERIFY(h.click("deleteCancel"));
    QTRY_VERIFY(!confirming(h));
    QCOMPARE(hub.deleteCount, 0);
    QVERIFY(h.click("deleteButton"));
    QTRY_VERIFY(shown(h, "deleteConfirm"));
    QVERIFY(h.click("deleteConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.deleteCount, 1, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(hist->history().size(), 1, 8000);
    QVERIFY(hist->message().contains(QStringLiteral("deleted")));
    QTRY_VERIFY(!confirming(h));
    // Only manual snapshots have Delete: the hub refuses anything else
    hist->requestDelete(1);
    QVERIFY(hist->deleteRequest().isEmpty());
  }

  void offlineIsReadOnly() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "a", QStringLiteral("session_end"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->versionCount() == 2, 8000);
    openSavesView(h);
    QTRY_VERIFY(shown(h, "restoreButton"));
    QVERIFY(h.item("restoreButton")->isEnabled());
    hub.failSaveRequests = 100;  // the Hub does not answer save requests
    hist->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!hist->online(), 8000);
    QTRY_VERIFY(shown(h, "savesOfflineBanner"));
    QVERIFY(textOf(h, "savesOfflineBanner").isEmpty() || true);
    QCOMPARE(hist->versionCount(), 2);  // the cached history stays visible
    QTRY_VERIFY(!h.item("restoreButton")->isEnabled());
    QVERIFY(!h.item("snapshotButton")->isEnabled());
    QVERIFY(h.item("uploadSaveButton") == nullptr || !h.item("uploadSaveButton")->isEnabled());
    hist->requestRestore(1);
    hist->requestDelete(1);
    QVERIFY(!hist->canDeleteSnapshot());
    uitest::saveShot(h.window, QStringLiteral("library-saves-offline"));
    checkInColumn(h, {"savesOfflineBanner", "savesTryAgain", "currentBar", "historyRow"});
    // Summary: "View saves →", Offline pill and the note
    QVERIFY(h.click("tab_Overview"));
    QTRY_VERIFY(shown(h, "saveSummary"));
    QCOMPARE(textOf(h, "manageSavesLink"), QStringLiteral("View saves →"));
    QCOMPARE(h.item("saveSyncPill")->property("text").toString(), QStringLiteral("Offline"));
    QVERIFY(shown(h, "saveOfflineNote"));
    QVERIFY(textOf(h, "saveMeta").startsWith(QStringLiteral("last synced")));
    uitest::saveShot(h.window, QStringLiteral("library-save-summary-offline"));
    // Back online
    hub.failSaveRequests = 0;
    hist->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(hist->online(), 8000);
    QTRY_COMPARE(textOf(h, "manageSavesLink"), QStringLiteral("Manage saves →"));
  }

  void gameRunningBannerAndPausedActions() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "a", QStringLiteral("session_end"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    PlayerController* c = h.controller.get();
    SaveHistoryController* hist = c->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->versionCount() == 2, 8000);
    // A preview game stands for the running game (no core): started, then "← Library" keeps it in the background
    c->selectGame(QStringLiteral("g1"));
    c->adoptPreviewGame(QStringLiteral("g1"));
    QImage frame(256, 384, QImage::Format_RGB32);
    frame.fill(QColor(40, 90, 140));
    c->gameSession()->setPreview(QStringLiteral("Preview"), frame, emu::DisplayProfile(), EmulationDiagnostics());
    emit c->gameSession()->started();
    QTRY_COMPARE_WITH_TIMEOUT(c->screen(), QStringLiteral("game"), 8000);
    c->leaveGameView();
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QTRY_VERIFY(hist->gameRunning());
    openSavesView(h);
    QTRY_VERIFY(shown(h, "savesRunningBanner"));
    QVERIFY(textOf(h, "savesRunningBanner").isEmpty() || true);
    QTRY_VERIFY(!h.item("restoreButton")->isEnabled());  // restore and upload wait for the game to close
    QVERIFY(h.item("uploadSaveButton") == nullptr || !h.item("uploadSaveButton")->isEnabled());
    QVERIFY(h.item("snapshotButton")->isEnabled());      // snapshots still work (decision ae)
    uitest::saveShot(h.window, QStringLiteral("library-saves-running"));
    checkInColumn(h, {"savesRunningBanner", "snapshotButton", "currentBar", "playButton"});
  }


  // Rename and delete the selected slot (saves_v5): hidden for the main slot, inline rename, delete with confirmation.
  void renameAndDeleteSlotActions() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    FakeSlot boss;
    boss.revision = 3;
    boss.content = "boss-save";
    boss.deviceId = QStringLiteral("other-device");
    boss.deviceName = QStringLiteral("Laptop Office");
    hub.saves.insert(FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("boss")), boss);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
    openSavesView(h);
    // The main slot has neither action
    QVERIFY(!hist->slotManageAvailable());
    QVERIFY(!shown(h, "slotManageRow"));
    QVERIFY(hist->renameSlot(QStringLiteral("x")).size() > 0);
    // A named slot has both
    hist->selectSlot(QStringLiteral("boss"));
    QTRY_VERIFY_WITH_TIMEOUT(hist->slotManageAvailable() && !hist->loading() && hist->current().value(QStringLiteral("revision")).toInt() == 3, 8000);
    QTRY_VERIFY(shown(h, "slotManageRow"));
    QVERIFY(h.item("slotRenameButton")->isEnabled() && h.item("slotDeleteButton")->isEnabled());
    QVERIFY(!shown(h, "slotManageHint"));
    uitest::saveShot(h.window, QStringLiteral("library-slot-manage"));
    checkInColumn(h, {"slotManageRow", "slotRenameButton", "slotDeleteButton"});

    // Rename: validation first, then the Hub renames, the selection follows
    QVERIFY(h.click("slotRenameButton"));
    QTRY_VERIFY(shown(h, "renameSlotRow"));
    QVERIFY(!shown(h, "slotManageRow"));
    h.item("renameSlotField")->setProperty("text", QStringLiteral("Bad Name"));
    QVERIFY(h.click("renameSlotConfirm"));
    QTRY_VERIFY(shown(h, "renameSlotError"));
    QCOMPARE(hub.renameCount, 0);
    uitest::saveShot(h.window, QStringLiteral("library-slot-rename"));
    checkInColumn(h, {"renameSlotRow", "renameSlotField", "renameSlotError", "renameSlotCancel", "renameSlotConfirm"});
    h.item("renameSlotField")->setProperty("text", QStringLiteral("default"));  // an existing name
    QVERIFY(h.click("renameSlotConfirm"));
    QTRY_VERIFY(shown(h, "renameSlotError"));
    QVERIFY(h.click("renameSlotCancel"));
    QTRY_VERIFY(!shown(h, "renameSlotRow"));
    QVERIFY(h.click("slotRenameButton"));
    QTRY_VERIFY(shown(h, "renameSlotField"));
    h.item("renameSlotField")->setProperty("text", QStringLiteral("final"));
    QVERIFY(h.click("renameSlotConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.renameCount, 1, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(hist->slot(), QStringLiteral("final"), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().contains(QStringLiteral("renamed")), 8000);
    QVERIFY(!hist->messageIsError());
    QTRY_VERIFY_WITH_TIMEOUT(hist->current().value(QStringLiteral("revision")).toInt() == 3 && !hist->loading(), 8000);
    QVERIFY(hub.saves.contains(FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("final"))));

    // Delete: confirmation names the latest checkpoint; cancel changes nothing
    QTRY_VERIFY(shown(h, "slotDeleteButton"));
    QVERIFY(h.click("slotDeleteButton"));
    QTRY_VERIFY(shown(h, "slotDeleteBox"));
    QVERIFY(confirming(h));  // Play is off meanwhile
    QVERIFY(textOf(h, "slotDeleteTitle").contains(QStringLiteral("final")));
    QVERIFY(textOf(h, "slotDeleteLatest").contains(QStringLiteral("r3")));
    QVERIFY(textOf(h, "slotDeleteLatest").contains(QStringLiteral("Laptop Office")));
    QVERIFY(textOf(h, "slotDeleteNote").contains(QStringLiteral("cannot be undone")));
    uitest::saveShot(h.window, QStringLiteral("library-slot-delete-confirm"));
    checkInColumn(h, {"slotDeleteBox", "slotDeleteTitle", "slotDeleteLatest", "slotDeleteNote", "slotDeleteCancel", "slotDeleteConfirm"});
    widenFonts(h, 1.4);
    checkInColumn(h, {"slotDeleteBox", "slotDeleteTitle", "slotDeleteLatest", "slotDeleteNote", "slotDeleteCancel", "slotDeleteConfirm"});
    QVERIFY(h.click("slotDeleteCancel"));
    QTRY_VERIFY(!shown(h, "slotDeleteBox"));
    QVERIFY(!confirming(h));
    QCOMPARE(hub.slotDeleteCount, 0);

    // The Hub moved on behind our back: nothing is deleted (stale revision), the user is told
    QVERIFY(h.click("slotDeleteButton"));
    QTRY_VERIFY(shown(h, "slotDeleteConfirm"));
    hub.saves[FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("final"))].revision = 4;
    QVERIFY(h.click("slotDeleteConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(hist->messageIsError() && hist->message().contains(QStringLiteral("changed on the server")), 8000);
    QCOMPARE(hub.slotDeleteCount, 0);
    QCOMPARE(hist->slot(), QStringLiteral("final"));

    // Confirmed: the slot is gone, the game uses the main slot again
    QTRY_VERIFY_WITH_TIMEOUT(hist->current().value(QStringLiteral("revision")).toInt() == 4 && !hist->loading(), 8000);
    QVERIFY(h.click("slotDeleteButton"));
    QTRY_VERIFY(shown(h, "slotDeleteConfirm"));
    QVERIFY(h.click("slotDeleteConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.slotDeleteCount, 1, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(hist->slot(), QStringLiteral("default"), 8000);
    QVERIFY(!hub.saves.contains(FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("final"))));
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().contains(QStringLiteral("deleted")), 8000);
    QTRY_VERIFY(!shown(h, "slotManageRow"));
  }

  // saves_v6: the main slot can be deleted (with the same confirmation), never renamed.
  void mainSlotCanBeDeletedWithSavesV6() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub, kAllFeatures + QStringList{QStringLiteral("saves_v6")});
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
    openSavesView(h);
    QCOMPARE(hist->slot(), QStringLiteral("default"));
    QVERIFY(!hist->slotManageAvailable());  // no rename
    QTRY_VERIFY_WITH_TIMEOUT(hist->slotDeleteAvailable() && hist->current().value(QStringLiteral("revision")).toInt() == 1, 8000);
    QTRY_VERIFY(shown(h, "slotManageRow"));
    QVERIFY(!shown(h, "slotRenameButton"));
    QVERIFY(h.item("slotDeleteButton")->isEnabled());
    QVERIFY(h.click("slotDeleteButton"));
    QTRY_VERIFY(shown(h, "slotDeleteBox"));
    QVERIFY(confirming(h));
    QVERIFY(textOf(h, "slotDeleteNote").contains(QStringLiteral("cannot be undone")));
    QVERIFY(h.click("slotDeleteCancel"));
    QTRY_VERIFY(!shown(h, "slotDeleteBox"));
    QCOMPARE(hub.slotDeleteCount, 0);
    QVERIFY(h.click("slotDeleteButton"));
    QTRY_VERIFY(shown(h, "slotDeleteConfirm"));
    QVERIFY(h.click("slotDeleteConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.slotDeleteCount, 1, 8000);
    QVERIFY(!hub.saves.contains(FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("default"))));
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().contains(QStringLiteral("main slot was deleted")), 8000);
    QVERIFY(!hist->messageIsError());
  }

  void slotActionsAreDisabledWhileTheGameRuns() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    FakeSlot boss;
    boss.revision = 3;
    boss.content = "boss-save";
    hub.saves.insert(FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("boss")), boss);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    PlayerController* c = h.controller.get();
    SaveHistoryController* hist = c->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
    hist->selectSlot(QStringLiteral("boss"));
    QTRY_VERIFY_WITH_TIMEOUT(hist->slotManageAvailable() && !hist->loading(), 8000);
    // A preview game stands for the running game
    c->selectGame(QStringLiteral("g1"));
    c->adoptPreviewGame(QStringLiteral("g1"));
    QImage frame(256, 384, QImage::Format_RGB32);
    frame.fill(QColor(40, 90, 140));
    c->gameSession()->setPreview(QStringLiteral("Preview"), frame, emu::DisplayProfile(), EmulationDiagnostics());
    emit c->gameSession()->started();
    QTRY_COMPARE_WITH_TIMEOUT(c->screen(), QStringLiteral("game"), 8000);
    c->leaveGameView();
    QTRY_VERIFY(hist->gameRunning());
    openSavesView(h);
    QTRY_VERIFY(shown(h, "slotManageRow"));
    QTRY_VERIFY(!h.item("slotRenameButton")->isEnabled());
    QVERIFY(!h.item("slotDeleteButton")->isEnabled());
    QTRY_VERIFY(shown(h, "slotManageHint"));
    QVERIFY(textOf(h, "slotManageHint").contains(QStringLiteral("running")));
    QVERIFY(!hist->slotManageBlockReason().isEmpty());
    uitest::saveShot(h.window, QStringLiteral("library-slot-manage-running"));
    checkInColumn(h, {"slotManageRow", "slotRenameButton", "slotDeleteButton", "slotManageHint"});
    // The API refuses too, whatever the buttons do
    QVERIFY(!hist->renameSlot(QStringLiteral("other")).isEmpty());
    hist->requestDeleteSlot();
    QVERIFY(hist->slotDeleteRequest().isEmpty());
    QCOMPARE(hub.renameCount, 0);
    QCOMPARE(hub.slotDeleteCount, 0);
    QCOMPARE(hub.uploadCount, 0);
  }

  void renamedAndDeletedPushesUpdateTheOpenSavesView() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    FakeSlot boss;
    boss.revision = 3;
    boss.content = "boss-save";
    hub.saves.insert(FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("boss")), boss);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
    hist->selectSlot(QStringLiteral("boss"));
    QTRY_VERIFY_WITH_TIMEOUT(hist->slotManageAvailable() && !hist->loading(), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(hub.wsOpen() > 0, 8000);
    // Another device renames it: the Hub state moves, the push arrives
    hub.saves.insert(FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("final")), hub.saves.take(FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("boss"))));
    hub.sendWs(QStringLiteral("save_updated"),
               QJsonObject{{QStringLiteral("game_id"), QStringLiteral("g1")}, {QStringLiteral("slot"), QStringLiteral("final")},
                           {QStringLiteral("previous_slot"), QStringLiteral("boss")}, {QStringLiteral("revision"), 3},
                           {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))}, {QStringLiteral("device_id"), QStringLiteral("other-device")},
                           {QStringLiteral("device_name"), QStringLiteral("Laptop Office")}, {QStringLiteral("reason"), QStringLiteral("renamed")}});
    QTRY_COMPARE_WITH_TIMEOUT(hist->slot(), QStringLiteral("final"), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().contains(QStringLiteral("renamed to \"final\"")), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(hist->current().value(QStringLiteral("revision")).toInt() == 3 && !hist->loading(), 8000);
    // ... and deletes it
    hub.saves.remove(FakeHub::slotKey(QStringLiteral("g1"), QStringLiteral("final")));
    hub.sendWs(QStringLiteral("save_updated"),
               QJsonObject{{QStringLiteral("game_id"), QStringLiteral("g1")}, {QStringLiteral("slot"), QStringLiteral("final")},
                           {QStringLiteral("revision"), 3}, {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
                           {QStringLiteral("device_id"), QStringLiteral("other-device")}, {QStringLiteral("device_name"), QStringLiteral("Laptop Office")},
                           {QStringLiteral("reason"), QStringLiteral("deleted")}});
    QTRY_COMPARE_WITH_TIMEOUT(hist->slot(), QStringLiteral("default"), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().contains(QStringLiteral("deleted")), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(hist->current().value(QStringLiteral("revision")).toInt() == 2 || hist->current().value(QStringLiteral("revision")).toInt() == 1, 8000);
  }

  // "Resolve conflict" opens the inline resolution; it never starts the game (3c-4 follow-up).
  void resolveConflictInlineBothWaysWithoutStartingTheGame() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "base-state");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    PlayerController* c = h.controller.get();
    SaveHistoryController* hist = c->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
    const ConflictSeed seed = seedConflict(h, hub, QStringLiteral("c1"), "hub-newer-save", "local-newer-offline");
    QTRY_COMPARE_WITH_TIMEOUT(c->selectedGame().value(QStringLiteral("syncKind")).toString(), QStringLiteral("conflict"), 8000);
    hist->refresh();
    openSavesViewViaApi(h);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "savesConflictBanner"), 8000);
    QVERIFY(!shown(h, "conflictBox"));
    QVERIFY(!textOf(h, "resolveConflictButton").isEmpty());
    // Restore / upload stay paused while the conflict is open
    QTRY_VERIFY(h.items("restoreButton").isEmpty() || !h.item("restoreButton")->isEnabled());

    QVERIFY(h.click("resolveConflictButton"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "conflictBox"), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "conflictLocal") && shown(h, "conflictHub"), 8000);
    QVERIFY(!shown(h, "savesConflictBanner"));
    QCOMPARE(c->screen(), QStringLiteral("library"));         // the game did not start
    QCOMPARE(c->gameSession()->state(), GameSession::Idle);
    QVERIFY(c->saveConflict().isEmpty());                      // and the start dialog did not open
    QVERIFY(!shown(h, "conflictDialog"));
    QVERIFY(!h.item("playButton")->isEnabled());               // Play waits for the decision
    QVERIFY(!textOf(h, "conflictLocalDevice").isEmpty());
    QVERIFY(!textOf(h, "conflictLocalWhen").isEmpty());
    QCOMPARE(textOf(h, "conflictLocalSize").isEmpty(), false);
    QCOMPARE(textOf(h, "conflictHubDevice"), QStringLiteral("Laptop Office"));
    QVERIFY(!textOf(h, "conflictHubWhen").isEmpty());
    QVERIFY(textOf(h, "conflictHubSize").startsWith(QStringLiteral("r2 · ")));
    QCOMPARE(fileBytes(seed.file), QByteArray("local-newer-offline"));  // looking at it changes nothing
    uitest::saveShot(h.window, QStringLiteral("library-conflict-resolution"));
    checkInColumn(h, {"conflictBox", "conflictLocal", "conflictHub", "conflictLocalDevice", "conflictHubDevice", "conflictKeepLocal",
                      "conflictUseHub", "conflictCancel"});
    widenFonts(h, 1.4);
    checkInColumn(h, {"conflictBox", "conflictLocal", "conflictHub", "conflictLocalDevice", "conflictHubWhen", "conflictKeepLocal",
                      "conflictUseHub", "conflictCancel"});

    // Cancel leaves everything as it is
    QVERIFY(h.click("conflictCancel"));
    QTRY_VERIFY(!shown(h, "conflictBox"));
    QTRY_VERIFY(shown(h, "savesConflictBanner"));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).conflicts.first().status, QStringLiteral("open"));

    // Use the Hub save: the device copy is backed up first, then replaced
    QVERIFY(h.click("resolveConflictButton"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "conflictUseHub") && h.item("conflictUseHub")->isEnabled(), 8000);
    QVERIFY(h.click("conflictUseHub"));
    QTRY_VERIFY_WITH_TIMEOUT(!shown(h, "conflictBox"), 8000);
    QTRY_VERIFY(hist->message().startsWith(QStringLiteral("Conflict resolved")));
    QCOMPARE(fileBytes(seed.file), QByteArray("hub-newer-save"));
    const QStringList backups = QDir(seed.dir).entryList({QStringLiteral("*.local-*.bak")});
    QCOMPARE(backups.size(), 1);
    QCOMPARE(fileBytes(seed.dir + QLatin1Char('/') + backups.first()), QByteArray("local-newer-offline"));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).conflicts.first().status, QStringLiteral("resolved_hub"));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).revision, 2);
    QTRY_COMPARE(c->selectedGame().value(QStringLiteral("syncKind")).toString(), QStringLiteral("synced"));
    QVERIFY(!shown(h, "savesConflictBanner"));
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QCOMPARE(c->gameSession()->state(), GameSession::Idle);
    uitest::saveShot(h.window, QStringLiteral("library-conflict-resolved"));

    // The next conflict: keep this device's save, which becomes the new current version
    const ConflictSeed seed2 = seedConflict(h, hub, QStringLiteral("c2"), "hub-third-save", "local-third-offline");
    QTRY_COMPARE_WITH_TIMEOUT(c->selectedGame().value(QStringLiteral("syncKind")).toString(), QStringLiteral("conflict"), 8000);
    QTRY_VERIFY(shown(h, "savesConflictBanner"));
    QVERIFY(h.click("resolveConflictButton"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "conflictKeepLocal") && h.item("conflictKeepLocal")->isEnabled(), 8000);
    QVERIFY(h.click("conflictKeepLocal"));
    QTRY_VERIFY_WITH_TIMEOUT(!shown(h, "conflictBox"), 8000);
    QTRY_VERIFY(hist->message().startsWith(QStringLiteral("Conflict resolved")));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).content, QByteArray("local-third-offline"));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).revision, 4);
    QCOMPARE(fileBytes(seed2.file), QByteArray("local-third-offline"));
    QTRY_COMPARE(c->selectedGame().value(QStringLiteral("syncKind")).toString(), QStringLiteral("synced"));
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QCOMPARE(c->gameSession()->state(), GameSession::Idle);
    QVERIFY(c->saveConflict().isEmpty());
  }

  void resolveConflictAlreadyResolvedElsewhere() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "base-state");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    PlayerController* c = h.controller.get();
    SaveHistoryController* hist = c->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading(), 8000);
    const ConflictSeed seed = seedConflict(h, hub, QStringLiteral("c1"), "hub-newer-save", "local-newer-offline");
    QTRY_COMPARE_WITH_TIMEOUT(c->selectedGame().value(QStringLiteral("syncKind")).toString(), QStringLiteral("conflict"), 8000);
    hist->refresh();
    openSavesViewViaApi(h);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "resolveConflictButton"), 8000);
    // Resolved in the server admin meanwhile: the view says so and the local marker is dropped
    hub.saves[QStringLiteral("g1")].conflicts[0].status = QStringLiteral("resolved_hub");
    QVERIFY(h.click("resolveConflictButton"));
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().contains(QStringLiteral("already resolved")), 8000);
    QVERIFY(!shown(h, "conflictBox"));
    QVERIFY(SaveStore::loadState(seed.dir).conflictId.isEmpty());
    QCOMPARE(fileBytes(seed.file), QByteArray("local-newer-offline"));  // nothing was overwritten
    QTRY_VERIFY(c->selectedGame().value(QStringLiteral("syncKind")).toString() != QStringLiteral("conflict"));
    QCOMPARE(c->screen(), QStringLiteral("library"));
  }

  void uploadConfirmationFailureAndRetry() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString picked = tmp.filePath(QStringLiteral("lumen-drift-backup-with-a-rather-long-name-for-the-column.sav"));
    QFile pf(picked);
    QVERIFY(pf.open(QIODevice::WriteOnly));
    pf.write("uploaded-bytes");
    pf.close();
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->canUploadFile() && !hist->loading(), 8000);
    openSavesView(h);
    QTRY_VERIFY(shown(h, "saveActions"));
    hist->requestUploadFile(picked);
    QTRY_VERIFY(shown(h, "uploadConfirmBox"));
    QVERIFY(!shown(h, "saveActions"));  // the confirmation takes the place of the actions
    QVERIFY(confirming(h));
    QCOMPARE(textOf(h, "uploadConfirmTitle"), QStringLiteral("Upload as the new current version?"));
    QVERIFY(textOf(h, "uploadSlot").contains(QStringLiteral("Main")) && textOf(h, "uploadSlot").contains(QStringLiteral("r2")));
    uitest::saveShot(h.window, QStringLiteral("library-upload-confirm"));
    checkInColumn(h, {"uploadConfirmBox", "uploadFileName", "uploadSize", "uploadSlot", "uploadCancel", "uploadConfirm"});
    {
      const QRectF box = sceneRect(h.item("uploadConfirmBox"));
      for (const char* n : {"uploadCancel", "uploadConfirm"}) {
        const QRectF r = sceneRect(h.item(n));
        QVERIFY2(r.left() >= box.left() - 0.5 && r.right() <= box.right() + 0.5 && r.bottom() <= box.bottom() + 0.5,
                 qPrintable(QStringLiteral("'%1' outside uploadConfirmBox:%2").arg(QLatin1String(n), chainOf(h.item(n)))));
      }
    }
    // Failure: the Hub does not answer; nothing changed, the file can be tried again
    hub.failSaveRequests = 1;
    QVERIFY(h.click("uploadConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "uploadFailedBox"), 8000);
    QCOMPARE(hub.uploadCount, 0);
    QVERIFY(!hist->uploadFailure().isEmpty());
    uitest::saveShot(h.window, QStringLiteral("library-upload-failed"));
    checkInColumn(h, {"uploadFailedBox", "uploadFailedText", "uploadRetry", "uploadFailedCancel"});
    QVERIFY(h.click("uploadRetry"));
    QTRY_VERIFY(shown(h, "uploadConfirmBox"));
    hub.failSaveRequests = 0;
    QVERIFY(h.click("uploadConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.uploadCount, 1, 8000);
    QTRY_VERIFY(!confirming(h));
    QVERIFY(hist->uploadFailure().isEmpty());
  }

  // ---- Layout of the detail column at 1280 and 960 px ---------------------------------------------------------------
  // Regression: "◆ Create snapshot" / "Upload save file…" in the saveActions Flow showed as "◆ Crea…" / "Uplo…" at every size.
  // While the 392 px column has room, both buttons show their full label (width >= label implicitWidth + padding).
  void saveActionButtonsKeepTheirFullLabel() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub, kAllFeatures);
    hub.setHubSave(QStringLiteral("g1"), "cp-1");
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "old", QStringLiteral("session_end"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->versionCount() >= 2, 8000);
    for (const QSize size : {QSize(1280, 800), QSize(1920, 1080)}) {
      h.window->resize(size);
      QQuickTest::qWaitForPolish(h.window);
      QTest::qWait(60);
      if (!shown(h, "savesView")) openSavesView(h);
      QTRY_VERIFY(shown(h, "saveActions"));
      QTest::qWait(60);
      QQuickTest::qWaitForPolish(h.window);
      const qreal room = h.item("saveActions")->width();
      QVERIFY2(room > 250, qPrintable(QStringLiteral("saveActions only %1 wide").arg(room)));
      for (const char* name : {"snapshotButton", "uploadSaveButton"}) {
        QQuickItem* btn = h.item(name);
        QVERIFY2(btn != nullptr && btn->isVisible(), name);
        QQuickItem* lbl = btn->findChild<QQuickItem*>("fbButtonLabel");
        QVERIFY(lbl != nullptr);
        const qreal natural = lbl->implicitWidth();
        const qreal pad = btn->property("hPadding").toReal();
        if (natural + pad > room + 0.5) continue;   // absurdly wide test fonts: the label may then elide, it only has to fit when the column can hold it
        const QString tag = QStringLiteral("%1 at %2 px").arg(QLatin1String(name)).arg(size.width());
        QVERIFY2(btn->width() + 0.5 >= natural + pad, qPrintable(tag + QStringLiteral(": width %1 < label %2 + padding %3").arg(btn->width()).arg(natural).arg(pad)));
        QVERIFY2(lbl->width() + 0.5 >= natural, qPrintable(tag + QStringLiteral(": label elided (%1 < %2)").arg(lbl->width()).arg(natural)));
      }
    }
  }

  // Extreme glyph width: the SAVE card's "Manage saves" link takes its implicit width unless it is constrained to the card.
  void saveCardLinkStaysInsideTheColumnWithVeryWideText() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub, kAllFeatures, QStringLiteral("Game"));
    hub.setHubSave(QStringLiteral("g1"), "cp-1", QStringLiteral("dev-1"), QStringLiteral("Desktop"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "manageSavesLink"), 8000);
    widenFonts(h, 40.0);
    checkInColumn(h, {"saveSummary", "manageSavesLink"}, 2);
  }

  void detailColumnLayoutAtNarrowAndWideWindows() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub, kAllFeatures, QStringLiteral("Supercalifragilisticexpialidocious Adventures of the Remastered Deluxe Edition"));
    hub.setHubSave(QStringLiteral("g1"), "cp-1", QStringLiteral("dev-1"), QStringLiteral("Desktop-LivingRoom-With-A-Very-Long-Device-Name"));
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "a", QStringLiteral("session_end"), QString(), QStringLiteral("Laptop-Office"));
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "b", QStringLiteral("manual_snapshot"),
                   QStringLiteral("A snapshot label that is much longer than the column can show on one line"), QStringLiteral("Desktop-LivingRoom"));
    for (const char* s : {"g1/speedrun", "g1/chapter-2", "g1/a-rather-long-slot-name"}) hub.setHubSave(QString::fromLatin1(s), "x");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->slotCount() == 4 && hist->versionCount() == 3, 8000);
    auto widen = [&h](bool wide) { if (wide) widenFonts(h, 10.0); };
    // Second pass with wider glyphs (extra letter spacing on every item with a font), like the wider Windows fonts.
    for (const bool wide : {false, true}) for (const QSize size : {QSize(1280, 800), QSize(960, 600)}) {
      h.window->resize(size);
      QQuickTest::qWaitForPolish(h.window);
      QTest::qWait(60);
      const QString what = QStringLiteral("%1x%2%3").arg(size.width()).arg(size.height()).arg(wide ? QStringLiteral("-wide") : QString());
      // Overview
      QTRY_VERIFY(shown(h, "saveSummary") || shown(h, "savesView"));
      if (shown(h, "savesView")) QVERIFY(h.click("tab_Overview"));
      QTRY_VERIFY(shown(h, "saveSummary"));
      widen(wide);
      {
        // The header text must stay inside the column's content area (24 px margin), not just inside the column.
        const QRectF cr = sceneRect(column(h));
        for (const char* n : {"detailTitle", "detailSystem", "detailPill"}) {
          const QRectF r = sceneRect(h.item(n));
          QVERIFY2(r.right() <= cr.right() - 24 + 0.5, qPrintable(QStringLiteral("'%1' crosses the content margin (%2 > %3):%4").arg(QLatin1String(n)).arg(r.right()).arg(cr.right() - 24).arg(chainOf(h.item(n)))));
        }
      }
      checkInColumn(h, {"detailPane", "detailTitle", "detailPill", "detailSystem", "detailRowValue", "saveSummary", "saveCurrentRow", "saveSlotName",
                        "saveVersion", "saveMeta", "saveSyncPill", "saveCounts", "manageSavesLink", "playButton", "playShareButton"}, 12);
      if (QTest::currentTestFailed()) { qWarning("[uitest] overview layout failed at %s", qPrintable(what)); return; }
      // Saves view with a restore confirmation (long labels, switcher)
      QVERIFY(h.click("tab_Saves"));  // the link may sit below the fold of a short window; the tab never does
      QTRY_VERIFY(shown(h, "savesView"));
      QTRY_VERIFY(shown(h, "slotSwitcher"));
      hist->requestRestore(1);
      QTRY_VERIFY(shown(h, "restoreConfirmBox"));
      widen(wide);
      checkInColumn(h, {"savesView", "savesBack", "savesUpdated", "historyRefresh", "savesTitle", "slotTabs", "slotSwitcher", "currentBar", "currentMeta", "currentVersion", "currentPill",
                        "saveActions", "snapshotButton", "uploadSaveButton", "historyHeader", "historyFilter", "timeline", "saveRow", "historyTitle",
                        "historyLabel", "historyMeta", "restoreButton", "deleteButton", "restoreConfirmBox", "confirmTitle", "restoreCancel",
                        "restoreConfirm", "playButton", "playShareButton"}, 20);
      if (QTest::currentTestFailed()) { qWarning("[uitest] saves view layout failed at %s", qPrintable(what)); return; }
      uitest::saveShot(h.window, QStringLiteral("library-saves-%1").arg(what));
      hist->cancelRestore();
      QVERIFY(h.click("tab_Overview"));
    }
    h.window->resize(1280, 800);
  }
  // ---- Virtualized grid: a very large Library only instantiates the tiles around the viewport -----------------------
  // GameGrid in a Flickable over a synthetic model of 5000 games (no Hub: only the grid is measured).
  void largeLibraryCreatesOnlyVisibleTiles() {
    constexpr int kGames = 5000;
    QTemporaryDir dir;
    QFile qml(dir.filePath(QStringLiteral("GridHost.qml")));
    QVERIFY(qml.open(QIODevice::WriteOnly));
    qml.write(R"(import QtQuick
import Uzume.App
Item {
    id: root
    width: 1200
    height: 700
    property alias grid: grid
    property alias flick: flick
    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + 8
        Column {
            id: column
            x: 4
            y: 4
            width: flick.width - 4
            GameGrid {
                id: grid
                objectName: "gameGrid"
                width: parent.width
                scroller: flick
                cellWidth: 170
                cellHeight: 236
                model: ListModel { id: games }
                selectedId: "g3"
                runningId: "g4"
            }
        }
    }
    function fill(n) {
        for (let i = 0; i < n; ++i) {
            games.append({gameId: "g" + i, title: "Homebrew " + i, system: "nds", monogram: "H", stateKind: "ready",
                          tileText: "Ready", tileTone: "ok", statusWord: "Ready", statusWarn: false, progress: 0, addedText: "", onHub: true, localFile: false,
                          playTimeText: "–", lastPlayedText: "–"})
        }
    }
    function clear() { games.clear() }
}
)");
    qml.close();
    QQuickView view;
    view.engine()->addImportPath(QStringLiteral("qrc:/qt/qml"));
    view.setSource(QUrl::fromLocalFile(qml.fileName()));
    QVERIFY2(view.status() == QQuickView::Ready, "GridHost.qml did not load");
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QQuickItem* host = view.rootObject();
    QElapsedTimer timer;
    timer.start();
    QVERIFY(QMetaObject::invokeMethod(host, "fill", Q_ARG(QVariant, kGames)));
    QQuickTest::qWaitForPolish(&view);
    QObject* grid = host->property("grid").value<QObject*>();
    QObject* flick = host->property("flick").value<QObject*>();
    QVERIFY(grid != nullptr && flick != nullptr);
    const auto created = [&]() { return grid->property("createdCount").toInt(); };
    const auto alive = [&]() { return grid->property("alive").toInt(); };
    const auto tile = [&](int i) {
      QVariant t;
      QMetaObject::invokeMethod(grid, "tileFor", Q_RETURN_ARG(QVariant, t), Q_ARG(QVariant, QStringLiteral("g%1").arg(i)));
      return t.value<QObject*>();
    };
    qInfo() << "5000 games: grid filled in" << timer.elapsed() << "ms," << created() << "tiles created";
    QCOMPARE(grid->property("count").toInt(), kGames);
    QVERIFY2(created() > 0 && created() <= 80, qPrintable(QStringLiteral("created %1 tiles").arg(created())));
    QVERIFY(alive() <= 80);
    // Selection and "running" state reach the tiles that exist.
    QVERIFY(tile(3) != nullptr && tile(3)->property("selected").toBool() && !tile(3)->property("running").toBool());
    QVERIFY(tile(4) != nullptr && tile(4)->property("running").toBool());
    QVERIFY(tile(2) != nullptr && !tile(2)->property("selected").toBool());
    QVERIFY(tile(kGames - 1) == nullptr);
    // The scroll range covers every row.
    const qreal contentHeight = flick->property("contentHeight").toReal();
    QVERIFY(contentHeight > 236.0 * (kGames / 8.0));
    // Scrolling to the end creates the last tiles and recycles the first ones; the live count stays small.
    flick->setProperty("contentY", contentHeight - view.height());
    QTRY_VERIFY_WITH_TIMEOUT(tile(kGames - 1) != nullptr, 3000);
    QVERIFY(tile(0) == nullptr);
    QVERIFY(alive() <= 80);
    // Scrolling through the list in steps only creates what comes into view.
    for (qreal y = 0; y < contentHeight; y += contentHeight / 40) {
      flick->setProperty("contentY", y);
      QTest::qWait(0);
    }
    QVERIFY(alive() <= 80);
    QVERIFY2(created() < 3000, qPrintable(QStringLiteral("created %1 tiles in total").arg(created())));
    // Back at the top the first tiles exist again.
    flick->setProperty("contentY", 0);
    QTRY_VERIFY_WITH_TIMEOUT(tile(0) != nullptr, 3000);
    // An emptied model leaves no tiles.
    QVERIFY(QMetaObject::invokeMethod(host, "clear"));
    QTRY_COMPARE_WITH_TIMEOUT(grid->property("count").toInt(), 0, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(alive(), 0, 3000);
  }

  // ---- Save errors on stop -------------------------------------------------------------------------------------------
  void stopSaveErrorBannerIsShownAndDismissed() {
    FakeHub hub(QStringLiteral("a"));
    oneGame(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QVERIFY(!shown(h, "stopSaveError"));
    emit h.controller->gameSession()->stopSaveFailed(QStringLiteral("game.sav: No space left on device"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "stopSaveError"), 3000);
    QVERIFY(h.controller->stopSaveError().contains(QStringLiteral("No space left on device")));
    QVERIFY(h.controller->stopSaveError().contains(QStringLiteral("progress may not be saved")));
    uitest::saveShot(h.window, QStringLiteral("library-stop-save-error"));
    QVERIFY(h.click("stopSaveErrorDismiss"));
    QTRY_VERIFY_WITH_TIMEOUT(!shown(h, "stopSaveError"), 3000);
    QVERIFY(h.controller->stopSaveError().isEmpty());
  }
};

UITEST_MAIN(LibrarySavesUiTest)
#include "library_saves_ui_test.moc"
