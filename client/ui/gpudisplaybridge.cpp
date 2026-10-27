#include "gpudisplaybridge.h"

#include <QLoggingCategory>

#include <utility>

namespace uzume::ui {

namespace {
Q_LOGGING_CATEGORY(lcDisplay, "uzume.gpudisplay")
}

ReadbackPlan planReadback(bool displayActive, const QSize& viewLimit, const QSize& shareSize) {
  ReadbackPlan p;
  p.displayLimit = viewLimit;
  if (!displayActive) {
    p.readback = true;
    p.readbackLimit = viewLimit.isEmpty() ? QSize() : viewLimit.expandedTo(shareSize);
  } else if (!shareSize.isEmpty()) {
    p.readback = true;
    p.readbackLimit = shareSize;
  } else {
    p.readback = false;
  }
  return p;
}

GpuDisplayBridge::GpuDisplayBridge(PlatformDisplay display)
    : shared_(std::move(display.shared)), producer_(std::move(display.producer)), source_(std::move(display.source)) {
  if (!producer_) state_.store(State::Unavailable);
}

GpuDisplayBridge::~GpuDisplayBridge() = default;

void GpuDisplayBridge::setStateCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

GpuDisplayBridge::State GpuDisplayBridge::state() const { return state_.load(); }

QString GpuDisplayBridge::reason() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return reason_;
}

DisplayTextureSource* GpuDisplayBridge::source() {
  if (source_ && source_->failed()) transition(State::Failed, QStringLiteral("the texture source failed"));
  const State s = state_.load();
  return (s == State::Pending || s == State::Active) ? source_.get() : nullptr;
}

void GpuDisplayBridge::confirmShown() {
  State expected = State::Pending;
  if (!state_.compare_exchange_strong(expected, State::Active)) return;
  qCInfo(lcDisplay).noquote() << QStringLiteral("Zero-copy display active: %1D3D11 textures on \"%2\" (no readback for the game view)")
                                     .arg(external_.load() ? QStringLiteral("Vulkan \u2192 ") : QString(),
                                          producer_ ? producer_->adapterName() : QString());
  notify();
}

bool GpuDisplayBridge::wanted() const {
  const State s = state_.load();
  if (s != State::Pending && s != State::Active) return false;
  if (source_ && source_->failed()) {  // the render side gave up (it told the producer itself): stop for this game
    transition(State::Failed, QStringLiteral("the texture source failed"));
    return false;
  }
  return true;
}

emu::GpuDisplayTarget::Acquire GpuDisplayBridge::acquire(int w, int h, unsigned* glTexture, QSize* texSize) {
  const State s = state_.load();
  if (!producer_ || (s != State::Pending && s != State::Active)) return Acquire::Skip;
  switch (producer_->acquire(w, h, glTexture, texSize)) {
    case DisplayProducer::Status::Ok:
      return Acquire::Ok;
    case DisplayProducer::Status::Skip:
      return Acquire::Skip;
    case DisplayProducer::Status::NotReady:
      return Acquire::NotReady;
    case DisplayProducer::Status::Unavailable:
      transition(State::Unavailable, producer_->reason());
      return Acquire::Unavailable;
    case DisplayProducer::Status::Failed: {
      const QString why = producer_->reason();
      transition(State::Failed, why.isEmpty() ? QStringLiteral("acquire failed") : why);
      return Acquire::Failed;
    }
  }
  return Acquire::Failed;
}

emu::GpuDisplayTarget::Acquire GpuDisplayBridge::acquireExternal(int w, int h, emu::ExternalImage* out) {
  const State s = state_.load();
  if (!producer_ || (s != State::Pending && s != State::Active)) return Acquire::Skip;
  ExternalDisplayImage img;
  switch (producer_->acquireExternal(w, h, &img)) {
    case DisplayProducer::Status::Ok:
      external_.store(true);
      out->handleType = img.handleType == ExternalDisplayImage::HandleType::OpaqueFd ? emu::ExternalImage::HandleType::OpaqueFd
                                                                                     : emu::ExternalImage::HandleType::D3D11TextureNt;
      out->handle = img.handle;
      out->texSize = img.texSize;
      out->memorySize = img.memorySize;
      out->generation = img.generation;
      out->slot = img.slot;
      out->keyedMutex = img.keyedMutex;
      out->acquireKey = img.acquireKey;
      out->releaseKey = img.releaseKey;
      return Acquire::Ok;
    case DisplayProducer::Status::Skip:
      return Acquire::Skip;
    case DisplayProducer::Status::NotReady:
      return Acquire::NotReady;
    case DisplayProducer::Status::Unavailable:
      transition(State::Unavailable, producer_->reason());
      return Acquire::Unavailable;
    case DisplayProducer::Status::Failed: {
      const QString why = producer_->reason();
      transition(State::Failed, why.isEmpty() ? QStringLiteral("acquire failed") : why);
      return Acquire::Failed;
    }
  }
  return Acquire::Failed;
}

emu::AdapterLuid GpuDisplayBridge::adapterLuid() const {
  emu::AdapterLuid l;
  if (producer_) l.valid = producer_->adapterLuid(l.bytes);
  return l;
}

// A display that already showed frames cannot go back to "not applicable": that is a failure for this game.
void GpuDisplayBridge::markUnavailable(const QString& reason) {
  transition(state_.load() == State::Active ? State::Failed : State::Unavailable, reason);
}

void GpuDisplayBridge::present(int w, int h) {
  if (producer_) producer_->present(w, h);
}

void GpuDisplayBridge::abort() {
  if (producer_) producer_->abort();
}

void GpuDisplayBridge::release(bool glCurrent) {
  if (producer_) producer_->release(glCurrent);
}

void GpuDisplayBridge::fail(const QString& reason) { transition(State::Failed, reason); }

void GpuDisplayBridge::transition(State to, const QString& why) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const State cur = state_.load();
    if (cur != State::Pending && cur != State::Active) return;  // the first reason wins; Unavailable stays quiet
    if (to == State::Unavailable && cur != State::Pending) return;
    reason_ = why;
    state_.store(to);
  }
  if (to == State::Failed) {
    qCInfo(lcDisplay).noquote() << QStringLiteral("Zero-copy display off for this game (%1); the game view uses readback frames").arg(why);
  } else if (to == State::Unavailable) {
    qCInfo(lcDisplay).noquote() << QStringLiteral("Zero-copy display not available (%1); the game view uses readback frames").arg(why);
  }
  notify();
}

void GpuDisplayBridge::notify() const {
  std::function<void()> cb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cb = callback_;
  }
  if (cb) cb();
}

}  // namespace uzume::ui
