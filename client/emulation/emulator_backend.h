#pragma once
// EmulatorBackend: core-agnostic interface between Uzume and the emulator.
// Externally a uniform format applies: video = QImage::Format_RGB32 (XRGB8888),
// audio = interleaved stereo int16 (L,R,L,R,... in native byte order).
//
// Threading: all methods except those marked "thread-safe" must be called from the same thread
// (in the player, the emulation thread; see EmulationRunner).

#include <QByteArray>
#include <functional>
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <memory>

#include "gpu_display_target.h"
#include "gpu_encode_target.h"
#include "memory_map.h"

namespace uzume::emu {

struct CoreInfo {
  QString name;
  QString version;
  QStringList extensions;  // normalized: lowercase, with dot (".nds")
  bool needFullpath = false;
};

struct AvInfo {
  int width = 0;  // base geometry of the core; actual frame size is in the QImage
  int height = 0;
  double aspectRatio = 0.0;
  double fps = 0.0;
  double sampleRate = 0.0;  // Hz, as specified by the core
};

// How the core renders (diagnostics overlay, ADR 0013): hardware (OpenGL) or software, and why not.
struct RenderInfo {
  bool hwRequested = false;  // the core asked for a hardware context (OpenGL or another API)
  bool hwActive = false;     // an OpenGL context is in use
  QString api;               // active hardware API, e.g. "OpenGL 4.6 Core"; empty for software
  QString gpu;               // "<renderer> · Driver <version>"; empty for software
  QString fallbackReason;    // set when hardware was requested but software runs, see kFallback* below
  bool vulkan = false;       // hwActive runs on Vulkan (readback only: no GL texture targets, ADR 0034)
  QString vulkanFallback;    // Vulkan was requested but could not start, so OpenGL (or software) runs: why, e.g. "Vulkan 1.3 needed, 1.1 available"
};
inline constexpr char kFallbackDisabled[] = "disabled by UZUME_DISABLE_HW_RENDER";
inline constexpr char kFallbackNoContext[] = "no OpenGL 3.3 context";
inline constexpr char kFallbackNoFramebuffer[] = "framebuffer creation failed";
inline constexpr char kFallbackUnsupported[] = "core asked for an unsupported context";
inline constexpr char kFallbackNoVulkan[] = "no Vulkan device";

// Values correspond to RETRO_DEVICE_ID_JOYPAD_*; bitmask = 1u << value.
enum class JoypadButton : unsigned {
  B = 0, Y, Select, Start, Up, Down, Left, Right, A, X, L, R, L2, R2, L3, R3
};
constexpr quint32 buttonMask(JoypadButton b) { return 1u << static_cast<unsigned>(b); }

struct CoreOptionValue {
  QString value;
  QString label;  // empty = show value
};

struct CoreOptionCategory {
  QString key;
  QString description;
  QString info;
};

struct CoreOption {
  QString key;
  QString description;
  QString info;
  QString categoryKey;  // empty = no category
  QList<CoreOptionValue> values;
  QString defaultValue;
  QString currentValue;
  bool visible = true;
};

class EmulatorBackend {
 public:
  virtual ~EmulatorBackend() = default;

  // Load/unload core. Error text (for diagnostics) in *error.
  virtual bool loadCore(const QString& libraryPath, QString* error = nullptr) = 0;
  virtual void unloadCore() = 0;
  virtual bool isCoreLoaded() const = 0;
  virtual CoreInfo coreInfo() const = 0;  // valid after loadCore

  // Set directories BEFORE loadCore (system = BIOS/firmware, save = save games); cores like
  // melonDS DS already read them in retro_set_environment.
  virtual void setSystemDirectory(const QString& path) = 0;
  virtual void setSaveDirectory(const QString& path) = 0;

  // The core only works with hardware rendering (OpenGL Core >= 3.3, e.g. Azahar): loadGame() then fails with a clear
  // error instead of letting the core run in an unusable software mode. Call BEFORE loadGame.
  virtual void setRequireHwRender(bool required) { Q_UNUSED(required); }
  // Renderer the user prefers for hardware-rendered cores: "opengl" (default) or "vulkan" (ADR 0034). It decides what
  // GET_PREFERRED_HW_RENDER answers; a core that asks for a specific API is served that API when it is available.
  // UZUME_RENDERER overrides it. Call BEFORE loadCore. Default: ignored.
  virtual void setPreferredRenderer(const QString& renderer) { Q_UNUSED(renderer); }
  // Vulkan was requested for this game but could not start (ADR 0034): the core asked for an API version the system lacks,
  // no device exists, or the device could not be created. Set by loadCore/loadGame, valid after a failed loadGame (until
  // the next loadCore). Default: never.
  virtual bool vulkanAttemptFailed() const { return false; }
  virtual QString vulkanFailureReason() const { return {}; }
  // Switches Vulkan off for this backend (a retry after a failed Vulkan attempt): the core is served OpenGL only, and the
  // render info says `reason` as RenderInfo::vulkanFallback. Empty re-enables Vulkan. Call BEFORE loadCore.
  virtual void setVulkanFallbackNote(const QString& reason) { Q_UNUSED(reason); }

  virtual bool loadGame(const QString& path, QString* error = nullptr) = 0;
  virtual void unloadGame() = 0;
  virtual bool isGameLoaded() const = 0;
  virtual AvInfo avInfo() const = 0;  // valid after loadGame

  // Whether the NEXT runFrame()'s video will be shown (emulation thread, call before runFrame). false lets the
  // backend skip the expensive video work (hardware readback, pixel conversion) and tell the core video is off;
  // videoFrame() then keeps the last frame. Audio is always produced. Default: always wanted.
  virtual void setVideoWanted(bool wanted) { Q_UNUSED(wanted); }
  // Emulate exactly one frame; then videoFrame()/takeAudio(). false = error/core shutdown.
  virtual bool runFrame() = 0;
  virtual void reset() = 0;
  // Writes the game's battery save (if any) to the save directory when it changed. Called on pause and
  // before unloading; default: nothing. false = the file could not be written (a changed save is still unsaved).
  virtual bool flushSave() { return true; }
  // Why the last flushSave failed (file name and reason); empty when the last flush succeeded (still readable after
  // unloadGame, so the owner can report a failed final write). Any thread once the emulation thread has finished.
  virtual QString lastSaveError() const { return {}; }
  // A save problem that will cost progress and is known while the game runs (for example a save file that cannot be
  // read at start, so nothing is written over it): `fn(message)` is called at once, on the emulation thread. Set before
  // loadGame; default: never called.
  virtual void setSaveProblemNotifier(std::function<void(const QString&)> fn) { Q_UNUSED(fn); }
  // Size of the core's battery save memory, -1 when unknown/none (emulation thread).
  virtual qint64 saveMemorySize() const { return -1; }
  // The game's memory as device virtual addresses (SET_MEMORY_MAPS, or SYSTEM_RAM at 0 as fallback). Emulation thread
  // only, valid while the game is loaded and not across frames being run concurrently; nullptr = no memory access.
  virtual const MemoryMap* memoryMap() const { return nullptr; }
  // Increments whenever memoryMap() was replaced (a core may re-send SET_MEMORY_MAPS during retro_run). Emulation thread.
  virtual uint64_t memoryMapGeneration() const { return 0; }
  // Called on the GUI thread right before the emulation thread starts (e.g. to create resources that only
  // the GUI thread may create, such as an offscreen surface for hardware rendering).
  virtual void prepareForStart() {}

  // Save states (retro_serialize*; emulation thread, game loaded). supportsStates() is false when the core lacks the
  // symbols. serializeSize() is 0 when serializing is not possible right now (no game, a core that needs a frame first).
  // serialize()/unserialize() are exact: a state loaded into the same core build and game continues deterministically.
  virtual bool supportsStates() const { return false; }
  virtual qint64 serializeSize() { return 0; }
  virtual bool serialize(QByteArray* out) { Q_UNUSED(out); return false; }
  virtual bool unserialize(const QByteArray& state) { Q_UNUSED(state); return false; }

  // Cheats (retro_cheat_reset / retro_cheat_set; emulation thread). A core without the symbols has no cheats. `code`
  // is passed to the core unchanged; the format is the core's own (see the cheat formats of the core profile).
  virtual bool supportsCheats() const { return false; }
  virtual void cheatReset() {}
  virtual void cheatSet(unsigned index, bool enabled, const QString& code) { Q_UNUSED(index); Q_UNUSED(enabled); Q_UNUSED(code); }

  // Last video frame (XRGB8888). Implicitly shared, cheap to copy.
  virtual QImage videoFrame() const = 0;
  virtual quint64 frameCount() const = 0;
  // Audio samples produced since the last call (interleaved stereo int16).
  virtual QByteArray takeAudio() = 0;
  // Diagnostics (thread-safe). Time the last runFrame() spent reading a hardware frame back; 0 for software.
  virtual double lastReadbackMs() const { return 0.0; }
  virtual RenderInfo renderInfo() const { return {}; }

  // Session encoding from the GPU (ADR 0019). thread-safe: the target is taken over by the emulation thread at the start
  // of the next runFrame (nullptr removes it). A backend that renders on the GPU then also hands each shown frame to the
  // target as a GL texture; software backends ignore it. Default: ignored.
  virtual void setGpuEncodeTarget(std::shared_ptr<GpuEncodeTarget>) {}
  // Diagnostics (thread-safe). Time the last runFrame() spent on the encode texture (blit + capture); 0 without a target.
  virtual double lastGpuCopyMs() const { return 0.0; }
  // Whether the last runFrame() captured a frame for the target.
  virtual bool lastGpuCaptured() const { return false; }

  // Zero-copy display (ADR 0027; thread-safe): the target is taken over by the emulation thread at the start of the next
  // runFrame (nullptr removes it). A backend that renders on the GPU then blits each shown frame into the textures the
  // target hands out, independent of the readback. Software backends ignore it. Default: ignored.
  virtual void setGpuDisplayTarget(std::shared_ptr<GpuDisplayTarget>) {}
  // Largest size of the display blit (thread-safe, applied on the next frame); empty = no limit. Like setReadbackLimit.
  virtual void setDisplayLimit(const QSize& maxSize) { Q_UNUSED(maxSize); }
  // Whether hardware frames are still read back into videoFrame() (thread-safe, default true). Off: videoFrame() keeps
  // its last image and frames are only shown through the display target; the frame is still "delivered" (frameCount,
  // sourceFrameSize). The caller turns it off only while a display target shows every frame.
  virtual void setReadbackEnabled(bool on) { Q_UNUSED(on); }
  // Diagnostics (thread-safe). Time the last runFrame() spent on the display blit, and whether it presented a frame.
  virtual double lastDisplayMs() const { return 0.0; }
  virtual bool lastDisplayed() const { return false; }

  // Readback size limit (thread-safe, applied on the next frame): the largest width/height the consumers (display,
  // Session encoder) need. A backend that reads frames back from the GPU downscales larger ones on the GPU first
  // (aspect kept, never upscaled); empty or a non-positive component = no limit. Software frames are never scaled.
  virtual void setReadbackLimit(const QSize& maxSize) { Q_UNUSED(maxSize); }
  // Diagnostics (thread-safe): size of the last frame as the core rendered it, before any readback downscaling;
  // empty when unknown (then videoFrame().size() is the source size).
  virtual QSize sourceFrameSize() const { return {}; }

  // Fast-forward (thread-safe). libretro has no capability flag: it is a frontend feature, unless the core forbids
  // it (SET_FASTFORWARDING_OVERRIDE with inhibit_toggle). setFastForwarding only stores a flag the core can query.
  virtual bool supportsFastForward() const { return false; }
  // `ratio` = the speed multiple the frontend runs at while on (reported to the core via GET_THROTTLE_STATE).
  virtual void setFastForwarding(bool on, double ratio) { Q_UNUSED(on); Q_UNUSED(ratio); }

  // thread-safe: input state, picked up on the next runFrame.
  virtual void setJoypadState(unsigned port, quint32 buttonMask) = 0;
  // Pointer/touch in normalized coordinates (0..1) relative to the whole video frame.
  virtual void setPointer(double x, double y, bool pressed) = 0;

  // Core options: reported dynamically by the core (libretro core options v0/v1/v2).
  virtual QList<CoreOption> coreOptions() const = 0;                 // thread-safe
  virtual QList<CoreOptionCategory> coreOptionCategories() const = 0;  // thread-safe
  // thread-safe. Values set before loadGame are remembered and applied at start.
  // false: option known but value invalid, or option unknown after registration.
  virtual bool setCoreOption(const QString& key, const QString& value) = 0;
};

}  // namespace uzume::emu
