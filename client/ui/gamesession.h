#pragma once

#include <functional>
// GameSession: a local emulation session (EmulationRunner + AudioOutput) for the game view.
// Holds the last frame, the keyboard state and the display profile for touch conversion.

#include <QHash>
#include <QImage>
#include <QMap>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <QtQml/qqmlregistration.h>
#include <memory>

#include "audiooutput.h"
#include "d3d11glbridge.h"
#include "gpudisplaybridge.h"
#include "emulation_runner.h"
#include "inputmapping.h"
#include "system_manifest.h"

namespace uzume::ui {

// Measurements of the running game for the diagnostics overlay (0.6 D6), assembled on the UI thread from the thread-safe
// values of the emulation runner and the audio output. `valid` = a game runs (or a preview was set).
struct EmulationDiagnostics {
  bool valid = false;
  QString core;                  // "melonDS DS 1.4.0"
  bool hwRequested = false;      // the core asked for a hardware (OpenGL) context
  bool hwActive = false;         // ... and it runs on it
  QString api;                   // "OpenGL 4.6 Core" while hwActive
  QString gpu;                   // "Example GPU · Driver 1.2.3" while hwActive
  QString vulkanFallback;        // Vulkan was requested but OpenGL runs: why (e.g. "Vulkan 1.3 needed, 1.1 available")
  QString fallbackReason;        // hardware requested but software runs: why (emu::kFallback*)
  QSize frameSize;               // size of the frame the core renders (all screens), before any readback downscale
  QSize readbackSize;            // size actually read back from the GPU when smaller than frameSize (else empty); empty while no readback runs
  QSize baseSize;                // size of the frame at 1x (system manifest)
  int screens = 0;
  QList<QSize> screenSizes;      // size of each screen at 1x (system manifest); they differ on the 3DS (400x240 top, 320x240 bottom)
  int requestedScale = 0;        // internal resolution the core option asks for (0 = no such option)
  int cpuThreads = 0;            // logical CPU threads (software renderer subline)
  double fps = 0;                // actual, sliding 1 s window; 0 while nothing runs (paused)
  double targetFps = 0;          // from the core
  double frameMs = 0;            // mean total frame time
  double emuMs = 0;              // ... of which the core
  double readbackMs = 0;         // ... of which GPU readback, mean over all frames (0 for software)
  double readbacksPerSec = 0;    // frames per second that were actually read back (skipped/unseen frames are not)
  double gpuCopyMs = 0;          // ... of which the Session encode texture (GPU-direct encoding, ADR 0019), mean over all frames
  double gpuCopiesPerSec = 0;    // frames per second that handed a frame to the Session encoder (0 without GPU-direct)
  double displayMs = 0;          // ... of which the zero-copy display blit (ADR 0027), mean over all frames (0 without)
  double displaysPerSec = 0;     // frames per second presented to the zero-copy display
  QSize displaySize;             // size of those frames when the zero-copy display is active and differs from frameSize
  bool zeroCopy = false;         // the game view shows GPU textures (display state Active)
  QVector<float> totalHistory;   // last 5 s, ms per frame
  QVector<float> emuHistory;
  bool audioActive = false;      // an audio output device is open
  double audioBufferMs = 0;
  int underruns = 0;
};

class GameSession : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")
  Q_PROPERTY(State state READ state NOTIFY stateChanged)
  Q_PROPERTY(QString title READ title NOTIFY titleChanged)
  Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)
  Q_PROPERTY(bool active READ isActive NOTIFY stateChanged)
  Q_PROPERTY(bool paused READ isPaused NOTIFY stateChanged)
  Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
  // Screens of the running system (system manifest) and the layouts the in-game switch offers for them (D12).
  Q_PROPERTY(int screenCount READ screenCount NOTIFY stateChanged)
  Q_PROPERTY(QStringList screenLayouts READ screenLayouts NOTIFY stateChanged)
  // Speed-up (fast-forward): offered when the running core allows it. The ratio is for the running game only.
  Q_PROPERTY(bool fastForwardAvailable READ fastForwardAvailable NOTIFY fastForwardChanged)
  Q_PROPERTY(bool fastForward READ fastForward WRITE setFastForward NOTIFY fastForwardChanged)
  Q_PROPERTY(double fastForwardRatio READ fastForwardRatio WRITE setFastForwardRatio NOTIFY fastForwardChanged)
  Q_PROPERTY(QVariantList speedUpRatios READ speedUpRatios CONSTANT)
 public:
  enum State { Idle, Starting, Running, Paused, Failed };
  Q_ENUM(State)

  struct LaunchConfig {
    QString title;
    QString corePath;
    QString gamePath;
    QString systemDir;
    QString saveDir;
    QMap<QString, QString> coreOptions;
    bool requireHwRender = false;  // core profile: no start without hardware rendering
    QMap<QString, QString> openglFallbackOptions;  // core options for the OpenGL retry when Vulkan cannot start (empty = no retry)
    QString preferredRenderer;     // uzume.renderer resolved game > system > global: "opengl" | "vulkan" (ADR 0034)
    emu::DisplayProfile display;
    // Speed-up settings (uzume.speedup_*), resolved game > system > global by the caller.
    double speedUpRatio = emu::EmulationRunner::kDefaultSpeedUpRatio;
    bool speedUpOnStart = false;
    bool speedUpAudio = true;
  };

  explicit GameSession(QObject* parent = nullptr);
  ~GameSession() override;

  State state() const { return state_; }
  QString title() const { return title_; }
  QString errorText() const { return error_; }
  bool isActive() const { return state_ == Starting || state_ == Running || state_ == Paused; }
  bool isPaused() const { return state_ == Paused; }
  bool hasFrame() const { return !frame_.isNull(); }
  QImage frame() const { return frame_; }
  quint64 frameNumber() const { return frameNr_; }
  // Speed-up: how long after its base-rate slot the current frame arrived (0 at normal speed); see SessionHost::pushFrame().
  qint64 frameLateNs() const { return frameLateNs_; }
  const emu::DisplayProfile& displayProfile() const { return display_; }
  const AudioOutput& audio() const { return audio_; }
  int screenCount() const { return static_cast<int>(display_.screens.size()); }
  QStringList screenLayouts() const;

  bool fastForwardAvailable() const { return fastForwardSupported_; }
  bool fastForward() const { return fastForward_; }
  double fastForwardRatio() const { return ffRatio_; }
  void setFastForwardRatio(double ratio);
  static QVariantList speedUpRatios();
  void setFastForward(bool on);
  Q_INVOKABLE void toggleFastForward() { setFastForward(!fastForward_); }
  // Tests and screenshots: pretend the core supports fast-forward (preview only).
  void setPreviewFastForwardSupported(bool supported);

  // Player hotkeys (configured keys, default F11/F3/F5/Space/F7) and Escape belong to the Player: never mapped to a joypad
  // button, never forwarded. The hotkey keys are set by the PlayerController from the Controllers settings.
  bool isReservedKey(int qtKey) const;
  void setHotkeyKeys(const QSet<int>& keys) { hotkeyKeys_ = keys; }
  // Key combinations (Qt::Key | modifiers, QKeyCombination::toCombined) that also belong to the Player: active plugin
  // hotkeys and Alt+1 ... Alt+9. They are never mapped to a joypad button.
  void setReservedCombos(const QSet<int>& combos) { reservedCombos_ = combos; }
  static int comboOf(int qtKey, Qt::KeyboardModifiers mods);

  // Diagnostics overlay: thread-safe reads of the emulation thread's measurements, UI thread only.
  EmulationDiagnostics diagnostics() const;
  // Tests and screenshots: pretend a running game without a core (title, frame, profile) and fixed measurements.
  void setPreview(const QString& title, const QImage& frame, const emu::DisplayProfile& profile, const EmulationDiagnostics& diag);

  // Readback size limit (hardware frames are downscaled on the GPU before readback, see EmulatorBackend). Each view
  // reports the physical pixel size it needs for the frame (empty removes it); the limit is the component-wise
  // maximum over all views and, while the own Session is shared, the encoder's size. No view = no limit.
  void setViewSize(const QObject* view, const QSize& pixels);
  void setShareSize(const QSize& pixels);
  // A view that always draws the CPU frame (the bottom-screen window): while one exists the readback keeps running even
  // with zero-copy display active, at the size those views need (0.19 package 6).
  void setCpuConsumer(const QObject* view, bool on);
  QSize readbackLimit() const { return limit_; }

  // GPU-direct Session encoding (ADR 0019): the target for the Session encode texture of the running game, nullptr =
  // none. Stored like the readback limit and handed to every runner this session creates (start() and the live-save
  // restart), so a restart re-attaches it. teardown() leaves it detached: unloading the game detaches it with GL current.
  void setGpuEncodeTarget(std::shared_ptr<emu::GpuEncodeTarget> target);
  // Zero-copy display (ADR 0027): the platform display of the running game (makePlatformDisplay at every game launch; a
  // live-save restart keeps a failed/unavailable one, else makes a new one for the new GL context). The state decides the
  // readback (planReadback); a Pending/Active one hands the GameViews their texture source.
  DisplayTextureSource* displaySource() const;  // render thread (GUI thread blocked in updatePaintNode); null unless Pending/Active
  void confirmDisplayShown();                   // render thread: a GameView drew the GPU texture
  GpuDisplayBridge::State displayState() const;  // Unavailable without a platform display
  QString displayReason() const;
  // The GL-D3D11 bridge of the display (real platform factory only; null otherwise, after the display went away, and on
  // non-Windows). Shared with the D3D11 encode capture (ADR 0027 D6). Set by setupDisplay() before the runner starts.
  std::shared_ptr<GlD3d11Bridge> displayBridge() const;
  bool displayReadbackEnabled() const { return readbackOn_; }
  // Tests: replaces makePlatformDisplay() for the next game launches (empty function = the real one).
  static void setPlatformDisplayFactoryForTests(std::function<PlatformDisplay(QString*)> factory);
  // The game runs on an OpenGL context (a software core, a preview, a Vulkan game or no game: false). Gates GPU-direct
  // Session encoding only; the zero-copy display target (setupDisplay) serves OpenGL and Vulkan games alike.
  bool hardwareRendered() const;

  void start(const LaunchConfig& config);

  // Plugins (video.capture): size of the frame the core renders, before any readback downscale (empty without a runner).
  QSize sourceFrameSize() const;
  // Hands the next frame at that native size to `done` (GUI thread, exactly once): the readback limit is lifted and the
  // readback switched on only while a request is waiting, so there is no per-frame cost otherwise. `error` is empty on
  // success, else "no_frame" (no game, stopped, or no frame within 2 s).
  void requestNativeFrame(std::function<void(const QImage& frame, const QString& error)> done);

  // Plugins: the memory watch set of the running core (null without a runner) and the memory regions the core exposes
  // (empty without a game or memory map). A live-save restart creates a new runner: memoryWatchReplaced() then fires so the
  // plugin memory adapter can attach to the new set.
  std::shared_ptr<emu::MemoryWatchSet> memoryWatch() const { return runner_ ? runner_->memoryWatch() : nullptr; }
  std::vector<emu::MemoryMap::RegionInfo> memoryRegions() const { return runner_ ? runner_->memoryRegions() : std::vector<emu::MemoryMap::RegionInfo>(); }

  // Rewind and run-ahead (0.20 package 4). The values are kept here and handed to every runner this session creates (a live-save
  // restart makes a new one). `seconds` 0 = rewind off; run-ahead 0..2 frames. setRewinding(true) = the key is held.
  void setRewindSeconds(int seconds);
  int rewindSeconds() const { return rewindSeconds_; }
  void setRunAhead(int frames);
  int runAhead() const { return runAhead_; }
  void setRewinding(bool on);
  bool rewinding() const { return rewinding_; }
  emu::EmulationRunner::RewindInfo rewindInfo() const { return runner_ ? runner_->rewindInfo() : emu::EmulationRunner::RewindInfo(); }

  // Debug access (MCP server, ADR 0032): runs `fn` on the emulation thread between frames, also while paused, and waits
  // (GUI thread). false: no game on a core, or the thread did not get to it within `timeoutMs`.
  bool runOnEmuThread(std::function<void(emu::EmulatorBackend&)> fn, int timeoutMs = 3000);
  // Emulation command queue (save state, load state, cheats, memory write; see EmulationRunner::submit): async, applied
  // between two frames in submit order. Returns the request id, 0 when no game runs. `onDone` runs on the emulation thread.
  quint64 submitEmuCommand(emu::EmulationRunner::Command cmd, std::function<void(const emu::EmulationRunner::CommandResult&)> onDone = {});
  // Paused game only: runs exactly `frames` frames (1..emu::EmulationRunner::kMaxStepFrames) and stays paused. Returns the
  // frame count afterwards (the last frame arrives through frameChanged), -1 when not possible.
  qint64 stepFrames(int frames);

  Q_INVOKABLE void pause();
  Q_INVOKABLE void resume();
  Q_INVOKABLE void togglePause();
  Q_INVOKABLE void reset();
  Q_INVOKABLE void stop();  // blocks until the core is unloaded

  // Live save (saves view in the game): all block the GUI thread until the emulation thread did it (a few ms).
  bool liveSaveReady() const { return runner_ && (state_ == Running || state_ == Paused); }
  bool liveSaveAccepts(qint64 size);                 // the core's battery save memory has exactly this size
  void flushLiveSave();                              // the save file is current (the core flushes about every 3 s)
  // Replaces the battery save by restarting the game (stop = flush old save, write `saveFile`, start again like a normal start).
  // Blocks until the old core is unloaded; the new core starts asynchronously (state Starting, then Running; no new started()).
  // false = the file could not be written or the game could not be restarted (the game then runs with the old save).
  bool restartWithSave(const QString& saveFile, const QByteArray& data);
  // Same restart for saves that are more than one file (3DS tree): `writer` runs between the stop and the start.
  bool restartWith(const std::function<bool()>& writer);

  // Input (UI thread). Returns true if the key is mapped.
  bool keyEvent(int qtKey, bool pressed, Qt::KeyboardModifiers mods = Qt::NoModifier);
  void releaseAllKeys();
  // Keyboard map of the keyboard profile (Qt::Key -> joypad mask); gamepad = joypad mask of P1. Both are merged.
  void setKeyboardMap(const QHash<int, quint32>& map);
  void setGamepadMask(quint32 mask);
  quint32 joypadMask() const { return inputBlocked_ ? 0 : (keys_.mask() | pad_ | debugPad_); }
  // Debug input (MCP server): libretro mask OR-ed on top of keyboard and gamepad without touching either.
  void setDebugPad(quint32 mask);
  // Game in the background (Library shown): no keyboard, gamepad or pointer input reaches the core.
  void setInputBlocked(bool blocked);
  bool inputBlocked() const { return inputBlocked_; }
  void setPointer(const QPointF& frameNormalized, bool pressed);
  // Multiview: exactly one surface is audible. A muted session keeps running, its audio is dropped.
  void setAudioMuted(bool muted);
  bool audioMuted() const { return audioMuted_; }
  // Master volume of this PC (AudioController): software gain 0..1 on what this app plays. The Session share is unaffected.
  void setMasterGain(double gain) { audio_.setVolume(gain); }
  void setAudioDevice(const QByteArray& id) { audio_.setDeviceId(id); }  // applies on the next start

 signals:
  void stateChanged();
  void displayStateChanged();            // zero-copy display state changed (readback plan re-applied)
  void fastForwardChanged();
  void titleChanged();
  void errorChanged();
  void frameChanged();
  void started();                       // core and game are running
  void startFailed(const QString& msg);  // failed before the first frame
  void saveProblem(const QString& message);  // known while running: this game's progress cannot be saved (e.g. an unreadable save file)
  void saveWriteFailed();                // an explicit save flush failed: the save file may be stale
  void stopSaveFailed(const QString& reason);  // the final battery save write at stop failed: the latest progress may be lost
  void finished();                       // after stop()
  void rewindProgress(double secondsLeft);  // rewinding: seconds of play that can still be stepped back
  void rewindFinished(bool exhausted);      // the key was released or the buffer ran dry
  void memoryRegionsChanged();           // the core re-sent its memory map: memoryRegions() changed
  void memoryWatchReplaced();            // a new runner (live-save restart) has a new memory watch set
  void audioChunk(const QByteArray& pcm, int sampleRate);  // core audio (int16 stereo), also while muted (Session share)

 private:
  void reportStopSaveError();
  QString stopSaveError_;  // set by teardown() when the final save write failed
  void setState(State s);
  void launch(const LaunchConfig& config, bool restart);
  void fail(const QString& msg);
  void teardown();
  void applyJoypad();
  void updateReadbackLimit();
  void setupDisplay(bool restart);
  void onDisplayStateChanged();

  std::unique_ptr<emu::EmulationRunner> runner_;
  quint64 runnerGen_ = 0;  // bumped per runner and by teardown(): events of a runner that is gone are dropped (PE-6)
  AudioOutput audio_;
  KeyboardJoypad keys_;
  QSet<int> hotkeyKeys_{Qt::Key_F11, Qt::Key_F3, Qt::Key_F5, Qt::Key_Space, Qt::Key_F7};
  QSet<int> reservedCombos_;
  quint32 pad_ = 0;
  quint32 debugPad_ = 0;
  bool inputBlocked_ = false;
  emu::DisplayProfile display_;
  QHash<const QObject*, QSize> viewSizes_;
  QSet<const QObject*> cpuViews_;
  QSize shareSize_;
  QSize limit_;
  QVector<std::function<void(const QImage&, const QString&)>> captureWaiters_;
  void serveCaptures(bool timedOut);
  std::shared_ptr<emu::GpuEncodeTarget> gpuTarget_;
  std::shared_ptr<GpuDisplayBridge> gpuDisplay_;  // per game launch; null without a platform display
  std::weak_ptr<GlD3d11Bridge> displayBridge_;     // the display's bridge, owned by gpuDisplay_
  QSize displayLimit_;
  bool readbackOn_ = true;
  QImage frame_;
  quint64 frameNr_ = 0;
  qint64 frameLateNs_ = 0;
  QString title_;
  QString error_;
  State state_ = Idle;
  bool startedEmitted_ = false;
  bool restarting_ = false;  // a live-save restart is starting the core again
  LaunchConfig config_;
  bool audioMuted_ = false;
  int rewindSeconds_ = 0;
  int runAhead_ = 0;
  bool rewinding_ = false;
  bool fastForwardSupported_ = false;
  bool fastForward_ = false;
  double ffRatio_ = emu::EmulationRunner::kDefaultSpeedUpRatio;
  bool ffAudio_ = true;
  void updateUnderrunCounting();
  QString coreName_;
  double targetFps_ = 0;
  QMap<QString, QString> coreOptions_;  // as launched (requested internal resolution)
  bool preview_ = false;
  EmulationDiagnostics previewDiag_;
};

}  // namespace uzume::ui
