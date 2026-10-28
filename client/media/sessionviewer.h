#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <memory>
#include <thread>

#include "bitratecontroller.h"
#include "controlproto.h"
#include "mediastats.h"
#include "opuscodec.h"
#include "qualitytier.h"
#include "rtcutil.h"
#include "sessiontypes.h"
#include "videodecoder.h"

namespace rtc {
class PeerConnection;
class Track;
class DataChannel;
}  // namespace rtc

namespace uzume {

class ThreadBridge;
class LossReceivingSession;

// Viewer side of a Session: answers the owner's offer recvonly, depacketizes H.264 and Opus, decodes to QImage
// frames (frameReady) and 48 kHz stereo int16 audio behind a ~60 ms jitter buffer (pullAudio, callable from an
// audio thread). Decode errors trigger a PLI (rate-limited).
class SessionViewer : public QObject {
  Q_OBJECT
 public:
  static constexpr int kJitterTargetMs = 60;
  static constexpr int kJitterMaxMs = 240;

  explicit SessionViewer(QObject* parent = nullptr);
  ~SessionViewer() override;

  // After the REST join: signaling for (sessionId, viewerId) is accepted from now on.
  // `turnServers`: relay credentials from the join response / hello_ack (ADR 0012 D3-D5).
  void open(const QString& sessionId, const QString& viewerId, const QStringList& iceServers,
            const QList<TurnServer>& turnServers = {});
  // ICE transport policy "relay" for the PeerConnection created from now on (default: UZUME_FORCE_RELAY=1).
  void setForceRelay(bool force) { forceRelay_ = force; }
  bool forceRelay() const { return forceRelay_; }
  // Leave / Session ended / viewer removed: closes the PeerConnection immediately.
  void close();
  // `ice_update` from the Hub: the next PeerConnection of this object uses it (a running one stays untouched).
  void updateIceServers(const QStringList& iceServers, const QList<TurnServer>& turnServers) {
    iceServers_ = iceServers;
    turnServers_ = turnServers;
    relayTcpOnly_ = turnServersTcpOnly(turnServers);
  }
  bool isOpen() const { return open_; }
  bool isConnected() const { return connected_; }
  QString viewerId() const { return viewerId_; }

  // Audio output pull API: returns exactly `frames` frames (4 bytes each) of 48 kHz stereo int16; silence while the
  // jitter buffer fills or runs dry. Thread-safe.
  QByteArray pullAudio(int frames);
  int bufferedAudioMs() const;
  // Highest absolute sample value of everything decoded so far (0 = only silence arrived).
  int audioPeak() const { return audioPeak_.load(); }

  // Video access units or decoded pictures dropped because the decoder or the UI thread fell behind (PN-6).
  qint64 droppedVideoFrames() const { return droppedVideo_; }
  SessionStats stats() const;
  // One-line ICE state for diagnostics (state, gathering, candidate counts).
  QString iceSummary() const;
  ViewerLinkStats link() const;
  // Host state signal (ADR 0026 D3): the owner paused or resumed its game. False / empty until the host says otherwise
  // (older hosts never do). Reset when the connection is closed.
  bool hostPaused() const { return hostPaused_; }
  QString hostPausedBy() const { return hostPausedBy_; }
  void requestKeyframe();  // sends a PLI now (rate-limited to one per 500 ms)
  // Quality (ADR 0033 D5): the pick goes to the host over fb-diag now, and again whenever the channel (re)opens. Valid before
  // open(): the pick of a new Session starts from the user's default. Old hosts ignore the message.
  void setQuality(Quality pick);
  Quality quality() const { return pick_; }
  // What the host says it serves this viewer ("tier" message); hasServedTier is false until the first one (old hosts never).
  bool hasServedTier() const { return hasTier_; }
  Quality servedTier() const { return servedTier_; }
  int servedKbps() const { return servedKbps_; }
  // Latency measured with the fb-diag ping/pong (nullopt until the first pong).
  std::optional<double> latencyMs() const { return latencyMs_; }

  // Controller hand-over (ADR 0033 D8). hostPorts(): what the host's system offers (control_caps; 0 until the host says, so older
  // hosts never get a button). requestControl asks for port 0 or 1 (only with hostPorts() > port and no ask or hold running);
  // the host answers with granted or a denial reason. releaseControl hands the port back. While Granted, setControlInput feeds the
  // fb-input channel: a frame on every change (at most one per 16 ms) and a keep-alive every 100 ms.
  enum class ControlState { Idle, Asking, Granted };
  int hostPorts() const { return hostPorts_; }
  ControlState controlState() const { return controlState_; }
  int controlPort() const { return controlPort_; }
  QString controlReason() const { return controlReason_; }  // why the last ask or hold ended: denied | busy | timeout | host | viewer | left ...
  bool requestControl(int port);
  void releaseControl();
  // Buttons (libretro joypad bits 0..15), sticks, and the touch point (0..1 over the whole frame, `touching` = pressed).
  void setControlInput(quint16 buttons, const qint16 axes[4], bool touching, double touchX, double touchY);
  void setControlButtons(quint16 buttons) {
    const qint16 axes[4] = {0, 0, 0, 0};
    setControlInput(buttons, axes, false, 0, 0);
  }
  int inputFramesSent() const { return inputFramesSent_; }  // tests

 public slots:
  void handleSignal(const uzume::SessionSignal& signal);  // offer / candidate from the owner

 signals:
  void signalOut(const uzume::SessionSignal& signal);
  void frameReady(const QImage& frame);
  void connectedChanged(bool connected);
  void closed(const QString& reason);  // the PeerConnection failed or closed on its own
  void errorOccurred(const QString& message);
  void hostStateChanged(bool paused, const QString& by);  // UI thread
  void qualityChanged();  // served tier, pick or latency changed (UI thread)
  void controlChanged();  // host ports, ask or hold changed (UI thread)

 private:
  void onPcState(int state);
  void onHostState(bool paused, const QString& by);
  void onTier(const TierMessage& m);
  void onPingPong(const PingMessage& m);
  void sendDiag(const QByteArray& msg);
  void sendPing();
  void onControlAnswer(const ControlAnswer& a);
  void onControlEnd(const ControlEnd& e);
  void sendInputFrame();
  void resetControl(const QString& reason);
  void onGatheringComplete();
  struct MediaPipe;
  // Receive side (PN-6): the libdatachannel threads only fill a bounded mailbox; a worker thread decodes (H.264, Opus) and hands
  // decoded pictures to the UI thread in a small coalescing queue. A stalled UI thread therefore costs dropped frames and a
  // keyframe request instead of unbounded memory and a stream that falls further behind.
  void startMedia();
  void mediaLoop(std::shared_ptr<MediaPipe> pipe, unsigned gen);
  void decodeVideo(const QByteArray& data, const std::shared_ptr<MediaPipe>& pipe, unsigned gen);
  void decodeAudio(const QByteArray& data);
  void updateStats();
  void sendRxReport(double videoKbps, double decodedFps);
  void teardown();

  QString sessionId_;
  QString viewerId_;
  QStringList iceServers_;
  QList<TurnServer> turnServers_;
  bool forceRelay_ = false;
  bool relayTcpOnly_ = false;
  bool open_ = false;
  unsigned pcGen_ = 0;  // UI thread only; bumped by teardown()
  bool connected_ = false;
  bool remoteSet_ = false;
  IceTrace ice_;  // candidate counts of the current PeerConnection
  QString pcState_ = QStringLiteral("new");
  QList<SessionSignal> pendingCandidates_;
  std::shared_ptr<ThreadBridge> bridge_;
  std::shared_ptr<rtc::PeerConnection> pc_;
  std::shared_ptr<rtc::Track> video_;
  std::shared_ptr<rtc::Track> audio_;
  std::shared_ptr<rtc::DataChannel> diag_;  // negotiated "fb-diag": SCTP for RTT
  std::shared_ptr<rtc::DataChannel> input_;  // negotiated "fb-input": controller input frames
  std::shared_ptr<LossReceivingSession> videoSession_;

  VideoDecoder decoder_;  // open/closed on the UI thread only while the worker is stopped
  bool codecsOpen_ = false;
  std::shared_ptr<MediaPipe> pipe_;
  std::thread worker_;
  OpusDepacker opus_;
  QElapsedTimer pliClock_;
  bool pliSent_ = false;
  bool hostPaused_ = false;
  QString hostPausedBy_;
  Quality pick_ = Quality::Auto;
  Quality servedTier_ = Quality::Auto;
  bool hasTier_ = false;
  int servedKbps_ = 0;
  std::optional<double> latencyMs_;
  int pingN_ = 0;
  QElapsedTimer pingClock_;
  QTimer pingTimer_;
  int hostPorts_ = 0;
  ControlState controlState_ = ControlState::Idle;
  int controlPort_ = -1;
  QString controlReason_;
  InputFrame inputState_;       // the state the viewer holds right now (seq/ts are set when it is sent)
  quint16 inputSeq_ = 0;
  int inputFramesSent_ = 0;
  QTimer inputKeepAlive_;
  QTimer inputCoalesce_;        // single shot: a change that came less than 16 ms after the last frame
  QElapsedTimer inputClock_;
  qint64 lastInputSendMs_ = -1000;

  mutable QMutex audioMutex_;
  QByteArray audioBuf_;
  bool audioPrimed_ = false;
  std::atomic<int> audioPeak_{0};

  QTimer statsTimer_;
  QElapsedTimer statsClock_;
  // Written by the receive threads / the worker, read by the UI thread.
  std::atomic<qint64> totalFrames_{0}, totalAudioFrames_{0}, totalVideoBytes_{0}, totalAudioBytes_{0};
  qint64 lastFrames_ = 0, lastVideoBytes_ = 0, lastAudioBytes_ = 0;
  std::atomic<int> decodeErrors_{0};
  std::atomic<qint64> droppedVideo_{0};
  int pliCount_ = 0;
  int width_ = 0, height_ = 0;
  int64_t lastExpected_ = 0, lastReceived_ = 0;  // RTP counters at the previous rx report
  SessionStats stats_;
  ViewerLinkStats link_;
};

}  // namespace uzume
