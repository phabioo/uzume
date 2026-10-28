#include "voicecontroller.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "playersettings.h"
#include "voicecapture.h"

namespace uzume::ui {

namespace {
constexpr int kMeterEveryFrames = 3;       // the meter updates every 60 ms
constexpr double kPttSpeakDb = -50.0;      // push-to-talk: the speaking mark shows while the level is above this
constexpr int kNoticeMs = 5000;
}  // namespace

VoiceController::VoiceController(HubSocket* socket, AudioMixer* mixer, PlayerSettings* settings, QObject* parent)
    : QObject(parent), socket_(socket), mixer_(mixer), settings_(settings), engine_(this), capture_(std::make_unique<VoiceCapture>(this)) {
  elapsed_.start();
  farFrame_.assign(uzume::kVoiceFrame, 0);
  noticeTimer_.setSingleShot(true);
  noticeTimer_.setInterval(kNoticeMs);
  connect(&noticeTimer_, &QTimer::timeout, this, [this]() {
    notice_.clear();
    emit noticeChanged();
  });
  injectTimer_.setInterval(20);
  injectTimer_.setTimerType(Qt::PreciseTimer);
  connect(&injectTimer_, &QTimer::timeout, this, &VoiceController::injectTick);
  connect(capture_.get(), &VoiceCapture::frame, this, &VoiceController::onCaptureFrame);
  connect(capture_.get(), &VoiceCapture::statusChanged, this, &VoiceController::onCaptureStatus);
  connect(&engine_, &VoiceEngine::signalOut, this, [this](const QString& to, const QJsonObject& body) {
    if (socket_ && want_) socket_->sendVoiceSignal(roomKind_, roomId_, to, body);
  });
  connect(&engine_, &VoiceEngine::pcmDecoded, this, &VoiceController::onPcm);
  connect(&engine_, &VoiceEngine::speakingChanged, this, &VoiceController::onSpeaking);
  connect(&engine_, &VoiceEngine::peerStateChanged, this, [this]() { emit membersChanged(); });
  if (socket_) {
    connect(socket_, &HubSocket::voiceRoomReceived, this, &VoiceController::onVoiceRoom);
    connect(socket_, &HubSocket::voiceSignalReceived, this, &VoiceController::onVoiceSignal);
    connect(socket_, &HubSocket::stateChanged, this, &VoiceController::onSocketState);
    connect(socket_, &HubSocket::hubErrorReply, this, &VoiceController::onHubError);
    connect(socket_, &HubSocket::iceUpdated, this, [this](const HelloAck& ack) { engine_.setIceServers(ack.iceServers, ack.turnServers); });
  }
  configureDsp();
}

VoiceController::~VoiceController() {
  if (mixer_) {
    mixer_->setOutputTap(nullptr);
    if (mixer_->ducking()) mixer_->setDucking(false);
  }
  dropInputs();
  if (mixerHeld_ && mixer_) mixer_->release();
}

qint64 VoiceController::now() const { return clock_ ? clock_() : elapsed_.elapsed(); }

bool VoiceController::featureOn() const {
  if (featureFn_) return featureFn_();
  return socket_ && socket_->helloAck().features.contains(QLatin1String("together_v1"));
}

bool VoiceController::available() const {
  if (!featureOn() || !scopeFn_) return false;
  return !scopeFn_().kind.isEmpty();
}

QString VoiceController::nameOf(const QString& userId) const {
  const QString n = nameFn_ ? nameFn_(userId) : QString();
  return n.isEmpty() ? tr("Someone") : n;
}

// ---------------------------------------------------------------- settings

QString VoiceController::talkMode() const { return settings_ ? settings_->voiceTalkMode() : QStringLiteral("ptt"); }
int VoiceController::thresholdDb() const { return settings_ ? settings_->voiceThresholdDb() : -42; }
bool VoiceController::noiseSuppression() const { return settings_ ? settings_->voiceNoiseSuppression() : true; }
bool VoiceController::ducking() const { return settings_ ? settings_->voiceDucking() : true; }
bool VoiceController::autoJoin() const { return settings_ ? settings_->voiceAutoJoin() : true; }
QString VoiceController::micDevice() const { return settings_ ? settings_->voiceMicDevice() : QString(); }
bool VoiceController::micOn() const { return settings_ ? settings_->voiceMicOn() : true; }

QVariantList VoiceController::micDevices() const { return VoiceCapture::devices(); }

void VoiceController::setTalkMode(const QString& mode) {
  if (!settings_ || mode == talkMode() || !settings_->setVoiceTalkMode(mode)) return;
  pttHeld_ = false;
  activity_.reset();
  emit settingsChanged();
  emit micChanged();
  sendWireState();
}

void VoiceController::setThresholdDb(int db) {
  if (!settings_) return;
  settings_->setVoiceThresholdDb(db);
  emit settingsChanged();
}

void VoiceController::setThresholdFraction(double f) {
  setThresholdDb(static_cast<int>(std::lround(std::clamp(f, 0.0, 1.0) * 60.0 - 60.0)));
}

void VoiceController::setNoiseSuppression(bool on) {
  if (!settings_ || on == noiseSuppression()) return;
  settings_->setVoiceNoiseSuppression(on);
  configureDsp();
  emit settingsChanged();
}

void VoiceController::setDucking(bool on) {
  if (!settings_ || on == ducking()) return;
  settings_->setVoiceDucking(on);
  updateDucking();
  emit settingsChanged();
}

void VoiceController::setAutoJoin(bool on) {
  if (!settings_ || on == autoJoin()) return;
  settings_->setVoiceAutoJoin(on);
  emit settingsChanged();
  if (on) refresh();
}

void VoiceController::setMicDevice(const QString& id) {
  if (!settings_ || id == micDevice()) return;
  settings_->setVoiceMicDevice(id);
  emit settingsChanged();
  if (capture_->status() != VoiceCapture::Status::Stopped) retryMic();
}

void VoiceController::setPttKeyLabel(const QString& label) {
  if (label.isEmpty() || label == pttKeyLabel_) return;
  pttKeyLabel_ = label;
  emit settingsChanged();
  emit micChanged();
}

void VoiceController::setMonitoring(bool on) {
  if (monitoring_ == on) return;
  monitoring_ = on;
  updateTap();
  if (on) startCaptureIfNeeded();
  else stopCaptureIfIdle();
  emit settingsChanged();
}

void VoiceController::configureDsp() {
  VoiceDsp::Options o;
  o.denoise = noiseSuppression();
  o.echoCancel = noiseSuppression();
  o.agc = false;  // the activation threshold is read from the processed level: an AGC would move it
  dspNoise_ = noiseSuppression();
  if (dspNoise_) dsp_.open(o);
  else dsp_.close();
  {
    std::lock_guard<std::mutex> lock(farMutex_);
    far_.clear();
  }
  updateTap();
}

// The echo reference is collected only while the microphone is open and the cancellation is on.
void VoiceController::updateTap() {
  if (!mixer_) return;
  if (dspNoise_ && (want_ || monitoring_)) mixer_->setOutputTap([this](const float* s, int n, int rate) { tapFarEnd(s, n, rate); });
  else mixer_->setOutputTap(nullptr);
}

// The audio thread: what the sink is about to play, as mono 48 kHz, for the echo canceller.
void VoiceController::tapFarEnd(const float* stereo, int frames, int rate) {
  if (frames <= 0 || rate <= 0) return;
  std::lock_guard<std::mutex> lock(farMutex_);
  const double step = double(rate) / double(uzume::kVoiceRate);
  while (farPos_ < frames) {
    const int i = static_cast<int>(farPos_);
    const float m = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
    far_.push_back(static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(m * 32767.0f)), -32768, 32767)));
    farPos_ += step;
  }
  farPos_ -= frames;
  const size_t cap = static_cast<size_t>(uzume::kVoiceRate);  // one second at most
  while (far_.size() > cap) far_.pop_front();
}

// ---------------------------------------------------------------- the mic

MicInputs VoiceController::inputs() const {
  MicInputs in;
  in.micOn = micOn();
  in.vad = talkMode() == QLatin1String("vad");
  in.pttHeld = pttHeld_ || injecting();
  in.mutedByHost = mutedByHost_;
  in.deviceAvailable = !captureNoDevice_ || injecting();
  in.blocked = captureBlocked_ && !injecting();
  in.activityOpen = activity_.open();
  return in;
}

QString VoiceController::micWord() const {
  switch (micState()) {
    case MicState::Off: return tr("Mic off");
    case MicState::PttIdle: return tr("Hold %1").arg(pttKeyLabel_);
    case MicState::Live: return tr("Live");
    case MicState::MutedByHost: return tr("Muted by host");
    case MicState::NoDevice: return tr("No microphone");
    case MicState::Blocked: return tr("Blocked");
  }
  return {};
}

QString VoiceController::micDetail() const {
  switch (micState()) {
    case MicState::Off: return tr("Your microphone is off. Others see “mic off”.");
    case MicState::PttIdle: return tr("Hold %1 to talk.").arg(pttKeyLabel_);
    case MicState::Live: return talkMode() == QLatin1String("vad") ? tr("Your microphone is live. It sends while you speak.") : tr("You are talking.");
    case MicState::MutedByHost: return tr("The host muted you. Only the host can undo this.");
    case MicState::NoDevice: return tr("No microphone found. You can still listen.");
    case MicState::Blocked:
#ifdef Q_OS_WIN
      return tr("Windows does not let Uzume use the microphone. Allow it in the Windows privacy settings.");
#else
      return tr("The system does not let Uzume use the microphone. Allow it in the prompt of your sound system (PipeWire) and try again.");
#endif
  }
  return {};
}

double VoiceController::level() const { return std::clamp((levelDb_ - kMinDb) / -kMinDb, 0.0, 1.0); }

double VoiceController::thresholdFraction() const { return std::clamp((thresholdDb() - kMinDb) / -kMinDb, 0.0, 1.0); }

void VoiceController::startCaptureIfNeeded() {
  if (!captureEnabled_ || (!want_ && !monitoring_) || capture_->running()) return;
  const auto st = capture_->start(VoiceCapture::decodeId(micDevice()));
  captureNoDevice_ = st == VoiceCapture::Status::NoDevice;
  captureBlocked_ = st == VoiceCapture::Status::Blocked;
  emit micChanged();
  sendWireState();
}

void VoiceController::stopCaptureIfIdle() {
  if (want_ || monitoring_) return;
  capture_->stop();
  captureNoDevice_ = captureBlocked_ = false;
  levelDb_ = -120;
  selfSpeaking_ = false;
  activity_.reset();
  emit levelChanged();
  emit micChanged();
}

void VoiceController::onCaptureStatus() {
  if (!captureEnabled_) return;
  const auto st = capture_->status();
  const bool blocked = st == VoiceCapture::Status::Blocked;
  const bool none = st == VoiceCapture::Status::NoDevice;
  if (blocked == captureBlocked_ && none == captureNoDevice_) return;
  captureBlocked_ = blocked;
  captureNoDevice_ = none;
  emit micChanged();
  sendWireState();
}

void VoiceController::retryMic() {
  capture_->stop();
  captureNoDevice_ = captureBlocked_ = false;
  startCaptureIfNeeded();
  emit micChanged();
}

void VoiceController::openSystemSettings() {
  if (openSystemFn_) {
    openSystemFn_();
    return;
  }
#ifdef Q_OS_WIN
  QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:privacy-microphone")));
#else
  retryMic();  // PipeWire asks again when the app opens the microphone
#endif
}

void VoiceController::onCaptureFrame(const QByteArray& mono16) {
  if (injecting() || mono16.size() < uzume::kVoiceFrame * int(sizeof(int16_t))) return;
  int16_t frame[uzume::kVoiceFrame];
  std::memcpy(frame, mono16.constData(), sizeof(frame));
  onMicFrame(frame);
}

void VoiceController::onMicFrame(const int16_t* mic) {
  int16_t out[uzume::kVoiceFrame];
  if (dspNoise_ && dsp_.isOpen()) {
    // Echo reference: what the speakers played, held back by the sink's buffer so that it lines up with what the microphone hears.
    {
      const size_t delay = static_cast<size_t>(std::clamp(mixer_ ? mixer_->sinkBufferedMs() : 0.0, 0.0, 300.0) * uzume::kVoiceRate / 1000.0);
      std::lock_guard<std::mutex> lock(farMutex_);
      const size_t cap = delay + 3 * static_cast<size_t>(uzume::kVoiceFrame);
      while (far_.size() > cap) far_.pop_front();
      if (far_.size() >= delay + static_cast<size_t>(uzume::kVoiceFrame)) {
        for (int i = 0; i < uzume::kVoiceFrame; ++i) {
          farFrame_[static_cast<size_t>(i)] = far_.front();
          far_.pop_front();
        }
        dsp_.pushFarEnd(farFrame_.data(), uzume::kVoiceFrame);
      }
    }
    dsp_.process(mic, out);
  } else {
    std::memcpy(out, mic, sizeof(out));
  }
  levelDb_ = VoiceDsp::levelDb(out, uzume::kVoiceFrame);
  MicInputs in = inputs();
  in.activityOpen = activity_.update(levelDb_, thresholdDb(), now());
  const bool tx = micTransmits(in);
  engine_.pushFrame(out, tx);
  const bool speaking = tx && (in.vad ? in.activityOpen : levelDb_ > kPttSpeakDb);
  if (speaking != selfSpeaking_) {
    selfSpeaking_ = speaking;
    emit micChanged();
    emit membersChanged();
  }
  if (++levelFrames_ >= kMeterEveryFrames) {
    levelFrames_ = 0;
    emit levelChanged();
  }
}

void VoiceController::injectTick() {
  if (inject_.empty()) {
    injectTimer_.stop();
    sendWireState();
    levelDb_ = -120;
    activity_.reset();
    selfSpeaking_ = false;
    emit levelChanged();
    emit micChanged();
    emit membersChanged();
    return;
  }
  const std::vector<int16_t> f = std::move(inject_.front());
  inject_.pop_front();
  // The synthetic source stands in for the microphone: no DSP (a pure tone is stationary noise to the suppressor), and it counts as
  // push-to-talk held / the person talking, while the person's own switches (mic off, muted by host) still apply.
  levelDb_ = VoiceDsp::levelDb(f.data(), uzume::kVoiceFrame);
  MicInputs in = inputs();
  in.activityOpen = activity_.update(levelDb_, thresholdDb(), now());
  const bool tx = micTransmits(in);
  engine_.pushFrame(f.data(), tx);
  const bool speaking = tx && levelDb_ > kPttSpeakDb;
  if (speaking != selfSpeaking_) {
    selfSpeaking_ = speaking;
    emit membersChanged();
  }
  emit micChanged();
  if (++levelFrames_ >= kMeterEveryFrames) {
    levelFrames_ = 0;
    emit levelChanged();
  }
}

bool VoiceController::injectTone(double hz, double seconds) {
  if (hz <= 0 || seconds <= 0) return false;
  const int frames = std::clamp(static_cast<int>(std::lround(seconds * 50.0)), 1, 50 * 60);
  for (int f = 0; f < frames; ++f) {
    std::vector<int16_t> v(uzume::kVoiceFrame);
    for (int i = 0; i < uzume::kVoiceFrame; ++i) {
      v[static_cast<size_t>(i)] = static_cast<int16_t>(9000.0 * std::sin(2.0 * M_PI * hz * double(f * uzume::kVoiceFrame + i) / uzume::kVoiceRate));
    }
    inject_.push_back(std::move(v));
  }
  if (!injectTimer_.isActive()) injectTimer_.start();
  emit micChanged();
  sendWireState();  // the synthetic source counts as a microphone
  return true;
}

// 16-bit PCM WAV, any channel count (the first channel is used), 48 kHz or 44.1 / 32 / 24 / 16 / 8 kHz (nearest-sample resampling).
bool VoiceController::injectWav(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return false;
  const QByteArray d = f.readAll();
  if (d.size() < 44 || d.left(4) != "RIFF" || d.mid(8, 4) != "WAVE") return false;
  int channels = 1, rate = 48000, bits = 16;
  qsizetype dataAt = -1, dataLen = 0;
  for (qsizetype p = 12; p + 8 <= d.size();) {
    const QByteArray id = d.mid(p, 4);
    quint32 len;
    std::memcpy(&len, d.constData() + p + 4, 4);
    if (id == "fmt " && p + 8 + 16 <= d.size()) {
      quint16 ch, bs;
      quint32 sr;
      std::memcpy(&ch, d.constData() + p + 10, 2);
      std::memcpy(&sr, d.constData() + p + 12, 4);
      std::memcpy(&bs, d.constData() + p + 22, 2);
      channels = ch;
      rate = static_cast<int>(sr);
      bits = bs;
    } else if (id == "data") {
      dataAt = p + 8;
      dataLen = std::min<qsizetype>(len, d.size() - dataAt);
      break;
    }
    p += 8 + qsizetype(len) + (len & 1);
  }
  if (dataAt < 0 || bits != 16 || channels < 1 || rate < 8000) return false;
  const qsizetype n = dataLen / (2 * channels);
  std::vector<int16_t> mono(static_cast<size_t>(n));
  for (qsizetype i = 0; i < n; ++i) std::memcpy(&mono[static_cast<size_t>(i)], d.constData() + dataAt + i * 2 * channels, 2);
  const qsizetype outN = static_cast<qsizetype>(double(n) * uzume::kVoiceRate / rate);
  if (outN < uzume::kVoiceFrame) return false;
  for (qsizetype off = 0; off + uzume::kVoiceFrame <= outN && inject_.size() < 50 * 60; off += uzume::kVoiceFrame) {
    std::vector<int16_t> v(uzume::kVoiceFrame);
    for (int i = 0; i < uzume::kVoiceFrame; ++i) v[static_cast<size_t>(i)] = mono[std::min<size_t>(mono.size() - 1, static_cast<size_t>(double(off + i) * rate / uzume::kVoiceRate))];
    inject_.push_back(std::move(v));
  }
  if (!injectTimer_.isActive()) injectTimer_.start();
  emit micChanged();
  sendWireState();
  return true;
}

void VoiceController::toggleMic() {
  switch (micState()) {
    case MicState::MutedByHost:
      showNotice(tr("The host muted you. Only the host can undo this."));
      return;
    case MicState::Blocked:
    case MicState::NoDevice:
      retryMic();
      if (micState() == MicState::Blocked || micState() == MicState::NoDevice) return;
      break;
    default:
      break;
  }
  if (!settings_) return;
  settings_->setVoiceMicOn(!micOn());
  pttHeld_ = false;
  activity_.reset();
  emit micChanged();
  sendWireState();
}

void VoiceController::setMicMode(const QString& mode) {
  if (!settings_) return;
  if (mode == QLatin1String("off")) {
    settings_->setVoiceMicOn(false);
  } else if (mode == QLatin1String("ptt") || mode == QLatin1String("on")) {
    settings_->setVoiceMicOn(true);
    settings_->setVoiceTalkMode(mode == QLatin1String("on") ? QStringLiteral("vad") : QStringLiteral("ptt"));
  } else {
    return;
  }
  pttHeld_ = false;
  activity_.reset();
  emit settingsChanged();
  emit micChanged();
  sendWireState();
}

void VoiceController::talkKey(bool down) {
  if (talkMode() == QLatin1String("vad")) {
    if (down) toggleMic();  // with voice activation the key turns the mic on or off
    return;
  }
  if (pttHeld_ == down) return;
  pttHeld_ = down;
  emit micChanged();
}

void VoiceController::micAction() {
  if (!want_) {
    join();
    return;
  }
  switch (micState()) {
    case MicState::Blocked: openSystemSettings(); break;
    case MicState::NoDevice: retryMic(); break;
    default: toggleMic(); break;
  }
}

void VoiceController::sendWireState() {
  const QString mode = micMode();
  if (!want_ || !joined_ || mode == lastWireMode_) return;
  lastWireMode_ = mode;
  if (socket_) socket_->sendVoiceState(roomKind_, roomId_, mode);
}

// ---------------------------------------------------------------- the room

void VoiceController::join() {
  if (want_ || !scopeFn_ || !featureOn()) return;
  const TogetherController::Scope scope = scopeFn_();
  if (scope.kind.isEmpty()) return;
  want_ = true;
  roomKind_ = scope.kind;
  roomId_ = scope.id;
  manualLeft_.remove(scopeKey(scope));
  firstRoomSeen_ = false;
  engine_.setSelf(selfFn_ ? selfFn_() : QString(), deviceFn_ ? deviceFn_() : QString());
  if (socket_) engine_.setIceServers(socket_->helloAck().iceServers, socket_->helloAck().turnServers);
  if (mixer_ && !mixerHeld_) mixerHeld_ = mixer_->acquire(uzume::kVoiceRate);
  updateTap();
  startCaptureIfNeeded();
  sendJoin();
  emit stateChanged();
  emit micChanged();
}

void VoiceController::sendJoin() {
  lastWireMode_ = micMode();
  if (socket_) joinRequestId_ = socket_->sendVoiceJoin(roomKind_, roomId_, lastWireMode_);
}

void VoiceController::leave() { leaveRoom(true); }

void VoiceController::leaveRoom(bool manual) {
  if (!want_) return;
  if (socket_) socket_->sendVoiceLeave(roomKind_, roomId_);
  if (manual) manualLeft_.insert(roomKind_ + QLatin1Char(':') + roomId_);
  want_ = false;
  joined_ = false;
  mutedByHost_ = false;
  members_.clear();
  firstRoomSeen_ = false;
  engine_.close();
  dropInputs();
  if (mixer_ && mixer_->ducking()) mixer_->setDucking(false);
  if (mixer_ && mixerHeld_) {
    mixer_->release();
    mixerHeld_ = false;
  }
  stopCaptureIfIdle();
  updateTap();
  roomKind_.clear();
  roomId_.clear();
  emit stateChanged();
  emit membersChanged();
  emit micChanged();
}

void VoiceController::refresh() {
  if (!scopeFn_) return;
  const TogetherController::Scope scope = scopeFn_();
  if (want_ && (!featureOn() || scope.kind != roomKind_ || scope.id != roomId_)) leaveRoom(false);
  if (!want_ && !scope.kind.isEmpty() && featureOn() && autoJoin() && autoFn_ && autoFn_(scope) && !manualLeft_.contains(scopeKey(scope)) &&
      socket_ && socket_->isOpen()) {
    join();
  }
  emit stateChanged();
}

void VoiceController::onSocketState(HubSocket::State s) {
  if (s != HubSocket::State::Open && want_) {  // the Hub no longer lists me; the next voice_room says when it does again
    joined_ = false;
    firstRoomSeen_ = false;
  }
  if (s == HubSocket::State::Open && want_) {
    // The Hub drops a device from its rooms when the link goes, and the others rebuild their peers for the rejoined device: close
    // mine too, so the voice_room that answers the join builds them again (an offerer would never re-offer on an old connection).
    engine_.close();
    engine_.setIceServers(socket_->helloAck().iceServers, socket_->helloAck().turnServers);
    sendJoin();
  }
  if (s == HubSocket::State::Open) refresh();
  emit stateChanged();
}

void VoiceController::onHubError(const QString& requestId, const QString& code, const QString& message) {
  Q_UNUSED(message);
  if (!want_ || joined_ || requestId.isEmpty() || requestId != joinRequestId_) return;
  if (code == QLatin1String("conflict")) {
    showNotice(tr("The voice room is full."));
  } else if (code == QLatin1String("forbidden") || code == QLatin1String("not_found") || code == QLatin1String("bad_request")) {
    showNotice(tr("Voice is not available here."));
  } else {
    return;
  }
  leaveRoom(false);
}

void VoiceController::showNotice(const QString& text) {
  notice_ = text;
  noticeTimer_.start();
  emit noticeChanged();
}

void VoiceController::onVoiceRoom(const VoiceRoomMessage& room) {
  if (!want_ || room.scope != roomKind_ || room.id != roomId_) return;
  const QString self = deviceFn_ ? deviceFn_() : QString();
  QList<Member> members;
  const Member* me = nullptr;
  for (const VoiceMember& m : room.members) members.append({m.userId, m.deviceId, m.mic, m.mutedByHost});
  for (const Member& m : std::as_const(members)) {
    if (m.deviceId == self) me = &m;
  }
  if (!me) {  // the room is closed, or I was removed from it
    leaveRoom(false);
    return;
  }
  const QString selfUser = selfFn_ ? selfFn_() : QString();
  if (firstRoomSeen_) {
    for (const Member& m : std::as_const(members)) {
      if (m.deviceId == self || m.userId == selfUser) continue;
      const bool known = std::any_of(members_.cbegin(), members_.cend(), [&](const Member& o) { return o.deviceId == m.deviceId; });
      if (!known) emit memberJoined(nameOf(m.userId));
    }
  }
  firstRoomSeen_ = true;
  const bool wasMuted = mutedByHost_;
  mutedByHost_ = me->mutedByHost;
  joined_ = true;
  applyMembers(members);
  if (wasMuted != mutedByHost_) emit micChanged();
  emit stateChanged();
}

void VoiceController::applyMembers(const QList<Member>& members) {
  members_ = members;
  QList<VoiceEngine::Member> em;
  for (const Member& m : members) em.append({m.userId, m.deviceId});
  engine_.setMembers(em);
  // Drop the mixer inputs of devices that left.
  QSet<QString> alive;
  for (const Member& m : members) alive.insert(m.deviceId);
  const QStringList have = voiceIn_.keys();
  for (const QString& d : have) {
    if (alive.contains(d)) continue;
    if (mixer_ && voiceIn_.value(d).input != 0) mixer_->removeVoiceInput(voiceIn_.value(d).input);
    voiceIn_.remove(d);
  }
  if (mixer_ && settings_) {
    for (const Member& m : members) mixer_->setPersonGain(AudioMixer::Voice, m.userId, settings_->voicePersonGain(m.userId) / 100.0);
  }
  updateDucking();
  emit membersChanged();
}

void VoiceController::onVoiceSignal(const QString& scope, const QString& id, const QString& from, const QJsonObject& body) {
  if (!want_ || scope != roomKind_ || id != roomId_ || from.isEmpty()) return;
  engine_.handleSignal(from, body);
}

void VoiceController::dropInputs() {
  if (mixer_) {
    for (auto it = voiceIn_.cbegin(); it != voiceIn_.cend(); ++it) {
      if (it.value().input != 0) mixer_->removeVoiceInput(it.value().input);
    }
  }
  voiceIn_.clear();
}

void VoiceController::onPcm(const QString& deviceId, const QByteArray& mono16) {
  if (!mixer_) return;
  // The host's mute is enforced here on receive: the sender's app may be modified or not know about it yet.
  for (const Member& m : std::as_const(members_)) {
    if (m.deviceId == deviceId && m.mutedByHost) return;
  }
  auto it = voiceIn_.find(deviceId);
  if (it == voiceIn_.end()) it = voiceIn_.insert(deviceId, VoiceIn());
  VoiceIn& v = it.value();
  if (v.input == 0) {
    for (const Member& m : std::as_const(members_)) {
      if (m.deviceId == deviceId) v.userId = m.userId;
    }
    v.input = mixer_->addVoiceInput(v.userId);
  }
  const int rate = mixer_->outputRate();
  if (rate <= 0) return;
  const qsizetype n = mono16.size() / 2;
  QByteArray stereo(n * 4, Qt::Uninitialized);
  const auto* src = reinterpret_cast<const int16_t*>(mono16.constData());
  auto* dst = reinterpret_cast<int16_t*>(stereo.data());
  for (qsizetype i = 0; i < n; ++i) dst[2 * i] = dst[2 * i + 1] = src[i];
  if (rate != uzume::kVoiceRate) {
    if (v.resampler.isPassthrough()) v.resampler.setRates(uzume::kVoiceRate, rate);
    stereo = v.resampler.process(stereo);
  }
  mixer_->push(v.input, stereo);
}

void VoiceController::onSpeaking(const QString& deviceId, bool speaking) {
  const auto it = voiceIn_.find(deviceId);
  if (it != voiceIn_.end()) {
    it.value().speaking = speaking;
  } else if (speaking) {
    VoiceIn v;
    v.speaking = true;
    for (const Member& m : std::as_const(members_)) {
      if (m.deviceId == deviceId) v.userId = m.userId;
    }
    voiceIn_.insert(deviceId, std::move(v));  // no mixer input yet: the first audio creates it (see onPcm)
  }
  updateDucking();
  emit membersChanged();
}

void VoiceController::updateDucking() {
  if (!mixer_) return;
  mixer_->setDucking(want_ && ducking() && someoneSpeaking());
}

bool VoiceController::someoneSpeaking() const {
  return std::any_of(voiceIn_.cbegin(), voiceIn_.cend(), [](const VoiceIn& v) { return v.speaking; });
}

bool VoiceController::canMuteOthers() const {
  return want_ && roomKind_ == QLatin1String("session") && ownerFn_ && ownerFn_(roomId_);
}

void VoiceController::mute(const QString& userId, bool muted) {
  if (!canMuteOthers() || userId.isEmpty() || !socket_) return;
  socket_->sendVoiceMute(roomId_, userId, muted);
}

void VoiceController::setPersonGain(const QString& userId, int percent) {
  if (!settings_ || userId.isEmpty()) return;
  settings_->setVoicePersonGain(userId, percent);
  if (mixer_) mixer_->setPersonGain(AudioMixer::Voice, userId, settings_->voicePersonGain(userId) / 100.0);
  emit membersChanged();
}

int VoiceController::personGain(const QString& userId) const { return settings_ ? settings_->voicePersonGain(userId) : 100; }

QVariantList VoiceController::members() const {
  QVariantList out;
  const QString self = deviceFn_ ? deviceFn_() : QString();
  for (const Member& m : members_) {
    const bool me = m.deviceId == self;
    bool speaking = me ? selfSpeaking_ : false;
    if (!me) {
      const auto it = voiceIn_.constFind(m.deviceId);
      speaking = it != voiceIn_.cend() && it->speaking;
    }
    const QString voice = m.mutedByHost ? QStringLiteral("mutedByHost") : (m.mic == QLatin1String("off") ? QStringLiteral("micOff") : (speaking ? QStringLiteral("speaking") : QString()));
    QString connection, state;
    for (const VoiceEngine::PeerInfo& p : engine_.peers()) {
      if (p.deviceId == m.deviceId) {
        connection = p.connection;
        state = p.state;
      }
    }
    out.append(QVariantMap{{QStringLiteral("userId"), m.userId},
                           {QStringLiteral("deviceId"), m.deviceId},
                           {QStringLiteral("name"), nameOf(m.userId)},
                           {QStringLiteral("me"), me},
                           {QStringLiteral("mic"), m.mic},
                           {QStringLiteral("mutedByHost"), m.mutedByHost},
                           {QStringLiteral("speaking"), speaking},
                           {QStringLiteral("voice"), voice},
                           {QStringLiteral("gain"), personGain(m.userId)},
                           {QStringLiteral("connection"), connection},
                           {QStringLiteral("state"), state}});
  }
  return out;
}

QVariantMap VoiceController::marks() const {
  QVariantMap out;
  const QVariantMap info = memberInfo();
  for (auto it = info.cbegin(); it != info.cend(); ++it) {
    const QString v = it.value().toMap().value(QStringLiteral("voice")).toString();
    if (!v.isEmpty()) out.insert(it.key(), v);
  }
  return out;
}

QVariantMap VoiceController::memberInfo() const {
  QVariantMap out;
  for (const QVariant& v : members()) {
    const QVariantMap m = v.toMap();
    const QString id = m.value(QStringLiteral("userId")).toString();
    QVariantMap cur = out.value(id).toMap();
    if (cur.isEmpty()) {
      cur = m;
    } else if (m.value(QStringLiteral("speaking")).toBool() && cur.value(QStringLiteral("voice")).toString() != QLatin1String("mutedByHost")) {
      cur = m;  // several devices of one person: the one that speaks counts
    }
    out.insert(id, cur);
  }
  return out;
}

QVariantMap VoiceController::debugState() const {
  QVariantList list;
  for (const QVariant& v : members()) {
    const QVariantMap m = v.toMap();
    list.append(QVariantMap{{QStringLiteral("user_id"), m.value(QStringLiteral("userId"))},
                            {QStringLiteral("name"), m.value(QStringLiteral("name"))},
                            {QStringLiteral("device_id"), m.value(QStringLiteral("deviceId"))},
                            {QStringLiteral("me"), m.value(QStringLiteral("me"))},
                            {QStringLiteral("mic"), m.value(QStringLiteral("mic"))},
                            {QStringLiteral("muted_by_host"), m.value(QStringLiteral("mutedByHost"))},
                            {QStringLiteral("speaking"), m.value(QStringLiteral("speaking"))},
                            {QStringLiteral("connection"), m.value(QStringLiteral("connection"))},
                            {QStringLiteral("link"), m.value(QStringLiteral("state"))}});
  }
  TogetherController::Scope scope;
  if (scopeFn_) scope = scopeFn_();
  return QVariantMap{{QStringLiteral("available"), available()},
                     {QStringLiteral("joined"), joined_},
                     {QStringLiteral("wanted"), want_},
                     {QStringLiteral("room"), QVariantMap{{QStringLiteral("scope"), roomKind_}, {QStringLiteral("id"), roomId_}}},
                     {QStringLiteral("scope"), QVariantMap{{QStringLiteral("scope"), scope.kind}, {QStringLiteral("id"), scope.id}}},
                     {QStringLiteral("mic_state"), micStateText()},
                     {QStringLiteral("mic_mode"), micMode()},
                     {QStringLiteral("mic_on"), micOn()},
                     {QStringLiteral("talk_mode"), talkMode()},
                     {QStringLiteral("level_db"), std::round(levelDb_ * 10.0) / 10.0},
                     {QStringLiteral("threshold_db"), thresholdDb()},
                     {QStringLiteral("self_speaking"), selfSpeaking_},
                     {QStringLiteral("muted_by_host"), mutedByHost_},
                     {QStringLiteral("injecting"), injecting()},
                     {QStringLiteral("ducking_active"), mixer_ && mixer_->ducking()},
                     {QStringLiteral("capture"), capture_->status() == VoiceCapture::Status::Running ? QStringLiteral("running")
                                                  : captureBlocked_ ? QStringLiteral("blocked") : captureNoDevice_ ? QStringLiteral("no_device") : QStringLiteral("stopped")},
                     {QStringLiteral("notice"), notice_},
                     {QStringLiteral("members"), list}};
}

}  // namespace uzume::ui
