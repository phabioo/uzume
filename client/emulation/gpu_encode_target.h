#pragma once
// GpuEncodeTarget: consumer of the Session encode texture (ADR 0019). uzume_emulation only defines the interface;
// the implementation (CUDA-GL interop for h264_nvenc, D3D11 captures for AMF/QSV/NVENC) lives outside and is injected by
// the UI. OpenGL games use attach(); Vulkan games use attachExternal() (ADR 0034 D9).
#include <QSize>
#include <QString>

#include "gpu_display_target.h"  // ExternalImage, AdapterLuid

namespace uzume::emu {

class GpuEncodeTarget {
 public:
  // Unavailable: GPU-direct encoding is not applicable here (e.g. the GL context is not on an NVIDIA GPU); the target
  // already reports that itself, so the caller releases it quietly (no fail(), no warning). Failed: unusable, a failure.
  enum class Attach { Ok, NotReady, Unavailable, Failed };
  virtual ~GpuEncodeTarget() = default;
  // Any thread, cheap (atomics). false: nothing is blitted or captured.
  virtual bool wanted() const = 0;
  // Any thread, constant. The encode texture is the frame fitted into it (aspect kept, never upscaled, even).
  virtual QSize maxSize() const = 0;
  // Emulation thread, GL current. Registers `texture` (GL_TEXTURE_2D, sized GL_RGBA8, width x height, image top row =
  // GL row 0). NotReady: nothing registered, ask again on a later frame. Unavailable: not applicable here, the caller
  // releases it quietly. Failed: unusable; the caller drops it.
  virtual Attach attach(unsigned texture, int width, int height) = 0;
  // Vulkan games (ADR 0034 D9), emulation thread, no GL involved: the target creates (or keeps) a shared texture of exactly
  // w x h (even, B8G8R8A8, keyed mutex on Windows) on the adapter of the Vulkan device (`device`: its LUID and PCI vendor id
  // decide the encoder) and hands it out in `out` (bgra = true, slot 0). Called again when the size changes; the previous
  // texture is then superseded (new generation). The Vulkan side imports it, blits the frame into it (fitted, linear
  // filter) and calls capture() right after the submit completed. Ok / NotReady / Unavailable / Failed mean what they do for
  // attach(). Default: Unavailable (GL-only targets).
  struct VulkanDevice {
    AdapterLuid luid;      // VkPhysicalDeviceIDProperties::deviceLUID (valid == false: the driver reports none)
    unsigned vendorId = 0;  // VkPhysicalDeviceProperties::vendorID
  };
  virtual Attach attachExternal(int w, int h, const VulkanDevice& device, ExternalImage* out) {
    Q_UNUSED(w);
    Q_UNUSED(h);
    Q_UNUSED(device);
    Q_UNUSED(out);
    return Attach::Unavailable;
  }
  // Any thread. The adapter attachExternal()'s texture lives on (valid == false: unknown); the Vulkan side compares it
  // with its physical device's LUID.
  virtual AdapterLuid adapterLuid() const { return {}; }
  // Any thread. The Vulkan side found GPU-direct encoding not applicable here (missing extension, other adapter): reported
  // like an Unavailable attach. Default: fail().
  virtual void markUnavailable(const QString& reason) { fail(reason); }
  // Emulation thread. The core delivered a duplicate frame: publishes the last captured frame once more. false: nothing to
  // repeat (the OpenGL path repeats by capturing its texture again, HwRenderContext::encodeRepeat).
  virtual bool repeat() { return false; }
  // Emulation thread. Releases the registration; called before the texture is deleted and before the target is
  // dropped. Idempotent. glCurrent == false: the GL context is lost; the registration is leaked, never released
  // without GL.
  virtual void detach(bool glCurrent) = 0;
  // Emulation thread, GL current (Vulkan games: no GL; the blit into the shared texture has completed). Copies the texture
  // into a new frame and publishes it. Returns after the copy has finished on the GPU, so the texture may be overwritten
  // afterwards (Vulkan games: the keyed mutex is handed back). false: failed; the caller drops the target.
  virtual bool capture() = 0;
  // Any thread, idempotent, the first reason wins. The producer gave up (GL-side failure); consumers fall back.
  virtual void fail(const QString& reason) = 0;
};

}  // namespace uzume::emu
