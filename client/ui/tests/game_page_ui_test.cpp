// The game page (usability pass 5): a row of the Library opens it instead of a side pane, Esc returns to the same row and scroll
// position, four tabs (Overview, Saves, Settings, Manage), the header with at most three actions plus the ⋯ menu, the admin
// actions under Manage and "Game overrides" of Settings > Emulation landing on the Settings tab. Offscreen, FakeHub, no core needed;
// synthetic data only.
#include <QJsonArray>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

class GamePageUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub, int width = 1280, int height = 800) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    h.window->resize(width, height);
  }
  static void seed(FakeHub& hub, int n, bool admin = false) {
    const QByteArray rom = "homebrew-dummy-rom-game-page";
    hub.adminUser = admin;
    hub.adminDirectory = admin;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("local_roms_v1"), QStringLiteral("runs_v1")};
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    QJsonArray games;
    for (int i = 1; i <= n; ++i) {
      games.append(uitest::gameJson(QStringLiteral("g%1").arg(i), QStringLiteral("Game %1").arg(i), uitest::sha256Hex(rom), rom.size()));
    }
    hub.games = QJsonObject{{QStringLiteral("games"), games}};
  }
  static bool shown(const Harness& h, const char* name) {
    for (QQuickItem* it : h.items(name)) {
      if (it->isVisible() && it->window() == h.window) return true;
    }
    return false;
  }
  static QString textOf(const Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr ? it->property("text").toString() : QString();
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // A click on a row opens the page: no list, no toolbar beside it; "‹ Library" and Esc lead back.
  void rowClickOpensThePage() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 5);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "gameTile_g3"), 5000);
    QVERIFY(!shown(h, "gamePage"));
    QVERIFY(!h.controller->gamePageOpen());

    QVERIFY(h.click("gameTile_g3"));
    QTRY_VERIFY(shown(h, "gamePage"));
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("g3"));
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));   // the page lives in the Library screen
    QCOMPARE(textOf(h, "detailTitle"), QStringLiteral("Game 3"));
    QVERIFY(!shown(h, "gameGrid"));                                // the list is not beside it
    QVERIFY(!shown(h, "libraryToolbar"));
    QVERIFY(!shown(h, "detailPane"));
    // The page fills the main column: it is much wider than the old 340 px side pane.
    QQuickItem* page = h.item("gamePage");
    QVERIFY(page->width() > 800);
    uitest::saveShot(h.window, QStringLiteral("game-page-overview"));

    // "‹ Library" returns to the list.
    QVERIFY(h.click("gameBack"));
    QTRY_VERIFY(!shown(h, "gamePage"));
    QTRY_VERIFY(shown(h, "gameTile_g3"));
    QVERIFY(shown(h, "libraryToolbar"));
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("g3"));

    // Enter on a focused row opens it too.
    QQuickItem* tile = h.item("gameTile_g2");
    QVERIFY(tile != nullptr);
    tile->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(h.window, Qt::Key_Return);
    QTRY_VERIFY(shown(h, "gamePage"));
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("g2"));
  }

  // Esc closes the page with the same row selected and the list at the same scroll position.
  void escReturnsWithTheSelectionAndScrollKept() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 40);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "gameScroll"), 5000);
    QQuickItem* scroll = h.item("gameScroll");
    QTRY_VERIFY(scroll->property("contentHeight").toReal() > scroll->height() + 400);
    scroll->setProperty("contentY", 400.0);
    QTRY_VERIFY(scroll->property("contentY").toReal() > 100);
    QQuickItem* pageScroll = h.item("libraryPageScroll");
    const qreal pageY = pageScroll->property("contentY").toReal();
    QTest::qWait(100);
    QQuickItem* scrollNow = h.item("gameScroll");
    const qreal scrolledY = scrollNow->property("contentY").toReal();
    QVERIFY(scrolledY > 100);
    const QRectF view = scrollNow->mapRectToScene(QRectF(0, 0, scrollNow->width(), scrollNow->height()));
    QString pick;
    for (int i = 1; i <= 40 && pick.isEmpty(); ++i) {
      const QByteArray n = QStringLiteral("gameTile_g%1").arg(i).toUtf8();
      for (QQuickItem* it : h.items(n.constData())) {
        const QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
        if (it->isVisible() && view.contains(r)) { pick = QStringLiteral("g%1").arg(i); break; }
      }
    }
    QVERIFY(!pick.isEmpty());
    const QByteArray tileName = ("gameTile_" + pick).toUtf8();
    QVERIFY(h.click(tileName.constData()));
    QTRY_VERIFY(shown(h, "gamePage"));
    QCOMPARE(h.controller->selectedGameId(), pick);
    QTest::qWait(60);
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "gamePage"));
    QVERIFY(!h.controller->gamePageOpen());
    QTRY_VERIFY(shown(h, "gameScroll"));
    QCOMPARE(h.item("gameScroll")->property("contentY").toReal(), scrolledY);
    QCOMPARE(h.item("libraryPageScroll")->property("contentY").toReal(), pageY);
    QTRY_VERIFY(shown(h, tileName.constData()));
    QVERIFY(h.item(tileName.constData())->property("selected").toBool());
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
  }

  // Four tabs; Settings embeds the settings of this game.
  void fourTabsAndTheSettingsOfThisGame() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 3);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QVERIFY(h.openGame(QStringLiteral("g2")));
    QCOMPARE(h.item("detailTabs")->property("items").toStringList(),
             (QStringList{QStringLiteral("Overview"), QStringLiteral("Saves"), QStringLiteral("Settings"), QStringLiteral("Manage")}));
    QCOMPARE(h.item("detailTabs")->property("size").toString(), QStringLiteral("lg"));
    QCOMPARE(h.item("detailTabs")->property("active").toString(), QStringLiteral("Overview"));
    QTRY_VERIFY(shown(h, "saveSummary"));
    QVERIFY(shown(h, "howItPlays"));
    QVERIFY(!shown(h, "gameSettingsContent"));
    QVERIFY(!shown(h, "detailFacts"));

    QVERIFY(h.click("tab_Saves"));
    QTRY_VERIFY(shown(h, "savesView"));
    QVERIFY(!shown(h, "saveSummary"));

    QVERIFY(h.click("tab_Settings"));
    QTRY_VERIFY(shown(h, "gameSettingsContent"));
    QCOMPARE(h.item("gameSettingsContent")->property("gameId").toString(), QStringLiteral("g2"));
    QVERIFY(!shown(h, "savesView"));
    QVERIFY(shown(h, "gamePageResetAll"));
    QVERIFY(h.item("gameSettingsContent")->property("tabs").toList().size() >= 1);
    uitest::saveShot(h.window, QStringLiteral("game-page-settings"));

    QVERIFY(h.click("tab_Manage"));
    QTRY_VERIFY(shown(h, "detailFacts"));
    QVERIFY(!shown(h, "gameSettingsContent"));
    uitest::saveShot(h.window, QStringLiteral("game-page-manage"));

    QVERIFY(h.click("tab_Overview"));
    QTRY_VERIFY(shown(h, "saveSummary"));
    // "Change" on a row of "How it plays" leads to the Settings tab.
    QTRY_VERIFY(shown(h, "playRowChange_speedup") || shown(h, "playRowChange_core"));
    QVERIFY(h.click(shown(h, "playRowChange_speedup") ? "playRowChange_speedup" : "playRowChange_core"));
    QTRY_VERIFY(shown(h, "gameSettingsContent"));
    QCOMPARE(h.item("detailTabs")->property("active").toString(), QStringLiteral("Settings"));
  }

  // The header carries at most three actions plus the ⋯ menu; the rest is in the menu.
  void headerHasThreeActionsAndAMenu() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 2);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QVERIFY(h.openGame(QStringLiteral("g1")));
    QQuickItem* actions = h.item("gameActions");
    QVERIFY(actions != nullptr);
    QStringList visibleButtons;
    for (QQuickItem* c : actions->childItems()) {
      if (c->isVisible() && !c->objectName().isEmpty()) visibleButtons << c->objectName();
    }
    QVERIFY2(visibleButtons.contains(QStringLiteral("playButton")), qPrintable(visibleButtons.join(' ')));
    // The menu button only shows when the menu has entries (add to collection, play and share): none for this plain game.
    visibleButtons.removeAll(QStringLiteral("detailMoreButton"));
    QVERIFY2(visibleButtons.size() <= 3, qPrintable(visibleButtons.join(' ')));
    QCOMPARE(textOf(h, "playButton").isEmpty(), false);
    QVERIFY(!shown(h, "addToCollection"));   // not buttons: they are items of the menu
    QVERIFY(!shown(h, "playShareButton"));
    QVERIFY(!shown(h, "uploadToHubButton"));
    QCOMPARE(textOf(h, "startRunButton"), QStringLiteral("Start a run…"));

    QVERIFY(!shown(h, "menuItem_gameSettings"));   // the settings are a tab now, not a menu entry
  }

  // Admin actions live under Manage, not under the list and not on the Overview.
  void adminActionsAreUnderManage() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 2, true);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub, 1400, 1000);
    QTRY_VERIFY(h.controller->admin()->available());
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "gameTile_g1"), 5000);
    QVERIFY(!shown(h, "libraryAdminDetails"));   // nothing under the list any more
    QVERIFY(h.click("gameTile_g1"));
    QTRY_VERIFY(shown(h, "gamePage"));
    QVERIFY(!shown(h, "libraryAdminRename"));
    QVERIFY(!shown(h, "libraryAdminDelete"));
    QVERIFY(h.click("tab_Manage"));
    QTRY_VERIFY(shown(h, "libraryAdminRename"));
    QVERIFY(shown(h, "libraryAdminDelete"));
    QVERIFY(shown(h, "libraryAdminSha"));
    QVERIFY(h.click("tab_Settings"));
    QTRY_VERIFY(!shown(h, "libraryAdminRename"));
  }

  // The MCP / controller entry: the screen stays "library".
  void controllerOpensAndClosesThePage() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 3);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->openGamePage(QStringLiteral("g2"), QStringLiteral("settings"));
    QTRY_VERIFY(shown(h, "gameSettingsContent"));
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
    QCOMPARE(h.item("detailTabs")->property("active").toString(), QStringLiteral("Settings"));
    h.controller->openGamePage(QStringLiteral("g2"), QStringLiteral("manage"));
    QTRY_VERIFY(shown(h, "detailFacts"));
    // Another screen and back: the page is still there; the Library entry again leads to the list.
    h.controller->showSettings();
    QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
    h.controller->showLibrary();
    QTRY_VERIFY(shown(h, "gamePage"));
    h.controller->showLibrary();
    QTRY_VERIFY(!shown(h, "gamePage"));
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("g2"));
    // An unknown tab opens the Overview.
    h.controller->openGamePage(QStringLiteral("g1"), QStringLiteral("nonsense"));
    QCOMPARE(h.controller->gamePageTab(), QStringLiteral("overview"));
  }
};

UITEST_MAIN(GamePageUiTest)
#include "game_page_ui_test.moc"
