#pragma once
// VkRenderContext: headless Vulkan device for libretro hardware rendering (ADR 0034, next to the OpenGL
// HwRenderContext of ADR 0013).
//
// The Vulkan loader is never linked: it is loaded at runtime (QLibrary) and every function comes from
// vkGetInstanceProcAddr. No window-system extensions, no surface. The core renders into its own image; per frame the
// image is copied into a host-visible buffer and converted to an RGB32 QImage (readback only; no zero-copy display and
// no GPU-direct encoding for Vulkan). Without the Vulkan headers at build time (UZUME_HAVE_VULKAN undefined) every
// call reports "unavailable".
//
// Thread rules: probe()/create()/destroy()/frame calls all run on the emulation thread.

#include <QImage>
#include <QSize>
#include <QString>

#include <memory>

namespace uzume::emu {

class VkRenderContext {
 public:
  VkRenderContext();
  ~VkRenderContext();
  Q_DISABLE_COPY(VkRenderContext)

  // true: the app was built with the Vulkan headers.
  static bool compiledIn();
  // Tests: makes probe() and create() fail as if the system had no Vulkan device.
  static void setUnavailableForTest(bool unavailable);
  // A Vulkan loader and at least one physical device exist (creates and destroys a temporary instance).
  static bool probe(QString* why = nullptr);

  // Pure helpers: packed VkPhysicalDeviceProperties::apiVersion -> "Vulkan 1.3"; device name + vendor id + packed driver
  // version -> "<name> · Driver <version>" (NVIDIA uses its own bit layout).
  static QString describeApi(unsigned apiVersion);
  static QString describeGpu(const QString& deviceName, unsigned vendorId, unsigned driverVersion);

  // The core's retro_hw_render_context_negotiation_interface_vulkan (copied; it may arrive after SET_HW_RENDER).
  // `iface` points to a retro_hw_render_context_negotiation_interface_vulkan of any version; nullptr clears it.
  void setNegotiationInterface(const void* iface);
  // Minimum Vulkan API version the core asked for in retro_hw_render_callback::version_major: a packed VK_MAKE_VERSION
  // value, or a plain major number (1 = Vulkan 1.0).
  void setRequiredApiVersion(unsigned versionMajor);

  // Creates instance, device and queue (through the core's negotiation interface where it offers one) and the frame
  // resources. After retro_load_game, before context_reset.
  bool create(QString* error);
  // vkDeviceWaitIdle, the core's destroy_device, then destroys everything. Safe to call twice.
  void destroy();
  bool hasContext() const;

  // retro_hw_render_interface_vulkan* for RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE; nullptr without a context.
  const void* renderInterface() const;

  // Diagnostics of the live context ("Vulkan 1.3", "<name> · Driver <version>"); empty before create().
  QString apiText() const;
  QString gpuText() const;

  // video_refresh(RETRO_HW_FRAME_BUFFER_VALID): submits the command buffers of set_command_buffers, copies the image of
  // set_image into a host buffer and returns it as RGB32 (alpha 0xFF); w x h is the frame size inside the image. A
  // frame larger than maxSize (empty or non-positive component = no limit) is scaled down on the CPU (aspect kept,
  // never upscaled, HwRenderContext::scaledReadbackSize). Null image on failure. read=false only submits.
  QImage presentFrame(int w, int h, const QSize& maxSize, bool read);
  // video_refresh with NULL (duplicate frame): command buffers the core queued with set_command_buffers still run once.
  void presentDuplicate();
  quint64 frameCount() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}  // namespace uzume::emu
