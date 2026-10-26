// MCP server over a real socket: HTTP rules, auth, JSON-RPC lifecycle, tools/call round trip, async tools and the timeout.
#include <QJsonArray>
#include <QTcpServer>
#include <QTimer>
#include <QtTest>

#include "mcp_registry.h"
#include "mcp_server.h"
#include "mcp_test_client.h"

using namespace uzume::mcp;
using namespace mcptest;

class McpServerTest : public QObject {
  Q_OBJECT
  McpToolRegistry* reg = nullptr;
  McpServer* server = nullptr;
  QByteArray token;
  quint16 port = 0;

 private slots:
  void init() {
    reg = new McpToolRegistry(this);
    ToolDef echo;
    echo.name = "echo";
    echo.description = "Echoes";
    echo.targetArg = "id";
    echo.handler = [](const QJsonObject& a, ToolReply r) { r(ToolResult::json(QJsonObject{{"got", a.value("text")}})); };
    reg->add(echo);
    ToolDef later;
    later.name = "later";
    later.handler = [](const QJsonObject&, ToolReply r) { QTimer::singleShot(50, [r]() { r(ToolResult::text("late")); }); };
    reg->add(later);
    ToolDef never;
    never.name = "never";
    never.timeoutMs = 150;
    never.handler = [](const QJsonObject&, ToolReply) {};
    reg->add(never);
    ToolDef img;
    img.name = "img";
    img.handler = [](const QJsonObject&, ToolReply r) { r(ToolResult::text("x").addImage("PNGBYTES")); };
    reg->add(img);
    server = new McpServer(reg, this);
    server->setServerInfo("uzume", "1.2.3");
    token = generateToken();
    QVERIFY(server->listen(0, token));
    port = server->port();
    QVERIFY(port != 0);
  }
  void cleanup() {
    delete server;
    delete reg;
  }

  void tokenIsRandomAndComparedExactly() {
    QCOMPARE(token.size(), 43);
    QVERIFY(token != generateToken());
    QVERIFY(tokenEquals(token, token));
    QVERIFY(!tokenEquals(token, token + "x"));
    QVERIFY(!tokenEquals(token, QByteArray(43, 'a')));
    QVERIFY(!tokenEquals("", token));
  }

  void listensOnLoopbackOnly() {
    QTcpServer other;
    QVERIFY(!other.listen(QHostAddress::LocalHost, port));  // taken
    QVERIFY(server->isListening());
  }

  void authAndHostAndOrigin() {
    const QByteArray body = rpc(1, "ping");
    auto h = goodHeaders(port, token);
    QCOMPARE(request(port, "POST", "/mcp", h, body).status, 200);

    auto noAuth = h;
    noAuth.remove("Authorization");
    QCOMPARE(request(port, "POST", "/mcp", noAuth, body).status, 401);
    auto wrong = h;
    wrong["Authorization"] = "Bearer " + token + "x";
    QCOMPARE(request(port, "POST", "/mcp", wrong, body).status, 401);
    wrong["Authorization"] = "Basic abc";
    QCOMPARE(request(port, "POST", "/mcp", wrong, body).status, 401);

    auto badHost = h;
    badHost["Host"] = "evil.example:" + QByteArray::number(port);
    QCOMPARE(request(port, "POST", "/mcp", badHost, body).status, 403);
    badHost["Host"] = "127.0.0.1:1";
    QCOMPARE(request(port, "POST", "/mcp", badHost, body).status, 403);
    auto local = h;
    local["Host"] = "localhost:" + QByteArray::number(port);
    QCOMPARE(request(port, "POST", "/mcp", local, body).status, 200);

    auto origin = h;
    origin["Origin"] = "https://evil.example";
    QCOMPARE(request(port, "POST", "/mcp", origin, body).status, 403);
    origin["Origin"] = "http://localhost:3000";
    QCOMPARE(request(port, "POST", "/mcp", origin, body).status, 200);
  }

  void httpRules() {
    const auto h = goodHeaders(port, token);
    QCOMPARE(request(port, "GET", "/mcp", h, "", false).status, 405);
    QCOMPARE(request(port, "DELETE", "/mcp", h, "", false).status, 405);
    QCOMPARE(request(port, "POST", "/other", h, "{}").status, 404);
    QCOMPARE(request(port, "POST", "/mcp", h, "{}", false).status, 411);
    QCOMPARE(request(port, "POST", "/mcp", h, QByteArray(kMaxBodyBytes + 1, ' ')).status, 413);
    auto big = h;
    for (int i = 0; i < 40; ++i) big["X-Pad-" + QByteArray::number(i)] = QByteArray(500, 'a');
    QCOMPARE(request(port, "POST", "/mcp", big, "{}").status, 431);
    QCOMPARE(request(port, "POST", "/mcp", h, "{not json").status, 400);
  }

  void lifecycleAndToolsRoundTrip() {
    const auto h = goodHeaders(port, token);
    Response r = request(port, "POST", "/mcp", h, rpc(1, "initialize", {{"protocolVersion", "2025-03-26"}}));
    QCOMPARE(r.status, 200);
    QCOMPARE(r.headers.value("content-type"), QByteArray("application/json"));
    QJsonObject res = r.json().value("result").toObject();
    QCOMPARE(res.value("protocolVersion").toString(), QStringLiteral("2025-03-26"));
    QVERIFY(res.value("capabilities").toObject().contains("tools"));
    QCOMPARE(res.value("serverInfo").toObject().value("name").toString(), QStringLiteral("uzume"));
    QCOMPARE(res.value("serverInfo").toObject().value("version").toString(), QStringLiteral("1.2.3"));
    r = request(port, "POST", "/mcp", h, rpc(2, "initialize", {{"protocolVersion", "1999-01-01"}}));
    QCOMPARE(r.json().value("result").toObject().value("protocolVersion").toString(), QStringLiteral("2025-06-18"));

    r = request(port, "POST", "/mcp", h, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    QCOMPARE(r.status, 202);
    QVERIFY(r.body.isEmpty());

    r = request(port, "POST", "/mcp", h, rpc(3, "tools/list"));
    const QJsonArray tools = r.json().value("result").toObject().value("tools").toArray();
    QCOMPARE(tools.size(), 4);
    QCOMPARE(tools[0].toObject().value("name").toString(), QStringLiteral("echo"));
    QVERIFY(tools[0].toObject().contains("inputSchema"));

    r = request(port, "POST", "/mcp", h, rpc(4, "tools/call", {{"name", "echo"}, {"arguments", QJsonObject{{"text", "hi"}}}}));
    QCOMPARE(r.json().value("id").toInt(), 4);
    const QJsonObject call = r.json().value("result").toObject();
    QCOMPARE(call.value("isError").toBool(), false);
    const QJsonObject content = call.value("content").toArray()[0].toObject();
    QCOMPARE(content.value("type").toString(), QStringLiteral("text"));
    QCOMPARE(QJsonDocument::fromJson(content.value("text").toString().toUtf8()).object().value("got").toString(), QStringLiteral("hi"));

    r = request(port, "POST", "/mcp", h, rpc(5, "tools/call", {{"name", "img"}}));
    const QJsonObject image = r.json().value("result").toObject().value("content").toArray()[1].toObject();
    QCOMPARE(image.value("type").toString(), QStringLiteral("image"));
    QCOMPARE(image.value("mimeType").toString(), QStringLiteral("image/png"));
    QCOMPARE(QByteArray::fromBase64(image.value("data").toString().toLatin1()), QByteArray("PNGBYTES"));

    r = request(port, "POST", "/mcp", h, rpc(6, "tools/call", {{"name", "nope"}}));
    QCOMPARE(r.json().value("error").toObject().value("code").toInt(), -32602);
    r = request(port, "POST", "/mcp", h, rpc(7, "bogus/method"));
    QCOMPARE(r.json().value("error").toObject().value("code").toInt(), -32601);
    r = request(port, "POST", "/mcp", h, rpc(8, "ping"));
    QVERIFY(r.json().contains("result"));
  }

  void asyncToolsAndTimeout() {
    const auto h = goodHeaders(port, token);
    Response r = request(port, "POST", "/mcp", h, rpc(1, "tools/call", {{"name", "later"}}));
    QCOMPARE(r.json().value("result").toObject().value("isError").toBool(), false);
    r = request(port, "POST", "/mcp", h, rpc(2, "tools/call", {{"name", "never"}}));
    const QJsonObject res = r.json().value("result").toObject();
    QCOMPARE(res.value("isError").toBool(), true);
    QCOMPARE(res.value("content").toArray()[0].toObject().value("text").toString(), QStringLiteral("timeout"));
  }

  void keepAliveServesSeveralRequests() {
    QTcpSocket s;
    s.connectToHost(QStringLiteral("127.0.0.1"), port);
    QVERIFY(s.waitForConnected(2000));
    QByteArray buf;
    for (int i = 0; i < 3; ++i) {
      const QByteArray body = rpc(i, "ping");
      QByteArray raw = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\nAuthorization: Bearer " + token +
                       "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
      s.write(raw);
      QCOMPARE(readResponse(s, buf).status, 200);
    }
  }

  void stopClosesAndRelistens() {
    server->stop();
    QVERIFY(!server->isListening());
    QCOMPARE(request(port, "POST", "/mcp", goodHeaders(port, token), rpc(1, "ping")).status, 0);
    const QByteArray t2 = generateToken();
    QVERIFY(server->listen(0, t2));
    const quint16 p2 = server->port();
    QCOMPARE(request(p2, "POST", "/mcp", goodHeaders(p2, token), rpc(1, "ping")).status, 401);  // the old token is dead
    QCOMPARE(request(p2, "POST", "/mcp", goodHeaders(p2, t2), rpc(1, "ping")).status, 200);
  }

  void logTargetOnlyForIdentifiers() {
    ToolDef d;
    d.targetArg = "id";
    QCOMPARE(McpToolRegistry::logTarget(d, {{"id", "uzume.soullink"}}), QStringLiteral("uzume.soullink"));
    QCOMPARE(McpToolRegistry::logTarget(d, {{"id", "two words with secrets"}}), QString());
    QCOMPARE(McpToolRegistry::logTarget(d, {{"text", "x"}}), QString());
  }
};

QTEST_MAIN(McpServerTest)
#include "mcp_server_test.moc"
