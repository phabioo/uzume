// Speed-up button and speed select in the game header (preview game, no core): hidden/visible, Space toggles.
#include <QPainter>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;

class FastForwardUiTest : public QObject {
  Q_OBJECT
 private slots:
  void buttonAndSpace() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-ff");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);

    GameSession* gs = h.controller->gameSession();
    QImage frame(256, 384, QImage::Format_RGB32);
    frame.fill(QColor(40, 90, 140));
    emu::DisplayProfile profile;
    gs->setPreview(QStringLiteral("Uzume Test"), frame, profile, EmulationDiagnostics());
    emit gs->started();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 8000);
    h.controller->sessions()->gameStarted(QStringLiteral("t1"), QStringLiteral("Uzume Test"));
    h.controller->sessions()->setTab(QStringLiteral("session"));
    QQuickTest::qWaitForPolish(h.window);

    // Unsupported core: no button.
    QVERIFY(!gs->fastForwardAvailable());
    QQuickItem* btn = h.item("fastForwardButton");
    QVERIFY(btn == nullptr || !btn->isVisible());
    gs->setFastForward(true);  // refused
    QVERIFY(!gs->fastForward());

    // Supported: button visible, enabled, off.
    gs->setPreviewFastForwardSupported(true);
    QQuickTest::qWaitForPolish(h.window);
    btn = h.item("fastForwardButton");
    QVERIFY(btn != nullptr && btn->isVisible() && btn->isEnabled());
    QVERIFY(h.click("fastForwardButton"));
    QVERIFY(gs->fastForward());
    QTest::keyClick(h.window, Qt::Key_Space);  // Space toggles (off)
    QTRY_VERIFY(!gs->fastForward());
    // Off and on read differently: the ratio alone looked the same in both states.
    QTRY_COMPARE(h.item("fastForwardButton")->property("value").toString(), QStringLiteral("Off · 2×"));
    QCOMPARE(h.item("fastForwardButton")->property("text").toString(), QStringLiteral("» Speed-up"));
    QTest::keyClick(h.window, Qt::Key_Space);
    QTRY_VERIFY(gs->fastForward());
    QTRY_COMPARE(h.item("fastForwardButton")->property("value").toString(), QStringLiteral("On · 2×"));
    QVERIFY(h.item("fastForwardButton")->property("inverted").toBool());   // on = the ink plane

    // Header at 1280 px (Windows fonts are wider): every header button lies inside the header, none overlap, and at
    // least 120 px stay free between "Library" and the right-hand controls.
    h.window->resize(1280, 800);
    QTest::qWait(100);
    QQuickTest::qWaitForPolish(h.window);
    {
      QQuickItem* header = h.item("gameHeader");
      QVERIFY(header != nullptr);
      const QRectF headerRect = header->mapRectToScene(QRectF(0, 0, header->width(), header->height()));
      QList<QPair<QString, QRectF>> rects;
      for (const char* name : {"backToLibraryButton", "fastForwardButton", "speedValueButton", "pauseButton", "moreButton", "tabMultiview", "panelToggle", "layoutSwitch"}) {
        QQuickItem* it = h.item(name);
        if (it == nullptr || !it->isVisible()) continue;
        const QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
        QVERIFY2(headerRect.contains(r), qPrintable(QStringLiteral("%1 outside header").arg(QLatin1String(name))));
        rects.append({QLatin1String(name), r});
      }
      QVERIFY(h.item("fastForwardButton")->isVisible());
      // (the halves of the split button share one pixel)
      for (int i = 0; i < rects.size(); ++i)
        for (int j = i + 1; j < rects.size(); ++j)
          QVERIFY2(!rects[i].second.adjusted(0.5, 0, -0.5, 0).intersects(rects[j].second.adjusted(0.5, 0, -0.5, 0)), qPrintable(rects[i].first + QStringLiteral(" overlaps ") + rects[j].first));
      const qreal spare = h.item("fastForwardButton")->mapRectToScene(QRectF(0, 0, 1, 1)).left() -
                          h.item("backToLibraryButton")->mapRectToScene(QRectF(0, 0, h.item("backToLibraryButton")->width(), 1)).right();
      QVERIFY2(spare >= 120, qPrintable(QStringLiteral("only %1 px spare").arg(spare)));
    }

    // Speed menu (split button, right half): changes the ratio of the running game; the value and the indicator follow.
    QCOMPARE(gs->fastForwardRatio(), 2.0);
    QCOMPARE(h.item("fastForwardButton")->property("value").toString(), QStringLiteral("On · 2×"));
    // The speed menu is the chevron next to the button, or, when the header is tight, the speed entries of the "..." menu.
    const auto openSpeed = [&]() {
      if (h.item("speedValueButton") != nullptr && h.item("speedValueButton")->isVisible()) return h.click("speedValueButton");
      return h.click("moreButton");
    };
    QVERIFY(openSpeed());
    QTRY_VERIFY(h.item("menuItem_speed_2") != nullptr && h.item("menuItem_speed_2")->isVisible());
    for (const char* option : {"menuItem_speed_1.5", "menuItem_speed_2", "menuItem_speed_3", "menuItem_speed_4", "menuItem_speed_6", "menuItem_speed_8"}) {
      QVERIFY2(h.item(option) != nullptr && h.item(option)->isVisible(), option);  // the built set of ADR 0018, no "Unlimited"
    }
    QVERIFY(h.click("menuItem_speed_6"));
    QCOMPARE(gs->fastForwardRatio(), 6.0);
    QTRY_VERIFY(h.item("menuItem_speed_2") == nullptr || !h.item("menuItem_speed_2")->isVisible());
    QCOMPARE(h.item("fastForwardButton")->property("value").toString(), QStringLiteral("On · 6×"));
    QVERIFY(h.item("fastForwardIndicator")->isVisible());

    // Still usable while the own Session is shared (no block): Space and the button keep working.
    QVERIFY(gs->fastForward());
    QTest::keyClick(h.window, Qt::Key_Space);
    QTRY_VERIFY(!gs->fastForward());
    QVERIFY(h.click("fastForwardButton"));
    QVERIFY(gs->fastForward());

    // Remapped speed-up key (Controllers > Hotkeys): the new key toggles, Space no longer does and reaches the keyboard map.
    ControllersController* cc = h.controller->controllers();
    cc->beginHotkeyCapture(QStringLiteral("speedup"));
    QVERIFY(cc->captureKey(Qt::Key_V));
    QCOMPARE(cc->hotkeyLabels().value(QStringLiteral("speedup")).toString(), QStringLiteral("V"));
    QVERIFY(gs->isReservedKey(Qt::Key_V));
    QVERIFY(!gs->isReservedKey(Qt::Key_Space));
    QVERIFY(gs->fastForward());
    QTest::keyClick(h.window, Qt::Key_V);
    QTRY_VERIFY(!gs->fastForward());
    QTest::keyClick(h.window, Qt::Key_Space);
    QTest::qWait(50);
    QVERIFY(!gs->fastForward());
    gs->setKeyboardMap({{Qt::Key_Space, 1u << 8}});
    QVERIFY(gs->keyEvent(Qt::Key_Space, true));
    gs->keyEvent(Qt::Key_Space, false);
    QVERIFY(!gs->keyEvent(Qt::Key_V, true));
    cc->resetHotkeys();
    QVERIFY(gs->isReservedKey(Qt::Key_Space));
    QTest::keyClick(h.window, Qt::Key_Space);
    QTRY_VERIFY(gs->fastForward());

    // Core without support (new game): everything off and hidden again.
    gs->setPreviewFastForwardSupported(false);
    QVERIFY(!gs->fastForward());
    QQuickTest::qWaitForPolish(h.window);
    QQuickItem* b2 = h.item("fastForwardButton");
    QVERIFY(b2 == nullptr || !b2->isVisible());
  }
};

QTEST_MAIN(FastForwardUiTest)
#include "fast_forward_ui_test.moc"
