#include "d3d11encodecapture.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <atomic>
#include <cstring>
#include <mutex>

#include "d3d11glbridge.h"
#include "videoencoder.h"

// The real implementation needs Windows and FFmpeg's D3D11VA header; everything else compiles the stub at the end.
#if defined(_WIN32) && defined(__has_include)
#if __has_include(<libavutil/hwcontext_d3d11va.h>)
#define FB_D3D11_CAPTURE 1
#endif
#endif

#ifdef FB_D3D11_CAPTURE
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h
#include <d3d10_1.h>  // ID3D10Multithread; d3d10_1.h must come before any d3d10.h (SAL check in the Windows SDK)
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <memory>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace uzume {

namespace {
Q_LOGGING_CATEGORY(lcD3dCap, "uzume.gpuencode")

constexpr unsigned kVendorAmd = 0x1002;
constexpr unsigned kVendorIntel = 0x8086;
constexpr unsigned kVendorNvidia = 0x10DE;
}  // namespace

D3d11EncodeCapture::Kind D3d11EncodeCapture::kindForVendor(unsigned vendorId, bool vulkanMode) {
  switch (vendorId) {
    case kVendorAmd:
      return Kind::Amf;
    case kVendorIntel:
      return Kind::Qsv;
    case kVendorNvidia:
      return vulkanMode ? Kind::Nvenc : Kind::None;
    default:
      return Kind::None;
  }
}

int D3d11EncodeCapture::alignedSize(int visible, Kind kind) {
  return kind == Kind::Qsv ? (visible + 15) & ~15 : visible;  // QSV surfaces are 16-aligned with a crop
}

#ifdef FB_D3D11_CAPTURE

namespace {

using Microsoft::WRL::ComPtr;

// GL constants (opengl32 headers are not used).
constexpr unsigned kGlTexture2D = 0x0DE1;
constexpr unsigned kGlReadFramebuffer = 0x8CA8;
constexpr unsigned kGlDrawFramebuffer = 0x8CA9;
constexpr unsigned kGlReadFramebufferBinding = 0x8CAA;
constexpr unsigned kGlDrawFramebufferBinding = 0x8CA6;
constexpr unsigned kGlColorAttachment0 = 0x8CE0;
constexpr unsigned kGlFramebufferComplete = 0x8CD5;
constexpr unsigned kGlColorBufferBit = 0x4000;
constexpr unsigned kGlNearest = 0x2600;
constexpr unsigned kGlScissorTest = 0x0C11;

// Frames the AMF pool holds: mailbox 1 + queue 2 + encoding 1 + AMF's own clone until the packet is dequeued + capturing 1,
// with headroom. The pool is one array texture (D3D11VA static pool); exhaustion is a capture failure, not a wait.
constexpr int kAmfPoolFrames = 8;
constexpr size_t kMaxCachedViews = 32;  // QSV output views, one per pool texture

QString ffmpegError(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(rc, buf, sizeof buf);  // never av_err2str: a C compound literal, does not compile as C++ on MSVC
  return QString::fromUtf8(buf);
}

QString hresultText(HRESULT hr) { return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')); }

// The recursive lock FFmpeg's D3D11VA context asks for (hwcontext_d3d11va.h: it MUST be recursive). Owned by the AVHWDeviceContext
// (freed by its free callback after device_uninit); we use it only while we hold the device reference.
struct LockBox {
  std::recursive_mutex mutex;
};
void lockFn(void* ctx) { static_cast<LockBox*>(ctx)->mutex.lock(); }
void unlockFn(void* ctx) { static_cast<LockBox*>(ctx)->mutex.unlock(); }
void freeLockBox(AVHWDeviceContext* ctx) {
  delete static_cast<LockBox*>(ctx->user_opaque);
  ctx->user_opaque = nullptr;
}

}  // namespace

struct D3d11EncodeCapture::Impl {
  explicit Impl(BridgeProvider p) : provider(std::move(p)) {}

  BridgeProvider provider;
  std::atomic<Status> status{Status::NotReady};
  std::atomic<Kind> kind{Kind::None};
  mutable std::mutex mutex;  // guards reason, deviceName
  QString reason;
  QString deviceName;

  // --- one attach cycle (attach .. detach), emulation thread only
  std::shared_ptr<GlD3d11Bridge> bridge;
  bool ownBridge = false;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  AVBufferRef* deviceRef = nullptr;  // D3D11VA device context wrapping `device` (AddRef'd)
  LockBox* lockBox = nullptr;        // owned by deviceRef
  AVBufferRef* qsvDeviceRef = nullptr;  // derived once per cycle (QSV)
  ComPtr<ID3D11VideoDevice> videoDevice;
  ComPtr<ID3D11VideoContext> videoContext;

  // --- per size
  unsigned srcTexture = 0;
  int width = 0, height = 0;
  ComPtr<ID3D11Texture2D> interop;
  unsigned glName = 0;       // GL name of `interop`
  void* reg = nullptr;       // interop registration
  unsigned fbo[2] = {0, 0};  // [0] reads the encode texture, [1] draws into the interop texture
  AVBufferRef* framesRef = nullptr;     // D3D11 frames context (AMF: the encoder's; QSV: the NV12 source)
  AVBufferRef* qsvFramesRef = nullptr;  // derived QSV frames context
  ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
  ComPtr<ID3D11VideoProcessor> vp;
  ComPtr<ID3D11VideoProcessorInputView> vpInput;
  std::vector<std::pair<ComPtr<ID3D11Texture2D>, ComPtr<ID3D11VideoProcessorOutputView>>> vpOutputs;

  // --- Vulkan mode (ADR 0034 D9): `interop` is the shared texture the Vulkan side blits into; no bridge, no GL
  bool vulkanMode = false;
  unsigned char luidBytes[8] = {};
  bool luidKnown = false;
  ComPtr<IDXGIKeyedMutex> keyedMutex;
  HANDLE sharedHandle = nullptr;        // NT handle of `interop`, closed in releaseSize
  std::shared_ptr<AVFrame> lastFrame;   // for repeat()

  Status unavailable(const QString& why) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      reason = why;
    }
    qCInfo(lcD3dCap).noquote() << QStringLiteral("GPU-direct capture unavailable: %1").arg(why);
    status = Status::Unavailable;
    return Status::Unavailable;
  }

  // `step`: bridge, device, frames context, register, framebuffer, interop lock, blit, interop unlock, frame buffer, copy,
  // video processor, map.
  Status fail(const char* step, const QString& detail) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      reason = QStringLiteral("%1: %2").arg(QLatin1String(step), detail);
    }
    qCWarning(lcD3dCap).noquote() << QStringLiteral("D3D11 capture failed at %1: %2").arg(QLatin1String(step), detail);
    status = Status::Failed;
    return Status::Failed;
  }

  const GlD3d11Bridge::GlFunctions* gl() const { return bridge ? bridge->gl() : nullptr; }

  Status attach(unsigned tex, int w, int h);
  Status attachShared(int w, int h, const unsigned char* luid, unsigned vendorId, SharedImage* out);
  void detach(bool glCurrent);
  std::shared_ptr<AVFrame> capture();
  std::shared_ptr<AVFrame> captureShared();

  Status resolveBridge();
  Status createVulkanDevice(const unsigned char* luid, unsigned vendorId);
  Status createDevice();
  Status createSize(unsigned tex, int w, int h);
  Status createSharedTexture(int w, int h);
  Status createFrames(int w, int h);
  void releaseSize(bool glCurrent);
  void releaseCycle(bool glCurrent);
  bool setupFramebuffers();
  bool blit();
  std::shared_ptr<AVFrame> frameAmf();
  std::shared_ptr<AVFrame> frameQsv();
  ID3D11VideoProcessorOutputView* outputView(ID3D11Texture2D* texture, HRESULT* hr);
};

D3d11EncodeCapture::Status D3d11EncodeCapture::Impl::resolveBridge() {
  std::shared_ptr<GlD3d11Bridge> shared = provider ? provider() : nullptr;
  if (shared) {
    switch (shared->open()) {  // idempotent; the display normally opened it already
      case GlD3d11Bridge::Status::Ok:
        shared->addUser();  // released in releaseCycle(); the last user closes the bridge
        bridge = std::move(shared);
        ownBridge = false;
        return Status::Ok;
      case GlD3d11Bridge::Status::NotReady:
        return Status::NotReady;  // Qt Quick's adapter is not known yet
      case GlD3d11Bridge::Status::Unavailable:
      case GlD3d11Bridge::Status::Failed:
        // The display cannot use the bridge (e.g. Qt Quick is not on D3D11); an own device on the adapter the GL context
        // opens may still work for encoding.
        qCInfo(lcD3dCap).noquote() << QStringLiteral("D3D11 capture: the display bridge is not usable (%1); opening an own one").arg(shared->reason());
        break;
    }
  }
  auto own = std::make_shared<GlD3d11Bridge>();
  own->setAnyAdapter(true);
  switch (own->open()) {
    case GlD3d11Bridge::Status::Ok:
      bridge = std::move(own);
      ownBridge = true;
      return Status::Ok;
    case GlD3d11Bridge::Status::NotReady:
      return Status::NotReady;
    case GlD3d11Bridge::Status::Unavailable:
      return unavailable(own->reason());
    case GlD3d11Bridge::Status::Failed:
      return fail("bridge", own->reason());
  }
  return Status::Failed;
}

D3d11EncodeCapture::Status D3d11EncodeCapture::Impl::createDevice() {
  if (!vulkanMode) {
    device = static_cast<ID3D11Device*>(bridge->d3dDevice());  // AddRef'd by ComPtr
  }
  if (!device) {
    return unavailable(QStringLiteral("the D3D11 device is gone"));
  }
  device->GetImmediateContext(&immediate);
  if (FAILED(device.As(&videoDevice)) || !immediate || FAILED(immediate.As(&videoContext))) {
    return unavailable(QStringLiteral("the D3D11 device has no video support"));
  }
  if (kind.load() == Kind::Amf) {  // Nvenc: the encoder reports a missing driver at open
    // Probe the runtime first: a missing amfrt64.dll makes FFmpeg log an error at avcodec_open2.
    if (HMODULE m = LoadLibraryW(L"amfrt64.dll")) {
      FreeLibrary(m);
    } else {
      return unavailable(QStringLiteral("the AMD AMF runtime (amfrt64.dll) is not installed"));
    }
  }
  deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (!deviceRef) {
    return unavailable(QStringLiteral("FFmpeg has no D3D11VA support"));
  }
  auto* dc = reinterpret_cast<AVHWDeviceContext*>(deviceRef->data);
  auto* hw = static_cast<AVD3D11VADeviceContext*>(dc->hwctx);
  device->AddRef();  // the context releases it when it is freed ("will always release this interface")
  hw->device = device.Get();
  lockBox = new LockBox;
  dc->user_opaque = lockBox;
  dc->free = &freeLockBox;  // runs after device_uninit
  hw->lock = &lockFn;
  hw->unlock = &unlockFn;
  hw->lock_ctx = lockBox;
  const int rc = av_hwdevice_ctx_init(deviceRef);
  if (rc < 0) {
    lockBox = nullptr;  // freed with the context
    av_buffer_unref(&deviceRef);
    return fail("device", QStringLiteral("av_hwdevice_ctx_init: %1").arg(ffmpegError(rc)));
  }
  return Status::Ok;
}

D3d11EncodeCapture::Status D3d11EncodeCapture::attach(unsigned tex, int w, int h) { return d_->attach(tex, w, h); }

D3d11EncodeCapture::Status D3d11EncodeCapture::Impl::attach(unsigned tex, int w, int h) {
  const Status current = status.load();
  if (current == Status::Unavailable || current == Status::Failed) {
    return current;
  }
  if (!wglGetCurrentContext()) {
    return unavailable(QStringLiteral("no current OpenGL context"));
  }
  if (w < 16 || h < 16 || (w & 1) != 0 || (h & 1) != 0) {
    return unavailable(QStringLiteral("encode size %1 x %2 is not an even size of at least 16").arg(w).arg(h));
  }
  QElapsedTimer timer;
  timer.start();

  if (reg && tex == srcTexture && w == width && h == height) {
    return Status::Ok;
  }
  if (!bridge) {
    const Status r = resolveBridge();
    if (r != Status::Ok) {
      status = r;
      return r;
    }
    const Kind k = D3d11EncodeCapture::kindForVendor(bridge->vendorId());
    if (k == Kind::None) {
      releaseCycle(true);
      return unavailable(QStringLiteral("not an AMD or Intel GPU"));
    }
    kind = k;
    const GlD3d11Bridge::GlFunctions* f = gl();
    if (!f || !f->genFramebuffers || !f->deleteFramebuffers || !f->bindFramebuffer || !f->framebufferTexture2D ||
        !f->checkFramebufferStatus || !f->blitFramebuffer || !f->getIntegerv || !f->enable || !f->disable || !f->isEnabled) {
      releaseCycle(true);
      return unavailable(QStringLiteral("the OpenGL driver has no framebuffer blits"));
    }
    {
      const std::lock_guard<std::mutex> lock(mutex);
      deviceName = bridge->adapterName();
    }
    const Status d = createDevice();
    if (d != Status::Ok) {
      releaseCycle(true);
      return d;
    }
  }
  if (reg) {  // a different texture or size: the frames context changes, so the encoder reopens
    releaseSize(true);
  }
  const Status s = createSize(tex, w, h);
  if (s != Status::Ok) {
    releaseSize(true);
    return s;
  }
  status = Status::Ok;
  qCInfo(lcD3dCap).noquote() << QStringLiteral("D3D11 capture on \"%1\": %2, %3 frames ready in %4 ms")
                                    .arg(bridge->adapterName(), kind.load() == Kind::Amf ? QStringLiteral("h264_amf") : QStringLiteral("h264_qsv"),
                                         kind.load() == Kind::Amf ? QStringLiteral("BGRA") : QStringLiteral("NV12"))
                                    .arg(timer.elapsed());
  return Status::Ok;
}

D3d11EncodeCapture::Status D3d11EncodeCapture::Impl::createSize(unsigned tex, int w, int h) {
  const GlD3d11Bridge::GlFunctions* f = gl();
  if (!f) {
    return fail("bridge", QStringLiteral("the bridge closed"));
  }
  // 1. The interop texture. FFmpeg's D3D11 frames have no RGBA sw_format, so B8G8R8A8 it is; GL sees it as RGBA8.
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;  // no sharing flags
  HRESULT hr = device->CreateTexture2D(&td, nullptr, &interop);
  if (FAILED(hr)) {
    return fail("interop texture", hresultText(hr));
  }
  f->genTextures(1, &glName);
  reg = bridge->registerTexture(interop.Get(), glName, /*writeDiscard=*/true);
  if (!reg) {
    return fail("register", QStringLiteral("wglDXRegisterObjectNV failed, error %1").arg(bridge->lastWin32Error()));
  }
  srcTexture = tex;
  width = w;
  height = h;
  if (!setupFramebuffers()) {
    return Status::Failed;  // fail() already called
  }
  return createFrames(w, h);
}

// The encoder-facing half of createSize: FFmpeg frames contexts (and the QSV video processor) for `interop`.
D3d11EncodeCapture::Status D3d11EncodeCapture::Impl::createFrames(int w, int h) {
  const bool qsv = kind.load() == Kind::Qsv;
  HRESULT hr = S_OK;

  // 2. FFmpeg frames contexts.
  framesRef = av_hwframe_ctx_alloc(deviceRef);
  if (!framesRef) {
    return fail("frames context", QStringLiteral("av_hwframe_ctx_alloc failed"));
  }
  auto* fc = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
  auto* fh = static_cast<AVD3D11VAFramesContext*>(fc->hwctx);
  fc->format = AV_PIX_FMT_D3D11;
  if (qsv) {
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = D3d11EncodeCapture::alignedSize(w, Kind::Qsv);
    fc->height = D3d11EncodeCapture::alignedSize(h, Kind::Qsv);
    fc->initial_pool_size = 0;                  // dynamic pool: single (non-array) textures, which QSV needs for RENDER_TARGET
    fh->BindFlags = D3D11_BIND_RENDER_TARGET;   // video processor output
  } else {
    fc->sw_format = AV_PIX_FMT_BGRA;
    fc->width = w;
    fc->height = h;
    // NVENC (Vulkan mode) registers single textures: a dynamic pool like QSV's, not the AMF array texture.
    fc->initial_pool_size = kind.load() == Kind::Nvenc ? 0 : kAmfPoolFrames;
    fh->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (kind.load() == Kind::Nvenc) {
      fc->user_opaque = const_cast<void*>(VideoEncoder::nvencD3d11Marker());  // BGRA D3D11 frames -> h264_nvenc, not h264_amf
    }
  }
  int rc = av_hwframe_ctx_init(framesRef);
  if (rc < 0) {
    return fail("frames context", QStringLiteral("av_hwframe_ctx_init: %1").arg(ffmpegError(rc)));
  }
  if (!qsv) {
    return Status::Ok;
  }

  // 3. QSV: derived device (once per cycle) and derived frames, no init.
  if (!qsvDeviceRef) {
    rc = av_hwdevice_ctx_create_derived(&qsvDeviceRef, AV_HWDEVICE_TYPE_QSV, deviceRef, 0);
    if (rc < 0) {
      qsvDeviceRef = nullptr;
      return unavailable(QStringLiteral("no QSV device on the D3D11 adapter: %1").arg(ffmpegError(rc)));
    }
  }
  rc = av_hwframe_ctx_create_derived(&qsvFramesRef, AV_PIX_FMT_QSV, qsvDeviceRef, framesRef, 0);
  if (rc < 0) {
    qsvFramesRef = nullptr;
    return fail("frames context", QStringLiteral("QSV frames: %1").arg(ffmpegError(rc)));
  }

  // 4. QSV: BGRA -> NV12 video processor, BT.601 limited output, input RGB full range.
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
  cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  cd.InputWidth = cd.OutputWidth = static_cast<UINT>(w);
  cd.InputHeight = cd.OutputHeight = static_cast<UINT>(h);
  cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  hr = videoDevice->CreateVideoProcessorEnumerator(&cd, &vpEnum);
  if (FAILED(hr)) {
    return fail("video processor", QStringLiteral("CreateVideoProcessorEnumerator %1").arg(hresultText(hr)));
  }
  UINT inFlags = 0, outFlags = 0;
  if (FAILED(vpEnum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &inFlags)) ||
      (inFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
      FAILED(vpEnum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outFlags)) ||
      (outFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
    return unavailable(QStringLiteral("the video processor cannot convert BGRA to NV12"));
  }
  hr = videoDevice->CreateVideoProcessor(vpEnum.Get(), 0, &vp);
  if (FAILED(hr)) {
    return fail("video processor", QStringLiteral("CreateVideoProcessor %1").arg(hresultText(hr)));
  }
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {};
  iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  hr = videoDevice->CreateVideoProcessorInputView(interop.Get(), vpEnum.Get(), &iv, &vpInput);
  if (FAILED(hr)) {
    return fail("video processor", QStringLiteral("CreateVideoProcessorInputView %1").arg(hresultText(hr)));
  }
  const RECT rect = {0, 0, w, h};
  videoContext->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  videoContext->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
  videoContext->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &rect);
  videoContext->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &rect);
  videoContext->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &rect);
  ComPtr<ID3D11VideoContext1> vc1;
  if (SUCCEEDED(videoContext.As(&vc1))) {
    vc1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    vc1->VideoProcessorSetOutputColorSpace1(vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);  // BT.601, 16-235
  } else {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};
    in.RGB_Range = 0;  // full
    videoContext->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {};
    out.YCbCr_Matrix = 0;  // BT.601
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext->VideoProcessorSetOutputColorSpace(vp.Get(), &out);
  }
  return Status::Ok;
}

// Read FBO: the encode texture; draw FBO: the interop texture, attached per capture (inside the interop lock). Bindings
// the game had are restored.
bool D3d11EncodeCapture::Impl::setupFramebuffers() {
  const GlD3d11Bridge::GlFunctions* f = gl();
  int prevRead = 0, prevDraw = 0;
  f->getIntegerv(kGlReadFramebufferBinding, &prevRead);
  f->getIntegerv(kGlDrawFramebufferBinding, &prevDraw);
  f->genFramebuffers(2, fbo);
  f->bindFramebuffer(kGlReadFramebuffer, fbo[0]);
  f->framebufferTexture2D(kGlReadFramebuffer, kGlColorAttachment0, kGlTexture2D, srcTexture, 0);
  const unsigned readStatus = f->checkFramebufferStatus(kGlReadFramebuffer);
  unsigned drawStatus = kGlFramebufferComplete;
  bool locked = false;
  if (readStatus == kGlFramebufferComplete) {
    locked = bridge->lock(&reg, 1);
    if (locked) {
      f->bindFramebuffer(kGlDrawFramebuffer, fbo[1]);
      f->framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, kGlTexture2D, glName, 0);
      drawStatus = f->checkFramebufferStatus(kGlDrawFramebuffer);
      bridge->unlock(&reg, 1);
    }
  }
  f->bindFramebuffer(kGlReadFramebuffer, static_cast<unsigned>(prevRead));
  f->bindFramebuffer(kGlDrawFramebuffer, static_cast<unsigned>(prevDraw));
  if (readStatus != kGlFramebufferComplete) {
    fail("framebuffer", QStringLiteral("encode texture framebuffer incomplete (0x%1)").arg(readStatus, 0, 16));
    return false;
  }
  if (!locked) {
    fail("interop lock", QStringLiteral("wglDXLockObjectsNV failed, error %1").arg(bridge->lastWin32Error()));
    return false;
  }
  if (drawStatus != kGlFramebufferComplete) {
    fail("framebuffer", QStringLiteral("interop texture framebuffer incomplete (0x%1)").arg(drawStatus, 0, 16));
    return false;
  }
  return true;
}

// Lock, blit 1:1, glFlush, unlock. Both textures hold the image top row at GL row 0: no flip. Scissor and the game's
// framebuffer bindings are restored.
bool D3d11EncodeCapture::Impl::blit() {
  const GlD3d11Bridge::GlFunctions* f = gl();
  if (!f) {
    fail("bridge", QStringLiteral("the bridge closed"));
    return false;
  }
  if (!bridge->lock(&reg, 1)) {
    fail("interop lock", QStringLiteral("wglDXLockObjectsNV failed, error %1").arg(bridge->lastWin32Error()));
    return false;
  }
  int prevRead = 0, prevDraw = 0;
  f->getIntegerv(kGlReadFramebufferBinding, &prevRead);
  f->getIntegerv(kGlDrawFramebufferBinding, &prevDraw);
  const bool scissor = f->isEnabled(kGlScissorTest) != 0;
  if (scissor) {
    f->disable(kGlScissorTest);  // would clip the blit
  }
  while (f->getError() != 0) {
  }
  f->bindFramebuffer(kGlReadFramebuffer, fbo[0]);
  f->bindFramebuffer(kGlDrawFramebuffer, fbo[1]);
  f->framebufferTexture2D(kGlDrawFramebuffer, kGlColorAttachment0, kGlTexture2D, glName, 0);  // re-attached inside every lock
  f->blitFramebuffer(0, 0, width, height, 0, 0, width, height, kGlColorBufferBit, kGlNearest);
  const unsigned err = f->getError();
  f->bindFramebuffer(kGlReadFramebuffer, static_cast<unsigned>(prevRead));
  f->bindFramebuffer(kGlDrawFramebuffer, static_cast<unsigned>(prevDraw));
  if (scissor) {
    f->enable(kGlScissorTest);
  }
  f->flush();
  const bool unlocked = bridge->unlock(&reg, 1);  // after this the D3D11 side owns the texture
  if (err != 0) {
    fail("blit", QStringLiteral("GL error 0x%1").arg(err, 0, 16));
    return false;
  }
  if (!unlocked) {
    fail("interop unlock", QStringLiteral("wglDXUnlockObjectsNV failed, error %1").arg(bridge->lastWin32Error()));
    return false;
  }
  return true;
}

std::shared_ptr<AVFrame> D3d11EncodeCapture::capture() { return d_->capture(); }

// Why no GPU wait: the interop unlock makes the GL write visible to D3D11 on the bridge's device; the copy (AMF) or the
// video processor blit (QSV) is queued on that same device's immediate context, and the encoder (AMF/Intel runtime) submits
// its work on that device afterwards, so the command stream orders them. The frame is complete for the encoder without a
// CPU wait; Flush only makes sure the work reaches the GPU promptly.
std::shared_ptr<AVFrame> D3d11EncodeCapture::Impl::capture() {
  if (status.load() != Status::Ok) {
    return nullptr;
  }
  if (vulkanMode) {
    return captureShared();
  }
  if (!reg || !framesRef) {
    fail("register", QStringLiteral("capture without a registered texture"));
    return nullptr;
  }
  if (!blit()) {
    return nullptr;
  }
  return kind.load() == Kind::Qsv ? frameQsv() : frameAmf();
}

// Vulkan mode: the Vulkan submit has completed (fence) and released key 1; take it, run the usual copy, give key 0 back.
// The whole sequence runs under the FFmpeg device lock: the encoder thread shares the immediate context.
std::shared_ptr<AVFrame> D3d11EncodeCapture::Impl::captureShared() {
  if (!interop || !framesRef || !keyedMutex || !lockBox) {
    fail("register", QStringLiteral("capture without a shared texture"));
    return nullptr;
  }
  const std::lock_guard<std::recursive_mutex> lock(lockBox->mutex);
  const HRESULT acq = keyedMutex->AcquireSync(1, 100);
  if (acq != S_OK) {  // WAIT_TIMEOUT is a success code that did not acquire
    fail("keyed mutex", acq == static_cast<HRESULT>(WAIT_TIMEOUT) ? QStringLiteral("timed out waiting for the Vulkan frame") : hresultText(acq));
    return nullptr;
  }
  std::shared_ptr<AVFrame> frame = kind.load() == Kind::Qsv ? frameQsv() : frameAmf();  // Nvenc: the same BGRA copy as AMF
  const HRESULT rel = keyedMutex->ReleaseSync(0);  // queued behind the copy on this device: the next Vulkan frame may write
  if (FAILED(rel)) {
    fail("keyed mutex", QStringLiteral("ReleaseSync %1").arg(hresultText(rel)));
    return nullptr;
  }
  if (frame) {
    lastFrame = frame;
  }
  return frame;
}

D3d11EncodeCapture::Status D3d11EncodeCapture::Impl::createVulkanDevice(const unsigned char* luid, unsigned vendorId) {
  ComPtr<IDXGIFactory4> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return fail("device", QStringLiteral("CreateDXGIFactory1 failed"));
  }
  LUID id = {};
  std::memcpy(&id, luid, sizeof id);  // VkPhysicalDeviceIDProperties::deviceLUID is a LUID in memory order
  ComPtr<IDXGIAdapter1> adapter;
  HRESULT hr = factory->EnumAdapterByLuid(id, IID_PPV_ARGS(&adapter));
  if (FAILED(hr)) {
    return unavailable(QStringLiteral("the Vulkan device's adapter is not a DXGI adapter (%1)").arg(hresultText(hr)));
  }
  DXGI_ADAPTER_DESC1 desc = {};
  adapter->GetDesc1(&desc);
  if (desc.VendorId != vendorId) {
    return unavailable(QStringLiteral("the adapter's vendor does not match the Vulkan device"));
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                         levels, 1, D3D11_SDK_VERSION, &device, nullptr, nullptr);
  if (FAILED(hr)) {
    return fail("device", QStringLiteral("D3D11CreateDevice %1").arg(hresultText(hr)));
  }
  ComPtr<ID3D10Multithread> mt;
  if (SUCCEEDED(device.As(&mt))) {
    mt->SetMultithreadProtected(TRUE);
  }
  std::memcpy(luidBytes, luid, 8);
  luidKnown = true;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    deviceName = QString::fromWCharArray(desc.Description);
  }
  return Status::Ok;
}

// The shared texture: B8G8R8A8, NT handle + keyed mutex (key 0 = the Vulkan side may write, key 1 = ready to capture).
D3d11EncodeCapture::Status D3d11EncodeCapture::Impl::createSharedTexture(int w, int h) {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  HRESULT hr = device->CreateTexture2D(&td, nullptr, &interop);
  if (FAILED(hr)) {
    return fail("interop texture", hresultText(hr));
  }
  hr = interop.As(&keyedMutex);
  if (FAILED(hr)) {
    return fail("keyed mutex", hresultText(hr));
  }
  ComPtr<IDXGIResource1> resource;
  hr = interop.As(&resource);
  if (FAILED(hr)) {
    return fail("shared handle", hresultText(hr));
  }
  hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle);
  if (FAILED(hr) || !sharedHandle) {
    sharedHandle = nullptr;
    return fail("shared handle", hresultText(hr));
  }
  width = w;
  height = h;
  return Status::Ok;
}

D3d11EncodeCapture::Status D3d11EncodeCapture::attachShared(int w, int h, const unsigned char* luid, unsigned vendorId, SharedImage* out) {
  return d_->attachShared(w, h, luid, vendorId, out);
}

D3d11EncodeCapture::Status D3d11EncodeCapture::Impl::attachShared(int w, int h, const unsigned char* luid, unsigned vendorId, SharedImage* out) {
  const Status current = status.load();
  if (current == Status::Unavailable || current == Status::Failed) {
    return current;
  }
  if (w < 16 || h < 16 || (w & 1) != 0 || (h & 1) != 0) {
    return unavailable(QStringLiteral("encode size %1 x %2 is not an even size of at least 16").arg(w).arg(h));
  }
  if (!luid) {
    return unavailable(QStringLiteral("the Vulkan device does not report its adapter (no LUID)"));
  }
  QElapsedTimer timer;
  timer.start();
  vulkanMode = true;
  if (sharedHandle && w == width && h == height) {
    out->ntHandle = sharedHandle;
    out->width = w;
    out->height = h;
    return Status::Ok;
  }
  if (!device) {
    const Kind k = D3d11EncodeCapture::kindForVendor(vendorId, /*vulkanMode=*/true);
    if (k == Kind::None) {
      return unavailable(QStringLiteral("not an AMD, Intel or NVIDIA GPU"));
    }
    if (k == Kind::Nvenc && !VideoEncoder::nvencD3d11InputSupported()) {
      return unavailable(QStringLiteral("this FFmpeg cannot feed h264_nvenc with D3D11 frames"));
    }
    kind = k;
    const Status v = createVulkanDevice(luid, vendorId);
    if (v != Status::Ok) {
      releaseCycle(false);
      return v;
    }
    const Status d = createDevice();
    if (d != Status::Ok) {
      releaseCycle(false);
      return d;
    }
  } else {
    releaseSize(false);  // a different size: the frames context changes, so the encoder reopens
  }
  Status s = createSharedTexture(w, h);
  if (s == Status::Ok) {
    s = createFrames(w, h);
  }
  if (s != Status::Ok) {
    releaseSize(false);
    return s;
  }
  status = Status::Ok;
  out->ntHandle = sharedHandle;
  out->width = w;
  out->height = h;
  const Kind k = kind.load();
  QString name;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    name = deviceName;
  }
  qCInfo(lcD3dCap).noquote() << QStringLiteral("D3D11 capture on \"%1\" (Vulkan): %2, %3 frames ready in %4 ms")
                                    .arg(name,
                                         k == Kind::Amf ? QStringLiteral("h264_amf") : k == Kind::Qsv ? QStringLiteral("h264_qsv") : QStringLiteral("h264_nvenc"),
                                         k == Kind::Qsv ? QStringLiteral("NV12") : QStringLiteral("BGRA"))
                                    .arg(timer.elapsed());
  return Status::Ok;
}

bool D3d11EncodeCapture::adapterLuid(unsigned char out[8]) const {
  if (!d_->luidKnown) {
    return false;
  }
  std::memcpy(out, d_->luidBytes, 8);
  return true;
}

std::shared_ptr<AVFrame> D3d11EncodeCapture::repeat() {
  if (!d_->vulkanMode || !d_->lastFrame || d_->status.load() != Status::Ok) {
    return nullptr;
  }
  AVFrame* clone = av_frame_clone(d_->lastFrame.get());  // the same D3D11 surface once more; nothing is copied
  return clone ? std::shared_ptr<AVFrame>(clone, [](AVFrame* p) { av_frame_free(&p); }) : nullptr;
}

std::shared_ptr<AVFrame> D3d11EncodeCapture::Impl::frameAmf() {
  AVFrame* f = av_frame_alloc();
  if (!f) {
    fail("frame buffer", QStringLiteral("out of memory"));
    return nullptr;
  }
  const int rc = av_hwframe_get_buffer(framesRef, f, 0);  // the encoder's own frames context: AMF asserts on any other
  if (rc < 0) {
    av_frame_free(&f);
    fail("frame buffer", QStringLiteral("av_hwframe_get_buffer: %1").arg(ffmpegError(rc)));
    return nullptr;
  }
  auto* dst = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
  const UINT index = static_cast<UINT>(reinterpret_cast<intptr_t>(f->data[1]));
  if (!dst) {
    av_frame_free(&f);
    fail("frame buffer", QStringLiteral("frame without a texture"));
    return nullptr;
  }
  {
    const std::lock_guard<std::recursive_mutex> lock(lockBox->mutex);
    immediate->CopySubresourceRegion(dst, index, 0, 0, 0, interop.Get(), 0, nullptr);
    immediate->Flush();
  }
  return std::shared_ptr<AVFrame>(f, [](AVFrame* p) { av_frame_free(&p); });
}

ID3D11VideoProcessorOutputView* D3d11EncodeCapture::Impl::outputView(ID3D11Texture2D* texture, HRESULT* hr) {
  for (auto& entry : vpOutputs) {
    if (entry.first.Get() == texture) {
      return entry.second.Get();
    }
  }
  if (vpOutputs.size() >= kMaxCachedViews) {
    vpOutputs.clear();
  }
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = {};
  ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11VideoProcessorOutputView> view;
  *hr = videoDevice->CreateVideoProcessorOutputView(texture, vpEnum.Get(), &ov, &view);
  if (FAILED(*hr)) {
    return nullptr;
  }
  vpOutputs.emplace_back(texture, view);  // the view keeps the texture alive; a pool texture is never replaced under us
  return vpOutputs.back().second.Get();
}

std::shared_ptr<AVFrame> D3d11EncodeCapture::Impl::frameQsv() {
  AVFrame* src = av_frame_alloc();
  AVFrame* dst = av_frame_alloc();
  if (!src || !dst) {
    av_frame_free(&src);
    av_frame_free(&dst);
    fail("frame buffer", QStringLiteral("out of memory"));
    return nullptr;
  }
  const auto bail = [&](const char* step, const QString& why) {
    av_frame_free(&src);
    av_frame_free(&dst);
    fail(step, why);
    return std::shared_ptr<AVFrame>();
  };
  int rc = av_hwframe_get_buffer(framesRef, src, 0);  // dynamic pool: a recycled or new single NV12 texture
  if (rc < 0) {
    return bail("frame buffer", QStringLiteral("av_hwframe_get_buffer: %1").arg(ffmpegError(rc)));
  }
  auto* texture = reinterpret_cast<ID3D11Texture2D*>(src->data[0]);
  if (!texture) {
    return bail("frame buffer", QStringLiteral("frame without a texture"));
  }
  {
    const std::lock_guard<std::recursive_mutex> lock(lockBox->mutex);
    HRESULT hr = S_OK;
    ID3D11VideoProcessorOutputView* view = outputView(texture, &hr);
    if (!view) {
      return bail("video processor", QStringLiteral("CreateVideoProcessorOutputView %1").arg(hresultText(hr)));
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = vpInput.Get();
    hr = videoContext->VideoProcessorBlt(vp.Get(), view, 0, 1, &stream);
    if (FAILED(hr)) {
      return bail("video processor", QStringLiteral("VideoProcessorBlt %1").arg(hresultText(hr)));
    }
    immediate->Flush();
  }
  dst->format = AV_PIX_FMT_QSV;
  dst->hw_frames_ctx = av_buffer_ref(qsvFramesRef);
  if (!dst->hw_frames_ctx) {
    return bail("map", QStringLiteral("out of memory"));
  }
  rc = av_hwframe_map(dst, src, 0);  // the mapped frame keeps `src` (and its pool texture) referenced
  if (rc < 0) {
    return bail("map", QStringLiteral("av_hwframe_map: %1").arg(ffmpegError(rc)));
  }
  dst->width = width;  // the visible size; the textures are 16-aligned (the encoder crops to this)
  dst->height = height;
  av_frame_free(&src);
  return std::shared_ptr<AVFrame>(dst, [](AVFrame* p) { av_frame_free(&p); });
}

void D3d11EncodeCapture::detach(bool glCurrent) { d_->detach(glCurrent); }

void D3d11EncodeCapture::Impl::releaseSize(bool glCurrent) {
  const GlD3d11Bridge::GlFunctions* f = gl();
  lastFrame.reset();
  keyedMutex.Reset();
  if (sharedHandle) {
    CloseHandle(sharedHandle);  // the Vulkan side imported its own reference; it frees it with its image
    sharedHandle = nullptr;
  }
  vpOutputs.clear();
  vpInput.Reset();
  vp.Reset();
  vpEnum.Reset();
  av_buffer_unref(&qsvFramesRef);  // frames and encoders in flight keep their own references
  av_buffer_unref(&framesRef);
  if (reg || interop || glName || fbo[0]) {
    if (glCurrent && f && bridge) {
      if (reg && !bridge->unregister(reg)) {
        qCWarning(lcD3dCap).noquote() << QStringLiteral("GPU-direct: unregistering the interop texture failed, error %1").arg(bridge->lastWin32Error());
      }
      if (fbo[0] && f->deleteFramebuffers) {
        f->deleteFramebuffers(2, fbo);
      }
      if (glName) {
        f->deleteTextures(1, &glName);
      }
      interop.Reset();
    } else if (reg) {
      qCWarning(lcD3dCap) << "GPU-direct: GL context lost; leaking the interop registration";
      interop.Detach();  // still registered with the interop device: never released without GL
    } else {
      interop.Reset();  // never registered
    }
  }
  reg = nullptr;
  glName = 0;
  fbo[0] = fbo[1] = 0;
  srcTexture = 0;
  width = height = 0;
}

void D3d11EncodeCapture::Impl::releaseCycle(bool glCurrent) {
  releaseSize(glCurrent);
  av_buffer_unref(&qsvDeviceRef);
  av_buffer_unref(&deviceRef);  // frees the lock box too, once the last frame holding the device is gone
  lockBox = nullptr;
  videoContext.Reset();
  videoDevice.Reset();
  immediate.Reset();
  device.Reset();
  if (bridge && ownBridge) {
    bridge->close(glCurrent);
  } else if (bridge) {
    bridge->releaseUser(glCurrent);  // the display bridge: the last user (display or capture) closes it
  }
  bridge.reset();
  ownBridge = false;
  vulkanMode = false;
  luidKnown = false;
}

void D3d11EncodeCapture::Impl::detach(bool glCurrent) { releaseCycle(glCurrent && wglGetCurrentContext() != nullptr); }

D3d11EncodeCapture::D3d11EncodeCapture() : D3d11EncodeCapture(BridgeProvider{}) {}
D3d11EncodeCapture::D3d11EncodeCapture(BridgeProvider provider) : d_(std::make_unique<Impl>(std::move(provider))) {}

D3d11EncodeCapture::~D3d11EncodeCapture() {
  if (d_->reg) {
    qCWarning(lcD3dCap).noquote() << QStringLiteral("GPU-direct: destroyed while a texture is still registered; leaking the interop registration");
  }
  d_->releaseCycle(false);  // never unregisters without GL
}

#else  // !FB_D3D11_CAPTURE

struct D3d11EncodeCapture::Impl {
  explicit Impl(BridgeProvider) {}
  std::atomic<Status> status{Status::NotReady};
  mutable std::mutex mutex;
  QString reason;
};

D3d11EncodeCapture::D3d11EncodeCapture() : D3d11EncodeCapture(BridgeProvider{}) {}
D3d11EncodeCapture::D3d11EncodeCapture(BridgeProvider provider) : d_(std::make_unique<Impl>(std::move(provider))) {}
D3d11EncodeCapture::~D3d11EncodeCapture() = default;

D3d11EncodeCapture::Status D3d11EncodeCapture::attach(unsigned, int, int) {
  if (d_->status.load() == Status::NotReady) {
    {
#ifdef _WIN32
      const QString why = QStringLiteral("FFmpeg without D3D11VA headers");
#else
      const QString why = QStringLiteral("not Windows");
#endif
      const std::lock_guard<std::mutex> lock(d_->mutex);
      d_->reason = why;
    }
    d_->status = Status::Unavailable;
    qCInfo(lcD3dCap).noquote() << QStringLiteral("GPU-direct capture unavailable: %1").arg(reason());
  }
  return d_->status.load();
}
D3d11EncodeCapture::Status D3d11EncodeCapture::attachShared(int, int, const unsigned char*, unsigned, SharedImage*) { return attach(0, 0, 0); }
bool D3d11EncodeCapture::adapterLuid(unsigned char[8]) const { return false; }
std::shared_ptr<AVFrame> D3d11EncodeCapture::repeat() { return nullptr; }
void D3d11EncodeCapture::detach(bool) {}
std::shared_ptr<AVFrame> D3d11EncodeCapture::capture() { return nullptr; }

#endif  // FB_D3D11_CAPTURE

D3d11EncodeCapture::Status D3d11EncodeCapture::status() const { return d_->status.load(); }

QString D3d11EncodeCapture::reason() const {
  const std::lock_guard<std::mutex> lock(d_->mutex);
  return d_->reason;
}

QString D3d11EncodeCapture::deviceName() const {
#ifdef FB_D3D11_CAPTURE
  const std::lock_guard<std::mutex> lock(d_->mutex);
  return d_->deviceName;
#else
  return {};
#endif
}

D3d11EncodeCapture::Kind D3d11EncodeCapture::kind() const {
#ifdef FB_D3D11_CAPTURE
  return d_->kind.load();
#else
  return Kind::None;
#endif
}

}  // namespace uzume
