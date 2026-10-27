// MCP tools against a PlayerController without a server (no Hub), local test plugins and the Uzume test core: plugin_call with
// a value and an image, plugin_eval gating, game tools, the opt-in setting and the token never reaching the log.
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

#include "filelog.h"
#include "mcp_test_client.h"
#include "mcp_tools.h"
#include "playercontroller.h"
#include "test_core_layout.h"

using namespace uzume;
using namespace uzume::mcp;
namespace mem = uzume::testcore::mem;

namespace {

quint16 freePort() {
  QTcpServer s;
  s.listen(QHostAddress::LocalHost, 0);
  return s.serverPort();
}

void writeFile(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(data);
}

QString writePlugin(const QString& parent, const QString& id, const QByteArray& lua) {
  const QString dir = QDir(parent).filePath(id);
  QJsonObject m{{"id", id}, {"name", id}, {"version", "1.0.0"}, {"api", 1}, {"type", "script"}, {"entry", "main.lua"},
                {"provides", QJsonArray{"test.iface"}}, {"permissions", QJsonArray{"memory.read"}}};
  writeFile(dir + "/manifest.json", QJsonDocument(m).toJson());
  writeFile(dir + "/main.lua", lua);
  return dir;
}

const char* kPlugin = R"LUA(
fb.provide("test.iface", {
  add = function(a, b) return a + b end,
  info = function() return {name = "x", list = {1, 2, 3}} end,
  img = function() return {width = 2, height = 2, rgba = string.rep("\255\0\0\255", 4)} end,
})
fb.log.info("plugin loaded")
)LUA";

QByteArray pngOfColor(QRgb c) {
  QImage img(3, 2, QImage::Format_RGB32);
  img.fill(c);
  QByteArray out;
  QBuffer b(&out);
  b.open(QIODevice::WriteOnly);
  img.save(&b, "PNG");
  return out;
}

}  // namespace

class McpToolsTest : public QObject {
  Q_OBJECT
  QTemporaryDir dir;
  std::shared_ptr<CredentialStore> creds = std::make_shared<MemoryCredentialStore>();
  std::unique_ptr<ui::PlayerController> pc;

  ui::PlayerController::Options options(const QString& sub) const {
    ui::PlayerController::Options o;
    o.dataDir = dir.filePath(sub);
    o.credentialStore = creds;
    o.probeCoreVersions = false;
    o.enableGamepads = false;
    o.allowHttp = true;
    return o;
  }

  // Runs a tool through the registry and waits for the answer.
  ToolResult call(const QString& name, const QJsonObject& args = {}) {
    bool done = false;
    ToolResult out;
    pc->mcp()->registry()->call(name, args, [&](ToolResult r) {
      out = std::move(r);
      done = true;
    });
    [&] { QTRY_VERIFY_WITH_TIMEOUT(done, 30000); }();
    return out;
  }
  static QJsonValue jsonOf(const ToolResult& r) {
    const QJsonDocument d = QJsonDocument::fromJson(r.content.value(0).text.toUtf8());
    return d.isObject() ? QJsonValue(d.object()) : QJsonValue(d.array());
  }
  static int images(const ToolResult& r) {
    int n = 0;
    for (const ToolContent& c : r.content) n += c.kind == ToolContent::Kind::Image;
    return n;
  }

 private slots:
  void init() {
    pc = std::make_unique<ui::PlayerController>(options(QStringLiteral("a")));
    pc->mcp()->setToolInstaller([this](McpToolRegistry& r) { installTools(r, *pc); });
  }
  void cleanup() { pc.reset(); }

  void allToolsAreRegistered() {
    QStringList names;
    for (const ToolDef& t : pc->mcp()->registry()->tools()) {
      names << t.name;
      QVERIFY2(!t.description.isEmpty(), qPrintable(t.name));
      QCOMPARE(t.inputSchema.value("type").toString(), QStringLiteral("object"));
    }
    for (const char* n : {"status", "logs_tail", "logs_search", "settings_get", "collect_diagnostics", "connection_check", "plugins_list", "plugin_console", "plugin_ui",
                          "plugin_action", "plugin_reload", "plugin_call", "plugin_eval", "plugin_cache_list", "plugin_cache_read", "plugin_image", "game_state",
                          "devconsole_state", "memory_regions", "memory_read", "memory_search", "romfs_list", "romfs_read", "rom_read", "screenshot", "game_input", "game_pause",
                          "game_resume", "game_frame_step", "game_state_save", "game_state_load", "game_memory_write", "cheats_list", "cheats_apply",
                          "cheats_clear", "store_get", "store_put", "consent_set", "server_sign_in", "states_list", "states_save", "states_load",
                          "states_delete", "rewind", "runs_list", "run_set_rules", "run_events", "plugin_link", "developer_mode", "file_dialog_pick", "settings_set", "saves_history", "toasts_list",
                          "run_recap", "together_state", "notifications_act", "session_share",
                          "session_stop_sharing", "together_ready_check", "together_pause", "chat_send", "chat_history", "session_quality", "session_viewers"})
      QVERIFY2(names.contains(QLatin1String(n)), n);
  }

  void statusAndSettings() {
    const QJsonObject s = jsonOf(call("status")).toObject();
    QVERIFY(!s.value("app").toObject().value("version").toString().isEmpty());
    QVERIFY(s.contains("servers"));
    const QJsonObject discord = s.value("discord").toObject();  // Rich Presence: off by default, nothing sent
    QVERIFY(discord.contains("enabled") && !discord.value("enabled").toBool());
    QVERIFY(!discord.value("connected").toBool());
    QVERIFY(discord.value("details").toString().isEmpty());
    QVERIFY(!s.value("data_dir").toString().isEmpty());
    const QString text = QJsonDocument(s).toJson();
    QVERIFY(!text.contains("token", Qt::CaseInsensitive));
    QVERIFY(jsonOf(call("settings_get")).toObject().contains("settings"));
    QVERIFY(call("game_state").isError == false);
    QVERIFY(call("memory_regions").isError);  // no game
    QCOMPARE(call("memory_read", {{"addr", 0}, {"len", 4}}).content.value(0).text, QStringLiteral("no_game"));
    QCOMPARE(call("game_state_save").content.value(0).text, QStringLiteral("no_game"));
    QCOMPARE(call("game_state_load").content.value(0).text, QStringLiteral("no_game"));
    QCOMPARE(call("game_memory_write", {{"addr", 0}, {"hex", "00"}}).content.value(0).text, QStringLiteral("no_game"));
    // 0.20 package 4: the app's real save states and rewind, argument checks and no game.
    for (const char* tool : {"states_save", "states_load", "states_delete"}) QVERIFY2(call(tool, {{"slot", 9}}).isError, tool);
    for (const char* tool : {"states_save", "states_load"})
      QVERIFY2(call(tool, {{"slot", 1}}).content.value(0).text.startsWith(QStringLiteral("no_game")), tool);
    QVERIFY(call("states_delete", {{"slot", 1}}).isError);
    QVERIFY(call("rewind", {{"seconds", 0}}).isError);
    QVERIFY(call("rewind").isError);
    QVERIFY(call("rewind", {{"seconds", 2}}).content.value(0).text.startsWith(QStringLiteral("no_game")));
    QVERIFY(!call("states_list").isError);
  }

  // 0.20 plugin API debug tools: without a game the cheat tools refuse, the plugin store and the consent answers work.
  void pluginApiDebugTools() {
    QCOMPARE(call("cheats_list").content.value(0).text, QStringLiteral("no_game"));
    QCOMPARE(call("cheats_apply", {{"cheats", QJsonArray()}}).content.value(0).text, QStringLiteral("no_game"));
    QCOMPARE(call("cheats_clear").content.value(0).text, QStringLiteral("no_game"));
    QCOMPARE(call("consent_set", {{"kind", "cheats"}, {"allow", true}}).content.value(0).text, QStringLiteral("no_game"));
    QVERIFY(call("consent_set", {{"kind", "bogus"}, {"allow", true}}).isError);
    const QJsonObject c = jsonOf(call("consent_set", {{"kind", "memory_write"}, {"allow", true}, {"game", "g-mcp-test"}})).toObject();
    QCOMPARE(c.value("consent").toString(), QStringLiteral("allowed"));
    QCOMPARE(jsonOf(call("consent_set", {{"kind", "memory_write"}, {"forget", true}, {"game", "g-mcp-test"}})).toObject().value("consent").toString(), QStringLiteral("ask"));

    // An unknown plugin id is an error; a plugin that already has data is known, so put first.
    QCOMPARE(call("store_get", {{"plugin_id", "uzume.cheats"}, {"key", "lists"}}).content.value(0).text, QStringLiteral("plugin_not_found"));
    QVERIFY(call("store_get", {{"plugin_id", "Bad Id"}, {"key", "lists"}}).isError);
    const QJsonObject put = jsonOf(call("store_put", {{"plugin_id", "uzume.cheats"}, {"key", "lists"}, {"value", QJsonObject{{"a", 1}}}})).toObject();
    QCOMPARE(put.value("result").toString(), QStringLiteral("ok"));
    QCOMPARE(put.value("revision").toInt(), 1);
    const QJsonObject got = jsonOf(call("store_get", {{"plugin_id", "uzume.cheats"}, {"key", "lists"}})).toObject();
    QVERIFY(got.value("found").toBool());
    QCOMPARE(got.value("value").toObject().value("a").toInt(), 1);
    const QJsonObject stale = jsonOf(call("store_put", {{"plugin_id", "uzume.cheats"}, {"key", "lists"}, {"value", 2}, {"base_revision", 0}})).toObject();
    QCOMPARE(stale.value("result").toString(), QStringLiteral("conflict"));
    QCOMPARE(jsonOf(call("store_get", {{"plugin_id", "uzume.cheats"}, {"key", "lists"}})).toObject().value("conflicts").toArray().size(), 1);
  }

  // 0.20 package 6 run and plugin helper tools without a server and without a game.
  void runAndPluginHelperTools() {
    QCOMPARE(call("runs_list").content.value(0).text, QStringLiteral("not_supported"));
    QVERIFY(call("run_set_rules", {{"run_id", "r1"}, {"cheats", "logged"}}).isError);
    QVERIFY(call("run_events", {{"run_id", "r1"}}).isError);
    QCOMPARE(call("developer_mode").content.value(0).text.contains("developer_mode"), true);
    QCOMPARE(jsonOf(call("developer_mode", {{"enabled", true}})).toObject().value("developer_mode").toBool(), true);
    QCOMPARE(call("plugin_link", {{"path", dir.filePath("nope")}}).content.value(0).text, QStringLiteral("path not found"));
    QCOMPARE(call("file_dialog_pick", {{"path", ""}}).content.value(0).text, QStringLiteral("no_pick_request"));
    QCOMPARE(jsonOf(call("developer_mode", {{"enabled", false}})).toObject().value("developer_mode").toBool(), false);
    QCOMPARE(call("plugin_link", {{"path", dir.path()}}).content.value(0).text, QStringLiteral("developer_mode_off"));
  }

  // 0.20 final live check: settings_set, saves_history, toasts_list, plugin id under both names, hold_ms and confirm in the schema.
  void liveCheckGapTools() {
    QCOMPARE(call("settings_set", {{"key", "uzume.nope"}, {"value", "1"}}).content.value(0).text, QStringLiteral("unknown_key"));
    QCOMPARE(call("settings_set", {{"key", "uzume.run_ahead"}, {"value", "7"}}).content.value(0).text, QStringLiteral("invalid_value"));
    QCOMPARE(call("settings_set", {{"key", "uzume.rewind_seconds"}, {"value", "601"}}).content.value(0).text, QStringLiteral("invalid_value"));
    const QJsonObject set = jsonOf(call("settings_set", {{"key", "uzume.rewind_seconds"}, {"value", 13}})).toObject();
    QCOMPARE(set.value("value").toString(), QStringLiteral("13"));
    QCOMPARE(pc->emulation()->uzumeValue(QStringLiteral("uzume.rewind_seconds"), QStringLiteral("nds")), QStringLiteral("13"));
    QCOMPARE(jsonOf(call("settings_set", {{"key", "uzume.run_ahead"}, {"value", "1"}, {"system", "gba"}})).toObject().value("value").toString(), QStringLiteral("1"));
    QCOMPARE(pc->emulation()->uzumeValue(QStringLiteral("uzume.run_ahead"), QStringLiteral("nds")), QStringLiteral("0"));
    QCOMPARE(jsonOf(call("settings_set", {{"key", "uzume.rewind_seconds"}, {"value", "0"}})).toObject().value("value").toString(), QStringLiteral("0"));

    QVERIFY(!call("saves_history").isError);
    QVERIFY(jsonOf(call("saves_history")).toObject().contains("versions"));
    QVERIFY(jsonOf(call("toasts_list")).toObject().contains("toasts"));

    // The plugin is `id` for most tools and `plugin_id` for others: both spellings reach the handler.
    call("store_put", {{"plugin_id", "uzume.cheats"}, {"key", "k"}, {"value", 1}});
    QVERIFY(jsonOf(call("store_get", {{"id", "uzume.cheats"}, {"key", "k"}})).toObject().value("found").toBool());
    QCOMPARE(call("plugin_console", {{"plugin_id", "uzume.nothing"}}).content.value(0).text, QStringLiteral("plugin_not_running"));
    QCOMPARE(call("file_dialog_pick", {{"id", "uzume.cheats"}, {"path", ""}}).content.value(0).text, QStringLiteral("no_pick_request"));

    for (const ToolDef& t : pc->mcp()->registry()->tools()) {
      if (t.name == QLatin1String("ui_key")) {
        QVERIFY(t.inputSchema.value("properties").toObject().contains("hold_ms"));
        QVERIFY(t.inputSchema.value("properties").toObject().contains("confirm"));
      }
      if (t.name == QLatin1String("ui_click")) QVERIFY(t.inputSchema.value("properties").toObject().contains("confirm"));
    }
  }

  void togetherToolsReadAndActOnNotifications() {
    QJsonObject st = jsonOf(call("together_state")).toObject();
    QCOMPARE(st.value("unread").toInt(), 0);
    QVERIFY(st.value("roster").toArray().isEmpty());
    QVERIFY(call("notifications_act", {{"index", 0}}).isError);  // nothing there yet
    QVERIFY(call("notifications_act").isError);
    pc->notifications()->notify(QStringLiteral("update"), QStringLiteral("Uzume 9.9.9 is available"), QStringLiteral("WHAT'S NEW"), QStringLiteral("settings:updates"));
    st = jsonOf(call("together_state")).toObject();
    QCOMPARE(st.value("unread").toInt(), 1);
    const QJsonObject n = st.value("notifications").toArray().at(0).toObject();
    QCOMPARE(n.value("kind").toString(), QStringLiteral("update"));
    QCOMPARE(n.value("action").toString(), QStringLiteral("WHAT'S NEW"));
    QVERIFY(!n.value("read").toBool());
    QVERIFY(!call("notifications_act", {{"index", 0}}).isError);
    QCOMPARE(jsonOf(call("together_state")).toObject().value("unread").toInt(), 0);
  }

  void sessionShareToolsNeedAGame() {
    QCOMPARE(call("session_share", {{"visibility", "home_server"}}).content.value(0).text, QStringLiteral("no_game"));
    QVERIFY(call("session_share", {{"visibility", "everyone"}}).isError);
    QVERIFY(call("session_share").isError);
    QVERIFY(!jsonOf(call("session_stop_sharing")).toObject().value("was_shared").toBool());
  }

  void sessionQualityNeedsAWatchedSession() {
    QCOMPARE(call("session_quality", {{"pick", "low"}}).content.value(0).text, QStringLiteral("not_watching"));
    QVERIFY(call("session_quality", {{"pick", "ultra"}}).isError);
    QVERIFY(call("session_quality").isError);
    const QJsonObject v = jsonOf(call("session_viewers")).toObject();
    QVERIFY(!v.value("hosting").toBool());
    QVERIFY(v.value("viewers").toArray().isEmpty());
    QVERIFY(v.value("watching").toArray().isEmpty());
    QCOMPARE(v.value("default_quality").toString(), QStringLiteral("auto"));
  }

  void togetherToolsNeedAScope() {
    QCOMPARE(call("together_ready_check", {{"action", "start"}}).content.value(0).text, QStringLiteral("no_scope"));
    QCOMPARE(call("together_ready_check", {{"action", "ready"}}).content.value(0).text, QStringLiteral("no_open_check"));
    QVERIFY(call("together_ready_check", {{"action", "dance"}}).isError);
    QCOMPARE(call("together_pause", {{"paused", true}}).content.value(0).text, QStringLiteral("no_scope"));
    QVERIFY(call("together_pause").isError);
    QCOMPARE(call("chat_send", {{"text", "hello"}}).content.value(0).text, QStringLiteral("no_scope"));
    QCOMPARE(call("chat_send", {{"text", "   "}}).content.value(0).text, QStringLiteral("bad_text"));
    QVERIFY(call("chat_send").isError);
    const QJsonObject hist = jsonOf(call("chat_history")).toObject();
    QVERIFY(!hist.value("scope").toObject().value("available").toBool());
    QVERIFY(hist.value("messages").toArray().isEmpty());
    QCOMPARE(jsonOf(call("together_state")).toObject().value("chat").toObject().value("unread").toInt(), 0);
    const QJsonObject st = jsonOf(call("together_state")).toObject();
    QVERIFY(!st.value("scope").toObject().value("available").toBool());
    QVERIFY(!st.value("ready_check").toObject().value("open").toBool());
    QVERIFY(!st.value("pause").toObject().value("paused").toBool());
  }

  void serverSignInValidatesArguments() {
    QVERIFY(call("server_sign_in").isError);
    QVERIFY(call("server_sign_in", {{"url", "https://127.0.0.1:1"}, {"username", "lena"}}).isError);  // no password
    QVERIFY(call("server_sign_in", {{"url", ""}, {"username", "lena"}, {"password", "x"}}).isError);
    QVERIFY(validateServerSignInArgs({{"url", "u"}, {"username", "n"}, {"password", "p"}}).isEmpty());
    QVERIFY(!validateServerSignInArgs({{"url", "u"}, {"username", "n"}, {"password", "p"}, {"trust_fingerprint", 5}}).isEmpty());
    // Fingerprint decision: colons/case do not matter, a different or missing value is refused.
    QVERIFY(serverFingerprintTrusted(QStringLiteral("AB:CD:EF"), QStringLiteral("abcdef")));
    QVERIFY(!serverFingerprintTrusted(QStringLiteral("AB:CD:EF"), QStringLiteral("abcdee")));
    QVERIFY(!serverFingerprintTrusted(QStringLiteral("AB:CD:EF"), QString()));
    QVERIFY(!serverFingerprintTrusted(QString(), QString()));
  }

  void runRecapValidatesArguments() {
    QVERIFY(call("run_recap").isError);
    QVERIFY(call("run_recap", {{"run_id", ""}}).isError);
    QVERIFY(call("run_recap", {{"run_id", 7}}).isError);
    QVERIFY(call("run_recap", {{"run_id", "not-a-uuid"}}).isError);
    const QString uuid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    QVERIFY(validateRunRecapArgs({{"run_id", uuid}}).isEmpty());
    QVERIFY(validateRunRecapArgs({{"run_id", uuid.toUpper()}}).isEmpty());
    QVERIFY(!validateRunRecapArgs({}).isEmpty());
    QVERIFY(!validateRunRecapArgs({{"run_id", "../etc"}}).isEmpty());
    // A valid id without a connected server is refused with a clear reason, not a timeout.
    const ToolResult r = call("run_recap", {{"run_id", uuid}});
    QVERIFY(r.isError);
  }

  void tokenFromEnvironmentOnlyWithMcpFlag() {
    const QByteArray tok(43, 'a');
    qputenv("UZUME_MCP_TOKEN", tok);
    QVERIFY(ui::McpController::tokenFromEnvironment(false).isEmpty());  // without --mcp it is ignored
    QCOMPARE(ui::McpController::tokenFromEnvironment(true), tok);
    QVERIFY(!pc->mcp()->setTokenForThisStart("short"));
    QVERIFY(!pc->mcp()->setTokenForThisStart(QByteArray(40, '!')));
    QVERIFY(pc->mcp()->setTokenForThisStart(tok));
    const quint16 port = freePort();
    QVERIFY(!pc->mcp()->setPortForThisStart(80));
    QVERIFY(pc->mcp()->setPortForThisStart(port));
    pc->mcp()->enableForThisStart();
    QVERIFY(pc->mcp()->listening());
    QCOMPARE(pc->mcp()->port(), int(port));
    QVERIFY(pc->mcp()->setupCommand().contains(QString::fromLatin1(tok)));
    QCOMPARE(mcptest::request(port, "POST", "/mcp", mcptest::goodHeaders(port, tok), mcptest::rpc(1, "ping")).status, 200);
    // Neither the token nor the port reached the credential store or the settings.
    QVERIFY(!creds->read(QString::fromLatin1(ui::McpController::kTokenTarget)).has_value());
    qunsetenv("UZUME_MCP_TOKEN");
  }

  void switchPersistsAndTokenStaysOutOfTheLog() {
    const QString logDir = dir.filePath("logdir");
    filelog::install();
    QVERIFY(filelog::setDirectory(logDir));
    const quint16 port = freePort();
    QVERIFY(!pc->mcp()->enabled());
    pc->mcp()->setPort(port);
    pc->mcp()->setEnabled(true);
    QVERIFY(pc->mcp()->listening());
    QVERIFY(pc->mcp()->error().isEmpty());
    const QString cmd = pc->mcp()->setupCommand();
    QVERIFY(cmd.startsWith(QStringLiteral("claude mcp add --transport http uzume http://127.0.0.1:%1/mcp --header \"Authorization: Bearer ").arg(port)));
    const QString token = cmd.section(QStringLiteral("Bearer "), 1).chopped(1);
    QCOMPARE(token.size(), 43);
    pc->mcp()->copySetupCommand();
    QCOMPARE(QGuiApplication::clipboard()->text(), cmd);

    // A real call and a refused one, both logged by name only.
    const QByteArray t = token.toLatin1();
    QCOMPARE(mcptest::request(port, "POST", "/mcp", mcptest::goodHeaders(port, t), mcptest::rpc(1, "tools/call", {{"name", "status"}})).status, 200);
    QCOMPARE(mcptest::request(port, "POST", "/mcp", mcptest::goodHeaders(port, "wrong" + t), mcptest::rpc(2, "ping")).status, 401);
    QCOMPARE(mcptest::request(port, "POST", "/mcp", mcptest::goodHeaders(port, t), mcptest::rpc(3, "tools/call", {{"name", "settings_get"}})).status, 200);
    pc->mcp()->renewToken();
    QVERIFY(pc->mcp()->listening());
    const QString cmd2 = pc->mcp()->setupCommand();
    QVERIFY(cmd2 != cmd);
    QCOMPARE(mcptest::request(port, "POST", "/mcp", mcptest::goodHeaders(port, t), mcptest::rpc(4, "ping")).status, 401);
    const QString token2 = cmd2.section(QStringLiteral("Bearer "), 1).chopped(1);

    QFile log(filelog::path());
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QByteArray text = log.readAll();
    QVERIFY(text.contains("mcp: status"));
    QVERIFY(text.contains("mcp: listening"));
    QVERIFY(!text.contains(token.toLatin1()));
    QVERIFY(!text.contains(token2.toLatin1()));
    QVERIFY(!text.contains("Bearer"));
    // logs_tail serves the same redacted file
    const QJsonObject tail = jsonOf(call("logs_tail", {{"contains", "mcp:"}, {"lines", 50}})).toObject();
    QVERIFY(tail.value("count").toInt() >= 2);
    QVERIFY(!QJsonDocument(tail).toJson().contains(token.toLatin1()));
    QVERIFY(jsonOf(call("logs_search", {{"pattern", "listening on"}})).toObject().value("count").toInt() >= 1);

    // The switch is persisted; a new controller on the same data directory and credential store keeps both.
    pc->mcp()->stop();
    QVERIFY(!pc->mcp()->listening());
    QVERIFY(!pc->mcp()->enabled());
    pc->mcp()->setEnabled(true);
    pc.reset();
    pc = std::make_unique<ui::PlayerController>(options(QStringLiteral("a")));
    QVERIFY(pc->mcp()->enabled());
    QCOMPARE(pc->mcp()->port(), int(port));
    QVERIFY(!pc->mcp()->listening());  // starts with startup()
    pc->mcp()->start();
    QVERIFY(pc->mcp()->listening());
    QCOMPARE(pc->mcp()->setupCommand(), cmd2);  // same token from the credential store
    // Off by default in a fresh data directory, and --mcp is for this run only.
    pc = std::make_unique<ui::PlayerController>(options(QStringLiteral("b")));
    QVERIFY(!pc->mcp()->enabled());
    pc->mcp()->setPort(freePort());
    pc->mcp()->enableForThisStart();
    QVERIFY(pc->mcp()->listening());
    pc = std::make_unique<ui::PlayerController>(options(QStringLiteral("b")));
    QVERIFY(!pc->mcp()->enabled());
    // A taken port is reported, not fatal.
    QTcpServer taken;
    QVERIFY(taken.listen(QHostAddress::LocalHost, 0));
    pc->mcp()->setPort(taken.serverPort());
    pc->mcp()->setEnabled(true);
    QVERIFY(!pc->mcp()->listening());
    QVERIFY(!pc->mcp()->error().isEmpty());
  }

  void pluginCallEvalAndGating() {
    pc->plugins()->setDeveloperMode(true);
    const auto r = pc->plugins()->manager()->installFromFolder(writePlugin(dir.filePath("src"), "net.test.local", kPlugin));
    QVERIFY2(r.ok, qPrintable(r.error));
    // A plugin that is not an installed local one (like a run copy) runs too.
    const QString otherDir = writePlugin(dir.filePath("src"), "net.test.other", kPlugin);
    const auto localInfo = pc->plugins()->manager()->info("net.test.local");
    QVERIFY(localInfo && localInfo->origin == QLatin1String("local"));
    plugins::PluginInfo other = *localInfo;
    const auto lm = plugins::loadManifest(otherDir);
    QVERIFY2(lm.ok, qPrintable(lm.error));
    other.manifest = lm.manifest;
    other.path = otherDir;
    other.cacheDir = dir.filePath("cache/other");
    other.origin = QStringLiteral("official");
    plugins::GameContext ctx;
    ctx.shell = true;
    pc->plugins()->host()->start(ctx, nullptr, {*localInfo, other});
    QTRY_VERIFY_WITH_TIMEOUT(pc->plugins()->host()->consoleSnapshot("net.test.local").status == plugins::PluginStatus::Running &&
                                 pc->plugins()->host()->consoleSnapshot("net.test.other").status == plugins::PluginStatus::Running, 10000);

    const QJsonArray list = jsonOf(call("plugins_list")).toArray();
    bool found = false;
    for (const QJsonValue& v : list)
      if (v.toObject().value("id").toString() == QStringLiteral("net.test.local")) {
        found = true;
        QCOMPARE(v.toObject().value("status").toString(), QStringLiteral("running"));
        QCOMPARE(v.toObject().value("origin").toString(), QStringLiteral("local"));
        QCOMPARE(v.toObject().value("provides").toArray()[0].toString(), QStringLiteral("test.iface"));
      }
    QVERIFY(found);
    const QJsonObject console = jsonOf(call("plugin_console", {{"id", "net.test.local"}})).toObject();
    QCOMPARE(console.value("status").toString(), QStringLiteral("running"));
    QVERIFY(QJsonDocument(console.value("log").toArray()).toJson().contains("plugin loaded"));
    QVERIFY(console.value("budgets").toObject().value("wall_ms_per_call").toInt() > 0);

    // 0.22: seq numbers, counts per level, level and since_seq filters, paused and error info.
    QVERIFY(console.value("log").toArray().at(0).toObject().contains("seq"));
    QVERIFY(!console.value("paused").toBool());
    QVERIFY(console.value("pause_reason").toString().isEmpty());
    QCOMPARE(console.value("errors_total").toInt(), 0);
    QVERIFY(!console.value("unpacked").toBool());
    QCOMPARE(console.value("reload").toObject().value("count").toInt(), 0);
    const int seq0 = console.value("last_seq").toInt();
    QVERIFY(seq0 >= 2);
    QVERIFY2(!call("plugin_eval", {{"id", "net.test.local"}, {"code", "fb.log.warn('w1') fb.log.error('e1') fb.log.error('e2')"}}).isError, "eval");
    const QJsonObject errs = jsonOf(call("plugin_console", {{"id", "net.test.local"}, {"level", "error"}, {"since_seq", seq0}})).toObject();
    QCOMPARE(errs.value("log").toArray().size(), 2);
    QCOMPARE(errs.value("log").toArray().at(0).toObject().value("text").toString(), QStringLiteral("e1"));  // oldest first
    QCOMPARE(errs.value("log").toArray().at(0).toObject().value("seq").toInt(), seq0 + 2);
    QCOMPARE(errs.value("counts").toObject().value("error").toInt(), 2);  // counts are not narrowed by the filters
    QCOMPARE(errs.value("counts").toObject().value("warn").toInt(), 1);
    QCOMPARE(errs.value("last_seq").toInt(), seq0 + 3);
    const QJsonObject both = jsonOf(call("plugin_console", {{"id", "net.test.local"}, {"level", QJsonArray{"warn", "error"}}})).toObject();
    QCOMPARE(both.value("log").toArray().size(), 3);
    const QJsonObject csv = jsonOf(call("plugin_console", {{"id", "net.test.local"}, {"level", "warn,error"}, {"since_seq", seq0 + 1}})).toObject();
    QCOMPARE(csv.value("log").toArray().size(), 2);
    QVERIFY(call("plugin_console", {{"id", "net.test.local"}, {"level", "loud"}}).isError);
    QVERIFY(call("plugin_console", {{"id", "net.test.local"}, {"since_seq", "x"}}).isError);
    // devconsole_state: the model the app's console shows.
    const QJsonObject dev = jsonOf(call("devconsole_state")).toObject();
    QVERIFY(dev.value("model").toObject().contains("counts"));
    QVERIFY(dev.value("model").toObject().contains("permissions"));
    QVERIFY(dev.value("memory").toObject().contains("available"));

    // plugin_call: a number, a table and an image as PNG content.
    ToolResult add = call("plugin_call", {{"id", "net.test.local"}, {"interface", "test.iface"}, {"function", "add"}, {"args", QJsonArray{2, 40}}});
    QVERIFY2(!add.isError, qPrintable(add.content.value(0).text));
    QCOMPARE(jsonOf(add).toObject().value("results").toArray()[0].toInt(), 42);
    const QJsonObject info = jsonOf(call("plugin_call", {{"id", "net.test.local"}, {"interface", "test.iface"}, {"function", "info"}})).toObject().value("results").toArray()[0].toObject();
    QCOMPARE(info.value("name").toString(), QStringLiteral("x"));
    QCOMPARE(info.value("list").toArray().size(), 3);
    ToolResult img = call("plugin_call", {{"id", "net.test.local"}, {"interface", "test.iface"}, {"function", "img"}});
    QVERIFY2(!img.isError, qPrintable(img.content.value(0).text));
    QCOMPARE(images(img), 1);
    QImage decoded;
    QVERIFY(decoded.loadFromData(img.content.value(1).png, "PNG"));
    QCOMPARE(decoded.size(), QSize(2, 2));
    QCOMPARE(decoded.pixelColor(0, 0), QColor(255, 0, 0));
    QCOMPARE(jsonOf(img).toObject().value("results").toArray()[0].toObject().value("image").toInt(), 0);
    QVERIFY(call("plugin_call", {{"id", "net.test.local"}, {"interface", "test.iface"}, {"function", "missing"}}).isError);
    QVERIFY(call("plugin_call", {{"id", "net.test.local"}, {"interface", "nope"}, {"function", "x"}}).isError);

    // plugin_eval: local only, output captured, errors reported, budget trip pauses.
    ToolResult ev = call("plugin_eval", {{"id", "net.test.local"}, {"code", "print('hi'); fb.log.warn('w'); return 1 + 1, 'two'"}});
    QVERIFY2(!ev.isError, qPrintable(ev.content.value(0).text));
    const QJsonObject evj = jsonOf(ev).toObject();
    QCOMPARE(evj.value("results").toArray()[0].toInt(), 2);
    QCOMPARE(evj.value("results").toArray()[1].toString(), QStringLiteral("two"));
    QCOMPARE(evj.value("output").toArray().size(), 2);
    QCOMPARE(jsonOf(call("plugin_eval", {{"id", "net.test.local"}, {"code", "1 + 2"}})).toObject().value("results").toArray()[0].toInt(), 3);  // expression form
    ToolResult bad = call("plugin_eval", {{"id", "net.test.local"}, {"code", "error('boom')"}});
    QVERIFY(bad.isError);
    QVERIFY(bad.content.value(0).text.contains("boom"));
    QVERIFY(call("plugin_eval", {{"id", "net.test.local"}, {"code", "load('return 1')"}}).isError);  // the plugin has no load
    QCOMPARE(call("plugin_eval", {{"id", "net.test.other"}, {"code", "1"}}).content.value(0).text, QStringLiteral("eval_not_allowed"));
    QCOMPARE(call("plugin_eval", {{"id", "net.test.unknown"}, {"code", "1"}}).content.value(0).text, QStringLiteral("eval_not_allowed"));
    ToolResult loop = call("plugin_eval", {{"id", "net.test.local"}, {"code", "while true do end"}});
    QVERIFY(loop.isError);
    QVERIFY(loop.content.value(0).text.contains("budget"));
    QCOMPARE(pc->plugins()->host()->consoleSnapshot("net.test.local").status, plugins::PluginStatus::Paused);
    QCOMPARE(pc->plugins()->host()->consoleSnapshot("net.test.other").status, plugins::PluginStatus::Running);
    pc->plugins()->host()->stop();
  }

  void pluginCacheFiles() {
    pc->plugins()->setDeveloperMode(true);
    const auto r = pc->plugins()->manager()->installFromFolder(writePlugin(dir.filePath("src"), "net.test.cache", kPlugin));
    QVERIFY2(r.ok, qPrintable(r.error));
    const auto info = pc->plugins()->manager()->info("net.test.cache");
    QVERIFY(info);
    writeFile(info->cacheDir + "/icons/a.png", pngOfColor(qRgb(0, 128, 255)));
    writeFile(info->cacheDir + "/data.bin", QByteArray("\x01\x02\x03", 3));
    const QJsonObject list = jsonOf(call("plugin_cache_list", {{"id", "net.test.cache"}})).toObject();
    QCOMPARE(list.value("files").toArray().size(), 2);
    ToolResult png = call("plugin_cache_read", {{"id", "net.test.cache"}, {"path", "icons/a.png"}});
    QVERIFY2(!png.isError, qPrintable(png.content.value(0).text));
    QCOMPARE(images(png), 1);
    ToolResult bin = call("plugin_cache_read", {{"id", "net.test.cache"}, {"path", "data.bin"}});
    QCOMPARE(QByteArray::fromBase64(jsonOf(bin).toObject().value("base64").toString().toLatin1()), QByteArray("\x01\x02\x03", 3));
    QVERIFY(call("plugin_cache_read", {{"id", "net.test.cache"}, {"path", "../manifest.json"}}).isError);
    QVERIFY(call("plugin_cache_read", {{"id", "net.test.cache"}, {"path", "nothing.bin"}}).isError);
    // image sources reach only the cache and declared assets (the plugin list is refreshed on install)
    ToolResult im = call("plugin_image", {{"id", "net.test.cache"}, {"src", "cache:icons/a.png"}});
    QVERIFY2(!im.isError, qPrintable(im.content.value(0).text));
    QCOMPARE(images(im), 1);
    QVERIFY(call("plugin_image", {{"id", "net.test.cache"}, {"src", "cache:../manifest.json"}}).isError);
    QVERIFY(call("plugin_image", {{"id", "net.test.cache"}, {"src", "asset:main.lua"}}).isError);
    QVERIFY(call("plugin_image", {{"id", "net.test.cache"}, {"src", "http://x/y.png"}}).isError);
  }

  void gameToolsAgainstTheTestCore() {
    ui::GameSession* gs = pc->gameSession();
    ui::GameSession::LaunchConfig cfg;
    cfg.title = QStringLiteral("Uzume Test");
    cfg.corePath = qEnvironmentVariable("UZUME_MELONDS_DS_CORE");
    cfg.gamePath = QStringLiteral(FB_TEST_ROM_PATH);
    cfg.systemDir = dir.filePath("sys");
    cfg.saveDir = dir.filePath("save");
    cfg.display.layout = QStringLiteral("vertical");
    cfg.display.screens = {{QStringLiteral("top"), 256, 192, false}, {QStringLiteral("bottom"), 256, 192, true}};
    gs->start(cfg);
    QTRY_COMPARE_WITH_TIMEOUT(gs->state(), ui::GameSession::Running, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(gs->frameNumber() >= 40, 20000);  // the late memory map arrives at frame 30

    const QJsonObject st = jsonOf(call("game_state")).toObject();
    QCOMPARE(st.value("state").toString(), QStringLiteral("running"));
    QCOMPARE(st.value("screens").toArray().size(), 2);
    QVERIFY(st.contains("diagnostics"));
    const QJsonArray regions = jsonOf(call("memory_regions")).toArray();
    QVERIFY(regions.size() >= 2);

    // Live memory, not a snapshot: the magic of the heap region, a search for it, a frame counter that moves.
    const QJsonObject rd = jsonOf(call("memory_read", {{"addr", "0x08000000"}, {"len", 4}})).toObject();
    QCOMPARE(rd.value("hex").toString(), QStringLiteral("46 42 4d 4d"));
    QCOMPARE(QByteArray::fromBase64(rd.value("base64").toString().toLatin1()), QByteArray("FBMM"));
    const QJsonObject found = jsonOf(call("memory_search", {{"start", "0x08000000"}, {"end", "0x08001000"}, {"pattern", "46 42 4D 4D"}})).toObject();
    QCOMPARE(found.value("matches").toArray()[0].toString(), QStringLiteral("0x08000000"));
    const QJsonObject byValue = jsonOf(call("memory_search", {{"start", "0x08000000"}, {"end", "0x08001000"}, {"value", int(mem::kMagic)}, {"type", "u32"}})).toObject();
    QCOMPARE(byValue.value("matches").toArray().size(), 1);
    QVERIFY(call("memory_read", {{"addr", 0x0F000000}, {"len", 4}}).isError);          // unmapped
    QVERIFY(call("memory_read", {{"addr", 0x08000000}, {"len", 2 * 1024 * 1024}}).isError);  // over the cap
    auto frameAt = [&]() { return QByteArray::fromBase64(jsonOf(call("memory_read", {{"addr", "0x08000004"}, {"len", 4}})).toObject().value("base64").toString().toLatin1()); };
    const QByteArray f1 = frameAt();
    QTRY_VERIFY_WITH_TIMEOUT(frameAt() != f1, 5000);

    // Screenshots: all screens and one screen as PNG content.
    ToolResult all = call("screenshot");
    QVERIFY2(!all.isError, qPrintable(all.content.value(0).text));
    QImage img;
    QVERIFY(img.loadFromData(all.content.value(1).png, "PNG"));
    QCOMPARE(img.size(), QSize(256, 384));
    ToolResult top = call("screenshot", {{"screen", "top"}});
    QVERIFY(img.loadFromData(top.content.value(1).png, "PNG"));
    QCOMPARE(img.size(), QSize(256, 192));
    ToolResult bottom = call("screenshot", {{"screen", "bottom"}});
    QVERIFY(img.loadFromData(bottom.content.value(1).png, "PNG"));
    QCOMPARE(img.size(), QSize(256, 192));
    QVERIFY(call("screenshot", {{"screen", "left"}}).isError);

    // Input: A is held for 60 frames; the test core draws the pressed button black in its input strip (bottom screen, cell 8).
    QVERIFY(call("game_input", {{"buttons", QJsonArray{"nope"}}}).isError);
    bool inputDone = false;
    ToolResult inputResult;
    pc->mcp()->registry()->call("game_input", {{"buttons", QJsonArray{"a"}}, {"frames", 60}}, [&](ToolResult r) { inputResult = r; inputDone = true; });
    QTRY_VERIFY_WITH_TIMEOUT(gs->frame().pixel(8 * 16 + 4, 192 + 4) == 0xFF000000u, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(inputDone, 10000);
    QVERIFY2(!inputResult.isError, qPrintable(inputResult.content.value(0).text));
    QTRY_VERIFY_WITH_TIMEOUT(gs->frame().pixel(8 * 16 + 4, 192 + 4) == 0xFFFF00FFu, 3000);  // released again

    // Pause, step exactly N frames, resume.
    QVERIFY(call("game_frame_step", {{"frames", 1}}).isError);  // not paused
    QVERIFY(!call("game_pause").isError);
    QVERIFY(gs->isPaused());
    const double f0 = jsonOf(call("game_frame_step", {{"frames", 1}})).toObject().value("frame").toDouble();
    const QJsonObject s5 = jsonOf(call("game_frame_step", {{"frames", 5}})).toObject();
    QCOMPARE(s5.value("frame").toDouble(), f0 + 5);
    QVERIFY(s5.value("paused").toBool());
    QCOMPARE(double(gs->frameNumber()), f0 + 5);
    QVERIFY(call("game_frame_step", {{"frames", 0}}).isError);
    QVERIFY(call("game_frame_step", {{"frames", 601}}).isError);
    const QJsonObject s2 = jsonOf(call("game_frame_step", {{"frames", 2}})).toObject();
    QCOMPARE(s2.value("frame").toDouble(), f0 + 7);
    QVERIFY(gs->isPaused());
    // Stepping with a button held runs the frames with it pressed (the strip is drawn into the last frame), then releases.
    ToolResult heldStep = call("game_input", {{"buttons", QJsonArray{"b"}}, {"frames", 3}});
    QVERIFY2(!heldStep.isError, qPrintable(heldStep.content.value(0).text));
    QVERIFY(jsonOf(heldStep).toObject().value("stepped").toBool());
    QVERIFY(!call("game_resume").isError);
    QVERIFY(!gs->isPaused());
    QVERIFY(call("rom_read", {{"offset", 0}, {"len", 16}}).isError);  // the controller has no library game here
    gs->stop();
    QVERIFY(call("memory_read", {{"addr", 0x08000000}, {"len", 4}}).isError);
  }

  // game_state_save / game_state_load / game_memory_write through the emulation command queue (0.20), against the test core.
  void emulationToolsAgainstTheTestCore() {
    ui::GameSession* gs = pc->gameSession();
    ui::GameSession::LaunchConfig cfg;
    cfg.title = QStringLiteral("Uzume Test");
    cfg.corePath = qEnvironmentVariable("UZUME_MELONDS_DS_CORE");
    cfg.gamePath = QStringLiteral(FB_TEST_ROM_PATH);
    cfg.systemDir = dir.filePath("sys");
    cfg.saveDir = dir.filePath("save");
    cfg.display.layout = QStringLiteral("vertical");
    cfg.display.screens = {{QStringLiteral("top"), 256, 192, false}, {QStringLiteral("bottom"), 256, 192, true}};
    gs->start(cfg);
    QTRY_COMPARE_WITH_TIMEOUT(gs->state(), ui::GameSession::Running, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(gs->frameNumber() >= 40, 20000);

    // Memory write: queued, applied between frames, read back live.
    const ToolResult w = call("game_memory_write", {{"addr", "0x08000800"}, {"hex", "DE AD be ef"}});
    QVERIFY2(!w.isError, qPrintable(w.content.value(0).text));
    QCOMPARE(jsonOf(w).toObject().value("written").toInt(), 4);
    QCOMPARE(jsonOf(call("memory_read", {{"addr", "0x08000800"}, {"len", 4}})).toObject().value("hex").toString(), QStringLiteral("de ad be ef"));
    QCOMPARE(call("game_memory_write", {{"addr", "0x0F000000"}, {"hex", "00"}}).content.value(0).text, QStringLiteral("unmapped"));
    QVERIFY(call("game_memory_write", {{"addr", "0x08000800"}, {"hex", "abc"}}).isError);  // odd hex
    QVERIFY(call("game_memory_write", {{"addr", "0x08000800"}}).isError);

    // Save state: size and frame; then the memory changes, and a load brings the saved bytes back (also while paused).
    const ToolResult sv = call("game_state_save", {{"name", "a"}});
    QVERIFY2(!sv.isError, qPrintable(sv.content.value(0).text));
    const QJsonObject so = jsonOf(sv).toObject();
    QVERIFY(so.value("size").toDouble() > 1000);
    QVERIFY(so.value("frame").toDouble() >= 40);
    QVERIFY(call("game_state_save", {{"name", "bad name!"}}).isError);
    QVERIFY(!call("game_pause").isError);
    QTRY_VERIFY_WITH_TIMEOUT(gs->isPaused(), 5000);
    QVERIFY(!call("game_memory_write", {{"addr", "0x08000800"}, {"hex", "00000000"}}).isError);
    QCOMPARE(jsonOf(call("memory_read", {{"addr", "0x08000800"}, {"len", 4}})).toObject().value("hex").toString(), QStringLiteral("00 00 00 00"));
    QCOMPARE(call("game_state_load", {{"name", "nope"}}).content.value(0).text, QStringLiteral("unknown_state"));
    const ToolResult ld = call("game_state_load", {{"name", "a"}});
    QVERIFY2(!ld.isError, qPrintable(ld.content.value(0).text));
    QCOMPARE(jsonOf(ld).toObject().value("size").toDouble(), so.value("size").toDouble());
    QCOMPARE(jsonOf(call("memory_read", {{"addr", "0x08000800"}, {"len", 4}})).toObject().value("hex").toString(), QStringLiteral("de ad be ef"));
    gs->stop();
    QCOMPARE(call("game_state_save").content.value(0).text, QStringLiteral("no_game"));
  }
};

QTEST_MAIN(McpToolsTest)
#include "mcp_tools_test.moc"
