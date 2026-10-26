// Admin area (0.18 package 8): the Users page (People, Invites, Devices) and the Reports page against FakeHub's admin directory
// fixtures: lists render, create invite shows the code once, approve and deny call the right routes, turn off with
// confirmation_required goes through the confirm dialog and replays, last admin, reports list, mark as seen, download one file,
// badges in AppNav. Offscreen; synthetic data only.
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

class AdminUsersUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    QTRY_VERIFY(h.controller->admin()->available());
    h.window->resize(1280, 800);
  }
  static void adminHub(FakeHub& hub) {
    hub.adminUser = true;
    hub.adminDirectory = true;
    hub.features = {QStringLiteral("saves_v1")};
  }
  static QString text(QQuickItem* it) { return it ? it->property("text").toString() : QString(); }
  static bool shown(QQuickItem* it) { return it != nullptr && it->isVisible(); }
  static void openUsers(Harness& h) {
    h.controller->showServerPage(QStringLiteral("users"));
    QTRY_VERIFY(h.item("serverUsersPage") != nullptr);
    QTRY_VERIFY(shown(h.item("peopleTableRow_2")));
  }
  static void openReports(Harness& h) {
    h.controller->showServerPage(QStringLiteral("reports"));
    QTRY_VERIFY(h.item("serverReportsPage") != nullptr);
    QTRY_VERIFY(shown(h.item("reportsTableRow_1")));
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // The list, the roles and statuses from real data, the badges, the last-admin rule.
  void peopleListRendersAndGuardsTheLastAdmin() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    // Badges of AppNav come from the status poll: 2 waiting devices, 1 new report.
    QTRY_VERIFY(shown(h.item("navServerUsersBadge")));
    QTRY_VERIFY(shown(h.item("navServerReportsBadge")));
    QCOMPARE(h.controller->admin()->pendingDevices(), 2);
    QCOMPARE(h.controller->admin()->newReports(), 1);

    openUsers(h);
    QCOMPARE(text(h.item("pageTitle")), QStringLiteral("Users"));
    QCOMPARE(text(h.item("personName_u_test_1")), QStringLiteral("Tester"));
    QCOMPARE(text(h.item("personName_u_2")), QStringLiteral("Lena"));
    QCOMPARE(text(h.item("peopleTableCell_0_role")), QStringLiteral("Admin"));
    QCOMPARE(text(h.item("peopleTableCell_1_role")), QStringLiteral("Member"));
    QCOMPARE(text(h.item("peopleTableCell_0_status")), QStringLiteral("Online"));
    QVERIFY2(text(h.item("peopleTableCell_1_status")).startsWith(QStringLiteral("Offline · ")), qPrintable(text(h.item("peopleTableCell_1_status"))));
    QCOMPARE(text(h.item("peopleTableCell_1_devices")), QStringLiteral("1"));
    // The Devices tab carries the number of waiting requests.
    QTRY_VERIFY(shown(h.item("tabCount_Devices")));
    QCOMPARE(text(h.item("tabCountText_Devices")), QStringLiteral("2"));
    QVERIFY(h.item("peopleDetails") == nullptr || !h.item("peopleDetails")->isVisible());

    // The only admin: Turn off and Make member are disabled with the reason.
    QVERIFY(h.click("peopleTableRow_0"));
    QTRY_VERIFY(shown(h.item("peopleDetails")));
    QVERIFY(!h.item("personTurnOff")->isEnabled());
    QCOMPARE(text(h.item("personHint")), QStringLiteral("Make someone else admin first."));
    QVERIFY(!shown(h.item("personRemove")));
    QCOMPARE(text(h.item("detailRole")), QStringLiteral("Admin"));
    QVERIFY(!h.item("personResetPassword")->isEnabled());   // your own password is not reset here
    uitest::saveShot(h.window, QStringLiteral("admin-users-people"));

    // A member: details with devices, all actions available.
    QVERIFY(h.click("peopleTableRow_1"));
    QTRY_COMPARE(text(h.item("detailDevices")), QStringLiteral("Lena-Laptop"));
    QVERIFY(h.item("personTurnOff")->isEnabled());
    QVERIFY(shown(h.item("personRemove")));
    QVERIFY(shown(h.item("personMakeAdmin")));
    QVERIFY(text(h.item("personHint")).isEmpty() || !shown(h.item("personHint")));
  }

  // Readable pass 7: role chips with counts filter the list; every person has the "⋯" menu; open invites are rows of their own.
  void roleChipsFilterThePeopleList() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openUsers(h);
    const auto chipCount = [&h](const char* name) { return h.item(name) ? h.item(name)->property("count").toInt() : -1; };
    QTRY_COMPARE(chipCount("usersRoleChip_all"), 3);
    QCOMPARE(chipCount("usersRoleChip_admin"), 1);
    QCOMPARE(chipCount("usersRoleChip_member"), 2);
    QCOMPARE(chipCount("usersRoleChip_invited"), 1);
    QVERIFY(h.item("usersRoleChip_all")->property("active").toBool());
    QVERIFY(shown(h.item("personMenu_u_2")));   // "⋯" per person
    QVERIFY(h.click("usersRoleChip_admin"));
    QTRY_VERIFY(!shown(h.item("peopleTableRow_1")));
    QCOMPARE(text(h.item("peopleTableCell_0_role")), QStringLiteral("Admin"));
    QVERIFY(h.click("usersRoleChip_member"));
    QTRY_VERIFY(shown(h.item("peopleTableRow_1")));
    QCOMPARE(text(h.item("peopleTableCell_0_role")), QStringLiteral("Member"));
    QVERIFY(!shown(h.item("peopleTableRow_2")));
    QVERIFY(h.click("usersRoleChip_invited"));
    QTRY_COMPARE(text(h.item("peopleTableCell_0_role")), QStringLiteral("Invited"));
    QCOMPARE(text(h.item("peopleTableCell_0_status")), QStringLiteral("Not joined yet"));
    QVERIFY(shown(h.item("withdrawOpenInvite_i_1")));
    QVERIFY(!shown(h.item("peopleTableRow_1")));
    QVERIFY(h.click("usersRoleChip_all"));
    QTRY_VERIFY(shown(h.item("peopleTableRow_2")));
  }

  void createInviteShowsTheCodeOnce() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openUsers(h);
    QVERIFY(h.click("usersCreateInvite"));
    QTRY_VERIFY(shown(h.item("usersInviteDialog")));
    QVERIFY(h.click("inviteCreate"));
    QTRY_VERIFY(shown(h.item("usersCodeDialog")));
    QCOMPARE(text(h.item("codeValue")), QStringLiteral("UZ-8XK2-M4QD"));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/invites")), 1);
    const QJsonObject sent = QJsonDocument::fromJson(hub.lastAdminBody).object();
    QCOMPARE(sent.value(QStringLiteral("expiry")).toString(), QStringLiteral("7d"));
    QCOMPARE(sent.value(QStringLiteral("authorize_device")).toBool(), false);
    QVERIFY(!shown(h.item("usersInviteDialog")));
    uitest::saveShot(h.window, QStringLiteral("admin-users-invite-code"));
    // Copy confirms; Done closes the dialog and the code is gone from the page.
    QVERIFY(h.click("codeCopy"));
    QTRY_VERIFY(shown(h.item("codeCopied")));
    QVERIFY(h.click("codeDone"));
    QTRY_VERIFY(!shown(h.item("usersCodeDialog")));
    QVERIFY(text(h.item("codeValue")).isEmpty());
    // The new invite is listed (without its code: the server never lists codes).
    QTRY_VERIFY(shown(h.item("invitesTableRow_2")));
    QCOMPARE(text(h.item("invitesTableCell_0_status")), QStringLiteral("Open"));
    QCOMPARE(text(h.item("invitesTableCell_2_usedBy")), QStringLiteral("Jonas"));
    QVERIFY(shown(h.item("withdrawInvite_i_new")));
    QVERIFY(!shown(h.item("withdrawInvite_i_2")));
    // Withdraw asks first.
    QVERIFY(h.click("withdrawInvite_i_1"));
    QTRY_VERIFY(shown(h.item("usersAskDialog")));
    QVERIFY(h.click("askConfirm"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("DELETE /api/v1/admin/invites/i_1")), 1);
    QTRY_COMPARE(text(h.item("invitesTableCell_1_status")), QStringLiteral("Withdrawn"));
  }

  void approveAndDenyCallTheRightRoutes() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openUsers(h);
    QVERIFY(h.click("tab_Devices"));
    QTRY_VERIFY(shown(h.item("request_r_1")));
    QCOMPARE(text(h.item("requestName_r_1")), QStringLiteral("Lena-PC"));
    QVERIFY2(text(h.item("requestText_r_1")).startsWith(QStringLiteral("Asks to sign in as Lena.")), qPrintable(text(h.item("requestText_r_1"))));
    QVERIFY(text(h.item("requestText_r_2")).startsWith(QStringLiteral("Asks to join.")));
    QCOMPARE(h.item("approveAs_r_1")->property("current").toString(), QStringLiteral("u_2"));
    // All devices below: sign-out buttons for trusted devices.
    QTRY_VERIFY(shown(h.item("signOutDevice_d_3")));
    uitest::saveShot(h.window, QStringLiteral("admin-users-devices"));

    QVERIFY(h.click("approve_r_1"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/pairing-requests/r_1/allow")), 1);
    QCOMPARE(QJsonDocument::fromJson(hub.lastAdminBody).object().value(QStringLiteral("user_id")).toString(), QStringLiteral("u_2"));
    QTRY_VERIFY(!shown(h.item("request_r_1")));
    QTRY_VERIFY(shown(h.item("usersToast")));

    QVERIFY(h.click("deny_r_2"));
    QTRY_VERIFY(shown(h.item("usersAskDialog")));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/pairing-requests/r_2/deny")), 0);   // asked first
    QVERIFY(h.click("askDanger"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/pairing-requests/r_2/deny")), 1);
    QTRY_VERIFY(shown(h.item("devicesNothingWaiting")));
    QTRY_VERIFY(!shown(h.item("tabCount_Devices")));
    QTRY_VERIFY(!shown(h.item("navServerUsersBadge")));

    // Sign out a device: asks, then revokes it.
    QVERIFY(h.click("signOutDevice_d_3"));
    QTRY_VERIFY(shown(h.item("usersAskDialog")));
    QVERIFY(h.click("askDanger"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/devices/d_3/revoke")), 1);
    QTRY_VERIFY(!shown(h.item("signOutDevice_d_3")));
    QTRY_COMPARE(text(h.item("devicesTableCell_2_seen")), QStringLiteral("Signed out"));
  }

  // Turn off needs "Confirm it's you": the request is parked, the dialog asks, the request replays; Undo turns on again.
  void turnOffGoesThroughTheConfirmDialog() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/users/u_2/disable")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openUsers(h);
    QVERIFY(h.click("peopleTableRow_1"));
    QTRY_VERIFY(shown(h.item("personTurnOff")));
    QVERIFY(h.click("personTurnOff"));
    QTRY_VERIFY(shown(h.item("usersAskDialog")));
    QVERIFY(text(h.item("dialogTitle")).isEmpty() == false);
    QCOMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/users/u_2/disable")), 0);
    QVERIFY(h.click("askDanger"));
    QTRY_VERIFY(h.controller->admin()->confirmNeeded());
    QTRY_VERIFY(shown(h.item("confirmPassword")));
    h.item("confirmPassword")->setProperty("text", hub.accountPassword);
    QVERIFY(h.click("confirmSubmit"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/users/u_2/disable")), 1);
    QTRY_VERIFY(!h.controller->admin()->confirmNeeded());
    QTRY_VERIFY(!shown(h.item("usersAskDialog")));
    QTRY_COMPARE(text(h.item("peopleTableCell_1_status")), QStringLiteral("Can't sign in"));
    QVERIFY(shown(h.item("personTurnOn")));
    QVERIFY(!shown(h.item("personTurnOff")));
    QTRY_VERIFY(shown(h.item("confirmedNote")));
    QTRY_VERIFY(shown(h.item("usersToast")));
    QCOMPARE(h.item("usersToast")->property("action").toString(), QStringLiteral("Undo"));
    QVERIFY(text(h.item("usersToast")).contains(QStringLiteral("Saves, play time and devices stay")));
    uitest::saveShot(h.window, QStringLiteral("admin-users-turned-off"));
    // Undo.
    QMetaObject::invokeMethod(h.item("usersToast"), "actionTriggered");
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/users/u_2/enable")), 1);
    QTRY_VERIFY(text(h.item("peopleTableCell_1_status")).startsWith(QStringLiteral("Offline")));
  }

  void cancelledConfirmationChangesNothing() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.confirmRequiredPaths = {QStringLiteral("/api/v1/admin/users/u_3/disable")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openUsers(h);
    QVERIFY(h.click("peopleTableRow_2"));
    QVERIFY(h.click("personTurnOff"));
    QTRY_VERIFY(shown(h.item("usersAskDialog")));
    QVERIFY(h.click("askDanger"));
    QTRY_VERIFY(shown(h.item("confirmCancel")));
    QVERIFY(h.click("confirmCancel"));
    QTRY_VERIFY(h.item("usersAskDialog")->property("error").toString().contains(QStringLiteral("Nothing has changed")));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/users/u_3/disable")), 0);
    QVERIFY(h.click("askCancel"));
    QTRY_VERIFY(!shown(h.item("usersAskDialog")));
  }

  void resetPasswordShowsTheTemporaryPasswordOnce() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openUsers(h);
    QVERIFY(h.click("peopleTableRow_2"));
    QVERIFY(h.click("personResetPassword"));
    QTRY_VERIFY(shown(h.item("usersAskDialog")));
    QVERIFY(h.click("askDanger"));
    QTRY_VERIFY(shown(h.item("usersCodeDialog")));
    QCOMPARE(text(h.item("codeValue")), QStringLiteral("PLCH-OLDR-PASS-WORD"));
    QVERIFY(h.click("codeDone"));
    QTRY_VERIFY(!shown(h.item("usersCodeDialog")));
    QVERIFY(text(h.item("codeValue")).isEmpty());
  }

  void makeAdminNeedsALongPasswordAndRemoveWarns() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openUsers(h);
    QVERIFY(h.click("peopleTableRow_1"));
    QVERIFY(h.click("personMakeAdmin"));
    QTRY_VERIFY(shown(h.item("usersAdminDialog")));
    QVERIFY(!h.item("adminConfirm")->isEnabled());
    h.item("adminPassword")->setProperty("text", QStringLiteral("short"));
    QVERIFY(!h.item("adminConfirm")->isEnabled());
    h.item("adminPassword")->setProperty("text", QStringLiteral("placeholder-password-123"));
    QTRY_VERIFY(h.item("adminConfirm")->isEnabled());
    QVERIFY(h.click("adminConfirm"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/users/u_2/role")), 1);
    QTRY_COMPARE(text(h.item("peopleTableCell_1_role")), QStringLiteral("Admin"));
    QTRY_VERIFY(!shown(h.item("usersAdminDialog")));
    // Two admins now: the one you are can be made a member, the other cannot be turned off.
    QVERIFY(h.click("peopleTableRow_1"));
    QVERIFY(!h.item("personTurnOff")->isEnabled());
    QVERIFY(text(h.item("personHint")).contains(QStringLiteral("Make Lena a member first.")));

    // Remove warns that the saves go with the account.
    QVERIFY(h.click("peopleTableRow_2"));
    QVERIFY(h.click("personRemove"));
    QTRY_VERIFY(shown(h.item("usersAskDialog")));
    QVERIFY(h.item("usersAskDialog")->property("text").toString().contains(QStringLiteral("saves")));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("DELETE /api/v1/admin/users/u_3")), 0);
    // "Download saves…" saves the zip, tells so and leaves the dialog open (the save dialog is native: the page function it ends in is
    // called directly), then Remove still works.
    QVERIFY(shown(h.item("askSecondary")));
    QCOMPARE(h.item("askSecondary")->property("text").toString(), QStringLiteral("Download saves…"));
    QVERIFY(!shown(h.item("askNote")));
    hub.adminRoutes[QStringLiteral("GET /api/v1/admin/users/u_3/saves/export")] = {200, "PK-fake-zip-bytes"};
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const QString target = out.filePath(QStringLiteral("uzume-saves-test.zip"));
    QVERIFY(QMetaObject::invokeMethod(h.item("serverUsersPage"), "startSavesDownload", Q_ARG(QString, QStringLiteral("u_3")),
                                      Q_ARG(QString, target)));
    QTRY_VERIFY(shown(h.item("askNote")));
    QCOMPARE(text(h.item("askNote")), QStringLiteral("Saves downloaded to uzume-saves-test.zip."));
    QFile f(target);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("PK-fake-zip-bytes"));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("GET /api/v1/admin/users/u_3/saves/export")), 1);
    QVERIFY(shown(h.item("usersAskDialog")));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("DELETE /api/v1/admin/users/u_3")), 0);
    QVERIFY(h.click("askDanger"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("DELETE /api/v1/admin/users/u_3")), 1);
    QTRY_VERIFY(!shown(h.item("peopleTableRow_2")));
  }

  void reportsListMarkSeenAndDownload() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(shown(h.item("navServerReportsBadge")));
    openReports(h);
    QCOMPARE(text(h.item("pageTitle")), QStringLiteral("Reports"));
    QCOMPARE(text(h.item("reportTitle_12")), QStringLiteral("Sound crackles in Multiview"));
    QCOMPARE(text(h.item("reportsTableCell_0_from")), QStringLiteral("Lena"));
    QCOMPARE(text(h.item("reportStatus_12")), QStringLiteral("New"));
    QCOMPARE(text(h.item("reportStatus_11")), QStringLiteral("Seen"));
    QTRY_VERIFY(shown(h.item("tabCount_New")));
    QCOMPARE(text(h.item("tabCountText_New")), QStringLiteral("1"));
    // Tabs filter.
    QVERIFY(h.click("tab_Seen"));
    QTRY_VERIFY(!shown(h.item("reportsTableRow_1")));
    QCOMPARE(text(h.item("reportTitle_11")), QStringLiteral("Touch offset on bottom screen"));
    QVERIFY(h.click("tab_All"));
    QTRY_VERIFY(shown(h.item("reportsTableRow_1")));

    // Details: opening does not mark it seen.
    QVERIFY(h.click("reportsTableRow_0"));
    QTRY_VERIFY(shown(h.item("reportFile_1")));
    QCOMPARE(text(h.item("reportFile_0")), QStringLiteral("app-log.txt"));
    QVERIFY(text(h.item("reportFrom")).startsWith(QStringLiteral("Lena · Lena-PC")));
    QVERIFY(text(h.item("reportLinked")).startsWith(QStringLiteral("Audio underrun")));
    QVERIFY(text(h.item("reportDescription")).contains(QStringLiteral("every few seconds")));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/reports/12/seen")), 0);
    uitest::saveShot(h.window, QStringLiteral("admin-reports"));

    // Download one file (the save dialog is native: the page function it ends in is called directly).
    QTemporaryDir out;
    QVERIFY(out.isValid());
    const QString target = out.filePath(QStringLiteral("app-log.txt"));
    QQuickItem* page = h.item("serverReportsPage");
    QVERIFY(QMetaObject::invokeMethod(page, "startDownload", Q_ARG(QString, QStringLiteral("/admin/reports/12/files/app-log.txt")),
                                      Q_ARG(QString, QStringLiteral("app-log.txt")), Q_ARG(QString, target)));
    QTRY_VERIFY(shown(h.item("reportsNote")) && text(h.item("reportsNote")).startsWith(QStringLiteral("Saved app-log.txt")));
    QFile f(target);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("content of app-log.txt\n"));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("GET /api/v1/admin/reports/12/files/app-log.txt")), 1);

    // Mark as seen: the status changes, the New count and the badge go away.
    QVERIFY(h.click("reportMarkSeen"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/reports/12/seen")), 1);
    QTRY_COMPARE(text(h.item("reportStatus_12")), QStringLiteral("Seen"));
    QTRY_VERIFY(!shown(h.item("tabCount_New")));
    QTRY_VERIFY(!shown(h.item("navServerReportsBadge")));
    QVERIFY(!shown(h.item("reportMarkSeen")));

    // Delete asks first.
    QVERIFY(h.click("reportDelete"));
    QTRY_VERIFY(shown(h.item("reportsDeleteDialog")));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("DELETE /api/v1/admin/reports/12")), 0);
    QVERIFY(h.click("reportDeleteConfirm"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("DELETE /api/v1/admin/reports/12")), 1);
    QTRY_VERIFY(!shown(h.item("reportsTableRow_1")));
  }

  // Uzume 0.19: play time column in People, play time, sessions and last played in the details of the selected person.
  void peopleShowPlayTime() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    hub.fillDirectory();
    for (int i = 0; i < hub.dirUsers.size(); ++i) {
      QJsonObject u = hub.dirUsers.at(i).toObject();
      if (u.value("id").toString() == QLatin1String("u_2")) {
        u.insert("play_seconds", 86400);
        u.insert("sessions", 12);
        u.insert("last_played_at", QDateTime::currentDateTimeUtc().addSecs(-3 * 3600).toString(Qt::ISODate));
      } else if (u.value("id").toString() == QLatin1String("u_3")) {
        u.insert("play_seconds", 1800);
        u.insert("sessions", 1);
      }  // Tester: none sent (an older server): a dash
      hub.dirUsers.replace(i, u);
    }
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    openUsers(h);
    // Select Lena: details with play time, sessions, last played
    QVERIFY(h.click("peopleTableRow_1"));
    QTRY_COMPARE(text(h.item("detailPlayTime")), QStringLiteral("24 h"));
    QCOMPARE(text(h.item("detailSessions")), QStringLiteral("12"));
    QCOMPARE(text(h.item("detailLastPlayed")), QStringLiteral("3 h"));
    QVERIFY(h.click("peopleTableRow_2"));
    QTRY_COMPARE(text(h.item("detailPlayTime")), QStringLiteral("30 min"));
    QCOMPARE(text(h.item("detailLastPlayed")), QStringLiteral("Never"));
    QVERIFY(h.click("peopleTableRow_0"));
    QTRY_COMPARE(text(h.item("detailPlayTime")), QStringLiteral("–"));
    QCOMPARE(text(h.item("detailSessions")), QStringLiteral("–"));
  }

  void reportsEmptyState() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    hub.dirReports = QJsonArray();
    hub.dirUsers = QJsonArray{QJsonObject{{"id", "u_test_1"}, {"display_name", "Tester"}, {"role", "admin"}, {"disabled", false}, {"devices", 0},
                                          {"username", "tester"}, {"created_at", "2026-01-01T00:00:00Z"}}};
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->showServerPage(QStringLiteral("reports"));
    QTRY_VERIFY(shown(h.item("reportsEmpty")));
    QVERIFY(!shown(h.item("reportsTable")));
    QVERIFY(!shown(h.item("navServerReportsBadge")));
  }
};

UITEST_MAIN(AdminUsersUiTest)
#include "admin_users_ui_test.moc"
