#pragma once

#include <QList>
#include <QPair>
#include <QSize>
#include <QString>
#include <QStringList>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace uzume {

struct EncodedVideoPacket {
  std::vector<uint8_t> data;  // Annex B (start codes), SPS/PPS in-band before keyframes
  bool keyframe = false;
  int64_t pts = 0;            // in 1/fps ticks as passed to encode()
};

enum class RawPixelFormat { Xrgb8888, Rgb565 };  // libretro formats, native endian

// H.264 encoder over libavcodec: low latency, no B-frames, constant bitrate, GOP 2 s (ADR 0006 D5).
class VideoEncoder {
 public:
  VideoEncoder();
  ~VideoEncoder();
  VideoEncoder(const VideoEncoder&) = delete;
  VideoEncoder& operator=(const VideoEncoder&) = delete;

  // Preference per ADR 0006 D5.
  static QStringList preferredEncoders();
  // Of the default candidates (h264_nvenc, h264_qsv, h264_amf, libopenh264, libx264) the ones this FFmpeg build contains.
  // Present does not mean the hardware works; the encoder actually opened is reported by the Session (diagnostics).
  static QStringList availableEncoders();

  // ADR 0019: GPU-direct input is configured: UZUME_DISABLE_GPU_ENCODE != "1" and UZUME_H264_ENCODER unset (a
  // forced encoder, also h264_nvenc, keeps the readback path so both paths can be compared). *why: reason when false,
  // e.g. "UZUME_DISABLE_GPU_ENCODE=1" or "UZUME_H264_ENCODER=h264_nvenc forces the readback path".
  static bool gpuInputConfigured(QString* why = nullptr);
  // FFmpeg can feed h264_nvenc with CUDA frames: avcodec_get_hw_config lists AV_PIX_FMT_CUDA with
  // AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX, and av_hwdevice_iterate_types includes AV_HWDEVICE_TYPE_CUDA.
  static bool cudaInputSupported();
  // Likewise h264_amf with AV_PIX_FMT_D3D11 frames (FFmpeg built with D3D11VA) and h264_qsv with AV_PIX_FMT_QSV frames
  // (FFmpeg built with libvpl). Cached.
  static bool amfInputSupported();
  static bool qsvInputSupported();
  // Likewise h264_nvenc with AV_PIX_FMT_D3D11 frames (Vulkan games on Windows, ADR 0034 D9). Cached.
  static bool nvencD3d11InputSupported();
  // Set as AVHWFramesContext::user_opaque by the D3D11 capture of an NVIDIA GPU: BGRA D3D11 frames then go to h264_nvenc
  // instead of h264_amf (the two share a frames format; the frames context is the only thing the encoder is chosen by).
  static const void* nvencD3d11Marker();
  // `order` (empty = preferredEncoders()) without h264_nvenc when cudaDead; unchanged if that would leave it empty.
  static QStringList effectiveOrder(const QStringList& order, bool cudaDead);

  // Opens the first encoder of `order` (empty: preferredEncoders()) that works. False if none opens.
  bool open(int width, int height, int fps = 60, int bitrate = 2'000'000, const QStringList& order = {});
  // GPU-direct encoders by frames context (ADR 0019, ADR 0027 D6). Pure; `hwFormat`/`swFormat` are AVPixelFormat values.
  //   AV_PIX_FMT_CUDA / RGB0   -> h264_nvenc ("CUDA frames")
  //   AV_PIX_FMT_D3D11 / BGRA  -> h264_amf   ("D3D11 frames"), colorspace SMPTE170M, color_range MPEG
  //   AV_PIX_FMT_D3D11 / BGRA with `nvencD3d11` -> h264_nvenc ("D3D11 frames"), the CUDA path's low-latency options, same colours
  //   AV_PIX_FMT_QSV   / NV12  -> h264_qsv   ("QSV frames")
  // encoder is empty for any other combination.
  struct GpuEncoderSpec {
    QString encoder;
    QString framesLabel;                      // CUDA, D3D11 or QSV (log text)
    QList<QPair<QString, QString>> options;  // private AVOptions, applied in order
    int colorSpace = -1;                      // AVColorSpace of the codec context, -1 = leave unspecified
    int colorRange = -1;                      // AVColorRange, -1 = leave unspecified
  };
  static GpuEncoderSpec gpuEncoderSpec(int hwFormat, int swFormat, bool nvencD3d11 = false);
  // The same for a frames context (AVHWFramesContext in framesCtx->data): format, sw_format and the nvenc marker. Empty spec
  // for null or an unsupported context.
  static GpuEncoderSpec gpuEncoderSpecOf(const AVBufferRef* framesCtx);
  // Opens the encoder gpuEncoderSpec() names for the frames of `framesCtx` (even size 16..8192). No other encoder is
  // tried. `visible`: the picture size when it is smaller than the frames (QSV frames are 16-aligned; the encoder crops to
  // it), else the frames size. *why: the reason when false.
  bool openGpu(AVBufferRef* framesCtx, int fps, int bitrate, QString* why = nullptr, const QSize& visible = QSize());
  void close();
  bool isOpen() const { return ctx_ != nullptr; }
  QString name() const { return name_; }
  int width() const { return width_; }
  int height() const { return height_; }
  bool isGpuInput() const { return gpuInput_; }
  // ctx_->hw_frames_ctx->data while open in GPU mode, else nullptr.
  const void* gpuFramesKey() const;

  // Target bitrate in bit/s of the open encoder.
  int bitrate() const { return bitrate_; }
  // True if the encoder can change its bitrate while running (libx264: reconfigured by libavcodec on the next frame).
  bool supportsRuntimeBitrate() const { return name_ == QLatin1String("libx264"); }
  // Changes the bitrate without reopening; false (and nothing changed) if not supported or not open.
  bool setBitrate(int bitrate);

  // The next frame becomes an IDR frame (viewer joined, PLI). Thread-safe.
  void requestKeyframe() { forceKeyframe_ = true; }

  // Encodes one frame (size must match open()) and appends the finished packets to `out` (no B-frames: normally one).
  bool encode(const uint8_t* data, int stride, RawPixelFormat format, int64_t pts, std::vector<EncodedVideoPacket>& out);
  // Encodes one CUDA frame of the frames context passed to openGpu (checked); keyframe handling as in encode().
  bool encodeGpu(const AVFrame* frame, int64_t pts, std::vector<EncodedVideoPacket>& out);

 private:
  bool openWith(const QString& name, int width, int height, int fps, int bitrate);
  void markKeyframe(AVFrame* frame);
  bool drainPackets(std::vector<EncodedVideoPacket>& out);

  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  SwsContext* sws_ = nullptr;
  int swsFormat_ = -1;
  QString name_;
  int width_ = 0;
  int height_ = 0;
  int bitrate_ = 0;
  int swsSrcFormat_ = -1;
  bool gpuInput_ = false;
  std::atomic<bool> forceKeyframe_{true};  // may be set from other threads
};

}  // namespace uzume
