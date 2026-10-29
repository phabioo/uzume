#pragma once
// LibretroBackend: loads a libretro core via QLibrary (dlopen/LoadLibrary, no linking).
//
// Limitation: libretro cores have global state and a C API without an instance handle.
// Therefore only ONE LibretroBackend per process may have a core loaded; loadCore() of a
// second backend fails with an error message until the first has called unloadCore().
//
// Hardware rendering (ADR 0013): SET_HW_RENDER with OpenGL / OpenGL Core is served by an offscreen context
// owned by the emulation thread (hw_render.h); every hardware frame is read back into the XRGB8888 frame.
// Without a QGuiApplication, with UZUME_DISABLE_HW_RENDER=1 or when the context fails, SET_HW_RENDER
// returns false (the core falls back to software). Other APIs are rejected. Unsupported environment
// callbacks are answered with false.

#include <QLibrary>
#include <QMap>
#include <QMutex>

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "emulator_backend.h"

namespace uzume::emu {

class HwRenderContext;
class VkRenderContext;

class LibretroBackend final : public EmulatorBackend {
 public:
  LibretroBackend();
  ~LibretroBackend() override;
  Q_DISABLE_COPY(LibretroBackend)

  bool loadCore(const QString& libraryPath, QString* error = nullptr) override;
  // Error text of loadCore() while an abandoned (hung) core still occupies the process. A stable code for the UI and the MCP server.
  static inline const QString kPreviousGameHungCode = QStringLiteral("previous_game_hung");
  // Called by the runner when it left a hung emulation thread behind. Libretro cores keep global state and the callbacks are static, so
  // no other core can be loaded until that thread ends (unloadCore clears the mark); the person has to restart the app.
  static void markCoreAbandoned() { s_abandoned.store(true); }
  static bool previousCoreHung() { return s_abandoned.load() && s_active.load() != nullptr; }
  void unloadCore() override;
  bool isCoreLoaded() const override;
  // True if the core declared RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME (reset by loadCore).
  bool supportsNoGame() const { return m_supportsNoGame; }
  CoreInfo coreInfo() const override;

  void setSystemDirectory(const QString& path) override;
  void setSaveDirectory(const QString& path) override;

  void setRequireHwRender(bool required) override { m_requireHw = required; }
  void setPreferredRenderer(const QString& renderer) override { m_preferredRenderer = renderer; }
  bool vulkanAttemptFailed() const override { return m_vkFailed && !m_openglTookOver; }  // a failed load is Vulkan's fault only then
  QString vulkanFailureReason() const override { return m_vkFailReason; }
  void setVulkanFallbackNote(const QString& reason) override { m_vkNote = reason; }
  bool loadGame(const QString& path, QString* error = nullptr) override;
  void unloadGame() override;
  bool isGameLoaded() const override;
  AvInfo avInfo() const override;

  void setVideoWanted(bool wanted) override { m_videoWanted.store(wanted); }
  bool runFrame() override;
  void reset() override;
  // Save states and cheats (optional symbols; a missing one means no support). Serialization quirks the core declared
  // (RETRO_SERIALIZATION_QUIRK_*): MUST_INITIALIZE blocks serializing before the first frame ran.
  bool supportsStates() const override;
  qint64 serializeSize() override;
  bool serialize(QByteArray* out) override;
  bool unserialize(const QByteArray& state) override;
  quint64 serializationQuirks() const { return m_serQuirks; }
  bool supportsCheats() const override;
  void cheatReset() override;
  void cheatSet(unsigned index, bool enabled, const QString& code) override;
  // Battery save (RETRO_MEMORY_SAVE_RAM) <-> <save dir>/<game basename>.sav: loaded after the game loads,
  // written when changed (about every 3 s while running, on pause and before unloading), atomically.
  bool flushSave() override;
  QString lastSaveError() const override { return m_lastSaveError; }
  void setSaveProblemNotifier(std::function<void(const QString&)> fn) override { m_saveProblemNotifier = std::move(fn); }
  qint64 saveMemorySize() const override;
  // GUI thread, before the emulation thread starts: creates the offscreen surface for hardware rendering.
  void prepareForStart() override;
  const MemoryMap* memoryMap() const override { return m_memMap.empty() ? nullptr : &m_memMap; }
  uint64_t memoryMapGeneration() const override { return m_memMapGen; }

  QImage videoFrame() const override;
  quint64 frameCount() const override;
  QByteArray takeAudio() override;
  double lastReadbackMs() const override;
  // ADR 0019: the target is parked here and applied by the emulation thread at the start of the next runFrame.
  void setGpuEncodeTarget(std::shared_ptr<GpuEncodeTarget> target) override;
  double lastGpuCopyMs() const override;
  // Zero-copy display (ADR 0027): parked here and applied by the emulation thread at the start of the next runFrame.
  void setGpuDisplayTarget(std::shared_ptr<GpuDisplayTarget> target) override;
  void setDisplayLimit(const QSize& maxSize) override { m_displayLimit.store(pack(maxSize)); }
  void setReadbackEnabled(bool on) override { m_readbackEnabled.store(on); }
  double lastDisplayMs() const override;
  bool lastDisplayed() const override { return m_lastDisplayed.load(); }
  bool lastGpuCaptured() const override { return m_lastGpuCaptured.load(); }
  // Diagnostics/tests: number of hardware frames read back from the GPU so far.
  quint64 hwReadbackCount() const { return m_hwReadbacks.load(); }
  RenderInfo renderInfo() const override;
  void setReadbackLimit(const QSize& maxSize) override {
    m_readbackLimit.store(pack(maxSize));
  }
  QSize sourceFrameSize() const override { return unpack(m_sourceSize.load()); }

  bool supportsFastForward() const override { return !m_ffInhibit.load(); }
  void setFastForwarding(bool on, double ratio) override {
    m_ffRatio.store(ratio);
    m_fastForwarding.store(on);
  }

  void setJoypadState(unsigned port, quint32 buttonMask) override;
  void setPointer(double x, double y, bool pressed) override;

  QList<CoreOption> coreOptions() const override;
  QList<CoreOptionCategory> coreOptionCategories() const override;
  bool setCoreOption(const QString& key, const QString& value) override;

  // The core requested shutdown via RETRO_ENVIRONMENT_SHUTDOWN.
  bool shutdownRequested() const { return m_shutdownRequested; }

 private:
  struct Api;
  struct InputState {
    quint32 joypad[2] = {0, 0};
    double px = 0.0;
    double py = 0.0;
    bool pressed = false;
  };

  // libretro callbacks (C-linkage compatible, no context -> static instance).
  static bool environmentCb(unsigned cmd, void* data);
  static void videoRefreshCb(const void* data, unsigned width, unsigned height, size_t pitch);
  static void audioSampleCb(int16_t left, int16_t right);
  static size_t audioBatchCb(const int16_t* data, size_t frames);
  static void inputPollCb();
  static int16_t inputStateCb(unsigned port, unsigned device, unsigned index, unsigned id);

  bool tryLoadSave();
  static QString backupStamp();
  bool handleEnvironment(unsigned cmd, void* data);
  bool setupHwRender(void* retroHwRenderCallback);
  bool finishHwSetup(unsigned maxWidth, unsigned maxHeight, QString* error);  // after retro_load_game
  void teardownHw();
  bool setupVulkan(void* retroHwRenderCallback);  // SET_HW_RENDER with RETRO_HW_CONTEXT_VULKAN
  bool vulkanAvailable();                          // cached probe, reset by loadCore
  bool vulkanPreferred() const;                    // UZUME_RENDERER, else the launch preference
  void applyTargetChange();  // emulation thread, context current

  void handleVideo(const void* data, unsigned width, unsigned height, size_t pitch);
  void registerOptionsV2(const void* options);
  void registerOptionsV1(const void* definitions);
  void registerOptionsV0(const void* variables);
  void setOptionVisible(const QString& key, bool visible);
  QString effectiveValue(const CoreOption& o) const;  // m_optMutex held

  static std::atomic<bool> s_abandoned;  // the backend in s_active belongs to an emulation thread that was left running
  static std::atomic<LibretroBackend*> s_active;  // the one backend of this process; claimed with compare_exchange in loadCore (PE-16)

  std::unique_ptr<Api> m_api;
  QLibrary m_lib;
  QString m_corePath;
  CoreInfo m_info;
  AvInfo m_av;
  bool m_coreLoaded = false;
  bool m_supportsNoGame = false;  // core declared SET_SUPPORT_NO_GAME(true)
  bool m_gameLoaded = false;
  bool m_shutdownRequested = false;
  quint64 m_serQuirks = 0;      // RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS
  int m_stateContext = 0;       // RETRO_SAVESTATE_CONTEXT_* answered to GET_SAVESTATE_CONTEXT (NORMAL: states go to disk/server)
  bool m_ranFrame = false;      // retro_run was called since the game loaded

  QByteArray m_systemDir;
  QByteArray m_saveDir;
  QString m_saveFilePath;
  QByteArray m_sramSnapshot;  // content last loaded/written
  unsigned m_framesSinceFlush = 0;
  bool m_savePendingLoad = false;  // save memory not yet exposed / file not yet applied
  QString m_lastSaveError;          // empty = the last flush succeeded
  int m_saveFailCount = 0;         // consecutive failed flushes (log rate limit)
  bool m_saveBlocked = false;      // existing file unreadable: never written
  std::function<void(const QString&)> m_saveProblemNotifier;
  // Last resort when the save file cannot be written: the SRAM goes to `<save>.unsaved-<stamp>`. Updates m_lastSaveError.
  void writeEmergencySave(const QByteArray& data, const QString& why);
  QByteArray m_corePathUtf8;
  QByteArray m_gameData;  // lives until unloadGame (the core may hold pointers)
  QByteArray m_gamePathUtf8;
  uint64_t m_memMapGen = 0;  // bumped on every replacement of m_memMap; emulation thread
  MemoryMap m_memMap;  // from SET_MEMORY_MAPS or the SYSTEM_RAM fallback; emulation thread
  QString m_loadedPath;  // path of the loaded game (for the live save reload)

  std::unique_ptr<HwRenderContext> m_hw;
  std::unique_ptr<VkRenderContext> m_vk;  // Vulkan next to OpenGL (ADR 0034); m_hwActive stays OpenGL-only
  bool m_vkActive = false;                // SET_HW_RENDER accepted with Vulkan (context created in finishHwSetup)
  bool m_openglTookOver = false;          // after a failed Vulkan attempt the core itself got OpenGL in the same load
  bool m_vkFailed = false;                // a Vulkan attempt of this load failed (reset by loadCore); GET_PREFERRED then says OpenGL
  QString m_vkFailReason;
  QString m_vkNote;                       // Vulkan is off for this backend (retry after a failure); also the diagnostics reason
  int m_vkProbe = -1;                     // -1 unknown, 0 none, 1 a Vulkan device exists
  QString m_preferredRenderer = QStringLiteral("opengl");
  // Core's hardware callbacks (copied from retro_hw_render_callback).
  void (*m_hwContextReset)() = nullptr;
  void (*m_hwContextDestroy)() = nullptr;
  bool m_requireHw = false;     // the core cannot run without a hardware context (loadGame fails otherwise)
  bool m_hwActive = false;      // SET_HW_RENDER accepted, context exists
  bool m_hwResetDone = false;   // context_reset called (context_destroy owed)
  bool m_hwBottomLeft = true;
  mutable QMutex m_renderMutex;
  RenderInfo m_render;                     // diagnostics, written on load/setup, read from the UI thread
  std::atomic<qint64> m_lastReadbackNs{0};
  // Session encode target (ADR 0019): handed over under m_targetMutex, the dirty flag is only a cheap pre-check.
  QMutex m_targetMutex;
  std::shared_ptr<GpuEncodeTarget> m_nextTarget;
  std::atomic<bool> m_targetChanged{false};
  std::shared_ptr<GpuDisplayTarget> m_nextDisplay;
  std::atomic<bool> m_displayChanged{false};
  std::atomic<qint64> m_lastDisplayNs{0};   // display blit of the last runFrame
  std::atomic<bool> m_lastDisplayed{false};
  std::atomic<bool> m_readbackEnabled{true};
  bool m_readbackWasEnabled = true;  // emulation thread: edge detection for discarding the pending readback
  std::atomic<qint64> m_lastGpuCopyNs{0};   // encode blit + capture of the last runFrame
  std::atomic<bool> m_lastGpuCaptured{false};
  std::atomic<bool> m_videoWanted{true};
  static quint64 pack(const QSize& s) {
    return (static_cast<quint64>(static_cast<quint32>((std::max)(0, s.width()))) << 32) | static_cast<quint32>((std::max)(0, s.height()));
  }
  static QSize unpack(quint64 v) { return QSize(static_cast<int>(v >> 32), static_cast<int>(v & 0xFFFFFFFFu)); }
  std::atomic<quint64> m_readbackLimit{0};  // packed QSize (0 = none)
  std::atomic<quint64> m_displayLimit{0};   // packed QSize (0 = none)
  std::atomic<quint64> m_sourceSize{0};     // packed QSize of the last frame as the core rendered it
  std::atomic<quint64> m_hwReadbacks{0};
  std::atomic<bool> m_fastForwarding{false};
  std::atomic<bool> m_ffInhibit{false};  // the core forbids toggling fast-forward
  std::atomic<double> m_ffRatio{1.0};    // speed multiple while fast-forwarding (chosen by the user)

  int m_pixelFormat = 0;  // RETRO_PIXEL_FORMAT_*
  QImage m_frame;
  quint64 m_frameCount = 0;
  QByteArray m_audio;

  mutable QMutex m_inputMutex;
  InputState m_input;   // written by the UI
  InputState m_polled;  // core thread, taken over at input_poll

  mutable QMutex m_optMutex;
  QList<CoreOption> m_options;
  QList<CoreOptionCategory> m_categories;
  QMap<QString, QString> m_overrides;  // requested values (also before registration)
  std::map<std::string, std::string> m_varCache;  // stable pointers for GET_VARIABLE
  bool m_optionsDirty = false;
};

}  // namespace uzume::emu
