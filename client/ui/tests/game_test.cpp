// Game view with the real melonDS DS core and the homebrew test ROM (NEEDS_CORE: QSKIP without a core). Also built as ui_game_testcore against the Uzume test core.
#include <QFile>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "gameview.h"
#include "screenlayout.h"
#include "variantcache.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;

class GameTest : public QObject {
  Q_OBJECT
 private slots:
  // 0.19 package 6: the view draws every layout (and swap) and sends touches to the bottom screen wherever it is drawn.
  void viewLayoutsSwapAndTouch() {
    for (const bool threeDs : {false, true}) {
      emu::DisplayProfile profile;
      profile.layout = QStringLiteral("vertical");
      if (threeDs) {
        profile.screens = {{QStringLiteral("top"), 400, 240, false, QStringLiteral("center")}, {QStringLiteral("bottom"), 320, 240, true, QStringLiteral("center")}};
      } else {
        profile.screens = {{QStringLiteral("top"), 256, 192, false}, {QStringLiteral("bottom"), 256, 192, true}};
      }
      GameSession gs;
      QImage frame(profile.frameSize(), QImage::Format_RGB32);
      frame.fill(QColor(40, 90, 140));
      gs.setPreview(QStringLiteral("t"), frame, profile, EmulationDiagnostics());
      QQuickWindow window;
      window.resize(1100, 800);
      GameView view;
      view.setParentItem(window.contentItem());
      view.setSize(QSizeF(1000, 700));
      view.setIntegerScale(false);
      view.setSession(&gs);
      for (const QString& layout : screenLayoutsFor(2)) {
        for (const bool swap : {false, true}) {
          view.setLayout(layout);
          view.setSwap(swap);
          const ScreenPlacement pl = view.placement();
          const QString tag = QStringLiteral("%1 swap=%2 3ds=%3").arg(layout).arg(swap).arg(threeDs);
          QCOMPARE(pl.targets.size(), 2);
          QVERIFY2(QRectF(0, 0, 1000, 700).contains(view.frameRect()), qPrintable(tag));
          const bool topHidden = layout == QLatin1String("bottom"), bottomHidden = layout == QLatin1String("top");
          QCOMPARE(pl.targets[0].isEmpty(), topHidden);
          QCOMPARE(pl.targets[1].isEmpty(), bottomHidden);
          if (layoutSupportsSwap(layout) && swap && layout == QLatin1String("stacked")) QVERIFY2(pl.targets[1].bottom() <= pl.targets[0].top() + 0.5, qPrintable(tag));
          const auto hit = view.touchTarget(pl.targets[1].center(), false);
          if (bottomHidden) {
            QVERIFY2(!hit.has_value(), qPrintable(tag));  // the touch screen is not drawn: nothing to touch
            continue;
          }
          QVERIFY2(hit.has_value(), qPrintable(tag));
          QVERIFY(qAbs(hit->x() - 0.5) < 1e-6 && qAbs(hit->y() - 0.75) < 1e-6);
          if (!topHidden) QVERIFY2(!view.touchTarget(pl.targets[0].center(), false).has_value(), qPrintable(tag));
        }
      }
      // Unknown layouts fall back to stacked; the old stored values stay valid.
      view.setSwap(false);
      view.setLayout(QStringLiteral("bogus"));
      QCOMPARE(view.layout(), QStringLiteral("stacked"));
      for (const char* old : {"stacked", "side", "top"}) {
        view.setLayout(QString::fromLatin1(old));
        QCOMPARE(view.layout(), QString::fromLatin1(old));
      }
      // The readback limit follows the screens that are really drawn (bottom only: the bottom screen at its size).
      view.setLayout(QStringLiteral("bottom"));
      const qreal dpr = window.effectiveDevicePixelRatio();
      const QRectF b = view.placement().targets[1];
      QCOMPARE(gs.readbackLimit().width(), int(qCeil(profile.frameSize().width() * (b.width() / profile.screens[1].width) * dpr - 1e-6)));
    }
  }

  // The view reports the physical pixel size it shows the frame at; the session derives the readback limit.
  void viewReportsPhysicalSizeToSession() {
    emu::DisplayProfile profile;
    profile.layout = QStringLiteral("vertical");
    profile.gap = 0;
    profile.screens = {{QStringLiteral("top"), 256, 192, false}, {QStringLiteral("bottom"), 256, 192, true}};
    GameSession gs;
    QImage frame(1024, 1536, QImage::Format_RGB32);  // a 4x internal resolution frame
    frame.fill(QColor(40, 90, 140));
    gs.setPreview(QStringLiteral("t"), frame, profile, EmulationDiagnostics());
    QCOMPARE(gs.readbackLimit(), QSize());  // no view yet: unlimited

    QQuickWindow window;
    window.resize(1000, 1000);
    const qreal dpr = window.effectiveDevicePixelRatio();
    {
      GameView view;
      view.setParentItem(window.contentItem());
      view.setIntegerScale(false);
      view.setSize(QSizeF(300, 450));
      view.setSession(&gs);
      const auto expected = [&](qreal w, qreal h) { return QSize(qCeil(w * dpr - 1e-6), qCeil(h * dpr - 1e-6)); };
      QCOMPARE(gs.readbackLimit(), expected(300, 450));
      view.setSize(QSizeF(600, 900));  // resize: reported again
      QCOMPARE(gs.readbackLimit(), expected(600, 900));
      // The Session encoder needs more than a small view: the larger of both applies while shared.
      gs.setShareSize(QSize(1280, 1920));
      QCOMPARE(gs.readbackLimit(), QSize(1280, 1920).expandedTo(expected(600, 900)));
      gs.setShareSize(QSize());
      QCOMPARE(gs.readbackLimit(), expected(600, 900));
      // A second view of the same game (multiview/fullscreen): the larger one wins.
      GameView other;
      other.setParentItem(window.contentItem());
      other.setIntegerScale(false);
      other.setSize(QSizeF(900, 1350));
      other.setSession(&gs);
      QCOMPARE(gs.readbackLimit(), expected(900, 1350));
    }
    {
      // Integer scaling fits the 1x base size, not the (larger) delivered frame: stable, no frame-size feedback.
      GameView view;
      view.setParentItem(window.contentItem());
      view.setSize(QSizeF(300, 450));
      view.setSession(&gs);
      QCOMPARE(gs.readbackLimit(), QSize(256, 384).expandedTo(QSize()));
    }
    QCOMPARE(gs.readbackLimit(), QSize());  // views gone: no limit again
    // devicePixelRatio: physical = logical * ratio, rounded up
    QCOMPARE(GameView::physicalSize(QSizeF(300, 450), 2.0), QSize(600, 900));
    QCOMPARE(GameView::physicalSize(QSizeF(301.2, 450.5), 1.5), QSize(452, 676));
    QCOMPARE(GameView::physicalSize(QSizeF(), 2.0), QSize());
  }

  void playTestRom() {
    if (qEnvironmentVariableIsEmpty("UZUME_MELONDS_DS_CORE")) {
      UITEST_SKIP_NO_CORE("UZUME_MELONDS_DS_CORE not set");
    }
    uitest::installWarningCounter();
    QFile rom(QStringLiteral(FB_TEST_ROM_PATH));
    QVERIFY2(rom.open(QIODevice::ReadOnly), "Test ROM missing (target uzume_test_rom)");
    const QByteArray romData = rom.readAll();
    const QString sha = uitest::sha256Hex(romData);

    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-game");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    QJsonArray games;
    games.append(uitest::gameJson(QStringLiteral("t1"), QStringLiteral("Arcade Uzume Test"), sha, romData.size(), QStringLiteral("uzume_test.nds")));
    games.append(uitest::gameJson(QStringLiteral("t2"), QStringLiteral("Paper Wizards"), uitest::sha256Hex("pw"), 128LL * 1024 * 1024));
    hub.games = QJsonObject{{QStringLiteral("games"), games}};
    hub.roms.insert(sha, romData);
    QVERIFY(hub.start());

    Harness h;
    QVERIFY(h.start(/*probeCores=*/true));
    // Core version for the handshake came from the core info (or: only core_id, empty version).
    // The probe runs on a worker thread (CoreCatalog::probeCores): wait for it, it can finish after the window is exposed.
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->handshakeCores().size(), 1, 15000);
    QCOMPARE(h.controller->handshakeCores().at(0).id, QStringLiteral("melondsds"));
    QVERIFY2(!h.controller->handshakeCores().at(0).version.isEmpty(), "Core version not determined");

    h.controller->addHub(hub.address());
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation);
    h.controller->confirmTrust();
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::NeedsPairing);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
    QTRY_COMPARE(h.controller->libraryState(), QStringLiteral("ready"));
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("t1"));

    QVariantMap g = h.controller->selectedGame();
    QVERIFY(g.value(QStringLiteral("coreText")).toString().contains(QStringLiteral("ready")));
    QCOMPARE(g.value(QStringLiteral("firmwareText")).toString(), QStringLiteral("Not required"));
    QVERIFY(g.value(QStringLiteral("canPlay")).toBool());
    QCOMPARE(g.value(QStringLiteral("playLabel")).toString(), QStringLiteral("Download and play"));
    uitest::saveShot(h.window, QStringLiteral("3c-library-core-ready"));

    // Play: downloads the ROM from the hub (SHA-256 verified), starts melonDS DS, switches to the game view.
    h.controller->playSelected();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 20000);
    GameSession* s = h.controller->gameSession();
    QCOMPARE(s->title(), QStringLiteral("Arcade Uzume Test"));
    QTRY_VERIFY_WITH_TIMEOUT(s->frameNumber() >= 5, 10000);
    QVERIFY(s->hasFrame());
    QVERIFY(!uitest::isAllBlack(s->frame()));
    QCOMPARE(s->frame().size(), QSize(256, 384));

    // The game view shows an image (not completely black) in the rendered scene.
    auto* view = h.window->findChild<GameView*>(QStringLiteral("gameView"));
    QVERIFY(view != nullptr);
    QTRY_VERIFY(!view->frameRect().isEmpty());
    QImage shot;
    QRect region;
    const auto grabGameView = [&]() {  // the first rendered frames may still be black: poll the rendered scene
      shot = h.window->grabWindow();
      const QPointF topLeft = view->mapToScene(view->frameRect().topLeft());
      region = QRectF(topLeft, view->frameRect().size()).toAlignedRect().intersected(shot.rect());
      return !region.isEmpty() && !uitest::isAllBlack(shot.copy(region));
    };
    QTRY_VERIFY2_WITH_TIMEOUT(grabGameView(), "Game view is black", 10000);
    // Aspect ratio 2:3 is preserved and the frame fills the height of the view.
    QVERIFY(std::abs(view->frameRect().width() / view->frameRect().height() - 256.0 / 384.0) < 0.01);
    QVERIFY(view->frameRect().height() >= view->height() * 0.9);
    uitest::saveShot(h.window, QStringLiteral("game-view"));

    // Saves kept separate per hub and user: <data>/hubs/<hub_id>/users/<user_id>/saves/<game_id>
    QVERIFY(QDir(QDir(h.controller->profileStore()->hubDir(QStringLiteral("hub-game"))).filePath(QStringLiteral("users/u_test_1/saves/t1"))).exists());

    // Header at the default test window (1280 wide): every visible header button lies fully inside the header
    // and no two overlap (clicks must never land on a neighbor).
    {
      QQuickTest::qWaitForPolish(h.window);
      QQuickItem* header = h.item("gameHeader");
      QVERIFY(header != nullptr);
      const QRectF headerRect = header->mapRectToScene(QRectF(0, 0, header->width(), header->height()));
      QList<QPair<QString, QRectF>> rects;
      for (const char* name : {"backToLibraryButton", "pauseButton", "tabMultiview", "layoutSwitch", "panelToggle", "moreButton"}) {
        QQuickItem* it = h.item(name);
        if (it == nullptr || !it->isVisible()) continue;
        const QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
        QVERIFY2(headerRect.contains(r), qPrintable(QStringLiteral("%1 outside header: %2,%3 %4x%5 (header width %6)")
                                                        .arg(QLatin1String(name)).arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(headerRect.width())));
        rects.append({QLatin1String(name), r});
      }
      QVERIFY(rects.size() >= 5);
      for (int i = 0; i < rects.size(); ++i)
        for (int j = i + 1; j < rects.size(); ++j)
          QVERIFY2(!rects[i].second.intersects(rects[j].second), qPrintable(rects[i].first + QStringLiteral(" overlaps ") + rects[j].first));
    }

    // Pause/Resume, Reset
    QVERIFY(h.click("pauseButton"));
    QTRY_COMPARE(s->state(), GameSession::Paused);
    const quint64 pausedAt = s->frameNumber();
    QTest::qWait(250);
    QVERIFY(s->frameNumber() <= pausedAt + 2);
    uitest::saveShot(h.window, QStringLiteral("game-view-paused"));
    QVERIFY(h.click("pauseButton"));
    QTRY_COMPARE(s->state(), GameSession::Running);
    QTRY_VERIFY(s->frameNumber() > pausedAt + 5);
    QVERIFY(h.click("moreButton"));  // Reset lives in the "..." menu and asks first (dialog), Reset confirms
    QTRY_VERIFY(h.item("menuItem_reset") != nullptr && h.item("menuItem_reset")->isVisible());
    QVERIFY(h.click("menuItem_reset"));
    QTRY_VERIFY(h.item("resetConfirm") != nullptr && h.item("resetConfirm")->isVisible());
    QVERIFY(h.click("resetConfirm"));
    QTRY_VERIFY(!h.item("resetConfirm")->isVisible());

    // Input: keyboard (A/B/arrows) and mouse touch reach the core without crashing.
    view->forceActiveFocus();
    QTest::keyClick(h.window, Qt::Key_X);
    QTest::keyPress(h.window, Qt::Key_Up);
    QTest::keyRelease(h.window, Qt::Key_Up);
    const QPointF touch = view->mapToScene(QPointF(view->frameRect().center().x(), view->frameRect().top() + view->frameRect().height() * 0.75));
    QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, touch.toPoint());
    // Esc = pause, Esc again = resume
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_COMPARE(s->state(), GameSession::Paused);
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_COMPARE(s->state(), GameSession::Running);

    // Quit: back to the Library.
    QVERIFY(h.click("endGameButton"));
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("library"));
    QCOMPARE(s->state(), GameSession::Idle);
    // Diagnostics: which files the core wrote into the save dir (save sync detects <rom basename>.sav)
    qInfo().noquote() << "Save dir content:"
                      << QDir(h.controller->profileStore()->hubDir(QStringLiteral("hub-game")) + QStringLiteral("/users/u_test_1/saves/t1"))
                             .entryList(QDir::Files).join(QLatin1Char(' '));
    QCOMPARE(uitest::warningCount().load(), 0);
  }

  // 0.21: a built variant starts from its own ROM in its own save slot; the chosen slot comes back afterwards.
  void playVariantInOwnSlot() {
    if (qEnvironmentVariableIsEmpty("UZUME_MELONDS_DS_CORE")) {
      UITEST_SKIP_NO_CORE("UZUME_MELONDS_DS_CORE not set");
    }
    if (uitest::previousHandler() == nullptr) uitest::installWarningCounter();
    QFile rom(QStringLiteral(FB_TEST_ROM_PATH));
    QVERIFY2(rom.open(QIODevice::ReadOnly), "Test ROM missing (target uzume_test_rom)");
    const QByteArray romData = rom.readAll();
    const QString sha = uitest::sha256Hex(romData);

    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-variant");
    hub.decision = FakeHub::Decision::Approve;
    hub.games = QJsonObject{{QStringLiteral("games"), QJsonArray{uitest::gameJson(QStringLiteral("t1"), QStringLiteral("Arcade Uzume Test"), sha, romData.size(), QStringLiteral("uzume_test.nds"))}}};
    // Main's ROM is NOT on the server: a variant start must not need it (it starts from the variant cache).
    QVERIFY(hub.start());

    Harness h;
    QVERIFY(h.start(/*probeCores=*/true));
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->handshakeCores().size(), 1, 15000);
    h.controller->addHub(hub.address());
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation);
    h.controller->confirmTrust();
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::NeedsPairing);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
    QTRY_COMPARE(h.controller->libraryState(), QStringLiteral("ready"));

    // A built variant in the cache (the ROM bytes are the homebrew test ROM; the build itself is tested elsewhere).
    VariantCache* cache = h.controller->variantCache();
    VariantRecipe r;
    r.variantId = QStringLiteral("vtest1");
    r.gameId = QStringLiteral("t1");
    r.name = QStringLiteral("Variant");
    r.slot = QStringLiteral("var-0a1b2c3d");
    r.system = QStringLiteral("nds");
    r.toolVersion = QStringLiteral("1.6.2");
    r.settings = QStringLiteral("428x");
    r.seed = QStringLiteral("42");
    r.romFileName = QStringLiteral("uzume_test.nds");
    QVERIFY(QDir().mkpath(cache->tempDir(r.variantId) + QStringLiteral("/out")));
    QFile out(cache->tempDir(r.variantId) + QStringLiteral("/out/uzume_test.nds"));
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(romData);
    out.close();
    QVERIFY(cache->commit(r));
    QVERIFY(cache->isBuilt(r.variantId));

    const QString before = h.controller->saveSync()->slotFor(QStringLiteral("t1"));
    QVERIFY(before != r.slot);
    emit h.controller->variants()->launchRequested(r.variantId);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 20000);
    QCOMPARE(h.controller->saveSync()->slotFor(QStringLiteral("t1")), r.slot);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->gameSession()->frameNumber() >= 3, 10000);
    QVERIFY(QDir(QDir(h.controller->profileStore()->hubDir(QStringLiteral("hub-variant"))).filePath(QStringLiteral("users/u_test_1/saves/t1"))).exists());

    QVERIFY(h.click("endGameButton"));
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("library"));
    QTRY_COMPARE(h.controller->saveSync()->slotFor(QStringLiteral("t1")), before);  // Main's slot is selected again
    QCOMPARE(uitest::warningCount().load(), 0);
  }
};

UITEST_MAIN(GameTest)
#include "game_test.moc"
