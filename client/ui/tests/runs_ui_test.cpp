// Runs in the Player UI (docs/design/legacy/plugins.md 4j-2, 0.18 package 9): the sidebar entry (the Hub announces runs_v1; no plugin
// needed), the invite card with the game picker, the run view in tabs (Board, Recap, Log, one tab per plugin with a screen), status
// actions, "Start a run" (Rules: None or a plugin), and "Play run" (Hub copies of the run's plugins, the run's own save slot).
// Synthetic plugins and a fake Hub only. With UZUME_SCREENSHOT_DIR=<dir> the views are also saved as PNGs (dark and light) for the review.
#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QQuickItemGrabResult>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "keyedrowsmodel.h"
#include "uzplugin_testutil.h"
#include "runrecapcontroller.h"
#include "runscontroller.h"
#include "savehistorycontroller.h"
#include "runtypes.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using namespace uzume::plugins::testutil;

namespace {

const QString kPluginId = QStringLiteral("net.test.runs");
const QString kGameId = QStringLiteral("00000000-0000-4000-8000-0000000000a1");

void pair(Harness& h, FakeHub& hub) {
  h.controller->addHub(hub.address());
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation, 8000);
  h.controller->confirmTrust();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsPairing, 8000);
  h.controller->requestPairing();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
}

}  // namespace

class RunsUiTest : public QObject {
  Q_OBJECT
  QString shotDir_;

  void shot(Harness& h, const QString& name) {
    if (shotDir_.isEmpty()) return;
    QQuickTest::qWaitForPolish(h.window);
    QTest::qWait(350);
    QDir().mkpath(shotDir_);
    const QImage img = h.window->grabWindow();
    QVERIFY2(!img.isNull() && !uitest::isAllBlack(img), qPrintable(name));
    QVERIFY(img.save(QDir(shotDir_).filePath(name + QStringLiteral(".png"))));
  }

 private slots:
  void initTestCase() {
    shotDir_ = qEnvironmentVariable("UZUME_SCREENSHOT_DIR");
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  static bool popupOpen(Harness& h, const char* name) {
    QObject* o = h.window->findChild<QObject*>(QString::fromLatin1(name));
    return o != nullptr && o->property("visible").toBool();
  }

  // The run actions live in the "⋯" menu (4j-3): probe whether an item exists, or choose it.
  static bool menuHas(Harness& h, const char* name) {
    if (h.item("runMenu") == nullptr || !h.item("runMenu")->isVisible() || !h.click("runMenu")) return false;
    QTest::qWait(120);
    const QByteArray item = QByteArray("runMenu_") + name;
    QQuickItem* it = h.item(item.constData());
    const bool has = it != nullptr && it->isVisible();
    h.click("runMenu");  // closes it again
    QTest::qWait(120);
    return has;
  }
  static bool menuPick(Harness& h, const char* name) {
    if (!h.click("runMenu")) return false;
    QTest::qWait(120);
    return h.click((QByteArray("runMenu_") + name).constData());
  }

  void runsFlow() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-runs");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1"), QStringLiteral("runs_v1"), QStringLiteral("sessions_v1")};
    hub.games = QJsonObject{
        {"games", QJsonArray{QJsonObject{{"id", kGameId},
                                         {"title", "Homebrew Quest"},
                                         {"system", "nds"},
                                         {"rom", QJsonObject{{"sha256", QString(64, QLatin1Char('b'))}, {"size", 4096}, {"filename", "quest.nds"}}},
                                         {"added_at", "2026-01-01T12:00:00Z"}}}}};
    hub.fakeUsers = QJsonArray{QJsonObject{{"id", "u_test_1"}, {"display_name", "Tester"}, {"online", true}},
                               QJsonObject{{"id", "u_lena"}, {"display_name", "Lena"}, {"online", true}}};

    FbSpec spec;
    spec.id = kPluginId;
    spec.name = "Run Notes";
    spec.permissions = {"run.state"};
    spec.recap = QJsonArray{QJsonObject{{"type", "tide.catch"}, {"glyph", "catch"}}, QJsonObject{{"type", "tide.badge"}, {"glyph", "badge"}}};
    const QByteArray file = makeUzPlugin(spec);
    const QString sha = sha256Hex(file);
    hub.pluginList.append(QJsonObject{{"plugin_id", spec.id}, {"version", spec.version}, {"name", spec.name}, {"description", "Notes for runs"}, {"api", 1},
                                      {"origin", "unverified"}, {"sha256", sha}, {"size", file.size()}, {"permissions", QJsonArray{"run.state"}},
                                      {"games", QJsonArray{QJsonObject{{"system", "nds"}}}}, {"created_at", "2026-10-12T10:00:00Z"}});
    hub.pluginFiles.insert(spec.id + "/" + spec.version, file);

    // A run Lena created and invited me to.
    const QJsonArray refs{QJsonObject{{"plugin_id", spec.id}, {"version", spec.version}, {"sha256", sha}}};
    QJsonObject run = hub.makeRun(QStringLiteral("Tide Hardcore"), {QStringLiteral("u_lena"), hub.runsMe}, QStringLiteral("u_lena"), refs);
    const QString runId = run.value("run_id").toString();
    QJsonArray ps = run.value("participants").toArray();
    QJsonObject lena = ps.at(0).toObject();
    lena.insert("user_name", "Lena");
    ps.replace(0, lena);
    run.insert("participants", ps);
    hub.runs.insert(runId, run);
    QVERIFY(hub.start());

    Harness h;
    QVERIFY(h.start());
    h.window->resize(1440, 900);
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    PluginsController* pc = h.controller->plugins();
    QVERIFY(runs != nullptr);

    // No plugin is installed here: the Runs entry shows anyway (runs_v1) with the invite count.
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded(), 8000);
    QVERIFY(pc->plugins().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(pc->hubPlugins().size(), 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(runs->available(), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(h.item("navRuns") != nullptr && h.item("navRuns")->isVisible(), 8000);
    QCOMPARE(runs->pendingInvites(), 1);
    QTRY_VERIFY(h.item("navRunsBadge") != nullptr && h.item("navRunsBadge")->isVisible());

    QVERIFY(h.click("navRuns"));
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("runs"), 8000);
    QTRY_VERIFY(h.item(("runInvite_" + runId).toUtf8().constData()) != nullptr);
    QCOMPARE(h.item("runInviteHeadline")->property("text").toString(), QStringLiteral("Lena invites you to \u201CTide Hardcore\u201D"));
    shot(h, QStringLiteral("4j-runs-invite"));
    h.controller->setAppearance(QStringLiteral("light"));
    shot(h, QStringLiteral("4j-runs-invite-light"));
    h.controller->setAppearance(QStringLiteral("dark"));

    // Join with the game picker (only games that fit the plugin's systems are offered).
    QVERIFY(h.click("runInviteJoin"));
    QTRY_VERIFY(popupOpen(h, "runJoinDialog"));
    QTRY_COMPARE_WITH_TIMEOUT(runs->games()->rowCount(), 1, 8000);
    // Every plugin of the run is listed with name, version, origin pill, permissions and the unverified warning.
    QCOMPARE(runs->joinPlugins()->rowCount(), 1);
    QTRY_VERIFY(h.item(("runJoinPlugin_" + spec.id).toUtf8().constData()) != nullptr);
    QCOMPARE(h.item("runJoinPluginOrigin")->property("text").toString(), QStringLiteral("Server upload · unverified"));
    QVERIFY(runs->joinUnverified());
    QTRY_VERIFY(h.item("runJoinUnverified") != nullptr && h.item("runJoinUnverified")->isVisible());
    QCOMPARE(h.item("runJoinUnverified")->property("text").toString(), runs->unverifiedWarning());
    QVERIFY(runs->unverifiedWarning().startsWith(QStringLiteral("Uzume has not checked this plugin.")));
    QVERIFY(h.item("runJoinDialog")->property("text").toString().startsWith(QStringLiteral("Lena invited you · ")));  // no nested quotes (P26)
    QVERIFY(!h.item("runJoinWhy")->isVisible());  // a fitting game exists, so Join is possible (P27)
    shot(h, QStringLiteral("4j-3-join"));
    QVERIFY(!h.item("runJoinConfirm")->isEnabled() || runs->games()->rowCount() == 1);
    QVERIFY(h.click(("runGame_" + kGameId).toUtf8().constData()));
    QVERIFY(h.click("runJoinConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(runs->openRunId() == runId, 8000);
    QCOMPARE(hub.lastRunBody.value("game_id").toString(), kGameId);
    QTRY_COMPARE_WITH_TIMEOUT(runs->pendingInvites(), 0, 8000);
    // Joining is the consent: the Hub copy of the plugin is fetched without a further question.
    QTRY_VERIFY_WITH_TIMEOUT(!pc->manager()->hubCopyDir(spec.id, spec.version, sha).isEmpty(), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(!runs->busy(), 8000);
    QTRY_VERIFY(h.item("runPlay") != nullptr && h.item("runPlay")->isVisible());
    QCOMPARE(h.item("runDetailHeader")->property("title").toString(), QStringLiteral("Tide Hardcore"));
    QTRY_COMPARE_WITH_TIMEOUT(runs->participants()->rowCount(), 2, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(runs->events()->rowCount() >= 2, 8000);  // run.created + run.joined, written by the Hub
    QVERIFY(h.item("runParticipant_u_lena") != nullptr);
    shot(h, QStringLiteral("4j-3-run-detail"));
    // The run view has tabs: Board (people with their save slot "Run: <name>") and Log; the plugin declares no screen, so no plugin tab.
    QCOMPARE(runs->runTab(), QStringLiteral("board"));
    QVERIFY(runs->runTabs().isEmpty());
    QTRY_VERIFY(h.item("runTabs") != nullptr && h.item("runTabs")->isVisible());
    QVERIFY(h.item("tab_Board") != nullptr && h.item("tab_Log") != nullptr);
    QVERIFY(h.item("runBoardTab")->isVisible() && !h.item("runLogTab")->isVisible());
    QCOMPARE(h.item("runParticipantSlot")->property("text").toString(), QStringLiteral("Run: Tide Hardcore"));
    QCOMPARE(runs->detail().value("saveSlotText").toString(), QStringLiteral("Run: Tide Hardcore"));
    QCOMPARE(runs->slotDisplayName(runSaveSlot(runId)), QStringLiteral("Run: Tide Hardcore"));
    QCOMPARE(runs->slotDisplayName(QStringLiteral("default")), QString());
    QCOMPARE(runs->slotDisplayName(QStringLiteral("my-own-slot")), QString());
    QVERIFY(h.click("tab_Log"));
    QTRY_VERIFY(h.item("runLogTab")->isVisible() && !h.item("runBoardTab")->isVisible());
    QCOMPARE(runs->runTab(), QStringLiteral("log"));
    QVERIFY(h.item("runEventsTitle")->isVisible());
    QVERIFY(h.click("tab_Board"));
    QTRY_VERIFY(h.item("runBoardTab")->isVisible());

    // Recap (0.22, board 4jh-app-2): a run without events of the plugin shows the empty state; mapped events of the plugin (the
    // pinned Hub copy's `recap` block) become glyphs, the tiles count them, Share link / HTML / image export work.
    {
      RunRecapController* recap = runs->recap();
      QVERIFY(h.item("tab_Recap") != nullptr);
      QVERIFY(h.click("tab_Recap"));
      QTRY_VERIFY(h.item("runRecapTab")->isVisible() && !h.item("runBoardTab")->isVisible());
      QCOMPARE(runs->runTab(), QStringLiteral("recap"));
      QTRY_COMPARE_WITH_TIMEOUT(recap->state(), QStringLiteral("ready"), 8000);
      QVERIFY(recap->recapEmpty());
      QTRY_VERIFY(h.item("recapEmpty") != nullptr && h.item("recapEmpty")->isVisible());
      QVERIFY(!h.item("recapSheet")->isVisible());
      QVERIFY(!h.item("recapExport")->isEnabled());
      shot(h, QStringLiteral("4jh-app-2-recap-empty"));

      hub.addRunEvent(runId, QStringLiteral("tide.catch"), QStringLiteral("Caught a Tentacool"), hub.runsMe, spec.id, {}, true);
      hub.addRunEvent(runId, QStringLiteral("tide.catch"), QStringLiteral("Caught a Staryu"), hub.runsMe, spec.id, {}, true);
      hub.addRunEvent(runId, QStringLiteral("tide.badge"), QStringLiteral("Badge 1"), hub.runsMe, spec.id, {}, true);
      hub.addRunEvent(runId, QStringLiteral("tide.unmapped"), QStringLiteral("Not shown"), hub.runsMe, spec.id, {}, true);
      hub.addRunEvent(runId, QStringLiteral("memory.write"), QStringLiteral("Wrote memory"), hub.runsMe, {}, {}, true);
      QTRY_VERIFY_WITH_TIMEOUT(!recap->recapEmpty() && recap->state() == QLatin1String("ready"), 12000);
      QTRY_VERIFY(h.item("recapSheet")->isVisible());
      const QVariantMap model = recap->model();
      QCOMPARE(model.value("shownEvents").toInt(), 4);  // 2 catches, 1 badge, 1 note
      const QVariantList tiles = model.value("tiles").toList();
      QCOMPARE(tiles.at(0).toMap().value("value").toString(), QStringLiteral("2"));
      QCOMPARE(tiles.at(2).toMap().value("value").toString(), QStringLiteral("1"));
      QCOMPARE(model.value("lanes").toList().size(), 2);  // one lane per person who joined (Lena has nothing on hers)
      QVERIFY(h.item("recapTile_catches") != nullptr && h.item("recapTile_catches")->isVisible());
      QVERIFY(h.item("recapLane_u_test_1") != nullptr);
      QVERIFY(h.item("recapLegend")->isVisible());
      QVERIFY(h.item("recapFootnote")->property("text").toString().contains(QStringLiteral("everyone on Home")));
      QVERIFY(h.item("recapExport")->isEnabled());
      QVERIFY(h.item("recapShare")->isEnabled());
      // The footnote is outside the exported sheet.
      for (QQuickItem* p = h.item("recapFootnote"); p != nullptr; p = p->parentItem()) QVERIFY(p != h.item("recapSheet"));
      // Same-moment events share a glyph with a "+N" badge; glyphs are focusable and carry the event text.
      {
        QTRY_VERIFY(!h.items("recapGlyphBadge").isEmpty());
        bool grouped = false;
        for (QQuickItem* b : h.items("recapGlyphBadge"))
          if (b->isVisible() && b->property("text").toString().startsWith(QLatin1Char('+'))) grouped = true;
        QVERIFY(grouped);
        QQuickItem* glyph = h.item("recapGlyph_catch");
        QVERIFY(glyph != nullptr && glyph->activeFocusOnTab());
        bool reduced = false;
        for (const QVariant& l : recap->model().value("lanes").toList()) {
          const QVariantList evs = l.toMap().value("events").toList();
          const QVariantList g = recap->groupEvents(evs, 400, 16);
          if (evs.size() >= 2 && g.size() < evs.size() && g.at(0).toMap().value("count").toInt() >= 2) reduced = true;
        }
        QVERIFY(reduced);
      }
      shot(h, QStringLiteral("4jh-app-2-recap"));
      h.controller->setAppearance(QStringLiteral("light"));
      shot(h, QStringLiteral("4jh-app-2-recap-light"));
      h.controller->setAppearance(QStringLiteral("dark"));

      // Share link: the run's app link goes to the clipboard and a note shows.
      QCOMPARE(recap->shareLink(), QStringLiteral("uzume://run/") + hub.address().section("://", -1) + QLatin1Char('/') + runId + QStringLiteral("/recap"));
      QCOMPARE(QGuiApplication::clipboard()->text(), recap->shareLink());
      QTRY_VERIFY(h.item("recapMessage")->isVisible());
      QVERIFY(!recap->messageIsError());
      {  // the note sits in the action row: it does not move the sheet
        const qreal sheetY = h.item("recapSheet")->mapToScene(QPointF(0, 0)).y();
        recap->dismissMessage();
        QTRY_VERIFY(!h.item("recapMessage")->isVisible());
        QCOMPARE(h.item("recapSheet")->mapToScene(QPointF(0, 0)).y(), sheetY);
      }
      recap->dismissMessage();

      // Export as HTML and as image.
      QTemporaryDir out;
      QVERIFY(out.isValid());
      const QString htmlPath = out.filePath(QStringLiteral("recap.html"));
      recap->saveHtml(QUrl::fromLocalFile(htmlPath).toString());
      QFile html(htmlPath);
      QVERIFY(html.open(QIODevice::ReadOnly));
      const QString htmlText = QString::fromUtf8(html.readAll());
      QVERIFY(htmlText.contains(QStringLiteral("Tide Hardcore")) && !htmlText.contains(QStringLiteral("http")));
      QVERIFY(recap->suggestedFileName("png").startsWith(QStringLiteral("Tide Hardcore - recap")));
      QQuickItem* sheet = h.item("recapSheet");
      QSharedPointer<QQuickItemGrabResult> grab = sheet->grabToImage();
      QVERIFY(grab);
      QSignalSpy grabbed(grab.data(), &QQuickItemGrabResult::ready);
      QVERIFY(grabbed.wait(8000));
      QVERIFY(!uitest::isAllBlack(grab->image()));
      const QString pngPath = out.filePath(QStringLiteral("recap.png"));
      recap->saveImage(QVariant::fromValue(grab->image()), pngPath);
      QVERIFY(QImage(pngPath).width() > 100);
      QVERIFY(!recap->messageIsError());
      recap->saveImage(QVariant(), pngPath);  // nothing grabbed: an error note, no crash
      QVERIFY(recap->messageIsError());
      recap->dismissMessage();

      // Error state with a retry.
      recap->load(QStringLiteral("00000000-0000-4000-8000-000000000000"));
      QTRY_COMPARE(recap->state(), QStringLiteral("error"));
      QTRY_VERIFY(h.item("recapError")->isVisible());
      QVERIFY(!recap->errorText().isEmpty() && !recap->errorText().contains(QStringLiteral("refused")));
      recap->load(runId);
      QTRY_COMPARE_WITH_TIMEOUT(recap->state(), QStringLiteral("ready"), 8000);
      // A status note does not survive a run change, a reload keeps the recap shown.
      recap->shareLink();
      QVERIFY(!recap->message().isEmpty());
      recap->load(QStringLiteral("00000000-0000-4000-8000-000000000000"));
      QVERIFY(recap->message().isEmpty());
      recap->load(runId);
      QTRY_COMPARE_WITH_TIMEOUT(recap->state(), QStringLiteral("ready"), 8000);
      recap->shareLink();
      recap->reload();
      QVERIFY(recap->message().isEmpty());
      QCOMPARE(recap->state(), QStringLiteral("ready"));
      QVERIFY(!recap->model().isEmpty());
      QTRY_VERIFY_WITH_TIMEOUT(recap->staleNotice().isEmpty() && recap->state() == QLatin1String("ready"), 8000);
      QVERIFY(h.click("tab_Board"));
      QTRY_VERIFY(h.item("runBoardTab")->isVisible());
    }

    // Status: the creator starts the run; any member can pause and resume it.
    QVERIFY(!menuHas(h, "start"));  // not the creator
    {
      QJsonObject r = hub.runs.value(runId);
      r.insert("status", "active");
      hub.runs.insert(runId, r);
      hub.pushRun(runId);
    }
    QTRY_VERIFY(menuHas(h, "pause"));
    QVERIFY(menuPick(h, "pause"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.runs.value(runId).value("status").toString(), QStringLiteral("paused"), 8000);
    QTRY_VERIFY(menuHas(h, "resume"));
    QVERIFY(h.item("runDetailLine")->property("text").toString().startsWith(QStringLiteral("Paused by")));
    shot(h, QStringLiteral("4j-3-run-detail-paused"));
    QVERIFY(menuPick(h, "resume"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.runs.value(runId).value("status").toString(), QStringLiteral("active"), 8000);

    // Play run: the Hub copy of every plugin is prepared, then the game starts with the run's save slot.
    QSignalSpy launch(runs, &RunsController::launchRequested);
    QTRY_VERIFY(h.item("runPlay") != nullptr && h.item("runPlay")->isEnabled());
    QVERIFY(h.click("runPlay"));
    QTRY_COMPARE_WITH_TIMEOUT(launch.size(), 1, 8000);
    QCOMPARE(launch.first().at(0).toString(), runId);
    QCOMPARE(launch.first().at(1).toString(), kGameId);
    QVERIFY(!pc->manager()->hubCopyDir(spec.id, spec.version, sha).isEmpty());
    // The test host has no core for the game: Play says so and does not leave the run's slot selected.
    QTRY_VERIFY(!runs->message().isEmpty());
    QVERIFY(h.controller->saveSync()->slotFor(kGameId) != runSaveSlot(runId));
    QVERIFY(runSaveSlot(runId).startsWith(QStringLiteral("run-")) && runSaveSlot(runId).size() == 12);
    // Wherever a slot name is shown, the run's slot reads "Run: <name>" (Main stays "Main", other slots keep their name).
    {
      SaveHistoryController* hist = h.controller->saveHistory();
      QVERIFY(h.controller->saveSync()->setSlot(kGameId, runSaveSlot(runId)));  // what a run start selects when the game can start
      QCOMPARE(hist->slot(), runSaveSlot(runId));
      QCOMPARE(hist->slotLabel(), QStringLiteral("Run: Tide Hardcore"));
      QCOMPARE(hist->slotName(QStringLiteral("default")), QStringLiteral("Main"));
      QCOMPARE(hist->slotName(QStringLiteral("boss")), QStringLiteral("boss"));
      bool found = false;
      for (const QVariant& o : hist->slotOptions()) {
        const QVariantMap m = o.toMap();
        if (m.value("value").toString() == runSaveSlot(runId)) {
          found = true;
          QCOMPARE(m.value("label").toString(), QStringLiteral("Run: Tide Hardcore"));
        }
        if (m.value("value").toString() == QLatin1String("default")) QCOMPARE(m.value("label").toString(), QStringLiteral("Main"));
      }
      QVERIFY(found);
    }
    const RunLaunch ctx = runs->launchFor(runId);
    QVERIFY(ctx.bridge != nullptr);
    QCOMPARE(ctx.pins.size(), 1);
    QCOMPARE(ctx.pins.first().sha256, sha);
    // A normal start selects the game's own slot again.
    QVERIFY(h.controller->saveSync()->setSlot(kGameId, QStringLiteral("default")));
    h.controller->playSelected();
    QCOMPARE(h.controller->saveSync()->slotFor(kGameId), QStringLiteral("default"));

    // Back to the list: the run sits in the Active group.
    runs->closeRun();
    QTRY_VERIFY(h.item("runTableRow_0") != nullptr);
    QCOMPARE(runs->activeTitle(), QStringLiteral("ACTIVE · 1"));
    {
        const auto* am = qobject_cast<KeyedRowsModel*>(runs->active());
        QVERIFY(am != nullptr);
        QVERIFY(!am->get(0).value("rulesName").toString().isEmpty());
        QVERIFY(!am->get(0).value("rulesVersion").toString().isEmpty());
    }
    shot(h, QStringLiteral("4j-runs-list"));

    // Start a run: name, rules (plugin), a player and the own game.
    QVERIFY(h.click("runsStartButton"));
    QTRY_VERIFY(popupOpen(h, "runStartDialog"));
    QTRY_VERIFY_WITH_TIMEOUT(!runs->startUsersLoading(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(runs->startPlugins()->rowCount(), 1, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(runs->startUsers()->rowCount(), 1, 8000);  // not myself
    // Rules select: this Hub has no runs_v2, so "None" is not offered and a plugin stays required.
    QCOMPARE(runs->startRulesOptions().size(), 1);
    QCOMPARE(runs->startRulesOptions().at(0).toMap().value("value").toString(), kPluginId);
    QVERIFY(h.item("runRulesSelect")->isVisible());
    QCOMPARE(h.item("runRulesHint")->property("text").toString(), QStringLiteral("Only installed run plugins are listed."));
    QCOMPARE(h.item("runStartSlotNote")->property("text").toString(), QStringLiteral("Each person gets their own save slot for this run. Main is not touched."));
    h.item("runNameField")->setProperty("text", QStringLiteral("Summer Duo"));
    QTRY_COMPARE(h.item("runStartSlotNote")->property("text").toString(), QStringLiteral("Each person gets Run: Summer Duo. Main is not touched."));
    QVERIFY(h.item("runStartUnverified") == nullptr || !h.item("runStartUnverified")->isVisible());  // nothing picked yet
    shot(h, QStringLiteral("4j-runs-start"));
    QVERIFY(h.click("runStartConfirm"));  // nothing picked yet
    QTRY_VERIFY(!runs->startError().isEmpty());
    runs->selectStartRules(kPluginId);
    QCOMPARE(runs->startRulesCurrent(), kPluginId);
    QVERIFY(runs->startUnverified());
    QTRY_VERIFY(h.item("runStartUnverified") != nullptr && h.item("runStartUnverified")->isVisible());
    QCOMPARE(h.item("runPluginOrigin")->property("text").toString(), QStringLiteral("Server upload · unverified"));
    QVERIFY(h.click("runUser_u_lena"));
    QVERIFY(h.click(("runGame_" + kGameId).toUtf8().constData()));
    QVERIFY(h.click("runStartConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(hub.runs.size() == 2, 8000);
    QCOMPARE(hub.lastRunBody.value("name").toString(), QStringLiteral("Summer Duo"));
    QCOMPARE(hub.lastRunBody.value("invite_user_ids").toArray().size(), 1);
    QCOMPARE(hub.lastRunBody.value("game_id").toString(), kGameId);
    QTRY_VERIFY_WITH_TIMEOUT(!runs->openRunId().isEmpty() && runs->openRunId() != runId, 8000);  // the new run opens
    QTRY_VERIFY(menuHas(h, "start"));  // creator of a starting run
    QVERIFY(menuPick(h, "start"));
    QTRY_VERIFY_WITH_TIMEOUT(hub.runs.value(runs->openRunId()).value("status").toString() == QLatin1String("active"), 8000);

    // Finishing asks first and ends the run for everyone.
    QTRY_VERIFY(menuHas(h, "finish"));
    QVERIFY(menuPick(h, "finish"));
    QTRY_VERIFY(popupOpen(h, "runConfirmDialog"));
    QCOMPARE(h.item("runConfirmDialog")->property("title").toString(), QStringLiteral("Finish this run?"));
    shot(h, QStringLiteral("4j-3-confirm"));
    QVERIFY(h.click("runConfirmOk"));
    QTRY_VERIFY_WITH_TIMEOUT(hub.runs.value(runs->openRunId()).value("status").toString() == QLatin1String("finished"), 8000);
    QTRY_VERIFY(h.item("runMenu") == nullptr || !h.item("runMenu")->isVisible() || !menuHas(h, "pause"));
    QTRY_VERIFY_WITH_TIMEOUT(runs->detail().value("over").toBool(), 8000);  // the FakeHub stores before the app gets the answer
    runs->closeRun();
    QTRY_COMPARE_WITH_TIMEOUT(runs->finishedCount(), 1, 8000);
  }

  // Run view: the run detail shows the "run" slot of the run's ui.run plugins (pinned Hub copies, read-only fb.run) for a
  // participant; opening another run, closing the detail or an invited-only run shows nothing.
  void runViewInDetail() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-runview");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1"), QStringLiteral("runs_v1"), QStringLiteral("sessions_v1")};
    FbSpec spec;
    spec.id = kPluginId;
    spec.name = "Run Notes";
    spec.permissions = {"run.state", "ui"};
    spec.ui = QJsonObject{{"run", QJsonObject{{"title", "Board"}}}};
    spec.lua = "local c = fb.run.current()\n"
               "local ok, err = fb.run.setState({a = 1})\n"
               "fb.ui.set('run', {type = 'text', text = 'view=' .. tostring(c.view) .. ' ' .. tostring(c.name) .. ' ' .. tostring(err)})\n";
    const QByteArray file = makeUzPlugin(spec);
    const QString sha = sha256Hex(file);
    hub.pluginList.append(QJsonObject{{"plugin_id", spec.id}, {"version", spec.version}, {"name", spec.name}, {"description", "Notes for runs"}, {"api", 1},
                                      {"origin", "unverified"}, {"sha256", sha}, {"size", file.size()}, {"permissions", QJsonArray{"run.state", "ui"}},
                                      {"created_at", "2026-10-12T10:00:00Z"}});
    hub.pluginFiles.insert(spec.id + "/" + spec.version, file);
    const QJsonArray refs{QJsonObject{{"plugin_id", spec.id}, {"version", spec.version}, {"sha256", sha}}};
    // Mine (creator, joined) and one I am only invited to.
    const QString mine = hub.makeRun(QStringLiteral("Mine"), {hub.runsMe, QStringLiteral("u_lena")}, hub.runsMe, refs).value("run_id").toString();
    const QString invited = hub.makeRun(QStringLiteral("Invited"), {QStringLiteral("u_lena"), hub.runsMe}, QStringLiteral("u_lena"), refs).value("run_id").toString();
    QVERIFY(hub.start());

    Harness h;
    QVERIFY(h.start());
    h.window->resize(1440, 900);
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    PluginsController* pc = h.controller->plugins();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(pc->hubPlugins().size(), 1, 8000);

    QVERIFY(h.click("navRuns"));
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("runs"), 8000);

    // An invited-only run does not start the plugins.
    runs->openRun(invited);
    QTest::qWait(300);
    QVERIFY(pc->runViewPluginIds().isEmpty());
    QVERIFY(pc->runSections().isEmpty());
    QVERIFY(pc->manager()->hubCopyDir(spec.id, spec.version, sha).isEmpty());

    // My run: the copy is fetched, the plugin runs read-only and its "run" tree reaches the model.
    runs->openRun(mine);
    QTRY_VERIFY_WITH_TIMEOUT(!pc->manager()->hubCopyDir(spec.id, spec.version, sha).isEmpty(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(pc->runSections().size(), 1, 10000);
    QCOMPARE(pc->runViewId(), mine);
    QCOMPARE(runs->runViewState(), QString());
    QTRY_VERIFY_WITH_TIMEOUT(!pc->runSections().at(0).toMap().value("tree").toMap().isEmpty(), 10000);
    const QVariantMap sec = pc->runSections().at(0).toMap();
    // ui.run only (no ui.screen): its "run" tree fills the plugin tab; the title "Board" belongs to Uzume's own tab, so the plugin name is used.
    QCOMPARE(sec.value("slot").toString(), QStringLiteral("run"));
    QCOMPARE(sec.value("title").toString(), QStringLiteral("Run Notes"));
    QCOMPARE(sec.value("tree").toMap().value("text").toString(), QStringLiteral("view=true Mine read only outside the game"));
    QTRY_COMPARE(runs->runTabs().size(), 1);
    QVERIFY(!h.item("runPluginTab")->isVisible());  // the Board tab is open
    QVERIFY(h.click(("tab_" + runs->runTabs().at(0).toMap().value("title").toString()).toUtf8().constData()));
    QCOMPARE(runs->runTab(), "plugin:" + spec.id);
    QTRY_VERIFY(h.item("runPluginTab")->isVisible() && !h.item("runBoardTab")->isVisible());
    QTRY_VERIFY(h.item(("pluginRun_" + spec.id).toUtf8().constData()) != nullptr && h.item(("pluginRun_" + spec.id).toUtf8().constData())->isVisible());
    shot(h, QStringLiteral("4j-5-plugin-tab"));
    QVERIFY(hub.runStatePuts.isEmpty());

    // Rebuilding the same detail does not restart the plugin.
    const auto log0 = pc->host()->consoleSnapshot(spec.id).log.size();
    runs->openRun(mine);
    QTest::qWait(300);
    QCOMPARE(pc->host()->consoleSnapshot(spec.id).log.size(), log0);
    QCOMPARE(pc->runSections().size(), 1);

    // Back to the list: the plugins stop.
    runs->closeRun();
    QVERIFY(pc->runViewPluginIds().isEmpty());
    QVERIFY(pc->runSections().isEmpty());
    QVERIFY(pc->runViewId().isEmpty());
    // Opening it again after a game would restart it: a game start ends the view, the end of the game brings it back.
    runs->openRun(mine);
    QTRY_COMPARE_WITH_TIMEOUT(pc->runSections().size(), 1, 10000);
    pc->gameStarted(QStringLiteral("nds"), QStringLiteral("/nonexistent/missing.nds"), QStringLiteral("abc"), QStringLiteral("Homebrew"));
    QVERIFY(pc->runSections().isEmpty());
    QVERIFY(pc->runViewId().isEmpty());
    pc->gameStopped();
    QTRY_COMPARE_WITH_TIMEOUT(pc->runSections().size(), 1, 10000);
    QCOMPARE(pc->runViewId(), mine);
  }

  // A plugin that declares ui.screen: its screen tree is the plugin tab (last, named after the screen title), fb.ui.badge shows on the
  // tab, fb.ui.open("screen") selects the tab in the open run and opens the screen sheet otherwise; a plugin the Hub removed loses its tab.
  void pluginTabFromScreen() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-tab");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1"), QStringLiteral("runs_v1"), QStringLiteral("runs_v2"), QStringLiteral("sessions_v1")};
    FbSpec spec;
    spec.id = kPluginId;
    spec.name = "Pair Board";
    spec.permissions = {"run.state", "ui"};
    spec.ui = QJsonObject{{"screen", QJsonObject{{"title", "Pairs"}}}, {"run", QJsonObject{{"title", "Ignored"}}}};
    spec.lua = "fb.ui.set('screen', {type = 'text', text = 'screen ' .. tostring(fb.run.current().view)})\n"
               "fb.ui.set('run', {type = 'text', text = 'run tree'})\n"
               "fb.ui.badge(3)\n";
    const QByteArray file = makeUzPlugin(spec);
    const QString sha = sha256Hex(file);
    hub.pluginList.append(QJsonObject{{"plugin_id", spec.id}, {"version", spec.version}, {"name", spec.name}, {"description", ""}, {"api", 1},
                                      {"origin", "unverified"}, {"sha256", sha}, {"size", file.size()}, {"permissions", QJsonArray{"run.state", "ui"}},
                                      {"created_at", "2026-10-12T10:00:00Z"}});
    hub.pluginFiles.insert(spec.id + "/" + spec.version, file);
    const QJsonArray refs{QJsonObject{{"plugin_id", spec.id}, {"version", spec.version}, {"sha256", sha}}};
    const QString mine = hub.makeRun(QStringLiteral("Mine"), {hub.runsMe}, hub.runsMe, refs).value("run_id").toString();
    QVERIFY(hub.start());

    Harness h;
    QVERIFY(h.start());
    h.window->resize(1440, 900);
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    PluginsController* pc = h.controller->plugins();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(pc->hubPlugins().size(), 1, 8000);
    QVERIFY(h.click("navRuns"));
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("runs"), 8000);

    // Outside a run view, fb.ui.open("screen") asks for the screen sheet; a plugin that does not run here (not installed) has none.
    QSignalSpy sheet(h.controller.get(), &PlayerController::pluginScreenRequested);
    h.controller->openPluginScreen(spec.id);
    QCOMPARE(sheet.size(), 0);

    runs->openRun(mine);
    QTRY_COMPARE_WITH_TIMEOUT(runs->runTabs().size(), 1, 15000);
    const QVariantMap tab = runs->runTabs().at(0).toMap();
    QCOMPARE(tab.value("pluginId").toString(), spec.id);
    QCOMPARE(tab.value("title").toString(), QStringLiteral("Pairs"));  // ui.screen title wins over the ui.run title
    QTRY_COMPARE(runs->runTabs().at(0).toMap().value("badge").toInt(), 3);  // fb.ui.badge(3) shows on the tab
    const QVariantMap sec = pc->runSections().at(0).toMap();
    QCOMPARE(sec.value("slot").toString(), QStringLiteral("screen"));
    QTRY_COMPARE_WITH_TIMEOUT(pc->runSections().at(0).toMap().value("tree").toMap().value("text").toString(), QStringLiteral("screen true"), 10000);

    // Tabs: Board, Recap, Log, then the plugin tab, with the badge.
    QTRY_VERIFY(h.item("runTabs") != nullptr && h.item("runTabs")->isVisible());
    const QVariantList opts = h.item("runTabs")->property("items").toList();
    QCOMPARE(opts.size(), 4);
    QCOMPARE(opts.at(0).toString(), QStringLiteral("Board"));
    QCOMPARE(opts.at(1).toString(), QStringLiteral("Recap"));
    QCOMPARE(opts.at(2).toString(), QStringLiteral("Log"));
    QCOMPARE(opts.at(3).toString(), QStringLiteral("Pairs"));
    QTRY_COMPARE(h.item("tabCountText_Pairs")->property("text").toString(), QStringLiteral("3"));  // the badge sits on the tab
    QCOMPARE(runs->runTab(), QStringLiteral("board"));

    // The run view instance is the plugin: fb.ui.open("screen") selects its tab, no sheet.
    h.controller->openPluginScreen(spec.id);
    QCOMPARE(sheet.size(), 0);
    QCOMPARE(runs->runTab(), "plugin:" + spec.id);
    QTRY_VERIFY(h.item("runPluginTab")->isVisible());
    QTRY_VERIFY(h.item(("pluginRun_" + spec.id).toUtf8().constData()) != nullptr);
    QVERIFY(!h.item("runBoardTab")->isVisible());

    // The Hub removes the plugin: its tab goes, Board and Log keep working.
    hub.pluginList = QJsonArray();
    pc->refreshHubPlugins();
    QTRY_COMPARE_WITH_TIMEOUT(pc->hubPlugins().size(), 0, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(runs->runTabs().isEmpty(), 8000);
    QCOMPARE(runs->runTab(), QStringLiteral("board"));
    QTRY_VERIFY(h.item("runBoardTab")->isVisible());
    QTest::qWait(300);  // the tab bar rebuilt its tabs
    QVERIFY(h.click("tab_Log"));
    QTRY_COMPARE(runs->runTab(), QStringLiteral("log"));
    QTRY_VERIFY(h.item("runLogTab")->isVisible());
    QCOMPARE(h.item("runTabs")->property("items").toList().size(), 3);
    QVERIFY(h.item("tab_Pairs") == nullptr);
  }

  // Runs are always visible with runs_v1: the entry and the screen exist without a plugin and without a run, with the empty state.
  void entryShowsWithoutPluginAndRuns() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-empty");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1"), QStringLiteral("runs_v1"), QStringLiteral("sessions_v1")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1440, 900);
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->runs()->loaded(), 8000);
    QVERIFY(h.controller->runs()->available());  // no invite, no installed run plugin: still there
    QTRY_VERIFY(h.item("navRuns") != nullptr && h.item("navRuns")->isVisible());
    QVERIFY(h.click("navRuns"));
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("runs"), 8000);
    QTRY_VERIFY(h.item("runsEmpty") != nullptr && h.item("runsEmpty")->isVisible());
    QCOMPARE(h.item("runsEmpty")->property("title").toString(), QStringLiteral("No runs yet"));
    QCOMPARE(h.item("runsEmpty")->property("text").toString(), QStringLiteral("A run is a playthrough with its own save slot, for 1-8 people."));
    QCOMPARE(h.item("stateBlockAction")->property("text").toString(), QStringLiteral("New run\u2026"));
    QCOMPARE(h.item("runsStartButton")->property("text").toString(), QStringLiteral("New run\u2026"));
    // No plugin offered and no runs_v2: the dialog says so and a run cannot start.
    QVERIFY(h.click("stateBlockAction"));
    QTRY_VERIFY(popupOpen(h, "runStartDialog"));
    QTRY_VERIFY(h.item("runPluginsNone") != nullptr && h.item("runPluginsNone")->isVisible());
    QVERIFY(!h.item("runRulesSelect")->isVisible());
    QVERIFY(h.controller->runs()->startRulesOptions().isEmpty());
  }

  // runs_v2: "None" comes first and a run without plugins is created with an empty plugin list. The Hub offers no plugin feature
  // at all, so nothing is fetched and the plugin-less run can be played.
  void runWithoutPlugin() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-plain");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("runs_v1"), QStringLiteral("runs_v2"), QStringLiteral("sessions_v1")};
    hub.games = QJsonObject{
        {"games", QJsonArray{QJsonObject{{"id", kGameId},
                                         {"title", "Homebrew Quest"},
                                         {"system", "nds"},
                                         {"rom", QJsonObject{{"sha256", QString(64, QLatin1Char('b'))}, {"size", 4096}, {"filename", "quest.nds"}}},
                                         {"added_at", "2026-01-01T12:00:00Z"}}}}};
    hub.fakeUsers = QJsonArray{QJsonObject{{"id", "u_test_1"}, {"display_name", "Tester"}, {"online", true}}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1440, 900);
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded(), 8000);
    QVERIFY(runs->available() && runs->pluginlessRuns());
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item("runsStartButton") != nullptr);
    QVERIFY(h.click("runsStartButton"));
    QTRY_VERIFY(popupOpen(h, "runStartDialog"));
    QTRY_VERIFY_WITH_TIMEOUT(!runs->startUsersLoading(), 8000);
    QCOMPARE(runs->startRulesOptions().size(), 1);
    QCOMPARE(runs->startRulesOptions().at(0).toMap().value("value").toString(), QString());
    QCOMPARE(runs->startRulesOptions().at(0).toMap().value("label").toString(), QStringLiteral("None"));
    QCOMPARE(runs->startRulesCurrent(), QString());
    QVERIFY(h.item("runRulesSelect")->isVisible());
    QCOMPARE(runs->startRules()->rowCount(), 0);  // no plugin: no rule settings
    h.item("runNameField")->setProperty("text", QStringLiteral("Ember Route"));
    QTRY_COMPARE(h.item("runStartSlotNote")->property("text").toString(), QStringLiteral("Each person gets Run: Ember Route. Main is not touched."));
    QTRY_COMPARE_WITH_TIMEOUT(runs->games()->rowCount(), 1, 8000);
    QVERIFY(h.click(("runGame_" + kGameId).toUtf8().constData()));
    QVERIFY(h.click("runStartConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(hub.runs.size() == 1, 8000);
    QVERIFY(hub.lastRunBody.contains(QStringLiteral("plugins")));
    QVERIFY(hub.lastRunBody.value("plugins").toArray().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!runs->openRunId().isEmpty(), 8000);
    const QString runId = runs->openRunId();
    QTRY_VERIFY(h.item("runDetailHeader") != nullptr && h.item("runDetailHeader")->isVisible());
    QCOMPARE(runs->detail().value("pluginsText").toString(), QStringLiteral("No rules"));
    QVERIFY(runs->detail().value("plain").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(runs->active()->rowCount() > 0, 8000);  // the run list refresh can land after the detail opens
    {
        const auto* am = qobject_cast<KeyedRowsModel*>(runs->active());
        QVERIFY(am != nullptr);
        QCOMPARE(am->get(0).value("rulesName").toString(), QString());
        QCOMPARE(am->get(0).value("rulesVersion").toString(), QString());
        QVERIFY(!am->get(0).value("game").toString().isEmpty());
    }
    QVERIFY(runs->runTabs().isEmpty());
    QVERIFY(h.item("tab_Board") != nullptr && h.item("tab_Log") != nullptr);
    QCOMPARE(h.item("runParticipantSlot")->property("text").toString(), QStringLiteral("Run: Ember Route"));
    QCOMPARE(runs->saveSlotFor(runId), runSaveSlot(runId));
    QCOMPARE(runs->runViewState(), QString());
    QVERIFY(!runs->busy());
    // Play: nothing to fetch, the launch goes out with the run's own slot.
    QSignalSpy launch(runs, &RunsController::launchRequested);
    QTRY_VERIFY(h.item("runPlay") != nullptr && h.item("runPlay")->isVisible());
    QVERIFY(h.click("runPlay"));
    QTRY_COMPARE_WITH_TIMEOUT(launch.size(), 1, 8000);
    QTRY_VERIFY(!runs->message().isEmpty());  // no core for the game in the test host: the reason shows, the run's slot is not left selected
    QVERIFY(h.controller->saveSync()->slotFor(kGameId) != runSaveSlot(runId));
    QCOMPARE(runs->launchFor(runId).pins.size(), 0);
  }

  void entryStaysHiddenWithoutHubSupport() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-noruns");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1")};  // no runs_v1
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QVERIFY(!h.controller->runs()->available());
    h.controller->showRuns();
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
    QVERIFY(h.item("navRuns") != nullptr && h.item("navRuns")->isVisible());  // the nav entry is always listed
  }
  // The game picker keeps what every plugin of the run accepts: system and, when the Hub knows it, the game code (globs,
  // case-insensitive). Unknown codes stay after the matching games with a hint; a known code that matches nothing is dropped.
  void gamePickerFiltersByGameCode() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-codes");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1"), QStringLiteral("runs_v1"), QStringLiteral("sessions_v1")};
    const auto game = [](const char* id, const char* title, const char* system, const char* code) {
      return QJsonObject{{"id", id},
                         {"title", title},
                         {"system", system},
                         {"game_code", code},
                         {"game_code_kind", QString::fromLatin1(code).isEmpty() ? "" : "nds-gamecode"},
                         {"rom", QJsonObject{{"sha256", QString(64, QLatin1Char('b'))}, {"size", 4096}, {"filename", "x.nds"}}},
                         {"added_at", "2026-01-01T12:00:00Z"}};
    };
    hub.games = QJsonObject{{"games", QJsonArray{game("g-alpha", "Alpha", "nds", "ABCE"), game("g-zeta", "Zeta", "nds", "BXYZ"),
                                                 game("g-unknown", "Unknown", "nds", ""), game("g-aaa", "Aaa unreadable", "nds", ""),
                                                 game("g-other", "Other", "nds", "CCCC"), game("g-beta", "Beta", "nds", "aqqe"),
                                                 game("g-gba", "Advance", "gba", "")}}};
    hub.fakeUsers = QJsonArray{QJsonObject{{"id", "u_test_1"}, {"display_name", "Tester"}, {"online", true}},
                               QJsonObject{{"id", "u_lena"}, {"display_name", "Lena"}, {"online", true}}};
    const auto offer = [&](const QString& id, const QJsonArray& games) {
      hub.pluginList.append(QJsonObject{{"plugin_id", id}, {"version", "1.0.0"}, {"name", id}, {"description", ""}, {"api", 1},
                                        {"origin", "unverified"}, {"sha256", QString(64, QLatin1Char('c'))}, {"size", 10},
                                        {"permissions", QJsonArray{"run.state"}}, {"games", games}, {"created_at", "2026-10-12T10:00:00Z"}});
    };
    offer(QStringLiteral("net.test.one"), QJsonArray{QJsonObject{{"system", "nds"}, {"codes", QJsonArray{"A??E", "BXYZ"}}}});
    offer(QStringLiteral("net.test.two"), QJsonArray{QJsonObject{{"system", "NDS"}, {"codes", QJsonArray{"A*"}}}});
    offer(QStringLiteral("net.test.any"), QJsonArray{QJsonObject{{"system", "nds"}}});
    const auto ref = [](const char* id) {
      return QJsonObject{{"plugin_id", id}, {"version", "1.0.0"}, {"sha256", QString(64, QLatin1Char('c'))}};
    };
    const auto addRun = [&](const QJsonArray& refs) {
      QJsonObject run = hub.makeRun(QStringLiteral("Run"), {QStringLiteral("u_lena"), hub.runsMe}, QStringLiteral("u_lena"), refs);
      hub.runs.insert(run.value("run_id").toString(), run);
      return run.value("run_id").toString();
    };
    const QString one = addRun({ref("net.test.one")});
    const QString both = addRun({ref("net.test.one"), ref("net.test.two")});
    const QString any = addRun({ref("net.test.any")});
    QVERIFY(hub.start());

    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->plugins()->hubPlugins().size(), 3, 8000);

    const auto rowsOf = [&]() {
      QList<QVariantMap> out;
      QAbstractItemModel* m = runs->games();
      const QHash<int, QByteArray> names = m->roleNames();
      for (int r = 0; r < m->rowCount(); ++r) {
        QVariantMap row;
        for (auto it = names.begin(); it != names.end(); ++it) row.insert(QString::fromUtf8(it.value()), m->data(m->index(r, 0), it.key()));
        out.append(row);
      }
      return out;
    };
    const auto titles = [&](QStringList* hints = nullptr) {
      QStringList out;
      for (const QVariantMap& r : rowsOf()) {
        out << r.value("title").toString();
        if (hints != nullptr) *hints << r.value("hint").toString();
      }
      return out;
    };
    // One plugin: glob A??E matches ABCE and AQQE (any case), BXYZ is listed literally; CCCC and the GBA game are dropped.
    // Games whose code the Hub does not know stay, after the matching ones, with a hint.
    QStringList hints;
    QTRY_COMPARE_WITH_TIMEOUT((runs->prepareGames(one), titles(&(hints = QStringList()))),
                              (QStringList{"Alpha", "Beta", "Zeta", "Aaa unreadable", "Unknown"}), 8000);
    QCOMPARE(hints, (QStringList{"", "", "", "Game not recognized", "Game not recognized"}));
    // Two plugins: the intersection (A* drops Zeta).
    runs->prepareGames(both);
    QCOMPARE(titles(), (QStringList{"Alpha", "Beta", "Aaa unreadable", "Unknown"}));
    // A plugin whose pattern has no codes accepts every game of the system, known or not.
    runs->prepareGames(any);
    QCOMPARE(titles(&(hints = QStringList())), (QStringList{"Aaa unreadable", "Alpha", "Beta", "Other", "Unknown", "Zeta"}));
    QCOMPARE(hints, QStringList(6));

    // The Start dialog shows the same filtered list for the ticked plugins.
    runs->beginStart();
    QTRY_COMPARE_WITH_TIMEOUT(runs->startPlugins()->rowCount(), 3, 8000);
    runs->toggleStartPlugin(QStringLiteral("net.test.one"));
    QCOMPARE(titles(), (QStringList{"Alpha", "Beta", "Zeta", "Aaa unreadable", "Unknown"}));
    runs->toggleStartPlugin(QStringLiteral("net.test.two"));
    QCOMPARE(titles(), (QStringList{"Alpha", "Beta", "Aaa unreadable", "Unknown"}));
    // A game that no longer fits cannot be started with.
    runs->startRun(QStringLiteral("Mismatch"), QStringLiteral("g-zeta"));
    QVERIFY(!runs->startError().isEmpty());
    QCOMPARE(hub.runs.size(), 3);
  }

  // A single fitting game that arrives after the Join dialog opened is selected like on open, never over the user's choice.
  void joinDialogSelectsLateSingleGame() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-late");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1"), QStringLiteral("runs_v1"), QStringLiteral("sessions_v1")};
    hub.fakeUsers = QJsonArray{QJsonObject{{"id", "u_test_1"}, {"display_name", "Tester"}, {"online", true}},
                               QJsonObject{{"id", "u_lena"}, {"display_name", "Lena"}, {"online", true}}};
    hub.pluginList.append(QJsonObject{{"plugin_id", "net.test.late"}, {"version", "1.0.0"}, {"name", "Late"}, {"description", ""}, {"api", 1},
                                      {"origin", "unverified"}, {"sha256", QString(64, QLatin1Char('c'))}, {"size", 10},
                                      {"permissions", QJsonArray{"run.state"}}, {"games", QJsonArray{QJsonObject{{"system", "nds"}}}},
                                      {"created_at", "2026-10-12T10:00:00Z"}});
    QJsonObject run = hub.makeRun(QStringLiteral("Late"), {QStringLiteral("u_lena"), hub.runsMe}, QStringLiteral("u_lena"),
                                  QJsonArray{QJsonObject{{"plugin_id", "net.test.late"}, {"version", "1.0.0"}, {"sha256", QString(64, QLatin1Char('c'))}}});
    const QString runId = run.value("run_id").toString();
    hub.runs.insert(runId, run);
    QVERIFY(hub.start());

    Harness h;
    QVERIFY(h.start());
    h.window->resize(1440, 900);
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->plugins()->hubPlugins().size(), 1, 8000);
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item(("runInvite_" + runId).toUtf8().constData()) != nullptr);
    QVERIFY(h.click("runInviteJoin"));
    QTRY_VERIFY(popupOpen(h, "runJoinDialog"));
    QCOMPARE(runs->games()->rowCount(), 0);
    QCOMPARE(h.item("runJoinGames")->property("selectedId").toString(), QString());

    hub.games = QJsonObject{{"games", QJsonArray{QJsonObject{{"id", kGameId},
                                                             {"title", "Late Game"},
                                                             {"system", "nds"},
                                                             {"rom", QJsonObject{{"sha256", QString(64, QLatin1Char('b'))}, {"size", 4096}, {"filename", "l.nds"}}},
                                                             {"added_at", "2026-01-01T12:00:00Z"}}}}};
    h.controller->hubLibrary()->reload();
    QTRY_COMPARE_WITH_TIMEOUT(runs->games()->rowCount(), 1, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(h.item("runJoinGames")->property("selectedId").toString(), kGameId, 8000);
    // a choice of the user stays
    h.item("runJoinGames")->setProperty("selectedId", QStringLiteral("user-choice"));
    h.controller->hubLibrary()->reload();
    QTest::qWait(400);
    QCOMPARE(h.item("runJoinGames")->property("selectedId").toString(), QStringLiteral("user-choice"));
  }

  static uzume::HubPluginInfo hubInfo(const QString& id, const QString& version, const QString& name, const QJsonObject& requires_ = {}) {
    QJsonObject o{{"plugin_id", id}, {"version", version}, {"name", name}, {"origin", "unverified"}, {"sha256", QString(64, QLatin1Char('c'))},
                  {"permissions", QJsonArray{"run.state"}}, {"requires", requires_}};
    return *uzume::parseHubPluginInfo(o);
  }

  // Requires: newest fitting version, transitive, deduplicated; missing and conflicting ranges give a start error.
  void resolveStartPluginsRules() {
    using uzume::ui::resolveStartPlugins;
    const QList<uzume::HubPluginInfo> offered{
        hubInfo("net.t.soul", "1.0.0", "SoulLink", {{"net.t.core", "^0.1"}, {"net.t.base", "*"}}),
        hubInfo("net.t.nuz", "1.0.0", "Nuzlocke", {{"net.t.core", "^0.1"}}),
        hubInfo("net.t.core", "0.1.0", "Core"), hubInfo("net.t.core", "0.1.7", "Core", {{"net.t.base", "^1.0"}}), hubInfo("net.t.core", "0.2.0", "Core"),
        hubInfo("net.t.base", "1.0.0", "Base"), hubInfo("net.t.base", "1.4.0", "Base"), hubInfo("net.t.base", "2.0.0", "Base"),
        hubInfo("net.t.strict", "1.0.0", "Strict", {{"net.t.core", "^0.2"}}),
        hubInfo("net.t.lost", "1.0.0", "Lost", {{"net.t.gone", "^1"}})};
    auto r = resolveStartPlugins(offered, {"net.t.soul", "net.t.nuz"});
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(r.picked.size(), 2);
    QCOMPARE(r.included.size(), 2);  // core (once) and base (transitive, also needed by core 0.1.7)
    QCOMPARE(r.included.at(0).info.id, QString("net.t.base"));
    QCOMPARE(r.included.at(0).info.version, QString("1.4.0"));  // soul says "*", core 0.1.7 says ^1.0: the newest that fits both
    QCOMPARE(r.included.at(1).info.version, QString("0.1.7"));
    QCOMPARE(r.included.at(1).neededBy, (QStringList{"Nuzlocke", "SoulLink"}));  // once, with both reasons
    QCOMPARE(r.total(), 4);
  }
  void resolveStartPluginsNarrowing() {
    using uzume::ui::resolveStartPlugins;
    const QList<uzume::HubPluginInfo> offered{
        hubInfo("net.t.soul", "1.0.0", "SoulLink", {{"net.t.core", "^0.1"}}),
        hubInfo("net.t.core", "0.1.0", "Core"), hubInfo("net.t.core", "0.1.7", "Core", {{"net.t.base", "^1.0"}}), hubInfo("net.t.core", "0.2.0", "Core"),
        hubInfo("net.t.base", "1.0.0", "Base"), hubInfo("net.t.base", "1.4.0", "Base"), hubInfo("net.t.base", "2.0.0", "Base")};
    auto r = resolveStartPlugins(offered, {"net.t.soul"});
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(r.included.size(), 2);
    QCOMPARE(r.included.at(0).info.id, QString("net.t.base"));
    QCOMPARE(r.included.at(0).info.version, QString("1.4.0"));
    QCOMPARE(r.included.at(0).neededBy, QStringList{"Core"});
    QCOMPARE(r.included.at(1).info.id, QString("net.t.core"));
    QCOMPARE(r.included.at(1).info.version, QString("0.1.7"));  // newest satisfying ^0.1
    QCOMPARE(r.included.at(1).neededBy, QStringList{"SoulLink"});
  }
  void resolveStartPluginsErrors() {
    using uzume::ui::resolveStartPlugins;
    const QList<uzume::HubPluginInfo> offered{
        hubInfo("net.t.soul", "1.0.0", "SoulLink & Nuzlocke", {{"net.t.core", "^0.1"}}), hubInfo("net.t.core", "0.2.0", "SoulLink Core"),
        hubInfo("net.t.strict", "1.0.0", "Strict", {{"net.t.core", "^0.2"}}), hubInfo("net.t.lost", "1.0.0", "Lost", {{"net.t.gone", "^1"}}),
        hubInfo("net.t.bad", "1.0.0", "Bad", {{"net.t.core", "huh"}})};
    QCOMPARE(resolveStartPlugins(offered, {"net.t.soul"}).error, QString("SoulLink & Nuzlocke needs SoulLink Core ^0.1, which this server does not offer."));
    QCOMPARE(resolveStartPlugins(offered, {"net.t.lost"}).error, QString("Lost needs net.t.gone ^1, which this server does not offer."));
    const QString conflict = resolveStartPlugins(offered, {"net.t.soul", "net.t.strict"}).error;
    QVERIFY2(conflict.contains("SoulLink & Nuzlocke needs SoulLink Core ^0.1") && conflict.contains("Strict needs SoulLink Core ^0.2"), qPrintable(conflict));
    QVERIFY(resolveStartPlugins(offered, {"net.t.bad"}).error.contains("not valid"));
    QVERIFY(resolveStartPlugins(offered, {"net.t.strict"}).error.isEmpty());
  }

  // Start dialog: rules from run_settings, required plugins included automatically, settings and pins sent to the Hub.
  void startDialogRulesAndRequires() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-rules");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1"), QStringLiteral("runs_v1"), QStringLiteral("sessions_v1")};
    hub.games = QJsonObject{{"games", QJsonArray{QJsonObject{{"id", kGameId}, {"title", "Quest"}, {"system", "nds"}, {"code", "ABCD"},
                                                             {"rom", QJsonObject{{"sha256", QString(64, QLatin1Char('b'))}, {"size", 4096}, {"filename", "q.nds"}}},
                                                             {"added_at", "2026-01-01T12:00:00Z"}}}}};
    hub.fakeUsers = QJsonArray{QJsonObject{{"id", "u_test_1"}, {"display_name", "Tester"}, {"online", true}}};
    const auto entry = [&](const QString& id, const QString& ver, const QString& name, const QJsonArray& perms, const QJsonObject& req, const QJsonArray& rules,
                           const QJsonArray& games) {
      // The packages the run will pin are served for real (the run view fetches them once the run is created).
      QString sha(64, QLatin1Char('c'));
      qint64 size = 10;
      if (id != QLatin1String("net.test.core") || ver == QLatin1String("0.1.4")) {
        FbSpec fs;
        fs.id = id;
        fs.version = ver;
        fs.name = name;
        fs.permissions = {};
        for (const QJsonValue& pv : perms) fs.permissions << pv.toString();
        for (auto rq = req.begin(); rq != req.end(); ++rq) fs.requires_.insert(rq.key(), rq.value());
        const QByteArray file = makeUzPlugin(fs);
        sha = sha256Hex(file);
        size = file.size();
        hub.pluginFiles.insert(id + "/" + ver, file);
      }
      QJsonObject o{{"plugin_id", id}, {"version", ver}, {"name", name}, {"description", ""}, {"api", 1}, {"origin", "unverified"},
                    {"sha256", sha}, {"size", size}, {"permissions", perms}, {"requires", req}, {"games", games},
                    {"created_at", "2026-10-12T10:00:00Z"}};
      if (!rules.isEmpty()) {
        o.insert("run_settings", rules);
        o.insert("run_settings_strings", QJsonObject{{"en", QJsonObject{{"r.mode", "Mode"}, {"r.mode.hint", "How the run is played"}, {"r.soul", "SoulLink"}, {"r.dupes", "Dupes clause"}}}});
      }
      hub.pluginList.append(o);
    };
    const QJsonArray rules{
        QJsonObject{{"key", "mode"}, {"type", "enum"}, {"label", "r.mode"}, {"hint", "r.mode.hint"}, {"default", "soullink"},
                    {"options", QJsonArray{QJsonObject{{"value", "soullink"}, {"label", "r.soul"}}, QJsonObject{{"value", "nuzlocke"}, {"label", "nuz.missing"}}}}},
        QJsonObject{{"key", "rules.dupes_clause"}, {"type", "boolean"}, {"label", "r.dupes"}, {"default", true}}};
    entry("net.test.soul", "1.0.0", "SoulLink", {"run.state"}, {{"net.test.core", "^0.1"}}, rules, {QJsonObject{{"system", "nds"}, {"codes", QJsonArray{"ABC*"}}}});
    entry("net.test.core", "0.1.0", "Core", {"memory.read"}, {}, {}, {QJsonObject{{"system", "nds"}, {"codes", QJsonArray{"AB??"}}}});
    entry("net.test.core", "0.1.4", "Core", {"memory.read"}, {}, {}, {QJsonObject{{"system", "nds"}, {"codes", QJsonArray{"AB??"}}}});
    entry("net.test.core", "0.2.0", "Core", {"memory.read"}, {}, {}, {});
    entry("net.test.plain", "1.0.0", "Plain", {"run.state"}, {}, {}, {});
    {
      QJsonObject run = hub.makeRun(QStringLiteral("Existing"), {hub.runsMe}, hub.runsMe,
                                    QJsonArray{QJsonObject{{"plugin_id", "net.test.plain"}, {"version", "1.0.0"}, {"sha256", QString(64, QLatin1Char('c'))}}});
      hub.runs.insert(run.value("run_id").toString(), run);  // makes the Runs entry show
    }
    QVERIFY(hub.start());

    Harness h;
    QVERIFY(h.start());
    h.window->resize(1440, 900);
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->plugins()->hubPlugins().size(), 3, 8000);
    // Runs need no installed plugin: the entry is there with runs_v1 alone.
    QVERIFY(runs->available());
    QVERIFY(!runs->pluginlessRuns());  // no runs_v2 on this Hub
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item("runsStartButton") != nullptr);
    QVERIFY(h.click("runsStartButton"));
    QTRY_VERIFY(popupOpen(h, "runStartDialog"));
    QTRY_COMPARE_WITH_TIMEOUT(runs->startPlugins()->rowCount(), 2, 8000);  // soul and plain; core has no run.state
    QCOMPARE(runs->startRules()->rowCount(), 0);  // no plugin picked: no rule settings
    QCOMPARE(runs->startIncluded()->rowCount(), 0);
    // The Rules select lists the offered run plugins only (no "None" without runs_v2); a pick replaces the previous one.
    QCOMPARE(runs->startRulesOptions().size(), 2);
    for (const QVariant& o : runs->startRulesOptions()) QVERIFY(!o.toMap().value("value").toString().isEmpty());
    QCOMPARE(runs->startRulesCurrent(), QString());

    runs->selectStartRules(QStringLiteral("net.test.plain"));
    QCOMPARE(runs->startRulesCurrent(), QStringLiteral("net.test.plain"));
    QCOMPARE(runs->startRules()->rowCount(), 0);  // no run_settings: nothing to set
    runs->selectStartRules(QString());            // None is refused without runs_v2
    QCOMPARE(runs->startRulesCurrent(), QStringLiteral("net.test.plain"));
    runs->selectStartRules(QStringLiteral("net.test.soul"));
    QCOMPARE(runs->startRulesCurrent(), QStringLiteral("net.test.soul"));
    int checked = 0;
    for (const QVariantMap& r : static_cast<uzume::ui::KeyedRowsModel*>(runs->startPlugins())->rows()) checked += r.value("checked").toBool() ? 1 : 0;
    QCOMPARE(checked, 1);
    QCOMPARE(runs->startIncluded()->rowCount(), 1);
    const QVariantMap inc = static_cast<uzume::ui::KeyedRowsModel*>(runs->startIncluded())->get(0);
    QCOMPARE(inc.value("text").toString(), QString("Included: Core 0.1.4 · needed by SoulLink"));
    QCOMPARE(runs->startRules()->rowCount(), 3);  // header + 2 settings
    auto* rulesModel = static_cast<uzume::ui::KeyedRowsModel*>(runs->startRules());
    QCOMPARE(rulesModel->get(0).value("title").toString(), QString("SoulLink"));
    QCOMPARE(rulesModel->get(1).value("label").toString(), QString("Mode"));
    QCOMPARE(rulesModel->get(1).value("hint").toString(), QString("How the run is played"));
    QCOMPARE(rulesModel->get(1).value("widget").toString(), QString("segments"));
    QCOMPARE(rulesModel->get(1).value("value").toString(), QString("soullink"));
    QCOMPARE(rulesModel->get(1).value("options").toList().at(1).toMap().value("label").toString(), QString("nuz.missing"));  // unknown key: the key itself
    QCOMPARE(rulesModel->get(2).value("widget").toString(), QString("toggle"));
    QCOMPARE(rulesModel->get(2).value("value").toBool(), true);

    // The dialog shows the select, the group header and the included line.
    QTRY_VERIFY(h.item("runRulesTitle") != nullptr && h.item("runRulesTitle")->isVisible());
    QVERIFY(h.item("runRulesSelect") != nullptr && h.item("runRulesSelect")->isVisible());
    QTRY_VERIFY(h.item("runIncluded_net.test.core") != nullptr && h.item("runIncluded_net.test.core")->isVisible());
    QCOMPARE(h.item("runIncluded_net.test.core")->property("text").toString(), inc.value("text").toString());
    QTRY_VERIFY(h.item("runRulesHeader_net.test.soul") != nullptr && h.item("runRulesHeader_net.test.soul")->isVisible());
    QTRY_VERIFY(h.item("runRule_net.test.soul_rules.dupes_clause") != nullptr && h.item("runRule_net.test.soul_rules.dupes_clause")->isVisible());
    shot(h, QStringLiteral("4j-runs-start-rules"));
    QVERIFY(h.click("runRule_net.test.soul_rules.dupes_clause"));  // the toggle in the dialog
    QCOMPARE(rulesModel->get(2).value("value").toBool(), false);
    QVERIFY(h.click("runRule_net.test.soul_rules.dupes_clause"));
    QCOMPARE(rulesModel->get(2).value("value").toBool(), true);
    runs->setStartRule(QStringLiteral("net.test.soul"), QStringLiteral("mode"), QStringLiteral("bogus"));  // not an option: ignored
    QCOMPARE(rulesModel->get(1).value("value").toString(), QString("soullink"));
    runs->setStartRule(QStringLiteral("net.test.soul"), QStringLiteral("mode"), QStringLiteral("nuzlocke"));
    runs->setStartRule(QStringLiteral("net.test.soul"), QStringLiteral("rules.dupes_clause"), false);
    QCOMPARE(rulesModel->get(1).value("value").toString(), QString("nuzlocke"));
    QCOMPARE(rulesModel->get(2).value("value").toBool(), false);

    // Game filter takes part with the included plugin (AB?? and ABC*): ABCD fits.
    QTRY_COMPARE_WITH_TIMEOUT(runs->games()->rowCount(), 1, 8000);
    runs->startRun(QStringLiteral("Rules Run"), kGameId);
    QTRY_VERIFY_WITH_TIMEOUT(hub.runs.size() == 2, 8000);
    QCOMPARE(hub.lastRunBody.value("settings").toObject(),
             (QJsonObject{{"net.test.soul", QJsonObject{{"mode", "nuzlocke"}, {"rules", QJsonObject{{"dupes_clause", false}}}}}}));
    const QJsonArray pins = hub.lastRunBody.value("plugins").toArray();
    QCOMPARE(pins.size(), 2);
    QStringList got;
    for (const QJsonValue& v : pins) got << v.toObject().value("plugin_id").toString() + "@" + v.toObject().value("version").toString();
    got.sort();
    QCOMPARE(got, (QStringList{"net.test.core@0.1.4", "net.test.soul@1.0.0"}));
  }


  // 0.20 package 6: the core rules (run_rules_v1). The Start dialog has the four rules and sends them with the run; the run view shows them
  // in one line; the creator changes them in the Rules dialog (PUT with the whole object); a push from the server updates the line; a
  // person who is not the creator only reads (and a forced PUT is answered 403).
  void coreRulesAtStartAndInTheRun() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-core-rules");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1"), QStringLiteral("runs_v1"), QStringLiteral("runs_v2"), QStringLiteral("run_rules_v1"),
                    QStringLiteral("sessions_v1")};
    hub.games = QJsonObject{{"games", QJsonArray{QJsonObject{{"id", kGameId}, {"title", "Quest"}, {"system", "nds"}, {"code", "ABCD"},
                                                             {"rom", QJsonObject{{"sha256", QString(64, QLatin1Char('b'))}, {"size", 4096}, {"filename", "q.nds"}}},
                                                             {"added_at", "2026-01-01T12:00:00Z"}}}}};
    hub.fakeUsers = QJsonArray{QJsonObject{{"id", "u_test_1"}, {"display_name", "Tester"}, {"online", true}}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1440, 900);
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded(), 8000);
    QVERIFY(runs->pluginlessRuns());
    QVERIFY(runs->coreRulesAvailable());

    // Start dialog: the defaults are everything off; the segments and toggles change the draft.
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item("runsStartButton") != nullptr);
    QVERIFY(h.click("runsStartButton"));
    QTRY_VERIFY(popupOpen(h, "runStartDialog"));
    QCOMPARE(runs->startCoreRules(), (QVariantMap{{"states", false}, {"rewind", false}, {"cheats", "off"}, {"memoryWrite", "off"}}));
    QTRY_VERIFY(h.item("runCoreRulesTitle") != nullptr && h.item("runCoreRulesTitle")->isVisible());
    QTRY_VERIFY(h.item("runRule_cheats") != nullptr && h.item("runRule_cheats")->isVisible());
    QVERIFY(h.click("runRuleCheatsLogged"));
    QVERIFY(h.click("runRule_states"));
    QCOMPARE(runs->startCoreRules().value("cheats").toString(), QStringLiteral("logged"));
    QCOMPARE(runs->startCoreRules().value("states").toBool(), true);
    runs->setStartCoreRule(QStringLiteral("cheats"), QStringLiteral("allowed"));  // not a level: ignored
    runs->setStartCoreRule(QStringLiteral("bogus"), true);
    QCOMPARE(runs->startCoreRules().value("cheats").toString(), QStringLiteral("logged"));
    shot(h, QStringLiteral("11e-run-start-rules"));
    QTRY_COMPARE_WITH_TIMEOUT(runs->games()->rowCount(), 1, 8000);
    runs->startRun(QStringLiteral("Rules Run"), kGameId);
    QTRY_VERIFY_WITH_TIMEOUT(hub.runs.size() == 1, 8000);
    QCOMPARE(hub.lastRunBody.value("rules").toObject(), (QJsonObject{{"states", true}, {"rewind", false}, {"cheats", "logged"}, {"memory_write", "off"}}));
    const QString runId = hub.runs.keys().first();

    // Run view: one line for everybody, "Rules…" for the creator.
    QTRY_COMPARE_WITH_TIMEOUT(runs->openRunId(), runId, 8000);
    QTRY_VERIFY(h.item("runRulesLine") != nullptr && h.item("runRulesLine")->isVisible());
    QVERIFY(h.item("runRulesLine")->property("text").toString().startsWith(QStringLiteral("Save states on · Rewind off · Cheats allowed, logged · Memory changes off · set by Tester")));
    QVERIFY(runs->detail().value("canEditRules").toBool());
    QTRY_VERIFY(h.item("runRulesOpen") != nullptr && h.item("runRulesOpen")->isVisible());
    QVERIFY(h.click("runRulesOpen"));
    QTRY_VERIFY(popupOpen(h, "runRulesDialog"));
    QVERIFY(!h.item("runRulesSave")->property("enabled").toBool());  // nothing changed yet
    QVERIFY(h.click("runRuleMemoryLogged"));
    QVERIFY(h.click("runRuleCheatsOff"));
    QTRY_VERIFY(h.item("runRulesWarning") != nullptr && h.item("runRulesWarning")->isVisible());
    QVERIFY(h.item("runRulesWarning")->property("text").toString().contains(QStringLiteral("turn off")));
    QVERIFY(h.item("runRulesSave")->property("enabled").toBool());
    shot(h, QStringLiteral("11f-run-rules-dialog"));
    QVERIFY(h.click("runRulesSave"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.runRulesPuts.size(), 1, 8000);
    QCOMPARE(hub.runRulesPuts.last(), (QJsonObject{{"states", true}, {"rewind", false}, {"cheats", "off"}, {"memory_write", "logged"}}));  // the whole object
    QTRY_VERIFY_WITH_TIMEOUT(!popupOpen(h, "runRulesDialog"), 8000);
    QTRY_VERIFY(h.item("runRulesLine")->property("text").toString().contains(QStringLiteral("Cheats off · Memory changes allowed, logged")));

    // The log row of rules.changed names who changed what.
    runs->setRunTab(QStringLiteral("log"));
    bool logged = false;
    for (int i = 0; i < 40 && !logged; ++i) {
      QTest::qWait(100);
      for (const QVariantMap& r : static_cast<uzume::ui::KeyedRowsModel*>(runs->events())->rows())
        logged = logged || r.value("text").toString() == QStringLiteral("Tester changed Cheats: Allowed, logged → Off; Memory changes: Off → Allowed, logged");
    }
    QVERIFY(logged);

    // A change from elsewhere (the server pushes run_updated) shows at once; a person who did not start the run only reads.
    QJsonObject run = hub.runs.value(runId);
    run.insert("rules", QJsonObject{{"states", false}, {"rewind", true}, {"cheats", "logged"}, {"memory_write", "logged"}});
    hub.runs.insert(runId, run);
    hub.pushRun(runId);
    QTRY_VERIFY_WITH_TIMEOUT(h.item("runRulesLine")->property("text").toString().startsWith(QStringLiteral("Save states off · Rewind on · Cheats allowed, logged")), 8000);
    QJsonObject other = hub.makeRun(QStringLiteral("Theirs"), {hub.runsMe, QStringLiteral("u_other")}, QStringLiteral("u_other"));
    other.insert("rules", QJsonObject{{"states", false}, {"rewind", false}, {"cheats", "logged"}, {"memory_write", "off"}});
    hub.runs.insert(other.value("run_id").toString(), other);
    hub.pushRun(other.value("run_id").toString());
    QTRY_VERIFY_WITH_TIMEOUT(runs->client()->run(other.value("run_id").toString()).has_value(), 8000);
    runs->openRun(other.value("run_id").toString());
    QTRY_COMPARE_WITH_TIMEOUT(runs->detail().value("name").toString(), QStringLiteral("Theirs"), 8000);
    QVERIFY(!runs->detail().value("canEditRules").toBool());
    QVERIFY(runs->detail().value("rulesLine").toString().contains(QStringLiteral("Cheats allowed, logged")));
    QTRY_VERIFY(h.item("runRulesLine") != nullptr && h.item("runRulesLine")->isVisible());
    QVERIFY(h.item("runRulesOpen") == nullptr || !h.item("runRulesOpen")->isVisible());
    QSignalSpy saved(runs, &RunsController::rulesSaved);
    runs->saveRunRules(other.value("run_id").toString(), QVariantMap{{"cheats", "off"}});  // the server says 403
    QTRY_VERIFY_WITH_TIMEOUT(!runs->rulesError().isEmpty(), 8000);
    QCOMPARE(runs->rulesError(), QStringLiteral("Only the person who started the run or an admin can change its rules."));
    QCOMPARE(saved.count(), 0);
    QVERIFY(!runs->rulesBusy());
  }

  // A requirement the Hub cannot satisfy blocks the start with a clear error.
  void startDialogRequiresMissing() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-req-missing");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1"), QStringLiteral("runs_v1"), QStringLiteral("sessions_v1")};
    hub.fakeUsers = QJsonArray{QJsonObject{{"id", "u_test_1"}, {"display_name", "Tester"}, {"online", true}}};
    hub.pluginList.append(QJsonObject{{"plugin_id", "net.test.soul"}, {"version", "1.0.0"}, {"name", "SoulLink"}, {"description", ""}, {"api", 1},
                                      {"origin", "unverified"}, {"sha256", QString(64, QLatin1Char('c'))}, {"size", 10},
                                      {"permissions", QJsonArray{"run.state"}}, {"requires", QJsonObject{{"net.test.core", "^0.1"}}},
                                      {"created_at", "2026-10-12T10:00:00Z"}});
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->plugins()->hubPlugins().size(), 1, 8000);
    runs->beginStart();
    QTRY_COMPARE_WITH_TIMEOUT(runs->startPlugins()->rowCount(), 1, 8000);
    runs->toggleStartPlugin(QStringLiteral("net.test.soul"));
    QCOMPARE(runs->startError(), QString("SoulLink needs net.test.core ^0.1, which this server does not offer."));
    runs->startRun(QStringLiteral("X"), QStringLiteral("g"));
    QCOMPARE(runs->startError(), QString("SoulLink needs net.test.core ^0.1, which this server does not offer."));
    QVERIFY(hub.runs.isEmpty());
    runs->toggleStartPlugin(QStringLiteral("net.test.soul"));  // unpicked: the error goes away
    QCOMPARE(runs->startError(), QString());
  }
};

QTEST_MAIN(RunsUiTest)
#include "runs_ui_test.moc"
