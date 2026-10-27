#include <QDebug>
#include "emulation_runner.h"

#include "frame_pacer.h"

#include <QDeadlineTimer>
#include <QThread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <future>
#include <memory>

namespace uzume::emu {

std::atomic<int> EmulationRunner::sleepQuantumMsForTest{0};

class EmulationRunner::Worker : public QThread {
 public:
  Worker(EmulationRunner* owner, EmulatorBackend* backend, StartRequest req)
      : m_owner(owner), m_backend(backend), m_req(std::move(req)) {}

  void requestStop() {
    QMutexLocker l(&m_mutex);
    m_stop = true;
    m_cond.wakeAll();
  }
  void setPaused(bool p) {
    QMutexLocker l(&m_mutex);
    m_paused = p;
    m_cond.wakeAll();
  }
  void requestReset() {
    QMutexLocker l(&m_mutex);
    m_reset = true;
    m_cond.wakeAll();
  }
  // Emulation thread, inside a task while paused: `n` frames without pacing; only the last one is read back and delivered.
  qint64 stepOnEmuThread(EmulatorBackend& be, int n) {
    for (int i = 0; i < n; ++i) {
      const bool last = i == n - 1;
      be.setVideoWanted(last);
      const auto t0 = std::chrono::steady_clock::now();
      if (!be.runFrame()) return -1;
      m_owner->m_timing.recordFrame(monotonicMs(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                                    be.lastReadbackMs(), be.lastGpuCopyMs(), be.lastGpuCaptured(), be.lastDisplayMs(), be.lastDisplayed());
      if (!m_owner->m_memoryWatch->empty()) {
        if (const MemoryMap* mm = be.memoryMap()) m_owner->m_memoryWatch->capture(*mm, be.frameCount());
      }
      (void)be.takeAudio();
    }
    be.setVideoWanted(true);
    emit m_owner->frameReady(be.videoFrame(), be.frameCount(), 0);
    return static_cast<qint64>(be.frameCount());
  }
  // Any thread: queue a command (never blocks on the emulation thread's work: the mutex is not held while commands run).
  // false: the thread is not running.
  bool submitCommand(quint64 id, Command cmd, std::function<void(const CommandResult&)> onDone, std::shared_ptr<std::atomic<bool>> cancelled) {
    QMutexLocker l(&m_mutex);
    if (!isRunning() || m_stop || m_exited) return false;
    m_cmds.push_back({id, std::move(cmd), std::move(onDone), std::move(cancelled)});
    m_cond.wakeAll();
    return true;
  }
  // GUI thread: queue `fn` for the emulation thread and wait until it ran.
  bool runTask(std::function<void(EmulatorBackend&)> fn, int timeoutMs) {
    auto task = std::make_shared<Task>();
    task->fn = std::move(fn);
    QMutexLocker l(&m_mutex);
    if (!isRunning() || m_stop || m_exited) return false;
    m_tasks.push_back(task);
    m_cond.wakeAll();
    QDeadlineTimer deadline(timeoutMs);
    while (!task->done && !m_exited && !deadline.hasExpired()) m_doneCond.wait(&m_mutex, deadline);
    if (!task->done) task->cancelled = true;  // never runs later
    return task->done;
  }

 protected:
  void run() override {
#ifdef Q_OS_WIN
    // Waits round up to the ~15.6 ms system tick otherwise; sped up, one frame lasts only 2-8 ms.
    struct TimerResolution {
      TimerResolution() { timeBeginPeriod(1); }
      ~TimerResolution() { timeEndPeriod(1); }
    } timerResolution;
#endif
    QString err;
    EmulatorBackend& be = *m_backend;
    // Directories before loadCore (cores already read them in retro_set_environment).
    be.setSystemDirectory(m_req.systemDir);
    be.setSaveDirectory(m_req.saveDir);
    bool ok = be.loadCore(m_req.corePath, &err);
    if (ok) {
      for (auto it = m_req.coreOptions.cbegin(); it != m_req.coreOptions.cend(); ++it) be.setCoreOption(it.key(), it.value());
      be.setRequireHwRender(m_req.requireHwRender);
      ok = be.loadGame(m_req.gamePath, &err);
    }
    if (!ok) {
      be.unloadCore();
      {
        QMutexLocker l(&m_mutex);
        m_exited = true;
        m_doneCond.wakeAll();
      }
      failPendingCommands();
      emit m_owner->startFailed(err);
      return;
    }
    auto publishRegions = [&]() {
      std::vector<MemoryMap::RegionInfo> regions;
      if (const MemoryMap* mm = be.memoryMap()) regions = mm->regions();
      QMutexLocker rl(&m_owner->m_regionsMutex);
      m_owner->m_regions = std::move(regions);
    };
    publishRegions();
    uint64_t memMapGen = be.memoryMapGeneration();
    const AvInfo av = be.avInfo();
    int rate = static_cast<int>(std::lround(av.sampleRate));
    double fps = av.fps > 1.0 ? av.fps : 60.0;
    double avFps = av.fps, avRate = av.sampleRate;  // raw values the backend reported (change detection)
    auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / fps));
    if (m_req.speedUpOnStart && be.supportsFastForward()) m_owner->m_fastForward.store(true);
    emit m_owner->started(av, be.coreInfo());
    m_owner->setState(State::Running);

    auto next = std::chrono::steady_clock::now();
    // Fast-forward: which emulated frames the UI/Session gets (base fps schedule, see FramePacer).
    const auto ticks = [](std::chrono::steady_clock::time_point t) {
      return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
    };
    FramePacer pacer;
    pacer.reset(ticks(next));
    bool wasPaused = false;
    bool wasFast = false;
    double audioPhase = 0.0;
    int framesSinceSnapshot = 1 << 30;  // first frame after a watch is registered is captured at once
    while (true) {
      {
        QMutexLocker l(&m_mutex);
        while (m_paused && !m_stop) {
          if (!wasPaused) { wasPaused = true; be.flushSave(); m_owner->setState(State::Paused); }
          drainTasks(be);
          drainCommands(be);
          if (!m_stop && m_cmds.empty() && m_tasks.empty()) m_cond.wait(&m_mutex);  // both may have grown while a command ran unlocked
        }
        drainTasks(be);
        if (m_stop) break;
        if (wasPaused) { wasPaused = false; next = std::chrono::steady_clock::now(); m_owner->setState(State::Running); }
        if (m_reset) {
          m_reset = false;
          be.reset();
          reapplyCheats(be);  // a core may drop its cheat engine state on reset
        }
        drainCommands(be);
        if (m_stop) break;
      }
      const bool fast = m_owner->m_fastForward.load();
      if (fast != wasFast) {  // resynchronize the clock like after a pause
        wasFast = fast;
        be.setFastForwarding(fast, m_owner->fastForwardRatio());
        next = std::chrono::steady_clock::now();
        pacer.reset(ticks(next));
        audioPhase = 0.0;
      }
      const double ratio = fast ? m_owner->fastForwardRatio() : 1.0;
      if (fast) be.setFastForwarding(true, ratio);
      const auto step = fast ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(period / ratio) : period;
      const auto frameStart = std::chrono::steady_clock::now();
      // Fast-forward: the UI sees at most the base fps, so only those frames need video (readback/conversion).
      const bool wantVideo = !fast || pacer.wantsFrame(ticks(frameStart));
      be.setVideoWanted(wantVideo);
      const bool frameOk = be.runFrame();
      const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
      if (frameOk) m_owner->m_timing.recordFrame(monotonicMs(), frameMs, be.lastReadbackMs(), be.lastGpuCopyMs(), be.lastGpuCaptured(),
                                                     be.lastDisplayMs(), be.lastDisplayed());
      if (frameOk && be.memoryMapGeneration() != memMapGen) {  // the core re-sent SET_MEMORY_MAPS during retro_run
        memMapGen = be.memoryMapGeneration();
        publishRegions();
        emit m_owner->memoryRegionsChanged();
      }
      if (frameOk && !m_owner->m_memoryWatch->empty()) {  // no lock is held here: the core is not running
        if (++framesSinceSnapshot >= m_owner->m_snapshotInterval.load()) {
          framesSinceSnapshot = 0;
          if (const MemoryMap* mm = be.memoryMap()) m_owner->m_memoryWatch->capture(*mm, be.frameCount());
        }
      }
      if (!frameOk) {
        emit m_owner->errorOccurred(QStringLiteral("Core stopped execution"));
        break;
      }
      if (wantVideo) {
        qint64 lateNs = 0;
        if (fast) {
          lateNs = pacer.delivered(ticks(frameStart), ticks(std::chrono::steady_clock::now()),
                                   std::chrono::duration_cast<std::chrono::nanoseconds>(period).count());
        }
        emit m_owner->frameReady(be.videoFrame(), be.frameCount(), lateNs);
      }
      {  // SET_SYSTEM_AV_INFO at runtime: follow new timing / sample rate (cheap compare, same thread)
        const AvInfo cur = be.avInfo();
        if (cur.fps != avFps || cur.sampleRate != avRate) {
          avFps = cur.fps;
          avRate = cur.sampleRate;
          rate = static_cast<int>(std::lround(cur.sampleRate));
          fps = cur.fps > 1.0 ? cur.fps : 60.0;
          period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / fps));
          audioPhase = 0.0;
        }
      }
      const QByteArray pcm = be.takeAudio();  // always drained
      if (!pcm.isEmpty()) {
        if (!fast) {
          emit m_owner->audioReady(pcm, rate);
        } else if (m_owner->speedUpAudio()) {  // real-time rate again: the sink and a shared Session see a normal stream
          const QByteArray slow = resampleForSpeedUp(pcm, ratio, &audioPhase);
          if (!slow.isEmpty()) emit m_owner->audioReady(slow, rate);
        }
      }

      next += step;
      const auto now = std::chrono::steady_clock::now();
      // Too far behind: resynchronize. At normal speed catch-up is limited to 2 frames, so a late frame never turns into
      // a long burst (average = target; the measured fps stays near it). Sped up, a coarse timer may oversleep by more than 5 short steps, which
      // would drop the catch-up and cap the speed, so the limit is at least 100 ms there.
      const auto maxBehind = fast ? (std::max<std::chrono::steady_clock::duration>)(5 * step, std::chrono::milliseconds(100)) : 2 * step;
      if (next < now - maxBehind) next = now;
      QMutexLocker l(&m_mutex);
      while (!m_stop && !m_paused && !m_reset && m_tasks.empty() && m_cmds.empty()) {
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next - std::chrono::steady_clock::now());
        if (wait.count() <= 0) break;
        unsigned long ms = static_cast<unsigned long>(wait.count());
        const int q = sleepQuantumMsForTest.load();
        if (q > 1) ms = (ms + q - 1) / q * q;  // test: sleeps overshoot like on Windows without 1 ms timer resolution
        m_cond.wait(&m_mutex, ms);
      }
    }
    {
      QMutexLocker l(&m_mutex);
      m_exited = true;  // waiting callers give up
      m_doneCond.wakeAll();
    }
    failPendingCommands();
    {
      QMutexLocker rl(&m_owner->m_regionsMutex);
      m_owner->m_regions.clear();
    }
    be.unloadGame();
    be.unloadCore();
  }

 private:
  struct QueuedCommand {
    quint64 id;
    Command cmd;
    std::function<void(const CommandResult&)> onDone;
    std::shared_ptr<std::atomic<bool>> cancelled;
  };
  void finish(QueuedCommand& q, CommandResult r) {
    r.requestId = q.id;
    r.kind = q.cmd.kind;
    if (q.onDone) q.onDone(r);
    emit m_owner->commandFinished(r);
  }
  // Emulation thread, m_mutex held on entry and on return; the mutex is released while a command runs.
  void drainCommands(EmulatorBackend& be) {
    while (!m_cmds.empty()) {
      QueuedCommand q = std::move(m_cmds.front());
      m_cmds.pop_front();
      m_mutex.unlock();
      CommandResult r;
      if (q.cancelled && q.cancelled->load()) {
        r.error = QStringLiteral("timeout");  // the caller gave up before it started
      } else {
        r = runCommand(be, q.cmd);
      }
      finish(q, std::move(r));
      m_mutex.lock();
    }
  }
  // Results for everything still queued once no frame will run any more.
  void failPendingCommands() {
    std::deque<QueuedCommand> rest;
    {
      QMutexLocker l(&m_mutex);
      rest.swap(m_cmds);
    }
    for (QueuedCommand& q : rest) {
      CommandResult r;
      r.error = QStringLiteral("stopped");
      finish(q, std::move(r));
    }
  }
  // Emulation thread: resets the core's cheats and sends the list again. Only enabled entries are sent, with consecutive
  // indices: mGBA and DeSmuME ignore the `enabled` flag of retro_cheat_set and would apply a disabled code. Returns the
  // number of entries sent.
  int sendCheats(EmulatorBackend& be, const QList<CheatEntry>& list) {
    be.cheatReset();
    unsigned index = 0;
    for (const CheatEntry& c : list) {
      if (!c.enabled || c.code.trimmed().isEmpty()) continue;
      be.cheatSet(index++, true, c.code);
    }
    return static_cast<int>(index);
  }
  void reapplyCheats(EmulatorBackend& be) {
    if (!be.supportsCheats() || !be.isGameLoaded()) return;
    const QList<CheatEntry> list = m_owner->activeCheats();
    if (!list.isEmpty()) sendCheats(be, list);
  }
  CommandResult runCommand(EmulatorBackend& be, const Command& cmd) {
    CommandResult r;
    if (!be.isGameLoaded()) { r.error = QStringLiteral("no_game"); return r; }
    switch (cmd.kind) {
      case Command::Kind::SaveState:
        if (!be.supportsStates()) { r.error = QStringLiteral("unsupported"); return r; }
        r.frame = be.frameCount();
        r.preview = be.videoFrame();
        if (!be.serialize(&r.state)) { r.error = QStringLiteral("failed"); r.preview = QImage(); return r; }
        r.ok = true;
        return r;
      case Command::Kind::LoadState:
        if (!be.supportsStates()) { r.error = QStringLiteral("unsupported"); return r; }
        if (cmd.data.isEmpty()) { r.error = QStringLiteral("invalid"); return r; }
        if (!be.unserialize(cmd.data)) { r.error = QStringLiteral("failed"); return r; }
        r.frame = be.frameCount();
        reapplyCheats(be);  // the core's cheat list is not part of a state
        r.ok = true;
        return r;
      case Command::Kind::SetCheats:
        if (!be.supportsCheats()) { r.error = QStringLiteral("unsupported"); return r; }
        r.applied = sendCheats(be, cmd.cheats);
        {
          QMutexLocker cl(&m_owner->m_cheatsMutex);
          m_owner->m_activeCheats = cmd.cheats;
        }
        r.ok = true;
        return r;
      case Command::Kind::WriteMemory: {
        const MemoryMap* mm = be.memoryMap();
        if (cmd.data.isEmpty() || cmd.data.size() > kMaxMemoryWriteBytes) { r.error = QStringLiteral("invalid"); return r; }
        if (!mm) { r.error = QStringLiteral("unmapped"); return r; }
        // First pass: the whole range must be mapped, so a failed write changes nothing.
        const uint64_t total = static_cast<uint64_t>(cmd.data.size());
        for (uint64_t done = 0; done < total;) {
          const MemoryMap::Span sp = mm->resolve(cmd.address + done);
          if (!sp.ptr || sp.len == 0) { r.error = QStringLiteral("unmapped"); return r; }
          done += (std::min)(sp.len, total - done);
        }
        for (uint64_t done = 0; done < total;) {
          const MemoryMap::Span sp = mm->resolve(cmd.address + done);
          const uint64_t n = (std::min)(sp.len, total - done);
          std::memcpy(sp.ptr, cmd.data.constData() + done, static_cast<size_t>(n));
          done += n;
        }
        r.ok = true;
        return r;
      }
    }
    return r;
  }
  std::deque<QueuedCommand> m_cmds;
  struct Task {
    std::function<void(EmulatorBackend&)> fn;
    bool done = false;
    bool cancelled = false;
  };
  // Called with m_mutex held, on the emulation thread.
  void drainTasks(EmulatorBackend& be) {
    while (!m_tasks.empty()) {
      const std::shared_ptr<Task> t = m_tasks.front();
      m_tasks.pop_front();
      if (!t->cancelled) {
        t->fn(be);
        t->done = true;
      }
      m_doneCond.wakeAll();
    }
  }
  std::deque<std::shared_ptr<Task>> m_tasks;
  QWaitCondition m_doneCond;
  bool m_exited = false;
  EmulationRunner* m_owner;
  EmulatorBackend* m_backend;
  StartRequest m_req;
  QMutex m_mutex;
  QWaitCondition m_cond;
  bool m_stop = false;
  bool m_paused = false;
  bool m_reset = false;
};

EmulationRunner::EmulationRunner(std::unique_ptr<EmulatorBackend> backend, QObject* parent)
    : QObject(parent), m_backend(std::move(backend)) {
  qRegisterMetaType<uzume::emu::AvInfo>();
  qRegisterMetaType<uzume::emu::CoreInfo>();
  qRegisterMetaType<uzume::emu::EmulationRunner::State>();
  qRegisterMetaType<uzume::emu::EmulationRunner::CommandResult>();
  // Called on the emulation thread; the signal reaches the GUI thread queued.
  m_backend->setSaveProblemNotifier([this](const QString& message) { emit saveProblem(message); });
}

EmulationRunner::~EmulationRunner() {
  stop();
  if (m_abandoned) (void)m_backend.release();  // the hung emulation thread may still use it: leak rather than crash
}

std::atomic<int> EmulationRunner::stopTimeoutMsForTest{0};

void EmulationRunner::setState(State s) {
  if (m_state.exchange(s) != s) emit stateChanged(s);
}

void EmulationRunner::start(const StartRequest& request) {
  if (m_worker && m_worker->isFinished()) m_worker.reset();  // finished by itself (error/core exit)
  if (m_worker) {
    emit startFailed(QStringLiteral("Emulation is already running"));
    return;
  }
  m_timing.reset();
  {
    QMutexLocker l(&m_cheatsMutex);
    m_activeCheats.clear();
  }
  m_fastForward.store(false);
  m_ratio.store(normalizeSpeedUpRatio(request.speedUpRatio));
  m_speedUpAudio.store(request.speedUpAudio);
  m_backend->prepareForStart();  // GUI thread: resources the emulation thread cannot create itself
  m_worker = std::make_unique<Worker>(this, m_backend.get(), request);
  setState(State::Starting);
  // If the thread finishes by itself (start failure/core exit), reset the state.
  QObject::connect(m_worker.get(), &QThread::finished, this, [this] {
    if (m_worker && m_worker->isFinished()) setState(State::Idle);
  });
  m_worker->start();
}

void EmulationRunner::pause() { if (m_worker) m_worker->setPaused(true); }
void EmulationRunner::resume() { if (m_worker) m_worker->setPaused(false); }
void EmulationRunner::reset() { if (m_worker) m_worker->requestReset(); }

bool EmulationRunner::runOnEmuThread(std::function<void(EmulatorBackend&)> fn, int timeoutMs) {
  return m_worker && m_worker->runTask(std::move(fn), timeoutMs);
}
qint64 EmulationRunner::stepFrames(int frames, int timeoutMs) {
  if (frames < 1 || frames > kMaxStepFrames || m_state.load() != State::Paused || !m_worker) return -1;
  qint64 result = -1;
  const bool ran = runOnEmuThread([&](EmulatorBackend& be) { result = m_worker->stepOnEmuThread(be, frames); }, timeoutMs);
  return ran ? result : -1;
}
quint64 EmulationRunner::submit(Command cmd, std::function<void(const CommandResult&)> onDone) {
  if (!m_worker) return 0;
  const quint64 id = m_nextRequestId.fetch_add(1);
  return m_worker->submitCommand(id, std::move(cmd), std::move(onDone), nullptr) ? id : 0;
}

EmulationRunner::CommandResult EmulationRunner::execute(Command cmd, int timeoutMs) {
  CommandResult stopped;
  stopped.kind = cmd.kind;
  stopped.error = QStringLiteral("stopped");
  if (!m_worker) return stopped;
  auto promise = std::make_shared<std::promise<CommandResult>>();
  std::future<CommandResult> future = promise->get_future();
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  const quint64 id = m_nextRequestId.fetch_add(1);
  if (!m_worker->submitCommand(id, std::move(cmd), [promise](const CommandResult& r) { promise->set_value(r); }, cancelled)) return stopped;
  if (future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
    cancelled->store(true);  // does nothing once it has started
    if (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
      CommandResult r;
      r.requestId = id;
      r.error = QStringLiteral("timeout");
      return r;
    }
  }
  return future.get();
}

QList<EmulationRunner::CheatEntry> EmulationRunner::activeCheats() const {
  QMutexLocker l(&m_cheatsMutex);
  return m_activeCheats;
}

bool EmulationRunner::flushSaveNow() {
  bool written = false;
  const bool ran = runOnEmuThread([&](EmulatorBackend& be) { written = be.flushSave(); });
  return ran && written;
}
bool EmulationRunner::saveMemoryAccepts(qint64 size) {
  bool ok = false;
  const bool ran = runOnEmuThread([&](EmulatorBackend& be) { ok = size > 0 && be.saveMemorySize() == size; });
  return ran && ok;
}

void EmulationRunner::setFastForward(bool on) {
  if (on && !m_backend->supportsFastForward()) return;
  m_fastForward.store(on);
}

double EmulationRunner::normalizeSpeedUpRatio(double ratio) {
  double best = kDefaultSpeedUpRatio;
  double dist = 1e9;
  for (const double r : speedUpRatios()) {
    if (std::abs(r - ratio) < dist) { dist = std::abs(r - ratio); best = r; }
  }
  return best;
}

QByteArray EmulationRunner::resampleForSpeedUp(const QByteArray& pcm, double ratio, double* phase) {
  const qsizetype n = pcm.size() / 4;  // stereo frames
  if (n == 0 || ratio <= 1.0) return pcm;
  const auto* in = reinterpret_cast<const qint16*>(pcm.constData());
  QByteArray out;
  out.reserve(static_cast<qsizetype>(static_cast<double>(n) / ratio + 2) * 4);
  double pos = phase ? *phase : 0.0;
  while (pos < static_cast<double>(n)) {
    const qsizetype i = static_cast<qsizetype>(pos);
    const qsizetype j = (std::min<qsizetype>)(i + 1, n - 1);
    const double f = pos - static_cast<double>(i);
    for (int ch = 0; ch < 2; ++ch) {
      const qint16 v = static_cast<qint16>(std::lround(in[2 * i + ch] * (1.0 - f) + in[2 * j + ch] * f));
      out.append(reinterpret_cast<const char*>(&v), 2);
    }
    pos += ratio;
  }
  if (phase) *phase = pos - static_cast<double>(n);
  return out;
}

void EmulationRunner::stop() {
  if (!m_worker) return;
  m_worker->requestStop();
  const int timeoutMs = stopTimeoutMsForTest.load() > 0 ? stopTimeoutMsForTest.load() : kStopTimeoutMs;
  if (!m_worker->wait(timeoutMs)) {
    // Flushing the save is the first thing the thread does after the loop; what hangs is the core's unload/deinit. A
    // QThread cannot be destroyed while it runs, so it is leaked together with the backend.
    qWarning() << "The emulation thread did not stop within" << timeoutMs << "ms; abandoning it";
    (void)m_worker.release();
    m_abandoned = true;
    m_finalSaveError.clear();
    setState(State::Idle);
    emit stopped();
    return;
  }
  m_worker.reset();
  m_finalSaveError = m_backend->lastSaveError();
  setState(State::Idle);
  emit stopped();
}

std::vector<MemoryMap::RegionInfo> EmulationRunner::memoryRegions() const {
  QMutexLocker l(&m_regionsMutex);
  return m_regions;
}

FrameTimingStats::Snapshot EmulationRunner::timing() const { return m_timing.snapshot(monotonicMs()); }

qint64 EmulationRunner::monotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void EmulationRunner::setJoypadState(unsigned port, quint32 mask) { m_backend->setJoypadState(port, mask); }
void EmulationRunner::setPointer(double x, double y, bool pressed) { m_backend->setPointer(x, y, pressed); }
bool EmulationRunner::setCoreOption(const QString& key, const QString& value) { return m_backend->setCoreOption(key, value); }
QList<CoreOption> EmulationRunner::coreOptions() const { return m_backend->coreOptions(); }
QList<CoreOptionCategory> EmulationRunner::coreOptionCategories() const { return m_backend->coreOptionCategories(); }

}  // namespace uzume::emu
