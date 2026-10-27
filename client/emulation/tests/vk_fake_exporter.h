// Test helper: a second Vulkan device that exports images as opaque file descriptors (VK_KHR_external_memory_fd), the way
// the Windows producers hand out D3D11 textures. The fake display/encode targets of vk_encode_test.cpp use it to receive
// frames from VkRenderContext on its own device (lavapipe is enough) and to read them back for comparison.
#pragma once
#define VK_NO_PROTOTYPES 1
#include <QImage>
#include <QLibrary>
#include <QString>

#include <cstring>
#include <vector>

#include <unistd.h>
#include <vulkan/vulkan.h>

namespace uzume::emu::testing {

class FakeVkExporter {
 public:
  ~FakeVkExporter() { destroyAll(); }

  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int fd = -1;  // owned here, the importer duplicates it
    VkDeviceSize memSize = 0;
    int w = 0, h = 0;
    bool bgra = false;
  };

  bool init(QString* why) {
    static QLibrary* lib = [] {
      auto* l = new QLibrary(QStringLiteral("libvulkan.so.1"));
      if (!l->load()) {
        delete l;
        return static_cast<QLibrary*>(nullptr);
      }
      return l;
    }();
    if (!lib) return fail_(why, "no Vulkan loader");
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(lib->resolve("vkGetInstanceProcAddr"));
    if (!gipa) return fail_(why, "no vkGetInstanceProcAddr");
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Uzume fake display";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (!createInstance || createInstance(&ici, nullptr, &instance) != VK_SUCCESS) return fail_(why, "vkCreateInstance failed");
#define LI(n) n = reinterpret_cast<PFN_##n>(gipa(instance, #n));
    LI(vkEnumeratePhysicalDevices) LI(vkGetPhysicalDeviceProperties) LI(vkGetPhysicalDeviceQueueFamilyProperties)
    LI(vkGetPhysicalDeviceMemoryProperties) LI(vkCreateDevice) LI(vkGetDeviceProcAddr) LI(vkDestroyInstance)
    LI(vkEnumerateDeviceExtensionProperties)
#undef LI
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(instance, &n, devs.data());
    int bestRank = 99;
    for (VkPhysicalDevice d : devs) {  // same preference as VkRenderContext::pickDevice
      VkPhysicalDeviceProperties p{};
      vkGetPhysicalDeviceProperties(d, &p);
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
      }
    }
    if (!gpu) return fail_(why, "no Vulkan device");
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> avail(en);
    if (en) vkEnumerateDeviceExtensionProperties(gpu, nullptr, &en, avail.data());
    auto has = [&](const char* name) {
      for (const auto& e : avail)
        if (std::strcmp(e.extensionName, name) == 0) return true;
      return false;
    };
    if (!has(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) || !has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
      return fail_(why, "the Vulkan device has no VK_KHR_external_memory_fd");
    const char* exts[] = {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qf.data());
    int pick = -1;
    for (uint32_t i = 0; i < qn && pick < 0; ++i)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) pick = static_cast<int>(i);
    if (pick < 0) return fail_(why, "no graphics queue");
    family = static_cast<uint32_t>(pick);
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 2;
    di.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(gpu, &di, nullptr, &device) != VK_SUCCESS) return fail_(why, "vkCreateDevice failed");
#define LD(n) n = reinterpret_cast<PFN_##n>(vkGetDeviceProcAddr(device, #n));
    LD(vkDestroyDevice) LD(vkGetDeviceQueue) LD(vkCreateImage) LD(vkDestroyImage) LD(vkGetImageMemoryRequirements)
    LD(vkAllocateMemory) LD(vkFreeMemory) LD(vkBindImageMemory) LD(vkGetMemoryFdKHR) LD(vkCreateBuffer) LD(vkDestroyBuffer)
    LD(vkGetBufferMemoryRequirements) LD(vkBindBufferMemory) LD(vkMapMemory) LD(vkCreateCommandPool) LD(vkDestroyCommandPool)
    LD(vkAllocateCommandBuffers) LD(vkBeginCommandBuffer) LD(vkEndCommandBuffer) LD(vkCmdPipelineBarrier)
    LD(vkCmdCopyImageToBuffer) LD(vkQueueSubmit) LD(vkQueueWaitIdle) LD(vkDeviceWaitIdle) LD(vkResetCommandBuffer)
#undef LD
    if (!vkGetMemoryFdKHR || !vkCreateImage || !vkQueueSubmit) return fail_(why, "missing device functions");
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = family;
    vkCreateCommandPool(device, &pi, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &ai, &cmd);
    return true;
  }

  // One exportable w x h image (optimal tiling, device local, transfer src/dst).
  bool create(Image* img, int w, int h, bool bgra) {
    VkExternalMemoryImageCreateInfo emi{};
    emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &emi;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ci, nullptr, &img->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, img->image, &req);
    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = img->image;
    VkExportMemoryAllocateInfo exp{};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exp.pNext = &ded;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &exp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &mai, nullptr, &img->memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(device, img->image, img->memory, 0) != VK_SUCCESS) return false;
    VkMemoryGetFdInfoKHR gi{};
    gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gi.memory = img->memory;
    gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (vkGetMemoryFdKHR(device, &gi, &img->fd) != VK_SUCCESS) return false;
    img->memSize = mai.allocationSize;
    img->w = w;
    img->h = h;
    img->bgra = bgra;
    return true;
  }

  void destroy(Image* img) {
    if (!device) return;
    vkDeviceWaitIdle(device);
    if (img->image) vkDestroyImage(device, img->image, nullptr);
    if (img->memory) vkFreeMemory(device, img->memory, nullptr);
    if (img->fd >= 0) ::close(img->fd);
    *img = Image{};
  }

  // Copies the top-left w x h of the image (as the producer left it: external queue family, GENERAL) into a host buffer and
  // returns it as RGB32 (alpha forced opaque).
  QImage readBack(const Image& img, int w, int h) {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bi, nullptr, &buf);
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &mai, nullptr, &mem);
    vkBindBufferMemory(device, buf, mem, 0);
    void* mapped = nullptr;
    vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &mapped);

    VkCommandBufferBeginInfo bgi{};
    bgi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &bgi);
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b.dstQueueFamilyIndex = family;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    VkImageMemoryBarrier back = b;
    back.srcQueueFamilyIndex = family;
    back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    // Bytes R,G,B,A (RGBA8888) or B,G,R,A (little-endian ARGB32 words).
    QImage raw(w, h, img.bgra ? QImage::Format_ARGB32 : QImage::Format_RGBA8888);
    std::memcpy(raw.bits(), mapped, static_cast<size_t>(bytes));
    const QImage out = raw.convertToFormat(QImage::Format_RGB32);
    vkDestroyBuffer(device, buf, nullptr);
    vkFreeMemory(device, mem, nullptr);
    return out;
  }

  void destroyAll() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (pool) vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
      device = VK_NULL_HANDLE;
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
  }

  PFN_vkGetInstanceProcAddr gipa = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
#define FN(n) PFN_##n n = nullptr;
  FN(vkEnumeratePhysicalDevices) FN(vkGetPhysicalDeviceProperties) FN(vkGetPhysicalDeviceQueueFamilyProperties)
  FN(vkGetPhysicalDeviceMemoryProperties) FN(vkCreateDevice) FN(vkGetDeviceProcAddr) FN(vkDestroyInstance)
  FN(vkEnumerateDeviceExtensionProperties) FN(vkDestroyDevice) FN(vkGetDeviceQueue) FN(vkCreateImage) FN(vkDestroyImage)
  FN(vkGetImageMemoryRequirements) FN(vkAllocateMemory) FN(vkFreeMemory) FN(vkBindImageMemory) FN(vkGetMemoryFdKHR)
  FN(vkCreateBuffer) FN(vkDestroyBuffer) FN(vkGetBufferMemoryRequirements) FN(vkBindBufferMemory) FN(vkMapMemory)
  FN(vkCreateCommandPool) FN(vkDestroyCommandPool) FN(vkAllocateCommandBuffers) FN(vkBeginCommandBuffer)
  FN(vkEndCommandBuffer) FN(vkCmdPipelineBarrier) FN(vkCmdCopyImageToBuffer) FN(vkQueueSubmit) FN(vkQueueWaitIdle)
  FN(vkDeviceWaitIdle) FN(vkResetCommandBuffer)
#undef FN

  bool fail_(QString* why, const char* m) {
    if (why) *why = QString::fromLatin1(m);
    return false;
  }

  uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if (bits & (1u << i)) return i;
    return 0;
  }

};

}  // namespace uzume::emu::testing
