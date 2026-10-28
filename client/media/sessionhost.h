#pragma once

#include <algorithm>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "bitratecontroller.h"
#include "controlarbiter.h"
#include "rtcutil.h"
#include "mediastats.h"
#include "qualitytier.h"
#include "tiermanager.h"
#include "sessiontypes.h"
#include "videoencoder.h"
#include "viewerreports.h"

struct AVFrame;

namespace rtc {
class PeerConnection;
class Track;
class DataChannel;
class RtpPacketizationConfig;
}  // namespace rtc

namespace uzume {

class ThreadBridge;
class EncodeWorker;
struct WorkerStats;
struct SinkList;

// Owner side of a Session (ADR 0006 D5): one encoder for all viewers (running only while at least one viewer is
// there), per viewer a PeerConnection with a sendonly H.264 and a sendonly Opus track. Signaling is decoupled:
// wire addViewer()/removeViewer()/handleSignal()/close() to the Hub messages and signalOut() to HubSocket.
//
// Thread ownership: the public API, the PeerConnections, signaling and all Qt state belong to the thread the object
// lives in (the UI thread); use queued connections otherwise. pushFrame()/pushAudio() only copy the data into a
// bounded queue and return. All video/Opus encoding and the RTP sendFrame() calls run on ONE dedicated worker
// thread (EncodeWorker, started with the encoder, joined in stopEncoder()/close()); when it falls behind the oldest
// pending video frame is dropped, audio stays ordered. stats()/encoderName()/encoderRunning() are thread-safe.
//
// GPU-direct input (ADR 0019): on an NVIDIA GPU the local game can hand CUDA frames to pushGpuFrame() instead of the
// readback frames of pushFrame(). h264_nvenc then takes them without a CPU copy. Readback stays the default and the
// safety net: the CPU encoder keeps running until the first CUDA frame was encoded, and any problem on the GPU path
// switches this Session back to readback frames (gpuInputChanged()), never to an error.
class SessionHost : public QObject {
  Q_OBJECT
 public:
  struct Options {
    int fps = 60;
    int videoBitrate = 2'000'000;  // start value of the bitrate adaptation (ADR 0012 D5, AIMD 300..4000 kbit/s)
    int audioBitrate = 96'000;
    QStringList encoderOrder;      // empty: ADR 0006 D5 preference
  };

  explicit SessionHost(QObject* parent = nullptr);
  ~SessionHost() override;

  void setOptions(const Options& o) { options_ = o; }
  // Session published: signaling and viewers refer to this id.
  // `turnServers`: relay credentials from hello_ack / the join response (ADR 0012 D3-D5).
  void open(const QString& sessionId, const QStringList& iceServers, const QList<TurnServer>& turnServers = {});
  // `ice_update` from the Hub: PeerConnections created from now on use this config, running ones stay untouched.
  void updateIceServers(const QStringList& iceServers, const QList<TurnServer>& turnServers);
  // ICE transport policy "relay" for the PeerConnections created from now on (default: UZUME_FORCE_RELAY=1).
  void setForceRelay(bool force) { forceRelay_ = force; }
  bool forceRelay() const { return forceRelay_; }
  // Session ended or stopped sharing: closes every viewer immediately and stops the encoder.
  void close();
  bool isOpen() const { return open_; }

  int viewerCount() const { return static_cast<int>(viewers_.size()); }
  bool hasViewer(const QString& viewerId) const { return viewers_.contains(viewerId); }
  bool encoderRunning() const { return encoderRunning_; }
  QString encoderName() const;
  // Last full second, aggregated over viewers; cheap.
  SessionStats stats() const;
  QList<ViewerLinkStats> viewerLinks() const;
  // Encoders alive for the viewers' quality picks (ADR 0033 D5): Auto = the adaptive one, plus one per fixed tier in use.
  // Auto viewers only: 1. Idle ones are dropped 10 s after their last viewer left.
  int encoderCount() const { return tiers_.encoderCount(); }
  QList<Quality> encoderTiers() const { return tiers_.aliveTiers(); }
  // The pick a viewer made and the tier it is served by (differs only when three encoders are alive already).
  Quality viewerPick(const QString& viewerId) const { return tiers_.pick(viewerId); }
  Quality viewerTier(const QString& viewerId) const { return tiers_.effective(viewerId); }
  // Tests: advance the tier clock (drop of idle encoders) without waiting; ms added to the real clock.
  void advanceTierClockForTest(qint64 ms) {
    clockSkewMs_ += ms;
    tickTiers();
  }

  // Video: raw libretro frame (XRGB8888/RGB565, stride in bytes) or a QImage (any format, converted). The first
  // frame after a viewer joined fixes the encoder resolution; a size change reopens the encoder.
  // lateNs (speed-up, 0 otherwise): how long after its base-rate slot the frame was delivered; the frame is stamped
  // that much earlier, so the stream keeps evenly spaced wall-clock timestamps at the base rate (streamTimestamp()).
  void pushFrame(const uint8_t* data, int width, int height, int stride, RawPixelFormat format, qint64 lateNs = 0);
  void pushFrame(const QImage& image, qint64 lateNs = 0);
  // Timestamp of a frame arriving at nowNs that is lateNs behind its slot; strictly after the previous stamp.
  static qint64 streamTimestamp(qint64 nowNs, qint64 lateNs, qint64 lastStampNs) {
    return std::max<qint64>(nowNs - std::max<qint64>(lateNs, 0), lastStampNs + 1);
  }
  // Host state signal (ADR 0026 D3): the owner's game is paused (by `by`, the owner's display name) or running again. Sent
  // over fb-diag to every viewer now and to every viewer whose channel opens later. State only, no picture.
  void setHostState(bool paused, const QString& by);
  bool hostPaused() const { return hostPaused_; }
  QString hostPausedBy() const { return hostPausedBy_; }

  // Controller hand-over (ADR 0033 D8). `ports` is what the running game's system offers (0 = nothing, 1, 2); viewers are told
  // (control_caps) and holders of a port that no longer exists lose it. A request needs the host's answer (answerControl); it is
  // denied after 20 s. Input arrives on the negotiated DataChannel fb-input and is judged by the ControlArbiter; applied frames
  // (and the neutral state after 500 ms without a frame or at the end of a hold) are reported by controlInput().
  void setControlPorts(int ports);
  int controlPorts() const { return control_.portCount(); }
  QList<ControlArbiter::Pending> controlPending() const { return control_.pending(); }
  QString controlHolder(int port) const { return control_.holder(port); }
  int controlPortOf(const QString& viewerId) const { return control_.portOf(viewerId); }
  InputFrame controlInputFrame(int port) const { return control_.input(port); }
  bool answerControl(const QString& viewerId, bool allow);  // false: no such pending request
  bool takeBack(int port);                                  // ends the hold of that port (reason "host")
  bool takeBackViewer(const QString& viewerId);
  // Tests: the clock of the arbitration (ms added to the real clock) and a frame as if it had arrived on fb-input.
  void advanceControlClockForTest(qint64 ms) {
    controlSkewMs_ += ms;
    tickControl();
  }
  qint64 controlNowMs() const { return controlClock_.elapsed() + controlSkewMs_; }
  ControlArbiter::Verdict injectInputFrameForTest(const QString& viewerId, const InputFrame& f) { return applyInputFrame(viewerId, f); }

  // Audio: interleaved int16 stereo at the core rate (any rate), e.g. straight from the emulation callback.
  void pushAudio(const QByteArray& pcm, int sampleRate);

  // GPU-direct input (ADR 0019): one CUDA frame (AV_PIX_FMT_CUDA, sw_format RGB0) of the local game. Never starts the
  // encoder (readback frames do); ignored unless the encoder runs and GPU input is not off for this Session. Does not
  // look at the driver or the caps (tests push fake frames).
  void pushGpuFrame(std::shared_ptr<AVFrame> frame, qint64 lateNs = 0);
  // GPU input may be tried in this Session: open, not off, configured (no kill switch, no forced encoder), no pinned
  // encoder order, and at least one backend (ADR 0019, ADR 0027 D6):
  //   CUDA: usable in this process, FFmpeg feeds h264_nvenc with CUDA frames and the encoder opens here.
  //   AMF:  Windows, FFmpeg feeds h264_amf with D3D11 frames and the encoder opens here.
  //   QSV:  Windows, FFmpeg feeds h264_qsv with QSV frames and the encoder opens here.
  // Which one is used is decided by the UI's GpuEncodeBridge (it knows the GPU vendor).
  struct GpuBackends {
    bool cuda = false;
    bool amf = false;
    bool qsv = false;
    bool any() const { return cuda || amf || qsv; }
  };
  GpuBackends gpuBackendsAllowed() const;
  bool gpuInputAllowed() const { return gpuBackendsAllowed().any(); }
  // The rules above as a pure function of their inputs (unit-tested).
  struct GpuInputFacts {
    bool open = false, off = false, configured = false, orderPinned = false;
    bool cudaDriverUsable = false, cudaInput = false, nvencProbed = false;
    bool d3d11Capture = false;  // the D3D11 capture exists on this platform
    bool amfInput = false, amfProbed = false;
    bool qsvInput = false, qsvProbed = false;
  };
  static GpuBackends gpuBackendRules(const GpuInputFacts& facts);
  // The encoder runs on GPU frames (confirmed by its first CUDA encode); readback frames are not copied then.
  bool gpuInputActive() const { return encoderRunning_ && gpuActive_ && !gpuInputOff_; }
  // GPU input was tried in this Session and failed (diagnostics: "readback (GPU-direct off)").
  bool gpuInputFailed() const { return gpuInputFailed_; }
  // Off for the rest of this Session: readback frames again. `failure` false = not applicable (silent), true = failed.
  // Idempotent; emits gpuInputChanged().
  void disableGpuInput(const QString& reason, bool failure);
  // Tests: gpuActive_ = true, as if the worker had confirmed GPU mode.
  void simulateGpuActiveForTest() {
    gpuActive_ = true;
    lastGpuNs_ = clock_.nsecsElapsed();
  }

 public slots:
  void addViewer(const QString& viewerId, const QList<TurnServer>& turnServers = {});     // `viewer_joined`: creates the PeerConnection and sends the offer
  void removeViewer(const QString& viewerId);  // `viewer_left`: closes the PeerConnection immediately
  void handleSignal(const uzume::SessionSignal& signal);  // answer / candidate from a viewer

 signals:
  void signalOut(const uzume::SessionSignal& signal);
  void viewerConnected(const QString& viewerId);  // media path established
  void viewerClosed(const QString& viewerId, const QString& reason);  // PeerConnection closed (removed, failed, ...)
  void encoderRunningChanged(bool running);
  void rxReportReceived(const QString& viewerId, double loss, double kbps);  // viewer report over fb-diag (tests, diagnostics)
  void errorOccurred(const QString& message);  // e.g. no encoder can be opened
  // GPU input became active or was switched off (UI thread). Read gpuInputActive()/gpuInputFailed().
  void gpuInputChanged();
  // Controller hand-over: a viewer asks for a port (the host decides), the pending/holder state changed, a hold ended, and the
  // input of a port changed (an applied frame, or neutral: all zero, `neutral` true).
  void controlRequested(const QString& viewerId, int port);
  void controlChanged();
  void controlEnded(const QString& viewerId, int port, const QString& reason);
  void controlInput(int port, const uzume::InputFrame& frame, bool neutral);

 private:
  struct Viewer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video;
    std::shared_ptr<rtc::Track> audio;
    std::shared_ptr<rtc::DataChannel> diag;  // negotiated "fb-diag": brings up SCTP so that rtt() is available
    std::shared_ptr<rtc::DataChannel> input;  // negotiated "fb-input" (id 1, unordered, maxRetransmits 0): controller input frames
    QList<SessionSignal> pendingCandidates;
    bool remoteSet = false;
    bool connected = false;
    QString state = QStringLiteral("new");
    IceTrace ice;  // candidate counts of this connection (UI thread)
    Quality fedTier = Quality::Auto;   // the encoder this viewer's video track is fed by (sinks); a change needs a keyframe
    Quality sentTier = Quality::Auto;  // last "tier" message (fb-diag), resent when the Auto target moves a lot
    int sentKbps = -1;
    int pingN = 0;
    std::optional<double> latencyMs;  // from the fb-diag ping/pong (the viewer answers ours)
  };

  void dropViewer(const QString& viewerId, const QString& reason);
  void sendHostState(const QString& viewerId);
  void sendDiagTo(const QString& viewerId, const QByteArray& msg);
  void onControlRequest(const QString& viewerId, int port);
  void onControlRelease(const QString& viewerId);
  ControlArbiter::Verdict applyInputFrame(const QString& viewerId, const InputFrame& f);
  void tickControl();
  void reportEnd(const ControlArbiter::End& e, bool notifyViewer);
  void onPcState(const QString& viewerId, int state);
  void onLocalCandidateType(const QString& viewerId, int type);
  void onGatheringComplete(const QString& viewerId);
  void startEncoder();
  void stopEncoder();
  void requestKeyframe();
  void requestKeyframeFor(const QString& viewerId);  // from the encoder of that viewer's tier
  qint64 tierNowMs() const { return adaptClock_.elapsed() + clockSkewMs_; }
  void onQualityPick(const QString& viewerId, Quality pick);
  void onPingPong(const QString& viewerId, const PingMessage& m);
  void applyTiers();  // after every change of picks or viewers: sinks, worker encoders, keyframes, tier messages
  void tickTiers();
  void sendTier(const QString& viewerId, bool force);
  void sendPings();
  int autoKbps() const { return targetKbps_.load(); }
  void publishSinks();
  void updateStats();
  void applyRemoteCandidate(Viewer& v, const SessionSignal& s);
  void onRxReport(const QString& viewerId, const RxReport& report);

  Options options_;
  QString sessionId_;
  QStringList iceServers_;
  QList<TurnServer> turnServers_;
  bool forceRelay_ = false;
  bool relayTcpOnly_ = false;
  BitrateController bitrate_;
  TierManager tiers_;
  QTimer pingTimer_;
  ControlArbiter control_;
  QTimer controlTimer_;
  QElapsedTimer controlClock_;
  qint64 controlSkewMs_ = 0;
  qint64 clockSkewMs_ = 0;
  QElapsedTimer adaptClock_;  // monotonic clock of the controller, never restarted
  std::atomic<int> targetKbps_{0};
  bool open_ = false;
  bool hostPaused_ = false;
  QString hostPausedBy_;
  QSet<QString> hostStateSent_;  // viewers that were told "paused" (a later "running" must reach them)
  QHash<QString, Viewer> viewers_;
  std::shared_ptr<ThreadBridge> bridge_;

  std::unique_ptr<EncodeWorker> worker_;  // exists while the encoder runs
  std::shared_ptr<SinkList> sinks_;       // tracks the worker sends to (guarded copy of viewers_)
  std::atomic<bool> encoderRunning_{false};
  std::atomic<int> viewerCountAtomic_{0};
  bool encoderFailed_ = false;
  unsigned workerGen_ = 0;
  bool keyframePending_ = false;
  QElapsedTimer clock_;
  qint64 lastEncodeNs_ = 0;  // arrival of the last frame taken (drop test)
  qint64 lastCpuEncodeNs_ = 0;  // same for the readback frames that only feed fixed tiers while the adaptive encoder is GPU-direct
  qint64 lastStampNs_ = 0;   // its stream timestamp

  // GPU-direct input (ADR 0019). Atomic where stats()/gpuInputActive() may be read from other threads.
  std::atomic<bool> gpuActive_{false};       // the worker confirmed GPU mode
  std::atomic<bool> gpuInputOff_{false};     // off for this Session (failed or not applicable)
  std::atomic<bool> gpuInputFailed_{false};  // ... because it failed
  bool gpuConfigured_ = false;               // computed at open(): kill switch and forced encoder
  int gpuSkips_ = 0;                         // watchdog: readback frames since the last GPU frame
  qint64 lastGpuNs_ = 0;  // clock_ time of the last GPU frame (watchdog)

  QTimer statsTimer_;
  QElapsedTimer statsClock_;
  qint64 lastStatFrames_ = 0, lastStatVideoBytes_ = 0, lastStatAudioBytes_ = 0;
  int keyframeRequests_ = 0;
  mutable std::mutex statsMutex_;  // guards stats_, links_, workerStats_ (the pointer)
  std::shared_ptr<WorkerStats> workerStats_;
  SessionStats stats_;
  QList<ViewerLinkStats> links_;
  ViewerReports reports_;  // latest rx report per viewer (UI thread); copied into links_ under statsMutex_
};

}  // namespace uzume
