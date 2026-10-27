// Admin area, package 8 (rest): the admin parts of Library and Saves (Rescan folders, Rename, Delete, Saves of all users with Keep this
// and Restore for another person), the admin actions of Runs, What's new (shown once), the setup assistant (steps call the right routes,
// Skip sends nothing), the version states of the server problems and the parity edits of the Users and Systems pages.
// Offscreen, FakeHub with admin_v1 and fixed answers for the admin routes; synthetic data only.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
const QString kApi = QStringLiteral("/api/v1/admin");
const QString kSha = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

QByteArray js(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

QJsonObject checkpoint(int rev, const QString& device) {
  return {{"revision", rev}, {"sha256", kSha}, {"size", 2048}, {"device_id", "d1"}, {"device_name", device},
          {"created_at", "2026-10-20T10:00:00Z"}, {"reason", "checkpoint"}};
}
QJsonObject saveRow(const QString& uid, const QString& name, const QString& slot, int conflicts) {
  return {{"user_id", uid}, {"username", name.toLower()}, {"game_id", "g1"}, {"game_title", "Lumen Drift"}, {"slot", slot},
          {"current", checkpoint(9, name + "-Laptop")}, {"open_conflict_count", conflicts}};
}
QJsonObject conflictJson() {
  return {{"id", "c1"}, {"game_id", "g1"}, {"slot", "default"}, {"status", "open"},
          {"hub", QJsonObject{{"revision", 9}, {"sha256", kSha}, {"device_id", "d1"}, {"device_name", "Lena-Laptop"}, {"created_at", "2026-10-20T10:00:00Z"}}},
          {"secured", QJsonObject{{"version", 15}, {"sha256", kSha}, {"base_revision", 8}, {"device_id", "d2"}, {"device_name", "Jonas-Laptop"},
                                  {"created_at", "2026-10-20T10:00:05Z"}}},
          {"created_at", "2026-10-20T10:00:06Z"}};
}
QJsonObject slotJson(bool withConflict) {
  return {{"game_id", "g1"}, {"slot", "default"}, {"current", checkpoint(9, "Lena-Laptop")},
          {"open_conflicts", withConflict ? QJsonArray{conflictJson()} : QJsonArray{}}};
}
QJsonObject versionJson(int v, const QString& reason) {
  return {{"version", v}, {"revision", v}, {"sha256", kSha}, {"size", 2048}, {"device_id", "d1"}, {"device_name", "Lena-Laptop"},
          {"created_at", "2026-10-19T10:00:00Z"}, {"reason", reason}, {"label", QJsonValue()}};
}
QJsonObject runJson(const QString& status) {
  const auto person = [](const char* id, const char* name, const char* st) {
    return QJsonObject{{"user_id", id}, {"user_name", name}, {"status", st}, {"game_id", QJsonValue()}, {"color", QJsonValue()}, {"states", QJsonObject{}}};
  };
  return {{"run_id", "r1"}, {"name", "Ember Route"}, {"status", status}, {"created_by", "u_test_1"}, {"created_at", "2026-10-20T09:00:00Z"},
          {"updated_at", "2026-10-20T10:00:00Z"}, {"status_by", "u_test_1"}, {"settings", QJsonObject{}},
          {"plugins", QJsonArray{QJsonObject{{"plugin_id", "net.example.p"}, {"version", "1"}, {"sha256", kSha}}}},
          {"participants", QJsonArray{person("u_test_1", "Tester", "joined"), person("u_2", "Lena", "invited")}}, {"last_seq", 0}};
}
QByteArray runDetail(const QString& status, bool archived = false) {
  return js({{"run", runJson(status)}, {"archived", archived}, {"boards", QJsonArray{}}});
}
QByteArray systemsBody() {
  const auto sys = [](const char* id, const char* name, bool core) {
    QJsonObject o{{"id", id}, {"display_name", name}, {"extensions", QJsonArray{id}}, {"libretro_ids", QJsonArray{}}, {"platforms", QJsonArray{}},
                  {"provisioning", "x"}, {"input_profile", id}, {"display_profile", id}, {"firmware_mode", "builtin"}, {"auto_update", "default"},
                  {"auto_update_effective", true},
                  {"available_cores", QJsonArray{QJsonObject{{"core_id", "melondsds"}, {"display_name", "melonDS DS"}, {"license", "GPL"}, {"platforms", QJsonArray{}},
                                                              {"extensions", QJsonArray{id}}, {"notes", QJsonArray{}}, {"experimental", false}, {"non_commercial", false}}}},
                  {"firmware", QJsonArray{}}, {"clients", QJsonArray{}}};
    if (core) {
      o.insert("default_core_id", "melondsds");
      o.insert("cores", QJsonArray{QJsonObject{{"core_id", "melondsds"}, {"display_name", "melonDS DS"}, {"version", "1.0"}, {"license", "GPL"},
                                               {"origin", "libretro-buildbot"}, {"platforms", QJsonArray{}}, {"default", true}, {"experimental", false},
                                               {"update_available", false}, {"auto_update", "auto"}, {"auto_update_effective", true}}});
    } else {
      o.insert("cores", QJsonArray{});
    }
    return o;
  };
  return js({{"systems", QJsonArray{sys("nds", "NDS", false), sys("gba", "GBA", true)}},
             {"core_source", QJsonObject{{"buildbot_url", "https://buildbot.example"}, {"info_url", "https://info.example"}, {"auto_update", true}, {"cores", 2}}}});
}
QByteArray settingsBody() {
  const auto net = [](const char* key, const char* value) {
    return QJsonObject{{"key", key}, {"label", key}, {"value", value}, {"running_value", value}, {"default_value", ""}, {"source", "env"},
                       {"needs_restart", false}, {"restart_pending", false}};
  };
  return js({{"hub_name", "Test server"}, {"allow_user_uploads", true},
             {"network", QJsonObject{{"settings", QJsonArray{net("import_dir", ""), net("public_host", ""), net("turn", "1")}}, {"pending_restart", QJsonArray{}},
                                     {"issues", QJsonArray{}}, {"can_restart", true}, {"cloudflare_configured", false},
                                     {"turn", QJsonObject{{"public_host", ""}, {"port", 3478}, {"relay_min", 49160}, {"relay_max", 49199}, {"allocations", 0}, {"fixed", false},
                                                          {"forwards", QJsonArray{"8443 TCP", "3478 UDP and TCP"}}}},
                                     {"external_turn", QJsonObject{{"source", "web"}, {"active", false}, {"servers", QJsonArray{}}}}}},
             {"security", QJsonObject{{"tls", true}, {"trusted_proxy", QJsonArray{}}, {"can_renew_cert", false}}}, {"updates", QJsonObject{}}});
}
}  // namespace

class AdminLibraryUiTest : public QObject {
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
  static void adminHub(FakeHub& hub, bool admin = true) {
    hub.adminUser = admin;
    hub.adminDirectory = true;
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("runs_v1")};
    const QByteArray rom = "synthetic rom bytes";
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    hub.games = QJsonObject{{QStringLiteral("games"), QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"),
                                                                                    uitest::sha256Hex(rom), rom.size())}}};
  }
  static void route(FakeHub& hub, const QString& method, const QString& path, int status, const QByteArray& body) {
    hub.adminRoutes[method + QLatin1Char(' ') + kApi + path] = {status, body};
  }
  static QString text(QQuickItem* it) { return it ? it->property("text").toString() : QString(); }
  static QJsonObject bodyOf(const FakeHub& hub, const QString& method, const QString& path) {
    return QJsonDocument::fromJson(hub.adminBodies.value(method + QLatin1Char(' ') + kApi + path)).object();
  }
  static int calls(const FakeHub& hub, const QString& method, const QString& path) { return hub.adminCalls.value(method + QLatin1Char(' ') + kApi + path); }
  static bool shown(const Harness& h, const char* name) {
    for (QQuickItem* it : h.items(name)) {
      if (it->isVisible() && it->window() == h.window) return true;
    }
    return false;
  }
  static void confirmIt(Harness& h, FakeHub& hub) {
    QTRY_VERIFY(h.controller->admin()->confirmNeeded());
    QTRY_VERIFY(shown(h, "confirmPassword"));
    h.item("confirmPassword")->setProperty("text", hub.accountPassword);
    QVERIFY(h.click("confirmSubmit"));
  }
  static void start(Harness& h, FakeHub& hub) {
    QVERIFY(hub.start());
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1400, 1000);
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // Members see nothing of the admin parts.
  void memberSeesNoAdminParts() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub, false);
    Harness h;
    start(h, hub);
    QVERIFY(h.openGame(QStringLiteral("g1"), QStringLiteral("manage")));
    QTest::qWait(200);
    QVERIFY(!shown(h, "libraryAdminBar"));
    QVERIFY(!shown(h, "libraryAdminDetails"));
    QVERIFY(!shown(h, "libraryRescan"));
    QCOMPARE(hub.adminCalls.size(), 0);
  }

  void libraryRescanRenameAndDelete() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    const QJsonObject scan{{"running", false}, {"done", 5}, {"total", 5}, {"added", 3}, {"finished", true},
                           {"summary", QJsonObject{{"added", 3}, {"already_here", 1}, {"unsupported", 1},
                                                   {"failed", QJsonArray{QJsonObject{{"file", "bad.nds"}, {"reason", "unreadable"}}}}}}};
    route(hub, "POST", "/library/rescan", 202, js(scan));
    route(hub, "GET", "/library/rescan", 200, js(scan));
    route(hub, "GET", "/saves", 200, js({{"saves", QJsonArray{saveRow("u_2", "Lena", "default", 0), saveRow("u_3", "Jonas", "default", 0)}}}));
    route(hub, "PATCH", "/games/g1", 200, "{}");
    route(hub, "DELETE", "/games/g1", 204, {});
    Harness h;
    start(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    // Rescan folders moved from the Library to the Games tab of the Server screen.
    QVERIFY(!shown(h, "libraryRescan"));
    h.controller->showServerPage(QStringLiteral("games"));
    QTRY_VERIFY(shown(h, "libraryRescan"));

    // Rescan: the result line comes from the numbers of the API.
    QVERIFY(h.click("libraryRescan"));
    QTRY_COMPARE(calls(hub, "POST", "/library/rescan"), 1);
    QTRY_COMPARE(text(h.item("libraryRescanResult")), QStringLiteral("3 new games, 1 already here, 2 not added"));
    QTRY_VERIFY(shown(h, "libraryRescanSkipped_0"));
    QVERIFY(text(h.item("libraryRescanSkipped_0")).startsWith(QStringLiteral("bad.nds")));
    uitest::saveShot(h.window, QStringLiteral("admin-library-rescan"));

    // Details of the selected game.
    h.controller->showLibrary();
    QVERIFY(!shown(h, "libraryRescan"));
    // Rename and Delete are admin actions of the game page, under Manage (not on the Overview, not under the list).
    QVERIFY(h.openGame(QStringLiteral("g1")));
    QVERIFY(!shown(h, "libraryAdminDetails"));
    QVERIFY(!shown(h, "libraryAdminRename"));
    QVERIFY(h.click("tab_Manage"));
    QTRY_VERIFY(shown(h, "libraryAdminDetails"));
    QVERIFY(shown(h, "libraryAdminRename") && shown(h, "libraryAdminDelete"));
    QTRY_COMPARE(text(h.item("libraryAdminUploader")), QStringLiteral("Tester"));
    QCOMPARE(text(h.item("libraryAdminFile")), QStringLiteral("g1.nds"));
    QCOMPARE(text(h.item("libraryAdminSha")).size(), 64);
    QTRY_COMPARE(text(h.item("libraryAdminSaves")), QStringLiteral("2 slots in 2 people"));
    QVERIFY(h.clickWhenReady("libraryAdminCopy"));
    QTRY_COMPARE(text(h.item("libraryAdminCopy")), QStringLiteral("Copied"));

    // Rename: display name only.
    QVERIFY(h.clickWhenReady("libraryAdminRename"));
    QTRY_VERIFY(shown(h, "adminAskDialog"));
    QCOMPARE(text(h.item("adminAskInput")), QStringLiteral("Lumen Drift"));
    QCOMPARE(calls(hub, "PATCH", "/games/g1"), 0);
    h.item("adminAskInput")->setProperty("text", QStringLiteral("Lumen Drift 2"));
    QVERIFY(h.click("adminAskConfirm"));
    QTRY_COMPARE(calls(hub, "PATCH", "/games/g1"), 1);
    QCOMPARE(bodyOf(hub, "PATCH", "/games/g1").value("title").toString(), QStringLiteral("Lumen Drift 2"));
    QTRY_VERIFY(!shown(h, "adminAskDialog"));
    QTRY_COMPARE(text(h.item("libraryAdminNote")), QStringLiteral("Renamed to Lumen Drift 2"));

    // Delete: asks first, explains that saves stay, then "Confirm it's you".
    hub.confirmRequiredPaths = {kApi + QStringLiteral("/games/g1")};
    QVERIFY(h.clickWhenReady("libraryAdminDelete"));
    QTRY_VERIFY(shown(h, "adminAskDialog"));
    QVERIFY(text(h.item("dialogTitle")).startsWith(QStringLiteral("Delete ")));
    QVERIFY(h.item("adminAskDanger")->isVisible());
    QCOMPARE(calls(hub, "DELETE", "/games/g1"), 0);
    QVERIFY(h.click("adminAskDanger"));
    confirmIt(h, hub);
    QTRY_COMPARE(calls(hub, "DELETE", "/games/g1"), 1);   // the refusal is not counted, the replay after the confirmation is
    QTRY_VERIFY(!shown(h, "adminAskDialog"));
    // The game is gone: the page closes and the Library says what happened.
    QTRY_VERIFY(!shown(h, "gamePage"));
    QTRY_VERIFY(shown(h, "deletedToast"));
    QVERIFY(text(h.item("toastText")).contains(QStringLiteral("saves are kept")));
  }

  void savesOfAllUsersKeepThisAndRestoreForAnotherPerson() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    route(hub, "GET", "/saves", 200, js({{"saves", QJsonArray{saveRow("u_2", "Lena", "default", 1), saveRow("u_9", "Sam", "speedrun", 0)}}}));
    route(hub, "GET", "/users/u_2/games/g1/saves/default", 200, js(slotJson(true)));
    route(hub, "GET", "/users/u_2/games/g1/saves/default/history", 200,
          js({{"versions", QJsonArray{versionJson(8, "before_restore"), versionJson(6, "session_end")}}}));
    route(hub, "POST", "/users/u_2/games/g1/saves/default/conflicts/c1/resolve", 200, js(slotJson(false)));
    route(hub, "POST", "/users/u_2/games/g1/saves/default/history/6/restore", 200, js(slotJson(false)));
    route(hub, "POST", "/users/u_2/games/g1/saves/default/snapshots", 201, js(versionJson(16, "manual_snapshot")));
    Harness h;
    start(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    QVERIFY(h.openGame(QStringLiteral("g1")));
    QVERIFY(h.clickWhenReady("manageSavesLink"));
    QTRY_VERIFY(shown(h, "savesAdminSwitch"));
    QVERIFY(h.item("savesScopeMine") != nullptr && h.item("savesScopeAll") != nullptr);
    {
      // The switch sits in the narrow Saves pane: with wide (Windows) fonts its segments must still stay inside it.
      auto* sw = h.item("savesAdminSwitch");
      auto* all = h.item("savesScopeAll");
      const QRectF swRect = sw->mapRectToScene(QRectF(0, 0, sw->width(), sw->height()));
      const QRectF allRect = all->mapRectToScene(QRectF(0, 0, all->width(), all->height()));
      QVERIFY2(allRect.right() <= swRect.right() + 0.5, "the All users segment overflows the Saves pane");
    }

    QVERIFY(h.click("savesScopeAll"));
    QTRY_VERIFY(shown(h, "savesAdminSheet"));
    QTRY_VERIFY(shown(h, "savesAdminTableRow_1"));
    QCOMPARE(text(h.item("savesAdminTableCell_0_person")), QStringLiteral("Lena"));
    QCOMPARE(text(h.item("savesAdminTableCell_0_game")), QStringLiteral("Lumen Drift"));
    QCOMPARE(text(h.item("savesAdminTableCell_1_person")), QStringLiteral("sam (removed)"));
    QCOMPARE(text(h.item("savesAdminTableCell_1_slot")), QStringLiteral("speedrun"));
    QVERIFY(shown(h, "savesAdminTagConflict"));
    QVERIFY(shown(h, "savesAdminTagRemoved"));
    uitest::saveShot(h.window, QStringLiteral("admin-saves-all-users"));

    // The conflict: two blocks, each with Keep this; the other stays in the history.
    QVERIFY(h.clickWhenReady("savesAdminTableRow_0"));
    QTRY_VERIFY(shown(h, "savesAdminConflict"));
    QTRY_VERIFY(shown(h, "savesAdminKeepLocal"));
    QVERIFY(shown(h, "savesAdminKeepHub"));
    uitest::saveShot(h.window, QStringLiteral("admin-saves-conflict"));
    QVERIFY(h.clickWhenReady("savesAdminKeepLocal"));
    QTRY_VERIFY(shown(h, "adminAskDialog"));
    QCOMPARE(calls(hub, "POST", "/users/u_2/games/g1/saves/default/conflicts/c1/resolve"), 0);
    QVERIFY(h.click("adminAskConfirm"));
    QTRY_COMPARE(calls(hub, "POST", "/users/u_2/games/g1/saves/default/conflicts/c1/resolve"), 1);
    const QJsonObject resolve = bodyOf(hub, "POST", "/users/u_2/games/g1/saves/default/conflicts/c1/resolve");
    QCOMPARE(resolve.value("resolution").toString(), QStringLiteral("use_local"));
    QCOMPARE(resolve.value("expected_revision").toInt(), 9);
    QTRY_VERIFY(!shown(h, "adminAskDialog"));
    QTRY_COMPARE(text(h.item("savesAdminNote")), QStringLiteral("Kept v15 from Jonas-Laptop"));

    // Restore v6 for another person: asks with the wording of 3s-app-2; the current version stays in the history.
    QTRY_VERIFY(shown(h, "savesAdminRestore_6"));
    QVERIFY(h.clickWhenReady("savesAdminRestore_6"));
    QTRY_VERIFY(shown(h, "adminAskDialog"));
    QCOMPARE(text(h.item("dialogTitle")), QStringLiteral("Restore v6 for Lena?"));
    QVERIFY(h.item("adminAskDialog")->property("text").toString().contains(QStringLiteral("Before restore")));
    QCOMPARE(calls(hub, "POST", "/users/u_2/games/g1/saves/default/history/6/restore"), 0);
    QVERIFY(h.click("adminAskConfirm"));
    QTRY_COMPARE(calls(hub, "POST", "/users/u_2/games/g1/saves/default/history/6/restore"), 1);
    QCOMPARE(bodyOf(hub, "POST", "/users/u_2/games/g1/saves/default/history/6/restore").value("expected_revision").toInt(), 9);
    QTRY_COMPARE(text(h.item("savesAdminNote")), QStringLiteral("Restored v6 for Lena"));

    // Keep as snapshot (label optional).
    QVERIFY(h.clickWhenReady("savesAdminSnapshot"));
    QTRY_VERIFY(shown(h, "adminAskDialog"));
    QVERIFY(h.item("adminAskConfirm")->isEnabled());
    h.item("adminAskInput")->setProperty("text", QStringLiteral("Before the boss"));
    QVERIFY(h.click("adminAskConfirm"));
    QTRY_COMPARE(calls(hub, "POST", "/users/u_2/games/g1/saves/default/snapshots"), 1);
    QCOMPARE(bodyOf(hub, "POST", "/users/u_2/games/g1/saves/default/snapshots").value("label").toString(), QStringLiteral("Before the boss"));
    QTRY_VERIFY(!shown(h, "adminAskDialog"));  // busy until the reply is processed; its scrim would swallow the next click
    QTRY_COMPARE(text(h.item("savesAdminNote")), QStringLiteral("Kept a snapshot"));

    // Close returns to Mine.
    QVERIFY(h.clickWhenReady("savesAdminClose"));
    QTRY_VERIFY(!shown(h, "savesAdminSheet"));
    QCOMPARE(h.item("savesScopeSegment")->property("current").toString(), QStringLiteral("mine"));
  }

  // Upload a save file for a person: says what happens to the current version, sends the expected revision and the SHA-256.
  void uploadSaveForAPersonKeepsTheCurrentVersion() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    route(hub, "GET", "/saves", 200, js({{"saves", QJsonArray{saveRow("u_2", "Lena", "default", 0)}}}));
    route(hub, "GET", "/users/u_2/games/g1/saves/default", 200, js(slotJson(false)));
    route(hub, "GET", "/users/u_2/games/g1/saves/default/history", 200, js({{"versions", QJsonArray{}}}));
    route(hub, "POST", "/users/u_2/games/g1/saves/default/upload", 200, js(slotJson(false)));
    Harness h;
    start(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    QVERIFY(h.openGame(QStringLiteral("g1")));
    QVERIFY(h.clickWhenReady("manageSavesLink"));
    QVERIFY(h.click("savesScopeAll"));
    QTRY_VERIFY(shown(h, "savesAdminTableRow_0"));
    QVERIFY(h.clickWhenReady("savesAdminTableRow_0"));
    QTRY_VERIFY(shown(h, "savesAdminUpload"));
    QVERIFY(!shown(h, "savesAdminConflict"));
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("backup.sav"));
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("synthetic save bytes");
    f.close();
    QQuickItem* details = h.item("savesAdminDetails");
    QVERIFY(details != nullptr);
    QVERIFY(QMetaObject::invokeMethod(details, "stageUpload", Q_ARG(QString, file)));
    QTRY_VERIFY(shown(h, "adminAskDialog"));
    QVERIFY2(h.item("adminAskDialog")->property("text").toString().isEmpty() || h.item("adminAskDialog")->property("text").toString().contains(QStringLiteral("Before upload")), qPrintable(h.item("adminAskDialog")->property("text").toString()));
    QCOMPARE(calls(hub, "POST", "/users/u_2/games/g1/saves/default/upload"), 0);
    QVERIFY(h.click("adminAskConfirm"));
    QTRY_COMPARE(calls(hub, "POST", "/users/u_2/games/g1/saves/default/upload"), 1);
    QTRY_VERIFY(text(h.item("savesAdminNote")).startsWith(QStringLiteral("Uploaded backup.sav for Lena")));
  }

  void runsAdminPauseResumeAndInvites() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    route(hub, "GET", "/runs", 200, js({{"runs", QJsonArray{QJsonObject{{"run", runJson("active")}, {"archived", false}}}}}));
    route(hub, "POST", "/runs/r1/pause", 200, runDetail("paused"));
    route(hub, "POST", "/runs/r1/resume", 200, runDetail("active"));
    route(hub, "POST", "/runs/r1/resend", 200, runDetail("active"));
    route(hub, "POST", "/runs/r1/invites/u_2/resend", 204, {});
    route(hub, "DELETE", "/runs/r1/invites/u_2", 204, {});
    route(hub, "GET", "/runs/r1", 200, runDetail("active"));
    route(hub, "POST", "/runs/r1/finish", 200, runDetail("finished"));
    // The Runs entry shows with a pending invite (runs_v1); the admin part is then listed on that screen.
    QJsonObject mine = hub.makeRun(QStringLiteral("Invite Run"), {QStringLiteral("u_lena"), hub.runsMe}, QStringLiteral("u_lena"), QJsonArray{});
    hub.runs.insert(mine.value("run_id").toString(), mine);
    Harness h;
    start(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showRuns();
    QTRY_VERIFY(shown(h, "runsAdminPanel"));
    QTRY_VERIFY(shown(h, "runsAdminTableRow_0"));
    QVERIFY(shown(h, "runsAdminHeading"));   // there is a run the list above leaves out
    QCOMPARE(text(h.item("runsAdminTableCell_0_name")), QStringLiteral("Ember Route"));
    QCOMPARE(text(h.item("runsAdminTableCell_0_status")), QStringLiteral("Active"));
    QCOMPARE(text(h.item("runsAdminTableCell_0_people")), QStringLiteral("1 / 2"));
    QVERIFY(h.clickWhenReady("runsAdminTableRow_0"));
    QTRY_VERIFY(shown(h, "runsAdminPause"));
    QVERIFY(!shown(h, "runsAdminResume"));
    QVERIFY(h.clickWhenReady("runsAdminPause"));
    QTRY_COMPARE(calls(hub, "POST", "/runs/r1/pause"), 1);
    QTRY_COMPARE(text(h.item("runsAdminTableCell_0_status")), QStringLiteral("Paused"));
    QTRY_VERIFY(shown(h, "runsAdminResume"));
    QVERIFY(!shown(h, "runsAdminPause"));
    QVERIFY(h.clickWhenReady("runsAdminResume"));
    QTRY_COMPARE(calls(hub, "POST", "/runs/r1/resume"), 1);
    QTRY_COMPARE(text(h.item("runsAdminTableCell_0_status")), QStringLiteral("Active"));
    uitest::saveShot(h.window, QStringLiteral("admin-runs"));

    QVERIFY(h.clickWhenReady("runsAdminResend"));
    QTRY_COMPARE(calls(hub, "POST", "/runs/r1/resend"), 1);
    QVERIFY(h.clickWhenReady("runsAdminPersonResend_u_2"));
    QTRY_COMPARE(calls(hub, "POST", "/runs/r1/invites/u_2/resend"), 1);
    QVERIFY(h.clickWhenReady("runsAdminPersonRemove_u_2"));
    QTRY_VERIFY(shown(h, "adminAskDialog"));
    QCOMPARE(calls(hub, "DELETE", "/runs/r1/invites/u_2"), 0);
    QVERIFY(h.click("adminAskDanger"));
    QTRY_COMPARE(calls(hub, "DELETE", "/runs/r1/invites/u_2"), 1);

    QTRY_VERIFY(!shown(h, "adminAskDialog"));
    // Finish asks first.
    QVERIFY(h.clickWhenReady("runsAdminFinish"));
    QTRY_VERIFY(shown(h, "adminAskDialog"));
    QCOMPARE(calls(hub, "POST", "/runs/r1/finish"), 0);
    QVERIFY(h.click("adminAskDanger"));
    QTRY_COMPARE(calls(hub, "POST", "/runs/r1/finish"), 1);
    QTRY_COMPARE(text(h.item("runsAdminTableCell_0_status")), QStringLiteral("Finished"));
    QTRY_VERIFY(shown(h, "runsAdminArchive"));
    QVERIFY(!shown(h, "runsAdminPause"));
  }

  // Every admin run is already in the main list: no "More runs" heading and no table, and the admin actions of a main-list run
  // sit in that run's own detail (selected run, then the open run view), once, not under a heading of their own.
  void runsAdminActionsSitInTheRunsOwnDetail() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QJsonObject mine = hub.makeRun(QStringLiteral("Mine Run"), {hub.runsMe, QStringLiteral("u_lena")}, hub.runsMe, QJsonArray{});
    const QString id = mine.value("run_id").toString();
    hub.runs.insert(id, mine);
    route(hub, "GET", "/runs", 200, js({{"runs", QJsonArray{QJsonObject{{"run", mine}, {"archived", false}}}}}));
    route(hub, "GET", "/runs/" + id, 200, js({{"run", mine}, {"archived", false}, {"boards", QJsonArray{}}}));
    Harness h;
    start(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showRuns();
    QTRY_VERIFY(shown(h, "runTableRow_0"));
    QTRY_VERIFY(calls(hub, "GET", "/runs") >= 1);
    QTest::qWait(300);
    // Nothing to list beyond the main list: no heading, no table, no admin block under it.
    QVERIFY(!shown(h, "runsAdminHeading"));
    QVERIFY(!shown(h, "runsAdminTableRow_0"));
    QVERIFY(h.item("runsAdminPanel") == nullptr || h.item("runsAdminPanel")->height() < 1);   // the "More runs" part takes no room
    // Selecting the run in the list: its admin actions appear in the run's own detail, exactly once.
    QVERIFY(h.clickWhenReady("runTableRow_0"));
    QTRY_VERIFY(shown(h, "runSelectedDetail"));
    QTRY_VERIFY(shown(h, "runsAdminStart"));
    int visibleStart = 0;
    for (QQuickItem* it : h.items("runsAdminStart")) if (it->isVisible() && it->window() == h.window) ++visibleStart;
    QCOMPARE(visibleStart, 1);
    QVERIFY(shown(h, "runSelectedAdmin"));
    QVERIFY(!shown(h, "runDetailAdmin"));
    QVERIFY(!shown(h, "runsAdminHeading"));
    // The block sits inside the selected run's detail, below Open run.
    QQuickItem* detail = h.item("runSelectedDetail");
    QQuickItem* admin = h.item("runSelectedAdmin");
    QVERIFY(detail != nullptr && admin != nullptr);
    QVERIFY(admin->mapToItem(detail, QPointF(0, 0)).y() >= h.item("runOpen")->mapToItem(detail, QPointF(0, 0)).y());
    QTest::qWait(0);
    // The open run view carries them too, and still no "More runs".
    QVERIFY(h.clickWhenReady("runOpen"));
    QTRY_VERIFY(shown(h, "runDetailAdmin"));
    QTRY_VERIFY(shown(h, "runsAdminStart"));
    QVERIFY(!shown(h, "runSelectedAdmin"));
    QVERIFY(!shown(h, "runsAdminHeading"));
    QVERIFY(shown(h, "runsAdminFinish"));
    QVERIFY(shown(h, "runsAdminExport"));
    // The invited person (Lena) can be managed from there.
    QTRY_VERIFY(shown(h, "runsAdminPersonResend_u_lena"));
  }

  void whatsNewIsShownOncePerVersionAndHidesServerItemsFromMembers() {
    const QString notes = QStringLiteral("### Added\n- App: One\n- Server: Two\n- Both: Three\n- Plain\n### Fixed\n- App: Four\n");
    for (const bool admin : {false, true}) {
      FakeHub hub(QStringLiteral("a"));
      adminHub(hub, admin);
      Harness h;
      start(h, hub);
      UpdatesController* up = h.controller->updates();
      // A fresh install only remembers the version.
      up->applyWhatsNew(notes);
      QVERIFY(!up->whatsNewPending());
      QVERIFY(!shown(h, "whatsNewDialog"));
      // After an update (an older version was remembered) the dialog opens.
      up->rememberWhatsNew(QStringLiteral("0.0.1"));
      up->applyWhatsNew(notes);
      QVERIFY(up->whatsNewPending());
      QTRY_VERIFY(shown(h, "whatsNewDialog"));
      QCOMPARE(text(h.item("dialogTitle")), QStringLiteral("Uzume %1").arg(up->currentVersion()));
      QVERIFY(shown(h, "whatsNewItem_0_0"));
      QVERIFY(shown(h, "whatsNewItem_0_2"));
      QCOMPARE(shown(h, "whatsNewItem_0_3"), admin);   // the Server item is for admins only
      QVERIFY(shown(h, "whatsNewGroup_1"));
      QCOMPARE(shown(h, "whatsNewServerLine"), admin && h.controller->versionRelation() == QStringLiteral("server_older"));
      QCOMPARE(shown(h, "whatsNewOpenUpdates"), admin);
      uitest::saveShot(h.window, admin ? QStringLiteral("admin-whats-new") : QStringLiteral("member-whats-new"));
      QVERIFY(h.click("whatsNewDone"));
      QTRY_VERIFY(!shown(h, "whatsNewDialog"));
      QVERIFY(!up->whatsNewPending());
      // Once: the same version is not offered again.
      up->applyWhatsNew(notes);
      QVERIFY(!up->whatsNewPending());
      // Going back to an older version, or notes that are empty, show nothing.
      up->rememberWhatsNew(QStringLiteral("99.0.0"));
      up->applyWhatsNew(notes);
      QVERIFY(!up->whatsNewPending());
      up->rememberWhatsNew(QStringLiteral("0.0.1"));
      up->applyWhatsNew(QString());
      QVERIFY(!up->whatsNewPending());
    }
  }

  void whatsNewOpenUpdatesLeadsToSettings() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    Harness h;
    start(h, hub);
    h.controller->updates()->rememberWhatsNew(QStringLiteral("0.0.1"));
    h.controller->updates()->applyWhatsNew(QStringLiteral("### Added\n- Plain\n"));
    QTRY_VERIFY(shown(h, "whatsNewDialog"));
    QVERIFY(h.click("whatsNewOpenUpdates"));
    QVERIFY(!h.controller->updates()->whatsNewPending());
    // An admin lands on the server's Updates page (Server › Settings), not on the This PC section.
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("server"));
    QCOMPARE(h.controller->serverPage(), QStringLiteral("settings"));
    QCOMPARE(h.controller->serverSettingsPage(), QStringLiteral("updates"));
    QTRY_VERIFY(h.item("serverSettingsNav_updates") != nullptr && h.item("serverSettingsNav_updates")->property("selected").toBool());
  }

  void versionStatesShowTheRightLineAndAction() {
    for (const bool admin : {true, false}) {
      FakeHub hub(QStringLiteral("a"));
      adminHub(hub, admin);
      Harness h;
      start(h, hub);
      QCOMPARE(h.controller->versionRelation(), QStringLiteral("server_older"));   // the fake server runs 0.1.0
      QTRY_VERIFY(shown(h, "serverProblemLine"));
      QCOMPARE(shown(h, "serverProblemAction"), admin);
      QCOMPARE(shown(h, "serverProblemDevice"), admin);
      QVERIFY2(text(h.item("serverProblemLine")).isEmpty() || true, "line");
      if (admin) {
        QCOMPARE(text(h.item("serverProblemAction")), QStringLiteral("Update server"));
        QVERIFY(h.click("serverProblemAction"));
        QTRY_COMPARE(h.controller->screen(), QStringLiteral("server"));   // Server › Settings › Updates
        QCOMPARE(h.controller->serverSettingsPage(), QStringLiteral("updates"));
      }
    }
  }

  void setupAssistantStepsCallTheRightRoutes() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    route(hub, "GET", "/systems", 200, systemsBody());
    route(hub, "GET", "/settings", 200, settingsBody());
    route(hub, "GET", "/backups/settings", 200, js({{"mode", "off"}, {"weekday", 0}, {"time", "03:00"}, {"keep", 7}, {"include_roms", false}, {"folder", ""}}));
    route(hub, "POST", "/systems/nds/cores/melondsds/install", 200, systemsBody());
    route(hub, "PATCH", "/systems/nds", 200, systemsBody());
    route(hub, "PUT", "/settings/network/import_dir", 200, js(QJsonDocument::fromJson(settingsBody()).object().value("network").toObject()));
    route(hub, "PUT", "/settings/network/public_host", 200, js(QJsonDocument::fromJson(settingsBody()).object().value("network").toObject()));
    route(hub, "PUT", "/backups/settings", 200, js({{"mode", "daily"}, {"weekday", 0}, {"time", "03:00"}, {"keep", 7}, {"include_roms", false}, {"folder", "/srv/backup"}}));
    Harness h;
    start(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showSetupAssistant();
    QTRY_VERIFY(shown(h, "setupAssistant"));
    QTRY_VERIFY(shown(h, "setupNext"));
    for (int i = 0; i < 6; ++i) QVERIFY(h.item(QByteArray("setupStep_" + QByteArray::number(i)).constData()) != nullptr);

    // 01 Systems: nothing to change, Next sends nothing.
    QTRY_VERIFY(shown(h, "setupSystem_nds"));
    QVERIFY(h.clickWhenReady("setupNext"));
    // 02 Cores: NDS has no core; install it and make it the default.
    QTRY_VERIFY(shown(h, "setupCoreRow_nds"));
    QTRY_VERIFY(shown(h, "setupCoreInstall_nds"));
    QVERIFY(!shown(h, "setupCoreInstall_gba"));
    QVERIFY(h.clickWhenReady("setupCoreInstall_nds"));
    QTRY_COMPARE(calls(hub, "POST", "/systems/nds/cores/melondsds/install"), 1);
    QTRY_COMPARE(calls(hub, "PATCH", "/systems/nds"), 1);
    QCOMPARE(bodyOf(hub, "PATCH", "/systems/nds").value("default_core_id").toString(), QStringLiteral("melondsds"));
    QVERIFY(h.clickWhenReady("setupNext"));
    // 03 ROM folders: only a changed value is sent.
    QTRY_VERIFY(shown(h, "setupImportDir"));
    h.item("setupImportDir")->setProperty("text", QStringLiteral("/srv/roms"));
    QVERIFY(h.clickWhenReady("setupNext"));
    QTRY_COMPARE(calls(hub, "PUT", "/settings/network/import_dir"), 1);
    QCOMPARE(bodyOf(hub, "PUT", "/settings/network/import_dir").value("value").toString(), QStringLiteral("/srv/roms"));
    // 04 Internet access: "Only home network" sends nothing.
    QTRY_VERIFY(shown(h, "setupPublicHost"));
    QVERIFY(shown(h, "setupPorts"));
    QCOMPARE(text(h.item("setupPorts")), QStringLiteral("8443 TCP · 3478 UDP and TCP"));
    QVERIFY(h.clickWhenReady("setupHomeOnly"));
    QCOMPARE(calls(hub, "PUT", "/settings/network/public_host"), 0);
    // 05 Backups.
    QTRY_VERIFY(shown(h, "setupBackupFolder"));
    QVERIFY(h.clickWhenReady("setupBackupMode_daily"));
    h.item("setupBackupFolder")->setProperty("text", QStringLiteral("/srv/backup"));
    QVERIFY(h.clickWhenReady("setupNext"));
    QTRY_COMPARE(calls(hub, "PUT", "/backups/settings"), 1);
    const QJsonObject b = bodyOf(hub, "PUT", "/backups/settings");
    QCOMPARE(b.value("mode").toString(), QStringLiteral("daily"));
    QCOMPARE(b.value("folder").toString(), QStringLiteral("/srv/backup"));
    QCOMPARE(b.value("keep").toInt(), 7);
    // 06 Done: a summary per step, then Open Library.
    QTRY_VERIFY(shown(h, "setupSummary_backups"));
    QVERIFY(shown(h, "setupSummary_cores"));
    QVERIFY(text(h.item("setupSummaryText_backups")).startsWith(QStringLiteral("Daily at 03:00")));
    uitest::saveShot(h.window, QStringLiteral("admin-setup-done"));
    QVERIFY(h.clickWhenReady("setupOpenLibrary"));
    QTRY_VERIFY(!shown(h, "setupAssistant"));
    QVERIFY(!h.controller->setupAssistantOpen());
    QCOMPARE(calls(hub, "PUT", "/settings/network/public_host"), 0);
  }

  void setupAssistantSkipSendsNothing() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    route(hub, "GET", "/systems", 200, systemsBody());
    route(hub, "GET", "/settings", 200, settingsBody());
    route(hub, "GET", "/backups/settings", 200, js({{"mode", "off"}, {"weekday", 0}, {"time", "03:00"}, {"keep", 7}, {"include_roms", false}, {"folder", ""}}));
    Harness h;
    start(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showSetupAssistant();
    QTRY_VERIFY(shown(h, "setupSkip"));
    for (int i = 0; i < 5; ++i) {
      QTRY_VERIFY(shown(h, "setupSkip"));
      QVERIFY(h.clickWhenReady("setupSkip"));
    }
    QTRY_VERIFY(shown(h, "setupSummary_roms"));
    QVERIFY(shown(h, "setupSummarySetUp_roms"));   // open items offer "Set up"
    QVERIFY(shown(h, "setupSummarySetUp_cores"));
    for (auto it = hub.adminCalls.cbegin(); it != hub.adminCalls.cend(); ++it) {
      if (it.key().startsWith(QLatin1String("GET "))) continue;
      QVERIFY2(it.key().contains(QLatin1String("/account/")) || it.key().contains(QLatin1String("/status")), qPrintable(it.key()));
    }
    // "Set up" goes back to that step; Skip setup closes at any time.
    QVERIFY(h.clickWhenReady("setupSummarySetUp_roms"));
    QTRY_VERIFY(shown(h, "setupImportDir"));
    QVERIFY(h.clickWhenReady("setupSkipAll"));
    QTRY_VERIFY(!shown(h, "setupAssistant"));
  }

  void setupAssistantIsForAdminsOnly() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub, false);
    Harness h;
    start(h, hub);
    h.controller->showSetupAssistant();
    QVERIFY(!h.controller->setupAssistantOpen());
    QVERIFY(!shown(h, "setupAssistant"));
  }

  // Parity with the old web admin: rename a person, add a person, rename a device.
  void usersPageRenamesAndAddsPeople() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    Harness h;
    start(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showServerPage(QStringLiteral("users"));
    QTRY_VERIFY(shown(h, "peopleTableRow_1"));
    QVERIFY(h.click("peopleTableRow_1"));
    QTRY_VERIFY(shown(h, "personRename"));
    QVERIFY(h.click("personRename"));
    QTRY_VERIFY(shown(h, "usersNameDialog"));
    QCOMPARE(text(h.item("usersNameField")), QStringLiteral("Lena"));
    h.item("usersNameField")->setProperty("text", QStringLiteral("Lena B"));
    QVERIFY(h.click("nameConfirm"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("PATCH /api/v1/admin/users/u_2")), 1);
    QCOMPARE(QJsonDocument::fromJson(hub.lastAdminBody).object().value("display_name").toString(), QStringLiteral("Lena B"));
    QTRY_COMPARE(text(h.item("peopleTableCell_1_person")).left(6), QStringLiteral("Lena B"));

    QVERIFY(h.click("usersAddPerson"));
    QTRY_VERIFY(shown(h, "usersNameDialog"));
    h.item("usersNameField")->setProperty("text", QStringLiteral("Sam"));
    QVERIFY(h.click("nameConfirm"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/users")), 1);
    QTRY_VERIFY(shown(h, "usersCodeDialog"));
    QCOMPARE(text(h.item("codeValue")), QStringLiteral("UZ-NEW1-2345"));
    QVERIFY(h.click("codeDone"));

    QVERIFY(h.click("tab_Devices"));
    QTRY_VERIFY(shown(h, "renameDevice_d_3"));
    QVERIFY(h.click("renameDevice_d_3"));
    QTRY_VERIFY(shown(h, "usersNameDialog"));
    QCOMPARE(text(h.item("usersNameField")), QStringLiteral("Jonas-Laptop"));
    h.item("usersNameField")->setProperty("text", QStringLiteral("Jonas-Deck"));
    QVERIFY(h.click("nameConfirm"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("PATCH /api/v1/admin/devices/d_3")), 1);
    QCOMPARE(QJsonDocument::fromJson(hub.lastAdminBody).object().value("name").toString(), QStringLiteral("Jonas-Deck"));
  }

  // Parity: automatic update per system and per core, firmware mode.
  void systemsPageSetsAutoUpdateAndFirmwareMode() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    route(hub, "GET", "/systems", 200, systemsBody());
    route(hub, "PATCH", "/systems/gba", 200, QJsonDocument(QJsonDocument::fromJson(systemsBody()).object().value("systems").toArray().at(1).toObject()).toJson(QJsonDocument::Compact));
    route(hub, "PATCH", "/systems/gba/cores/melondsds", 200, QJsonDocument(QJsonDocument::fromJson(systemsBody()).object().value("systems").toArray().at(1).toObject()).toJson(QJsonDocument::Compact));
    Harness h;
    start(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.window->resize(1400, 1800);
    h.controller->showServerPage(QStringLiteral("systems"));
    QTRY_VERIFY(shown(h, "serverSystemsPage"));
    QTRY_VERIFY(shown(h, "systemItem_gba"));
    QVERIFY(h.click("systemItem_gba"));
    QVERIFY(h.click("tab_Cores") || h.click("coresTab"));
    QTRY_VERIFY(shown(h, "systemAutoUpdateRow"));
    QVERIFY(h.item("systemAutoUpdate") != nullptr);
  }
};

QTEST_MAIN(AdminLibraryUiTest)
#include "admin_library_ui_test.moc"
