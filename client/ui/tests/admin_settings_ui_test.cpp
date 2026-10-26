// Server settings in the app (0.18 package 8, 3q-app-1 to 3q-app-7): Settings for "Server" with the seven pages General, Network & sharing,
// Sign-in, Updates, Backups, Storage and Diagnostics. Offscreen, FakeHub with admin_v1 and fixed or hook-driven admin routes; synthetic data only.
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "controllerprofiles.h"
#include "controllerscontroller.h"
#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
const QString kApi = QStringLiteral("/api/v1/admin");

QString get(const QString& path) { return QStringLiteral("GET ") + kApi + path; }
QString put(const QString& path) { return QStringLiteral("PUT ") + kApi + path; }
QString post(const QString& path) { return QStringLiteral("POST ") + kApi + path; }
QString del(const QString& path) { return QStringLiteral("DELETE ") + kApi + path; }

QByteArray netSetting(const char* key, const char* label, const char* value, const char* source = "env", bool pending = false) {
  return QByteArray("{\"key\": \"") + key + "\", \"label\": \"" + label + "\", \"value\": \"" + value + "\", \"running_value\": \"" + value +
         "\", \"default_value\": \"" + value + "\", \"source\": \"" + source + "\", \"needs_restart\": true, \"restart_pending\": " +
         (pending ? "true" : "false") + "}";
}

// The network block; `pending` lists the labels that wait for a restart, `port` is the listen port.
QByteArray networkBody(const char* port = "8443", const char* pending = "") {
  QByteArray p = pending[0] ? QByteArray("[\"") + pending + "\"]" : QByteArray("[]");
  return "{\"settings\": [" + netSetting("listen_port", "Listen port", port, pending[0] ? "web" : "env", pending[0] != 0) + ", " +
         netSetting("public_host", "Public host", "server.example.com:8443") + ", " + netSetting("turn", "Built-in TURN relay", "true") + ", " +
         netSetting("turn_port", "TURN port", "3478") + ", " + netSetting("turn_relay_ports", "Relay port range", "49160-49199") + ", " +
         netSetting("turn_relay_ip", "Relay IP", "") + ", " + netSetting("ice_servers", "External STUN servers", "null") + ", " +
         netSetting("save_keep_recent", "Keep newest versions", "20") + ", " + netSetting("save_keep_daily", "Keep daily versions", "7") + ", " +
         netSetting("save_keep_weekly", "Keep weekly versions", "4") + ", " + netSetting("network_sharing", "Network sharing", "true") + ", " +
         netSetting("import_dir", "Import folder", "/srv/uzume/import") + "], \"pending_restart\": " + p +
         ", \"issues\": [], \"can_restart\": true, \"turn\": {\"public_host\": \"server.example.com\", \"relay_ip\": \"203.0.113.10\", \"fixed\": false, "
         "\"port\": 3478, \"relay_min\": 49160, \"relay_max\": 49199, \"allocations\": 2, \"forwards\": []}, \"cloudflare_configured\": false, "
         "\"external_turn\": {\"source\": \"flags\", \"active\": true, \"servers\": []}}";
}

QByteArray updatesBody(bool available = true) {
  return QByteArray("{\"current_version\": \"0.17.0\", \"channel\": \"stable\", \"channel_is_set\": true, \"default_channel\": \"stable\", "
                    "\"compiled_channel\": \"stable\", \"auto\": true, \"packaged\": true, \"request_pending\": false, \"active_sessions\": 0, "
                    "\"in_container\": false") +
         (available ? ", \"available\": {\"version\": \"0.17.1\", \"channel\": \"stable\", \"published_at\": \"2026-10-20T10:00:00Z\", \"size\": 52428800, "
                      "\"breaking\": false, \"breaking_count\": 0, \"skipped\": false}"
                    : "") +
         "}";
}

QByteArray settingsBody(const QByteArray& name = "Home server", bool uploads = true, const char* port = "8443", const char* pending = "",
                        bool available = true) {
  return "{\"hub_name\": \"" + name + "\", \"allow_user_uploads\": " + (uploads ? "true" : "false") + ", \"network\": " + networkBody(port, pending) +
         ", \"security\": {\"tls\": true, \"trusted_proxy\": [], \"fingerprint\": \"AA:BB:CC\", \"cert_source\": \"self\", "
         "\"cert_not_after\": \"2027-01-01T00:00:00Z\", \"can_renew_cert\": true}, \"updates\": " + updatesBody(available) + "}";
}

QByteArray backupStatus(const QByteArray& extra = "", const char* mode = "daily", int keep = 7) {
  return "{\"settings\": {\"mode\": \"" + QByteArray(mode) + "\", \"weekday\": 0, \"time\": \"03:00\", \"keep\": " + QByteArray::number(keep) +
         ", \"include_roms\": false, \"folder\": \"/mnt/backup/uzume\"}, "
         "\"folder_set\": true, \"disk_total\": 1000000000000, \"disk_free\": 400000000000, \"can_restore\": true" +
         (extra.isEmpty() ? QByteArray() : ", " + extra) + "}";
}
const char* kRunning = R"JSON( "running": {"kind": "backup", "phase": "saves and snapshots", "done_bytes": 1000, "total_bytes": 4000, "started": "2026-10-22T10:00:00Z"})JSON";
const char* kDone = R"JSON( "last": {"ok": true, "started": "2026-10-22T10:00:00Z", "finished": "2026-10-22T10:01:00Z", "name": "uzume-server-home-20261022T100000Z.tar", "size": 4000},
  "last_ok": {"ok": true, "started": "2026-10-22T10:00:00Z", "finished": "2026-10-22T10:01:00Z", "name": "uzume-server-home-20261022T100000Z.tar", "size": 4000})JSON";
const char* kOldBackup = R"JSON({"name": "uzume-server-home-20261014T010000Z.tar", "created": "2026-10-14T01:00:00Z", "size": 3000, "safety": false,
  "includes_roms": false, "restored": false, "contents": "Database · saves · plugins", "hub_version": "0.17.0"})JSON";
const char* kNewBackup = R"JSON({"name": "uzume-server-home-20261022T100000Z.tar", "created": "2026-10-22T10:00:00Z", "size": 4000, "safety": false,
  "includes_roms": false, "restored": false})JSON";

const char* kVersions = R"JSON({"current": "0.17.0", "current_schema": 12, "can_rollback": true, "in_container": false, "backup_folder": "/mnt/backup/uzume",
  "connected_players": 2, "versions": [
    {"version": "0.16.4", "channel": "stable", "published_at": "2026-09-01T10:00:00Z", "schema_version": 12, "data": "keep"},
    {"version": "0.16.3", "channel": "stable", "published_at": "2026-08-01T10:00:00Z", "schema_version": 11, "data": "restore", "lost_since": "2026-10-01T10:00:00Z"}]})JSON";
}  // namespace

class AdminSettingsUiTest : public QObject {
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
    hub.adminRoutes[get(QStringLiteral("/settings"))] = {200, settingsBody()};
    hub.adminRoutes[get(QStringLiteral("/accounts/settings"))] = {200, R"JSON({"require_passwords": false})JSON"};
    hub.adminRoutes[get(QStringLiteral("/backups/status"))] = {200, backupStatus()};
    hub.adminRoutes[get(QStringLiteral("/backups"))] = {200, QByteArray("{\"backups\": [") + kOldBackup + "]}"};
    hub.adminRoutes[get(QStringLiteral("/updates/versions"))] = {200, QByteArray(kVersions)};
    hub.adminRoutes[get(QStringLiteral("/metrics"))] = {200, R"JSON({"enabled": false, "token_set": false})JSON"};
  }
  static QString text(QQuickItem* it) { return it ? it->property("text").toString() : QString(); }
  static QJsonObject bodyOf(const FakeHub& hub, const QString& key) { return QJsonDocument::fromJson(hub.adminBodies.value(key)).object(); }
  static int calls(const FakeHub& hub, const QString& key) { return hub.adminCalls.value(key); }
  static bool shown(const Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr && it->isVisible();
  }
  // Keys-only walk helpers: the row (or control) that has the focus, found by the nearest objectName up the parent chain.
  static QQuickItem* focused(Harness& h) { return h.window->activeFocusItem(); }
  static QString focusName(Harness& h) {
    for (QQuickItem* i = focused(h); i != nullptr; i = i->parentItem()) {
      if (!i->objectName().isEmpty()) return i->objectName();
    }
    return {};
  }
  static bool focusIsRow(Harness& h) { return focused(h) != nullptr && focused(h)->property("colnavRow").toBool(); }
  static bool focusInside(Harness& h, const char* name) {
    QQuickItem* c = h.item(name);
    return c != nullptr && focused(h) != nullptr && (c == focused(h) || c->isAncestorOf(focused(h)));
  }
  static void key(Harness& h, Qt::Key k) { QTest::keyClick(h.window, k); QTest::qWait(30); }
  // Tab until `pred` holds (at most `max` presses).
  template <typename F>
  static bool tabUntil(Harness& h, F pred, int max = 80) {
    for (int i = 0; i < max; ++i) {
      if (pred()) return true;
      key(h, Qt::Key_Tab);
    }
    return pred();
  }
  // Esc from the column leaves it for the AppNav.
  static void expectNavFocus(Harness& h) { QTRY_VERIFY(focusInside(h, "appNav")); }

  // Settings, scope Server, the given page.
  static void openPage(Harness& h, const char* nav, const char* pageName) {
    h.window->resize(1400, 1800);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showSettings();
    QTRY_VERIFY(h.item("scopeServer") != nullptr);
    QVERIFY(h.clickWhenReady("scopeServer"));
    QTRY_VERIFY(shown(h, "serverSettingsPage"));
    if (nav != nullptr) QVERIFY(h.clickWhenReady(nav));
    QTRY_VERIFY(shown(h, pageName));
  }
  static void editAndSave(Harness& h, const QString& value) {
    QTRY_VERIFY(shown(h, "settingsEditDialog"));
    h.item("settingsEditField")->setProperty("text", value);
    QVERIFY(h.clickWhenReady("settingsEditSave"));
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void serverScopeListsTheSevenPages() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, nullptr, "settingsGeneral");
    QVERIFY(h.item("settingsServerEmpty") == nullptr);
    for (const char* n : {"serverSettingsNav_general", "serverSettingsNav_network", "serverSettingsNav_signin", "serverSettingsNav_updates",
                          "serverSettingsNav_backups", "serverSettingsNav_storage", "serverSettingsNav_diagnostics"}) {
      QVERIFY2(h.item(n) != nullptr, n);
    }
    QCOMPARE(text(h.item("generalName")), QStringLiteral("Home server"));
    QCOMPARE(text(h.item("generalImport")).left(17), QStringLiteral("/srv/uzume/import"));
    QTRY_COMPARE(text(h.item("serverSettingsNavStatus_updates")), QStringLiteral("0.17.0"));
    QTRY_COMPARE(text(h.item("serverSettingsNavStatus_backups")), QStringLiteral("03:00"));
    // This PC again: the pane goes away.
    QVERIFY(h.click("scopePc"));
    QTRY_VERIFY(!shown(h, "serverSettingsPage"));
  }

  void generalRenameAndUploadsPatch() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString patch = QStringLiteral("PATCH ") + kApi + QStringLiteral("/settings");
    hub.adminRoutes[patch] = {200, settingsBody("Den", true)};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, nullptr, "settingsGeneral");
    QVERIFY(h.clickWhenReady("generalRename"));
    editAndSave(h, QStringLiteral("Den"));
    QTRY_COMPARE(calls(hub, patch), 1);
    QCOMPARE(bodyOf(hub, patch).value("name").toString(), QStringLiteral("Den"));
    QTRY_COMPARE(text(h.item("generalName")), QStringLiteral("Den"));
    QTRY_COMPARE(text(h.item("serverSettingsNote")), QStringLiteral("Renamed the server to Den."));
    QTRY_VERIFY(!shown(h, "settingsEditDialog"));
    hub.adminRoutes[patch] = {200, settingsBody("Den", false)};
    QVERIFY(h.click("generalUploads"));
    QTRY_COMPARE(calls(hub, patch), 2);
    QCOMPARE(bodyOf(hub, patch).value("allow_user_uploads").toBool(), false);
    // A refusal shows the message of the server in the dialog and keeps it open.
    hub.adminRoutes[patch] = {400, R"JSON({"error": {"code": "bad_request", "message": "Name is too long"}})JSON"};
    QVERIFY(h.clickWhenReady("generalRename"));
    editAndSave(h, QStringLiteral("x"));
    QTRY_VERIFY(shown(h, "dialogError"));
    QCOMPARE(text(h.item("dialogError")), QStringLiteral("Name is too long"));
    QVERIFY(shown(h, "settingsEditDialog"));
  }

  void networkChangeShowsRestartAndRestartPosts() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString route = put(QStringLiteral("/settings/network/listen_port"));
    hub.adminRoutes[route] = {200, networkBody("9000", "Listen port")};
    hub.adminRoutes[post(QStringLiteral("/settings/network/restart"))] = {202, R"JSON({"port_changed": true, "url": "server.example.com:9000"})JSON"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_network", "settingsNetwork");
    QVERIFY(!shown(h, "serverSettingsRestart"));
    QCOMPARE(text(h.item("networkDone")), QStringLiteral("Prepared for the internet: public address and relay are set."));
    QCOMPARE(text(h.item("networkPorts")), QStringLiteral("8443 TCP · 3478 UDP+TCP · 49160-49199 UDP"));
    QVERIFY(h.clickWhenReady("networkChange_listen"));
    editAndSave(h, QStringLiteral("9000"));
    QTRY_COMPARE(calls(hub, route), 1);
    QCOMPARE(bodyOf(hub, route).value("value").toString(), QStringLiteral("9000"));
    QTRY_VERIFY(shown(h, "serverSettingsRestart"));
    QVERIFY(h.item("serverSettingsRestart")->findChild<QQuickItem*>() != nullptr);
    QVERIFY(h.clickWhenReady("serverSettingsRestartButton"));
    QTRY_COMPARE(calls(hub, post(QStringLiteral("/settings/network/restart"))), 1);
    QTRY_VERIFY(text(h.item("serverSettingsNote")).contains(QStringLiteral("server.example.com:9000")));
  }

  void sharingAsksToConfirm() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString route = put(QStringLiteral("/settings/network/network_sharing"));
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/settings/network/network_sharing")};
    hub.adminRoutes[route] = {200, networkBody()};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_network", "settingsNetwork");
    QVERIFY(h.click("networkSharing"));
    QTRY_VERIFY(h.controller->admin()->confirmNeeded());
    QCOMPARE(calls(hub, route), 0);
    QTRY_VERIFY(shown(h, "confirmPassword"));
    h.item("confirmPassword")->setProperty("text", hub.accountPassword);
    QVERIFY(h.click("confirmSubmit"));
    QTRY_COMPARE(calls(hub, route), 1);
    QCOMPARE(bodyOf(hub, route).value("value").toString(), QStringLiteral("0"));
  }

  void signInSavesThePolicyAfterAChange() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString route = put(QStringLiteral("/accounts/settings"));
    hub.adminRoutes[route] = {200, R"JSON({"require_passwords": true})JSON"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_signin", "settingsSignIn");
    QVERIFY(!shown(h, "signinWarning"));
    QVERIFY(h.item("signinPasswords") != nullptr);
    // Pick "Required for everyone" in the select: the row is marked and Save / Cancel appear; Cancel drops the draft.
    h.item("settingsSignIn")->setProperty("draft", true);
    QTRY_VERIFY(shown(h, "signinWarning"));
    QTRY_VERIFY(shown(h, "changedDot_passwords"));
    QVERIFY(h.click("signinCancel"));
    QTRY_VERIFY(!shown(h, "signinWarning"));
    QCOMPARE(calls(hub, route), 0);
    h.item("settingsSignIn")->setProperty("draft", true);
    QVERIFY(h.clickWhenReady("signinSave"));
    QTRY_COMPARE(calls(hub, route), 1);
    QCOMPARE(bodyOf(hub, route).value("require_passwords").toBool(), true);
    QTRY_VERIFY(!shown(h, "signinWarning"));
    QTRY_COMPARE(text(h.item("serverSettingsNavStatus_signin")), QStringLiteral("Password"));
  }

  void updatesInstallAndRollback() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString install = post(QStringLiteral("/updates/install"));
    const QString rollback = post(QStringLiteral("/updates/rollback"));
    hub.adminRoutes[install] = {202, R"JSON({"version": "0.17.1"})JSON"};
    hub.adminRoutes[rollback] = {202, R"JSON({"version": "0.16.3", "data": "restore", "safety_backup": "x.tar", "safety_folder": "/mnt/backup/uzume"})JSON"};
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/updates/rollback")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_updates", "settingsUpdates");
    QCOMPARE(text(h.item("updatesDone")), QStringLiteral("Server 0.17.1 is ready."));
    QVERIFY(h.clickWhenReady("updatesNow"));
    QTRY_COMPARE(calls(hub, install), 1);
    QCOMPARE(bodyOf(hub, install).value("confirm_breaking").toBool(), false);
    // Earlier versions: the second one restores the database, so the dialog says so; the server asks to confirm.
    QTRY_VERIFY(h.item("versionsTableRow_1") != nullptr);
    QVERIFY(h.click("versionsTableRow_1"));
    QTRY_VERIFY(shown(h, "versionsDetails"));
    QVERIFY(h.clickWhenReady("versionsGoBack"));
    QTRY_VERIFY(shown(h, "rollbackDialog"));
    QCOMPARE(calls(hub, rollback), 0);
    QVERIFY(h.click("rollbackConfirm"));
    QTRY_VERIFY(h.controller->admin()->confirmNeeded());
    QTRY_VERIFY(shown(h, "confirmPassword"));
    h.item("confirmPassword")->setProperty("text", hub.accountPassword);
    QVERIFY(h.click("confirmSubmit"));
    QTRY_COMPARE(calls(hub, rollback), 1);
    QCOMPARE(bodyOf(hub, rollback).value("version").toString(), QStringLiteral("0.16.3"));
    QCOMPARE(bodyOf(hub, rollback).value("confirm_data_loss").toBool(), true);
    QTRY_VERIFY(!shown(h, "rollbackDialog"));
  }

  void backupRunPollsUntilDone() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString start = post(QStringLiteral("/backups"));
    hub.adminRoutes[start] = {202, ""};
    hub.adminHook = [&hub, start](const QString& key) {
      if (key == start) hub.adminRoutes[get(QStringLiteral("/backups/status"))] = {200, backupStatus(kRunning)};
    };
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_backups", "settingsBackups");
    QTRY_VERIFY(h.item("backupsTableRow_0") != nullptr);
    QVERIFY(h.item("backupsTableRow_1") == nullptr);
    QVERIFY(h.clickWhenReady("backupNow"));
    QTRY_COMPARE(calls(hub, start), 1);
    QTRY_VERIFY(shown(h, "backupProgress"));
    QVERIFY(!h.item("backupNow")->isEnabled());
    // Done: the poll sees no running job any more and reads the list again.
    hub.adminRoutes[get(QStringLiteral("/backups/status"))] = {200, backupStatus(kDone)};
    hub.adminRoutes[get(QStringLiteral("/backups"))] = {200, QByteArray("{\"backups\": [") + kNewBackup + "," + kOldBackup + "]}"};
    QTRY_VERIFY_WITH_TIMEOUT(!shown(h, "backupProgress"), 8000);
    QTRY_VERIFY(h.item("backupsTableRow_1") != nullptr);
    QTRY_COMPARE(text(h.item("backupNote")).left(12), QStringLiteral("Backup done:"));
    QTRY_VERIFY(h.item("backupNow")->isEnabled());
  }

  void backupScheduleAndFolderSave() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString route = put(QStringLiteral("/backups/settings"));
    hub.adminRoutes[route] = {200, R"JSON({"mode": "weekly", "weekday": 0, "time": "03:00", "keep": 7, "include_roms": false, "folder": "/mnt/backup/uzume"})JSON"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_backups", "settingsBackups");
    hub.adminRoutes[get(QStringLiteral("/backups/status"))] = {200, backupStatus("", "weekly")};
    QVERIFY(h.clickWhenReady("backupModeWeekly"));
    QTRY_COMPARE(calls(hub, route), 1);
    QCOMPARE(bodyOf(hub, route).value("mode").toString(), QStringLiteral("weekly"));
    QTRY_VERIFY(shown(h, "backupDayRow"));
    hub.adminRoutes[route] = {200, R"JSON({"mode": "weekly", "weekday": 0, "time": "03:00", "keep": 14, "include_roms": false, "folder": "/mnt/backup/uzume"})JSON"};
    hub.adminRoutes[get(QStringLiteral("/backups/status"))] = {200, backupStatus("", "weekly", 14)};
    QVERIFY(h.clickWhenReady("backupKeepChange"));  // after the Day row moved it down: click() waits for the layout polish
    QTRY_VERIFY(shown(h, "settingsEditDialog"));
    editAndSave(h, QStringLiteral("14"));
    QTRY_COMPARE(calls(hub, route), 2);
    QCOMPARE(bodyOf(hub, route).value("keep").toInt(), 14);
  }

  void restoreGoesThroughConfirm() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString name = QStringLiteral("uzume-server-home-20261014T010000Z.tar");
    const QString restore = post(QStringLiteral("/backups/") + name + QStringLiteral("/restore"));
    hub.adminRoutes[restore] = {202, ""};
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/backups/") + name + QStringLiteral("/restore")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_backups", "settingsBackups");
    QTRY_VERIFY(h.item("backupsTableRow_0") != nullptr);
    QVERIFY(h.click("backupsTableRow_0"));
    QVERIFY(h.clickWhenReady("backupRestore"));
    QTRY_VERIFY(shown(h, "restoreDialog"));
    QCOMPARE(calls(hub, restore), 0);
    QVERIFY(h.click("restoreConfirm"));
    QTRY_VERIFY(h.controller->admin()->confirmNeeded());
    QCOMPARE(calls(hub, restore), 0);
    QTRY_VERIFY(shown(h, "confirmPassword"));
    h.item("confirmPassword")->setProperty("text", hub.accountPassword);
    QVERIFY(h.click("confirmSubmit"));
    QTRY_COMPARE(calls(hub, restore), 1);
    QTRY_VERIFY(!shown(h, "restoreDialog"));
    // Delete asks first and then deletes.
    const QString del1 = del(QStringLiteral("/backups/") + name);
    hub.adminRoutes[del1] = {204, ""};
    QVERIFY(h.clickWhenReady("backupDelete"));
    QTRY_VERIFY(shown(h, "deleteBackupDialog"));
    QCOMPARE(calls(hub, del1), 0);
    QVERIFY(h.click("deleteBackupConfirm"));
    QTRY_COMPARE(calls(hub, del1), 1);
  }

  void backupDownloadWritesAFile() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString name = QStringLiteral("uzume-server-home-20261014T010000Z.tar");
    hub.adminRoutes[get(QStringLiteral("/backups/") + name + QStringLiteral("/download"))] = {200, "TARDATA"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_backups", "settingsBackups");
    QTRY_VERIFY(h.item("backupsTableRow_0") != nullptr);
    QVERIFY(h.click("backupsTableRow_0"));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("b.tar"));
    QVERIFY(QMetaObject::invokeMethod(h.item("settingsBackups"), "saveBackup", Q_ARG(QString, target)));
    QTRY_VERIFY(QFile::exists(target));
    QFile f(target);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("TARDATA"));
    QTRY_VERIFY(text(h.item("backupNote")).startsWith(QStringLiteral("Saved the backup to")));
  }

  void metricsTokenIsShownOnce() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString route = put(QStringLiteral("/metrics"));
    hub.adminRoutes[route] = {200, R"JSON({"enabled": true, "token_set": true, "token": "fbm_placeholdertoken"})JSON"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_diagnostics", "settingsDiagnostics");
    QVERIFY(!shown(h, "metricsToken"));
    QTRY_VERIFY(h.item("metricsToggle")->isEnabled());
    QVERIFY(h.click("metricsToggle"));
    QTRY_COMPARE(calls(hub, route), 1);
    QCOMPARE(bodyOf(hub, route).value("enabled").toBool(), true);
    QTRY_VERIFY(shown(h, "metricsToken"));
    QCOMPARE(text(h.item("metricsToken")), QStringLiteral("fbm_placeholdertoken"));
    // Leaving the page and coming back: the server never returns the token again.
    hub.adminRoutes[get(QStringLiteral("/metrics"))] = {200, R"JSON({"enabled": true, "token_set": true})JSON"};
    QVERIFY(h.click("serverSettingsNav_general"));
    QTRY_VERIFY(shown(h, "settingsGeneral"));
    QVERIFY(h.click("serverSettingsNav_diagnostics"));
    QTRY_VERIFY(shown(h, "settingsDiagnostics"));
    QTRY_VERIFY(!shown(h, "metricsToken"));
    QTRY_VERIFY(shown(h, "metricsNew"));
    // A new token asks first.
    const QString token = post(QStringLiteral("/metrics/token"));
    hub.adminRoutes[token] = {200, R"JSON({"token": "fbm_secondtoken"})JSON"};
    QVERIFY(h.click("metricsNew"));
    QTRY_VERIFY(shown(h, "metricsNewDialog"));
    QCOMPARE(calls(hub, token), 0);
    QVERIFY(h.click("metricsNewConfirm"));
    QTRY_COMPARE(calls(hub, token), 1);
    QTRY_COMPARE(text(h.item("metricsToken")), QStringLiteral("fbm_secondtoken"));
  }

  void diagnosticsBundleWritesAFile() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString route = post(QStringLiteral("/diagnostics/bundle"));
    hub.adminRoutes[route] = {200, "PK-zip-bytes"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_diagnostics", "settingsDiagnostics");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("diag.zip"));
    QVERIFY(QMetaObject::invokeMethod(h.item("settingsDiagnostics"), "saveBundle", Q_ARG(QString, target)));
    QTRY_COMPARE(calls(hub, route), 1);
    QTRY_VERIFY(QFile::exists(target));
    QFile f(target);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("PK-zip-bytes"));
    QTRY_VERIFY(text(h.item("bundleNote")).startsWith(QStringLiteral("Saved the server logs to")));
  }

  void storageShowsTheDiskFromStatus() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_storage", "settingsStorage");
    QTRY_VERIFY(shown(h, "storageBar"));
    QTRY_VERIFY(!text(h.item("storageUsed")).isEmpty());
    QTRY_VERIFY(shown(h, "storageWarning"));   // the sample status has a backup disk at 92 %
    QVERIFY(h.item("storageRecentRow") != nullptr);
    // An old server without GET /admin/storage (404): the disk-only view, no clean-up section.
    QVERIFY(!shown(h, "storageCleanupLabel"));
    QVERIFY(!shown(h, "storageShotsLabel"));
  }

  // GET /admin/storage (API 1.38): the five bar kinds, the legend values, the table and the clean-up rows that ask first.
  void storageShowsTheSplitAndCleansUp() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QString usage = get(QStringLiteral("/storage"));
    const QString cleanup = post(QStringLiteral("/storage/cleanup"));
    hub.adminRoutes[usage] = {200, R"JSON({"measured_at": "2026-10-23T10:00:00Z",
      "disk": {"total_bytes": 1000000000000, "free_bytes": 400000000000},
      "backup_disk": {"folder": "/mnt/backup/uzume", "total_bytes": 1000000000000, "free_bytes": 80000000000},
      "kinds": [{"id": "games", "bytes": 214748364800, "count": 120}, {"id": "saves", "bytes": 1073741824, "count": 900},
                {"id": "snapshots", "bytes": 1073741824, "count": 400}, {"id": "states", "bytes": 0, "count": 0},
                {"id": "screenshots", "bytes": 52428800, "count": 30}, {"id": "cores", "bytes": 536870912, "count": 4},
                {"id": "firmware", "bytes": 536870912, "count": 3}, {"id": "backups", "bytes": 5368709120, "count": 7}],
      "screenshots": [{"user_id": "u1", "user_name": "Max", "game_id": "g1", "game_name": "Demo Game", "bytes": 31457280, "count": 20},
                      {"user_id": "u2", "user_name": "Lena", "game_id": "g1", "game_name": "Demo Game", "bytes": 20971520, "count": 10}],
      "cleanup": [{"id": "old_snapshots", "bytes": 536870912, "count": 120}, {"id": "upload_leftovers", "bytes": 0, "count": 0},
                  {"id": "unused_cores", "bytes": 268435456, "count": 2}]})JSON"};
    hub.adminRoutes[cleanup] = {200, R"JSON({"id": "old_snapshots", "freed_bytes": 536870912, "removed": 120})JSON"};
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/storage/cleanup")};  // like the admin save-version deletes
    hub.adminHook = [&hub, cleanup, usage](const QString& key) {
      if (key == cleanup) {
        hub.adminRoutes[usage] = {200, R"JSON({"measured_at": "2026-10-23T10:05:00Z", "disk": {"total_bytes": 1000000000000, "free_bytes": 400000000000},
          "kinds": [{"id": "games", "bytes": 214748364800, "count": 120}], "screenshots": [],
          "cleanup": [{"id": "unused_cores", "bytes": 268435456, "count": 2}]})JSON"};
      }
    };
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_storage", "settingsStorage");
    QTRY_VERIFY(shown(h, "storageBar"));
    QTRY_COMPARE(text(h.item("storageKind_games")), QStringLiteral("200 GB"));
    QCOMPARE(text(h.item("storageKind_saves")), QStringLiteral("2 GB"));       // saves + snapshots
    QCOMPARE(text(h.item("storageKind_states")), QStringLiteral("1 KB"));
    QCOMPARE(text(h.item("storageKind_screenshots")), QStringLiteral("50 MB"));
    QCOMPARE(text(h.item("storageKind_cores")), QStringLiteral("1 GB"));       // cores + firmware
    QCOMPARE(text(h.item("storageKind_backups")), QStringLiteral("5 GB"));
    QVERIFY(!text(h.item("storageFree")).isEmpty());
    QVERIFY(!text(h.item("storageBackupDisk")).isEmpty());
    QTRY_VERIFY(shown(h, "storageWarning"));  // backup disk 92 % full
    QVERIFY(shown(h, "storageShotsLabel"));
    QTRY_VERIFY(h.item("storageShotsTableRow_0") != nullptr);
    QVERIFY(h.item("storageShotsTableRow_1") != nullptr);
    QVERIFY(h.item("storageShotsTableRow_2") == nullptr);
    // Clean-up rows: the empty suggestion is hidden, old save versions say what stays.
    QTRY_VERIFY(shown(h, "storageCleanup_old_snapshots"));
    QVERIFY(shown(h, "storageCleanup_unused_cores"));
    QVERIFY(h.item("storageCleanup_upload_leftovers") == nullptr || !shown(h, "storageCleanup_upload_leftovers"));
    QVERIFY(h.clickWhenReady("storageCleanupButton_old_snapshots"));
    QTRY_VERIFY(shown(h, "storageCleanupConfirm_old_snapshots"));
    QVERIFY(text(h.item("storageCleanupConfirmText_old_snapshots")).contains(QStringLiteral("Manual snapshots")));
    QCOMPARE(calls(hub, cleanup), 0);
    QVERIFY(h.clickWhenReady("storageCleanupCancel_old_snapshots"));
    QCOMPARE(calls(hub, cleanup), 0);
    QVERIFY(h.clickWhenReady("storageCleanupButton_old_snapshots"));
    QVERIFY(h.clickWhenReady("storageCleanupConfirmButton_old_snapshots"));
    // The server asks "Confirm it's you": the request is parked, the global dialog asks, then the same request goes through.
    QTRY_VERIFY(h.controller->admin()->confirmNeeded());
    QTRY_VERIFY(shown(h, "confirmPassword"));
    h.item("confirmPassword")->setProperty("text", hub.accountPassword);
    QVERIFY(h.click("confirmSubmit"));
    QTRY_COMPARE(calls(hub, cleanup), 1);
    QCOMPARE(bodyOf(hub, cleanup).value("id").toString(), QStringLiteral("old_snapshots"));
    QTRY_VERIFY(text(h.item("storageCleanupResult")).startsWith(QStringLiteral("Freed 512 MB")));
    QTRY_VERIFY(h.item("storageCleanup_old_snapshots") == nullptr || !shown(h, "storageCleanup_old_snapshots"));  // read again
    QVERIFY(shown(h, "storageCleanup_unused_cores"));
  }

  void storageWithNothingToCleanSaysSo() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.adminRoutes[get(QStringLiteral("/storage"))] = {200, R"JSON({"measured_at": "2026-10-23T10:00:00Z", "disk": {"total_bytes": 1000, "free_bytes": 500},
      "kinds": [], "screenshots": [], "cleanup": [{"id": "old_snapshots", "bytes": 0, "count": 0}]})JSON"};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openPage(h, "serverSettingsNav_storage", "settingsStorage");
    QTRY_VERIFY(shown(h, "storageNothingToClean"));
    QVERIFY(!shown(h, "storageShotsLabel"));
  }

  // Keys alone (Tab / arrows / Enter / Esc): Settings (This PC and Server), Emulation, Controllers and an admin page reach the column
  // rows and the content controls, and Esc steps back: content -> column -> AppNav -> Library.
  void keysAloneWalkTheMainPages() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1400, 1000);
    QTRY_VERIFY(h.controller->admin()->available());

    // Settings: the section column.
    h.controller->showSettings();
    QTRY_VERIFY(h.item("section_account") != nullptr);
    QTRY_VERIFY(h.item("settingsColumnNav") != nullptr);
    QVERIFY(tabUntil(h, [&] { return focusIsRow(h); }));
    QVERIFY(focusName(h).startsWith(QStringLiteral("section_")));
    const QString first = focusName(h);
    key(h, Qt::Key_Down);
    QVERIFY(focusIsRow(h));
    QVERIFY(focusName(h) != first);
    key(h, Qt::Key_Up);
    QCOMPARE(focusName(h), first);
    key(h, Qt::Key_Down);
    key(h, Qt::Key_Return);   // opens the section: the row is selected
    QVERIFY(h.item(focusName(h).toLatin1().constData())->property("selected").toBool());
    key(h, Qt::Key_Right);    // into the content
    QTRY_VERIFY(focusInside(h, "settingsScroll"));
    QVERIFY(!focusIsRow(h));
    key(h, Qt::Key_Escape);   // content -> column
    QTRY_VERIFY(focusIsRow(h));
    QVERIFY(h.item(focusName(h).toLatin1().constData())->property("selected").toBool());
    key(h, Qt::Key_Escape);   // column -> AppNav
    expectNavFocus(h);
    QCOMPARE(h.controller->screen(), QStringLiteral("settings"));

    // Settings for Server: the server pages are rows of the same column.
    QVERIFY(h.clickWhenReady("scopeServer"));
    QTRY_VERIFY(h.item("serverSettingsNav_network") != nullptr);
    QVERIFY(tabUntil(h, [&] { return focusIsRow(h); }));
    QVERIFY(focusName(h).startsWith(QStringLiteral("serverSettingsNav_")));
    for (int i = 0; i < 7 && focusName(h) != QStringLiteral("serverSettingsNav_network"); ++i) key(h, Qt::Key_Down);
    QCOMPARE(focusName(h), QStringLiteral("serverSettingsNav_network"));
    key(h, Qt::Key_Return);
    QTRY_VERIFY(shown(h, "settingsNetwork"));
    key(h, Qt::Key_Right);
    QTRY_VERIFY(!focusIsRow(h) && focusInside(h, "settingsScroll"));
    key(h, Qt::Key_Escape);
    QTRY_VERIFY(focusIsRow(h));

    // Emulation.
    h.controller->showEmulation();
    QTRY_VERIFY(h.item("defaultsCard") != nullptr);
    QVERIFY(tabUntil(h, [&] { return focusIsRow(h); }));
    key(h, Qt::Key_Down);
    QVERIFY(focusIsRow(h));
    QVERIFY(focusName(h).startsWith(QStringLiteral("systemCard_")));
    key(h, Qt::Key_Return);
    QVERIFY(h.item(focusName(h).toLatin1().constData())->property("selected").toBool());
    key(h, Qt::Key_Right);
    QTRY_VERIFY(focusInside(h, "emulationScroll") && !focusIsRow(h));
    key(h, Qt::Key_Escape);
    QTRY_VERIFY(focusIsRow(h));
    key(h, Qt::Key_Escape);
    expectNavFocus(h);

    // Controllers: the device column, then a binding field takes Enter.
    h.controller->showControllers();
    QTRY_VERIFY(h.item("controllersColumnNav") != nullptr);
    QVERIFY(tabUntil(h, [&] { return focusIsRow(h); }));
    QVERIFY(focusName(h).startsWith(QStringLiteral("device_")));
    key(h, Qt::Key_Right);
    QTRY_VERIFY(!focusIsRow(h));
    QVERIFY(tabUntil(h, [&] { return focusName(h).startsWith(QStringLiteral("mapField_")); }, 120));
    QVERIFY(focused(h)->property("capturing").isValid());
    key(h, Qt::Key_Return);   // starts capture (a built-in profile is duplicated first)
    key(h, Qt::Key_Escape);   // cancels the capture
    h.controller->showControllers();
    QTRY_VERIFY(h.item("controllersColumnNav") != nullptr);

    // An admin page: Esc goes to the AppNav, and from there to the Library.
    h.controller->showServerPage(QStringLiteral("status"));
    QTRY_VERIFY(h.item("serverScreen") != nullptr && h.item("serverScreen")->isVisible());
    QTRY_VERIFY(h.item("appNav") != nullptr);
    // Tab walks the AppNav first, then reaches a control of the page (Esc there moves to the AppNav).
    QVERIFY(tabUntil(h, [&] { return focusInside(h, "serverScreen") && !focusInside(h, "appNav"); }, 120));
    key(h, Qt::Key_Escape);
    expectNavFocus(h);
    key(h, Qt::Key_Escape);
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("library"));
  }

  // The hint row shows while the last input was a pad, takes no space otherwise, and a key press hides it again.
  void hintRowFollowsTheLastInput() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->showSettings();
    QTRY_VERIFY(h.item("pageHints") != nullptr);
    QVERIFY(!h.item("pageHints")->isVisible());
    QCOMPARE(h.item("pageHints")->height(), 0.0);
    h.controller->controllers()->notePadInput();
    QTRY_VERIFY(h.item("pageHints")->isVisible());
    QCOMPARE(h.item("pageHints")->height(), 44.0);
    key(h, Qt::Key_Shift);
    QTRY_VERIFY(!h.item("pageHints")->isVisible());
  }

  // The pad as UI input: the same path the SDL poll uses (ControllersController::feedPadNav). D-pad moves the focus (with repeat), A
  // opens, B steps back, the gate (a running game) silences it, and every translated input shows the hint row.
  void padInputNavigatesTheShell() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1400, 1000);
    auto* ctl = h.controller->controllers();
    const auto tok = [](std::initializer_list<const char*> names) {
      QSet<QString> s;
      for (const char* n : names) s.insert(padToken(QString::fromLatin1(n)));
      return s;
    };
    qint64 t = 1000;
    const auto tap = [&](std::initializer_list<const char*> names) {
      ctl->feedPadNav(tok(names), t += 20);
      ctl->feedPadNav({}, t += 20);
      QTest::qWait(30);
    };
    h.controller->showSettings();
    QTRY_VERIFY(h.item("settingsColumnNav") != nullptr);
    QVERIFY(!h.item("pageHints")->isVisible());
    QVERIFY(tabUntil(h, [&] { return focusIsRow(h); }));
    const QString first = focusName(h);
    tap({"dpdown"});
    QVERIFY(h.item("pageHints")->isVisible());   // pad input shows the hint row
    QVERIFY(focusIsRow(h));
    QVERIFY(focusName(h) != first);
    tap({"dpup"});
    QCOMPARE(focusName(h), first);
    // A stick direction repeats after 400 ms, then every 80 ms.
    const QString start = focusName(h);
    ctl->feedPadNav(tok({"lefty+"}), 5000);            // moves one row
    const QString one = focusName(h);
    QVERIFY(one != start);
    ctl->feedPadNav(tok({"lefty+"}), 5300);            // too early
    QCOMPARE(focusName(h), one);
    ctl->feedPadNav(tok({"lefty+"}), 5410);            // first repeat
    QVERIFY(focusName(h) != one);
    const QString two = focusName(h);
    ctl->feedPadNav(tok({"lefty+"}), 5440);            // before the 80 ms repeat
    QCOMPARE(focusName(h), two);
    ctl->feedPadNav(tok({"lefty+"}), 5500);
    QVERIFY(focusName(h) != two);
    ctl->feedPadNav({}, 5520);
    // A opens the row, D-pad right enters the content, B steps back twice.
    tap({"a"});
    QVERIFY(h.item(focusName(h).toLatin1().constData())->property("selected").toBool());
    tap({"dpright"});
    QTRY_VERIFY(focusInside(h, "settingsScroll") && !focusIsRow(h));
    tap({"b"});
    QTRY_VERIFY(focusIsRow(h));
    tap({"b"});
    expectNavFocus(h);
    // The gate: while a game has the pad nothing is translated.
    ctl->setPadNavGate([] { return false; });
    const QString before = focusName(h);
    tap({"dpdown"});
    QCOMPARE(focusName(h), before);
    ctl->setPadNavGate({});
    // Y, L, R are commands for the page.
    QStringList got;
    QObject::connect(ctl, &ControllersController::padCommand, ctl, [&](const QString& c) { got << c; });
    tap({"y"});
    tap({"leftshoulder"});
    tap({"rightshoulder"});
    QCOMPARE(got, (QStringList{QStringLiteral("search"), QStringLiteral("filterPrev"), QStringLiteral("filterNext")}));
  }

  void withoutAdminV1ThePaneExplainsIt() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QVERIFY(!h.controller->admin()->available());
    QVERIFY(h.item("serverSettingsPage") == nullptr || !h.item("serverSettingsPage")->isVisible());
  }
};

UITEST_MAIN(AdminSettingsUiTest)
#include "admin_settings_ui_test.moc"
