// Screenshots (Uzume 0.19, board 3c-1): the strip "Screenshots (N)" on the game page, the viewer (previous / next, run name, Save as...,
// Delete... with a confirm), the capture path (pending file first, notice, retry when the server is back) and an older server without
// any of it. FakeHub serves the screenshots; offscreen; synthetic pictures only.
#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
QByteArray pngOf(int w, int h, QRgb color) {
  QImage img(w, h, QImage::Format_RGB32);
  img.fill(color);
  QByteArray out;
  QBuffer buf(&out);
  buf.open(QIODevice::WriteOnly);
  img.save(&buf, "PNG");
  return out;
}
}  // namespace

class ScreenshotsUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    h.window->resize(1400, 900);
  }
  static void seed(FakeHub& hub, bool supported = true) {
    const QByteArray rom = "homebrew-dummy-rom-screenshots";
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("saves_v2"), QStringLiteral("sessions_v1")};
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    QJsonArray games;
    for (const QString& id : {QStringLiteral("g1"), QStringLiteral("g2")}) {
      games.append(uitest::gameJson(id, id == QLatin1String("g1") ? QStringLiteral("Zephyr Run") : QStringLiteral("Alpha Garden"),
                                    uitest::sha256Hex(rom), rom.size()));
    }
    hub.games = QJsonObject{{QStringLiteral("games"), games}};
    hub.screenshotsUnsupported = !supported;
  }
  // Three screenshots of g1 (the second one played in run r1), one of g2; oldest first like the server stores them.
  static void addShots(FakeHub& hub) {
    const auto add = [&](const QString& id, const QString& game, const QString& run, const QString& at, int w, int h, QRgb color) {
      FakeHub::FakeScreenshot s;
      s.png = pngOf(w, h, color);
      s.meta = QJsonObject{{"id", id}, {"game_id", game}, {"run_id", run.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(run)},
                           {"user_id", "u_test_1"}, {"user_name", "Tester"}, {"created_at", at}, {"width", w}, {"height", h},
                           {"size_bytes", double(s.png.size())}};
      hub.screenshots.append(s);
    };
    add(QStringLiteral("s1"), QStringLiteral("g1"), {}, QStringLiteral("2026-10-01T10:00:00Z"), 256, 192, qRgb(200, 30, 30));
    add(QStringLiteral("s2"), QStringLiteral("g1"), QStringLiteral("r1"), QStringLiteral("2026-10-12T18:30:00Z"), 256, 192, qRgb(30, 200, 30));
    add(QStringLiteral("s3"), QStringLiteral("g1"), {}, QStringLiteral("2026-10-20T12:00:00Z"), 400, 480, qRgb(30, 30, 200));
    add(QStringLiteral("s4"), QStringLiteral("g2"), {}, QStringLiteral("2026-10-21T09:15:00Z"), 256, 192, qRgb(90, 90, 90));
  }
  static bool shown(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr && it->isVisible();
  }
  static QString textOf(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr ? it->property("text").toString() : QString();
  }
  static void selectGame(Harness& h, const QString& id) {
    QVERIFY(h.openGame(id));   // the screenshots section belongs to the overview of the game page
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->selectedGameId(), id, 3000);
  }
  // The Image inside a frame (thumbnail) or the viewer's Image: its status.
  static int statusOf(QQuickItem* it) {
    if (it == nullptr) return -1;
    if (it->property("status").isValid()) return it->property("status").toInt();
    for (QQuickItem* c : it->childItems()) {
      if (c->property("status").isValid() && c->property("fillMode").isValid()) return c->property("status").toInt();
    }
    return -1;
  }
  static int imageReady() { return 1; }  // Image.Ready
  QString pendingDir(Harness& h) const { return QDir(h.controller->profileStore()->baseDir()).filePath(QStringLiteral("screenshots/pending")); }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void stripListsOwnScreenshotsNewestFirstWithDates() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    addShots(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    selectGame(h, QStringLiteral("g1"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "screenshotSection"), 5000);
    QTRY_COMPARE(h.controller->screenshots()->count(), 3);
    QCOMPARE(textOf(h, "screenshotLabel"), QStringLiteral("Screenshots (3)"));
    QVERIFY(!shown(h, "screenshotEmpty"));
    const QVariantList rows = h.controller->screenshots()->shots();
    QCOMPARE(rows.at(0).toMap().value("id").toString(), QStringLiteral("s3"));  // newest first
    QCOMPARE(rows.at(2).toMap().value("id").toString(), QStringLiteral("s1"));
    QTRY_VERIFY(shown(h, "screenshotThumb_s3"));
    QCOMPARE(textOf(h, "screenshotDate_s3"), QStringLiteral("20.10.2026"));  // UTC noon-ish dates stay on their day in any time zone near UTC
    QTRY_COMPARE_WITH_TIMEOUT(statusOf(h.item("screenshotThumb_s3")), imageReady(), 5000);  // loaded through image://screenshots
    QTRY_COMPARE_WITH_TIMEOUT(statusOf(h.item("screenshotThumb_s1")), imageReady(), 5000);
    QVERIFY(h.controller->screenshots()->cache()->contains(QStringLiteral("s3")));
    // The other game has its own
    selectGame(h, QStringLiteral("g2"));
    QTRY_COMPARE(h.controller->screenshots()->count(), 1);
    QTRY_COMPARE(textOf(h, "screenshotLabel"), QStringLiteral("Screenshots (1)"));
  }

  void viewerStepsShowsTheRunAndDeletesWithAConfirm() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    addShots(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->screenshots()->setRunNameLookup([](const QString& id) { return id == QLatin1String("r1") ? QStringLiteral("Nuzlocke night") : QString(); });
    selectGame(h, QStringLiteral("g1"));
    QTRY_COMPARE(h.controller->screenshots()->count(), 3);
    QTRY_VERIFY(shown(h, "screenshotThumb_s3"));
    QVERIFY(!shown(h, "screenshotViewer"));
    QVERIFY(h.click("screenshotThumb_s3"));
    QTRY_VERIFY(shown(h, "screenshotViewer"));
    QVERIFY(!shown(h, "screenshotViewerRun"));  // the newest one was played outside a run
    QVERIFY(!textOf(h, "screenshotViewerDate").isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(statusOf(h.item("screenshotViewerImage")), imageReady(), 5000);
    QVERIFY(!h.item("screenshotPrev")->isEnabled() || h.item("screenshotPrev")->property("enabled").toBool() == false);
    QVERIFY(h.item("screenshotNext")->isEnabled());
    QVERIFY(h.click("screenshotNext"));
    QTRY_VERIFY(shown(h, "screenshotViewerRun"));
    QCOMPARE(textOf(h, "screenshotViewerRun"), QStringLiteral("Run: Nuzlocke night"));
    QTRY_COMPARE_WITH_TIMEOUT(statusOf(h.item("screenshotViewerImage")), imageReady(), 5000);
    QVERIFY(h.item("screenshotPrev")->isEnabled());
    QVERIFY(h.click("screenshotPrev"));
    QTRY_VERIFY(!shown(h, "screenshotViewerRun"));
    QVERIFY(h.click("screenshotNext"));
    QTRY_VERIFY(shown(h, "screenshotViewerRun"));

    // Save as...: the PNG of the shown picture goes to the chosen file
    const QString target = QDir(h.dir.path()).filePath(QStringLiteral("export.png"));
    QSignalSpy finished(h.controller->screenshots(), &ScreenshotController::finished);
    h.controller->screenshots()->saveAs(QStringLiteral("s2"), QUrl::fromLocalFile(target).toString());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QVERIFY(finished.first().at(1).toBool());
    QFile f(target);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), hub.screenshots.at(1).png);
    f.close();

    // Delete...: nothing happens before the confirm; Cancel keeps it
    QVERIFY(h.click("screenshotDelete"));
    QTRY_VERIFY(shown(h, "screenshotDeleteDialog"));
    QCOMPARE(hub.count(QStringLiteral("/api/v1/screenshots/s2"), "DELETE"), 0);
    QVERIFY(h.click("screenshotDeleteCancel"));
    QTRY_VERIFY(!shown(h, "screenshotDeleteDialog"));
    QVERIFY(shown(h, "screenshotViewer"));
    QCOMPARE(hub.screenshots.size(), 4);
    QVERIFY(h.click("screenshotDelete"));
    QTRY_VERIFY(shown(h, "screenshotDeleteDialog"));
    QVERIFY(h.click("screenshotDeleteConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.count(QStringLiteral("/api/v1/screenshots/s2"), "DELETE"), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!shown(h, "screenshotViewer"), 5000);
    QTRY_COMPARE(h.controller->screenshots()->count(), 2);  // the list was loaded again
    QTRY_COMPARE(textOf(h, "screenshotLabel"), QStringLiteral("Screenshots (2)"));
    QVERIFY(!h.controller->screenshots()->cache()->contains(QStringLiteral("s2")));
    QCOMPARE(hub.screenshots.size(), 3);
    // Esc closes the viewer
    QVERIFY(h.click("screenshotThumb_s1"));
    QTRY_VERIFY(shown(h, "screenshotViewer"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "screenshotViewer"));
  }

  void emptyLineAndOlderServer() {
    {
      FakeHub hub(QStringLiteral("a"));
      seed(hub);
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      selectGame(h, QStringLiteral("g1"));
      QTRY_VERIFY_WITH_TIMEOUT(shown(h, "screenshotEmpty"), 5000);
      QCOMPARE(textOf(h, "screenshotLabel"), QStringLiteral("Screenshots (0)"));
      QVERIFY(textOf(h, "screenshotEmpty").contains(QStringLiteral("F12")));
      QVERIFY(!shown(h, "screenshotStrip"));
    }
    FakeHub old(QStringLiteral("b"));
    seed(old, false);
    QVERIFY(old.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, old);
    selectGame(h, QStringLiteral("g1"));
    QTest::qWait(400);
    QVERIFY(!h.controller->screenshots()->supported());
    QVERIFY(!shown(h, "screenshotSection"));
  }

  void captureWritesPendingFirstThenUploadsAndRetries() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    ScreenshotController* sc = h.controller->screenshots();
    QSignalSpy notices(sc, &ScreenshotController::notice);
    QImage frame(256, 192, QImage::Format_RGB32);
    frame.fill(qRgb(10, 120, 200));
    selectGame(h, QStringLiteral("g1"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "screenshotEmpty"), 5000);

    // Connected: saved on the server, the local copy is gone, the strip shows it
    sc->submit(frame, QStringLiteral("g1"), QStringLiteral("r7"));
    QTRY_COMPARE_WITH_TIMEOUT(notices.count(), 1, 8000);
    QCOMPARE(notices.last().at(0).toString(), QStringLiteral("Screenshot saved"));
    QCOMPARE(hub.screenshots.size(), 1);
    QCOMPARE(hub.screenshots.first().meta.value("run_id").toString(), QStringLiteral("r7"));
    QCOMPARE(hub.screenshots.first().meta.value("width").toInt(), 256);
    QCOMPARE(QDir(pendingDir(h)).entryList(QDir::Files).size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(sc->count(), 1, 5000);
    QCOMPARE(textOf(h, "screenshotLabel"), QStringLiteral("Screenshots (1)"));

    // The server answers 503: the picture stays on this PC and the line says so
    hub.screenshotFailNext = 1;
    sc->submit(frame, QStringLiteral("g1"), {});
    QTRY_COMPARE_WITH_TIMEOUT(notices.count(), 2, 8000);
    QCOMPARE(notices.last().at(0).toString(), QStringLiteral("Screenshot saved on this PC. It uploads when the server is back."));
    QCOMPARE(QDir(pendingDir(h)).entryList(QDir::Files).size(), 2);
    QCOMPARE(sc->pendingCount(), 1);
    QCOMPARE(textOf(h, "screenshotLabel"), QStringLiteral("Screenshots (1)"));
    // Back: the retry uploads it and removes the files
    sc->retryPending();
    QTRY_COMPARE_WITH_TIMEOUT(hub.screenshots.size(), 2, 8000);
    QTRY_COMPARE(QDir(pendingDir(h)).entryList(QDir::Files).size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(sc->count(), 2, 5000);
    QCOMPARE(sc->pendingCount(), 0);
    QVERIFY(hub.screenshots.last().meta.value("run_id").isNull());
  }

  void captureTakesTheRunningGameAndItsRun() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    ScreenshotController* sc = h.controller->screenshots();
    QImage frame(240, 160, QImage::Format_ARGB32);
    frame.fill(qRgba(1, 2, 3, 0));  // a core frame can carry an unset alpha: the file is opaque RGB
    QString game = QStringLiteral("g2");
    sc->setGameSource([&]() { return game; }, []() { return QStringLiteral("run-5"); },
                      [&](ScreenshotController::FrameCallback done) { done(frame, QString()); });
    QSignalSpy notices(sc, &ScreenshotController::notice);
    sc->capture();
    QTRY_COMPARE_WITH_TIMEOUT(hub.screenshots.size(), 1, 8000);
    QCOMPARE(hub.screenshots.first().meta.value("game_id").toString(), QStringLiteral("g2"));
    QCOMPARE(hub.screenshots.first().meta.value("run_id").toString(), QStringLiteral("run-5"));
    const QImage back = QImage::fromData(hub.screenshots.first().png, "PNG");
    QCOMPARE(back.size(), QSize(240, 160));
    QVERIFY(!back.hasAlphaChannel());
    // No game running: nothing is taken
    game.clear();
    sc->capture();
    QTest::qWait(200);
    QCOMPARE(hub.screenshots.size(), 1);
    // No picture yet: a line, no file
    game = QStringLiteral("g2");
    sc->setGameSource([&]() { return game; }, []() { return QString(); },
                      [&](ScreenshotController::FrameCallback done) { done(QImage(), QStringLiteral("no_frame")); });
    notices.clear();
    sc->capture();
    QCOMPARE(notices.count(), 1);
    QCOMPARE(hub.screenshots.size(), 1);
  }

  void pendingGoesUpWithTheNextConnection() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    ScreenshotController* sc = h.controller->screenshots();
    h.controller->connection()->disconnectFromHub();
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->connection()->state() != S::Connected, 4000);
    QSignalSpy notices(sc, &ScreenshotController::notice);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::red);
    sc->submit(frame, QStringLiteral("g1"), {});  // offline in the game: kept, the line says so at once
    QCOMPARE(notices.count(), 1);
    QCOMPARE(notices.last().at(0).toString(), QStringLiteral("Screenshot saved on this PC. It uploads when the server is back."));
    QCOMPARE(QDir(pendingDir(h)).entryList(QDir::Files).size(), 2);
    QCOMPARE(hub.screenshots.size(), 0);
    h.controller->connection()->connectToProfile(hub.hubId);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->connection()->state() == S::Connected, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(hub.screenshots.size(), 1, 8000);
    QTRY_COMPARE(QDir(pendingDir(h)).entryList(QDir::Files).size(), 0);
  }
};

UITEST_MAIN(ScreenshotsUiTest)
#include "screenshots_ui_test.moc"
