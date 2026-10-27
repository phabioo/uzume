// Variant tools of the debug MCP server (Uzume 0.21, ADR 0031/0032): the randomizer engine from the server's tools catalog, game
// variants (list, build, status, files, start, delete) and run variants, plus two plugin helpers (install a .fbplugin file, turn a
// plugin on or off). The handlers drive the same VariantsController the UI uses. Nothing here reads or returns spoiler text: the
// spoiler log has no tool, and variant_files lists only the built files (name, size, SHA-256).
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <memory>

#include "mcp_tools.h"
#include "pluginscontroller.h"
#include "playercontroller.h"
#include "tools_util.h"
#include "variantscontroller.h"

namespace uzume::mcp {

using namespace tools;

namespace {

constexpr int kBuildWaitMs = 10 * 60 * 1000;
constexpr int kMaxListedFiles = 500;

QJsonValue toJson(const QVariant& v) { return QJsonValue::fromVariant(v); }

QJsonArray toArray(const QVariantList& l) {
  QJsonArray a;
  for (const QVariant& v : l) a.append(toJson(v));
  return a;
}

// A reply that arrives once, from whichever signal comes first, or from the fallback timer.
struct Waiter {
  QList<QMetaObject::Connection> conns;
  bool done = false;
  void finish(const std::function<void()>& fn) {
    if (done) return;
    done = true;
    for (const auto& c : std::as_const(conns)) QObject::disconnect(c);
    conns.clear();
    fn();
  }
};

QString sha256Of(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash h(QCryptographicHash::Sha256);
  if (!h.addData(&f)) return {};
  return QString::fromLatin1(h.result().toHex());
}

bool validVariantId(const QString& id) { return VariantCache::isValidId(id); }

}  // namespace

void installVariantTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);

  {
    ToolDef t;
    t.name = QStringLiteral("tools_list");
    t.description = QStringLiteral(
        "The randomizer engine as the app sees it: state (no_server | not_offered | not_on_pc | downloading | ready | failed), the offered "
        "version, size and license, and the tools catalog of the server (versions, platforms, sizes) plus the versions on this PC.");
    t.handler = [pc](const QJsonObject&, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      QJsonObject o;
      o.insert(QStringLiteral("engine"), toJson(pc->variants()->engine()));
      QJsonArray catalog;
      for (const ToolInfo& ti : pc->toolsClient()->tools()) {
        QJsonObject to{{QStringLiteral("id"), ti.id}, {QStringLiteral("name"), ti.name}, {QStringLiteral("offered"), ti.offered},
                       {QStringLiteral("version"), ti.version}, {QStringLiteral("license"), ti.license}};
        QJsonArray vers;
        for (const ToolVersionInfo& v : ti.versions) {
          QJsonArray plats;
          for (const ToolPlatformInfo& p : v.platforms)
            plats.append(QJsonObject{{QStringLiteral("platform"), p.platform}, {QStringLiteral("size"), static_cast<double>(p.size)}, {QStringLiteral("cached"), p.cached}});
          vers.append(QJsonObject{{QStringLiteral("version"), v.version}, {QStringLiteral("platforms"), plats}});
        }
        to.insert(QStringLiteral("versions"), vers);
        QJsonArray onPc;
        for (const QString& v : pc->toolCache()->versions(ti.id, ToolCache::normalizePlatform(pc->toolsClient()->platform()))) onPc.append(v);
        to.insert(QStringLiteral("onThisPc"), onPc);
        catalog.append(to);
      }
      o.insert(QStringLiteral("tools"), catalog);
      reply(ToolResult::json(o));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("tool_prepare");
    t.description = QStringLiteral("Downloads the engine the server offers into the tool cache (SHA-256 checked) and waits until it is ready or failed.");
    t.timeoutMs = kBuildWaitMs;
    t.handler = [pc](const QJsonObject&, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      ui::VariantsController* vc = pc->variants();
      const auto state = [vc]() { return vc->engine().value(QStringLiteral("state")).toString(); };
      if (state() == QLatin1String("ready")) return reply(ToolResult::json(toJson(vc->engine())));
      auto w = std::make_shared<Waiter>();
      w->conns << QObject::connect(vc, &ui::VariantsController::engineChanged, vc, [w, vc, state, reply]() {
        const QString s = state();
        if (s == QLatin1String("ready") || s == QLatin1String("failed") || s == QLatin1String("not_offered") || s == QLatin1String("no_server")) {
          w->finish([&]() {
            ToolResult r = ToolResult::json(toJson(vc->engine()));
            r.isError = s != QLatin1String("ready");
            reply(r);
          });
        }
      });
      vc->prepareEngine();
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("variants_list");
    t.description = QStringLiteral("The variants of a game (newest first): id, name, slot, state (building | ready | needs_rebuild | failed), progress, error.");
    t.inputSchema = schema({{QStringLiteral("game_id"), typeStr(QStringLiteral("Library game id."))}}, {QStringLiteral("game_id")});
    t.targetArg = QStringLiteral("game_id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString gameId = a.value(QStringLiteral("game_id")).toString();
      if (gameId.isEmpty()) return reply(ToolResult::error(QStringLiteral("game_id required")));
      ui::VariantsController* vc = pc->variants();
      auto w = std::make_shared<Waiter>();
      const auto answer = [w, vc, gameId, reply]() {
        w->finish([&]() { reply(ToolResult::json(QJsonObject{{QStringLiteral("gameId"), gameId}, {QStringLiteral("variants"), toArray(vc->variantsOf(gameId))}})); });
      };
      w->conns << QObject::connect(vc, &ui::VariantsController::variantsChanged, vc, answer);
      QTimer::singleShot(3000, vc, answer);  // an unreachable server: the local recipes
      vc->setGame(gameId);
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("variant_presets");
    t.description = QStringLiteral(
        "Whether a plugin registered the game for the randomizer (fb.variants.register) and its presets [{id, label, hint, custom}]; Uzume's "
        "own custom entry comes last.");
    t.inputSchema = schema({{QStringLiteral("game_id"), typeStr(QStringLiteral("Library game id."))}}, {QStringLiteral("game_id")});
    t.targetArg = QStringLiteral("game_id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString gameId = a.value(QStringLiteral("game_id")).toString();
      ui::VariantsController* vc = pc->variants();
      reply(ToolResult::json(QJsonObject{{QStringLiteral("gameId"), gameId}, {QStringLiteral("supported"), vc->supported(gameId)},
                                         {QStringLiteral("presets"), toArray(vc->presets(gameId))}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("variant_build");
    t.description = QStringLiteral(
        "Creates a variant (recipe on the server) and builds it on this PC with the engine in the tool cache. With wait (default true) the "
        "answer comes when the build is finished; otherwise right after the server accepted the recipe. Returns the variant id.");
    t.inputSchema = schema({{QStringLiteral("game_id"), typeStr(QStringLiteral("Library game id."))},
                            {QStringLiteral("name"), typeStr(QStringLiteral("Variant name, 1 to 48 characters."))},
                            {QStringLiteral("preset_id"), typeStr(QStringLiteral("Preset id of a plugin, or \"custom\" (default)."))},
                            {QStringLiteral("settings"), typeStr(QStringLiteral("Settings string of the engine."))},
                            {QStringLiteral("seed"), typeStr(QStringLiteral("Digits; random when left out."))},
                            {QStringLiteral("spoiler_log"), typeBool(QStringLiteral("Also write the spoiler log (never returned by a tool)."))},
                            {QStringLiteral("wait"), typeBool(QStringLiteral("Wait for the build to finish, default true."))}},
                           {QStringLiteral("game_id"), QStringLiteral("name"), QStringLiteral("settings")});
    t.targetArg = QStringLiteral("game_id");
    t.timeoutMs = kBuildWaitMs;
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      ui::VariantsController* vc = pc->variants();
      const bool wait = a.value(QStringLiteral("wait")).toBool(true);
      const QString seed = a.value(QStringLiteral("seed")).isDouble() ? QString::number(static_cast<qint64>(a.value(QStringLiteral("seed")).toDouble()))
                                                                       : a.value(QStringLiteral("seed")).toString();
      auto w = std::make_shared<Waiter>();
      auto id = std::make_shared<QString>();
      // Connect first: the answers can come in the same event loop turn as build() returns.
      auto reqId = std::make_shared<quint64>(0);
      w->conns << QObject::connect(vc, &ui::VariantsController::createFailed, vc, [w, reqId, reply](quint64 rid, const QString& err) {
        if (rid != *reqId) return;
        w->finish([&]() { reply(ToolResult::error(err)); });
      });
      w->conns << QObject::connect(vc, &ui::VariantsController::variantCreated, vc, [w, vc, reqId, id, wait, reply](quint64 rid, const QString& vid) {
        if (rid != *reqId) return;
        *id = vid;
        if (!wait) w->finish([&]() { reply(ToolResult::json(QJsonObject{{QStringLiteral("variantId"), vid}, {QStringLiteral("state"), QStringLiteral("building")}})); });
      });
      w->conns << QObject::connect(vc, &ui::VariantsController::buildFinished, vc, [w, vc, id, reply](const QString& vid, bool ok, const QString& err) {
        if (id->isEmpty() || vid != *id) return;
        w->finish([&]() {
          QJsonObject o = toJson(vc->status(vid)).toObject();
          o.insert(QStringLiteral("variantId"), vid);
          ToolResult r = ToolResult::json(o);
          r.isError = !ok;
          if (!ok) r.addText(err);
          reply(r);
        });
      });
      const QVariantMap res = vc->build(a.value(QStringLiteral("game_id")).toString(), a.value(QStringLiteral("name")).toString(),
                                        a.value(QStringLiteral("preset_id")).toString(QStringLiteral("custom")), a.value(QStringLiteral("settings")).toString(),
                                        seed, !seed.isEmpty(), a.value(QStringLiteral("spoiler_log")).toBool(false));
      if (!res.value(QStringLiteral("ok")).toBool()) {
        w->finish([&]() { reply(ToolResult::error(res.value(QStringLiteral("error")).toString())); });
        return;
      }
      *reqId = res.value(QStringLiteral("requestId")).toULongLong();
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("variant_status");
    t.description = QStringLiteral("State of one variant: building (step, percent, ETA), ready, needs_rebuild or failed (error code and text), plus the cache sizes.");
    t.inputSchema = schema({{QStringLiteral("id"), typeStr(QStringLiteral("Variant id."))}}, {QStringLiteral("id")});
    t.targetArg = QStringLiteral("id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      if (!validVariantId(id)) return reply(ToolResult::error(QStringLiteral("id required")));
      const QVariantMap st = pc->variants()->status(id);
      if (st.isEmpty()) return reply(ToolResult::error(QStringLiteral("not_found")));
      QJsonObject o = toJson(st).toObject();
      o.insert(QStringLiteral("cache"), toJson(pc->variants()->cache()));
      reply(ToolResult::json(o));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("variant_cancel");
    t.description = QStringLiteral("Stops a running or queued build; nothing is kept.");
    t.inputSchema = schema({{QStringLiteral("id"), typeStr(QStringLiteral("Variant id."))}}, {QStringLiteral("id")});
    t.targetArg = QStringLiteral("id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      if (!validVariantId(id)) return reply(ToolResult::error(QStringLiteral("id required")));
      pc->variants()->cancel(id);
      reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("variantId"), id}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("variant_delete");
    t.description = QStringLiteral(
        "Deletes a variant (recipe on the server and built files here). keep_saves (default true) keeps the variant's save slot; "
        "keep_saves=false deletes the saves of the variant too and needs confirm:true (ADR 0032 D4).");
    t.inputSchema = schema({{QStringLiteral("id"), typeStr(QStringLiteral("Variant id."))},
                            {QStringLiteral("keep_saves"), typeBool(QStringLiteral("Keep the saves of the variant, default true."))},
                            {QStringLiteral("confirm"), typeBool(QStringLiteral("Required when keep_saves is false."))}},
                           {QStringLiteral("id")});
    t.targetArg = QStringLiteral("id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      if (!validVariantId(id)) return reply(ToolResult::error(QStringLiteral("id required")));
      const bool keep = a.value(QStringLiteral("keep_saves")).toBool(true);
      if (!keep && !a.value(QStringLiteral("confirm")).toBool(false)) {
        ToolResult r = ToolResult::error(QStringLiteral("confirm_required"));
        r.addText(QStringLiteral("Deleting the saves of a variant cannot be undone. Call again with confirm:true."));
        return reply(r);
      }
      ui::VariantsController* vc = pc->variants();
      auto w = std::make_shared<Waiter>();
      const auto okAnswer = [w, reply, id]() { w->finish([&]() { reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("variantId"), id}})); }); };
      w->conns << QObject::connect(vc, &ui::VariantsController::actionFailed, vc, [w, reply, id](const QString&, const QString& vid, const QString& err) {
        if (vid != id) return;
        w->finish([&]() { reply(ToolResult::error(err)); });
      });
      w->conns << QObject::connect(vc, &ui::VariantsController::variantsChanged, vc, okAnswer);
      QTimer::singleShot(5000, vc, okAnswer);
      vc->remove(id, keep);
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("variant_files");
    t.description = QStringLiteral(
        "The built files of a variant in the variant cache: relative path, size and SHA-256 of each file (at most 500). Never file content "
        "and not the spoiler log.");
    t.inputSchema = schema({{QStringLiteral("id"), typeStr(QStringLiteral("Variant id."))}}, {QStringLiteral("id")});
    t.targetArg = QStringLiteral("id");
    t.timeoutMs = 120000;
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      if (!validVariantId(id)) return reply(ToolResult::error(QStringLiteral("id required")));
      VariantCache* cache = pc->variantCache();
      if (!cache->isBuilt(id)) return reply(ToolResult::error(QStringLiteral("not_built")));
      const QString out = cache->outDir(id);
      const QDir outDir(out);
      QStringList rel;
      QDirIterator it(out, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
      while (it.hasNext()) rel.append(outDir.relativeFilePath(it.next()));
      rel.sort();
      QJsonArray files;
      qint64 total = 0;
      for (const QString& r : std::as_const(rel)) {
        if (files.size() >= kMaxListedFiles) break;
        const qint64 size = QFileInfo(outDir.filePath(r)).size();
        total += size;
        files.append(QJsonObject{{QStringLiteral("path"), r}, {QStringLiteral("size"), static_cast<double>(size)}, {QStringLiteral("sha256"), sha256Of(outDir.filePath(r))}});
      }
      QJsonObject o{{QStringLiteral("variantId"), id}, {QStringLiteral("files"), files}, {QStringLiteral("totalBytes"), static_cast<double>(total)}};
      if (rel.size() > files.size()) o.insert(QStringLiteral("truncated"), true);
      const auto recipe = cache->recipe(id);
      if (recipe) o.insert(QStringLiteral("kind"), recipe->isMod() ? QStringLiteral("mod") : QStringLiteral("rom"));
      reply(ToolResult::json(o));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("variant_start");
    t.description = QStringLiteral(
        "Plays a variant in its own save slot (the same as Play on the variant); builds it first when its files are gone. Needs no game running.");
    t.inputSchema = schema({{QStringLiteral("id"), typeStr(QStringLiteral("Variant id."))}}, {QStringLiteral("id")});
    t.targetArg = QStringLiteral("id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      if (!validVariantId(id)) return reply(ToolResult::error(QStringLiteral("id required")));
      if (pc->variants()->status(id).isEmpty()) return reply(ToolResult::error(QStringLiteral("not_found")));
      pc->variants()->play(id);
      reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("variantId"), id}, {QStringLiteral("status"), toJson(pc->variants()->status(id))}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("run_variant_build");
    t.description = QStringLiteral(
        "Builds the run variant of a run with a randomizer (recipe from the server, build into the run slot) and reports building and ready "
        "to the server. Waits until ready or failed.");
    t.inputSchema = schema({{QStringLiteral("run_id"), typeStr(QStringLiteral("Run id."))}, {QStringLiteral("game_id"), typeStr(QStringLiteral("Library game id of the run."))}},
                           {QStringLiteral("run_id"), QStringLiteral("game_id")});
    t.targetArg = QStringLiteral("run_id");
    t.timeoutMs = kBuildWaitMs;
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString runId = a.value(QStringLiteral("run_id")).toString();
      const QString gameId = a.value(QStringLiteral("game_id")).toString();
      if (runId.isEmpty() || gameId.isEmpty()) return reply(ToolResult::error(QStringLiteral("run_id and game_id required")));
      ui::VariantsController* vc = pc->variants();
      auto w = std::make_shared<Waiter>();
      w->conns << QObject::connect(vc, &ui::VariantsController::runVariantReady, vc, [w, vc, runId, reply](const QString& rid) {
        if (rid != runId) return;
        w->finish([&]() { reply(ToolResult::json(toJson(vc->runVariantState(runId)))); });
      });
      w->conns << QObject::connect(vc, &ui::VariantsController::runVariantFailed, vc, [w, runId, reply](const QString& rid, const QString& err) {
        if (rid != runId) return;
        w->finish([&]() { reply(ToolResult::error(err)); });
      });
      if (vc->runVariantBuilt(runId)) return w->finish([&]() { reply(ToolResult::json(toJson(vc->runVariantState(runId)))); });
      vc->ensureRunVariant(runId, gameId);
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_install_file");
    t.description = QStringLiteral(
        "Installs a plugin from a .fbplugin file or a plugin folder on this PC (like Settings > Plugins > Install). A valid Uzume signature "
        "makes it official; anything else is a local plugin that needs developer mode. The plugin stays off until plugin_set_enabled.");
    t.inputSchema = schema({{QStringLiteral("path"), typeStr(QStringLiteral("Absolute path of a .fbplugin file or a plugin folder."))}}, {QStringLiteral("path")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString path = a.value(QStringLiteral("path")).toString();
      if (path.isEmpty() || !QFileInfo::exists(path)) return reply(ToolResult::error(QStringLiteral("path not found")));
      const plugins::InstallResult r = pc->plugins()->installFromPathResult(path);
      if (!r.ok) return reply(ToolResult::error(r.error));
      reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), r.id}, {QStringLiteral("version"), r.version},
                                         {QStringLiteral("origin"), r.origin}, {QStringLiteral("addedPermissions"), QJsonArray::fromStringList(r.addedPermissions)}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("plugin_set_enabled");
    t.description = QStringLiteral("Turns an installed plugin on or off (its required plugins turn on with it). Returns whether it is enabled and why it cannot start, if so.");
    t.inputSchema = schema({{QStringLiteral("id"), typeStr(QStringLiteral("Plugin id."))}, {QStringLiteral("enabled"), typeBool(QStringLiteral("On or off."))}},
                           {QStringLiteral("id"), QStringLiteral("enabled")});
    t.targetArg = QStringLiteral("id");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString id = a.value(QStringLiteral("id")).toString();
      if (!pc->plugins()->manager()->info(id)) return reply(ToolResult::error(QStringLiteral("plugin_not_found")));
      pc->plugins()->setPluginEnabled(id, a.value(QStringLiteral("enabled")).toBool());
      const auto info = pc->plugins()->manager()->info(id);
      reply(ToolResult::json(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("enabled"), info && info->enabled},
                                         {QStringLiteral("problems"), QJsonArray::fromStringList(info ? info->problems : QStringList())}}));
    };
    registry.add(std::move(t));
  }
}

}  // namespace uzume::mcp
