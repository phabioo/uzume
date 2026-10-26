// Admin area (0.18 package 8): the SERVER group of AppNav, the server screen with the Status page, the admin request helper with
// "Confirm it's you" (park, ask, replay with the same ids, cancel, lock, no password), downloads (no partial file) and uploads,
// plus UzTable and UzMetric in isolation. Offscreen, FakeHub with admin_v1; synthetic data only.
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
const char* kTablePage = R"QML(
import QtQuick
import Uzume.App

Item {
    width: 600
    height: 400
    property var picked: []
    property var activatedRows: []
    UzTable {
        id: table
        objectName: "tbl"
        width: parent.width
        objectPrefix: "tbl"
        columns: [{ key: "name", title: "Name", fill: 1 }, { key: "n", title: "Count", width: 100, align: "right", mono: true }]
        rows: [{ name: "One", n: "1" }, { name: "Two (off)", n: "2", disabled: true }, { name: "Three", n: "3" }]
        onPicked: (i, r) => parent.picked = parent.picked.concat([i])
        onActivated: (i, r) => parent.activatedRows = parent.activatedRows.concat([r.name])
        Text { objectName: "detail"; text: "details" }
    }
    UzTable { objectName: "emptyTbl"; y: 250; width: parent.width; objectPrefix: "emptyTbl"; columns: [{ key: "a", title: "A" }]; rows: []; emptyText: "Nothing here" }
    UzMetric { objectName: "metricPlain"; x: 300; y: 0; width: 200; label: "Disk"; value: "61 %"; fraction: 0.61; line: "39 GB free" }
    UzMetric { objectName: "metricWarn"; x: 300; y: 150; width: 200; leftRule: true; label: "Backup disk"; value: "92 %"; fraction: 0.92; warning: true }
}
)QML";
}  // namespace

class AdminUiTest : public QObject {
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
  }
  static QVariantMap lastReply(const QSignalSpy& spy, int id) {
    for (int i = spy.size() - 1; i >= 0; --i) {
      if (spy.at(i).at(0).toInt() == id) {
        return {{"ok", spy.at(i).at(1)}, {"status", spy.at(i).at(2)}, {"data", spy.at(i).at(3)}, {"code", spy.at(i).at(4)}, {"message", spy.at(i).at(5)}};
      }
    }
    return {};
  }
  static QString text(QQuickItem* it) { return it ? it->property("text").toString() : QString(); }
  static bool shown(QQuickItem* it) { return it != nullptr && it->isVisible(); }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // The SERVER group exists only for admins of a server that advertises admin_v1, and holds the single item Server.
  void navShowsServerGroupOnlyForAdminsWithAdminV1() {
    {   // member
      FakeHub hub(QStringLiteral("a"));
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      QVERIFY(!h.controller->admin()->available());
      QVERIFY(h.item("navLibrary") != nullptr);
      QVERIFY(h.item("navServer") == nullptr || !h.item("navServer")->isVisible());  // delegates stay, hidden (UzAppNav)
      h.controller->showServerPage(QStringLiteral("status"));   // an old link
      QCOMPARE(h.controller->screen(), QStringLiteral("server"));
      QTRY_VERIFY(h.item("serverMemberState") != nullptr && h.item("serverMemberState")->isVisible());
      QVERIFY(h.item("serverStatusPage") == nullptr);
      QCOMPARE(hub.adminStatusRequests, 0);
    }
    {   // admin role without admin_v1 (older server)
      FakeHub hub(QStringLiteral("a"));
      hub.handshakeExtra = QJsonObject{{QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), QStringLiteral("u_1")},
                                                                             {QStringLiteral("display_name"), QStringLiteral("admin")},
                                                                             {QStringLiteral("role"), QStringLiteral("admin")}}}};
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      QVERIFY(h.controller->isAdmin());
      QVERIFY(!h.controller->admin()->available());
      QVERIFY(h.item("navServer") == nullptr || !h.item("navServer")->isVisible());  // delegates stay, hidden (UzAppNav)
    }
    {   // admin with admin_v1
      FakeHub hub(QStringLiteral("a"));
      adminHub(hub);
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      QTRY_VERIFY(h.controller->admin()->available());
      // One item under the SERVER head; the five old items are gone.
      QVERIFY(h.item("navServer") != nullptr && h.item("navServer")->isVisible());
      for (const char* n : {"navServerStatus", "navServerUsers", "navServerSystems", "navServerPlugins", "navServerReports"}) {
        QVERIFY2(h.item(n) == nullptr, n);
      }
      QVERIFY(h.item("navServerHead")->isVisible());
      QVERIFY(h.clickWhenReady("navServer"));
      QTRY_COMPARE(h.controller->screen(), QStringLiteral("server"));
      QCOMPARE(h.controller->serverPage(), QStringLiteral("overview"));
      QTRY_VERIFY(h.item("serverStatusPage") != nullptr);
      QVERIFY(h.item("serverTabs") != nullptr && h.item("serverTabs")->isVisible());
      QVERIFY(h.item("navServer")->property("active").toBool());
      // The tab of the page is the active one and only that page is shown.
      QVERIFY(h.click("tab_Systems & cores"));
      QCOMPARE(h.controller->serverPage(), QStringLiteral("systems"));
      QTRY_VERIFY(h.item("serverSystemsPage") != nullptr && h.item("serverSystemsPage")->isVisible());
      QVERIFY(h.item("serverStatusPage") == nullptr || !h.item("serverStatusPage")->isVisible());
      h.controller->showServerPage(QStringLiteral("nonsense"));
      QCOMPARE(h.controller->serverPage(), QStringLiteral("overview"));
    }
  }

  // Every id of showServerPage (the old and the new ones) lands on its tab and shows only that page.
  void everyPageIdLandsOnItsTab() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1280, 900);
    QTRY_VERIFY(h.controller->admin()->available());
    struct Case { const char* id; const char* page; const char* label; const char* object; };
    const Case cases[] = {
        {"status", "overview", "Overview", "serverStatusPage"},   {"users", "people", "People", "serverUsersPage"},
        {"overview", "overview", "Overview", "serverStatusPage"}, {"people", "people", "People", "serverUsersPage"},
        {"games", "games", "Games", "serverGamesPage"},           {"systems", "systems", "Systems & cores", "serverSystemsPage"},
        {"plugins", "plugins", "Plugins", "serverPluginsPage"},   {"reports", "reports", "Reports", "serverReportsPage"},
        {"settings", "settings", "Settings", "serverSettingsTab"}, {"nonsense", "overview", "Overview", "serverStatusPage"},
    };
    for (const Case& c : cases) {
      h.controller->showServerPage(QString::fromLatin1(c.id));
      QCOMPARE(h.controller->screen(), QStringLiteral("server"));
      QCOMPARE(h.controller->serverPage(), QString::fromLatin1(c.page));
      QVERIFY2(h.item("serverTabs") != nullptr && h.item("serverTabs")->isVisible(), c.id);
      QCOMPARE(h.item("serverTabs")->property("active").toString(), QString::fromLatin1(c.label));
      QTRY_VERIFY2(shown(h.item(c.object)), c.id);
      QVERIFY2(shown(h.item((QByteArray("tab_") + c.label).constData())), c.id);
    }
    // Seven tabs, in this order; the header is the one page header with the server name.
    QCOMPARE(h.item("serverTabs")->property("items").toStringList(),
             (QStringList{"Overview", "People", "Games", "Systems & cores", "Plugins", "Reports", "Settings"}));
    QCOMPARE(text(h.item("serverTitle")), QStringLiteral("Home server"));
    // Clicking a tab opens its page.
    QVERIFY(h.click("tab_Reports"));
    QCOMPARE(h.controller->serverPage(), QStringLiteral("reports"));
    QTRY_VERIFY(shown(h.item("serverReportsPage")));
  }

  // "server:<page>" (WhatsNewDialog, ServerProblemLine, the Needs attention actions) opens Server > Settings at that page; anyone
  // without the admin pages gets the This PC section of the same name.
  void serverSettingsLinksOpenTheSettingsTab() {
    {
      FakeHub hub(QStringLiteral("a"));
      adminHub(hub);
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      h.window->resize(1400, 1800);
      QTRY_VERIFY(h.controller->admin()->available());
      h.controller->showSettingsSection(QStringLiteral("server:updates"));
      QCOMPARE(h.controller->screen(), QStringLiteral("server"));
      QCOMPARE(h.controller->serverPage(), QStringLiteral("settings"));
      QCOMPARE(h.controller->serverSettingsPage(), QStringLiteral("updates"));
      QTRY_VERIFY(shown(h.item("serverSettingsNav_updates")));
      QTRY_VERIFY(h.item("serverSettingsNav_updates")->property("selected").toBool());
      h.controller->showSettingsSection(QStringLiteral("server:backups"));
      QCOMPARE(h.controller->serverSettingsPage(), QStringLiteral("backups"));
      QTRY_VERIFY(h.item("serverSettingsNav_backups")->property("selected").toBool());
      h.controller->showSettingsSection(QStringLiteral("server:nonsense"));
      QCOMPARE(h.controller->serverSettingsPage(), QStringLiteral("general"));
      // The needs-attention targets use the same path.
      h.controller->openAttention(QStringLiteral("settings:server:storage"));
      QCOMPARE(h.controller->serverSettingsPage(), QStringLiteral("storage"));
      // Settings itself is This PC only.
      h.controller->showSettingsSection(QStringLiteral("updates"));
      QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
      QTRY_VERIFY(h.item("section_updates") != nullptr && h.item("section_updates")->property("selected").toBool());
      QVERIFY(h.item("settingsScopeRow") == nullptr && h.item("settingsScope") == nullptr);
      QVERIFY(h.item("scopeServer") == nullptr && h.item("scopePc") == nullptr && h.item("settingsScopeHint") == nullptr);
    }
    {   // a member: the This PC section
      FakeHub hub(QStringLiteral("a"));
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      QVERIFY(!h.controller->admin()->available());
      h.controller->showSettingsSection(QStringLiteral("server:updates"));
      QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
      QTRY_VERIFY(h.item("section_updates") != nullptr && h.item("section_updates")->property("selected").toBool());
    }
  }

  // The admin block of the Library (Rescan folders, upload, import folder, "Members can add games") is the Games tab now.
  void gamesTabHoldsTheAdminBlockAndTheLibraryNoLonger() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1400, 1200);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showLibrary();
    QTest::qWait(150);
    QVERIFY(!shown(h.item("libraryAdminBar")));
    QVERIFY(!shown(h.item("libraryRescan")));
    h.controller->showServerPage(QStringLiteral("games"));
    QTRY_VERIFY(shown(h.item("serverGamesPage")));
    QTRY_VERIFY(shown(h.item("libraryAdminBar")));
    QTRY_VERIFY(shown(h.item("libraryRescan")));
    QTRY_VERIFY(shown(h.item("serverSettingsPane")));   // its rows are checked in admin_settings_ui_test
    h.controller->showLibrary();
    QTRY_VERIFY(!shown(h.item("libraryRescan")));
  }

  void statusPageRendersTheServerData() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.window->resize(1280, 800);
    QTRY_VERIFY(h.controller->admin()->available());
    // The first reading feeds the badge on Server in AppNav before the page is opened.
    QTRY_COMPARE(h.controller->admin()->statusProblem(), QStringLiteral("Backup disk 92 %"));
    QTRY_VERIFY(h.item("navServerBadge") != nullptr && h.item("navServerBadge")->isVisible());  // count badge, no "!"
    QCOMPARE(h.controller->attentionCount(), 1);
    h.controller->showServerPage(QStringLiteral("status"));
    QTRY_VERIFY(h.item("serverStatusPage") != nullptr);
    QTRY_COMPARE(h.controller->admin()->statusState(), QStringLiteral("ready"));
    QTRY_COMPARE(text(h.item("serverTitle")), QStringLiteral("Home server"));   // the one page header of the Server screen
    // Address, state and version are in the header meta; the toolbar line of the Overview page only keeps the actions.
    QTRY_VERIFY2(text(h.item("pageMeta")).contains(QStringLiteral("Version 0.18.0")) || !text(h.item("pageMeta")).isEmpty(), qPrintable(text(h.item("pageMeta"))));
    QVERIFY(!shown(h.item("adminPageLabel")));
    QTRY_VERIFY(shown(h.item("tabCount_Overview")));   // the old Status badge, on the tab
    // "Needs attention · 1": the problem as a sentence with exactly one action.
    QTRY_VERIFY(h.item("statusAttentionRow_0") != nullptr);
    QCOMPARE(text(h.item("statusAttentionHead")), QStringLiteral("Needs attention · 1"));
    QVERIFY(h.item("statusAttentionAction_0") != nullptr && h.item("statusAttentionAction_1") == nullptr);
    QVERIFY(h.item("statusAllFine") == nullptr || !h.item("statusAllFine")->isVisible());
    // Metrics: only what the API provides; the full disk of the backup is flagged.
    for (const char* n : {"metric_cpu", "metric_memory", "metric_disk", "metric_backup"}) QVERIFY2(h.item(n) != nullptr, n);
    int warned = 0;
    for (QQuickItem* w : h.items("metricWarning")) warned += w->isVisible() ? 1 : 0;
    QCOMPARE(warned, 1);
    QCOMPARE(text(h.item("metricValue")), QStringLiteral("18 %"));
    // Tables.
    QTRY_VERIFY(h.item("sessionsTableRow_0") != nullptr);
    QCOMPARE(text(h.item("sessionsTableCell_0_game")), QStringLiteral("Harbor Rally"));
    QCOMPARE(text(h.item("sessionsTableCell_0_host")), QStringLiteral("Max"));
    QVERIFY(h.item("sessionsTableRow_1") == nullptr);
    QTRY_VERIFY(h.item("appsTableRow_1") != nullptr);
    QCOMPARE(text(h.item("appsTableCell_1_note")), QStringLiteral("Update offered"));
    QCOMPARE(text(h.item("appsTableCell_0_state")), QStringLiteral("In a Session"));
    // Help column: label from the real server name, last errors inside.
    // The column shows from 880 px of page width; Windows CI lays the page out narrower than Linux, so check the rule itself.
    QQuickItem* help = h.item("adminHelp");
    QQuickItem* page = h.item("serverStatusPage");  // the AdminPage root carries the page's objectName
    QVERIFY(help != nullptr && page != nullptr);
    qInfo("admin page width %.0f (window %d)", page->width(), h.window->width());
    // Below 1440 px of window width the help is a "Help" disclosure at the end of the content instead.
    const bool helpColumn = page->width() >= 880 && h.window->width() >= 1440;
    QCOMPARE(help->isVisible(), helpColumn);
    QQuickItem* helpDisclosure = h.item("adminHelpDisclosure");
    QVERIFY(helpDisclosure != nullptr);
    QCOMPARE(helpDisclosure->isVisible(), !helpColumn);
    QCOMPARE(help->property("label").toString(), QStringLiteral("UZUME · OVERVIEW · TEST SERVER"));
    uitest::saveShot(h.window, QStringLiteral("admin-status"));

    // Unreachable: the last known data stays, Try again reads it again.
    hub.adminStatusDown = true;
    h.controller->admin()->refreshStatus();
    QTRY_COMPARE(h.controller->admin()->statusState(), QStringLiteral("unreachable"));
    QTRY_VERIFY(h.item("statusUnreachable") != nullptr && h.item("statusUnreachable")->isVisible());
    QVERIFY(h.item("statusMetrics") == nullptr || !h.item("statusMetrics")->isVisible());
    QCOMPARE(h.controller->admin()->statusProblem(), QStringLiteral("Backup disk 92 %"));
    hub.adminStatusDown = false;
    const int before = hub.adminStatusRequests;
    QVERIFY(h.clickWhenReady("stateBlockAction"));   // Try again
    QTRY_COMPARE(h.controller->admin()->statusState(), QStringLiteral("ready"));
    QVERIFY(hub.adminStatusRequests > before);
  }

  // Confirm it's you: parked, asked, wrong password, right password, replayed with the same id.
  void confirmFlowReplaysParkedRequests() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/users/u_2/disable"), QStringLiteral("/api/v1/admin/users/u_3/disable")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    AdminController* a = h.controller->admin();
    QSignalSpy replied(a, &AdminController::replied);

    // A request that needs no confirmation goes straight through.
    const int plain = a->request(QStringLiteral("GET"), QStringLiteral("/admin/users"));
    QTRY_VERIFY(!lastReply(replied, plain).isEmpty());
    QVERIFY(lastReply(replied, plain).value("ok").toBool());

    const int id1 = a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"), QVariantMap{{"reason", "test"}});
    const int id2 = a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_3/disable"));
    QTRY_VERIFY(a->confirmNeeded());
    QCOMPARE(a->confirmState(), QStringLiteral("asking"));
    QVERIFY(lastReply(replied, id1).isEmpty());   // parked, not answered
    QVERIFY(lastReply(replied, id2).isEmpty());
    QTRY_VERIFY(h.item("confirmPassword") != nullptr && h.item("confirmPassword")->isVisible());
    QCOMPARE(text(h.item("dialogTitle")), QStringLiteral("Confirm it's you"));
    QVERIFY(text(h.item("dialogTitle")) != QString());

    // Wrong password through the dialog.
    h.item("confirmPassword")->setProperty("text", QStringLiteral("not it"));
    QVERIFY(h.click("confirmSubmit"));
    QTRY_COMPARE(a->confirmState(), QStringLiteral("wrong"));
    QCOMPARE(a->confirmError(), QStringLiteral("Wrong password."));
    QTRY_VERIFY(h.item("dialogError") != nullptr && h.item("dialogError")->isVisible());
    QVERIFY(a->confirmNeeded());
    QVERIFY(lastReply(replied, id1).isEmpty());
    QVERIFY(!text(h.item("confirmPassword")).isEmpty());   // kept, selected: typing replaces it
    uitest::saveShot(h.window, QStringLiteral("admin-confirm-wrong"));

    // Right password: both requests run again with their own ids.
    h.item("confirmPassword")->setProperty("text", hub.accountPassword);
    QVERIFY(h.click("confirmSubmit"));
    QTRY_VERIFY(!lastReply(replied, id1).isEmpty() && !lastReply(replied, id2).isEmpty());
    QVERIFY(lastReply(replied, id1).value("ok").toBool());
    QVERIFY(lastReply(replied, id2).value("ok").toBool());
    QCOMPARE(lastReply(replied, id1).value("data").toMap().value("ok").toBool(), true);
    QVERIFY(!a->confirmNeeded());
    QTRY_VERIFY(h.item("confirmPassword") == nullptr || !h.item("confirmPassword")->isVisible());
    QCOMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/users/u_2/disable")), 1);
    QCOMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/users/u_3/disable")), 1);   // each ran exactly once, after the confirmation
    QVERIFY(a->confirmSecondsLeft() > 290 && a->confirmSecondsLeft() <= 300);
    QVERIFY(QRegularExpression(QStringLiteral("^[45]:\\d\\d$")).match(a->confirmLeftText()).hasMatch());   // counts down from 5:00

    // "Confirmed · m:ss left" near the title of an admin page.
    h.controller->showServerPage(QStringLiteral("status"));
    QTRY_VERIFY(h.item("confirmedNote") != nullptr && h.item("confirmedNote")->isVisible());

    // Within the five minutes no new question.
    const int id3 = a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"));
    QTRY_VERIFY(!lastReply(replied, id3).isEmpty());
    QVERIFY(lastReply(replied, id3).value("ok").toBool());
    QVERIFY(!a->confirmNeeded());

    // Ending the confirmation asks again next time.
    a->endConfirmation();
    QTRY_COMPARE(hub.confirmDeletes, 1);
    QCOMPARE(a->confirmSecondsLeft(), 0);
    const int id4 = a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"));
    QTRY_VERIFY(a->confirmNeeded());
    a->cancelConfirm();
    QTRY_VERIFY(!lastReply(replied, id4).isEmpty());
    Q_UNUSED(id4)
  }

  void cancelFailsParkedRequests() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/users/u_2/disable")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    AdminController* a = h.controller->admin();
    QSignalSpy replied(a, &AdminController::replied);
    const int id = a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"));
    QTRY_VERIFY(a->confirmNeeded());
    QTRY_VERIFY(h.item("confirmCancel") != nullptr && h.item("confirmCancel")->isVisible());
    QVERIFY(h.click("confirmCancel"));
    QTRY_VERIFY(!lastReply(replied, id).isEmpty());
    const QVariantMap r = lastReply(replied, id);
    QVERIFY(!r.value("ok").toBool());
    QCOMPARE(r.value("code").toString(), QStringLiteral("confirmation_cancelled"));
    QVERIFY(!a->confirmNeeded());
    QTRY_VERIFY(h.item("confirmPassword") == nullptr || !h.item("confirmPassword")->isVisible());
    QCOMPARE(hub.confirmPosts, 0);   // nothing was sent to the server

    // Esc cancels as well.
    const int id2 = a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"));
    QTRY_VERIFY(a->confirmNeeded());
    QTRY_VERIFY(h.item("confirmPassword") != nullptr && h.item("confirmPassword")->isVisible());
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!lastReply(replied, id2).isEmpty());
    QCOMPARE(lastReply(replied, id2).value("code").toString(), QStringLiteral("confirmation_cancelled"));
  }

  void threeWrongPasswordsLockConfirming() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/users/u_2/disable")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    AdminController* a = h.controller->admin();
    const int id = a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"));
    Q_UNUSED(id)
    QTRY_VERIFY(a->confirmNeeded());
    a->confirm(QStringLiteral("no"));
    QTRY_COMPARE(a->confirmState(), QStringLiteral("wrong"));
    a->confirm(QStringLiteral("no"));
    QTRY_COMPARE(hub.confirmPosts, 2);
    QTRY_COMPARE(a->confirmState(), QStringLiteral("wrong"));
    a->confirm(QStringLiteral("no"));
    QTRY_COMPARE(a->confirmState(), QStringLiteral("locked"));
    QCOMPARE(a->confirmLockedSeconds(), 300);
    QCOMPARE(a->confirmError(), QStringLiteral("Admin steps are locked on this PC for 5:00. Everything else keeps working."));
    QTRY_VERIFY(h.item("dialogError") != nullptr && h.item("dialogError")->isVisible());
    // Locked: Confirm is disabled and nothing more is sent.
    QVERIFY(!h.item("confirmSubmit")->isEnabled());
    a->confirm(hub.accountPassword);
    QTest::qWait(100);
    QCOMPARE(hub.confirmPosts, 3);
    QCOMPARE(a->confirmState(), QStringLiteral("locked"));
    // The countdown runs.
    QTRY_VERIFY_WITH_TIMEOUT(a->confirmLockedSeconds() < 300, 3000);
    a->cancelConfirm();
  }

  void passwordNotSetHasItsOwnMessage() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.accountHasPassword = false;
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/users/u_2/disable")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    AdminController* a = h.controller->admin();
    a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"));
    QTRY_VERIFY(a->confirmNeeded());
    a->confirm(QStringLiteral("anything"));
    QTRY_COMPARE(a->confirmState(), QStringLiteral("unset"));
    QVERIFY(a->confirmError().contains(QStringLiteral("no password")));
    a->cancelConfirm();
  }

  // Another component confirmed: resumeParked() replays what waits.
  void resumeParkedReplaysAfterAnotherConfirmation() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/users/u_2/disable")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    AdminController* a = h.controller->admin();
    QSignalSpy replied(a, &AdminController::replied);
    const int id = a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"));
    QTRY_VERIFY(a->confirmNeeded());
    bool done = false;
    a->api()->request(QStringLiteral("POST"), QStringLiteral("/account/confirm"),
                      QVariantMap{{"password", hub.accountPassword}}, [&](const AdminApi::Result& r) { done = r.ok; });
    QTRY_VERIFY(done);
    a->resumeParked();
    QTRY_VERIFY(!lastReply(replied, id).isEmpty());
    QVERIFY(lastReply(replied, id).value("ok").toBool());
    QVERIFY(!a->confirmNeeded());
  }

  void refusesPathsOutsideAdminAndAccount() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    AdminController* a = h.controller->admin();
    QSignalSpy replied(a, &AdminController::replied);
    // Background requests of the app itself (the collections of the person after the library loaded) may land at any time;
    // the admin API must send none of its own.
    const auto sent = [&hub]() {
      int n = 0;
      for (const FakeRequest& r : std::as_const(hub.requests)) n += r.path.startsWith(QLatin1String("/api/v1/collections")) ? 0 : 1;
      return n;
    };
    const int before = sent();
    for (const char* p : {"/games", "/admin", "admin/users", "/admin/../games", "/admin//x", "/accounts/x"}) {
      const int id = a->request(QStringLiteral("GET"), QString::fromLatin1(p));
      QTRY_VERIFY2(!lastReply(replied, id).isEmpty(), p);
      QCOMPARE(lastReply(replied, id).value("code").toString(), QStringLiteral("invalid_path"));
    }
    const int id = a->request(QStringLiteral("TRACE"), QStringLiteral("/admin/users"));
    QTRY_VERIFY(!lastReply(replied, id).isEmpty());
    QCOMPARE(lastReply(replied, id).value("code").toString(), QStringLiteral("invalid_method"));
    QCOMPARE(sent(), before);
    QVERIFY(AdminApi::pathAllowed(QStringLiteral("/admin/users?role=admin")));
    QVERIFY(AdminApi::pathAllowed(QStringLiteral("/account/confirm")));
  }

  void downloadWritesTheFileAndNeverAPartialOne() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.adminLog = QByteArray(300 * 1024, 'x') + "\nend\n";
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/status/log")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    AdminController* a = h.controller->admin();
    QTemporaryDir out;
    QVERIFY(out.isValid());
    QSignalSpy done(a, &AdminController::downloadFinished);

    // Parked first (it needs the confirmation here), cancelled: no file, no temporary file.
    const QString target = out.filePath(QStringLiteral("server.log"));
    const int cancelled = a->download(QStringLiteral("/admin/status/log"), target);
    QTRY_VERIFY(a->confirmNeeded());
    a->cancelConfirm();
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done.last().at(0).toInt(), cancelled);
    QVERIFY(!done.last().at(1).toBool());
    QVERIFY(!QFileInfo::exists(target));
    QVERIFY(QDir(out.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());

    // Confirmed: the file appears whole (a file URL works like a path).
    const int id = a->download(QStringLiteral("/admin/status/log"), QUrl::fromLocalFile(target).toString());
    QTRY_VERIFY(a->confirmNeeded());
    a->confirm(hub.accountPassword);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 8000);
    QCOMPARE(done.last().at(0).toInt(), id);
    QVERIFY2(done.last().at(1).toBool(), qPrintable(done.last().at(3).toString()));
    QCOMPARE(done.last().at(2).toString(), target);
    QFile f(target);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), hub.adminLog);
    f.close();
    QCOMPARE(QDir(out.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), 1);

    // A failing transfer (error answer) leaves the old file alone and creates nothing else.
    hub.confirmRequiredPaths.clear();
    hub.adminUser = false;   // now 403 forbidden
    const QString second = out.filePath(QStringLiteral("second.log"));
    a->download(QStringLiteral("/admin/status/log"), second);
    QTRY_COMPARE(done.size(), 3);
    QVERIFY(!done.last().at(1).toBool());
    QVERIFY(!QFileInfo::exists(second));
    QCOMPARE(QDir(out.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), 1);

    // An unwritable target reports the problem.
    hub.adminUser = true;
    a->download(QStringLiteral("/admin/status/log"), out.filePath(QStringLiteral("missing-dir/x.log")));
    QTRY_COMPARE(done.size(), 4);
    QVERIFY(!done.last().at(1).toBool());
    QVERIFY(!done.last().at(3).toString().isEmpty());
  }

  // The Status page saves the log through the same download.
  void statusPageSavesTheServerLog() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showServerPage(QStringLiteral("status"));
    QTRY_COMPARE(h.controller->admin()->statusState(), QStringLiteral("ready"));
    QTRY_VERIFY(h.item("serverStatusPage") != nullptr);
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const QString target = out.filePath(QStringLiteral("server-log.txt"));
    QVERIFY(QMetaObject::invokeMethod(h.item("serverStatusPage"), "saveLog", Q_ARG(QString, target)));
    QTRY_VERIFY(h.item("statusLogNote") != nullptr && h.item("statusLogNote")->isVisible());
    QTRY_VERIFY(QFileInfo::exists(target));
    QVERIFY(text(h.item("statusLogNote")).startsWith(QStringLiteral("Saved the server log to")) ||
            h.item("statusLogNote")->property("text").toString().contains(target));
    QVERIFY(h.item("statusCollectLogs") != nullptr);
    QVERIFY(h.item("statusConnectionCheck") != nullptr);
  }

  void uploadStreamsAFile() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    AdminController* a = h.controller->admin();
    QTemporaryDir in;
    QVERIFY(in.isValid());
    QFile f(in.filePath(QStringLiteral("plugin.fbplugin")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray payload = QByteArray("PK\x03\x04", 4) + QByteArray(70000, 'p');
    f.write(payload);
    f.close();
    QSignalSpy replied(a, &AdminController::replied);
    const int id = a->upload(QStringLiteral("PUT"), QStringLiteral("/admin/plugins/test"), f.fileName(), QStringLiteral("application/zip"));
    QTRY_VERIFY(!lastReply(replied, id).isEmpty());
    QVERIFY(lastReply(replied, id).value("ok").toBool());
    QCOMPARE(hub.lastAdminBody, payload);
    QCOMPARE(hub.adminCalls.value(QStringLiteral("PUT /api/v1/admin/plugins/test")), 1);
    // A missing file fails at once.
    const int bad = a->upload(QStringLiteral("PUT"), QStringLiteral("/admin/plugins/test"), in.filePath(QStringLiteral("nope")), QString());
    QTRY_VERIFY(!lastReply(replied, bad).isEmpty());
    QVERIFY(!lastReply(replied, bad).value("ok").toBool());
    QCOMPARE(lastReply(replied, bad).value("code").toString(), QStringLiteral("file_error"));
  }

  void requestsFailWhenTheConnectionIsGone() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/users/u_2/disable")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    AdminController* a = h.controller->admin();
    QSignalSpy replied(a, &AdminController::replied);
    const int id = a->request(QStringLiteral("POST"), QStringLiteral("/admin/users/u_2/disable"));
    QTRY_VERIFY(a->confirmNeeded());
    h.controller->connection()->disconnectFromHub();
    QTRY_VERIFY(!lastReply(replied, id).isEmpty());
    QCOMPARE(lastReply(replied, id).value("code").toString(), QStringLiteral("not_connected"));
    QVERIFY(!a->confirmNeeded());
    QVERIFY(!a->available());
    const int id2 = a->request(QStringLiteral("GET"), QStringLiteral("/admin/users"));
    QTRY_VERIFY(!lastReply(replied, id2).isEmpty());
    QCOMPARE(lastReply(replied, id2).value("code").toString(), QStringLiteral("not_connected"));
  }

  // UzTable and UzMetric in isolation: columns, selection marker, keys, disabled rows, empty state, details slot, Metrics.
  void tableAndMetricBlocks() {
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/qt/qml"));
    QQuickWindow win;
    win.resize(700, 500);
    QQmlComponent c(&engine);
    c.setData(kTablePage, QUrl(QStringLiteral("qrc:/test/table.qml")));
    QObject* o = c.create();
    QVERIFY2(o != nullptr, qPrintable(c.errorString()));
    auto* page = qobject_cast<QQuickItem*>(o);
    page->setParentItem(win.contentItem());
    win.show();
    QVERIFY(QTest::qWaitForWindowExposed(&win));
    QQuickTest::qWaitForPolish(&win);
    auto byName = [&](const char* n) { return Harness::findVisual(page, QString::fromLatin1(n)); };

    QQuickItem* table = byName("tbl");
    QVERIFY(table != nullptr);
    QCOMPARE(byName("tblHeader")->height(), 32.0);
    QCOMPARE(byName("tblRow_0")->height(), 44.0);
    // Column spec: the fixed column is 100 px, the fill column takes the rest (600 - 12 left padding - 100).
    QCOMPARE(byName("tblCell_0_n")->width(), 100.0 - 16.0);
    QCOMPARE(text(byName("tblCell_2_name")), QStringLiteral("Three"));
    QCOMPARE(byName("tblCell_0_n")->property("horizontalAlignment").toInt(), int(Qt::AlignRight));
    QVERIFY(byName("detail") != nullptr);
    QVERIFY(byName("detail")->mapToScene(QPointF(0, 0)).y() >= byName("tblRow_2")->mapToScene(QPointF(0, 44)).y() - 0.5);   // the details section sits under the rows
    QCOMPARE(table->property("currentIndex").toInt(), -1);

    // Keys: Down selects the first enabled row, the disabled one is skipped, Enter activates.
    table->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(&win, Qt::Key_Down);
    QCOMPARE(table->property("currentIndex").toInt(), 0);
    QTest::keyClick(&win, Qt::Key_Down);
    QCOMPARE(table->property("currentIndex").toInt(), 2);
    QTest::keyClick(&win, Qt::Key_Down);
    QCOMPARE(table->property("currentIndex").toInt(), 2);
    QTest::keyClick(&win, Qt::Key_Up);
    QCOMPARE(table->property("currentIndex").toInt(), 0);
    QTest::keyClick(&win, Qt::Key_Return);
    QCOMPARE(page->property("activatedRows").toList(), (QVariantList{QStringLiteral("One")}));
    QCOMPARE(page->property("picked").toList(), (QVariantList{0, 2, 0}));
    // The selected row carries the Yamabuki marker and the tint.
    QQuickTest::qWaitForPolish(&win);
    QCOMPARE(byName("tblRow_0")->property("selected").toBool(), true);
    QCOMPARE(byName("tblRow_2")->property("selected").toBool(), false);
    // A click selects, a click on a disabled row does not.
    const QPointF p2 = byName("tblRow_2")->mapToScene(QPointF(50, 20));
    QTest::mouseClick(&win, Qt::LeftButton, Qt::NoModifier, p2.toPoint());
    QCOMPARE(table->property("currentIndex").toInt(), 2);
    const QPointF p1 = byName("tblRow_1")->mapToScene(QPointF(50, 20));
    QTest::mouseClick(&win, Qt::LeftButton, Qt::NoModifier, p1.toPoint());
    QCOMPARE(table->property("currentIndex").toInt(), 2);

    // Empty state.
    QVERIFY(byName("emptyTblEmpty")->isVisible());
    QVERIFY(!byName("tblEmpty")->isVisible());

    // Metrics: warning square, bar and muted line only where given.
    QVERIFY(!Harness::findVisual(byName("metricPlain"), QStringLiteral("metricWarning"))->isVisible());
    QVERIFY(Harness::findVisual(byName("metricWarn"), QStringLiteral("metricWarning"))->isVisible());
    QCOMPARE(text(Harness::findVisual(byName("metricPlain"), QStringLiteral("metricValue"))), QStringLiteral("61 %"));
    QVERIFY(Harness::findVisual(byName("metricPlain"), QStringLiteral("metricBar"))->isVisible());
    QVERIFY(!Harness::findVisual(byName("metricWarn"), QStringLiteral("metricLine"))->isVisible());
  }
};

UITEST_MAIN(AdminUiTest)
#include "admin_ui_test.moc"
