#pragma once

// Internal: the single-threaded plugin engine. All methods except ConsoleStore's run on the plugin thread.
#include <QFile>
#include <QHash>
#include <QMap>
#include <QSet>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "manifest.h"
#include "plugin_memory.h"
#include "plugin_types.h"
#include "plugin_ui.h"
#include "plugin_value.h"
#include "plugin_video.h"

namespace uzume::plugins {

// Thread-safe console data shared with the UI thread.
class ConsoleStore {
 public:
  void reset(const QStringList& ids, PluginStatus status);
  void setStatus(const QString& id, PluginStatus status, const QString& error);
  void addLog(const QString& id, LogLevel level, const QString& text);
  void addEvent(const QString& id, const EventLine& line);
  bool setWatched(const QString& id, const QList<WatchedRow>& rows);  // true when changed
  ConsoleSnapshot snapshot(const QString& id) const;
  QStringList ids() const;
  QStringList takeDirty();

  UiStore ui;  // trees, titles, badges and toasts of fb.ui (own mutex)

 private:
  mutable std::mutex m_mutex;
  QMap<QString, ConsoleSnapshot> m_map;
  QStringList m_order;
  QSet<QString> m_dirty;
};

class PluginEngine;
struct ProbeResult;

struct DevWatch {
  QString label;
  uint64_t addr = 0;
  QString kind;  // u8 u16 u32 s32 hex
  uint32_t size = 0;
};

struct EventSub {
  QString plugin;  // "*" = any
  QString name;
  int ref = 0;
};

struct RunSub {
  std::string kind;  // event | state | status | participants
  int ref = 0;
};

// Results of fb.video.capture arrive on any thread (the Player's GUI thread) and are drained on the plugin thread.
// Shared with the callbacks, so a late result after stop() lands in a mailbox nobody reads and is freed with it.
struct CaptureInbox {
  struct Item {
    QString plugin;
    uint64_t id = 0;
    RawFrame frame;
  };
  std::mutex mutex;
  std::vector<Item> items;
};

struct PendingCapture {
  uint64_t id = 0;
  int x = 0, y = 0, w = 0, h = 0;
  std::chrono::steady_clock::time_point at;
};

struct Plugin {
  PluginInfo info;
  PluginEngine* engine = nullptr;
  std::unique_ptr<LuaSandbox> lua;
  PluginStatus status = PluginStatus::Loading;
  int tickRef = -2;  // LUA_NOREF
  std::vector<EventSub> subs;
  std::map<std::string, int> provided;  // interface name -> registry ref of its table
  std::map<std::string, WatchRange> watches;
  std::vector<DevWatch> devWatches;
  std::unique_ptr<QFile> rom;
  QHash<QString, std::optional<QHash<QString, QString>>> i18n;  // language -> strings (nullopt = no such file)
  qint64 cacheBytes = -1;
  int cacheFiles = 0;  // files in the cache, valid with cacheBytes
  QList<WatchedRow> lastRows;
  int actionRef = -2;                         // fb.ui.on_action handler (LUA_NOREF)
  std::vector<RunSub> runSubs;                // fb.run.on callbacks
  QJsonObject boardPending;                   // fb.run.setBoard: latest tree waiting for the 1/s limit
  bool hasBoardPending = false;
  std::chrono::steady_clock::time_point boardLast;
  std::map<QString, QVariantMap> uiTrees;     // slot -> current tree (for fb.ui.patch)
  uint64_t captureNextId = 1;                 // fb.video.capture request ids
  std::optional<PendingCapture> capture;      // at most one pending capture
  std::optional<std::chrono::steady_clock::time_point> captureLast;  // last accepted request (1 per second)
  std::optional<std::chrono::steady_clock::time_point> snapshotLast;  // fb.save.snapshot: last accepted call (1 per minute)
  std::map<uint64_t, int> pickRefs;           // fb.ui.pick_file: request id -> registry ref of the callback
  uint64_t pickNextId = 1;
  double toastTokens = 3;                     // notify rate limit: burst 3, one per second sustained
  std::chrono::steady_clock::time_point toastLast = std::chrono::steady_clock::now();

  int slowCalls = 0;                          // consecutive callbacks above half the wall budget

  QString id() const { return info.manifest.id; }
};

struct QueuedEvent {
  QString from;
  std::string name;
  Value value;
  QString target;  // empty = every subscribed plugin
  size_t bytes = 0;  // valueBytes(value) + name: what the queue accounts for
};

// Event queue limits: by count and by (real) bytes, in total and per emitting plugin (plugin "fb" = host events).
inline constexpr size_t kMaxQueuedEventCount = 10000;
inline constexpr size_t kMaxQueuedEventBytes = 16u * 1024 * 1024;
inline constexpr size_t kMaxQueuedEventBytesPerPlugin = 4u * 1024 * 1024;
// One iterate() delivers events for at most this long; the rest stays queued.
inline constexpr int kIterateBudgetMs = 50;

class PluginEngine {
 public:
  PluginEngine(ConsoleStore& store, const std::atomic<int>& tickHz, std::function<void(const QString&)> statusChanged);
  ~PluginEngine();

  void start(const GameContext& ctx, std::shared_ptr<MemoryProvider> provider, const QList<PluginInfo>& list, const HostLimits& limits);
  void stop();
  void reload(const QString& id);
  void iterate();  // one host iteration: events, tick, watched rows
  // Host-side input from the UI thread (queued onto the plugin thread by the host).
  void deliverAction(const QString& pluginId, const QString& slot, const QString& action, const Value& value);
  void postHostEvent(const QString& name, Value value, const QString& target);  // event source "fb"
  void postAppEvent(const QString& name, Value value, const QString& target);   // event source "app" (cheats.blocked ...)
  // fb.ui.pick_file result: calls the callback of the request once; `error` empty = content valid.
  void deliverFile(const QString& pluginId, uint64_t requestId, const std::string& content, const std::string& name, const std::string& error);
  // Whether an event of `bytes` from `from` still fits the queue (count, total bytes, per-source bytes).
  bool eventFits(const QString& from, size_t bytes) const;
  size_t queuedEventBytes() const { return queuedBytes; }
  void clearSlotEverywhere(const QString& slot);                                 // e.g. "detail" when the game changes

  // --- used by plugin_api.cpp ---
  Plugin* find(const QString& id);
  void log(Plugin& p, LogLevel level, const std::string& text);
  void emitEvent(Plugin& p, const std::string& name, Value value);
  void syncWatches(Plugin& p);
  void pause(Plugin& p, const std::string& error);
  // Runs a callback (registry ref) of `p` with arguments pushed by `pushArgs`; pauses `p` on failure.
  void callRef(Plugin& p, int ref, const std::function<int(lua_State*)>& pushArgs);
  void releaseState(Plugin& p);
  // fb.video.capture: returns the request id; the result (or the error) arrives as host event "video.captured" to `p`.
  // `screen` non-empty = a named screen, otherwise the rectangle x/y/w/h (source-frame pixels).
  uint64_t requestCapture(Plugin& p, const std::string& screen, int x, int y, int w, int h);

  // Debug probes of the MCP server (ADR 0032), plugin thread: call fb.provide'd function / run a chunk in the plugin's
  // state under the normal budget. Output (print, fb.log) written meanwhile is captured into the result.
  ProbeResult probeCall(const QString& id, const std::string& iface, const std::string& fn, const std::vector<Value>& args);
  ProbeResult probeEval(const QString& id, const std::string& code);

  ConsoleStore& store;
  GameContext ctx;
  std::shared_ptr<MemoryProvider> provider;
  HostLimits limits;
  std::deque<QueuedEvent> events;
  size_t queuedBytes = 0;
  std::map<QString, size_t> queuedBy;
  void pushEvent(QueuedEvent ev);
  QueuedEvent popEvent();
  std::chrono::steady_clock::time_point started;

 private:
  ProbeResult probe(const QString& id, const std::function<void(lua_State*, std::vector<Value>&)>& body);
  QList<LogLine>* m_captureLog = nullptr;  // set during a probe
  std::chrono::steady_clock::time_point m_iterDeadline;
  void load(Plugin& p);
  void setStatus(Plugin& p, PluginStatus s, const QString& error = QString());
  void deliverEvents();
  void tick();
  void pollRun();
  void updateWatched();
  void pollCaptures();
  void finishCapture(Plugin& p, uint64_t id, const RawFrame* frame, const PendingCapture* cap, const std::string& error);

  std::shared_ptr<CaptureInbox> m_captureInbox = std::make_shared<CaptureInbox>();
  const std::atomic<int>& m_tickHz;
  std::function<void(const QString&)> m_statusChanged;
  std::vector<std::unique_ptr<Plugin>> m_plugins;
  std::optional<uint64_t> m_lastGen;
  std::chrono::steady_clock::time_point m_lastTick;
  std::chrono::steady_clock::time_point m_lastWatched;
};

// fb.cache file names: A-Z a-z 0-9 . _ - and /, no "..", no device names (also used by the MCP server's cache tools).
bool validCacheName(const std::string& name);
// plugin_api.cpp: creates the `fb` global in p.lua.
void registerFbApi(PluginEngine& engine, Plugin& p);
// Formats one watched value ("u8", "u16", "u32", "s32", "hex"); nullopt when not in the snapshot.
std::optional<QString> formatWatched(const MemoryProvider& mem, const DevWatch& w);

}  // namespace uzume::plugins
