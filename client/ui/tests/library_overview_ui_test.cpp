// The readable Library (readable pass, PR 5; boards 3c-1-r6, 3c-2-r6, 3c-3-r6): the status chips with their counts and the
// filter they set, the List / Grid switch and its persistence, the single problem sentence of the game page with its fix as
// the primary action, and "Remove from library" with Undo. Offscreen; synthetic data only.
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

#include "fakehub.h"
#include "playersettings.h"
#include "profilestore.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

class LibraryOverviewUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub, int width = 1280) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    h.window->resize(width, 900);
  }
  static void seed(FakeHub& hub, int n) {
    const QByteArray rom = "homebrew-dummy-rom-overview";
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("local_roms_v1")};
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    QJsonArray games;
    for (int i = 1; i <= n; ++i) {
      games.append(uitest::gameJson(QStringLiteral("g%1").arg(i), QStringLiteral("Game %1").arg(i), uitest::sha256Hex(rom), rom.size()));
    }
    hub.games = QJsonObject{{QStringLiteral("games"), games}};
  }
  static bool shown(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr && it->isVisible();
  }
  static QString textOf(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr ? it->property("text").toString() : QString();
  }
  static int visibleCount(Harness& h, const char* name) {
    int n = 0;
    for (QQuickItem* t : h.items(name)) {
      if (t->isVisible() && t->window() == h.window) ++n;
    }
    return n;
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // The status filter lives in the Systems… menu (the four status chips are gone): counts in the menu, one scope at a time.
  void statusFilterInTheSystemsMenu() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 4);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "scopeChip_all") && shown(h, "scopeChip_menu"), 5000);
    QVERIFY(h.item("statusChip_all") == nullptr);
    const QVariantMap counts = h.controller->library()->counts();
    QCOMPARE(counts.value(QStringLiteral("all")).toInt(), 4);
    QCOMPARE(counts.value(QStringLiteral("ready")).toInt() + counts.value(QStringLiteral("attention")).toInt() + counts.value(QStringLiteral("download")).toInt(), 4);
    QCOMPARE(h.item("scopeChip_all")->property("count").toInt(), 4);
    QVERIFY(h.item("scopeChip_all")->property("active").toBool());
    QCOMPARE(h.item("scopeChip_menu")->property("text").toString(), QStringLiteral("Systems…"));

    // Choosing a status filters, the rows follow its count and the chip shows the choice.
    for (const QString& id : {QStringLiteral("attention"), QStringLiteral("ready"), QStringLiteral("download")}) {
      QVERIFY(h.click("scopeChip_menu"));
      const QByteArray item = QByteArray("menuItem_status_") + id.toUtf8();
      QTRY_VERIFY(shown(h, item.constData()));
      QVERIFY(textOf(h, item.constData()).endsWith(QStringLiteral(" · ") + QString::number(counts.value(id).toInt())));
      QVERIFY(h.click(item.constData()));
      QTRY_COMPARE(h.controller->library()->filter(), id);
      QCOMPARE(h.controller->library()->rowCount(), counts.value(id).toInt());
      QVERIFY(h.item("scopeChip_menu")->property("active").toBool());
      QVERIFY(!h.item("scopeChip_all")->property("active").toBool());
      QCOMPARE(h.item("scopeChip_menu")->property("badgeAccent").toBool(), id == QLatin1String("attention"));
    }
    QVERIFY(h.click("scopeChip_all"));
    QTRY_COMPARE(h.controller->library()->filter(), QStringLiteral("all"));
    QCOMPARE(h.controller->library()->rowCount(), 4);
    QVERIFY(!h.item("scopeChip_menu")->property("active").toBool());

    // A system is chosen from the same menu; every seeded game is a "nds" game here.
    const QVariantList systems = h.controller->library()->systemChips();
    QVERIFY(!systems.isEmpty());
    const QString sys = systems.first().toMap().value(QStringLiteral("value")).toString();
    QVERIFY(h.click("scopeChip_menu"));
    const QByteArray sysItem = QByteArray("menuItem_system_") + sys.toLower().toUtf8();
    QTRY_VERIFY(shown(h, sysItem.constData()));
    QVERIFY(h.click(sysItem.constData()));
    QTRY_COMPARE(h.controller->library()->systemFilter().toLower(), sys.toLower());
    QCOMPARE(h.controller->library()->rowCount(), systems.first().toMap().value(QStringLiteral("count")).toInt());
    QVERIFY(h.click("scopeChip_all"));
    QTRY_COMPARE(h.controller->library()->systemFilter(), QString());

    // Pad L / R step through the chips and wrap around.
    QCOMPARE(h.controller->library()->filter(), QStringLiteral("all"));
    emit h.controller->controllers()->padCommand(QStringLiteral("filterNext"));
    QTRY_COMPARE(h.controller->library()->filter(), QStringLiteral("ready"));
    emit h.controller->controllers()->padCommand(QStringLiteral("filterPrev"));
    QTRY_COMPARE(h.controller->library()->filter(), QStringLiteral("all"));
    emit h.controller->controllers()->padCommand(QStringLiteral("filterPrev"));
    QTRY_COMPARE(h.controller->library()->filter(), QStringLiteral("download"));
    emit h.controller->controllers()->padCommand(QStringLiteral("filterNext"));
    QTRY_COMPARE(h.controller->library()->filter(), QStringLiteral("all"));

    // The warning stays on every row that needs attention: each row of the list has its square and a status word.
    QTRY_COMPARE(visibleCount(h, "tileStatus"), 4);
    QCOMPARE(visibleCount(h, "tileWarn"), counts.value(QStringLiteral("attention")).toInt());
  }

  // The uncluttered Library at 1280 x 800 with an admin, notices and a played game: one notices line ("+N more" folds the others),
  // one toolbar row, a Continue card, and at least seven game rows without scrolling.
  void noticesFoldIntoOneLineAndSevenRowsFit() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 12);
    QJsonArray games = hub.games.value(QStringLiteral("games")).toArray();
    QJsonObject first = games.at(0).toObject();
    first.insert(QStringLiteral("personal"), QJsonObject{{QStringLiteral("play_seconds"), 600}, {QStringLiteral("sessions"), 2},
                                                          {QStringLiteral("last_played_at"), QStringLiteral("2026-10-22T10:00:00Z")},
                                                          {QStringLiteral("favorite"), false}, {QStringLiteral("collection_ids"), QJsonArray()}});
    games.replace(0, first);
    hub.games = QJsonObject{{QStringLiteral("games"), games}};
    hub.features = QStringList{QStringLiteral("saves_v1"), QStringLiteral("uploads_v1"), QStringLiteral("local_roms_v1")};
    hub.handshakeExtra = QJsonObject{
        {QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), QStringLiteral("u_1")}, {QStringLiteral("display_name"), QStringLiteral("admin")},
                                             {QStringLiteral("role"), QStringLiteral("admin")}}},
        {QStringLiteral("problems"),
         QJsonArray{QJsonObject{{QStringLiteral("code"), QStringLiteral("core_version_mismatch")},
                                {QStringLiteral("detail"), QStringLiteral("Core melonds_ds 1.3.0, expected 1.4.0")},
                                {QStringLiteral("core_id"), QStringLiteral("melonds_ds")}},
                    QJsonObject{{QStringLiteral("code"), QStringLiteral("core_missing")},
                                {QStringLiteral("detail"), QStringLiteral("Core mgba is not installed")},
                                {QStringLiteral("core_id"), QStringLiteral("mgba")}}}}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub, 1280);
    h.window->resize(1280, 800);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "libraryNotice"), 5000);
    QTest::qWait(300);
    QQuickTest::qWaitForPolish(h.window);

    // One notices line; the further notices wait behind "+N more".
    QCOMPARE(visibleCount(h, "libraryNotice"), 1);
    QVERIFY(shown(h, "libraryNoticeMore"));
    QVERIFY(textOf(h, "libraryNoticeMore").startsWith(QStringLiteral("+")));
    QVERIFY(h.controller->coreWarnings().size() >= 2);
    QCOMPARE(h.item("libraryNotice")->height(), 32.0);
    QVERIFY(shown(h, "libraryNoticeAction"));  // admin: the link to Systems & cores
    QVERIFY(h.click("libraryNoticeMore"));
    QTRY_VERIFY(visibleCount(h, "libraryNotice") >= 2);
    QVERIFY(h.click("libraryNoticeMore"));
    QTRY_COMPARE(visibleCount(h, "libraryNotice"), 1);

    // The page: header action, one toolbar row, Continue (one card) and "All games".
    QVERIFY(shown(h, "uploadButton"));
    QCOMPARE(textOf(h, "uploadButton"), QStringLiteral("Add games…"));
    QVERIFY(shown(h, "searchField") && shown(h, "scopeChip_all") && shown(h, "sortControl") && shown(h, "libraryViewSwitch"));
    QVERIFY(h.item("statusChip_all") == nullptr && !shown(h, "readyFirstToggle"));  // no status chips, Ready first sits in the sort menu
    const auto sceneRect = [&](const char* name) { QQuickItem* it = h.item(name); return it->mapRectToScene(it->boundingRect()); };
    const QRectF chip = sceneRect("scopeChip_all"), sort = sceneRect("sortControl"), toolbar = sceneRect("libraryToolbar");
    QVERIFY2(qAbs(chip.center().y() - sort.center().y()) < 4 && toolbar.height() <= 40, "chips and sort share one toolbar row");
    QVERIFY(shown(h, "continueCard"));
    QCOMPARE(visibleCount(h, "continueCard") + visibleCount(h, "recentEntry"), 1);
    QCOMPARE(h.item("continueCard")->height(), 64.0);
    QVERIFY(shown(h, "libraryAllLabel"));

    // At least seven game rows are inside the list viewport (no scrolling).
    QQuickItem* scroll = h.item("gameScroll");
    QVERIFY(scroll != nullptr);
    const QRectF view = scroll->mapRectToScene(scroll->boundingRect());
    int rows = 0;
    for (QQuickItem* t : h.items("tileTitle")) {
      if (!t->isVisible() || t->window() != h.window) continue;
      const QRectF r = t->mapRectToScene(t->boundingRect());
      if (r.top() >= view.top() - 0.5 && r.bottom() <= view.bottom() + 0.5) ++rows;
    }
    QVERIFY2(rows >= 7, qPrintable(QStringLiteral("%1 rows visible").arg(rows)));
    uitest::saveShot(h.window, QStringLiteral("library-uncluttered"));
  }

  void listAndGridSwitchPersists() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 7);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub, 1280);
    QCOMPARE(h.controller->libraryView(), QStringLiteral("list"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "libraryViewSwitch"), 5000);
    QTRY_VERIFY(visibleCount(h, "tileTitle") > 0);

    QVERIFY(h.click("viewGrid"));
    QTRY_COMPARE(h.controller->libraryView(), QStringLiteral("grid"));
    QCOMPARE(PlayerSettings(h.controller->profileStore()->baseDir()).libraryView(), QStringLiteral("grid"));
    // Five tiles per row at 1280 and nothing else beside the grid until a game is chosen.
    QTRY_VERIFY_WITH_TIMEOUT(visibleCount(h, "tileArt") >= 5, 5000);
    QVector<qreal> ys;
    for (QQuickItem* t : h.items("tileArt")) {
      if (t->isVisible() && t->window() == h.window) ys << qRound(t->mapToScene(QPointF(0, 0)).y());
    }
    const qreal firstY = *std::min_element(ys.begin(), ys.end());
    QCOMPARE(int(std::count(ys.begin(), ys.end(), firstY)), 5);
    QVERIFY(!shown(h, "playButton"));

    // Choosing a tile opens the detail column; Esc closes it.
    QVERIFY(h.click("gameTile_g2"));
    QTRY_VERIFY(shown(h, "playButton"));
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("g2"));

    // The choice is stored in the profile settings.
    QCOMPARE(PlayerSettings(h.controller->profileStore()->baseDir()).libraryView(), QStringLiteral("grid"));
    QVERIFY(h.click("viewList"));
    QTRY_COMPARE(h.controller->libraryView(), QStringLiteral("list"));
    QCOMPARE(PlayerSettings(h.controller->profileStore()->baseDir()).libraryView(), QStringLiteral("list"));
    QTRY_VERIFY(shown(h, "playButton"));
  }

  void oneProblemSentenceWithTheFixAsPrimary() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 2);
    hub.adminUser = false;
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub, 1400);
    h.controller->selectGame(QStringLiteral("g1"));
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->selectedGameId(), QStringLiteral("g1"), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "playButton"), 5000);
    const QVariantMap g = h.controller->selectedGame();
    QVERIFY2(!g.value(QStringLiteral("problemText")).toString().isEmpty(), "no core: the game page names the problem");
    QCOMPARE(g.value(QStringLiteral("fixKind")).toString(), QStringLiteral("admin"));
    // One sentence, one fix: the problem line shows once, the fix is the primary, the checklist stays away.
    QTRY_VERIFY(shown(h, "detailProblem"));
    QCOMPARE(visibleCount(h, "detailProblem"), 1);
    QCOMPARE(visibleCount(h, "playDisabledReason"), 1);
    QCOMPARE(textOf(h, "playButton"), QStringLiteral("Ask an admin"));
    QVERIFY(h.item("playButton")->property("enabled").toBool());
    QVERIFY(!shown(h, "startChecklist"));
    // The Saves tab keeps the same primary while the game cannot start; Overview / Saves / Details are one tab row.
    QVERIFY(h.click("tab_Saves"));
    QCOMPARE(textOf(h, "playButton"), QStringLiteral("Ask an admin"));
    QVERIFY(!shown(h, "startChecklist"));
    // File details stay closed by default.
    QVERIFY(!shown(h, "fileDetails") || !h.item("fileDetails")->property("visible").toBool());
    QVERIFY(h.click("tab_Overview"));

  }

  void removeFromLibraryOffersUndo() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QByteArray local = "homebrew-overview-local-only";
    {
      QFile out(folder.filePath(QStringLiteral("overview-local.nds")));
      QVERIFY(out.open(QIODevice::WriteOnly));
      out.write(local);
    }
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 1);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub, 1400);
    h.controller->addLibraryFolder(folder.path());
    QTRY_VERIFY_WITH_TIMEOUT(!hub.registerBodies.isEmpty(), 15000);
    QString localId;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
          h.controller->selectGame(QStringLiteral("reg-1"));
          if (h.controller->selectedGame().value(QStringLiteral("localFile")).toBool()) localId = QStringLiteral("reg-1");
          return !localId.isEmpty();
        }(),
        10000);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "detailMoreButton"), 5000);
    const int registered = hub.registerBodies.size();

    QVERIFY(h.click("detailMoreButton"));
    QTRY_VERIFY(shown(h, "menuItem_removeFromLibrary"));
    QVERIFY(h.click("menuItem_removeFromLibrary"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "removedToast"), 8000);
    QVERIFY(textOf(h, "toastText").contains(QStringLiteral("Removed from library")));
    QVERIFY(textOf(h, "toastText").contains(QStringLiteral("the file stays on this PC")));
    QVERIFY(hub.unregisteredGames.contains(localId));
    QVERIFY(QFile::exists(folder.filePath(QStringLiteral("overview-local.nds"))));  // nothing is deleted from disk

    QVERIFY(h.click("toastAction"));
    QTRY_VERIFY_WITH_TIMEOUT(!shown(h, "removedToast"), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(hub.registerBodies.size() > registered, 15000);  // the entry is registered again
    QVERIFY(QFile::exists(folder.filePath(QStringLiteral("overview-local.nds"))));
  }
};

QTEST_MAIN(LibraryOverviewUiTest)
#include "library_overview_ui_test.moc"
