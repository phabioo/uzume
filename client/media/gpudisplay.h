#pragma once
// Zero-copy display (ADR 0027): platform-neutral producer/consumer interfaces. The Windows implementation is the D3D11
// display ring (d3d11displayring.h); other platforms have none and keep the readback path.
#include <QSize>
#include <QString>
#include <QtGlobal>

namespace uzume {

// A texture the producer owns, handed to a Vulkan device as an external-memory handle (acquireExternal). Mirrors
// emu::ExternalImage (uzume_media does not depend on uzume_emulation; GpuDisplayBridge converts).
struct ExternalDisplayImage {
  enum class HandleType { D3D11TextureNt, OpaqueFd };
  HandleType handleType = HandleType::D3D11TextureNt;
  qintptr handle = 0;      // NT HANDLE or fd; owned by the producer
  QSize texSize;           // whole texture, >= the requested w x h
  quint64 memorySize = 0;  // OpaqueFd only
  quint64 generation = 0;
  int slot = 0;
  bool keyedMutex = false;  // the importer acquires acquireKey and releases releaseKey itself (producer does no mutex calls)
  quint64 acquireKey = 0;
  quint64 releaseKey = 1;
};

// Producer side. Emulation thread only; acquire() with the game's GL context current, acquireExternal() for Vulkan games
// (no GL). The first of the two that is called fixes the mode of the producer for its lifetime.
class DisplayProducer {
 public:
  enum class Status { Ok, Skip, NotReady, Unavailable, Failed };
  virtual ~DisplayProducer() = default;
  // Hands out a GL_TEXTURE_2D (RGBA8, *texSize >= w x h) to write a w x h image into: top-left aligned, image top row =
  // GL row 0. Ok: the texture is locked for GL until present()/abort(). Skip: no free slot this frame (draw nothing,
  // not an error). NotReady: ask again on a later frame. Unavailable: not applicable here (reason()). Failed: unusable.
  virtual Status acquire(int w, int h, unsigned* glTexture, QSize* texSize) = 0;
  // Vulkan games: like acquire(), but hands out an external texture for a Vulkan device to copy the w x h image into
  // (top-left aligned, no flip). Same Status meanings. The Vulkan submit takes and gives the keyed mutex when keyedMutex.
  // Default: Unavailable (producers without Vulkan support).
  virtual Status acquireExternal(int w, int h, ExternalDisplayImage* out) {
    Q_UNUSED(w);
    Q_UNUSED(h);
    Q_UNUSED(out);
    return Status::Unavailable;
  }
  // Any thread. The adapter (8 bytes as VkPhysicalDeviceIDProperties::deviceLUID) the textures live on; valid == false:
  // unknown or not applicable.
  virtual bool adapterLuid(quint8 out[8]) const {
    Q_UNUSED(out);
    return false;
  }
  // After a successful acquire() and a complete blit (glFlush done): unlocks and publishes the w x h frame.
  virtual void present(int w, int h) = 0;
  // After a successful acquire() when the blit failed: unlocks without publishing.
  virtual void abort() = 0;
  // Releases every GL/interop registration (glCurrent == false: GL is gone, registrations are leaked). Idempotent.
  virtual void release(bool glCurrent) = 0;
  virtual QString reason() const = 0;       // any thread; why Unavailable/Failed
  virtual QString adapterName() const = 0;  // any thread; e.g. "AMD Radeon RX 7800 XT" once open, else empty
};

}  // namespace uzume
