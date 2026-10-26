// Game in the background (0.7.x): "← Library" pauses and keeps the game loaded; Library strip, Resume, Quit, the
// confirmation before another game starts, and no input to the core while in the background (preview game, no core).
#include <QDir>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;

class BackgroundGameUiTest : public QObject {
  Q_OBJECT

  static void pair(FakeHub& hub, Harness& h) {
    hub.hubId = QStringLiteral("hub-bg");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    hub.games = QJsonObject{{QStringLiteral("games"),
                             QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"), uitest::sha256Hex("a"), 1),
                                        uitest::gameJson(QStringLiteral("g2"), QStringLiteral("Night Run"), uitest::sha256Hex("b"), 1)}}};
    QVERIFY(hub.start());
    QVERIFY(h.start());
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
  }

  static void startPreview(Harness& h, const QString& gameId) {
    h.controller->selectGame(gameId);
    h.controller->adoptPreviewGame(gameId);
    GameSession* gs = h.controller->gameSession();
    QImage frame(256, 384, QImage::Format_RGB32);
    frame.fill(QColor(40, 90, 140));
    gs->setPreview(QStringLiteral("Preview"), frame, emu::DisplayProfile(), EmulationDiagnostics());
    emit gs->started();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 8000);
  }


  static QList<QQuickItem*> itemsNamed(Harness& h, const char* name) {
    QList<QQuickItem*> all{h.window->contentItem()}, out;
    for (int i = 0; i < all.size(); ++i) all.append(all.at(i)->childItems());
    for (QQuickItem* it : std::as_const(all)) if (it->objectName() == QLatin1String(name)) out.append(it);
    return out;
  }
  static QRectF sceneRect(QQuickItem* it) { return it->mapRectToScene(QRectF(0, 0, it->width(), it->height())); }
  static QQuickItem* visibleOne(Harness& h, const char* name) {
    QQuickItem* found = nullptr;
    for (QQuickItem* it : itemsNamed(h, name)) {
      if (!it->isVisible()) continue;
      if (found != nullptr) return nullptr;  // more than one
      found = it;
    }
    return found;
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
  static void inside(Harness& h, const char* name, const QRectF& bounds) {
    QQuickItem* it = visibleOne(h, name);
    QVERIFY2(it != nullptr, name);
    const QRectF r = sceneRect(it);
    QVERIFY2(r.left() >= bounds.left() - 0.5 && r.right() <= bounds.right() + 0.5 && r.top() >= bounds.top() - 0.5 && r.bottom() <= bounds.bottom() + 0.5,
             qPrintable(QStringLiteral("'%1' %2,%3 %4x%5 outside %6,%7 %8x%9").arg(QLatin1String(name)).arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())
                            .arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height())));
  }
  static void shot(Harness& h, const QString& name) {
    const QString dir = qEnvironmentVariable("UZUME_SCREENSHOT_DIR");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    QTest::qWait(150);
    h.window->grabWindow().save(QDir(dir).filePath(name + QStringLiteral(".png")));
  }
  // Layout checks of the running game's Library (3c-5) at the current window size.
  static void checkRunningLayout(Harness& h) {
    const QRectF win(0, 0, h.window->width(), h.window->height());
    QQuickItem* pill = visibleOne(h, "tileRunningPill");
    QVERIFY2(pill != nullptr, "exactly one running marker expected");
    QQuickItem* tile = pill;
    while (tile != nullptr && !tile->property("gameId").isValid()) tile = tile->parentItem();
    QVERIFY(tile != nullptr);
    QCOMPARE(tile->property("gameId").toString(), QStringLiteral("g1"));
    const QRectF tr = sceneRect(tile);
    const QRectF pr = sceneRect(pill);
    // The "Paused" tag sits in the title cell of the table row, inside the row.
    QVERIFY2(pr.left() >= tr.left() - 0.5 && pr.right() <= tr.right() + 0.5 && pr.top() >= tr.top() - 0.5 && pr.bottom() <= tr.bottom() + 0.5,
             "marker outside the row");
    QQuickItem* pane = h.item("detailPane");
    QVERIFY(pane != nullptr);
    const QRectF pb = sceneRect(pane);
    QVERIFY(pb.right() <= win.right() + 0.5);
    for (const char* n : {"playButton", "playShareButton", "runningNote", "quitNote", "detailPill", "detailTitle"}) inside(h, n, pb);
    inside(h, "nowRunningStrip", win);
    QVERIFY(h.item("startChecklist") == nullptr || !h.item("startChecklist")->isVisible());
  }

 private slots:
  void initTestCase() { uitest::installWarningCounter(); }

  void backgroundResumeQuit() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h);
    PlayerController* c = h.controller.get();
    GameSession* gs = c->gameSession();
    startPreview(h, QStringLiteral("g1"));
    gs->setKeyboardMap({{Qt::Key_A, 1u << 8}});
    QVERIFY(gs->keyEvent(Qt::Key_A, true));
    gs->keyEvent(Qt::Key_A, false);
    QVERIFY(c->backgroundGame().isEmpty());

    // "← Library": paused, still loaded, Library page shown.
    c->leaveGameView();
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QVERIFY(gs->isActive());
    QTRY_VERIFY(gs->isPaused());
    QCOMPARE(c->backgroundGame().value(QStringLiteral("id")).toString(), QStringLiteral("g1"));
    QCOMPARE(c->backgroundGame().value(QStringLiteral("title")).toString(), QStringLiteral("Lumen Drift"));
    QQuickTest::qWaitForPolish(h.window);
    // The interim Library strip is gone; the sidebar "Now running" strip shows the game (3c-5).
    QVERIFY(h.item("runningStrip") == nullptr && h.item("stripResumeButton") == nullptr && h.item("stripQuitButton") == nullptr);
    QQuickItem* strip = h.item("nowRunningStrip");
    QVERIFY(strip != nullptr && strip->isVisible());
    QVERIFY(h.item("nowRunningResume") != nullptr && h.item("nowRunningQuit") != nullptr);
    const QVariantMap sel = c->selectedGame();
    QCOMPARE(sel.value(QStringLiteral("running")).toBool(), true);
    QCOMPARE(sel.value(QStringLiteral("playLabel")).toString(), QStringLiteral("Resume"));
    QQuickItem* second = h.item("playShareButton");
    QVERIFY(second != nullptr && second->isVisible());

    // Input does not reach the core; hotkeys and Esc do not act on the background game.
    QVERIFY(!gs->keyEvent(Qt::Key_A, true));
    gs->setGamepadMask(1u << 3);
    QCOMPARE(gs->joypadMask(), 0u);
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTest::keyClick(h.window, Qt::Key_Space);
    QTest::qWait(50);
    QVERIFY(gs->isPaused());

    // Other pages stay usable, the game stays in the background.
    c->showSettings();
    QCOMPARE(c->screen(), QStringLiteral("settings"));
    c->showEmulation();
    QCOMPARE(c->screen(), QStringLiteral("emulation"));
    c->showControllers();
    QCOMPARE(c->screen(), QStringLiteral("controllers"));
    c->showLibrary();
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QVERIFY(gs->isActive());

    // Resume: back in the game view, running again, input works again.
    c->resumeGame();
    QCOMPARE(c->screen(), QStringLiteral("game"));
    QTRY_VERIFY(!gs->isPaused());
    QVERIFY(c->backgroundGame().isEmpty());
    QCOMPARE(gs->joypadMask(), 1u << 3);
    QVERIFY(gs->keyEvent(Qt::Key_A, true));
    gs->keyEvent(Qt::Key_A, false);

    // Back to the background and quit explicitly.
    c->leaveGameView();
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QVERIFY(!c->backgroundGame().isEmpty());
    c->quitGame();
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QVERIFY(!gs->isActive());
    QVERIFY(c->backgroundGame().isEmpty());
    QVERIFY(c->selectedGame().value(QStringLiteral("running")).toBool() == false);
    QQuickTest::qWaitForPolish(h.window);
    QQuickItem* gone = h.item("nowRunningStrip");
    QVERIFY(gone == nullptr || !gone->isVisible());
    QCOMPARE(uitest::warningCount().load(), 0);
  }

  void startOtherGameAsksFirst() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h);
    PlayerController* c = h.controller.get();
    GameSession* gs = c->gameSession();
    startPreview(h, QStringLiteral("g1"));
    c->leaveGameView();
    QVERIFY(gs->isActive());

    // The running game itself: Play resumes it, no question.
    c->playSelected();
    QCOMPARE(c->screen(), QStringLiteral("game"));
    c->leaveGameView();

    // Another game: confirmation, Cancel keeps everything as it is.
    c->selectGame(QStringLiteral("g2"));
    QCOMPARE(c->selectedGame().value(QStringLiteral("running")).toBool(), false);
    c->playSelected();
    QVERIFY(c->startConfirm().value(QStringLiteral("active")).toBool());
    QCOMPARE(c->startConfirm().value(QStringLiteral("runningTitle")).toString(), QStringLiteral("Lumen Drift"));
    QCOMPARE(c->startConfirm().value(QStringLiteral("newTitle")).toString(), QStringLiteral("Night Run"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.item("startConfirmDialog") != nullptr && h.item("startConfirmDialog")->isVisible());
    QVERIFY(h.click("startConfirmCancel"));
    QTRY_VERIFY(!c->startConfirm().value(QStringLiteral("active")).toBool());
    QVERIFY(gs->isActive());
    QVERIFY(!c->backgroundGame().isEmpty());

    // Confirm: the running game quits first.
    c->playSelected();
    QVERIFY(c->startConfirm().value(QStringLiteral("active")).toBool());
    c->confirmQuitAndStart();
    QVERIFY(c->backgroundGame().isEmpty());
    QVERIFY(!gs->isActive() || gs->state() == GameSession::Starting);
    QVERIFY(!c->startConfirm().value(QStringLiteral("active")).toBool());
    QCOMPARE(uitest::warningCount().load(), 0);
  }
  void libraryShowsRunningGame() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h);
    PlayerController* c = h.controller.get();
    startPreview(h, QStringLiteral("g1"));
    c->leaveGameView();
    c->selectGame(QStringLiteral("g1"));
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QQuickTest::qWaitForPolish(h.window);

    // Marker on the right tile only, status line, detail column with Resume / Quit game, no interim strip.
    QVERIFY(h.item("runningStrip") == nullptr);
    const QList<QQuickItem*> statuses = itemsNamed(h, "tileStatus");
    int running = 0;
    for (QQuickItem* st : statuses) {
      if (!st->isVisible() || !st->property("text").toString().contains(QStringLiteral("Running · paused"))) continue;
      ++running;
      QQuickItem* t = st;
      while (t != nullptr && !t->property("gameId").isValid()) t = t->parentItem();
      QVERIFY(t != nullptr);
      QCOMPARE(t->property("gameId").toString(), QStringLiteral("g1"));
    }
    QCOMPARE(running, 1);
    QCOMPARE(h.item("detailPill")->property("text").toString(), QStringLiteral("❚❚ Running · paused"));
    QVERIFY(h.item("playButton")->property("text").toString().contains(QStringLiteral("Resume")));
    QCOMPARE(h.item("playShareButton")->property("text").toString(), QStringLiteral("Quit game"));
    QVERIFY(h.item("playShareButton")->isVisible());
    QVERIFY(h.item("quitNote")->isVisible() && h.item("runningNote")->isVisible());

    struct Size { int w, h; };
    for (const Size sz : {Size{960, 600}, Size{1280, 800}, Size{1920, 1080}}) {
      h.window->resize(sz.w, sz.h);
      QQuickTest::qWaitForPolish(h.window);
      QTest::qWait(60);
      checkRunningLayout(h);
      if (sz.w == 1920) shot(h, QStringLiteral("3c-5-library-running-1920"));
    }
    h.window->resize(1440, 900);
    QQuickTest::qWaitForPolish(h.window);
    shot(h, QStringLiteral("3c-5-library-running"));
    // Wide Windows fonts at the narrowest window.
    h.window->resize(960, 600);
    widenFonts(h, 3.0);
    checkRunningLayout(h);
    QCOMPARE(uitest::warningCount().load(), 0);
  }

  void playOnAnotherGameAsksAndCancelKeepsRunning() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h);
    PlayerController* c = h.controller.get();
    GameSession* gs = c->gameSession();
    startPreview(h, QStringLiteral("g1"));
    c->leaveGameView();
    c->selectGame(QStringLiteral("g2"));
    QQuickTest::qWaitForPolish(h.window);
    // Another game selected: plain detail column (Play), no running marker on its tile.
    QVERIFY(!h.item("playButton")->property("text").toString().contains(QStringLiteral("Resume")));
    QVERIFY(h.item("quitNote") == nullptr || !h.item("quitNote")->isVisible());
    c->playSelected();  // what the Play button does for this game (the dummy ROM is not downloaded, so the button itself is not clickable)
    QQuickItem* dlg = h.item("startConfirmDialog");
    QTRY_VERIFY(dlg != nullptr && dlg->isVisible());
    QCOMPARE(h.item("startConfirm")->property("label").toString(), QStringLiteral("Lumen Drift is running · paused"));
    QCOMPARE(h.item("startConfirm")->property("title").toString(), QStringLiteral("Quit Lumen Drift and start Night Run?"));
    QVERIFY(h.item("startConfirmQuit")->property("text").toString().contains(QStringLiteral("Night Run")));
    struct Size { int w, h; };
    for (const Size sz : {Size{960, 600}, Size{1280, 800}, Size{1920, 1080}}) {
      h.window->resize(sz.w, sz.h);
      QQuickTest::qWaitForPolish(h.window);
      QTest::qWait(60);
      const QRectF win(0, 0, sz.w, sz.h);
      for (const char* n : {"startConfirmDialog", "startConfirmCancel", "startConfirmQuit"}) inside(h, n, win);
      const QRectF dr = sceneRect(h.item("startConfirmDialog"));
      QVERIFY2(dr.width() <= 400.5, "dialog wider than 400");
      inside(h, "startConfirmQuit", dr);
    }
    h.window->resize(1440, 900);
    QQuickTest::qWaitForPolish(h.window);
    shot(h, QStringLiteral("3c-5-quit-and-start-dialog"));
    h.window->resize(960, 600);
    widenFonts(h, 10.0);  // far wider than any Windows UI font: the confirm button must elide, never spill out of the dialog
    for (const char* n : {"startConfirmCancel", "startConfirmQuit"}) inside(h, n, sceneRect(h.item("startConfirmDialog")));
    // The buttons stack when they do not fit side by side: the confirm first, Cancel below it.
    const QRectF quitR = sceneRect(h.item("startConfirmQuit")), cancelR = sceneRect(h.item("startConfirmCancel"));
    QVERIFY(quitR.left() >= cancelR.right() - 0.5 || quitR.bottom() <= cancelR.top() + 0.5);
    h.window->resize(1280, 800);
    QQuickTest::qWaitForPolish(h.window);

    // Cancel: nothing changes, the game keeps running in the background.
    QVERIFY(h.click("startConfirmCancel"));
    QTRY_VERIFY(!c->startConfirm().value(QStringLiteral("active")).toBool());
    QVERIFY(gs->isActive() && gs->isPaused());
    QCOMPARE(c->backgroundGame().value(QStringLiteral("id")).toString(), QStringLiteral("g1"));
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(!h.item("startConfirmDialog")->isVisible());
    QVERIFY(visibleOne(h, "tileRunningPill") != nullptr);
    QCOMPARE(uitest::warningCount().load(), 0);
  }
};

QTEST_MAIN(BackgroundGameUiTest)
#include "background_game_ui_test.moc"
