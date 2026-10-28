// Uzume test core: a small deterministic libretro core of our own (test only, never installed or packaged). It
// exercises the frontend (LibretroBackend, core options, CoreLocator, EmulationRunner, GameSession, save sync) without a
// real emulator, so merge-gating CI needs no melonDS/DeSmuME. Real cores run in the nightly workflow.
//
// What it does (layout of the frame and of the data row: test_core_layout.h):
//   - loads a game file (any file: the generated homebrew test ROM or a dummy), fails on a missing file;
//   - 256x384 XRGB8888 frames (two NDS-like screens stacked), or scaled OpenGL hardware frames in "opengl" render mode
//     (falls back to software when the frontend rejects SET_HW_RENDER); or Vulkan hardware frames in "vulkan" render mode
//     (generic flavor): SET_HW_RENDER VULKAN + context negotiation interface (FB_TEST_CORE_VK_NEG = v1 (default) | v2 |
//     none), GET_HW_RENDER_INTERFACE, an own B8G8R8A8 image (FB_TEST_CORE_VK_FORMAT=rgba: R8G8B8A8) filled by a buffer copy
//     with the same quadrants as the OpenGL mode plus a moving cyan marker (vkMarkerX in test_core_layout.h), set_image +
//     video_refresh(VALID). Own queue submit by default, FB_TEST_CORE_VK_CMDBUFS=1 uses set_command_buffers,
//     FB_TEST_CORE_VK_VERSION=<major>.<minor> asks for that Vulkan version (1.99: impossible), FB_TEST_CORE_VK_RETRY_GL=1 retries
//     a refused SET_HW_RENDER with OpenGL Core itself, FB_TEST_CORE_FAIL_LOAD_AFTER_HW=1 refuses the game after that negotiation,
//     FB_TEST_CORE_VK_LAYOUT=general hands the image over in VK_IMAGE_LAYOUT_GENERAL. Also falls back to software when
//     the frontend rejects SET_HW_RENDER. uzume_test_core_vk_state() reports what the frontend exercised;
//   - 800 (or rate/fps) stereo audio frames per retro_run;
//   - SAVE_RAM (not in the desmume flavor, which manages its save itself);
//   - joypad via one RETRO_DEVICE_ID_JOYPAD_MASK word (GET_INPUT_BITMASKS) or per button, both ports, plus the pointer;
//   - save states (real retro_serialize*: frame counter, phase, input, heap/linear/late RAM, SRAM) and cheats (format "raw":
//     lines "AAAAAAAA VV" separated by '+' or newline, device addresses in the heap/linear/late regions, written every
//     frame while enabled; enabled count at heap kCheatCountOff). retro_reset and retro_unserialize drop the cheat list;
//   - core options (V2 with categories; V1/V0 via FB_TEST_CORE_OPTIONS), GET_VARIABLE, GET_VARIABLE_UPDATE, display hiding;
//   - GET_SYSTEM_DIRECTORY / GET_SAVE_DIRECTORY, GET_FASTFORWARDING, SET_SUPPORT_NO_GAME (options registered in
//     retro_load_game, also with NULL), fps/sample rate in the AV info.
// Flavors (FB_TEST_CORE_FLAVOR) give it the option keys and names of the product core profiles so the profiles'
// defaults and locks resolve against it:
//   generic (default)  library "Uzume Test Core", test_* options
//   melondsds          library "Uzume Test Core (melonDS DS options)", the melonds_* keys of manifests/cores/melondsds.json
//   desmume            library "Uzume Test Core (DeSmuME options)", desmume_* keys, no SAVE_RAM
// Other environment: FB_TEST_CORE_OPTIONS (v2 default | v1 | v0), FB_TEST_CORE_NO_BITMASK=1, FB_TEST_CORE_SRAM_SIZE
// (default 8192), FB_TEST_CORE_SRAM_DELAY_FRAMES, FB_TEST_CORE_SRAM_WRITE (byte written into SRAM[0] by the "game"),
// FB_TEST_CORE_FF_INHIBIT=1, FB_TEST_CORE_WRITE_DIRS=1 (writes uzume_test_core_dirs.txt into the save directory),
// FB_TEST_CORE_FPS, FB_TEST_CORE_SAMPLE_RATE, FB_TEST_CORE_NO_NOGAME=1 (does not declare SET_SUPPORT_NO_GAME).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "synth_formats.h"
#include "test_core_layout.h"
#include "third_party/libretro/libretro.h"

#if UZUME_HAVE_VULKAN
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include "third_party/libretro/libretro_vulkan.h"
#endif

#if defined(_WIN32)
#define FB_EXPORT extern "C" __declspec(dllexport)
#define FB_GL __stdcall
#else
#define FB_EXPORT extern "C" __attribute__((visibility("default")))
#define FB_GL
#endif

namespace {
namespace tc = uzume::testcore;

enum class Flavor { Generic, Melonds, Desmume };

struct OptSpec {
  const char* key;
  const char* desc;
  const char* category;
  const char* def;
  const char* values;  // '|' separated
};

const OptSpec kGenericOptions[] = {
    {"test_text_mode", "Text mode", "video", "alpha", "alpha|beta|gamma"},
    {"test_render_mode", "Render mode", "video", "software", "software|opengl|vulkan|auto"},
    {"test_gl_scale", "GL scale", "video", "1", "1|2|3|4"},
    {"test_audio", "Audio", "audio", "on", "on|off"},
    {"test_hidden_by_core", "Hidden by the core", "video", "x", "x|y"},
};
const OptSpec kMelondsOptions[] = {
    {"melonds_sysfile_mode", "BIOS/Firmware Mode", "system", "native", "native|builtin"},
    {"melonds_firmware_nds_path", "Firmware Path", "system", "default", "default|firmware.bin"},
    {"melonds_boot_mode", "Boot Mode", "system", "direct", "direct|native"},
    {"melonds_render_mode", "Render Mode", "video", "software", "software|opengl"},
    {"melonds_opengl_resolution", "Internal Resolution", "video", "1", "1|2|3|4|5|6|7|8"},
    {"melonds_opengl_filtering", "OpenGL Filtering", "video", "nearest", "nearest|linear"},
    {"melonds_audio_interpolation", "Interpolation", "audio", "disabled", "disabled|linear|cosine|cubic"},
    {"melonds_number_of_screen_layouts", "Number of Screen Layouts", "screen", "1", "1|2|3|4|5|6|7|8"},
    {"melonds_screen_layout1", "Layout #1", "screen", "top-bottom", "top-bottom|bottom-top|left-right|right-left|top|bottom|hybrid-top|hybrid-bottom"},
    {"melonds_screen_layout2", "Layout #2", "screen", "top-bottom", "top-bottom|bottom-top|left-right|right-left|top|bottom|hybrid-top|hybrid-bottom"},
    {"melonds_screen_layout3", "Layout #3", "screen", "top-bottom", "top-bottom|bottom-top|left-right|right-left|top|bottom|hybrid-top|hybrid-bottom"},
    {"melonds_screen_layout4", "Layout #4", "screen", "top-bottom", "top-bottom|bottom-top|left-right|right-left|top|bottom|hybrid-top|hybrid-bottom"},
    {"melonds_screen_layout5", "Layout #5", "screen", "top-bottom", "top-bottom|bottom-top|left-right|right-left|top|bottom|hybrid-top|hybrid-bottom"},
    {"melonds_screen_layout6", "Layout #6", "screen", "top-bottom", "top-bottom|bottom-top|left-right|right-left|top|bottom|hybrid-top|hybrid-bottom"},
    {"melonds_screen_layout7", "Layout #7", "screen", "top-bottom", "top-bottom|bottom-top|left-right|right-left|top|bottom|hybrid-top|hybrid-bottom"},
    {"melonds_screen_layout8", "Layout #8", "screen", "top-bottom", "top-bottom|bottom-top|left-right|right-left|top|bottom|hybrid-top|hybrid-bottom"},
    {"melonds_screen_gap", "Screen Gap", "screen", "0", "0|1|2|8|16|32|64"},
    {"melonds_hybrid_ratio", "Hybrid Ratio", "screen", "2", "2|3"},
    {"melonds_hybrid_small_screen", "Hybrid Small Screen", "screen", "both", "both|top|bottom"},
    {"melonds_secondary_screen_scale", "Secondary Screen Scale", "screen", "1", "1|2|3"},
    {"melonds_secondary_screen_filtering", "Secondary Screen Filtering", "screen", "nearest", "nearest|linear"},
    {"melonds_show_current_layout", "Show Current Layout", "osd", "disabled", "disabled|enabled"},
    {"melonds_show_mic_state", "Show Microphone State", "osd", "disabled", "disabled|enabled"},
    {"melonds_show_lid_state", "Show Lid State", "osd", "disabled", "disabled|enabled"},
    {"melonds_show_camera_state", "Show Camera State", "osd", "disabled", "disabled|enabled"},
    {"melonds_show_sensor_reading", "Show Sensor Reading", "osd", "disabled", "disabled|enabled"},
    {"melonds_show_unsupported_features", "Show Unsupported Features", "osd", "disabled", "disabled|enabled"},
    {"melonds_show_bios_warnings", "Show BIOS Warnings", "osd", "disabled", "disabled|enabled"},
    {"melonds_touch_mode", "Touch Mode", "input", "auto", "auto|disabled|mouse|touch|joystick"},
};
const OptSpec kDesmumeOptions[] = {
    {"desmume_use_external_bios", "Use External BIOS/Firmware", "system", "disabled", "disabled|enabled"},
    {"desmume_boot_into_bios", "Boot Into BIOS", "system", "disabled", "disabled|enabled"},
    {"desmume_internal_resolution", "Internal Resolution", "video", "256x192", "256x192|512x384|768x576|1024x768"},
    {"desmume_opengl_mode", "OpenGL Rendering", "video", "disabled", "disabled|enabled"},
    {"desmume_screens_layout", "Screen Layout", "screen", "top/bottom", "top/bottom|bottom/top|left/right|right/left|top only|bottom only|hybrid/top|hybrid/bottom"},
    {"desmume_screens_gap", "Screen Gap", "screen", "0", "0|1|2|4|8|16|32|64|100"},
    {"desmume_hybrid_showboth_screens", "Hybrid: Show Both Screens", "screen", "disabled", "disabled|enabled"},
    {"desmume_hybrid_layout_ratio", "Hybrid: Ratio", "screen", "2:1", "2:1|3:1"},
    {"desmume_hybrid_layout_scale", "Hybrid: Scale", "screen", "1", "1|2|3"},
    {"desmume_hybrid_cursor_always_smallscreen", "Hybrid: Cursor Always on Small Screen", "screen", "enabled", "disabled|enabled"},
    {"desmume_pointer_type", "Pointer Type", "input", "mouse", "mouse|touch"},
    {"desmume_pointer_mouse", "Mouse Mode", "input", "enabled", "disabled|enabled"},
    {"desmume_pointer_device_l", "Pointer Device Left", "input", "none", "none|mouse|touch"},
    {"desmume_pointer_device_r", "Pointer Device Right", "input", "none", "none|mouse|touch"},
    {"desmume_mouse_speed", "Mouse Speed", "input", "1.0", "0.5|1.0|2.0"},
    {"desmume_pointer_colour", "Pointer Color", "input", "white", "white|red|green"},
    {"desmume_input_rotation", "Input Rotation", "input", "0", "0|90|180|270"},
};

struct CategorySpec {
  const char* key;
  const char* desc;
};
const CategorySpec kCategories[] = {{"system", "System"}, {"video", "Video"}, {"audio", "Audio"},
                                    {"screen", "Screens"}, {"osd", "On-screen display"}, {"input", "Input"}};

// ----------------------------------------------------------------------------------------------- state
retro_environment_t g_env = nullptr;
retro_video_refresh_t g_video = nullptr;
retro_audio_sample_batch_t g_audio = nullptr;
retro_input_poll_t g_poll = nullptr;
retro_input_state_t g_input = nullptr;

bool g_gameLoaded = false;
bool g_useBitmask = false;
int g_frames = 0;
int g_loads = 0;  // survives reloads of the game within one loaded library
int g_resets = 0;
int g_varUpdates = 0;
uint32_t g_romSize = 0;
uint32_t g_romSum = 0;
uint32_t g_dirsFlags = 0;
std::vector<uint8_t> g_sram;
int g_sramDelay = 0;
int g_sramWrite = -1;
double g_phase = 0.0;
std::vector<uint32_t> g_fb(static_cast<size_t>(tc::kWidth) * tc::kHeight);

bool g_hw = false;  // hardware frames in use
unsigned g_scale = 1;
retro_hw_render_callback g_hwCb{};
using GlBindFramebuffer = void(FB_GL*)(unsigned, unsigned);
using GlClearColor = void(FB_GL*)(float, float, float, float);
using GlClear = void(FB_GL*)(unsigned);
using GlEnable = void(FB_GL*)(unsigned);
using GlScissor = void(FB_GL*)(int, int, int, int);
using GlViewport = void(FB_GL*)(int, int, int, int);
GlBindFramebuffer glBindFramebuffer_ = nullptr;
GlClearColor glClearColor_ = nullptr;
GlClear glClear_ = nullptr;
GlEnable glEnable_ = nullptr;
GlScissor glScissor_ = nullptr;
GlViewport glViewport_ = nullptr;

// ----------------------------------------------------------------------------------------------- helpers
int envInt(const char* n, int def) {
  const char* v = std::getenv(n);
  return v && *v ? std::atoi(v) : def;
}
bool envSet(const char* n) { return envInt(n, 0) != 0; }

Flavor flavor() {
  const char* v = std::getenv("FB_TEST_CORE_FLAVOR");
  if (v && std::strcmp(v, "melondsds") == 0) return Flavor::Melonds;
  if (v && std::strcmp(v, "desmume") == 0) return Flavor::Desmume;
  return Flavor::Generic;
}

struct Table {
  const OptSpec* options;
  size_t count;
};
Table optionTable() {
  switch (flavor()) {
    case Flavor::Melonds: return {kMelondsOptions, sizeof kMelondsOptions / sizeof *kMelondsOptions};
    case Flavor::Desmume: return {kDesmumeOptions, sizeof kDesmumeOptions / sizeof *kDesmumeOptions};
    default: return {kGenericOptions, sizeof kGenericOptions / sizeof *kGenericOptions};
  }
}
const char* renderKey() {
  switch (flavor()) {
    case Flavor::Melonds: return "melonds_render_mode";
    case Flavor::Desmume: return "desmume_opengl_mode";
    default: return "test_render_mode";
  }
}
const char* hwValue() { return flavor() == Flavor::Desmume ? "enabled" : "opengl"; }
const char* scaleKey() {
  switch (flavor()) {
    case Flavor::Melonds: return "melonds_opengl_resolution";
    case Flavor::Desmume: return "desmume_internal_resolution";  // "512x384" -> 2
    default: return "test_gl_scale";
  }
}
const char* tagKey() {
  switch (flavor()) {
    case Flavor::Melonds: return "melonds_audio_interpolation";
    case Flavor::Desmume: return "desmume_screens_gap";
    default: return "test_text_mode";
  }
}
bool hasSaveRam() { return flavor() != Flavor::Desmume; }

std::string getVar(const char* key) {
  retro_variable v{key, nullptr};
  if (g_env && g_env(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value) return v.value;
  return {};
}

// Static storage for the option definitions of the selected flavor (the frontend copies them at registration).
struct OptStore {
  std::vector<std::vector<retro_core_option_value>> values;  // terminated by {nullptr, nullptr}
  std::vector<retro_core_option_v2_definition> v2;
  std::vector<retro_core_option_definition> v1;
  std::vector<retro_variable> v0;
  std::vector<std::string> v0text;
  std::vector<retro_core_option_v2_category> cats;
};
OptStore g_store;

std::vector<std::string> split(const char* s) {
  std::vector<std::string> out;
  std::string cur;
  for (; *s; ++s) {
    if (*s == '|') { out.push_back(cur); cur.clear(); } else cur += *s;
  }
  out.push_back(cur);
  return out;
}

void buildStore() {
  g_store = OptStore{};
  static std::vector<std::vector<std::string>> texts;  // keeps the value strings alive
  texts.clear();
  const Table t = optionTable();
  texts.reserve(t.count);
  g_store.values.reserve(t.count);
  g_store.v0text.reserve(t.count);
  for (size_t i = 0; i < t.count; ++i) {
    const OptSpec& o = t.options[i];
    texts.push_back(split(o.values));
    std::vector<retro_core_option_value> vals;
    for (const std::string& s : texts.back()) vals.push_back({s.c_str(), nullptr});
    vals.push_back({nullptr, nullptr});
    g_store.values.push_back(std::move(vals));
    // V0: "Description; default|other|..." (the first value is the default)
    std::string spec = std::string(o.desc) + "; " + o.def;
    for (const std::string& s : texts.back())
      if (s != o.def) spec += "|" + s;
    g_store.v0text.push_back(spec);
  }
  for (size_t i = 0; i < t.count; ++i) {
    const OptSpec& o = t.options[i];
    retro_core_option_v2_definition d{};
    d.key = o.key;
    d.desc = o.desc;
    d.desc_categorized = o.desc;
    d.info = "Test core option.";
    d.info_categorized = "Test core option.";
    d.category_key = o.category;
    for (size_t k = 0; k < g_store.values[i].size() - 1 && k < RETRO_NUM_CORE_OPTION_VALUES_MAX; ++k)
      d.values[k] = g_store.values[i][k];
    d.default_value = o.def;
    g_store.v2.push_back(d);
    retro_core_option_definition d1{};
    d1.key = o.key;
    d1.desc = o.desc;
    d1.info = "Test core option.";
    for (size_t k = 0; k < g_store.values[i].size() - 1 && k < RETRO_NUM_CORE_OPTION_VALUES_MAX; ++k)
      d1.values[k] = g_store.values[i][k];
    d1.default_value = o.def;
    g_store.v1.push_back(d1);
    g_store.v0.push_back({o.key, g_store.v0text[i].c_str()});
  }
  g_store.v2.push_back({});
  g_store.v1.push_back({});
  g_store.v0.push_back({nullptr, nullptr});
  for (const CategorySpec& c : kCategories) g_store.cats.push_back({c.key, c.desc, "Test core category."});
  g_store.cats.push_back({nullptr, nullptr, nullptr});
}

void registerOptions() {
  if (!g_env) return;
  buildStore();
  const char* api = std::getenv("FB_TEST_CORE_OPTIONS");
  unsigned version = 0;
  g_env(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version);
  if ((!api || std::strcmp(api, "v2") == 0) && version >= 2) {
    retro_core_options_v2 o{g_store.cats.data(), g_store.v2.data()};
    if (g_env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &o)) {
      if (flavor() == Flavor::Generic) {
        retro_core_option_display d{"test_hidden_by_core", false};
        g_env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &d);
      }
      return;
    }
  }
  if (api && std::strcmp(api, "v1") == 0 && g_env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, g_store.v1.data())) return;
  g_env(RETRO_ENVIRONMENT_SET_VARIABLES, g_store.v0.data());
}

uint32_t optionTag() {
  const std::string v = getVar(tagKey());
  return v.empty() ? 0u : (static_cast<uint32_t>(v.size()) << 8) | static_cast<uint8_t>(v[0]);
}

// ----------------------------------------------------------------------------------------------- video
uint32_t quadrant(int x, int y) {
  if (y < 96) return x < 128 ? 0xFFFF0000 : 0xFF00FF00;
  return x < 128 ? 0xFF0000FF : 0xFFFFFFFF;
}

uint16_t g_masks[2] = {0, 0};  // joypad state of both ports, polled every frame

void drawSoftware() {
  const int W = tc::kWidth, H = tc::kHeight;
  for (int y = 0; y < H; ++y) {
    uint32_t* row = &g_fb[static_cast<size_t>(y) * W];
    for (int x = 0; x < W; ++x) row[x] = y < 192 ? quadrant(x, y) : tc::kMagenta;
  }
  const int sx = (g_frames * 3) % (W - 8);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) g_fb[static_cast<size_t>(y) * W + sx + x] = 0xFF00FFFF;
}

void drawInputStrips() {
  const int W = tc::kWidth;
  for (int port = 0; port < 2; ++port)
    for (int row = 0; row < tc::kStripHeight; ++row) {
      uint32_t* line = &g_fb[static_cast<size_t>(tc::kStripY0 + port * tc::kStripHeight + row) * W];
      for (int id = 0; id < 16; ++id)
        for (int x = 0; x < tc::kCellWidth; ++x) line[id * tc::kCellWidth + x] = (g_masks[port] >> id) & 1 ? tc::kPressed : tc::kMagenta;
    }
}

void RETRO_CALLCONV contextReset() {
  auto load = [](auto& fn, const char* name) { fn = reinterpret_cast<std::decay_t<decltype(fn)>>(g_hwCb.get_proc_address(name)); };
  load(glBindFramebuffer_, "glBindFramebuffer");
  load(glClearColor_, "glClearColor");
  load(glClear_, "glClear");
  load(glEnable_, "glEnable");
  load(glScissor_, "glScissor");
  load(glViewport_, "glViewport");
}
void RETRO_CALLCONV contextDestroy() { glBindFramebuffer_ = nullptr; }

// Fills a rectangle given in image coordinates (origin top-left); the framebuffer has a bottom-left origin.
void glFill(int x, int yTop, int w, int h, float r, float g, float b) {
  const int H = static_cast<int>(tc::kHeight * g_scale);
  glScissor_(x, H - (yTop + h), w, h);
  glClearColor_(r, g, b, 1.f);
  glClear_(0x4000);  // GL_COLOR_BUFFER_BIT
}

void drawHardware() {
  const int k = static_cast<int>(g_scale);
  const int W = tc::kWidth * k, H = tc::kHeight * k;
  glBindFramebuffer_(0x8D40, static_cast<unsigned>(g_hwCb.get_current_framebuffer()));  // GL_FRAMEBUFFER
  glViewport_(0, 0, W, H);
  glEnable_(0x0C11);  // GL_SCISSOR_TEST
  glFill(0, 0, W, H, 1.f, 0.f, 1.f);  // magenta backdrop (bottom screen)
  glFill(0, 0, W / 2, 96 * k, 1.f, 0.f, 0.f);
  glFill(W / 2, 0, W / 2, 96 * k, 0.f, 1.f, 0.f);
  glFill(0, 96 * k, W / 2, 96 * k, 0.f, 0.f, 1.f);
  glFill(W / 2, 96 * k, W / 2, 96 * k, 1.f, 1.f, 1.f);
}


// ---------------------------------------------------------------- Vulkan render mode
bool g_vk = false;  // the hardware frames of this game come from Vulkan
int g_vkState[7] = {0, 0, 0, 0, 0, -1, -1};  // create_device (v1) calls, create_device2 calls, create_instance calls, frames presented, GET_PREFERRED_HW_RENDER answer (0 none, 1 + context type), negotiation support version for Vulkan / for an unknown type (-1: call refused)

#if UZUME_HAVE_VULKAN
namespace vkc {
const retro_hw_render_interface_vulkan* g_if = nullptr;
VkImage g_image = VK_NULL_HANDLE;
VkDeviceMemory g_imageMem = VK_NULL_HANDLE;
VkImageView g_view = VK_NULL_HANDLE;
VkBuffer g_buf = VK_NULL_HANDLE;
VkDeviceMemory g_bufMem = VK_NULL_HANDLE;
void* g_mapped = nullptr;
VkCommandPool g_pool = VK_NULL_HANDLE;
VkCommandBuffer g_cmd = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;
VkImageLayout g_layout = VK_IMAGE_LAYOUT_UNDEFINED;
retro_vulkan_image g_retroImage{};
bool g_rgba = false;
bool g_cmdbufs = false;
bool g_general = false;
bool g_ready = false;

#define VKC_FNS(X)                                                                                                         \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)                   \
  X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer)                     \
  X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool)            \
  X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer)                      \
  X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkQueueSubmit) X(vkCreateFence)                \
  X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkDeviceWaitIdle)
#define VKC_DECL(n) PFN_##n n##_ = nullptr;
VKC_FNS(VKC_DECL)
#undef VKC_DECL
PFN_vkGetPhysicalDeviceMemoryProperties getMemProps_ = nullptr;

uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp{};
  getMemProps_(g_if->gpu, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return ~0u;
}

// ---- context negotiation (v1 create_device, v2 create_instance / create_device2)
const VkApplicationInfo* RETRO_CALLCONV appInfo() {
  static const VkApplicationInfo a = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Uzume Test Core", 1, "Uzume Test Core", 1, VK_API_VERSION_1_1};
  return &a;
}

bool makeDevice(retro_vulkan_context* ctx, VkInstance inst, VkPhysicalDevice gpu, PFN_vkGetInstanceProcAddr gipa,
                retro_vulkan_create_device_wrapper_t wrapper, void* opaque, const char** exts = nullptr, unsigned numExts = 0) {
  if (gpu == VK_NULL_HANDLE) return false;
  auto getQf = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
  auto createDev = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst, "vkCreateDevice"));
  auto getDevProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr"));
  if (!getQf || !createDev || !getDevProc) return false;
  uint32_t n = 0;
  getQf(gpu, &n, nullptr);
  std::vector<VkQueueFamilyProperties> qf(n);
  getQf(gpu, &n, qf.data());
  int fam = -1;
  for (uint32_t i = 0; i < n && fam < 0; ++i)
    if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) fam = static_cast<int>(i);
  for (uint32_t i = 0; i < n && fam < 0; ++i)
    if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) fam = static_cast<int>(i);
  if (fam < 0) return false;
  const float prio = 1.f;
  VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, static_cast<uint32_t>(fam), 1, &prio};
  VkDeviceCreateInfo di = {};
  di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.enabledExtensionCount = exts ? numExts : 0;  // v1: the extensions the frontend asked for
  di.ppEnabledExtensionNames = exts;
  VkDevice dev = VK_NULL_HANDLE;
  if (wrapper) {
    dev = wrapper(gpu, opaque, &di);
  } else if (createDev(gpu, &di, nullptr, &dev) != VK_SUCCESS) {
    dev = VK_NULL_HANDLE;
  }
  if (dev == VK_NULL_HANDLE) return false;
  auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(getDevProc(dev, "vkGetDeviceQueue"));
  VkQueue q = VK_NULL_HANDLE;
  getQueue(dev, static_cast<uint32_t>(fam), 0, &q);
  ctx->gpu = gpu;
  ctx->device = dev;
  ctx->queue = q;
  ctx->queue_family_index = static_cast<uint32_t>(fam);
  ctx->presentation_queue = q;
  ctx->presentation_queue_family_index = static_cast<uint32_t>(fam);
  return true;
}
bool RETRO_CALLCONV createDeviceV1(retro_vulkan_context* ctx, VkInstance inst, VkPhysicalDevice gpu, VkSurfaceKHR,
                                   PFN_vkGetInstanceProcAddr gipa, const char** exts, unsigned numExts, const char**, unsigned,
                                   const VkPhysicalDeviceFeatures*) {
  ++g_vkState[0];
  return makeDevice(ctx, inst, gpu, gipa, nullptr, nullptr, exts, numExts);
}
bool RETRO_CALLCONV createDevice2(retro_vulkan_context* ctx, VkInstance inst, VkPhysicalDevice gpu, VkSurfaceKHR,
                                  PFN_vkGetInstanceProcAddr gipa, retro_vulkan_create_device_wrapper_t wrapper, void* opaque) {
  ++g_vkState[1];
  return makeDevice(ctx, inst, gpu, gipa, wrapper, opaque);
}
VkInstance RETRO_CALLCONV createInstance(PFN_vkGetInstanceProcAddr, const VkApplicationInfo* app,
                                         retro_vulkan_create_instance_wrapper_t wrapper, void* opaque) {
  ++g_vkState[2];
  VkInstanceCreateInfo ci = {};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = app;
  return wrapper(opaque, &ci);
}
void RETRO_CALLCONV destroyDevice() {}

retro_hw_render_context_negotiation_interface_vulkan g_neg{};
bool negotiate() {
  for (int i = 0; i < 2; ++i) {  // GET_..._NEGOTIATION_INTERFACE_SUPPORT: Vulkan, then a type nobody knows
    retro_hw_render_context_negotiation_interface q{};
    q.interface_type = i == 0 ? RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN
                              : static_cast<retro_hw_render_context_negotiation_interface_type>(99);
    q.interface_version = 7;
    g_vkState[5 + i] = g_env && g_env(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &q)
                           ? static_cast<int>(q.interface_version)
                           : -1;
  }
  const char* mode = std::getenv("FB_TEST_CORE_VK_NEG");
  const std::string m = mode ? mode : "v1";
  if (m == "none") return true;
  g_neg = {};
  g_neg.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
  g_neg.get_application_info = appInfo;
  g_neg.destroy_device = destroyDevice;
  if (m == "v2") {
    g_neg.interface_version = 2;
    g_neg.create_instance = createInstance;
    g_neg.create_device2 = createDevice2;
  } else {
    g_neg.interface_version = 1;
    g_neg.create_device = createDeviceV1;
  }
  return g_env && g_env(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &g_neg);
}

// ---- resources
void destroyResources() {
  if (!g_if || !g_ready) return;
  const VkDevice d = g_if->device;
  vkDeviceWaitIdle_(d);
  if (g_fence) vkDestroyFence_(d, g_fence, nullptr);
  if (g_pool) vkDestroyCommandPool_(d, g_pool, nullptr);
  if (g_view) vkDestroyImageView_(d, g_view, nullptr);
  if (g_image) vkDestroyImage_(d, g_image, nullptr);
  if (g_imageMem) vkFreeMemory_(d, g_imageMem, nullptr);
  if (g_mapped) vkUnmapMemory_(d, g_bufMem);
  if (g_buf) vkDestroyBuffer_(d, g_buf, nullptr);
  if (g_bufMem) vkFreeMemory_(d, g_bufMem, nullptr);
  g_fence = VK_NULL_HANDLE; g_pool = VK_NULL_HANDLE; g_view = VK_NULL_HANDLE; g_image = VK_NULL_HANDLE;
  g_imageMem = VK_NULL_HANDLE; g_mapped = nullptr; g_buf = VK_NULL_HANDLE; g_bufMem = VK_NULL_HANDLE; g_cmd = VK_NULL_HANDLE;
  g_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  g_ready = false;
}

void RETRO_CALLCONV contextReset() {
  g_ready = false;
  const void* iface = nullptr;
  if (!g_env || !g_env(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) || !iface) return;
  g_if = static_cast<const retro_hw_render_interface_vulkan*>(iface);
  if (g_if->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN || g_if->interface_version < 5) return;
  const VkDevice d = g_if->device;
#define VKC_LOAD(n) n##_ = reinterpret_cast<PFN_##n>(g_if->get_device_proc_addr(d, #n));
  VKC_FNS(VKC_LOAD)
#undef VKC_LOAD
  getMemProps_ = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(g_if->get_instance_proc_addr(g_if->instance, "vkGetPhysicalDeviceMemoryProperties"));
  const char* fmtEnv = std::getenv("FB_TEST_CORE_VK_FORMAT");
  g_rgba = fmtEnv && std::string(fmtEnv) == "rgba";
  g_cmdbufs = std::getenv("FB_TEST_CORE_VK_CMDBUFS") != nullptr;
  const char* lay = std::getenv("FB_TEST_CORE_VK_LAYOUT");
  g_general = lay && std::string(lay) == "general";
  const uint32_t W = tc::kWidth * g_scale, H = tc::kHeight * g_scale;

  VkImageCreateInfo ii = {};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = g_rgba ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
  ii.extent = {W, H, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage_(d, &ii, nullptr, &g_image) != VK_SUCCESS) return;
  VkMemoryRequirements mr{};
  vkGetImageMemoryRequirements_(d, g_image, &mr);
  VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (ma.memoryTypeIndex == ~0u) ma.memoryTypeIndex = memType(mr.memoryTypeBits, 0);
  if (vkAllocateMemory_(d, &ma, nullptr, &g_imageMem) != VK_SUCCESS || vkBindImageMemory_(d, g_image, g_imageMem, 0) != VK_SUCCESS) return;

  VkImageViewCreateInfo vi = {};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = g_image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = ii.format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView_(d, &vi, nullptr, &g_view) != VK_SUCCESS) return;
  g_retroImage.image_view = g_view;
  g_retroImage.image_layout = g_general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  g_retroImage.create_info = vi;

  VkBufferCreateInfo bi = {};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = static_cast<VkDeviceSize>(W) * H * 4;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer_(d, &bi, nullptr, &g_buf) != VK_SUCCESS) return;
  vkGetBufferMemoryRequirements_(d, g_buf, &mr);
  ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size,
        memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  if (ma.memoryTypeIndex == ~0u) return;
  if (vkAllocateMemory_(d, &ma, nullptr, &g_bufMem) != VK_SUCCESS || vkBindBufferMemory_(d, g_buf, g_bufMem, 0) != VK_SUCCESS ||
      vkMapMemory_(d, g_bufMem, 0, VK_WHOLE_SIZE, 0, &g_mapped) != VK_SUCCESS)
    return;

  VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_if->queue_index};
  if (vkCreateCommandPool_(d, &pi, nullptr, &g_pool) != VK_SUCCESS) return;
  VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  if (vkAllocateCommandBuffers_(d, &ca, &g_cmd) != VK_SUCCESS) return;
  VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
  if (vkCreateFence_(d, &fi, nullptr, &g_fence) != VK_SUCCESS) return;
  g_ready = true;
}
void RETRO_CALLCONV contextDestroy() {
  destroyResources();
  g_if = nullptr;
}

uint32_t pixel(int x, int y, int k, uint64_t frame) {
  const int W = tc::kWidth * k;
  const int mx = tc::vkMarkerX(frame, k);
  if (y < 8 * k && x >= mx && x < mx + 8 * k) return 0xFF00FFFFu;  // cyan marker
  if (y < 96 * k) return x < W / 2 ? 0xFFFF0000u : 0xFF00FF00u;
  if (y < 192 * k) return x < W / 2 ? 0xFF0000FFu : 0xFFFFFFFFu;
  return 0xFFFF00FFu;  // magenta backdrop
}

void drawFrame() {
  const int k = static_cast<int>(g_scale);
  const int W = tc::kWidth * k, H = tc::kHeight * k;
  const VkDevice d = g_if->device;
  if (g_cmdbufs) g_if->wait_sync_index(g_if->handle);
  auto* px = static_cast<uint32_t*>(g_mapped);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const uint32_t p = pixel(x, y, k, static_cast<uint64_t>(g_frames));  // 0xAARRGGBB
      px[static_cast<size_t>(y) * W + x] = g_rgba ? ((p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16)) : p;
    }
  vkResetCommandBuffer_(g_cmd, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  vkBeginCommandBuffer_(g_cmd, &bi);
  VkImageMemoryBarrier b = {};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = g_image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.oldLayout = g_layout;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier_(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {static_cast<uint32_t>(W), static_cast<uint32_t>(H), 1};
  vkCmdCopyBufferToImage_(g_cmd, g_buf, g_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = g_retroImage.image_layout;
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier_(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  vkEndCommandBuffer_(g_cmd);
  g_layout = g_retroImage.image_layout;
  if (g_cmdbufs) {
    g_if->set_command_buffers(g_if->handle, 1, &g_cmd);
  } else {
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    g_if->lock_queue(g_if->handle);
    const VkResult r = vkQueueSubmit_(g_if->queue, 1, &si, g_fence);
    g_if->unlock_queue(g_if->handle);
    if (r == VK_SUCCESS) {
      vkWaitForFences_(d, 1, &g_fence, VK_TRUE, UINT64_MAX);
      vkResetFences_(d, 1, &g_fence);
    }
  }
  g_if->set_image(g_if->handle, &g_retroImage, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
}
}  // namespace vkc
#endif  // UZUME_HAVE_VULKAN

void writeDataRow() {
  uint32_t* row = &g_fb[static_cast<size_t>(tc::kDataRow) * tc::kWidth];
  bool ff = false;
  if (g_env) g_env(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &ff);
  const bool sramOk = hasSaveRam() && g_frames >= g_sramDelay && !g_sram.empty();
  int16_t px = 0, py = 0, down = 0;
  if (g_input) {
    px = g_input(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    py = g_input(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
    down = g_input(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
  }
  row[tc::DataFrame] = tc::pixel(static_cast<uint32_t>(g_frames));
  row[tc::DataPointerX] = tc::pixel(static_cast<uint32_t>(px + 0x8000));
  row[tc::DataPointerY] = tc::pixel(static_cast<uint32_t>(py + 0x8000));
  row[tc::DataPointerDown] = tc::pixel(down ? 1 : 0);
  row[tc::DataFastForward] = tc::pixel(ff ? 1 : 0);
  row[tc::DataSram0] = tc::pixel(sramOk ? g_sram[0] : 0);
  row[tc::DataSram1] = tc::pixel(sramOk && g_sram.size() > 1 ? g_sram[1] : 0);
  row[tc::DataLoads] = tc::pixel(static_cast<uint32_t>(g_loads));
  row[tc::DataResets] = tc::pixel(static_cast<uint32_t>(g_resets));
  row[tc::DataRomSize] = tc::pixel(g_romSize);
  row[tc::DataRomSum] = tc::pixel(g_romSum);
  row[tc::DataOptionTag] = tc::pixel(optionTag());
  row[tc::DataDirs] = tc::pixel(g_dirsFlags);
  row[tc::DataBitmask] = tc::pixel(g_useBitmask ? 1 : 0);
  row[tc::DataVarUpdates] = tc::pixel(static_cast<uint32_t>(g_varUpdates));
  {
    static const char stale[] = "stale";
    retro_variable v{"fb_test_no_such_option", stale};
    const bool hit = g_env && g_env(RETRO_ENVIRONMENT_GET_VARIABLE, &v);
    row[tc::DataUnknownVar] = tc::pixel(!hit && v.value == nullptr ? 1 : 2);
  }
}

void pollInput() {
  if (g_poll) g_poll();
  for (int port = 0; port < 2; ++port) {
    uint16_t mask = 0;
    if (g_input) {
      if (g_useBitmask) {
        mask = static_cast<uint16_t>(g_input(static_cast<unsigned>(port), RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
      } else {
        for (unsigned id = 0; id < 16; ++id)
          if (g_input(static_cast<unsigned>(port), RETRO_DEVICE_JOYPAD, 0, id)) mask = static_cast<uint16_t>(mask | (1u << id));
      }
    }
    g_masks[port] = mask;
  }
}

void sendAudio(unsigned frames, double rate) {
  if (!g_audio) return;
  std::vector<int16_t> pcm(static_cast<size_t>(frames) * 2);
  const double step = 2.0 * 3.14159265358979323846 * 440.0 / rate;
  for (unsigned i = 0; i < frames; ++i) {
    const auto s = static_cast<int16_t>(std::lround(std::sin(g_phase) * 8000.0));
    pcm[2 * i] = s;
    pcm[2 * i + 1] = s;
    g_phase += step;
  }
  g_phase = std::fmod(g_phase, 2.0 * 3.14159265358979323846);
  g_audio(pcm.data(), frames);
}

double fps() { return envInt("FB_TEST_CORE_FPS", flavor() == Flavor::Generic ? 50 : 60); }
double sampleRate() { return envInt("FB_TEST_CORE_SAMPLE_RATE", flavor() == Flavor::Generic ? 32000 : 48000); }

unsigned scaleFromOption() {
  const std::string v = getVar(scaleKey());
  if (v.empty()) return 1;
  const int n = std::atoi(v.c_str());  // "2" -> 2; "512x384" -> 512
  if (v.find('x') != std::string::npos) return static_cast<unsigned>((std::max)(1, n / tc::kWidth));
  return static_cast<unsigned>((std::max)(1, (std::min)(n, 8)));
}

// Synthetic memory for the plugin memory access tests; layout in test_core_layout.h (namespace mem).
alignas(16) uint8_t g_heapMem[tc::mem::kRegionSize];
alignas(16) uint8_t g_linearMem[tc::mem::kRegionSize];
alignas(16) uint8_t g_lateMem[tc::mem::kLateSize];
retro_memory_descriptor g_memDescs[3];

struct CheatWrite {
  uint64_t addr;
  uint8_t value;
};
struct Cheat {
  unsigned index = 0;
  bool enabled = false;
  std::vector<CheatWrite> writes;
};
std::vector<Cheat> g_cheats;

uint8_t* ramPtr(uint64_t addr) {
  struct Region { uint64_t start; uint8_t* mem; uint64_t size; };
  const Region regions[] = {{tc::mem::kHeapAddr, g_heapMem, sizeof g_heapMem},
                            {tc::mem::kLinearAddr, g_linearMem, sizeof g_linearMem},
                            {tc::mem::kLateAddr, g_lateMem, sizeof g_lateMem}};
  for (const Region& r : regions)
    if (addr >= r.start && addr < r.start + r.size) return r.mem + (addr - r.start);
  return nullptr;
}

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// Fills the party block (linear heap at kPartyAddr): slot count, then kPartySlots synthetic encrypted PK6 party
// structures (testutil/synth_formats.h, valid checksum). Slot i: species kSpeciesBase + i, encryption constant
// kSlotEcBase | (kSlotMarker + i), so byte 0 of every slot is still the marker.
void fillParty(uint8_t* block) {
  putU32(block, tc::mem::kPartySlots);
  for (uint32_t i = 0; i < tc::mem::kPartySlots; ++i) {
    const uint32_t ec = tc::mem::kSlotEcBase | (tc::mem::kSlotMarker + i);
    const auto pk = uzume::synth::makePk6(ec, static_cast<uint16_t>(tc::mem::kSpeciesBase + i), true, 100 + i);
    std::memcpy(block + 4 + i * tc::mem::kPartySlotSize, pk.encrypted.data(), tc::mem::kPartySlotSize);
  }
}

void registerMemoryMap() {
  std::memset(g_heapMem, 0, sizeof g_heapMem);
  std::memset(g_linearMem, 0, sizeof g_linearMem);
  putU32(g_heapMem + tc::mem::kMagicOff, tc::mem::kMagic);
  putU32(g_heapMem + tc::mem::kPartyPtrOff, tc::mem::kPartyAddr);
  fillParty(g_linearMem + (tc::mem::kPartyAddr - tc::mem::kLinearAddr));
  std::memset(g_memDescs, 0, sizeof g_memDescs);
  g_memDescs[0].ptr = g_heapMem;
  g_memDescs[0].start = tc::mem::kHeapAddr;
  g_memDescs[0].len = tc::mem::kRegionSize;
  g_memDescs[0].addrspace = "heap";
  g_memDescs[1].ptr = g_linearMem;
  g_memDescs[1].start = tc::mem::kLinearAddr;
  g_memDescs[1].len = tc::mem::kRegionSize;
  g_memDescs[1].addrspace = "linear";
  retro_memory_map map = {g_memDescs, 2};
  if (g_env) g_env(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
}

// Re-sends the map from retro_run with an additional "late" region (synthetic, like a core that maps memory later).
void registerLateMemoryMap() {
  std::memset(g_lateMem, 0, sizeof g_lateMem);
  putU32(g_lateMem, tc::mem::kLateMagic);
  g_memDescs[2] = {};
  g_memDescs[2].ptr = g_lateMem;
  g_memDescs[2].start = tc::mem::kLateAddr;
  g_memDescs[2].len = tc::mem::kLateSize;
  g_memDescs[2].addrspace = "late";
  retro_memory_map map = {g_memDescs, 3};
  if (g_env) g_env(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
}
}  // namespace

// ----------------------------------------------------------------------------------------------- libretro API
FB_EXPORT unsigned retro_api_version() { return RETRO_API_VERSION; }
FB_EXPORT void retro_set_environment(retro_environment_t e) {
  g_env = e;
  if (!envSet("FB_TEST_CORE_NO_NOGAME")) {
    bool yes = true;
    e(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &yes);
  }
}
FB_EXPORT void retro_set_video_refresh(retro_video_refresh_t v) { g_video = v; }
FB_EXPORT void retro_set_audio_sample(retro_audio_sample_t) {}
FB_EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t a) { g_audio = a; }
FB_EXPORT void retro_set_input_poll(retro_input_poll_t p) { g_poll = p; }
FB_EXPORT void retro_set_input_state(retro_input_state_t s) { g_input = s; }
FB_EXPORT void retro_init() {
  g_loads = 0;
  g_resets = 0;
  g_varUpdates = 0;
  g_useBitmask = !envSet("FB_TEST_CORE_NO_BITMASK") && g_env && g_env(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
}
FB_EXPORT void retro_deinit() {}
FB_EXPORT void retro_get_system_info(retro_system_info* i) {
  std::memset(i, 0, sizeof(*i));
  switch (flavor()) {
    case Flavor::Melonds: i->library_name = "Uzume Test Core (melonDS DS options)"; break;
    case Flavor::Desmume: i->library_name = "Uzume Test Core (DeSmuME options)"; break;
    default: i->library_name = "Uzume Test Core"; break;
  }
  i->library_version = "1.0-test";
  i->valid_extensions = "nds|bin";
  i->need_fullpath = true;
  i->block_extract = false;
}
FB_EXPORT void retro_get_system_av_info(retro_system_av_info* i) {
  std::memset(i, 0, sizeof(*i));
  const unsigned k = g_hw ? g_scale : 1;
  i->geometry.base_width = i->geometry.max_width = tc::kWidth * k;
  i->geometry.base_height = i->geometry.max_height = tc::kHeight * k;
  i->geometry.aspect_ratio = static_cast<float>(tc::kWidth) / tc::kHeight;
  i->timing.fps = fps();
  i->timing.sample_rate = sampleRate();
}

FB_EXPORT bool retro_load_game(const retro_game_info* info) {
  int fmt = RETRO_PIXEL_FORMAT_XRGB8888;
  if (g_env) g_env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
  registerOptions();  // like melonDS DS: only here, also for the no-game probe (info == NULL)
  g_frames = 0;
  g_hw = false;
  g_vk = false;
  g_scale = 1;
  g_masks[0] = g_masks[1] = 0;

  const char* sys = nullptr;
  const char* save = nullptr;
  g_dirsFlags = 0;
  if (g_env && g_env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys) && sys) g_dirsFlags |= 1;
  if (g_env && g_env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save) && save) g_dirsFlags |= 2;
  if (envSet("FB_TEST_CORE_WRITE_DIRS") && save) {
    const std::string path = std::string(save) + "/uzume_test_core_dirs.txt";
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fprintf(f, "system=%s\nsave=%s\n", sys ? sys : "", save);
      std::fclose(f);
    }
  }

  if (!info) {  // SET_SUPPORT_NO_GAME probe: options registered, nothing to run
    g_gameLoaded = false;
    return true;
  }
  // The "game": any readable file.
  g_romSize = 0;
  g_romSum = 0;
  if (!info->path) return false;
  std::FILE* f = std::fopen(info->path, "rb");
  if (!f) return false;
  unsigned char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    g_romSize += static_cast<uint32_t>(n);
    for (size_t k = 0; k < n; ++k) g_romSum += buf[k];
  }
  std::fclose(f);

  g_sram.assign(hasSaveRam() ? static_cast<size_t>(envInt("FB_TEST_CORE_SRAM_SIZE", 8192)) : 0, 0);
  g_sramDelay = envInt("FB_TEST_CORE_SRAM_DELAY_FRAMES", 0);
  g_sramWrite = envInt("FB_TEST_CORE_SRAM_WRITE", -1);
  if (g_env && envSet("FB_TEST_CORE_FF_INHIBIT")) {
    retro_fastforwarding_override o = {};
    o.inhibit_toggle = true;
    g_env(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &o);
  }

  // Render mode "auto" (generic flavor): ask the frontend (GET_PREFERRED_HW_RENDER) and request what it answers.
  bool wantVk = false;
  bool wantGl = getVar(renderKey()) == hwValue();
  if (flavor() == Flavor::Generic) {
    const std::string mode = getVar("test_render_mode");
    g_vkState[4] = 0;
    wantVk = mode == "vulkan";
    if (mode == "auto") {
      unsigned pref = 0;
      const bool ok = g_env && g_env(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &pref);
      g_vkState[4] = ok ? static_cast<int>(1 + pref) : 0;
      wantVk = ok && pref == RETRO_HW_CONTEXT_VULKAN;
      wantGl = ok && !wantVk;
    }
  }
#if UZUME_HAVE_VULKAN
  // Vulkan render mode (generic flavor): hardware frames from our own VkImage; falls back to software like OpenGL.
  if (wantVk) {
    g_scale = scaleFromOption();
    std::memset(&g_hwCb, 0, sizeof g_hwCb);
    g_hwCb.context_type = RETRO_HW_CONTEXT_VULKAN;
    g_hwCb.version_major = VK_MAKE_VERSION(1, 1, 0);
    if (const char* ver = std::getenv("FB_TEST_CORE_VK_VERSION")) {  // "<major>.<minor>": asks for a Vulkan version, e.g. 1.99
      unsigned maj = 0, mnr = 0;
      if (std::sscanf(ver, "%u.%u", &maj, &mnr) == 2) g_hwCb.version_major = VK_MAKE_VERSION(maj, mnr, 0);
    }
    g_hwCb.version_minor = 0;
    g_hwCb.context_reset = vkc::contextReset;
    g_hwCb.context_destroy = vkc::contextDestroy;
    g_hw = g_env && g_env(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hwCb);
    if (g_hw) {
      g_vk = true;
      vkc::negotiate();  // after SET_HW_RENDER, like Azahar
    } else {
      g_scale = 1;
    }
  }
#endif
  // OpenGL render mode: hardware frames; when the frontend rejects SET_HW_RENDER the core falls back to software.
  if (!g_hw && (wantGl || (wantVk && envSet("FB_TEST_CORE_VK_RETRY_GL")))) {  // the latter: a core that retries with OpenGL itself
    g_scale = scaleFromOption();
    std::memset(&g_hwCb, 0, sizeof g_hwCb);
    g_hwCb.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    g_hwCb.version_major = 3;
    g_hwCb.version_minor = 3;
    g_hwCb.context_reset = contextReset;
    g_hwCb.context_destroy = contextDestroy;
    g_hwCb.bottom_left_origin = true;
    g_hw = g_env && g_env(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hwCb);
    if (!g_hw) g_scale = 1;
  }
  if (envSet("FB_TEST_CORE_FAIL_LOAD_AFTER_HW")) {  // the game is refused after the hardware render negotiation (not a renderer problem)
    g_hw = false;
    g_vk = false;
    return false;
  }
  registerMemoryMap();
  ++g_loads;
  g_gameLoaded = true;
  return true;
}
FB_EXPORT void retro_unload_game() { g_gameLoaded = false; g_hw = false; g_vk = false; }

// Test hook: what the frontend exercised in Vulkan mode (0 create_device calls, 1 create_device2, 2 create_instance, 3 frames, 4 preferred answer).
FB_EXPORT int uzume_test_core_vk_state(int what) { return what >= 0 && what < 7 ? g_vkState[what] : -1; }

FB_EXPORT void retro_run() {
  if (!g_gameLoaded) return;
  bool updated = false;
  if (g_env && g_env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) ++g_varUpdates;
  ++g_frames;
  putU32(g_heapMem + tc::mem::kFrameOff, static_cast<uint32_t>(g_frames));
  {
    uint32_t active = 0;
    for (const Cheat& c : g_cheats) {
      if (!c.enabled) continue;
      ++active;
      for (const CheatWrite& w : c.writes)
        if (uint8_t* p = ramPtr(w.addr)) *p = w.value;
    }
    putU32(g_heapMem + tc::mem::kCheatCountOff, active);
  }
  if (const int lateFrame = envInt("FB_TEST_CORE_LATE_MAP_FRAME", tc::mem::kLateMapFrame); lateFrame > 0 && g_frames == lateFrame)
    registerLateMemoryMap();
  pollInput();
  if (g_sramWrite >= 0 && g_frames > g_sramDelay && !g_sram.empty()) g_sram[0] = static_cast<uint8_t>(g_sramWrite);
  sendAudio(static_cast<unsigned>(std::lround(sampleRate() / fps())), sampleRate());
#if UZUME_HAVE_VULKAN
  if (g_vk) {
    if (!vkc::g_ready) {
      if (g_video) g_video(nullptr, tc::kWidth * g_scale, tc::kHeight * g_scale, 0);  // context not ready: duplicate
      return;
    }
    vkc::drawFrame();
    ++g_vkState[3];
    if (g_video) g_video(RETRO_HW_FRAME_BUFFER_VALID, tc::kWidth * g_scale, tc::kHeight * g_scale, 0);
    return;
  }
#endif
  if (g_hw) {
    if (!glBindFramebuffer_) {
      if (g_video) g_video(nullptr, tc::kWidth * g_scale, tc::kHeight * g_scale, 0);  // context not ready: duplicate
      return;
    }
    drawHardware();
    if (g_video) g_video(RETRO_HW_FRAME_BUFFER_VALID, tc::kWidth * g_scale, tc::kHeight * g_scale, 0);
    return;
  }
  drawSoftware();
  drawInputStrips();
  writeDataRow();
  if (g_video) g_video(g_fb.data(), tc::kWidth, tc::kHeight, static_cast<size_t>(tc::kWidth) * 4);
}
FB_EXPORT void retro_reset() { ++g_resets; g_cheats.clear(); }
FB_EXPORT void* retro_get_memory_data(unsigned t) {
  return (t == RETRO_MEMORY_SAVE_RAM && g_gameLoaded && g_frames >= g_sramDelay && !g_sram.empty()) ? g_sram.data() : nullptr;
}
FB_EXPORT size_t retro_get_memory_size(unsigned t) {
  return (t == RETRO_MEMORY_SAVE_RAM && g_gameLoaded && g_frames >= g_sramDelay) ? g_sram.size() : 0;
}
namespace {
// Save states: a flat little-endian blob with everything that makes the next frames deterministic. Layout:
//   "UTC1" | i32 frames | f64 phase | u16 masks[2] | u32 sramSize | heap | linear | late | sram
// The cheat list is deliberately NOT part of a state (like most real cores); retro_unserialize drops the core's cheat list
// (and so does retro_reset), the way cores that rebuild their cheat engine do, so the frontend must re-apply it.
constexpr size_t kStateHeader = 4 + 4 + 8 + 4 + 4;
constexpr char kStateMagic[4] = {'U', 'T', 'C', '1'};
size_t stateSize() { return kStateHeader + sizeof g_heapMem + sizeof g_linearMem + sizeof g_lateMem + g_sram.size(); }
}  // namespace

FB_EXPORT size_t retro_serialize_size() { return g_gameLoaded ? stateSize() : 0; }
FB_EXPORT bool retro_serialize(void* data, size_t size) {
  if (!g_gameLoaded || size < stateSize()) return false;
  auto* p = static_cast<uint8_t*>(data);
  auto put = [&p](const void* src, size_t n) { std::memcpy(p, src, n); p += n; };
  const int32_t frames = g_frames;
  const uint32_t sramSize = static_cast<uint32_t>(g_sram.size());
  put(kStateMagic, 4);
  put(&frames, 4);
  put(&g_phase, 8);
  put(g_masks, sizeof g_masks);
  put(&sramSize, 4);
  put(g_heapMem, sizeof g_heapMem);
  put(g_linearMem, sizeof g_linearMem);
  put(g_lateMem, sizeof g_lateMem);
  if (!g_sram.empty()) put(g_sram.data(), g_sram.size());
  return true;
}
FB_EXPORT bool retro_unserialize(const void* data, size_t size) {
  if (!g_gameLoaded || size < kStateHeader || std::memcmp(data, kStateMagic, 4) != 0) return false;
  const auto* p = static_cast<const uint8_t*>(data) + 4;
  int32_t frames = 0;
  uint32_t sramSize = 0;
  double phase = 0.0;
  uint16_t masks[2];
  std::memcpy(&frames, p, 4); p += 4;
  std::memcpy(&phase, p, 8); p += 8;
  std::memcpy(masks, p, sizeof masks); p += sizeof masks;
  std::memcpy(&sramSize, p, 4); p += 4;
  if (sramSize != g_sram.size() || size < stateSize()) return false;
  g_frames = frames;
  g_phase = phase;
  std::memcpy(g_masks, masks, sizeof masks);
  std::memcpy(g_heapMem, p, sizeof g_heapMem); p += sizeof g_heapMem;
  std::memcpy(g_linearMem, p, sizeof g_linearMem); p += sizeof g_linearMem;
  std::memcpy(g_lateMem, p, sizeof g_lateMem); p += sizeof g_lateMem;
  if (!g_sram.empty()) std::memcpy(g_sram.data(), p, g_sram.size());
  g_cheats.clear();
  return true;
}

// Cheats, format "raw": lines "AAAAAAAA VV" (hex device address inside the heap/linear/late regions, hex byte value),
// separated by '+' or newline. While enabled, every retro_run writes the bytes. Count of enabled entries: heap kCheatCountOff.
FB_EXPORT void retro_cheat_reset() { g_cheats.clear(); }
FB_EXPORT void retro_cheat_set(unsigned index, bool enabled, const char* code) {
  Cheat c;
  c.index = index;
  c.enabled = enabled;
  std::string text = code ? code : "";
  for (char& ch : text)
    if (ch == '+') ch = '\n';
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    unsigned long addr = 0, val = 0;
    if (std::sscanf(line.c_str(), " %lx %lx", &addr, &val) == 2 && val <= 0xFF) c.writes.push_back({static_cast<uint64_t>(addr), static_cast<uint8_t>(val)});
  }
  for (Cheat& existing : g_cheats)
    if (existing.index == index) { existing = c; return; }
  g_cheats.push_back(c);
}
FB_EXPORT bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }
FB_EXPORT unsigned retro_get_region() { return RETRO_REGION_NTSC; }
FB_EXPORT void retro_set_controller_port_device(unsigned, unsigned) {}
