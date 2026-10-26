#pragma once
// Tool registry of the debug MCP server (ADR 0032). A tool has a name, a description, a JSON input schema and a handler
// that may answer later (callback), so tools that need the plugin thread, the emulation thread or a frame work too.
// Handlers run on the GUI thread; the answer must come within the timeout (default 10 s, then isError "timeout").
// Later packages add tools (UI control) by calling add(); nothing here knows the app.
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>

namespace uzume::mcp {

inline constexpr int kDefaultToolTimeoutMs = 10000;

struct ToolContent {
  enum class Kind { Text, Image };
  Kind kind = Kind::Text;
  QString text;     // Text
  QByteArray png;   // Image: raw PNG bytes (the server sends them base64 encoded)
};

struct ToolResult {
  bool isError = false;
  QList<ToolContent> content;

  static ToolResult text(const QString& text);
  static ToolResult json(const QJsonValue& value);  // compact JSON text
  static ToolResult error(const QString& message);  // isError with a text
  ToolResult& addImage(const QByteArray& png);
  ToolResult& addText(const QString& text);
  // MCP "CallToolResult": {content: [...], isError}
  QJsonObject toJson() const;
};

// Answers a tool call. Call it exactly once; further calls are ignored. From any thread (it hops to the GUI thread).
using ToolReply = std::function<void(ToolResult)>;

struct ToolDef {
  QString name;
  QString description;
  QJsonObject inputSchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), QJsonObject()}};
  // Name of the argument whose value is written to the log as the target of the call ("id" for plugin tools). Only
  // short identifier-like values are logged; never other argument content and never results.
  QString targetArg;
  int timeoutMs = kDefaultToolTimeoutMs;
  std::function<void(const QJsonObject& args, ToolReply reply)> handler;
};

class McpToolRegistry : public QObject {
  Q_OBJECT
 public:
  explicit McpToolRegistry(QObject* parent = nullptr);

  // Adds a tool; a tool with the same name is replaced.
  void add(ToolDef def);
  QList<ToolDef> tools() const { return m_tools; }
  const ToolDef* find(const QString& name) const;
  // MCP tools/list entries: [{name, description, inputSchema}]
  QJsonArray listJson() const;

  // Logs "mcp: <tool> <target>" and runs the handler with the timeout. `done` is called exactly once, on the GUI thread.
  void call(const QString& name, const QJsonObject& args, ToolReply done);

  // Pure helper (tests): the logged target of a call, empty when none.
  static QString logTarget(const ToolDef& def, const QJsonObject& args);

 private:
  QList<ToolDef> m_tools;
};

}  // namespace uzume::mcp
