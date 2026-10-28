// Zero-copy display for Vulkan games (ADR 0034 D8) on Linux: the "vulkan" render mode of the Uzume test core through
// LibretroBackend and VkRenderContext, with a fake emu::GpuDisplayTarget that owns exportable RGBA8 images (opaque fd,
// VK_KHR_external_memory_fd) on its own Vulkan device. The frame must land in the target's image without readback; the
// fake reads its image back after present() and compares content, orientation and size with the test core's known frame.
// Needs a Vulkan device with external-memory fd support (lavapipe is enough) and a QGuiApplication; without one the test
// returns 77 (ctest: SKIP), except with UZUME_REQUIRE_VULKAN=1 (Linux CI).
#include <QGuiApplication>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "emulation_runner.h"
#include "gpu_display_target.h"
#include "libretro_backend.h"
#include "test_core_layout.h"
#include "vk_render.h"

#if UZUME_HAVE_VULKAN && defined(__unix__) && !defined(__APPLE__)
#define UZUME_VK_DISPLAY_TEST 1
#define VK_NO_PROTOTYPES 1
#include <unistd.h>
#include <vulkan/vulkan.h>
#else
#define UZUME_VK_DISPLAY_TEST 0
#endif

using namespace uzume::emu;
namespace tc = uzume::testcore;

#if UZUME_VK_DISPLAY_TEST

namespace {
constexpr QRgb kRed = 0xFFFF0000, kGreen = 0xFF00FF00, kBlue = 0xFF0000FF, kWhite = 0xFFFFFFFF, kMagenta = 0xFFFF00FF,
               kCyan = 0xFF00FFFF;

struct EnvGuard {
  QByteArray name, old;
  bool had;
  EnvGuard(const char* n, const char* v) : name(n), old(qgetenv(n)), had(qEnvironmentVariableIsSet(n)) {
    if (v && *v) qputenv(n, v);
    else qunsetenv(n);  // the test core looks at the presence of some of these
  }
  ~EnvGuard() {
    if (had) qputenv(name.constData(), old);
    else qunsetenv(name.constData());
  }
};

// The fake display target: its own Vulkan instance and device, 3 exportable images per generation (grow-only, sizes
// rounded up to 64 like the real ring), handed out round-robin; present() copies the image into a host buffer.
class FakeExternalTarget final : public GpuDisplayTarget {
 public:
  ~FakeExternalTarget() override { destroyAll(); }

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

  // ---- knobs and results (test thread reads after runFrame(), which runs inline)
  bool wantedFlag = true;
  bool luidValid = false;
  quint8 luid[8] = {};
  bool failNextAcquire = false;      // acquireExternal answers Failed once
  bool unavailableNextAcquire = false;
  bool badHandleNext = false;        // hands out an fd that cannot be duplicated
  int presented = 0, aborts = 0, releases = 0, acquires = 0;
  int lastSlot = -1;
  quint64 generation = 0;            // current generation id
  QString failReason, unavailableReason;
  QImage lastImage;                  // RGB32, the last presented w x h region as read back from the target's image
  QSize lastSize;
  QSize lastTex;

  // ---- GpuDisplayTarget
  bool wanted() const override { return wantedFlag && failReason.isEmpty() && unavailableReason.isEmpty(); }
  Acquire acquire(int, int, unsigned*, QSize*) override { return Acquire::Unavailable; }
  Acquire acquireExternal(int w, int h, ExternalImage* out) override {
    ++acquires;
    if (failNextAcquire) {
      failNextAcquire = false;
      return Acquire::Failed;
    }
    if (unavailableNextAcquire) {
      unavailableNextAcquire = false;
      return Acquire::Unavailable;
    }
    const int rw = (w + 63) / 64 * 64, rh = (h + 63) / 64 * 64;
    if (rw > texW || rh > texH) {  // grow-only, new generation
      texW = std::max(texW, rw);
      texH = std::max(texH, rh);
      if (!makeGeneration()) return Acquire::Failed;
    }
    writing = next;
    next = (next + 1) % 3;
    const Slot& s = imgs[writing];
    out->handleType = ExternalImage::HandleType::OpaqueFd;
    out->handle = badHandleNext ? -1 : s.fd;
    badHandleNext = false;
    out->texSize = QSize(texW, texH);
    out->memorySize = s.memSize;
    out->generation = generation;
    out->slot = writing;
    out->keyedMutex = false;
    return Acquire::Ok;
  }
  AdapterLuid adapterLuid() const override {
    AdapterLuid l;
    l.valid = luidValid;
    std::memcpy(l.bytes, luid, 8);
    return l;
  }
  void markUnavailable(const QString& reason) override { unavailableReason = reason; }
  void present(int w, int h) override {
    ++presented;
    lastSlot = writing;
    lastSize = QSize(w, h);
    lastTex = QSize(texW, texH);
    readBack(writing, w, h);
    writing = -1;
  }
  void abort() override {
    ++aborts;
    writing = -1;
  }
  void release(bool) override {
    ++releases;
    destroyImages();
  }
  void fail(const QString& reason) override {
    if (failReason.isEmpty()) failReason = reason;
  }

 private:
  struct Slot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int fd = -1;
    VkDeviceSize memSize = 0;
  };
  Slot imgs[3];
  int texW = 0, texH = 0, next = 0, writing = -1;
  quint64 nextGen = 1;

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

  void destroyImages() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    for (Slot& s : imgs) {
      if (s.image) vkDestroyImage(device, s.image, nullptr);
      if (s.memory) vkFreeMemory(device, s.memory, nullptr);
      if (s.fd >= 0) ::close(s.fd);
      s = Slot{};
    }
  }

  bool makeGeneration() {
    destroyImages();
    for (Slot& s : imgs) {
      VkExternalMemoryImageCreateInfo emi{};
      emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
      emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      VkImageCreateInfo ci{};
      ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      ci.pNext = &emi;
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.format = VK_FORMAT_R8G8B8A8_UNORM;
      ci.extent = {static_cast<uint32_t>(texW), static_cast<uint32_t>(texH), 1};
      ci.mipLevels = 1;
      ci.arrayLayers = 1;
      ci.samples = VK_SAMPLE_COUNT_1_BIT;
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (vkCreateImage(device, &ci, nullptr, &s.image) != VK_SUCCESS) return false;
      VkMemoryRequirements req{};
      vkGetImageMemoryRequirements(device, s.image, &req);
      VkMemoryDedicatedAllocateInfo ded{};
      ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      ded.image = s.image;
      VkExportMemoryAllocateInfo exp{};
      exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
      exp.pNext = &ded;
      exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      VkMemoryAllocateInfo mai{};
      mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      mai.pNext = &exp;
      mai.allocationSize = req.size;
      mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (vkAllocateMemory(device, &mai, nullptr, &s.memory) != VK_SUCCESS) return false;
      if (vkBindImageMemory(device, s.image, s.memory, 0) != VK_SUCCESS) return false;
      VkMemoryGetFdInfoKHR gi{};
      gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
      gi.memory = s.memory;
      gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      if (vkGetMemoryFdKHR(device, &gi, &s.fd) != VK_SUCCESS) return false;
      s.memSize = mai.allocationSize;
    }
    generation = nextGen++;
    return true;
  }

  // Copies the top-left w x h of slot `i` into a host buffer (acquire from the external family, as the producer released it).
  void readBack(int i, int w, int h) {
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
    b.image = imgs[i].image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    vkCmdCopyImageToBuffer(cmd, imgs[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
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
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), mapped, static_cast<size_t>(bytes));
    lastImage = img.convertToFormat(QImage::Format_RGB32);
    vkDestroyBuffer(device, buf, nullptr);
    vkFreeMemory(device, mem, nullptr);
  }

  void destroyAll() {
    if (device) {
      destroyImages();
      if (pool) vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
  }
};

}  // namespace

class VkDisplayTest : public QObject {
  Q_OBJECT
  QTemporaryDir m_dirs;
  bool m_ready = false;  // the fake can run here

  // Starts the test core in Vulkan mode with `target` set before the game loads (GameSession order).
  bool start(LibretroBackend& be, const std::shared_ptr<FakeExternalTarget>& target, QString* err) {
    be.prepareForStart();
    be.setPreferredRenderer(QStringLiteral("vulkan"));
    be.setSystemDirectory(m_dirs.filePath(QStringLiteral("system")));
    be.setSaveDirectory(m_dirs.filePath(QStringLiteral("save")));
    if (!be.loadCore(QStringLiteral(FB_TEST_CORE_PATH), err)) return false;
    be.setCoreOption(QStringLiteral("test_render_mode"), QStringLiteral("vulkan"));
    if (target) be.setGpuDisplayTarget(target);
    return be.loadGame(QStringLiteral(FB_TEST_ROM_PATH), err);
  }

  std::shared_ptr<FakeExternalTarget> makeTarget() {
    auto t = std::make_shared<FakeExternalTarget>();
    QString why;
    if (!t->init(&why)) {
      if (qEnvironmentVariable("UZUME_REQUIRE_VULKAN") == QLatin1String("1")) QTest::qFail(qPrintable(why), __FILE__, __LINE__);
      else QTest::qSkip(qPrintable(why), __FILE__, __LINE__);
      return nullptr;
    }
    return t;
  }

  // The test core's pattern at scale 1 (or scaled by `k` = shown size / source size), see vk_render_test.cpp.
  void checkPattern(const QImage& f, int frames, bool marker = true) {
    QCOMPARE(f.format(), QImage::Format_RGB32);
    const int W = f.width(), H = f.height();
    QCOMPARE(W * tc::kHeight, H * tc::kWidth);  // aspect kept
    const double k = double(W) / tc::kWidth;
    auto at = [&](double fx, double fy) { return f.pixel(int(fx * W), int(fy * H)); };
    QCOMPARE(at(0.25, 48.0 / 384), kRed);
    QCOMPARE(at(0.75, 48.0 / 384), kGreen);
    QCOMPARE(at(0.25, 144.0 / 384), kBlue);
    QCOMPARE(at(0.75, 144.0 / 384), kWhite);
    QCOMPARE(at(0.5, 288.0 / 384), kMagenta);
    if (marker) {
      QCOMPARE(k, 1.0);
      const int mx = tc::vkMarkerX(static_cast<uint64_t>(frames), 1);
      QCOMPARE(f.pixel(mx + 1, 1), kCyan);
      QCOMPARE(f.pixel(mx + 8 - 1, 8 - 1), kCyan);
      QCOMPARE(f.pixel(mx + 8, 1) == kCyan, false);
    }
  }

 private slots:
  void initTestCase() {
    QVERIFY(QDir().mkpath(m_dirs.filePath(QStringLiteral("system"))));
    QVERIFY(QDir().mkpath(m_dirs.filePath(QStringLiteral("save"))));
  }
  void cleanup() {
    VkRenderContext::setExternalMemoryMissingForTest(false);
    VkRenderContext::setDeviceLuidForTest({});
  }

  // The frame lands in the external image, orientation and content as the test core drew it; no readback at all.
  void frameLandsWithoutReadback_data() {
    QTest::addColumn<QByteArray>("neg");
    QTest::addColumn<QByteArray>("cmdbufs");
    QTest::addColumn<QByteArray>("layout");
    QTest::addColumn<QByteArray>("format");
    QTest::newRow("v1 own submit") << QByteArray("v1") << QByteArray("") << QByteArray("") << QByteArray("");
    QTest::newRow("v2 wrapper") << QByteArray("v2") << QByteArray("") << QByteArray("") << QByteArray("");
    QTest::newRow("no negotiation") << QByteArray("none") << QByteArray("") << QByteArray("") << QByteArray("");
    QTest::newRow("command buffers") << QByteArray("v2") << QByteArray("1") << QByteArray("") << QByteArray("");
    QTest::newRow("general layout") << QByteArray("v1") << QByteArray("") << QByteArray("general") << QByteArray("");
    QTest::newRow("rgba source") << QByteArray("v1") << QByteArray("") << QByteArray("") << QByteArray("rgba");
  }
  void frameLandsWithoutReadback() {
    QFETCH(QByteArray, neg);
    QFETCH(QByteArray, cmdbufs);
    QFETCH(QByteArray, layout);
    QFETCH(QByteArray, format);
    EnvGuard g1("FB_TEST_CORE_VK_NEG", neg.constData());
    EnvGuard g2("FB_TEST_CORE_VK_CMDBUFS", cmdbufs.constData());
    EnvGuard g3("FB_TEST_CORE_VK_LAYOUT", layout.constData());
    EnvGuard g4("FB_TEST_CORE_VK_FORMAT", format.constData());
    auto t = makeTarget();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);  // the display is the only consumer
    const int n = 12;
    for (int i = 0; i < n; ++i) QVERIFY(be.runFrame());
    QVERIFY(be.renderInfo().vulkan);
    QCOMPARE(t->presented, n);
    QVERIFY(be.lastDisplayed());
    QCOMPARE(t->aborts, 0);
    QVERIFY2(t->failReason.isEmpty() && t->unavailableReason.isEmpty(), qPrintable(t->failReason + t->unavailableReason));
    QCOMPARE(be.hwReadbackCount(), quint64(0));  // no readback while the display shows every frame
    QVERIFY(be.videoFrame().isNull());
    QCOMPARE(t->lastSize, QSize(tc::kWidth, tc::kHeight));
    QCOMPARE(t->lastTex, QSize(256, 384));  // rounded up to 64
    QCOMPARE(be.sourceFrameSize(), QSize(tc::kWidth, tc::kHeight));
    checkPattern(t->lastImage, n);
    be.runFrame();
    checkPattern(t->lastImage, n + 1);  // successive frames differ (the marker moves)
    be.unloadCore();
    QVERIFY(t->releases >= 1);  // the context hands the target back when it goes away
  }

  void readbackAndDisplayShareOneSubmit() {
    auto t = makeTarget();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));  // readback stays on (a Session needs the pixels)
    for (int i = 0; i < 6; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->presented, 6);
    QVERIFY(be.hwReadbackCount() >= 6);
    checkPattern(be.videoFrame(), 6);
    checkPattern(t->lastImage, 6);
    QCOMPARE(be.videoFrame(), t->lastImage);
    be.unloadCore();
  }

  void frameIsScaledToTheDisplayLimit() {
    auto t = makeTarget();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    be.setDisplayLimit(QSize(128, 192));
    for (int i = 0; i < 4; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->presented, 4);
    QCOMPARE(t->lastSize, QSize(128, 192));  // aspect kept, never upscaled
    QCOMPARE(t->lastTex, QSize(128, 192));
    checkPattern(t->lastImage, 4, false);
    // A limit larger than the frame never upscales.
    be.setDisplayLimit(QSize(1024, 1024));
    QVERIFY(be.runFrame());
    QCOMPARE(t->lastSize, QSize(tc::kWidth, tc::kHeight));
    be.unloadCore();
  }

  // Textures only grow: a bigger frame is a new generation (the importer drops and re-imports), content still right.
  void generationGrowthReimports() {
    auto t = makeTarget();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    be.setDisplayLimit(QSize(128, 192));
    for (int i = 0; i < 4; ++i) QVERIFY(be.runFrame());
    const quint64 first = t->generation;
    QCOMPARE(first, quint64(1));
    be.setDisplayLimit(QSize());
    for (int i = 0; i < 4; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->generation, quint64(2));
    QCOMPARE(t->lastTex, QSize(256, 384));
    QCOMPARE(t->presented, 8);
    checkPattern(t->lastImage, 8);
    // Shrinking the limit again keeps the big textures (grow-only) and the frame is the smaller region of them.
    be.setDisplayLimit(QSize(128, 192));
    QVERIFY(be.runFrame());
    QCOMPARE(t->generation, quint64(2));
    QCOMPARE(t->lastSize, QSize(128, 192));
    be.unloadCore();
  }

  void sameAdapterIsAccepted() {
    auto t = makeTarget();
    if (!t) return;
    t->luidValid = true;
    for (int i = 0; i < 8; ++i) t->luid[i] = static_cast<quint8>(0x10 + i);
    VkRenderContext::setDeviceLuidForTest(QByteArray(reinterpret_cast<const char*>(t->luid), 8));
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->presented, 3);
    be.unloadCore();
  }

  void otherAdapterIsUnavailableAndReadbackContinues() {
    auto t = makeTarget();
    if (!t) return;
    t->luidValid = true;
    for (int i = 0; i < 8; ++i) t->luid[i] = static_cast<quint8>(0x10 + i);
    QByteArray other(8, '\x7f');
    VkRenderContext::setDeviceLuidForTest(other);
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);  // the caller thinks the display will show the frames
    QVERIFY(be.runFrame());
    QCOMPARE(t->unavailableReason, QStringLiteral("Vulkan device is on another adapter"));
    QCOMPARE(t->presented, 0);
    QCOMPARE(t->acquires, 0);  // known before the target hands out anything
    QVERIFY(t->releases >= 1);
    QVERIFY(!be.videoFrame().isNull());  // the frame that found out was read back anyway
    be.setReadbackEnabled(true);         // what GameSession does on the state change
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    checkPattern(be.videoFrame(), 4);
    QVERIFY(be.renderInfo().vulkan);
    be.unloadCore();
  }

  void missingExtensionIsUnavailableNotAFailedGame() {
    VkRenderContext::setExternalMemoryMissingForTest(true);
    auto t = makeTarget();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QVERIFY2(t->unavailableReason.contains(QLatin1String("VK_KHR_external_memory")), qPrintable(t->unavailableReason));
    QCOMPARE(t->presented, 0);
    QVERIFY(t->failReason.isEmpty());
    QVERIFY(be.renderInfo().vulkan);
    checkPattern(be.videoFrame(), 3);
    be.unloadCore();
  }

  void targetThatArrivesAfterCreateIsServed() {
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, nullptr, &err), qPrintable(err));  // no target at device creation: the interop extensions are enabled anyway
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    auto t = makeTarget();
    if (!t) return;
    be.setGpuDisplayTarget(t);
    be.setReadbackEnabled(false);
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    QVERIFY2(t->unavailableReason.isEmpty(), qPrintable(t->unavailableReason));
    QVERIFY2(t->presented > 0, "a late target gets the frames (a Session started after the game)");
    QVERIFY(!be.videoFrame().isNull());
    be.unloadCore();
  }

  // The target gives up in the middle of the game (consumer side): no more copies; readback comes back with the plan.
  void targetFailureMidGameKeepsFramesComing() {
    auto t = makeTarget();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    for (int i = 0; i < 4; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->presented, 4);
    QVERIFY(be.videoFrame().isNull());
    t->fail(QStringLiteral("consumer gave up"));  // wanted() is false from now on
    be.setReadbackEnabled(true);                  // GameSession's re-plan after the state callback
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->presented, 4);
    checkPattern(be.videoFrame(), 7);
    be.unloadCore();
  }

  // A Vulkan-side failure (here: the target's acquire fails) drops the target and the very frame is still read back.
  void acquireFailureDropsTheTargetAndReadsBack() {
    auto t = makeTarget();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QVERIFY(be.videoFrame().isNull());
    t->failNextAcquire = true;
    QVERIFY(be.runFrame());
    QVERIFY(!t->failReason.isEmpty());
    QVERIFY(t->releases >= 1);
    QVERIFY(!be.videoFrame().isNull());
    checkPattern(be.videoFrame(), 4);
    const int acquires = t->acquires;
    QVERIFY(be.runFrame());
    QCOMPARE(t->acquires, acquires);  // forgotten: never asked again in this game
    be.unloadCore();
  }

  void importFailureDropsTheTarget() {
    auto t = makeTarget();
    if (!t) return;
    t->badHandleNext = true;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    QVERIFY(be.runFrame());
    QVERIFY2(!t->failReason.isEmpty(), "an import that fails must fail the target");
    QCOMPARE(t->aborts, 1);  // the locked slot was given back
    QCOMPARE(t->presented, 0);
    QVERIFY(!be.videoFrame().isNull());
    QVERIFY(be.runFrame());
    QVERIFY(be.renderInfo().vulkan);
    be.unloadCore();
  }

  void targetThatIsNotApplicableIsReleasedQuietly() {
    auto t = makeTarget();
    if (!t) return;
    t->unavailableNextAcquire = true;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    QVERIFY(be.runFrame());
    QVERIFY(t->releases >= 1);
    QVERIFY(t->failReason.isEmpty());
    QVERIFY(!be.videoFrame().isNull());
    be.unloadCore();
  }

  void replacingTheTargetReleasesTheOldOne() {
    auto a = makeTarget();
    if (!a) return;
    auto b = makeTarget();
    if (!b) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, a, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    QCOMPARE(a->presented, 2);
    be.setGpuDisplayTarget(b);
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    QVERIFY(a->releases >= 1);
    QCOMPARE(a->presented, 2);
    QCOMPARE(b->presented, 2);
    checkPattern(b->lastImage, 4);
    be.setGpuDisplayTarget(nullptr);
    QVERIFY(be.runFrame());
    QVERIFY(b->releases >= 1);
    be.unloadCore();
  }

  void restartInTheSameProcessImportsAgain() {
    for (int round = 0; round < 3; ++round) {
      auto t = makeTarget();
      if (!t) return;
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, t, &err), qPrintable(err));
      be.setReadbackEnabled(false);
      for (int i = 0; i < 4; ++i) QVERIFY(be.runFrame());
      QCOMPARE(t->presented, 4);
      checkPattern(t->lastImage, 4);
      be.unloadCore();
    }
  }
};

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  QString why;
  if (!VkRenderContext::compiledIn() || !VkRenderContext::probe(&why)) {
    const bool required = qEnvironmentVariable("UZUME_REQUIRE_VULKAN") == QLatin1String("1");
    qWarning("No Vulkan device available (%s)%s", qPrintable(why.isEmpty() ? QStringLiteral("built without Vulkan headers") : why),
             required ? " - UZUME_REQUIRE_VULKAN=1, failing" : " - test skipped");
    return required ? 1 : 77;
  }
  VkDisplayTest t;
  return QTest::qExec(&t, argc, argv);
}

#include "vk_display_test.moc"

#else  // no Vulkan headers or not a Unix: the fake target needs opaque fds

int main() {
  qWarning("vk_display_test: needs the Vulkan headers and Linux - skipped");
  return 77;
}

#endif
