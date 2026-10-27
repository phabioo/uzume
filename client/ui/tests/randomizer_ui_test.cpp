// Randomizer screens of the app (Uzume 0.21, randomizer.md 10e, 10f, 10h): Server > Tools (engine facts, Offer to apps, update, remove with
// the 409 counts, no tools yet, no internet), the Randomizer section of "Start a run", joining a randomized run with an own variant, and the
// run board (Run variant / Own variant, the Randomizer block). Offscreen, FakeHub with fixed answers; synthetic data only, no game files and
// no engine.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "fakehub.h"
#include "keyedrowsmodel.h"
#include "runscontroller.h"
#include "testsupport.h"
#include "variantscontroller.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
const QString kApi = QStringLiteral("/api/v1/admin");
const QString kGameId = QStringLiteral("00000000-0000-4000-8000-0000000000a1");

QJsonObject platform(const QString& id, qint64 size, const QString& origin = QStringLiteral("download")) {
  return {{"platform", id}, {"size", size}, {"sha256", QString(64, QLatin1Char('a'))}, {"cached", true}, {"root", ""}, {"origin", origin}, {"url", ""}};
}
QJsonObject version(const QString& v, int variants, int runs) {
  return {{"version", v}, {"installed", true}, {"complete", true}, {"used_by", QJsonObject{{"variants", variants}, {"runs", runs}}},
          {"platforms", QJsonArray{platform(QStringLiteral("windows-x64"), 38000000), platform(QStringLiteral("linux-x64"), 37000000)}}};
}
QByteArray toolBody(bool offered, bool withVersions = true, const QString& sourceState = QStringLiteral("ok"), const QString& latest = QStringLiteral("2.4.0")) {
  QJsonObject t{{"id", "randomizer"}, {"name", "Randomizer engine"}, {"license", "GPL-3.0"}, {"source_url", "https://example.org/engine"},
                {"offered", offered}, {"latest_version", latest}, {"update_available", latest == QLatin1String("2.4.0") && withVersions},
                {"source", QJsonObject{{"url", "https://example.org/releases"}, {"state", sourceState},
                                       {"last_check", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                       {"last_error", sourceState == QLatin1String("error") ? QJsonValue(QStringLiteral("dial tcp: no route to host")) : QJsonValue()}}},
                {"versions", withVersions ? QJsonArray{version(QStringLiteral("2.3.1"), 5, 1), version(QStringLiteral("2.2.0"), 2, 0)} : QJsonArray()}};
  return QJsonDocument(QJsonObject{{"tools", QJsonArray{t}}}).toJson(QJsonDocument::Compact);
}
QByteArray oneTool(bool offered) {
  return QJsonDocument(QJsonDocument::fromJson(toolBody(offered)).object().value("tools").toArray().first().toObject()).toJson(QJsonDocument::Compact);
}
}  // namespace

class RandomizerUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
  }
  static QString text(QQuickItem* it) { return it ? it->property("text").toString() : QString(); }
  static bool shown(QQuickItem* it) { return it != nullptr && it->isVisible(); }
  static bool popupOpen(Harness& h, const char* name) {
    QObject* o = h.window->findChild<QObject*>(QString::fromLatin1(name));
    return o != nullptr && o->property("visible").toBool();
  }
  static void adminHub(FakeHub& hub, const QByteArray& tools) {
    hub.adminUser = true;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("tools_v1"), QStringLiteral("variants_v1")};
    hub.adminRoutes[QStringLiteral("GET ") + kApi + QStringLiteral("/tools")] = {200, tools};
  }
  static void openTools(Harness& h) {
    h.window->resize(1280, 800);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showServerPage(QStringLiteral("tools"));
    QTRY_VERIFY(h.item("serverToolsPage") != nullptr);
  }

  // A hub with runs, tools and variants where the Randomizer plugin registered the test game.
  static void runsHub(FakeHub& hub) {
    hub.hubId = QStringLiteral("hub-rz");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("runs_v1"), QStringLiteral("runs_v2"), QStringLiteral("tools_v1"), QStringLiteral("variants_v1")};
    hub.games = QJsonObject{{"games", QJsonArray{uitest::gameJson(kGameId, QStringLiteral("Coral Saga: Ember"), QString(64, QLatin1Char('b')), 4096)}}};
    hub.fakeUsers = QJsonArray{QJsonObject{{"id", "u_test_1"}, {"display_name", "Tester"}, {"online", true}},
                               QJsonObject{{"id", "u_lena"}, {"display_name", "Lena"}, {"online", true}}};
    hub.customRoute = [](const FakeRequest& req, int& status, QByteArray& body, QByteArray&) {
      Q_UNUSED(status)
      if (req.method == "GET" && req.path == QLatin1String("/api/v1/tools")) {
        const QJsonObject p{{"platform", "linux-x64"}, {"size", 38000000}, {"sha256", QString(64, QLatin1Char('a'))}, {"cached", true}, {"root", ""}};
        const QJsonObject v{{"version", "2.3.1"}, {"used_by", QJsonObject{{"variants", 0}, {"runs", 0}}}, {"platforms", QJsonArray{p}}};
        const QJsonObject t{{"id", "randomizer"}, {"name", "Randomizer engine"}, {"offered", true}, {"version", "2.3.1"}, {"license", "GPL-3.0"},
                            {"source_url", "https://example.org/engine"}, {"versions", QJsonArray{v}}};
        body = QJsonDocument(QJsonObject{{"tools", QJsonArray{t}}}).toJson(QJsonDocument::Compact);
        return true;
      }
      if (req.method == "GET" && req.path.endsWith(QLatin1String("/variants"))) {
        body = R"({"variants":[]})";
        return true;
      }
      return false;
    };
  }
  static void registerGame(Harness& h) {
    plugins::VariantRegistration r;
    r.pluginId = QStringLiteral("uzume.randomizer");
    r.engine = QStringLiteral("randomizer");
    r.presets = {plugins::VariantPreset{QStringLiteral("nuzlocke"), QStringLiteral("Nuzlocke friendly"), QStringLiteral("Fair start"), {{QStringLiteral("428"), QStringLiteral("428nuz")}}},
                 plugins::VariantPreset{QStringLiteral("chaos"), QStringLiteral("Chaos"), QStringLiteral("Everything"), {{QStringLiteral("428"), QStringLiteral("428chaos")}}}};
    r.games = {plugins::VariantGameSupport{QStringLiteral("nds"), {}}};
    h.controller->variants()->registry()->set(r);
  }
  static QJsonObject randomizedRun(FakeHub& hub, bool allowOwn, const QString& meKind, const QString& meState, bool meJoined) {
    QJsonObject run = hub.makeRun(QStringLiteral("Ember Shuffle"), {QStringLiteral("u_lena"), hub.runsMe}, QStringLiteral("u_lena"), {});
    run.insert("randomizer", QJsonObject{{"mode", "each"}, {"preset_id", "nuzlocke"}, {"tool_id", "randomizer"}, {"tool_version", "2.3.1"}, {"allow_own", allowOwn}});
    QJsonArray ps = run.value("participants").toArray();
    for (int i = 0; i < ps.size(); ++i) {
      QJsonObject p = ps.at(i).toObject();
      if (p.value("user_id").toString() == QLatin1String("u_lena")) {
        p.insert("user_name", "Lena");
        p.insert("variant_kind", "own");
        p.insert("variant_state", "building");
      } else if (meJoined) {
        p.insert("status", "joined");
        p.insert("game_id", kGameId);
        p.insert("color", 1);
        p.insert("variant_kind", meKind);
        p.insert("variant_state", meState);
      }
      ps.replace(i, p);
    }
    run.insert("participants", ps);
    hub.runs.insert(run.value("run_id").toString(), run);
    return run;
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // ------------------------------------------------------------------ Server > Tools (10h)
  void toolsTabRendersTheEngine() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub, toolBody(false));
    hub.adminRoutes[QStringLiteral("PUT ") + kApi + QStringLiteral("/tools/randomizer")] = {200, oneTool(true)};
    hub.adminRoutes[QStringLiteral("POST ") + kApi + QStringLiteral("/tools/randomizer/versions/2.4.0/install")] = {200, oneTool(true)};
    hub.adminRoutes[QStringLiteral("POST ") + kApi + QStringLiteral("/tools/randomizer/sync")] = {200, oneTool(false)};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openTools(h);
    QCOMPARE(h.item("serverTabs")->property("active").toString(), QStringLiteral("Tools"));
    QTRY_VERIFY(shown(h.item("toolCard")));
    QVERIFY(text(h.item("toolsIntro")).startsWith(QStringLiteral("Programs Test server offers to apps")));
    QCOMPARE(text(h.item("toolFactEngine")), QStringLiteral("FVX · OPEN-SOURCE RANDOMIZER"));
    QVERIFY2(text(h.item("toolFactVersion")).startsWith(QStringLiteral("2.3.1 · checked today")), qPrintable(text(h.item("toolFactVersion"))));
    QCOMPARE(text(h.item("toolFactSize")), QStringLiteral("36 MB per PC · downloaded once"));
    QCOMPARE(text(h.item("toolFactPlatforms")), QStringLiteral("Windows · Linux"));
    QCOMPARE(text(h.item("toolFactLicense")), QStringLiteral("GPL-3.0 ·"));
    QVERIFY(shown(h.item("toolSourceLink")));
    // Offer to apps is off: the reason is the line under it. The update line offers the newer release.
    QVERIFY(text(h.item("toolOfferNote")).startsWith(QStringLiteral("Apps can't build new variants")));
    QVERIFY(shown(h.item("toolUpdateLine")));
    QCOMPARE(text(h.item("toolUpdate")), QStringLiteral("Update to 2.4.0"));
    // Versions kept: both rows with their counts.
    QTRY_VERIFY(h.item("toolVersionsTableRow_1") != nullptr);
    QCOMPARE(text(h.item("toolVersionsTableCell_0_used")), QStringLiteral("5 variant recipes · 1 run"));
    QCOMPARE(text(h.item("toolVersionsTableCell_1_used")), QStringLiteral("2 variant recipes"));
    QVERIFY(shown(h.item("toolRemove_2.3.1")) && shown(h.item("toolRemove_2.2.0")));
    uitest::saveShot(h.window, QStringLiteral("rz_tools"));

    // Offer to apps: PUT {offered: true}
    QVERIFY(h.click("toolOffer"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("PUT ") + kApi + QStringLiteral("/tools/randomizer")), 1);
    QCOMPARE(QJsonDocument::fromJson(hub.adminBodies.value(QStringLiteral("PUT ") + kApi + QStringLiteral("/tools/randomizer"))).object().value("offered").toBool(), true);
    QTRY_VERIFY(text(h.item("toolOfferNote")).startsWith(QStringLiteral("Apps download the engine")));
    // Check now reads the source; Update to 2.4.0 installs that version.
    QVERIFY(h.click("toolCheckNow"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST ") + kApi + QStringLiteral("/tools/randomizer/sync")), 1);
    QVERIFY(h.clickWhenReady("toolUpdate"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST ") + kApi + QStringLiteral("/tools/randomizer/versions/2.4.0/install")), 1);
  }

  void removeInUseShowsTheCounts() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub, toolBody(true));
    hub.adminRoutes[QStringLiteral("DELETE ") + kApi + QStringLiteral("/tools/randomizer/versions/2.2.0")] = {
        409, R"({"error":{"code":"tool_in_use","message":"in use"},"used_by":{"variants":2,"runs":1}})"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openTools(h);
    QTRY_VERIFY(h.item("toolRemove_2.2.0") != nullptr);
    QVERIFY(h.click("toolRemove_2.2.0"));
    QTRY_VERIFY(popupOpen(h, "toolRemoveDialog"));
    QCOMPARE(h.item("toolRemoveDialog")->property("title").toString(), QStringLiteral("Remove version 2.2.0?"));
    QVERIFY2(h.item("toolRemoveDialog")->property("text").toString().startsWith(QStringLiteral("2 variant recipes use it.")),
             qPrintable(h.item("toolRemoveDialog")->property("text").toString()));
    QVERIFY(h.clickWhenReady("toolRemoveConfirm"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("DELETE ") + kApi + QStringLiteral("/tools/randomizer/versions/2.2.0")), 1);
    // The server refuses (there is no force): the dialog stays and names what pins the version.
    QTRY_COMPARE(h.item("toolRemoveDialog")->property("error").toString(), QStringLiteral("Test server keeps 2.2.0 while 2 variant recipes · 1 run use it."));
    QVERIFY(popupOpen(h, "toolRemoveDialog"));
    QVERIFY(h.click("toolRemoveCancel"));
    QTRY_VERIFY(!popupOpen(h, "toolRemoveDialog"));
  }

  void noInternetAndNoToolsYet() {
    {
      // a: the source cannot be reached; the offline import is a server command (no upload from the app).
      FakeHub hub(QStringLiteral("a"));
      adminHub(hub, toolBody(true, true, QStringLiteral("error")));
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      openTools(h);
      QTRY_VERIFY(shown(h.item("toolSourceProblem")));
      QCOMPARE(text(h.item("toolSourceProblem")), QStringLiteral("Test server can't reach the internet."));
      QVERIFY(text(h.item("toolImportHint")).contains(QStringLiteral("uzume-server import-tools")));
      QVERIFY(h.item("toolImportFile") == nullptr);
    }
    {
      // e: nothing installed yet: Get the engine installs the newest release the source listed.
      FakeHub hub(QStringLiteral("a"));
      adminHub(hub, toolBody(false, false, QStringLiteral("ok"), QStringLiteral("2.3.1")));
      hub.adminRoutes[QStringLiteral("POST ") + kApi + QStringLiteral("/tools/randomizer/versions/2.3.1/install")] = {200, oneTool(false)};
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      openTools(h);
      QTRY_VERIFY(shown(h.item("toolsEmpty")));
      QVERIFY(!shown(h.item("toolCard")));
      QVERIFY(h.clickWhenReady("stateBlockAction"));
      QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST ") + kApi + QStringLiteral("/tools/randomizer/versions/2.3.1/install")), 1);
    }
  }

  // ------------------------------------------------------------------ New run (10e)
  void newRunSendsTheRandomizer() {
    FakeHub hub(QStringLiteral("a"));
    runsHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    pair(h, hub);
    registerGame(h);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded() && runs->available(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->variants()->engine().value("version").toString(), QStringLiteral("2.3.1"), 8000);
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item("runsStartButton") != nullptr);
    QVERIFY(h.click("runsStartButton"));
    QTRY_VERIFY(popupOpen(h, "runStartDialog"));
    QTRY_VERIFY_WITH_TIMEOUT(!runs->startUsersLoading(), 8000);
    // The section shows once the plugin registered games; a game has to be picked for the rest.
    QTRY_VERIFY(shown(h.item("runRandomizer")));
    h.item("runNameField")->setProperty("text", QStringLiteral("Ember Shuffle"));
    QTRY_COMPARE_WITH_TIMEOUT(runs->games()->rowCount(), 1, 8000);
    QVERIFY(h.click(("runGame_" + kGameId).toUtf8().constData()));
    QTRY_VERIFY(shown(h.item("runRandomizerMode")));
    QCOMPARE(h.item("runRandomizerMode")->property("current").toString(), QStringLiteral("off"));
    QVERIFY(!shown(h.item("runRandomizerPreset")));   // Off hides the preset and Allow own variants
    QVERIFY(h.click("runRandomizerModeEach"));
    QTRY_VERIFY(shown(h.item("runRandomizerPreset")));
    QVERIFY(shown(h.item("runRandomizerOwn")));
    QCOMPARE(text(h.item("runRandomizerPin")), QStringLiteral("PINS FVX 2.3.1 FOR THIS RUN"));
    QCOMPARE(text(h.item("runRandomizerNote")), QStringLiteral("Each person’s app builds their copy before the first start. Spoilers stay hidden."));
    QCOMPARE(text(h.item("runStartSlotNote")), QStringLiteral("Each person gets Run: Ember Shuffle. Main and their own variants are not touched."));
    QVERIFY(h.click("runRandomizerOwn"));
    uitest::saveShot(h.window, QStringLiteral("rz_newrun"));
    QVERIFY(h.click("runStartConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(hub.runs.size() == 1, 8000);
    const QJsonObject rz = hub.lastRunBody.value("randomizer").toObject();
    QCOMPARE(rz.value("mode").toString(), QStringLiteral("each"));
    QCOMPARE(rz.value("preset_id").toString(), QStringLiteral("nuzlocke"));
    QCOMPARE(rz.value("settings").toString(), QStringLiteral("428nuz"));
    QCOMPARE(rz.value("tool_id").toString(), QStringLiteral("randomizer"));
    QCOMPARE(rz.value("tool_version").toString(), QStringLiteral("2.3.1"));
    QCOMPARE(rz.value("allow_own").toBool(), true);
  }

  void newRunWithoutEngineOrSupport() {
    FakeHub hub(QStringLiteral("a"));
    runsHub(hub);
    // The server offers no engine: the row stays, turned off, with the reason (10e-2d).
    hub.customRoute = [](const FakeRequest& req, int&, QByteArray& body, QByteArray&) {
      if (req.method == "GET" && req.path == QLatin1String("/api/v1/tools")) {
        body = R"({"tools":[]})";
        return true;
      }
      return false;
    };
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    pair(h, hub);
    registerGame(h);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded() && runs->available(), 8000);
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item("runsStartButton") != nullptr);
    QVERIFY(h.click("runsStartButton"));
    QTRY_VERIFY(popupOpen(h, "runStartDialog"));
    QTRY_COMPARE_WITH_TIMEOUT(runs->games()->rowCount(), 1, 8000);
    QVERIFY(h.click(("runGame_" + kGameId).toUtf8().constData()));
    QTRY_VERIFY(shown(h.item("runRandomizerReason")));
    QCOMPARE(text(h.item("runRandomizerReason")), QStringLiteral("Home offers no randomizer. Admins: Server › Tools."));
    QVERIFY(!h.item("runRandomizerMode")->isEnabled());
    QVERIFY(!shown(h.item("runRandomizerPreset")));
    // A run without the randomizer carries no randomizer object.
    h.item("runNameField")->setProperty("text", QStringLiteral("Plain"));
    QVERIFY(h.click("runStartConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(hub.runs.size() == 1, 8000);
    QVERIFY(!hub.lastRunBody.contains("randomizer"));
  }

  // ------------------------------------------------------------------ Join (10f-1, 10f-2)
  void joinWithAnOwnVariantSendsIt() {
    FakeHub hub(QStringLiteral("a"));
    runsHub(hub);
    const QByteArray mine = R"({"variants":[
      {"id":"var-ok","game_id":"00000000-0000-4000-8000-0000000000a1","name":"Randomizer #1","slot":"var-0a1b2c3d","tool_id":"randomizer","tool_version":"2.3.1","settings":"428nuz","seed":1,"spoiler_log":false,"run_id":null,"created_at":"2026-10-14T10:00:00Z"},
      {"id":"var-old","game_id":"00000000-0000-4000-8000-0000000000a1","name":"Chaos night","slot":"var-1a1b2c3d","tool_id":"randomizer","tool_version":"2.2.0","settings":"428chaos","seed":2,"spoiler_log":false,"run_id":null,"created_at":"2026-10-02T10:00:00Z"}]})";
    const auto base = hub.customRoute;
    hub.customRoute = [base, mine](const FakeRequest& req, int& status, QByteArray& body, QByteArray& ct) {
      if (req.method == "GET" && req.path.endsWith(QLatin1String("/variants"))) {
        body = mine;
        return true;
      }
      return base(req, status, body, ct);
    };
    const QJsonObject run = randomizedRun(hub, true, QString(), QString(), false);
    const QString runId = run.value("run_id").toString();
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    pair(h, hub);
    registerGame(h);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded() && runs->pendingInvites() == 1, 8000);
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item("runInviteJoin") != nullptr);
    QVERIFY(h.click("runInviteJoin"));
    QTRY_VERIFY(popupOpen(h, "runJoinDialog"));
    QTRY_COMPARE_WITH_TIMEOUT(runs->games()->rowCount(), 1, 8000);
    QVERIFY(h.click(("runGame_" + kGameId).toUtf8().constData()));
    // The RANDOMIZER block: the run's world and the pinned engine; own variants are allowed.
    QTRY_VERIFY(shown(h.item("runJoinRandomizer")));
    QCOMPARE(text(h.item("runJoinRandomizerLine")), QStringLiteral("Each person gets their own · Nuzlocke friendly. Spoilers stay hidden."));
    QCOMPARE(text(h.item("runJoinEngine")), QStringLiteral("FVX 2.3.1 · PINNED BY THE RUN"));
    QVERIFY(!shown(h.item("runJoinOwnOff")));
    QVERIFY(shown(h.item("runJoinBringOwn")));
    QVERIFY(h.click("runJoinBringOwn"));
    QTRY_VERIFY(shown(h.item("runJoinOwnVariant")));
    // Two own variants of the game: the older engine is listed but cannot be picked.
    QTRY_COMPARE_WITH_TIMEOUT(h.item("runJoinOwnVariant")->property("count").toInt(), 3, 8000);
    const QVariantList opts = h.item("runJoinOwnVariant")->property("model").toList();
    QCOMPARE(opts.at(2).toMap().value("disabled").toBool(), true);
    QCOMPARE(opts.at(2).toMap().value("hint").toString(), QStringLiteral("Older engine · doesn’t match"));
    QCOMPARE(opts.at(1).toMap().value("disabled").toBool(), false);
    h.item("runJoinOwnVariant")->setProperty("current", QStringLiteral("var-ok"));
    QMetaObject::invokeMethod(h.item("runJoinOwnVariant"), "picked", Q_ARG(QString, QStringLiteral("var-ok")));
    uitest::saveShot(h.window, QStringLiteral("rz_join"));
    QTRY_COMPARE(text(h.item("runJoinSlotNote")), QStringLiteral("You get Run: Ember Shuffle. Main and your variants are not touched."));
    QVERIFY(h.click("runJoinConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(hub.lastRunBody.contains("variant"), 8000);
    const QJsonObject v = hub.lastRunBody.value("variant").toObject();
    QCOMPARE(v.value("kind").toString(), QStringLiteral("own"));
    QCOMPARE(v.value("variant_id").toString(), QStringLiteral("var-ok"));
    QCOMPARE(hub.lastRunBody.value("game_id").toString(), kGameId);
    Q_UNUSED(runId)
  }

  void joinWithTheRunVariantWhenOwnIsOff() {
    FakeHub hub(QStringLiteral("a"));
    runsHub(hub);
    randomizedRun(hub, false, QString(), QString(), false);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    pair(h, hub);
    registerGame(h);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded() && runs->pendingInvites() == 1, 8000);
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item("runInviteJoin") != nullptr);
    QVERIFY(h.click("runInviteJoin"));
    QTRY_VERIFY(popupOpen(h, "runJoinDialog"));
    QTRY_COMPARE_WITH_TIMEOUT(runs->games()->rowCount(), 1, 8000);
    QVERIFY(h.click(("runGame_" + kGameId).toUtf8().constData()));
    QTRY_VERIFY(shown(h.item("runJoinOwnOff")));
    QCOMPARE(text(h.item("runJoinOwnOff")), QStringLiteral("Lena’s run builds everyone’s variant. Own variants are off."));
    QVERIFY(!shown(h.item("runJoinBringOwn")));
    QVERIFY(h.click("runJoinConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(hub.lastRunBody.contains("variant"), 8000);
    QCOMPARE(hub.lastRunBody.value("variant").toObject().value("kind").toString(), QStringLiteral("run"));
    QVERIFY(!hub.lastRunBody.value("variant").toObject().contains("variant_id"));
  }

  // ------------------------------------------------------------------ Run board (10f-4)
  void boardShowsOnlyTheKindOfVariant() {
    FakeHub hub(QStringLiteral("a"));
    runsHub(hub);
    const QJsonObject run = randomizedRun(hub, true, QStringLiteral("run"), QStringLiteral("ready"), true);
    const QString runId = run.value("run_id").toString();
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    pair(h, hub);
    registerGame(h);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded() && runs->active()->rowCount() == 1, 8000);
    // The list tags the run.
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item(("runRowRandomized_" + runId).toUtf8().constData()) != nullptr);
    QCOMPARE(text(h.item(("runRowRandomized_" + runId).toUtf8().constData())), QStringLiteral("Randomized"));
    runs->openRun(runId);
    QTRY_VERIFY(shown(h.item("runBoardRandomizer")));
    // Game column: only "Run variant" / "Own variant"; STATUS: Building while a variant builds.
    const auto* pm = qobject_cast<KeyedRowsModel*>(runs->participants());
    QVERIFY(pm != nullptr);
    QTRY_COMPARE(pm->rowCount(), 2);
    QMap<QString, QVariantMap> byName;
    for (int i = 0; i < pm->rowCount(); ++i) byName.insert(pm->get(i).value("name").toString(), pm->get(i));
    QCOMPARE(byName.value("Tester").value("gameTitle").toString(), QStringLiteral("Run variant"));
    QCOMPARE(byName.value("Tester").value("statusText").toString(), QStringLiteral("Joined"));
    QCOMPARE(byName.value("Lena").value("gameTitle").toString(), QStringLiteral("Own variant"));
    QCOMPARE(byName.value("Lena").value("statusText").toString(), QStringLiteral("Building"));
    // The Randomizer block of the side: world, preset, pinned engine, own variants; never a seed or settings.
    QCOMPARE(text(h.item("runRzWorld")), QStringLiteral("Each person gets their own"));
    QCOMPARE(text(h.item("runRzEngine")), QStringLiteral("2.3.1 · pinned"));
    QCOMPARE(text(h.item("runRzOwn")), QStringLiteral("Allowed, same engine"));
    QCOMPARE(text(h.item("runRzNote")), QStringLiteral("Nobody sees another person’s world, seed or settings, admins neither."));
    QVERIFY(!shown(h.item("runRzSpoiler")));   // only after the run ended
    uitest::saveShot(h.window, QStringLiteral("rz_board"));
  }

  // ------------------------------------------------------------------ Play run builds the variant first (10f-3)
  void playRunNeedsTheVariantAndShowsAFailure() {
    FakeHub hub(QStringLiteral("a"));
    runsHub(hub);
    const auto base = hub.customRoute;
    hub.customRoute = [base](const FakeRequest& req, int& status, QByteArray& body, QByteArray& ct) {
      if (req.method == "GET" && req.path.endsWith(QLatin1String("/variant"))) {   // the recipe of the run is not available
        status = 404;
        body = R"({"error":{"code":"not_found","message":"no recipe"}})";
        return true;
      }
      return base(req, status, body, ct);
    };
    const QJsonObject run = randomizedRun(hub, true, QStringLiteral("run"), QStringLiteral("not_built"), true);
    const QString runId = run.value("run_id").toString();
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    pair(h, hub);
    registerGame(h);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded() && runs->active()->rowCount() == 1, 8000);
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item("runsStartButton") != nullptr);
    runs->openRun(runId);
    QTRY_VERIFY(shown(h.item("runPlay")));
    QVERIFY(h.controller->runBuild().isEmpty());
    QVERIFY(h.click("runPlay"));
    // No game starts: the variant of the run has to be built, which failed here; the dialog says so and closes with Close.
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->runBuild().value("state").toString(), QStringLiteral("failed"), 8000);
    QTRY_VERIFY(popupOpen(h, "runBuildDialog"));
    QVERIFY(h.item("runBuildFailed")->isVisible());
    QVERIFY(!shown(h.item("runBuildStart")));
    uitest::saveShot(h.window, QStringLiteral("rz_buildfailed"));
    QVERIFY(h.click("runBuildClose"));
    QTRY_VERIFY(h.controller->runBuild().isEmpty());
    QTRY_VERIFY(!popupOpen(h, "runBuildDialog"));
  }

  // A run whose game cannot start (its core is neither installed nor offered): Play says why, every time, and leaves nothing stuck.
  void playRunShowsWhyTheGameCannotStart() {
    FakeHub hub(QStringLiteral("a"));
    runsHub(hub);
    const QString runId = randomizedRun(hub, true, QStringLiteral("run"), QStringLiteral("ready"), true).value("run_id").toString();
    QJsonObject plain = hub.runs.value(runId);
    plain.remove("randomizer");
    hub.runs.insert(runId, plain);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1280, 800);
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded() && runs->active()->rowCount() == 1, 8000);
    const QString slotBefore = h.controller->saveSync()->slotFor(kGameId);
    QVERIFY(h.click("navRuns"));
    QTRY_VERIFY(h.item("runsStartButton") != nullptr);
    runs->openRun(runId);
    QTRY_VERIFY(shown(h.item("runPlay")));
    for (int attempt = 0; attempt < 2; ++attempt) {
      QVERIFY(h.click("runPlay"));
      QTRY_VERIFY_WITH_TIMEOUT(!runs->message().isEmpty(), 8000);
      QVERIFY2(runs->message().contains(QStringLiteral("does not offer it")), qPrintable(runs->message()));
      QVERIFY(runs->messageIsError());
      QVERIFY(shown(h.item("runsMessage")));
      QCOMPARE(h.controller->selectedGame().value("playDisabledReason").toString(), runs->message());
      QVERIFY(h.controller->runBuild().isEmpty());
      QVERIFY(!runs->busy());
      QCOMPARE(h.controller->saveSync()->slotFor(kGameId), slotBefore);  // the run's slot is not left selected
      runs->dismissMessage();
      QVERIFY(runs->message().isEmpty());
      QTRY_VERIFY(h.item("runPlay")->isEnabled());
    }
  }
};

QTEST_MAIN(RandomizerUiTest)
#include "randomizer_ui_test.moc"
