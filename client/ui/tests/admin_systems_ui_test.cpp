// Admin area, package 8: the Systems & cores page (systems column with "No core", Use this, switching, automatic update,
// firmware) and the server Plugins page (index Add, upload -> stage -> offer, remove asks first). Offscreen, FakeHub with admin_v1
// and fixed answers for the admin routes; synthetic data only, no ROMs, firmware or plugin code.
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlContext>
#include <QQmlProperty>
#include <QTemporaryDir>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
const QString kApi = QStringLiteral("/api/v1/admin");

const char* kNdsNoCore = R"JSON({
  "id": "nds", "display_name": "NDS", "extensions": ["nds"], "libretro_ids": [], "platforms": [], "provisioning": "x",
  "input_profile": "nds", "display_profile": "nds", "firmware_mode": "builtin", "auto_update": "default", "auto_update_effective": true,
  "cores": [],
  "available_cores": [
    {"core_id": "melondsds", "display_name": "melonDS DS", "license": "GPL", "platforms": [], "extensions": ["nds"],
     "notes": ["Accurate."], "experimental": false, "non_commercial": false, "build_date": "2026-10-01"},
    {"core_id": "desmume", "display_name": "DeSmuME", "license": "GPL", "platforms": [], "extensions": ["nds"],
     "notes": ["Older, lighter."], "experimental": false, "non_commercial": false, "build_date": "2026-09-01"}
  ],
  "firmware": [
    {"id": "bios9.bin", "display_name": "bios9.bin", "sizes": [4096], "required": true, "present": true, "state": "mismatch", "sha256": "aa"},
    {"id": "firmware.bin", "display_name": "firmware.bin", "sizes": [262144], "required": false, "present": false, "state": "optional"},
    {"id": "bios7.bin", "display_name": "bios7.bin", "sizes": [16384], "required": true, "present": false, "state": "missing"}
  ],
  "clients": []
})JSON";

const char* kGbaMgba = R"JSON({
  "id": "gba", "display_name": "GBA", "extensions": ["gba"], "libretro_ids": [], "platforms": [], "provisioning": "x",
  "input_profile": "gba", "display_profile": "gba", "firmware_mode": "builtin", "auto_update": "default", "auto_update_effective": true,
  "default_core_id": "mgba",
  "cores": [{"core_id": "mgba", "display_name": "mGBA", "version": "0.10", "license": "MPL", "origin": "libretro-buildbot", "platforms": [],
             "default": true, "experimental": false, "update_available": true, "update_build_date": "2026-10-20", "auto_update": "auto",
             "auto_update_effective": true}],
  "available_cores": [
    {"core_id": "mgba", "display_name": "mGBA", "license": "MPL", "platforms": [], "extensions": ["gba"], "notes": [], "experimental": false, "non_commercial": false},
    {"core_id": "gbasp", "display_name": "GBA Spare", "license": "GPL", "platforms": [], "extensions": ["gba"], "notes": [], "experimental": true, "non_commercial": false}
  ],
  "firmware": [], "clients": []
})JSON";

const char* kSource = R"JSON({"buildbot_url": "https://buildbot.example", "info_url": "https://info.example",
  "auto_update": true, "cores": 4, "last_success": "2026-10-22T09:00:00Z"})JSON";

QByteArray systemsBody(const QByteArray& nds = QByteArray(kNdsNoCore)) {
  return "{\"systems\": [" + nds + ", " + QByteArray(kGbaMgba) + "], \"core_source\": " + QByteArray(kSource) + "}";
}

QByteArray pluginsBody() {
  return R"JSON({
    "plugins": [
      {"info": {"plugin_id": "net.example.notes", "version": "0.1.0", "name": "Run Notes", "description": "", "api": 1, "origin": "unverified",
                "sha256": "00", "size": 10, "permissions": ["ui"], "requires": {}, "provides": [], "games": [], "created_at": "2026-09-14T10:00:00Z"},
       "source": "upload", "uploaded_by_name": "Max"},
      {"info": {"plugin_id": "net.example.notes", "version": "0.2.0", "name": "Run Notes", "description": "", "api": 1, "origin": "unverified",
                "sha256": "01", "size": 10, "permissions": ["ui", "run.state"], "requires": {}, "provides": [], "games": [], "created_at": "2026-10-01T10:00:00Z"},
       "source": "upload", "uploaded_by_name": "Max"},
      {"info": {"plugin_id": "net.example.core", "version": "1.4.0", "name": "Example Core", "description": "", "api": 1, "origin": "official",
                "sha256": "02", "size": 10, "permissions": ["memory.read"], "requires": {}, "provides": [], "games": [], "created_at": "2026-10-02T10:00:00Z"},
       "source": "index"}
    ],
    "index": {"configured": true, "last_check": "2026-10-22T08:00:00Z", "catalog": [
      {"id": "net.example.splits", "name": "Splits Timer", "version": "0.5.1", "api": 1, "description": "Run timer.", "permissions": ["ui"], "size": 10, "state": "install"},
      {"id": "net.example.core", "name": "Example Core", "version": "1.4.0", "api": 1, "description": "Shared base.", "permissions": [], "size": 10, "state": "installed"},
      {"id": "net.example.notes2", "name": "Run Notes 2", "version": "1.0.0", "api": 1, "description": "Notes.", "permissions": [], "size": 10, "state": "replace",
       "replaces": {"id": "net.example.notes", "name": "Run Notes", "version": "0.2.0"}}
    ]}
  })JSON";
}

const char* kStage = R"JSON({"stage_id": "st_1", "plugin_id": "net.example.tool", "name": "Tool", "version": "0.3.0", "description": "",
  "permissions": ["memory.read", "ui"], "official": false, "signed": false, "sha256": "ab", "size": 12, "already_offered": false, "conflict": false})JSON";
}  // namespace

class AdminSystemsUiTest : public QObject {
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
  static void adminHub(FakeHub& hub) {
    hub.adminUser = true;
    hub.features = {QStringLiteral("saves_v1")};
    hub.adminRoutes[QStringLiteral("GET ") + kApi + QStringLiteral("/systems")] = {200, systemsBody()};
    hub.adminRoutes[QStringLiteral("GET ") + kApi + QStringLiteral("/plugins")] = {200, pluginsBody()};
  }
  static QString text(QQuickItem* it) { return it ? it->property("text").toString() : QString(); }
  static QJsonObject bodyOf(const FakeHub& hub, const QString& key) { return QJsonDocument::fromJson(hub.adminBodies.value(key)).object(); }
  static int calls(const FakeHub& hub, const QString& key) { return hub.adminCalls.value(key); }
  static bool shown(const Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr && it->isVisible();
  }
  static void openPage(Harness& h, const QString& page, const char* pageName) {
    h.window->resize(1400, 1800);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showServerPage(page);
    QTRY_VERIFY(h.item(pageName) != nullptr);
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void systemsColumnWarnsAboutASystemWithoutACore() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("systems"), "serverSystemsPage");
    QTRY_VERIFY(h.item("systemItem_nds") != nullptr);
    QCOMPARE(text(h.item("systemCore_nds")), QStringLiteral("No core"));
    QCOMPARE(text(h.item("systemCore_gba")), QStringLiteral("mGBA"));
    {   // #364: the part that tells the cores of one system apart comes first; the full names are the tooltip
      QString r1, r2;
      QVERIFY(QMetaObject::invokeMethod(h.item("serverSystemsPage"), "shortCoreName", Q_RETURN_ARG(QString, r1), Q_ARG(QString, QStringLiteral("Nintendo - Game Boy / Color (Gambatte)"))));
      QVERIFY(QMetaObject::invokeMethod(h.item("serverSystemsPage"), "shortCoreName", Q_RETURN_ARG(QString, r2), Q_ARG(QString, QStringLiteral("mGBA"))));
      QCOMPARE(r1, QStringLiteral("Gambatte"));
      QCOMPARE(r2, QStringLiteral("mGBA"));
      const QVariant tip = QQmlProperty(h.item("systemItem_gba"), QStringLiteral("ToolTip.text"), qmlContext(h.item("systemItem_gba"))).read();
      QVERIFY(tip.toString().contains(QStringLiteral("mGBA")));
    }
    // Nothing is preselected: the warning and the two cores of the catalog, none "In use".
    QTRY_VERIFY(shown(h, "systemsNoCore"));
    QCOMPARE(text(h.item("systemsNoCore")), QStringLiteral("No core picked. NDS games can't start until you pick one."));
    QTRY_VERIFY(h.item("coresTableRow_1") != nullptr);
    QCOMPARE(text(h.item("coresTableCell_0_name")), QStringLiteral("melonDS DS"));
    QCOMPARE(text(h.item("coresTableCell_0_note")), QStringLiteral("Accurate."));
    QVERIFY(shown(h, "useCore_melondsds") && shown(h, "useCore_desmume"));
    QVERIFY(!shown(h, "coreInUse_melondsds"));
    // Another system: no warning, its core is in use.
    QVERIFY(h.click("systemItem_gba"));
    QTRY_VERIFY(!shown(h, "systemsNoCore"));
    QTRY_VERIFY(shown(h, "coreInUse_mgba"));
    QVERIFY(!shown(h, "useCore_mgba"));
  }

  void useThisInstallsAndPicksTheCore() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString nds = kApi + QStringLiteral("/systems/nds");
    QString picked = QString::fromLatin1(kNdsNoCore);
    picked.replace(QStringLiteral("\"firmware_mode\""), QStringLiteral("\"default_core_id\": \"melondsds\", \"firmware_mode\""));
    picked.replace(QStringLiteral("\"cores\": [],"),
                   QString::fromLatin1(R"JSON("cores": [{"core_id": "melondsds", "display_name": "melonDS DS", "version": "1.1", "license": "GPL",
                   "origin": "libretro-buildbot", "platforms": [], "default": true, "experimental": false, "update_available": false,
                   "auto_update": "auto", "auto_update_effective": true}],)JSON"));
    hub.adminRoutes[QStringLiteral("POST ") + nds + QStringLiteral("/cores/melondsds/install")] = {200, picked.toUtf8()};
    hub.adminRoutes[QStringLiteral("PATCH ") + nds] = {200, picked.toUtf8()};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("systems"), "serverSystemsPage");
    QTRY_VERIFY(h.item("useCore_melondsds") != nullptr);
    QVERIFY(h.clickWhenReady("useCore_melondsds"));
    QTRY_COMPARE(calls(hub, QStringLiteral("PATCH ") + nds), 1);
    QCOMPARE(calls(hub, QStringLiteral("POST ") + nds + QStringLiteral("/cores/melondsds/install")), 1);
    QCOMPARE(bodyOf(hub, QStringLiteral("PATCH ") + nds).value("default_core_id").toString(), QStringLiteral("melondsds"));
    QTRY_COMPARE(text(h.item("systemCore_nds")), QStringLiteral("melonDS DS"));
    QTRY_VERIFY(!shown(h, "systemsNoCore"));
    QTRY_VERIFY(shown(h, "coreInUse_melondsds"));
    QTRY_COMPARE(text(h.item("systemsNote")), QStringLiteral("melonDS DS is now the core for NDS."));
  }

  void switchingACoreAsksFirst() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString gba = kApi + QStringLiteral("/systems/gba");
    hub.adminRoutes[QStringLiteral("POST ") + gba + QStringLiteral("/cores/gbasp/install")] = {200, QByteArray(kGbaMgba)};
    hub.adminRoutes[QStringLiteral("PATCH ") + gba] = {200, QByteArray(kGbaMgba)};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("systems"), "serverSystemsPage");
    QTRY_VERIFY(h.item("systemItem_gba") != nullptr);
    QVERIFY(h.click("systemItem_gba"));
    QTRY_VERIFY(h.item("useCore_gbasp") != nullptr);
    QVERIFY(h.clickWhenReady("useCore_gbasp"));
    QTRY_VERIFY(shown(h, "switchCoreDialog"));
    QCOMPARE(calls(hub, QStringLiteral("PATCH ") + gba), 0);
    QVERIFY(h.click("switchCoreCancel"));
    QTRY_VERIFY(!shown(h, "switchCoreDialog"));
    QCOMPARE(calls(hub, QStringLiteral("PATCH ") + gba), 0);
    QVERIFY(h.clickWhenReady("useCore_gbasp"));
    QTRY_VERIFY(shown(h, "switchCoreDialog"));
    QVERIFY(h.click("switchCoreConfirm"));
    QTRY_COMPARE(calls(hub, QStringLiteral("PATCH ") + gba), 1);
    QCOMPARE(bodyOf(hub, QStringLiteral("PATCH ") + gba).value("default_core_id").toString(), QStringLiteral("gbasp"));
    QTRY_VERIFY(!shown(h, "switchCoreDialog"));  // busy until the reply is processed; its scrim would swallow the next click
    // Update now of an installed core (details of the selected row).
    QVERIFY(h.click("coresTableRow_0"));
    hub.adminRoutes[QStringLiteral("POST ") + gba + QStringLiteral("/cores/mgba/update")] = {200, QByteArray(kGbaMgba)};
    QVERIFY(h.clickWhenReady("coreUpdate"));
    QTRY_COMPARE(calls(hub, QStringLiteral("POST ") + gba + QStringLiteral("/cores/mgba/update")), 1);
  }

  void removingACoreAsksFirst() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString route = QStringLiteral("DELETE ") + kApi + QStringLiteral("/systems/gba/cores/mgba");
    hub.adminRoutes[route] = {409, R"({"error": {"code": "conflict", "message": "It is the core in use."}})"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("systems"), "serverSystemsPage");
    QTRY_VERIFY(h.item("systemItem_gba") != nullptr);
    QVERIFY(h.click("systemItem_gba"));
    QTRY_VERIFY(h.item("coresTableRow_0") != nullptr);
    QVERIFY(h.click("coresTableRow_0"));
    QVERIFY(h.clickWhenReady("coreRemove"));
    QTRY_VERIFY(shown(h, "removeCoreDialog"));
    QCOMPARE(calls(hub, route), 0);
    QVERIFY(h.click("removeCoreConfirm"));
    QTRY_COMPARE(calls(hub, route), 1);
    QTRY_VERIFY(text(h.item("systemsNote")).contains(QStringLiteral("It is the core in use.")));
  }

  void autoUpdateToggleSendsTheSetting() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString route = QStringLiteral("PATCH ") + kApi + QStringLiteral("/cores/settings");
    hub.adminRoutes[route] = {200, R"({"buildbot_url": "b", "info_url": "i", "auto_update": false, "cores": 4})"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("systems"), "serverSystemsPage");
    QTRY_VERIFY(h.item("autoUpdateToggle") != nullptr);
    QTRY_VERIFY(h.item("autoUpdateToggle")->property("checked").toBool());
    QVERIFY(h.clickWhenReady("autoUpdateToggle"));
    QTRY_COMPARE(calls(hub, route), 1);
    const QJsonObject body = bodyOf(hub, route);
    QVERIFY(body.contains(QStringLiteral("auto_update")));
    QCOMPARE(body.value("auto_update").toBool(true), false);
    // Check now starts the sync.
    const QString sync = QStringLiteral("POST ") + kApi + QStringLiteral("/cores/sync");
    hub.adminRoutes[sync] = {202, {}};
    QVERIFY(h.click("checkNow"));
    QTRY_COMPARE(calls(hub, sync), 1);
  }

  void firmwareStatusReplaceKeepAndRemove() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString base = kApi + QStringLiteral("/systems/nds/firmware/");
    hub.adminRoutes[QStringLiteral("PUT ") + base + QStringLiteral("bios7.bin")] = {200, QByteArray(kNdsNoCore)};
    hub.adminRoutes[QStringLiteral("POST ") + base + QStringLiteral("bios9.bin/pin")] = {200, QByteArray(kNdsNoCore)};
    hub.adminRoutes[QStringLiteral("DELETE ") + base + QStringLiteral("bios9.bin")] = {200, QByteArray(kNdsNoCore)};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("systems"), "serverSystemsPage");
    QTRY_VERIFY(h.item("systemsTabs") != nullptr);
    QVERIFY(h.item("serverSystemsPage")->setProperty("tab", QStringLiteral("firmware")));
    QTRY_VERIFY(h.item("firmwareTableRow_2") != nullptr);
    QCOMPARE(text(h.item("firmwareStatus_bios9.bin")), QStringLiteral("Doesn't match"));
    QCOMPARE(text(h.item("firmwareStatus_firmware.bin")), QStringLiteral("Missing"));
    QCOMPARE(text(h.item("firmwareStatus_bios7.bin")), QStringLiteral("Missing"));
    QTRY_VERIFY(shown(h, "firmwareMismatch"));
    QCOMPARE(text(h.item("firmwareAdd_bios9.bin")), QStringLiteral("Replace…"));
    QCOMPARE(text(h.item("firmwareAdd_bios7.bin")), QStringLiteral("Add…"));
    // A file is streamed with PUT and a raw body (the native dialog itself is not driven offscreen).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile f(dir.filePath(QStringLiteral("dummy.bin")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray payload(16384, 'x');
    f.write(payload);
    f.close();
    QQuickItem* page = h.item("serverSystemsPage");
    page->setProperty("uploadFileId", QStringLiteral("bios7.bin"));
    QVERIFY(QMetaObject::invokeMethod(page, "uploadFirmware", Q_ARG(QString, f.fileName())));
    const QString put = QStringLiteral("PUT ") + base + QStringLiteral("bios7.bin");
    QTRY_COMPARE(calls(hub, put), 1);
    QCOMPARE(hub.adminBodies.value(put), payload);
    // Keep anyway pins the file that is there.
    QVERIFY(h.clickWhenReady("firmwareKeep_bios9.bin"));
    const QString pin = QStringLiteral("POST ") + base + QStringLiteral("bios9.bin/pin");
    QTRY_COMPARE(calls(hub, pin), 1);
    QCOMPARE(bodyOf(hub, pin).value("sha256").toString(), QStringLiteral("aa"));
    // Remove asks first.
    const QString del = QStringLiteral("DELETE ") + base + QStringLiteral("bios9.bin");
    QVERIFY(h.clickWhenReady("firmwareRemove_bios9.bin"));
    QTRY_VERIFY(shown(h, "removeFirmwareDialog"));
    QCOMPARE(calls(hub, del), 0);
    QVERIFY(h.click("removeFirmwareConfirm"));
    QTRY_COMPARE(calls(hub, del), 1);
  }

  void pluginsListAndIndexAdd() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString install = QStringLiteral("POST ") + kApi + QStringLiteral("/plugin-index/install");
    hub.adminRoutes[install] = {201, R"({"info": {}, "source": "index"})"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("plugins"), "serverPluginsPage");
    QTRY_VERIFY(h.item("pluginsTableRow_1") != nullptr);
    QVERIFY(h.item("pluginsTableRow_2") == nullptr);   // two plugins, the older version of one is not a row
    QCOMPARE(text(h.item("pluginsTableCell_0_version")), QStringLiteral("0.2.0"));
    QCOMPARE(text(h.item("pluginsTableCell_0_permissions")), QStringLiteral("Show panels in game, Share run progress"));
    QCOMPARE(text(h.item("pluginOrigin_net.example.notes")), QStringLiteral("Not signed"));
    QCOMPARE(text(h.item("pluginOrigin_net.example.core")), QStringLiteral("Official"));
    QTRY_VERIFY(h.item("indexAdd_net.example.splits") != nullptr);
    QVERIFY(h.item("indexAdd_net.example.core") == nullptr || !h.item("indexAdd_net.example.core")->isVisible());
    QTRY_VERIFY(h.item("indexAdd_net.example.notes2") != nullptr);
    QCOMPARE(text(h.item("indexAdd_net.example.notes2")), QStringLiteral("Replace"));
    QVERIFY(text(h.item("indexTableCell_2_description")).contains(QStringLiteral("Replaces Run Notes 0.2.0")));
    QCOMPARE(text(h.item("pluginReplaced_net.example.notes")), QStringLiteral("Replaced by Run Notes 2"));
    QVERIFY(h.clickWhenReady("indexAdd_net.example.splits"));
    QTRY_COMPARE(calls(hub, install), 1);
    QCOMPARE(bodyOf(hub, install).value("id").toString(), QStringLiteral("net.example.splits"));
    QCOMPARE(bodyOf(hub, install).value("version").toString(), QStringLiteral("0.5.1"));
  }

  void indexCheckSaysHowManyUpdatesAreWaiting() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString refresh = QStringLiteral("POST ") + kApi + QStringLiteral("/plugin-index/refresh");
    hub.adminRoutes[refresh] = {200, R"({"configured": true, "last_check": "2026-10-29T08:00:00Z", "catalog": [
      {"id": "net.example.soullink", "name": "SoulLink", "version": "0.9.2", "api": 1, "description": "", "permissions": [], "size": 10, "state": "update", "installed_version": "0.9.1"},
      {"id": "net.example.splits", "name": "Splits", "version": "0.1.4", "api": 1, "description": "", "permissions": [], "size": 10, "state": "update", "installed_version": "0.1.3"},
      {"id": "net.example.core", "name": "Example Core", "version": "1.4.0", "api": 1, "description": "", "permissions": [], "size": 10, "state": "installed"}]})"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("plugins"), "serverPluginsPage");
    QTRY_VERIFY(h.item("indexRefresh") != nullptr);
    QCOMPARE(h.controller->admin()->pluginUpdates(), 0);
    QVERIFY(h.clickWhenReady("indexRefresh"));
    QTRY_COMPARE(calls(hub, refresh), 1);
    QTRY_COMPARE(text(h.item("pluginsNote")), QStringLiteral("2 updates available: SoulLink 0.9.2, Splits 0.1.4"));
    QCOMPARE(h.controller->admin()->pluginUpdates(), 2);

    // Nothing waiting: the message says so instead of claiming more.
    hub.adminRoutes[refresh] = {200, R"({"configured": true, "last_check": "2026-10-29T09:00:00Z", "catalog": [
      {"id": "net.example.core", "name": "Example Core", "version": "1.4.0", "api": 1, "description": "", "permissions": [], "size": 10, "state": "installed"}]})"};
    QVERIFY(h.clickWhenReady("indexRefresh"));
    QTRY_COMPARE(calls(hub, refresh), 2);
    QTRY_COMPARE(text(h.item("pluginsNote")), QStringLiteral("No new versions."));
    QCOMPARE(h.controller->admin()->pluginUpdates(), 0);
  }

  void uploadStagesThenOffersWithEveryPermissionTicked() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString stage = QStringLiteral("POST ") + kApi + QStringLiteral("/plugins/stage");
    const QString offer = QStringLiteral("POST ") + kApi + QStringLiteral("/plugins/stage/st_1/offer");
    const QString discard = QStringLiteral("DELETE ") + kApi + QStringLiteral("/plugins/stage/st_1");
    hub.adminRoutes[stage] = {201, QByteArray(kStage)};
    hub.adminRoutes[offer] = {201, R"({"info": {}, "source": "upload"})"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("plugins"), "serverPluginsPage");
    QTRY_VERIFY(h.item("pluginUpload") != nullptr);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile f(dir.filePath(QStringLiteral("tool.uzplugin")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray payload = QByteArray("PK\x03\x04", 4) + QByteArray(2000, 'z');
    f.write(payload);
    f.close();
    QQuickItem* page = h.item("serverPluginsPage");
    // First round: Cancel throws the staged file away.
    QVERIFY(QMetaObject::invokeMethod(page, "uploadPlugin", Q_ARG(QString, f.fileName())));
    QTRY_VERIFY(shown(h, "stageDialog"));
    QCOMPARE(hub.adminBodies.value(stage), payload);
    QCOMPARE(text(h.item("dialogTitle")), QStringLiteral("Offer Tool 0.3.0 to everyone?"));
    QCOMPARE(text(h.item("stagePermission_0")), QStringLiteral("Read game memory"));
    QCOMPARE(text(h.item("stagePermission_1")), QStringLiteral("Show panels in game"));
    QVERIFY(h.click("stageCancel"));
    QTRY_VERIFY(!shown(h, "stageDialog"));
    QTRY_COMPARE(calls(hub, discard), 1);
    QCOMPARE(calls(hub, offer), 0);
    // Second round: Offer stays disabled until every permission is ticked.
    QVERIFY(QMetaObject::invokeMethod(page, "uploadPlugin", Q_ARG(QString, f.fileName())));
    QTRY_VERIFY(shown(h, "stageDialog"));
    QVERIFY(!h.item("stageOffer")->property("enabled").toBool());
    QVERIFY(h.click("stagePermission_0"));
    QVERIFY(!h.item("stageOffer")->property("enabled").toBool());
    QVERIFY(h.click("stagePermission_1"));
    QTRY_VERIFY(h.item("stageOffer")->property("enabled").toBool());
    QVERIFY(h.click("stageOffer"));
    QTRY_COMPARE(calls(hub, offer), 1);
    const QJsonArray granted = bodyOf(hub, offer).value("granted_permissions").toArray();
    QCOMPARE(granted.size(), 2);
    QCOMPARE(granted.at(0).toString(), QStringLiteral("memory.read"));
    QCOMPARE(granted.at(1).toString(), QStringLiteral("ui"));
    QTRY_VERIFY(!shown(h, "stageDialog"));
    QCOMPARE(calls(hub, discard), 1);   // an offered stage is not discarded
    QTRY_VERIFY(text(h.item("pluginsNote")).contains(QStringLiteral("Offered Tool 0.3.0 to everyone.")));
  }

  void removingAPluginAsksFirst() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString route = QStringLiteral("DELETE ") + kApi + QStringLiteral("/plugins/net.example.notes/0.2.0");
    hub.adminRoutes[route] = {204, {}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, QStringLiteral("plugins"), "serverPluginsPage");
    QTRY_VERIFY(h.item("pluginsTableRow_0") != nullptr);
    QVERIFY(h.click("pluginsTableRow_0"));
    QVERIFY(h.clickWhenReady("pluginRemove"));
    QTRY_VERIFY(shown(h, "removePluginDialog"));
    QCOMPARE(calls(hub, route), 0);
    QVERIFY(h.click("removePluginCancel"));
    QTRY_VERIFY(!shown(h, "removePluginDialog"));
    QCOMPARE(calls(hub, route), 0);
    QVERIFY(h.clickWhenReady("pluginRemove"));
    QTRY_VERIFY(shown(h, "removePluginDialog"));
    QVERIFY(h.click("removePluginConfirm"));
    QTRY_COMPARE(calls(hub, route), 1);
  }
};

UITEST_MAIN(AdminSystemsUiTest)
#include "admin_systems_ui_test.moc"
