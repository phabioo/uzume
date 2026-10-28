// Backend choice of GpuEncodeBridge (ADR 0027 D6) with fake captures: no GL, no GPU. The ADR 0019 CUDA behaviour with the
// real capture classes is covered by ui_gpu_encode.
#include <QtTest>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "d3d11glbridge.h"
#include "gpuencodebridge.h"
#include "processguard.h"

extern "C" {
#include <libavutil/frame.h>
}

using namespace uzume;
using namespace uzume::ui;
using Attach = emu::GpuEncodeTarget::Attach;
using St = GpuCapture::Status;

namespace {

struct Log {
  int attaches = 0, detaches = 0, captures = 0;
  bool lastDetachGlCurrent = false;
  int sharedAttaches = 0, repeats = 0, lastW = 0, lastH = 0;
  unsigned lastVendor = 0;
  bool lastLuidNull = false, hasFrame = false;
  unsigned char lastLuid[8] = {};
  void* handle = reinterpret_cast<void*>(0x1234);
};

// Scripted capture: returns the given statuses one per attach (the last one repeats).
class FakeCapture final : public GpuCapture {
 public:
  FakeCapture(Backend b, std::vector<St> script, QString why, std::shared_ptr<Log> log)
      : backend_(b), script_(std::move(script)), why_(std::move(why)), log_(std::move(log)) {}
  Status attach(unsigned, int, int) override {
    ++log_->attaches;
    const size_t i = std::min<size_t>(static_cast<size_t>(log_->attaches) - 1, script_.size() - 1);
    return script_[i];
  }
  void detach(bool glCurrent) override {
    ++log_->detaches;
    log_->lastDetachGlCurrent = glCurrent;
  }
  std::shared_ptr<AVFrame> capture() override {
    ++log_->captures;
    if (failCapture) return nullptr;
    log_->hasFrame = true;
    return std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
  }
  Status attachShared(int w, int h, const unsigned char* luid, unsigned vendorId, SharedImage* out) override {
    ++log_->attaches;
    ++log_->sharedAttaches;
    log_->lastW = w;
    log_->lastH = h;
    log_->lastVendor = vendorId;
    log_->lastLuidNull = luid == nullptr;
    if (luid) std::memcpy(log_->lastLuid, luid, 8);
    const size_t i = std::min<size_t>(static_cast<size_t>(log_->attaches) - 1, script_.size() - 1);
    if (script_[i] == Status::Ok) {
      out->ntHandle = log_->handle;
      out->width = w;
      out->height = h;
      out->luidValid = true;  // the adapter the capture's device ended up on: deliberately not the Vulkan device's (0x27 in byte 7)
      for (int k = 0; k < 8; ++k) out->luid[k] = static_cast<unsigned char>(0x40 + k);
    }
    return script_[i];
  }
  std::shared_ptr<AVFrame> repeat() override {
    ++log_->repeats;
    if (!log_->hasFrame) return nullptr;
    return std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
  }
  QString reason() const override { return why_; }
  Backend backend() const override { return backend_; }
  bool failCapture = false;

 private:
  Backend backend_;
  std::vector<St> script_;
  QString why_;
  std::shared_ptr<Log> log_;
};

struct Rig {
  GpuEncodeOptions options;
  std::shared_ptr<Log> cudaLog = std::make_shared<Log>();
  std::shared_ptr<Log> d3dLog = std::make_shared<Log>();
  int cudaMade = 0, d3dMade = 0;
  FakeCapture* lastD3d = nullptr;
  FakeCapture* lastCuda = nullptr;

  Rig(GpuEncodeAllowed allowed, const QString& vendor, std::vector<St> cudaScript, std::vector<St> d3dScript,
      GpuCapture::Backend d3dBackend = GpuCapture::Backend::Amf, const QString& cudaWhy = QStringLiteral("cuda reason"),
      const QString& d3dWhy = QStringLiteral("d3d11 reason")) {
    options.allowed = allowed;
    options.glVendor = [vendor]() { return vendor; };
    options.makeCuda = [this, cudaScript, cudaWhy]() -> std::unique_ptr<GpuCapture> {
      ++cudaMade;
      auto c = std::make_unique<FakeCapture>(GpuCapture::Backend::Cuda, cudaScript, cudaWhy, cudaLog);
      lastCuda = c.get();
      return c;
    };
    options.makeD3d11 = [this, d3dScript, d3dBackend, d3dWhy](std::function<std::shared_ptr<GlD3d11Bridge>()> provider) -> std::unique_ptr<GpuCapture> {
      ++d3dMade;
      provided = std::move(provider);
      auto c = std::make_unique<FakeCapture>(d3dBackend, d3dScript, d3dWhy, d3dLog);
      lastD3d = c.get();
      return c;
    };
  }
  std::function<std::shared_ptr<GlD3d11Bridge>()> provided;
};

const QSize kMax(1280, 1920);

emu::GpuEncodeTarget::VulkanDevice device(unsigned vendor, bool luidValid = true) {
  emu::GpuEncodeTarget::VulkanDevice d;
  d.vendorId = vendor;
  d.luid.valid = luidValid;
  for (int i = 0; i < 8; ++i) d.luid.bytes[i] = static_cast<quint8>(0x20 + i);
  return d;
}

}  // namespace

class GpuEncodeSelectTest : public QObject {
  Q_OBJECT
 private slots:
  void amdGpuTakesTheD3d11CaptureAndNeverTouchesCuda() {
    Rig rig({true, true, true}, QStringLiteral("ATI Technologies Inc."), {St::Ok}, {St::Ok}, GpuCapture::Backend::Amf);
    GpuEncodeBridge bridge(kMax, rig.options);
    bridge.setWanted(true);
    QCOMPARE(bridge.attach(7, 256, 192), Attach::Ok);
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Running);
    QCOMPARE(rig.cudaMade, 0);
    QVERIFY(bridge.capture());
    QCOMPARE(rig.d3dLog->captures, 1);
    QVERIFY(bridge.takeFrame());
    QCOMPARE(bridge.attach(7, 256, 192), Attach::Ok);  // later attaches go to the chosen capture, no new one
    QCOMPARE(rig.d3dMade, 1);
    QCOMPARE(rig.d3dLog->attaches, 2);
  }

  void intelGpuTakesQsvWhenQsvIsAllowed() {
    Rig rig({false, false, true}, QStringLiteral("Intel"), {St::Ok}, {St::Ok}, GpuCapture::Backend::Qsv);
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    QCOMPARE(rig.cudaMade, 0);
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Running);
  }

  void nvidiaGlContextGoesStraightToCuda() {
    Rig rig({true, true, true}, QStringLiteral("NVIDIA Corporation"), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    QCOMPARE(rig.d3dMade, 0);  // ADR 0019: D3D11 is never touched on an NVIDIA GL context
    QCOMPARE(rig.cudaMade, 1);
  }

  void cudaOnlyNeverAsksTheVendor() {
    Rig rig({true, false, false}, QStringLiteral("anything"), {St::Ok}, {St::Ok});
    bool asked = false;
    rig.options.glVendor = [&asked]() {
      asked = true;
      return QString();
    };
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    QVERIFY(!asked);
    QCOMPARE(rig.d3dMade, 0);
  }

  void d3d11UnavailableFallsBackToCuda() {
    Rig rig({true, true, true}, QStringLiteral("Mesa"), {St::Ok}, {St::Unavailable}, GpuCapture::Backend::Amf, QStringLiteral("cuda reason"),
            QStringLiteral("not an AMD or Intel GPU"));
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    QCOMPARE(rig.d3dMade, 1);
    QCOMPARE(rig.d3dLog->detaches, 1);  // the failed capture released what it had, GL current
    QVERIFY(rig.d3dLog->lastDetachGlCurrent);
    QCOMPARE(rig.cudaMade, 1);
    QVERIFY(bridge.capture());
    QCOMPARE(rig.cudaLog->captures, 1);
    QCOMPARE(rig.d3dLog->captures, 0);
  }

  void withoutCudaAnUnavailableD3d11MeansUnavailable() {
    Rig rig({false, true, true}, QStringLiteral("Mesa"), {St::Ok}, {St::Unavailable}, GpuCapture::Backend::Amf, QString(),
            QStringLiteral("not an AMD or Intel GPU"));
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Unavailable);
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Unavailable);
    QCOMPARE(bridge.reason(), QStringLiteral("not an AMD or Intel GPU"));
    QCOMPARE(rig.cudaMade, 0);
    QVERIFY(!bridge.wanted());
  }

  void aD3d11CaptureOfAnEncoderThatIsNotAllowedIsReleased() {
    // QSV GPU but only h264_amf is allowed (e.g. h264_qsv did not open at the probe).
    Rig rig({false, true, false}, QStringLiteral("Intel"), {St::Ok}, {St::Ok}, GpuCapture::Backend::Qsv);
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Unavailable);
    QCOMPARE(rig.d3dLog->detaches, 1);
    QVERIFY(bridge.reason().contains(QLatin1String("h264_qsv")));
  }

  void notReadyAsksTheSameCaptureAgain() {
    Rig rig({true, true, true}, QStringLiteral("AMD"), {St::Ok}, {St::NotReady, St::NotReady, St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::NotReady);
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Running);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::NotReady);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    QCOMPARE(rig.d3dMade, 1);
    QCOMPARE(rig.cudaMade, 0);
    QCOMPARE(rig.d3dLog->attaches, 3);
  }

  void notReadyThenUnavailableStillReachesCuda() {
    Rig rig({true, true, true}, QStringLiteral("AMD"), {St::Ok}, {St::NotReady, St::Unavailable});
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::NotReady);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    QCOMPARE(rig.cudaMade, 1);
  }

  void aRealD3d11FailureIsReportedAsFailed() {
    Rig rig({false, true, true}, QStringLiteral("AMD"), {St::Ok}, {St::Failed}, GpuCapture::Backend::Amf, QString(),
            QStringLiteral("register: wglDXRegisterObjectNV failed"));
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Failed);
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Failed);
    QCOMPARE(bridge.reason(), QStringLiteral("register: wglDXRegisterObjectNV failed"));
  }

  void aRealD3d11FailureIsNotHiddenByCudaBeingUnavailable() {
    Rig rig({true, true, true}, QStringLiteral("AMD"), {St::Unavailable}, {St::Failed}, GpuCapture::Backend::Amf,
            QStringLiteral("the game's OpenGL context runs on 'AMD', not an NVIDIA GPU"), QStringLiteral("device: av_hwdevice_ctx_init failed"));
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Failed);
    QCOMPARE(bridge.reason(), QStringLiteral("device: av_hwdevice_ctx_init failed"));
  }

  void captureFailureFailsTheBridge() {
    Rig rig({false, true, false}, QStringLiteral("AMD"), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    bridge.setWanted(true);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    rig.lastD3d->failCapture = true;
    QVERIFY(!bridge.capture());
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Failed);
    QVERIFY(!bridge.takeFrame());
  }

  void framesGoToTheMailboxOnlyWhileWanted() {
    Rig rig({false, true, false}, QStringLiteral("AMD"), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    QVERIFY(bridge.capture());  // not wanted: the frame is dropped
    QVERIFY(!bridge.takeFrame());
    bridge.setWanted(true);
    QVERIFY(bridge.capture());
    QVERIFY(bridge.takeFrame());
    QVERIFY(!bridge.takeFrame());
  }

  void detachReachesTheChosenAndThePendingCapture() {
    {
      Rig rig({true, true, true}, QStringLiteral("AMD"), {St::Ok}, {St::NotReady});
      GpuEncodeBridge bridge(kMax, rig.options);
      bridge.detach(true);  // before any attach: nothing
      QCOMPARE(rig.d3dLog->detaches, 0);
      QCOMPARE(bridge.attach(1, 256, 192), Attach::NotReady);
      bridge.detach(false);  // GL lost while the capture waited
      QCOMPARE(rig.d3dLog->detaches, 1);
      QVERIFY(!rig.d3dLog->lastDetachGlCurrent);
    }
    Rig rig({false, true, false}, QStringLiteral("AMD"), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    bridge.detach(true);
    QCOMPARE(rig.d3dLog->detaches, 1);
    QCOMPARE(bridge.attach(2, 256, 192), Attach::Ok);  // a live-save restart re-attaches the same capture
    QCOMPARE(rig.d3dMade, 1);
  }

  void theDisplayBridgeProviderReachesTheD3d11Capture() {
    Rig rig({false, true, false}, QStringLiteral("AMD"), {St::Ok}, {St::Ok});
    auto shared = std::make_shared<GlD3d11Bridge>();
    rig.options.displayBridge = [shared]() { return shared; };
    GpuEncodeBridge bridge(kMax, rig.options);
    QCOMPARE(bridge.attach(1, 256, 192), Attach::Ok);
    QVERIFY(rig.provided);
    QCOMPARE(rig.provided().get(), shared.get());
  }

  // ---- Vulkan games (ADR 0034 D9): attachExternal(), the D3D11 backend for every vendor

  void vulkanBackendTruthTable() {
    const GpuEncodeAllowed none{false, false, false}, all{true, true, true};
    const struct {
      unsigned vendor;
      GpuEncodeAllowed allowed;
      VulkanBackend expect;
    } cases[] = {
        {0x1002, all, VulkanBackend::Amf},    {0x1002, {true, false, true}, VulkanBackend::None},
        {0x8086, all, VulkanBackend::Qsv},    {0x8086, {true, true, false}, VulkanBackend::None},
        {0x10DE, all, VulkanBackend::Nvenc},  {0x10DE, {false, true, true}, VulkanBackend::None},  // NVENC needs the cuda flag
        {0x10DE, {true, false, false}, VulkanBackend::Nvenc},
        {0x1002, none, VulkanBackend::None},  {0x8086, none, VulkanBackend::None},
        {0x10DE, none, VulkanBackend::None},  {0x13B5, all, VulkanBackend::None},   // Arm
        {0x0, all, VulkanBackend::None},      {0x10005, all, VulkanBackend::None},  // Mesa software renderer
    };
    for (const auto& c : cases) {
      QVERIFY2(chooseVulkanBackend(c.vendor, c.allowed) == c.expect,
               qPrintable(QStringLiteral("vendor 0x%1 cuda=%2 amf=%3 qsv=%4").arg(c.vendor, 0, 16).arg(c.allowed.cuda).arg(c.allowed.amf).arg(c.allowed.qsv)));
    }
  }

  void vulkanAmdTakesTheD3d11CaptureWithoutAGlVendorOrBridge() {
    Rig rig({true, true, true}, QStringLiteral("must not be asked"), {St::Ok}, {St::Ok}, GpuCapture::Backend::Amf);
    rig.options.glVendor = []() -> QString {
      qFatal("a Vulkan game has no GL vendor");
      return {};
    };
    rig.options.displayBridge = []() { return std::make_shared<GlD3d11Bridge>(); };
    GpuEncodeBridge bridge(kMax, rig.options);
    bridge.setWanted(true);
    emu::ExternalImage img;
    QCOMPARE(bridge.attachExternal(640, 480, device(0x1002), &img), Attach::Ok);
    QCOMPARE(rig.cudaMade, 0);
    QCOMPARE(rig.d3dMade, 1);
    QVERIFY(!rig.provided);  // no GL display bridge in Vulkan mode: the capture opens a device on the Vulkan adapter
    QCOMPARE(rig.d3dLog->sharedAttaches, 1);
    QCOMPARE(rig.d3dLog->lastVendor, 0x1002u);
    QVERIFY(!rig.d3dLog->lastLuidNull);
    QCOMPARE(rig.d3dLog->lastLuid[3], quint8(0x23));
    QCOMPARE(rig.d3dLog->lastW, 640);
    QCOMPARE(rig.d3dLog->lastH, 480);
    QVERIFY(img.handleType == emu::ExternalImage::HandleType::D3D11TextureNt);
    QCOMPARE(img.handle, reinterpret_cast<qintptr>(rig.d3dLog->handle));
    QCOMPARE(img.texSize, QSize(640, 480));
    QCOMPARE(img.slot, 0);
    QVERIFY(img.bgra);
    QVERIFY(img.keyedMutex);
    QCOMPARE(img.acquireKey, quint64(0));
    QCOMPARE(img.releaseKey, quint64(1));
    const quint64 gen = img.generation;
    QVERIFY(gen != 0);
    QVERIFY(bridge.adapterLuid().valid);
    QCOMPARE(bridge.adapterLuid().bytes[7], quint8(0x47));  // the capture's adapter, not a copy of the device's LUID
    emu::ExternalImage again;  // same size: the same texture, the same generation, no second capture object
    QCOMPARE(bridge.attachExternal(640, 480, device(0x1002), &again), Attach::Ok);
    QCOMPARE(again.generation, gen);
    QCOMPARE(rig.d3dMade, 1);
  }

  void vulkanSizeChangeIsANewGeneration() {
    Rig rig({false, true, false}, QString(), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    emu::ExternalImage a, b;
    QCOMPARE(bridge.attachExternal(640, 480, device(0x1002), &a), Attach::Ok);
    QCOMPARE(bridge.attachExternal(800, 600, device(0x1002), &b), Attach::Ok);
    QVERIFY(b.generation > a.generation);
    QCOMPARE(b.texSize, QSize(800, 600));
  }

  void vulkanIntelAndNvidiaChooseTheirBackends() {
    {
      Rig rig({false, false, true}, QString(), {St::Ok}, {St::Ok}, GpuCapture::Backend::Qsv);
      GpuEncodeBridge bridge(kMax, rig.options);
      emu::ExternalImage img;
      QCOMPARE(bridge.attachExternal(640, 480, device(0x8086), &img), Attach::Ok);
      QCOMPARE(rig.d3dLog->lastVendor, 0x8086u);
    }
    {
      Rig rig({true, false, false}, QString(), {St::Ok}, {St::Ok}, GpuCapture::Backend::Nvenc);
      GpuEncodeBridge bridge(kMax, rig.options);
      emu::ExternalImage img;
      QCOMPARE(bridge.attachExternal(640, 480, device(0x10DE), &img), Attach::Ok);
      QCOMPARE(rig.cudaMade, 0);  // CUDA stays out of it: the frames are D3D11
      QCOMPARE(rig.d3dMade, 1);
      QCOMPARE(rig.d3dLog->lastVendor, 0x10DEu);
    }
  }

  void vulkanBackendThatIsNotAllowedIsUnavailableNotFailed() {
    Rig rig({false, false, true}, QString(), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    emu::ExternalImage img;
    QCOMPARE(bridge.attachExternal(640, 480, device(0x1002), &img), Attach::Unavailable);  // AMD, only QSV allowed
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Unavailable);
    QVERIFY(bridge.reason().contains(QLatin1String("h264_amf")));
    QCOMPARE(rig.d3dMade, 0);  // nothing was created
  }

  void vulkanUnknownVendorIsUnavailable() {
    Rig rig({true, true, true}, QString(), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    emu::ExternalImage img;
    QCOMPARE(bridge.attachExternal(640, 480, device(0x10005), &img), Attach::Unavailable);
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Unavailable);
    QCOMPARE(rig.d3dMade, 0);
    QCOMPARE(rig.cudaMade, 0);
  }

  void vulkanWithoutALuidPassesNull() {
    Rig rig({false, true, false}, QString(), {St::Unavailable}, {St::Unavailable}, GpuCapture::Backend::Amf, QString(), QStringLiteral("no LUID"));
    GpuEncodeBridge bridge(kMax, rig.options);
    emu::ExternalImage img;
    QCOMPARE(bridge.attachExternal(640, 480, device(0x1002, false), &img), Attach::Unavailable);
    QVERIFY(rig.d3dLog->lastLuidNull);
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Unavailable);
    QCOMPARE(bridge.reason(), QStringLiteral("no LUID"));
  }

  void vulkanCaptureFailuresMapToFailed() {
    Rig rig({false, true, false}, QString(), {St::Ok}, {St::Failed}, GpuCapture::Backend::Amf, QString(), QStringLiteral("device"));
    GpuEncodeBridge bridge(kMax, rig.options);
    emu::ExternalImage img;
    QCOMPARE(bridge.attachExternal(640, 480, device(0x1002), &img), Attach::Failed);
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Failed);
    QCOMPARE(bridge.reason(), QStringLiteral("device"));
  }

  void vulkanMarkUnavailableIsQuietAndFirstReasonWins() {
    Rig rig({false, true, false}, QString(), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    bridge.markUnavailable(QStringLiteral("Vulkan device is on another adapter"));
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Unavailable);
    bridge.fail(QStringLiteral("later"));
    QCOMPARE(bridge.state(), GpuEncodeBridge::State::Unavailable);
    QCOMPARE(bridge.reason(), QStringLiteral("Vulkan device is on another adapter"));
    QVERIFY(!bridge.wanted());
  }

  void vulkanFramesAndRepeatGoToTheMailbox() {
    Rig rig({false, true, false}, QString(), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    bridge.setWanted(true);
    emu::ExternalImage img;
    QCOMPARE(bridge.attachExternal(640, 480, device(0x1002), &img), Attach::Ok);
    QVERIFY(!bridge.repeat());  // nothing captured yet: nothing to repeat
    QVERIFY(bridge.capture());
    QVERIFY(bridge.takeFrame());
    QVERIFY(bridge.repeat());
    QVERIFY(bridge.takeFrame());  // the duplicate frame is a Session frame too
    QCOMPARE(rig.d3dLog->repeats, 2);
    bridge.setWanted(false);
    QVERIFY(bridge.repeat());  // published nowhere while not wanted
    QVERIFY(!bridge.takeFrame());
  }

  void vulkanDetachReachesTheCapture() {
    Rig rig({false, true, false}, QString(), {St::Ok}, {St::Ok});
    GpuEncodeBridge bridge(kMax, rig.options);
    emu::ExternalImage img;
    QCOMPARE(bridge.attachExternal(640, 480, device(0x1002), &img), Attach::Ok);
    bridge.detach(true);
    QCOMPARE(rig.d3dLog->detaches, 1);
  }

  // A GL-only target answers Unavailable to attachExternal() by default (the Vulkan side then releases it quietly).
  void glOnlyTargetsRefuseTheVulkanAttach() {
    class GlOnly final : public emu::GpuEncodeTarget {
     public:
      bool wanted() const override { return true; }
      QSize maxSize() const override { return QSize(); }
      Attach attach(unsigned, int, int) override { return Attach::Ok; }
      void detach(bool) override {}
      bool capture() override { return true; }
      void fail(const QString&) override {}
    } t;
    emu::ExternalImage img;
    QCOMPARE(t.attachExternal(640, 480, device(0x1002), &img), Attach::Unavailable);
    QVERIFY(!t.adapterLuid().valid);
    QVERIFY(!t.repeat());
  }

  void theLegacyConstructorsAreCudaOnly() {
    GpuEncodeBridge a(kMax);
    GpuEncodeBridge b(kMax, CudaGlCapture::Deps{});
    QCOMPARE(a.state(), GpuEncodeBridge::State::Running);
    QCOMPARE(b.state(), GpuEncodeBridge::State::Running);
    QVERIFY(!a.wanted());
  }
};

FB_TEST_MAIN(GpuEncodeSelectTest)

#include "gpu_encode_select_test.moc"
