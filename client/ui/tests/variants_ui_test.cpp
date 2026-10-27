// The Randomizer screens (0.21, design sheet 5): Randomize… on the game page (hidden without a plugin, off for a game the plugin does not
// list, on otherwise), the Randomize dialog with the engine step, the build with the test engine, the Variants section, the toast, the
// Library status, Delete (Keep saves) and the Storage section. Offscreen, FakeHub with the tools catalog and variants routes, the test
// engine; synthetic data only, no core.
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtTest>

#include "corecache.h"
#include "engine_pack.h"
#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

class VariantsUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    h.window->resize(1280, 800);
  }
  static bool shown(const Harness& h, const char* name) {
    for (QQuickItem* it : h.items(name))
      if (it->isVisible() && it->window() == h.window) return true;
    return false;
  }
  static QString textOf(const Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr ? it->property("text").toString() : QString();
  }
  static void registerPlugin(Harness& h, const QStringList& codes) {
    plugins::VariantRegistration r;
    r.pluginId = QStringLiteral("uzume.randomizer");
    r.engine = QStringLiteral("randomizer");
    r.presets = {plugins::VariantPreset{QStringLiteral("nuzlocke"), QStringLiteral("Nuzlocke friendly"), QStringLiteral("Fair start, game stays finishable"), {{QStringLiteral("428"), QStringLiteral("428nuz")}}},
                 plugins::VariantPreset{QStringLiteral("chaos"), QStringLiteral("Chaos"), QStringLiteral("Everything that can change"), {{QStringLiteral("428"), QStringLiteral("428chaos")}}}};
    r.games = {plugins::VariantGameSupport{QStringLiteral("nds"), codes}};
    h.controller->variants()->registry()->set(r);
  }

  // The fake server: tools catalog with the test engine, a stateful variants list.
  struct Server {
    QByteArray zip;
    QJsonArray variants;
    QStringList deleted;
    int next = 1;
  };
  static void routes(FakeHub& hub, Server& s) {
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("tools_v1"), QStringLiteral("variants_v1")};
    hub.customRoute = [&s](const FakeRequest& req, int& status, QByteArray& body, QByteArray& ct) {
      const QString plat = CoreCache::currentPlatform();
      const QString base = QStringLiteral("/api/v1");
      if (req.method == "GET" && req.path == base + QStringLiteral("/tools")) {
        QJsonObject p{{QStringLiteral("platform"), plat}, {QStringLiteral("size"), s.zip.size()}, {QStringLiteral("sha256"), testutil::sha256Hex(s.zip)}, {QStringLiteral("cached"), true},
                      {QStringLiteral("root"), QStringLiteral("UPR_FVX-v1_6_2-Test/")}};
        QJsonObject v{{QStringLiteral("version"), QStringLiteral("1.6.2")}, {QStringLiteral("platforms"), QJsonArray{p}}};
        QJsonObject t{{QStringLiteral("id"), QStringLiteral("randomizer")}, {QStringLiteral("name"), QStringLiteral("Randomizer engine")}, {QStringLiteral("offered"), true},
                      {QStringLiteral("version"), QStringLiteral("1.6.2")}, {QStringLiteral("license"), QStringLiteral("GPL-3.0")}, {QStringLiteral("versions"), QJsonArray{v}}};
        body = QJsonDocument(QJsonObject{{QStringLiteral("tools"), QJsonArray{t}}}).toJson(QJsonDocument::Compact);
        return true;
      }
      if (req.method == "GET" && req.path == base + QStringLiteral("/tools/randomizer/packages/1.6.2/") + plat) {
        body = s.zip;
        ct = "application/zip";
        return true;
      }
      if (req.method == "GET" && (req.path == base + QStringLiteral("/variants") || req.path == base + QStringLiteral("/games/g1/variants"))) {
        body = QJsonDocument(QJsonObject{{QStringLiteral("variants"), s.variants}}).toJson(QJsonDocument::Compact);
        return true;
      }
      if (req.method == "POST" && req.path == base + QStringLiteral("/games/g1/variants")) {
        const QJsonObject in = QJsonDocument::fromJson(req.body).object();
        const int n = s.next++;
        QJsonObject v{{QStringLiteral("id"), QStringLiteral("v%1").arg(n)}, {QStringLiteral("game_id"), QStringLiteral("g1")}, {QStringLiteral("name"), in.value(QStringLiteral("name"))},
                      {QStringLiteral("slot"), QStringLiteral("var-0a1b2c3%1").arg(n)}, {QStringLiteral("tool_id"), QStringLiteral("randomizer")},
                      {QStringLiteral("tool_version"), QStringLiteral("1.6.2")}, {QStringLiteral("settings"), in.value(QStringLiteral("settings"))},
                      {QStringLiteral("seed"), 4242}, {QStringLiteral("spoiler_log"), in.value(QStringLiteral("spoiler_log")).toBool()},
                      {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
        s.variants.append(v);
        status = 201;
        body = QJsonDocument(v).toJson(QJsonDocument::Compact);
        return true;
      }
      if (req.method == "DELETE" && req.path.startsWith(base + QStringLiteral("/variants/"))) {
        s.deleted << req.path;
        const QString id = req.path.mid(base.size() + 10).section(QLatin1Char('?'), 0, 0);
        QJsonArray rest;
        for (const QJsonValue& v : std::as_const(s.variants))
          if (v.toObject().value(QStringLiteral("id")).toString() != id) rest.append(v);
        s.variants = rest;
        status = 204;
        body = {};
        return true;
      }
      return false;
    };
  }
  static void seed(FakeHub& hub, const QByteArray& rom) {
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    hub.games = QJsonObject{{QStringLiteral("games"), QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"), uitest::sha256Hex(rom), rom.size())}}};
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // Randomize… hidden without a plugin, off with the reason as tooltip for a game the plugin does not list, on otherwise (10a-1, 10a-3a).
  void randomizeButtonStates() {
    QTemporaryDir tmp;
    Server s;
    testutil::writeEngineZip(tmp.path(), QStringLiteral(FB_TEST_ENGINE_PATH), QStringLiteral("UPR_FVX-v1_6_2-Test"), false, &s.zip);
    FakeHub hub(QStringLiteral("a"));
    seed(hub, QByteArray(40000, 'r'));
    routes(hub, s);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QVERIFY(h.openGame(QStringLiteral("g1")));
    QTRY_VERIFY(shown(h, "playButton"));
    QVERIFY(!shown(h, "randomizeButton"));   // no plugin: no button
    QVERIFY(!shown(h, "variantsSection"));   // and no section

    registerPlugin(h, {QStringLiteral("ZZZZ")});   // the plugin lists other games only
    QTRY_VERIFY(shown(h, "randomizeButton"));
    QVERIFY(!h.item("randomizeButton")->isEnabled());
    QCOMPARE(h.item("randomizeWrap")->property("reason").toString(), QStringLiteral("Lumen Drift can’t be randomized."));

    registerPlugin(h, {});                         // the whole system
    QTRY_VERIFY(h.item("randomizeButton")->isEnabled());
    QTRY_VERIFY(shown(h, "variantsEmpty"));        // 10a-3d
    uitest::saveShot(h.window, QStringLiteral("variants-empty"));
  }

  // The full path: dialog, engine step, build, row, toast, Library status, Storage, Delete with Keep saves.
  void buildThroughTheDialogThenDelete() {
    QTemporaryDir tmp;
    Server s;
    testutil::writeEngineZip(tmp.path(), QStringLiteral(FB_TEST_ENGINE_PATH), QStringLiteral("UPR_FVX-v1_6_2-Test"), false, &s.zip);
    FakeHub hub(QStringLiteral("a"));
    seed(hub, QByteArray(40000, 'r'));
    routes(hub, s);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    registerPlugin(h, {});
    QVERIFY(h.openGame(QStringLiteral("g1")));
    QTRY_VERIFY(h.item("randomizeButton") != nullptr && h.item("randomizeButton")->isVisible() && h.item("randomizeButton")->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->variants()->engine().value(QStringLiteral("state")).toString(), QStringLiteral("not_on_pc"), 8000);

    QVERIFY(h.click("randomizeButton"));
    QTRY_VERIFY(shown(h, "randomizeDialog"));
    QCOMPARE(textOf(h, "randomizeName"), QStringLiteral("Randomizer #1"));
    QCOMPARE(textOf(h, "randomizeSlotLine"), QStringLiteral("Plays in its own slot: Randomizer #1. Main is not touched."));
    QVERIFY(textOf(h, "randomizeEngineLine").startsWith(QStringLiteral("FVX 1.6.2")));
    // Custom: a string from a newer settings version stays invalid, a known one is valid.
    auto* vc = h.controller->variants();
    QCOMPARE(vc->checkSettings(QStringLiteral("g1"), QStringLiteral("428abc")).value(QStringLiteral("state")).toString(), QStringLiteral("ok"));
    QCOMPARE(vc->checkSettings(QStringLiteral("g1"), QStringLiteral("500abc")).value(QStringLiteral("state")).toString(), QStringLiteral("other_version"));
    QCOMPARE(vc->checkSettings(QStringLiteral("g1"), QStringLiteral("no string")).value(QStringLiteral("state")).toString(), QStringLiteral("invalid"));
    // The engine is not on this PC: Build asks first (10d-1a), then downloads and builds.
    uitest::saveShot(h.window, QStringLiteral("variants-randomize-dialog"));
    {
      // Custom shows the settings string with its check: Build stays off until the string is valid (10b-2, 10b-3b).
      QQuickItem* dlg = h.item("randomizeDialog");
      dlg->setProperty("presetId", QStringLiteral("custom"));
      QTRY_VERIFY(shown(h, "randomizeCustom"));
      QVERIFY(!h.item("randomizeBuild")->isEnabled());                       // nothing pasted yet
      h.item("randomizeSettings")->setProperty("text", QStringLiteral("500abc"));
      QTRY_COMPARE(textOf(h, "randomizeSettingsState"), QStringLiteral("This string is from a newer engine than Test server has."));
      QVERIFY(!h.item("randomizeBuild")->isEnabled());
      h.item("randomizeSettings")->setProperty("text", QStringLiteral("428abc"));
      QTRY_COMPARE(textOf(h, "randomizeSettingsState"), QStringLiteral("Valid · settings version 428"));
      QTRY_VERIFY(h.item("randomizeBuild")->isEnabled());
      h.controller->setAppearance(QStringLiteral("light"));
      uitest::saveShot(h.window, QStringLiteral("variants-randomize-custom-light"));
      h.controller->setAppearance(QStringLiteral("dark"));
      dlg->setProperty("presetId", QStringLiteral("nuzlocke"));
      QTRY_VERIFY(!shown(h, "randomizeCustom"));
    }
    QVERIFY(h.click("randomizeBuild"));
    QTRY_VERIFY(shown(h, "variantEngineDialog"));
    uitest::saveShot(h.window, QStringLiteral("variants-engine-dialog"));
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "2500");   // the build dialog stays up long enough to look at
    QVERIFY(h.click("variantEngineStart"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "variantBuildDialog"), 30000);
    QCOMPARE(textOf(h, "variantBuildSlot"), QStringLiteral("Plays in its own slot: Randomizer #1. Main is not touched."));
    QTRY_VERIFY(h.item("variantBuildBackground")->hasActiveFocus());
    uitest::saveShot(h.window, QStringLiteral("variants-build-dialog"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "variantRow_0"), 30000);
    QTRY_COMPARE_WITH_TIMEOUT(textOf(h, "variantStatus_0"), QStringLiteral("Ready"), 60000);
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "");
    QTRY_VERIFY(!shown(h, "randomizeDialog"));
    QTRY_VERIFY(!shown(h, "variantEngineDialog"));
    QTRY_VERIFY(!shown(h, "variantBuildDialog"));
    QCOMPARE(textOf(h, "variantName_0"), QStringLiteral("Randomizer #1"));
    QVERIFY(shown(h, "variantsNote"));
    QCOMPARE(textOf(h, "variantsNote"), QStringLiteral("Each variant plays in its own slot. Main is not touched."));
    QVERIFY(shown(h, "variantToast"));
    QCOMPARE(textOf(h, "variantToast"), QStringLiteral("Randomizer #1 is ready"));
    QVERIFY(shown(h, "variantPlay_0"));
    // No text sticks out of the section (Windows renders wider; the rows elide).
    {
      QQuickItem* sec = h.item("variantsSection");
      const QRectF secRect = sec->mapRectToScene(QRectF(0, 0, sec->width(), sec->height()));
      for (const char* n : {"variantName_0", "variantStatus_0", "variantPlay_0", "variantMenuButton_0", "variantsNote"}) {
        QQuickItem* it = h.item(n);
        QVERIFY2(it != nullptr, n);
        const QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
        QVERIFY2(secRect.contains(r.adjusted(1, 1, -1, -1)), n);
      }
    }
    uitest::saveShot(h.window, QStringLiteral("variants-ready"));

    // The menu: Open spoiler log is only there when the variant was built with a log (not here).
    QVERIFY(h.click("variantMenuButton_0"));
    QTRY_VERIFY(shown(h, "menuItem_variantRename"));
    QVERIFY(shown(h, "menuItem_variantDelete"));
    QVERIFY(!shown(h, "menuItem_variantSpoiler"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "menuItem_variantRename"));

    // Library status: the game shows its variant count.
    QVERIFY(h.click("gameBack"));
    QTRY_VERIFY(shown(h, "gameTile_g1"));
    {
      // (The tile shows "Core missing" first on a PC without a core; the status of a ready game is the variant count.)
      LibraryModel* model = h.controller->library();
      const int role = model->roleNames().key("statusText");
      QCOMPARE(model->rowCount(), 1);
      QCOMPARE(model->data(model->index(0), role).toString(), QStringLiteral("1 variant"));
    }

    // Storage: the variants section lists it.
    h.controller->showSettingsSection(QStringLiteral("storage"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "storageVariantRow_0"), 8000);
    QCOMPARE(textOf(h, "storageVariantName_0"), QStringLiteral("Randomizer #1"));
    QVERIFY(shown(h, "storageVariantsFree"));
    QVERIFY(shown(h, "storageVariantsAgeSelect"));
    QVERIFY(!textOf(h, "storageVariantSize_0").isEmpty());
    QTRY_VERIFY(h.item("localStorageKind_variants") != nullptr);
    uitest::saveShot(h.window, QStringLiteral("variants-storage"));

    // Delete keeping the saves: the dialog opens on Cancel, Keep saves is the default, the server hears saves=keep.
    h.controller->showLibrary();
    QVERIFY(h.openGame(QStringLiteral("g1")));
    QTRY_VERIFY(shown(h, "variantRow_0"));
    QVERIFY(h.click("variantMenuButton_0"));
    QTRY_VERIFY(shown(h, "menuItem_variantDelete"));
    QVERIFY(h.click("menuItem_variantDelete"));
    QTRY_VERIFY(shown(h, "variantDeleteDialog"));
    QCOMPARE(h.item("variantDeleteMode")->property("current").toString(), QStringLiteral("keep"));
    QVERIFY(h.item("variantDeleteConfirm")->property("destructive").toBool());
    QVERIFY(h.click("variantDeleteConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(!s.deleted.isEmpty(), 5000);
    QVERIFY2(s.deleted.first().contains(QStringLiteral("saves=keep")), qPrintable(s.deleted.first()));
    QTRY_VERIFY(!shown(h, "variantRow_0"));
  }

  // Cancel asks first (10c-3b): Keep building carries on, Stop building stops and keeps nothing.
  void cancelAsksFirst() {
    QTemporaryDir tmp;
    Server s;
    testutil::writeEngineZip(tmp.path(), QStringLiteral(FB_TEST_ENGINE_PATH), QStringLiteral("UPR_FVX-v1_6_2-Test"), false, &s.zip);
    FakeHub hub(QStringLiteral("a"));
    seed(hub, QByteArray(40000, 'r'));
    routes(hub, s);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    registerPlugin(h, {});
    auto* vc = h.controller->variants();
    QTRY_COMPARE_WITH_TIMEOUT(vc->engine().value(QStringLiteral("state")).toString(), QStringLiteral("not_on_pc"), 8000);
    vc->prepareEngine();
    QTRY_COMPARE_WITH_TIMEOUT(vc->engine().value(QStringLiteral("state")).toString(), QStringLiteral("ready"), 20000);
    QVERIFY(h.openGame(QStringLiteral("g1")));
    QTRY_VERIFY(h.item("randomizeButton") != nullptr && h.item("randomizeButton")->isEnabled());
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "20000");
    QVERIFY(h.click("randomizeButton"));
    QTRY_VERIFY(shown(h, "randomizeDialog"));
    QVERIFY(h.click("randomizeBuild"));   // the engine is on this PC: no question, the build starts
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "variantBuildDialog"), 20000);
    QVERIFY(vc->building());
    QVERIFY(h.click("variantBuildCancel"));
    QTRY_VERIFY(shown(h, "variantBuildCancelConfirm"));
    QTRY_VERIFY(h.item("variantBuildKeep")->hasActiveFocus());   // the safe button
    QVERIFY(h.item("variantBuildStop")->property("destructive").toBool());
    QVERIFY(h.click("variantBuildKeep"));
    QTRY_VERIFY(!shown(h, "variantBuildCancelConfirm"));
    QVERIFY(vc->building());
    QVERIFY(h.click("variantBuildBackground"));          // in the background: the row shows the build
    QTRY_VERIFY(!shown(h, "variantBuildDialog"));
    QTRY_VERIFY(shown(h, "variantStatus_0"));
    QVERIFY(textOf(h, "variantStatus_0").startsWith(QStringLiteral("Building")));
    QVERIFY(vc->building());
    QVERIFY(h.click("variantDetails_0"));                // Details opens the dialog again
    QTRY_VERIFY(shown(h, "variantBuildDialog"));
    QVERIFY(h.click("variantBuildCancel"));
    QTRY_VERIFY(shown(h, "variantBuildCancelConfirm"));
    QVERIFY(h.click("variantBuildStop"));
    QTRY_VERIFY_WITH_TIMEOUT(!vc->building(), 20000);
    QTRY_VERIFY(!shown(h, "variantBuildDialog"));
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "");
    QVERIFY(!shown(h, "variantToast"));                   // nothing is ready
  }

  // Variants the server lists but this PC has not built: Needs rebuild with a Rebuild button; the section stays inside the page at the
  // smallest window; with the plugin off the "Randomizer is off" line replaces the table (10a-3c).
  void listedVariantsNeedRebuildAndPluginOff() {
    QTemporaryDir tmp;
    Server s;
    testutil::writeEngineZip(tmp.path(), QStringLiteral(FB_TEST_ENGINE_PATH), QStringLiteral("UPR_FVX-v1_6_2-Test"), false, &s.zip);
    for (int i = 1; i <= 2; ++i)
      s.variants.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("old%1").arg(i)}, {QStringLiteral("game_id"), QStringLiteral("g1")},
                                    {QStringLiteral("name"), i == 1 ? QStringLiteral("Chaos night") : QStringLiteral("A very long variant name that has to be cut by the row so nothing sticks out")},
                                    {QStringLiteral("slot"), QStringLiteral("var-0a1b2c4%1").arg(i)}, {QStringLiteral("tool_id"), QStringLiteral("randomizer")},
                                    {QStringLiteral("tool_version"), QStringLiteral("1.6.2")}, {QStringLiteral("settings"), QStringLiteral("428chaos")},
                                    {QStringLiteral("seed"), 7}, {QStringLiteral("created_at"), QStringLiteral("2026-10-02T10:00:00Z")}});
    FakeHub hub(QStringLiteral("a"));
    seed(hub, QByteArray(40000, 'r'));
    routes(hub, s);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    registerPlugin(h, {});
    h.window->resize(960, 600);
    QVERIFY(h.openGame(QStringLiteral("g1")));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "variantRow_1"), 8000);
    QCOMPARE(textOf(h, "variantStatus_0"), QStringLiteral("Needs rebuild"));
    QVERIFY(shown(h, "variantRebuild_0"));
    QVERIFY(!shown(h, "variantPlay_0"));
    QQuickItem* page = h.item("gamePage");
    const QRectF pageRect = page->mapRectToScene(QRectF(0, 0, page->width(), page->height()));
    for (const char* n : {"variantName_0", "variantName_1", "variantStatus_1", "variantRebuild_1", "variantMenuButton_1"}) {
      QQuickItem* it = h.item(n);
      QVERIFY2(it != nullptr, n);
      const QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
      QVERIFY2(r.left() >= pageRect.left() - 1 && r.right() <= pageRect.right() + 1, n);   // (the page scrolls vertically)
    }
    h.controller->setAppearance(QStringLiteral("light"));
    if (QQuickItem* flick = h.item("gameOverview")) flick->setProperty("contentY", flick->property("contentHeight").toReal());   // the section is below the fold
    QTest::qWait(200);
    uitest::saveShot(h.window, QStringLiteral("variants-needs-rebuild"));
    h.controller->setAppearance(QStringLiteral("dark"));

    // Rename keeps the dialog's rule: another variant's name is taken.
    QVERIFY(h.click("variantMenuButton_0"));
    QTRY_VERIFY(shown(h, "menuItem_variantRename"));
    QVERIFY(h.click("menuItem_variantRename"));
    QTRY_VERIFY(shown(h, "variantRenameDialog"));
    QCOMPARE(textOf(h, "variantRenameField"), QStringLiteral("Chaos night"));
    QVERIFY(h.item("variantRenameSave")->isEnabled());
    QVERIFY(h.click("variantRenameCancel"));
    QTRY_VERIFY(!shown(h, "variantRenameDialog"));

    // The plugin turns off: the line replaces the table, the variants stay.
    h.controller->variants()->registry()->remove(QStringLiteral("uzume.randomizer"));
    QTRY_VERIFY(shown(h, "variantsPluginOff"));
    QVERIFY(!shown(h, "variantRow_0"));
    QVERIFY(!shown(h, "randomizeButton"));
    QVERIFY(shown(h, "variantsOpenPlugins"));
  }
};

UITEST_MAIN(VariantsUiTest)
#include "variants_ui_test.moc"
