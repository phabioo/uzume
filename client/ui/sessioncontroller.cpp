#include "sessioncontroller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <algorithm>
#include <limits>
#include <utility>

#include "screenlayout.h"

namespace uzume::ui {

namespace {
QString tr2(const char* text) { return QCoreApplication::translate("uzume::ui::SessionController", text); }

// "Direct" / "Relayed": the connection pill without the TURN detail (that stays in the diagnostics).
QString shortPath(const QString& pillText) {
  return pillText.startsWith(QLatin1String("Relayed")) ? tr2("Relayed") : pillText;
}

// What a person gets, as the People tab words it: "gets High" (Auto: the tier the adaptive bitrate is closest to), "picked Medium", or
// "picked Low · gets Medium" when three encoders were alive already and the host served the closest tier.
QString qualityPhrase(Quality pick, Quality tier, int kbps, bool known) {
  const Quality got = tier == Quality::Auto ? qualityForKbps(kbps) : tier;
  if (pick == Quality::Auto) return known ? tr2("gets %1").arg(qualityLabel(got)) : QString();
  if (known && got != pick) return tr2("picked %1 · gets %2").arg(qualityLabel(pick), qualityLabel(got));
  return tr2("picked %1").arg(qualityLabel(pick));
}
}  // namespace


SessionController::SessionController(HubConnection* conn, ProfileStore* profiles, GameSession* game, QObject* parent)
    : QObject(parent), conn_(conn), profiles_(profiles), game_(game), api_(conn), socket_(conn) {
  qRegisterMetaType<uzume::SessionInfo>();
  qRegisterMetaType<uzume::SessionSignal>();
  diagnostics_ = new DiagnosticsModel(this);
  mixer_ = game_->audioOutput().mixer();
  if (!mixer_) {
    mixer_ = &ownMixer_;
    game_->audioOutput().setMixer(mixer_, AudioMixer::Game);
  }
  connect(mixer_, &AudioMixer::routingChanged, this, &SessionController::audioMixChanged);
  connect(this, &SessionController::surfacesChanged, this, &SessionController::selectionChanged);
  connect(diagnostics_, &DiagnosticsModel::statesChanged, this, [this]() {
    if (diagnostics_->isOpen()) {
      refreshDiagnostics();
    }
    emit viewChanged();
  });

  connect(conn_, &HubConnection::stateChanged, this, &SessionController::onConnectionState);
  connect(&socket_, &HubSocket::stateChanged, this, [this](HubSocket::State st) {
    if (st != HubSocket::State::Open) roster_.clear();  // the server sends a fresh snapshot after the next hello_ack
    emit linkChanged();
  });
  connect(&socket_, &HubSocket::presenceUpdated, &roster_, &OnlineRoster::apply);
  connect(&socket_, &HubSocket::helloAcked, this, &SessionController::onHelloAck);
  connect(&socket_, &HubSocket::iceUpdated, this, [this](const HelloAck& ack) { host_.updateIceServers(ack.iceServers, ack.turnServers); });
  connect(&socket_, &HubSocket::sessionUpdated, this, &SessionController::onSessionUpdated);
  connect(&socket_, &HubSocket::sessionInvited, this, &SessionController::onSessionUpdated);
  connect(&socket_, &HubSocket::sessionEnded, this, &SessionController::onSessionEnded);
  connect(&socket_, &HubSocket::viewerJoined, this, &SessionController::onViewerJoined);
  connect(&socket_, &HubSocket::viewerLeft, this, &SessionController::onViewerLeft);
  connect(&socket_, &HubSocket::signalReceived, this, &SessionController::onSignal);
  connect(&socket_, &HubSocket::hubError, this, [](const QString&, const QString&) {});

  connect(&host_, &SessionHost::signalOut, &socket_, &HubSocket::sendSignal);
  connect(&host_, &SessionHost::errorOccurred, this, [this](const QString& m) { say(m, true); });
  connect(&host_, &SessionHost::viewerConnected, this, [this]() {
    emit shareChanged();
    // A paused game delivers no frameChanged, so the encoder would never start for the new viewer: push the current frame once.
    if (shared_ && hasLocalGame()) {
      host_.pushFrame(game_->frame());
    }
  });
  // GPU-direct encoding (ADR 0019) follows the encoder and the GPU input state; queued, so a bridge is never created or
  // dropped from inside the SessionHost call that changed the state.
  connect(&host_, &SessionHost::encoderRunningChanged, this, [this]() { updateGpuEncode(); }, Qt::QueuedConnection);
  connect(&host_, &SessionHost::gpuInputChanged, this, [this]() { updateGpuEncode(); }, Qt::QueuedConnection);
  // The game's state too: after a live-save restart the plan runs again once the core is back (Running).
  connect(game_, &GameSession::stateChanged, this, [this]() { updateGpuEncode(); }, Qt::QueuedConnection);
  connect(game_, &GameSession::stateChanged, this, [this]() { updateHostState(); });

  // Local game: frames and audio feed the host only while shared (SessionHost drops them otherwise).
  connect(game_, &GameSession::frameChanged, this, [this]() {
    if (!shared_) return;
    if (gpu_) {
      const auto st = gpu_->state();
      if (st != GpuEncodeBridge::State::Running) {
        host_.disableGpuInput(gpu_->reason(), st == GpuEncodeBridge::State::Failed);  // idempotent
      } else if (std::shared_ptr<AVFrame> f = gpu_->takeFrame()) {
        host_.pushGpuFrame(std::move(f), game_->frameLateNs());
      }
    }
    host_.pushFrame(game_->frame(), game_->frameLateNs());  // unchanged call; SessionHost skips it while GPU input is active
  });
  connect(game_, &GameSession::audioChunk, this, [this](const QByteArray& pcm, int rate) {
    if (shared_) {
      host_.pushAudio(pcm, rate);
    }
  });

  minuteTimer_.setInterval(30000);
  connect(&minuteTimer_, &QTimer::timeout, this, [this]() {
    if (!sessions_.isEmpty()) {
      emit sessionsChanged();
    }
  });
  minuteTimer_.start();
  diagTimer_.setInterval(500);
  connect(&diagTimer_, &QTimer::timeout, this, &SessionController::refreshDiagnostics);
  diagTimer_.start();
  audioTimer_.setInterval(10);
  connect(&audioTimer_, &QTimer::timeout, this, &SessionController::pumpAudio);
  messageTimer_.setSingleShot(true);
  messageTimer_.setInterval(10000);
  connect(&messageTimer_, &QTimer::timeout, this, &SessionController::dismissMessage);
  usersAge_.invalidate();
  usersLoaded_ = false;
}

SessionController::~SessionController() {
  socket_.stop();
  host_.close();
  for (const auto& r : remotes_) {
    r->viewer->close();
  }
}

// ---------------------------------------------------------------- settings

void SessionController::setPlayerSettings(PlayerSettings* settings) {
  settings_ = settings;
  diagnostics_->setSettings(settings);
  if (settings_ == nullptr) {
    return;
  }
  mixer_->setMixAll(settings_->audioMixAll());
  applyAudioRouting();
  if (!settings_->sessionVisibility().isEmpty()) {
    visibility_ = settings_->sessionVisibility();
    return;
  }
  // Migration: the value used to live in <data>/player-settings.json. The legacy file goes only after player.json holds it.
  const QString legacyPath = QDir(profiles_->baseDir()).filePath(QStringLiteral("player-settings.json"));
  QFile f(legacyPath);
  if (!f.open(QIODevice::ReadOnly)) {
    return;
  }
  const QString v = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("session_visibility")).toString();
  f.close();
  if (settings_->setSessionVisibility(v)) {
    visibility_ = v;
    QFile::remove(legacyPath);
  }
}

void SessionController::saveSettings() const {
  if (settings_ != nullptr) {
    settings_->setSessionVisibility(visibility_);
  }
}

// ---------------------------------------------------------------- link / lists

QString SessionController::hubLink() const {
  switch (socket_.state()) {
    case HubSocket::State::Open: return QStringLiteral("online");
    case HubSocket::State::Connecting: return QStringLiteral("connecting");
    default: return QStringLiteral("offline");
  }
}

bool SessionController::available() const {
  return conn_->state() == HubConnection::State::Connected && conn_->hubHasFeature(QString::fromLatin1(kSessionsFeature));
}

void SessionController::onConnectionState(HubConnection::State s) {
  using S = HubConnection::State;
  if (s == S::Connected) {
    if (available()) {
      socket_.start();
    }
  } else if (s != S::Identifying && s != S::Authenticating) {
    socket_.stop();
    closeShare(QString());
    cancelJoin();
    while (!remotes_.empty()) {
      closeRemote(remotes_.back()->info.sessionId, QString(), false);
    }
    sessions_.clear();
    sessionsLoaded_ = false;
    pendingWatch_.clear();
    roster_.clear();
    users_.clear();
    emit sessionsChanged();
  }
  emit linkChanged();
}

bool SessionController::canInvite(const QString& userId) const {
  if (!shared_ || userId.isEmpty()) return false;
  for (const SessionInviteEntry& i : own_.invites) {
    if (i.userId == userId && (i.state == QLatin1String("invited") || i.state == QLatin1String("joined"))) return false;
  }
  return true;
}

void SessionController::watchFromRoster(const QString& sessionId) {
  if (sessionId.isEmpty()) return;
  if (sessions_.contains(sessionId)) {
    watch(sessionId);
    return;
  }
  pendingWatch_ = sessionId;
  refreshSessions();
}

void SessionController::onHelloAck() {
  presence();
  refreshSessions();
}

void SessionController::presence() { socket_.sendPresence(hasLocalGame() ? QStringLiteral("in_game") : QStringLiteral("online"), gameId_, hasLocalGame() ? runId_ : QString()); }

QString SessionController::visibilityLabel(const QString& v) {
  if (v == QLatin1String("private")) return tr("Private");
  if (v == QLatin1String("invite_only")) return tr("Invite only");
  return tr("Everyone here");
}

QVariantList SessionController::sessions() const {
  QList<SessionInfo> list = sessions_.values();
  std::sort(list.begin(), list.end(), [](const SessionInfo& a, const SessionInfo& b) { return a.createdAt > b.createdAt; });
  QVariantList out;
  const QDateTime now = QDateTime::currentDateTimeUtc();
  for (const SessionInfo& s : list) {
    const qint64 mins = QDateTime::fromString(s.createdAt, Qt::ISODate).secsTo(now) / 60;
    const QString meta = s.invited ? tr("inviting you")
                                   : visibilityLabel(s.visibility) + QStringLiteral(" · ") +
                                         (mins < 1 ? tr("just started") : tr("for %1 min").arg(mins));
    out.append(QVariantMap{{QStringLiteral("sessionId"), s.sessionId},
                           {QStringLiteral("title"), s.owner.displayName + QStringLiteral(" · ") + s.gameTitle},
                           {QStringLiteral("who"), s.owner.displayName},
                           {QStringLiteral("game"), s.gameTitle},
                           {QStringLiteral("meta"), meta},
                           {QStringLiteral("invited"), s.invited},
                           {QStringLiteral("visibility"), s.visibility}});
  }
  return out;
}

void SessionController::refreshSessions() {
  if (!available()) {
    return;
  }
  const quint64 gen = sessionEventGen_;
  api_.list([this, gen](const SessionApiResult& r) {
    if (!r.ok() || gen != sessionEventGen_) {
      return;  // a live event arrived after the request was sent: the list may be older than it, drop the result
    }
    sessions_.clear();
    for (const SessionInfo& s : r.sessions) {
      if (!s.isOwner) {
        sessions_.insert(s.sessionId, s);
      }
    }
    sessionsLoaded_ = true;
    emit sessionsChanged();
    if (const QString id = std::exchange(pendingWatch_, QString()); !id.isEmpty()) watch(id);
  });
}

void SessionController::onSessionUpdated(const SessionInfo& s) {
  ++sessionEventGen_;
  if (shared_ && s.sessionId == own_.sessionId) {
    // This Player is the only writer of the own Session's visibility, and the updates carry no revision: one that
    // differs from the local choice was emitted before the Hub applied the latest PATCH (also after its answer).
    SessionInfo copy = s;
    copy.visibility = visibility_;
    applyOwnSession(copy);
    return;
  }
  if (s.isOwner) {
    return;
  }
  const bool wasKnown = sessions_.contains(s.sessionId);
  const bool wasInvited = wasKnown && sessions_.value(s.sessionId).invited;
  sessions_.insert(s.sessionId, s);
  if (sessionsLoaded_) {
    if (s.invited && !wasInvited) {
      emit notice(QStringLiteral("invite"), tr("%1 invited you to watch %2").arg(s.owner.displayName, s.gameTitle), tr("WATCH"), QStringLiteral("watch:") + s.sessionId);
    } else if (!s.invited && !wasKnown) {
      emit notice(QStringLiteral("session_started"), tr("%1 started a Session: %2").arg(s.owner.displayName, s.gameTitle), tr("WATCH"), QStringLiteral("watch:") + s.sessionId);
    }
  }
  if (Remote* r = remote(s.sessionId)) {
    r->info = s;
    emit surfacesChanged();
  }
  emit sessionsChanged();
}

void SessionController::onSessionEnded(const SessionEnded& e) {
  ++sessionEventGen_;
  if (shared_ && e.sessionId == own_.sessionId) {
    QString note = tr("The Session ended.");
    if (e.reason == QLatin1String("replaced")) note = tr("The Session was replaced by a newer one.");
    else if (e.reason == QLatin1String("owner_disconnected")) note = tr("The Session ended because the server lost this device.");
    else if (e.reason == QLatin1String("device_revoked")) note = tr("The Session ended: this device was revoked.");
    closeShare(note);
    return;
  }
  if (sessions_.remove(e.sessionId)) {
    emit sessionsChanged();
  }
  // Only the surface of that Session goes away; the others keep running.
  const QString note = e.reason == QLatin1String("no_longer_visible") ? tr("You can no longer watch this Session.")
                                                                      : tr("The Session ended.");
  if (remote(e.sessionId) != nullptr) {
    closeRemote(e.sessionId, note, false);
  } else if (joining_ && joinInfo_.sessionId == e.sessionId) {
    cancelJoin();
    say(note, false);
  }
}

// ---------------------------------------------------------------- own Session

int SessionController::viewerCount() const {
  return shared_ ? std::max(own_.viewerCount, static_cast<int>(own_.viewers.size())) : 0;
}

QString SessionController::participantsTitle() const {
  return visibility_ == QLatin1String("invite_only") ? tr("INVITED · %1").arg(participants().size())
                                                     : tr("WATCHING · %1").arg(participants().size());
}

QVariantList SessionController::participants() const {
  QVariantList out;
  if (!shared_) {
    return out;
  }
  QStringList watchingNames;
  for (const SessionViewerEntry& v : own_.viewers) {
    watchingNames << v.displayName;
    out.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("viewer")},
                           {QStringLiteral("role"), sessionRoleToken(SessionRole::Viewer)},
                           {QStringLiteral("id"), v.viewerId},
                           {QStringLiteral("name"), v.displayName},
                           {QStringLiteral("online"), true},
                           {QStringLiteral("status"), tr("watching")},
                           {QStringLiteral("connection"), linkByViewer_.value(v.viewerId).toMap().value(QStringLiteral("text"), tr("Connecting"))},
                           {QStringLiteral("connectionTone"), linkByViewer_.value(v.viewerId).toMap().value(QStringLiteral("tone"), QStringLiteral("neutral"))},
                           {QStringLiteral("connectionReason"), linkByViewer_.value(v.viewerId).toMap().value(QStringLiteral("reason"))},
                           {QStringLiteral("action"), tr("Remove")}});
  }
  if (visibility_ == QLatin1String("invite_only")) {
    for (const SessionInviteEntry& i : own_.invites) {
      if (i.state == QLatin1String("joined") || watchingNames.contains(i.displayName)) {
        continue;
      }
      QString status = i.state == QLatin1String("declined") ? tr("declined") : tr("invited");
      if (!i.online) {
        status += QStringLiteral(" · ") + tr("offline");
      }
      out.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("invite")},
                             {QStringLiteral("id"), i.userId},
                             {QStringLiteral("name"), i.displayName},
                             {QStringLiteral("online"), i.online},
                             {QStringLiteral("status"), status},
                             {QStringLiteral("action"), tr("Withdraw")}});
    }
  }
  return out;
}

QVariantList SessionController::sessionPeople(const QString& sessionId) const {
  QVariantList out;
  const auto person = [](const QString& userId, const QString& name, const QString& role, bool me, const QString& conn, const QString& tone, int color) {
    return QVariantMap{{QStringLiteral("userId"), userId}, {QStringLiteral("name"), name}, {QStringLiteral("role"), role}, {QStringLiteral("me"), me},
                       {QStringLiteral("roleKey"), role == QLatin1String("HOST") ? sessionRoleToken(SessionRole::Host) : (role == QLatin1String("VIEWER") ? sessionRoleToken(SessionRole::Viewer) : QString())},
                       {QStringLiteral("color"), color}, {QStringLiteral("connection"), conn}, {QStringLiteral("tone"), tone}};
  };
  if (shared_ && own_.sessionId == sessionId) {
    out.append(person(conn_->hubUserId(), own_.owner.displayName.isEmpty() ? conn_->userDisplayName() : own_.owner.displayName, QStringLiteral("HOST"), true, {}, {}, 1));
    int n = 1;
    for (const SessionViewerEntry& v : own_.viewers) {
      const QVariantMap link = linkByViewer_.value(v.viewerId).toMap();
      const QString line = link.value(QStringLiteral("line")).toString();
      out.append(person({}, v.displayName, QStringLiteral("VIEWER"), false, line.isEmpty() ? link.value(QStringLiteral("text")).toString() : line,
                        link.value(QStringLiteral("tone"), QStringLiteral("neutral")).toString(), 2 + (n++ % 7)));
    }
    return out;
  }
  if (const Remote* r = remote(sessionId)) {
    const QVariantMap link = surfaceLinks_.value(sessionId).toMap();
    QString conn = shortPath(link.value(QStringLiteral("text")).toString());
    if (!link.value(QStringLiteral("rtt")).toString().isEmpty()) conn += QStringLiteral(" · ") + link.value(QStringLiteral("rtt")).toString();
    out.append(person(r->info.owner.userId, r->info.owner.displayName, QStringLiteral("HOST"), false, conn, link.value(QStringLiteral("tone"), QStringLiteral("neutral")).toString(), 1));
    QVariantMap me = person(conn_->hubUserId(), conn_->userDisplayName(), QStringLiteral("VIEWER"), true, link.value(QStringLiteral("quality")).toString(), {}, 2);
    me.insert(QStringLiteral("qualityPick"), qualityToken(r->viewer->quality()));
    me.insert(QStringLiteral("sessionId"), sessionId);
    out.append(me);
    // Other viewers are not known to a viewer: only their number (the Hub counts them in viewerCount)
    for (int i = 1; i < r->info.viewerCount; ++i) out.append(person({}, tr("Another viewer"), QStringLiteral("VIEWER"), false, {}, {}, 3 + (i % 6)));
  }
  return out;
}

QString SessionController::role(const QString& sessionId) const {
  if (shared_ && own_.sessionId == sessionId) return sessionRoleToken(SessionRole::Host);
  return remote(sessionId) ? sessionRoleToken(SessionRole::Viewer) : QString();
}

QString SessionController::quality(const QString& sessionId) const {
  const Remote* r = remote(sessionId);
  return r ? qualityToken(r->viewer->quality()) : QString();
}

QString SessionController::defaultQuality() const { return settings_ ? settings_->viewerQuality() : QStringLiteral("auto"); }

void SessionController::setQuality(const QString& sessionId, const QString& pick) {
  const auto q = parseQualityToken(pick);
  Remote* r = remote(sessionId);
  if (!q || !r) return;  // viewers only: the host has no quality to pick
  r->viewer->setQuality(*q);
  if (settings_ && settings_->viewerQuality() != pick && settings_->setViewerQuality(pick)) emit qualityDefaultChanged();
  updateLinks();
}

QVariantList SessionController::viewerStates() const {
  QVariantList out;
  if (!shared_) return out;
  const QList<ViewerLinkStats> links = viewerLinks();
  for (const SessionViewerEntry& v : own_.viewers) {
    ViewerLinkStats l;
    for (const ViewerLinkStats& cand : links) {
      if (cand.viewerId == v.viewerId) l = cand;
    }
    const std::optional<double> rtt = l.latencyMs ? l.latencyMs : l.rttMs;
    out.append(QVariantMap{{QStringLiteral("viewer_id"), v.viewerId},
                           {QStringLiteral("name"), v.displayName},
                           {QStringLiteral("role"), sessionRoleToken(SessionRole::Viewer)},
                           {QStringLiteral("rtt_ms"), rtt ? QVariant(qRound(*rtt)) : QVariant()},
                           {QStringLiteral("pick"), qualityToken(l.pick)},
                           {QStringLiteral("tier"), qualityToken(l.tier)},
                           {QStringLiteral("kbps"), l.tierKbps},
                           {QStringLiteral("gets"), qualityLabel(l.tier == Quality::Auto ? qualityForKbps(l.tierKbps) : l.tier)},
                           {QStringLiteral("connection"), l.connectionType}});
  }
  return out;
}

void SessionController::applyOwnSession(const SessionInfo& s) {
  own_ = s;
  visibility_ = s.visibility;
  emit shareChanged();
}

// Taken when a REST request is SENT: a request overlapping a visibility PATCH (separate connection) may be served
// before the Hub applied it, so its answer is never trusted for the visibility (the sentinel never equals visGen_).
quint64 SessionController::requestVisToken() const {
  return visInFlight_ > 0 ? std::numeric_limits<quint64>::max() : visGen_;
}

// A REST answer to a request sent before the user changed the visibility again carries the older visibility:
// take everything else from it but keep the newer local choice.
void SessionController::applyOwnSession(const SessionInfo& s, quint64 requestVisGen) {
  if (requestVisGen == visGen_ && visInFlight_ == 0) {  // a PATCH still in flight: the Hub may not have applied it yet
    applyOwnSession(s);
    return;
  }
  SessionInfo copy = s;
  copy.visibility = visibility_;
  applyOwnSession(copy);
}

QString SessionController::errorText(const SessionApiResult& r, const QString& what) const {
  using K = SessionApiResult::Kind;
  switch (r.kind) {
    case K::Full: return tr("The Session is full (up to 4 viewers).");
    case K::Forbidden: return tr("You are not allowed to watch this Session.");
    case K::NotFound:
    case K::Ended: return tr("The Session has ended.");
    case K::CapabilityMissing: return tr("%1: this device has no usable H.264 codec.").arg(what);
    case K::Offline: return tr("%1: the server is not reachable.").arg(what);
    default: return tr("%1 failed (%2).").arg(what, r.errorCode.isEmpty() ? QString::number(r.status) : r.errorCode);
  }
}

void SessionController::shareSession() {
  if (shared_ || shareBusy_ || !hasLocalGame() || !socket_.isOpen()) {
    if (!socket_.isOpen()) {
      say(tr("The server is not reachable, so the Session cannot be shared yet."), true);
    }
    return;
  }
  shareBusy_ = true;
  const quint64 gen = ++shareGen_;
  emit shareChanged();
  api_.publish(gameId_, visibility_, [this, gen](const SessionApiResult& r) {
    if (gen != shareGen_ || !hasLocalGame() || !socket_.isOpen()) {
      // The attempt was cancelled (game quit, Hub lost, share closed) while the answer was in flight: do not adopt the
      // Session, end it so the Hub does not keep a live Session without a game.
      if (r.ok() && r.session) {
        api_.end(r.session->sessionId, [](const SessionApiResult&) {});
      }
      if (gen == shareGen_) {
        shareBusy_ = false;
        emit shareChanged();
      }
      return;
    }
    shareBusy_ = false;
    if (!r.ok() || !r.session) {
      pendingViewers_.clear();
      pendingHostSignals_.clear();
      say(errorText(r, tr("Sharing the Session")), true);
      emit shareChanged();
      return;
    }
    own_ = *r.session;
    shared_ = true;
    updateGpuEncode();  // share size = kShareEncodeMax; a bridge follows once the encoder runs
    saveSettings();
    host_.open(own_.sessionId, socket_.helloAck().iceServers, socket_.helloAck().turnServers);
    updateHostState();
    for (const ViewerJoined& v : std::exchange(pendingViewers_, {})) {
      host_.addViewer(v.viewerId, v.turnServers);
    }
    for (const SessionSignal& s : std::exchange(pendingHostSignals_, {})) {
      host_.handleSignal(s);
    }
    host_.pushFrame(game_->frame());
    emit shareChanged();
  });
}

// ADR 0019: the readback size limit and the GPU-direct bridge of the own Session follow one pure plan (planGpuEncode).
// The bridge is created once the encoder runs on a hardware-rendered game with GPU input allowed, kept while the share
// lasts (also while nobody watches: its CUDA context and registration stay) and dropped when GPU input is off, the
// game is not hardware rendered any more or the share closes. A live-save restart keeps it: GameSession re-applies the
// target to the new runner. While the core (re)starts, hardwareRendered() is false for a moment; an existing bridge
// counts as hardware rendered then, or a queued update in that window would drop it for good.
void SessionController::updateGpuEncode() {
  const bool hw = game_->hardwareRendered() || (gpu_ && game_->state() == GameSession::Starting);
  const SessionHost::GpuBackends backends = host_.gpuBackendsAllowed();
  const GpuEncodePlan p = planGpuEncode(shared_, hw, host_.encoderRunning(), backends.any(), host_.gpuInputActive());
  if (!p.keep && gpu_) {
    game_->setGpuEncodeTarget(nullptr);
    gpu_.reset();
  }
  if (p.create && !gpu_) {
    // The allowed backends are fixed for this bridge (ADR 0027 D6); the bridge picks one at its first attach, on the
    // emulation thread, from the GPU vendor. The display bridge is looked up there too (a live-save restart makes a new
    // one); the QPointer is only read while the emulation thread runs, which GameSession joins before it is destroyed.
    GpuEncodeOptions options;
    options.allowed = GpuEncodeAllowed{backends.cuda, backends.amf, backends.qsv};
    options.displayBridge = [game = QPointer<GameSession>(game_)]() { return game ? game->displayBridge() : nullptr; };
    gpu_ = std::make_shared<GpuEncodeBridge>(kShareEncodeMax, std::move(options));
    game_->setGpuEncodeTarget(gpu_);
  }
  if (gpu_) {
    gpu_->setWanted(p.wanted);
  }
  game_->setShareSize(p.shareSize);
}

void SessionController::closeShare(const QString& note) {
  const bool was = shared_ || shareBusy_;
  ++shareGen_;  // invalidates a publish answer that is still in flight
  host_.close();
  shared_ = false;
  updateGpuEncode();  // drops the bridge, share size = empty
  shareBusy_ = false;
  own_ = SessionInfo();
  visInFlight_ = 0;
  pendingViewers_.clear();
  pendingHostSignals_.clear();
  userResults_.clear();
  if (!note.isEmpty()) {
    say(note, false);
  }
  if (was) {
    emit shareChanged();
    emit userResultsChanged();
  }
}

void SessionController::stopSharing() {
  if (!shared_) {
    if (shareBusy_) {
      closeShare(QString());  // the late publish answer ends the Session it created
    }
    return;
  }
  const QString id = own_.sessionId;
  closeShare(QString());
  api_.end(id, [this](const SessionApiResult& r) {
    if (!r.ok() && r.kind != SessionApiResult::Kind::Ended && r.kind != SessionApiResult::Kind::NotFound) {
      say(errorText(r, tr("Ending the Session")), true);
    }
  });
}

void SessionController::setVisibility(const QString& v) {
  if (v != QLatin1String("private") && v != QLatin1String("hub_users") && v != QLatin1String("invite_only")) {
    return;
  }
  if (v == visibility_) {
    return;
  }
  const QString before = visibility_;
  visibility_ = v;
  const quint64 gen = ++visGen_;
  saveSettings();
  emit shareChanged();
  if (shared_) {
    ++visInFlight_;
    const QString sessionId = own_.sessionId;
    api_.setVisibility(sessionId, v, [this, before, gen, sessionId](const SessionApiResult& r) {
      if (!shared_ || own_.sessionId != sessionId) {  // stopped or replaced meanwhile: the answer belongs to an old Session
        return;
      }
      if (visInFlight_ > 0) {
        --visInFlight_;
      }
      if (r.ok() && r.session) {
        applyOwnSession(*r.session, gen);
      } else if (gen == visGen_) {
        visibility_ = before;
        saveSettings();
        say(errorText(r, tr("Changing the visibility")), true);
        emit shareChanged();
      }
    });
  }
}

void SessionController::searchUsers(const QString& text) {
  userQuery_ = text.trimmed();
  const auto filter = [this]() {
    userResults_.clear();
    userHint_.clear();
    if (!userQuery_.isEmpty()) {
      bool others = false;
      QStringList taken;
      for (const SessionInviteEntry& i : own_.invites) {
        taken << i.userId;
      }
      for (const HubUser& u : users_) {
        if (u.id != conn_->hubUserId()) {
          others = true;
        }
        if (u.id == conn_->hubUserId() || taken.contains(u.id) || !u.displayName.contains(userQuery_, Qt::CaseInsensitive)) {
          continue;
        }
        userResults_.append(QVariantMap{
            {QStringLiteral("id"), u.id},
            {QStringLiteral("name"), u.displayName},
            {QStringLiteral("online"), u.online},
            {QStringLiteral("hint"), u.online ? tr("online") : tr("offline · receives the invite as long as the Session is running")}});
        if (userResults_.size() >= 6) {
          break;
        }
      }
      if (userResults_.isEmpty() && usersLoaded_) {
        userHint_ = others ? tr("No matching users") : tr("Nobody else is here yet");
      }
    }
    emit userResultsChanged();
  };
  if (userQuery_.isEmpty()) {
    usersAge_.invalidate();  // a new search starts with a fresh user list
    filter();
    return;
  }
  if ((usersAge_.isValid() && usersAge_.elapsed() < 10000)) {
    filter();
    return;
  }
  api_.listUsers([this, filter](const SessionApiResult& r) {
    if (r.ok()) {
      users_ = r.users;
      usersLoaded_ = true;
      usersAge_.restart();
    }
    filter();
  });
}

void SessionController::invite(const QString& userId) {
  if (!shared_) {
    return;
  }
  const quint64 gen = requestVisToken();
  const QString sessionId = own_.sessionId;
  api_.invite(sessionId, userId, [this, gen, sessionId](const SessionApiResult& r) {
    if (r.ok() && r.session && shared_ && own_.sessionId == sessionId) {
      applyOwnSession(*r.session, gen);
      searchUsers(userQuery_);
    } else if (!r.ok()) {
      say(errorText(r, tr("Inviting")), true);
    }
  });
}

void SessionController::withdrawInvite(const QString& userId) {
  if (!shared_) {
    return;
  }
  const QString id = own_.sessionId;
  api_.withdrawInvite(id, userId, [this, id](const SessionApiResult& r) {
    if (!r.ok()) {
      say(errorText(r, tr("Withdrawing the invite")), true);
    } else if (shared_ && own_.sessionId == id) {
      const quint64 gen = requestVisToken();
      api_.get(id, [this, gen, id](const SessionApiResult& g) {
        if (g.ok() && g.session && shared_ && own_.sessionId == id) {
          applyOwnSession(*g.session, gen);
        }
      });
    }
  });
}

void SessionController::removeViewer(const QString& viewerId) {
  if (!shared_) {
    return;
  }
  host_.removeViewer(viewerId);  // media path is cut immediately
  api_.removeViewer(own_.sessionId, viewerId, [this](const SessionApiResult& r) {
    if (!r.ok() && r.kind != SessionApiResult::Kind::NotFound) {
      say(errorText(r, tr("Removing the viewer")), true);
    }
  });
}

void SessionController::onViewerJoined(const ViewerJoined& v) {
  if (shared_ && v.sessionId == own_.sessionId) {
    host_.addViewer(v.viewerId, v.turnServers);  // fresh credentials of this viewer_joined win over hello_ack
  } else if (shareBusy_) {
    pendingViewers_.append(v);
  }
}

void SessionController::onViewerLeft(const ViewerLeft& v) {
  if (shared_ && v.sessionId == own_.sessionId) {
    host_.removeViewer(v.viewerId);
    emit shareChanged();
    return;
  }
  const Remote* r = remote(v.sessionId);
  const bool joinHit = joining_ && joinInfo_.sessionId == v.sessionId;
  if ((r != nullptr && v.viewerId == r->viewerId) || joinHit) {
    QString note = tr("You left the Session.");
    if (v.reason == QLatin1String("removed")) note = tr("You were removed from the Session.");
    else if (v.reason == QLatin1String("revoked")) note = tr("You can no longer watch this Session.");
    else if (v.reason == QLatin1String("disconnected")) note = tr("The Session connection was lost.");
    if (r != nullptr && !joinHit) {
      closeRemote(v.sessionId, note, false);
    } else {
      cancelJoin();
      say(note, false);
    }
  }
}

void SessionController::onSignal(const SessionSignal& s) {
  if (shared_ && host_.hasViewer(s.viewerId)) {
    host_.handleSignal(s);
  } else if (Remote* r = remote(s.sessionId); r != nullptr && s.viewerId == r->viewerId) {
    r->viewer->handleSignal(s);
  } else if (joining_ && s.sessionId == joinInfo_.sessionId) {
    earlySignals_.append(s);  // the owner may offer before the join response arrives
  } else if (shareBusy_) {
    pendingHostSignals_.append(s);
  }
}

// ---------------------------------------------------------------- watching

SessionController::Remote* SessionController::remote(const QString& sessionId) const {
  for (const auto& r : remotes_) {
    if (r->info.sessionId == sessionId) {
      return r.get();
    }
  }
  return nullptr;
}

// ADR 0026 D3: viewers of the own Session learn whether the game is paused and by whom (the owner's display name).
void SessionController::updateHostState() {
  if (!shared_) return;
  host_.setHostState(game_->isPaused(), game_->isPaused() && !hostPauseBy_.isEmpty() ? hostPauseBy_ : own_.owner.displayName);
}

SessionViewer* SessionController::viewer(const QString& sessionId) {
  Remote* r = remote(sessionId);
  return r ? r->viewer : nullptr;
}

QImage SessionController::remoteFrame(const QString& sessionId) const {
  const Remote* r = remote(sessionId);
  return r ? r->frame : QImage();
}

quint64 SessionController::remoteFrameNumber(const QString& sessionId) const {
  const Remote* r = remote(sessionId);
  return r ? r->frameNr : 0;
}

// "Watch Session" / "Join" / "Add": every remote Session is its own viewer join and SessionViewer, up to four surfaces.
void SessionController::watch(const QString& sessionId) {
  if (joining_ || !sessions_.contains(sessionId) || remote(sessionId) != nullptr) {
    return;
  }
  if (surfaceCount() >= kMaxSurfaces) {
    say(tr("A multiview shows up to %1 Sessions at once. Remove one first.").arg(kMaxSurfaces), true);
    return;
  }
  if (!socket_.isOpen()) {
    say(tr("The server is not reachable, so the Session cannot be joined yet."), true);
    return;
  }
  joinInfo_ = sessions_.value(sessionId);
  joining_ = true;
  earlySignals_.clear();
  emit watchChanged();
  api_.join(sessionId, [this, sessionId](const SessionApiResult& r) {
    if (!joining_ || joinInfo_.sessionId != sessionId) {
      if (r.ok() && r.join) {  // ended while joining: leave again
        api_.removeViewer(sessionId, r.join->viewerId, [](const SessionApiResult&) {});
      }
      return;
    }
    joining_ = false;
    if (!r.ok() || !r.join) {
      earlySignals_.clear();
      say(errorText(r, tr("Joining the Session")), true);
      emit watchChanged();
      return;
    }
    auto rem = std::make_unique<Remote>();
    rem->info = joinInfo_;
    rem->viewerId = r.join->viewerId;
    rem->viewer = new SessionViewer(this);
    SessionViewer* v = rem->viewer;
    if (settings_) v->setQuality(parseQualityToken(settings_->viewerQuality()).value_or(Quality::Auto));  // this PC's default
    connect(v, &SessionViewer::signalOut, &socket_, &HubSocket::sendSignal);
    connect(v, &SessionViewer::frameReady, this, [this, sessionId](const QImage& img) {
      if (Remote* cur = remote(sessionId)) {
        cur->frame = img;
        ++cur->frameNr;
        emit remoteFrameChanged(sessionId);
        if (cur->frameNr == 1) emit surfacesChanged();  // the tile stops saying "loading"
      }
    });
    connect(v, &SessionViewer::hostStateChanged, this, [this, sessionId](bool paused, const QString& by) {
      if (Remote* cur = remote(sessionId)) {
        if (cur->paused == paused && cur->pausedBy == by) return;
        cur->paused = paused;
        cur->pausedBy = paused ? by : QString();
        cur->pausedAtMs = paused ? QDateTime::currentMSecsSinceEpoch() : 0;
        emit surfacesChanged();
      }
    });
    connect(v, &SessionViewer::closed, this, [this, sessionId]() {
      closeRemote(sessionId, tr("The connection to the Session was lost."), true);
    });
    connect(v, &SessionViewer::errorOccurred, this, [this](const QString& m) { say(m, true); });
    const bool first = remotes_.empty();
    rem->audio.setMixer(mixer_, AudioMixer::Tiles, rem->info.owner.userId);
    if (settings_ && !rem->info.owner.userId.isEmpty())
      mixer_->setPersonGain(AudioMixer::Tiles, rem->info.owner.userId, settings_->audioTileGain(rem->info.owner.userId) / 100.0);
    rem->audio.start(48000);
    remotes_.push_back(std::move(rem));
    order_.append(sessionId);
    v->open(sessionId, r.join->viewerId, r.join->iceServers.isEmpty() ? socket_.helloAck().iceServers : r.join->iceServers,
            r.join->turnServers.isEmpty() ? socket_.helloAck().turnServers : r.join->turnServers);
    tab_ = QStringLiteral("multiview");
    if (first) {
      audioClock_.restart();
      audioDueFrames_ = 0;
      audioTimer_.start();
    }
    for (const SessionSignal& s : std::exchange(earlySignals_, {})) {
      v->handleSignal(s);
    }
    applyAudioRouting();
    emit watchChanged();
    emit surfacesChanged();
    emit viewChanged();
  });
}

void SessionController::decline(const QString& sessionId) {
  api_.decline(sessionId, [this, sessionId](const SessionApiResult& r) {
    if (r.ok() || r.kind == SessionApiResult::Kind::NotFound || r.kind == SessionApiResult::Kind::Ended) {
      if (sessions_.remove(sessionId)) {
        emit sessionsChanged();
      }
    } else {
      say(errorText(r, tr("Declining")), true);
    }
  });
}

void SessionController::leaveWatch() {
  cancelJoin();
  while (!remotes_.empty()) {
    closeRemote(remotes_.back()->info.sessionId, QString(), true);
  }
}

void SessionController::removeSurface(const QString& sessionId) { closeRemote(sessionId, QString(), true); }

void SessionController::cancelJoin() {
  if (joining_) {
    joining_ = false;
    earlySignals_.clear();
    emit watchChanged();
  }
}

// Leaves one remote Session (and only that one): closes its PeerConnection and, with callHub, leaves via the Hub.
void SessionController::closeRemote(const QString& sessionId, const QString& note, bool callHub) {
  auto it = std::find_if(remotes_.begin(), remotes_.end(), [&](const auto& r) { return r->info.sessionId == sessionId; });
  if (it == remotes_.end()) {
    return;
  }
  std::unique_ptr<Remote> r = std::move(*it);
  remotes_.erase(it);
  if (callHub && !r->viewerId.isEmpty()) {
    api_.removeViewer(sessionId, r->viewerId, [](const SessionApiResult&) {});
  }
  r->viewer->disconnect(this);
  r->viewer->close();
  r->viewer->deleteLater();
  order_.removeAll(sessionId);
  fedFrames_.remove(sessionId);
  if (focusPref_ == sessionId) {  // the focused surface goes away: local game, else the first remaining surface
    focusPref_ = hasLocalGame() || order_.isEmpty() ? QStringLiteral("local") : order_.first();
  }
  if (remotes_.empty()) {
    audioTimer_.stop();
  }
  emit remoteFrameChanged(sessionId);
  applyAudioRouting();
  emit watchChanged();
  emit surfacesChanged();
  emit viewChanged();
  if (!note.isEmpty()) {
    say(note, false);
  }
}

// ---------------------------------------------------------------- views / audio

QString SessionController::effectiveTab() const {
  if (!hasLocalGame() && tab_ == QLatin1String("session")) {
    return QStringLiteral("multiview");
  }
  return tab_;
}

QString SessionController::tab() const { return effectiveTab(); }

void SessionController::setTab(const QString& t) {
  if ((t != QLatin1String("session") && t != QLatin1String("multiview")) || t == tab_) {
    return;
  }
  tab_ = t;
  emit viewChanged();
}

// Layouts (ADR 0026 D2, 3r-3): two or more surfaces -> PiP | Row | Grid 2 x 2. Row (columns) is for two or three surfaces,
// Grid 2 x 2 from four; below four Grid is disabled in the UI ("Grid 2×2 · from 4", gridAvailable) and never shows an empty
// slot. One surface is a single picture (nothing to choose).
QStringList SessionController::availableLayouts() const {
  if (surfaceCount() < 2) {
    return {};
  }
  return {QStringLiteral("pip"), QStringLiteral("row"), QStringLiteral("grid")};
}

QString SessionController::multiviewMode() const {
  const int n = surfaceCount();
  if (n < 2 || mode_ == QLatin1String("pip")) {
    return mode_;  // a single surface has no layout to choose; the choice is kept for the next surface
  }
  if (n < kMaxSurfaces) {
    return QStringLiteral("row");  // Grid is not available below four surfaces
  }
  return mode_ == QLatin1String("row") && rowForced_ ? QStringLiteral("row") : QStringLiteral("grid");
}

void SessionController::setMultiviewMode(const QString& in) {
  // The choice is kept as a preference (also from the default-multiview setting before any Session is shown);
  // multiviewMode() maps it to what the current surface count offers. "side" is the pre-0.14 name of "row".
  const QString m = in == QLatin1String("side") ? QStringLiteral("row") : in;
  if (m != QLatin1String("pip") && m != QLatin1String("row") && m != QLatin1String("grid")) {
    return;
  }
  const bool force = m == QLatin1String("row") && surfaceCount() >= kMaxSurfaces;
  if (m == mode_ && force == rowForced_) {
    return;
  }
  mode_ = m;
  rowForced_ = force;
  emit viewChanged();
}

QStringList SessionController::surfaceIds() const {
  QStringList ids;
  if (hasLocalGame()) {
    ids << QStringLiteral("local");
  }
  ids << shownSessionIds();
  return ids;
}

QStringList SessionController::shownSessionIds() const {
  QStringList ids;
  for (const auto& r : remotes_) {
    ids << r->info.sessionId;
  }
  return ids;
}

QVariantMap SessionController::surfaceInfo() const {
  QVariantMap out;
  if (hasLocalGame()) {
    out.insert(QStringLiteral("local"), QVariantMap{{QStringLiteral("kind"), QStringLiteral("local")},
                                                    {QStringLiteral("who"), tr("You")},
                                                    {QStringLiteral("game"), gameTitle_},
                                                    {QStringLiteral("name"), tr("You · %1").arg(gameTitle_)},
                                                    {QStringLiteral("meta"), tr("local")}});
  }
  for (const auto& r : remotes_) {
    out.insert(r->info.sessionId, QVariantMap{{QStringLiteral("kind"), QStringLiteral("remote")},
                                              {QStringLiteral("who"), r->info.owner.displayName},
                                              {QStringLiteral("userId"), r->info.owner.userId},
                                              {QStringLiteral("game"), r->info.gameTitle},
                                              {QStringLiteral("visibility"), visibilityLabel(r->info.visibility)},
                                              {QStringLiteral("since"), r->info.createdAt},
                                              {QStringLiteral("name"), tr("%1 · %2").arg(r->info.owner.displayName, r->info.gameTitle)},
                                              {QStringLiteral("meta"), tr("Session from %1").arg(r->info.owner.displayName)},
                                              {QStringLiteral("paused"), r->paused},
                                              {QStringLiteral("pausedBy"), r->pausedBy.isEmpty() ? r->info.owner.displayName : r->pausedBy},
                                              {QStringLiteral("pausedAt"), r->pausedAtMs},
                                              {QStringLiteral("loading"), r->frameNr == 0}});
  }
  return out;
}

// "Swap": the surface and the main surface exchange their positions.
void SessionController::makeMain(const QString& surface) {
  const qsizetype i = order_.indexOf(surface);
  if (i <= 0) {
    return;
  }
  order_.swapItemsAt(0, i);
  emit surfacesChanged();
  emit viewChanged();
}

// Exactly one audible surface: the chosen one while it exists, else the local game, else the first remaining one.
QString SessionController::audioFocus() const {
  if (focusPref_ == QLatin1String("local") ? hasLocalGame() : remote(focusPref_) != nullptr) {
    return focusPref_;
  }
  if (hasLocalGame() || order_.isEmpty()) {
    return QStringLiteral("local");
  }
  return order_.first();
}

// The selected tile: the chosen one while it exists, else your game's tile, else the first remaining one.
QString SessionController::selectedSurface() const {
  if (order_.contains(selected_)) {
    return selected_;
  }
  if (order_.contains(QStringLiteral("local"))) {
    return QStringLiteral("local");
  }
  return order_.isEmpty() ? QString() : order_.first();
}

void SessionController::selectSurface(const QString& surface) {
  if (!order_.contains(surface) || selectedSurface() == surface) {
    return;
  }
  selected_ = surface;
  emit selectionChanged();
}

void SessionController::audioHere(const QString& surface) {
  if (!order_.contains(surface)) {
    return;
  }
  focusPref_ = surface;
  applyAudioRouting();
  emit viewChanged();
}

// Exclusive (default): exactly one audible surface, the focused one. "Mix all tiles": every surface, each with its own gain.
void SessionController::applyAudioRouting() {
  const QString focus = audioFocus();
  const Remote* fr = focus == QLatin1String("local") ? nullptr : remote(focus);
  mixer_->setFocus(fr ? fr->audio.mixerInput() : game_->audio().mixerInput());
  game_->setAudioMuted(!mixer_->isRouted(game_->audio().mixerInput()));
  emit audioMixChanged();
}

void SessionController::setMixAllTiles(bool on) {
  if (on == mixer_->mixAll()) return;
  if (settings_) settings_->setAudioMixAll(on);
  mixer_->setMixAll(on);
  applyAudioRouting();
}

QStringList SessionController::audibleSurfaces() const {
  QStringList out;
  for (const QString& s : order_) {
    const Remote* r = s == QLatin1String("local") ? nullptr : remote(s);
    const AudioMixer::InputId in = r ? r->audio.mixerInput() : game_->audio().mixerInput();
    if ((r || hasLocalGame()) && mixer_->isRouted(in)) out.append(s);
  }
  return out;
}

QVariantList SessionController::mixerTiles() const {
  QVariantList out;
  for (int i = 0; i < order_.size(); ++i) {
    const Remote* r = remote(order_.at(i));
    if (!r) continue;
    out.append(QVariantMap{{QStringLiteral("sid"), r->info.sessionId},
                           {QStringLiteral("userId"), r->info.owner.userId},
                           {QStringLiteral("who"), r->info.owner.displayName},
                           {QStringLiteral("person"), i + 1},
                           {QStringLiteral("gain"), qRound(mixer_->personGain(AudioMixer::Tiles, r->info.owner.userId) * 100.0)}});
  }
  return out;
}

void SessionController::setTileGain(const QString& surface, int percent) {
  const Remote* r = remote(surface);
  if (!r || r->info.owner.userId.isEmpty()) return;
  percent = std::clamp(percent, 0, 100);
  if (settings_) settings_->setAudioTileGain(r->info.owner.userId, percent);
  mixer_->setPersonGain(AudioMixer::Tiles, r->info.owner.userId, percent / 100.0);
}

void SessionController::pumpAudio() {
  const qint64 target = audioClock_.elapsed() * 48;
  const qint64 n = std::min<qint64>(target - audioDueFrames_, 4800);
  if (n <= 0) {
    return;
  }
  audioDueFrames_ += n;
  for (const auto& r : remotes_) {
    const QByteArray pcm = r->viewer->pullAudio(static_cast<int>(n));  // always drained, only the routed ones are played
    if (mixer_->isRouted(r->audio.mixerInput())) {
      r->audio.push(pcm);
      fedFrames_[r->info.sessionId] += n;
    }
  }
}

void SessionController::setScreenLayout(const QString& layout) {
  if (!isScreenLayout(layout) || layout == screenLayout_) {
    return;
  }
  screenLayout_ = layout;
  emit viewChanged();
}

void SessionController::setScreenSwap(bool on) {
  if (on == screenSwap_) {
    return;
  }
  screenSwap_ = on;
  emit viewChanged();
}

void SessionController::setBottomWindow(bool on) {
  if (on == bottomWindow_) {
    return;
  }
  bottomWindow_ = on;
  emit viewChanged();
}

QStringList SessionController::watchLayouts() const { return screenLayoutsFor(2); }

QString SessionController::layoutLabel(const QString& layout) const { return screenLayoutLabel(layout); }

QString SessionController::nextLayout(const QStringList& layouts) const { return nextScreenLayout(layouts, screenLayout_); }

void SessionController::chooseScreenLayout(const QString& layout) {
  if (!isScreenLayout(layout)) {
    return;
  }
  setScreenLayout(layout);
  emit layoutNotice(tr("Layout: %1").arg(screenLayoutLabel(layout)));
}

void SessionController::chooseScreenSwap(bool on) {
  setScreenSwap(on);
  emit layoutNotice(on ? tr("Screens swapped") : tr("Screens not swapped"));
}

void SessionController::chooseBottomWindow(bool on) {
  setBottomWindow(on);
  emit layoutNotice(on ? tr("Bottom screen in its own window") : tr("Bottom screen back in the game view"));
}

void SessionController::setPictureOptions(bool integerScale, const QString& aspectRatio) {
  if (integerScale == integerScale_ && aspectRatio == aspectRatio_) {
    return;
  }
  integerScale_ = integerScale;
  aspectRatio_ = aspectRatio;
  emit viewChanged();
}

void SessionController::gameStarted(const QString& gameId, const QString& title, const QString& runId) {
  gameId_ = gameId;
  runId_ = runId;
  gameTitle_ = title;
  screenLayout_ = QStringLiteral("stacked");  // the in-game switch applies to this game only
  screenSwap_ = false;
  bottomWindow_ = false;
  focusPref_ = QStringLiteral("local");
  if (!order_.contains(QStringLiteral("local"))) {
    order_.prepend(QStringLiteral("local"));
  }
  tab_ = watching() ? QStringLiteral("multiview") : QStringLiteral("session");
  applyAudioRouting();
  presence();
  emit gameChanged();
  emit surfacesChanged();
  emit viewChanged();
}

void SessionController::gameEnded() {
  if (gameId_.isEmpty()) {
    return;
  }
  stopSharing();
  gameId_.clear();
  runId_.clear();
  gameTitle_.clear();
  bottomWindow_ = false;  // leaving the game closes the bottom-screen window
  order_.removeAll(QStringLiteral("local"));
  if (focusPref_ == QLatin1String("local")) {
    focusPref_ = order_.isEmpty() ? QStringLiteral("local") : order_.first();
  }
  fedFrames_.remove(QStringLiteral("local"));
  game_->setAudioMuted(false);
  applyAudioRouting();
  presence();
  emit gameChanged();
  emit surfacesChanged();
  emit viewChanged();
}

// ---------------------------------------------------------------- messages / diagnostics

void SessionController::say(const QString& text, bool error) {
  message_ = text;
  messageIsError_ = error;
  messageTimer_.start();
  emit messageChanged();
}

void SessionController::dismissMessage() {
  if (!message_.isEmpty()) {
    message_.clear();
    emit messageChanged();
  }
}

void SessionController::setStatsOverride(const SessionStats* local, const QHash<QString, SessionStats>& remotes) {
  override_ = local != nullptr || !remotes.isEmpty();
  overrideLocalSet_ = local != nullptr;
  overrideRemotes_ = remotes;
  if (local) overrideLocal_ = *local;
  refreshDiagnostics();
}

QList<ViewerLinkStats> SessionController::viewerLinks() const { return linksOverrideOn_ ? overrideLinks_ : host_.viewerLinks(); }

void SessionController::setLinksOverride(bool on, const QList<ViewerLinkStats>& links) {
  linksOverrideOn_ = on;
  overrideLinks_ = links;
  updateLinks();
  refreshDiagnostics();
}

// Connection pill per viewer of the own Session and the relay hint of the side panel (D9): cheap, twice a second.
void SessionController::updateLinks() {
  QVariantMap byViewer;
  QStringList relayed;
  if (shared_) {
    const QList<ViewerLinkStats> links = viewerLinks();
    for (const SessionViewerEntry& v : own_.viewers) {
      QString type;
      RelayReason reason = RelayReason::None;
      ViewerLinkStats found;
      bool haveLink = false;
      for (const ViewerLinkStats& l : links) {
        if (l.viewerId == v.viewerId) {
          type = l.connectionType;
          reason = l.relayReason;
          found = l;
          haveLink = true;
        }
      }
      const DiagnosticsModel::Pill pill = DiagnosticsModel::connectionPill(type);
      const QString reasonText = DiagnosticsModel::relayReasonText(reason);
      // People tab line (3g-12): "Direct · 18 ms · gets High" / "Relayed · 60 ms · picked Medium"
      QStringList line{shortPath(pill.text)};
      const std::optional<double> rtt = found.latencyMs ? found.latencyMs : found.rttMs;
      if (rtt) line << tr("%1 ms").arg(qRound(*rtt));
      const QString phrase = qualityPhrase(found.pick, found.tier, found.tierKbps, haveLink && found.tierKbps > 0);
      if (!phrase.isEmpty()) line << phrase;
      byViewer.insert(v.viewerId, QVariantMap{{QStringLiteral("text"), pill.text}, {QStringLiteral("tone"), pill.tone},
                                              {QStringLiteral("reason"), reasonText},
                                              {QStringLiteral("line"), line.join(QStringLiteral(" · "))}});
      if (DiagnosticsModel::isRelayed(type)) {
        relayed << (reasonText.isEmpty() ? v.displayName : QStringLiteral("%1 (%2)").arg(v.displayName, reasonText));
      }
    }
  }
  QString hint;
  if (relayed.size() == 1) {
    hint = tr("%1 is relayed via the server (TURN) · may lag slightly.").arg(relayed.first());
  } else if (relayed.size() > 1) {
    hint = tr("%1 are relayed via the server (TURN) · may lag slightly.").arg(relayed.join(QStringLiteral(", ")));
  }
  QString sending;
  if (shared_ && !own_.viewers.isEmpty()) {
    const SessionStats st = override_ && overrideLocalSet_ ? overrideLocal_ : host_.stats();
    if (st.active && st.encoders > 0) {
      sending = st.encoders == 1 ? tr("Sending 1 stream · %1 Mbit/s").arg(DiagnosticsModel::mbit(st.videoBitrateKbps))
                                 : tr("Sending %1 streams · %2 Mbit/s").arg(st.encoders).arg(DiagnosticsModel::mbit(st.videoBitrateKbps));
    }
  }
  if (byViewer != linkByViewer_ || hint != relayHint_ || sending != sendingLine_) {
    linkByViewer_ = byViewer;
    relayHint_ = hint;
    sendingLine_ = sending;
    emit shareChanged();
  }
  QVariantMap remoteLinks;
  for (const auto& r : remotes_) {
    const QString id = r->info.sessionId;
    const SessionStats st = override_ && overrideRemotes_.contains(id) ? overrideRemotes_.value(id) : r->viewer->stats();
    const DiagnosticsModel::Pill pill = DiagnosticsModel::connectionPill(st.connectionType);
    const std::optional<double> latency = st.latencyMs ? st.latencyMs : st.rttMs;  // the ping/pong, else the SCTP RTT
    remoteLinks.insert(id, QVariantMap{{QStringLiteral("text"), pill.text},
                                       {QStringLiteral("tone"), pill.tone},
                                       {QStringLiteral("reason"), DiagnosticsModel::relayReasonText(st.relayReason)},
                                       {QStringLiteral("rtt"), latency ? tr("%1 ms").arg(qRound(*latency)) : QString()},
                                       {QStringLiteral("quality"), qualityPhrase(r->viewer->quality(), st.servedTier, st.servedKbps, st.hasServedTier)},
                                       {QStringLiteral("pick"), qualityToken(r->viewer->quality())}});
  }
  if (remoteLinks != surfaceLinks_) {
    surfaceLinks_ = remoteLinks;
    emit surfaceLinksChanged();
  }
}

void SessionController::refreshDiagnostics() {
  updateLinks();
  if (!diagnostics_->isOpen()) {
    return;  // nothing is assembled while the overlay is closed
  }
  const EmulationDiagnostics emu = game_->diagnostics();
  diagnostics_->setEmulation(emu);

  const SessionStats local = override_ && overrideLocalSet_ ? overrideLocal_ : host_.stats();
  const bool sharedNow = shared_ || (override_ && overrideLocalSet_ && local.active);
  const QString turn = DiagnosticsModel::turnEndpoint(socket_.helloAck().turnServers);
  const QVariantMap info = surfaceInfo();

  QVariantList tiles, groups;
  int tileNo = 0;
  for (const QString& id : order_) {
    ++tileNo;
    const QVariantMap si = info.value(id).toMap();
    const QString title = si.value(QStringLiteral("name")).toString();
    if (id == QLatin1String("local")) {
      tiles.append(QVariantMap{{QStringLiteral("surface"), id}, {QStringLiteral("index"), tileNo},
                               {QStringLiteral("title"), title}, {QStringLiteral("sub"), tr("tile %1 · local").arg(tileNo)},
                               {QStringLiteral("local"), true}, {QStringLiteral("emulation"), DiagnosticsModel::emulationMap(emu)},
                               {QStringLiteral("note"), QString()}});
      if (sharedNow) {
        const QList<ViewerLinkStats> links = viewerLinks();
        QVariantList people;
        int viewers = 0;
        QVariantList viewerRows;
        for (const SessionViewerEntry& v : own_.viewers) {
          ++viewers;
          ViewerLinkStats l;
          for (const ViewerLinkStats& cand : links) {
            if (cand.viewerId == v.viewerId) l = cand;
          }
          const std::optional<double> kbps = l.hasReport ? std::optional<double>(l.reportKbps) : std::nullopt;
          const std::optional<double> loss = l.hasReport ? std::optional<double>(l.reportLoss * 100.0) : std::nullopt;
          viewerRows.append(DiagnosticsModel::participant(
              v.displayName, tr("viewer"), DiagnosticsModel::connectionPill(l.connectionType),
              DiagnosticsModel::decoderLine(l.reportDecoder, kbps, l.reportFps),
              DiagnosticsModel::linkLine(l.rttMs, loss, l.connectionType, turn, l.relayReason),
              DiagnosticsModel::linkFields(l.rttMs, loss, l.connectionType, l.relayReason)));
        }
        people.append(DiagnosticsModel::participant(tr("You"), tr("host"), {tr("Local"), QStringLiteral("neutral")},
                                                    DiagnosticsModel::hostLine(local),
                                                    DiagnosticsModel::hostSendingLine(std::max(viewers, local.viewers), local.encoders)));
        people.append(viewerRows);
        groups.append(QVariantMap{{QStringLiteral("surface"), id}, {QStringLiteral("title"), tr("Tile %1 · your session").arg(tileNo)},
                                  {QStringLiteral("details"), DiagnosticsModel::hostDetailRows(local)},
                                  {QStringLiteral("participants"), people}});
      }
    } else if (const Remote* r = remote(id)) {
      const QString who = r->info.owner.displayName.isEmpty() ? tr("Session") : r->info.owner.displayName;
      tiles.append(QVariantMap{{QStringLiteral("surface"), id}, {QStringLiteral("index"), tileNo},
                               {QStringLiteral("title"), title}, {QStringLiteral("sub"), tr("tile %1 · remote").arg(tileNo)},
                               {QStringLiteral("local"), false}, {QStringLiteral("emulation"), QVariantMap()},
                               {QStringLiteral("note"), tr("Emulation runs on %1's PC · not measured here").arg(who)}});
      const SessionStats st = override_ && overrideRemotes_.contains(id) ? overrideRemotes_.value(id) : r->viewer->stats();
      const std::optional<double> loss = st.packetLossPercent;
      groups.append(QVariantMap{
          {QStringLiteral("surface"), id},
          {QStringLiteral("title"), tr("Tile %1 · %2's session").arg(tileNo).arg(who)},
          {QStringLiteral("details"), DiagnosticsModel::viewerDetailRows(st)},
          {QStringLiteral("participants"),
           QVariantList{DiagnosticsModel::participant(who, tr("host"), DiagnosticsModel::connectionPill(st.connectionType),
                                                      DiagnosticsModel::decoderLine(st.decoderName, st.videoBitrateKbps, st.fps),
                                                      DiagnosticsModel::linkLine(st.rttMs, loss, st.connectionType, turn, st.relayReason),
                                                      DiagnosticsModel::linkFields(st.rttMs, loss, st.connectionType, st.relayReason))}}});
    }
  }
  diagnostics_->setTiles(tiles);
  diagnostics_->setStreaming(groups);
}

}  // namespace uzume::ui
