#include "sessionviewer.h"

#include <QLoggingCategory>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <rtc/rtc.hpp>
#include <variant>

#include "hoststate.h"
#include "opusdepacketizer.h"
#include "rtcutil.h"
#include "viewerbacklog.h"

namespace uzume {

namespace {
Q_LOGGING_CATEGORY(lcViewer, "uzume.sessionviewer")

QString stateName(rtc::PeerConnection::State s) {
  using S = rtc::PeerConnection::State;
  switch (s) {
    case S::New: return QStringLiteral("new");
    case S::Connecting: return QStringLiteral("connecting");
    case S::Connected: return QStringLiteral("connected");
    case S::Disconnected: return QStringLiteral("disconnected");
    case S::Failed: return QStringLiteral("failed");
    case S::Closed: return QStringLiteral("closed");
  }
  return QStringLiteral("new");
}

constexpr int kBytesPerMs = 48 * 4;  // 48 kHz stereo int16
constexpr size_t kMaxQueuedVideo = 6;    // access units waiting for the decoder
constexpr size_t kMaxQueuedAudio = 100;  // Opus packets (about 2 s)
constexpr size_t kMaxQueuedFrames = 3;   // decoded pictures waiting for the UI thread
}  // namespace

struct SessionViewer::MediaPipe {
  std::mutex mutex;
  std::condition_variable cv;
  VideoBacklog video{kMaxQueuedVideo};
  std::deque<QByteArray> audio;
  std::deque<QImage> frames;  // decoded, waiting for the UI thread
  bool stop = false;
  bool framePosted = false;   // a delivery to the UI thread is queued
  bool pliPosted = false;     // a keyframe request is queued
};

// RTCP receiving session that also exposes the loss derived from RTP sequence numbers (RFC 3550 A.3).
class LossReceivingSession : public rtc::RtcpReceivingSession {
 public:
  void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override {
    rtc::RtcpReceivingSession::incoming(messages, send);
    std::lock_guard<std::mutex> lock(statMutex_);
    if (mReceived > 0) {
      expected_ = static_cast<int64_t>(mCycles) + mMaxSeq - mBaseSeq + 1;
      received_ = mReceived;
    }
  }
  // Cumulative counters (0/0 until the first packet).
  void counters(int64_t& expected, int64_t& received) {
    std::lock_guard<std::mutex> lock(statMutex_);
    expected = expected_;
    received = received_;
  }
  // nullopt until enough packets were seen.
  std::optional<double> lossPercent() {
    std::lock_guard<std::mutex> lock(statMutex_);
    if (expected_ < 50) {
      return std::nullopt;
    }
    const int64_t lost = std::max<int64_t>(0, expected_ - received_);
    return 100.0 * static_cast<double>(lost) / static_cast<double>(expected_);
  }

 private:
  std::mutex statMutex_;
  int64_t expected_ = 0;
  int64_t received_ = 0;
};

SessionViewer::SessionViewer(QObject* parent) : QObject(parent), forceRelay_(forceRelayFromEnv()), bridge_(std::make_shared<ThreadBridge>(this)) {
  statsTimer_.setInterval(1000);
  connect(&statsTimer_, &QTimer::timeout, this, &SessionViewer::updateStats);
  pingTimer_.setInterval(2000);
  connect(&pingTimer_, &QTimer::timeout, this, &SessionViewer::sendPing);
  pingClock_.start();
  inputClock_.start();
  inputKeepAlive_.setInterval(kInputKeepAliveMs);
  connect(&inputKeepAlive_, &QTimer::timeout, this, &SessionViewer::sendInputFrame);
  inputCoalesce_.setSingleShot(true);
  inputCoalesce_.setInterval(16);
  connect(&inputCoalesce_, &QTimer::timeout, this, &SessionViewer::sendInputFrame);
}

SessionViewer::~SessionViewer() {
  bridge_->detach();
  teardown();
}

void SessionViewer::open(const QString& sessionId, const QString& viewerId, const QStringList& iceServers,
                         const QList<TurnServer>& turnServers) {
  close();
  sessionId_ = sessionId;
  viewerId_ = viewerId;
  iceServers_ = iceServers;
  turnServers_ = turnServers;
  relayTcpOnly_ = turnServersTcpOnly(turnServers);
  open_ = true;
  remoteSet_ = false;
  ice_ = IceTrace();
  pendingCandidates_.clear();
  totalFrames_ = totalAudioFrames_ = totalVideoBytes_ = totalAudioBytes_ = 0;
  lastFrames_ = lastVideoBytes_ = lastAudioBytes_ = 0;
  decodeErrors_ = 0;
  droppedVideo_ = 0;
  pliCount_ = 0;
  lastExpected_ = lastReceived_ = 0;
  audioPeak_ = 0;
  pliSent_ = false;
  stats_ = SessionStats();
  link_ = ViewerLinkStats();
  link_.viewerId = viewerId;
  link_.state = QStringLiteral("new");
  {
    QMutexLocker lock(&audioMutex_);
    audioBuf_.clear();
    audioPrimed_ = false;
  }
  if (!decoder_.open()) {
    emit errorOccurred(QStringLiteral("No H.264 decoder available"));
  }
  opus_.open();
  codecsOpen_ = true;
  statsClock_.restart();
  statsTimer_.start();
}

void SessionViewer::onHostState(bool paused, const QString& by) {
  if (paused == hostPaused_ && by == hostPausedBy_) return;
  hostPaused_ = paused;
  hostPausedBy_ = by;
  emit hostStateChanged(paused, by);
}

void SessionViewer::sendDiag(const QByteArray& msg) {
  if (!diag_) return;
  try {
    if (diag_->isOpen()) diag_->send(std::string(msg.constData(), static_cast<size_t>(msg.size())));
  } catch (const std::exception& e) {
    qCWarning(lcViewer) << "diag send:" << e.what();
  }
}

void SessionViewer::setQuality(Quality pick) {
  if (pick == pick_) return;
  pick_ = pick;
  sendDiag(makeQualityPick(pick));
  emit qualityChanged();
}

bool SessionViewer::requestControl(int port) {
  if (port < 0 || port > 1 || port >= hostPorts_ || controlState_ != ControlState::Idle || !diag_ || !diag_->isOpen()) return false;
  controlState_ = ControlState::Asking;
  controlPort_ = port;
  controlReason_.clear();
  sendDiag(makeControlRequest(port));
  emit controlChanged();
  return true;
}

void SessionViewer::releaseControl() {
  if (controlState_ == ControlState::Idle) return;
  sendDiag(makeControlRelease());
  resetControl(QStringLiteral("viewer"));
}

void SessionViewer::resetControl(const QString& reason) {
  inputKeepAlive_.stop();
  inputCoalesce_.stop();
  inputState_ = InputFrame();
  if (controlState_ == ControlState::Idle) return;
  controlState_ = ControlState::Idle;
  controlPort_ = -1;
  controlReason_ = reason;
  emit controlChanged();
}

void SessionViewer::onControlAnswer(const ControlAnswer& a) {
  if (controlState_ != ControlState::Asking || a.port != controlPort_) return;
  if (!a.granted) {
    resetControl(a.reason.isEmpty() ? QStringLiteral("denied") : a.reason);
    return;
  }
  controlState_ = ControlState::Granted;
  controlReason_.clear();
  inputState_ = InputFrame();
  inputState_.port = static_cast<quint8>(a.port);
  inputKeepAlive_.start();
  sendInputFrame();  // the first frame tells the host the channel works
  emit controlChanged();
}

void SessionViewer::onControlEnd(const ControlEnd& e) {
  if (controlState_ == ControlState::Idle || e.port != controlPort_) return;
  resetControl(e.reason);
}

void SessionViewer::setControlInput(quint16 buttons, const qint16 axes[4], bool touching, double touchX, double touchY) {
  if (controlState_ != ControlState::Granted) return;
  InputFrame next = inputState_;
  next.buttons = buttons;
  for (int i = 0; i < 4; ++i) next.axes[i] = axes[i];
  next.touching = touching;
  const auto q = [](double v) { return static_cast<qint16>(std::lround(std::clamp(v, 0.0, 1.0) * kTouchMax)); };
  next.touchX = touching ? q(touchX) : inputState_.touchX;
  next.touchY = touching ? q(touchY) : inputState_.touchY;
  if (next.sameInput(inputState_)) return;
  inputState_ = next;
  // At most one frame per emulated frame: a change right after a send waits for the coalescing timer.
  if (inputClock_.elapsed() - lastInputSendMs_ >= 16) sendInputFrame();
  else if (!inputCoalesce_.isActive()) inputCoalesce_.start();
}

void SessionViewer::sendInputFrame() {
  if (controlState_ != ControlState::Granted || !input_) return;
  InputFrame f = inputState_;
  f.seq = ++inputSeq_;
  f.tsMs = static_cast<quint16>(inputClock_.elapsed() & 0xFFFF);
  lastInputSendMs_ = inputClock_.elapsed();
  inputCoalesce_.stop();
  try {
    if (input_->isOpen()) {
      const QByteArray b = encodeInputFrame(f);
      input_->send(reinterpret_cast<const std::byte*>(b.constData()), static_cast<size_t>(b.size()));
      ++inputFramesSent_;
    }
  } catch (const std::exception& e) {
    qCWarning(lcViewer) << "input send:" << e.what();
  }
}

void SessionViewer::sendPing() { sendDiag(makePing(++pingN_, pingClock_.elapsed())); }

void SessionViewer::onPingPong(const PingMessage& m) {
  if (!m.pong) {
    sendDiag(makePong(m.n, m.ts));  // the host measures its own latency
    return;
  }
  if (const auto rtt = rttFromPong(pingClock_.elapsed(), m.ts)) {
    latencyMs_ = *rtt;
    emit qualityChanged();
  }
}

void SessionViewer::onTier(const TierMessage& m) {
  if (hasTier_ && servedTier_ == m.tier && servedKbps_ == m.kbps) return;
  hasTier_ = true;
  servedTier_ = m.tier;
  servedKbps_ = m.kbps;
  emit qualityChanged();
}

void SessionViewer::teardown() {
  ++pcGen_;
  if (hostPaused_) {
    hostPaused_ = false;
    hostPausedBy_.clear();
    emit hostStateChanged(false, QString());
  }  // invalidates queued frames of this connection before the codecs are closed
  statsTimer_.stop();
  pingTimer_.stop();
  resetControl(QStringLiteral("left"));
  if (hostPorts_ != 0) {
    hostPorts_ = 0;
    emit controlChanged();
  }
  if (hasTier_ || latencyMs_) {
    hasTier_ = false;
    servedTier_ = Quality::Auto;
    servedKbps_ = 0;
    latencyMs_.reset();
    emit qualityChanged();
  }
  if (pc_) {
    try {
      pc_->resetCallbacks();
      if (video_) video_->resetCallbacks();
      if (audio_) audio_->resetCallbacks();
      if (diag_) diag_->resetCallbacks();
      if (input_) input_->resetCallbacks();
      pc_->close();
    } catch (const std::exception& e) {
      qCWarning(lcViewer) << "Closing PeerConnection:" << e.what();
    }
  }
  video_.reset();
  audio_.reset();
  diag_.reset();
  input_.reset();
  videoSession_.reset();
  pc_.reset();
  if (pipe_) {  // the producers are gone; the worker finishes its current picture and ends before the codecs are closed
    {
      std::lock_guard<std::mutex> lock(pipe_->mutex);
      pipe_->stop = true;
    }
    pipe_->cv.notify_all();
  }
  if (worker_.joinable()) worker_.join();
  pipe_.reset();
  decoder_.close();
  opus_.close();
  codecsOpen_ = false;
}

void SessionViewer::startMedia() {
  if (!codecsOpen_) {  // a previous connection of this open viewer was torn down
    if (!decoder_.open()) {
      emit errorOccurred(QStringLiteral("No H.264 decoder available"));
    }
    opus_.open();
    codecsOpen_ = true;
  }
  if (!statsTimer_.isActive()) {
    statsClock_.restart();
    statsTimer_.start();
  }
  pipe_ = std::make_shared<MediaPipe>();
  worker_ = std::thread([this, pipe = pipe_, gen = pcGen_]() { mediaLoop(pipe, gen); });
}

void SessionViewer::close() {
  const bool wasConnected = connected_;
  teardown();
  open_ = false;
  connected_ = false;
  remoteSet_ = false;
  pcState_ = QStringLiteral("closed");
  link_.state = pcState_;
  if (wasConnected) {
    emit connectedChanged(false);
  }
}

// ---------------------------------------------------------------- signaling

void SessionViewer::handleSignal(const SessionSignal& s) {
  if (!open_ || s.viewerId != viewerId_ || s.sessionId != sessionId_) {
    return;
  }
  try {
    if (s.kind == QLatin1String("offer")) {
      if (pc_) {
        return;  // renegotiation is not part of the PoC
      }
      pc_ = std::make_shared<rtc::PeerConnection>(makeRtcConfig(iceServers_, turnServers_, forceRelay_));
      const auto bridge = bridge_;
      const unsigned gen = pcGen_;  // frames of an older connection that are still queued must not reach the codecs
      startMedia();
      const std::shared_ptr<MediaPipe> pipe = pipe_;
      pc_->onTrack(guarded("onTrack", [this, bridge, gen, pipe](std::shared_ptr<rtc::Track> track) {
        const std::string type = track->description().type();
        if (type == "video") {
          auto depack = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence);
          auto session = std::make_shared<LossReceivingSession>();
          depack->addToChain(session);
          track->setMediaHandler(depack);
          track->onFrame(guarded("onFrame", [this, bridge, gen, pipe](rtc::binary data, rtc::FrameInfo) {
            QByteArray ba(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
            totalVideoBytes_ += ba.size();
            const bool key = startsKeyframe(ba);
            bool wantPli = false;
            {
              std::lock_guard<std::mutex> lock(pipe->mutex);
              const VideoBacklog::PushResult r = pipe->video.push(std::move(ba), key);  // behind: resync on the next keyframe
              droppedVideo_ += static_cast<qint64>(r.dropped);
              if (r.wantKeyframe && !pipe->pliPosted) {
                pipe->pliPosted = true;
                wantPli = true;
              }
            }
            pipe->cv.notify_one();
            if (wantPli) {
              bridge->post([this, gen, pipe]() {
                {
                  std::lock_guard<std::mutex> lock(pipe->mutex);
                  pipe->pliPosted = false;
                }
                if (gen == pcGen_) requestKeyframe();
              });
            }
          }));
          bridge->post([this, track, session]() {
            if (open_ && pc_) {
              video_ = track;
              videoSession_ = session;
            }
          });
        } else if (type == "audio") {
          auto depack = std::make_shared<OpusRtpDepacketizer>();
          depack->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
          track->setMediaHandler(depack);
          track->onFrame(guarded("onFrame", [pipe](rtc::binary data, rtc::FrameInfo) {
            QByteArray ba(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
            {
              std::lock_guard<std::mutex> lock(pipe->mutex);
              if (pipe->audio.size() >= kMaxQueuedAudio) pipe->audio.pop_front();  // oldest audio goes first
              pipe->audio.push_back(std::move(ba));
            }
            pipe->cv.notify_one();
          }));
          bridge->post([this, track]() {
            if (open_ && pc_) {
              audio_ = track;
            }
          });
        }
      }));
      pc_->onLocalDescription(guarded("onLocalDescription", [this, bridge](rtc::Description d) {
        SessionSignal out;
        out.kind = QString::fromStdString(d.typeString());
        out.sdp = QString::fromStdString(std::string(d));
        bridge->post([this, out]() mutable {
          out.sessionId = sessionId_;
          out.viewerId = viewerId_;
          emit signalOut(out);
        });
      }));
      pc_->onLocalCandidate(guarded("onLocalCandidate", [this, bridge, gen](rtc::Candidate c) {
        SessionSignal out;
        const rtc::Candidate::Type type = c.type();
        out.kind = QStringLiteral("candidate");
        out.candidate = QString::fromStdString(c.candidate());
        out.mid = QString::fromStdString(c.mid());
        bridge->post([this, out, type, gen]() mutable {
          if (gen == pcGen_) countCandidate(ice_.local, type);
          out.sessionId = sessionId_;
          out.viewerId = viewerId_;
          emit signalOut(out);
        });
      }));
      pc_->onGatheringStateChange(guarded("onGatheringStateChange", [this, bridge, gen](rtc::PeerConnection::GatheringState st) {
        if (st == rtc::PeerConnection::GatheringState::Complete) {
          bridge->post([this, gen]() {
            if (gen == pcGen_) onGatheringComplete();
          });
        }
      }));
      pc_->onStateChange(guarded("onStateChange", [this, bridge, gen](rtc::PeerConnection::State st) {
        bridge->post([this, st, gen]() {
          if (gen == pcGen_) onPcState(static_cast<int>(st));  // a state of a torn-down connection must not end a new one
        });
      }));
      // The answer is produced automatically (onLocalDescription) once the offer is applied.
      pc_->setRemoteDescription(rtc::Description(s.sdp.toStdString(), rtc::Description::Type::Offer));
      remoteSet_ = true;
      // Negotiated data channel matching the host's (same id, no protocol change): brings up SCTP so rtt() works.
      // Created after the remote offer: before it, libdatachannel would start an offer of its own.
      rtc::DataChannelInit diagInit;
      diagInit.negotiated = true;
      diagInit.id = 0;
      diag_ = pc_->createDataChannel("fb-diag", diagInit);
      // Host state signal {"t":"state","paused":..,"by":..} (ADR 0026 D3); every other message is ignored.
      diag_->onMessage(guarded("diag onMessage", [this, bridge](rtc::message_variant m) {
        const auto* text = std::get_if<rtc::string>(&m);
        if (text == nullptr) return;
        const QByteArray raw(text->data(), static_cast<qsizetype>(text->size()));
        if (const auto state = parseHostState(raw)) {
          bridge->post([this, st = *state]() { onHostState(st.paused, st.by); });
        } else if (const auto tier = parseTier(raw)) {
          bridge->post([this, t = *tier]() { onTier(t); });
        } else if (const auto ping = parsePingPong(raw)) {
          bridge->post([this, p = *ping]() { onPingPong(p); });
        } else if (const auto caps = parseControlCaps(raw)) {
          bridge->post([this, ports = *caps]() {
            if (ports == hostPorts_) return;
            hostPorts_ = ports;
            emit controlChanged();
          });
        } else if (const auto answer = parseControlAnswer(raw)) {
          bridge->post([this, a = *answer]() { onControlAnswer(a); });
        } else if (const auto end = parseControlEnd(raw)) {
          bridge->post([this, e = *end]() { onControlEnd(e); });
        }  // anything else is ignored
      }));
      // Controller input frames (ADR 0033 D8): the host's twin of this channel; an old host has none and never grants.
      rtc::DataChannelInit inputInit;
      inputInit.negotiated = true;
      inputInit.id = kInputChannelId;
      inputInit.reliability.unordered = true;
      inputInit.reliability.maxRetransmits = 0;
      input_ = pc_->createDataChannel("fb-input", inputInit);
      // Pick (when not the default) and the first ping go out as soon as the channel is open.
      diag_->onOpen(guarded("diag onOpen", [this, bridge]() {
        bridge->post([this]() {
          if (pick_ != Quality::Auto) sendDiag(makeQualityPick(pick_));
          sendPing();
          pingTimer_.start();
        });
      }));
      const QList<SessionSignal> pending = std::exchange(pendingCandidates_, {});
      for (const SessionSignal& c : pending) {
        handleSignal(c);
      }
    } else if (s.kind == QLatin1String("candidate")) {
      if (!pc_ || !remoteSet_) {
        pendingCandidates_.append(s);
        return;
      }
      const rtc::Candidate cand(s.candidate.toStdString(), s.mid.toStdString());
      countCandidate(ice_.remote, cand.type());
      pc_->addRemoteCandidate(cand);
    }
  } catch (const std::exception& e) {
    if (s.kind == QLatin1String("offer") && pc_) {
      // A half-built connection would swallow every later offer (`if (pc_) return`): start over and tell the owner (PN-11).
      teardown();
      remoteSet_ = false;
      pendingCandidates_.clear();
      pcState_ = QStringLiteral("failed");
      link_.state = pcState_;
      emit errorOccurred(QStringLiteral("Signal: ") + QString::fromUtf8(e.what()));
      emit closed(pcState_);
      return;
    }
    emit errorOccurred(QStringLiteral("Signal: ") + QString::fromUtf8(e.what()));
  }
}

void SessionViewer::onGatheringComplete() {
  ice_.gatheringComplete = true;
  qCInfo(lcViewer).noquote() << QStringLiteral("ICE session %1: gathering complete, local %2").arg(sessionId_, ice_.local.text());
}

void SessionViewer::onPcState(int state) {
  if (!open_ || !pc_) {
    return;
  }
  const auto st = static_cast<rtc::PeerConnection::State>(state);
  pcState_ = stateName(st);
  link_.state = pcState_;
  if (st == rtc::PeerConnection::State::Connected) {
    if (!connected_) {
      connected_ = true;
      const QString type = connectionTypeOf(*pc_, relayTcpOnly_, forceRelay_);
      const RelayReason reason = classifyRelayReason(type, forceRelay_, ice_.local, ice_.remote);
      qCInfo(lcViewer).noquote() << QStringLiteral("ICE session %1: connected %2, pair %3, local %4, remote %5%6")
                                        .arg(sessionId_, type.isEmpty() ? QStringLiteral("(unknown)") : type, selectedPairText(*pc_),
                                             ice_.local.text(), ice_.remote.text(),
                                             reason == RelayReason::None ? QString() : QStringLiteral(", relay reason %1").arg(relayReasonToken(reason)));
      emit connectedChanged(true);
      requestKeyframe();
    }
  } else if (st == rtc::PeerConnection::State::Failed || st == rtc::PeerConnection::State::Closed ||
             st == rtc::PeerConnection::State::Disconnected) {
    if (st == rtc::PeerConnection::State::Failed) {
      qCInfo(lcViewer).noquote() << QStringLiteral("ICE session %1: failed (%2 connected), local %3, remote %4")
                                        .arg(sessionId_, connected_ ? QStringLiteral("was") : QStringLiteral("never"), ice_.local.text(),
                                             ice_.remote.text());
    }
    if (connected_) {
      connected_ = false;
      emit connectedChanged(false);
    }
    if (st != rtc::PeerConnection::State::Disconnected) {
      emit closed(pcState_);
    }
  }
}

// ---------------------------------------------------------------- media

void SessionViewer::requestKeyframe() {
  if (!video_) {
    return;
  }
  if (pliSent_ && pliClock_.elapsed() < 500) {
    return;
  }
  pliSent_ = true;
  pliClock_.restart();
  ++pliCount_;
  try {
    video_->requestKeyframe();
  } catch (const std::exception& e) {
    qCWarning(lcViewer) << "PLI:" << e.what();
  }
}

void SessionViewer::mediaLoop(std::shared_ptr<MediaPipe> pipe, unsigned gen) {
  while (true) {
    std::deque<QByteArray> audio;
    QByteArray video;
    bool haveVideo = false;
    {
      std::unique_lock<std::mutex> lock(pipe->mutex);
      pipe->cv.wait(lock, [&]() { return pipe->stop || !pipe->video.empty() || !pipe->audio.empty(); });
      if (pipe->stop) return;
      audio.swap(pipe->audio);
      if (!pipe->video.empty()) {
        video = pipe->video.pop();
        haveVideo = true;
      }
    }
    for (const QByteArray& a : audio) decodeAudio(a);
    if (haveVideo) decodeVideo(video, pipe, gen);
  }
}

void SessionViewer::decodeVideo(const QByteArray& data, const std::shared_ptr<MediaPipe>& pipe, unsigned gen) {
  std::vector<QImage> frames;
  if (!decoder_.decode(reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size()), frames)) {
    ++decodeErrors_;
    bool wantPli = false;
    {
      std::lock_guard<std::mutex> lock(pipe->mutex);
      if (!pipe->pliPosted) {
        pipe->pliPosted = true;
        wantPli = true;
      }
    }
    if (wantPli) {
      bridge_->post([this, gen, pipe]() {
        {
          std::lock_guard<std::mutex> lock(pipe->mutex);
          pipe->pliPosted = false;
        }
        if (gen == pcGen_) requestKeyframe();
      });
    }
  }
  for (QImage& f : frames) {
    ++totalFrames_;
    bool post = false;
    {
      std::lock_guard<std::mutex> lock(pipe->mutex);
      pipe->frames.push_back(std::move(f));
      while (pipe->frames.size() > kMaxQueuedFrames) {  // the UI is behind: show the newest
        pipe->frames.pop_front();
        ++droppedVideo_;
      }
      post = !pipe->framePosted;
      pipe->framePosted = true;
    }
    if (post) {
      bridge_->post([this, gen, pipe]() {
        std::deque<QImage> ready;
        {
          std::lock_guard<std::mutex> lock(pipe->mutex);
          ready.swap(pipe->frames);
          pipe->framePosted = false;
        }
        if (gen != pcGen_ || !open_) return;
        for (const QImage& img : ready) {
          width_ = img.width();
          height_ = img.height();
          emit frameReady(img);
        }
      });
    }
  }
}

void SessionViewer::decodeAudio(const QByteArray& data) {
  totalAudioBytes_ += data.size();
  const QByteArray pcm = opus_.decode(reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size()));
  if (pcm.isEmpty()) {
    return;
  }
  ++totalAudioFrames_;
  const auto* s = reinterpret_cast<const int16_t*>(pcm.constData());
  int peak = audioPeak_.load();
  for (qsizetype i = 0; i < pcm.size() / 2; ++i) {
    peak = std::max(peak, std::abs(static_cast<int>(s[i])));
  }
  audioPeak_ = peak;
  QMutexLocker lock(&audioMutex_);
  audioBuf_.append(pcm);
  const int maxBytes = kJitterMaxMs * kBytesPerMs;
  if (audioBuf_.size() > maxBytes) {  // too far behind: drop the oldest audio down to the target
    audioBuf_.remove(0, audioBuf_.size() - kJitterTargetMs * kBytesPerMs);
  }
}

QByteArray SessionViewer::pullAudio(int frames) {
  QByteArray out(frames * 4, 0);
  QMutexLocker lock(&audioMutex_);
  if (!audioPrimed_) {
    if (audioBuf_.size() < kJitterTargetMs * kBytesPerMs) {
      return out;
    }
    audioPrimed_ = true;
  }
  const qsizetype n = std::min<qsizetype>(out.size(), audioBuf_.size());
  std::memcpy(out.data(), audioBuf_.constData(), static_cast<size_t>(n));
  audioBuf_.remove(0, n);
  if (n < out.size()) {
    audioPrimed_ = false;  // underrun: refill to the target before playing on
  }
  return out;
}

int SessionViewer::bufferedAudioMs() const {
  QMutexLocker lock(&audioMutex_);
  return static_cast<int>(audioBuf_.size() / kBytesPerMs);
}

// ---------------------------------------------------------------- stats

void SessionViewer::updateStats() {
  const double secs = std::max(0.001, statsClock_.restart() / 1000.0);
  SessionStats s;
  s.active = connected_;
  s.fps = static_cast<double>(totalFrames_ - lastFrames_) / secs;
  s.videoBitrateKbps = static_cast<double>(totalVideoBytes_ - lastVideoBytes_) * 8.0 / 1000.0 / secs;
  s.audioBitrateKbps = static_cast<double>(totalAudioBytes_ - lastAudioBytes_) * 8.0 / 1000.0 / secs;
  lastFrames_ = totalFrames_;
  lastVideoBytes_ = totalVideoBytes_;
  lastAudioBytes_ = totalAudioBytes_;
  s.width = width_;
  s.height = height_;
  s.codec = QStringLiteral("H264 + Opus");
  if (connected_ && pc_) {
    s.rttMs = rttOf(*pc_);
    s.connectionType = connectionTypeOf(*pc_, relayTcpOnly_, forceRelay_);
    s.relayReason = classifyRelayReason(s.connectionType, forceRelay_, ice_.local, ice_.remote);
  }
  if (videoSession_) {
    s.packetLossPercent = videoSession_->lossPercent();
  }
  s.viewers = 1;
  link_.rttMs = s.rttMs;
  link_.connectionType = s.connectionType;
  link_.relayReason = s.relayReason;
  stats_ = s;
  sendRxReport(s.videoBitrateKbps, s.fps);
}

// ADR 0012 D5: once per second, loss (RTP sequence numbers) and received video rate go to the host over fb-diag.
void SessionViewer::sendRxReport(double videoKbps, double decodedFps) {
  if (!connected_ || !diag_ || !videoSession_) {
    return;
  }
  int64_t expected = 0, received = 0;
  videoSession_->counters(expected, received);
  const int64_t dExpected = expected - lastExpected_, dReceived = received - lastReceived_;
  lastExpected_ = expected;
  lastReceived_ = received;
  const double loss = dExpected > 0 ? std::clamp(static_cast<double>(dExpected - dReceived) / static_cast<double>(dExpected), 0.0, 1.0) : 0.0;
  try {
    if (diag_->isOpen()) {
      const QByteArray msg = makeRxReport(loss, videoKbps, decodedFps, decoder_.name());
      diag_->send(std::string(msg.constData(), static_cast<size_t>(msg.size())));
    }
  } catch (const std::exception& e) {
    qCWarning(lcViewer) << "rx report:" << e.what();
  }
}

QString SessionViewer::iceSummary() const {
  return QStringLiteral("state=%1 gathering=%2 local=%3 remote=%4")
      .arg(pcState_.isEmpty() ? QStringLiteral("new") : pcState_, ice_.gatheringComplete ? QStringLiteral("complete") : QStringLiteral("pending"), ice_.local.text(), ice_.remote.text());
}

SessionStats SessionViewer::stats() const {
  SessionStats s = stats_;
  s.active = connected_;
  s.videoFrames = totalFrames_;
  s.audioFrames = totalAudioFrames_;
  s.decodeErrors = decodeErrors_;
  s.keyframeRequests = pliCount_;
  s.decoderName = decoder_.name();
  s.width = width_;
  s.height = height_;
  s.codec = QStringLiteral("H264 + Opus");
  s.hasServedTier = hasTier_;
  s.servedTier = servedTier_;
  s.servedKbps = servedKbps_;
  s.pick = pick_;
  s.latencyMs = latencyMs_;
  return s;
}

ViewerLinkStats SessionViewer::link() const { return link_; }

}  // namespace uzume
