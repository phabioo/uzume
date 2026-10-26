// Report tools of the debug MCP server against FakeHub's admin directory: list, get, text and binary file, not_admin.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "fakehub.h"
#include "mcp_registry.h"
#include "mcp_tools.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::mcp;
using uitest::Harness;
using S = HubConnection::State;

class McpReportsTest : public QObject {
  Q_OBJECT

  static ToolResult call(McpToolRegistry& reg, const QString& tool, const QJsonObject& args = {}) {
    ToolResult out;
    bool done = false;
    reg.call(tool, args, [&](ToolResult r) {
      out = std::move(r);
      done = true;
    });
    (void)QTest::qWaitFor([&]() { return done; }, 15000);
    if (!done) out = ToolResult::error(QStringLiteral("test_timeout"));
    return out;
  }
  static QString text(const ToolResult& r) {
    QString s;
    for (const ToolContent& c : r.content)
      if (c.kind == ToolContent::Kind::Text) s += c.text;
    return s;
  }
  static QJsonObject json(const ToolResult& r) { return QJsonDocument::fromJson(text(r).toUtf8()).object(); }

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
  }

 private slots:
  void initTestCase() { uitest::installWarningCounter(); }

  void listGetAndFiles() {
    FakeHub hub(QStringLiteral("a"));
    hub.adminUser = true;
    hub.adminDirectory = true;
    hub.features = {QStringLiteral("saves_v1")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY(h.controller->admin()->available());
    McpToolRegistry reg;
    installTools(reg, *h.controller);

    const ToolResult all = call(reg, QStringLiteral("reports_list"));
    QVERIFY2(!all.isError, qPrintable(text(all)));
    QCOMPARE(json(all).value(QStringLiteral("reports")).toArray().size(), 2);
    QCOMPARE(json(all).value(QStringLiteral("count_new")).toInt(), 1);
    const ToolResult fresh = call(reg, QStringLiteral("reports_list"), {{"state", "new"}});
    QCOMPARE(json(fresh).value(QStringLiteral("reports")).toArray().size(), 1);
    QCOMPARE(json(call(reg, QStringLiteral("reports_list"), {{"limit", 1}})).value(QStringLiteral("shown")).toInt(), 1);
    QVERIFY(call(reg, QStringLiteral("reports_list"), {{"state", "archived"}}).isError);

    const ToolResult one = call(reg, QStringLiteral("report_get"), {{"id", 12}});
    QVERIFY2(!one.isError, qPrintable(text(one)));
    const QJsonObject o = json(one);
    QVERIFY(o.value(QStringLiteral("description")).toString().contains(QStringLiteral("every few seconds")));
    QCOMPARE(o.value(QStringLiteral("user")).toString(), QStringLiteral("Lena"));
    QCOMPARE(o.value(QStringLiteral("files")).toArray().size(), 2);
    QCOMPARE(hub.adminCalls.value(QStringLiteral("POST /api/v1/admin/reports/12/seen")), 0);  // reading does not mark it
    QVERIFY(call(reg, QStringLiteral("report_get"), {{"id", "999"}}).isError);

    const ToolResult file = call(reg, QStringLiteral("report_file"), {{"id", "12"}, {"name", "app-log.txt"}});
    QVERIFY2(!file.isError, qPrintable(text(file)));
    QCOMPARE(text(file), QStringLiteral("content of app-log.txt\n"));
    QVERIFY(call(reg, QStringLiteral("report_file"), {{"id", "12"}, {"name", "nope.txt"}}).isError);
    QVERIFY(call(reg, QStringLiteral("report_file"), {{"id", "12"}, {"name", "../x"}}).isError);

    // binary content comes back as base64
    hub.reportFiles.insert(QStringLiteral("12/app-log.txt"), QByteArray("PK\x03\x04\x00zipdata", 10));
    const ToolResult bin = call(reg, QStringLiteral("report_file"), {{"id", "12"}, {"name", "app-log.txt"}});
    QVERIFY2(!bin.isError, qPrintable(text(bin)));
    QCOMPARE(json(bin).value(QStringLiteral("encoding")).toString(), QStringLiteral("base64"));
    QCOMPARE(QByteArray::fromBase64(json(bin).value(QStringLiteral("data")).toString().toLatin1()).left(2), QByteArray("PK"));
  }

  void notAdmin() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    McpToolRegistry reg;
    installTools(reg, *h.controller);
    for (const QString& t : {QStringLiteral("reports_list"), QStringLiteral("report_get"), QStringLiteral("report_file")}) {
      const ToolResult r = call(reg, t, {{"id", "12"}, {"name", "a.txt"}});
      QVERIFY(r.isError);
      QCOMPARE(text(r), QStringLiteral("not_admin"));
    }
  }
};

QTEST_MAIN(McpReportsTest)
#include "mcp_reports_test.moc"
