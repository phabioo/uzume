#pragma once
// Zero-copy display ring (ADR 0027, design D4): the emulation thread renders the game into D3D11 textures through the
// GL-D3D11 bridge (WGL_NV_DX_interop2); Qt Quick's render thread draws those textures through its own D3D11 device.
// Windows only: other platforms compile every method; the producer reports Unavailable("not Windows").
// No Qt Quick headers, no windows.h/d3d11.h: COM objects cross the API as void*.
//
// Vulkan mode (ADR 0034 D8): a producer whose first call is acquireExternal() runs without GL and without the bridge: it
// creates the slots on an own D3D11 device opened on Qt Quick's adapter and hands out their NT handles; the Vulkan submit
// (VK_KHR_win32_keyed_mutex) does AcquireSync(0) before and ReleaseSync(1) after the copy, so the consumer protocol is the
// same. The producer itself makes no keyed-mutex call in that mode; present() only publishes.
//
// Protocol (3 slots per generation, DisplaySlots in displayslots.h is the pure state machine):
//  - Each slot is a shared R8G8B8A8_UNORM texture (NT handle + keyed mutex) on the bridge's device, registered with
//    GL (WRITE_DISCARD). Textures only grow (sizes rounded up to 64); growing makes a new generation (3 new slots).
//  - Producer: AcquireSync(0, 0) -> wglDXLockObjectsNV -> GL blit -> glFlush -> wglDXUnlockObjectsNV ->
//    ReleaseSync(1) -> publish (newest wins). No free slot or a busy mutex skips the frame ("display frames skipped").
//  - Consumer: AcquireSync(1, 0) on the newest published slot; superseded pending slots are acquired and released to
//    key 0 right away; the previously shown slot is released to key 0 in frameEnd(), after Qt executed the frame that
//    no longer uses it. A timeout keeps the old slot on screen.
//
// Thread rules:
//  - D3d11DisplayProducer (DisplayProducer): acquire/present/abort/release on the emulation thread with the game's GL
//    current; reason()/adapterName()/skippedFrames() on any thread.
//  - D3d11DisplayConsumer: frame()/frameEnd()/close() on Qt Quick's render thread ONLY, the textures it hands out
//    live on Qt's device. frame() is idempotent until frameEnd() (several GameViews of one window ask in one frame).
//    failed()/failureReason() on any thread.
//  - Producer and consumer share state and a failed flag: a failure on either side makes the other report Failed with
//    the reason. The consumer may outlive the producer; the NT handles close when both sides dropped a generation.
#include <QSize>
#include <QString>
#include <QtGlobal>
#include <memory>

#include "d3d11glbridge.h"
#include "gpudisplay.h"

namespace uzume {

struct D3d11RingShared;

struct ConsumerFrame {
  void* texture = nullptr;  // ID3D11Texture2D* on Qt's device (opened with OpenSharedResource1); null: nothing yet
  QSize textureSize;        // the whole texture (grow-only, multiple of 64)
  QSize contentSize;        // the published image, top-left aligned
  quint64 generation = 0;   // textures of one generation never change identity; a new one means "drop old wrappers"
  quint64 sequence = 0;     // publish counter of the frame shown
  int slot = -1;            // 0..2 within the generation
};

class D3d11DisplayProducer final : public DisplayProducer {
 public:
  D3d11DisplayProducer(std::shared_ptr<D3d11RingShared> shared, void* testDevice);
  ~D3d11DisplayProducer() override;
  Status acquire(int w, int h, unsigned* glTexture, QSize* texSize) override;
  Status acquireExternal(int w, int h, ExternalDisplayImage* out) override;
  bool adapterLuid(quint8 out[8]) const override;
  void present(int w, int h) override;
  void abort() override;
  void release(bool glCurrent) override;
  QString reason() const override;
  QString adapterName() const override;

  quint64 skippedFrames() const;  // any thread: "display frames skipped" (no free slot or busy keyed mutex)
  // Test seam (makeD3d11DisplayForTest only): the ID3D11Texture2D* of the slot between acquire() and present(), on the
  // producer's device. In that mode no GL and no interop is involved and acquire() hands out GL name 0.
  void* testTexture() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

class D3d11DisplayConsumer {
 public:
  explicit D3d11DisplayConsumer(std::shared_ptr<D3d11RingShared> shared);
  ~D3d11DisplayConsumer();
  D3d11DisplayConsumer(const D3d11DisplayConsumer&) = delete;
  D3d11DisplayConsumer& operator=(const D3d11DisplayConsumer&) = delete;

  // Render thread. true: *out is valid (out->texture null while nothing is published; draw the CPU frame then).
  // false: the ring failed for good, *error says why. `qtD3d11Device` is Qt's ID3D11Device*.
  bool frame(void* qtD3d11Device, ConsumerFrame* out, QString* error);
  // Render thread, QQuickWindow::afterFrameEnd: releases the slot the last frame stopped using.
  void frameEnd();
  // Render thread: releases everything (the shown slot goes back with key 0). Idempotent.
  void close();
  // Any thread. Qt Quick's adapter (DXGI LUID: high << 32 | low) and "Qt Quick does not render with D3D11": forwarded to
  // the GL-D3D11 bridge (GL mode) and kept in the shared state, where the Vulkan mode of the producer waits for it.
  void setTargetAdapter(quint64 luid);
  void setUnavailable(const QString& reason);
  bool failed() const;
  QString failureReason() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

struct D3d11Display {
  std::unique_ptr<DisplayProducer> producer;  // emulation thread (really a D3d11DisplayProducer)
  std::shared_ptr<D3d11DisplayConsumer> consumer;  // render thread
};

// One ring for one game: the producer uses the bridge's device and interop; nothing happens until acquire().
D3d11Display makeD3d11Display(std::shared_ptr<GlD3d11Bridge> bridge);
// Test seam: the producer creates its textures on `producerD3dDevice` (an ID3D11Device*) and skips GL/interop.
D3d11Display makeD3d11DisplayForTest(void* producerD3dDevice);

}  // namespace uzume
