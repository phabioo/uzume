#include "vk_render.h"

#include <QLibrary>
#include <QLoggingCategory>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "hw_render.h"

#if UZUME_HAVE_VULKAN
#define VK_NO_PROTOTYPES 1
#include <libretro.h>
#include <vulkan/vulkan.h>

#include "libretro_vulkan.h"
#endif

Q_LOGGING_CATEGORY(lcVk, "uzume.vulkan")

namespace uzume::emu {

namespace {
std::atomic<bool> g_forceUnavailable{false};
}

void VkRenderContext::setUnavailableForTest(bool unavailable) { g_forceUnavailable.store(unavailable); }

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
bool VkRenderContext::create(QString* error) {
  if (error) *error = QStringLiteral("built without the Vulkan headers");
  return false;
}
void VkRenderContext::destroy() {}
bool VkRenderContext::hasContext() const { return false; }
const void* VkRenderContext::renderInterface() const { return nullptr; }
QString VkRenderContext::apiText() const { return {}; }
QString VkRenderContext::gpuText() const { return {}; }
QImage VkRenderContext::presentFrame(int, int, const QSize&, bool) { return {}; }
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

  // ---- core inputs
  retro_hw_render_context_negotiation_interface_vulkan neg{};
  bool hasNeg = false;
  uint32_t requiredApi = VK_API_VERSION_1_0;

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
    if (self->vkCreateDevice(g, ci, nullptr, &dev) != VK_SUCCESS) return VK_NULL_HANDLE;
    self->wrapperDevice = dev;
    return dev;
  }

  bool createInstance(QString* error) {
    uint32_t loaderApi = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderApi);
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
      if (instance == VK_NULL_HANDLE) qCInfo(lcVk) << "core create_instance failed; creating the instance ourselves";
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
        ok = neg.create_device(&ctx, instance, gpu, VK_NULL_HANDLE, gipa, nullptr, 0, nullptr, 0, &features);
      }
      if (ok && ctx.device != VK_NULL_HANDLE && ctx.queue != VK_NULL_HANDLE) {
        coreDeviceOk = true;
        device = ctx.device;
        queue = ctx.queue;
        family = ctx.queue_family_index;
        if (ctx.gpu != VK_NULL_HANDLE && ctx.gpu != gpu) {
          gpu = ctx.gpu;
          vkGetPhysicalDeviceProperties(gpu, &props);
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
    if (!loadLoader(error) || !createInstance(error) || !pickDevice(error) || !createDevice(error)) return false;
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
  bool submit(bool copy, int w, int h, VkDeviceSize copyBytes) {
    const bool useCopy = copy && hasImage;
    const bool coreCmdMode = !coreCmds.empty();
    std::vector<VkCommandBuffer> cmds = coreCmds;
    if (useCopy) {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkResetCommandBuffer(cmd, 0);
      if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return false;
      const VkImage img = image.create_info.image;
      const VkImageLayout layout = image.image_layout;
      const bool general = layout == VK_IMAGE_LAYOUT_GENERAL;
      const bool transfer = srcFamily != VK_QUEUE_FAMILY_IGNORED && srcFamily != family;
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
      vkCmdCopyImageToBuffer(cmd, img, b.newLayout, buffer, 1, &region);
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
                           nullptr, 1, &hb, 1, &back);
      (void)copyBytes;
      if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
      cmds.push_back(cmd);
    }
    // Semaphores of set_image are ignored when the core uses set_command_buffers (libretro_vulkan.h).
    std::vector<VkSemaphore> waits;
    if (!coreCmdMode) waits = imageSemaphores;
    std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
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
    const bool nothing = cmds.empty() && waits.empty() && sig == VK_NULL_HANDLE;
    coreCmds.clear();
    imageSemaphores.clear();  // waited for once
    signalSemaphore = VK_NULL_HANDLE;
    if (nothing) return true;
    VkResult r;
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      r = vkQueueSubmit(queue, 1, &si, fence);
    }
    if (r != VK_SUCCESS) {
      lost = r == VK_ERROR_DEVICE_LOST;
      qCWarning(lcVk) << "vkQueueSubmit failed" << static_cast<int>(r);
      return false;
    }
    r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
    vkResetFences(device, 1, &fence);
    if (r != VK_SUCCESS) {
      lost = true;
      qCWarning(lcVk) << "waiting for the Vulkan frame failed" << static_cast<int>(r);
      return false;
    }
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
  d->requiredApi = versionMajor >= (1u << 22) ? versionMajor : VK_MAKE_API_VERSION(0, std::max(1u, versionMajor), 0, 0);
  if (d->requiredApi < VK_API_VERSION_1_0) d->requiredApi = VK_API_VERSION_1_0;
}

bool VkRenderContext::create(QString* error) {
  if (d->created) return true;
  if (!d->createAll(error)) {
    d->destroy();
    return false;
  }
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

QImage VkRenderContext::presentFrame(int w, int h, const QSize& maxSize, bool read) {
  Impl& s = *d;
  if (!s.created || s.lost || w <= 0 || h <= 0) return {};
  ++s.frames;
  BytesPerFrame fm;
  if (read && s.hasImage && s.imageFresh) {
    fm = formatMode(s.image.create_info.format);
    if (fm.mode == BytesPerFrame::None) {
      if (!s.warnedFormat) qCWarning(lcVk) << "unsupported frame format" << static_cast<int>(s.image.create_info.format);
      s.warnedFormat = true;
    }
  }
  const bool copy = read && s.hasImage && s.imageFresh && fm.mode != BytesPerFrame::None;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * static_cast<VkDeviceSize>(fm.bpp ? fm.bpp : 4);
  if (copy && !s.ensureBuffer(bytes)) {
    qCWarning(lcVk) << "readback buffer could not be allocated";
    s.submit(false, 0, 0, 0);
    return {};
  }
  const bool ok = s.submit(copy, w, h, bytes);
  s.imageFresh = false;
  if (!ok || !copy) return {};
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
