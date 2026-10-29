#pragma once
// EmulationRunner: runs an EmulatorBackend in its own thread and paces runFrame()
// at the FPS reported by the core. No audio output and no rendering (the UI does that).
//
// The runner lives in the caller's thread (UI); all backend calls (load core, frames, unload)
// run in the emulation thread. Signals are emitted from the emulation thread; receivers in
// other threads get them via queued connections (auto connection).

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <functional>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include "emulator_backend.h"
#include "frame_stats.h"
#include "memory_snapshot.h"
#include "rewind_buffer.h"

namespace uzume::emu {

class EmulationRunner : public QObject {
  Q_OBJECT
 public:
  enum class State { Idle, Starting, Running, Paused };
  Q_ENUM(State)

  static constexpr double kDefaultSpeedUpRatio = 2.0;
  // Speeds the user can choose: 1.5, 2, 3, 4, 6, 8.
  static QList<double> speedUpRatios() { return {1.5, 2.0, 3.0, 4.0, 6.0, 8.0}; }
  static double normalizeSpeedUpRatio(double ratio);  // nearest allowed value
  // Speed-up audio: linear-interpolating decimation of interleaved stereo int16 by `ratio` to real time (pitch rises).
  // `phase` carries the fractional read position across chunks (start with 0).
  static QByteArray resampleForSpeedUp(const QByteArray& pcm, double ratio, double* phase);

  struct StartRequest {
    QString corePath;
    QString gamePath;
    QString systemDir;
    QString saveDir;
    QMap<QString, QString> coreOptions;  // e.g. manifest defaults, set before the game starts
    bool requireHwRender = false;        // core profile "requires_hw_render": start fails without an OpenGL context
    QString preferredRenderer;           // "opengl" (default, also when empty) | "vulkan" (ADR 0034); UZUME_RENDERER overrides
    // Core options that switch the core to OpenGL (core profile "renderer" mapping), set only when Vulkan is the chosen
    // renderer. When the Vulkan start fails (API version above the system's, no device, device creation failed) the
    // runner starts the core once more with the preferred renderer "opengl" and these options overlaid; empty = no retry.
    QMap<QString, QString> openglFallbackOptions;
    // Speed-up (fast-forward) settings: chosen speed, start sped up (if the core allows it), audio during speed-up.
    double speedUpRatio = kDefaultSpeedUpRatio;
    bool speedUpOnStart = false;
    bool speedUpAudio = true;
  };

  // One entry of the active cheat list. `format` is informational for the runner (the caller checks it against the core
  // profile's cheat formats); `code` goes to retro_cheat_set unchanged.
  struct CheatEntry {
    QString id;
    QString format;
    QString code;
    bool enabled = true;
  };

  // Commands for the emulation thread (0.20 foundation). All go through one queue, are applied between two frames (also
  // while paused) in submit order, and report asynchronously: commandFinished() carries the request id that submit()
  // returned. The emulation thread never waits for the GUI thread.
  struct Command {
    enum class Kind { SaveState, LoadState, SetCheats, WriteMemory, Rewind };
    Kind kind = Kind::SaveState;
    QByteArray data;             // LoadState: the state; WriteMemory: the bytes
    QList<CheatEntry> cheats;    // SetCheats: the whole active list (empty = clear)
    quint64 address = 0;         // WriteMemory: address in the device address space (the core's memory map)
    int count = 0;               // Rewind: how many kept states to step back
    static Command saveState() { return {}; }
    static Command loadState(QByteArray state) { Command c; c.kind = Kind::LoadState; c.data = std::move(state); return c; }
    static Command setCheats(QList<CheatEntry> list) { Command c; c.kind = Kind::SetCheats; c.cheats = std::move(list); return c; }
    static Command rewind(int snapshots) { Command c; c.kind = Kind::Rewind; c.count = snapshots; return c; }
    static Command writeMemory(quint64 address, QByteArray bytes) {
      Command c; c.kind = Kind::WriteMemory; c.address = address; c.data = std::move(bytes); return c;
    }
  };
  struct CommandResult {
    quint64 requestId = 0;
    Command::Kind kind = Command::Kind::SaveState;
    bool ok = false;
    // On failure: "unsupported" (core has no states/cheats), "no_game", "failed" (the core refused), "unmapped" (address or
    // part of the range is not in the memory map; nothing was written), "invalid", "stopped" (emulation ended first), "timeout", "empty" (Rewind: nothing kept yet).
    QString error;
    QByteArray state;  // SaveState: the serialized state
    quint64 frame = 0; // SaveState: backend frame number at the time
    QImage preview;    // SaveState: the current video frame (XRGB8888)
    int applied = 0;   // SetCheats: number of enabled entries handed to the core; Rewind: states stepped back
  };
  static constexpr int kMaxMemoryWriteBytes = 1 << 20;

  explicit EmulationRunner(std::unique_ptr<EmulatorBackend> backend, QObject* parent = nullptr);
  ~EmulationRunner() override;

  State state() const { return m_state; }

  // Asynchronous: load core, load game, start the clock. Result: started() or startFailed().
  void start(const StartRequest& request);
  void pause();
  void resume();
  void reset();
  // Runs `fn` on the emulation thread between two frames (also while paused) and waits for it (GUI thread). false: the
  // thread is not running, stopped meanwhile, or did not get to it within `timeoutMs` (then `fn` never runs).
  bool runOnEmuThread(std::function<void(EmulatorBackend&)> fn, int timeoutMs = 3000);
  // Debug frame step (MCP server): while paused, runs exactly `frames` (1..kMaxStepFrames) frames on the emulation thread
  // without pacing, then stays paused; the last frame is delivered through frameReady like a normal one, earlier frames
  // are not (no readback), audio is dropped. Returns the frame count after the step, or -1 when the runner is not
  // paused/running, the count is out of range or the thread did not get to it within `timeoutMs`.
  static constexpr int kMaxStepFrames = 600;
  qint64 stepFrames(int frames, int timeoutMs = 20000);
  // Queues a command; returns its request id (> 0), or 0 when the emulation thread is not running (no result follows then).
  // `onDone` (optional) is called on the emulation thread right before commandFinished is emitted.
  quint64 submit(Command cmd, std::function<void(const CommandResult&)> onDone = {});
  // Blocking convenience for non-emulation threads (MCP, tests): submit + wait. error "timeout" if the thread did not get to
  // it in time (a command that has not started then never runs), "stopped" if the runner is not running.
  CommandResult execute(Command cmd, int timeoutMs = 5000);
  // The active cheat list as last accepted by a SetCheats command (thread-safe copy). It is re-applied after reset() and
  // after a state load.
  QList<CheatEntry> activeCheats() const;

  // Rewind (0.20 package 4; thread-safe, effective from the next frame). `seconds` is the length of the ring buffer (0 = off);
  // the emulation thread keeps one serialized state every kRewindIntervalFrames frames while the core supports states. While
  // setRewinding(true) is in effect each frame steps back one state and the sound is muted; releasing it plays on from there.
  void setRewindSeconds(int seconds) { m_rewindSeconds.store((std::max)(0, seconds)); }
  int rewindSeconds() const { return m_rewindSeconds.load(); }
  void setRewinding(bool on) { m_rewinding.store(on); }
  bool rewinding() const { return m_rewinding.load(); }
  struct RewindInfo {
    bool active = false;       // a buffer is filling (setting on, core supports states, game running)
    int snapshots = 0;
    int capacity = 0;
    qint64 bytes = 0;
    qint64 stateBytes = 0;     // size of one state, 0 until the first was taken
    double secondsAvailable = 0;
  };
  RewindInfo rewindInfo() const;
  // Run-ahead (thread-safe): 0 = off, 1 or 2 frames. Only effective for cores whose profile says `run_ahead`; the caller decides.
  // Not used at speed-up or while rewinding.
  void setRunAhead(int frames) { m_runAhead.store((std::clamp)(frames, 0, 2)); }
  int runAhead() const { return m_runAhead.load(); }
  // Writes the battery save to its file now if it changed (blocking). Without it the file lags up to ~3 s.
  bool flushSaveNow();
  // Whether the running core's battery save memory has exactly `size` bytes (blocking).
  bool saveMemoryAccepts(qint64 size);
  // Blocks until the emulation thread has finished and the core is unloaded; then stopped(). The wait is bounded
  // (kStopTimeoutMs): a core that hangs while unloading is logged and abandoned (the save was flushed before that).
  void stop();
  static constexpr int kStopTimeoutMs = 10000;
  // Cores whose Vulkan start failed in this app run (key: core path): their next start goes straight to OpenGL.
  static void resetVulkanFailuresForTest();
  static std::atomic<int> stopTimeoutMsForTest;  // tests only: overrides kStopTimeoutMs when > 0
  // After stop(): why the final battery save write failed (progress may be lost); empty = saved or nothing to save.
  QString finalSaveError() const { return m_finalSaveError; }

  // thread-safe, take effect on the next frame
  void setJoypadState(unsigned port, quint32 buttonMask);
  void setAnalogState(unsigned port, const qint16 axes[4]);
  void setPointer(double x, double y, bool pressed);
  // Largest frame size the consumers need (pixels); hardware frames are downscaled on the GPU to it before readback.
  // Empty = no limit. See EmulatorBackend::setReadbackLimit.
  void setReadbackLimit(const QSize& maxSize) { m_backend->setReadbackLimit(maxSize); }
  QSize sourceFrameSize() const { return m_backend->sourceFrameSize(); }
  // Session encoding from the GPU (ADR 0019; thread-safe): hands the backend a target for the Session encode texture,
  // applied by the emulation thread at the start of its next frame. nullptr removes it. See EmulatorBackend.
  void setGpuEncodeTarget(std::shared_ptr<GpuEncodeTarget> target) { m_backend->setGpuEncodeTarget(std::move(target)); }
  // Zero-copy display (ADR 0027; thread-safe). See EmulatorBackend. The display limit is the largest size of the display
  // blit; readback off keeps the last image while the display target shows every frame.
  void setGpuDisplayTarget(std::shared_ptr<GpuDisplayTarget> target) { m_backend->setGpuDisplayTarget(std::move(target)); }
  void setDisplayLimit(const QSize& maxSize) { m_backend->setDisplayLimit(maxSize); }
  void setReadbackEnabled(bool on) { m_backend->setReadbackEnabled(on); }
  bool setCoreOption(const QString& key, const QString& value);
  QList<CoreOption> coreOptions() const;
  QList<CoreOptionCategory> coreOptionCategories() const;

  // Speed-up (thread-safe): the clock runs at fps * fastForwardRatio(); the UI still gets frames at the base fps and
  // audio is resampled to real time (or dropped when speedUpAudio is off). Only possible if the core allows it (known
  // after started()). The user's ratio wins over a ratio in the core's override.
  void setFastForward(bool on);
  bool fastForward() const { return m_fastForward.load(); }
  bool supportsFastForward() const { return m_backend->supportsFastForward(); }
  void setFastForwardRatio(double ratio) { m_ratio.store(normalizeSpeedUpRatio(ratio)); }
  double fastForwardRatio() const { return m_ratio.load(); }
  void setSpeedUpAudio(bool on) { m_speedUpAudio.store(on); }
  bool speedUpAudio() const { return m_speedUpAudio.load(); }

  // Plugin memory access: ranges registered here are copied into immutable snapshots on the emulation thread after
  // each successful runFrame (every snapshotInterval frames), only while the set is non-empty and the backend has a
  // memory map. Always non-null, lives as long as the runner.
  std::shared_ptr<MemoryWatchSet> memoryWatch() const { return m_memoryWatch; }
  void setSnapshotInterval(int frames) { m_snapshotInterval.store((std::max)(1, frames)); }
  int snapshotInterval() const { return m_snapshotInterval.load(); }
  // The game's memory regions (thread-safe copy), taken after the game loaded and refreshed when the core re-sends its map (memoryRegionsChanged); empty when none or not running.
  std::vector<MemoryMap::RegionInfo> memoryRegions() const;

  // Diagnostics (thread-safe, cheap): measured frame timing of the emulation thread and how the core renders.
  FrameTimingStats::Snapshot timing() const;
  RenderInfo renderInfo() const { return m_backend->renderInfo(); }
  // Tests only: round every pacing sleep up to a multiple of this many ms (0 = off), simulating coarse OS timers.
  static std::atomic<int> sleepQuantumMsForTest;
  static qint64 monotonicMs();  // the clock of timing()

 signals:
  void started(const uzume::emu::AvInfo& av, const uzume::emu::CoreInfo& core);
  void startFailed(const QString& error);
  // Frame in QImage::Format_RGB32 (XRGB8888); implicitly shared, do not modify.
  // lateNs: during speed-up how long after its base-rate slot the frame is delivered (0 at normal speed); the Session
  // backdates its timestamp by it so the stream stays evenly spaced.
  void frameReady(const QImage& frame, quint64 frameNumber, qint64 lateNs);
  // Interleaved stereo int16, native endian; sampleRate in Hz (from the core).
  void audioReady(const QByteArray& pcm, int sampleRate);
  void stateChanged(uzume::emu::EmulationRunner::State state);
  // The core shut itself down or runFrame failed; the thread winds down (then stop()).
  void errorOccurred(const QString& message);
  // A save problem known while the game runs (e.g. the save file cannot be read, so this game will not be saved).
  void saveProblem(const QString& message);
  void stopped();
  // A queued command finished (emulation thread; queued to receivers elsewhere). See submit().
  void commandFinished(const uzume::emu::EmulationRunner::CommandResult& result);
  // Rewinding: seconds of play that can still be stepped back (emulation thread, at most 4 times a second while the key is held).
  void rewindProgress(double secondsLeft);
  // The rewind key was released or the buffer ran out (`exhausted`).
  void rewindFinished(bool exhausted);
  // The core re-sent its memory map while running; memoryRegions() now returns the new list.
  void memoryRegionsChanged();

 private:
  class Worker;
  void setState(State s);

  std::unique_ptr<EmulatorBackend> m_backend;
  QString m_finalSaveError;
  bool m_abandoned = false;  // the emulation thread did not stop in time and was left running (the backend must outlive it)
  std::unique_ptr<Worker> m_worker;
  std::atomic<State> m_state{State::Idle};
  FrameTimingStats m_timing;
  std::atomic<bool> m_fastForward{false};
  std::atomic<double> m_ratio{kDefaultSpeedUpRatio};
  std::atomic<bool> m_speedUpAudio{true};
  std::shared_ptr<MemoryWatchSet> m_memoryWatch = std::make_shared<MemoryWatchSet>();
  std::atomic<int> m_snapshotInterval{1};
  mutable QMutex m_regionsMutex;
  std::vector<MemoryMap::RegionInfo> m_regions;
  std::atomic<quint64> m_nextRequestId{1};
  std::atomic<int> m_rewindSeconds{0};
  std::atomic<bool> m_rewinding{false};
  std::atomic<int> m_runAhead{0};
  mutable QMutex m_rewindMutex;
  RewindInfo m_rewindInfo;
  mutable QMutex m_cheatsMutex;
  QList<CheatEntry> m_activeCheats;
};

}  // namespace uzume::emu

Q_DECLARE_METATYPE(uzume::emu::AvInfo)
Q_DECLARE_METATYPE(uzume::emu::CoreInfo)
Q_DECLARE_METATYPE(uzume::emu::EmulationRunner::State)
Q_DECLARE_METATYPE(uzume::emu::EmulationRunner::CommandResult)
