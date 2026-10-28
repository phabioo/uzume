#include "togethercontroller.h"

#include <QDateTime>
#include <QVariantMap>

#include "onlineroster.h"

namespace uzume::ui {

namespace {
constexpr const char* kFeature = "together_v1";
}

TogetherController::Scope TogetherController::pickScope(const ScopeInputs& in) {
  if (!in.boardRunId.isEmpty()) return {QStringLiteral("run"), in.boardRunId, in.gameActive && in.gameRunId == in.boardRunId};
  if (!in.gameRunId.isEmpty() && in.gameActive) return {QStringLiteral("run"), in.gameRunId, true};
  if (!in.ownSessionId.isEmpty()) return {QStringLiteral("session"), in.ownSessionId, true};
  if (!in.watched.isEmpty()) return {QStringLiteral("session"), in.watched.contains(in.focus) ? in.focus : in.watched.first(), false};
  return {};
}

TogetherController::TogetherController(HubSocket* socket, QObject* parent) : QObject(parent), socket_(socket) {
  connect(socket_, &HubSocket::readyCheckReceived, this, &TogetherController::onReadyCheck);
  connect(socket_, &HubSocket::pauseStateReceived, this, &TogetherController::onPauseState);
  connect(socket_, &HubSocket::stateChanged, this, &TogetherController::onSocketState);
  timer_.setInterval(250);
  connect(&timer_, &QTimer::timeout, this, &TogetherController::tick);
  timer_.start();
}

qint64 TogetherController::now() const { return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch(); }

bool TogetherController::featureOn() const {
  if (featureFn_) return featureFn_();
  return socket_ && socket_->helloAck().features.contains(QLatin1String(kFeature));
}

QString TogetherController::nameOf(const QString& userId) const {
  const QString n = nameFn_ ? nameFn_(userId) : QString();
  return n.isEmpty() ? tr("Someone") : n;
}

QString TogetherController::checkTitle() const { return check_.open ? tr("Ready check · %1").arg(nameOf(check_.by)) : QString(); }

QString TogetherController::myState() const {
  if (!check_.open) return {};
  for (const Member& m : check_.members) {
    if (m.userId == self()) return m.state;
  }
  return {};
}

QVariantList TogetherController::checkMembers() const {
  QVariantList out;
  for (const Member& m : check_.members) {
    QString text = tr("Waiting");
    QString mark = QStringLiteral("…");
    if (m.state == QLatin1String("ready")) {
      text = tr("Ready");
      mark = QStringLiteral("✓");
    } else if (m.state == QLatin1String("not_ready")) {
      text = tr("Not yet");
      mark = QStringLiteral("✕");
    }
    out.append(QVariantMap{{QStringLiteral("userId"), m.userId},
                           {QStringLiteral("name"), nameOf(m.userId)},
                           {QStringLiteral("color"), OnlineRoster::colorOf(m.userId)},
                           {QStringLiteral("state"), m.state},
                           {QStringLiteral("stateText"), text},
                           {QStringLiteral("mark"), mark},
                           {QStringLiteral("me"), m.userId == self()}});
  }
  return out;
}

int TogetherController::secondsLeft() const {
  if (!check_.open) return 0;
  const qint64 left = check_.endAtMs - now();
  return left <= 0 ? 0 : static_cast<int>((left + 999) / 1000);
}

QString TogetherController::pausedText() const {
  if (!paused_) return {};
  if (!pausedBy_.isEmpty() && pausedBy_ == self()) return tr("You paused for everyone");
  if (pausedBy_.isEmpty()) return tr("Paused for everyone");
  return tr("%1 paused for everyone").arg(nameOf(pausedBy_));
}

void TogetherController::startCheck() {
  if (!canControl()) return;
  socket_->sendReadyCheckStart(scope_.kind, scope_.id);
}

void TogetherController::answer(bool ready) {
  if (!check_.open) return;
  socket_->sendReadyCheckAnswer(check_.checkId, ready);
  for (Member& m : check_.members) {
    if (m.userId == self()) m.state = ready ? QStringLiteral("ready") : QStringLiteral("not_ready");  // the Hub's next message confirms
  }
  emit checkChanged();
}

void TogetherController::cancelCheck() {
  if (check_.open && iStarted()) socket_->sendReadyCheckCancel(check_.checkId);
}

void TogetherController::setPaused(bool paused) {
  if (!canControl()) return;
  socket_->sendPauseRequest(scope_.kind, scope_.id, paused);
}

void TogetherController::showNotice(const QString& text) {
  notice_ = text;
  noticeUntilMs_ = now() + kNoticeMs;
  emit noticeChanged();
}

void TogetherController::clearCheck() {
  if (!check_.open) return;
  check_ = {};
  lastSeconds_ = 0;
  emit checkChanged();
  emit secondsLeftChanged();
}

void TogetherController::releaseHold(bool resume) {
  if (!hold_) return;
  hold_ = false;
  if (resume && game_.isPaused && game_.isPaused() && game_.resume) game_.resume();
}

void TogetherController::onReadyCheck(const ReadyCheckMessage& m) {
  refresh();
  if (!inScope(m.scope, m.id)) return;
  if (m.status == QLatin1String("open")) {
    check_.open = true;
    check_.checkId = m.checkId;
    check_.by = m.by;
    check_.members.clear();
    for (const ReadyCheckMember& rm : m.members) check_.members.append({rm.userId, rm.state});
    // The Hub's expiry is on its clock; the local countdown never exceeds the check length.
    qint64 left = m.expiresAtMs > 0 ? m.expiresAtMs - QDateTime::currentMSecsSinceEpoch() : 0;
    if (left <= 0 || left > kCheckMs + 1000) left = kCheckMs;
    check_.endAtMs = now() + left;
    lastSeconds_ = secondsLeft();
    emit checkChanged();
    emit secondsLeftChanged();
    return;
  }
  const bool known = check_.open && check_.checkId == m.checkId;
  clearCheck();
  if (!known && m.status != QLatin1String("all_ready")) return;
  if (m.status == QLatin1String("all_ready")) showNotice(tr("Everyone is ready"));
  else if (m.status == QLatin1String("expired")) showNotice(tr("Ready check expired"));
  else if (m.status == QLatin1String("cancelled")) showNotice(tr("Ready check cancelled"));
}

void TogetherController::onPauseState(const PauseStateMessage& m) {
  refresh();
  if (!inScope(m.scope, m.id)) return;
  if (m.paused) {
    paused_ = true;
    pausedBy_ = m.by;
    holdAndPause();
  } else {
    paused_ = false;
    pausedBy_.clear();
    releaseHold(true);
  }
  emit pauseChanged();
}

// Pauses my game for the shared pause. Only a game that this pause stops is resumed by it.
void TogetherController::holdAndPause() {
  if (hold_ || !scope_.hostsGame || !game_.isPaused || game_.isPaused() || !game_.pause) return;
  hold_ = true;
  game_.pause();
}

void TogetherController::onSocketState(HubSocket::State s) {
  refresh();
  emit scopeChanged();  // `available` also follows the Hub's features
  if (s == HubSocket::State::Open) return;
  // The link is gone: a resume or an answer could be missed, so nothing stays held.
  clearCheck();
  if (paused_ || hold_) {
    paused_ = false;
    pausedBy_.clear();
    releaseHold(true);
    emit pauseChanged();
  }
}

void TogetherController::refresh() {
  Scope next = scopeFn_ ? scopeFn_() : Scope{};
  const bool same = next.kind == scope_.kind && next.id == scope_.id && next.hostsGame == scope_.hostsGame;
  if (same) return;
  const bool moved = next.kind != scope_.kind || next.id != scope_.id;
  scope_ = next;
  if (moved) {
    clearCheck();
    if (paused_ || hold_) {
      paused_ = false;
      pausedBy_.clear();
      releaseHold(true);
      emit pauseChanged();
    }
  } else if (paused_) {
    holdAndPause();  // my game started inside a scope that is already paused
  }
  emit scopeChanged();
}

void TogetherController::noteLocalPauseChanged() {
  if (hold_ && game_.isPaused && !game_.isPaused()) {
    hold_ = false;  // someone resumed the game by hand: the shared pause no longer owns it
    emit pauseChanged();
  }
}

void TogetherController::tick() {
  if (!notice_.isEmpty() && now() >= noticeUntilMs_) {
    notice_.clear();
    emit noticeChanged();
  }
  if (!check_.open) return;
  if (now() > check_.endAtMs + 2000) {  // the final message was lost
    clearCheck();
    return;
  }
  const int s = secondsLeft();
  if (s != lastSeconds_) {
    lastSeconds_ = s;
    emit secondsLeftChanged();
  }
}

}  // namespace uzume::ui
