// Save-state tools of the debug MCP server (0.20 package 4): the states of the running game as the person sees them (slots A and
// 1 to 4, kept per person and server, synced when the server has `states_v1`) and rewind. They go through the same
// StatesController as the hotkeys, so run rules (rules.states / rules.rewind) apply. Unlike game_state_save these are the real,
// persistent states of the app.
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <algorithm>
#include <cmath>

#include "emulation_runner.h"
#include "emulationcontroller.h"
#include "mcp_tools.h"
#include "pluginscontroller.h"
#include "playercontroller.h"
#include "savehistorycontroller.h"
#include "rewind_buffer.h"
#include "statescontroller.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;

namespace {

std::optional<int> slotArg(const QJsonObject& a, bool allowAuto) {
  const QJsonValue v = a.value(QStringLiteral("slot"));
  if (v.isUndefined() || v.isNull()) return std::nullopt;
  const int n = v.isString() ? v.toString().toInt() : v.toInt(-1);
  if (n < (allowAuto ? 0 : 1) || n > kStateSlotCount) return std::nullopt;
  return n;
}

ToolResult resultJson(const ui::StatesController::Result& r, const QJsonObject& extra = {}) {
  if (!r.ok) return ToolResult::error(r.error.isEmpty() ? QStringLiteral("failed") : r.error + (r.message.isEmpty() ? QString() : QStringLiteral(": ") + r.message));
  QJsonObject o = extra;
  o.insert(QStringLiteral("slot"), r.n);
  if (!r.message.isEmpty()) o.insert(QStringLiteral("message"), r.message);
  return ToolResult::json(o);
}

}  // namespace

void installStatesTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);
  const auto states = [pc]() -> ui::StatesController* { return pc ? pc->states() : nullptr; };

  {
    ToolDef t;
    t.name = QStringLiteral("states_list");
    t.description = QStringLiteral(
        "Lists the save states of the running game (or the game open on its game page): Auto (slot 0, \"A\") and slots 1 to 4 with "
        "when and by which core they were saved, whether they are on the server or still waiting to be uploaded, and the "
        "\"Older core\" tag. Also the slot F6/F8 work on, why saving is blocked right now (run rule; empty = allowed), whether the "
        "core supports states and whether rewind is available.");
    t.inputSchema = schema();
    t.handler = [states](const QJsonObject&, ToolReply reply) {
      ui::StatesController* s = states();
      if (s == nullptr) return reply(ToolResult::error(QStringLiteral("no_game")));
      QJsonArray rows;
      for (const QVariant& v : s->slotRows()) {
        QVariantMap m = v.toMap();
        m.remove(QStringLiteral("previewUrl"));
        rows.append(QJsonObject::fromVariantMap(m));
      }
      reply(ToolResult::json(QJsonObject{{QStringLiteral("supported"), s->supported()},
                                         {QStringLiteral("playing"), s->playing()},
                                         {QStringLiteral("slot"), s->slot()},
                                         {QStringLiteral("summary"), s->summary()},
                                         {QStringLiteral("blocked"), s->blockedText()},
                                         {QStringLiteral("rewind_available"), s->rewindAvailable()},
                                         {QStringLiteral("states"), rows}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("states_save");
    t.description = QStringLiteral(
        "Saves a state of the running game into slot 1 to 4 (default: the slot F6 uses), like the F6 key: stored on this PC and "
        "uploaded to the server when it has `states_v1`. Not the game's save. Errors: \"no_game\", \"unsupported\", \"rule_states\" "
        "(the run's rule set forbids states), \"failed\".");
    t.inputSchema = schema({{QStringLiteral("slot"), typeInt(QStringLiteral("State slot 1 to 4 (default: the current slot)."))}});
    t.targetArg = QStringLiteral("slot");
    t.handler = [states](const QJsonObject& a, ToolReply reply) {
      ui::StatesController* s = states();
      if (s == nullptr) return reply(ToolResult::error(QStringLiteral("no_game")));
      const bool given = a.contains(QStringLiteral("slot"));
      const std::optional<int> slot = slotArg(a, false);
      if (given && !slot) return reply(ToolResult::error(QStringLiteral("slot must be 1 to 4")));
      s->save(slot.value_or(s->slot()), [reply](const ui::StatesController::Result& r) { reply(resultJson(r)); });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("states_load");
    t.description = QStringLiteral(
        "Loads the state of a slot (0 = Auto, 1 to 4) into the running game without asking. The progress before the load is kept "
        "in Auto first (loading Auto itself keeps nothing new). Errors: \"no_game\", \"unsupported\", \"rule_states\", \"empty\", "
        "\"other_core\", \"unavailable\" (the state is on the server but could not be downloaded), \"failed\".");
    t.inputSchema = schema({{QStringLiteral("slot"), typeInt(QStringLiteral("State slot 0 (Auto) to 4 (default: the current slot)."))}});
    t.targetArg = QStringLiteral("slot");
    t.handler = [states](const QJsonObject& a, ToolReply reply) {
      ui::StatesController* s = states();
      if (s == nullptr) return reply(ToolResult::error(QStringLiteral("no_game")));
      const bool given = a.contains(QStringLiteral("slot"));
      const std::optional<int> slot = slotArg(a, true);
      if (given && !slot) return reply(ToolResult::error(QStringLiteral("slot must be 0 to 4")));
      s->load(slot.value_or(s->slot()), true, [reply](const ui::StatesController::Result& r) { reply(resultJson(r)); });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("states_delete");
    t.description = QStringLiteral(
        "Deletes the state of a slot (0 = Auto, 1 to 4) on this PC and on the server (a delete that cannot reach the server stays "
        "pending). Works for the game open on its game page too. Error \"empty\" when the slot holds nothing.");
    t.inputSchema = schema({{QStringLiteral("slot"), typeInt(QStringLiteral("State slot 0 (Auto) to 4."))}}, {QStringLiteral("slot")});
    t.targetArg = QStringLiteral("slot");
    t.handler = [states](const QJsonObject& a, ToolReply reply) {
      ui::StatesController* s = states();
      if (s == nullptr) return reply(ToolResult::error(QStringLiteral("no_game")));
      const std::optional<int> slot = slotArg(a, true);
      if (!slot) return reply(ToolResult::error(QStringLiteral("slot must be 0 to 4")));
      s->remove(*slot, [reply](const ui::StatesController::Result& r) { reply(resultJson(r)); });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("rewind");
    t.description = QStringLiteral(
        "Steps the running game back in time by `seconds` or by `frames` (one of them; rounded up to the rewind buffer's step of 4 "
        "frames), also while paused. Needs rewind to be on (Settings > Emulation > Rewind length) and the core to support states. "
        "Returns the frames it really went back. Errors: \"no_game\", \"unsupported\", \"rule_rewind\" (the run's rule set forbids "
        "rewind), \"empty\" (nothing recorded yet).");
    t.inputSchema = schema({{QStringLiteral("seconds"), typeAny(QStringLiteral("Seconds to go back (at 60 frames per second)."))},
                            {QStringLiteral("frames"), typeInt(QStringLiteral("Frames to go back."))}});
    t.handler = [states](const QJsonObject& a, ToolReply reply) {
      ui::StatesController* s = states();
      if (s == nullptr) return reply(ToolResult::error(QStringLiteral("no_game")));
      double frames = 0;
      if (a.contains(QStringLiteral("frames"))) frames = a.value(QStringLiteral("frames")).toDouble();
      else if (a.contains(QStringLiteral("seconds"))) frames = a.value(QStringLiteral("seconds")).toVariant().toDouble() * 60.0;
      if (!(frames > 0) || frames > 60.0 * 600) return reply(ToolResult::error(QStringLiteral("seconds or frames (positive) is required")));
      const int snapshots = (std::max)(1, static_cast<int>(std::ceil(frames / emu::kRewindIntervalFrames)));
      s->rewindBy(snapshots, [reply](const ui::StatesController::Result& r) {
        if (!r.ok) return reply(resultJson(r));
        const int back = r.n * emu::kRewindIntervalFrames;
        reply(ToolResult::json(QJsonObject{{QStringLiteral("frames"), back}, {QStringLiteral("seconds"), back / 60.0}}));
      });
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("saves_history");
    t.description = QStringLiteral(
        "Read only: the save history of the game open in the saves view (game page or in-game panel): the slot, the current checkpoint and "
        "the versions newest first with reason, device, whether made with cheats and the automatic kind (\"cheats\" = Before cheats, "
        "\"memory\" = Before memory changes). Restoring stays with the person.");
    t.inputSchema = schema();
    t.handler = [pc](const QJsonObject&, ToolReply reply) {
      ui::SaveHistoryController* h = pc ? pc->saveHistory() : nullptr;
      if (h == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      QJsonArray rows;
      for (const QVariant& v : h->history()) rows.append(QJsonObject::fromVariantMap(v.toMap()));
      reply(ToolResult::json(QJsonObject{{QStringLiteral("game_id"), h->gameId()},
                                         {QStringLiteral("slot"), h->slot()},
                                         {QStringLiteral("slot_label"), h->slotLabel()},
                                         {QStringLiteral("loading"), h->loading()},
                                         {QStringLiteral("online"), h->online()},
                                         {QStringLiteral("current"), QJsonObject::fromVariantMap(h->current())},
                                         {QStringLiteral("versions"), rows}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("toasts_list");
    t.description = QStringLiteral(
        "The last toasts the app showed (20 per source, oldest first): the states toasts (saved, loaded, refused with the rule text) and the "
        "notices of plugins and of the app (\"Cheats are off again\", \"Run rules\"). Each has source, text, a time and, for plugin toasts, "
        "the plugin and tone. Read only.");
    t.inputSchema = schema();
    t.handler = [pc](const QJsonObject&, ToolReply reply) {
      if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
      QList<QVariantMap> all;
      if (ui::StatesController* s = pc->states())
        for (const QVariant& v : s->toastLog()) all.append(v.toMap());
      if (ui::PluginsController* p = pc->plugins())
        for (const QVariant& v : p->toastLog()) all.append(v.toMap());
      std::stable_sort(all.begin(), all.end(), [](const QVariantMap& a, const QVariantMap& b) { return a.value(QStringLiteral("time")).toString() < b.value(QStringLiteral("time")).toString(); });
      QJsonArray out;
      for (const QVariantMap& m : all) out.append(QJsonObject::fromVariantMap(m));
      reply(ToolResult::json(QJsonObject{{QStringLiteral("toasts"), out}}));
    };
    registry.add(std::move(t));
  }

  {
    ToolDef t;
    t.name = QStringLiteral("settings_set");
    t.description = QStringLiteral(
        "Sets an app setting of Settings > Emulation > Uzume the way a person would, for tests: key is the option key (for example "
        "uzume.rewind_seconds with seconds 0 to 600, uzume.run_ahead 0 | 1 | 2, uzume.fullscreen on | off). Scope: global by default, "
        "system for a system, game_id for one game. Takes effect on the running game at once. Back to the default value removes the "
        "stored value. Errors: unknown_key, invalid_value.");
    t.inputSchema = schema({{QStringLiteral("key"), typeStr(QStringLiteral("Option key, for example uzume.rewind_seconds."))},
                            {QStringLiteral("value"), typeStr(QStringLiteral("The value as text, for example \"30\"."))},
                            {QStringLiteral("system"), typeStr(QStringLiteral("System id for a system override (nds, gba, ...)."))},
                            {QStringLiteral("game_id"), typeStr(QStringLiteral("Game id for a game override."))}},
                           {QStringLiteral("key"), QStringLiteral("value")});
    t.targetArg = QStringLiteral("key");
    t.handler = [pc](const QJsonObject& a, ToolReply reply) {
      if (!pc || pc->emulation() == nullptr) return reply(ToolResult::error(QStringLiteral("app_closing")));
      const QString key = a.value(QStringLiteral("key")).toString();
      const QJsonValue v = a.value(QStringLiteral("value"));
      const QString value = v.isString() ? v.toString() : (v.isDouble() ? QString::number(v.toInt()) : (v.isBool() ? (v.toBool() ? QStringLiteral("true") : QStringLiteral("false")) : QString()));
      if (key.isEmpty() || value.isEmpty()) return reply(ToolResult::error(QStringLiteral("key and value required")));
      const QString system = a.value(QStringLiteral("system")).toString(), game = a.value(QStringLiteral("game_id")).toString();
      const QString err = pc->emulation()->setUzumeOption(key, value, system, game);
      if (!err.isEmpty()) return reply(ToolResult::error(err));
      reply(ToolResult::json(QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("value"), pc->emulation()->uzumeValue(key, system, game)}}));
    };
    registry.add(std::move(t));
  }
}

}  // namespace uzume::mcp
