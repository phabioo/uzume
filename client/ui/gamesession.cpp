#include "gamesession.h"

#include <algorithm>
#include <QDir>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QSaveFile>
#include <QThread>
#include <QTimer>

#include "screenlayout.h"

#include "hw_render.h"
#include "libretro_backend.h"

namespace uzume::ui {

using emu::EmulationRunner;

namespace {
Q_LOGGING_CATEGORY(lcDisplayHost, "uzume.gpudisplay")
std::function<PlatformDisplay(QString*)>& platformDisplayFactory() {
  static std::function<PlatformDisplay(QString*)> f;
  return f;
}
}  // namespace

void GameSession::setPlatformDisplayFactoryForTests(std::function<PlatformDisplay(QString*)> factory) {
  platformDisplayFactory() = std::move(factory);
}

GameSession::GameSession(QObject* parent) : QObject(parent) {
  qRegisterMetaType<emu::AvInfo>();
  qRegisterMetaType<emu::CoreInfo>();
  qRegisterMetaType<EmulationRunner::State>();
}

GameSession::~GameSession() {
  if (gpuDisplay_) gpuDisplay_->setStateCallback({});
  teardown();
}

void GameSession::setState(State s) {
  if (state_ == s) {
    return;
  }
  state_ = s;
  emit stateChanged();
}

bool GameSession::previousGameHung() { return emu::LibretroBackend::previousCoreHung(); }

void GameSession::fail(const QString& rawMsg) {
  QString msg = rawMsg;
  errorCode_.clear();
  if (rawMsg == emu::LibretroBackend::kPreviousGameHungCode) {
    errorCode_ = rawMsg;
    msg = tr("The last game did not stop. Restart Uzume to play again.");
  }
  error_ = msg;
  restarting_ = false;
  const bool before = !startedEmitted_;
  teardown();
  reportStopSaveError();
  setState(Failed);
  emit errorChanged();
  if (before) {
    emit startFailed(msg);
  }
}

void GameSession::start(const LaunchConfig& config) { launch(config, false); }

void GameSession::launch(const LaunchConfig& config, bool restart) {
  teardown();
  config_ = config;
  restarting_ = restart;
  setState(Starting);
  error_.clear();
  errorCode_.clear();
  emit errorChanged();
  if (!restart) {  // a restart keeps the last picture and the title: the game screen stays as it is
    frame_ = QImage();
    frameNr_ = 0;
    emit frameChanged();
    title_ = config.title;
    emit titleChanged();
  }
  startedEmitted_ = restart;  // a failure after a restart is a runtime error of a game that was already running
  display_ = config.display;
  coreOptions_ = config.coreOptions;
  coreName_.clear();
  targetFps_ = 0;
  preview_ = false;
  audio_.setUnderrunCounting(true);
  keys_.clear();
  fastForward_ = false;
  fastForwardSupported_ = false;
  ffRatio_ = emu::EmulationRunner::normalizeSpeedUpRatio(config.speedUpRatio);
  ffAudio_ = config.speedUpAudio;
  emit fastForwardChanged();

  runner_ = std::make_unique<EmulationRunner>(std::make_unique<emu::LibretroBackend>());
  setupDisplay(restart);
  updateReadbackLimit();  // readback limit/enable and display limit for the new runner
  if (restart) emit memoryWatchReplaced();
  if (gpuTarget_) {
    runner_->setGpuEncodeTarget(gpuTarget_);  // applied by the emulation thread at its first frame
  }
  if (gpuDisplay_) {
    runner_->setGpuDisplayTarget(gpuDisplay_);
  }
  runner_->setRewindSeconds(rewindSeconds_);
  runner_->setRunAhead(runAhead_);
  rewinding_ = false;
  EmulationRunner* r = runner_.get();
  // Events the emulation thread already posted survive disconnect(): each handler checks that its runner is still the current one.
  const quint64 gen = ++runnerGen_;
  connect(r, &EmulationRunner::started, this, [this, gen](const emu::AvInfo& av, const emu::CoreInfo& core) {
    if (gen != runnerGen_) return;
    coreName_ = core.version.isEmpty() ? core.name : core.name + QLatin1Char(' ') + core.version;
    targetFps_ = av.fps;
    fastForwardSupported_ = runner_ && runner_->supportsFastForward();
    fastForward_ = fastForwardSupported_ && runner_->fastForward();  // speed-up on start
    updateUnderrunCounting();
    emit fastForwardChanged();
    if (!audio_.start(static_cast<int>(av.sampleRate + 0.5))) {
      qCWarning(lcAudio) << "Audio output not available; session runs without sound";
    }
    startedEmitted_ = true;
    setState(Running);
    if (restarting_) {
      restarting_ = false;  // same game, same Session: no new "started" (save session, Session share stay as they are)
    } else {
      emit started();
    }
  });
  connect(r, &EmulationRunner::rewindProgress, this, [this, gen](double left) {
    if (gen == runnerGen_) emit rewindProgress(left);
  });
  connect(r, &EmulationRunner::rewindFinished, this, [this, gen](bool exhausted) {
    if (gen == runnerGen_) emit rewindFinished(exhausted);
  });
  connect(r, &EmulationRunner::memoryRegionsChanged, this, [this, gen]() {
    if (gen == runnerGen_) emit memoryRegionsChanged();
  });
  connect(r, &EmulationRunner::saveProblem, this, [this, gen](const QString& message) {
    if (gen != runnerGen_) return;
    qWarning().noquote() << "Save problem at start:" << message;
    emit saveProblem(message);
  });
  connect(r, &EmulationRunner::startFailed, this, [this, gen](const QString& e) {
    if (gen == runnerGen_) fail(e);
  });
  connect(r, &EmulationRunner::errorOccurred, this, [this, gen](const QString& e) {
    if (gen == runnerGen_) fail(e);
  });
  connect(r, &EmulationRunner::frameReady, this, [this, gen](const QImage& img, quint64 nr, qint64 lateNs) {
    if (gen != runnerGen_) return;
    frame_ = img;
    frameNr_ = nr;
    frameLateNs_ = lateNs;
    emit frameChanged();
    if (!captureWaiters_.isEmpty()) serveCaptures(false);
  });
  connect(r, &EmulationRunner::audioReady, this, [this, gen](const QByteArray& pcm, int rate) {
    if (gen != runnerGen_) return;
    if (!audioMuted_) {
      audio_.setCoreRate(rate);  // PE-10: follows a runtime rate change
      audio_.push(pcm);
    }
    emit audioChunk(pcm, rate);
  });
  connect(r, &EmulationRunner::stateChanged, this, [this, gen](EmulationRunner::State s) {
    if (gen != runnerGen_ || state_ == Failed || state_ == Idle) {
      return;
    }
    if (s == EmulationRunner::State::Paused) {
      setState(Paused);
    } else if (s == EmulationRunner::State::Running && startedEmitted_ && !restarting_) {
      setState(Running);
    }
  });

  QDir().mkpath(config.systemDir);
  QDir().mkpath(config.saveDir);
  EmulationRunner::StartRequest req;
  req.corePath = config.corePath;
  req.gamePath = config.gamePath;
  req.systemDir = config.systemDir;
  req.saveDir = config.saveDir;
  req.coreOptions = config.coreOptions;
  req.requireHwRender = config.requireHwRender;
  req.preferredRenderer = config.preferredRenderer;
  if (config.preferredRenderer.trimmed().toLower() == QLatin1String("vulkan")) req.openglFallbackOptions = config.openglFallbackOptions;
  req.speedUpRatio = config.speedUpRatio;
  req.speedUpOnStart = config.speedUpOnStart;
  req.speedUpAudio = config.speedUpAudio;
  r->start(req);
  applyJoypad();  // a gamepad button that is already held counts from the first frame
}

void GameSession::teardown() {
  debugPad_ = 0;
  remoteControlled_[0] = remoteControlled_[1] = false;
  remoteMask_[0] = remoteMask_[1] = 0;  // a debug button press never outlives the game
  ++runnerGen_;  // queued events of the runner that is stopped now must not reach the session any more
  if (!captureWaiters_.isEmpty()) {  // plugin captures waiting for a frame fail with the game
    auto waiters = std::move(captureWaiters_);
    captureWaiters_.clear();
    for (auto& w : waiters) w(QImage(), QStringLiteral("no_frame"));
  }
  if (runner_) {
    runner_->disconnect(this);
    runner_->stop();  // unloading the game detaches the GPU encode target (GL current); gpuTarget_ stays for a restart
    stopSaveError_ = runner_->finalSaveError();
    runner_.reset();
  }
  audio_.stop();
  keys_.clear();
  preview_ = false;
  if (fastForward_ || fastForwardSupported_) {
    fastForward_ = false;
    fastForwardSupported_ = false;
    emit fastForwardChanged();
  }
}

bool GameSession::runOnEmuThread(std::function<void(emu::EmulatorBackend&)> fn, int timeoutMs) {
  return runner_ && runner_->runOnEmuThread(std::move(fn), timeoutMs);
}

quint64 GameSession::submitEmuCommand(emu::EmulationRunner::Command cmd, std::function<void(const emu::EmulationRunner::CommandResult&)> onDone) {
  return runner_ ? runner_->submit(std::move(cmd), std::move(onDone)) : 0;
}

qint64 GameSession::stepFrames(int frames) {
  return runner_ && state_ == Paused ? runner_->stepFrames(frames) : -1;
}

void GameSession::pause() {
  if (preview_ && !runner_ && state_ == Running) {
    setState(Paused);  // preview game (tests, screenshots) has no core, the state alone follows
    return;
  }
  if (runner_ && state_ == Running) {
    runner_->pause();
    audio_.setUnderrunCounting(false);
  }
}

void GameSession::resume() {
  if (preview_ && !runner_ && state_ == Paused) {
    setState(Running);
    return;
  }
  if (runner_ && state_ == Paused) {
    runner_->resume();
    audio_.setUnderrunCounting(!audioMuted_ && !(fastForward_ && !ffAudio_));
  }
}

// Nothing is pushed on purpose while paused, muted or fast-forwarding: the sink running dry is not an underrun.
void GameSession::updateUnderrunCounting() {
  audio_.setUnderrunCounting(!audioMuted_ && state_ != Paused && !(fastForward_ && !ffAudio_));
}

QVariantList GameSession::speedUpRatios() {
  QVariantList l;
  for (const double r : emu::EmulationRunner::speedUpRatios()) l.append(r);
  return l;
}

void GameSession::setRewindSeconds(int seconds) {
  rewindSeconds_ = (std::max)(0, seconds);
  if (runner_) runner_->setRewindSeconds(rewindSeconds_);
}

void GameSession::setRunAhead(int frames) {
  runAhead_ = (std::clamp)(frames, 0, 2);
  if (runner_) runner_->setRunAhead(runAhead_);
}

void GameSession::setRewinding(bool on) {
  rewinding_ = on;
  if (runner_) runner_->setRewinding(on);
}

void GameSession::setFastForwardRatio(double ratio) {
  ratio = emu::EmulationRunner::normalizeSpeedUpRatio(ratio);
  if (ratio == ffRatio_) return;
  ffRatio_ = ratio;
  if (runner_) runner_->setFastForwardRatio(ratio);
  emit fastForwardChanged();
}

void GameSession::setFastForward(bool on) {
  on = on && fastForwardSupported_;
  if (on == fastForward_) return;
  fastForward_ = on;
  if (runner_) runner_->setFastForward(on);
  updateUnderrunCounting();
  emit fastForwardChanged();
}

void GameSession::setPreviewFastForwardSupported(bool supported) {
  fastForwardSupported_ = supported;
  if (!supported) fastForward_ = false;
  emit fastForwardChanged();
}

void GameSession::setAudioMuted(bool muted) {
  audioMuted_ = muted;
  updateUnderrunCounting();
}

void GameSession::togglePause() {
  if (state_ == Running) {
    pause();
  } else if (state_ == Paused) {
    resume();
  }
}

void GameSession::reset() {
  if (runner_ && (state_ == Running || state_ == Paused)) {
    runner_->reset();
  }
}

bool GameSession::liveSaveAccepts(qint64 size) {
  return runner_ && (state_ == Running || state_ == Paused) && runner_->saveMemoryAccepts(size);
}

void GameSession::flushLiveSave() {
  if (runner_ && (state_ == Running || state_ == Paused)) {
    if (!runner_->flushSaveNow()) {
      qWarning() << "The battery save could not be written; the save file on disk may be stale";
      emit saveWriteFailed();
    }
  }
}

// A live save is a full game restart, the same path as Quit + Start: the running core is stopped (its normal stop flushes
// the old save to the file), only then the new save is written, and the game is started again (prepareForStart on this
// thread, fresh worker, core load, load game, load save). The core is never reloaded in place: unload_game + load_game
// inside one retro_init (with GL context teardown) is not something cores like melonDS survive.
bool GameSession::restartWithSave(const QString& saveFile, const QByteArray& data) {
  if (saveFile.isEmpty()) return false;
  return restartWith([&]() {
    QSaveFile f(saveFile);
    const bool written = f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
    if (!written) qWarning() << "Live save: the new save file could not be written, the game restarts with the old save";
    return written;
  });
}

// `writer` runs between the stop (old save flushed) and the start; false = the game restarts with the old data.
bool GameSession::restartWith(const std::function<bool()>& writer) {
  if (!runner_ || !(state_ == Running || state_ == Paused) || config_.gamePath.isEmpty() || !writer) return false;
  const LaunchConfig cfg = config_;
  teardown();  // blocks until the core is unloaded and the old save is flushed
  stopSaveError_.clear();  // the writer replaces the file with the live save on purpose
  const bool written = writer();
  launch(cfg, true);
  return written;
}

// A failed final save write is reported once, after the core is unloaded (the owner shows it in the Library).
void GameSession::reportStopSaveError() {
  if (stopSaveError_.isEmpty()) return;
  const QString why = stopSaveError_;
  stopSaveError_.clear();
  qWarning().noquote() << "The battery save could not be written at stop:" << why;
  emit stopSaveFailed(why);
}

void GameSession::stop() {
  restarting_ = false;
  const bool wasActive = state_ != Idle;
  teardown();
  reportStopSaveError();
  setState(Idle);
  if (wasActive) {
    emit finished();
  }
}

void GameSession::applyJoypad() {
  if (runner_) {
    runner_->setJoypadState(0, remoteControlled_[0] ? remoteMask_[0] : joypadMask());
    runner_->setJoypadState(1, remoteControlled_[1] ? remoteMask_[1] : 0);
  }
}

void GameSession::setRemoteControlled(int port, bool controlled) {
  if (port < 0 || port > 1 || remoteControlled_[port] == controlled) return;
  remoteControlled_[port] = controlled;
  remoteMask_[port] = 0;
  if (port == 0 && runner_) runner_->setPointer(0.0, 0.0, false);  // a local touch never continues as a remote one, or the other way
  applyJoypad();
}

void GameSession::setRemoteInput(int port, quint32 mask, bool touching, const QPointF& touch) {
  if (port < 0 || port > 1 || !remoteControlled_[port]) return;
  if (port == 0 && runner_) runner_->setPointer(touch.x(), touch.y(), touching);
  if (mask == remoteMask_[port]) return;
  remoteMask_[port] = mask;
  applyJoypad();
}

bool GameSession::isReservedKey(int qtKey) const {
  return qtKey == Qt::Key_Escape || hotkeyKeys_.contains(qtKey);
}

QStringList GameSession::screenLayouts() const { return screenLayoutsFor(screenCount()); }

namespace {
// Core option that asks for the internal resolution of the OpenGL renderer ("2", "3", "2x native"): the scale, 0 if none.
int requestedScaleOf(const QMap<QString, QString>& options) {
  for (auto it = options.cbegin(); it != options.cend(); ++it) {
    if (it.key().contains(QLatin1String("opengl_resolution"))) {
      int n = 0;
      for (const QChar c : it.value()) {
        if (!c.isDigit()) break;
        n = n * 10 + c.digitValue();
      }
      return n;
    }
  }
  return 0;
}
}  // namespace

void GameSession::setViewSize(const QObject* view, const QSize& pixels) {
  if (!view) return;
  if (pixels.isEmpty()) viewSizes_.remove(view);
  else viewSizes_.insert(view, pixels);
  updateReadbackLimit();
}

void GameSession::setCpuConsumer(const QObject* view, bool on) {
  if (!view || cpuViews_.contains(view) == on) return;
  if (on) cpuViews_.insert(view);
  else cpuViews_.remove(view);
  updateReadbackLimit();
}

void GameSession::setShareSize(const QSize& pixels) {
  if (shareSize_ == pixels) return;
  shareSize_ = pixels;
  updateReadbackLimit();
}

// The platform display of this game launch. A live-save restart is the same game: a display that already gave up
// (Failed/Unavailable) stays as it is, an Active/Pending one is made anew, because the new core has a new GL context and
// the old ring was released with the old one. Without a platform display (gpuDisplay_ null) nothing changes at all.
void GameSession::setupDisplay(bool restart) {
  if (restart && gpuDisplay_ && (gpuDisplay_->state() == GpuDisplayBridge::State::Failed || gpuDisplay_->state() == GpuDisplayBridge::State::Unavailable)) {
    return;
  }
  const bool hadDisplay = gpuDisplay_ != nullptr;
  if (gpuDisplay_) gpuDisplay_->setStateCallback({});
  gpuDisplay_.reset();
  // Destroying the old source schedules the cleanup of its textures on the render thread; a GameView whose node still
  // holds one must rebuild it in the sync that follows, also when no frameChanged comes (live-save restart).
  if (hadDisplay) emit displayStateChanged();
  QString why;
  PlatformDisplay pd = platformDisplayFactory() ? platformDisplayFactory()(&why) : makePlatformDisplay(&why);
  if (!pd.producer) {
    // Not applicable. "not Windows" (no implementation here) stays at debug level so Linux logs are as they were; any
    // other reason (e.g. the kill switch) is worth an info line.
    const QString line = QStringLiteral("Zero-copy display not available (%1); the game view uses readback frames").arg(why);
    if (why == QLatin1String("not Windows")) qCDebug(lcDisplayHost).noquote() << line;
    else qCInfo(lcDisplayHost).noquote() << line;
    return;
  }
  if (!platformDisplayFactory()) displayBridge_ = std::static_pointer_cast<GlD3d11Bridge>(pd.shared);  // the real factory's shared object is the bridge
  gpuDisplay_ = std::make_shared<GpuDisplayBridge>(std::move(pd));
  // Called from the emulation or the render thread; the plan is re-applied on the GUI thread (QPointer: the session may
  // be gone by then).
  gpuDisplay_->setStateCallback([self = QPointer<GameSession>(this)]() {
    if (self) QMetaObject::invokeMethod(self.data(), [self]() { if (self) self->onDisplayStateChanged(); }, Qt::QueuedConnection);
  });
}

void GameSession::onDisplayStateChanged() {
  updateReadbackLimit();
  emit displayStateChanged();
}

std::shared_ptr<GlD3d11Bridge> GameSession::displayBridge() const {
  std::shared_ptr<GlD3d11Bridge> b = displayBridge_.lock();
  // A closed, failed or unavailable bridge (the last run's, or Qt Quick is not on D3D11) is never handed on: the capture
  // opens an own one.
  if (b && (b->status() == GlD3d11Bridge::Status::Unavailable || b->status() == GlD3d11Bridge::Status::Failed)) return nullptr;
  return b;
}

DisplayTextureSource* GameSession::displaySource() const { return gpuDisplay_ && runner_ ? gpuDisplay_->source() : nullptr; }

void GameSession::confirmDisplayShown() {
  if (gpuDisplay_) gpuDisplay_->confirmShown();
}

GpuDisplayBridge::State GameSession::displayState() const {
  return gpuDisplay_ ? gpuDisplay_->state() : GpuDisplayBridge::State::Unavailable;
}

QString GameSession::displayReason() const { return gpuDisplay_ ? gpuDisplay_->reason() : QString(); }

void GameSession::setGpuEncodeTarget(std::shared_ptr<emu::GpuEncodeTarget> target) {
  gpuTarget_ = std::move(target);
  if (runner_) {
    runner_->setGpuEncodeTarget(gpuTarget_);
  }
}

bool GameSession::hardwareRendered() const {
  if (!runner_ || !isActive()) return false;
  const emu::RenderInfo ri = runner_->renderInfo();
  return ri.hwActive;  // OpenGL and Vulkan games alike: GPU-direct Session encoding (ADR 0019, ADR 0034 D9) and the zero-copy display (ADR 0034 D8)
}

void GameSession::updateReadbackLimit() {
  QSize views;  // empty = nobody reported a size: unlimited
  for (const QSize& s : std::as_const(viewSizes_)) views = views.expandedTo(s);
  const bool active = gpuDisplay_ && gpuDisplay_->state() == GpuDisplayBridge::State::Active;
  ReadbackPlan plan = planReadback(active, views, shareSize_);
  if (active && !cpuViews_.isEmpty()) {  // zero-copy shows the game in one window; the others need readback frames
    QSize cpu;
    bool unlimited = false;
    for (const QObject* v : std::as_const(cpuViews_)) {
      const QSize s = viewSizes_.value(v);
      if (s.isEmpty()) unlimited = true;
      cpu = cpu.expandedTo(s);
    }
    plan.readback = true;
    plan.readbackLimit = unlimited ? QSize() : cpu.expandedTo(shareSize_);
  }
  if (!captureWaiters_.isEmpty()) {  // a plugin waits for a native-size frame (video.capture): full size, readback on
    plan.readback = true;
    plan.readbackLimit = QSize();
  }
  limit_ = plan.readbackLimit;
  displayLimit_ = plan.displayLimit;
  readbackOn_ = plan.readback;
  if (runner_) {
    runner_->setReadbackLimit(limit_);
    runner_->setDisplayLimit(displayLimit_);
    runner_->setReadbackEnabled(readbackOn_);
  }
  // Zero-copy active and no readback: the readback image stops being updated and would turn stale (shown again on stop
  // or a live-save restart, or sent to a viewer that connects). Drop it; a resumed readback (Failed, shared) refills it.
  if (active && !readbackOn_ && !frame_.isNull()) {
    frame_ = QImage();
    emit frameChanged();
  }
}

QSize GameSession::sourceFrameSize() const {
  if (!runner_) return preview_ ? frame_.size() : QSize();
  return runner_->sourceFrameSize();
}

void GameSession::requestNativeFrame(std::function<void(const QImage&, const QString&)> done) {
  if (!isActive() && !preview_) {
    done(QImage(), QStringLiteral("no_frame"));
    return;
  }
  captureWaiters_.append(std::move(done));
  const bool readbackWasOn = readbackOn_;  // with readback off, frame_ is not kept up to date: wait for a fresh frame
  updateReadbackLimit();
  if (readbackWasOn) serveCaptures(false);  // the current frame may already have the native size
  if (!captureWaiters_.isEmpty()) {
    QPointer<GameSession> self(this);
    QTimer::singleShot(2000, this, [self]() {
      if (self && !self->captureWaiters_.isEmpty()) self->serveCaptures(true);
    });
  }
}

// Serves the waiting capture requests with the current frame when it has the native size (or always when timed out/stopped).
void GameSession::serveCaptures(bool timedOut) {
  if (captureWaiters_.isEmpty()) return;
  const QSize native = sourceFrameSize();
  const bool haveFrame = !frame_.isNull() && readbackOn_ && (native.isEmpty() || frame_.size() == native);
  if (!haveFrame && !timedOut) return;
  auto waiters = std::move(captureWaiters_);
  captureWaiters_.clear();
  const QImage img = haveFrame ? frame_ : QImage();
  updateReadbackLimit();  // back to the normal plan
  for (auto& w : waiters) w(img, haveFrame ? QString() : QStringLiteral("no_frame"));
}

EmulationDiagnostics GameSession::diagnostics() const {
  if (preview_) {
    return previewDiag_;
  }
  EmulationDiagnostics d;
  if (!runner_ || !isActive()) {
    return d;
  }
  d.valid = true;
  d.core = coreName_;
  const emu::RenderInfo ri = runner_->renderInfo();
  d.hwRequested = ri.hwRequested;
  d.hwActive = ri.hwActive;
  d.api = ri.api;
  d.gpu = ri.gpu;
  d.fallbackReason = ri.fallbackReason;
  d.vulkanFallback = ri.vulkanFallback;
  d.frameSize = runner_->sourceFrameSize();
  if (d.frameSize.isEmpty()) d.frameSize = frame_.size();
  if (readbackOn_ && !frame_.isNull() && frame_.size() != d.frameSize) d.readbackSize = frame_.size();  // only while a readback runs
  d.baseSize = display_.frameSize();
  d.screens = screenCount();
  for (const auto& s : display_.screens) d.screenSizes.append(QSize(s.width, s.height));
  d.requestedScale = requestedScaleOf(coreOptions_);
  d.cpuThreads = QThread::idealThreadCount();
  const auto t = runner_->timing();
  d.targetFps = targetFps_;
  if (t.valid) {
    d.fps = t.fps;
    d.frameMs = t.frameMs;
    d.emuMs = t.emuMs;
    d.readbackMs = t.readbackMs;
    d.readbacksPerSec = t.readbacksPerSec;
    d.gpuCopyMs = t.gpuCopyMs;
    d.gpuCopiesPerSec = t.gpuCopiesPerSec;
    d.displayMs = t.displayMs;
    d.displaysPerSec = t.displaysPerSec;
  }
  d.zeroCopy = gpuDisplay_ && gpuDisplay_->state() == GpuDisplayBridge::State::Active;
  if (d.zeroCopy && !d.frameSize.isEmpty()) {
    const QSize shown = emu::HwRenderContext::scaledReadbackSize(d.frameSize.width(), d.frameSize.height(), displayLimit_);
    if (shown != d.frameSize) d.displaySize = shown;
  }
  d.totalHistory.reserve(t.history.size());
  d.emuHistory.reserve(t.history.size());
  for (const auto& s : t.history) {
    d.totalHistory.append(s.totalMs);
    d.emuHistory.append(s.emuMs);
  }
  d.audioActive = audio_.isActive();
  d.audioBufferMs = audio_.bufferedMs();
  d.underruns = audio_.underruns();
  return d;
}

void GameSession::setPreview(const QString& title, const QImage& frame, const emu::DisplayProfile& profile,
                             const EmulationDiagnostics& diag) {
  preview_ = true;
  previewDiag_ = diag;
  previewDiag_.valid = true;
  title_ = title;
  display_ = profile;
  frame_ = frame;
  setState(Running);
  emit titleChanged();
  emit frameChanged();
}

int GameSession::comboOf(int qtKey, Qt::KeyboardModifiers mods) {
  return qtKey | int(mods & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier));
}

bool GameSession::keyEvent(int qtKey, bool pressed, Qt::KeyboardModifiers mods) {
  if (inputBlocked_ || isReservedKey(qtKey) || (!reservedCombos_.isEmpty() && reservedCombos_.contains(comboOf(qtKey, mods)))) {
    return false;
  }
  const bool mapped = pressed ? keys_.press(qtKey) : keys_.release(qtKey);
  if (mapped) {
    applyJoypad();
  }
  return mapped;
}

void GameSession::releaseAllKeys() {
  keys_.clear();
  if (runner_) {
    applyJoypad();  // a held gamepad button stays pressed
    runner_->setPointer(0.0, 0.0, false);
  }
}

void GameSession::setKeyboardMap(const QHash<int, quint32>& map) {
  keys_.setMap(map);
  applyJoypad();
}

void GameSession::setGamepadMask(quint32 mask) {
  if (mask == pad_) return;
  pad_ = mask;
  applyJoypad();
}

void GameSession::setDebugPad(quint32 mask) {
  if (mask == debugPad_) return;
  debugPad_ = mask;
  applyJoypad();
}

void GameSession::setInputBlocked(bool blocked) {
  if (blocked == inputBlocked_) return;
  inputBlocked_ = blocked;
  if (blocked) {
    keys_.clear();
    if (runner_) runner_->setPointer(0.0, 0.0, false);
  }
  applyJoypad();  // blocked: released; unblocked: a gamepad button that is still held counts again
}

void GameSession::setPointer(const QPointF& p, bool pressed) {
  if (inputBlocked_ || remoteControlled_[0]) return;
  if (runner_) {
    runner_->setPointer(p.x(), p.y(), pressed);
  }
}

}  // namespace uzume::ui
