#pragma once

#include <QString>
#include <QtGlobal>
#include <optional>

#include "connectionpath.h"
#include "qualitytier.h"

namespace uzume {

// Diagnostics of one side of a Session (ADR 0006 D6). Plain struct; "unavailable" values are std::nullopt / empty.
// Link state of one viewer connection (host side per viewer, viewer side its single link).
struct ViewerLinkStats {
  QString viewerId;
  QString state;  // new | connecting | connected | disconnected | failed | closed
  std::optional<double> rttMs;
  QString connectionType;  // "direct (host|srflx|prflx)" | "relay (udp|tcp)" | empty (unknown), see rtcutil.h
  RelayReason relayReason = RelayReason::None;  // why the path is relayed (None when direct or unknown), see connectionpath.h
  // Host side: the latest rx report this viewer sent over fb-diag (ADR 0012 D5, 0.6 D7). hasReport = false until the
  // first one arrives; fps and decoder stay unset for Players that do not send them yet.
  bool hasReport = false;
  double reportLoss = 0.0;                // 0..1
  double reportKbps = 0.0;                // video kbit/s the viewer received
  std::optional<double> reportFps;        // frames/s the viewer decoded
  QString reportDecoder;                  // decoder the viewer uses, e.g. "h264"
  // Host side, ADR 0033 D5: the quality the viewer picked, the tier it is served by and its bitrate (Auto: the adaptive
  // target), and the latency measured with the fb-diag ping/pong (nullopt until the first pong).
  Quality pick = Quality::Auto;
  Quality tier = Quality::Auto;
  int tierKbps = 0;
  std::optional<double> latencyMs;
};

struct SessionStats {
  bool active = false;           // media path exists (host: encoder running; viewer: connected)
  double fps = 0.0;              // host: encoded frames/s, viewer: decoded frames/s
  int width = 0;
  int height = 0;
  QString encoderName;           // host only, e.g. "libx264"
  QString decoderName;           // viewer only: libavcodec decoder in use, e.g. "h264" (never a name that is not in use)
  QString codec;                 // "H264 + Opus"
  double videoBitrateKbps = 0.0; // measured over the last second(s), payload
  double audioBitrateKbps = 0.0;
  std::optional<double> rttMs;                // nullopt: unavailable
  QString connectionType;        // selected candidate pair: "direct (host)" | "direct (srflx)" | "direct (prflx)" | "relay (udp)" |
                                 // "relay (tcp)" | empty (unknown). Host: the worst of its viewers (relay > srflx/prflx > host).
  RelayReason relayReason = RelayReason::None;  // viewer: reason for a relayed path; host: see ViewerLinkStats per viewer
  double targetBitrateKbps = 0.0; // host: current AIMD target of the encoder (ADR 0012 D5), 0 while the encoder is off
  std::optional<double> packetLossPercent;    // viewer: from RTP sequence numbers; host: unavailable
  int viewers = 0;               // host: connected viewers
  qint64 videoFrames = 0;        // total frames encoded (host) / decoded (viewer)
  qint64 audioFrames = 0;        // total 20 ms Opus frames sent / decoded
  int decodeErrors = 0;          // viewer
  qint64 droppedFrames = 0;      // host: video frames dropped because the encoder fell behind
  int keyframeRequests = 0;      // host: PLI received, viewer: PLI sent
  bool gpuInput = false;         // host: the encoder takes CUDA frames straight from the GPU (GPU-direct, ADR 0019)
  bool gpuInputFailed = false;   // host: GPU-direct was tried in this Session and failed; readback frames again
  int encoders = 0;              // host: encoders sending (Auto = adaptive + one per fixed quality tier with viewers, ADR 0033 D5)
  QString tiersInUse;            // host: e.g. "Auto, Low" (diagnostics)
  // Viewer: the tier the host serves this viewer ("tier" message), the viewer's own pick and the latency from the ping/pong.
  Quality servedTier = Quality::Auto;
  bool hasServedTier = false;
  int servedKbps = 0;
  Quality pick = Quality::Auto;
  std::optional<double> latencyMs;
};

}  // namespace uzume
