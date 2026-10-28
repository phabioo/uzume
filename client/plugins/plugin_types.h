#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <chrono>
#include <deque>
#include <memory>

#include "lua_sandbox.h"
#include "plugin_variants.h"
#include "plugin_emulation.h"
#include "plugin_sound.h"
#include "plugin_store.h"
#include "plugin_video.h"
#include "run_bridge.h"

namespace uzume::plugins {

enum class PluginStatus { Loading, Running, Paused, Stopped };

// What the plugins are told about the running game (uz.game). Empty fields become nil in Lua.
struct GameContext {
  QString system;    // "nds", "3ds", ...
  QString code;      // game code, e.g. the 4-letter DS code or the 3DS title id
  QString sha256;
  QString title;
  QString romPath;   // game file for uz.rom (rom.read)
  QString language;  // game language for uz.i18n, e.g. "de", "en", "de-DE"
  bool shell = false;  // shell context: no game runs; uz.game is an empty table, memory/rom reads return nil
  std::shared_ptr<RunBridge> run;  // the game was started in a run: uz.run exists (permission run.state); null = no uz.run
  std::shared_ptr<EmulationBridge> emulation;  // uz.memory.write, uz.cheats, uz.save.snapshot (0.20); null = "no_game"
  std::shared_ptr<PluginStore> store;  // uz.store (permission store); null = nil, "unavailable"
  std::shared_ptr<VideoProvider> video;  // frame source of the running game (uz.video, video.capture); null = uz.video.screens() is nil, captures fail with "unsupported"
  // New fields go at the end (the tests aggregate-initialise the leading ones).
  QString gameId;    // library id of the game (uz.variants.list); empty in the shell context
  QString modDir;    // 3DS variant: LayeredFS mod folder laid over the RomFS of romPath by uz.rom.romfs / romfs_list; empty otherwise
  std::shared_ptr<SoundSink> sound;  // uz.sound (permission sound); null = uz.sound.play returns nil, "unavailable"
  std::shared_ptr<VariantsProvider> variants;  // uz.variants (permission variants.build); null = uz.variants.register / build fail with "variants are not available"
};

struct HostLimits {
  size_t memoryBytes = 64u * 1024 * 1024;
  qint64 instructions = 20'000'000;
  int wallMs = 500;
};

// Developer-console data (design 4d-4-2).
struct LogLine {
  qint64 timeMs = 0;  // ms since epoch
  LogLevel level = LogLevel::Info;
  QString text;
  qint64 seq = 0;     // per plugin, 1, 2, 3 ... in the order written (the console and the MCP "since_seq" filter); not reset by a reload
};
struct EventLine {
  qint64 timeMs = 0;
  QString plugin;  // emitting plugin
  QString name;    // event name ("kind" chip)
  QString text;    // short rendering of the value
};
struct WatchedRow {
  QString label;
  QString address;  // "0x08001A10"
  QString value;    // rendered per kind; "-" when not in the latest snapshot
};
struct ConsoleSnapshot {
  PluginStatus status = PluginStatus::Stopped;
  QString error;  // message + traceback of the error that paused the plugin; empty otherwise
  QList<LogLine> log;       // ring of 500
  QList<EventLine> events;  // ring of 200
  QList<WatchedRow> watched;
  qint64 lastSeq = 0;       // seq of the newest log line ever written (also after it left the ring)
  QStringList notGranted;   // permissions the plugin tried to use without declaring them (fb.* call refused), in first-use order
  qint64 reloadMs = -1;     // duration of the last reload (entry file read and run), -1 = never reloaded
  int reloadCount = 0;      // reloads since the game started
  int errorCount = 0;       // handler errors (logged, not paused) plus the one that paused the plugin
};

// Pause rule of 0.22 (design 4d-2D): a plugin is paused after three handler errors within ten seconds, not on the first.
// Single errors are only logged. Budget and memory overruns do not count here: they pause at once.
class ErrorBurst {
 public:
  static constexpr int kLimit = 3;
  static constexpr int kWindowMs = 10000;
  using Clock = std::chrono::steady_clock;
  // Records an error at `now`; true when it is the third (or later) within the window.
  bool record(Clock::time_point now) {
    m_times.push_back(now);
    while (!m_times.empty() && now - m_times.front() > std::chrono::milliseconds(kWindowMs)) m_times.pop_front();
    return static_cast<int>(m_times.size()) >= kLimit;
  }
  int count() const { return static_cast<int>(m_times.size()); }
  void reset() { m_times.clear(); }

 private:
  std::deque<Clock::time_point> m_times;
};

inline constexpr int kLogRing = 500;
inline constexpr int kEventRing = 200;

}  // namespace uzume::plugins
