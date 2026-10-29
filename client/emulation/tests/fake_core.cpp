// Tiny fake libretro core (test only, no ROM): exposes SAVE_RAM. Configuration via environment:
//   FB_FAKE_SRAM_SIZE (default 8), FB_FAKE_SRAM_DELAY_FRAMES (no save memory for the first N frames),
//   FB_FAKE_FF_INHIBIT=1 (SET_FASTFORWARDING_OVERRIDE with inhibit_toggle), FB_FAKE_FF_RATIO (override ratio).
// Every frame the core queries GET_FASTFORWARDING and renders it into pixel 0 (nonzero = on) and sends audio.
//   FB_FAKE_SRAM_WRITE (byte written into SRAM[0] once memory is available, as a change by the "game").
//   FB_FAKE_RUN_HANG_MS (retro_run sleeps that long once, on frame 3: a core that blocks inside a frame).
//   FB_FAKE_UNLOAD_HANG_MS (retro_unload_game sleeps that long: a core that hangs while unloading).
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "third_party/libretro/libretro.h"

#if defined(_WIN32)
#define FB_EXPORT extern "C" __declspec(dllexport)
#else
#define FB_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
std::vector<uint8_t> g_sram;
int g_frames = 0;
int g_delay = 0;
int g_write = -1;
int g_loads = 0;  // retro_load_game calls (survives reloads), rendered into pixel 1 (blue component) for tests
retro_video_refresh_t g_video = nullptr;
retro_environment_t g_env = nullptr;
retro_audio_sample_batch_t g_audio = nullptr;
int16_t g_samples[2 * 735] = {};
uint16_t g_pixels[16 * 16];
int envInt(const char* n, int def) {
  const char* v = std::getenv(n);
  return v ? std::atoi(v) : def;
}
bool memAvailable() { return g_frames >= g_delay; }
}  // namespace

FB_EXPORT unsigned retro_api_version() { return RETRO_API_VERSION; }
FB_EXPORT void retro_set_environment(retro_environment_t e) { g_env = e; }
FB_EXPORT void retro_set_video_refresh(retro_video_refresh_t v) { g_video = v; }
FB_EXPORT void retro_set_audio_sample(retro_audio_sample_t) {}
FB_EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t a) { g_audio = a; }
FB_EXPORT void retro_set_input_poll(retro_input_poll_t) {}
FB_EXPORT void retro_set_input_state(retro_input_state_t) {}
FB_EXPORT void retro_init() {}
FB_EXPORT void retro_deinit() {}
FB_EXPORT void retro_get_system_info(retro_system_info* i) {
  std::memset(i, 0, sizeof(*i));
  i->library_name = "fake";
  i->library_version = "0";
  i->valid_extensions = "bin";
  i->need_fullpath = false;
}
FB_EXPORT void retro_get_system_av_info(retro_system_av_info* i) {
  std::memset(i, 0, sizeof(*i));
  i->geometry.base_width = i->geometry.max_width = 16;
  i->geometry.base_height = i->geometry.max_height = 16;
  i->geometry.aspect_ratio = 1.0f;
  i->timing.fps = 60.0;
  i->timing.sample_rate = 44100.0;
}
FB_EXPORT bool retro_load_game(const retro_game_info*) {
  g_sram.assign(static_cast<size_t>(envInt("FB_FAKE_SRAM_SIZE", 8)), 0);
  g_delay = envInt("FB_FAKE_SRAM_DELAY_FRAMES", 0);
  g_write = envInt("FB_FAKE_SRAM_WRITE", -1);
  g_frames = 0;
  ++g_loads;
  if (g_env && (envInt("FB_FAKE_FF_INHIBIT", 0) || envInt("FB_FAKE_FF_RATIO", 0))) {
    retro_fastforwarding_override o = {};
    o.ratio = static_cast<float>(envInt("FB_FAKE_FF_RATIO", 0));
    o.inhibit_toggle = envInt("FB_FAKE_FF_INHIBIT", 0) != 0;
    g_env(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &o);
  }
  return true;
}
FB_EXPORT void retro_unload_game() {
  if (const int ms = envInt("FB_FAKE_UNLOAD_HANG_MS", 0); ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
FB_EXPORT void retro_run() {
  ++g_frames;
  if (g_frames == 3) {
    if (const int ms = envInt("FB_FAKE_RUN_HANG_MS", 0); ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }
  if (g_write >= 0 && g_frames > g_delay && !g_sram.empty()) g_sram[0] = static_cast<uint8_t>(g_write);
  bool ff = false;
  if (g_env) g_env(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &ff);
  g_pixels[0] = ff ? 0x7fff : 0;
  g_pixels[1] = static_cast<uint16_t>(g_loads & 0x1f);
  g_pixels[2] = static_cast<uint16_t>((g_sram.empty() ? 0 : g_sram[0]) & 0x1f);  // first save byte, low 5 bits
  if (g_audio) g_audio(g_samples, 735);
  if (g_video) g_video(g_pixels, 16, 16, 32);
}
FB_EXPORT void retro_reset() {}
FB_EXPORT void* retro_get_memory_data(unsigned t) { return (t == RETRO_MEMORY_SAVE_RAM && memAvailable()) ? g_sram.data() : nullptr; }
FB_EXPORT size_t retro_get_memory_size(unsigned t) { return (t == RETRO_MEMORY_SAVE_RAM && memAvailable()) ? g_sram.size() : 0; }
