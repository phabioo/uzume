#include "mcp_server.h"

#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <map>

#include "mcp_registry.h"

namespace uzume::mcp {

namespace {
constexpr int kMaxConnections = 16;
constexpr int kIdleMs = 60000;

const char* statusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    default: return "Error";
  }
}

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message) {
  return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), id},
                     {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}

QJsonObject rpcResult(const QJsonValue& id, const QJsonObject& result) {
  return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("result"), result}};
}

bool loopbackOrigin(const QString& origin) {
  const QUrl u(origin);
  const QString h = u.host().toLower();
  return u.isValid() && !h.isEmpty() && (h == QLatin1String("localhost") || h == QLatin1String("127.0.0.1") || h == QLatin1String("::1") ||
                                         h == QLatin1String("[::1]"));
}
}  // namespace

QByteArray generateToken() {
  quint64 words[4];
  for (quint64& w : words) w = QRandomGenerator::system()->generate64();
  const QByteArray raw(reinterpret_cast<const char*>(words), sizeof(words));
  return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool tokenEquals(const QByteArray& a, const QByteArray& b) {
  const qsizetype n = (std::max)(a.size(), b.size());
  unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
  for (qsizetype i = 0; i < n; ++i) {
    const unsigned ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
    const unsigned cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
    diff |= ca ^ cb;
  }
  return diff == 0;
}

// One client connection: parses requests one at a time (keep-alive), answers them, drops the socket on any protocol error.
class McpServer::Connection : public QObject {
 public:
  Connection(McpServer* server, QTcpSocket* socket) : QObject(server), m_server(server), m_socket(socket) {
    m_socket->setParent(this);
    connect(m_socket, &QTcpSocket::readyRead, this, [this]() { onReadyRead(); });
    connect(m_socket, &QTcpSocket::disconnected, this, [this]() { deleteLater(); });
    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, [this]() {
      if (!m_busy) m_socket->disconnectFromHost();
    });
    m_idle.start(kIdleMs);
    m_server->m_conns.insert(this);
  }
  ~Connection() override { m_server->m_conns.remove(this); }

 private:
  struct Head {
    QByteArray method, path;
    std::map<QByteArray, QByteArray> headers;  // lower-case names
    qint64 contentLength = -1;
    bool close = false;
  };

  void onReadyRead() {
    if (m_closing) {
      m_socket->readAll();
      return;
    }
    m_buf.append(m_socket->readAll());
    m_idle.start(kIdleMs);
    process();
  }

  void process() {
    while (!m_busy && !m_closing && m_socket->isOpen()) {
      if (!m_haveHead) {
        const qsizetype end = m_buf.indexOf("\r\n\r\n");
        if (end < 0) {
          if (m_buf.size() > kMaxHeaderBytes) reject(431, "headers too large");
          return;
        }
        if (end > kMaxHeaderBytes) {
          reject(431, "headers too large");
          return;
        }
        if (!parseHead(m_buf.left(end))) return;  // answered
        m_buf.remove(0, end + 4);
        m_haveHead = true;
      }
      if (m_buf.size() < m_head.contentLength) return;
      const QByteArray body = m_buf.left(m_head.contentLength);
      m_buf.remove(0, m_head.contentLength);
      m_haveHead = false;
      m_busy = true;
      handleBody(body);
    }
  }

  // Validates the request line and the headers; on a problem answers and returns false.
  bool parseHead(const QByteArray& text) {
    const QList<QByteArray> lines = text.split('\n');
    if (lines.isEmpty()) return reject(400, "bad request"), false;
    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() != 3 || !parts[2].startsWith("HTTP/1.")) return reject(400, "bad request"), false;
    m_head = Head();
    m_head.method = parts[0];
    m_head.path = parts[1];
    for (qsizetype i = 1; i < lines.size(); ++i) {
      const QByteArray line = lines[i].trimmed();
      if (line.isEmpty()) continue;
      const qsizetype colon = line.indexOf(':');
      if (colon <= 0) return reject(400, "bad header"), false;
      const QByteArray name = line.left(colon).trimmed().toLower();
      const QByteArray value = line.mid(colon + 1).trimmed();
      auto it = m_head.headers.find(name);
      if (it != m_head.headers.end() && name == "content-length" && it->second != value) return reject(400, "bad header"), false;
      if (it == m_head.headers.end()) m_head.headers.emplace(name, value);
    }
    const auto header = [&](const char* n) -> QByteArray {
      const auto it = m_head.headers.find(QByteArray(n));
      return it == m_head.headers.end() ? QByteArray() : it->second;
    };
    m_head.close = header("connection").toLower() == "close";

    // Order: route, method, Host and Origin (rebinding), token, then the body rules.
    const QByteArray path = m_head.path.split('?').value(0);
    if (path != "/mcp") return reject(404, "not found"), false;
    if (m_head.method != "POST") return reject(405, "method not allowed", "Allow: POST\r\n"), false;
    const QByteArray port = QByteArray::number(m_server->m_server->serverPort());
    const QByteArray host = header("host").toLower();
    if (host != "127.0.0.1:" + port && host != "localhost:" + port) return reject(403, "forbidden host"), false;
    if (m_head.headers.count("origin") != 0 && !loopbackOrigin(QString::fromLatin1(header("origin")))) return reject(403, "forbidden origin"), false;
    const QByteArray auth = header("authorization");
    const bool bearer = auth.size() > 7 && auth.left(7).toLower() == "bearer ";
    if (!bearer || !tokenEquals(auth.mid(7).trimmed(), m_server->m_token))
      return reject(401, "unauthorized", "WWW-Authenticate: Bearer\r\n"), false;
    if (m_head.headers.count("transfer-encoding") != 0) return reject(411, "length required"), false;
    bool ok = false;
    const qint64 len = header("content-length").toLongLong(&ok);
    if (!ok || len < 0) return reject(411, "length required"), false;
    if (len > kMaxBodyBytes) return reject(413, "body too large"), false;
    m_head.contentLength = len;
    return true;
  }

  void handleBody(const QByteArray& body) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError) {
      sendJson(400, rpcError(QJsonValue::Null, -32700, QStringLiteral("Parse error")));
      return;
    }
    if (!doc.isObject()) {  // batches are not part of the protocol revisions we answer with
      sendJson(400, rpcError(QJsonValue::Null, -32600, QStringLiteral("Invalid Request: send one JSON-RPC message per POST")));
      return;
    }
    const QJsonObject msg = doc.object();
    const QString method = msg.value(QStringLiteral("method")).toString();
    if (method.isEmpty()) {  // a response from the client: nothing to answer
      sendEmpty(202);
      return;
    }
    if (!msg.contains(QStringLiteral("id"))) {  // notification
      sendEmpty(202);
      return;
    }
    const QJsonValue id = msg.value(QStringLiteral("id"));
    const QJsonObject params = msg.value(QStringLiteral("params")).toObject();
    if (method == QLatin1String("initialize")) {
      const QString asked = params.value(QStringLiteral("protocolVersion")).toString();
      static const QStringList supported{QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"), QStringLiteral("2025-11-25")};
      const QString version = supported.contains(asked) ? asked : QStringLiteral("2025-06-18");
      sendJson(200, rpcResult(id, QJsonObject{{QStringLiteral("protocolVersion"), version},
                                               {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject()}}},
                                               {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), m_server->m_name},
                                                                                           {QStringLiteral("version"), m_server->m_version}}}}));
    } else if (method == QLatin1String("ping")) {
      sendJson(200, rpcResult(id, QJsonObject()));
    } else if (method == QLatin1String("tools/list")) {
      sendJson(200, rpcResult(id, QJsonObject{{QStringLiteral("tools"), m_server->m_registry->listJson()}}));
    } else if (method == QLatin1String("tools/call")) {
      const QString name = params.value(QStringLiteral("name")).toString();
      const QJsonValue argsValue = params.value(QStringLiteral("arguments"));
      if (name.isEmpty() || m_server->m_registry->find(name) == nullptr) {
        sendJson(200, rpcError(id, -32602, QStringLiteral("Unknown tool: ") + name.left(80)));
        return;
      }
      if (!argsValue.isUndefined() && !argsValue.isNull() && !argsValue.isObject()) {
        sendJson(200, rpcError(id, -32602, QStringLiteral("arguments must be an object")));
        return;
      }
      QPointer<Connection> self(this);
      m_server->m_registry->call(name, argsValue.toObject(), [self, id](ToolResult r) {
        if (self.isNull()) return;
        self->sendJson(200, rpcResult(id, r.toJson()));
      });
    } else {
      sendJson(200, rpcError(id, -32601, QStringLiteral("Method not found")));
    }
  }

  void sendJson(int status, const QJsonObject& obj) { send(status, QJsonDocument(obj).toJson(QJsonDocument::Compact), "application/json"); }
  void sendEmpty(int status) { send(status, QByteArray(), QByteArray()); }

  void send(int status, const QByteArray& body, const QByteArray& contentType, const QByteArray& extra = QByteArray(), bool close = false) {
    if (!m_socket->isOpen()) return;
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " " + statusText(status) + "\r\n";
    if (!contentType.isEmpty()) out += "Content-Type: " + contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += extra;
    out += (close || m_head.close) ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
    out += "\r\n";
    out += body;
    m_socket->write(out);
    m_busy = false;
    if (close || m_head.close) {
      m_closing = true;  // unread request bytes are dropped; the close waits briefly so the answer is not lost to a reset
      QTimer::singleShot(300, this, [this]() { m_socket->disconnectFromHost(); });
      return;
    }
    m_idle.start(kIdleMs);
    QMetaObject::invokeMethod(this, [this]() { process(); }, Qt::QueuedConnection);  // a pipelined request that is already buffered
  }

  // Protocol-level refusal: a short JSON error body, then the connection closes.
  bool reject(int status, const char* what, const QByteArray& extra = QByteArray()) {
    m_server->noteReject(status);
    const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("error"), QString::fromLatin1(what)}}).toJson(QJsonDocument::Compact);
    send(status, body, "application/json", extra, true);
    return false;
  }

  McpServer* m_server;
  QTcpSocket* m_socket;
  QTimer m_idle;
  QByteArray m_buf;
  Head m_head;
  bool m_haveHead = false;
  bool m_busy = false;
  bool m_closing = false;
};

McpServer::McpServer(McpToolRegistry* registry, QObject* parent) : QObject(parent), m_registry(registry), m_server(new QTcpServer(this)) {
  connect(m_server, &QTcpServer::newConnection, this, [this]() { onNewConnection(); });
}

McpServer::~McpServer() { stop(); }

void McpServer::setServerInfo(const QString& name, const QString& version) {
  m_name = name;
  m_version = version;
}

bool McpServer::listen(quint16 port, const QByteArray& token, QString* error) {
  stop();
  if (token.isEmpty()) {
    if (error) *error = QStringLiteral("No token.");
    return false;
  }
  if (!m_server->listen(QHostAddress::LocalHost, port)) {
    if (error) *error = m_server->errorString();
    return false;
  }
  m_token = token;
  qInfo().noquote() << "mcp: listening on 127.0.0.1:" << m_server->serverPort();
  emit listeningChanged();
  return true;
}

void McpServer::stop() {
  const bool was = m_server->isListening();
  m_server->close();
  const QSet<Connection*> conns = m_conns;
  for (Connection* c : conns) delete c;
  m_token.clear();
  if (was) {
    qInfo() << "mcp: stopped";
    emit listeningChanged();
  }
}

bool McpServer::isListening() const { return m_server->isListening(); }
quint16 McpServer::port() const { return m_server->isListening() ? m_server->serverPort() : 0; }

void McpServer::onNewConnection() {
  while (QTcpSocket* s = m_server->nextPendingConnection()) {
    if (m_conns.size() >= kMaxConnections) {
      s->deleteLater();
      s->abort();
      continue;
    }
    new Connection(this, s);
  }
}

void McpServer::noteReject(int status) {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - m_lastRejectLog < 1000) return;
  m_lastRejectLog = now;
  qInfo("mcp: rejected a request (HTTP %d)", status);
}

}  // namespace uzume::mcp
