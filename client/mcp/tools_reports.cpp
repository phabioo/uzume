// Report tools of the debug MCP server: read-only access to the problem reports on the connected server, through the app's own
// admin connection (the calls behind SERVER > Reports). Nothing here marks, archives or deletes a report; the files are already
// redacted by the app and the server and are passed on unchanged.
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QUrl>
#include <memory>

#include "adminapi.h"
#include "admincontroller.h"
#include "mcp_tools.h"
#include "playercontroller.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;

namespace {

constexpr qint64 kMaxReportFileBytes = 4 * 1024 * 1024;
constexpr int kDefaultReportLimit = 50;
constexpr int kMaxReportLimit = 500;

// Empty when the app may ask the server for reports, else the error code.
QString unavailable(ui::PlayerController& pc) {
  if (!pc.isAdmin()) return QStringLiteral("not_admin");
  HubConnection* c = pc.connection();
  if (c->state() != HubConnection::State::Connected) return QStringLiteral("not_admin");
  if (!c->hubHasFeature(QStringLiteral("admin_v1"))) return QStringLiteral("not_supported");
  return {};
}

ToolResult failure(const AdminApi::Result& r) {
  const QString code = r.code.isEmpty() ? QStringLiteral("error") : r.code;
  ToolResult res = ToolResult::error(r.status == 404 ? QStringLiteral("not_found") : code);
  if (!r.message.isEmpty()) res.addText(r.message);
  return res;
}

QString idPath(const QString& id) { return QStringLiteral("/admin/reports/") + QString::fromLatin1(QUrl::toPercentEncoding(id)); }

bool validId(const QString& id) {
  if (id.isEmpty() || id.size() > 64) return false;
  for (const QChar c : id)
    if (!c.isLetterOrNumber() && c != QLatin1Char('-') && c != QLatin1Char('_')) return false;
  return true;
}

bool validName(const QString& n) {
  return !n.isEmpty() && n.size() <= 200 && !n.contains(QLatin1Char('/')) && !n.contains(QLatin1Char('\\')) && n != QLatin1String("..") && n != QLatin1String(".");
}

// The id argument may be a number or a string.
QString idArg(const QJsonValue& v) { return v.isDouble() ? QString::number(static_cast<qint64>(v.toDouble())) : v.toString(); }

}  // namespace

void installReportTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);

  {
    ToolDef t;
    t.name = QStringLiteral("reports_list");
    t.description = QStringLiteral(
        "Problem reports on the connected server (SERVER > Reports, admin only): id, status, title, sender user and device, app version, OS, "
        "created, size and file count, plus the counts. Read-only.");
    t.inputSchema = schema({{QStringLiteral("state"), typeStr(QStringLiteral("new | seen | all (default)."))},
                            {QStringLiteral("limit"), typeInt(QStringLiteral("At most this many reports, default 50, max 500."))}});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      if (const QString why = unavailable(*pc); !why.isEmpty()) return reply(ToolResult::error(why));
      const QString state = a.value(QStringLiteral("state")).toString(QStringLiteral("all"));
      if (state != QLatin1String("all") && state != QLatin1String("new") && state != QLatin1String("seen"))
        return reply(ToolResult::error(QStringLiteral("state must be new, seen or all (the server has no archived state)")));
      const int limit = std::clamp(a.contains(QStringLiteral("limit")) ? a.value(QStringLiteral("limit")).toInt(kDefaultReportLimit) : kDefaultReportLimit, 1, kMaxReportLimit);
      const QString path = state == QLatin1String("all") ? QStringLiteral("/admin/reports") : QStringLiteral("/admin/reports?status=") + state;
      pc->admin()->api()->request(QStringLiteral("GET"), path, QVariant(), [reply, limit](const AdminApi::Result& r) {
        if (!r.ok) return reply(failure(r));
        QJsonObject o = QJsonValue::fromVariant(r.data).toObject();
        QJsonArray list = o.value(QStringLiteral("reports")).toArray();
        const int total = static_cast<int>(list.size());
        while (list.size() > limit) list.removeLast();
        o.insert(QStringLiteral("reports"), list);
        o.insert(QStringLiteral("shown"), static_cast<int>(list.size()));
        if (total > list.size()) o.insert(QStringLiteral("truncated"), true);
        reply(ToolResult::json(o));
      });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("report_get");
    t.description = QStringLiteral(
        "One problem report: description, sender user and device, app version, OS, created, status, the linked event and the attachments "
        "(names and sizes). Does not mark it as seen.");
    t.inputSchema = schema({{QStringLiteral("id"), typeAny(QStringLiteral("Report id."))}}, {QStringLiteral("id")});
    t.targetArg = QStringLiteral("id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      if (const QString why = unavailable(*pc); !why.isEmpty()) return reply(ToolResult::error(why));
      const QString id = idArg(a.value(QStringLiteral("id")));
      if (!validId(id)) return reply(ToolResult::error(QStringLiteral("id required")));
      pc->admin()->api()->request(QStringLiteral("GET"), idPath(id), QVariant(), [reply](const AdminApi::Result& r) {
        if (!r.ok) return reply(failure(r));
        reply(ToolResult::json(QJsonValue::fromVariant(r.data)));
      });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("report_file");
    t.description = QStringLiteral(
        "One attachment of a problem report (already redacted): text as text, JSON pretty-printed, zip and other binary as base64. Max 4 MiB.");
    t.inputSchema = schema({{QStringLiteral("id"), typeAny(QStringLiteral("Report id."))}, {QStringLiteral("name"), typeStr(QStringLiteral("File name from report_get."))}},
                           {QStringLiteral("id"), QStringLiteral("name")});
    t.targetArg = QStringLiteral("id");
    t.timeoutMs = 30000;
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      if (const QString why = unavailable(*pc); !why.isEmpty()) return reply(ToolResult::error(why));
      const QString id = idArg(a.value(QStringLiteral("id")));
      const QString name = a.value(QStringLiteral("name")).toString();
      if (!validId(id) || !validName(name)) return reply(ToolResult::error(QStringLiteral("id and name required")));
      AdminApi* api = pc->admin()->api();
      // The size comes from the report itself, so an oversized file is never fetched.
      api->request(QStringLiteral("GET"), idPath(id), QVariant(), [api, id, name, reply](const AdminApi::Result& r) {
        if (!r.ok) return reply(failure(r));
        qint64 size = -1;
        for (const QJsonValue& f : QJsonValue::fromVariant(r.data).toObject().value(QStringLiteral("files")).toArray())
          if (f.toObject().value(QStringLiteral("name")).toString() == name) size = static_cast<qint64>(f.toObject().value(QStringLiteral("size")).toDouble());
        if (size < 0) return reply(ToolResult::error(QStringLiteral("not_found")));
        if (size > kMaxReportFileBytes) return reply(ToolResult::error(QStringLiteral("too_large: %1 bytes, the limit is %2").arg(size).arg(kMaxReportFileBytes)));
        auto dir = std::make_shared<QTemporaryDir>();
        if (!dir->isValid()) return reply(ToolResult::error(QStringLiteral("no_temp_dir")));
        const QString target = dir->filePath(QStringLiteral("file"));
        api->download(idPath(id) + QStringLiteral("/files/") + QString::fromLatin1(QUrl::toPercentEncoding(name)), target, [dir, target, name, reply](const AdminApi::Result& d) {
          if (!d.ok) return reply(failure(d));
          QFile f(target);
          if (!f.open(QIODevice::ReadOnly)) return reply(ToolResult::error(QStringLiteral("read_failed")));
          const QByteArray data = f.read(kMaxReportFileBytes + 1);
          if (data.size() > kMaxReportFileBytes) return reply(ToolResult::error(QStringLiteral("too_large")));
          const bool zip = name.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive) || data.startsWith(QByteArray("PK\x03\x04"));
          QStringConverter::Encoding enc = QStringConverter::Utf8;
          QStringDecoder dec(enc, QStringConverter::Flag::Stateless);
          const QString text = dec.decode(data);
          if (zip || data.contains('\0') || dec.hasError())
            return reply(ToolResult::json(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("size"), static_cast<int>(data.size())},
                                                      {QStringLiteral("encoding"), QStringLiteral("base64")}, {QStringLiteral("data"), QString::fromLatin1(data.toBase64())}}));
          QJsonParseError pe;
          const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
          if (pe.error == QJsonParseError::NoError && name.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
            return reply(ToolResult::text(QString::fromUtf8(doc.toJson(QJsonDocument::Indented))));
          reply(ToolResult::text(text));
        });
      });
    };
    registry.add(std::move(t));
  }
}

}  // namespace uzume::mcp
