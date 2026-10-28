#include "vk_render.h"

#include <QLibrary>
#include <QLoggingCategory>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include "gpu_display_target.h"
#include "gpu_encode_target.h"
#include "hw_render.h"

#if UZUME_HAVE_VULKAN
#define VK_NO_PROTOTYPES 1
#ifdef _WIN32  // VK_KHR_external_memory_win32 / VK_KHR_win32_keyed_mutex (zero-copy display into D3D11 textures)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <libretro.h>
#include <vulkan/vulkan.h>

#include "libretro_vulkan.h"

#if !defined(_WIN32) && defined(__unix__) && !defined(__APPLE__)
#include <unistd.h>
#endif
#endif

Q_LOGGING_CATEGORY(lcVk, "uzume.vulkan")

namespace uzume::emu {

namespace {
std::atomic<bool> g_forceUnavailable{false};
std::atomic<bool> g_forceCreateFail{false};
std::atomic<unsigned> g_highestApi{0};        // from the last successful probe()
std::atomic<unsigned> g_highestApiForTest{0};  // 0 = off
std::atomic<bool> g_extMissingForTest{false};
std::mutex g_luidMutex;
QByteArray g_luidForTest;  // 8 bytes or empty
}

void VkRenderContext::setUnavailableForTest(bool unavailable) { g_forceUnavailable.store(unavailable); }

void VkRenderContext::setCreateFailsForTest(bool fails) { g_forceCreateFail.store(fails); }
void VkRenderContext::setExternalMemoryMissingForTest(bool missing) { g_extMissingForTest.store(missing); }
void VkRenderContext::setDeviceLuidForTest(const QByteArray& luid8) {
  std::lock_guard<std::mutex> lock(g_luidMutex);
  g_luidForTest = luid8.size() == 8 ? luid8 : QByteArray();
}
void VkRenderContext::setHighestApiForTest(unsigned packedVersion) { g_highestApiForTest.store(packedVersion); }
unsigned VkRenderContext::highestUsableApi() {
  const unsigned forced = g_highestApiForTest.load();
  return forced ? forced : g_highestApi.load();
}
unsigned VkRenderContext::normalizeApiVersion(unsigned versionMajor) {
  constexpr unsigned kVulkan10 = 1u << 22;
  if (versionMajor >= kVulkan10) return versionMajor & ~0xfffu;  // major.minor only: a core's patch level never refuses a driver
  return std::max(1u, versionMajor) << 22;
}
bool VkRenderContext::apiSatisfied(unsigned versionMajor, unsigned highest) {
  return highest == 0 || normalizeApiVersion(versionMajor) <= (highest & ~0xfffu);
}

QString VkRenderContext::describeApi(unsigned apiVersion) {
  return QStringLiteral("Vulkan %1.%2").arg((apiVersion >> 22) & 0x7f).arg((apiVersion >> 12) & 0x3ff);
}

QString VkRenderContext::describeGpu(const QString& deviceName, unsigned vendorId, unsigned v) {
  QString ver;
  if (vendorId == 0x10DE) {  // NVIDIA: 10.8.8.6 bits
    ver = QStringLiteral("%1.%2.%3").arg((v >> 22) & 0x3ff).arg((v >> 14) & 0xff).arg((v >> 6) & 0xff);
  }
#ifdef Q_OS_WIN
  else if (vendorId == 0x8086) {  // Intel on Windows: 18.14 bits
    ver = QStringLiteral("%1.%2").arg(v >> 14).arg(v & 0x3fff);
  }
#endif
  else {
    ver = QStringLiteral("%1.%2.%3").arg(v >> 22).arg((v >> 12) & 0x3ff).arg(v & 0xfff);
  }
  return QStringLiteral("%1 · Driver %2").arg(deviceName, ver);
}

#if !UZUME_HAVE_VULKAN

struct VkRenderContext::Impl {};
VkRenderContext::VkRenderContext() = default;
VkRenderContext::~VkRenderContext() = default;
bool VkRenderContext::compiledIn() { return false; }
bool VkRenderContext::probe(QString* why) {
  if (why) *why = QStringLiteral("built without the Vulkan headers");
  return false;
}
void VkRenderContext::setNegotiationInterface(const void*) {}
void VkRenderContext::setRequiredApiVersion(unsigned) {}
void VkRenderContext::setExternalMemoryWanted(bool) {}
void VkRenderContext::setDisplayTarget(std::shared_ptr<GpuDisplayTarget>) {}
bool VkRenderContext::hasDisplayTarget() const { return false; }
quint64 VkRenderContext::displayBlitCount() const { return 0; }
void VkRenderContext::setEncodeTarget(std::shared_ptr<GpuEncodeTarget>) {}
bool VkRenderContext::hasEncodeTarget() const { return false; }
quint64 VkRenderContext::encodeBlitCount() const { return 0; }
bool VkRenderContext::repeatEncode() { return false; }
bool VkRenderContext::create(QString* error) {
  if (error) *error = QStringLiteral("built without the Vulkan headers");
  return false;
}
void VkRenderContext::destroy() {}
bool VkRenderContext::hasContext() const { return false; }
const void* VkRenderContext::renderInterface() const { return nullptr; }
QString VkRenderContext::apiText() const { return {}; }
QString VkRenderContext::gpuText() const { return {}; }
QImage VkRenderContext::presentFrame(int, int, const QSize&, bool, const QSize&, bool*, bool, EncodeOutcome*) { return {}; }
void VkRenderContext::presentDuplicate() {}
quint64 VkRenderContext::frameCount() const { return 0; }

#else  // UZUME_HAVE_VULKAN

namespace {

QLibrary* loaderLibrary() {
  // Loaded once and kept for the process lifetime: unloading a Vulkan loader (and the ICD behind it) is a known source
  // of crashes at dlclose time.
  static QLibrary* lib = [] {
    const char* names[] = {"vulkan-1", "libvulkan.so.1", "vulkan", "libvulkan.1.dylib"};
    for (const char* n : names) {
      auto* l = new QLibrary(QString::fromLatin1(n));
      if (l->load()) return l;
      delete l;
    }
    return static_cast<QLibrary*>(nullptr);
  }();
  return lib;
}

struct BytesPerFrame {
  enum Mode { None, Bgra8, Rgba8, A2B10G10R10, A2R10G10B10, Rgb565 } mode = None;
  int bpp = 0;
};

BytesPerFrame formatMode(VkFormat f) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: return {BytesPerFrame::Bgra8, 4};
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return {BytesPerFrame::Rgba8, 4};
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return {BytesPerFrame::A2B10G10R10, 4};
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return {BytesPerFrame::A2R10G10B10, 4};
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return {BytesPerFrame::Rgb565, 2};
    default: return {};
  }
}

// ---- zero-copy display: what this platform can import (ADR 0034 D8)
#if defined(_WIN32)
#define UZUME_VK_EXTERNAL 1
constexpr VkExternalMemoryHandleTypeFlagBits kNativeHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
constexpr ExternalImage::HandleType kNativeType = ExternalImage::HandleType::D3D11TextureNt;
constexpr const char* kPlatformExts[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME};
#elif defined(__unix__) && !defined(__APPLE__)
#define UZUME_VK_EXTERNAL 1
constexpr VkExternalMemoryHandleTypeFlagBits kNativeHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr ExternalImage::HandleType kNativeType = ExternalImage::HandleType::OpaqueFd;
constexpr const char* kPlatformExts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
#else
#define UZUME_VK_EXTERNAL 0
#endif

constexpr uint32_t kDisplaySlots = 3;  // DisplaySlots::kCount of the producers (ring slots per generation)

// Source formats the GPU blit converts exactly like the readback does (sRGB variants would be decoded by the blit).
bool displayFormatOk(VkFormat f) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return true;
    default: return false;
  }
}

}  // namespace

struct VkRenderContext::Impl {
  // ---- loader and functions
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  PFN_vkCreateInstance vkCreateInstance = nullptr;
  PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
  PFN_vkDestroyInstance vkDestroyInstance = nullptr;
  PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
  PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkCreateDevice vkCreateDevice = nullptr;
  PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
  PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;                       // Vulkan 1.1 instance
  PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;
  PFN_vkGetPhysicalDeviceImageFormatProperties2 vkGetPhysicalDeviceImageFormatProperties2 = nullptr;  // Vulkan 1.1 instance

  PFN_vkDestroyDevice vkDestroyDevice = nullptr;
  PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
  PFN_vkQueueSubmit vkQueueSubmit = nullptr;
  PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
  PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
  PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
  PFN_vkCreateFence vkCreateFence = nullptr;
  PFN_vkDestroyFence vkDestroyFence = nullptr;
  PFN_vkWaitForFences vkWaitForFences = nullptr;
  PFN_vkResetFences vkResetFences = nullptr;
  PFN_vkCreateBuffer vkCreateBuffer = nullptr;
  PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkFreeMemory vkFreeMemory = nullptr;
  PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
  PFN_vkMapMemory vkMapMemory = nullptr;
  PFN_vkUnmapMemory vkUnmapMemory = nullptr;
  PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges = nullptr;
  // zero-copy display (optional; only used when the external-memory extensions are enabled)
  PFN_vkCreateImage vkCreateImage = nullptr;
  PFN_vkDestroyImage vkDestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory = nullptr;
  PFN_vkCmdBlitImage vkCmdBlitImage = nullptr;
#ifdef _WIN32
  PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
#endif

  // ---- core inputs
  retro_hw_render_context_negotiation_interface_vulkan neg{};
  bool hasNeg = false;
  uint32_t requiredApi = VK_API_VERSION_1_0;
  uint32_t loaderApiSeen = VK_API_VERSION_1_0;  // instance version the loader reports (createInstance)
  uint32_t highestSeen = 0;                     // pickDevice: max over devices of min(loader, device apiVersion)

  // ---- context
  VkInstance instance = VK_NULL_HANDLE;
  uint32_t instanceApi = VK_API_VERSION_1_0;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props{};
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  bool coreDeviceOk = false;  // negotiation create_device/create_device2 succeeded: destroy_device is owed
  VkDevice wrapperDevice = VK_NULL_HANDLE;
  VkInstance wrapperInstance = VK_NULL_HANDLE;
  bool created = false;
  bool lost = false;
  quint64 frames = 0;

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize bufferSize = 0;
  void* mapped = nullptr;
  bool coherent = true;

  retro_hw_render_interface_vulkan iface{};
  std::mutex queueMutex;

  // ---- per-frame state set by the core
  retro_vulkan_image image{};
  bool hasImage = false;
  bool imageFresh = false;
  std::vector<VkSemaphore> imageSemaphores;
  uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED;
  std::vector<VkCommandBuffer> coreCmds;
  VkSemaphore signalSemaphore = VK_NULL_HANDLE;
  bool warnedFormat = false;

  // ---- zero-copy display (ADR 0034 D8)
  std::shared_ptr<GpuDisplayTarget> display;
  bool extWanted = false;                 // a display or encode target is expected: enable the external-memory extensions
  std::vector<const char*> extNames;      // device extensions enabled for it (empty: none, see extMissing)
  QString extMissing;                     // why extNames is empty although wanted
  std::vector<const char*> wrapperNames;  // scratch for createDeviceWrapper
  bool devLuidValid = false;
  quint8 devLuid[8] = {};
  struct ExtSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool primed = false;  // has been through a release to the external queue family at least once
  };
  ExtSlot extSlots[kDisplaySlots];
  quint64 extGeneration = 0;
  bool luidChecked = false;
  bool importFormatChecked = false;
  VkFormat blitFormat = VK_FORMAT_UNDEFINED;  // last source format checked for the blit
  bool blitFormatOk = false;
  bool blitLinear = false;
  quint64 displayBlits = 0;
  struct DisplayJob {  // one blit of the frame into an imported image (display slot or the encode image)
    bool active = false;
    ExtSlot* slot = nullptr;
    ExternalImage ext;
    int sw = 0, sh = 0;  // source region
    int dw = 0, dh = 0;  // destination region
  };

  // ---- GPU-direct Session encoding (ADR 0034 D9): one imported image of the target's encode size
  std::shared_ptr<GpuEncodeTarget> encode;
  ExtSlot encSlot;
  ExternalImage encExt;            // what the target handed out for encSlot (keyed mutex keys)
  bool encAttached = false;        // encSlot holds the image of the current attachExternal()
  QSize encSize;                   // size of that image
  bool encLuidChecked = false;
  bool encImportChecked = false;
  bool encHasFrame = false;        // the target captured at least one frame (repeatEncode)
  quint64 encodeBlits = 0;

  ~Impl() { destroy(); }

  bool loadLoader(QString* error) {
    if (gipa) return true;
    QLibrary* lib = loaderLibrary();
    if (!lib) {
      if (error) *error = QStringLiteral("no Vulkan loader found");
      return false;
    }
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(lib->resolve("vkGetInstanceProcAddr"));
    if (!gipa) {
      if (error) *error = QStringLiteral("Vulkan loader without vkGetInstanceProcAddr");
      return false;
    }
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    vkEnumerateInstanceVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (!vkCreateInstance) {
      if (error) *error = QStringLiteral("Vulkan loader without vkCreateInstance");
      return false;
    }
    return true;
  }

  template <typename T>
  void loadI(T& fn, const char* name) {
    fn = reinterpret_cast<T>(gipa(instance, name));
  }
  template <typename T>
  void loadD(T& fn, const char* name) {
    fn = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
  }

  // The instance, created directly or through the core's v2 create_instance.
  static VkInstance createInstanceWrapper(void* opaque, const VkInstanceCreateInfo* ci) {
    auto* self = static_cast<Impl*>(opaque);
    VkInstance inst = VK_NULL_HANDLE;
    if (self->vkCreateInstance(ci, nullptr, &inst) != VK_SUCCESS) return VK_NULL_HANDLE;
    self->wrapperInstance = inst;
    return inst;
  }
  static VkDevice createDeviceWrapper(VkPhysicalDevice g, void* opaque, const VkDeviceCreateInfo* ci) {
    auto* self = static_cast<Impl*>(opaque);
    VkDevice dev = VK_NULL_HANDLE;
    // The core's create info plus the extensions the zero-copy display needs (the core's own list stays as it is).
    VkDeviceCreateInfo merged = *ci;
    self->wrapperNames.assign(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + (ci->ppEnabledExtensionNames ? ci->enabledExtensionCount : 0));
    for (const char* e : self->extNames) {
      bool have = false;
      for (const char* n : self->wrapperNames) have = have || std::strcmp(n, e) == 0;
      if (!have) self->wrapperNames.push_back(e);
    }
    if (!self->extNames.empty()) {
      merged.enabledExtensionCount = static_cast<uint32_t>(self->wrapperNames.size());
      merged.ppEnabledExtensionNames = self->wrapperNames.data();
    }
    if (self->vkCreateDevice(g, &merged, nullptr, &dev) != VK_SUCCESS) return VK_NULL_HANDLE;
    self->wrapperDevice = dev;
    return dev;
  }

  bool createInstance(QString* error) {
    uint32_t loaderApi = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderApi);
    loaderApiSeen = loaderApi;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Uzume";
    app.pEngineName = "Uzume";
    app.apiVersion = std::min<uint32_t>(std::max<uint32_t>(requiredApi, VK_API_VERSION_1_1), loaderApi);
    if (hasNeg && neg.get_application_info) {
      if (const VkApplicationInfo* a = neg.get_application_info()) {
        app.pApplicationName = a->pApplicationName;
        app.applicationVersion = a->applicationVersion;
        app.pEngineName = a->pEngineName;
        app.engineVersion = a->engineVersion;
        app.apiVersion = std::min<uint32_t>(std::max<uint32_t>(app.apiVersion, a->apiVersion), loaderApi);
      }
    }
    if (app.apiVersion < requiredApi) {
      if (error) *error = QStringLiteral("the Vulkan loader is older than the core requires");
      return false;
    }
    instanceApi = app.apiVersion;
    wrapperInstance = VK_NULL_HANDLE;
    if (hasNeg && neg.interface_version >= 2 && neg.create_instance) {
      instance = neg.create_instance(gipa, &app, &Impl::createInstanceWrapper, this);
      if (instance == VK_NULL_HANDLE) {
        qCInfo(lcVk) << "core create_instance failed; creating the instance ourselves";
        if (wrapperInstance != VK_NULL_HANDLE) {  // our wrapper created one before the core gave up: not ours to keep
          auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(wrapperInstance, "vkDestroyInstance"));
          if (destroy) destroy(wrapperInstance, nullptr);
          wrapperInstance = VK_NULL_HANDLE;
        }
      }
    }
    if (instance == VK_NULL_HANDLE) {
      VkInstanceCreateInfo ci{};
      ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
      ci.pApplicationInfo = &app;
      const VkResult r = vkCreateInstance(&ci, nullptr, &instance);
      if (r != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        instance = VK_NULL_HANDLE;
        if (error) *error = QStringLiteral("vkCreateInstance failed (%1)").arg(static_cast<int>(r));
        return false;
      }
    }
    loadI(vkDestroyInstance, "vkDestroyInstance");
    loadI(vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    loadI(vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    loadI(vkGetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    loadI(vkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    loadI(vkCreateDevice, "vkCreateDevice");
    loadI(vkGetDeviceProcAddr, "vkGetDeviceProcAddr");
    loadI(vkEnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
    loadI(vkGetPhysicalDeviceFormatProperties, "vkGetPhysicalDeviceFormatProperties");
    if (instanceApi >= VK_API_VERSION_1_1) {
      loadI(vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
      loadI(vkGetPhysicalDeviceImageFormatProperties2, "vkGetPhysicalDeviceImageFormatProperties2");
    }
    if (!vkDestroyInstance || !vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties ||
        !vkGetPhysicalDeviceQueueFamilyProperties || !vkGetPhysicalDeviceMemoryProperties || !vkCreateDevice ||
        !vkGetDeviceProcAddr) {
      if (error) *error = QStringLiteral("incomplete Vulkan instance functions");
      return false;
    }
    return true;
  }

  // First discrete GPU, else integrated, else anything (virtual, CPU/lavapipe) with a graphics queue and enough API.
  bool pickDevice(QString* error) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(instance, &n, devs.data());
    devs.resize(n);
    int bestRank = 99;
    for (VkPhysicalDevice d : devs) {
      VkPhysicalDeviceProperties p{};
      vkGetPhysicalDeviceProperties(d, &p);
      highestSeen = std::max(highestSeen, std::min<uint32_t>(p.apiVersion, loaderApiSeen));
      if (p.apiVersion < requiredApi) continue;
      uint32_t qn = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qf(qn);
      if (qn) vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
      bool gfx = false;
      for (const auto& q : qf) gfx = gfx || (q.queueFlags & VK_QUEUE_GRAPHICS_BIT);
      if (!gfx) continue;
      int rank = 4;
      switch (p.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: rank = 0; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: rank = 1; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: rank = 2; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: rank = 3; break;
        default: break;
      }
      if (rank < bestRank) {
        bestRank = rank;
        gpu = d;
        props = p;
      }
    }
    if (gpu == VK_NULL_HANDLE) {
      if (error) *error = n == 0 ? QStringLiteral("no Vulkan device") : QStringLiteral("no Vulkan device with a graphics queue");
      return false;
    }
    return true;
  }

  // Which device extensions the zero-copy display needs and the device supports (extNames), the device LUID. Never fails:
  // an unsupported device only leaves extNames empty and says why in extMissing.
  void planExternal() {
    extNames.clear();
    extMissing.clear();
    devLuidValid = false;
    if (vkGetPhysicalDeviceProperties2) {
      VkPhysicalDeviceIDProperties idp{};
      idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
      VkPhysicalDeviceProperties2 p2{};
      p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      p2.pNext = &idp;
      vkGetPhysicalDeviceProperties2(gpu, &p2);
      devLuidValid = idp.deviceLUIDValid == VK_TRUE;
      std::memcpy(devLuid, idp.deviceLUID, 8);
    }
    if (!extWanted) return;
#if UZUME_VK_EXTERNAL
    if (g_extMissingForTest.load()) {
      extMissing = QStringLiteral("the Vulkan device has no VK_KHR_external_memory");
      return;
    }
    if (instanceApi < VK_API_VERSION_1_1 || !vkGetPhysicalDeviceImageFormatProperties2) {
      extMissing = QStringLiteral("the Vulkan instance is older than 1.1");
      return;
    }
    uint32_t n = 0;
    std::vector<VkExtensionProperties> avail;
    if (vkEnumerateDeviceExtensionProperties) {
      vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
      avail.resize(n);
      if (n) vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, avail.data());
      avail.resize(n);
    }
    auto has = [&](const char* name) {
      for (const auto& e : avail)
        if (std::strcmp(e.extensionName, name) == 0) return true;
      return false;
    };
    const bool core11 = props.apiVersion >= VK_API_VERSION_1_1;  // promoted: no extension to enable
    std::vector<const char*> names;
    QString missing;
    auto need = [&](const char* name, bool promoted) {
      if (promoted && core11) return;
      if (has(name)) names.push_back(name);
      else if (missing.isEmpty()) missing = QString::fromLatin1(name);
    };
    need(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, true);
    need(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, true);
    need(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, true);
    for (const char* e : kPlatformExts) need(e, false);
    if (!missing.isEmpty()) {
      extMissing = QStringLiteral("the Vulkan device has no %1").arg(missing);
      return;
    }
    extNames = std::move(names);
#else
    extMissing = QStringLiteral("not supported on this platform");
#endif
  }

  bool createDevice(QString* error) {
    // Negotiation: the core may create the device itself (v2 create_device2 preferred over v1 create_device).
    if (hasNeg && ((neg.interface_version >= 2 && neg.create_device2) || neg.create_device)) {
      retro_vulkan_context ctx{};
      wrapperDevice = VK_NULL_HANDLE;
      bool ok;
      if (neg.interface_version >= 2 && neg.create_device2) {
        ok = neg.create_device2(&ctx, instance, gpu, VK_NULL_HANDLE, gipa, &Impl::createDeviceWrapper, this);
      } else {
        VkPhysicalDeviceFeatures features{};
        ok = neg.create_device(&ctx, instance, gpu, VK_NULL_HANDLE, gipa, extNames.empty() ? nullptr : extNames.data(),
                               static_cast<unsigned>(extNames.size()), nullptr, 0, &features);
      }
      if (ok && ctx.device != VK_NULL_HANDLE && ctx.queue != VK_NULL_HANDLE) {
        coreDeviceOk = true;
        device = ctx.device;
        queue = ctx.queue;
        family = ctx.queue_family_index;
        if (ctx.gpu != VK_NULL_HANDLE && ctx.gpu != gpu) {
          gpu = ctx.gpu;
          vkGetPhysicalDeviceProperties(gpu, &props);
          planExternal();  // the LUID and the extension support described the GPU we picked, not the core's
        }
        return true;
      }
      qCInfo(lcVk) << "core device creation declined; creating the device ourselves";
      if (ok && neg.destroy_device) neg.destroy_device();
      if (wrapperDevice != VK_NULL_HANDLE) {
        // Created through the wrapper but unusable: not ours to keep.
        auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(vkGetDeviceProcAddr(wrapperDevice, "vkDestroyDevice"));
        if (destroy) destroy(wrapperDevice, nullptr);
        wrapperDevice = VK_NULL_HANDLE;
      }
    }
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qf.data());
    int pick = -1;
    for (uint32_t i = 0; i < qn && pick < 0; ++i) {
      if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) pick = static_cast<int>(i);
    }
    for (uint32_t i = 0; i < qn && pick < 0; ++i) {
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) pick = static_cast<int>(i);
    }
    if (pick < 0) {
      if (error) *error = QStringLiteral("no graphics queue");
      return false;
    }
    family = static_cast<uint32_t>(pick);
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = static_cast<uint32_t>(extNames.size());
    di.ppEnabledExtensionNames = extNames.empty() ? nullptr : extNames.data();
    const VkResult r = vkCreateDevice(gpu, &di, nullptr, &device);
    if (r != VK_SUCCESS) {
      device = VK_NULL_HANDLE;
      if (error) *error = QStringLiteral("vkCreateDevice failed (%1)").arg(static_cast<int>(r));
      return false;
    }
    return true;
  }

  bool loadDeviceFunctions() {
    loadD(vkDestroyDevice, "vkDestroyDevice");
    loadD(vkGetDeviceQueue, "vkGetDeviceQueue");
    loadD(vkDeviceWaitIdle, "vkDeviceWaitIdle");
    loadD(vkQueueSubmit, "vkQueueSubmit");
    loadD(vkCreateCommandPool, "vkCreateCommandPool");
    loadD(vkDestroyCommandPool, "vkDestroyCommandPool");
    loadD(vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    loadD(vkResetCommandBuffer, "vkResetCommandBuffer");
    loadD(vkBeginCommandBuffer, "vkBeginCommandBuffer");
    loadD(vkEndCommandBuffer, "vkEndCommandBuffer");
    loadD(vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
    loadD(vkCmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
    loadD(vkCreateFence, "vkCreateFence");
    loadD(vkDestroyFence, "vkDestroyFence");
    loadD(vkWaitForFences, "vkWaitForFences");
    loadD(vkResetFences, "vkResetFences");
    loadD(vkCreateBuffer, "vkCreateBuffer");
    loadD(vkDestroyBuffer, "vkDestroyBuffer");
    loadD(vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    loadD(vkAllocateMemory, "vkAllocateMemory");
    loadD(vkFreeMemory, "vkFreeMemory");
    loadD(vkBindBufferMemory, "vkBindBufferMemory");
    loadD(vkMapMemory, "vkMapMemory");
    loadD(vkUnmapMemory, "vkUnmapMemory");
    loadD(vkInvalidateMappedMemoryRanges, "vkInvalidateMappedMemoryRanges");
    if (!extNames.empty()) {
      loadD(vkCreateImage, "vkCreateImage");
      loadD(vkDestroyImage, "vkDestroyImage");
      loadD(vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
      loadD(vkBindImageMemory, "vkBindImageMemory");
      loadD(vkCmdBlitImage, "vkCmdBlitImage");
#ifdef _WIN32
      loadD(vkGetMemoryWin32HandlePropertiesKHR, "vkGetMemoryWin32HandlePropertiesKHR");
      const bool win = vkGetMemoryWin32HandlePropertiesKHR != nullptr;
#else
      const bool win = true;
#endif
      if (!vkCreateImage || !vkDestroyImage || !vkGetImageMemoryRequirements || !vkBindImageMemory || !vkCmdBlitImage || !win) {
        extNames.clear();  // enabled on the device but not callable: the display stays Unavailable
        extMissing = QStringLiteral("the Vulkan device does not expose the image import functions");
      }
    }
    return vkDestroyDevice && vkDeviceWaitIdle && vkQueueSubmit && vkCreateCommandPool && vkAllocateCommandBuffers &&
           vkBeginCommandBuffer && vkEndCommandBuffer && vkCmdPipelineBarrier && vkCmdCopyImageToBuffer && vkCreateFence &&
           vkWaitForFences && vkResetFences && vkCreateBuffer && vkGetBufferMemoryRequirements && vkAllocateMemory &&
           vkBindBufferMemory && vkMapMemory && vkResetCommandBuffer && vkDestroyCommandPool && vkDestroyFence &&
           vkDestroyBuffer && vkFreeMemory && vkUnmapMemory && vkInvalidateMappedMemoryRanges && vkGetDeviceQueue;
  }

  bool createFrameResources(QString* error) {
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = family;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateCommandPool(device, &pi, nullptr, &pool) != VK_SUCCESS) goto fail;
    ai.commandPool = pool;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) goto fail;
    if (vkCreateFence(device, &fi, nullptr, &fence) != VK_SUCCESS) goto fail;
    return true;
  fail:
    if (error) *error = QStringLiteral("Vulkan frame resources could not be created");
    return false;
  }

  bool ensureBuffer(VkDeviceSize size) {
    if (buffer != VK_NULL_HANDLE && bufferSize >= size) return true;
    freeBuffer();
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, nullptr, &buffer) != VK_SUCCESS) {
      buffer = VK_NULL_HANDLE;
      return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    const VkMemoryPropertyFlags prefs[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT};
    int type = -1;
    VkMemoryPropertyFlags flags = 0;
    for (VkMemoryPropertyFlags want : prefs) {
      for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; ++i) {
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
          type = static_cast<int>(i);
          flags = mp.memoryTypes[i].propertyFlags;
        }
      }
      if (type >= 0) break;
    }
    if (type < 0) return freeBuffer(), false;
    VkMemoryAllocateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(device, &mi, nullptr, &memory) != VK_SUCCESS) return memory = VK_NULL_HANDLE, freeBuffer(), false;
    if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS ||
        vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      mapped = nullptr;
      return freeBuffer(), false;
    }
    coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    bufferSize = size;
    return true;
  }

  void freeBuffer() {
    if (mapped) vkUnmapMemory(device, memory);
    mapped = nullptr;
    if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    bufferSize = 0;
  }

  void destroy() {
    if (device != VK_NULL_HANDLE) {
      if (vkDeviceWaitIdle) vkDeviceWaitIdle(device);
      freeExternal();
      if (display) {
        display->release(true);  // the target frees what it handed out; nothing of it is in use any more
        display.reset();
      }
      releaseEncode();  // the imported image first, then the target's own side (detach)
      if (vkDestroyFence && fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
      fence = VK_NULL_HANDLE;
      if (vkDestroyCommandPool && pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
      pool = VK_NULL_HANDLE;
      cmd = VK_NULL_HANDLE;
      if (vkUnmapMemory) freeBuffer();
    }
    if (coreDeviceOk && neg.destroy_device) neg.destroy_device();  // auxiliary core resources, before the device goes
    coreDeviceOk = false;
    if (device != VK_NULL_HANDLE && vkDestroyDevice) vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    if (instance != VK_NULL_HANDLE && vkDestroyInstance) vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
    gpu = VK_NULL_HANDLE;
    created = false;
    hasImage = imageFresh = false;
    imageSemaphores.clear();
    coreCmds.clear();
    signalSemaphore = VK_NULL_HANDLE;
    iface = retro_hw_render_interface_vulkan{};
    if (display) {  // never got a device: the target is still released
      display->release(true);
      display.reset();
    }
    if (encode) releaseEncode();
    luidChecked = importFormatChecked = false;
    blitFormat = VK_FORMAT_UNDEFINED;
    extGeneration = 0;
  }

  // ---- zero-copy display (ADR 0034 D8)

  // Frees every imported image (the target's handles are not ours: the memory objects only drop their references).
  void freeExternal() {
    for (ExtSlot& e : extSlots) {
      if (e.image != VK_NULL_HANDLE && vkDestroyImage) vkDestroyImage(device, e.image, nullptr);
      if (e.memory != VK_NULL_HANDLE && vkFreeMemory) vkFreeMemory(device, e.memory, nullptr);
      e = ExtSlot{};
    }
    extGeneration = 0;
  }

  // The target cannot show the frames: tell it (consumers fall back to readback frames), release it, forget it.
  void dropDisplay(const QString& why) {
    freeExternal();
    if (display) {
      display->fail(why);
      display->release(true);
    }
    display.reset();
    qCWarning(lcVk).noquote() << QStringLiteral("Display target dropped (%1); the game view uses readback frames").arg(why);
  }
  // Zero-copy display is not applicable with this device/core: the target reports it as Unavailable (quiet).
  void displayUnavailable(const QString& why) {
    freeExternal();
    if (display) {
      display->markUnavailable(why);
      display->release(true);
    }
    display.reset();
    qCInfo(lcVk).noquote() << QStringLiteral("Display target not applicable (%1); the game view uses readback frames").arg(why);
  }
  // The target reported itself unavailable in acquireExternal(): release and forget, no second report.
  void releaseDisplayQuietly() {
    freeExternal();
    if (display) display->release(true);
    display.reset();
    qCInfo(lcVk) << "Display target not applicable; the game view uses readback frames";
  }

  // The Vulkan device's LUID (the test override included).
  AdapterLuid deviceLuid() const {
    AdapterLuid l;
    l.valid = devLuidValid;
    std::memcpy(l.bytes, devLuid, 8);
    std::lock_guard<std::mutex> lock(g_luidMutex);
    if (g_luidForTest.size() == 8) {
      std::memcpy(l.bytes, g_luidForTest.constData(), 8);
      l.valid = true;
    }
    return l;
  }
  enum class LuidCheck { Unknown, Match, Mismatch };
  LuidCheck checkLuid(const AdapterLuid& t) const {
    if (!t.valid) return LuidCheck::Unknown;
    const AdapterLuid dev = deviceLuid();
    return dev.valid && std::memcmp(dev.bytes, t.bytes, 8) == 0 ? LuidCheck::Match : LuidCheck::Mismatch;
  }

  // Whether the device can import RGBA8 optimal-tiling images of the platform's handle type as transfer destinations.
  bool checkImportFormat(ExternalImage::HandleType type, bool bgra, QString* why) {
#if UZUME_VK_EXTERNAL
    if (type != kNativeType) {
      *why = QStringLiteral("the target's texture handle type is not importable here");
      return false;
    }
    VkPhysicalDeviceExternalImageFormatInfo ei{};
    ei.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    ei.handleType = kNativeHandle;
    VkPhysicalDeviceImageFormatInfo2 fi{};
    fi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    fi.pNext = &ei;
    fi.format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    fi.type = VK_IMAGE_TYPE_2D;
    fi.tiling = VK_IMAGE_TILING_OPTIMAL;
    fi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkExternalImageFormatProperties efp{};
    efp.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 fp{};
    fp.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    fp.pNext = &efp;
    const VkResult r = vkGetPhysicalDeviceImageFormatProperties2(gpu, &fi, &fp);
    if (r != VK_SUCCESS) {
      *why = QStringLiteral("the Vulkan device cannot import RGBA8 textures of the display (%1)").arg(static_cast<int>(r));
      return false;
    }
    const VkExternalMemoryProperties& mp = efp.externalMemoryProperties;
    if (!(mp.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) || !(mp.compatibleHandleTypes & kNativeHandle)) {
      *why = QStringLiteral("the Vulkan device cannot import the display's texture handles");
      return false;
    }
    return true;
#else
    Q_UNUSED(type);
    *why = QStringLiteral("not supported on this platform");
    return false;
#endif
  }

  // vkCreateImage + imported, dedicated memory for one (generation, slot) of the target.
  bool importExternal(const ExternalImage& e, ExtSlot& slot, QString* why) {
#if UZUME_VK_EXTERNAL
    VkExternalMemoryImageCreateInfo emi{};
    emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = kNativeHandle;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = e.bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {static_cast<uint32_t>(e.texSize.width()), static_cast<uint32_t>(e.texSize.height()), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(device, &ici, nullptr, &img);
    if (r != VK_SUCCESS) {
      *why = QStringLiteral("vkCreateImage for the display texture failed (%1)").arg(static_cast<int>(r));
      return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, img, &req);
    uint32_t bits = req.memoryTypeBits;
#ifdef _WIN32
    VkMemoryWin32HandlePropertiesKHR hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
    r = vkGetMemoryWin32HandlePropertiesKHR(device, kNativeHandle, reinterpret_cast<HANDLE>(e.handle), &hp);
    if (r != VK_SUCCESS) {
      vkDestroyImage(device, img, nullptr);
      *why = QStringLiteral("vkGetMemoryWin32HandlePropertiesKHR failed (%1)").arg(static_cast<int>(r));
      return false;
    }
    bits &= hp.memoryTypeBits;
#endif
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    int type = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; ++i)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = static_cast<int>(i);
    for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; ++i)
      if (bits & (1u << i)) type = static_cast<int>(i);
    if (type < 0) {
      vkDestroyImage(device, img, nullptr);
      *why = QStringLiteral("no memory type fits the display texture");
      return false;
    }
    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = img;
#ifdef _WIN32
    VkImportMemoryWin32HandleInfoKHR imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    imp.handleType = kNativeHandle;
    imp.handle = reinterpret_cast<HANDLE>(e.handle);
#else
    VkImportMemoryFdInfoKHR imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    imp.handleType = kNativeHandle;
    imp.fd = ::dup(static_cast<int>(e.handle));  // a successful import takes the fd over; the target keeps its own
    if (imp.fd < 0) {
      vkDestroyImage(device, img, nullptr);
      *why = QStringLiteral("the display texture's file descriptor could not be duplicated");
      return false;
    }
#endif
    imp.pNext = &ded;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &imp;
    mai.allocationSize = e.handleType == ExternalImage::HandleType::OpaqueFd && e.memorySize > req.size ? e.memorySize : req.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(device, &mai, nullptr, &mem);
    if (r != VK_SUCCESS) {
#ifndef _WIN32
      ::close(imp.fd);
#endif
      vkDestroyImage(device, img, nullptr);
      *why = QStringLiteral("importing the display texture failed (%1)").arg(static_cast<int>(r));
      return false;
    }
    r = vkBindImageMemory(device, img, mem, 0);
    if (r != VK_SUCCESS) {
      vkFreeMemory(device, mem, nullptr);
      vkDestroyImage(device, img, nullptr);
      *why = QStringLiteral("binding the display texture failed (%1)").arg(static_cast<int>(r));
      return false;
    }
    slot.image = img;
    slot.memory = mem;
    slot.primed = false;
    return true;
#else
    Q_UNUSED(e);
    Q_UNUSED(slot);
    *why = QStringLiteral("not supported on this platform");
    return false;
#endif
  }

  // Whether the core's image format can be blitted (exactly like the readback converts it) and with which filter.
  bool blitSourceOk() {
    const VkFormat fmt = image.create_info.format;
    if (fmt != blitFormat) {
      blitFormat = fmt;
      blitFormatOk = false;
      blitLinear = false;
      if (displayFormatOk(fmt)) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(gpu, fmt, &fp);
        const VkFormatFeatureFlags f = fp.optimalTilingFeatures;  // tiling is not part of retro_vulkan_image: optimal assumed
        blitFormatOk = (f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
        blitLinear = (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
      }
    }
    return blitFormatOk;
  }

  // Before the frame's submit: decides whether this frame goes to the target and, if so, locks a slot (acquireExternal)
  // and makes its image ready. false: no display copy this frame; display may have been dropped (then it is null).
  bool prepareDisplay(int w, int h, const QSize& limit, DisplayJob* job) {
    if (!display->wanted()) return false;
    if (extNames.empty()) {
      displayUnavailable(extMissing.isEmpty() ? QStringLiteral("the Vulkan device was created without the external-memory extensions") : extMissing);
      return false;
    }
    if (!blitSourceOk()) {
      displayUnavailable(QStringLiteral("the frame format (%1) is not copied on the GPU").arg(static_cast<int>(image.create_info.format)));
      return false;
    }
    if (!luidChecked && checkLuid(display->adapterLuid()) == LuidCheck::Mismatch) {
      displayUnavailable(QStringLiteral("Vulkan device is on another adapter"));
      return false;
    }
    const QSize ds = HwRenderContext::scaledReadbackSize(w, h, limit);
    ExternalImage e;
    switch (display->acquireExternal(ds.width(), ds.height(), &e)) {
      case GpuDisplayTarget::Acquire::Skip:
      case GpuDisplayTarget::Acquire::NotReady: return false;
      case GpuDisplayTarget::Acquire::Unavailable: releaseDisplayQuietly(); return false;
      case GpuDisplayTarget::Acquire::Failed: dropDisplay(QStringLiteral("acquire failed")); return false;
      case GpuDisplayTarget::Acquire::Ok: break;
    }
    auto bail = [&](const QString& why) {
      display->abort();
      dropDisplay(why);
      return false;
    };
    if (!luidChecked) {  // the target only knows its adapter once it is ready: check again now that it handed out an image
      if (checkLuid(display->adapterLuid()) == LuidCheck::Mismatch) {
        display->abort();
        displayUnavailable(QStringLiteral("Vulkan device is on another adapter"));
        return false;
      }
      luidChecked = true;
    }
    if (!importFormatChecked) {
      QString why;
      if (!checkImportFormat(e.handleType, e.bgra, &why)) {
        display->abort();
        displayUnavailable(why);
        return false;
      }
      importFormatChecked = true;
    }
    if (e.slot < 0 || e.slot >= static_cast<int>(kDisplaySlots) || e.texSize.width() < ds.width() || e.texSize.height() < ds.height())
      return bail(QStringLiteral("the display texture does not fit the frame"));
    if (e.generation != extGeneration) {
      freeExternal();
      extGeneration = e.generation;
      qCInfo(lcVk).noquote() << QStringLiteral("Zero-copy display: importing generation %1 (%2x%3 textures)")
                                    .arg(e.generation).arg(e.texSize.width()).arg(e.texSize.height());
    }
    ExtSlot& slot = extSlots[e.slot];
    if (slot.image == VK_NULL_HANDLE) {
      QString why;
      if (!importExternal(e, slot, &why)) return bail(why);
    }
    job->active = true;
    job->slot = &slot;
    job->ext = e;
    job->sw = w;
    job->sh = h;
    job->dw = ds.width();
    job->dh = ds.height();
    return true;
  }

  // Display or encode part of the frame's command buffer: acquire the external image, blit, release it to the external family.
  void recordBlit(const DisplayJob& j, VkImageLayout srcLayout, const VkImageSubresourceRange& sr) {
    ExtSlot& slot = *j.slot;
    VkImageSubresourceRange er{};
    er.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    er.levelCount = 1;
    er.layerCount = 1;
    VkImageMemoryBarrier acq{};
    acq.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    acq.dstQueueFamilyIndex = family;
    acq.image = slot.image;
    acq.subresourceRange = er;
    acq.srcAccessMask = 0;
    acq.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    acq.oldLayout = slot.primed ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;  // first use may discard
    acq.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &acq);
    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = sr.baseMipLevel;
    blit.srcSubresource.baseArrayLayer = sr.baseArrayLayer;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1] = {j.sw, j.sh, 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1] = {j.dw, j.dh, 1};
    const bool scaled = j.sw != j.dw || j.sh != j.dh;
    vkCmdBlitImage(cmd, image.create_info.image, srcLayout, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   scaled && blitLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    VkImageMemoryBarrier rel = acq;
    rel.srcQueueFamilyIndex = family;
    rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    rel.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    rel.dstAccessMask = 0;
    rel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    rel.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &rel);
    slot.primed = true;
  }

  // ---- GPU-direct Session encoding (ADR 0034 D9)

  void freeEncodeImage() {
    if (encSlot.image != VK_NULL_HANDLE && vkDestroyImage) vkDestroyImage(device, encSlot.image, nullptr);
    if (encSlot.memory != VK_NULL_HANDLE && vkFreeMemory) vkFreeMemory(device, encSlot.memory, nullptr);
    encSlot = ExtSlot{};
    encAttached = false;
    encSize = QSize();
    encHasFrame = false;
  }
  // Frees what was imported, then lets the target release its side (idempotent), then forgets it.
  void releaseEncode() {
    freeEncodeImage();
    if (encode) encode->detach(true);
    encode.reset();
    encLuidChecked = encImportChecked = false;
  }
  // The target cannot take the frames: tell it (the Session falls back to readback frames), release it, forget it.
  void dropEncode(const QString& why) {
    if (encode) encode->fail(why);
    releaseEncode();
    qCWarning(lcVk).noquote() << QStringLiteral("Session encode target dropped (%1); the Session encoder gets readback frames").arg(why);
  }
  // GPU-direct encoding is not applicable with this device/core: the target reports it as Unavailable (quiet).
  void encodeUnavailable(const QString& why) {
    if (encode) encode->markUnavailable(why);
    releaseEncode();
    qCInfo(lcVk).noquote() << QStringLiteral("Session encode target not applicable (%1); the Session encoder gets readback frames").arg(why);
  }
  // The target reported itself unavailable in attachExternal(): release and forget, no second report.
  void releaseEncodeQuietly() {
    releaseEncode();
    qCInfo(lcVk) << "Session encode target not applicable; the Session encoder gets readback frames";
  }

  // Before the frame's submit: decides whether this frame goes to the encode target and makes its image ready (attaches at
  // the first frame and whenever the fitted size changes). false: no encode copy this frame; encode may have been dropped.
  bool prepareEncode(int w, int h, DisplayJob* job) {
    if (!encode->wanted()) return false;
    if (extNames.empty()) {
      encodeUnavailable(extMissing.isEmpty() ? QStringLiteral("the Vulkan device was created without the external-memory extensions") : extMissing);
      return false;
    }
    if (!blitSourceOk()) {
      encodeUnavailable(QStringLiteral("the frame format (%1) is not copied on the GPU").arg(static_cast<int>(image.create_info.format)));
      return false;
    }
    if (!encLuidChecked && checkLuid(encode->adapterLuid()) == LuidCheck::Mismatch) {
      encodeUnavailable(QStringLiteral("Vulkan device is on another adapter"));
      return false;
    }
    const QSize es = HwRenderContext::encodeSizeFor(w, h, encode->maxSize());
    if (!encAttached || es != encSize) {
      ExternalImage e;
      GpuEncodeTarget::VulkanDevice vd;
      vd.luid = deviceLuid();
      vd.vendorId = props.vendorID;
      switch (encode->attachExternal(es.width(), es.height(), vd, &e)) {
        case GpuEncodeTarget::Attach::NotReady: return false;  // the target is still being set up; the readback keeps encoding
        case GpuEncodeTarget::Attach::Unavailable: releaseEncodeQuietly(); return false;
        case GpuEncodeTarget::Attach::Failed: dropEncode(QStringLiteral("attach failed")); return false;
        case GpuEncodeTarget::Attach::Ok: break;
      }
      if (!encLuidChecked) {  // the target only knows its adapter once it is ready
        if (checkLuid(encode->adapterLuid()) == LuidCheck::Mismatch) {
          encodeUnavailable(QStringLiteral("Vulkan device is on another adapter"));
          return false;
        }
        encLuidChecked = true;
      }
      if (!encImportChecked) {
        QString why;
        if (!checkImportFormat(e.handleType, e.bgra, &why)) {
          encodeUnavailable(why);
          return false;
        }
        encImportChecked = true;
      }
      if (e.slot != 0 || e.texSize.width() < es.width() || e.texSize.height() < es.height()) {
        dropEncode(QStringLiteral("the encode texture does not fit the frame"));
        return false;
      }
      freeEncodeImage();  // the previous size's image is superseded
      QString why;
      if (!importExternal(e, encSlot, &why)) {
        dropEncode(why);
        return false;
      }
      encExt = e;
      encAttached = true;
      encSize = es;
      qCInfo(lcVk).noquote() << QStringLiteral("Session encode image %1x%2 for a %3x%4 frame").arg(es.width()).arg(es.height()).arg(w).arg(h);
    }
    job->active = true;
    job->slot = &encSlot;
    job->ext = encExt;
    job->sw = w;
    job->sh = h;
    job->dw = es.width();
    job->dh = es.height();
    return true;
  }

  // ---- libretro interface callbacks (handle = Impl*)
  static Impl* self(void* h) { return static_cast<Impl*>(h); }
  static void setImage(void* h, const retro_vulkan_image* img, uint32_t nsem, const VkSemaphore* sems, uint32_t srcFamily) {
    Impl* s = self(h);
    if (!img) return;
    s->image = *img;
    s->image.create_info.pNext = nullptr;  // not deep-copied, never read
    s->hasImage = true;
    s->imageFresh = true;
    s->imageSemaphores.assign(sems, sems + (sems ? nsem : 0));
    s->srcFamily = srcFamily;
  }
  static uint32_t getSyncIndex(void*) { return 0; }  // every frame is finished (fence wait) inside video_refresh
  static uint32_t getSyncIndexMask(void*) { return 1; }
  static void setCommandBuffers(void* h, uint32_t n, const VkCommandBuffer* cmds) {
    self(h)->coreCmds.assign(cmds, cmds + (cmds ? n : 0));
  }
  static void waitSyncIndex(void*) {}
  static void lockQueue(void* h) { self(h)->queueMutex.lock(); }
  static void unlockQueue(void* h) { self(h)->queueMutex.unlock(); }
  static void setSignalSemaphore(void* h, VkSemaphore s) { self(h)->signalSemaphore = s; }

  void fillInterface() {
    iface = retro_hw_render_interface_vulkan{};
    iface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    iface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    iface.handle = this;
    iface.instance = instance;
    iface.gpu = gpu;
    iface.device = device;
    iface.get_device_proc_addr = vkGetDeviceProcAddr;
    iface.get_instance_proc_addr = gipa;
    iface.queue = queue;
    iface.queue_index = family;
    iface.set_image = &Impl::setImage;
    iface.get_sync_index = &Impl::getSyncIndex;
    iface.get_sync_index_mask = &Impl::getSyncIndexMask;
    iface.set_command_buffers = &Impl::setCommandBuffers;
    iface.wait_sync_index = &Impl::waitSyncIndex;
    iface.lock_queue = &Impl::lockQueue;
    iface.unlock_queue = &Impl::unlockQueue;
    iface.set_signal_semaphore = &Impl::setSignalSemaphore;
  }

  bool createAll(QString* error) {
    if (g_forceUnavailable.load()) {
      if (error) *error = QStringLiteral("no Vulkan device");
      return false;
    }
    if (!loadLoader(error) || !createInstance(error) || !pickDevice(error)) return false;
    planExternal();
    if (!createDevice(error)) return false;
    loadD(vkGetDeviceQueue, "vkGetDeviceQueue");
    if (queue == VK_NULL_HANDLE && vkGetDeviceQueue) vkGetDeviceQueue(device, family, 0, &queue);  // our own device
    if (queue == VK_NULL_HANDLE || !loadDeviceFunctions()) {
      if (error) *error = QStringLiteral("incomplete Vulkan device functions");
      return false;
    }
    if (!createFrameResources(error)) return false;
    fillInterface();
    created = true;
    qCInfo(lcVk).noquote() << "Vulkan context:" << VkRenderContext::describeApi(props.apiVersion)
                           << VkRenderContext::describeGpu(QString::fromUtf8(props.deviceName), props.vendorID, props.driverVersion);
    return true;
  }

  // Submits the core's command buffers and, with `copy`, our readback copy; waits for the fence.
  bool submit(bool copy, int w, int h, VkDeviceSize copyBytes, const DisplayJob* job = nullptr, const DisplayJob* enc = nullptr) {
    const bool useCopy = copy && hasImage;
    const bool useDisp = job && job->active && hasImage;
    const bool useEnc = enc && enc->active && hasImage;
    const bool coreCmdMode = !coreCmds.empty();
    std::vector<VkCommandBuffer> cmds = coreCmds;
    // The core released the fresh image to our queue family (set_image's src_queue_family): the acquire (and the release back)
    // is owed even when nothing is copied this frame.
    const bool transfer = hasImage && srcFamily != VK_QUEUE_FAMILY_IGNORED && srcFamily != family;
    const bool ownBatch = useCopy || useDisp || useEnc || (transfer && imageFresh);
    std::vector<VkCommandBuffer> mine;  // our own command buffer: runs in a submit of its own after the core's work
    if (ownBatch) {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkResetCommandBuffer(cmd, 0);
      if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return false;
      const VkImage img = image.create_info.image;
      const VkImageLayout layout = image.image_layout;
      const bool general = layout == VK_IMAGE_LAYOUT_GENERAL;
      VkImageSubresourceRange sr{};
      sr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      sr.baseMipLevel = image.create_info.subresourceRange.baseMipLevel;
      sr.levelCount = 1;
      sr.baseArrayLayer = image.create_info.subresourceRange.baseArrayLayer;
      sr.layerCount = 1;
      VkImageMemoryBarrier b{};
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.image = img;
      b.subresourceRange = sr;
      b.srcQueueFamilyIndex = transfer ? srcFamily : VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = transfer ? family : VK_QUEUE_FAMILY_IGNORED;
      b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      b.oldLayout = layout;
      b.newLayout = general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      VkBufferImageCopy region{};
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.mipLevel = sr.baseMipLevel;
      region.imageSubresource.baseArrayLayer = sr.baseArrayLayer;
      region.imageSubresource.layerCount = 1;
      region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
      if (useCopy) vkCmdCopyImageToBuffer(cmd, img, b.newLayout, buffer, 1, &region);
      if (useDisp) recordBlit(*job, b.newLayout, sr);
      if (useEnc) recordBlit(*enc, b.newLayout, sr);
      // Give the image back in the layout and queue family the core handed it over in.
      VkImageMemoryBarrier back = b;
      back.srcQueueFamilyIndex = transfer ? family : VK_QUEUE_FAMILY_IGNORED;
      back.dstQueueFamilyIndex = transfer ? srcFamily : VK_QUEUE_FAMILY_IGNORED;
      back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      back.oldLayout = b.newLayout;
      back.newLayout = layout;
      VkBufferMemoryBarrier hb{};
      hb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      hb.buffer = buffer;
      hb.size = VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, useCopy ? 1 : 0, useCopy ? &hb : nullptr, 1, &back);
      (void)copyBytes;
      if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
      mine.push_back(cmd);
    }
    // Semaphores of set_image are ignored when the core uses set_command_buffers (libretro_vulkan.h).
    std::vector<VkSemaphore> waits;
    if (!coreCmdMode) waits = imageSemaphores;
    std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    // Two batches: the core's work (waits, its command buffers, the signal semaphore) and ours (acquire, copy, blits, release,
    // keyed mutex). A failing keyed mutex acquire then only drops our batch, never the core's work.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    si.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
    si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount = static_cast<uint32_t>(cmds.size());
    si.pCommandBuffers = cmds.empty() ? nullptr : cmds.data();
    VkSemaphore sig = signalSemaphore;
    si.signalSemaphoreCount = sig != VK_NULL_HANDLE ? 1 : 0;
    si.pSignalSemaphores = sig != VK_NULL_HANDLE ? &sig : nullptr;
    VkSubmitInfo si2{};
    si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si2.commandBufferCount = static_cast<uint32_t>(mine.size());
    si2.pCommandBuffers = mine.empty() ? nullptr : mine.data();
#ifdef _WIN32
    // The keyed mutex of a D3D11 texture is taken and given on by the submit itself: key 0 (the producer's side) in, key 1
    // (the consumer's side) out, as the ring's (display) and the encode capture's consumer protocol expect. The display slot
    // and the encode image can both be in one submit.
    VkWin32KeyedMutexAcquireReleaseInfoKHR km{};
    VkDeviceMemory kmMem[2] = {};
    uint64_t kmAcq[2] = {}, kmRel[2] = {};
    uint32_t kmTimeout[2] = {100, 100};  // ms; a free slot's key 0 is available
    uint32_t kmCount = 0;
    auto addKeyed = [&](const DisplayJob* j) {
      if (!j || !j->active || !hasImage || !j->ext.keyedMutex) return;
      kmMem[kmCount] = j->slot->memory;
      kmAcq[kmCount] = j->ext.acquireKey;
      kmRel[kmCount] = j->ext.releaseKey;
      ++kmCount;
    };
    addKeyed(job);
    addKeyed(enc);
    if (kmCount > 0) {
      km.sType = VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR;
      km.acquireCount = kmCount;
      km.pAcquireSyncs = kmMem;
      km.pAcquireKeys = kmAcq;
      km.pAcquireTimeouts = kmTimeout;
      km.releaseCount = kmCount;
      km.pReleaseSyncs = kmMem;
      km.pReleaseKeys = kmRel;
      si2.pNext = &km;
    }
#endif
    const bool coreWork = !cmds.empty() || !waits.empty() || sig != VK_NULL_HANDLE;
    const bool ownWork = !mine.empty();
    coreCmds.clear();
    imageSemaphores.clear();  // waited for once
    signalSemaphore = VK_NULL_HANDLE;
    if (!coreWork && !ownWork) return true;
    VkResult r;
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      r = vkQueueSubmit(queue, 1, &si, ownWork ? VK_NULL_HANDLE : fence);  // an empty batch is valid and keeps the order
      if (r == VK_SUCCESS && ownWork) r = vkQueueSubmit(queue, 1, &si2, fence);
    }
    if (r != VK_SUCCESS) {
      lost = r == VK_ERROR_DEVICE_LOST;
      qCWarning(lcVk) << "vkQueueSubmit failed" << static_cast<int>(r);
      // One batch was rejected: drain what did get in, so the next frame starts from an idle queue.
      if (!lost && vkDeviceWaitIdle) vkDeviceWaitIdle(device);
      return false;
    }
    r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
    if (r == VK_TIMEOUT) {  // the fence is still pending: it must not be reset; wait for the device instead
      qCWarning(lcVk) << "the Vulkan frame takes longer than 5 s; waiting for the device";
      const VkResult idle = vkDeviceWaitIdle ? vkDeviceWaitIdle(device) : VK_ERROR_DEVICE_LOST;
      r = idle == VK_SUCCESS ? vkWaitForFences(device, 1, &fence, VK_TRUE, 0) : idle;
    }
    if (r != VK_SUCCESS) {
      lost = r == VK_ERROR_DEVICE_LOST;
      if (lost) qCWarning(lcVk) << "the Vulkan device was lost while waiting for the frame";
      else qCWarning(lcVk) << "waiting for the Vulkan frame failed" << static_cast<int>(r);
      return false;
    }
    vkResetFences(device, 1, &fence);  // only a signaled fence
    return true;
  }
};

VkRenderContext::VkRenderContext() : d(std::make_unique<Impl>()) {}
VkRenderContext::~VkRenderContext() = default;
bool VkRenderContext::compiledIn() { return true; }

bool VkRenderContext::probe(QString* why) {
  if (g_forceUnavailable.load()) {
    if (why) *why = QStringLiteral("no Vulkan device");
    return false;
  }
  Impl p;
  QString err;
  const bool ok = p.loadLoader(&err) && p.createInstance(&err) && p.pickDevice(&err);
  g_highestApi.store(ok ? p.highestSeen : 0);
  if (!ok && why) *why = err;
  return ok;  // ~Impl destroys the temporary instance
}

void VkRenderContext::setNegotiationInterface(const void* iface) {
  d->hasNeg = false;
  d->neg = retro_hw_render_context_negotiation_interface_vulkan{};
  if (!iface) return;
  const auto* base = static_cast<const retro_hw_render_context_negotiation_interface*>(iface);
  if (base->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN) return;
  const auto* v = static_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(iface);
  d->neg.interface_type = v->interface_type;
  d->neg.interface_version = std::min<unsigned>(v->interface_version, RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION);
  d->neg.get_application_info = v->get_application_info;
  d->neg.create_device = v->create_device;
  d->neg.destroy_device = v->destroy_device;
  if (v->interface_version >= 2) {  // fields of v1 structs end after destroy_device: never read beyond that
    d->neg.create_instance = v->create_instance;
    d->neg.create_device2 = v->create_device2;
  }
  d->hasNeg = true;
}

void VkRenderContext::setRequiredApiVersion(unsigned versionMajor) {
  d->requiredApi = std::max<uint32_t>(normalizeApiVersion(versionMajor), VK_API_VERSION_1_0);
}

bool VkRenderContext::create(QString* error) {
  if (d->created) return true;
  if (g_forceCreateFail.load()) {
    if (error) *error = QStringLiteral("the Vulkan device could not be created");
    return false;
  }
  if (!d->createAll(error)) {
    d->destroy();
    return false;
  }
  return true;
}

void VkRenderContext::setExternalMemoryWanted(bool wanted) { d->extWanted = wanted; }

void VkRenderContext::setDisplayTarget(std::shared_ptr<GpuDisplayTarget> target) {
  Impl& s = *d;
  if (target == s.display) return;
  if (s.display) {
    s.freeExternal();
    s.display->release(true);  // before the images it handed out are gone
  }
  s.display = std::move(target);  // the old target may end here (last reference)
  s.luidChecked = s.importFormatChecked = false;
}
bool VkRenderContext::hasDisplayTarget() const { return d->display != nullptr; }
quint64 VkRenderContext::displayBlitCount() const { return d->displayBlits; }

void VkRenderContext::setEncodeTarget(std::shared_ptr<GpuEncodeTarget> target) {
  Impl& s = *d;
  if (target == s.encode) return;
  s.releaseEncode();  // before the images it handed out are gone: the imported image first, then detach
  s.encode = std::move(target);  // the old target may end here (last reference)
}
bool VkRenderContext::hasEncodeTarget() const { return d->encode != nullptr; }
quint64 VkRenderContext::encodeBlitCount() const { return d->encodeBlits; }

bool VkRenderContext::repeatEncode() {
  Impl& s = *d;
  if (!s.created || s.lost || !s.encode || !s.encAttached || !s.encHasFrame || !s.encode->wanted()) return false;
  if (!s.encode->repeat()) return false;
  return true;
}

void VkRenderContext::destroy() { d->destroy(); }
bool VkRenderContext::hasContext() const { return d->created; }
const void* VkRenderContext::renderInterface() const { return d->created ? &d->iface : nullptr; }
QString VkRenderContext::apiText() const {
  return d->created ? describeApi(std::min<uint32_t>(d->props.apiVersion, d->instanceApi)) : QString();
}
QString VkRenderContext::gpuText() const {
  return d->created ? describeGpu(QString::fromUtf8(d->props.deviceName), d->props.vendorID, d->props.driverVersion) : QString();
}
quint64 VkRenderContext::frameCount() const { return d->frames; }

void VkRenderContext::presentDuplicate() {
  if (!d->created || d->lost) return;
  if (!d->coreCmds.empty() || !d->imageSemaphores.empty() || d->signalSemaphore != VK_NULL_HANDLE) d->submit(false, 0, 0, 0);
}

QImage VkRenderContext::presentFrame(int w, int h, const QSize& maxSize, bool read, const QSize& displayLimit, bool* displayed,
                                     bool withTargets, EncodeOutcome* encodeOut) {
  Impl& s = *d;
  if (displayed) *displayed = false;
  if (encodeOut) *encodeOut = EncodeOutcome{};
  if (!s.created || s.lost || w <= 0 || h <= 0) return {};
  ++s.frames;
  Impl::DisplayJob job, encJob;
  if (withTargets && s.display && s.hasImage && s.imageFresh) {
    s.prepareDisplay(w, h, displayLimit, &job);
    if (!s.display) read = true;  // the target went away in this frame: it must not cost the game view a frame
  }
  if (withTargets && s.encode && s.hasImage && s.imageFresh) s.prepareEncode(w, h, &encJob);  // a dropped target never costs a frame
  BytesPerFrame fm;
  if (read && s.hasImage && s.imageFresh) {
    fm = formatMode(s.image.create_info.format);
    if (fm.mode == BytesPerFrame::None) {
      if (!s.warnedFormat) qCWarning(lcVk) << "unsupported frame format" << static_cast<int>(s.image.create_info.format);
      s.warnedFormat = true;
    }
  }
  bool copy = read && s.hasImage && s.imageFresh && fm.mode != BytesPerFrame::None;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * static_cast<VkDeviceSize>(fm.bpp ? fm.bpp : 4);
  if (copy && !s.ensureBuffer(bytes)) {
    qCWarning(lcVk) << "readback buffer could not be allocated";
    copy = false;  // the display copy (if any) and the core's work still run
  }
  const bool wasCopy = copy;
  const bool ok = s.submit(copy, w, h, bytes, job.active ? &job : nullptr, encJob.active ? &encJob : nullptr);
  s.imageFresh = false;
  if (job.active) {
    if (ok && s.display) {
      s.display->present(job.dw, job.dh);
      ++s.displayBlits;
      if (displayed) *displayed = true;
    } else if (s.display) {
      s.display->abort();
      s.dropDisplay(QStringLiteral("the copy into the display texture failed"));
    }
  }
  if (encJob.active) {
    if (ok && s.encode) {
      // The submit waited for its fence: the blit is complete and the keyed mutex is at the consumer's key.
      const auto t0 = std::chrono::steady_clock::now();
      const bool captured = s.encode->capture();
      const qint64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
      if (captured) {
        ++s.encodeBlits;
        s.encHasFrame = true;
        if (encodeOut) {
          encodeOut->captured = true;
          encodeOut->ns = ns;
        }
      } else {
        s.dropEncode(QStringLiteral("capture failed"));
      }
    } else if (s.encode) {
      s.dropEncode(QStringLiteral("the copy into the encode texture failed"));
    }
  }
  if (!ok || !wasCopy) return {};
  if (!s.coherent) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = s.memory;
    range.size = VK_WHOLE_SIZE;
    s.vkInvalidateMappedMemoryRanges(s.device, 1, &range);
  }
  QImage img(w, h, QImage::Format_RGB32);
  const auto* src = static_cast<const uchar*>(s.mapped);
  const int rowBytes = w * fm.bpp;
  for (int y = 0; y < h; ++y, src += rowBytes) {
    auto* dst = reinterpret_cast<quint32*>(img.scanLine(y));
    switch (fm.mode) {
      case BytesPerFrame::Bgra8: {
        const auto* p = reinterpret_cast<const quint32*>(src);
        for (int x = 0; x < w; ++x) dst[x] = p[x] | 0xFF000000u;
        break;
      }
      case BytesPerFrame::Rgba8:
        for (int x = 0; x < w; ++x) dst[x] = 0xFF000000u | (quint32(src[x * 4]) << 16) | (quint32(src[x * 4 + 1]) << 8) | src[x * 4 + 2];
        break;
      case BytesPerFrame::A2B10G10R10:
      case BytesPerFrame::A2R10G10B10: {
        const auto* p = reinterpret_cast<const quint32*>(src);
        for (int x = 0; x < w; ++x) {
          const quint32 lo = (p[x] & 0x3ff) >> 2, mid = ((p[x] >> 10) & 0x3ff) >> 2, hi = ((p[x] >> 20) & 0x3ff) >> 2;
          dst[x] = 0xFF000000u | (fm.mode == BytesPerFrame::A2B10G10R10 ? (lo << 16 | mid << 8 | hi) : (hi << 16 | mid << 8 | lo));
        }
        break;
      }
      case BytesPerFrame::Rgb565:
        for (int x = 0; x < w; ++x) {
          const quint32 p = src[x * 2] | (quint32(src[x * 2 + 1]) << 8);
          quint32 r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
          r = (r << 3) | (r >> 2);
          g = (g << 2) | (g >> 4);
          b = (b << 3) | (b >> 2);
          dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        break;
      case BytesPerFrame::None: break;
    }
  }
  const QSize target = HwRenderContext::scaledReadbackSize(w, h, maxSize);
  if (target.isValid() && target != QSize(w, h)) return img.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  return img;
}

#endif  // UZUME_HAVE_VULKAN

}  // namespace uzume::emu
