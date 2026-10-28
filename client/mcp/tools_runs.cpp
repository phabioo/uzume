// Run tools of the debug MCP server (Uzume 0.20 package 6): list the runs of the connected server, read their log, change the core
// rules (run_rules_v1), plus the plugin helpers needed to drive plugins without a mouse: link a developer folder, turn Developer mode on or off, and answer an open uz.ui.pick_file request with a path. The handlers use the same calls the UI uses
// (RunClient::setRules, PluginsController::linkDevFolder / completePick).
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>

#include "mcp_tools.h"
#include "playercontroller.h"
#include "pluginscontroller.h"
#include "runclient.h"
#include "runrules.h"
#include "runscontroller.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;

namespace {

QJsonObject rulesJson(const RunRules& r) { return runRulesToJson(r); }

QJsonObject runJson(const RunInfo& r) {
  QJsonArray people;
  for (const RunParticipant& p : r.participants)
    people.append(QJsonObject{{QStringLiteral("user_id"), p.userId}, {QStringLiteral("name"), p.userName}, {QStringLiteral("status"), p.status},
                              {QStringLiteral("game_id"), p.gameId}});
  QJsonArray plugins;
  for (const RunPluginRef& p : r.plugins) plugins.append(QJsonObject{{QStringLiteral("plugin_id"), p.pluginId}, {QStringLiteral("version"), p.version}});
  return QJsonObject{{QStringLiteral("run_id"), r.runId},         {QStringLiteral("name"), r.name},          {QStringLiteral("status"), r.status},
                     {QStringLiteral("created_by"), r.createdBy}, {QStringLiteral("save_slot"), r.saveSlot}, {QStringLiteral("rules"), rulesJson(r.rules)},
                     {QStringLiteral("plugins"), plugins},        {QStringLiteral("participants"), people}, {QStringLiteral("last_seq"), static_cast<double>(r.lastSeq)}};
}

}  // namespace

void installRunTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);

  {
    ToolDef t;
    t.name = QStringLiteral("runs_list");
    t.description = QStringLiteral(
        "Lists the runs of the connected server that the signed-in person is part of: id, name, status, creator, save slot, core rules "
        "{states, rewind, cheats, memory_write}, plugins and participants. Needs a server with runs_v1.");
    t.inputSchema = schema({{QStringLiteral("status"), typeStr(QStringLiteral("Only runs with this status (starting, active, paused, finished, abandoned)."))}});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc || pc->runs() == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      RunClient* c = pc->runs()->client();
      if (!c->supported()) return reply(ToolResult::error(QStringLiteral("not_supported")));
      const QString status = a.value(QStringLiteral("status")).toString();
      QJsonArray out;
      for (const RunInfo& r : c->runs())
        if (status.isEmpty() || r.status == status) out.append(runJson(r));
      reply(ToolResult::json(QJsonObject{{QStringLiteral("runs"), out}, {QStringLiteral("loaded"), c->loaded()}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("run_set_rules");
    t.description = QStringLiteral(
        "Changes the core rules of a run with PUT /runs/{id}/rules (only the creator or an admin; 403 forbidden otherwise). Give the "
        "keys to change; the others keep their value, the whole object is sent. Returns the rules the server now has.");
    t.inputSchema = schema({{QStringLiteral("run_id"), typeStr(QStringLiteral("Run id."))},
                            {QStringLiteral("states"), typeBool(QStringLiteral("Save states allowed."))},
                            {QStringLiteral("rewind"), typeBool(QStringLiteral("Rewind allowed."))},
                            {QStringLiteral("cheats"), typeStr(QStringLiteral("\"off\" or \"logged\"."))},
                            {QStringLiteral("memory_write"), typeStr(QStringLiteral("\"off\" or \"logged\"."))}},
                           {QStringLiteral("run_id")});
    t.targetArg = QStringLiteral("run_id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc || pc->runs() == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      RunClient* c = pc->runs()->client();
      const QString id = a.value(QStringLiteral("run_id")).toString().toLower();
      const auto run = c->run(id);
      if (!run) return reply(ToolResult::error(QStringLiteral("run_not_found")));
      if (!c->supportsRules()) return reply(ToolResult::error(QStringLiteral("not_supported")));
      RunRules rules = run->rules;
      for (const QString& key : {QStringLiteral("states"), QStringLiteral("rewind"), QStringLiteral("cheats"), QStringLiteral("memory_write")}) {
        if (!a.contains(key)) continue;
        const QJsonValue v = a.value(key);
        const bool isBool = key == QLatin1String("states") || key == QLatin1String("rewind");
        if (isBool ? !v.isBool() : !v.isString()) return reply(ToolResult::error(QStringLiteral("invalid %1").arg(key)));
        if (!ui::runrules::setRule(rules, key, v.toVariant())) return reply(ToolResult::error(QStringLiteral("invalid %1").arg(key)));
      }
      c->setRules(id, rules, [reply](const RunResult& r) {
        if (!r.ok) return reply(ToolResult::error(r.errorCode.isEmpty() ? QStringLiteral("failed") : r.errorCode));
        reply(ToolResult::json(QJsonObject{{QStringLiteral("rules"), r.run ? rulesJson(r.run->rules) : QJsonObject()}}));
      });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("run_events");
    t.description = QStringLiteral(
        "Reads the event log of a run from the app's cache (rules.changed, cheat.enabled, cheat.disabled, memory.write, plugin events): "
        "seq, time, user, plugin, source, type, text, payload. Oldest first, after_seq filters.");
    t.inputSchema = schema({{QStringLiteral("run_id"), typeStr(QStringLiteral("Run id."))},
                            {QStringLiteral("after_seq"), typeInt(QStringLiteral("Only events with a higher seq (default 0)."))},
                            {QStringLiteral("type"), typeStr(QStringLiteral("Only this event type, e.g. \"rules.changed\"."))},
                            {QStringLiteral("limit"), typeInt(QStringLiteral("At most this many events (default 200, max 500)."))}},
                           {QStringLiteral("run_id")});
    t.targetArg = QStringLiteral("run_id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc || pc->runs() == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      RunClient* c = pc->runs()->client();
      const QString id = a.value(QStringLiteral("run_id")).toString().toLower();
      if (!c->run(id)) return reply(ToolResult::error(QStringLiteral("run_not_found")));
      const int limit = qBound(1, a.value(QStringLiteral("limit")).toInt(200), 500);
      const QString type = a.value(QStringLiteral("type")).toString();
      QJsonArray out;
      for (const RunEvent& e : c->events(id, static_cast<qint64>(a.value(QStringLiteral("after_seq")).toDouble(0)), 500)) {
        if (!type.isEmpty() && e.type != type) continue;
        if (out.size() >= limit) break;
        out.append(QJsonObject{{QStringLiteral("seq"), static_cast<double>(e.seq)}, {QStringLiteral("time"), e.createdAt}, {QStringLiteral("user_id"), e.userId},
                               {QStringLiteral("user_name"), e.userName}, {QStringLiteral("plugin_id"), e.pluginId}, {QStringLiteral("source"), e.source},
                               {QStringLiteral("type"), e.type}, {QStringLiteral("text"), e.text}, {QStringLiteral("payload"), e.payload}});
      }
      reply(ToolResult::json(QJsonObject{{QStringLiteral("events"), out}, {QStringLiteral("last_seq"), static_cast<double>(c->lastSeq(id))}}));
    };
    registry.add(std::move(t));
  }

  // ---- plugin helpers ----
  {
    ToolDef t;
    t.name = QStringLiteral("developer_mode");
    t.description = QStringLiteral("Turns Developer mode (Settings > Plugins) on or off, or reads it when `enabled` is missing. Local plugins and linked folders run only while it is on.");
    t.inputSchema = schema({{QStringLiteral("enabled"), typeBool(QStringLiteral("On or off; leave out to only read."))}});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc || pc->plugins() == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      if (a.contains(QStringLiteral("enabled"))) pc->plugins()->setDeveloperMode(a.value(QStringLiteral("enabled")).toBool());
      reply(ToolResult::json(QJsonObject{{QStringLiteral("developer_mode"), pc->plugins()->developerMode()}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_link");
    t.description = QStringLiteral(
        "Links a plugin folder for development (like Settings > Plugins > Link folder): it reloads when its files change. Needs Developer "
        "mode (developer_mode). The plugin stays off until plugin_set_enabled.");
    t.inputSchema = schema({{QStringLiteral("path"), typeStr(QStringLiteral("Absolute path of the plugin folder (the one with manifest.json)."))}}, {QStringLiteral("path")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc || pc->plugins() == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      if (!pc->plugins()->developerMode()) return reply(ToolResult::error(QStringLiteral("developer_mode_off")));
      const QString path = a.value(QStringLiteral("path")).toString();
      if (path.isEmpty() || !QFileInfo(path).isDir()) return reply(ToolResult::error(QStringLiteral("path not found")));
      pc->plugins()->linkDevFolder(path);
      reply(ToolResult::json(QJsonObject{{QStringLiteral("message"), pc->plugins()->message()}, {QStringLiteral("error"), pc->plugins()->messageIsError()}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("file_dialog_pick");
    t.description = QStringLiteral(
        "Answers an open uz.ui.pick_file request of a plugin with a file path, without any dialog (the dialog or path field that is open "
        "closes). The plugin gets the content, never the path. An empty path cancels. Without plugin_id the oldest open request is answered; "
        "error \"no_pick_request\" when none is open.");
    t.inputSchema = schema({{QStringLiteral("path"), typeStr(QStringLiteral("Absolute path of the file; empty = cancel."))},
                            {QStringLiteral("plugin_id"), typeStr(QStringLiteral("Only a request of this plugin."))}},
                           {QStringLiteral("path")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc || pc->plugins() == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString want = a.value(QStringLiteral("plugin_id")).toString();
      for (const QVariant& v : pc->plugins()->pendingPicks()) {
        const QVariantMap m = v.toMap();
        if (!want.isEmpty() && m.value(QStringLiteral("pluginId")).toString() != want) continue;
        const QString pid = m.value(QStringLiteral("pluginId")).toString(), rid = m.value(QStringLiteral("requestId")).toString();
        const QString path = a.value(QStringLiteral("path")).toString();
        if (!path.isEmpty() && !QFileInfo(path).isFile()) return reply(ToolResult::error(QStringLiteral("path not found")));
        pc->plugins()->completePick(pid, rid, path);
        return reply(ToolResult::json(QJsonObject{{QStringLiteral("plugin_id"), pid}, {QStringLiteral("request_id"), rid}, {QStringLiteral("cancelled"), path.isEmpty()}}));
      }
      reply(ToolResult::error(QStringLiteral("no_pick_request")));
    };
    registry.add(std::move(t));
  }
}

}  // namespace uzume::mcp
