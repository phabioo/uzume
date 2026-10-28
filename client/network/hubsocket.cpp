#include "hubsocket.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSslCertificate>
#include <QSslError>
#include <QWebSocket>
#include <algorithm>

namespace uzume {

namespace {
Q_LOGGING_CATEGORY(lcSocket, "uzume.hubsocket")

qint64 nowMs() {
  static QElapsedTimer t = [] {
    QElapsedTimer e;
    e.start();
    return e;
  }();
  return t.elapsed();
}

QString str(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }
QStringList strList(const QJsonValue& v) {
  QStringList l;
  for (const QJsonValue& e : v.toArray()) {
    if (e.isString()) {
      l.append(e.toString());
    }
  }
  return l;
}
}  // namespace

HubSocket::HubSocket(HubConnection* connection, QObject* parent) : QObject(parent), conn_(connection) {
  reconnectTimer_.setSingleShot(true);
  helloTimer_.setSingleShot(true);
  connect(&reconnectTimer_, &QTimer::timeout, this, &HubSocket::connectNow);
  connect(&pingTimer_, &QTimer::timeout, this, &HubSocket::onPingTimer);
  connect(&helloTimer_, &QTimer::timeout, this, [this]() {
    emit connectionError(QStringLiteral("hello_timeout"), QStringLiteral("No hello_ack from the server"));
    teardown();
    scheduleReconnect();
  });
  if (conn_) {
    connect(conn_, &HubConnection::stateChanged, this, [this](HubConnection::State s) {
      if (!wanted_) {
        return;
      }
      if (s == HubConnection::State::Connected) {
        if (state_ == State::Stopped || state_ == State::Backoff) {
          reconnectTimer_.stop();
          connectNow();
        }
      } else {
        teardown();
        setState(State::Backoff);  // waits for Connected again
      }
    });
  }
}

HubSocket::~HubSocket() { teardown(); }

void HubSocket::setState(State s) {
  if (state_ != s) {
    state_ = s;
    emit stateChanged(s);
  }
}

void HubSocket::start() {
  wanted_ = true;
  backoffMs_ = backoffInitialMs_;
  teardown();
  reconnectTimer_.stop();
  if (conn_ && conn_->state() == HubConnection::State::Connected) {
    connectNow();
  } else {
    setState(State::Backoff);
  }
}

void HubSocket::stop() {
  wanted_ = false;
  queued_.clear();
  queuedPresence_.reset();
  reconnectTimer_.stop();
  teardown();
  setState(State::Stopped);
}

void HubSocket::teardown() {
  ++generation_;
  pingTimer_.stop();
  helloTimer_.stop();
  if (ws_) {
    ws_->disconnect(this);
    ws_->abort();
    ws_.release()->deleteLater();
  }
}

void HubSocket::scheduleReconnect() {
  if (!wanted_) {
    return;
  }
  setState(State::Backoff);
  const int delay = backoffMs_;
  backoffMs_ = std::min(backoffMs_ * 2, backoffMaxMs_);
  qCDebug(lcSocket) << "Reconnect in" << delay << "ms";
  reconnectTimer_.start(delay);
}

void HubSocket::connectNow() {
  if (!wanted_ || !conn_) {
    return;
  }
  if (conn_->state() != HubConnection::State::Connected) {
    setState(State::Backoff);  // HubConnection::stateChanged restarts us
    return;
  }
  const QNetworkRequest req = conn_->webSocketRequest(QStringLiteral("/ws"));
  if (req.url().isEmpty()) {
    scheduleReconnect();
    return;
  }
  teardown();
  certMismatch_ = false;
  reachedServer_ = false;
  ws_ = std::make_unique<QWebSocket>(QString(), QWebSocketProtocol::VersionLatest, this);
  wssConnection_ = req.url().scheme() == QLatin1String("wss");
  if (wssConnection_) {
    ws_->setSslConfiguration(req.sslConfiguration());
  }
  QWebSocket* ws = ws_.get();
  connect(ws, &QWebSocket::sslErrors, this, &HubSocket::onSslErrors);
  connect(ws, &QWebSocket::connected, this, &HubSocket::onConnected);
  connect(ws, &QWebSocket::textMessageReceived, this, &HubSocket::onTextMessage);
  connect(ws, &QWebSocket::pong, this, [this]() { lastTrafficMs_ = nowMs(); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(ws, &QWebSocket::errorOccurred, this, [this]() { onSocketError(); });
#else
  connect(ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this, [this]() { onSocketError(); });
#endif
  connect(ws, &QWebSocket::disconnected, this, [this]() { onSocketError(); });
  setState(State::Connecting);
  lastTrafficMs_ = nowMs();
  ws->open(req);
}

void HubSocket::onSslErrors(const QList<QSslError>& errors) {
  reachedServer_ = true;  // TLS got as far as the certificate
  QSslCertificate leaf = ws_ ? ws_->sslConfiguration().peerCertificate() : QSslCertificate();
  if (leaf.isNull() && !errors.isEmpty()) {
    leaf = errors.first().certificate();
  }
  const QString pin = conn_ ? conn_->pinnedFingerprint() : QString();
  if (!pin.isEmpty() && HubHttp::fingerprintsEqual(HubHttp::fingerprint(leaf), pin)) {
    ws_->ignoreSslErrors();
  } else {
    certMismatch_ = true;  // handshake fails; never silently accepted
  }
}

void HubSocket::onSocketError() {
  if (!ws_ || state_ == State::Stopped) {
    return;
  }
  const QString msg = ws_->errorString();
  if (state_ == State::Open || state_ == State::Connecting) {
    if (certMismatch_) {
      emit connectionError(QStringLiteral("certificate_changed"), msg);
    } else if (msg.contains(QLatin1String("401"))) {
      upgradeRejects_ = 0;
      emit connectionError(QStringLiteral("unauthorized"), msg);
      if (conn_) {
        conn_->noteUnauthorized();  // renews the token before the next attempt
      }
    } else if (state_ == State::Connecting && (reachedServer_ || !ws_->peerAddress().isNull())) {
      // The server was reached but the WebSocket upgrade did not complete. The error text is not reliable across
      // Qt versions/platforms (a 401 does not always say "401"), so do not match on it: renew the token once; if
      // the upgrade is rejected again with a fresh token, report it as unauthorized.
      ++upgradeRejects_;
      if (upgradeRejects_ >= 2) {
        emit connectionError(QStringLiteral("unauthorized"), msg);
      } else {
        emit connectionError(QStringLiteral("unreachable"), msg);
      }
      if (conn_) {
        conn_->noteUnauthorized();
      }
    } else {
      upgradeRejects_ = 0;
      emit connectionError(QStringLiteral("unreachable"), msg);
    }
  }
  const bool wasOpen = state_ == State::Open;
  teardown();
  if (wasOpen) {
    backoffMs_ = backoffInitialMs_;
  }
  scheduleReconnect();
}

void HubSocket::onConnected() {
  lastTrafficMs_ = nowMs();
  // Pin check on every WebSocket connection (PN-9), not only from sslErrors: a platform that trusts the certificate itself never
  // raises it. QWebSocket exposes the peer certificate only on some Qt versions; without one the sslErrors path stays the guard.
  if (ws_ && wssConnection_ && conn_) {
    const QSslCertificate peer = ws_->sslConfiguration().peerCertificate();
    if (!peer.isNull() && !HubHttp::peerAccepted(HubHttp::fingerprint(peer), conn_->pinnedFingerprint(), false)) {
      certMismatch_ = true;
      onSocketError();  // reports certificate_changed, tears down, reconnects with backoff
      return;
    }
  }
  const QString deviceId = conn_ ? conn_->deviceId() : QString();
  QJsonObject hello{{QStringLiteral("protocol_version"), kProtocolVersion}, {QStringLiteral("device_id"), deviceId}};
  if (helloRuns_) {
    const QJsonArray runs = helloRuns_();  // runs_v1: last seen seq per run (catch-up after hello_ack)
    if (!runs.isEmpty()) hello.insert(QStringLiteral("runs"), runs);
  }
  send(QStringLiteral("hello"), hello);
  helloTimer_.start(10000);
}

void HubSocket::onPingTimer() {
  if (state_ != State::Open || !ws_) {
    return;
  }
  if (nowMs() - lastTrafficMs_ > 2.5 * pingIntervalMs_) {
    emit connectionError(QStringLiteral("timeout"), QStringLiteral("No answer to WebSocket ping"));
    teardown();
    scheduleReconnect();
    return;
  }
  ws_->ping();
}

void HubSocket::send(const QString& type, const QJsonObject& payload) {
  if (!ws_ || (state_ != State::Open && state_ != State::Connecting)) {
    return;
  }
  const QJsonObject env{{QStringLiteral("type"), type}, {QStringLiteral("payload"), payload}};
  ws_->sendTextMessage(QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)));
}

void HubSocket::queueOrSend(const QString& type, const QJsonObject& payload) {
  if (state_ == State::Open) {
    send(type, payload);
    return;
  }
  if (!wanted_) {
    return;
  }
  if (type == QLatin1String("presence_update")) {  // a state, not an event: only the latest one is worth sending
    queuedPresence_ = Queued{type, payload, nowMs()};
    return;
  }
  queued_.append({type, payload, nowMs()});
  while (queued_.size() > kMaxQueued) {
    queued_.removeFirst();
  }
}

void HubSocket::flushQueued() {
  QList<Queued> pending = std::exchange(queued_, {});
  if (queuedPresence_) pending.append(*std::exchange(queuedPresence_, std::nullopt));
  const qint64 now = nowMs();
  for (const Queued& q : pending) {
    if (now - q.atMs <= kQueuedMaxAgeMs) {
      send(q.type, q.payload);
    }
  }
}

void HubSocket::sendSignal(const SessionSignal& s) { queueOrSend(QStringLiteral("signal"), s.toPayload()); }

void HubSocket::sendPresence(const QString& state, const QString& gameId, const QString& runId) {
  QJsonObject p{{QStringLiteral("state"), state}};
  if (!gameId.isEmpty()) {
    p.insert(QStringLiteral("game_id"), gameId);
  }
  if (!runId.isEmpty() && state == QLatin1String("in_game")) {
    p.insert(QStringLiteral("run_id"), runId);
  }
  queueOrSend(QStringLiteral("presence_update"), p);
}

void HubSocket::sendReadyCheckStart(const QString& scope, const QString& id) {
  send(QStringLiteral("ready_check_start"), {{QStringLiteral("scope"), scope}, {QStringLiteral("id"), id}});
}
void HubSocket::sendReadyCheckAnswer(const QString& checkId, bool ready) {
  send(QStringLiteral("ready_check_answer"), {{QStringLiteral("check_id"), checkId}, {QStringLiteral("ready"), ready}});
}
void HubSocket::sendReadyCheckCancel(const QString& checkId) { send(QStringLiteral("ready_check_cancel"), {{QStringLiteral("check_id"), checkId}}); }
void HubSocket::sendPauseRequest(const QString& scope, const QString& id, bool paused) {
  send(QStringLiteral("pause_request"), {{QStringLiteral("scope"), scope}, {QStringLiteral("id"), id}, {QStringLiteral("paused"), paused}});
}
bool HubSocket::sendChat(const QString& scope, const QString& id, const QString& clientId, const QString& text) {
  if (state_ != State::Open) return false;
  send(QStringLiteral("chat_send"),
       {{QStringLiteral("scope"), scope}, {QStringLiteral("id"), id}, {QStringLiteral("client_id"), clientId}, {QStringLiteral("text"), text}});
  return true;
}

void HubSocket::sendVoiceJoin(const QString& scope, const QString& id, const QString& mic) {
  send(QStringLiteral("voice_join"), {{QStringLiteral("scope"), scope}, {QStringLiteral("id"), id}, {QStringLiteral("mic"), mic}});
}
void HubSocket::sendVoiceLeave(const QString& scope, const QString& id) {
  send(QStringLiteral("voice_leave"), {{QStringLiteral("scope"), scope}, {QStringLiteral("id"), id}});
}
void HubSocket::sendVoiceState(const QString& scope, const QString& id, const QString& mic) {
  send(QStringLiteral("voice_state"), {{QStringLiteral("scope"), scope}, {QStringLiteral("id"), id}, {QStringLiteral("mic"), mic}});
}
void HubSocket::sendVoiceMute(const QString& id, const QString& userId, bool muted) {
  send(QStringLiteral("voice_mute"),
       {{QStringLiteral("scope"), QStringLiteral("session")}, {QStringLiteral("id"), id}, {QStringLiteral("user_id"), userId}, {QStringLiteral("muted"), muted}});
}
void HubSocket::sendVoiceSignal(const QString& scope, const QString& id, const QString& toDeviceId, const QJsonObject& body) {
  QJsonObject p = body;
  p.insert(QStringLiteral("scope"), scope);
  p.insert(QStringLiteral("id"), id);
  p.insert(QStringLiteral("to_device_id"), toDeviceId);
  send(QStringLiteral("voice_signal"), p);
}

void HubSocket::onTextMessage(const QString& text) {
  lastTrafficMs_ = nowMs();
  QJsonParseError pe;
  const QJsonObject env = QJsonDocument::fromJson(text.toUtf8(), &pe).object();
  if (pe.error != QJsonParseError::NoError) {
    return;
  }
  const QString type = str(env, "type");
  const QJsonObject p = env.value(QStringLiteral("payload")).toObject();
  if (type == QLatin1String("hello_ack")) {
    helloTimer_.stop();
    hello_ = {p.value(QStringLiteral("protocol_version")).toInt(), str(p, "hub_version"), strList(p.value(QStringLiteral("features"))),
              strList(p.value(QStringLiteral("ice_servers"))), parseTurnServers(p.value(QStringLiteral("turn_servers")))};
    backoffMs_ = backoffInitialMs_;
    upgradeRejects_ = 0;
    setState(State::Open);
    pingTimer_.start(pingIntervalMs_);
    flushQueued();
    emit helloAcked(hello_);
  } else if (type == QLatin1String("ice_update")) {
    // ice_update_v1: replaces the stored ICE/TURN config; connections created from now on use it, running ones stay as they are.
    hello_.iceServers = strList(p.value(QStringLiteral("ice_servers")));
    hello_.turnServers = parseTurnServers(p.value(QStringLiteral("turn_servers")));
    qCInfo(lcSocket).noquote() << QStringLiteral("ICE servers updated: %1 stun, %2 turn").arg(hello_.iceServers.size()).arg(hello_.turnServers.size());
    emit iceUpdated(hello_);
  } else if (type == QLatin1String("presence_update")) {
    emit presenceUpdated({str(p, "user_id"), str(p, "device_id"), str(p, "state"), str(p, "game_id"), str(p, "display_name"), str(p, "game_title"),
                          str(p, "system"), str(p, "session_id")});
  } else if (type == QLatin1String("session_update") || type == QLatin1String("session_invite")) {
    if (const auto s = parseSession(p.value(QStringLiteral("session")).toObject())) {
      if (type == QLatin1String("session_update")) {
        emit sessionUpdated(*s);
      } else {
        emit sessionInvited(*s);
      }
    }
  } else if (type == QLatin1String("session_ended")) {
    emit sessionEnded({str(p, "session_id"), str(p, "reason")});
  } else if (type == QLatin1String("viewer_joined")) {
    emit viewerJoined({str(p, "session_id"), str(p, "viewer_id"), str(p, "display_name"), str(p, "device_name"),
                       parseTurnServers(p.value(QStringLiteral("turn_servers")))});
  } else if (type == QLatin1String("viewer_left")) {
    emit viewerLeft({str(p, "session_id"), str(p, "viewer_id"), str(p, "reason")});
  } else if (type == QLatin1String("signal")) {
    if (const auto s = parseSignal(p)) {
      emit signalReceived(*s);
    }
  } else if (type == QLatin1String("save_updated")) {
    if (const auto u = parseSaveUpdate(p)) {
      emit saveUpdated(*u);
    }
  } else if (type == QLatin1String("run_updated")) {
    emit runUpdated(p.value(QStringLiteral("run")).toObject());
  } else if (type == QLatin1String("run_state")) {
    emit runStateReceived(p);
  } else if (type == QLatin1String("run_event")) {
    emit runEventReceived(p);
  } else if (type == QLatin1String("ready_check")) {
    ReadyCheckMessage m{str(p, "check_id"), str(p, "scope"), str(p, "id"), str(p, "by"), str(p, "status"), 0, {}};
    const QDateTime when = QDateTime::fromString(str(p, "expires_at"), Qt::ISODate);
    m.expiresAtMs = when.isValid() ? when.toMSecsSinceEpoch() : 0;
    for (const QJsonValue& v : p.value(QStringLiteral("members")).toArray()) {
      const QJsonObject o = v.toObject();
      m.members.append({str(o, "user_id"), str(o, "state")});
    }
    emit readyCheckReceived(m);
  } else if (type == QLatin1String("pause_state")) {
    emit pauseStateReceived({str(p, "scope"), str(p, "id"), p.value(QStringLiteral("paused")).toBool(), str(p, "by")});
  } else if (type == QLatin1String("voice_room")) {
    VoiceRoomMessage m;
    m.scope = str(p, "scope");
    m.id = str(p, "id");
    for (const QJsonValue& v : p.value(QStringLiteral("members")).toArray()) {
      const QJsonObject o = v.toObject();
      m.members.append({str(o, "user_id"), str(o, "device_id"), str(o, "mic"), o.value(QStringLiteral("muted_by_host")).toBool()});
    }
    emit voiceRoomReceived(m);
  } else if (type == QLatin1String("voice_signal")) {
    emit voiceSignalReceived(str(p, "scope"), str(p, "id"), str(p, "from_device_id"), p);
  } else if (type == QLatin1String("chat_message")) {
    emit chatReceived(parseChatMessage(p));
  } else if (type == QLatin1String("error")) {
    emit hubError(str(p, "code"), str(p, "message"));
  } else {
    qCDebug(lcSocket) << "Ignoring unknown message type" << type;
  }
}

}  // namespace uzume
