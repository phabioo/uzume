#pragma once
// fb.run (permission run.state): the plugin thread's view of the run the game was started in. The plugin thread never
// touches the network: reads come from a snapshot, writes are handed to the UI thread. Implemented by
// RunSessionBridge (production) and by fakes in tests.
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <memory>

#include "plugin_value.h"

namespace uzume::plugins {

// Something the plugins subscribed to with fb.run.on: kind "event" (data = event), "state" (data = {user_id, plugin_id, state}),
// "status" (data = {status, previous}) or "participants" (data = {participants}).
struct RunNotification {
  QString kind;
  QJsonObject data;
};

// All methods are called from the plugin thread and must be thread-safe.
class RunBridge {
 public:
  virtual ~RunBridge() = default;

  // {id, name, status, me, settings, plugins:[{id, version}], participants:[{user_id, name, color, status, game_id}]}.
  virtual QJsonObject current() const = 0;
  // State of a participant for a plugin of the run (any plugin's state may be read); *found = false when there is none.
  virtual QJsonObject stateOf(const QString& userId, const QString& pluginId, bool* found) const = 0;
  virtual QJsonObject states(const QString& pluginId) const = 0;  // {user_id: state} for that plugin
  // Cached events with seq > afterSeq as {seq, time, user_id, user_name, plugin_id, source, type, text, payload, client_id}.
  virtual QJsonArray events(qint64 afterSeq, int limit) const = 0;

  // Writes: "" = accepted, otherwise why the write was refused (run not starting/active, not joined, too large ...).
  // Replaces the own state of the given plugin (the calling plugin; plugins write only their own state).
  virtual QString setState(const QString& pluginId, const QJsonObject& state) = 0;
  virtual QString postEvent(const QString& pluginId, const QString& source, const QString& type, const QString& text, const QJsonObject& payload,
                            QString* clientId) = 0;
  virtual QString setBoard(const QString& pluginId, const QJsonObject& tree) = 0;
  // An event the app writes itself, without a plugin (cheat.enabled, cheat.disabled, memory.write; run_rules_v1). "" = accepted.
  virtual QString postCoreEvent(const QString& type, const QString& text, const QJsonObject& payload) {
    Q_UNUSED(type) Q_UNUSED(text) Q_UNUSED(payload)
    return QStringLiteral("not supported");
  }

  virtual QList<RunNotification> takeNotifications() = 0;
};

// fb.run for plugins that run in the run view (Runs screen, outside a game): reads and notifications come from `inner`, current()
// has the extra field view = true, every write is refused with a message (no Lua error), like in a paused run.
class ReadOnlyRunBridge : public RunBridge {
 public:
  explicit ReadOnlyRunBridge(std::shared_ptr<RunBridge> inner) : inner_(std::move(inner)) {}
  QJsonObject current() const override {
    QJsonObject c = inner_->current();
    if (!c.isEmpty()) c.insert(QStringLiteral("view"), true);
    return c;
  }
  QJsonObject stateOf(const QString& userId, const QString& pluginId, bool* found) const override { return inner_->stateOf(userId, pluginId, found); }
  QJsonObject states(const QString& pluginId) const override { return inner_->states(pluginId); }
  QJsonArray events(qint64 afterSeq, int limit) const override { return inner_->events(afterSeq, limit); }
  QString setState(const QString&, const QJsonObject&) override { return refusal(); }
  QString postEvent(const QString&, const QString&, const QString&, const QString&, const QJsonObject&, QString*) override { return refusal(); }
  QString setBoard(const QString&, const QJsonObject&) override { return refusal(); }
  QString postCoreEvent(const QString&, const QString&, const QJsonObject&) override { return refusal(); }
  QList<RunNotification> takeNotifications() override { return inner_->takeNotifications(); }

 private:
  static QString refusal() { return QStringLiteral("read only outside the game"); }
  std::shared_ptr<RunBridge> inner_;
};

// Lua value <-> JSON. Tables whose keys are 1..n become arrays (an empty nested table too), others objects with string
// keys. Fails (err) for NaN/inf, non-string/non-integer keys and for too deep nesting.
Value valueFromJson(const QJsonValue& v, int depth = 0);
bool jsonFromValue(const Value& v, QJsonValue& out, QString& err, int depth = 0);
bool jsonObjectFromValue(const Value& v, QJsonObject& out, QString& err);  // the value must be a table; empty = {}

}  // namespace uzume::plugins
