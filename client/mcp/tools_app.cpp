// App tools of the debug MCP server: status, logs, settings, diagnostics bundle, connection check.
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QtConcurrent>
#include <memory>

#include "connectioncheckcontroller.h"
#include "diagbundle.h"
#include "filelog.h"
#include "mcp_tools.h"
#include "playercontroller.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;

namespace {

constexpr qint64 kMaxLogRead = 4ll * 1024 * 1024;  // newest bytes of one log file that a call looks at

// Log files newest first: player.log, player.1.log ...
QStringList logFiles() {
  QStringList out;
  const QString current = filelog::path();
  if (current.isEmpty()) return out;
  const QFileInfo fi(current);
  out << fi.absoluteFilePath();
  for (int i = 1; i <= 5; ++i) {
    const QString p = fi.absoluteDir().filePath(fi.completeBaseName() + QLatin1Char('.') + QString::number(i) + QLatin1Char('.') + fi.suffix());
    if (QFileInfo::exists(p)) out << p;
  }
  return out;
}

// The newest `kMaxLogRead` bytes of a file as redacted lines (oldest first).
QStringList readLines(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  const qint64 size = f.size();
  if (size > kMaxLogRead) f.seek(size - kMaxLogRead);
  QByteArray data = f.readAll();
  if (size > kMaxLogRead) {
    const qsizetype nl = data.indexOf('\n');
    if (nl >= 0) data.remove(0, nl + 1);  // a cut first line is dropped
  }
  data = diag::redact(data);
  QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  for (QString& l : lines)
    if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
  return lines;
}

// 0 debug .. 3 error; -1 unknown line. Format: "<ISO time> [L] category: message"
int lineLevel(const QString& line) {
  const qsizetype a = line.indexOf(QLatin1String(" ["));
  if (a < 0 || a + 3 >= line.size() || line[a + 3] != QLatin1Char(']')) return -1;
  switch (line[a + 2].toLatin1()) {
    case 'D': return 0;
    case 'I': return 1;
    case 'W': return 2;
    case 'E':
    case 'F': return 3;
    default: return -1;
  }
}

int levelFromName(const QString& name) {
  const QString n = name.toLower();
  if (n == QLatin1String("debug")) return 0;
  if (n == QLatin1String("info")) return 1;
  if (n == QLatin1String("warning") || n == QLatin1String("warn")) return 2;
  if (n == QLatin1String("error")) return 3;
  return -1;
}

int clampInt(const QJsonValue& v, int def, int lo, int hi) {
  const int n = v.isDouble() ? v.toInt(def) : def;
  return (std::max)(lo, (std::min)(hi, n));
}

}  // namespace

void installAppTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);

  {
    ToolDef t;
    t.name = QStringLiteral("status");
    t.description = QStringLiteral(
        "App version, channel and commit, OS, data directory, portable mode, the servers (name, address, connected; no tokens), the current screen, "
        "the running game (id, title, system, core), developer mode and the running plugins.");
    t.inputSchema = schema();
    t.handler = [pc](const QJsonObject&, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QJsonObject info = pc->diagnosticsSnapshot();
      QJsonArray servers;
      for (const QJsonValue& h : info.value(QStringLiteral("hubs")).toArray()) {
        const QJsonObject o = h.toObject();
        servers.append(QJsonObject{{QStringLiteral("name"), o.value(QStringLiteral("name"))},
                                   {QStringLiteral("address"), o.value(QStringLiteral("address"))},
                                   {QStringLiteral("connected"), o.value(QStringLiteral("connected"))},
                                   {QStringLiteral("server_version"), o.value(QStringLiteral("hub_version"))}});
      }
      QJsonObject game;
      const QVariantMap g = pc->runningGameInfo();
      if (!g.isEmpty()) {
        game = QJsonObject{{QStringLiteral("id"), g.value(QStringLiteral("id")).toString()},
                           {QStringLiteral("title"), g.value(QStringLiteral("title")).toString()},
                           {QStringLiteral("system"), g.value(QStringLiteral("system")).toString()},
                           {QStringLiteral("core"), g.value(QStringLiteral("core")).toString()}};
      }
      QJsonArray plugins;
      for (const QString& id : pc->plugins()->host()->pluginIds()) plugins.append(id);
      const QJsonObject sys = info.value(QStringLiteral("system")).toObject();
      reply(ToolResult::json(QJsonObject{
          {QStringLiteral("app"), info.value(QStringLiteral("player"))},
          {QStringLiteral("os"), sys.value(QStringLiteral("os"))},
          {QStringLiteral("arch"), sys.value(QStringLiteral("arch"))},
          {QStringLiteral("qt_version"), sys.value(QStringLiteral("qt_version"))},
          {QStringLiteral("data_dir"), sys.value(QStringLiteral("data_dir"))},
          {QStringLiteral("portable"), sys.value(QStringLiteral("portable"))},
          {QStringLiteral("servers"), servers},
          {QStringLiteral("screen"), pc->screen()},
          {QStringLiteral("game"), game.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(game)},
          {QStringLiteral("developer_mode"), pc->plugins()->developerMode()},
          {QStringLiteral("plugins_running"), plugins}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("logs_tail");
    t.description = QStringLiteral("The newest lines of the app log (redacted like the diagnostics bundle). Lines look like \"<time> [I] category: message\".");
    t.inputSchema = schema({{QStringLiteral("lines"), typeInt(QStringLiteral("How many lines, 1 to 2000 (default 200)."))},
                            {QStringLiteral("contains"), typeStr(QStringLiteral("Only lines that contain this text (case-insensitive)."))},
                            {QStringLiteral("level"), typeStr(QStringLiteral("Only this level and worse: debug | info | warning | error."))}});
    t.handler = [](const QJsonObject& a, ToolReply reply) {
      const int n = clampInt(a.value(QStringLiteral("lines")), 200, 1, kMaxLogLines);
      const QString contains = a.value(QStringLiteral("contains")).toString();
      const int minLevel = a.contains(QStringLiteral("level")) ? levelFromName(a.value(QStringLiteral("level")).toString()) : 0;
      if (minLevel < 0) return reply(ToolResult::error(QStringLiteral("level must be debug, info, warning or error")));
      const QStringList files = logFiles();
      if (files.isEmpty()) return reply(ToolResult::error(QStringLiteral("File logging is not active.")));
      QStringList out;
      for (const QString& f : files) {  // newest file first, until enough lines are collected
        const QStringList lines = readLines(f);
        for (qsizetype i = lines.size() - 1; i >= 0 && out.size() < n; --i) {
          const QString& l = lines[i];
          if (!contains.isEmpty() && !l.contains(contains, Qt::CaseInsensitive)) continue;
          if (minLevel > 0 && lineLevel(l) < minLevel) continue;
          out.prepend(l);
        }
        if (out.size() >= n) break;
      }
      QJsonArray arr;
      for (const QString& l : out) arr.append(l);
      reply(ToolResult::json(QJsonObject{{QStringLiteral("count"), static_cast<int>(out.size())}, {QStringLiteral("lines"), arr}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("logs_search");
    t.description = QStringLiteral("Searches the app log and its rotated files with a regular expression (case-insensitive); newest matches first.");
    t.inputSchema = schema({{QStringLiteral("pattern"), typeStr(QStringLiteral("Regular expression (PCRE)."))},
                            {QStringLiteral("max"), typeInt(QStringLiteral("At most this many matches, 1 to 2000 (default 100)."))}},
                           {QStringLiteral("pattern")});
    t.handler = [](const QJsonObject& a, ToolReply reply) {
      const QRegularExpression re(a.value(QStringLiteral("pattern")).toString(), QRegularExpression::CaseInsensitiveOption);
      if (!re.isValid()) return reply(ToolResult::error(QStringLiteral("invalid pattern: ") + re.errorString()));
      const int max = clampInt(a.value(QStringLiteral("max")), 100, 1, kMaxLogLines);
      const QStringList files = logFiles();
      if (files.isEmpty()) return reply(ToolResult::error(QStringLiteral("File logging is not active.")));
      QJsonArray arr;
      for (const QString& f : files) {
        const QStringList lines = readLines(f);
        for (qsizetype i = lines.size() - 1; i >= 0 && arr.size() < max; --i)
          if (re.match(lines[i]).hasMatch()) arr.append(lines[i]);
        if (arr.size() >= max) break;
      }
      reply(ToolResult::json(QJsonObject{{QStringLiteral("count"), static_cast<int>(arr.size())}, {QStringLiteral("matches"), arr}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("settings_get");
    t.description = QStringLiteral("The non-secret settings summary of the diagnostics bundle (player-info.json \"settings\") plus emulation, media and the Claude debugging switch.");
    t.inputSchema = schema();
    t.handler = [pc](const QJsonObject&, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QJsonObject info = pc->diagnosticsSnapshot();
      reply(ToolResult::json(QJsonObject{{QStringLiteral("settings"), info.value(QStringLiteral("settings"))},
                                         {QStringLiteral("emulation"), info.value(QStringLiteral("emulation"))},
                                         {QStringLiteral("media"), info.value(QStringLiteral("media"))},
                                         {QStringLiteral("controllers"), info.value(QStringLiteral("controllers"))},
                                         {QStringLiteral("mcp"), QJsonObject{{QStringLiteral("port"), pc->mcp()->port()}}}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("collect_diagnostics");
    t.description = QStringLiteral(
        "Writes the diagnostics bundle (the \"Collect logs\" zip: redacted logs, system information, no passwords or tokens). Default: <data>/diag/. "
        "Returns the path. Never overwrites an existing file.");
    t.inputSchema = schema({{QStringLiteral("path"), typeStr(QStringLiteral("Output .zip path (default: <data>/diag/uzume-diagnostics-<time>.zip)."))}});
    t.timeoutMs = 60000;
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString dataDir = pc->profileStore()->baseDir();
      QString out = a.value(QStringLiteral("path")).toString();
      if (out.isEmpty()) {
        QDir().mkpath(QDir(dataDir).filePath(QStringLiteral("diag")));
        out = QDir(dataDir).filePath(QStringLiteral("diag/") + diag::fileName(QDateTime::currentDateTimeUtc()));
      }
      if (QFileInfo::exists(out)) return reply(ToolResult::error(QStringLiteral("file_exists")));
      const QJsonObject info = pc->diagnosticsSnapshot();
      (void)QtConcurrent::run([out, info, dataDir, reply]() {
        QString err;
        if (diag::writeBundle(out, info, dataDir, {}, &err)) {
          reply(ToolResult::json(QJsonObject{{QStringLiteral("path"), QDir::toNativeSeparators(out)}, {QStringLiteral("bytes"), static_cast<double>(QFileInfo(out).size())}}));
        } else {
          reply(ToolResult::error(err.isEmpty() ? QStringLiteral("could not write the bundle") : err));
        }
      });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("connection_check");
    t.description = QStringLiteral("Runs the connection check (server reachable, STUN, TURN, NAT type, test connection) and returns its JSON result. Needs a connected server.");
    t.inputSchema = schema();
    t.timeoutMs = 90000;
    t.handler = [pc](const QJsonObject&, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      ui::ConnectionCheckController* cc = pc->connectionCheck();
      if (!cc->available()) return reply(ToolResult::error(QStringLiteral("not_connected")));
      if (cc->running()) return reply(ToolResult::error(QStringLiteral("already_running")));
      auto conn = std::make_shared<QMetaObject::Connection>();
      QPointer<ui::ConnectionCheckController> ccp(cc);
      *conn = QObject::connect(cc, &ui::ConnectionCheckController::changed, cc, [ccp, conn, reply]() {
        if (!ccp || ccp->running() || !ccp->finished()) return;
        QObject::disconnect(*conn);
        const QJsonDocument doc = QJsonDocument::fromJson(ccp->lastResultJson().toUtf8());
        reply(ToolResult::json(doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(QJsonObject())));
      });
      cc->run();
    };
    registry.add(std::move(t));
  }
}

}  // namespace uzume::mcp
