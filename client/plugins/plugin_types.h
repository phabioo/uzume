#pragma once

#include <QList>
#include <QString>
#include <memory>

#include "lua_sandbox.h"
#include "plugin_variants.h"
#include "plugin_video.h"
#include "run_bridge.h"

namespace uzume::plugins {

enum class PluginStatus { Loading, Running, Paused, Stopped };

// What the plugins are told about the running game (fb.game). Empty fields become nil in Lua.
struct GameContext {
  QString system;    // "nds", "3ds", ...
  QString code;      // game code, e.g. the 4-letter DS code or the 3DS title id
  QString sha256;
  QString title;
  QString romPath;   // game file for fb.rom (rom.read)
  QString language;  // game language for fb.i18n, e.g. "de", "en", "de-DE"
  bool shell = false;  // shell context: no game runs; fb.game is an empty table, memory/rom reads return nil
  std::shared_ptr<RunBridge> run;  // the game was started in a run: fb.run exists (permission run.state); null = no fb.run
  std::shared_ptr<VideoProvider> video;  // frame source of the running game (fb.video, video.capture); null = fb.video.screens() is nil, captures fail with "unsupported"
  // New fields go at the end (the tests aggregate-initialise the leading ones).
  QString gameId;    // library id of the game (fb.variants.list); empty in the shell context
  QString modDir;    // 3DS variant: LayeredFS mod folder laid over the RomFS of romPath by fb.rom.romfs / romfs_list; empty otherwise
  std::shared_ptr<VariantsProvider> variants;  // fb.variants (permission variants.build); null = fb.variants.register / build fail with "variants are not available"
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
};

inline constexpr int kLogRing = 500;
inline constexpr int kEventRing = 200;

}  // namespace uzume::plugins
