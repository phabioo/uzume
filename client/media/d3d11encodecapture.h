#pragma once
// D3D11 encode capture for GPU-direct Session encoding with AMD AMF and Intel QSV (ADR 0027, design D6): the GL encode
// texture goes through the GL-D3D11 bridge into FFmpeg D3D11 frames. Same shape and thread rules as CudaGlCapture
// (ADR 0019). NVIDIA stays on CUDA: a bridge on any other vendor than AMD (0x1002) or Intel (0x8086) reports
// Unavailable("not an AMD or Intel GPU") and the caller uses CUDA or readback.
//
// Vulkan mode (ADR 0034 D9): for Vulkan games there is no GL context. attachShared() opens an own D3D11 device on the Vulkan
// device's adapter (LUID), creates a shared B8G8R8A8 texture (NT handle, keyed mutex) of the encode size and hands the handle
// out; the Vulkan side blits each frame into it (acquire key 0, release key 1) and capture() takes key 1, runs the same AMF
// copy / QSV video-processor path and gives key 0 back. NVIDIA is served here too (Kind::Nvenc: h264_nvenc fed with BGRA
// D3D11 frames of a dynamic pool); for OpenGL games NVIDIA stays on CUDA (ADR 0019).
//
// Windows only: elsewhere (and with an FFmpeg without D3D11VA headers) attach() reports Unavailable.
//  - AMF: BGRA D3D11 frames from the encoder's own frames context (h264_amf asserts on a foreign context).
//  - QSV: NV12 single RENDER_TARGET textures (ID3D11VideoProcessor BGRA -> NV12, BT.601 limited), mapped with
//    av_hwframe_map to AV_PIX_FMT_QSV frames of a derived QSV frames context.
// attach/detach/capture run on ONE thread, the one with the game's GL context current (the emulation thread).
// status()/reason()/kind() are thread-safe.
#include <QString>
#include <functional>
#include <memory>

struct AVFrame;

namespace uzume {

class GlD3d11Bridge;

class D3d11EncodeCapture {
 public:
  enum class Status { Ok, NotReady, Unavailable, Failed };
  enum class Kind { None, Amf, Qsv, Nvenc };  // known once attach() returned Ok; Nvenc only in Vulkan mode

  // Emulation thread, evaluated at the first attach of every attach cycle (after a detach again, because a live-save
  // restart makes a new display bridge). Returns the bridge of the game's display, or null: then the capture uses its own
  // bridge on the adapter the GL context can open (UZUME_DISABLE_ZERO_COPY=1, no D3D11 Qt Quick).
  using BridgeProvider = std::function<std::shared_ptr<GlD3d11Bridge>()>;
  D3d11EncodeCapture();
  explicit D3d11EncodeCapture(BridgeProvider provider);
  ~D3d11EncodeCapture();
  D3d11EncodeCapture(const D3d11EncodeCapture&) = delete;
  D3d11EncodeCapture& operator=(const D3d11EncodeCapture&) = delete;

  // Registers `glTexture2D` (GL_TEXTURE_2D, sized GL_RGBA8, width x height, image top row = GL row 0). NotReady while
  // the shared bridge is not open yet; ask again on a later frame.
  Status attach(unsigned glTexture2D, int width, int height);

  // Vulkan mode (no GL involved), emulation thread. The shared texture (width x height: even, >= 16) and its NT handle
  // (OWNED by the capture, valid until detach() or the next attachShared() of another size; the Vulkan side imports it
  // with VK_KHR_external_memory_win32). `luid`: the Vulkan device's LUID (8 bytes; null = unknown, then Unavailable),
  // `vendorId`: its PCI vendor id (AMD -> AMF, Intel -> QSV, NVIDIA -> NVENC with D3D11 frames).
  struct SharedImage {
    void* ntHandle = nullptr;
    int width = 0;
    int height = 0;
  };
  Status attachShared(int width, int height, const unsigned char* luid, unsigned vendorId, SharedImage* out);
  // LUID of the adapter the Vulkan-mode device lives on (8 bytes in memory order low, high), false before attachShared().
  bool adapterLuid(unsigned char out[8]) const;
  // Vulkan mode: publishes the last captured frame once more (a clone of the same D3D11 surface); null before the first one.
  std::shared_ptr<AVFrame> repeat();
  void detach(bool glCurrent);  // unregister and delete GL objects (GL current) or leak them; Vulkan mode: release the shared texture
  // Copies the registered texture into a new D3D11 (AMF) or QSV frame. Complete frame, or null (then status() is Failed).
  // No GPU wait: the copy and the encoder's work run on the same device, in submission order (see the .cpp).
  std::shared_ptr<AVFrame> capture();
  Status status() const;
  QString reason() const;      // why Unavailable/Failed (already logged)
  QString deviceName() const;  // adapter name after the first Ok
  Kind kind() const;

  // The frame size QSV allocates for a visible size (16-aligned); AMF uses the visible size. Pure, unit-tested.
  static int alignedSize(int visible, Kind kind);
  // Pure: Kind for a PCI vendor id; None for anything but AMD/Intel, with `vulkanMode` NVIDIA (0x10DE) is Nvenc.
  static Kind kindForVendor(unsigned vendorId, bool vulkanMode = false);

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace uzume
