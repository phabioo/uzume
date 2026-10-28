#pragma once
// Plugin UI v1, runtime side without Lua: validation/conversion of block trees (spec: docs/design/legacy/plugins.md building
// blocks) and the thread-safe store through which the plugin thread hands trees, titles, badges and toasts to the UI.
#include <QHash>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <mutex>

#include "plugin_value.h"

namespace uzume::plugins {

inline constexpr int kUiMaxDepth = 24;
inline constexpr int kUiMaxNodes = 2000;
inline constexpr int kUiMaxString = 2000;
inline constexpr size_t kUiMaxBytes = 512 * 1024;
inline constexpr int kUiReadDepth = 64;  // table nesting of the Lua value (blocks use two levels each)
inline constexpr int kUiMaxToastText = 300;
inline constexpr int kUiMaxTextareaBytes = 64 * 1024;  // value of a textarea block

// Validates `tree` (one block) and converts it to a QVariantMap: type, id, children (QVariantList), props. On failure
// returns false and sets `error` to e.g. `children[2].type: unknown block "foo"` (no "ui tree: " prefix).
bool convertUiTree(const Value& tree, QVariantMap& out, QString& error);
// Merges `props` into the node with `id` in `tree` (validated like set). The tree is unchanged on failure.
bool patchUiTree(QVariantMap& tree, const QString& id, const Value& props, QString& error);
// uz.ui.notify argument -> {tone, title, text, actions:[{label, action, value}]}.
bool convertToast(const Value& v, QVariantMap& out, QString& error);

// Plain-data conversions (UI action values <-> Lua values).
Value valueFromVariant(const QVariant& v, int depth = 0);
QVariant variantFromValue(const Value& v);
// Approximate size in bytes (limit check).
size_t estimateBytes(const QVariant& v);

struct UiPluginState {
  QHash<QString, QVariantMap> trees;  // slot -> tree
  QHash<QString, QString> titles;     // slot -> title override
  int badge = 0;
  QString dot;                        // "" | "news"
  bool paused = false;                // the plugin was paused; trees are kept so the UI can show them dimmed
};

// uz.ui.pick_file: a file dialog the person has to confirm. The plugin never gets the path, only the content (delivered by
// PluginHost::deliverFile to the callback).
struct PickRequest {
  QString pluginId;
  quint64 id = 0;
  QString title;
  QStringList filters;  // "*.cht"
  qint64 maxBytes = 0;
};

// Thread-safe: written on the plugin thread, read on the UI thread. Changes are only marked dirty; the plugin host
// flushes the dirty set at most about every 33 ms, which coalesces bursts to <= 30 updates per second and slot.
class UiStore {
 public:
  void reset(const QStringList& ids);
  void setTree(const QString& id, const QString& slot, const QVariantMap& tree);
  void clearSlot(const QString& id, const QString& slot);
  void setTitle(const QString& id, const QString& slot, const QString& title);  // empty = remove the override
  void setBadge(const QString& id, int n);
  void setDot(const QString& id, const QString& dot);
  void markPaused(const QString& id);
  void clearPlugin(const QString& id);  // reload / stop: trees, titles, badge, dot and paused flag are removed
  int addToast(const QString& id, QVariantMap toast);
  // uz.ui.open: a request to show the plugin's slot ("screen" or "panel"); queued like toasts, dirty "@open".
  void addOpenRequest(const QString& id, const QString& slot);
  // uz.ui.close: like addOpenRequest, dirty "@close".
  void addCloseRequest(const QString& id, const QString& slot);
  QList<QPair<QString, QString>> takeCloseRequests();
  // uz.ui.pick_file: queued like toasts, dirty "@pick".
  void addPickRequest(const PickRequest& r);
  QList<PickRequest> takePickRequests();

  UiPluginState state(const QString& id) const;
  QVariantMap tree(const QString& id, const QString& slot) const;
  // (plugin id, what) with what = slot name, "@meta" (title/badge/dot/paused), "@toast" or "@open".
  QList<QPair<QString, QString>> takeDirty();
  QList<QVariantMap> takeToasts();  // each has toastId and pluginId
  QList<QPair<QString, QString>> takeOpenRequests();  // (plugin id, slot), oldest first

 private:
  void dirty(const QString& id, const QString& what) { m_dirty.insert({id, what}); }

  mutable std::mutex m_mutex;
  QHash<QString, UiPluginState> m_state;
  QSet<QPair<QString, QString>> m_dirty;
  QList<QVariantMap> m_toasts;
  QList<QPair<QString, QString>> m_openRequests;
  QList<PickRequest> m_pickRequests;
  QList<QPair<QString, QString>> m_closeRequests;
  int m_nextToast = 1;
};

}  // namespace uzume::plugins
