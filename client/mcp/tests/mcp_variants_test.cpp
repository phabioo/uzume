// Variant tools of the debug MCP server against FakeHub (tools catalog, variants REST) and the test engine: variant_presets,
// tool_prepare, variant_build (wait), variant_status, variant_files, variant_delete brakes. No real ROM or engine.
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtTest>

#include "corecache.h"
#include "engine_pack.h"
#include "fakehub.h"
#include "mcp_registry.h"
#include "mcp_tools.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::mcp;
using uitest::Harness;
using S = HubConnection::State;

class McpVariantsTest : public QObject {
  Q_OBJECT

  static ToolResult call(McpToolRegistry& reg, const QString& tool, const QJsonObject& args = {}) {
    ToolResult out;
    bool done = false;
    reg.call(tool, args, [&](ToolResult r) {
      out = std::move(r);
      done = true;
    });
    (void)QTest::qWaitFor([&]() { return done; }, 60000);
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
  void initTestCase() {
    uitest::installWarningCounter();
    qputenv("UZUME_TEST_ENGINE_DELAY_MS", "");
  }

  void presetsBuildAndFiles() {
    QTemporaryDir tmp;
    QByteArray zip;
    testutil::writeEngineZip(tmp.path(), QStringLiteral(FB_TEST_ENGINE_PATH), QStringLiteral("UPR_FVX-v1_6_2-Test"), false, &zip);
    const QByteArray rom(40000, 'r');

    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("tools_v1"), QStringLiteral("variants_v1")};
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    hub.games = QJsonObject{{QStringLiteral("games"), QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"), uitest::sha256Hex(rom), rom.size())}}};
    QStringList deleted;
    hub.customRoute = [&](const FakeRequest& req, int& status, QByteArray& body, QByteArray& ct) {
      const QString plat = CoreCache::currentPlatform();
      if (req.method == "GET" && req.path == QLatin1String("/api/v1/tools")) {
        QJsonObject p{{QStringLiteral("platform"), plat}, {QStringLiteral("size"), zip.size()}, {QStringLiteral("sha256"), testutil::sha256Hex(zip)}, {QStringLiteral("cached"), true}, {QStringLiteral("root"), QStringLiteral("UPR_FVX-v1_6_2-Test/")}};
        QJsonObject v{{QStringLiteral("version"), QStringLiteral("1.6.2")}, {QStringLiteral("platforms"), QJsonArray{p}}};
        QJsonObject t{{QStringLiteral("id"), QStringLiteral("randomizer")}, {QStringLiteral("name"), QStringLiteral("Randomizer engine")}, {QStringLiteral("offered"), true},
                      {QStringLiteral("version"), QStringLiteral("1.6.2")}, {QStringLiteral("license"), QStringLiteral("GPL-3.0")}, {QStringLiteral("versions"), QJsonArray{v}}};
        body = QJsonDocument(QJsonObject{{QStringLiteral("tools"), QJsonArray{t}}}).toJson(QJsonDocument::Compact);
        return true;
      }
      if (req.method == "GET" && req.path == QLatin1String("/api/v1/tools/randomizer/packages/1.6.2/") + plat) {
        body = zip;
        ct = "application/zip";
        return true;
      }
      if (req.method == "GET" && req.path == QLatin1String("/api/v1/games/g1/variants")) {
        body = R"({"variants":[]})";
        return true;
      }
      if (req.method == "POST" && req.path == QLatin1String("/api/v1/games/g1/variants")) {
        const QJsonObject in = QJsonDocument::fromJson(req.body).object();
        const auto m = QRegularExpression(QStringLiteral("\"seed\"\\s*:\\s*(\\d+)")).match(QString::fromUtf8(req.body));
        body = QStringLiteral(R"({"id":"v1","game_id":"g1","name":"%1","slot":"var-0a1b2c3d","tool_id":"randomizer","tool_version":"1.6.2","settings":"%2","seed":%3,"spoiler_log":false,"run_id":null,"created_at":"2026-10-27T10:00:00Z"})")
                   .arg(in.value(QStringLiteral("name")).toString(), in.value(QStringLiteral("settings")).toString(), m.hasMatch() ? m.captured(1) : QStringLiteral("1"))
                   .toUtf8();
        status = 201;
        return true;
      }
      if (req.method == "DELETE" && req.path.startsWith(QLatin1String("/api/v1/variants/v1"))) {
        deleted << req.path;
        status = 204;
        body = {};
        return true;
      }
      return false;
    };
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    McpToolRegistry reg;
    installTools(reg, *h.controller);

    // no plugin registered the game yet
    QVERIFY(!json(call(reg, QStringLiteral("variant_presets"), {{"game_id", "g1"}})).value(QStringLiteral("supported")).toBool());
    plugins::VariantRegistration r;
    r.pluginId = QStringLiteral("uzume.randomizer");
    r.engine = QStringLiteral("randomizer");
    r.presets = {plugins::VariantPreset{QStringLiteral("chaos"), QStringLiteral("Chaos"), QStringLiteral("hint"), {{QStringLiteral("428"), QStringLiteral("428chaos")}}}};
    r.games = {plugins::VariantGameSupport{QStringLiteral("nds"), {}}};
    h.controller->variants()->registry()->set(r);
    const QJsonObject pr = json(call(reg, QStringLiteral("variant_presets"), {{"game_id", "g1"}}));
    QVERIFY(pr.value(QStringLiteral("supported")).toBool());
    const QJsonArray presets = pr.value(QStringLiteral("presets")).toArray();
    QCOMPARE(presets.size(), 2);  // the registered one plus "custom"
    QCOMPARE(presets.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("chaos"));
    QVERIFY(presets.last().toObject().value(QStringLiteral("custom")).toBool());

    // engine from the (fake) server
    const ToolResult prep = call(reg, QStringLiteral("tool_prepare"));
    QVERIFY2(!prep.isError, qPrintable(text(prep)));
    QCOMPARE(json(prep).value(QStringLiteral("state")).toString(), QStringLiteral("ready"));

    // build, wait
    const ToolResult b = call(reg, QStringLiteral("variant_build"), {{"game_id", "g1"}, {"name", "Run A"}, {"preset_id", "chaos"}, {"settings", "428chaos"}, {"seed", "424242"}});
    QVERIFY2(!b.isError, qPrintable(text(b)));
    QCOMPARE(json(b).value(QStringLiteral("variantId")).toString(), QStringLiteral("v1"));
    QCOMPARE(json(b).value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    const ToolResult st = call(reg, QStringLiteral("variant_status"), {{"id", "v1"}});
    QCOMPARE(json(st).value(QStringLiteral("state")).toString(), QStringLiteral("ready"));

    // the built file: a ROM that differs from the base and is stable
    const ToolResult f1 = call(reg, QStringLiteral("variant_files"), {{"id", "v1"}});
    QVERIFY2(!f1.isError, qPrintable(text(f1)));
    const QJsonArray files = json(f1).value(QStringLiteral("files")).toArray();
    QCOMPARE(files.size(), 1);
    const QJsonObject file = files.first().toObject();
    QCOMPARE(file.value(QStringLiteral("size")).toInt(), rom.size());
    QVERIFY(file.value(QStringLiteral("sha256")).toString() != uitest::sha256Hex(rom));
    QVERIFY(!file.value(QStringLiteral("path")).toString().startsWith(QLatin1Char('/')));
    QCOMPARE(json(call(reg, QStringLiteral("variant_files"), {{"id", "v1"}})).value(QStringLiteral("files")).toArray().first().toObject().value(QStringLiteral("sha256")),
             file.value(QStringLiteral("sha256")));
    QVERIFY(call(reg, QStringLiteral("variant_files"), {{"id", "nope"}}).isError);

    // the listing shows it
    const QJsonArray list = json(call(reg, QStringLiteral("variants_list"), {{"game_id", "g1"}})).value(QStringLiteral("variants")).toArray();
    QVERIFY(list.size() >= 1);

    // deleting the saves needs a confirmation (ADR 0032 D4)
    const ToolResult noConfirm = call(reg, QStringLiteral("variant_delete"), {{"id", "v1"}, {"keep_saves", false}});
    QVERIFY(noConfirm.isError);
    QVERIFY(text(noConfirm).contains(QStringLiteral("confirm_required")));
    QVERIFY(deleted.isEmpty());
    const ToolResult del = call(reg, QStringLiteral("variant_delete"), {{"id", "v1"}, {"keep_saves", false}, {"confirm", true}});
    QVERIFY2(!del.isError, qPrintable(text(del)));
    QTRY_VERIFY_WITH_TIMEOUT(!deleted.isEmpty(), 5000);
    QVERIFY(deleted.first().contains(QStringLiteral("saves=delete")));
  }

  void toolsAndPluginHelpersReportErrors() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    McpToolRegistry reg;
    installTools(reg, *h.controller);
    const ToolResult tl = call(reg, QStringLiteral("tools_list"));
    QVERIFY(!tl.isError);
    QCOMPARE(json(tl).value(QStringLiteral("engine")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("no_server"));
    QVERIFY(call(reg, QStringLiteral("plugin_install_file"), {{"path", "/nonexistent/x.fbplugin"}}).isError);
    QVERIFY(call(reg, QStringLiteral("plugin_set_enabled"), {{"id", "no.such"}, {"enabled", true}}).isError);
    QVERIFY(call(reg, QStringLiteral("variant_start"), {{"id", "nope"}}).isError);
  }
};

QTEST_MAIN(McpVariantsTest)
#include "mcp_variants_test.moc"
