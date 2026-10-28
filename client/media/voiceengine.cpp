#include "voiceengine.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <algorithm>
#include <deque>
#include <mutex>

#include "opusdepacketizer.h"
#include "rtcutil.h"
#include "voicecodec.h"
#include "voicedsp.h"

namespace uzume {

Q_LOGGING_CATEGORY(lcVoice, "uzume.voice")

namespace {
constexpr int kVoicePayloadType = 111;
constexpr int kMaxRetries = 3;

qint64 nowMs() {
  static const QElapsedTimer clock = [] {
    QElapsedTimer t;
    t.start();
    return t;
  }();
  return clock.elapsed();
}

struct QueuedFrame {
  QByteArray data;
  uint32_t ts = 0;
  qint64 arrivedMs = 0;
};
}  // namespace

struct VoiceEngine::Pipe {
  std::mutex mutex;
  std::deque<QueuedFrame> frames;
  qint64 dropped = 0;
};

struct VoiceEngine::Encoder {
  VoiceEncoder enc;
};

struct VoiceEngine::Peer {
  QString deviceId;
  QString userId;
  bool offerer = false;
  unsigned gen = 0;
  int retries = 0;
  std::shared_ptr<rtc::PeerConnection> pc;
  std::shared_ptr<rtc::Track> track;
  std::shared_ptr<Pipe> pipe;
  bool remoteSet = false;
  QList<QJsonObject> pendingCandidates;
  QString state = QStringLiteral("new");
  QString connection;
  VoiceDecoder dec;
  // Jitter buffer
  bool playing = false;
  qint64 nextDueMs = 0;
  int miss = 0;
  bool haveTs = false;
  uint32_t lastTs = 0;
  // Speaking detection
  bool speaking = false;
  double levelDb = -120;
  qint64 lastLoudMs = 0;
  int loudFrames = 0;
  qint64 framesReceived = 0;
  qint64 framesConcealed = 0;
};

VoiceEngine::VoiceEngine(QObject* parent) : QObject(parent), enc_(std::make_unique<Encoder>()), bridge_(std::make_shared<ThreadBridge>(this)) {
  forceRelay_ = forceRelayFromEnv();
  decodeTimer_.setInterval(10);
  decodeTimer_.setTimerType(Qt::PreciseTimer);
  connect(&decodeTimer_, &QTimer::timeout, this, &VoiceEngine::decodeTick);
}

VoiceEngine::~VoiceEngine() {
  bridge_->detach();
  close();
}

void VoiceEngine::setSelf(const QString& userId, const QString& deviceId) {
  userId_ = userId;
  deviceId_ = deviceId;
}

void VoiceEngine::setIceServers(const QStringList& stun, const QList<TurnServer>& turn) {
  stun_ = stun;
  turn_ = turn;
}

void VoiceEngine::close() {
  decodeTimer_.stop();
  const QStringList ids = peers_.keys();
  for (const QString& id : ids) removePeer(id);
  frameCounter_ = 0;
}

void VoiceEngine::teardown(Peer& p) {
  ++p.gen;  // frames and states of the old connection that are still queued must not touch the new one
  if (p.pc) {
    try {
      p.pc->resetCallbacks();
      if (p.track) p.track->resetCallbacks();
      p.pc->close();
    } catch (const std::exception& e) {
      qCWarning(lcVoice) << "Closing voice connection:" << e.what();
    }
  }
  p.track.reset();
  p.pc.reset();
  p.pipe.reset();
  p.remoteSet = false;
  p.pendingCandidates.clear();
  p.playing = false;
  p.haveTs = false;
  p.dec.close();
}

void VoiceEngine::removePeer(const QString& deviceId) {
  const auto it = peers_.find(deviceId);
  if (it == peers_.end()) return;
  std::shared_ptr<Peer> p = it.value();
  peers_.erase(it);
  const bool wasSpeaking = p->speaking;
  teardown(*p);
  if (wasSpeaking) emit speakingChanged(deviceId, false);
  emit peerStateChanged(deviceId, QStringLiteral("closed"));
}

VoiceEngine::Peer* VoiceEngine::ensurePeer(const QString& deviceId, bool /*wantOffer*/) {
  if (deviceId.isEmpty() || deviceId == deviceId_) return nullptr;
  if (const auto it = peers_.find(deviceId); it != peers_.end()) return it.value().get();
  if (peers_.size() >= kMaxPeers) return nullptr;
  auto p = std::make_shared<Peer>();
  p->deviceId = deviceId;
  p->offerer = isOfferer(deviceId_, deviceId);
  peers_.insert(deviceId, p);
  buildPeer(*p);
  if (!decodeTimer_.isActive()) decodeTimer_.start();
  return p.get();
}

void VoiceEngine::buildPeer(Peer& p) {
  teardown(p);
  p.pipe = std::make_shared<Pipe>();
  p.dec.open();
  const unsigned gen = p.gen;
  const QString id = p.deviceId;
  const auto bridge = bridge_;
  const std::shared_ptr<Pipe> pipe = p.pipe;
  try {
    rtc::Configuration cfg = makeRtcConfig(stun_, turn_, forceRelay_);
    cfg.disableAutoNegotiation = true;
    p.pc = std::make_shared<rtc::PeerConnection>(cfg);
    const rtc::SSRC ssrc = QRandomGenerator::global()->generate();
    rtc::Description::Audio media("audio", rtc::Description::Direction::SendRecv);
    media.addOpusCodec(kVoicePayloadType);
    media.addSSRC(ssrc, "uzume-voice");
    p.track = p.pc->addTrack(media);
    auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "uzume-voice", kVoicePayloadType, OpusRtpDepacketizer::kClockRate);
    // One chain for both directions. Outgoing messages run head to tail (packetize, then count for the sender report); incoming
    // ones tail to head (RTP is checked and acknowledged by the receiving session first, then cut into frames).
    auto chain = std::make_shared<rtc::RtpPacketizer>(rtp);
    chain->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
    chain->addToChain(std::make_shared<OpusRtpDepacketizer>());
    chain->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    p.track->setMediaHandler(chain);
    p.track->onFrame(guarded("voice onFrame", [pipe](rtc::binary data, rtc::FrameInfo info) {
      QueuedFrame f;
      f.data = QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
      f.ts = info.timestamp;
      f.arrivedMs = nowMs();
      std::lock_guard<std::mutex> lock(pipe->mutex);
      if (pipe->frames.size() >= 64) {
        pipe->frames.pop_front();
        ++pipe->dropped;
      }
      pipe->frames.push_back(std::move(f));
    }));
    p.pc->onLocalDescription(guarded("voice onLocalDescription", [this, bridge, id, gen](rtc::Description d) {
      QJsonObject body{{QStringLiteral("kind"), QString::fromStdString(d.typeString())}, {QStringLiteral("sdp"), QString::fromStdString(std::string(d))}};
      bridge->post([this, id, gen, body]() {
        const auto it = peers_.find(id);
        if (it != peers_.end() && it.value()->gen == gen) emit signalOut(id, body);
      });
    }));
    p.pc->onLocalCandidate(guarded("voice onLocalCandidate", [this, bridge, id, gen](rtc::Candidate c) {
      QJsonObject body{{QStringLiteral("kind"), QStringLiteral("candidate")},
                       {QStringLiteral("candidate"), QString::fromStdString(c.candidate())},
                       {QStringLiteral("mid"), QString::fromStdString(c.mid())}};
      bridge->post([this, id, gen, body]() {
        const auto it = peers_.find(id);
        if (it != peers_.end() && it.value()->gen == gen) emit signalOut(id, body);
      });
    }));
    p.pc->onStateChange(guarded("voice onStateChange", [this, bridge, id, gen](rtc::PeerConnection::State st) {
      bridge->post([this, id, gen, st]() { onPcState(id, gen, static_cast<int>(st)); });
    }));
    if (p.offerer) p.pc->setLocalDescription(rtc::Description::Type::Offer);
  } catch (const std::exception& e) {
    qCWarning(lcVoice) << "Voice connection to" << id << "failed to start:" << e.what();
    p.state = QStringLiteral("failed");
    emit peerStateChanged(id, p.state);
  }
}

void VoiceEngine::onPcState(const QString& deviceId, unsigned gen, int state) {
  const auto it = peers_.find(deviceId);
  if (it == peers_.end() || it.value()->gen != gen) return;
  Peer& p = *it.value();
  using S = rtc::PeerConnection::State;
  QString name;
  switch (static_cast<S>(state)) {
    case S::New: name = QStringLiteral("new"); break;
    case S::Connecting: name = QStringLiteral("connecting"); break;
    case S::Connected: name = QStringLiteral("connected"); break;
    case S::Disconnected: name = QStringLiteral("connecting"); break;
    case S::Failed: name = QStringLiteral("failed"); break;
    case S::Closed: name = QStringLiteral("closed"); break;
  }
  p.state = name;
  if (name == QLatin1String("connected")) {
    p.retries = 0;
    if (p.pc) p.connection = connectionTypeOf(*p.pc, false, forceRelay_);
  }
  emit peerStateChanged(deviceId, name);
  if (name == QLatin1String("failed") && p.retries < kMaxRetries) {
    ++p.retries;
    const unsigned g = p.gen;
    QTimer::singleShot(1500, this, [this, deviceId, g]() {
      const auto i = peers_.find(deviceId);
      if (i != peers_.end() && i.value()->gen == g) buildPeer(*i.value());
    });
  }
}

void VoiceEngine::setMembers(const QList<Member>& members) {
  QSet<QString> keep;
  for (const Member& m : members) {
    if (m.deviceId == deviceId_) continue;
    keep.insert(m.deviceId);
    if (Peer* p = ensurePeer(m.deviceId, true)) p->userId = m.userId;
  }
  const QStringList have = peers_.keys();
  for (const QString& id : have) {
    if (!keep.contains(id)) removePeer(id);
  }
  if (peers_.isEmpty()) decodeTimer_.stop();
}

void VoiceEngine::handleSignal(const QString& fromDeviceId, const QJsonObject& body) {
  const QString kind = body.value(QStringLiteral("kind")).toString();
  try {
    if (kind == QLatin1String("offer")) {
      if (isOfferer(deviceId_, fromDeviceId)) return;  // I am the offerer of this pair: a stray offer is ignored
      Peer* p = ensurePeer(fromDeviceId, false);
      if (!p) return;
      if (p->remoteSet) buildPeer(*p);  // the other side started over
      if (!p->pc) return;
      p->pc->setRemoteDescription(rtc::Description(body.value(QStringLiteral("sdp")).toString().toStdString(), rtc::Description::Type::Offer));
      p->remoteSet = true;
      p->pc->setLocalDescription(rtc::Description::Type::Answer);  // auto negotiation is off
      const QList<QJsonObject> pending = std::exchange(p->pendingCandidates, {});
      for (const QJsonObject& c : pending) handleSignal(fromDeviceId, c);
    } else if (kind == QLatin1String("answer")) {
      const auto it = peers_.find(fromDeviceId);
      if (it == peers_.end() || !it.value()->offerer || !it.value()->pc || it.value()->remoteSet) return;
      Peer& p = *it.value();
      p.pc->setRemoteDescription(rtc::Description(body.value(QStringLiteral("sdp")).toString().toStdString(), rtc::Description::Type::Answer));
      p.remoteSet = true;
      const QList<QJsonObject> pending = std::exchange(p.pendingCandidates, {});
      for (const QJsonObject& c : pending) handleSignal(fromDeviceId, c);
    } else if (kind == QLatin1String("candidate")) {
      Peer* p = peers_.value(fromDeviceId).get();
      if (!p) {
        p = ensurePeer(fromDeviceId, false);  // a candidate that overtook the offer
        if (!p) return;
      }
      if (!p->remoteSet || !p->pc) {
        p->pendingCandidates.append(body);
        return;
      }
      p->pc->addRemoteCandidate(rtc::Candidate(body.value(QStringLiteral("candidate")).toString().toStdString(), body.value(QStringLiteral("mid")).toString().toStdString()));
    }
  } catch (const std::exception& e) {
    qCWarning(lcVoice) << "Voice signal" << kind << "from" << fromDeviceId << "failed:" << e.what();
  }
}

int VoiceEngine::pushFrame(const int16_t* pcm, bool transmit) {
  ++frameCounter_;
  if (!transmit || peers_.isEmpty()) return 0;
  if (!enc_->enc.isOpen() && !enc_->enc.open()) return 0;
  const std::vector<uint8_t> packet = enc_->enc.encode(pcm);
  if (packet.empty()) return 0;
  const double seconds = static_cast<double>(frameCounter_) * 0.02;
  int sent = 0;
  for (const auto& p : std::as_const(peers_)) {
    if (!p->track || !p->track->isOpen()) continue;
    try {
      p->track->sendFrame(toBinary(packet.data(), packet.size()), rtc::FrameInfo(std::chrono::duration<double>(seconds)));
      ++sent;
    } catch (const std::exception& e) {
      if (sendErrors_++ % 500 == 0) qCWarning(lcVoice) << "sendFrame:" << e.what();
    }
  }
  if (sent > 0) ++framesSent_;
  return sent;
}

void VoiceEngine::decodeOne(Peer& p, const uint8_t* data, size_t size, bool fec, bool conceal) {
  int16_t out[kVoiceFrame];
  if (!p.dec.decode(conceal ? nullptr : data, size, out, fec)) return;
  const double level = VoiceDsp::levelDb(out, kVoiceFrame);
  const qint64 now = nowMs();
  p.levelDb = level;
  if (!conceal) {
    if (level > kSpeakOnDb) {
      ++p.loudFrames;
      if (!p.speaking && p.loudFrames >= 2) {
        p.speaking = true;
        emit speakingChanged(p.deviceId, true);
      }
    } else if (level <= kSpeakOffDb) {
      p.loudFrames = 0;
    }
    if (level > kSpeakOffDb) p.lastLoudMs = now;
  } else {
    ++p.framesConcealed;
  }
  emit pcmDecoded(p.deviceId, QByteArray(reinterpret_cast<const char*>(out), kVoiceFrame * int(sizeof(int16_t))));
}

void VoiceEngine::decodeTick() {
  const qint64 now = nowMs();
  const QStringList ids = peers_.keys();
  for (const QString& id : ids) {
    const auto it = peers_.find(id);
    if (it == peers_.end()) continue;
    Peer& p = *it.value();
    if (!p.pipe) continue;
    if (!p.playing) {
      std::lock_guard<std::mutex> lock(p.pipe->mutex);
      const auto& q = p.pipe->frames;
      if (!q.empty() && (static_cast<int>(q.size()) >= kPrefillFrames || now - q.front().arrivedMs >= 60)) {
        p.playing = true;
        p.nextDueMs = now;
        p.miss = 0;
      }
    }
    int guard = 0;
    while (p.playing && now >= p.nextDueMs && guard++ < 8) {
      QueuedFrame f;
      bool got = false;
      {
        std::lock_guard<std::mutex> lock(p.pipe->mutex);
        auto& q = p.pipe->frames;
        while (static_cast<int>(q.size()) > kMaxQueuedFrames) q.pop_front();  // the sender runs ahead: stay close to real time
        if (!q.empty()) {
          f = std::move(q.front());
          q.pop_front();
          got = true;
        }
      }
      if (got) {
        const auto* data = reinterpret_cast<const uint8_t*>(f.data.constData());
        if (p.haveTs && (f.ts - p.lastTs + 480u) / 960u == 2u) decodeOne(p, data, size_t(f.data.size()), true, false);  // one frame lost: recover it from the redundancy
        decodeOne(p, data, size_t(f.data.size()), false, false);
        p.lastTs = f.ts;
        p.haveTs = true;
        p.miss = 0;
        ++p.framesReceived;
      } else if (++p.miss <= 3) {
        decodeOne(p, nullptr, 0, false, true);  // late: conceal
      } else {
        p.playing = false;  // the sender stopped: the next talkspurt prefills again
        p.haveTs = false;
      }
      p.nextDueMs += 20;
    }
    if (p.playing && now - p.nextDueMs > 200) p.nextDueMs = now;
    if (p.speaking && now - p.lastLoudMs > kSpeakHoldMs) {
      p.speaking = false;
      p.loudFrames = 0;
      emit speakingChanged(p.deviceId, false);
    }
  }
}

QList<VoiceEngine::PeerInfo> VoiceEngine::peers() const {
  QList<PeerInfo> out;
  for (const auto& p : peers_) {
    out.append({p->deviceId, p->userId, p->state, p->connection, p->speaking, p->levelDb, p->framesReceived, p->framesConcealed});
  }
  return out;
}

}  // namespace uzume
