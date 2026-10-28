// Error groups (ADR 0035), app side: the opt-in prompt and setting, events reaching the server only with the setting on, and
// Server > Reports > Errors (groups, details, Report on GitHub with lookup and preview, Link issue, Mark as seen, Delete) against
// FakeHub's error group fixtures. Offscreen; synthetic data only.
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
// Answers every request with a fixed body (the GitHub search).
class OneShotHttp : public QObject {
 public:
  OneShotHttp() {
    connect(&server, &QTcpServer::newConnection, this, [this]() {
      QTcpSocket* s = server.nextPendingConnection();
      connect(s, &QTcpSocket::readyRead, this, [this, s]() {
        request += s->readAll();
        if (!request.contains("\r\n\r\n")) return;
        ++requests;
        if (hang) return;
        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
                 "\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
      });
    });
  }
  bool listen() { return server.listen(QHostAddress::LocalHost); }
  QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
  QTcpServer server;
  QByteArray body = R"({"total_count":0,"items":[]})", request;
  int requests = 0;
  bool hang = false;  // read the request, never answer
};
}  // namespace

class ErrorsUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    h.window->resize(1280, 900);
  }
  static void adminHub(FakeHub& hub, bool v2 = true) {
    hub.adminUser = true;
    hub.adminDirectory = true;
    hub.features = {QStringLiteral("saves_v1")};
    if (v2) hub.features << QStringLiteral("reports_v2");
  }
  static QString text(QQuickItem* it) { return it ? it->property("text").toString() : QString(); }
  static bool shown(QQuickItem* it) { return it != nullptr && it->isVisible(); }
  static void openErrors(Harness& h) {
    h.controller->showServerPage(QStringLiteral("reports"));
    QTRY_VERIFY(h.item("serverReportsPage") != nullptr);
    QTRY_VERIFY(shown(h.item("reportsSections")));
    QVERIFY(h.click("reportsSectionErrors"));
    QTRY_VERIFY(shown(h.item("errorsTableRow_2")));
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // ------------------------------------------------------------ opt-in

  void theFirstEventAsksOnceAndAnAnswerEndsIt() {
    Harness h;
    QVERIFY(h.start());
    ErrorsController* e = h.controller->errors();
    QVERIFY(!e->answered());
    QVERIFY(!e->sendAutomatically());
    QVERIFY(!e->promptVisible());
    QVERIFY(h.item("errorsPrompt") == nullptr || !shown(h.item("errorsPrompt")));
    QVERIFY(e->record("sync_failure", "app", "Save sync failed", "upload failed: http_500"));
    QVERIFY(e->promptVisible());
    QTRY_VERIFY(shown(h.item("errorsPrompt")));
    QCOMPARE(e->pending(), 1);
    QVERIFY(h.click("errorsPromptLater"));  // "Not now" answers: off, never asked again
    QVERIFY(e->answered());
    QVERIFY(!e->sendAutomatically());
    QVERIFY(!e->promptVisible());
    QTRY_VERIFY(!shown(h.item("errorsPrompt")));
    QCOMPARE(h.controller->playerSettings()->sendErrors(), std::optional<bool>(false));
    QVERIFY(e->record("core_failure", "core:azahar", "Core failed", "load failed"));
    QVERIFY(!e->promptVisible());
    QCOMPARE(e->pending(), 2);  // still queued locally, never sent
  }

  void sendAutomaticallyAnswerIsKept() {
    Harness h;
    QVERIFY(h.start());
    ErrorsController* e = h.controller->errors();
    e->recordUpdateFailure("The update file could not be written.");
    QTRY_VERIFY(shown(h.item("errorsPrompt")));
    QVERIFY(h.click("errorsPromptAllow"));
    QVERIFY(e->sendAutomatically());
    QVERIFY(e->answered());
    QCOMPARE(h.controller->playerSettings()->sendErrors(), std::optional<bool>(true));
    QVERIFY(!e->promptVisible());
  }

  void settingsRowToggles() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->showSettingsSection(QStringLiteral("diagnostics"));
    QTRY_VERIFY(shown(h.item("sendErrorsRow")));
    QVERIFY(!h.controller->errors()->sendAutomatically());
    QVERIFY(h.click("sendErrorsToggle"));
    QTRY_VERIFY(h.controller->errors()->sendAutomatically());
    QVERIFY(h.controller->errors()->answered());
    QVERIFY(h.click("sendErrorsToggle"));
    QTRY_VERIFY(!h.controller->errors()->sendAutomatically());
    QVERIFY(h.controller->errors()->answered());  // answered stays
  }

  void queuedEventsOfAnEarlierRunAskAtStart() {
    QTemporaryDir dir;
    {
      errors::ErrorQueue q(dir.path());
      QVERIFY(q.add(errors::makeEvent("app_crash", "app", "App closed unexpectedly", "", {"0.24.0", "Linux"})));
    }
    PlayerSettings settings(dir.path());
    ErrorsController::Deps d;
    d.settings = &settings;
    d.dataDir = dir.path();
    ErrorsController c(d);
    QVERIFY(c.promptVisible());
    QCOMPARE(c.pending(), 1);
  }

  void startAfterAnUncleanExitRecordsOneAppCrash() {
    Harness h;
    QVERIFY(h.start());
    ErrorsController* e = h.controller->errors();
    const QString log = h.dir.filePath("player.1.log");
    QFile f(log);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("2026-10-28T10:00:00.000 [I] hub: connected\n2026-10-28T10:00:05.000 [E] emu: core crashed password=hunter2 at C:\\Users\\Anna\\x\n");
    f.close();
    e->noteStart(false, log);
    QCOMPARE(e->pending(), 0);  // a clean previous exit records nothing
    e->noteStart(true, log);
    QCOMPARE(e->pending(), 1);
    const errors::ErrorEvent ev = e->queue()->events().first();
    QCOMPARE(ev.kind, QString("app_crash"));
    QCOMPARE(ev.component, QString("app"));
    QCOMPARE(ev.title, QString("App closed unexpectedly"));
    QVERIFY(ev.message.startsWith("emu: core crashed"));
    QVERIFY(!ev.message.contains("hunter2"));
    e->noteStart(true, h.dir.filePath("missing.log"));  // no log: empty message (a problem of its own, not the same fingerprint)
    QCOMPARE(e->pending(), 2);
    QVERIFY(e->queue()->events().last().message.isEmpty());
    e->noteStart(true, h.dir.filePath("missing.log"));  // the same again within ten minutes is merged
    QCOMPARE(e->pending(), 2);
  }

  void eventsLeaveOnlyWithTheSettingOnAndReports_v2() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    ErrorsController* e = h.controller->errors();
    QTRY_VERIFY(e->serverSupportsErrors());
    e->recordCoreFailure("azahar", "Core failed", "Failed to load C:\\x\\azahar.dll: error 126");
    QTest::qWait(300);
    QCOMPARE(hub.errorBatches.size(), 0);  // unanswered = off
    QCOMPARE(e->pending(), 1);
    e->setSendAutomatically(true);          // turning it on sends what waits
    QTRY_COMPARE_WITH_TIMEOUT(hub.errorBatches.size(), 1, 8000);
    const QJsonObject sent = hub.errorBatches.first().first().toObject();
    QCOMPARE(sent.value("kind").toString(), QString("core_failure"));
    QCOMPARE(sent.value("component").toString(), QString("core:azahar"));
    QVERIFY(!sent.value("message").toString().isEmpty());
    QVERIFY(sent.value("fingerprint").toString().startsWith("uz-"));
    QTRY_COMPARE(e->pending(), 0);
    e->recordPluginError("uzume.soullink", "main.lua:12: attempt to index a nil value");
    QTRY_COMPARE_WITH_TIMEOUT(hub.errorBatches.size(), 2, 8000);
    QCOMPARE(hub.errorBatches.last().first().toObject().value("component").toString(), QString("plugin:uzume.soullink"));
  }

  // ------------------------------------------------------------ view

  void noErrorsViewWithoutReports_v2() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub, false);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->showServerPage(QStringLiteral("reports"));
    QTRY_VERIFY(h.item("serverReportsPage") != nullptr);
    QTRY_VERIFY(shown(h.item("reportsTable")));
    QVERIFY(!shown(h.item("reportsSections")));
    QVERIFY(!shown(h.item("serverErrorsView")));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("GET /api/v1/admin/reports/errors")), 0);
  }

  void groupsListAndDetails() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    openErrors(h);
    QVERIFY(!shown(h.item("reportsTable")));
    QCOMPARE(text(h.item("errorTitle_uz-aaaaaaaaaaaa")), QString("Core failed"));
    QCOMPARE(text(h.item("errorsTableCell_0_component")), QString("core:azahar"));
    QCOMPARE(text(h.item("errorsTableCell_0_count")), QString("7"));
    QCOMPARE(text(h.item("errorsTableCell_0_devices")), QString("2"));
    QCOMPARE(text(h.item("errorsTableCell_0_version")), QString("0.24.0"));
    QVERIFY(shown(h.item("errorNew_uz-aaaaaaaaaaaa")));   // unseen: new mark
    QVERIFY(!shown(h.item("errorNew_uz-bbbbbbbbbbbb")));
    // Opening a group does not mark it seen.
    QVERIFY(h.click("errorsTableRow_0"));
    QTRY_VERIFY(shown(h.item("errorEventHead_0")));
    QCOMPARE(text(h.item("errorComponent")), QString("core_failure · core:azahar"));
    QVERIFY(text(h.item("errorVersions")).contains("0.24.0, 0.23.2"));
    QVERIFY(text(h.item("errorEventHead_0")).contains("Lena-PC"));
    QCOMPARE(text(h.item("errorEventMessage_0")), QString("boom in the core"));
    QVERIFY(shown(h.item("errorReportGithub")));   // issues_base is set, no issue yet
    QVERIFY(!shown(h.item("errorOpenIssue")));
    QVERIFY(shown(h.item("errorMarkSeen")));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("PATCH /api/v1/admin/reports/errors/uz-aaaaaaaaaaaa")), 0);
    uitest::saveShot(h.window, QStringLiteral("admin-errors"));
    // A group with a linked issue offers to open it instead.
    QVERIFY(h.click("errorsTableRow_1"));
    QTRY_VERIFY(shown(h.item("errorOpenIssue")));
    QCOMPARE(text(h.item("errorOpenIssue")), QString("Open issue #4"));
    QVERIFY(!shown(h.item("errorReportGithub")));
    QVERIFY(!shown(h.item("errorMarkSeen")));  // already seen
    // A group without a repository has no GitHub button at all.
    QVERIFY(h.click("errorsTableRow_2"));
    QTRY_VERIFY(shown(h.item("errorLinkIssue")));
    QTRY_COMPARE(text(h.item("errorComponent")), QString("sync_failure · app"));
    QVERIFY(!shown(h.item("errorReportGithub")));
    QVERIFY(!shown(h.item("errorOpenIssue")));
  }

  void markSeenAndDelete() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    openErrors(h);
    QVERIFY(h.click("errorsTableRow_0"));
    QTRY_VERIFY(shown(h.item("errorMarkSeen")));
    QVERIFY(h.click("errorMarkSeen"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("PATCH /api/v1/admin/reports/errors/uz-aaaaaaaaaaaa")), 1);
    QCOMPARE(QJsonDocument::fromJson(hub.lastAdminBody).object(),
             (QJsonObject{{"seen", true}}));
    QTRY_VERIFY(!shown(h.item("errorNew_uz-aaaaaaaaaaaa")));
    QTRY_VERIFY(!shown(h.item("errorMarkSeen")));
    // Delete asks first.
    QVERIFY(h.click("errorDelete"));
    QTRY_VERIFY(shown(h.item("errorsDeleteDialog")));
    QCOMPARE(hub.adminCalls.value(QStringLiteral("DELETE /api/v1/admin/reports/errors/uz-aaaaaaaaaaaa")), 0);
    QVERIFY(h.click("errorsDeleteConfirm"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("DELETE /api/v1/admin/reports/errors/uz-aaaaaaaaaaaa")), 1);
    QTRY_VERIFY(!shown(h.item("errorsTableRow_2")));
    QCOMPARE(hub.dirErrorGroups.size(), 2);
  }

  void linkIssueChecksTheAddress() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    openErrors(h);
    QVERIFY(h.click("errorsTableRow_0"));
    QTRY_VERIFY(shown(h.item("errorLinkIssue")));
    QVERIFY(h.click("errorLinkIssue"));
    QTRY_VERIFY(shown(h.item("errorsLinkDialog")));
    QQuickItem* field = h.item("errorsLinkField");
    QVERIFY(field != nullptr);
    field->setProperty("text", QStringLiteral("http://evil.example/x"));
    QTRY_VERIFY(!h.item("errorsLinkSave")->isEnabled());
    field->setProperty("text", QStringLiteral("https://github.com/phabioo/azahar/issues/31"));
    QTRY_VERIFY(h.item("errorsLinkSave")->isEnabled());
    QVERIFY(h.click("errorsLinkSave"));
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("PATCH /api/v1/admin/reports/errors/uz-aaaaaaaaaaaa")), 1);
    QCOMPARE(QJsonDocument::fromJson(hub.lastAdminBody).object(),
             (QJsonObject{{"issue_url", "https://github.com/phabioo/azahar/issues/31"}}));
    QTRY_VERIFY(shown(h.item("errorOpenIssue")));
    QCOMPARE(text(h.item("errorOpenIssue")), QString("Open issue #31"));
    QVERIFY(!shown(h.item("errorReportGithub")));
  }

  void reportOnGithubFindsAnExistingIssueFirst() {
    OneShotHttp http;
    QVERIFY(http.listen());
    http.body = R"({"items":[{"number":57,"state":"open","html_url":"https://github.com/phabioo/azahar/issues/57"}]})";
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    QStringList opened;
    h.controller->errors()->github()->setApiBase(http.base());
    h.controller->errors()->setOpener([&](const QUrl& u) { opened << u.toString(); return true; });
    openErrors(h);
    QVERIFY(h.click("errorsTableRow_0"));
    QTRY_VERIFY(shown(h.item("errorReportGithub")));
    QVERIFY(h.click("errorReportGithub"));
    QTRY_COMPARE(http.requests, 1);
    QVERIFY(http.request.contains("q=uz-aaaaaaaaaaaa+repo:phabioo/azahar+is:issue"));
    // The hit is stored on the group (PATCH) and offered as "Open issue #57"; no preview, no browser yet.
    QTRY_COMPARE(hub.adminCalls.value(QStringLiteral("PATCH /api/v1/admin/reports/errors/uz-aaaaaaaaaaaa")), 1);
    QCOMPARE(QJsonDocument::fromJson(hub.lastAdminBody).object(),
             (QJsonObject{{"issue_url", "https://github.com/phabioo/azahar/issues/57"}}));
    QTRY_VERIFY(shown(h.item("errorOpenIssue")));
    QCOMPARE(text(h.item("errorOpenIssue")), QString("Open issue #57"));
    QVERIFY(!shown(h.item("errorsPreviewDialog")));
    QVERIFY(opened.isEmpty());
    QVERIFY(h.click("errorOpenIssue"));
    QCOMPARE(opened, QStringList{"https://github.com/phabioo/azahar/issues/57"});
  }

  void reportOnGithubShowsThePublicTextThenOpensThePrefilledAddress() {
    OneShotHttp http;  // no issue found
    QVERIFY(http.listen());
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    QList<QUrl> opened;
    h.controller->errors()->github()->setApiBase(http.base());
    h.controller->errors()->setOpener([&](const QUrl& u) { opened << u; return true; });
    openErrors(h);
    QVERIFY(h.click("errorsTableRow_0"));
    QTRY_VERIFY(shown(h.item("errorReportGithub")));
    QVERIFY(h.click("errorReportGithub"));
    QTRY_VERIFY(shown(h.item("errorsPreviewDialog")));
    QCOMPARE(text(h.item("errorsPreviewTitle")), QString("Core failed (public)"));
    QCOMPARE(text(h.item("errorsPreviewSummary")), QString("Seen 7 times on 2 devices, versions 0.24.0, 0.23.2."));
    QVERIFY(opened.isEmpty());  // nothing opens before the person confirms the preview
    QCOMPARE(hub.adminCalls.value(QStringLiteral("PATCH /api/v1/admin/reports/errors/uz-aaaaaaaaaaaa")), 0);
    uitest::saveShot(h.window, QStringLiteral("admin-errors-preview"));
    QVERIFY(h.click("errorsPreviewOpen"));
    QCOMPARE(opened.size(), 1);
    const QString url = QString::fromLatin1(opened.first().toEncoded());
    QVERIFY(url.startsWith("https://github.com/phabioo/azahar/issues/new?template=bug_report.yml&title=Core%20failed%20%28public%29"));
    QVERIFY(url.contains("&fingerprint=uz-aaaaaaaaaaaa"));
    QVERIFY(url.contains("&app-version=0.24.0"));
    QVERIFY(url.contains("&issue-desc=Seen%207%20times%20on%202%20devices"));
    QVERIFY(url.endsWith("&labels=bug,auto-report"));
    QTRY_VERIFY(!shown(h.item("errorsPreviewDialog")));
  }

  void reportOnGithubWhenTheLookupFailsStillPreviews() {
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->errors()->github()->setApiBase(QStringLiteral("http://127.0.0.1:1"));  // refused
    openErrors(h);
    QVERIFY(h.click("errorsTableRow_0"));
    QTRY_VERIFY(shown(h.item("errorReportGithub")));
    QVERIFY(h.click("errorReportGithub"));
    QTRY_VERIFY(shown(h.item("errorsPreviewDialog")));
  }

  // After a PATCH the detail is read again, so the GitHub text is still the server's public text, never the plain title.
  void reportAfterMarkSeenUsesThePublicTitle() {
    OneShotHttp http;
    QVERIFY(http.listen());
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    QList<QUrl> opened;
    h.controller->errors()->github()->setApiBase(http.base());
    h.controller->errors()->setOpener([&](const QUrl& u) { opened << u; return true; });
    openErrors(h);
    QVERIFY(h.click("errorsTableRow_0"));
    QTRY_VERIFY(shown(h.item("errorMarkSeen")));
    QVERIFY(h.click("errorMarkSeen"));
    QTRY_VERIFY(!shown(h.item("errorMarkSeen")));
    QTRY_VERIFY(shown(h.item("errorEventHead_0")));  // the full detail came back
    QTRY_VERIFY(h.item("errorReportGithub")->isEnabled());
    QVERIFY(h.click("errorReportGithub"));
    QTRY_VERIFY(shown(h.item("errorsPreviewDialog")));
    QCOMPARE(text(h.item("errorsPreviewTitle")), QString("Core failed (public)"));
    QVERIFY(h.click("errorsPreviewOpen"));
    QCOMPARE(opened.size(), 1);
    QVERIFY(QString::fromLatin1(opened.first().toEncoded()).contains("title=Core%20failed%20%28public%29"));
  }

  // Changing the selection during a lookup ends it: the button works again for the next group and for the same one.
  void selectionChangeEndsARunningLookup() {
    OneShotHttp http;
    QVERIFY(http.listen());
    http.hang = true;
    FakeHub hub(QStringLiteral("a"));
    adminHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    h.controller->errors()->github()->setApiBase(http.base());
    openErrors(h);
    QVERIFY(h.click("errorsTableRow_0"));
    QTRY_VERIFY(shown(h.item("errorReportGithub")));
    QTRY_VERIFY(h.item("errorReportGithub")->isEnabled());
    QVERIFY(h.click("errorReportGithub"));
    QTRY_COMPARE(http.requests, 1);
    QTRY_VERIFY(h.item("errorReportGithub")->property("busy").toBool());
    QVERIFY(h.click("errorsTableRow_1"));
    QVERIFY(h.click("errorsTableRow_0"));
    QTRY_VERIFY(shown(h.item("errorReportGithub")));
    QTRY_VERIFY(!h.item("errorReportGithub")->property("busy").toBool());
    QTRY_VERIFY(h.item("errorReportGithub")->isEnabled());
    QVERIFY(h.click("errorReportGithub"));
    QTRY_COMPARE(http.requests, 2);  // a new lookup could start
  }

  void onlyHttpsGithubAddressesOpen() {
    Harness h;
    QVERIFY(h.start());
    ErrorsController* e = h.controller->errors();
    QStringList opened;
    e->setOpener([&](const QUrl& u) { opened << u.toString(); return true; });
    QVERIFY(!e->openInBrowser("http://github.com/a/b"));
    QVERIFY(!e->openInBrowser("file:///etc/passwd"));
    QVERIFY(!e->openInBrowser("https://example.com/a/b"));
    QVERIFY(e->openInBrowser("https://github.com/phabioo/uzume/issues/1"));
    QCOMPARE(opened, QStringList{"https://github.com/phabioo/uzume/issues/1"});
    QVERIFY(e->newIssueUrl("https://example.com/a/b", "t", "uz-0123456789ab", "1", "x").isEmpty());
  }
};

QTEST_MAIN(ErrorsUiTest)
#include "errors_ui_test.moc"
