// Emulation tools of the debug MCP server (0.20): raw save states kept in memory under a name (never sent to a server, never
// written to disk) and memory writes. Both go through the emulation command queue (EmulationRunner::submit): they run on the
// emulation thread between two frames, also while the game is paused. Unlike the other game tools these change the game.
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QRegularExpression>
#include <memory>

#include "emulation_runner.h"
#include "gamesession.h"
#include "mcp_tools.h"
#include "playercontroller.h"
#include "pluginemulation.h"
#include "pluginscontroller.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;

namespace {

constexpr int kMaxSavedStates = 16;
constexpr qint64 kMaxSavedBytes = 256ll * 1024 * 1024;

struct SavedState {
  QByteArray data;
  quint64 frame = 0;
};

// Named states of this MCP session; written from the emulation thread (onDone), read from the GUI thread.
struct StateStore {
  QMutex mutex;
  QHash<QString, SavedState> states;
  qint64 totalBytes() const {
    qint64 n = 0;
    for (const SavedState& s : states) n += s.data.size();
    return n;
  }
};

QString validName(const QJsonObject& a) {
  const QString n = a.value(QStringLiteral("name")).toString(QStringLiteral("default"));
  static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
  return re.match(n).hasMatch() ? n : QString();
}

ToolResult failure(const emu::EmulationRunner::CommandResult& r) { return ToolResult::error(r.error.isEmpty() ? QStringLiteral("failed") : r.error); }

}  // namespace

void installEmulationTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);
  const auto session = [pc]() -> ui::GameSession* { return pc ? pc->gameSession() : nullptr; };
  const auto noGame = [](ui::GameSession* gs) { return gs == nullptr || !gs->isActive(); };
  const auto store = std::make_shared<StateStore>();

  {
    ToolDef t;
    t.name = QStringLiteral("game_state_save");
    t.description = QStringLiteral(
        "Saves the core's state of the running game (also while paused) into app memory under a name (default \"default\", a "
        "repeated name replaces the state). Raw: no server, no file. Returns the size in bytes and the frame number. Needs a core "
        "with retro_serialize (error \"unsupported\" otherwise).");
    t.inputSchema = schema({{QStringLiteral("name"), typeStr(QStringLiteral("Name, [A-Za-z0-9_.-], at most 64 characters (default \"default\")."))}});
    t.targetArg = QStringLiteral("name");
    t.handler = [session, noGame, store](const QJsonObject& a, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      const QString name = validName(a);
      if (name.isEmpty()) return reply(ToolResult::error(QStringLiteral("invalid name")));
      const quint64 id = gs->submitEmuCommand(emu::EmulationRunner::Command::saveState(),
                                              [store, name, reply](const emu::EmulationRunner::CommandResult& r) {
        if (!r.ok) return reply(failure(r));
        {
          QMutexLocker l(&store->mutex);
          const qint64 others = store->totalBytes() - (store->states.contains(name) ? store->states.value(name).data.size() : 0);
          if ((!store->states.contains(name) && store->states.size() >= kMaxSavedStates) || others + r.state.size() > kMaxSavedBytes)
            return reply(ToolResult::error(QStringLiteral("state_store_full")));
          store->states.insert(name, SavedState{r.state, r.frame});
        }
        reply(ToolResult::json(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("size"), static_cast<double>(r.state.size())},
                                           {QStringLiteral("frame"), static_cast<double>(r.frame)}}));
      });
      if (id == 0) reply(ToolResult::error(QStringLiteral("no_game")));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("game_state_load");
    t.description = QStringLiteral(
        "Loads a state saved with game_state_save back into the running game (also while paused). The active cheat list is "
        "re-applied afterwards. The next frames continue from the saved moment.");
    t.inputSchema = schema({{QStringLiteral("name"), typeStr(QStringLiteral("Name of the state (default \"default\")."))}});
    t.targetArg = QStringLiteral("name");
    t.handler = [session, noGame, store](const QJsonObject& a, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      const QString name = validName(a);
      if (name.isEmpty()) return reply(ToolResult::error(QStringLiteral("invalid name")));
      SavedState saved;
      {
        QMutexLocker l(&store->mutex);
        if (!store->states.contains(name)) return reply(ToolResult::error(QStringLiteral("unknown_state")));
        saved = store->states.value(name);
      }
      const qint64 size = saved.data.size();
      const quint64 savedFrame = saved.frame;
      const quint64 id = gs->submitEmuCommand(emu::EmulationRunner::Command::loadState(std::move(saved.data)),
                                              [name, size, savedFrame, reply](const emu::EmulationRunner::CommandResult& r) {
        if (!r.ok) return reply(failure(r));
        reply(ToolResult::json(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("size"), static_cast<double>(size)},
                                           {QStringLiteral("saved_frame"), static_cast<double>(savedFrame)}}));
      });
      if (id == 0) reply(ToolResult::error(QStringLiteral("no_game")));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("game_memory_write");
    t.description = QStringLiteral(
        "Writes bytes into live game memory at an address of the core's memory map (up to 1 MiB). The write is queued and applied "
        "on the emulation thread between two frames, also while paused. The whole range must be mapped, otherwise nothing is "
        "written (error \"unmapped\"). Changes the running game; save progress is not touched here.");
    t.inputSchema = schema({{QStringLiteral("addr"), typeAny(QStringLiteral("Address: number or \"0x08000000\"."))},
                            {QStringLiteral("hex"), typeStr(QStringLiteral("Bytes as hex, e.g. \"DE AD be ef\" or \"0xDEADBEEF\"."))}},
                           {QStringLiteral("addr"), QStringLiteral("hex")});
    t.handler = [session, noGame](const QJsonObject& a, ToolReply reply) {
      ui::GameSession* gs = session();
      if (noGame(gs)) return reply(ToolResult::error(QStringLiteral("no_game")));
      const auto addr = parseNumber(a.value(QStringLiteral("addr")));
      QString hex = a.value(QStringLiteral("hex")).toString();
      hex.remove(QLatin1Char(' ')).remove(QLatin1Char(':'));
      if (hex.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) hex = hex.mid(2);
      const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
      if (!addr || bytes.isEmpty() || hex.size() % 2 != 0 || bytes.size() * 2 != hex.size() || bytes.size() > emu::EmulationRunner::kMaxMemoryWriteBytes)
        return reply(ToolResult::error(QStringLiteral("addr and hex (1 to 1048576 bytes) are required")));
      const quint64 address = *addr;
      const qsizetype n = bytes.size();
      const quint64 id = gs->submitEmuCommand(emu::EmulationRunner::Command::writeMemory(address, bytes),
                                              [address, n, reply](const emu::EmulationRunner::CommandResult& r) {
        if (!r.ok) return reply(failure(r));
        reply(ToolResult::json(QJsonObject{{QStringLiteral("addr"), hexAddr(address)}, {QStringLiteral("written"), static_cast<double>(n)}}));
      });
      if (id == 0) reply(ToolResult::error(QStringLiteral("no_game")));
    };
    registry.add(std::move(t));
  }

  // ---- 0.20 plugin API: cheats, plugin store and consent (debug) ----
  const auto service = [pc]() -> ui::PluginEmulationService* { return pc && pc->plugins() ? pc->plugins()->emulation() : nullptr; };
  const auto cheatJson = [](const plugins::CheatItem& i) {
    return QJsonObject{{QStringLiteral("id"), i.id}, {QStringLiteral("name"), i.name}, {QStringLiteral("format"), i.format},
                       {QStringLiteral("code"), i.code}, {QStringLiteral("enabled"), i.enabled}};
  };
  const auto consentName = [](ui::PluginEmulationService::Consent c) {
    return c == ui::PluginEmulationService::Consent::Allowed ? QStringLiteral("allowed")
           : c == ui::PluginEmulationService::Consent::Denied ? QStringLiteral("denied") : QStringLiteral("ask");
  };

  {
    ToolDef t;
    t.name = QStringLiteral("cheats_list");
    t.description = QStringLiteral(
        "Lists the cheats that are active in the running game (the one list all plugins share), the code formats the core "
        "accepts, how many are on, why cheats are blocked (empty = allowed; \"blocked_by_run_rule\") and the per-game consent "
        "(\"ask\" | \"allowed\" | \"denied\") for cheats and memory writes.");
    t.inputSchema = schema();
    t.handler = [service, noGame, session, cheatJson, consentName](const QJsonObject&, ToolReply reply) {
      ui::PluginEmulationService* s = service();
      if (s == nullptr || noGame(session())) return reply(ToolResult::error(QStringLiteral("no_game")));
      QJsonArray active;
      for (const plugins::CheatItem& i : s->activeCheatList()) active.append(cheatJson(i));
      QJsonObject o{{QStringLiteral("active"), active},
                    {QStringLiteral("on"), s->cheatsOn()},
                    {QStringLiteral("formats"), QJsonArray::fromStringList(s->cheatFormatList())},
                    {QStringLiteral("core"), s->coreName()},
                    {QStringLiteral("blocked"), s->cheatsBlockedReason()},
                    {QStringLiteral("consent"), QJsonObject{{QStringLiteral("cheats"), consentName(s->consent(ui::PluginEmulationService::Kind::Cheats))},
                                                            {QStringLiteral("memory_write"), consentName(s->consent(ui::PluginEmulationService::Kind::MemoryWrite))}}}};
      reply(ToolResult::json(o));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("cheats_apply");
    t.description = QStringLiteral(
        "Replaces the whole active cheat list of the running game, as the plugin \"mcp\" (same path as fb.cheats.apply: run rule, "
        "consent, safety copy \"Before cheats\", run log, cheats-used marker). Items: {id?, name, format, code, enabled?}. Without "
        "bypass_consent the per-game question is asked in the app and the call returns \"consent_required\"; with bypass_consent "
        "true the question is skipped (debug only; the run rule and the safety copy still apply). Returns {applied, rejected}.");
    t.inputSchema = schema({{QStringLiteral("cheats"), typeArray(typeObject({{QStringLiteral("id"), typeStr(QStringLiteral("Stable id (default: the index)."))},
                                                                              {QStringLiteral("name"), typeStr(QStringLiteral("Display name."))},
                                                                              {QStringLiteral("format"), typeStr(QStringLiteral("One of the core's formats (cheats_list)."))},
                                                                              {QStringLiteral("code"), typeStr(QStringLiteral("The code text."))},
                                                                              {QStringLiteral("enabled"), typeBool(QStringLiteral("Default true."))}},
                                                                             {QStringLiteral("format"), QStringLiteral("code")}),
                                                      QStringLiteral("The new list (max 256).")) },
                            {QStringLiteral("bypass_consent"), typeBool(QStringLiteral("Skip the per-game consent question (debug)."))}},
                           {QStringLiteral("cheats")});
    t.handler = [service, noGame, session](const QJsonObject& a, ToolReply reply) {
      ui::PluginEmulationService* s = service();
      if (s == nullptr || noGame(session())) return reply(ToolResult::error(QStringLiteral("no_game")));
      const QJsonArray arr = a.value(QStringLiteral("cheats")).toArray();
      if (arr.size() > 256) return reply(ToolResult::error(QStringLiteral("at most 256 cheats")));
      QList<plugins::CheatItem> list;
      for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject o = arr.at(i).toObject();
        plugins::CheatItem it;
        it.id = o.value(QStringLiteral("id")).toString(QString::number(i + 1));
        it.name = o.value(QStringLiteral("name")).toString(it.id);
        it.format = o.value(QStringLiteral("format")).toString();
        it.code = o.value(QStringLiteral("code")).toString();
        it.enabled = o.value(QStringLiteral("enabled")).toBool(true);
        list.append(it);
      }
      const plugins::CheatApplyResult r = s->applyCheatsAs(QStringLiteral("mcp"), list, a.value(QStringLiteral("bypass_consent")).toBool(false));
      if (!r.error.isEmpty()) return reply(ToolResult::error(r.error));
      QJsonArray rej;
      for (const plugins::CheatReject& x : r.rejected) rej.append(QJsonObject{{QStringLiteral("id"), x.id}, {QStringLiteral("reason"), x.reason}});
      reply(ToolResult::json(QJsonObject{{QStringLiteral("applied"), r.applied}, {QStringLiteral("rejected"), rej}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("cheats_clear");
    t.description = QStringLiteral("Turns off and removes all active cheats of the running game (the run log gets cheat.disabled for each).");
    t.inputSchema = schema();
    t.handler = [service, noGame, session](const QJsonObject&, ToolReply reply) {
      ui::PluginEmulationService* s = service();
      if (s == nullptr || noGame(session())) return reply(ToolResult::error(QStringLiteral("no_game")));
      const QString err = s->clearCheats(QStringLiteral("mcp"));
      if (!err.isEmpty()) return reply(ToolResult::error(err));
      reply(ToolResult::json(QJsonObject{{QStringLiteral("cleared"), true}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("store_get");
    t.description = QStringLiteral(
        "Reads a key of a plugin's own data (fb.store) from the local copy of the connected user: {found, value, revision, "
        "conflicts: [{value, revision, source}]}. Does not need the plugin to run; error \"plugin_not_found\" for a plugin that is neither "
        "installed nor has data.");
    t.inputSchema = schema({{QStringLiteral("plugin_id"), typeStr(QStringLiteral("Plugin id, e.g. \"uzume.cheats\"."))},
                            {QStringLiteral("key"), typeStr(QStringLiteral("Key ([a-z0-9_.-], 1 to 64 characters)."))}},
                           {QStringLiteral("plugin_id"), QStringLiteral("key")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      const auto store = pc && pc->plugins() ? pc->plugins()->store() : nullptr;
      const QString id = a.value(QStringLiteral("plugin_id")).toString(), key = a.value(QStringLiteral("key")).toString();
      if (!store) return reply(ToolResult::error(QStringLiteral("no_store")));
      if (!plugins::PluginStore::validPluginId(id) || !plugins::PluginStore::validKey(key)) return reply(ToolResult::error(QStringLiteral("invalid")));
      // A plugin the app has never heard of (not installed, no data kept) is an error, not an empty key.
      if (!pc->plugins()->manager()->info(id) && !store->knownPlugins().contains(id)) return reply(ToolResult::error(QStringLiteral("plugin_not_found")));
      const auto e = store->get(id, key);
      QJsonArray conflicts;
      for (const auto& c : store->conflicts(id, key))
        conflicts.append(QJsonObject{{QStringLiteral("value"), c.value}, {QStringLiteral("revision"), static_cast<double>(c.revision)}, {QStringLiteral("source"), c.source}});
      reply(ToolResult::json(QJsonObject{{QStringLiteral("found"), e.found}, {QStringLiteral("value"), e.value},
                                         {QStringLiteral("revision"), static_cast<double>(e.revision)}, {QStringLiteral("conflicts"), conflicts}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("store_put");
    t.description = QStringLiteral(
        "Writes a key of a plugin's own data (fb.store) into the local copy and lets the sync push it to the server. Optional "
        "base_revision as in fb.store.put: a stale one keeps both versions as a conflict. Changes the user's plugin data.");
    t.inputSchema = schema({{QStringLiteral("plugin_id"), typeStr(QStringLiteral("Plugin id."))},
                            {QStringLiteral("key"), typeStr(QStringLiteral("Key ([a-z0-9_.-], 1 to 64 characters)."))},
                            {QStringLiteral("value"), typeAny(QStringLiteral("Any JSON value (max 256 KiB)."))},
                            {QStringLiteral("base_revision"), typeInt(QStringLiteral("Revision the value is based on (0 = the key must be new)."))}},
                           {QStringLiteral("plugin_id"), QStringLiteral("key"), QStringLiteral("value")});
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      const auto store = pc && pc->plugins() ? pc->plugins()->store() : nullptr;
      const QString id = a.value(QStringLiteral("plugin_id")).toString(), key = a.value(QStringLiteral("key")).toString();
      if (!store) return reply(ToolResult::error(QStringLiteral("no_store")));
      if (!plugins::PluginStore::validPluginId(id) || !plugins::PluginStore::validKey(key)) return reply(ToolResult::error(QStringLiteral("invalid")));
      std::optional<qint64> base;
      if (a.contains(QStringLiteral("base_revision"))) base = static_cast<qint64>(a.value(QStringLiteral("base_revision")).toDouble());
      const auto r = store->put(id, key, a.value(QStringLiteral("value")), base);
      using K = plugins::PluginStore::PutResult::Kind;
      const char* kind = r.kind == K::Ok ? "ok" : r.kind == K::Conflict ? "conflict" : r.kind == K::TooLarge ? "too_large" : r.kind == K::Quota ? "quota" : "invalid";
      if (r.kind != K::Ok && r.kind != K::Conflict) return reply(ToolResult::error(QString::fromLatin1(kind)));
      reply(ToolResult::json(QJsonObject{{QStringLiteral("result"), QString::fromLatin1(kind)}, {QStringLiteral("revision"), static_cast<double>(r.revision)}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("consent_set");
    t.description = QStringLiteral(
        "Pre-answers the per-game consent question of plugins (\"cheats\" or \"memory_write\") without showing it. allow true is "
        "remembered for the game (like the confirm button); allow false refuses for this session; forget true removes the "
        "answer so the question is asked again. game = library game id, empty = the running game.");
    t.inputSchema = schema({{QStringLiteral("kind"), typeStr(QStringLiteral("\"cheats\" or \"memory_write\"."))},
                            {QStringLiteral("allow"), typeBool(QStringLiteral("true = allow (remembered), false = refuse."))},
                            {QStringLiteral("forget"), typeBool(QStringLiteral("true = ask again (allow is ignored)."))},
                            {QStringLiteral("game"), typeStr(QStringLiteral("Library game id; empty = the running game."))}},
                           {QStringLiteral("kind")});
    t.handler = [service, consentName](const QJsonObject& a, ToolReply reply) {
      ui::PluginEmulationService* s = service();
      if (s == nullptr) return reply(ToolResult::error(QStringLiteral("no_service")));
      const QString k = a.value(QStringLiteral("kind")).toString();
      if (k != QLatin1String("cheats") && k != QLatin1String("memory_write")) return reply(ToolResult::error(QStringLiteral("kind must be cheats or memory_write")));
      const auto kind = k == QLatin1String("cheats") ? ui::PluginEmulationService::Kind::Cheats : ui::PluginEmulationService::Kind::MemoryWrite;
      const QString game = a.value(QStringLiteral("game")).toString();
      const QString key = game.isEmpty() ? QString() : QStringLiteral("game:") + game;
      if (key.isEmpty() && !s->active()) return reply(ToolResult::error(QStringLiteral("no_game")));
      const auto value = a.value(QStringLiteral("forget")).toBool(false)    ? ui::PluginEmulationService::Consent::Ask
                         : a.value(QStringLiteral("allow")).toBool(false) ? ui::PluginEmulationService::Consent::Allowed
                                                                           : ui::PluginEmulationService::Consent::Denied;
      s->setConsent(kind, value, key);
      reply(ToolResult::json(QJsonObject{{QStringLiteral("kind"), k}, {QStringLiteral("consent"), consentName(value)}}));
    };
    registry.add(std::move(t));
  }
}

}  // namespace uzume::mcp
