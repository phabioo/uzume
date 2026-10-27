// Emulation tools of the debug MCP server (0.20): raw save states kept in memory under a name (never sent to a server, never
// written to disk) and memory writes. Both go through the emulation command queue (EmulationRunner::submit): they run on the
// emulation thread between two frames, also while the game is paused. Unlike the other game tools these change the game.
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QRegularExpression>
#include <memory>

#include "emulation_runner.h"
#include "gamesession.h"
#include "mcp_tools.h"
#include "playercontroller.h"
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
}

}  // namespace uzume::mcp
