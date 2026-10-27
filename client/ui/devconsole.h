#pragma once
// DevConsole: the one data source of the plugin developer console (design boards 4d-1 and 4d-2, Uzume 0.22). The QML console
// (Settings > Plugins > Developer console and the Plugin console tab of the game panel) and the debug MCP tools (plugin_console,
// devconsole_state) read the same model, so what Claude sees is what the person sees. It owns the console state (selected
// plugin, level filters, paused log, cleared lines, the memory viewer) and builds the model from a few sources that the
// PluginsController wires (and tests replace with fakes). It never touches the Lua states itself.
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "manifest.h"
#include "memory_access.h"
#include "plugin_types.h"

namespace uzume::ui {

class GameSession;

// One log line as the console shows it. `seq` counts per plugin (1, 2, 3 ...), `level` is error | warn | info | debug.
struct ConsoleLogEntry {
  qint64 seq = 0;
  qint64 timeMs = 0;
  QString level;
  QString text;
};

struct ConsoleLogFilter {
  QSet<QString> levels;      // shown levels; empty = all four
  qint64 sinceSeq = 0;       // only lines with seq > sinceSeq
  qint64 maxSeq = -1;        // -1 = none; otherwise only lines with seq <= maxSeq (the log is paused at that line)
  qint64 clearedBeforeMs = 0;  // "Clear": lines up to this time are hidden
  int limit = 200;           // lines returned
};

struct ConsoleLogView {
  QList<ConsoleLogEntry> lines;  // newest first, after all filters
  int error = 0, warn = 0, info = 0, debug = 0;  // lines per level BEFORE the level and since filters (the chips' counts)
  int total = 0;
  qint64 lastSeq = 0;            // newest seq in the input
  int hiddenNew = 0;             // lines newer than maxSeq (a paused log)
  int countOf(const QString& level) const;
};

// Read access to the running game's memory for the viewer (the same code the MCP tools memory_read / memory_search use).
class MemorySource {
 public:
  virtual ~MemorySource() = default;
  virtual bool available() const = 0;  // a game with a memory map runs
  virtual std::vector<emu::MemoryMap::RegionInfo> regions() const = 0;
  // false + error ("no_game", "no_memory_map", "emulation_busy") when the read could not be done; data may be shorter than len.
  virtual bool read(quint64 addr, quint64 len, QByteArray* out, QString* error) = 0;
  virtual bool search(quint64 start, quint64 end, const QByteArray& pattern, int max, emu::memaccess::SearchResult* out, QString* error) = 0;
};
// The live source: GameSession::runOnEmuThread on the emulation thread between frames.
std::unique_ptr<MemorySource> makeGameMemorySource(GameSession* session);

class DevConsole : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PluginsController")

  // Console model, refreshed at most about 10 times per second. Keys: developerMode, plugins [{id, name, status, statusText,
  // running, unpacked, folder}], selected, name, running, unpacked, folder, status, statusText, paused, error, errorFirst,
  // errorLine, permissions {granted [{id,label}], notGranted [{id,label}]}, watched, events, log (filtered, newest first:
  // {seq,time,level,text}), counts {error,warn,info,debug,total}, filters {error,warn,info,debug}, logPaused, logPending,
  // regions, regionsText, inspect {runState, runName, store [{key,bytes,revision,preview}], watched}, reload {count, ms, text},
  // reloadOnChange, watching.
  Q_PROPERTY(QVariantMap model READ model NOTIFY modelChanged)
  // Memory viewer: {available, regions [{index,value,label,start,length,addrspace}], regionIndex, baseText, rows [{addr,hex,ascii}],
  // error, search {kind, text, matches [string], truncated, scanned, error, active}}.
  Q_PROPERTY(QVariantMap memory READ memory NOTIFY memoryChanged)
  Q_PROPERTY(bool reloadOnChange READ reloadOnChange WRITE setReloadOnChange NOTIFY reloadOnChangeChanged)

 public:
  struct Sources {
    std::function<bool()> developerMode;
    std::function<QStringList()> activeIds;  // plugins that run now (the game's set, else the shell's), start order
    std::function<std::optional<plugins::PluginInfo>(const QString&)> info;
    std::function<plugins::ConsoleSnapshot(const QString&)> snapshot;
    std::function<QList<plugins::PluginInfo>()> unpacked;  // dev-linked plugins (listed even when they do not run)
    std::function<QVariantMap(const QString&)> inspect;    // {runName, runState, store: [{key, bytes, revision, preview}]}
    std::function<QString(const QString&)> permissionLabel;
    std::function<void(const QString&)> reload;
    std::function<bool(bool)> setWatching;  // "Reload on file change" (PluginManager::setWatchEnabled); optional
  };
  explicit DevConsole(Sources sources, QObject* parent = nullptr);

  QVariantMap model() const { return model_; }
  QVariantMap memory() const { return memory_; }
  bool reloadOnChange() const { return reloadOnChange_; }
  void setReloadOnChange(bool on);
  void setMemorySource(std::shared_ptr<MemorySource> source);

  // --- pure pieces (tests, MCP) ---
  static ConsoleLogView buildLogView(const QList<ConsoleLogEntry>& entries, const ConsoleLogFilter& filter);
  static QList<ConsoleLogEntry> entriesOf(const QList<plugins::LogLine>& log);
  static QString levelName(plugins::LogLevel level);
  static QString statusKey(const plugins::ConsoleSnapshot& s);  // running | loading | paused | error | stopped
  static QString timeText(qint64 ms);

  // The model now (the property is throttled). Used by the MCP tools and tests.
  QVariantMap buildModel();
  // The log of a plugin as it is now, with `filter` only (the person's Clear / Pause log do not hide lines from the MCP tools).
  ConsoleLogView logView(const QString& pluginId, const ConsoleLogFilter& filter = {}) const;
  QString selectedId() const { return selected_; }

  // --- state changes ---
  Q_INVOKABLE void selectPlugin(const QString& id);
  Q_INVOKABLE void setLevel(const QString& level, bool shown);
  Q_INVOKABLE void setLogPaused(bool paused);
  Q_INVOKABLE void clearLog(const QString& id);
  Q_INVOKABLE QString logText(const QString& id) const;  // visible lines, oldest first (Copy)
  Q_INVOKABLE void reloadSelected();
  // Memory viewer.
  Q_INVOKABLE void memorySelectRegion(int index);
  Q_INVOKABLE void memoryGoto(const QString& addressText);
  Q_INVOKABLE void memoryScroll(int rows);  // +/- rows of 16 bytes
  Q_INVOKABLE void memoryRefresh();
  Q_INVOKABLE void memorySearch(const QString& kind, const QString& text, const QString& type);  // kind "value" | "bytes"
  Q_INVOKABLE void memoryClearSearch();

  // The sources changed (log, status, regions ...): rebuilds the model within ~100 ms and notices reloads and pauses.
  void schedule();
  // A plugin's console data changed: emits reloaded() / pausedAfterError() once per event.
  void noteChanged(const QString& pluginId);
  // A new game or no game: forgets what was seen (toasts fire again), the selection stays if the plugin is still there.
  void resetSeen();
  // Tests and screenshots: shows `model` without sources; an empty map ends the preview.
  void setPreview(const QVariantMap& model);
  bool hasPreview() const { return hasPreview_; }

 signals:
  void modelChanged();
  void memoryChanged();
  void reloadOnChangeChanged();
  // Once per reload that ended running: the toast "<name> reloaded · <ms> ms".
  void reloaded(const QString& pluginId, const QString& name, qint64 ms);
  // Once per pause caused by errors: the toast "<name> paused after an error" with "Open console".
  void pausedAfterError(const QString& pluginId, const QString& name);

 private:
  struct Seen {
    int reloadCount = 0;
    bool paused = false;
  };
  QString nameOf(const QString& id) const;
  QStringList consoleIds() const;  // active plugins, then unpacked ones that do not run
  ConsoleLogFilter filterFor(const QString& id) const;
  QVariantMap permissionsOf(const plugins::PluginInfo& info, const plugins::ConsoleSnapshot& snap) const;
  void rebuildMemory();
  void viewMemory();
  void setMemoryError(const QString& text);

  Sources src_;
  QTimer timer_;
  QVariantMap model_, memory_;
  QString selected_;
  QSet<QString> levels_;                 // shown; all four at the start
  bool logPaused_ = false;
  QHash<QString, qint64> pausedAtSeq_;   // plugin id -> seq at which the log was paused
  QHash<QString, qint64> clearedAt_;     // plugin id -> lines up to this time are hidden
  QHash<QString, Seen> seen_;
  bool reloadOnChange_ = true;
  bool hasPreview_ = false;
  QVariantMap preview_;

  std::shared_ptr<MemorySource> mem_;
  int regionIndex_ = 0;
  quint64 base_ = 0;
  bool baseSet_ = false;
  QString memError_;
  QVariantList rows_;
  QVariantMap search_;
  static constexpr int kViewRows = 16;
  static constexpr int kRowBytes = 16;
};

}  // namespace uzume::ui
