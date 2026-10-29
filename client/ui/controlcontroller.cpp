#include "controlcontroller.h"

#include <cmath>

#include "gamesession.h"
#include "sessioncontroller.h"

namespace uzume::ui {

namespace {
constexpr int kEscWindowMs = 2000;
constexpr int kAskSwitchGuardMs = 500;  // after the card switches to another request, a click or key 1/2 meant for the old one is ignored
}

ControlController::ControlController(SessionController* sessions, GameSession* game, QObject* parent)
    : QObject(parent), sessions_(sessions), game_(game) {
  ticker_.setInterval(500);
  connect(&ticker_, &QTimer::timeout, this, &ControlController::tick);
  SessionHost* host = sessions_->host();
  connect(host, &SessionHost::controlChanged, this, &ControlController::onHostControl);
  connect(host, &SessionHost::controlRequested, this, &ControlController::onHostControl);
  connect(host, &SessionHost::controlInput, this, [this](int port, const InputFrame& f, bool neutral) {
    if (!game_) return;
    const quint32 mask = neutral ? 0 : inputFrameMask(f);
    const qint16 none[4] = {0, 0, 0, 0};
    game_->setRemoteInput(port, mask, !neutral && f.touching, QPointF(f.touchX / double(kTouchMax), f.touchY / double(kTouchMax)), neutral ? none : f.axes);
  });
  connect(host, &SessionHost::controlEnded, this, [this](const QString& viewerId, int port, const QString& reason) {
    const QString name = sessions_->viewerName(viewerId);
    if (name.isEmpty()) return;
    if (reason == QLatin1String("left")) emit notice(tr("%1 stopped playing as P%2").arg(name).arg(port + 1));
    else if (reason == QLatin1String("viewer")) emit notice(tr("%1 stopped playing").arg(name));
  });
  connect(sessions_, &SessionController::controlChanged, this, &ControlController::onViewerControl);
  connect(sessions_, &SessionController::shareChanged, this, &ControlController::refreshPorts);
  connect(sessions_, &SessionController::gameChanged, this, &ControlController::refreshPorts);
  if (game_) connect(game_, &GameSession::stateChanged, this, &ControlController::refreshPorts);
}

void ControlController::setPortsProvider(std::function<int()> provider) {
  ports_ = std::move(provider);
  refreshPorts();
}

void ControlController::refreshPorts() {
  const bool live = game_ && game_->isActive() && sessions_->shared();
  const int ports = live && ports_ ? ports_() : 0;
  // A grant belongs to the game it was given for: another game ends every hold (the port count alone may be the same).
  const QString game = sessions_->hasLocalGame() ? sessions_->localTitle() : QString();
  if (game != lastGameTitle_ && !lastGameTitle_.isEmpty() && hasHolder()) takeBack();
  lastGameTitle_ = game;
  sessions_->host()->setControlPorts(ports);
  syncGame();
}

// The game follows the holders: a held port is remote-controlled (P1: the local input is suspended), a free one is not.
void ControlController::syncGame() {
  if (!game_) return;
  SessionHost* host = sessions_->host();
  for (int p = 0; p < 2; ++p) game_->setRemoteControlled(p, !host->controlHolder(p).isEmpty());
}

void ControlController::onHostControl() {
  SessionHost* host = sessions_->host();
  syncGame();
  const auto pending = host->controlPending();
  Ask next;
  if (!pending.isEmpty()) next = {pending.first().viewerId, pending.first().port, pending.first().deadlineMs};
  if (askOpen() && !next.viewerId.isEmpty() && (next.viewerId != ask_.viewerId || next.port != ask_.port)) askSwitched_.restart();
  else if (next.viewerId.isEmpty()) askSwitched_.invalidate();
  ask_ = next;
  if (askOpen() || hasHolder()) ticker_.start();
  else ticker_.stop();
  tick();
  emit changed();
}

void ControlController::tick() {
  const int before = askSeconds_;
  askSeconds_ = askOpen() ? (std::max)(0, int(std::ceil((ask_.deadlineMs - sessions_->host()->controlNowMs()) / 1000.0))) : 0;
  if (askSeconds_ != before) emit secondsChanged();
}

QString ControlController::askText() const {
  if (!askOpen()) return {};
  const QString name = sessions_->viewerName(ask_.viewerId);
  return ask_.port == 0 ? tr("%1 wants to take over P1").arg(name) : tr("%1 wants to play as P2").arg(name);
}

bool ControlController::hasHolder() const {
  return !sessions_->host()->controlHolder(0).isEmpty() || !sessions_->host()->controlHolder(1).isEmpty();
}

QVariantList ControlController::holders() const {
  QVariantList out;
  for (int p = 0; p < 2; ++p) {
    const QString id = sessions_->host()->controlHolder(p);
    if (!id.isEmpty()) out.append(QVariantMap{{QStringLiteral("viewerId"), id}, {QStringLiteral("name"), sessions_->viewerName(id)}, {QStringLiteral("port"), p}});
  }
  return out;
}

QString ControlController::holderText() const {
  const QVariantList h = holders();
  if (h.isEmpty()) return {};
  const QVariantMap first = h.first().toMap();
  QString text = tr("%1 plays P%2").arg(first.value(QStringLiteral("name")).toString()).arg(first.value(QStringLiteral("port")).toInt() + 1);
  if (h.size() > 1) text += tr(" and %n other", "", h.size() - 1);
  return text;
}

void ControlController::answer(bool allow) {
  if (!askOpen()) return;
  if (askSwitched_.isValid() && askSwitched_.elapsed() < kAskSwitchGuardMs) return;
  const QString id = ask_.viewerId;
  sessions_->host()->answerControl(id, allow);
}

void ControlController::takeBack() {
  SessionHost* host = sessions_->host();
  for (int p = 0; p < 2; ++p) host->takeBack(p);
}

bool ControlController::escapePressed() {
  // Only a take-over of P1 takes the Esc: the host's own game is suspended then. A guest on P2 leaves Esc to pause as usual.
  if (sessions_->host()->controlHolder(0).isEmpty()) {
    escArmed_ = false;
    return false;
  }
  if (escArmed_ && escClock_.isValid() && escClock_.elapsed() <= kEscWindowMs) {
    escArmed_ = false;
    sessions_->host()->takeBack(0);
    emit notice(tr("Controller taken back"));
    return true;
  }
  escArmed_ = true;
  escClock_.restart();
  emit notice(tr("Press Esc again to take back the controller"));
  return true;
}

// ---------------------------------------------------------------- viewer

QString ControlController::playingSession() const {
  for (const QString& sid : sessions_->shownSessionIds()) {
    if (SessionViewer* v = sessions_->viewer(sid); v && v->controlState() == SessionViewer::ControlState::Granted) return sid;
  }
  return {};
}

bool ControlController::controlling(const QString& sessionId) const {
  SessionViewer* v = sessions_->viewer(sessionId);
  return v && v->controlState() == SessionViewer::ControlState::Granted;
}

bool ControlController::canTouch(const QString& sessionId) const {
  SessionViewer* v = sessions_->viewer(sessionId);
  return v && v->controlState() == SessionViewer::ControlState::Granted && v->controlPort() == 0;
}

void ControlController::setKeyboardMap(const QHash<int, quint32>& map) {
  keys_.setMap(map);
  if (!focus_.isEmpty()) pushInput(focus_);
}

void ControlController::setPadMask(quint32 mask) {
  if (mask == pad_) return;
  pad_ = mask;
  if (!focus_.isEmpty()) pushInput(focus_);
}

void ControlController::tileFocus(const QString& sessionId, bool focused) {
  if (focused) {
    focus_ = sessionId;
    pushInput(sessionId);
    return;
  }
  if (focus_ != sessionId) return;
  releaseKeys(sessionId);
  focus_.clear();
}

void ControlController::releaseKeys(const QString& sessionId) {
  keys_.clear();
  touching_ = false;
  pushInput(sessionId);
}

bool ControlController::remoteKey(const QString& sessionId, int key, bool pressed, Qt::KeyboardModifiers mods) {
  if (!controlling(sessionId) || reserved_.contains(key) || key == Qt::Key_Escape) return false;
  if (mods & (Qt::AltModifier | Qt::ControlModifier | Qt::MetaModifier)) return false;
  const bool mapped = pressed ? keys_.press(key) : keys_.release(key);
  if (mapped) pushInput(sessionId);
  return mapped;
}

void ControlController::remoteTouch(const QString& sessionId, double x, double y, bool pressed) {
  if (!canTouch(sessionId)) return;
  touching_ = pressed;
  if (pressed) {
    touchX_ = x;
    touchY_ = y;
  }
  pushInput(sessionId);
}

void ControlController::pushInput(const QString& sessionId) {
  SessionViewer* v = sessions_->viewer(sessionId);
  if (!v || v->controlState() != SessionViewer::ControlState::Granted) return;
  const bool mine = sessionId == focus_;  // keyboard and pad only count while the tile has the focus
  const quint16 buttons = static_cast<quint16>(((mine ? (keys_.mask() | pad_) : 0u) | debug_) & 0xFFFFu);
  const qint16 axes[4] = {0, 0, 0, 0};
  v->setControlInput(buttons, axes, touching_ && v->controlPort() == 0, touchX_, touchY_);
}

void ControlController::onViewerControl() {
  // A grant gives the tile the keyboard; a hold that ends lets go of everything.
  bool lost = false;
  for (const QString& sid : sessions_->shownSessionIds()) {
    SessionViewer* v = sessions_->viewer(sid);
    if (!v) continue;
    const int now = static_cast<int>(v->controlState());
    const int was = lastState_.value(sid, 0);
    lastState_.insert(sid, now);
    if (now == was) continue;
    if (v->controlState() != SessionViewer::ControlState::Granted) {
      if (was == static_cast<int>(SessionViewer::ControlState::Granted)) {
        lost = true;
        keys_.clear();
        touching_ = false;
        debug_ = 0;
        const QString why = v->controlReason();
        if (why == QLatin1String("host")) emit notice(tr("%1 took the controller back").arg(sessions_->surfaceInfo().value(sid).toMap().value(QStringLiteral("who")).toString()));
      }
    } else {
      focus_ = sid;  // the tile is the one the person just asked from
      pushInput(sid);
    }
  }
  if (lost) {
    for (const QString& sid : sessions_->shownSessionIds()) pushInput(sid);  // other sessions I still play in see the released keys
  }
  emit changed();
}

// ---------------------------------------------------------------- MCP / tests

bool ControlController::request(const QString& sessionId, int port, QString* error) {
  SessionViewer* v = sessions_->viewer(sessionId);
  if (!v) {
    if (error) *error = QStringLiteral("not_watching");
    return false;
  }
  if (v->hostPorts() <= 0) {
    if (error) *error = QStringLiteral("host_offers_no_controller");
    return false;
  }
  if (port < 0 || port >= v->hostPorts()) {
    if (error) *error = QStringLiteral("port_not_available");
    return false;
  }
  if (v->controlState() != SessionViewer::ControlState::Idle) {
    if (error) *error = QStringLiteral("already_asking_or_playing");
    return false;
  }
  sessions_->controlAction(sessionId, {}, port == 0 ? QStringLiteral("p1") : QStringLiteral("p2"));
  if (v->controlState() != SessionViewer::ControlState::Asking) {
    if (error) *error = QStringLiteral("channel_not_open");
    return false;
  }
  return true;
}

bool ControlController::answerViewer(const QString& viewerId, bool allow) { return sessions_->host()->answerControl(viewerId, allow); }

bool ControlController::release(const QString& sessionId) {
  SessionViewer* v = sessions_->viewer(sessionId);
  if (!v || v->controlState() == SessionViewer::ControlState::Idle) return false;
  v->releaseControl();
  return true;
}

bool ControlController::remoteInput(const QString& sessionId, quint32 buttons) {
  if (!controlling(sessionId)) return false;
  debug_ = buttons & 0xFFFFu;
  pushInput(sessionId);
  return true;
}

QVariantMap ControlController::state() const {
  SessionHost* host = sessions_->host();
  QVariantList pending;
  for (const auto& p : host->controlPending())
    pending.append(QVariantMap{{QStringLiteral("viewer_id"), p.viewerId}, {QStringLiteral("name"), sessions_->viewerName(p.viewerId)}, {QStringLiteral("port"), p.port}});
  QVariantList holders;
  for (int p = 0; p < 2; ++p) {
    const QString id = host->controlHolder(p);
    if (id.isEmpty()) continue;
    const InputFrame f = host->controlInputFrame(p);
    holders.append(QVariantMap{{QStringLiteral("port"), p},
                               {QStringLiteral("viewer_id"), id},
                               {QStringLiteral("name"), sessions_->viewerName(id)},
                               {QStringLiteral("buttons"), int(f.buttons)},
                               {QStringLiteral("neutral"), f.buttons == 0 && !f.touching}});
  }
  QVariantList watching;
  for (const QString& sid : sessions_->shownSessionIds()) {
    SessionViewer* v = sessions_->viewer(sid);
    if (!v) continue;
    static const char* names[] = {"idle", "asking", "playing"};
    watching.append(QVariantMap{{QStringLiteral("session_id"), sid},
                                {QStringLiteral("state"), QString::fromLatin1(names[static_cast<int>(v->controlState())])},
                                {QStringLiteral("port"), v->controlPort()},
                                {QStringLiteral("host_ports"), v->hostPorts()},
                                {QStringLiteral("reason"), v->controlReason()},
                                {QStringLiteral("frames_sent"), v->inputFramesSent()}});
  }
  QVariantMap applied;
  if (game_) applied = QVariantMap{{QStringLiteral("p1_remote"), game_->remoteControlled(0)}, {QStringLiteral("p1_mask"), int(game_->remoteMask(0))},
                                   {QStringLiteral("p2_remote"), game_->remoteControlled(1)}, {QStringLiteral("p2_mask"), int(game_->remoteMask(1))}};
  return QVariantMap{{QStringLiteral("ports"), host->controlPorts()}, {QStringLiteral("holders"), holders}, {QStringLiteral("pending"), pending},
                     {QStringLiteral("watching"), watching}, {QStringLiteral("game"), applied}};
}

}  // namespace uzume::ui
