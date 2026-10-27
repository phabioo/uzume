// GPU-direct Session encoding backends (ADR 0027 D6), the parts that need no GPU: the frames-context -> encoder mapping of
// VideoEncoder::openGpu, the per-backend gpuInputAllowed() rules of SessionHost and the pure helpers of D3d11EncodeCapture.
// The capture itself (interop, D3D11, AMF, QSV) is verified on real GPUs only.
#include <QtTest>

#include "d3d11encodecapture.h"
#include "d3d11glbridge.h"
#include "processguard.h"
#include "sessionhost.h"
#include "videoencoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

using namespace uzume;

namespace {

AVBufferRef* fakeFramesContext(AVPixelFormat format, AVPixelFormat swFormat, int width, int height) {
  AVBufferRef* ref = av_buffer_allocz(sizeof(AVHWFramesContext));
  auto* fc = reinterpret_cast<AVHWFramesContext*>(ref->data);
  fc->format = format;
  fc->sw_format = swFormat;
  fc->width = width;
  fc->height = height;
  return ref;
}

QString option(const VideoEncoder::GpuEncoderSpec& spec, const QString& key) {
  for (const auto& o : spec.options) {
    if (o.first == key) {
      return o.second;
    }
  }
  return QStringLiteral("<unset>");
}

}  // namespace

class GpuBackendsTest : public QObject {
  Q_OBJECT
 private slots:
  void specMapsCudaFramesToNvenc() {
    const auto spec = VideoEncoder::gpuEncoderSpec(AV_PIX_FMT_CUDA, AV_PIX_FMT_RGB0);
    QCOMPARE(spec.encoder, QStringLiteral("h264_nvenc"));
    QCOMPARE(spec.framesLabel, QStringLiteral("CUDA"));
    QCOMPARE(option(spec, "preset"), QStringLiteral("p1"));
    QCOMPARE(option(spec, "tune"), QStringLiteral("ull"));
    QCOMPARE(option(spec, "rc"), QStringLiteral("cbr"));
    QCOMPARE(option(spec, "zerolatency"), QStringLiteral("1"));
    QCOMPARE(option(spec, "profile"), QStringLiteral("baseline"));
    QCOMPARE(option(spec, "forced-idr"), QStringLiteral("1"));
    QCOMPARE(option(spec, "delay"), QStringLiteral("0"));
    QCOMPARE(spec.options.last().first, QStringLiteral("delay"));  // ADR 0019 order: the CPU options, then delay
    QCOMPARE(spec.colorSpace, -1);
    QCOMPARE(spec.colorRange, -1);
  }

  void specMapsD3d11FramesToAmf() {
    const auto spec = VideoEncoder::gpuEncoderSpec(AV_PIX_FMT_D3D11, AV_PIX_FMT_BGRA);
    QCOMPARE(spec.encoder, QStringLiteral("h264_amf"));
    QCOMPARE(spec.framesLabel, QStringLiteral("D3D11"));
    QCOMPARE(option(spec, "async_depth"), QStringLiteral("1"));  // the default 16 would hold packets back
    QCOMPARE(option(spec, "usage"), QStringLiteral("ultralowlatency"));
    QCOMPARE(option(spec, "profile"), QStringLiteral("constrained_baseline"));
    QCOMPARE(option(spec, "rc"), QStringLiteral("cbr"));
    QCOMPARE(spec.colorSpace, static_cast<int>(AVCOL_SPC_SMPTE170M));
    QCOMPARE(spec.colorRange, static_cast<int>(AVCOL_RANGE_MPEG));
  }

  // Vulkan games on NVIDIA (ADR 0034 D9): BGRA D3D11 frames go to h264_nvenc when the frames context carries the marker.
  void specMapsMarkedD3d11FramesToNvenc() {
    const auto spec = VideoEncoder::gpuEncoderSpec(AV_PIX_FMT_D3D11, AV_PIX_FMT_BGRA, /*nvencD3d11=*/true);
    QCOMPARE(spec.encoder, QStringLiteral("h264_nvenc"));
    QCOMPARE(spec.framesLabel, QStringLiteral("D3D11"));
    const auto cuda = VideoEncoder::gpuEncoderSpec(AV_PIX_FMT_CUDA, AV_PIX_FMT_RGB0);
    QCOMPARE(spec.options, cuda.options);  // the same low-latency set as the CUDA path
    QCOMPARE(spec.colorSpace, static_cast<int>(AVCOL_SPC_SMPTE170M));  // BGRA is converted: BT.601 limited like AMF
    QCOMPARE(spec.colorRange, static_cast<int>(AVCOL_RANGE_MPEG));
    // The flag changes nothing for the other frame kinds.
    QCOMPARE(VideoEncoder::gpuEncoderSpec(AV_PIX_FMT_CUDA, AV_PIX_FMT_RGB0, true).framesLabel, QStringLiteral("CUDA"));
    QCOMPARE(VideoEncoder::gpuEncoderSpec(AV_PIX_FMT_QSV, AV_PIX_FMT_NV12, true).encoder, QStringLiteral("h264_qsv"));
    QVERIFY(VideoEncoder::gpuEncoderSpec(AV_PIX_FMT_D3D11, AV_PIX_FMT_NV12, true).encoder.isEmpty());
  }

  void specOfAFramesContextReadsTheMarker() {
    AVBufferRef* ref = fakeFramesContext(AV_PIX_FMT_D3D11, AV_PIX_FMT_BGRA, 1280, 960);
    auto* fc = reinterpret_cast<AVHWFramesContext*>(ref->data);
    QCOMPARE(VideoEncoder::gpuEncoderSpecOf(ref).encoder, QStringLiteral("h264_amf"));
    fc->user_opaque = const_cast<void*>(VideoEncoder::nvencD3d11Marker());
    QCOMPARE(VideoEncoder::gpuEncoderSpecOf(ref).encoder, QStringLiteral("h264_nvenc"));
    QVERIFY(VideoEncoder::gpuEncoderSpecOf(nullptr).encoder.isEmpty());
    av_buffer_unref(&ref);
  }

  void specMapsQsvFramesToQsv() {
    const auto spec = VideoEncoder::gpuEncoderSpec(AV_PIX_FMT_QSV, AV_PIX_FMT_NV12);
    QCOMPARE(spec.encoder, QStringLiteral("h264_qsv"));
    QCOMPARE(spec.framesLabel, QStringLiteral("QSV"));
    QCOMPARE(option(spec, "async_depth"), QStringLiteral("1"));
    QCOMPARE(option(spec, "bf"), QStringLiteral("0"));
    QCOMPARE(option(spec, "low_delay_brc"), QStringLiteral("1"));
    QCOMPARE(option(spec, "look_ahead"), QStringLiteral("0"));
    QCOMPARE(option(spec, "profile"), QStringLiteral("baseline"));
    QCOMPARE(spec.colorSpace, -1);
  }

  void specRejectsEveryOtherCombination() {
    const struct {
      AVPixelFormat hw, sw;
    } cases[] = {{AV_PIX_FMT_CUDA, AV_PIX_FMT_NV12},  {AV_PIX_FMT_CUDA, AV_PIX_FMT_BGRA}, {AV_PIX_FMT_D3D11, AV_PIX_FMT_NV12},
                 {AV_PIX_FMT_D3D11, AV_PIX_FMT_RGB0}, {AV_PIX_FMT_QSV, AV_PIX_FMT_BGRA},  {AV_PIX_FMT_QSV, AV_PIX_FMT_RGB0},
                 {AV_PIX_FMT_VAAPI, AV_PIX_FMT_NV12}, {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P}, {AV_PIX_FMT_NONE, AV_PIX_FMT_NONE}};
    for (const auto& c : cases) {
      QVERIFY2(VideoEncoder::gpuEncoderSpec(c.hw, c.sw).encoder.isEmpty(), qPrintable(QStringLiteral("%1/%2").arg(c.hw).arg(c.sw)));
    }
  }

  void openGpuGuardsTheNewFrameKinds() {
    VideoEncoder enc;
    QString why;
    struct Case {
      AVPixelFormat format, sw;
      int w, h;
      QSize visible;
      const char* what;
    };
    const Case cases[] = {
        {AV_PIX_FMT_D3D11, AV_PIX_FMT_BGRA, 255, 192, {}, "AMF odd width"},
        {AV_PIX_FMT_D3D11, AV_PIX_FMT_BGRA, 256, 8, {}, "AMF too low"},
        {AV_PIX_FMT_D3D11, AV_PIX_FMT_BGRA, 8194, 192, {}, "AMF too wide"},
        {AV_PIX_FMT_D3D11, AV_PIX_FMT_NV12, 256, 192, {}, "D3D11 NV12 is not an encoder input"},
        {AV_PIX_FMT_QSV, AV_PIX_FMT_BGRA, 256, 192, {}, "QSV BGRA"},
        {AV_PIX_FMT_QSV, AV_PIX_FMT_NV12, 288, 208, QSize(300, 200), "visible wider than the frames"},
        {AV_PIX_FMT_QSV, AV_PIX_FMT_NV12, 288, 208, QSize(281, 200), "visible odd"},
        {AV_PIX_FMT_QSV, AV_PIX_FMT_NV12, 288, 208, QSize(8, 200), "visible too small"},
    };
    for (const Case& c : cases) {
      AVBufferRef* ref = fakeFramesContext(c.format, c.sw, c.w, c.h);
      why.clear();
      QVERIFY2(!enc.openGpu(ref, 60, 2'000'000, &why, c.visible), c.what);
      QVERIFY2(!why.isEmpty(), c.what);
      QVERIFY2(!enc.isOpen(), c.what);
      av_buffer_unref(&ref);
    }
  }

  void gpuInputSupportQueriesAreStable() {
    // Whatever this FFmpeg has, the answers are cached and consistent.
    QCOMPARE(VideoEncoder::amfInputSupported(), VideoEncoder::amfInputSupported());
    QCOMPARE(VideoEncoder::qsvInputSupported(), VideoEncoder::qsvInputSupported());
    if (!VideoEncoder::availableEncoders().contains(QStringLiteral("h264_amf"))) {
      QVERIFY(!VideoEncoder::amfInputSupported());
    }
    if (!VideoEncoder::availableEncoders().contains(QStringLiteral("h264_qsv"))) {
      QVERIFY(!VideoEncoder::qsvInputSupported());
    }
  }

  void backendRulesPerEncoder() {
    using Facts = SessionHost::GpuInputFacts;
    Facts all;
    all.open = all.configured = true;
    all.cudaDriverUsable = all.cudaInput = all.nvencProbed = true;
    all.d3d11Capture = true;
    all.amfInput = all.amfProbed = true;
    all.qsvInput = all.qsvProbed = true;
    auto b = SessionHost::gpuBackendRules(all);
    QVERIFY(b.cuda && b.amf && b.qsv && b.any());

    // CUDA: today's conditions, unchanged and independent of the others.
    Facts f = all;
    f.cudaDriverUsable = false;
    b = SessionHost::gpuBackendRules(f);
    QVERIFY(!b.cuda && b.amf && b.qsv);
    f = all;
    f.cudaInput = false;
    QVERIFY(!SessionHost::gpuBackendRules(f).cuda);
    f = all;
    f.nvencProbed = false;
    QVERIFY(!SessionHost::gpuBackendRules(f).cuda);

    // AMF: needs h264_amf probed and D3D11 input; not CUDA, not QSV.
    f = all;
    f.amfProbed = false;
    b = SessionHost::gpuBackendRules(f);
    QVERIFY(b.cuda && !b.amf && b.qsv);
    f = all;
    f.amfInput = false;
    QVERIFY(!SessionHost::gpuBackendRules(f).amf);

    // QSV likewise.
    f = all;
    f.qsvProbed = false;
    b = SessionHost::gpuBackendRules(f);
    QVERIFY(b.cuda && b.amf && !b.qsv);
    f = all;
    f.qsvInput = false;
    QVERIFY(!SessionHost::gpuBackendRules(f).qsv);

    // No D3D11 capture on this platform (not Windows): only CUDA remains.
    f = all;
    f.d3d11Capture = false;
    b = SessionHost::gpuBackendRules(f);
    QVERIFY(b.cuda && !b.amf && !b.qsv);

    // The common conditions switch every backend off.
    for (int i = 0; i < 4; ++i) {
      f = all;
      switch (i) {
        case 0: f.open = false; break;         // no share
        case 1: f.off = true; break;           // failed or not applicable in this Session
        case 2: f.configured = false; break;   // UZUME_DISABLE_GPU_ENCODE=1 or a forced UZUME_H264_ENCODER
        case 3: f.orderPinned = true; break;   // tests pin encoders
      }
      b = SessionHost::gpuBackendRules(f);
      QVERIFY2(!b.any(), qPrintable(QString::number(i)));
    }
    QVERIFY(!SessionHost::gpuBackendRules(Facts{}).any());
  }

  void captureHelpersArePure() {
    using Kind = D3d11EncodeCapture::Kind;
    QCOMPARE(D3d11EncodeCapture::kindForVendor(0x1002), Kind::Amf);
    QCOMPARE(D3d11EncodeCapture::kindForVendor(0x8086), Kind::Qsv);
    QCOMPARE(D3d11EncodeCapture::kindForVendor(0x10DE), Kind::None);  // NVIDIA keeps CUDA for OpenGL games
    QCOMPARE(D3d11EncodeCapture::kindForVendor(0), Kind::None);
    QCOMPARE(D3d11EncodeCapture::kindForVendor(0x10DE, true), Kind::Nvenc);  // ... and takes D3D11 frames for Vulkan games
    QCOMPARE(D3d11EncodeCapture::kindForVendor(0x1002, true), Kind::Amf);
    QCOMPARE(D3d11EncodeCapture::kindForVendor(0x8086, true), Kind::Qsv);
    QCOMPARE(D3d11EncodeCapture::kindForVendor(0, true), Kind::None);
    QCOMPARE(D3d11EncodeCapture::alignedSize(1280, Kind::Qsv), 1280);
    QCOMPARE(D3d11EncodeCapture::alignedSize(1278, Kind::Qsv), 1280);
    QCOMPARE(D3d11EncodeCapture::alignedSize(1, Kind::Qsv), 16);
    QCOMPARE(D3d11EncodeCapture::alignedSize(1278, Kind::Amf), 1278);
  }

  // Slots stay declared on every platform: moc does not see _WIN32, so an #ifdef around slots breaks the Windows build.
  void captureIsUnavailableOffWindows() {
#ifdef _WIN32
    QSKIP("Windows has the real capture");
#else
    D3d11EncodeCapture capture;
    QCOMPARE(capture.attach(1, 256, 192), D3d11EncodeCapture::Status::Unavailable);
    QCOMPARE(capture.status(), D3d11EncodeCapture::Status::Unavailable);
    QCOMPARE(capture.reason(), QStringLiteral("not Windows"));
    QCOMPARE(capture.attach(1, 256, 192), D3d11EncodeCapture::Status::Unavailable);  // sticky
    QCOMPARE(capture.kind(), D3d11EncodeCapture::Kind::None);
    QVERIFY(!capture.capture());
    capture.detach(true);
#endif
  }

  void bridgeWithoutTargetStaysUnavailableOffWindows() {
#ifdef _WIN32
    QSKIP("Windows has the real bridge");
#else
    GlD3d11Bridge bridge;
    bridge.setAnyAdapter(true);
    QCOMPARE(bridge.open(), GlD3d11Bridge::Status::Unavailable);
    QCOMPARE(bridge.reason(), QStringLiteral("not Windows"));
    QVERIFY(!bridge.d3dDevice());
    QVERIFY(!bridge.gl());
#endif
  }
};

FB_TEST_MAIN(GpuBackendsTest)

#include "gpu_backends_test.moc"
