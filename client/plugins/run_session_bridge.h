#pragma once
// RunSessionBridge: uz.run of the plugins for one run, backed by the RunClient. Lives on the UI thread; the plugin
// thread reads a mutex-protected snapshot and hands writes over with queued calls (it never blocks on the UI thread).
// Writes are checked against the snapshot first, so a plugin gets a clear refusal when the run is not starting/active.
#include <QHash>
#include <QObject>
#include <QSet>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "run_bridge.h"
#include "runclient.h"

namespace uzume::plugins {

class RunSessionBridge : public QObject, public RunBridge {
  Q_OBJECT
 public:
  // The bridge is released from whichever thread drops the last reference; it deletes itself on the UI thread.
  static std::shared_ptr<RunSessionBridge> create(RunClient* client, const QString& runId);

  QString runId() const { return runId_; }

  QJsonObject current() const override;
  QJsonObject stateOf(const QString& userId, const QString& pluginId, bool* found) const override;
  QJsonObject states(const QString& pluginId) const override;
  QJsonArray events(qint64 afterSeq, int limit) const override;
  QString setState(const QString& pluginId, const QJsonObject& state) override;
  QString postEvent(const QString& pluginId, const QString& source, const QString& type, const QString& text, const QJsonObject& payload,
                    QString* clientId) override;
  QString setBoard(const QString& pluginId, const QJsonObject& tree) override;
  QString postCoreEvent(const QString& type, const QString& text, const QJsonObject& payload) override;
  QList<RunNotification> takeNotifications() override;

 signals:
  void rulesChanged();  // the run's core rules changed (run_updated); UI thread

 private:
  RunSessionBridge(RunClient* client, const QString& runId);
  void rebuild();
  void note(const QString& kind, const QJsonObject& data);
  QString refusal(const QString& what) const;  // caller holds the lock
  void flushStates();
  void flushBoards();
  static QJsonObject eventJson(const RunEvent& e);

  QPointer<RunClient> client_;
  QString runId_;
  mutable std::mutex m_;
  QJsonObject current_;
  RunRules rules_;
  QJsonObject states_;  // {user_id: {plugin_id: state}}
  QList<QJsonObject> events_;  // ascending, at most 500
  QList<RunNotification> notes_;
  QString me_;
  QString status_;
  QString notifiedStatus_;  // UI thread only: the last status the plugins were told about
  bool joined_ = false;
  bool writable_ = false;
  QSet<QString> pluginIds_;
  std::map<QString, QJsonObject> pendingStates_;  // by plugin id: the latest own value not handed to the RunClient yet
  bool statesQueued_ = false;
  std::map<QString, QJsonObject> pendingBoards_;
  bool boardsQueued_ = false;
  std::atomic<int> eventsQueued_{0};
};

}  // namespace uzume::plugins
