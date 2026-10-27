// run_recap (Uzume 0.22 package 2): the computed model of the run recap (tiles, lanes with their events per day), built by the same
// controller the Recap tab uses (all events of the run, its screenshots, the `recap` mappings of the plugins on this PC).
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>

#include "mcp_tools.h"
#include "playercontroller.h"
#include "runrecapcontroller.h"
#include "runscontroller.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;

QString validateRunRecapArgs(const QJsonObject& a) {
  const QJsonValue v = a.value(QStringLiteral("run_id"));
  if (!v.isString() || v.toString().trimmed().isEmpty()) return QStringLiteral("run_id is required (a non-empty string)");
  static const QRegularExpression uuid(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
  if (!uuid.match(v.toString().trimmed()).hasMatch()) return QStringLiteral("run_id must be a run id (uuid)");
  return {};
}

void installRunRecapTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);
  ToolDef t;
  t.name = QStringLiteral("run_recap");
  t.description = QStringLiteral(
      "The run recap of a run on the connected server as the Recap tab computes it: tiles (catches, lost, badges, play time), and one "
      "lane per person with the events drawn on it (glyph catch | lost | badge | note | screenshot, type, text, day, time) and the "
      "same events grouped per day. Reads the whole event log and the run's screenshots; events of plugins that are not installed on "
      "this PC and events without a recap mapping are not part of it. Read-only.");
  t.inputSchema = schema({{QStringLiteral("run_id"), typeStr(QStringLiteral("The run id (uuid), as shown by the Runs screen."))}}, {QStringLiteral("run_id")});
  t.targetArg = QStringLiteral("run_id");
  t.timeoutMs = 60000;
  t.handler = [pc](const QJsonObject& args, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    if (const QString bad = validateRunRecapArgs(args); !bad.isEmpty()) return reply(ToolResult::error(bad));
    ui::RunsController* runs = pc->runs();
    if (runs == nullptr || !runs->available()) return reply(ToolResult::error(QStringLiteral("not_connected: no server with runs is connected")));
    runs->recap()->compute(args.value(QStringLiteral("run_id")).toString().trimmed(),
                           [reply](bool ok, const QString& error, const RecapModel& model) {
                             if (!ok) return reply(ToolResult::error(error));
                             reply(ToolResult::json(model.toJson()));
                           });
  };
  registry.add(std::move(t));
}

}  // namespace uzume::mcp
