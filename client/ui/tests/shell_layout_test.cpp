// Layout of the shell right after the first load (round 3 P8, P9): items that become visible after the first layout
// ("Upload ROM" once the handshake reports uploads, the Hub user name once the user is known) must not overlap their
// neighbours. Fake Hub, no ROMs.
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

class ShellLayoutTest : public QObject {
  Q_OBJECT

  static QRectF rectOf(QQuickItem* it) { return it->mapRectToScene(QRectF(0, 0, it->width(), it->height())); }

  static void pairWithUploadsAndUser(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
  }

  static void checkLibraryHeader(Harness& h, const char* when) {
    QQuickTest::qWaitForPolish(h.window);
    QQuickItem* title = h.item("libraryTitle");
    QQuickItem* upload = h.item("uploadButton");
    QQuickItem* search = h.item("searchField");
    QVERIFY2(title != nullptr && upload != nullptr && search != nullptr, when);
    QVERIFY2(upload->isVisible(), when);
    const QRectF t = rectOf(title), u = rectOf(upload), s = rectOf(search);
    QVERIFY2(!t.intersects(u), qPrintable(QStringLiteral("%1: title %2,%3 %4x%5 intersects Add games %6,%7 %8x%9")
                                              .arg(QLatin1String(when)).arg(t.x()).arg(t.y()).arg(t.width()).arg(t.height()).arg(u.x()).arg(u.y()).arg(u.width()).arg(u.height())));
    // One page header: Upload ROM sits at the right of the title while it fits, otherwise it wraps below it. The title never
    // shortens and the action never leaves the header.
    const QRectF hd = rectOf(h.item("libraryHeader"));
    QVERIFY2(t.left() >= hd.left() - 0.5, when);
    QVERIFY2(h.item("libraryTitle")->property("truncated").toBool() == false, when);
    QVERIFY2(u.right() <= hd.right() + 0.5, when);
    // The one toolbar row: below the header and inside the column; search, the scope chips and sort / view never overlap (they
    // wrap to a second line when the search would get narrower than 100 px).
    const QRectF tb = rectOf(h.item("libraryToolbar")), chips = rectOf(h.item("libraryChips")), tail = rectOf(h.item("libraryToolbar2"));
    QVERIFY2(tb.top() >= hd.bottom() - 0.5, when);
    QVERIFY2(!s.intersects(u) && s.width() >= 100 && s.right() <= tb.right() + 0.5, when);
    QVERIFY2(!s.intersects(chips) && !s.intersects(tail) && !chips.intersects(tail), when);
    QVERIFY2(chips.right() <= tb.right() + 0.5 && chips.left() >= tb.left() - 0.5 && tail.right() <= tb.right() + 0.5, when);
    QRectF previous;
    for (const char* name : {"scopeChip_all", "scopeChip_menu"}) {
      QQuickItem* chip = h.item(name);
      QVERIFY2(chip != nullptr && chip->isVisible(), when);
      const QRectF c = rectOf(chip);
      QVERIFY2(c.right() <= tb.right() + 0.5 && c.left() >= tb.left() - 0.5, when);
      QVERIFY2(!c.intersects(previous), when);
      previous = c;
    }
  }

  static void checkUserBlock(Harness& h, const char* when) {
    QQuickTest::qWaitForPolish(h.window);
    QQuickItem* name = h.item("userName");
    QQuickItem* device = h.item("userDevice");
    QVERIFY2(name != nullptr && device != nullptr, when);
    const QRectF n = rectOf(name), d = rectOf(device);
    QVERIFY2(!n.intersects(d), qPrintable(QStringLiteral("%1: user name %2,%3 intersects device line %4,%5").arg(QLatin1String(when)).arg(n.x()).arg(n.y()).arg(d.x()).arg(d.y())));
    QVERIFY2(n.bottom() <= d.top() + 0.5, when);
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // The first library layout after pairing: "Add games…" and the user name arrive with the handshake.
  void firstLoadHasNoOverlap() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = QStringList{QStringLiteral("saves_v1"), QStringLiteral("uploads_v1")};
    hub.handshakeExtra = QJsonObject{{QStringLiteral("user"),
                                      QJsonObject{{QStringLiteral("id"), QStringLiteral("u_1")},
                                                  {QStringLiteral("display_name"), QStringLiteral("admin")},
                                                  {QStringLiteral("role"), QStringLiteral("admin")}}}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    QTest::qWait(50);
    pairWithUploadsAndUser(h, hub);
    // Right after the screen switched: before anything else relays out.
    checkLibraryHeader(h, "first layout");
    checkUserBlock(h, "first layout");
    QTest::qWait(400);
    checkLibraryHeader(h, "settled");
    checkUserBlock(h, "settled");
    // Every width the Player supports: the header stays on one row without overlap.
    for (const int w : {960, 1280, 1920, 3840}) {
      h.window->resize(w, 800);
      QTest::qWait(100);
      checkLibraryHeader(h, qPrintable(QStringLiteral("width %1").arg(w)));
    }
  }

  // Windows renders the same pixel sizes wider (Qt 6.8 / MSVC): widen the header texts and require no overlap at
  // 960 px and at the window minimum.
  void wideFontHasNoOverlap() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = QStringList{QStringLiteral("saves_v1"), QStringLiteral("uploads_v1")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    pairWithUploadsAndUser(h, hub);
    QTest::qWait(200);
    for (const char* name : {"libraryTitle", "uploadButton"}) {
      QQuickItem* it = h.item(name);
      QVERIFY(it != nullptr);
      QFont f = it->property("font").value<QFont>();
      f.setPixelSize(f.pixelSize() * 3 / 2);
      f.setLetterSpacing(QFont::AbsoluteSpacing, 3);
      it->setProperty("font", f);
    }
    const int minW = int(h.window->minimumWidth());
    for (const int w : {1280, 960, minW}) {
      h.window->resize(w, 800);
      QTest::qWait(100);
      checkLibraryHeader(h, qPrintable(QStringLiteral("wide font, width %1").arg(w)));
    }
  }

  // The user name can arrive after the sidebar was laid out without it (older Hub first, then a Hub with user).
  void userNameArrivesLate() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    pairWithUploadsAndUser(h, hub);
    QTest::qWait(200);
    checkUserBlock(h, "no server user");
    QCOMPARE(h.item("userDevice")->property("text").toString(), QStringLiteral("This device"));
  }
};

UITEST_MAIN(ShellLayoutTest)
#include "shell_layout_test.moc"
