#pragma once

#include <functional>
#include <string>
#include <vector>

// Seam between the plugin runtime and the emulation layer for uz.video (permission video.capture). The runtime never
// touches Qt Gui: the Player hands over raw RGBA pixels, the runtime crops and encodes the PNG itself.
namespace uzume::plugins {

// Where one emulated screen lies in the core's output frame, in source-frame pixels.
struct ScreenInfo {
  std::string name;  // "top" | "bottom" | "main"
  int x = 0, y = 0, w = 0, h = 0;
};

// One frame at the core's native resolution (before scaling and shaders, after the HW readback).
struct RawFrame {
  int w = 0, h = 0;
  std::string rgba;   // w * h * 4 bytes, R G B A; empty on error
  std::string error;  // empty = ok; otherwise "no_frame", "denied", "unsupported", ...
};

class VideoProvider {
 public:
  virtual ~VideoProvider() = default;
  // The screens of the running game in source-frame pixels; empty = unknown or no game. Any thread, cheap.
  virtual std::vector<ScreenInfo> screens() const = 0;
  // Asks for the next native-resolution frame. Non-blocking, any thread. `done` is called exactly once, on any thread.
  virtual void captureFrame(std::function<void(RawFrame)> done) = 0;
};

}  // namespace uzume::plugins
