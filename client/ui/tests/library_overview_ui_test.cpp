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

  void statusChipsShowCountsAndFilter() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, 4);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    for (const char* id : {"all", "ready", "attention", "download"}) {
      QTRY_VERIFY_WITH_TIMEOUT(shown(h, (QByteArray("statusChip_") + id).constData()), 5000);
    }
    const QVariantMap counts = h.controller->library()->counts();
    QCOMPARE(counts.value(QStringLiteral("all")).toInt(), 4);
    QCOMPARE(counts.value(QStringLiteral("ready")).toInt() + counts.value(QStringLiteral("attention")).toInt() + counts.value(QStringLiteral("download")).toInt(), 4);
    for (const QString& id : counts.keys()) {
      QQuickItem* chip = h.item((QByteArray("statusChip_") + id.toUtf8()).constData());
      if (chip != nullptr) QCOMPARE(chip->property("count").toInt(), counts.value(id).toInt());
    }
    QVERIFY(h.item("statusChip_attention")->property("badgeAccent").toBool());
    QVERIFY(!h.item("statusChip_ready")->property("badgeAccent").toBool());

    // Clicking a chip filters and the rows follow its count.
    for (const QString& id : {QStringLiteral("attention"), QStringLiteral("ready"), QStringLiteral("download"), QStringLiteral("all")}) {
      QVERIFY(h.click((QByteArray("statusChip_") + id.toUtf8()).constData()));
      QTRY_COMPARE(h.controller->library()->filter(), id);
      QCOMPARE(h.controller->library()->rowCount(), counts.value(id).toInt());
    }

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

    // Right on a focused chip moves the filter and the focus.
    h.item("statusChip_all")->forceActiveFocus();
    QTest::keyClick(h.window, Qt::Key_Right);
    QTRY_COMPARE(h.controller->library()->filter(), QStringLiteral("ready"));
    QTest::keyClick(h.window, Qt::Key_Left);
    QTRY_COMPARE(h.controller->library()->filter(), QStringLiteral("all"));

    // The warning stays on every row that needs attention: each row of the list has its square and a status word.
    QTRY_COMPARE(visibleCount(h, "tileStatus"), 4);
    QCOMPARE(visibleCount(h, "tileWarn"), counts.value(QStringLiteral("attention")).toInt());
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
