#include "sessionhost.h"

#include <QLoggingCategory>
#include <QRandomGenerator>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <rtc/rtc.hpp>
#include <thread>
#include <variant>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#include "cudadriver.h"
#include "cudaglcapture.h"
#include "hoststate.h"
#include "mediacaps.h"
#include "opuscodec.h"
#include "controlproto.h"
#include "qualitytier.h"
#include "rtcutil.h"

namespace uzume {

namespace {
Q_LOGGING_CATEGORY(lcHost, "uzume.sessionhost")

constexpr int kVideoPayloadType = 96;
constexpr int kAudioPayloadType = 111;
constexpr int kMaxFragment = 1200;
constexpr uint32_t kOpusClockRate = 48000;
constexpr uint16_t kDiagChannelId = 0;
constexpr int kTierFps = 30;                       // fixed tiers (ADR 0033 D5): 30 fps, every 2nd frame of a 60 fps core is skipped
constexpr qint64 kTierFrameGapNs = 26'000'000;     // a bit under 1/30 s: 60 fps input gives every 2nd frame, jitter does not
constexpr int kPingIntervalMs = 2000;
constexpr int kGpuWatchdogFrames = 60;  // readback frames without a GPU frame before GPU input is turned off (ADR 0019)

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

// ---------------------------------------------------------------- encode worker

struct Sink {
  std::shared_ptr<rtc::Track> video;
  std::shared_ptr<rtc::Track> audio;
  int tier = 0;  // Quality: the encoder that feeds this viewer's video track (0 = Auto, the adaptive encoder)
};
}  // namespace

// Tracks the worker sends to; written by the owner thread, read by the worker.
struct SinkList {
  std::mutex mutex;
  std::vector<Sink> sinks;
  std::vector<Sink> get() {
    std::lock_guard<std::mutex> lock(mutex);
    return sinks;
  }
  void set(std::vector<Sink> s) {
    std::lock_guard<std::mutex> lock(mutex);
    sinks = std::move(s);
  }
};

// Counters of one worker run, readable from any thread.
struct WorkerStats {
  std::atomic<qint64> frames{0}, videoBytes{0}, audioBytes{0}, audioFrames{0}, dropped{0};
  mutable std::mutex mutex;
  QString encoderName;
  int width = 0, height = 0;
};

// Owns the H.264 encoders and the Opus framer and runs them on its own thread. Producers (owner thread) only copy
// data into a bounded queue. Video: at most kMaxPendingVideo frames wait, the oldest is dropped; readback frames and
// GPU frames (ADR 0019, CUDA frames of the local game, not copied) share that bound. Audio: ordered, bounded to
// ~1.4 s of PCM (only exceeded if the thread is stuck). Destruction joins the thread (pending work is discarded) and so
// guarantees that no callback or sendFrame() runs afterwards. Jobs that may hold a CUDA frame are never destroyed
// under the queue lock: returning a pool buffer pushes the CUDA context.
class EncodeWorker {
 public:
  static constexpr int kMaxPendingVideo = 2;
  static constexpr qint64 kMaxPendingAudioBytes = 256 * 1024;

  EncodeWorker(const SessionHost::Options& options, std::shared_ptr<SinkList> sinks, std::shared_ptr<WorkerStats> stats,
               std::function<void(QString)> onError, std::function<void()> onOpenFailed, std::function<void()> onGpuActive,
               std::function<void(QString)> onGpuOff, std::function<void(int)> onTierFailed)
      : options_(options), targetKbps_(options.videoBitrate / 1000), sinks_(std::move(sinks)), stats_(std::move(stats)), onError_(std::move(onError)),
        onOpenFailed_(std::move(onOpenFailed)), onGpuActive_(std::move(onGpuActive)), onGpuOff_(std::move(onGpuOff)),
        onTierFailed_(std::move(onTierFailed)), thread_([this]() { run(); }) {}
  ~EncodeWorker() {
    std::deque<Job> doomed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      doomed = std::move(queue_);
      queue_.clear();
    }
    cv_.notify_all();
    doomed.clear();  // outside the lock
    thread_.join();
  }
  EncodeWorker(const EncodeWorker&) = delete;
  EncodeWorker& operator=(const EncodeWorker&) = delete;

  void requestKeyframe() { keyframe_ = true; }
  // Keyframe from the encoder of `tier` (a Quality value; 0 = the adaptive encoder).
  void requestKeyframe(int tier) {
    if (tier <= 0 || tier >= 4) {
      keyframe_ = true;
    } else {
      fixed_[tier].keyframe = true;
    }
  }
  // Which encoders the host wants (TierManager): bit (1 << tier) per fixed tier, and the adaptive encoder. An encoder that
  // is no longer wanted is closed before the next video job; a wanted one opens lazily with its first frame.
  void setTiers(int fixedMask, bool adaptive) {
    fixedMask_ = fixedMask;
    adaptiveWanted_ = adaptive;
  }
  // A failed fixed tier was evicted by the host: its next pick starts a fresh encoder instead of staying skipped.
  void resetTierFailure(int tier) { failedReset_ |= (1 << tier); }
  // Bitrate adaptation: picked up before the next frame (runtime change or reopen, see applyTargetBitrate()).
  void setTargetBitrate(int kbps) { targetKbps_ = kbps; }
  // GPU input was switched off by the owner: the worker leaves GPU mode before its next job (any thread).
  void disableGpu() {
    gpuDisabled_ = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);  // pairs with the wait predicate in run()
    }
    cv_.notify_one();
  }

  void pushVideo(const uint8_t* data, int width, int height, int stride, RawPixelFormat format, qint64 ns) {
    Job j;
    j.video = true;
    j.width = width;
    j.height = height;
    j.stride = stride;
    j.format = format;
    j.ns = ns;
    const int bpp = format == RawPixelFormat::Rgb565 ? 2 : 4;
    const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(height - 1) + static_cast<size_t>(width) * bpp;
    j.pixels = acquireBuffer();  // reused buffer: no multi-MB malloc/free per frame on the producer thread
    j.pixels.resize(bytes);
    std::memcpy(j.pixels.data(), data, bytes);  // copy outside the queue lock
    insertVideo(std::move(j));
  }

  // One CUDA frame of the local game (no copy). Same bounded insert as pushVideo().
  void pushGpuVideo(std::shared_ptr<AVFrame> frame, qint64 ns) {
    Job j;
    j.video = true;
    j.gpu = std::move(frame);
    j.ns = ns;
    insertVideo(std::move(j));
  }

  void pushAudio(const QByteArray& pcm, int rate) {
    Job j;
    j.video = false;
    j.pcm = pcm;  // implicitly shared, the producer does not modify it afterwards
    j.rate = rate;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) return;
      pendingAudio_ += pcm.size();
      queue_.push_back(std::move(j));
      for (auto it = queue_.begin(); pendingAudio_ > kMaxPendingAudioBytes && it != queue_.end();) {
        if (!it->video) {
          pendingAudio_ -= it->pcm.size();
          // Keep the audio RTP clock in step with wall time: account the dropped PCM (S16 stereo) as elapsed time.
          if (it->rate > 0) droppedAudioSeconds_ += static_cast<double>(it->pcm.size()) / (kOpusChannels * sizeof(int16_t)) / it->rate;
          it = queue_.erase(it);
        } else {
          ++it;
        }
      }
    }
    cv_.notify_one();
  }

 private:
  struct Job {
    bool video = false;
    std::vector<uint8_t> pixels;
    std::shared_ptr<AVFrame> gpu;  // GPU-direct job: a CUDA frame instead of pixels (video is true as well)
    int width = 0, height = 0, stride = 0;
    RawPixelFormat format = RawPixelFormat::Xrgb8888;
    qint64 ns = 0;
    QByteArray pcm;
    int rate = 0;
  };
  // Where the video frames come from. Untried: only the CPU encoder runs, a GPU job opens the CUDA encoder next to it.
  // Active: the CUDA encoder works, the CPU encoder is closed and readback jobs are skipped. Off: back to readback for
  // the rest of this worker (failed, or switched off by the owner).
  enum class GpuState { Untried, Active, Off };

  // Small free list of frame buffers shared between pushVideo() (producer) and the worker, which returns them after
  // encoding or dropping a job. Own mutex, held only for a vector move.
  static constexpr size_t kMaxFreeBuffers = 3;
  std::vector<uint8_t> acquireBuffer() {
    std::lock_guard<std::mutex> lock(poolMutex_);
    if (freeBuffers_.empty()) return {};
    std::vector<uint8_t> b = std::move(freeBuffers_.back());
    freeBuffers_.pop_back();
    return b;
  }
  void releaseBuffer(std::vector<uint8_t>&& b) {
    if (b.capacity() == 0) return;
    std::lock_guard<std::mutex> lock(poolMutex_);
    if (freeBuffers_.size() < kMaxFreeBuffers) freeBuffers_.push_back(std::move(b));
  }
  std::mutex poolMutex_;
  std::vector<std::vector<uint8_t>> freeBuffers_;

  // Bounded insert shared by both video job kinds. Dropped jobs are destroyed after the unlock (they may hold CUDA frames).
  void insertVideo(Job&& j) {
    std::vector<Job> dropped;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) {
        dropped.push_back(std::move(j));
      } else {
        int pending = 0;
        for (const Job& q : queue_) pending += q.video ? 1 : 0;
        // GPU-direct plus a fixed tier: every frame is two jobs (the CUDA frame and its readback copy).
        const int limit = fixedMask_.load() != 0 && gpuSeen_.load() ? 2 * kMaxPendingVideo : kMaxPendingVideo;
        for (auto it = queue_.begin(); pending >= limit && it != queue_.end();) {
          if (it->video) {
            dropped.push_back(std::move(*it));
            it = queue_.erase(it);
            --pending;
            ++stats_->dropped;
          } else {
            ++it;
          }
        }
        queue_.push_back(std::move(j));
      }
    }
    cv_.notify_one();
    for (Job& d : dropped) releaseBuffer(std::move(d.pixels));
  }

  void run() {
    opus_.open(options_.audioBitrate);
    for (;;) {
      Job j;
      bool leaveOnly = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return stop_ || !queue_.empty() || (gpuDisabled_.load() && gpu_ != GpuState::Off); });
        if (stop_) break;
        if (queue_.empty()) {
          leaveOnly = true;  // woken by disableGpu(): close the CUDA encoder and drop queued GPU jobs now
        } else {
          j = std::move(queue_.front());
          queue_.pop_front();
          if (!j.video) {
            pendingAudio_ -= j.pcm.size();
            audioClockOffset_ += droppedAudioSeconds_;
            droppedAudioSeconds_ = 0;
          }
        }
      }
      try {
        if (leaveOnly) {
          leaveGpu({}, /*notify*/ false);
        } else if (j.video) {
          encodeVideo(j);
        } else {
          encodeAudio(j);
        }
      } catch (const std::exception& e) {
        qCWarning(lcHost) << "Worker:" << e.what();
      }
      releaseBuffer(std::move(j.pixels));
    }
    encoder_.close();
    gpuEncoder_.close();
    for (Fixed& f : fixed_) f.enc.close();
    opus_.close();
  }

  // Encoders the CPU path tries: after a fatal CUDA error the driver is Dead and h264_nvenc (which would create its own
  // CUDA context) is left out.
  QStringList cpuOrder() const {
    return VideoEncoder::effectiveOrder(options_.encoderOrder, cuda::Driver::instance().state() == cuda::Driver::State::Dead);
  }

  int64_t nextPts(const Job& j) {
    int64_t pts = static_cast<int64_t>(j.ns * options_.fps / 1'000'000'000.0 + 0.5);
    pts = std::max<int64_t>(pts, lastPts_ + 1);
    lastPts_ = pts;
    return pts;
  }

  // `tier`: only the viewers on that encoder get the packets. Frame counts are the adaptive stream's; the byte count (the
  // Session's video bitrate) is everything the host sends.
  void sendPackets(const std::vector<EncodedVideoPacket>& packets, int tier = 0, int fps = 0) {
    const std::vector<Sink> sinks = sinks_->get();
    for (const EncodedVideoPacket& p : packets) {
      if (tier == 0) ++stats_->frames;
      stats_->videoBytes += static_cast<qint64>(p.data.size());
      const double seconds = static_cast<double>(p.pts) / std::max(1, fps > 0 ? fps : options_.fps);
      for (const Sink& sink : sinks) {
        if (sink.tier == tier) send(sink.video, p.data.data(), p.data.size(), seconds, "video");
      }
    }
  }

  // Dropped adaptive encoder (nobody is on Auto any more): free the hardware/software encoder; it reopens with a keyframe.
  void closeFixed() {
    for (Fixed& f : fixed_) {
      if (f.enc.isOpen()) f.enc.close();
      f.failed = false;
      f.lastNs = -1;
    }
  }
  void closeAdaptive() {
    if (encoder_.isOpen()) encoder_.close();
    if (gpuEncoder_.isOpen()) gpuEncoder_.close();
  }

  // Fixed tiers (ADR 0033 D5): software encoders on readback frames, 30 fps, fixed bitrate, one per tier in use.
  void encodeFixedTiers(const Job& j) {
    const int mask = fixedMask_.load();
    for (int t = 1; t < 4; ++t) {
      Fixed& f = fixed_[t];
      if (failedReset_.fetch_and(~(1 << t)) & (1 << t)) f.failed = false;
      if (!(mask & (1 << t))) {
        if (f.enc.isOpen()) f.enc.close();
        f.failed = false;
        f.lastNs = -1;
        continue;
      }
      if (f.failed) continue;
      if (f.lastNs >= 0 && j.ns - f.lastNs < kTierFrameGapNs) continue;  // 60 fps core: every 2nd frame
      if (!f.enc.isOpen() || f.enc.width() != j.width || f.enc.height() != j.height) {
        f.enc.close();
        if (!f.enc.open(j.width, j.height, kTierFps, qualityKbps(static_cast<Quality>(t)) * 1000, cpuOrder())) {
          f.failed = true;
          onTierFailed_(t);
          continue;
        }
        f.lastPts = -1;
        std::lock_guard<std::mutex> lock(stats_->mutex);
        if (stats_->width == 0) {
          stats_->width = j.width;
          stats_->height = j.height;
          stats_->encoderName = f.enc.name();
        }
      }
      if (f.keyframe.exchange(false)) f.enc.requestKeyframe();
      f.lastNs = j.ns;
      int64_t pts = static_cast<int64_t>(j.ns * kTierFps / 1'000'000'000.0 + 0.5);
      pts = std::max<int64_t>(pts, f.lastPts + 1);
      f.lastPts = pts;
      std::vector<EncodedVideoPacket> packets;
      if (!f.enc.encode(j.pixels.data(), j.stride, j.format, pts, packets)) {
        f.failed = true;
        onTierFailed_(t);
        continue;
      }
      sendPackets(packets, t, kTierFps);
    }
  }

  void encodeVideo(const Job& j) {
    if (!j.gpu && fixedMask_.load() != 0) encodeFixedTiers(j);
    else if (fixedMask_.load() == 0) closeFixed();
    if (failed_) return;
    if (gpuDisabled_.load() && gpu_ != GpuState::Off) leaveGpu({}, /*notify*/ false);  // the host already knows
    if (!adaptiveWanted_.load()) {
      closeAdaptive();
      return;
    }
    if (j.gpu) {
      if (gpu_ != GpuState::Off) encodeGpuJob(j);
      return;
    }
    if (gpu_ == GpuState::Active) return;  // the CUDA encoder works; readback frames are not needed
    if (!encoder_.isOpen() || encoder_.width() != j.width || encoder_.height() != j.height) {
      encoder_.close();  // first frame or resolution change (the first frame is a keyframe again)
      if (!encoder_.open(j.width, j.height, options_.fps, targetKbps_.load() * 1000, cpuOrder())) {
        failed_ = true;
        onOpenFailed_();
        return;
      }
      lastReopen_ = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lock(stats_->mutex);
      stats_->encoderName = encoder_.name();
      stats_->width = j.width;
      stats_->height = j.height;
    }
    applyTargetBitrate(j);
    if (failed_) return;
    if (keyframe_.exchange(false)) {
      encoder_.requestKeyframe();
    }
    const int64_t pts = nextPts(j);
    std::vector<EncodedVideoPacket> packets;
    const auto t0 = std::chrono::steady_clock::now();
    if (!encoder_.encode(j.pixels.data(), j.stride, j.format, pts, packets)) {
      if (!encodeErrorReported_) {
        encodeErrorReported_ = true;
        onError_(QStringLiteral("H.264 encoding failed"));
      }
      return;
    }
    recordTiming(false, t0);
    sendPackets(packets);
  }

  static VideoEncoder::GpuEncoderSpec gpuSpecOf(AVBufferRef* fc) { return VideoEncoder::gpuEncoderSpecOf(fc); }
  static QString gpuEncoderName(AVBufferRef* fc) {
    const QString n = gpuSpecOf(fc).encoder;
    return n.isEmpty() ? QStringLiteral("h264_nvenc") : n;
  }
  static QString gpuFramesLabel(AVBufferRef* fc) {
    const QString l = gpuSpecOf(fc).framesLabel;
    return l.isEmpty() ? QStringLiteral("CUDA") : l;
  }

  // ADR 0019, ADR 0027 D6: one GPU frame (CUDA, D3D11 or QSV). Every error leaves GPU mode (failGpu); none of them touches failed_, onOpenFailed_ or
  // encodeErrorReported_, which belong to the CPU encoder.
  void encodeGpuJob(const Job& j) {
    AVBufferRef* fc = j.gpu->hw_frames_ctx;
    if (!fc) {
      leaveGpu(QStringLiteral("GPU frame without frames context"), true);
      return;
    }
    // Also whenever the frame's frames context is not the open encoder's: h264_amf asserts (aborts the process) on a frame of
    // another frames context, so a frame never reaches an encoder opened on a different one.
    const QSize visible(j.gpu->width, j.gpu->height);  // QSV frames are 16-aligned; the picture is smaller
    if (!gpuEncoder_.isOpen() || gpuEncoder_.gpuFramesKey() != fc->data) {  // first frame or new encode size
      QString why;
      if (!gpuEncoder_.openGpu(fc, options_.fps, targetKbps_.load() * 1000, &why, visible)) {
        failGpu(why.isEmpty() ? QStringLiteral("%1 does not open with %2 frames").arg(gpuEncoderName(fc), gpuFramesLabel(fc)) : why, fc);
        return;
      }
      lastReopen_ = std::chrono::steady_clock::now();
      if (gpu_ == GpuState::Active) {
        std::lock_guard<std::mutex> lock(stats_->mutex);
        stats_->width = gpuEncoder_.width();
        stats_->height = gpuEncoder_.height();
      }
    }
    applyGpuBitrate(fc, visible);
    if (gpu_ == GpuState::Off) return;
    if (keyframe_.exchange(false)) {
      gpuEncoder_.requestKeyframe();
    }
    const int64_t pts = nextPts(j);
    std::vector<EncodedVideoPacket> packets;
    const auto t0 = std::chrono::steady_clock::now();
    if (!gpuEncoder_.encodeGpu(j.gpu.get(), pts, packets)) {
      failGpu(QStringLiteral("encoding a %1 frame failed").arg(gpuFramesLabel(fc)), fc);
      return;
    }
    recordTiming(true, t0);
    if (gpu_ == GpuState::Untried) {
      gpu_ = GpuState::Active;
      gpuSeen_ = true;
      encoder_.close();  // only now: a failed GPU open never cost a CPU reopen
      const int w = gpuEncoder_.width(), h = gpuEncoder_.height();
      {
        std::lock_guard<std::mutex> lock(stats_->mutex);
        stats_->encoderName = gpuEncoder_.name();
        stats_->width = w;
        stats_->height = h;
      }
      qCInfo(lcHost).noquote() << QStringLiteral("GPU-direct encoding active: %1 takes %2 frames %3 x %4 "
                                                 "(no readback, no CPU conversion)")
                                      .arg(gpuEncoder_.name(), gpuFramesLabel(fc))
                                      .arg(w)
                                      .arg(h);
      onGpuActive_();
    }
    sendPackets(packets);
  }

  // After a GPU open/encode failure: if the CUDA context is dead the whole driver goes Dead (health check), then leave.
  void failGpu(const QString& what, AVBufferRef* fc) {
    const int r = cudaHealthCheck(fc);
    leaveGpu(r != 0 ? QStringLiteral("%1; %2").arg(what, cuda::Driver::describe(r)) : what, true);
  }

  // Back to readback frames for the rest of this worker. The CPU encoder is still open unless GPU mode was Active; then
  // the next readback job reopens it (with a keyframe). `notify`: tell the owner (false when it asked for this).
  void leaveGpu(const QString& reason, bool notify) {
    gpuEncoder_.close();
    gpu_ = GpuState::Off;
    std::vector<Job> dropped;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->gpu) {
          dropped.push_back(std::move(*it));
          it = queue_.erase(it);
        } else {
          ++it;
        }
      }
    }
    dropped.clear();  // CUDA buffers go back to their pool outside the lock
    if (notify) onGpuOff_(reason);
  }

  // Encode time per input kind, logged every 10 s: the CPU-side number for comparing both paths.
  void recordTiming(bool gpu, std::chrono::steady_clock::time_point started) {
    const auto now = std::chrono::steady_clock::now();
    EncodeTiming& t = timing_[gpu ? 1 : 0];
    const double ms = std::chrono::duration<double, std::milli>(now - started).count();
    ++t.frames;
    t.sumMs += ms;
    t.maxMs = std::max(t.maxMs, ms);
    if (timingStart_ == std::chrono::steady_clock::time_point{}) timingStart_ = started;
    if (now - timingStart_ < std::chrono::seconds(10)) return;
    const qint64 dropped = stats_->dropped.load();
    for (int i = 0; i < 2; ++i) {
      EncodeTiming& e = timing_[i];
      if (e.frames > 0) {
        qCInfo(lcHost).noquote() << QStringLiteral("Encode timing (10 s): input %1, %2 frames, %3 ms/frame (max %4 ms), %5 dropped")
                                        .arg(i == 1 ? QStringLiteral("gpu-direct") : QStringLiteral("readback"))
                                        .arg(e.frames)
                                        .arg(QString::number(e.sumMs / static_cast<double>(e.frames), 'f', 1))
                                        .arg(QString::number(e.maxMs, 'f', 1))
                                        .arg(dropped - timingDropped_);
      }
      e = EncodeTiming();
    }
    timingDropped_ = dropped;
    timingStart_ = now;
  }

  // ADR 0012 D5: libx264 changes its bitrate at runtime; any other encoder is reopened (with a keyframe) at most every 5 s.
  void applyTargetBitrate(const Job& j) {
    const int want = targetKbps_.load() * 1000;
    if (!encoder_.isOpen() || want == encoder_.bitrate()) return;
    if (encoder_.setBitrate(want)) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReopen_ < std::chrono::seconds(5)) return;  // the target stays pending
    const QString name = encoder_.name();
    if (!encoder_.open(j.width, j.height, options_.fps, want, {name}) &&
        !encoder_.open(j.width, j.height, options_.fps, want, cpuOrder())) {
      failed_ = true;
      onOpenFailed_();
      return;
    }
    lastReopen_ = now;
    std::lock_guard<std::mutex> lock(stats_->mutex);
    stats_->encoderName = encoder_.name();
  }

  // The same 5 s rule for the CUDA encoder (a runtime reconfigure forces an IDR in FFmpeg anyway). A failed reopen
  // leaves GPU mode.
  void applyGpuBitrate(AVBufferRef* fc, const QSize& visible) {
    const int want = targetKbps_.load() * 1000;
    if (!gpuEncoder_.isOpen() || want == gpuEncoder_.bitrate()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReopen_ < std::chrono::seconds(5)) return;  // the target stays pending
    QString why;
    if (!gpuEncoder_.openGpu(fc, options_.fps, want, &why, visible)) {
      failGpu(why.isEmpty() ? QStringLiteral("%1 does not reopen with %2 frames").arg(gpuEncoderName(fc), gpuFramesLabel(fc)) : why, fc);
      return;
    }
    lastReopen_ = now;
  }

  void encodeAudio(const Job& j) {
    if (!opus_.isOpen()) return;
    std::vector<std::vector<uint8_t>> packets;
    opus_.push(j.pcm, j.rate, packets);
    if (packets.empty()) return;
    const std::vector<Sink> sinks = sinks_->get();
    for (const auto& pk : packets) {
      const double seconds = static_cast<double>(audioFramesSent_++) * 0.02 + audioClockOffset_;
      ++stats_->audioFrames;
      stats_->audioBytes += static_cast<qint64>(pk.size());
      for (const Sink& sink : sinks) {
        send(sink.audio, pk.data(), pk.size(), seconds, "audio");
      }
    }
  }

  void send(const std::shared_ptr<rtc::Track>& track, const uint8_t* data, size_t size, double seconds, const char* what) {
    if (!track || !track->isOpen()) return;
    try {
      track->sendFrame(toBinary(data, size), rtc::FrameInfo(std::chrono::duration<double>(seconds)));
    } catch (const std::exception& e) {
      if (sendErrors_++ % 500 == 0) {  // per-frame failures must not flood the log
        qCWarning(lcHost) << "sendFrame(" << what << "):" << e.what() << "(failures so far:" << sendErrors_ << ")";
      }
    }
  }

  const SessionHost::Options options_;
  std::atomic<int> targetKbps_;
  std::shared_ptr<SinkList> sinks_;
  std::shared_ptr<WorkerStats> stats_;
  std::function<void(QString)> onError_;
  std::function<void()> onOpenFailed_;
  std::function<void()> onGpuActive_;       // worker thread; the owner posts it to its thread with the generation check
  std::function<void(QString)> onGpuOff_;   // ... likewise: the GPU path failed, reason
  std::function<void(int)> onTierFailed_;   // ... likewise: a fixed tier's encoder does not open or failed (tier)
  std::atomic<int> fixedMask_{0};
  std::atomic<int> failedReset_{0};
  std::atomic<bool> adaptiveWanted_{true};
  std::atomic<bool> gpuSeen_{false};

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  qint64 pendingAudio_ = 0;
  double droppedAudioSeconds_ = 0;  // PCM dropped by pushAudio, not yet added to audioClockOffset_
  bool stop_ = false;
  std::atomic<bool> keyframe_{false};
  std::atomic<bool> gpuDisabled_{false};  // set by the owner (UI thread)

  // Worker-thread only:
  struct EncodeTiming {
    qint64 frames = 0;
    double sumMs = 0, maxMs = 0;
  };
  std::chrono::steady_clock::time_point lastReopen_{};
  VideoEncoder encoder_;     // readback frames
  VideoEncoder gpuEncoder_;  // CUDA frames (h264_nvenc); a second instance next to encoder_ (ADR 0019)
  struct Fixed {
    VideoEncoder enc;
    int64_t lastPts = -1;
    qint64 lastNs = -1;
    bool failed = false;
    std::atomic<bool> keyframe{false};
  };
  Fixed fixed_[4];  // [1..3] used: High, Medium, Low
  GpuState gpu_ = GpuState::Untried;
  EncodeTiming timing_[2];   // [0] readback, [1] gpu-direct
  std::chrono::steady_clock::time_point timingStart_{};
  qint64 timingDropped_ = 0;
  OpusFramer opus_;
  bool failed_ = false;
  bool encodeErrorReported_ = false;
  int64_t lastPts_ = -1;
  qint64 audioFramesSent_ = 0;
  double audioClockOffset_ = 0;  // seconds of audio dropped under overload; keeps audio timestamps aligned with video
  int sendErrors_ = 0;

  std::thread thread_;  // last member: starts after everything else is constructed
};

SessionHost::SessionHost(QObject* parent) : QObject(parent), forceRelay_(forceRelayFromEnv()), bridge_(std::make_shared<ThreadBridge>(this)) {
  sinks_ = std::make_shared<SinkList>();
  workerStats_ = std::make_shared<WorkerStats>();
  statsTimer_.setInterval(1000);
  connect(&statsTimer_, &QTimer::timeout, this, &SessionHost::updateStats);
  clock_.start();
  adaptClock_.start();
  pingTimer_.setInterval(kPingIntervalMs);
  connect(&pingTimer_, &QTimer::timeout, this, &SessionHost::sendPings);
  controlClock_.start();
  controlTimer_.setInterval(50);
  connect(&controlTimer_, &QTimer::timeout, this, &SessionHost::tickControl);
}

SessionHost::~SessionHost() {
  bridge_->detach();
  close();  // joins the worker thread
}

void SessionHost::open(const QString& sessionId, const QStringList& iceServers, const QList<TurnServer>& turnServers) {
  close();
  sessionId_ = sessionId;
  iceServers_ = iceServers;
  turnServers_ = turnServers;
  relayTcpOnly_ = turnServersTcpOnly(turnServers);
  bitrate_.reset(options_.videoBitrate / 1000);
  targetKbps_ = bitrate_.targetKbps();
  tiers_.clear();
  pingTimer_.start();
  controlTimer_.start();
  encoderFailed_ = false;
  gpuInputOff_ = false;
  gpuInputFailed_ = false;
  gpuActive_ = false;
  gpuSkips_ = 0;
  lastGpuNs_ = 0;
  QString notUsed;
  gpuConfigured_ = VideoEncoder::gpuInputConfigured(&notUsed);
  if (!gpuConfigured_) {
    qCInfo(lcHost).noquote() << QStringLiteral("GPU-direct encoding not used: %1").arg(notUsed);
  } else if (const cuda::Driver& driver = cuda::Driver::instance();
             driver.state() == cuda::Driver::State::Unavailable || driver.state() == cuda::Driver::State::Dead) {
    qCInfo(lcHost).noquote() << QStringLiteral("GPU-direct encoding not used: CUDA unavailable (%1)").arg(driver.reason());
  }
  open_ = true;
}

void SessionHost::close() {
  const QStringList ids = viewers_.keys();
  for (const QString& id : ids) {
    dropViewer(id, QStringLiteral("session_closed"));
  }
  stopEncoder();
  pingTimer_.stop();
  controlTimer_.stop();
  tiers_.clear();
  hostStateSent_.clear();
  open_ = false;
}

// ---------------------------------------------------------------- viewers / signaling

void SessionHost::updateIceServers(const QStringList& iceServers, const QList<TurnServer>& turnServers) {
  iceServers_ = iceServers;
  turnServers_ = turnServers;
  relayTcpOnly_ = turnServersTcpOnly(turnServers);
}

void SessionHost::addViewer(const QString& viewerId, const QList<TurnServer>& turnServers) {
  if (!open_ || viewers_.contains(viewerId)) {
    return;
  }
  Viewer v;
  try {
    rtc::Configuration cfg = makeRtcConfig(iceServers_, turnServers.isEmpty() ? turnServers_ : turnServers, forceRelay_);
    cfg.disableAutoNegotiation = true;  // creating the data channel must not trigger an offer of its own; we offer explicitly below
    v.pc = std::make_shared<rtc::PeerConnection>(cfg);
    const auto bridge = bridge_;
    const QString id = viewerId;

    const auto addTrack = [&](bool isVideo) {
      const rtc::SSRC ssrc = QRandomGenerator::global()->generate();
      const char* cname = isVideo ? "fb-video" : "fb-audio";
      std::shared_ptr<rtc::Track> track;
      if (isVideo) {
        rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
        media.addH264Codec(kVideoPayloadType);
        media.addSSRC(ssrc, cname);
        track = v.pc->addTrack(media);
      } else {
        rtc::Description::Audio media("audio", rtc::Description::Direction::SendOnly);
        media.addOpusCodec(kAudioPayloadType);
        media.addSSRC(ssrc, cname);
        track = v.pc->addTrack(media);
      }
      auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(
          ssrc, cname, isVideo ? kVideoPayloadType : kAudioPayloadType,
          isVideo ? rtc::H264RtpPacketizer::ClockRate : kOpusClockRate);
      std::shared_ptr<rtc::RtpPacketizer> packetizer;
      if (isVideo) {
        packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, cfg, kMaxFragment);
      } else {
        packetizer = std::make_shared<rtc::RtpPacketizer>(cfg)  /* non-template base: one Opus frame per packet */;
      }
      packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(cfg));
      packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
      if (isVideo) {
        packetizer->addToChain(std::make_shared<rtc::PliHandler>(guarded("pli", [this, bridge, id]() {
          bridge->post([this, id]() {
            ++keyframeRequests_;
            requestKeyframeFor(id);
          });
        })));
        // First frame must be an IDR even if the track opens in the middle of a GOP.
        track->onOpen(guarded("onOpen", [this, bridge, id]() { bridge->post([this, id]() { requestKeyframeFor(id); }); }));
      }
      track->setMediaHandler(packetizer);
      return track;
    };
    v.video = addTrack(true);
    v.audio = addTrack(false);
    // Negotiated data channel (same SDP flow, no protocol change): establishes SCTP, which libdatachannel needs
    // to measure RTT (PeerConnection::rtt()). Never used for payload.
    rtc::DataChannelInit diagInit;
    diagInit.negotiated = true;
    diagInit.id = kDiagChannelId;
    v.diag = v.pc->createDataChannel("fb-diag", diagInit);
    // Viewer reports {"t":"rx","loss":..,"kbps":..} drive the bitrate adaptation; everything else is ignored.
    v.diag->onMessage(guarded("diag onMessage", [this, bridge, id](rtc::message_variant m) {
      const auto* text = std::get_if<rtc::string>(&m);
      if (text == nullptr) return;
      const QByteArray raw(text->data(), static_cast<qsizetype>(text->size()));
      if (const auto report = parseRxReport(raw)) {
        bridge->post([this, id, r = *report]() { onRxReport(id, r); });
      } else if (const auto pick = parseQualityPick(raw)) {
        bridge->post([this, id, q = *pick]() { onQualityPick(id, q); });
      } else if (const auto ping = parsePingPong(raw)) {
        bridge->post([this, id, m = *ping]() { onPingPong(id, m); });
      } else if (const auto port = parseControlRequest(raw)) {
        bridge->post([this, id, p = *port]() { onControlRequest(id, p); });
      } else if (parseControlRelease(raw)) {
        bridge->post([this, id]() { onControlRelease(id); });
      }  // anything else (unknown type, garbage) is ignored
    }));
    // Controller input (ADR 0033 D8): binary frames on a second negotiated channel, unordered and without retransmits.
    rtc::DataChannelInit inputInit;
    inputInit.negotiated = true;
    inputInit.id = kInputChannelId;
    inputInit.reliability.unordered = true;
    inputInit.reliability.maxRetransmits = 0;
    v.input = v.pc->createDataChannel("fb-input", inputInit);
    v.input->onMessage(guarded("input onMessage", [this, bridge, id](rtc::message_variant m) {
      const auto* bin = std::get_if<rtc::binary>(&m);
      if (bin == nullptr) return;
      const auto frame = decodeInputFrame(QByteArray(reinterpret_cast<const char*>(bin->data()), static_cast<qsizetype>(bin->size())));
      if (!frame) return;
      bridge->post([this, id, f = *frame]() { applyInputFrame(id, f); });
    }));

    // The state signal (paused / running) goes out as soon as the channel is open, for a viewer that joins while paused.
    v.diag->onOpen(guarded("diag onOpen", [this, bridge, id]() {
      bridge->post([this, id]() {
        sendHostState(id);
        sendTier(id, /*force*/ true);
        if (control_.portCount() > 0) sendDiagTo(id, makeControlCaps(control_.portCount()));
      });
    }));

    v.pc->onLocalDescription(guarded("onLocalDescription", [this, bridge, id](rtc::Description d) {
      SessionSignal s;
      s.viewerId = id;
      s.kind = QString::fromStdString(d.typeString());
      s.sdp = QString::fromStdString(std::string(d));
      bridge->post([this, s]() mutable {
        s.sessionId = sessionId_;
        emit signalOut(s);
      });
    }));
    v.pc->onLocalCandidate(guarded("onLocalCandidate", [this, bridge, id](rtc::Candidate c) {
      SessionSignal s;
      s.viewerId = id;
      s.kind = QStringLiteral("candidate");
      s.candidate = QString::fromStdString(c.candidate());
      s.mid = QString::fromStdString(c.mid());
      const int type = static_cast<int>(c.type());
      bridge->post([this, s, id, type]() mutable {
        onLocalCandidateType(id, type);
        s.sessionId = sessionId_;
        emit signalOut(s);
      });
    }));
    v.pc->onGatheringStateChange(guarded("onGatheringStateChange", [this, bridge, id](rtc::PeerConnection::GatheringState st) {
      if (st == rtc::PeerConnection::GatheringState::Complete) {
        bridge->post([this, id]() { onGatheringComplete(id); });
      }
    }));
    v.pc->onStateChange(guarded("onStateChange", [this, bridge, id](rtc::PeerConnection::State st) {
      bridge->post([this, id, st]() { onPcState(id, static_cast<int>(st)); });
    }));
  } catch (const std::exception& e) {
    emit errorOccurred(QStringLiteral("PeerConnection: ") + QString::fromUtf8(e.what()));
    return;
  }
  Viewer& stored = viewers_.insert(viewerId, std::move(v)).value();
  viewerCountAtomic_ = static_cast<int>(viewers_.size());
  tiers_.addViewer(viewerId, tierNowMs());
  applyTiers();
  requestKeyframe();
  qCInfo(lcHost) << "Viewer" << viewerId << "added, offering";
  try {
    stored.pc->setLocalDescription(rtc::Description::Type::Offer);  // offer goes out via onLocalDescription
  } catch (const std::exception& e) {
    emit errorOccurred(QStringLiteral("Offer: ") + QString::fromUtf8(e.what()));
    dropViewer(viewerId, QStringLiteral("error"));
  }
}

void SessionHost::removeViewer(const QString& viewerId) { dropViewer(viewerId, QStringLiteral("removed")); }

void SessionHost::setHostState(bool paused, const QString& by) {
  const QString name = paused ? by : QString();
  if (paused == hostPaused_ && name == hostPausedBy_) return;
  hostPaused_ = paused;
  hostPausedBy_ = name;
  const QStringList ids = viewers_.keys();
  for (const QString& id : ids) sendHostState(id);
}

void SessionHost::sendHostState(const QString& viewerId) {
  const auto it = viewers_.constFind(viewerId);
  if (it == viewers_.constEnd() || !it->diag) return;
  // A viewer that joins a running game needs no message: "running" is the state it starts in.
  if (!hostPaused_ && !hostStateSent_.contains(viewerId)) return;
  try {
    if (it->diag->isOpen()) {
      const QByteArray msg = makeHostState(hostPaused_, hostPausedBy_);
      it->diag->send(std::string(msg.constData(), static_cast<size_t>(msg.size())));
      if (hostPaused_) hostStateSent_.insert(viewerId);
      else hostStateSent_.remove(viewerId);
    }
  } catch (const std::exception& e) {
    qCWarning(lcHost) << "host state:" << e.what();
  }
}

// ---------------------------------------------------------------- controller hand-over (ADR 0033 D8)

void SessionHost::sendDiagTo(const QString& viewerId, const QByteArray& msg) {
  const auto it = viewers_.constFind(viewerId);
  if (it == viewers_.constEnd() || !it->diag) return;
  try {
    if (it->diag->isOpen()) it->diag->send(std::string(msg.constData(), static_cast<size_t>(msg.size())));
  } catch (const std::exception& e) {
    qCWarning(lcHost) << "diag send:" << e.what();
  }
}

void SessionHost::setControlPorts(int ports) {
  const int before = control_.portCount();
  const QList<ControlArbiter::End> ended = control_.setPortCount(ports);
  for (const auto& e : ended) reportEnd(e, true);
  if (control_.portCount() == before && ended.isEmpty()) return;
  const QByteArray caps = makeControlCaps(control_.portCount());
  for (const QString& id : viewers_.keys()) sendDiagTo(id, caps);
  emit controlChanged();
}

void SessionHost::onControlRequest(const QString& viewerId, int port) {
  const auto it = viewers_.constFind(viewerId);
  if (!open_ || it == viewers_.constEnd()) return;
  ControlArbiter::Request r;
  if (!it->input || !it->input->isOpen()) {
    r.reason = QStringLiteral("unsupported");  // the input channel is not up (an older app, or not connected yet)
  } else {
    r = control_.request(viewerId, port, controlNowMs());
  }
  if (r.kind == ControlArbiter::Request::Denied) {
    sendDiagTo(viewerId, makeControlAnswer(port, false, r.reason));
    return;
  }
  emit controlChanged();
  emit controlRequested(viewerId, port);
}

void SessionHost::onControlRelease(const QString& viewerId) {
  const bool had = control_.portOf(viewerId) >= 0 || control_.hasPending(viewerId);
  if (const auto e = control_.release(viewerId, QStringLiteral("viewer"))) reportEnd(*e, false);
  if (had) emit controlChanged();
}

bool SessionHost::answerControl(const QString& viewerId, bool allow) {
  const ControlArbiter::Answer a = control_.answer(viewerId, allow, controlNowMs());
  if (!a.known) return false;
  sendDiagTo(viewerId, makeControlAnswer(a.port, a.granted, a.granted ? QString() : QStringLiteral("denied")));
  if (a.granted) emit controlInput(a.port, control_.input(a.port), true);
  emit controlChanged();
  return true;
}

bool SessionHost::takeBack(int port) {
  const auto e = control_.releasePort(port, QStringLiteral("host"));
  if (!e) return false;
  reportEnd(*e, true);
  emit controlChanged();
  return true;
}

bool SessionHost::takeBackViewer(const QString& viewerId) {
  const int p = control_.portOf(viewerId);
  return p >= 0 && takeBack(p);
}

void SessionHost::reportEnd(const ControlArbiter::End& e, bool notifyViewer) {
  if (notifyViewer) sendDiagTo(e.viewerId, makeControlEnd(e.port, e.reason));
  emit controlInput(e.port, InputFrame(), true);  // the port is neutral again
  emit controlEnded(e.viewerId, e.port, e.reason);
}

ControlArbiter::Verdict SessionHost::applyInputFrame(const QString& viewerId, const InputFrame& f) {
  const ControlArbiter::Verdict v = control_.accept(viewerId, f, controlNowMs());
  if (v == ControlArbiter::Verdict::Applied) emit controlInput(f.port, f, false);
  return v;
}

void SessionHost::tickControl() {
  const qint64 now = controlNowMs();
  bool changed = false;
  for (const auto& x : control_.expire(now)) {
    sendDiagTo(x.viewerId, makeControlAnswer(x.port, false, QStringLiteral("timeout")));
    changed = true;
  }
  for (const int p : control_.expireNeutral(now)) emit controlInput(p, InputFrame(), true);
  if (changed) emit controlChanged();
}

void SessionHost::dropViewer(const QString& viewerId, const QString& reason) {
  auto it = viewers_.find(viewerId);
  if (it == viewers_.end()) {
    return;
  }
  Viewer v = std::move(it.value());
  viewers_.erase(it);
  const bool hadControl = control_.portOf(viewerId) >= 0 || control_.hasPending(viewerId);
  if (const auto e = control_.release(viewerId, QStringLiteral("left"))) reportEnd(*e, false);
  if (hadControl) emit controlChanged();
  viewerCountAtomic_ = static_cast<int>(viewers_.size());
  bitrate_.removeViewer(viewerId);
  reports_.remove(viewerId);
  hostStateSent_.remove(viewerId);
  tiers_.removeViewer(viewerId, tierNowMs());
  if (viewers_.isEmpty()) tiers_.clear();
  applyTiers();  // the worker stops sending to this viewer before its PeerConnection closes
  QString finalState = QStringLiteral("closed");
  try {
    v.pc->resetCallbacks();
    if (v.video) {
      v.video->resetCallbacks();
    }
    if (v.audio) {
      v.audio->resetCallbacks();
    }
    if (v.diag) {
      v.diag->resetCallbacks();
    }
    if (v.input) {
      v.input->resetCallbacks();
    }
    v.pc->close();  // immediate: transports are shut down, the media path is cut
    // With SCTP up, libdatachannel finishes the state change to Closed asynchronously; the connection is cut now.
    finalState = v.pc->state() == rtc::PeerConnection::State::Failed ? QStringLiteral("failed") : QStringLiteral("closed");
  } catch (const std::exception& e) {
    qCWarning(lcHost) << "Closing PeerConnection:" << e.what();
  }
  v.video.reset();
  v.audio.reset();
  v.diag.reset();
  v.input.reset();
  v.pc.reset();
  qCInfo(lcHost) << "Viewer" << viewerId << "closed (" << reason << ")," << viewers_.size() << "left";
  if (viewers_.isEmpty()) {
    stopEncoder();
  }
  emit viewerClosed(viewerId, reason + QLatin1Char(':') + finalState);
}

void SessionHost::onLocalCandidateType(const QString& viewerId, int type) {
  if (const auto it = viewers_.find(viewerId); it != viewers_.end()) {
    countCandidate(it->ice.local, static_cast<rtc::Candidate::Type>(type));
  }
}

void SessionHost::onGatheringComplete(const QString& viewerId) {
  if (const auto it = viewers_.find(viewerId); it != viewers_.end()) {
    it->ice.gatheringComplete = true;
    qCInfo(lcHost).noquote() << QStringLiteral("ICE viewer %1: gathering complete, local %2").arg(viewerId, it->ice.local.text());
  }
}

void SessionHost::onPcState(const QString& viewerId, int state) {
  auto it = viewers_.find(viewerId);
  if (it == viewers_.end()) {
    return;
  }
  const auto st = static_cast<rtc::PeerConnection::State>(state);
  it->state = stateName(st);
  if (st == rtc::PeerConnection::State::Connected && !it->connected) {
    it->connected = true;
    const QString type = it->pc ? connectionTypeOf(*it->pc, relayTcpOnly_, forceRelay_) : QString();
    const RelayReason reason = classifyRelayReason(type, forceRelay_, it->ice.local, it->ice.remote);
    qCInfo(lcHost).noquote() << QStringLiteral("ICE viewer %1: connected %2, pair %3, local %4, remote %5%6")
                                    .arg(viewerId, type.isEmpty() ? QStringLiteral("(unknown)") : type,
                                         it->pc ? selectedPairText(*it->pc) : QString(), it->ice.local.text(), it->ice.remote.text(),
                                         reason == RelayReason::None ? QString() : QStringLiteral(", relay reason %1").arg(relayReasonToken(reason)));
    requestKeyframe();
    emit viewerConnected(viewerId);
  } else if (st == rtc::PeerConnection::State::Failed || st == rtc::PeerConnection::State::Closed) {
    if (st == rtc::PeerConnection::State::Failed) {
      qCInfo(lcHost).noquote() << QStringLiteral("ICE viewer %1: failed (%2 connected), local %3, remote %4")
                                      .arg(viewerId, it->connected ? QStringLiteral("was") : QStringLiteral("never"), it->ice.local.text(),
                                           it->ice.remote.text());
    }
    dropViewer(viewerId, QStringLiteral("connection_") + it->state);
  }
}

void SessionHost::onRxReport(const QString& viewerId, const RxReport& report) {
  if (!open_ || !viewers_.contains(viewerId)) {
    return;
  }
  reports_.set(viewerId, report);
  const bool autoViewer = tiers_.isAuto(viewerId);
  {
    std::lock_guard<std::mutex> lock(statsMutex_);  // visible to viewerLinks() right away, not only after the next stats tick
    for (ViewerLinkStats& l : links_) {
      if (l.viewerId == viewerId) {
        reports_.applyTo(l);
      }
    }
  }
  emit rxReportReceived(viewerId, report.loss, report.kbps);
  if (!autoViewer) return;  // a fixed tier has a fixed bitrate: only Auto viewers steer the adaptive encoder
  if (const auto target = bitrate_.report(viewerId, report, adaptClock_.elapsed())) {
    qCInfo(lcHost) << "Target bitrate" << *target << "kbit/s (viewer" << viewerId << "loss" << report.loss << ")";
    targetKbps_ = *target;
    if (worker_) {
      worker_->setTargetBitrate(*target);
    }
    for (const QString& id : viewers_.keys()) {
      if (tiers_.isAuto(id)) sendTier(id, /*force*/ false);
    }
  }
}

void SessionHost::onQualityPick(const QString& viewerId, Quality pick) {
  if (!open_ || !viewers_.contains(viewerId)) return;
  const TierManager::Change ch = tiers_.setPick(viewerId, pick, tierNowMs());
  if (ch.effective != Quality::Auto) bitrate_.removeViewer(viewerId);  // a frozen report must not keep steering the adaptive encoder
  qCInfo(lcHost).noquote() << QStringLiteral("Viewer %1 picked %2, served by %3%4")
                                  .arg(viewerId, qualityLabel(pick), qualityLabel(ch.effective), ch.switched ? QStringLiteral(" (switched)") : QString());
  applyTiers();
  sendTier(viewerId, /*force*/ true);  // also answers a pick that changes nothing
}

void SessionHost::onPingPong(const QString& viewerId, const PingMessage& m) {
  const auto it = viewers_.find(viewerId);
  if (!open_ || it == viewers_.end() || !it->diag) return;
  if (!m.pong) {
    try {
      if (it->diag->isOpen()) {
        const QByteArray msg = makePong(m.n, m.ts);
        it->diag->send(std::string(msg.constData(), static_cast<size_t>(msg.size())));
      }
    } catch (const std::exception& e) {
      qCWarning(lcHost) << "pong:" << e.what();
    }
    return;
  }
  if (const auto rtt = rttFromPong(tierNowMs(), m.ts)) {
    it->latencyMs = *rtt;
    std::lock_guard<std::mutex> lock(statsMutex_);
    for (ViewerLinkStats& l : links_) {
      if (l.viewerId == viewerId) l.latencyMs = *rtt;
    }
  }
}

void SessionHost::sendPings() {
  const qint64 now = tierNowMs();
  for (auto it = viewers_.begin(); it != viewers_.end(); ++it) {
    if (!it->diag) continue;
    try {
      if (it->diag->isOpen()) {
        const QByteArray msg = makePing(++it->pingN, now);
        it->diag->send(std::string(msg.constData(), static_cast<size_t>(msg.size())));
      }
    } catch (const std::exception& e) {
      qCWarning(lcHost) << "ping:" << e.what();
    }
  }
}

// "tier" message (host to viewer): what this viewer gets. Auto: the adaptive target, resent when it moved by 15 % or more.
void SessionHost::sendTier(const QString& viewerId, bool force) {
  const auto it = viewers_.find(viewerId);
  if (it == viewers_.end() || !it->diag || !tiers_.hasViewer(viewerId)) return;
  const Quality tier = tiers_.effective(viewerId);
  const int kbps = tier == Quality::Auto ? autoKbps() : qualityKbps(tier);
  if (!force && tier == it->sentTier && it->sentKbps > 0 && std::abs(kbps - it->sentKbps) * 100 < it->sentKbps * 15) return;
  try {
    if (!it->diag->isOpen()) return;  // sent again when the channel opens
    const QByteArray msg = makeTier(tier, kbps);
    it->diag->send(std::string(msg.constData(), static_cast<size_t>(msg.size())));
    it->sentTier = tier;
    it->sentKbps = kbps;
  } catch (const std::exception& e) {
    qCWarning(lcHost) << "tier:" << e.what();
  }
}

void SessionHost::requestKeyframeFor(const QString& viewerId) {
  if (!tiers_.hasViewer(viewerId)) {
    requestKeyframe();
    return;
  }
  const int tier = static_cast<int>(tiers_.effective(viewerId));
  if (tier == 0) {
    requestKeyframe();
  } else if (worker_) {
    worker_->requestKeyframe(tier);
  }  // no worker yet: the fixed encoder opens with the first frame, which is a keyframe
}

// Sinks, worker encoders and keyframes after any change of viewers or picks. A viewer that moved to another encoder gets a
// keyframe from it (its decoder needs a fresh start) and the matching tier message.
void SessionHost::applyTiers() {
  for (auto it = viewers_.begin(); it != viewers_.end(); ++it) {
    if (!tiers_.isAuto(it.key())) bitrate_.removeViewer(it.key());  // also covers tick() and substitution moves
  }
  for (auto it = viewers_.begin(); it != viewers_.end(); ++it) {
    const Quality eff = tiers_.effective(it.key());
    if (eff != it->fedTier) {
      it->fedTier = eff;
      requestKeyframeFor(it.key());
    }
  }
  publishSinks();
  if (worker_) worker_->setTiers(tiers_.fixedMask(), tiers_.adaptiveAlive());
}

void SessionHost::tickTiers() {
  if (tiers_.tick(tierNowMs())) {
    applyTiers();
    for (const QString& id : viewers_.keys()) sendTier(id, /*force*/ false);
  }
}

void SessionHost::applyRemoteCandidate(Viewer& v, const SessionSignal& s) {
  try {
    const rtc::Candidate cand(s.candidate.toStdString(), s.mid.toStdString());
    countCandidate(v.ice.remote, cand.type());
    v.pc->addRemoteCandidate(cand);
  } catch (const std::exception& e) {
    qCWarning(lcHost) << "Remote candidate rejected:" << e.what();
  }
}

void SessionHost::handleSignal(const SessionSignal& s) {
  auto it = viewers_.find(s.viewerId);
  if (it == viewers_.end() || (!sessionId_.isEmpty() && s.sessionId != sessionId_)) {
    return;
  }
  Viewer& v = it.value();
  try {
    if (s.kind == QLatin1String("answer")) {
      v.pc->setRemoteDescription(rtc::Description(s.sdp.toStdString(), rtc::Description::Type::Answer));
      v.remoteSet = true;
      const QList<SessionSignal> pending = std::exchange(v.pendingCandidates, {});
      for (const SessionSignal& c : pending) {
        applyRemoteCandidate(v, c);
      }
    } else if (s.kind == QLatin1String("candidate")) {
      if (v.remoteSet) {
        applyRemoteCandidate(v, s);
      } else {
        v.pendingCandidates.append(s);
      }
    }
  } catch (const std::exception& e) {
    emit errorOccurred(QStringLiteral("Signal: ") + QString::fromUtf8(e.what()));
    dropViewer(s.viewerId, QStringLiteral("error"));
  }
}

// ---------------------------------------------------------------- encoder / media input

void SessionHost::publishSinks() {
  std::vector<Sink> sinks;
  for (auto it = viewers_.constBegin(); it != viewers_.constEnd(); ++it) {
    sinks.push_back({it->video, it->audio, static_cast<int>(tiers_.effective(it.key()))});
  }
  sinks_->set(std::move(sinks));
}

void SessionHost::requestKeyframe() {
  if (worker_) {
    worker_->requestKeyframe();
  } else {
    keyframePending_ = true;
  }
}

void SessionHost::startEncoder() {
  auto stats = std::make_shared<WorkerStats>();
  {
    std::lock_guard<std::mutex> lock(statsMutex_);
    workerStats_ = stats;
  }
  const unsigned gen = ++workerGen_;
  const auto bridge = bridge_;
  Options workerOptions = options_;
  workerOptions.videoBitrate = bitrate_.targetKbps() * 1000;  // a restarted encoder keeps the adapted rate
  worker_ = std::make_unique<EncodeWorker>(
      workerOptions, sinks_, stats,
      [this, bridge](QString msg) { bridge->post([this, msg]() { emit errorOccurred(msg); }); },
      [this, bridge, gen]() {
        bridge->post([this, gen]() {
          if (gen != workerGen_ || !worker_) return;
          encoderFailed_ = true;
          emit errorOccurred(QStringLiteral("No H.264 encoder can be opened"));
          stopEncoder();
        });
      },
      [this, bridge, gen]() {  // the first CUDA frame was encoded: GPU mode is confirmed
        bridge->post([this, gen]() {
          if (gen != workerGen_ || !worker_ || gpuInputOff_) return;
          gpuActive_ = true;
          lastGpuNs_ = clock_.nsecsElapsed();  // the watchdog counts from the confirmation
          emit gpuInputChanged();
        });
      },
      [this, bridge, gen](QString reason) {  // the GPU path failed in the worker; it already left GPU mode
        bridge->post([this, gen, reason]() {
          if (gen != workerGen_ || !worker_) return;
          disableGpuInput(reason, true);
        });
      },
      [this, bridge, gen](int tier) {  // a fixed tier's encoder failed: its viewers go back to Auto
        bridge->post([this, gen, tier]() {
          if (gen != workerGen_ || !worker_) return;
          qCWarning(lcHost) << "Quality tier" << qualityLabel(static_cast<Quality>(tier)) << "failed; its viewers return to Auto";
          for (const QString& id : viewers_.keys()) {
            if (static_cast<int>(tiers_.effective(id)) == tier || static_cast<int>(tiers_.pick(id)) == tier) {
              tiers_.setPick(id, Quality::Auto, tierNowMs());
              sendTier(id, true);
            }
          }
          tiers_.evict(static_cast<Quality>(tier));  // not pickable again from the idle cache; a later pick starts it fresh
          applyTiers();
          worker_->resetTierFailure(tier);
        });
      });
  worker_->setTiers(tiers_.fixedMask(), tiers_.adaptiveAlive());
  keyframePending_ = false;  // a fresh encoder starts with a keyframe anyway
  lastEncodeNs_ = 0;
  lastCpuEncodeNs_ = 0;
  lastStampNs_ = 0;
  clock_.restart();
  statsClock_.restart();
  lastStatFrames_ = lastStatVideoBytes_ = lastStatAudioBytes_ = 0;
  statsTimer_.start();
  encoderRunning_ = true;
  emit encoderRunningChanged(true);
}

void SessionHost::stopEncoder() {
  if (!encoderRunning_ && !worker_) {
    return;
  }
  statsTimer_.stop();
  worker_.reset();  // joins the worker thread: no encode or send runs after this line
  encoderRunning_ = false;
  gpuActive_ = false;  // a new worker starts with the CPU encoder; gpuInputOff_ stays for this Session
  gpuSkips_ = 0;
  lastGpuNs_ = 0;
  {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_ = SessionStats();
    links_.clear();
    workerStats_ = std::make_shared<WorkerStats>();
  }
  emit encoderRunningChanged(false);
}

void SessionHost::pushFrame(const QImage& image, qint64 lateNs) {
  if (!open_ || viewers_.isEmpty() || image.isNull()) {
    return;
  }
  if (image.format() == QImage::Format_RGB32 || image.format() == QImage::Format_ARGB32 ||
      image.format() == QImage::Format_ARGB32_Premultiplied) {
    pushFrame(image.constBits(), image.width(), image.height(), static_cast<int>(image.bytesPerLine()), RawPixelFormat::Xrgb8888, lateNs);
  } else if (image.format() == QImage::Format_RGB16) {
    pushFrame(image.constBits(), image.width(), image.height(), static_cast<int>(image.bytesPerLine()), RawPixelFormat::Rgb565, lateNs);
  } else {
    const QImage conv = image.convertToFormat(QImage::Format_RGB32);
    pushFrame(conv.constBits(), conv.width(), conv.height(), static_cast<int>(conv.bytesPerLine()), RawPixelFormat::Xrgb8888, lateNs);
  }
}

void SessionHost::pushFrame(const uint8_t* data, int width, int height, int stride, RawPixelFormat format, qint64 lateNs) {
  if (!open_ || viewers_.isEmpty() || encoderFailed_ || data == nullptr || width <= 0 || height <= 0) {
    return;  // encoding only while somebody watches
  }
  if (!encoderRunning_) {
    bool anyConnected = false;
    for (auto it = viewers_.constBegin(); it != viewers_.constEnd(); ++it) {
      anyConnected = anyConnected || it->connected;
    }
    if (!anyConnected) {
      return;  // the encoder starts when the first viewer has a media path
    }
    startEncoder();
  }
  // GPU-direct and a fixed quality tier at once (ADR 0033 D5): the adaptive encoder takes the CUDA frames, the readback frame
  // below only feeds the software tier encoders (the worker skips it for the adaptive one). Its own gap clock, because the
  // GPU frame of the same emulation frame just moved lastEncodeNs_.
  bool cpuOnlyForTiers = false;
  if (gpuInputActive()) {
    // Trips only when both hold: kGpuWatchdogFrames readback frames and no GPU frame for 1 s on the host clock. A UI
    // stall queues a burst of frame events that is drained in a few ms while the mailbox holds a single capture.
    if (++gpuSkips_ < kGpuWatchdogFrames || clock_.nsecsElapsed() - lastGpuNs_ < 1'000'000'000LL) {
      if (tiers_.fixedMask() == 0) return;  // the encoder takes GPU frames; no 10 MB copy
      cpuOnlyForTiers = true;
    } else {
      // No GPU frame for kGpuWatchdogFrames frames and a second, whatever the reason: back to readback; this frame is encoded below.
      disableGpuInput(QStringLiteral("no GPU frames for %1 frames").arg(kGpuWatchdogFrames), true);
    }
  }
  const qint64 nowNs = clock_.nsecsElapsed();
  const qint64 minGapNs = 1'000'000'000LL / (2 * std::max(1, options_.fps));
  qint64& gapClock = cpuOnlyForTiers ? lastCpuEncodeNs_ : lastEncodeNs_;
  if (gapClock != 0 && nowNs - gapClock < minGapNs) {
    return;  // source faster than 2x the target rate: drop before copying
  }
  gapClock = nowNs;
  if (keyframePending_) {
    keyframePending_ = false;
    worker_->requestKeyframe();
  }
  lastStampNs_ = streamTimestamp(nowNs, lateNs, lastStampNs_);
  worker_->pushVideo(data, width, height, stride, format, lastStampNs_);  // copies; encoding happens on the worker thread
}

// ---------------------------------------------------------------- GPU-direct input (ADR 0019)

void SessionHost::pushGpuFrame(std::shared_ptr<AVFrame> frame, qint64 lateNs) {
  if (!frame || !open_ || !encoderRunning_ || !worker_ || encoderFailed_ || gpuInputOff_ || !gpuConfigured_) {
    return;  // never starts the encoder: only readback frames do
  }
  gpuSkips_ = 0;  // watchdog: GPU frames arrive
  lastGpuNs_ = clock_.nsecsElapsed();
  const qint64 nowNs = clock_.nsecsElapsed();  // the same clock and gap as pushFrame()
  const qint64 minGapNs = 1'000'000'000LL / (2 * std::max(1, options_.fps));
  if (lastEncodeNs_ != 0 && nowNs - lastEncodeNs_ < minGapNs) {
    return;
  }
  lastEncodeNs_ = nowNs;
  if (keyframePending_) {
    keyframePending_ = false;
    worker_->requestKeyframe();
  }
  lastStampNs_ = streamTimestamp(nowNs, lateNs, lastStampNs_);
  worker_->pushGpuVideo(std::move(frame), lastStampNs_);
}

SessionHost::GpuBackends SessionHost::gpuBackendRules(const GpuInputFacts& f) {
  GpuBackends b;
  if (!f.open || f.off || !f.configured || f.orderPinned) {
    return b;  // tests pin encoders; kill switch and forced encoder are "not configured"
  }
  b.cuda = f.cudaDriverUsable && f.cudaInput && f.nvencProbed;
  b.amf = f.d3d11Capture && f.amfInput && f.amfProbed;
  b.qsv = f.d3d11Capture && f.qsvInput && f.qsvProbed;
  return b;
}

SessionHost::GpuBackends SessionHost::gpuBackendsAllowed() const {
  GpuInputFacts f;
  f.open = open_;
  f.off = gpuInputOff_;
  f.configured = gpuConfigured_;
  f.orderPinned = !options_.encoderOrder.isEmpty();
  if (!f.open || f.off || !f.configured || f.orderPinned) {
    return {};
  }
  const cuda::Driver::State state = cuda::Driver::instance().state();  // does not load the driver
  f.cudaDriverUsable = state != cuda::Driver::State::Unavailable && state != cuda::Driver::State::Dead;
  f.cudaInput = VideoEncoder::cudaInputSupported();
  const QStringList encoders = detectMediaCapabilities().encoders;  // cached; the Hub handshake computed it
  f.nvencProbed = encoders.contains(QStringLiteral("h264_nvenc"));
#ifdef _WIN32
  f.d3d11Capture = true;  // D3d11EncodeCapture exists on Windows only
#endif
  f.amfInput = VideoEncoder::amfInputSupported();
  f.amfProbed = encoders.contains(QStringLiteral("h264_amf"));
  f.qsvInput = VideoEncoder::qsvInputSupported();
  f.qsvProbed = encoders.contains(QStringLiteral("h264_qsv"));
  return gpuBackendRules(f);
}

void SessionHost::disableGpuInput(const QString& reason, bool failure) {
  if (gpuInputOff_) {
    return;
  }
  gpuInputOff_ = true;
  gpuInputFailed_ = failure;
  gpuActive_ = false;
  gpuSkips_ = 0;
  lastGpuNs_ = 0;
  if (failure) {
    qCWarning(lcHost).noquote() << QStringLiteral("GPU-direct encoding off for this Session (%1); encoding readback frames").arg(reason);
  } else {
    qCInfo(lcHost).noquote() << QStringLiteral("GPU-direct encoding not available (%1); encoding readback frames").arg(reason);
  }
  if (worker_) {
    worker_->disableGpu();
  }
  emit gpuInputChanged();
}

void SessionHost::pushAudio(const QByteArray& pcm, int sampleRate) {
  if (!open_ || viewers_.isEmpty() || !worker_) {
    return;  // the audio encoder starts together with the video encoder (first frame)
  }
  worker_->pushAudio(pcm, sampleRate);
}

QString SessionHost::encoderName() const { return stats().encoderName; }

// ---------------------------------------------------------------- stats

void SessionHost::updateStats() {
  tickTiers();
  std::shared_ptr<WorkerStats> ws;
  {
    std::lock_guard<std::mutex> lock(statsMutex_);
    ws = workerStats_;
  }
  const double secs = std::max(0.001, statsClock_.restart() / 1000.0);
  const qint64 frames = ws->frames, vBytes = ws->videoBytes, aBytes = ws->audioBytes;
  SessionStats s;
  s.active = encoderRunning_;
  s.fps = static_cast<double>(frames - lastStatFrames_) / secs;
  s.videoBitrateKbps = static_cast<double>(vBytes - lastStatVideoBytes_) * 8.0 / 1000.0 / secs;
  s.audioBitrateKbps = static_cast<double>(aBytes - lastStatAudioBytes_) * 8.0 / 1000.0 / secs;
  lastStatFrames_ = frames;
  lastStatVideoBytes_ = vBytes;
  lastStatAudioBytes_ = aBytes;
  s.codec = QStringLiteral("H264 + Opus");
  s.viewers = static_cast<int>(viewers_.size());
  s.keyframeRequests = keyframeRequests_;
  // Encoders with at least one viewer on them: one still waiting out its 10 s idle time sends nothing.
  QStringList names;
  for (const Quality q : tiers_.aliveTiers()) {
    if (tiers_.users(q) > 0) names << qualityLabel(q);
  }
  s.encoders = encoderRunning_ ? static_cast<int>(names.size()) : 0;
  s.tiersInUse = names.join(QStringLiteral(", "));
  // packet loss is not obtainable on the sending side (RTCP receiver reports are not exposed by libdatachannel)
  QList<ViewerLinkStats> links;
  int rank = -1;
  for (auto it = viewers_.begin(); it != viewers_.end(); ++it) {
    ViewerLinkStats l;
    l.viewerId = it.key();
    l.state = it->state;
    l.pick = tiers_.pick(it.key());
    l.tier = tiers_.effective(it.key());
    l.tierKbps = l.tier == Quality::Auto ? autoKbps() : qualityKbps(l.tier);
    l.latencyMs = it->latencyMs;
    if (it->connected && it->pc) {
      l.rttMs = rttOf(*it->pc);
      l.connectionType = connectionTypeOf(*it->pc, relayTcpOnly_, forceRelay_);
      l.relayReason = classifyRelayReason(l.connectionType, forceRelay_, it->ice.local, it->ice.remote);
    }
    if (l.rttMs && (!s.rttMs || *l.rttMs > *s.rttMs)) {
      s.rttMs = l.rttMs;
    }
    const int r = connectionTypeRank(l.connectionType);
    if (r > rank) {
      rank = r;
      s.connectionType = l.connectionType;
    }
    links.append(l);
  }
  reports_.applyTo(links);
  std::lock_guard<std::mutex> lock(statsMutex_);
  stats_ = s;
  links_ = links;
}

SessionStats SessionHost::stats() const {
  std::lock_guard<std::mutex> lock(statsMutex_);
  SessionStats s = stats_;
  s.viewers = viewerCountAtomic_;
  s.active = encoderRunning_;
  if (encoderRunning_ && workerStats_) {
    s.videoFrames = workerStats_->frames;
    s.audioFrames = workerStats_->audioFrames;
    s.droppedFrames = workerStats_->dropped;
    s.codec = QStringLiteral("H264 + Opus");
    s.targetBitrateKbps = targetKbps_;
    std::lock_guard<std::mutex> wl(workerStats_->mutex);
    s.encoderName = workerStats_->encoderName;
    s.width = workerStats_->width;
    s.height = workerStats_->height;
  }
  s.gpuInput = gpuInputActive();
  s.gpuInputFailed = gpuInputFailed_;
  return s;
}

QList<ViewerLinkStats> SessionHost::viewerLinks() const {
  std::lock_guard<std::mutex> lock(statsMutex_);
  return links_;
}

}  // namespace uzume
