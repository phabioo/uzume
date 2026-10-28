#pragma once

#include <QObject>
#include <QStringList>
#include <QVariant>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "manifest.h"
#include "plugin_memory.h"
#include "plugin_types.h"
#include "plugin_ui.h"
#include "plugin_value.h"

class QThread;

namespace uzume::plugins {

class ConsoleStore;
class PluginEngine;
class HostWorker;

// Runs plugins on one dedicated plugin thread (never the UI or emulation thread): one Lua state and budget per plugin.
// A Lua error, budget or memory overrun pauses only that plugin; the game and the other plugins continue.
// All public methods are called from the owner's (UI) thread, consoleSnapshot() from any thread.
// Result of a debug probe on the plugin thread (PluginHost::callProvided / evalChunk, used by the MCP server).
struct ProbeResult {
  bool ok = false;
  QString error;                // first line of the problem ("plugin_not_running", a Lua error, a budget trip ...)
  QString trace;                // the rest of the Lua error (traceback), may be empty
  std::vector<Value> values;    // results of the call or chunk
  QList<LogLine> output;        // print() and uz.log lines written during the call
  bool paused = false;          // a tripped budget paused the plugin
};
using ProbeCallback = std::function<void(ProbeResult)>;

class PluginHost : public QObject {
  Q_OBJECT
 public:
  explicit PluginHost(QObject* parent = nullptr);
  ~PluginHost() override;

  // Starts `plugins` (already in dependency order, see PluginManager::resolveForGame) for a game. Replaces a running
  // set. Statuses are Loading immediately and become Running/Paused on the plugin thread.
  void start(const GameContext& ctx, std::shared_ptr<MemoryProvider> memory, const QList<PluginInfo>& plugins);
  // Stops all plugins (blocks until the plugin thread is done), unwatches their ranges.
  void stop();
  // Re-reads the manifest and entry file of one plugin and restarts its Lua state.
  void reload(const QString& id);
  // Maximum on_tick rate per plugin, 1..240, default 30.
  void setTickHz(int hz);
  // Budgets for plugins started or reloaded afterwards.
  void setLimits(const HostLimits& limits);
  HostLimits limits() const { return m_limits; }

  // --- plugin UI (uz.ui), see plugin_ui.h ---
  // An action of the UI (button, toggle, list row, toast action, hotkey ...): calls the plugin's uz.ui.on_action handler
  // on the plugin thread with (action, value, slot). Ignored when the plugin has no handler or is not running.
  void sendAction(const QString& pluginId, const QString& slot, const QString& action, const QVariant& value = QVariant());
  // Host event with source plugin id "uz" (uz.events.on("uz", name, fn)); `targetPlugin` empty = all subscribers.
  void postHostEvent(const QString& name, const QVariant& value, const QString& targetPlugin = QString());
  // Host event with source plugin id "app" (uz.events.on("app", name, fn)): events the app raises on behalf of the person or a run
  // rule, e.g. cheats.blocked {reason}. `targetPlugin` empty = all subscribers.
  void postAppEvent(const QString& name, const QVariant& value, const QString& targetPlugin = QString());
  // uz.ui.pick_file: requests waiting for a file dialog, and the answer. `error` empty = `content` (and the base name of the file)
  // is handed to the callback; otherwise the callback gets nil, error ("cancelled", "too_large", "read_failed").
  QList<PickRequest> takePickRequests();
  void deliverFile(const QString& pluginId, quint64 requestId, const QByteArray& content, const QString& name, const QString& error = QString());
  // Removes the tree of `slot` from every plugin (the host clears "detail" when the shown game changes).
  void clearSlot(const QString& slot);
  UiPluginState uiState(const QString& pluginId) const;
  QVariantMap uiTree(const QString& pluginId, const QString& slot) const;
  QList<QVariantMap> takeToasts();
  QList<QPair<QString, QString>> takeOpenRequests();  // uz.ui.open requests (plugin id, slot)
  QList<QPair<QString, QString>> takeCloseRequests();  // uz.ui.close requests (plugin id, slot)

  // --- debug probes (MCP server, ADR 0032) ---
  // Both run on the plugin thread in the plugin's own Lua state under the normal budget and call `done` on the owner's
  // (UI) thread: callProvided calls `fn` of the table the plugin registered with uz.provide(`iface`); evalChunk compiles
  // `code` as a text chunk (first as an expression, "return <code>") in the plugin's global environment. A tripped budget
  // pauses the plugin like any handler would. Results larger than 32 MiB fail.
  void callProvided(const QString& pluginId, const QString& iface, const QString& fn, const std::vector<Value>& args, ProbeCallback done);
  void evalChunk(const QString& pluginId, const QString& code, ProbeCallback done);

  QStringList pluginIds() const;
  ConsoleSnapshot consoleSnapshot(const QString& id) const;

 signals:
  void statusChanged(const QString& id);
  // Console data changed (log, events, watched rows); rate-limited to about 10 per second per plugin.
  void consoleChanged(const QString& id);
  // UI data changed: `what` is a slot name, "@meta" (title, badge, dot, paused), "@toast" or "@open". Coalesced: at most one
  // emission per slot about every 33 ms (30 updates per second).
  void uiChanged(const QString& id, const QString& what);

 private:
  std::unique_ptr<ConsoleStore> m_store;
  std::atomic<int> m_tickHz{30};
  HostLimits m_limits;
  QThread* m_thread = nullptr;
  HostWorker* m_worker = nullptr;
};

}  // namespace uzume::plugins
