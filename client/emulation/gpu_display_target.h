#pragma once
// GpuDisplayTarget: producer of the zero-copy game view (ADR 0027). uzume_emulation only defines the interface;
// the implementation (a platform display ring behind uzume::DisplayProducer) lives outside and is injected by the UI.
// Unlike GpuEncodeTarget the target owns the GL texture that receives the frame; HwRenderContext only attaches it to an
// FBO, blits into it and never deletes it.
#include <QSize>
#include <QString>
#include <QtGlobal>

namespace uzume::emu {

// Vulkan producers (ADR 0034 D8, D9): a texture the target created and owns, handed out as an external-memory handle that
// the Vulkan device imports (once per generation and slot, cached by the importer). RGBA8 (R8G8B8A8_UNORM) unless `bgra`,
// optimal tiling.
struct ExternalImage {
  enum class HandleType { D3D11TextureNt, OpaqueFd };
  HandleType handleType = HandleType::OpaqueFd;
  qintptr handle = 0;       // NT HANDLE or file descriptor; STILL OWNED by the target (the importer duplicates fds)
  QSize texSize;            // whole texture, >= the requested w x h
  quint64 memorySize = 0;   // OpaqueFd: allocation size of the exporting memory object (0 = unknown)
  quint64 generation = 0;   // textures of one generation never change identity; a new value means "drop what you imported"
  int slot = 0;             // 0..2 within the generation
  bool bgra = false;        // B8G8R8A8_UNORM instead of R8G8B8A8_UNORM (the Session encode texture, ADR 0034 D9)
  bool keyedMutex = false;  // D3D11: the submit itself acquires acquireKey and releases releaseKey (VK_KHR_win32_keyed_mutex)
  quint64 acquireKey = 0;
  quint64 releaseKey = 1;
};

// 8 bytes as VkPhysicalDeviceIDProperties::deviceLUID (Windows LUID in memory order: low part, then high part).
struct AdapterLuid {
  bool valid = false;
  quint8 bytes[8] = {};
};

class GpuDisplayTarget {
 public:
  // Ok: *glTexture (GL_TEXTURE_2D, RGBA8, *texSize >= w x h) is locked for GL until present()/abort(). Skip: no free
  // slot this frame, nothing to draw (not an error). NotReady: ask again on a later frame. Unavailable: not applicable
  // here, the target already reports that itself; the caller releases it quietly. Failed: unusable; the caller drops it.
  enum class Acquire { Ok, Skip, NotReady, Unavailable, Failed };
  virtual ~GpuDisplayTarget() = default;
  // Any thread, cheap (atomics). false: nothing is blitted.
  virtual bool wanted() const = 0;
  // Emulation thread, GL current. Image top row = GL row 0, drawn into the top-left w x h region of the texture.
  virtual Acquire acquire(int w, int h, unsigned* glTexture, QSize* texSize) = 0;
  // Vulkan producers, emulation thread (no GL involved): like acquire(), but hands out an external image to copy the
  // w x h frame into (top-left aligned, image top row = row 0, no flip). Ok: *out is locked until present()/abort().
  // The Skip/NotReady/Unavailable/Failed meanings are those of acquire(). Default: Unavailable (GL-only targets).
  virtual Acquire acquireExternal(int w, int h, ExternalImage* out) {
    Q_UNUSED(w);
    Q_UNUSED(h);
    Q_UNUSED(out);
    return Acquire::Unavailable;
  }
  // Any thread. The adapter the target's textures live on (valid == false: unknown or not applicable); the Vulkan side
  // compares it with its physical device's LUID.
  virtual AdapterLuid adapterLuid() const { return {}; }
  // Any thread. The Vulkan side found zero-copy display not applicable here (missing extension, other adapter): reported
  // like an Unavailable acquire. Default: fail().
  virtual void markUnavailable(const QString& reason) { fail(reason); }
  // After Ok and a complete blit (glFlush done): unlocks and publishes the w x h frame.
  virtual void present(int w, int h) = 0;
  // After Ok when the blit failed: unlocks without publishing.
  virtual void abort() = 0;
  // Emulation thread. Releases every GL registration; glCurrent == false: the context is lost, they are leaked.
  // Idempotent.
  virtual void release(bool glCurrent) = 0;
  // Any thread, idempotent, the first reason wins. The producer gave up (GL-side failure); consumers fall back.
  virtual void fail(const QString& reason) = 0;
};

}  // namespace uzume::emu
