#pragma once
// VkRenderContext: headless Vulkan device for libretro hardware rendering (ADR 0034, next to the OpenGL
// HwRenderContext of ADR 0013).
//
// The Vulkan loader is never linked: it is loaded at runtime (QLibrary) and every function comes from
// vkGetInstanceProcAddr. No window-system extensions, no surface. The core renders into its own image; per frame the
// image is copied into a host-visible buffer and converted to an RGB32 QImage (readback; no GPU-direct encoding for
// Vulkan). With an emu::GpuDisplayTarget that offers acquireExternal() the same image is also blitted on the GPU into
// the target's external-memory image (zero-copy game view, ADR 0034 D8); any failure there drops the target and the
// readback goes on. Without the Vulkan headers at build time (UZUME_HAVE_VULKAN undefined) every call reports
// "unavailable".
//
// Thread rules: probe()/create()/destroy()/frame calls all run on the emulation thread.

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

#include <memory>

namespace uzume::emu {

class GpuDisplayTarget;

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
  // Highest packed Vulkan API version a core can get on this system: the maximum over the physical devices of
  // min(loader instance version, device apiVersion). Cached by probe(); 0 before a successful probe and without Vulkan.
  static unsigned highestUsableApi();
  // Tests: forces highestUsableApi() to report `packedVersion` (0 = off, report the probed value).
  static void setHighestApiForTest(unsigned packedVersion);
  // Tests: makes create() fail (with a Vulkan device present) as if the device could not be created.
  static void setCreateFailsForTest(bool fails);
  // Pure: a retro_hw_render_callback::version_major value (packed VK_MAKE_VERSION, or a plain major number where 1 =
  // Vulkan 1.0) as a packed version, at least Vulkan 1.0. Same rule as setRequiredApiVersion.
  static unsigned normalizeApiVersion(unsigned versionMajor);
  // Pure: the system can serve a core that asks for `versionMajor` when `highest` (packed) reaches it. highest 0 = unknown
  // and never refuses.
  static bool apiSatisfied(unsigned versionMajor, unsigned highest);

  // Pure helpers: packed VkPhysicalDeviceProperties::apiVersion -> "Vulkan 1.3"; device name + vendor id + packed driver
  // version -> "<name> · Driver <version>" (NVIDIA uses its own bit layout).
  static QString describeApi(unsigned apiVersion);
  static QString describeGpu(const QString& deviceName, unsigned vendorId, unsigned driverVersion);

  // Tests: pretends the device lacks the external-memory extensions (create() then does not enable them).
  static void setExternalMemoryMissingForTest(bool missing);
  // Tests: replaces the physical device's LUID (8 bytes) in the adapter comparison; empty = use the real one.
  static void setDeviceLuidForTest(const QByteArray& luid8);

  // The core's retro_hw_render_context_negotiation_interface_vulkan (copied; it may arrive after SET_HW_RENDER).
  // `iface` points to a retro_hw_render_context_negotiation_interface_vulkan of any version; nullptr clears it.
  void setNegotiationInterface(const void* iface);
  // Minimum Vulkan API version the core asked for in retro_hw_render_callback::version_major: a packed VK_MAKE_VERSION
  // value, or a plain major number (1 = Vulkan 1.0).
  void setRequiredApiVersion(unsigned versionMajor);

  // Zero-copy display (ADR 0034 D8). `wanted` = a display target is going to be set: create() then enables the
  // external-memory extensions the device supports (own device and through the negotiation interface). Call before
  // create(); extensions that are missing are not an error here, the target is told Unavailable at its first frame.
  void setExternalMemoryWanted(bool wanted);
  // Emulation thread, after create(). Replaces the target: the old one is released (release(true)) and what was imported
  // for it is freed (nullptr = none). The target is also released and forgotten when the context is destroyed.
  void setDisplayTarget(std::shared_ptr<GpuDisplayTarget> target);
  bool hasDisplayTarget() const;
  quint64 displayBlitCount() const;  // frames handed to the target so far (tests, diagnostics)

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
  // With a display target the frame is also blitted (scaled to displayLimit like the readback, aspect kept, never
  // upscaled) into the image the target hands out, in the same submit; *displayed tells whether a frame was presented.
  // When the target is dropped during this call the frame is read back after all, whatever `read` says.
  // withDisplay=false (the frame is not shown anywhere) skips the display copy and leaves the target alone.
  QImage presentFrame(int w, int h, const QSize& maxSize, bool read, const QSize& displayLimit = QSize(),
                      bool* displayed = nullptr, bool withDisplay = true);
  // video_refresh with NULL (duplicate frame): command buffers the core queued with set_command_buffers still run once.
  void presentDuplicate();
  quint64 frameCount() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}  // namespace uzume::emu
