#pragma once
// Debug MCP server (ADR 0032 D1): "Streamable HTTP" with JSON responses on 127.0.0.1 only, POST /mcp, JSON-RPC 2.0, a
// bearer token, a Host and Origin check (DNS rebinding) and bounded requests. No SSE stream, no sessions. The server
// only parses and dispatches; the tools live in the McpToolRegistry.
#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>

class QTcpServer;

namespace uzume::mcp {

class McpToolRegistry;

inline constexpr quint16 kDefaultPort = 47615;
inline constexpr qint64 kMaxBodyBytes = 1024 * 1024;
inline constexpr qint64 kMaxHeaderBytes = 16 * 1024;

// 32 random bytes, base64url without padding (43 characters).
QByteArray generateToken();
// Comparison whose time does not depend on where the inputs differ.
bool tokenEquals(const QByteArray& a, const QByteArray& b);

class McpServer : public QObject {
  Q_OBJECT
 public:
  explicit McpServer(McpToolRegistry* registry, QObject* parent = nullptr);
  ~McpServer() override;

  // serverInfo of initialize.
  void setServerInfo(const QString& name, const QString& version);
  // Starts listening on 127.0.0.1:port (0 = any free port, tests). false + *error (a plain sentence) on failure. A running
  // server is stopped first; open connections are closed.
  bool listen(quint16 port, const QByteArray& token, QString* error = nullptr);
  void stop();
  bool isListening() const;
  quint16 port() const;  // the bound port

 signals:
  void listeningChanged();

 private:
  class Connection;
  friend class Connection;
  void onNewConnection();
  // Rate-limited line in the log for a rejected request (status only, never header content).
  void noteReject(int status);

  McpToolRegistry* m_registry;
  QTcpServer* m_server;
  QByteArray m_token;
  QString m_name = QStringLiteral("uzume");
  QString m_version;
  qint64 m_lastRejectLog = 0;
  QSet<Connection*> m_conns;
};

}  // namespace uzume::mcp
