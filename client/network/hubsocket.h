#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "chattypes.h"
#include "hubconnection.h"
#include "saveapi.h"
#include "sessiontypes.h"

class QWebSocket;
class QSslError;

namespace uzume {

// together_v1 (ADR 0033 D3): ready check and shared pause, scope "run" | "session". `by` is a user id.
struct ReadyCheckMember {
  QString userId;
  QString state;  // pending | ready | not_ready
};
struct ReadyCheckMessage {
  QString checkId;
  QString scope;
  QString id;
  QString by;
  QString status;  // open | all_ready | cancelled | expired
  qint64 expiresAtMs = 0;  // epoch ms, 0 when unknown
  QList<ReadyCheckMember> members;
};
struct PauseStateMessage {
  QString scope;
  QString id;
  bool paused = false;
  QString by;  // user id, "" when the server cleared the pause
};

// Voice chat (ADR 0033 D7): one member (device) of a voice room. `mic` is on | off | ptt.
struct VoiceMember {
  QString userId;
  QString deviceId;
  QString mic;
  bool mutedByHost = false;
};
struct VoiceRoomMessage {
  QString scope;
  QString id;
  QList<VoiceMember> members;  // in join order; empty = the room is closed or I was removed
};

// WSS client of the Hub (`GET /api/v1/ws`, ADR 0006 D3): Bearer upgrade with the same leaf pinning as HubHttp,
// `hello` after connect, typed signals for every Hub message, WebSocket ping every 20 s, automatic reconnect
// with backoff (the token is renewed through HubConnection when the Hub rejects it).
// A running media path does not depend on this socket (Hub failure: the Session lives on until the Hub ends it).
class HubSocket : public QObject {
  Q_OBJECT
 public:
  enum class State { Stopped, Connecting, Open, Backoff };
  Q_ENUM(State)

  explicit HubSocket(HubConnection* connection, QObject* parent = nullptr);
  ~HubSocket() override;

  // Starts (or restarts) the connection; reconnects until stop(). Waits for HubConnection::Connected if needed.
  void start();
  void stop();
  State state() const { return state_; }
  bool isOpen() const { return state_ == State::Open; }
  const HelloAck& helloAck() const { return hello_; }

  // runs_v1: provider of the `runs` array of the next hello ([{run_id, last_seq}]); empty array = field omitted.
  void setHelloRunsProvider(std::function<QJsonArray()> provider) { helloRuns_ = std::move(provider); }

  void sendSignal(const SessionSignal& s);
  // `runId` (Uzume 0.19) only with state "in_game": the game was started as a run game, the server counts the play time for the run.
  void sendPresence(const QString& state, const QString& gameId = {}, const QString& runId = {});

  // together_v1 (ADR 0033 D3). Sent now or dropped: a late ready check or pause is worse than none.
  void sendReadyCheckStart(const QString& scope, const QString& id);
  void sendReadyCheckAnswer(const QString& checkId, bool ready);
  void sendReadyCheckCancel(const QString& checkId);
  void sendPauseRequest(const QString& scope, const QString& id, bool paused);
  // Chat (ADR 0033 D4): false = not sent because the link is not open (the caller keeps the text).
  bool sendChat(const QString& scope, const QString& id, const QString& clientId, const QString& text);

  // Voice chat (ADR 0033 D7). Sent now or dropped (the VoiceController rejoins after a reconnect).
  void sendVoiceJoin(const QString& scope, const QString& id, const QString& mic);
  void sendVoiceLeave(const QString& scope, const QString& id);
  void sendVoiceState(const QString& scope, const QString& id, const QString& mic);
  void sendVoiceMute(const QString& id, const QString& userId, bool muted);  // Session owner only
  // `body` carries kind and the SDP / candidate fields; scope, id and to_device_id are added.
  void sendVoiceSignal(const QString& scope, const QString& id, const QString& toDeviceId, const QJsonObject& body);

  // Timing (tests): ping interval, backoff start/max, hello_ack timeout.
  void setPingIntervalMs(int ms) { pingIntervalMs_ = ms; }
  void setBackoffMs(int initial, int max) {
    backoffInitialMs_ = initial;
    backoffMaxMs_ = max;
  }

 signals:
  void stateChanged(uzume::HubSocket::State state);
  void helloAcked(const uzume::HelloAck& ack);  // connection usable
  void iceUpdated(const uzume::HelloAck& ack);  // `ice_update` applied: ack.iceServers / ack.turnServers are the new config
  void presenceUpdated(const uzume::PresenceUpdate& update);
  void sessionUpdated(const uzume::SessionInfo& session);
  void sessionInvited(const uzume::SessionInfo& session);
  void sessionEnded(const uzume::SessionEnded& ended);
  void viewerJoined(const uzume::ViewerJoined& viewer);
  void viewerLeft(const uzume::ViewerLeft& viewer);
  void signalReceived(const uzume::SessionSignal& signal);
  void saveUpdated(const uzume::SaveUpdate& update);  // saves_v2: a slot checkpoint changed on another device
  // runs_v1: raw payloads, parsed by the RunClient.
  void runUpdated(const QJsonObject& run);
  void runStateReceived(const QJsonObject& payload);  // {run_id, user_id, state_rev, state}
  void runEventReceived(const QJsonObject& payload);  // {run_id, event}
  void readyCheckReceived(const uzume::ReadyCheckMessage& check);
  void pauseStateReceived(const uzume::PauseStateMessage& state);
  void chatReceived(const uzume::ChatMessage& message);  // `chat_message`, also the echo of my own
  void voiceRoomReceived(const uzume::VoiceRoomMessage& room);
  void voiceSignalReceived(const QString& scope, const QString& id, const QString& fromDeviceId, const QJsonObject& body);  // kind, sdp, candidate, mid
  void hubError(const QString& code, const QString& message);  // `error` message of the Hub
  void connectionError(const QString& code, const QString& message);  // transport: unreachable, certificate_changed, ...

 private:
  void connectNow();
  void teardown();
  void scheduleReconnect();
  void setState(State s);
  void send(const QString& type, const QJsonObject& payload);
  void queueOrSend(const QString& type, const QJsonObject& payload);
  void flushQueued();
  void onTextMessage(const QString& text);
  void onSslErrors(const QList<QSslError>& errors);
  void onSocketError();
  void onConnected();
  void onPingTimer();

  QPointer<HubConnection> conn_;
  std::unique_ptr<QWebSocket> ws_;
  State state_ = State::Stopped;
  bool wanted_ = false;
  bool certMismatch_ = false;
  bool wssConnection_ = false;  // the current connection is wss
  HelloAck hello_;
  std::function<QJsonArray()> helloRuns_;
  QTimer reconnectTimer_;
  QTimer pingTimer_;
  QTimer helloTimer_;
  int pingIntervalMs_ = 20000;
  int backoffInitialMs_ = 1000;
  int backoffMaxMs_ = 30000;
  int backoffMs_ = 1000;
  bool reachedServer_ = false;  // TLS/TCP reached the Hub during this attempt
  int upgradeRejects_ = 0;      // consecutive upgrade failures after reaching the Hub
  qint64 lastTrafficMs_ = 0;
  quint64 generation_ = 0;
  // Signalling and presence produced while the socket is not open (reconnect, PN-15): bounded, flushed after the next hello_ack.
  struct Queued {
    QString type;
    QJsonObject payload;
    qint64 atMs = 0;
  };
  static constexpr int kMaxQueued = 64;
  static constexpr qint64 kQueuedMaxAgeMs = 30000;  // an older candidate or offer belongs to a connection attempt that gave up
  QList<Queued> queued_;            // signals, oldest first
  std::optional<Queued> queuedPresence_;  // presence is a state: only the latest one is kept
};

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::ReadyCheckMessage)
Q_DECLARE_METATYPE(uzume::PauseStateMessage)
Q_DECLARE_METATYPE(uzume::VoiceRoomMessage)
