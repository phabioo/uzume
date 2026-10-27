// Round 3 "Player A" artboards (0.14): 3a-3 connection / pairing lost, 3c-6 Library at 1280 and 3840, 3c-7 detail variants
// (Hub game, local file, local copy of a Hub game, Hub offline), 3p-3 Settings (hubs, library folders). The plugin
// artboards 4a-3, 4a-4 (plugins_ui_test), 4f-3 (plugin_slots_ui_test) and 4j-3 (runs_ui_test) write their shots there.
// Skipped unless UZUME_SCREENSHOT_DIR is set. Homebrew-style dummy bytes only (no ROM/BIOS/firmware).
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

class ShotsPlayerATest : public QObject {
  Q_OBJECT

  QString dir_;

  void shot(Harness& h, const QString& name) {
    QTest::qWait(350);
    QDir().mkpath(dir_);
    const QImage img = h.window->grabWindow();
    QVERIFY2(!img.isNull() && !uitest::isAllBlack(img), qPrintable(name));
    QVERIFY(img.save(QDir(dir_).filePath(name + QStringLiteral(".png"))));
  }
  static void resize(Harness& h, int w, int hgt = 900) {
    h.window->resize(w, hgt);
    QTest::qWait(150);
    QQuickTest::qWaitForPolish(h.window);
  }
  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
  }
  static QJsonObject game(const QString& id, const QString& title, const QByteArray& rom, FakeHub& hub) {
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    return uitest::gameJson(id, title, uitest::sha256Hex(rom), rom.size());
  }

 private slots:
  void initTestCase() {
    dir_ = qEnvironmentVariable("UZUME_SCREENSHOT_DIR");
    if (dir_.isEmpty()) QSKIP("UZUME_SCREENSHOT_DIR not set");
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // 3a-3: the first-start connect screen and the "Pair again" variant of a known Hub that lost its pairing.
  void connection() {
    {
      Harness h;
      QVERIFY(h.start());
      resize(h, 1280);
      QCOMPARE(h.item("addButton")->property("text").toString(), QStringLiteral("Add server"));
      shot(h, QStringLiteral("3a-3-connect"));
    }
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    resize(h, 1280);
    pair(h, hub);
    h.controller->connection()->revokeSelf();
    uitest::warningCount() = 0;  // the revoke itself is logged as a warning
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    QTRY_VERIFY(h.item("pairingTitle") != nullptr && h.item("pairingTitle")->isVisible());
    QCOMPARE(h.item("pairingTitle")->property("text").toString(), QStringLiteral("Pair again"));
    shot(h, QStringLiteral("3a-3-pair-again"));
  }

  // 3c-6: the Library at 1280 and on a 3840 wide window (grid capped at 1680, left-aligned), with filters.
  void libraryWidths() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("local_roms_v1")};
    QJsonArray games;
    const QStringList titles{"Clocktower Kids", "Copper Courier", "Harbor Rally", "Pocket Comet", "Coral Saga", "Orbit Gardens",
                             "Paper Wizards", "Stylus Knights", "Lumen Drift", "Tide Lantern", "Moss Runner", "Night Postal"};
    for (int i = 0; i < titles.size(); ++i) games.append(game(QStringLiteral("g%1").arg(i), titles.at(i), QByteArray("homebrew-") + titles.at(i).toUtf8(), hub));
    hub.games = QJsonObject{{QStringLiteral("games"), games}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    resize(h, 1280);
    h.controller->selectGame(QStringLiteral("g2"));
    QCOMPARE(h.controller->library()->counts().value(QStringLiteral("all")).toInt(), 12);
    shot(h, QStringLiteral("3c-6-1280"));
    h.controller->selectGame({});
    resize(h, 3840, 1400);
    shot(h, QStringLiteral("3c-6-3840"));
    h.controller->selectGame(QStringLiteral("g2"));
    shot(h, QStringLiteral("3c-6-3840-detail"));
  }

  // 3c-7 and 3p-3: a library folder with a local copy of a Hub game and a file that is only local.
  void localFilesAndSettings() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QByteArray harbor = "homebrew-harbor-rally-local";
    const QByteArray comet = "homebrew-pocket-comet-local";
    for (const auto& f : {QPair<QString, QByteArray>{"harbor-rally.nds", harbor}, {"pocket-comet.nds", comet}}) {
      QFile out(folder.filePath(f.first));
      QVERIFY(out.open(QIODevice::WriteOnly));
      out.write(f.second);
    }
    auto hub = std::make_unique<FakeHub>(QStringLiteral("a"));
    hub->features = {QStringLiteral("saves_v1"), QStringLiteral("local_roms_v1")};
    hub->games = QJsonObject{{QStringLiteral("games"), QJsonArray{game(QStringLiteral("g1"), QStringLiteral("Clocktower Kids"), "hub-only-rom", *hub),
                                                                game(QStringLiteral("g2"), QStringLiteral("Harbor Rally"), harbor, *hub)}}};
    QVERIFY(hub->start());
    Harness h;
    QVERIFY(h.start());
    pair(h, *hub);
    resize(h, 1440);

    h.controller->selectGame(QStringLiteral("g1"));
    shot(h, QStringLiteral("3c-7-hub-game"));

    h.controller->addLibraryFolder(folder.path());
    QTRY_VERIFY_WITH_TIMEOUT(!h.controller->libraryScan().value("active").toBool() && h.controller->libraryGameCount() >= 2, 15000);
    h.controller->selectGame(QStringLiteral("g2"));
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->selectedGame().value("localFile").toBool(), 8000);  // tile pill shows "Core missing" without a core
    QVERIFY(!h.controller->selectedGame().value("localOnly").toBool());
    shot(h, QStringLiteral("3c-7-local-on-hub"));

    QTRY_VERIFY_WITH_TIMEOUT(!hub->registerBodies.isEmpty(), 8000);  // the file that only exists locally is registered
    QString localId;
    for (int i = 0; i < 60 && localId.isEmpty(); ++i) {
      QTest::qWait(100);
      h.controller->selectGame(QStringLiteral("reg-1"));
      if (h.controller->selectedGame().value("localFile").toBool()) localId = QStringLiteral("reg-1");
    }
    QVERIFY(!localId.isEmpty());
    shot(h, QStringLiteral("3c-7-local-file"));

    // 3p-3: Settings with the library folder row, hubs, updates.
    h.controller->showSettingsSection(QStringLiteral("storage"));
    QTest::qWait(200);
    QTRY_VERIFY(h.item("libraryFolderRow") != nullptr);
    QCOMPARE(h.item("sectionStatus_storage")->property("text").toString(), QStringLiteral("1 folder · 2 games"));  // plurals without "(s)"
    shot(h, QStringLiteral("3p-3-settings-top"));
    QVERIFY(h.click("section_storage"));
    QTest::qWait(250);
    shot(h, QStringLiteral("3p-3-library-folders"));
    QVERIFY(h.click("section_hubs"));
    QTest::qWait(250);
    shot(h, QStringLiteral("3p-3-hubs"));
    QVERIFY(h.click((QByteArray("hubEdit_") + hub->hubId.toUtf8()).constData()));
    shot(h, QStringLiteral("3p-3-hub-edit"));
    QVERIFY(h.click((QByteArray("hubEditCancel_") + hub->hubId.toUtf8()).constData()));
    h.controller->setAppearance(QStringLiteral("light"));
    shot(h, QStringLiteral("3p-3-hubs-light"));
    h.controller->setAppearance(QStringLiteral("dark"));

    // 3c-7 offline: the Hub goes away; the local game keeps its Play, a Hub-only game says why it cannot start.
    h.controller->showLibrary();
    h.controller->selectGame(QStringLiteral("g1"));
    uitest::warningCount() = 0;
    hub.reset();
    h.controller->connection()->disconnectFromHub();
    h.controller->retryConnection();  // identify again: the Hub is gone
    QTest::qWait(4000);
    if (h.controller->selectedGame().value("hubUp").toBool()) QSKIP("the connection did not drop in this environment");
    // R-P2: back on the Connect screen the saved Hub is a full-width card with its name and a labelled Connect button
    // (it was created while the screen was hidden and once stayed at 0 px).
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("connection"));
    QTRY_VERIFY(h.item("hubCard_hub-test-1") != nullptr);
    {
      QQuickItem* card = h.item("hubCard_hub-test-1");
      QTRY_VERIFY2(card->width() >= 400, qPrintable(QStringLiteral("card width %1").arg(card->width())));
      QQuickItem* name = Harness::findVisual(card, QStringLiteral("hubCardName"));
      QVERIFY(name != nullptr && name->width() > 40);
      QQuickItem* connect = Harness::findVisual(card, QStringLiteral("hubConnectButton"));
      QVERIFY(connect != nullptr && connect->width() >= 70);
    }
    shot(h, QStringLiteral("3c-7-hub-offline"));
    uitest::warningCount() = 0;  // the unreachable Hub is logged as a warning
  }
};

UITEST_MAIN(ShotsPlayerATest)
#include "screenshots_player_a_test.moc"
