#include "libretro_backend.h"
#include "envvar.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QThread>
#include <QFileInfo>
#include <QLoggingCategory>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <QHash>
#include <mutex>
#include <cstdio>
#include <cstring>

#include "hw_render.h"
#include "vk_render.h"
#include "third_party/libretro/libretro.h"

namespace uzume::emu {

namespace {
constexpr int kSaveOpenAttempts = 5;      // an existing save file is opened up to 5 times ...
constexpr unsigned kSaveOpenRetryMs = 300;  // ... 300 ms apart (about 1.2 s) before saving is blocked
Q_LOGGING_CATEGORY(lcCore, "uzume.emulation.core")

// The core repeats the same lines for every frame (OSD texts via SET_MESSAGE, debug lines): only the first
// occurrence of each distinct message is reported, then every 1000th (with the count). false = suppress.
// A core whose text keeps changing (counters, timestamps) produces endless "new" lines: at most kMaxNewPerSecond distinct new
// lines are reported per second, the rest is counted and announced with the next reported line. The table of known
// messages forgets the oldest entry when full instead of starting over, so repeats stay suppressed (PE-14).
bool dedupeLog(QString& msg) {
  constexpr int kMaxKnown = 256;
  constexpr int kMaxNewPerSecond = 20;
  static std::mutex m;
  static QHash<QString, int> seen;
  static QList<QString> order;  // insertion order, oldest first
  static qint64 windowStartMs = 0;
  static int windowNew = 0;
  static int suppressed = 0;
  std::lock_guard<std::mutex> lock(m);
  if (auto it = seen.find(msg); it != seen.end()) {
    const int n = it.value()++;
    if (n == 0 || n % 1000 != 0) return false;
    msg += QStringLiteral(" (repeated %1 times)").arg(n);
    return true;
  }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - windowStartMs >= 1000) {
    windowStartMs = now;
    windowNew = 0;
  }
  if (windowNew >= kMaxNewPerSecond) {
    ++suppressed;
    return false;
  }
  ++windowNew;
  if (seen.size() >= kMaxKnown && !order.isEmpty()) seen.remove(order.takeFirst());
  seen.insert(msg, 1);
  order.append(msg);
  if (suppressed > 0) {
    msg += QStringLiteral(" (%1 other core messages suppressed)").arg(suppressed);
    suppressed = 0;
  }
  return true;
}

void RETRO_CALLCONV coreLog(enum retro_log_level level, const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
  va_end(ap);
  QString msg = QString::fromUtf8(buf).trimmed();
  if (msg.isEmpty()) return;
  if (!dedupeLog(msg)) return;
  switch (level) {
    case RETRO_LOG_ERROR: qCWarning(lcCore).noquote() << "[error]" << msg; break;
    case RETRO_LOG_WARN: qCWarning(lcCore).noquote() << msg; break;
    case RETRO_LOG_INFO: qCInfo(lcCore).noquote() << msg; break;
    default: qCDebug(lcCore).noquote() << msg; break;
  }
}

HwRenderContext* s_hwCtx = nullptr;  // context of the active backend, for the C callbacks below

uintptr_t RETRO_CALLCONV hwGetFramebuffer() { return s_hwCtx ? s_hwCtx->framebuffer() : 0; }
retro_proc_address_t RETRO_CALLCONV hwGetProcAddress(const char* sym) {
  return s_hwCtx ? reinterpret_cast<retro_proc_address_t>(s_hwCtx->procAddress(sym)) : nullptr;
}

QString fromC(const char* s) { return s ? QString::fromUtf8(s) : QString(); }

CoreOption makeOption(const char* key, const QString& desc, const QString& info, const QString& cat,
                      const retro_core_option_value* vals, const char* def) {
  CoreOption o;
  o.key = fromC(key);
  o.description = desc;
  o.info = info;
  o.categoryKey = cat;
  for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && vals[i].value; ++i)
    o.values.append({fromC(vals[i].value), fromC(vals[i].label)});
  o.defaultValue = fromC(def);
  if (o.defaultValue.isEmpty() && !o.values.isEmpty()) o.defaultValue = o.values.first().value;
  o.currentValue = o.defaultValue;
  return o;
}

void applyOverrides(QList<CoreOption>& options, const QMap<QString, QString>& overrides) {
  for (CoreOption& opt : options) {
    auto it = overrides.constFind(opt.key);
    if (it == overrides.constEnd()) continue;
    for (const auto& v : std::as_const(opt.values))
      if (v.value == it.value()) opt.currentValue = it.value();
  }
}

}  // namespace

struct LibretroBackend::Api {
  void(RETRO_CALLCONV* set_environment)(retro_environment_t) = nullptr;
  void(RETRO_CALLCONV* set_video_refresh)(retro_video_refresh_t) = nullptr;
  void(RETRO_CALLCONV* set_audio_sample)(retro_audio_sample_t) = nullptr;
  void(RETRO_CALLCONV* set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
  void(RETRO_CALLCONV* set_input_poll)(retro_input_poll_t) = nullptr;
  void(RETRO_CALLCONV* set_input_state)(retro_input_state_t) = nullptr;
  void(RETRO_CALLCONV* init)() = nullptr;
  void(RETRO_CALLCONV* deinit)() = nullptr;
  unsigned(RETRO_CALLCONV* api_version)() = nullptr;
  void(RETRO_CALLCONV* get_system_info)(retro_system_info*) = nullptr;
  void(RETRO_CALLCONV* get_system_av_info)(retro_system_av_info*) = nullptr;
  bool(RETRO_CALLCONV* load_game)(const retro_game_info*) = nullptr;
  void(RETRO_CALLCONV* unload_game)() = nullptr;
  void(RETRO_CALLCONV* run)() = nullptr;
  void(RETRO_CALLCONV* reset)() = nullptr;
  void*(RETRO_CALLCONV* get_memory_data)(unsigned) = nullptr;  // optional
  size_t(RETRO_CALLCONV* get_memory_size)(unsigned) = nullptr;  // optional
  size_t(RETRO_CALLCONV* serialize_size)() = nullptr;           // optional (states)
  bool(RETRO_CALLCONV* serialize)(void*, size_t) = nullptr;     // optional
  bool(RETRO_CALLCONV* unserialize)(const void*, size_t) = nullptr;  // optional
  void(RETRO_CALLCONV* cheat_reset)() = nullptr;                // optional (cheats)
  void(RETRO_CALLCONV* cheat_set)(unsigned, bool, const char*) = nullptr;  // optional
};

std::atomic<LibretroBackend*> LibretroBackend::s_active{nullptr};

LibretroBackend::LibretroBackend() : m_api(std::make_unique<Api>()) {}

LibretroBackend::~LibretroBackend() { unloadCore(); }

bool LibretroBackend::loadCore(const QString& libraryPath, QString* error) {
  auto fail = [&](const QString& msg) {
    if (error) *error = msg;
    return false;
  };
  if (m_coreLoaded) return fail(QStringLiteral("Core already loaded"));
  if (s_active.load()) return fail(QStringLiteral("A libretro core is already loaded in this process"));

  m_lib.setFileName(libraryPath);
  if (!m_lib.load()) return fail(m_lib.errorString());

  bool ok = true;
  auto sym = [&](auto& fn, const char* name) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(m_lib.resolve(name));
    if (!fn) ok = false;
  };
  Api& a = *m_api;
  sym(a.set_environment, "retro_set_environment");
  sym(a.set_video_refresh, "retro_set_video_refresh");
  sym(a.set_audio_sample, "retro_set_audio_sample");
  sym(a.set_audio_sample_batch, "retro_set_audio_sample_batch");
  sym(a.set_input_poll, "retro_set_input_poll");
  sym(a.set_input_state, "retro_set_input_state");
  sym(a.init, "retro_init");
  sym(a.deinit, "retro_deinit");
  sym(a.api_version, "retro_api_version");
  sym(a.get_system_info, "retro_get_system_info");
  sym(a.get_system_av_info, "retro_get_system_av_info");
  sym(a.load_game, "retro_load_game");
  sym(a.unload_game, "retro_unload_game");
  sym(a.run, "retro_run");
  sym(a.reset, "retro_reset");
  a.get_memory_data = reinterpret_cast<void*(RETRO_CALLCONV*)(unsigned)>(m_lib.resolve("retro_get_memory_data"));
  a.get_memory_size = reinterpret_cast<size_t(RETRO_CALLCONV*)(unsigned)>(m_lib.resolve("retro_get_memory_size"));
  a.serialize_size = reinterpret_cast<size_t(RETRO_CALLCONV*)()>(m_lib.resolve("retro_serialize_size"));
  a.serialize = reinterpret_cast<bool(RETRO_CALLCONV*)(void*, size_t)>(m_lib.resolve("retro_serialize"));
  a.unserialize = reinterpret_cast<bool(RETRO_CALLCONV*)(const void*, size_t)>(m_lib.resolve("retro_unserialize"));
  a.cheat_reset = reinterpret_cast<void(RETRO_CALLCONV*)()>(m_lib.resolve("retro_cheat_reset"));
  a.cheat_set = reinterpret_cast<void(RETRO_CALLCONV*)(unsigned, bool, const char*)>(m_lib.resolve("retro_cheat_set"));
  if (!ok) {
    m_lib.unload();
    return fail(QStringLiteral("Not a valid libretro library (symbols missing)"));
  }
  const unsigned apiVersion = a.api_version();
  if (apiVersion != RETRO_API_VERSION) {
    m_lib.unload();
    return fail(QStringLiteral("libretro API version %1 not supported").arg(apiVersion));
  }

  {
    // The claim is atomic: a probe on a worker thread and a game start cannot both pass (PE-16).
    LibretroBackend* expected = nullptr;
    if (!s_active.compare_exchange_strong(expected, this)) {
      m_lib.unload();
      return fail(QStringLiteral("A libretro core is already loaded in this process"));
    }
  }
  m_corePath = libraryPath;
  m_corePathUtf8 = QDir::toNativeSeparators(libraryPath).toUtf8();
  m_shutdownRequested = false;
  m_serQuirks = 0;
  m_stateContext = RETRO_SAVESTATE_CONTEXT_NORMAL;
  m_ranFrame = false;
  m_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
  m_frame = QImage();
  m_frameCount = 0;
  m_audio.clear();
  m_lastReadbackNs = 0;
  m_lastGpuCopyNs = 0;
  m_lastGpuCaptured = false;
  m_lastDisplayNs = 0;
  m_lastDisplayed = false;
  m_readbackWasEnabled = true;
  {
    QMutexLocker l(&m_renderMutex);
    m_render = RenderInfo{};
  }
  {
    QMutexLocker l(&m_optMutex);
    m_options.clear();
    m_categories.clear();
  }

  m_supportsNoGame = false;
  m_vkProbe = -1;
  a.set_environment(&LibretroBackend::environmentCb);
  a.set_video_refresh(&LibretroBackend::videoRefreshCb);
  a.set_audio_sample(&LibretroBackend::audioSampleCb);
  a.set_audio_sample_batch(&LibretroBackend::audioBatchCb);
  a.set_input_poll(&LibretroBackend::inputPollCb);
  a.set_input_state(&LibretroBackend::inputStateCb);
  a.init();

  retro_system_info si{};
  a.get_system_info(&si);
  m_info = CoreInfo{};
  m_info.name = fromC(si.library_name);
  m_info.version = fromC(si.library_version);
  m_info.needFullpath = si.need_fullpath;
  for (const QString& e : fromC(si.valid_extensions).split(QLatin1Char('|'), Qt::SkipEmptyParts))
    m_info.extensions.append(QLatin1Char('.') + e.toLower());

  m_coreLoaded = true;
  return true;
}

void LibretroBackend::unloadCore() {
  if (!m_coreLoaded) return;
  unloadGame();
  m_memMap = MemoryMap(); ++m_memMapGen;
  m_api->deinit();
  teardownHw();
  m_lib.unload();
  m_coreLoaded = false;
  s_active.store(nullptr);
  m_hw.reset();
  m_vk.reset();
}

bool LibretroBackend::isCoreLoaded() const { return m_coreLoaded; }
CoreInfo LibretroBackend::coreInfo() const { return m_info; }

void LibretroBackend::setSystemDirectory(const QString& path) {
  m_systemDir = QDir::toNativeSeparators(path).toUtf8();
}
void LibretroBackend::setSaveDirectory(const QString& path) {
  m_saveDir = QDir::toNativeSeparators(path).toUtf8();
}

bool LibretroBackend::loadGame(const QString& path, QString* error) {
  auto fail = [&](const QString& msg) {
    if (error) *error = msg;
    return false;
  };
  if (!m_coreLoaded) return fail(QStringLiteral("No core loaded"));
  // Never call retro_load_game(NULL) on a core that did not declare SET_SUPPORT_NO_GAME (it may dereference it).
  if (path.isEmpty() && !m_supportsNoGame) return fail(QStringLiteral("Core does not support starting without a game"));
  if (m_gameLoaded) unloadGame();
  m_memMap = MemoryMap(); ++m_memMapGen;  // set again by the core in retro_load_game

  // Empty path: no-game mode (retro_load_game(NULL)); only used to let a core register its options.
  const bool noGame = path.isEmpty();
  QFileInfo fi(path);
  if (!noGame && !fi.isFile()) return fail(QStringLiteral("Game file not found"));
  m_gamePathUtf8 = noGame ? QByteArray() : QDir::toNativeSeparators(fi.absoluteFilePath()).toUtf8();
  m_gameData.clear();

  retro_game_info gi{};
  gi.path = m_gamePathUtf8.constData();
  if (noGame) {
    m_shutdownRequested = false;
    m_frame = QImage();
    m_frameCount = 0;
    m_audio.clear();
    if (!m_api->load_game(nullptr)) {
      teardownHw();
      return fail(QStringLiteral("Core could not start without a game"));
    }
    if (m_hwActive || m_vkActive) {
      retro_system_av_info hwAv{};
      m_api->get_system_av_info(&hwAv);
      QString hwErr;
      if (!finishHwSetup(hwAv.geometry.max_width, hwAv.geometry.max_height, &hwErr)) {
        m_api->unload_game();
        teardownHw();
        return fail(hwErr);
      }
    }
    m_gameLoaded = true;
    m_saveFilePath.clear();
    return true;
  }
  if (!m_info.needFullpath) {
    QFile f(fi.absoluteFilePath());
    if (!f.open(QIODevice::ReadOnly)) return fail(f.errorString());
    m_gameData = f.readAll();
    gi.data = m_gameData.constData();
    gi.size = static_cast<size_t>(m_gameData.size());
  }

  m_shutdownRequested = false;
  m_frame = QImage();
  m_frameCount = 0;
  m_audio.clear();
  if (!m_api->load_game(&gi)) {
    teardownHw();
    return fail(QStringLiteral("Core could not load the game"));
  }

  if (m_requireHw && !m_hwActive && !m_vkActive) {
    m_api->unload_game();
    teardownHw();
    return fail(QStringLiteral("This game needs hardware rendering (OpenGL Core 3.3 or newer), which is not available here. "
                               "Update the graphics driver, or check that UZUME_DISABLE_HW_RENDER is not set."));
  }
  retro_system_av_info av{};
  m_api->get_system_av_info(&av);
  if (m_hwActive || m_vkActive) {
    QString hwErr;
    if (!finishHwSetup(av.geometry.max_width, av.geometry.max_height, &hwErr)) {
      m_api->unload_game();
      teardownHw();
      return fail(hwErr);
    }
  }
  m_av.width = static_cast<int>(av.geometry.base_width);
  m_av.height = static_cast<int>(av.geometry.base_height);
  m_av.aspectRatio = static_cast<double>(av.geometry.aspect_ratio);
  m_av.fps = av.timing.fps;
  m_av.sampleRate = av.timing.sample_rate;
  m_gameLoaded = true;
  m_loadedPath = path;

  // Cores that set no memory map (most NDS/GB cores): expose SYSTEM_RAM at virtual address 0 so plugins still get
  // a flat address space for it. Cores with a map (e.g. Azahar) keep their own virtual addresses.
  if (m_memMap.empty() && m_api->get_memory_data && m_api->get_memory_size) {
    void* ram = m_api->get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    const size_t ramSize = m_api->get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (ram && ramSize > 0) { m_memMap = MemoryMap::fromBuffer(ram, ramSize, 0, "system_ram"); ++m_memMapGen; }
  }

  // Battery save: the core exposes it as memory, the frontend persists it (like RetroArch's .srm).
  m_saveFilePath.clear();
  m_sramSnapshot.clear();
  m_framesSinceFlush = 0;
  m_savePendingLoad = false;
  m_saveBlocked = false;
  if (!m_saveDir.isEmpty() && m_api->get_memory_data && m_api->get_memory_size) {
    m_saveFilePath = QDir(QString::fromUtf8(m_saveDir)).filePath(fi.completeBaseName() + QStringLiteral(".sav"));
    m_savePendingLoad = true;  // applied as soon as the core reports save memory (possibly only after a few frames)
    tryLoadSave();
  }
  return true;
}

// Loads the save file into SAVE_RAM once the core exposes it. Never overwrites a file that could not be
// read or whose size differs: such a file is backed up first (never deleted) and left alone.
bool LibretroBackend::tryLoadSave() {
  if (!m_savePendingLoad) return true;
  void* mem = m_api->get_memory_data(RETRO_MEMORY_SAVE_RAM);
  const size_t size = m_api->get_memory_size(RETRO_MEMORY_SAVE_RAM);
  if (mem == nullptr || size == 0) return false;
  m_savePendingLoad = false;
  const QFileInfo fi(m_saveFilePath);
  if (fi.exists()) {
    // A scanner, a sync client or the previous start may hold the file for a moment: retry before giving up.
    QFile sf(m_saveFilePath);
    bool opened = sf.open(QIODevice::ReadOnly);
    for (int attempt = 1; !opened && attempt < kSaveOpenAttempts; ++attempt) {
      QThread::msleep(kSaveOpenRetryMs);
      opened = sf.open(QIODevice::ReadOnly);
    }
    if (!opened) {
      qCWarning(lcCore) << "Save file exists but cannot be read; it will not be overwritten:" << m_saveFilePath;
      QFile::copy(m_saveFilePath, m_saveFilePath + QStringLiteral(".unreadable-") + backupStamp() + QStringLiteral(".bak"));
      m_saveBlocked = true;
      m_lastSaveError = QStringLiteral("%1: the save file cannot be read (%2). This game will not be saved over it.")
                            .arg(fi.fileName(), sf.errorString());
      if (m_saveProblemNotifier) m_saveProblemNotifier(m_lastSaveError);
    } else {
      const QByteArray data = sf.readAll();
      if (static_cast<size_t>(data.size()) != size) {
        qCWarning(lcCore) << "Save file size" << data.size() << "differs from the core's save memory" << size
                          << "; keeping a backup before the first write";
        // One backup per distinct content: a start that finds the same mismatching file again adds nothing.
        bool have = false;
        const QFileInfo sfi(m_saveFilePath);
        for (const QString& name : sfi.dir().entryList({sfi.fileName() + QStringLiteral(".size-mismatch-*.bak")}, QDir::Files)) {
          QFile bf(sfi.dir().filePath(name));
          if (bf.size() == data.size() && bf.open(QIODevice::ReadOnly) && bf.readAll() == data) {
            have = true;
            break;
          }
        }
        if (!have) {
          QFile::copy(m_saveFilePath, m_saveFilePath + QStringLiteral(".size-mismatch-") + backupStamp() + QStringLiteral(".bak"));
        }
      }
      std::memcpy(mem, data.constData(), (std::min)(static_cast<size_t>(data.size()), size));
    }
  }
  m_sramSnapshot = QByteArray(static_cast<const char*>(mem), static_cast<qsizetype>(size));
  return true;
}

QString LibretroBackend::backupStamp() { return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")); }

bool LibretroBackend::flushSave() {
  if (!m_gameLoaded || m_saveFilePath.isEmpty() || !m_api->get_memory_data || !m_api->get_memory_size) return true;
  if (m_savePendingLoad && !tryLoadSave()) return true;  // never flush before an existing file was applied
  if (m_saveBlocked) return true;
  const void* mem = m_api->get_memory_data(RETRO_MEMORY_SAVE_RAM);
  const size_t size = m_api->get_memory_size(RETRO_MEMORY_SAVE_RAM);
  if (mem == nullptr || size == 0) return true;
  const QByteArray now(static_cast<const char*>(mem), static_cast<qsizetype>(size));
  if (now == m_sramSnapshot) return true;
  QSaveFile f(m_saveFilePath);
  QString why;
  if (f.open(QIODevice::WriteOnly)) {
    if (f.write(now) == now.size() && f.commit()) {
      m_sramSnapshot = now;
      m_saveFailCount = 0;
      m_lastSaveError.clear();
      return true;
    }
  }
  why = f.errorString();
  m_lastSaveError = QStringLiteral("%1: %2").arg(QFileInfo(m_saveFilePath).fileName(), why);
  // Retried every few seconds: log the first failure of a streak, then only every 20th attempt.
  if (m_saveFailCount++ % 20 == 0) {
    qCWarning(lcCore) << "Battery save could not be written:" << m_saveFilePath << "-" << why;
  }
  return false;
}

qint64 LibretroBackend::saveMemorySize() const {
  if (!m_gameLoaded || m_saveFilePath.isEmpty() || !m_api->get_memory_data || !m_api->get_memory_size) return -1;
  if (m_api->get_memory_data(RETRO_MEMORY_SAVE_RAM) == nullptr) return -1;
  return static_cast<qint64>(m_api->get_memory_size(RETRO_MEMORY_SAVE_RAM));
}

void LibretroBackend::writeEmergencySave(const QByteArray& data, const QString& why) {
  const QString name = QFileInfo(m_saveFilePath).fileName();
  const QString target = m_saveFilePath + QStringLiteral(".unsaved-") + backupStamp();
  QSaveFile ef(target);
  if (ef.open(QIODevice::WriteOnly) && ef.write(data) == data.size() && ef.commit()) {
    qCWarning(lcCore) << "Battery save could not be written to" << m_saveFilePath << "-" << why << "; progress kept in" << target;
    m_lastSaveError = QStringLiteral("%1: %2. The latest progress was saved to %3 instead.").arg(name, why, QFileInfo(target).fileName());
  } else {
    qCWarning(lcCore) << "Battery save could not be written to" << m_saveFilePath << "or" << target << "-" << why << "/" << ef.errorString();
    m_lastSaveError = QStringLiteral("%1: %2. The progress could not be saved elsewhere either (%3).").arg(name, why, ef.errorString());
  }
}

void LibretroBackend::unloadGame() {
  if (!m_gameLoaded) return;
  // The last write of the session: retries after short pauses (a virus scanner or sync client may hold the file
  // briefly). If it still fails, the SRAM goes to a sibling "<save>.unsaved-<stamp>" file so the progress is not lost
  // with the core memory; lastSaveError() then says so. The same for progress that could not go to an unreadable file.
  bool saved = flushSave();
  for (const unsigned delayMs : {250u, 500u}) {
    if (saved) break;
    QThread::msleep(delayMs);
    saved = flushSave();
  }
  if (m_gameLoaded && !m_saveFilePath.isEmpty() && m_api->get_memory_data && m_api->get_memory_size && (!saved || m_saveBlocked)) {
    const void* mem = m_api->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t size = m_api->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (mem != nullptr && size > 0) {
      const QByteArray now(static_cast<const char*>(mem), static_cast<qsizetype>(size));
      if (now != m_sramSnapshot) {
        const QString why = saved ? QStringLiteral("the save file could not be read at start") : m_lastSaveError.section(QStringLiteral(": "), 1);
        writeEmergencySave(now, why);
      }
    }
  }
  if (m_hwActive) {
    m_hw->makeCurrent();
    if (m_hwResetDone && m_hwContextDestroy) m_hwContextDestroy();
    m_hwResetDone = false;
  }
  if (m_vkActive) {
    if (m_hwResetDone && m_hwContextDestroy) m_hwContextDestroy();
    m_hwResetDone = false;
  }
  m_api->unload_game();
  teardownHw();
  m_gameLoaded = false;
  m_ranFrame = false;
  m_gameData.clear();
  m_memMap = MemoryMap(); ++m_memMapGen;
}

bool LibretroBackend::isGameLoaded() const { return m_gameLoaded; }
AvInfo LibretroBackend::avInfo() const { return m_av; }

bool LibretroBackend::runFrame() {
  if (!m_gameLoaded || m_shutdownRequested) return false;
  m_lastReadbackNs = 0;  // only frames that really read back count; a skipped frame must not repeat the last value
  m_lastGpuCopyNs = 0;
  m_lastGpuCaptured = false;
  m_lastDisplayNs = 0;
  m_lastDisplayed = false;
  if (m_hwActive && !m_hw->makeCurrent()) return false;
  if (m_hwActive) {
    applyTargetChange();
    // The frame blitted by the previous runFrame is complete once the target has copied it (ADR 0019): capture it now,
    // before the core renders into the same context again.
    if (m_hw->hasEncodeTarget()) {
      const auto t0 = std::chrono::steady_clock::now();
      m_lastGpuCaptured = m_hw->captureEncode();
      m_lastGpuCopyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }
  }
  m_api->run();
  m_ranFrame = true;
  if (m_savePendingLoad) tryLoadSave();  // as soon as the core exposes save memory, before any flush
  if (++m_framesSinceFlush >= 180) {
    m_framesSinceFlush = 0;
    flushSave();
  }
  return !m_shutdownRequested;
}

void LibretroBackend::reset() {
  if (m_gameLoaded) m_api->reset();
}

bool LibretroBackend::supportsStates() const {
  return m_coreLoaded && m_api->serialize_size && m_api->serialize && m_api->unserialize;
}

qint64 LibretroBackend::serializeSize() {
  if (!supportsStates() || !m_gameLoaded) return 0;
  if ((m_serQuirks & RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE) && !m_ranFrame) return 0;
  if (m_hwActive && !m_hw->makeCurrent()) return 0;
  m_stateContext = RETRO_SAVESTATE_CONTEXT_NORMAL;
  return static_cast<qint64>(m_api->serialize_size());
}

bool LibretroBackend::serialize(QByteArray* out) {
  if (!out) return false;
  out->clear();
  const qint64 size = serializeSize();  // asked right before: a core may change its size from frame to frame
  if (size <= 0) return false;
  out->resize(static_cast<qsizetype>(size));
  if (m_api->serialize(out->data(), static_cast<size_t>(size))) return true;
  out->clear();
  return false;
}

bool LibretroBackend::unserialize(const QByteArray& state) {
  if (!supportsStates() || !m_gameLoaded || state.isEmpty()) return false;
  if ((m_serQuirks & RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE) && !m_ranFrame) return false;
  if (m_hwActive && !m_hw->makeCurrent()) return false;
  m_stateContext = RETRO_SAVESTATE_CONTEXT_NORMAL;
  return m_api->unserialize(state.constData(), static_cast<size_t>(state.size()));
}

bool LibretroBackend::supportsCheats() const { return m_coreLoaded && m_api->cheat_reset && m_api->cheat_set; }
void LibretroBackend::cheatReset() {
  if (supportsCheats() && m_gameLoaded) m_api->cheat_reset();
}
void LibretroBackend::cheatSet(unsigned index, bool enabled, const QString& code) {
  if (supportsCheats() && m_gameLoaded) m_api->cheat_set(index, enabled, code.toUtf8().constData());
}

QImage LibretroBackend::videoFrame() const { return m_frame; }
quint64 LibretroBackend::frameCount() const { return m_frameCount; }

double LibretroBackend::lastReadbackMs() const { return static_cast<double>(m_lastReadbackNs.load()) / 1e6; }
double LibretroBackend::lastGpuCopyMs() const { return static_cast<double>(m_lastGpuCopyNs.load()) / 1e6; }
double LibretroBackend::lastDisplayMs() const { return static_cast<double>(m_lastDisplayNs.load()) / 1e6; }

void LibretroBackend::setGpuDisplayTarget(std::shared_ptr<GpuDisplayTarget> target) {
  {
    QMutexLocker l(&m_targetMutex);
    m_nextDisplay.swap(target);
    m_displayChanged.store(true);
  }
  // `target` now holds the replaced one (if the emulation thread has not taken it yet), released outside the lock.
}

void LibretroBackend::setGpuEncodeTarget(std::shared_ptr<GpuEncodeTarget> target) {
  {
    QMutexLocker l(&m_targetMutex);
    m_nextTarget.swap(target);
    m_targetChanged.store(true);
  }
  // `target` now holds the replaced one (if the emulation thread has not taken it yet): released outside the lock, as
  // dropping the last reference may free CUDA resources.
}

void LibretroBackend::applyTargetChange() {
  if (m_targetChanged.load()) {
    std::shared_ptr<GpuEncodeTarget> next;
    {
      QMutexLocker l(&m_targetMutex);
      next = std::move(m_nextTarget);
      m_nextTarget.reset();
      m_targetChanged.store(false);  // under the lock: a setter running meanwhile is never lost
    }
    m_hw->setEncodeTarget(std::move(next));
  }
  if (m_displayChanged.load()) {
    std::shared_ptr<GpuDisplayTarget> next;
    {
      QMutexLocker l(&m_targetMutex);
      next = std::move(m_nextDisplay);
      m_nextDisplay.reset();
      m_displayChanged.store(false);
    }
    m_hw->setDisplayTarget(std::move(next));
  }
}

RenderInfo LibretroBackend::renderInfo() const {
  QMutexLocker l(&m_renderMutex);
  return m_render;
}

QByteArray LibretroBackend::takeAudio() {
  QByteArray out;
  out.swap(m_audio);
  return out;
}

void LibretroBackend::setJoypadState(unsigned port, quint32 mask) {
  if (port > 1) return;
  QMutexLocker l(&m_inputMutex);
  m_input.joypad[port] = mask;
}

void LibretroBackend::setPointer(double x, double y, bool pressed) {
  QMutexLocker l(&m_inputMutex);
  m_input.px = std::clamp(x, 0.0, 1.0);
  m_input.py = std::clamp(y, 0.0, 1.0);
  m_input.pressed = pressed;
}

// ---------------------------------------------------------------- Hardware rendering

void LibretroBackend::prepareForStart() {
  if (!HwRenderContext::allowed()) return;
  if (!m_hw) m_hw = std::make_unique<HwRenderContext>();
  m_hw->prepareSurface();
}

bool LibretroBackend::setupHwRender(void* data) {
  auto* cb = static_cast<retro_hw_render_callback*>(data);
  if (!cb || m_hwActive) return false;
  // Diagnostics: a request for hardware that ends in software gets a reason (shown as "OpenGL requested · fell back").
  auto fallback = [this](const char* reason) {
    QMutexLocker l(&m_renderMutex);
    m_render.hwRequested = true;
    m_render.hwActive = false;
    m_render.fallbackReason = QString::fromLatin1(reason);
  };
  if (cb->context_type == RETRO_HW_CONTEXT_VULKAN) return setupVulkan(cb);
  if (cb->context_type != RETRO_HW_CONTEXT_OPENGL && cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE) {
    if (cb->context_type == RETRO_HW_CONTEXT_NONE) return false;  // melonDS DS announces "no hardware" for software mode
    qCInfo(lcCore) << "Hardware render API" << static_cast<int>(cb->context_type) << "not supported; core falls back to software";
    fallback(kFallbackUnsupported);
    return false;
  }
  if (!HwRenderContext::allowed()) {
    qCInfo(lcCore) << "Hardware rendering disabled (no GUI application or UZUME_DISABLE_HW_RENDER=1); core falls back to software";
    fallback(uzume::env::get("UZUME_DISABLE_HW_RENDER") == QLatin1String("1") ? kFallbackDisabled : kFallbackNoContext);
    return false;
  }
  if (!m_hw) m_hw = std::make_unique<HwRenderContext>();
  QString err;
  const bool core = cb->context_type == RETRO_HW_CONTEXT_OPENGL_CORE;
  if (!m_hw->createContext(core, cb->version_major, cb->version_minor, cb->depth, cb->stencil, &err)) {
    qCWarning(lcCore).noquote() << "Hardware rendering unavailable:" << err << "; core falls back to software";
    fallback(kFallbackNoContext);
    return false;
  }
  s_hwCtx = m_hw.get();
  cb->get_current_framebuffer = &hwGetFramebuffer;
  cb->get_proc_address = &hwGetProcAddress;
  m_hwContextReset = cb->context_reset;
  m_hwContextDestroy = cb->context_destroy;
  m_hwBottomLeft = cb->bottom_left_origin;
  m_hwActive = true;
  m_hwResetDone = false;
  {
    QMutexLocker l(&m_renderMutex);
    m_render.hwRequested = true;
    m_render.hwActive = true;
    m_render.fallbackReason.clear();
    m_render.api = HwRenderContext::describeApi(m_hw->glVersion(), m_hw->isCoreProfile());
    m_render.gpu = HwRenderContext::describeGpu(m_hw->glRenderer(), m_hw->glVersion());
  }
  qCInfo(lcCore).noquote() << "Hardware rendering enabled (OpenGL" << (core ? "core" : "compat") << ")," << m_hw->glInfo();
  return true;
}

bool LibretroBackend::vulkanPreferred() const {
  const QString env = uzume::env::get("UZUME_RENDERER").trimmed().toLower();
  if (env == QLatin1String("vulkan")) return true;
  if (env == QLatin1String("opengl")) return false;
  return m_preferredRenderer.trimmed().toLower() == QLatin1String("vulkan");
}

bool LibretroBackend::vulkanAvailable() {
  if (m_vkProbe < 0) m_vkProbe = VkRenderContext::probe() ? 1 : 0;
  return m_vkProbe == 1;
}

// Vulkan (ADR 0034): only checks that a device exists here; instance and device are created in finishHwSetup, after
// retro_load_game, because a core may announce its negotiation interface after SET_HW_RENDER.
bool LibretroBackend::setupVulkan(void* data) {
  auto* cb = static_cast<retro_hw_render_callback*>(data);
  auto fallback = [this](const char* reason) {
    QMutexLocker l(&m_renderMutex);
    m_render.hwRequested = true;
    m_render.hwActive = false;
    m_render.fallbackReason = QString::fromLatin1(reason);
  };
  if (!HwRenderContext::allowed()) {
    qCInfo(lcCore) << "Hardware rendering disabled (no GUI application or UZUME_DISABLE_HW_RENDER=1); core falls back to software";
    fallback(uzume::env::get("UZUME_DISABLE_HW_RENDER") == QLatin1String("1") ? kFallbackDisabled : kFallbackNoContext);
    return false;
  }
  if (!VkRenderContext::compiledIn() || !vulkanAvailable()) {
    qCInfo(lcCore) << "No Vulkan device; core falls back to software";
    fallback(kFallbackNoVulkan);
    return false;
  }
  if (!m_vk) m_vk = std::make_unique<VkRenderContext>();
  m_vk->setRequiredApiVersion(cb->version_major);
  m_hwContextReset = cb->context_reset;
  m_hwContextDestroy = cb->context_destroy;
  m_hwBottomLeft = false;
  m_vkActive = true;
  m_hwResetDone = false;
  {
    QMutexLocker l(&m_renderMutex);
    m_render.hwRequested = true;
    m_render.hwActive = true;
    m_render.vulkan = true;
    m_render.fallbackReason.clear();
    m_render.api = QStringLiteral("Vulkan");
    m_render.gpu.clear();
  }
  return true;
}

// After retro_load_game: size the FBO from the core's maximum geometry, then context_reset.
bool LibretroBackend::finishHwSetup(unsigned maxWidth, unsigned maxHeight, QString* error) {
  if (m_vkActive) {
    QString err;
    if (!m_vk->create(&err)) {
      qCWarning(lcCore).noquote() << "Vulkan context could not be created:" << err;
      if (error) *error = QStringLiteral("Vulkan hardware rendering could not be started: %1").arg(err);
      QMutexLocker l(&m_renderMutex);
      m_render.hwActive = false;
      m_render.fallbackReason = QString::fromLatin1(kFallbackNoVulkan);
      return false;
    }
    {
      QMutexLocker l(&m_renderMutex);
      m_render.api = m_vk->apiText();
      m_render.gpu = m_vk->gpuText();
    }
    if (m_hwContextReset) m_hwContextReset();
    m_hwResetDone = true;
    return true;
  }
  if (!m_hw->makeCurrent() ||
      !m_hw->ensureSize(static_cast<int>(maxWidth), static_cast<int>(maxHeight))) {
    if (error) *error = QStringLiteral("Hardware render framebuffer could not be created");
    QMutexLocker l(&m_renderMutex);
    m_render.hwActive = false;
    m_render.fallbackReason = QString::fromLatin1(kFallbackNoFramebuffer);
    return false;
  }
  if (m_hwContextReset) m_hwContextReset();
  m_hwResetDone = true;
  return true;
}

void LibretroBackend::teardownHw() {
  if (m_vk) {
    if (m_vk->hasContext() && m_hwResetDone && m_hwContextDestroy) m_hwContextDestroy();
    m_vk->destroy();
  }
  m_vkActive = false;
  if (m_hw && m_hw->hasContext()) {
    m_hw->makeCurrent();
    if (m_hwResetDone && m_hwContextDestroy) m_hwContextDestroy();
    m_hw->destroyContext();
  }
  if (s_hwCtx == m_hw.get()) s_hwCtx = nullptr;
  m_hwActive = false;
  m_hwResetDone = false;
  m_hwContextReset = m_hwContextDestroy = nullptr;
}

// ---------------------------------------------------------------- Core Options

QList<CoreOption> LibretroBackend::coreOptions() const {
  QMutexLocker l(&m_optMutex);
  return m_options;
}

QList<CoreOptionCategory> LibretroBackend::coreOptionCategories() const {
  QMutexLocker l(&m_optMutex);
  return m_categories;
}

bool LibretroBackend::setCoreOption(const QString& key, const QString& value) {
  QMutexLocker l(&m_optMutex);
  for (CoreOption& o : m_options) {
    if (o.key != key) continue;
    bool valid = o.values.isEmpty();
    for (const auto& v : std::as_const(o.values)) valid = valid || v.value == value;
    if (!valid) return false;
    o.currentValue = value;
    m_overrides.insert(key, value);
    m_optionsDirty = true;
    return true;
  }
  if (!m_options.isEmpty()) return false;  // options known, key not among them
  m_overrides.insert(key, value);          // before registration: remember
  return true;
}

void LibretroBackend::setOptionVisible(const QString& key, bool visible) {
  QMutexLocker l(&m_optMutex);
  for (CoreOption& o : m_options)
    if (o.key == key) o.visible = visible;
}

void LibretroBackend::registerOptionsV2(const void* options) {
  const auto* o = static_cast<const retro_core_options_v2*>(options);
  QList<CoreOption> list;
  QList<CoreOptionCategory> cats;
  if (o->categories) {
    for (const auto* c = o->categories; c->key; ++c)
      cats.append({fromC(c->key), fromC(c->desc), fromC(c->info)});
  }
  for (const auto* d = o->definitions; d && d->key; ++d) {
    const bool cat = d->category_key && d->category_key[0];
    const QString desc = cat && d->desc_categorized ? fromC(d->desc_categorized) : fromC(d->desc);
    const QString info = cat && d->info_categorized ? fromC(d->info_categorized) : fromC(d->info);
    list.append(makeOption(d->key, desc, info, fromC(d->category_key), d->values, d->default_value));
  }
  QMutexLocker l(&m_optMutex);
  m_options = list;
  m_categories = cats;
  applyOverrides(m_options, m_overrides);
}

void LibretroBackend::registerOptionsV1(const void* definitions) {
  QList<CoreOption> list;
  for (const auto* d = static_cast<const retro_core_option_definition*>(definitions); d && d->key; ++d)
    list.append(makeOption(d->key, fromC(d->desc), fromC(d->info), QString(), d->values, d->default_value));
  QMutexLocker l(&m_optMutex);
  m_options = list;
  m_categories.clear();
  applyOverrides(m_options, m_overrides);
}

void LibretroBackend::registerOptionsV0(const void* variables) {
  QList<CoreOption> list;
  for (const auto* v = static_cast<const retro_variable*>(variables); v && v->key; ++v) {
    // "Description; value1|value2|..." - the first value is the default.
    const QString spec = fromC(v->value);
    const int sep = spec.indexOf(QLatin1Char(';'));
    CoreOption o;
    o.key = fromC(v->key);
    o.description = sep >= 0 ? spec.left(sep) : spec;
    const QStringList vals = (sep >= 0 ? spec.mid(sep + 1) : QString()).trimmed().split(QLatin1Char('|'));
    for (const QString& s : vals) o.values.append({s, QString()});
    if (!o.values.isEmpty()) o.defaultValue = o.values.first().value;
    o.currentValue = o.defaultValue;
    list.append(o);
  }
  QMutexLocker l(&m_optMutex);
  m_options = list;
  m_categories.clear();
  applyOverrides(m_options, m_overrides);
}

// ---------------------------------------------------------------- Callbacks

bool LibretroBackend::environmentCb(unsigned cmd, void* data) {
  LibretroBackend* self = s_active.load();
  return self && self->handleEnvironment(cmd, data);
}

void LibretroBackend::videoRefreshCb(const void* data, unsigned w, unsigned h, size_t pitch) {
  if (LibretroBackend* self = s_active.load()) self->handleVideo(data, w, h, pitch);
}

void LibretroBackend::audioSampleCb(int16_t l, int16_t r) {
  LibretroBackend* self = s_active.load();
  if (!self) return;
  const int16_t s[2] = {l, r};
  self->m_audio.append(reinterpret_cast<const char*>(s), sizeof s);
}

size_t LibretroBackend::audioBatchCb(const int16_t* data, size_t frames) {
  LibretroBackend* self = s_active.load();
  if (self && data) self->m_audio.append(reinterpret_cast<const char*>(data), static_cast<qsizetype>(frames * 4));
  return frames;
}

void LibretroBackend::inputPollCb() {
  LibretroBackend* self = s_active.load();
  if (!self) return;
  QMutexLocker l(&self->m_inputMutex);
  self->m_polled = self->m_input;
}

int16_t LibretroBackend::inputStateCb(unsigned port, unsigned device, unsigned index, unsigned id) {
  LibretroBackend* self = s_active.load();
  if (!self) return 0;
  const InputState& in = self->m_polled;
  switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_JOYPAD: {
      if (port > 1) return 0;
      const quint32 m = in.joypad[port];
      if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(m & 0xFFFFu);
      return id < 16 ? static_cast<int16_t>((m >> id) & 1u) : 0;
    }
    case RETRO_DEVICE_ANALOG: {
      // Circle pad (left, mask bits 20..23) and C-stick (right, bits 16..19) of the "3ds" input profile: digital
      // directions served as full deflection. Cores that read the D-pad as digital only never ask for this.
      if (port > 1 || index > RETRO_DEVICE_INDEX_ANALOG_RIGHT || id > RETRO_DEVICE_ID_ANALOG_Y) return 0;
      const quint32 m = in.joypad[port] >> (index == RETRO_DEVICE_INDEX_ANALOG_LEFT ? 20 : 16);
      const int neg = id == RETRO_DEVICE_ID_ANALOG_X ? 2 : 0;  // left / up
      const int pos = id == RETRO_DEVICE_ID_ANALOG_X ? 3 : 1;  // right / down  (bit order: up down left right)
      return static_cast<int16_t>(((m >> pos) & 1u ? 0x7fff : 0) - ((m >> neg) & 1u ? 0x7fff : 0));
    }
    case RETRO_DEVICE_POINTER: {
      if (port != 0 || index != 0) return 0;
      auto conv = [](double v) { return static_cast<int16_t>(std::lround((v * 2.0 - 1.0) * 0x7fff)); };
      switch (id) {
        case RETRO_DEVICE_ID_POINTER_X: return conv(in.px);
        case RETRO_DEVICE_ID_POINTER_Y: return conv(in.py);
        case RETRO_DEVICE_ID_POINTER_PRESSED: return in.pressed ? 1 : 0;
        case RETRO_DEVICE_ID_POINTER_COUNT: return in.pressed ? 1 : 0;
        default: return 0;
      }
    }
    default: return 0;
  }
}

void LibretroBackend::handleVideo(const void* data, unsigned width, unsigned height, size_t pitch) {
  if (!m_videoWanted.load()) {  // frame is not shown: no readback, no conversion; the last image stays
    ++m_frameCount;
    if (m_vkActive && data == RETRO_HW_FRAME_BUFFER_VALID && width != 0 && height != 0)
      m_vk->presentFrame(static_cast<int>(width), static_cast<int>(height), QSize(), false);  // the core's work still runs
    return;
  }
  if (m_vkActive && (!data || data == RETRO_HW_FRAME_BUFFER_VALID)) {  // Vulkan: readback only (ADR 0034)
    if (data == RETRO_HW_FRAME_BUFFER_VALID && width != 0 && height != 0) {
      m_sourceSize.store(pack(QSize(static_cast<int>(width), static_cast<int>(height))));
      const bool readbackOn = m_readbackEnabled.load();
      const auto t0 = std::chrono::steady_clock::now();
      const QImage img = m_vk->presentFrame(static_cast<int>(width), static_cast<int>(height), unpack(m_readbackLimit.load()), readbackOn);
      m_lastReadbackNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
      if (readbackOn) ++m_hwReadbacks;
      if (!img.isNull()) m_frame = img;
    } else {
      m_vk->presentDuplicate();
    }
    ++m_frameCount;
    return;
  }
  if (data == RETRO_HW_FRAME_BUFFER_VALID && m_hwActive && width != 0 && height != 0) {
    m_sourceSize.store(pack(QSize(static_cast<int>(width), static_cast<int>(height))));  // also without readback
    if (m_hw->hasDisplayTarget()) {  // zero-copy display (ADR 0027): right after the core frame, independent of readback
      const auto t2 = std::chrono::steady_clock::now();
      m_lastDisplayed = m_hw->displayBlit(static_cast<int>(width), static_cast<int>(height), m_hwBottomLeft,
                                          unpack(m_displayLimit.load()));
      m_lastDisplayNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t2).count();
    }
    const bool readbackOn = m_readbackEnabled.load();
    if (readbackOn) {
      if (!m_readbackWasEnabled) m_hw->discardReadback();  // switched on again: no frame from before the pause
      const auto t0 = std::chrono::steady_clock::now();
      const QImage img = m_hw->readback(static_cast<int>(width), static_cast<int>(height), m_hwBottomLeft,
                                        unpack(m_readbackLimit.load()));
      m_lastReadbackNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
      ++m_hwReadbacks;
      if (!img.isNull()) m_frame = img;
    }
    m_readbackWasEnabled = readbackOn;
    ++m_frameCount;
    if (m_hw->hasEncodeTarget()) {  // Session encode texture (ADR 0019); captured at the start of the next runFrame
      const auto t1 = std::chrono::steady_clock::now();
      m_hw->encodeBlit(static_cast<int>(width), static_cast<int>(height), m_hwBottomLeft);
      m_lastGpuCopyNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t1).count();
    }
    return;
  }
  if (!data || data == RETRO_HW_FRAME_BUFFER_VALID || width == 0 || height == 0) {
    ++m_frameCount;  // duplicate frame: last image stays
    if (m_hwActive) m_hw->encodeRepeat();  // the Session still gets one frame per displayed frame
    return;
  }
  if (m_hwActive && m_hw->hasDisplayTarget()) m_hw->dropDisplayTarget(QStringLiteral("software frame"));  // the stale GPU slot must not win
  m_sourceSize.store(pack(QSize(static_cast<int>(width), static_cast<int>(height))));
  QImage img(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB32);
  const auto* src = static_cast<const uchar*>(data);
  for (unsigned y = 0; y < height; ++y, src += pitch) {
    auto* dst = reinterpret_cast<quint32*>(img.scanLine(static_cast<int>(y)));
    if (m_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
      const auto* s = reinterpret_cast<const quint32*>(src);
      for (unsigned x = 0; x < width; ++x) dst[x] = s[x] | 0xFF000000u;
    } else {
      const auto* s = reinterpret_cast<const quint16*>(src);
      for (unsigned x = 0; x < width; ++x) {
        const quint32 p = s[x];
        quint32 r, g, b;
        if (m_pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
          r = (p >> 11) & 0x1F; g = (p >> 5) & 0x3F; b = p & 0x1F;
          r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        } else {  // 0RGB1555
          r = (p >> 10) & 0x1F; g = (p >> 5) & 0x1F; b = p & 0x1F;
          r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        }
        dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
      }
    }
  }
  m_frame = img;
  ++m_frameCount;
}

bool LibretroBackend::handleEnvironment(unsigned rawCmd, void* data) {
  const unsigned cmd = rawCmd & ~(RETRO_ENVIRONMENT_EXPERIMENTAL | RETRO_ENVIRONMENT_PRIVATE);
  switch (cmd) {
    case RETRO_ENVIRONMENT_SET_ROTATION: return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *static_cast<bool*>(data) = true; return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
      if (const auto* m = static_cast<const retro_message*>(data)) {
        QString t = fromC(m->msg);
        if (dedupeLog(t)) qCInfo(lcCore).noquote() << t;
      }
      return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
      if (const auto* m = static_cast<const retro_message_ext*>(data)) {
        QString t = fromC(m->msg);
        if (dedupeLog(t)) qCInfo(lcCore).noquote() << t;
      }
      return true;
    }
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION: *static_cast<unsigned*>(data) = 1; return true;
    case RETRO_ENVIRONMENT_SHUTDOWN: m_shutdownRequested = true; return true;
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      if (m_systemDir.isEmpty()) return false;
      *static_cast<const char**>(data) = m_systemDir.constData();
      return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      if (m_saveDir.isEmpty()) return false;
      *static_cast<const char**>(data) = m_saveDir.constData();
      return true;
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
      *static_cast<const char**>(data) = m_corePathUtf8.constData();
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
      const int fmt = *static_cast<const int*>(data);
      if (fmt != RETRO_PIXEL_FORMAT_0RGB1555 && fmt != RETRO_PIXEL_FORMAT_XRGB8888 && fmt != RETRO_PIXEL_FORMAT_RGB565)
        return false;
      m_pixelFormat = fmt;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: return true;
    case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
      *static_cast<uint64_t*>(data) = (1ull << RETRO_DEVICE_JOYPAD) | (1ull << RETRO_DEVICE_ANALOG) | (1ull << RETRO_DEVICE_POINTER);
      return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS & ~RETRO_ENVIRONMENT_EXPERIMENTAL: return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      static_cast<retro_log_callback*>(data)->log = &coreLog;
      return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      m_supportsNoGame = data != nullptr && *static_cast<const bool*>(data);
      return true;
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
      if (data) m_serQuirks = *static_cast<const uint64_t*>(data);
      return true;
    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT & ~RETRO_ENVIRONMENT_EXPERIMENTAL:
      if (data) *static_cast<int*>(data) = m_stateContext;
      return true;
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE: return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: return true;
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO: return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS & ~RETRO_ENVIRONMENT_EXPERIMENTAL:
      // Plugin memory access: copy the descriptors; the core's buffers stay valid while the game is loaded.
      // May be called again during retro_run (core maps more memory later): the runner watches memoryMapGeneration().
      if (data) {
        m_memMap = MemoryMap(*static_cast<const retro_memory_map*>(data));
        ++m_memMapGen;
      }
      return data != nullptr;
    case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE: {
      // Only inhibit_toggle matters: the user's chosen speed always wins over the core's ratio.
      if (data) m_ffInhibit.store(static_cast<const retro_fastforwarding_override*>(data)->inhibit_toggle);
      return true;
    }
    // rawCmd is masked: the experimental flag is not part of the case labels.
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING & ~RETRO_ENVIRONMENT_EXPERIMENTAL:
      *static_cast<bool*>(data) = m_fastForwarding.load();
      return true;
    case RETRO_ENVIRONMENT_GET_THROTTLE_STATE & ~RETRO_ENVIRONMENT_EXPERIMENTAL: {
      auto* t = static_cast<retro_throttle_state*>(data);
      const double fps = m_av.fps > 1.0 ? m_av.fps : 60.0;
      if (m_fastForwarding.load()) {
        t->mode = RETRO_THROTTLE_FAST_FORWARD;
        t->rate = static_cast<float>(fps * m_ffRatio.load());
      } else {
        t->mode = RETRO_THROTTLE_NONE;
        t->rate = static_cast<float>(fps);
      }
      return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE & ~RETRO_ENVIRONMENT_EXPERIMENTAL:
      // bit 0 = video, bit 1 = audio: audio is always on; video only when the frame will be shown
      if (data) *static_cast<int*>(data) = m_videoWanted.load() ? 3 : 2;
      return true;
    case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE & ~RETRO_ENVIRONMENT_EXPERIMENTAL: *static_cast<float*>(data) = static_cast<float>(m_av.fps > 1.0 ? m_av.fps : 60.0); return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH; return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
      const auto* g = static_cast<const retro_game_geometry*>(data);
      m_av.width = static_cast<int>(g->base_width);
      m_av.height = static_cast<int>(g->base_height);
      m_av.aspectRatio = static_cast<double>(g->aspect_ratio);
      if (m_hwActive && m_hw->makeCurrent())
        return m_hw->ensureSize(static_cast<int>(g->max_width), static_cast<int>(g->max_height));
      return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
      const auto* av = static_cast<const retro_system_av_info*>(data);
      m_av.width = static_cast<int>(av->geometry.base_width);
      m_av.height = static_cast<int>(av->geometry.base_height);
      m_av.aspectRatio = static_cast<double>(av->geometry.aspect_ratio);
      m_av.fps = av->timing.fps;
      m_av.sampleRate = av->timing.sample_rate;
      if (m_hwActive && m_hw->makeCurrent())
        return m_hw->ensureSize(static_cast<int>(av->geometry.max_width), static_cast<int>(av->geometry.max_height));
      return true;
    }

    // Core Options
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *static_cast<unsigned*>(data) = 2; return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: registerOptionsV2(data); return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
      registerOptionsV2(static_cast<const retro_core_options_v2_intl*>(data)->us);
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: registerOptionsV1(data); return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
      registerOptionsV1(static_cast<const retro_core_options_intl*>(data)->us);
      return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: registerOptionsV0(data); return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
      if (const auto* d = static_cast<const retro_core_option_display*>(data)) setOptionVisible(fromC(d->key), d->visible);
      return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* var = static_cast<retro_variable*>(data);
      if (!var) return false;
      var->value = nullptr;  // libretro: a miss leaves NULL, a core that only checks `value` must not see a stale pointer (PE-8)
      if (!var->key) return false;
      QMutexLocker l(&m_optMutex);
      const QString key = fromC(var->key);
      for (const CoreOption& o : std::as_const(m_options)) {
        if (o.key != key) continue;
        std::string& slot = m_varCache[var->key];
        slot = o.currentValue.toStdString();
        var->value = slot.c_str();
        return true;
      }
      return false;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLE: {
      const auto* var = static_cast<const retro_variable*>(data);
      if (!var) return true;  // query whether supported
      if (!var->key || !var->value) return false;
      QMutexLocker l(&m_optMutex);
      for (CoreOption& o : m_options) {
        if (o.key != fromC(var->key)) continue;
        o.currentValue = fromC(var->value);
        m_overrides.insert(o.key, o.currentValue);
        return true;
      }
      return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
      QMutexLocker l(&m_optMutex);
      *static_cast<bool*>(data) = m_optionsDirty;
      m_optionsDirty = false;
      return true;
    }

    // Hardware rendering (OpenGL / OpenGL Core only, see setupHwRender)
    case RETRO_ENVIRONMENT_SET_HW_RENDER: return setupHwRender(data);
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: {
      if (!HwRenderContext::allowed()) return false;
      if (vulkanPreferred() && VkRenderContext::compiledIn() && vulkanAvailable()) {
        *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_VULKAN;
        return true;
      }
      if (!m_hw) m_hw = std::make_unique<HwRenderContext>();
      if (!m_hw->available()) return false;
      *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGL_CORE;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE & ~RETRO_ENVIRONMENT_EXPERIMENTAL: {
      if (!data || !VkRenderContext::compiledIn()) return false;
      if (static_cast<const retro_hw_render_context_negotiation_interface*>(data)->interface_type !=
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
        return false;
      if (!m_vk) m_vk = std::make_unique<VkRenderContext>();
      m_vk->setNegotiationInterface(data);
      return true;
    }
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT & ~RETRO_ENVIRONMENT_EXPERIMENTAL: {
      auto* q = static_cast<retro_hw_render_context_negotiation_interface*>(data);
      if (!q || !VkRenderContext::compiledIn() || q->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN) return false;
      q->interface_version = std::min(q->interface_version, 2u);  // create_instance / create_device2 are served
      return true;
    }
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE & ~RETRO_ENVIRONMENT_EXPERIMENTAL: {
      if (!data || !m_vkActive || !m_vk || !m_vk->hasContext()) return false;
      *static_cast<const void**>(data) = m_vk->renderInterface();
      return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT & ~RETRO_ENVIRONMENT_EXPERIMENTAL: return false;  // no shared contexts offered

    // Deliberately rejected: other HW APIs, rumble, sensors, VFS, microphone, netpacket, ...
    default: return false;
  }
}

}  // namespace uzume::emu
