#pragma once
// Zero-copy display (ADR 0027): the adapter between the emulation thread (emu::GpuDisplayTarget), the platform's display
// ring (uzume::DisplayProducer / DisplayTextureSource) and GameSession. uzume_emulation only knows the emu
// interface; the platform implementation (Windows: D3D11 ring) comes out of makePlatformDisplay().

#include <QSize>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "displaytexturesource.h"
#include "gpu_display_target.h"
#include "gpudisplay.h"

namespace uzume::ui {

struct PlatformDisplay {
  std::unique_ptr<uzume::DisplayProducer> producer;  // emulation thread
  std::unique_ptr<DisplayTextureSource> source;          // render thread
  std::shared_ptr<void> shared;                          // keeps the platform objects (the GL-D3D11 bridge) alive
};
// GUI thread, cheap (no GPU work). Empty producer = not available; *reason says why (e.g. "not Windows",
// "UZUME_DISABLE_ZERO_COPY=1").
PlatformDisplay makePlatformDisplay(QString* reason);

// What the game view's pixels need (D5). Pure; unit-tested as a truth table.
//   displayActive = the GPU frame has been drawn (State::Active), not merely tried.
//   viewLimit     = component-wise maximum of the sizes the views report (empty = nobody reported)
//   shareSize     = the size the own Session needs from the CPU readback (empty = nobody or GPU-direct encoding)
struct ReadbackPlan {
  bool readback = true;   // hardware frames are read back into the game's QImage
  QSize readbackLimit;    // GPU downscale limit of that readback (empty = none)
  QSize displayLimit;     // size limit of the display blit (empty = none)
};
//   !displayActive                     -> readback, limit = viewLimit (expanded to shareSize when a view exists)
//   displayActive && shareSize !empty  -> readback, limit = shareSize
//   displayActive && shareSize empty   -> no readback
//   displayLimit = viewLimit, always
ReadbackPlan planReadback(bool displayActive, const QSize& viewLimit, const QSize& shareSize);

// emu::GpuDisplayTarget on top of a DisplayProducer, plus the DisplayTextureSource for the GameViews and the state.
//   Pending     trying: blits run, the game view still draws the readback image
//   Active      a GameView has drawn a GPU frame (confirmShown): the readback may stop
//   Unavailable not applicable here (the producer says why); quiet
//   Failed      the producer or the source gave up; not retried within the game
// Log lines (category uzume.gpudisplay) once each, see the .cpp.
class GpuDisplayBridge final : public emu::GpuDisplayTarget {
 public:
  enum class State { Pending, Active, Unavailable, Failed };
  explicit GpuDisplayBridge(PlatformDisplay display);
  ~GpuDisplayBridge() override;  // any thread

  // Called (from any thread, outside every lock) whenever state() changes. Set before the bridge is handed out.
  void setStateCallback(std::function<void()> callback);

  State state() const;       // atomic
  QString reason() const;    // mutex; set together with Unavailable/Failed

  // Render thread. The source for the GameViews; a failed source turns the state to Failed first. Null unless the state
  // is Pending or Active.
  DisplayTextureSource* source();
  // Render thread: a GameView drew a GPU texture. Pending -> Active, once.
  void confirmShown();

  // emu::GpuDisplayTarget
  bool wanted() const override;  // state Pending or Active, and the source has not failed
  Acquire acquire(int w, int h, unsigned* glTexture, QSize* texSize) override;
  Acquire acquireExternal(int w, int h, emu::ExternalImage* out) override;  // Vulkan games (ADR 0034 D8)
  emu::AdapterLuid adapterLuid() const override;
  void markUnavailable(const QString& reason) override;  // Pending -> Unavailable (quiet)
  void present(int w, int h) override;
  void abort() override;
  void release(bool glCurrent) override;
  void fail(const QString& reason) override;  // Pending/Active -> Failed, first reason wins

 private:
  // Pending/Active -> `to` (Active only via confirmShown); a no-op in any terminal state.
  // const: wanted() (emulation thread) may notice a failed source; the state is atomic and the rest is under the mutex.
  void transition(State to, const QString& reason) const;
  void notify() const;

  std::shared_ptr<void> shared_;  // destroyed last: the producer and the source may use the objects behind it
  std::unique_ptr<uzume::DisplayProducer> producer_;
  std::unique_ptr<DisplayTextureSource> source_;
  mutable std::atomic<State> state_{State::Pending};
  std::atomic<bool> external_{false};  // the frames come from Vulkan (acquireExternal handed out a texture)
  mutable std::mutex mutex_;  // guards reason_ and callback_
  mutable QString reason_;
  mutable std::function<void()> callback_;
};

}  // namespace uzume::ui
