#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "hubconnection.h"
#include "hubsocket.h"
#include "runtypes.h"
#include "sessiontypes.h"

namespace uzume {

// Result of one run REST call. `run` is set when the Hub answered with a Run object (already applied to the client).
struct RunResult {
  bool ok = false;
  int status = 0;  // HTTP status, 0 = no response
  bool networkError = false;
  QString errorCode;  // forbidden | conflict | run_not_active | state_conflict | too_large | rate_limited | ...
  QString message;
  std::optional<RunInfo> run;
  QJsonObject json;
};
using RunCallback = std::function<void(const RunResult&)>;

// Runs of the active Hub (feature runs_v1, 0.11 package 3 contract section 5): REST calls, the run_updated / run_state /
// run_event messages of the WSS, catch-up after reconnect (hello `runs` with the last seen seq per run, paging through
// GET .../events when the Hub sent nothing), and writes with the limits of the Hub:
//  - own state, per plugin of the run (PUT .../state/{plugin_id}): coalesced per plugin (at most 4 writes per second each),
//    a 409 state_conflict adopts the Hub's revision of that plugin and re-sends the latest local value;
//  - events: queued with a uuid client_id, sent in order and retried on network errors / 5xx / 429 (idempotent through
//    the client_id);
//  - board: coalesced to at most 1 write per second.
// A small per-run cache (last seen seq + recent events + states per user and plugin) lives in <dataDir>/runs/<hub id>/<user id>/<run id>.json.
// Writes are refused locally with a clear message when the run is not starting or active. Single-threaded (UI thread).
class RunClient : public QObject {
  Q_OBJECT
 public:
  // Per server and user data folder (<dataDir>/runs/<hub>/<user>); the run cache and the per-run plugin consent live in it. Empty without both ids.
  static QString scopeDir(const QString& dataDir, const QString& hubId, const QString& hubUserId);
  RunClient(HubConnection* connection, HubSocket* socket, const QString& dataDir, QObject* parent = nullptr);
  ~RunClient() override;

  bool supported() const;  // connected and the Hub advertises runs_v1
  bool supportsPluginless() const;  // runs_v2: the Hub accepts runs with no plugin
  bool supportsPluginUpdates() const;  // run_plugin_updates_v1: PUT /runs/{id}/plugins
  bool supportsRules() const;       // run_rules_v1: `rules` on create and PUT /runs/{id}/rules
  QString myUserId() const;

  QList<RunInfo> runs() const;  // newest update first
  std::optional<RunInfo> run(const QString& runId) const;
  // Runs I am invited to and have not answered (not finished/abandoned).
  int pendingInviteCount() const;
  bool loaded() const { return loaded_; }  // a list from the Hub arrived since the connection was made

  void refresh();  // GET /runs

  // --- REST (callbacks run on the UI thread, after the Run was applied) ---
  void createRun(const QString& name, const QList<RunPluginRef>& plugins, const QString& gameId, const QStringList& inviteUserIds,
                 const QJsonObject& settings, RunCallback cb, const QJsonObject& randomizer = QJsonObject());
  // Same with the core rules of the new run (sent only when the Hub advertises run_rules_v1; an older one starts with the defaults).
  void createRun(const QString& name, const QList<RunPluginRef>& plugins, const QString& gameId, const QStringList& inviteUserIds,
                 const QJsonObject& settings, const RunRules& rules, RunCallback cb, const QJsonObject& randomizer = QJsonObject());
  // PUT /runs/{id}/rules: replaces the whole rules object (creator or admin; 403 forbidden otherwise).
  void setRules(const QString& runId, const RunRules& rules, RunCallback cb);
  void setPlugins(const QString& runId, const QList<RunPluginRef>& plugins, RunCallback cb);  // run_plugin_updates_v1
  // variant: {kind: run|own, variant_id?} of a randomized run (variants_v1); empty = the Hub's default (the run variant).
  void joinRun(const QString& runId, const QString& gameId, RunCallback cb, const QJsonObject& variant = QJsonObject());
  void declineRun(const QString& runId, RunCallback cb);
  void leaveRun(const QString& runId, RunCallback cb);
  void setStatus(const QString& runId, const QString& status, RunCallback cb);
  void inviteUsers(const QString& runId, const QStringList& userIds, RunCallback cb);
  void loadUsers(std::function<void(bool ok, const QList<HubUser>& users, const QString& error)> cb);  // GET /users

  // --- writes of the running game (uz.run); return an error text ("" = accepted) ---
  // Own state of one plugin: replaces that plugin's whole state object. Refused when I have not joined, the plugin is
  // not part of the run or the run does not accept writes.
  QString setOwnState(const QString& runId, const QString& pluginId, const QJsonObject& state);
  QJsonObject ownState(const QString& runId, const QString& pluginId) const;  // the latest local value
  // State of any participant for a plugin (nullopt when there is none); my own unsent value counts.
  std::optional<QJsonObject> participantState(const QString& runId, const QString& userId, const QString& pluginId) const;
  // Queues an event (plugin_id, source "automatic" | "manual", type [a-z0-9_.-]{1,64}, text <= 280, payload <= 4 KiB).
  // *clientId: a uuid to use as the idempotency key (empty or null = a new one); on return the id that was used.
  QString postEvent(const QString& runId, const QString& pluginId, const QString& source, const QString& type, const QString& text,
                    const QJsonObject& payload, QString* clientId = nullptr);
  QString setBoard(const QString& runId, const QString& pluginId, const QJsonObject& tree);

  // Cached events with seq > afterSeq, ascending, at most `limit`.
  QList<RunEvent> events(const QString& runId, qint64 afterSeq, int limit) const;
  qint64 lastSeq(const QString& runId) const;  // highest contiguous seq seen
  // Pages through GET .../events until the cache is current (no-op when it already is).
  void catchUp(const QString& runId);
  int pendingEvents(const QString& runId) const;  // queued, not yet confirmed by the Hub

  // The `runs` array of the next hello: [{run_id, last_seq}] from the cache.
  QJsonArray helloRuns();

  // Timing (tests).
  void setStateIntervalMs(int ms) { stateIntervalMs_ = ms; }
  void setBoardIntervalMs(int ms) { boardIntervalMs_ = ms; }
  void setRetryBaseMs(int ms) { retryBaseMs_ = ms; }
  void setCatchUpDelayMs(int ms) { catchUpDelayMs_ = ms; }

 signals:
  void connectionReset();                              // in-flight requests are dropped without their callback (gen_ bumped)
  void runsChanged();                                  // the set of runs or any run changed
  void runChanged(const QString& runId);               // run_updated / REST answer applied
  void statusChanged(const QString& runId, const QString& status);
  void participantsChanged(const QString& runId);      // joins, leaves, invites, colours
  void stateChanged(const QString& runId, const QString& userId, const QString& pluginId);  // one plugin's state of a participant changed
  void eventAdded(const QString& runId, const uzume::RunEvent& event);
  // A queued write was dropped for good: kind = "state" | "event" | "board".
  void writeFailed(const QString& runId, const QString& kind, const QString& code, const QString& message);

 private:
  struct PendingEvent {
    QJsonObject body;
    int attempts = 0;
  };
  struct Board {
    QJsonObject tree;
    bool dirty = false;
    bool inFlight = false;
    qint64 lastSendMs = 0;
    int failures = 0;
  };
  struct OwnState {
    QJsonObject desired;
    bool dirty = false;
    bool inFlight = false;
    bool timerScheduled = false;
    qint64 lastSendMs = 0;
    int failures = 0;
  };
  struct RunData {
    RunInfo info;
    quint64 appliedSerial = 0;  // applySerial_ when this run was last updated (guards against stale list answers)
    qint64 lastSeq = 0;   // highest contiguous event seq seen
    qint64 knownSeq = 0;  // highest seq the Hub is known to have
    QList<RunEvent> events;
    QSet<QString> clientIds;
    bool paging = false;
    bool catchUpScheduled = false;
    bool cacheDirty = false;
    // own state by plugin id
    std::map<QString, OwnState> own;
    // events
    QList<PendingEvent> queue;
    bool eventInFlight = false;
    bool eventTimerScheduled = false;
    // boards by plugin
    std::map<QString, Board> boards;
  };

  void createRunBody(const QString& name, const QList<RunPluginRef>& plugins, const QString& gameId, const QStringList& inviteUserIds,
                     const QJsonObject& settings, const RunRules* rules, RunCallback cb, const QJsonObject& randomizer);
  quint64 applySerial_ = 0;
  RunData* data(const QString& runId);
  const RunData* data(const QString& runId) const;
  void onConnectionState(HubConnection::State s);
  void reset();
  void ensureCacheLoaded();
  QString cacheDir() const;
  void loadCache();
  void markCacheDirty(RunData& d);
  void saveCache();
  void applyRun(const RunInfo& incoming);
  void applyRunJson(const QJsonObject& o);
  void onRunState(const QJsonObject& payload);
  void onRunEvent(const QJsonObject& payload);
  bool acceptEvent(RunData& d, const RunEvent& e, bool contiguousPage);
  void noteSeq(RunData& d, qint64 seq);
  void scheduleCatchUp(const QString& runId);
  void pageEvents(const QString& runId, int pagesLeft);
  void flushState(const QString& runId, const QString& pluginId);
  void flushEvents(const QString& runId);
  void flushBoards(const QString& runId);
  int retryDelayMs(int failures) const;
  static qint64 nowMs();

  void send(const QByteArray& method, const QString& path, const std::optional<QJsonObject>& body, std::function<void(const HttpResult&)> cb);
  void sendRun(const QByteArray& method, const QString& path, const std::optional<QJsonObject>& body, RunCallback cb);
  static RunResult toResult(const HttpResult& r);

  QPointer<HubConnection> conn_;
  QPointer<HubSocket> socket_;
  QString dataDir_;
  QHash<QString, std::shared_ptr<RunData>> runs_;
  QHash<QString, qint64> cachedSeq_;      // from disk: run id -> last seq (runs not in memory yet)
  QHash<QString, QList<RunEvent>> cachedEvents_;
  QHash<QString, QJsonObject> cachedStates_;  // run id -> {user id: {plugin id: {state, state_rev}}}
  QString cacheOwner_;                    // "<hub>/<user>" the disk cache was loaded for
  bool loaded_ = false;
  quint64 gen_ = 0;
  QTimer saveTimer_;
  int stateIntervalMs_ = 250;
  int boardIntervalMs_ = 1000;
  int retryBaseMs_ = 1000;
  int catchUpDelayMs_ = 300;
};

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::RunEvent)
