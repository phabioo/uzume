// Plugins in the Player UI: Settings > Plugins (Developer mode, list, install, uninstall, empty state) and the Plugin
// console tab of the game panel (preview game, no core). With UZUME_SCREENSHOT_DIR=<dir> the views are also saved as
// PNGs (dark and light, 1440x900) for the review; they are never checked in. Homebrew plugin folders only.
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <functional>

#include "fakehub.h"
#include "uzplugin_testutil.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using namespace uzume::plugins::testutil;

namespace {

void pair(Harness& h, FakeHub& hub) {
  h.controller->addHub(hub.address());
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation, 8000);
  h.controller->confirmTrust();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsPairing, 8000);
  h.controller->requestPairing();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
}

QString writePlugin(const QString& parent, const QString& id, const QString& name, const QJsonObject& requires_ = {}) {
  const QString dir = QDir(parent).filePath(id);
  QDir().mkpath(dir);
  QJsonObject m{{"id", id}, {"name", name}, {"version", "1.2.0"}, {"api", 1}, {"type", "script"}, {"entry", "main.lua"},
                {"permissions", QJsonArray{"memory.read"}}};
  if (!requires_.isEmpty()) m["requires"] = requires_;
  QFile mf(dir + "/manifest.json");
  if (!mf.open(QIODevice::WriteOnly)) return {};
  mf.write(QJsonDocument(m).toJson());
  QFile lf(dir + "/main.lua");
  if (!lf.open(QIODevice::WriteOnly)) return {};
  lf.write("fb.log.info('hello')\n");
  return dir;
}

QVariantMap consoleData(bool error, bool withRegions) {
  QVariantList plugins{QVariantMap{{"id", "creature-core"}, {"name", "Creature Core"}, {"status", error ? "error" : "running"}, {"statusText", error ? "Paused after an error" : "Running"}},
                       QVariantMap{{"id", "item-helper"}, {"name", "Item Helper"}, {"status", "running"}, {"statusText", "Running"}}};
  QVariantList watched{QVariantMap{{"address", "0x08001A10"}, {"value", "6"}, {"label", "party count"}},
                       QVariantMap{{"address", "0x08002B44"}, {"value", "0x00000142"}, {"label", "rng seed"}},
                       QVariantMap{{"address", "0x14000104"}, {"value", "-"}, {"label", "slot 1 (not in snapshot)"}}};
  QVariantList events{QVariantMap{{"time", "18:42:07.311"}, {"plugin", "creature-core"}, {"kind", "party.changed"}, {"text", "slot 1 species 1"}},
                      QVariantMap{{"time", "18:42:05.002"}, {"plugin", "creature-core"}, {"kind", "game.ready"}, {"text", "{code=\"00040000\"}"}}};
  QVariantList log{QVariantMap{{"time", "18:42:09.870"}, {"level", "error"}, {"text", "main.lua:41: attempt to index a nil value (field 'party')"}},
                   QVariantMap{{"time", "18:42:08.120"}, {"level", "warn"}, {"text", "party pointer is 0, retrying"}},
                   QVariantMap{{"time", "18:42:07.311"}, {"level", "info"}, {"text", "party changed: 6 members"}},
                   QVariantMap{{"time", "18:42:05.001"}, {"level", "debug"}, {"text", "watch party_ptr 0x08000010 4"}}};
  QVariantList regions;
  if (withRegions)
    regions = {QVariantMap{{"start", "0x08000000"}, {"length", "0x100000"}, {"addrspace", "heap"}},
               QVariantMap{{"start", "0x14000000"}, {"length", "0x100000"}, {"addrspace", "linear"}}};
  return QVariantMap{{"plugins", plugins},
                     {"selected", "creature-core"},
                     {"status", error ? "error" : "running"},
                     {"statusText", error ? "Paused after an error" : "Running"},
                     {"error", error ? "main.lua:41: attempt to index a nil value (field 'party')\nstack traceback:\n\tmain.lua:41: in function 'on_tick'" : ""},
                     {"watched", watched},
                     {"events", events},
                     {"log", log},
                     {"regions", regions},
                     {"regionsText", withRegions ? "" : "The core exposes no memory map"}};
}

}  // namespace

class PluginsUiTest : public QObject {
  Q_OBJECT
  QString shotDir_;
  QTemporaryDir src_;

  void shot(Harness& h, const QString& name) {
    if (shotDir_.isEmpty()) return;
    QQuickTest::qWaitForPolish(h.window);
    QTest::qWait(350);
    QDir().mkpath(shotDir_);
    const QImage img = h.window->grabWindow();
    QVERIFY2(!img.isNull() && !uitest::isAllBlack(img), qPrintable(name));
    QVERIFY(img.save(QDir(shotDir_).filePath(name + QStringLiteral(".png"))));
  }
  static void gotoPlugins(Harness& h) {
    h.window->resize(1440, 900);
    h.controller->showSettings();
    QTest::qWait(150);
    QVERIFY(h.click("section_plugins"));
    QTest::qWait(300);
    QVERIFY(h.click("pluginDeveloperSettings"));  // Developer mode and the folder installs sit behind this link
    QTest::qWait(100);
  }
  static void startPreviewGame(Harness& h) {
    GameSession* gs = h.controller->gameSession();
    QImage frame(256, 384, QImage::Format_RGB32);
    frame.fill(QColor(40, 90, 140));
    gs->setPreview(QStringLiteral("Uzume Test"), frame, emu::DisplayProfile(), EmulationDiagnostics());
    emit gs->started();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 8000);
    h.controller->sessions()->gameStarted(QStringLiteral("t1"), QStringLiteral("Uzume Test"));
    h.controller->sessions()->setTab(QStringLiteral("session"));
    QQuickTest::qWaitForPolish(h.window);
  }

 private slots:
  void initTestCase() {
    shotDir_ = qEnvironmentVariable("UZUME_SCREENSHOT_DIR");
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void settingsSection() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-pl");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    PluginsController* pc = h.controller->plugins();
    QVERIFY(pc != nullptr);
    QVERIFY(!pc->developerMode());
    QVERIFY(!pc->consoleAvailable());
    QCOMPARE(pc->manager()->rootDir(), QDir::cleanPath(h.controller->profileStore()->baseDir() + "/plugins"));

    // Empty state (dark and light).
    gotoPlugins(h);
    QVERIFY(h.item("pluginsSection") != nullptr && h.item("pluginsSection")->isVisible());
    QCOMPARE(h.item("sectionStatus_plugins")->property("text").toString(), QStringLiteral("None installed"));
    QVERIFY(h.item("pluginsEmpty")->isVisible());
    QVERIFY(h.item("pluginInstallButton")->isVisible());
    QVERIFY(!h.item("consoleLoadUnpacked")->isVisible());  // Developer mode only
    QVERIFY(h.item("pluginDeveloperModeHint")->property("text").toString().startsWith(
        QStringLiteral("Allows unsigned local plugins, adds the Plugin console tab in game")));
    shot(h, QStringLiteral("4a-plugins-empty"));
    h.controller->setAppearance(QStringLiteral("light"));
    shot(h, QStringLiteral("4a-plugins-empty-light"));
    h.controller->setAppearance(QStringLiteral("dark"));

    // Developer mode persists and shows the link button.
    QVERIFY(h.click("pluginDeveloperModeToggle"));
    QTRY_VERIFY(pc->developerMode());
    QVERIFY(h.controller->playerSettings()->developerMode());
    QVERIFY(pc->manager()->developerMode());
    QVERIFY(PlayerSettings(h.controller->profileStore()->baseDir()).developerMode());
    QTRY_VERIFY(h.item("consoleLoadUnpacked")->isVisible());

    // Plugin language: Auto by default, explicit choice persists and wins over the 3DS option.
    QVERIFY(h.item("pluginLanguageSelect") != nullptr);
    QCOMPARE(pc->pluginLanguage(), QStringLiteral("auto"));
    QCOMPARE(pc->pluginLanguageOptions().size(), 3);
    QCOMPARE(pc->resolveLanguage(QStringLiteral("3ds"), {{QStringLiteral("citra_language_value"), QStringLiteral("German")}}), QStringLiteral("de"));
    pc->setPluginLanguage(QStringLiteral("en"));
    QCOMPARE(PlayerSettings(h.controller->profileStore()->baseDir()).pluginLanguage(), QStringLiteral("en"));
    QCOMPARE(pc->resolveLanguage(QStringLiteral("3ds"), {{QStringLiteral("citra_language_value"), QStringLiteral("German")}}), QStringLiteral("en"));
    pc->setPluginLanguage(QStringLiteral("auto"));

    // Install two plugins from folders (copy); one has an unmet dependency.
    QVERIFY(!writePlugin(src_.path(), "net.example.creature-core", "Creature Core").isEmpty());
    QVERIFY(!writePlugin(src_.path(), "net.example.item-helper", "Item Helper", QJsonObject{{"net.example.missing-core", "^2.0"}}).isEmpty());
    pc->installFromFolder(QUrl::fromLocalFile(src_.filePath("net.example.creature-core")).toString());
    QVERIFY2(pc->messageIsError() == false, qPrintable(pc->message()));
    pc->installFromFolder(src_.filePath("net.example.item-helper"));
    QVERIFY(QFileInfo(h.controller->profileStore()->baseDir() + "/plugins/installed/net.example.creature-core/manifest.json").exists());
    QTRY_VERIFY(h.item("pluginRow_net.example.creature-core") != nullptr);
    QVERIFY(h.item("pluginRow_net.example.item-helper") != nullptr);
    QCOMPARE(h.item("pluginsInstalledTitle")->property("text").toString(), QStringLiteral("Installed · 2"));
    QVERIFY(!h.item("pluginsEmpty")->isVisible());
    QCOMPARE(pc->plugins().size(), 2);
    const QVariantMap healthy = pc->plugins().value(0).toMap();
    const QVariantMap blocked = pc->plugins().value(1).toMap();
    QCOMPARE(healthy.value("id").toString(), QStringLiteral("net.example.creature-core"));
    QCOMPARE(healthy.value("originText").toString(), QStringLiteral("Local file"));
    QVERIFY(!healthy.value("enabled").toBool());
    QVERIFY(healthy.value("canToggle").toBool());
    QVERIFY(!blocked.value("canToggle").toBool());
    QVERIFY(!blocked.value("problemsText").toString().isEmpty());

    // Toggle on; the blocked plugin cannot be switched on.
    pc->setPluginEnabled(QStringLiteral("net.example.creature-core"), true);
    QVERIFY(pc->plugins().value(0).toMap().value("enabled").toBool());
    pc->setPluginEnabled(QStringLiteral("net.example.item-helper"), true);
    QVERIFY(!pc->plugins().value(1).toMap().value("enabled").toBool());
    QVERIFY(pc->messageIsError());
    pc->dismissMessage();
    QCOMPARE(h.item("sectionStatus_plugins")->property("text").toString(), QStringLiteral("2 installed · 1 on"));
    QTest::qWait(200);
    shot(h, QStringLiteral("4a-plugins-list"));
    h.controller->setAppearance(QStringLiteral("light"));
    shot(h, QStringLiteral("4a-plugins-list-light"));
    h.controller->setAppearance(QStringLiteral("dark"));

    // Uninstall asks first.
    QVERIFY(h.click("pluginOpen"));
    QTRY_VERIFY(h.item("pluginDetailSheet") != nullptr && h.item("pluginDetailSheet")->isVisible());
    QVERIFY(h.click("pluginDetailMenu"));
    QTest::qWait(150);
    QVERIFY(h.click("pluginDetailMenu_uninstall"));
    QTRY_VERIFY(h.item("pluginUninstallConfirm") != nullptr && h.item("pluginUninstallConfirm")->isVisible());
    QVERIFY(h.click("pluginUninstallConfirm"));
    QTRY_COMPARE(pc->plugins().size(), 1);
    QVERIFY(!QFileInfo(h.controller->profileStore()->baseDir() + "/plugins/installed/net.example.creature-core").exists());

    // Developer mode off again: console never offered, link button gone.
    QVERIFY(h.click("pluginDeveloperModeToggle"));
    QTRY_VERIFY(!pc->developerMode());
    QVERIFY(!PlayerSettings(h.controller->profileStore()->baseDir()).developerMode());
    QTRY_VERIFY(!h.item("consoleLoadUnpacked")->isVisible());
  }

  // Boards 4d-1 and 4d-2: the Developer console in Settings > Plugins (preview data), then the Developer mode tag and Ctrl+Shift+D.
  void developerConsolePage() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-dc");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    PluginsController* pc = h.controller->plugins();
    gotoPlugins(h);
    QVERIFY(h.item("pluginDeveloperConsole") != nullptr);
    QVERIFY(!h.item("pluginDeveloperConsole")->isVisible());  // Developer mode only
    QVERIFY(h.click("pluginDeveloperModeToggle"));
    QTRY_VERIFY(pc->developerMode());
    QTRY_VERIFY(h.item("pluginDeveloperConsole")->isVisible());
    QVERIFY(h.item("consoleNoPlugins")->isVisible());  // nothing runs: the empty state says what to do

    QVariantMap data = consoleData(true, true);
    data.insert("unpacked", true);
    data.insert("folder", "/home/dev/item-helper");
    data.insert("running", true);
    data.insert("errorLine", "Paused after an error. Your game keeps running.");
    data.insert("permissions", QVariantMap{{"granted", QVariantList{QVariantMap{{"id", "memory.read"}, {"label", "Read game memory"}}}},
                                           {"notGranted", QVariantList{QVariantMap{{"id", "memory.write"}, {"label", "Change game memory"}}}}});
    pc->setPreviewConsole(data);
    QTRY_VERIFY(h.item("consoleError")->isVisible());
    QVERIFY(h.item("consoleLoadUnpacked")->isVisible());
    QVERIFY(h.item("consoleReload")->isVisible());
    QCOMPARE(h.item("consoleRestart")->property("text").toString(), QStringLiteral("Reload and resume"));
    // The tag only shows while the select is at least 320 px wide (with a wide font the buttons take the room; the folder row
    // below still says "unpacked"), so the check follows the select's width instead of the font of the machine.
    QCOMPARE(h.item("consoleUnpackedTag")->isVisible(), h.item("consoleSelect")->width() >= 320);
    QCOMPARE(h.item("consoleFolder")->property("text").toString(), QStringLiteral("/home/dev/item-helper"));
    QVERIFY(h.item("consoleReloadOnChange")->isVisible());
    QCOMPARE(h.items("consolePermGranted").size(), 1);
    QVERIFY(h.item("consolePermNotGranted")->property("text").toString().contains(QStringLiteral("Not granted: Change game memory")));

    // Level chips with counts; a chip filters the table, Pause log is a button.
    QCOMPARE(h.item("consoleChip_error")->property("count").toInt(), 1);
    QCOMPARE(h.item("consoleChip_warn")->property("count").toInt(), 1);
    QCOMPARE(h.item("consoleChip_debug")->property("count").toInt(), 1);
    QCOMPARE(h.items("consoleLog_debug").size(), 1);
    QVERIFY(h.click("consoleChip_debug"));
    QTRY_COMPARE(h.items("consoleLog_debug").size(), 0);
    QCOMPARE(h.item("consoleChip_debug")->property("count").toInt(), 1);  // still counted
    QVERIFY(!h.item("consoleChip_debug")->property("active").toBool());
    QVERIFY(h.click("consoleChip_debug"));
    QTRY_COMPARE(h.items("consoleLog_debug").size(), 1);
    QCOMPARE(h.item("consolePauseLog")->property("text").toString(), QStringLiteral("Pause log"));
    QVERIFY(h.click("consolePauseLog"));
    QTRY_COMPARE(h.item("consolePauseLog")->property("text").toString(), QStringLiteral("Resume log"));
    QVERIFY(h.click("consolePauseLog"));
    QTRY_COMPARE(h.item("consolePauseLog")->property("text").toString(), QStringLiteral("Pause log"));
    shot(h, QStringLiteral("4d-1-developer-console"));

    // The console restarts the plugin through the controller.
    QSignalSpy reload(pc, &PluginsController::reloadRequested);
    QVERIFY(h.click("consoleRestart"));
    QCOMPARE(reload.size(), 1);
    QVERIFY(h.click("consoleReload"));
    QCOMPARE(reload.size(), 2);

    // 4d-2B: a manifest problem is told next to the button and installs nothing.
    pc->loadUnpacked(QDir::tempPath() + "/uzume-no-such-folder");
    QTRY_VERIFY(h.item("consoleLoadResult")->isVisible());
    QVERIFY(h.item("consoleLoadRetry")->isVisible());
    shot(h, QStringLiteral("4d-2b-manifest-problem"));
    QVERIFY(h.click("consoleLoadDismiss"));
    QTRY_VERIFY(!h.item("consoleLoadResult")->isVisible());
    pc->setPreviewConsole({});
  }

  // The game header tag and Ctrl+Shift+D (board 4d-2F).
  void developerModeTagAndShortcut() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-dt");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1440, 900);
    startPreviewGame(h);
    PluginsController* pc = h.controller->plugins();
    QVERIFY(h.item("developerModeTag") == nullptr || !h.item("developerModeTag")->isVisible());
    pc->setDeveloperMode(true);
    // The tag shows when it fits next to the title (the very wide font of the widefont run leaves no room by design).
    if (qEnvironmentVariableIsEmpty("UZUME_TEST_WIDE_FONT")) {
      QTRY_VERIFY(h.item("developerModeTag") != nullptr && h.item("developerModeTag")->isVisible());
      QCOMPARE(h.item("developerModeTag")->property("text").toString(), QStringLiteral("Developer mode"));
      shot(h, QStringLiteral("4d-2f-developer-mode-tag"));
    }
    // The console tab exists while plugins run (preview); Ctrl+Shift+D opens it.
    pc->setPreviewConsole(consoleData(false, true));
    QTRY_VERIFY(h.item("panelTab_console") != nullptr && h.item("panelTab_console")->isVisible());
    QVERIFY(h.item("pluginConsole") == nullptr || !h.item("pluginConsole")->isVisible());
    QTest::keyClick(h.window, Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(h.item("pluginConsole") != nullptr && h.item("pluginConsole")->isVisible());
    pc->setPreviewConsole({});
    pc->setDeveloperMode(false);
    QTRY_VERIFY(h.item("developerModeTag") == nullptr || !h.item("developerModeTag")->isVisible());
  }

  void consoleTab() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-pc");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1440, 900);
    startPreviewGame(h);
    PluginsController* pc = h.controller->plugins();

    // Without plugins/Developer mode the strip shows only the Session tab.
    QVERIFY(h.item("panelTabBar") != nullptr && h.item("panelTabBar")->isVisible());
    QVERIFY(h.item("panelTab_game") != nullptr && h.item("panelTab_game")->isVisible());
    QVERIFY(h.item("panelTab_console") == nullptr || !h.item("panelTab_console")->isVisible());

    pc->setPreviewConsole(consoleData(false, true));
    QVERIFY(pc->consoleAvailable());
    QTRY_VERIFY(h.item("panelTab_console") != nullptr && h.item("panelTab_console")->isVisible());
    QVERIFY(h.item("pluginConsole") == nullptr || !h.item("pluginConsole")->isVisible());
    QVERIFY(h.click("panelTab_console"));
    QTRY_VERIFY(h.item("pluginConsole") != nullptr && h.item("pluginConsole")->isVisible());
    QVERIFY(!h.item("consoleError")->isVisible());
    QCOMPARE(h.items("consoleWatched").size(), 3);
    QCOMPARE(h.items("consoleEvent").size(), 2);
    QCOMPARE(h.items("consoleLog_warn").size(), 1);
    QCOMPARE(h.items("consoleLog_error").size(), 1);
    QCOMPARE(h.items("consoleRegion").size(), 2);
    QVERIFY(!h.item("consoleNoRegions")->isVisible());
    QVERIFY(h.item("consoleReload") != nullptr);
    QCOMPARE(h.item("consoleReload")->property("tip").toString(), QStringLiteral("Reload plugin · Ctrl+R"));
    shot(h, QStringLiteral("4d-console"));

    // Clear empties the log (preview).
    QVERIFY(h.click("consoleClear"));
    QTRY_COMPARE(h.items("consoleLog_warn").size(), 0);

    // Error state with "Restart plugin"; no memory map.
    pc->setPreviewConsole(consoleData(true, false));
    QTRY_VERIFY(h.item("consoleError") != nullptr && h.item("consoleError")->isVisible());
    QVERIFY(h.item("consoleRestart")->isVisible());
    QVERIFY(h.item("consoleErrorText")->property("text").toString().contains(QStringLiteral("attempt to index a nil value")));
    QVERIFY(h.item("consoleNoRegions")->isVisible());
    QCOMPARE(h.item("consoleNoRegions")->property("text").toString(), QStringLiteral("The core exposes no memory map"));
    shot(h, QStringLiteral("4d-console-error"));

    // Selecting another plugin goes through the controller.
    QVERIFY(h.click("consoleSelect"));
    QTRY_VERIFY(h.item("consolePlugin_item-helper") != nullptr && h.item("consolePlugin_item-helper")->isVisible());
    QVERIFY(h.click("consolePlugin_item-helper"));
    QTRY_COMPARE(pc->console().value("selected").toString(), QStringLiteral("item-helper"));

    // The console goes away with Developer mode / the plugins: back on the Game tab, no tab bar.
    pc->setPreviewConsole({});
    QTRY_VERIFY(h.item("panelTab_console") == nullptr || !h.item("panelTab_console")->isVisible());  // the strip stays, the console tab is gone
    QVERIFY(h.item("sharingBlock") != nullptr && h.item("sharingBlock")->isVisible());
  }

  // 4a-3 / 4a-4 and P20: a plugin that needs another one. Install offers both, turning on offers both, the detail sheet
  // shows the relations, and the dependency's toggle agrees with the plugin that uses it.
  void dependencyDialogsAndDetail() {
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-dep");
    hub.name = QStringLiteral("Home server");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1")};
    const auto offer = [&hub](const FbSpec& spec) {
      const QByteArray file = makeUzPlugin(spec);
      hub.pluginList.append(QJsonObject{{"plugin_id", spec.id}, {"version", spec.version}, {"name", spec.name}, {"description", ""}, {"api", 1},
                                        {"origin", "unverified"}, {"sha256", sha256Hex(file)}, {"size", file.size()},
                                        {"permissions", QJsonArray::fromStringList(spec.permissions)}, {"requires", spec.requires_},
                                        {"created_at", "2026-10-12T10:00:00Z"}});
      hub.pluginFiles.insert(spec.id + "/" + spec.version, file);
    };
    FbSpec core;
    core.id = "net.example.pokemon-core";
    core.name = "SoulLink Core";
    core.version = "0.3.1";
    core.permissions = {"memory.read"};
    FbSpec link;
    link.id = "net.example.soullink";
    link.name = "SoulLink & Nuzlocke";
    link.version = "0.3.2";
    link.permissions = {"ui", "run.state"};
    link.requires_ = QJsonObject{{core.id, "^0.3"}};
    link.ui = QJsonObject{{"panel", QJsonObject{{"title", "SoulLink"}}}, {"screen", QJsonObject{{"title", "Pairs"}}}};
    offer(core);
    offer(link);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1440, 900);
    PluginsController* pc = h.controller->plugins();
    QTRY_COMPARE_WITH_TIMEOUT(pc->hubPlugins().size(), 2, 8000);
    gotoPlugins(h);
    QTRY_VERIFY(h.item("hubPluginRow_net.example.soullink") != nullptr);
    QCOMPARE(h.item("pluginsEmptyText")->property("text").toString(),
             QStringLiteral("Home server offers 2 plugins. They are listed below under Available on Home server."));
    shot(h, QStringLiteral("4a-3-plugins-available"));

    // SoulLink needs SoulLink Core: the Hub row says so and Install offers both.
    QVariantMap linkOffer;
    for (const QVariant& v : pc->hubPlugins())
      if (v.toMap().value("id").toString() == link.id) linkOffer = v.toMap();
    QCOMPARE(linkOffer.value("needsInstall").toList().size(), 1);
    // The rows' buttons share one name; pick the one of the SoulLink row.
    {
      QQuickItem* row = h.item("hubPluginRow_net.example.soullink");
      QVERIFY(row != nullptr);
      QQuickItem* btn = nullptr;
      std::function<void(QQuickItem*)> find = [&](QQuickItem* i) {
        if (i->objectName() == QLatin1String("hubPluginInstall")) btn = i;
        for (QQuickItem* c : i->childItems()) find(c);
      };
      find(row);
      QVERIFY(btn != nullptr);
      QVERIFY(QMetaObject::invokeMethod(btn, "clicked"));
    }
    QTRY_VERIFY(h.item("pluginInstallDialog") != nullptr && h.item("pluginInstallDialog")->isVisible());
    QVERIFY(h.item("pluginInstallDependency")->property("text").toString().contains(QStringLiteral("Also install SoulLink Core 0.3.1")));
    shot(h, QStringLiteral("4a-3-dialog-install-both"));
    QVERIFY(h.click("pluginInstallBoth"));
    QTRY_COMPARE_WITH_TIMEOUT(pc->plugins().size(), 2, 12000);
    QVERIFY2(!pc->messageIsError(), qPrintable(pc->message()));

    // Installed first: two rows, both off. The warning about the dependency never shows a raw id.
    QTest::qWait(300);
    for (const QVariant& v : pc->plugins()) {
      QVERIFY(!v.toMap().value("problemsText").toString().contains(QStringLiteral("net.example")));
      QVERIFY(!v.toMap().value("enabled").toBool());
      QVERIFY(!v.toMap().value("subtitle").toString().isEmpty());  // every row has a description line (P21)
    }
    QCOMPARE(h.item("sectionStatus_plugins")->property("text").toString(), QStringLiteral("2 installed · none on"));

    // Turning on SoulLink while SoulLink Core is off asks, then turns both on (P20): the toggles agree.
    {
      QQuickItem* row = h.item("pluginRow_net.example.soullink");
      QVERIFY(row != nullptr);
      QQuickItem* toggle = nullptr;
      std::function<void(QQuickItem*)> find = [&](QQuickItem* i) {
        if (i->objectName() == QLatin1String("pluginToggle")) toggle = i;
        for (QQuickItem* c : i->childItems()) find(c);
      };
      find(row);
      QVERIFY(toggle != nullptr);
      const QPointF p = toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2));
      QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, p.toPoint());
    }
    QTRY_VERIFY(h.item("pluginEnableConfirm") != nullptr && h.item("pluginEnableConfirm")->isVisible());
    QVERIFY(h.item("pluginEnableDependency")->isVisible());
    QVERIFY(h.item("pluginEnableDependency")->property("text").toString().contains(QStringLiteral("SoulLink Core")));
    QCOMPARE(h.item("pluginEnableConfirmButton")->property("text").toString(), QStringLiteral("Turn on both"));
    shot(h, QStringLiteral("4a-3-dialog-turn-on-both"));
    QVERIFY(h.click("pluginEnableConfirmButton"));
    QTRY_COMPARE(h.item("sectionStatus_plugins")->property("text").toString(), QStringLiteral("2 installed · 2 on"));
    shot(h, QStringLiteral("4a-3-plugins-installed"));

    // 4a-4: the detail sheet.
    {
      QQuickItem* row = h.item("pluginRow_net.example.soullink");
      QQuickItem* open = nullptr;
      std::function<void(QQuickItem*)> find = [&](QQuickItem* i) {
        if (i->objectName() == QLatin1String("pluginOpen")) open = i;
        for (QQuickItem* c : i->childItems()) find(c);
      };
      find(row);
      QVERIFY(open != nullptr);
      QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
    }
    QTRY_VERIFY(h.item("pluginDetailSheet") != nullptr && h.item("pluginDetailSheet")->isVisible());
    QCOMPARE(h.item("pluginDetailName")->property("text").toString(), QStringLiteral("SoulLink & Nuzlocke"));
    QVERIFY(h.item("pluginDetailVersion")->property("text").toString().startsWith(QStringLiteral("0.3.2 · up to date")));
    QCOMPARE(h.item("pluginDetailOrigin")->property("text").toString(), QStringLiteral("Server upload · unverified"));
    QVERIFY(qAbs(h.item("pluginDetailSheet")->width() - 460) < 1.0);
    // ui.screen is the plugin's run tab; outside a run it opens from the detail sheet ("Open"), never from a sidebar row.
    QCOMPARE(h.item("pluginAdds")->property("text").toString(), QStringLiteral("Adds: panel tab · run tab"));
    QVERIFY(h.item("pluginDetailOpen") != nullptr && h.item("pluginDetailOpen")->isVisible());
    QCOMPARE(h.item("pluginDetailOpen")->property("text").toString(), QStringLiteral("Open"));
    QVERIFY(h.item("pluginDetailOpen")->isEnabled());  // on and without problems
    for (const QVariant& v : pc->plugins()) {
      const QVariantMap m = v.toMap();
      QCOMPARE(m.value("hasScreen").toBool(), m.value("id").toString() == link.id);
    }
    QVERIFY(h.item("navPlugin_net.example.soullink") == nullptr);
    shot(h, QStringLiteral("4a-4-plugin-detail"));
    h.controller->setAppearance(QStringLiteral("light"));
    shot(h, QStringLiteral("4a-4-plugin-detail-light"));
    h.controller->setAppearance(QStringLiteral("dark"));
    QVERIFY(h.click("pluginDetailClose"));
  }

  // Hub delivery in Settings > Plugins: browse, install (official and unverified), origin pills, the warning before an
  // unverified plugin is turned on, local files, a failed download. Synthetic plugin files only.
  void hubPluginsAndOrigins() {
    const TestKey key = fixedTestKey();
    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-pl2");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1")};
    const auto offer = [&hub](const FbSpec& spec, const QByteArray& served = {}) {
      const QByteArray file = makeUzPlugin(spec);
      hub.pluginList.append(QJsonObject{{"plugin_id", spec.id}, {"version", spec.version}, {"name", spec.name}, {"description", "Does a thing"},
                                        {"api", 1}, {"origin", spec.sign ? "official" : "unverified"}, {"sha256", sha256Hex(file)},
                                        {"size", file.size()}, {"permissions", QJsonArray::fromStringList(spec.permissions)},
                                        {"created_at", "2026-10-12T10:00:00Z"}});
      hub.pluginFiles.insert(spec.id + "/" + spec.version, served.isNull() ? file : served);
    };
    FbSpec official;
    official.id = "net.example.official";
    official.name = "Official Tracker";
    official.sign = true;
    official.permissions = {"memory.read", "ui"};
    FbSpec community;
    community.id = "net.example.community";
    community.name = "Community Helper";
    community.permissions = {"memory.read", "run.state"};
    FbSpec broken;
    broken.id = "net.example.broken";
    broken.name = "Broken Download";
    FbSpec brokenServed = broken;
    brokenServed.lua = "fb.log.info('HELLO')\n";  // same length: the size check passes, the SHA-256 pin does not
    offer(official);
    offer(community);
    offer(broken, makeUzPlugin(brokenServed));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    PluginsController* pc = h.controller->plugins();
    pc->manager()->setTrustedKeys({key.pub});
    QTRY_VERIFY_WITH_TIMEOUT(pc->hubSupported(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(pc->hubPlugins().size(), 3, 8000);

    gotoPlugins(h);
    // The Hub list is there without a "Browse" step (P19).
    QTRY_VERIFY(h.item("hubPluginsPanel")->isVisible());
    QTRY_VERIFY(h.item("hubPluginRow_net.example.official") != nullptr);
    QCOMPARE(h.item("hubPluginsTitle")->property("text").toString(), QStringLiteral("Available on Home · 3"));
    QStringList pills;
    for (QQuickItem* i : h.items("hubPluginOrigin")) pills << i->property("text").toString();
    QVERIFY(pills.contains(QStringLiteral("✓ Official")));
    QVERIFY(pills.contains(QStringLiteral("Server upload · unverified")));
    shot(h, QStringLiteral("4a-plugins-hub-browse"));
    h.controller->setAppearance(QStringLiteral("light"));
    shot(h, QStringLiteral("4a-plugins-hub-browse-light"));
    h.controller->setAppearance(QStringLiteral("dark"));

    // Install the official one (first row): it needs no Developer mode and stays off until turned on.
    QVERIFY(!pc->developerMode());
    QVERIFY(h.click("hubPluginInstall"));
    QTRY_VERIFY_WITH_TIMEOUT(pc->plugins().size() == 1, 8000);
    QVERIFY2(!pc->messageIsError(), qPrintable(pc->message()));
    QVERIFY(pc->message().startsWith(QStringLiteral("Installed Official Tracker 1.0.0 from your server")));
    QVariantMap row = pc->plugins().value(0).toMap();
    QCOMPARE(row.value("origin").toString(), QStringLiteral("official"));
    QCOMPARE(row.value("originText").toString(), QStringLiteral("✓ Official"));
    QVERIFY(!row.value("enabled").toBool());
    QVERIFY(row.value("canToggle").toBool());
    QVERIFY(row.value("problemsText").toString().isEmpty());  // runs without Developer mode
    QVERIFY(!row.value("needsConfirm").toBool());
    QTRY_COMPARE(pc->hubPlugins().value(0).toMap().value("state").toString(), QStringLiteral("installed"));
    QCOMPARE(pc->hubPlugins().value(1).toMap().value("state").toString(), QStringLiteral("install"));
    QTRY_COMPARE(h.item("pluginOrigin")->property("text").toString(), QStringLiteral("✓ Official"));
    pc->setPluginEnabled(QStringLiteral("net.example.official"), true);
    QVERIFY(pc->plugins().value(0).toMap().value("enabled").toBool());
    QCOMPARE(h.item("sectionStatus_plugins")->property("text").toString(), QStringLiteral("1 installed · 1 on"));
    // Remove it again so that the next row is the only toggle on the screen.
    pc->uninstall(QStringLiteral("net.example.official"));
    QTRY_COMPARE(pc->plugins().size(), 0);

    // The unverified Hub upload: pill, warning before it is turned on, still no Developer mode needed.
    pc->installHubPlugin(QStringLiteral("net.example.community"), QStringLiteral("1.0.0"));
    QTRY_VERIFY_WITH_TIMEOUT(pc->plugins().size() == 1, 8000);
    QVERIFY(pc->message().contains(QStringLiteral("Uzume has not checked this plugin")));
    row = pc->plugins().value(0).toMap();
    QCOMPARE(row.value("origin").toString(), QStringLiteral("unverified"));
    QCOMPARE(row.value("originText").toString(), QStringLiteral("Server upload · unverified"));
    QVERIFY(row.value("needsConfirm").toBool());
    QVERIFY(row.value("problemsText").toString().isEmpty());
    QCOMPARE(row.value("permissionsText").toString(), QStringLiteral("Read game memory · Share run progress with your server"));
    QTRY_VERIFY(h.item("pluginToggle") != nullptr && h.item("pluginToggle")->isVisible());
    QVERIFY(h.click("pluginToggle"));
    QTRY_VERIFY(h.item("pluginEnableConfirm")->isVisible());
    QVERIFY(h.item("pluginUnverifiedWarning")->property("text").toString().startsWith(QStringLiteral("Uzume has not checked this plugin")));
    QVERIFY(h.item("pluginEnablePermissions")->property("text").toString().contains(QStringLiteral("Read game memory")));
    QVERIFY(!pc->plugins().value(0).toMap().value("enabled").toBool());
    QVERIFY(!h.item("pluginToggle")->property("checked").toBool());
    shot(h, QStringLiteral("4c-plugins-unverified-warning"));
    h.controller->setAppearance(QStringLiteral("light"));
    shot(h, QStringLiteral("4c-plugins-unverified-warning-light"));
    h.controller->setAppearance(QStringLiteral("dark"));
    QVERIFY(h.click("pluginEnableCancel"));
    QTRY_VERIFY(!h.item("pluginEnableConfirm")->isVisible());
    QVERIFY(!pc->plugins().value(0).toMap().value("enabled").toBool());
    QVERIFY(h.click("pluginToggle"));
    QTRY_VERIFY(h.item("pluginEnableConfirm")->isVisible());
    QVERIFY(h.click("pluginEnableConfirmButton"));
    QTRY_VERIFY(pc->plugins().value(0).toMap().value("enabled").toBool());
    QTRY_VERIFY(!h.item("pluginEnableConfirm")->isVisible());

    // A download that does not match the pin installs nothing and says so.
    pc->installHubPlugin(QStringLiteral("net.example.broken"), QStringLiteral("1.0.0"));
    QTRY_VERIFY_WITH_TIMEOUT(pc->messageIsError(), 8000);
    QVERIFY2(pc->message().contains(QStringLiteral("does not match")), qPrintable(pc->message()));
    QCOMPARE(pc->plugins().size(), 1);
    QVERIFY(!QFileInfo::exists(h.controller->profileStore()->baseDir() + "/plugins/hub/net.example.broken"));
    for (const QVariant& v : pc->hubPlugins())
      if (v.toMap().value("id").toString() == QLatin1String("net.example.broken")) QCOMPARE(v.toMap().value("state").toString(), QStringLiteral("install"));

    // A local .uzplugin: unsigned = Local file, needs Developer mode; signed = Official, needs none.
    FbSpec localSpec;
    localSpec.id = "net.example.localfile";
    localSpec.name = "Local Thing";
    const QString localFile = src_.filePath("local.uzplugin");
    {
      QFile f(localFile);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(makeUzPlugin(localSpec));
    }
    pc->installFromPath(QUrl::fromLocalFile(localFile).toString());
    QVERIFY2(!pc->messageIsError(), qPrintable(pc->message()));
    QVERIFY(pc->message().contains(QStringLiteral("Developer mode")));
    QCOMPARE(pc->plugins().size(), 2);
    QVariantMap local;
    for (const QVariant& v : pc->plugins())
      if (v.toMap().value("id").toString() == QLatin1String("net.example.localfile")) local = v.toMap();
    QCOMPARE(local.value("origin").toString(), QStringLiteral("local"));
    QCOMPARE(local.value("originText").toString(), QStringLiteral("Local file"));
    QCOMPARE(local.value("problemsText").toString(), QStringLiteral("Runs only while Developer mode is on"));
    FbSpec signedSpec;
    signedSpec.id = "net.example.signedfile";
    signedSpec.name = "Signed Thing";
    signedSpec.sign = true;
    const QString signedFile = src_.filePath("signed.uzplugin");
    {
      QFile f(signedFile);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(makeUzPlugin(signedSpec));
    }
    pc->installFromPath(signedFile);
    QVERIFY2(!pc->messageIsError(), qPrintable(pc->message()));
    QVERIFY(!pc->message().contains(QStringLiteral("Developer mode")));
    for (const QVariant& v : pc->plugins())
      if (v.toMap().value("id").toString() == QLatin1String("net.example.signedfile")) {
        QCOMPARE(v.toMap().value("origin").toString(), QStringLiteral("official"));
        QVERIFY(v.toMap().value("problemsText").toString().isEmpty());
      }
    // not a plugin file
    const QString junk = src_.filePath("junk.uzplugin");
    {
      QFile f(junk);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("nope");
    }
    pc->installFromPath(junk);
    QVERIFY(pc->messageIsError());
    QCOMPARE(pc->plugins().size(), 3);
    shot(h, QStringLiteral("4a-plugins-origins"));
  }
};

UITEST_MAIN(PluginsUiTest)
#include "plugins_ui_test.moc"
