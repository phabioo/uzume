// Plugin tools of the debug MCP server: list, console, UI trees and actions, reload, call a provided function, eval in a local
// plugin, cache files and images. All Lua work runs on the plugin thread (PluginHost::callProvided / evalChunk).
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <memory>

#include "mcp_tools.h"
#include "playercontroller.h"
#include "plugin_engine.h"
#include "plugin_host.h"
#include "run_bridge.h"
#include "pluginimageprovider.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;
using plugins::PluginStatus;
using plugins::ProbeResult;
using plugins::Value;

namespace {

constexpr qint64 kMaxCacheFileRead = 8ll * 1024 * 1024;
constexpr int kMaxCacheListEntries = 2000;

QString statusName(PluginStatus s) {
  switch (s) {
    case PluginStatus::Loading: return QStringLiteral("loading");
    case PluginStatus::Running: return QStringLiteral("running");
    case PluginStatus::Paused: return QStringLiteral("paused");
    case PluginStatus::Stopped: return QStringLiteral("stopped");
  }
  return QStringLiteral("unknown");
}

QString levelName(plugins::LogLevel l) {
  switch (l) {
    case plugins::LogLevel::Debug: return QStringLiteral("debug");
    case plugins::LogLevel::Info: return QStringLiteral("info");
    case plugins::LogLevel::Warn: return QStringLiteral("warn");
    case plugins::LogLevel::Error: return QStringLiteral("error");
  }
  return QStringLiteral("info");
}

QString isoTime(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toUTC().toString(Qt::ISODateWithMs); }

QJsonArray logJson(const QList<plugins::LogLine>& lines) {
  QJsonArray a;
  for (const plugins::LogLine& l : lines)
    a.append(QJsonObject{{QStringLiteral("time"), isoTime(l.timeMs)}, {QStringLiteral("level"), levelName(l.level)}, {QStringLiteral("text"), l.text}});
  return a;
}

// Result of plugin_call / plugin_eval: results as JSON (images replaced by references), output lines, images as PNG content.
ToolResult probeToResult(const ProbeResult& r) {
  if (!r.ok) {
    QJsonObject o{{QStringLiteral("error"), r.error}};
    if (!r.trace.isEmpty()) o.insert(QStringLiteral("trace"), r.trace);
    if (r.paused) o.insert(QStringLiteral("paused"), true);
    if (!r.output.isEmpty()) o.insert(QStringLiteral("output"), logJson(r.output));
    ToolResult res = ToolResult::json(o);
    res.isError = true;
    return res;
  }
  QList<QByteArray> images;
  QJsonArray results;
  for (const Value& v : r.values) results.append(valueToJson(v, images));
  ToolResult res = ToolResult::json(QJsonObject{{QStringLiteral("results"), results},
                                                {QStringLiteral("output"), logJson(r.output)},
                                                {QStringLiteral("images"), static_cast<int>(images.size())}});
  for (const QByteArray& png : images) res.addImage(png);
  return res;
}

std::optional<plugins::PluginInfo> infoOf(ui::PlayerController& pc, const QString& id) { return pc.plugins()->manager()->info(id); }

QJsonObject idSchemaProp() { return {{QStringLiteral("id"), typeStr(QStringLiteral("Plugin id, e.g. uzume.soullink."))}}; }

}  // namespace

void installPluginTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);
  const auto idProps = [](QJsonObject more = {}) {
    QJsonObject p = idSchemaProp();
    for (auto it = more.begin(); it != more.end(); ++it) p.insert(it.key(), it.value());
    return p;
  };

  {
    ToolDef t;
    t.name = QStringLiteral("plugins_list");
    t.description = QStringLiteral("Installed and running plugins: id, name, version, origin (official | unverified | local), status, error, provided interfaces, permissions.");
    t.inputSchema = schema();
    t.handler = [pc](const QJsonObject&, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      plugins::PluginHost* host = pc->plugins()->host();
      const QStringList running = host->pluginIds();
      QJsonArray arr;
      QStringList seen;
      for (const plugins::PluginInfo& p : pc->plugins()->manager()->list()) {
        seen << p.manifest.id;
        const bool started = running.contains(p.manifest.id);
        const plugins::ConsoleSnapshot snap = started ? host->consoleSnapshot(p.manifest.id) : plugins::ConsoleSnapshot();
        QJsonArray provides, perms, problems;
        for (const QString& s : p.manifest.provides) provides.append(s);
        for (const QString& s : p.manifest.permissions) perms.append(s);
        for (const QString& s : p.problems) problems.append(s);
        arr.append(QJsonObject{{QStringLiteral("id"), p.manifest.id},
                               {QStringLiteral("name"), p.manifest.name},
                               {QStringLiteral("version"), p.manifest.version.toString()},
                               {QStringLiteral("origin"), p.origin},
                               {QStringLiteral("enabled"), p.enabled},
                               {QStringLiteral("dev_linked"), p.devLinked},
                               {QStringLiteral("status"), started ? statusName(snap.status) : QStringLiteral("not_started")},
                               {QStringLiteral("error"), snap.error.section(QLatin1Char('\n'), 0, 0)},
                               {QStringLiteral("problems"), problems},
                               {QStringLiteral("provides"), provides},
                               {QStringLiteral("permissions"), perms}});
      }
      for (const QString& id : running) {  // run copies that are not the installed one
        if (seen.contains(id)) continue;
        const plugins::ConsoleSnapshot snap = host->consoleSnapshot(id);
        arr.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("status"), statusName(snap.status)},
                               {QStringLiteral("error"), snap.error.section(QLatin1Char('\n'), 0, 0)}, {QStringLiteral("run_copy"), true}});
      }
      reply(ToolResult::json(arr));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_console");
    t.description = QStringLiteral("The developer console of a running plugin: status, error with traceback, log (500 lines), events (200), watched memory rows and the budgets.");
    t.targetArg = QStringLiteral("id");
    t.inputSchema = schema(idProps(), {QStringLiteral("id")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      plugins::PluginHost* host = pc->plugins()->host();
      if (!host->pluginIds().contains(id)) return reply(ToolResult::error(QStringLiteral("plugin_not_running")));
      const plugins::ConsoleSnapshot s = host->consoleSnapshot(id);
      QJsonArray events, watched;
      for (const plugins::EventLine& e : s.events)
        events.append(QJsonObject{{QStringLiteral("time"), isoTime(e.timeMs)}, {QStringLiteral("plugin"), e.plugin}, {QStringLiteral("name"), e.name}, {QStringLiteral("text"), e.text}});
      for (const plugins::WatchedRow& w : s.watched)
        watched.append(QJsonObject{{QStringLiteral("label"), w.label}, {QStringLiteral("address"), w.address}, {QStringLiteral("value"), w.value}});
      const plugins::HostLimits lim = host->limits();
      reply(ToolResult::json(QJsonObject{
          {QStringLiteral("status"), statusName(s.status)},
          {QStringLiteral("error"), s.error},
          {QStringLiteral("log"), logJson(s.log)},
          {QStringLiteral("events"), events},
          {QStringLiteral("watched"), watched},
          {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("memory_bytes"), static_cast<double>(lim.memoryBytes)},
                                                  {QStringLiteral("instructions_per_call"), static_cast<double>(lim.instructions)},
                                                  {QStringLiteral("wall_ms_per_call"), lim.wallMs}}}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_ui");
    t.description = QStringLiteral("The UI trees (fb.ui blocks) of a plugin: one slot (panel, hud, screen, detail, run, tile:<id>) or, without slot, all slots, with titles, badge and dot.");
    t.targetArg = QStringLiteral("id");
    t.inputSchema = schema(idProps({{QStringLiteral("slot"), typeStr(QStringLiteral("Slot name; omit for all slots."))}}), {QStringLiteral("id")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      plugins::PluginHost* host = pc->plugins()->host();
      if (!host->pluginIds().contains(id)) return reply(ToolResult::error(QStringLiteral("plugin_not_running")));
      const QString slot = a.value(QStringLiteral("slot")).toString();
      if (!slot.isEmpty()) {
        reply(ToolResult::json(QJsonObject{{QStringLiteral("slot"), slot}, {QStringLiteral("tree"), QJsonObject::fromVariantMap(host->uiTree(id, slot))}}));
        return;
      }
      const plugins::UiPluginState st = host->uiState(id);
      QJsonObject trees, titles;
      for (auto it = st.trees.cbegin(); it != st.trees.cend(); ++it) trees.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
      for (auto it = st.titles.cbegin(); it != st.titles.cend(); ++it) titles.insert(it.key(), it.value());
      reply(ToolResult::json(QJsonObject{{QStringLiteral("slots"), trees},
                                         {QStringLiteral("titles"), titles},
                                         {QStringLiteral("badge"), st.badge},
                                         {QStringLiteral("dot"), st.dot},
                                         {QStringLiteral("paused"), st.paused}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_action");
    t.description = QStringLiteral("Sends a UI action (button, toggle, list row ...) to the plugin's fb.ui.on_action handler, like a click in the app would.");
    t.targetArg = QStringLiteral("id");
    t.inputSchema = schema(idProps({{QStringLiteral("slot"), typeStr(QStringLiteral("Slot the action comes from, e.g. panel."))},
                                    {QStringLiteral("action"), typeStr(QStringLiteral("Action id of the block."))},
                                    {QStringLiteral("value"), typeAny(QStringLiteral("Optional value (string, number, boolean, list, object)."))}}),
                           {QStringLiteral("id"), QStringLiteral("slot"), QStringLiteral("action")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      if (!pc->plugins()->host()->pluginIds().contains(id)) return reply(ToolResult::error(QStringLiteral("plugin_not_running")));
      const QString action = a.value(QStringLiteral("action")).toString();
      if (action.isEmpty()) return reply(ToolResult::error(QStringLiteral("action is required")));
      pc->plugins()->sendAction(id, a.value(QStringLiteral("slot")).toString(), action, a.value(QStringLiteral("value")).toVariant());
      reply(ToolResult::json(QJsonObject{{QStringLiteral("sent"), true}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_reload");
    t.description = QStringLiteral("Re-reads the manifest and entry file of a running plugin and restarts its Lua state; returns the status afterwards.");
    t.targetArg = QStringLiteral("id");
    t.inputSchema = schema(idProps(), {QStringLiteral("id")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      plugins::PluginHost* host = pc->plugins()->host();
      if (!host->pluginIds().contains(id)) return reply(ToolResult::error(QStringLiteral("plugin_not_running")));
      auto conn = std::make_shared<QMetaObject::Connection>();
      auto seenLoading = std::make_shared<bool>(false);
      *conn = QObject::connect(host, &plugins::PluginHost::statusChanged, host, [host, id, conn, seenLoading, reply](const QString& changed) {
        if (changed != id) return;
        const plugins::ConsoleSnapshot s = host->consoleSnapshot(id);
        if (s.status == PluginStatus::Loading) {
          *seenLoading = true;
          return;
        }
        if (!*seenLoading) return;  // the status before the restart began
        QObject::disconnect(*conn);
        reply(ToolResult::json(QJsonObject{{QStringLiteral("status"), statusName(s.status)}, {QStringLiteral("error"), s.error.section(QLatin1Char('\n'), 0, 0)}}));
      });
      pc->plugins()->reloadPlugin(id);
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_call");
    t.description = QStringLiteral(
        "Calls a function of an interface the plugin registered with fb.provide (its manifest \"provides\") on the plugin thread under the normal budget. "
        "Image results {width, height, rgba} come back as PNG images, referenced by index in the JSON.");
    t.targetArg = QStringLiteral("id");
    t.inputSchema = schema(idProps({{QStringLiteral("interface"), typeStr(QStringLiteral("Interface name, e.g. pokemon.core."))},
                                    {QStringLiteral("function"), typeStr(QStringLiteral("Function name in the interface table."))},
                                    {QStringLiteral("args"), typeArray(typeAny(QString()), QStringLiteral("Arguments (JSON values; objects and arrays become Lua tables)."))}}),
                           {QStringLiteral("id"), QStringLiteral("interface"), QStringLiteral("function")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      if (!pc->plugins()->host()->pluginIds().contains(id)) return reply(ToolResult::error(QStringLiteral("plugin_not_running")));
      std::vector<Value> args;
      for (const QJsonValue& v : a.value(QStringLiteral("args")).toArray()) args.push_back(plugins::valueFromJson(v));
      pc->plugins()->host()->callProvided(id, a.value(QStringLiteral("interface")).toString(), a.value(QStringLiteral("function")).toString(), args,
                                          [reply](ProbeResult r) { reply(probeToResult(r)); });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_eval");
    t.description = QStringLiteral(
        "Runs a Lua chunk in the state of a plugin with origin local (developer mode) with the plugin's own sandbox, API and budgets; other plugins are refused "
        "(eval_not_allowed). The code is tried as an expression first (\"fb.game.title\"), then as statements. Returns the values, print/fb.log output of the call and images.");
    t.targetArg = QStringLiteral("id");
    t.inputSchema = schema(idProps({{QStringLiteral("code"), typeStr(QStringLiteral("Lua code."))}}), {QStringLiteral("id"), QStringLiteral("code")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      const auto info = infoOf(*pc, id);
      if (!info || info->origin != QLatin1String("local") || info->hubCopy) return reply(ToolResult::error(QStringLiteral("eval_not_allowed")));
      if (!pc->plugins()->host()->pluginIds().contains(id)) return reply(ToolResult::error(QStringLiteral("plugin_not_running")));
      pc->plugins()->host()->evalChunk(id, a.value(QStringLiteral("code")).toString(), [reply](ProbeResult r) { reply(probeToResult(r)); });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_cache_list");
    t.description = QStringLiteral("Files in the plugin's cache folder (fb.cache): path and size.");
    t.targetArg = QStringLiteral("id");
    t.inputSchema = schema(idProps(), {QStringLiteral("id")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const auto info = infoOf(*pc, a.value(QStringLiteral("id")).toString());
      if (!info) return reply(ToolResult::error(QStringLiteral("plugin_not_found")));
      const QString base = QDir::cleanPath(info->cacheDir);
      QJsonArray files;
      qint64 total = 0;
      bool truncated = false;
      QDirIterator it(base, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        it.next();
        if (it.fileName().contains(QLatin1Char('~')) || it.fileInfo().isSymLink()) continue;
        if (files.size() >= kMaxCacheListEntries) {
          truncated = true;
          break;
        }
        total += it.fileInfo().size();
        files.append(QJsonObject{{QStringLiteral("path"), QDir(base).relativeFilePath(it.filePath())}, {QStringLiteral("size"), static_cast<double>(it.fileInfo().size())}});
      }
      reply(ToolResult::json(QJsonObject{{QStringLiteral("files"), files}, {QStringLiteral("total_bytes"), static_cast<double>(total)}, {QStringLiteral("truncated"), truncated}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_cache_read");
    t.description = QStringLiteral("Reads a file from the plugin's cache folder (same path rules as fb.cache, up to 8 MiB). PNG files come back as an image, other files as base64 in JSON.");
    t.targetArg = QStringLiteral("id");
    t.inputSchema = schema(idProps({{QStringLiteral("path"), typeStr(QStringLiteral("Cache file name, e.g. icons/25.png."))}}), {QStringLiteral("id"), QStringLiteral("path")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const auto info = infoOf(*pc, a.value(QStringLiteral("id")).toString());
      if (!info) return reply(ToolResult::error(QStringLiteral("plugin_not_found")));
      const QString name = a.value(QStringLiteral("path")).toString();
      if (!plugins::validCacheName(name.toStdString())) return reply(ToolResult::error(QStringLiteral("invalid_path")));
      const QString base = QDir::cleanPath(info->cacheDir);
      const QString path = QDir::cleanPath(base + QLatin1Char('/') + name);
      const QFileInfo fi(path);
      if (!path.startsWith(base + QLatin1Char('/')) || !fi.isFile() || fi.isSymLink()) return reply(ToolResult::error(QStringLiteral("not_found")));
      if (fi.size() > kMaxCacheFileRead) return reply(ToolResult::error(QStringLiteral("file_too_large")));
      QFile f(path);
      if (!f.open(QIODevice::ReadOnly)) return reply(ToolResult::error(QStringLiteral("not_found")));
      const QByteArray data = f.readAll();
      if (data.startsWith("\x89PNG\r\n\x1a\n")) {
        QImage img;
        img.loadFromData(data, "PNG");
        if (!img.isNull() && img.width() <= kMaxImageDim && img.height() <= kMaxImageDim) {
          ToolResult r = ToolResult::json(QJsonObject{{QStringLiteral("path"), name}, {QStringLiteral("width"), img.width()}, {QStringLiteral("height"), img.height()}, {QStringLiteral("bytes"), static_cast<double>(data.size())}});
          r.addImage(data);
          return reply(std::move(r));
        }
      }
      reply(ToolResult::json(QJsonObject{{QStringLiteral("path"), name}, {QStringLiteral("bytes"), static_cast<double>(data.size())}, {QStringLiteral("base64"), QString::fromLatin1(data.toBase64())}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_image");
    t.description = QStringLiteral("Loads an image the way the plugin UI does (image block src): \"cache:<path>\" from the plugin cache or \"asset:<path>\" from its declared assets; returned as PNG (scaled to at most 2048 px).");
    t.targetArg = QStringLiteral("id");
    t.inputSchema = schema(idProps({{QStringLiteral("src"), typeStr(QStringLiteral("cache:<path> or asset:<path>."))}}), {QStringLiteral("id"), QStringLiteral("src")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      const QString src = a.value(QStringLiteral("src")).toString();
      const int colon = src.indexOf(QLatin1Char(':'));
      const QString kind = colon > 0 ? src.left(colon) : QString();
      if (kind != QLatin1String("cache") && kind != QLatin1String("asset")) return reply(ToolResult::error(QStringLiteral("src must start with cache: or asset:")));
      ui::PluginImageSource source;
      if (!pc->plugins()->imageSources()->get(id, source)) return reply(ToolResult::error(QStringLiteral("plugin_not_found")));
      QString err;
      const QString file = ui::PluginImageProvider::resolve(source, id + QLatin1Char('/') + kind + QLatin1Char('/') + src.mid(colon + 1), &err);
      if (file.isEmpty()) return reply(ToolResult::error(err.isEmpty() ? QStringLiteral("not_found") : err));
      const QImage img = ui::PluginImageProvider::loadPng(file, &err);
      if (img.isNull()) return reply(ToolResult::error(err.isEmpty() ? QStringLiteral("not_an_image") : err));
      ToolResult r = ToolResult::json(QJsonObject{{QStringLiteral("src"), src}, {QStringLiteral("width"), img.width()}, {QStringLiteral("height"), img.height()}});
      r.addImage(pngOf(img));
      reply(std::move(r));
    };
    registry.add(std::move(t));
  }
}

}  // namespace uzume::mcp
