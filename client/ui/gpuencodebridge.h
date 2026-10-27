#pragma once
// GPU-direct encoding of the own Session (ADR 0019, ADR 0027 D6, ADR 0034 D9): the adapter between the emulation thread and the UI
// thread. uzume_emulation only knows the interface emu::GpuEncodeTarget; uzume_media implements the captures
// (CudaGlCapture for NVIDIA, D3d11EncodeCapture for AMD AMF and Intel QSV); uzume_ui, which links both, joins them here.

#include <QSize>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "cudaglcapture.h"
#include "gpu_encode_target.h"

struct AVFrame;

namespace uzume {
class GlD3d11Bridge;
}

namespace uzume::ui {

// Largest frame the Session encoder is fed from the local game (the encoder opens at the frame size it gets). While
// shared, the GPU readback keeps at least this much of a larger hardware frame (the view's own size may need more), and
// the GPU-direct encode texture is the frame fitted into it.
inline constexpr QSize kShareEncodeMax(1280, 1920);

// Vulkan games (ADR 0034 D9): the D3D11 backend serves every vendor, chosen by the Vulkan device's PCI vendor id.
//   AMD (0x1002)    -> Amf   when allowed.amf
//   Intel (0x8086)  -> Qsv   when allowed.qsv
//   NVIDIA (0x10DE) -> Nvenc when allowed.cuda (h264_nvenc probed and usable; the frames are D3D11, not CUDA)
// None for any other vendor or a backend that is not allowed. Pure; unit-tested as a truth table.
enum class VulkanBackend { None, Amf, Qsv, Nvenc };
struct GpuEncodeAllowed;
VulkanBackend chooseVulkanBackend(unsigned vendorId, const GpuEncodeAllowed& allowed);

struct GpuEncodePlan {
  bool keep = false;    // a bridge may exist (else drop it)
  bool create = false;  // a bridge must exist now
  bool wanted = false;  // the bridge produces frames
  QSize shareSize;      // GameSession::setShareSize
};
// Pure; unit-tested as a truth table.
//   keep      = shared && hwGame && gpuAllowed
//   create    = keep && encoderRunning
//   wanted    = create
//   shareSize = shared ? (gpuActive ? QSize() : kShareEncodeMax) : QSize()
GpuEncodePlan planGpuEncode(bool shared, bool hwGame, bool encoderRunning, bool gpuAllowed, bool gpuActive);

// The capture seam: what the bridge needs from CudaGlCapture and D3d11EncodeCapture. All calls on the emulation thread
// with the game's GL current, like emu::GpuEncodeTarget.
class GpuCapture {
 public:
  enum class Status { Ok, NotReady, Unavailable, Failed };
  enum class Backend { Cuda, Amf, Qsv, Nvenc };
  virtual ~GpuCapture() = default;
  virtual Status attach(unsigned glTexture2D, int width, int height) = 0;
  // Vulkan mode (D3d11EncodeCapture only): no GL; the shared texture's NT handle is owned by the capture.
  struct SharedImage {
    void* ntHandle = nullptr;
    int width = 0;
    int height = 0;
  };
  virtual Status attachShared(int width, int height, const unsigned char* luid, unsigned vendorId, SharedImage* out) {
    Q_UNUSED(width);
    Q_UNUSED(height);
    Q_UNUSED(luid);
    Q_UNUSED(vendorId);
    Q_UNUSED(out);
    return Status::Unavailable;
  }
  virtual std::shared_ptr<AVFrame> repeat() { return nullptr; }  // Vulkan mode: the last frame once more
  virtual void detach(bool glCurrent) = 0;
  virtual std::shared_ptr<AVFrame> capture() = 0;  // null: failed
  virtual QString reason() const = 0;
  virtual Backend backend() const = 0;  // valid once attach() returned Ok
};

// Which backends SessionHost::gpuBackendsAllowed() permits (computed on the UI thread, fixed for the bridge's life).
struct GpuEncodeAllowed {
  bool cuda = false;
  bool amf = false;
  bool qsv = false;
  bool any() const { return cuda || amf || qsv; }
};

struct GpuEncodeOptions {
  GpuEncodeAllowed allowed;
  // Emulation thread: the GL-D3D11 bridge of the game's display, or null/empty (then the D3D11 capture opens its own).
  std::function<std::shared_ptr<GlD3d11Bridge>()> displayBridge;
  // Seams for tests; empty = the real captures. makeD3d11 gets displayBridge.
  std::function<std::unique_ptr<GpuCapture>()> makeCuda;
  std::function<std::unique_ptr<GpuCapture>(std::function<std::shared_ptr<GlD3d11Bridge>()>)> makeD3d11;
  std::function<QString()> glVendor;  // empty: GL_VENDOR of the current context
};

// emu::GpuEncodeTarget on top of a capture (chosen lazily at the first attach) plus a one-slot mailbox for the UI thread.
//  Backend choice, emulation thread, first attach: the D3D11 capture first when AMF or QSV is allowed and the game's GL
//  context is not on an NVIDIA GPU (with CUDA allowed; NVIDIA GL contexts never touch D3D11, so ADR 0019 behaves exactly as
//  before there): taken when it reports Ok for a backend that is allowed. Otherwise CUDA when allowed. Otherwise
//  Unavailable (Failed when the D3D11 capture failed for real).
class GpuEncodeBridge final : public emu::GpuEncodeTarget {
 public:
  enum class State { Running, Unavailable, Failed };
  // Constructors instead of `Deps deps = {}`: a default argument may not use Deps's member initializers before the end of
  // the enclosing class (GCC/Clang reject it; CudaGlCapture has the same pair).
  explicit GpuEncodeBridge(const QSize& maxSize);  // CUDA only: the real driver and the FFmpeg pool
  GpuEncodeBridge(const QSize& maxSize, CudaGlCapture::Deps deps);  // CUDA only, test deps
  GpuEncodeBridge(const QSize& maxSize, GpuEncodeOptions options);
  ~GpuEncodeBridge() override;  // any thread

  // UI thread
  void setWanted(bool on);                  // false also empties the mailbox (frame released outside the lock)
  std::shared_ptr<AVFrame> takeFrame();     // newest captured frame or null
  State state() const;                      // atomic
  QString reason() const;                   // mutex; set together with Unavailable/Failed

  // emu::GpuEncodeTarget
  bool wanted() const override;             // wanted_ && state_ == Running
  QSize maxSize() const override;
  Attach attach(unsigned texture, int width, int height) override;  // Ok/NotReady; Unavailable/Failed also set the state
  // Vulkan games (ADR 0034 D9): the D3D11 backend for the device's vendor (chooseVulkanBackend), its shared texture handed out.
  Attach attachExternal(int width, int height, const VulkanDevice& device, emu::ExternalImage* out) override;
  emu::AdapterLuid adapterLuid() const override;  // the adapter of the shared texture, once attachExternal() returned Ok
  void markUnavailable(const QString& reason) override;  // Running -> Unavailable (quiet), first reason wins
  bool repeat() override;                   // Vulkan mode: the last captured frame once more
  void detach(bool glCurrent) override;     // chosen capture's detach(glCurrent)
  bool capture() override;                  // stores the frame in the mailbox, only while wanted
  void fail(const QString& reason) override;  // Running -> Failed, first reason wins; never overrides Unavailable

 private:
  // Running -> `to`, with the reason; a no-op in any other state (the first reason wins).
  void transition(State to, const QString& reason);

  emu::GpuEncodeTarget::Attach finish(GpuCapture::Status status, const QString& reason);
  void publish(std::shared_ptr<AVFrame> frame);
  emu::GpuEncodeTarget::Attach selectAndAttach(unsigned texture, int width, int height);

  const GpuEncodeOptions options_;
  std::unique_ptr<GpuCapture> capture_;  // emulation thread: chosen at the first attach, null before
  std::unique_ptr<GpuCapture> pendingD3d11_;  // a D3D11 capture whose bridge was NotReady; the choice is made again
  const QSize maxSize_;
  std::atomic<bool> wanted_{false};
  std::atomic<State> state_{State::Running};
  mutable std::mutex mutex_;                // guards latest_ and reason_
  std::shared_ptr<AVFrame> latest_;
  QString reason_;
  emu::AdapterLuid luid_;                   // guarded by mutex_
  void* sharedHandle_ = nullptr;            // emulation thread: the handle and size last handed out (generation_ follows their changes)
  QSize sharedSize_;
  quint64 generation_ = 0;
};

}  // namespace uzume::ui
