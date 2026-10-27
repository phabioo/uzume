// Zero-copy display (ADR 0027), Windows: makePlatformDisplay() builds the GL-D3D11 bridge, the D3D11 display ring and
// the DisplayTextureSource that wraps the ring's textures for Qt Quick's D3D11 scene graph (QSGD3D11Texture::fromNative).
//
// Thread rules: makePlatformDisplay() on the GUI thread (no GPU work). D3d11TextureSource::texture() and everything it
// triggers (afterFrameEnd, sceneGraphInvalidated) run on the scene graph render thread; the QSGTexture wrappers are
// created and deleted there only. failed() may be read from any thread. The destructor may run anywhere: it hands the
// wrappers to the render thread (scheduleRenderJob) unless sceneGraphInvalidated already cleaned them.
#include <QtGlobal>

#ifdef Q_OS_WIN

#include <QMetaObject>
#include <QPointer>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QtQuick/qsgtexture_platform.h>
#include <atomic>
#include <mutex>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "d3d11displayring.h"
#include "envvar.h"
#include "d3d11glbridge.h"
#include "displayslots.h"
#include "gpudisplaybridge.h"

namespace uzume::ui {

namespace {

using Microsoft::WRL::ComPtr;

QString apiName(QSGRendererInterface::GraphicsApi api) {
  if (api == QSGRendererInterface::Software) return QStringLiteral("the software renderer");
  if (api == QSGRendererInterface::OpenGL) return QStringLiteral("OpenGL");
  if (api == QSGRendererInterface::Vulkan) return QStringLiteral("Vulkan");
  if (api == QSGRendererInterface::Metal) return QStringLiteral("Metal");
  if (api == QSGRendererInterface::Null) return QStringLiteral("the null backend");
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  if (api == QSGRendererInterface::Direct3D12) return QStringLiteral("Direct3D 12");
#endif
  return QStringLiteral("graphics API %1").arg(static_cast<int>(api));
}

// Render-thread state: shared with the Qt connections and the cleanup job, so it outlives the source object.
struct RenderState {
  std::shared_ptr<GlD3d11Bridge> bridge;
  std::shared_ptr<D3d11DisplayConsumer> consumer;

  std::mutex m;  // wrappers / retired / window (cleanup may race with the GUI-thread destructor's scheduling)
  QPointer<QQuickWindow> window;  // the one window that uses the ring
  void* device = nullptr;         // Qt's ID3D11Device* of that window
  bool disabled = false;          // not D3D11 / no device: the CPU frame is drawn, forever
  bool cleaned = false;
  quint64 wrapGeneration = 0;
  QSGTexture* wrapped[uzume::DisplaySlots::kCount] = {};
  void* wrappedNative[uzume::DisplaySlots::kCount] = {};
  std::vector<QSGTexture*> retired;  // old generation, deleted in afterFrameEnd (after Qt rendered the frame)
  QMetaObject::Connection endConn;
  QMetaObject::Connection invalidatedConn;
  std::atomic<bool> failed{false};

  void retireWrappers() {  // m held
    for (int i = 0; i < uzume::DisplaySlots::kCount; ++i) {
      if (wrapped[i]) {
        retired.push_back(wrapped[i]);
      }
      wrapped[i] = nullptr;
      wrappedNative[i] = nullptr;
    }
  }
  void deleteRetired() {  // m held, render thread
    for (QSGTexture* t : retired) {
      delete t;
    }
    retired.clear();
  }
  // Render thread: window's scene graph is going away, or the source was destroyed. `invalidated`: the scene graph was
  // invalidated (device loss, window recreation) while the source lives on: texture() returns nullptr from now on, so
  // the source reports itself failed and the GpuDisplayBridge turns Failed (readback resumes).
  void cleanup(bool invalidated = false) {
    std::lock_guard lock(m);
    if (cleaned) {
      return;
    }
    cleaned = true;
    if (invalidated) {
      failed = true;
    }
    retireWrappers();
    deleteRetired();
    consumer->close();
  }
  // No render thread to clean up on: leak the Qt-side wrappers and the consumer (its destructor would call close() on
  // Qt's device from this thread) instead of touching Qt's device from the wrong thread.
  void abandon() {
    std::lock_guard lock(m);
    if (cleaned) {
      return;
    }
    cleaned = true;
    for (int i = 0; i < uzume::DisplaySlots::kCount; ++i) wrapped[i] = nullptr;
    retired.clear();
    new std::shared_ptr<D3d11DisplayConsumer>(consumer);  // one reference never dropped
    failed = true;
  }
};

// Render-thread cleanup. scheduleRenderJob() deletes a job that never ran (window not exposed, scene graph gone) on the
// thread that scheduled it, possibly the GUI thread: then no D3D11/Qt call may be made, the wrappers and the consumer's
// Qt-side objects are leaked on purpose (abandon()).
class CleanupJob final : public QRunnable {
 public:
  explicit CleanupJob(std::shared_ptr<RenderState> s) : s_(std::move(s)) { setAutoDelete(true); }
  ~CleanupJob() override {
    if (!ran_) s_->abandon();
  }
  void run() override {
    ran_ = true;
    s_->cleanup();
  }

 private:
  std::shared_ptr<RenderState> s_;
  bool ran_ = false;
};

class D3d11TextureSource final : public DisplayTextureSource {
 public:
  D3d11TextureSource(std::shared_ptr<GlD3d11Bridge> bridge, std::shared_ptr<D3d11DisplayConsumer> consumer)
      : s_(std::make_shared<RenderState>()) {
    s_->bridge = std::move(bridge);
    s_->consumer = std::move(consumer);
  }

  ~D3d11TextureSource() override {
    QPointer<QQuickWindow> window;
    {
      std::lock_guard lock(s_->m);
      window = s_->window;
      QObject::disconnect(s_->endConn);
      QObject::disconnect(s_->invalidatedConn);
      if (s_->cleaned) {
        return;
      }
    }
    if (window) {
      window->scheduleRenderJob(new CleanupJob(s_), QQuickWindow::BeforeSynchronizingStage);
    } else {
      s_->cleanup();  // never used a window: nothing wrapped
    }
  }

  QSGTexture* texture(QQuickWindow* window, QSize* contentSize) override {
    RenderState& s = *s_;
    if (!window) {
      return nullptr;
    }
    {
      std::lock_guard lock(s.m);
      if (s.cleaned || s.disabled) {
        return nullptr;
      }
      if (!s.window) {
        if (!attach(window)) {
          return nullptr;
        }
      } else if (s.window != window) {
        return nullptr;  // the ring has a single consumer window; another window draws the CPU frame
      }
    }
    if (s.failed.load() || s.consumer->failed()) {
      return nullptr;
    }

    ConsumerFrame f;
    QString err;
    if (!s.consumer->frame(s.device, &f, &err)) {
      s.failed = true;
      return nullptr;
    }
    if (!f.texture) {
      return nullptr;  // nothing published yet
    }
    std::lock_guard lock(s.m);
    if (f.generation != s.wrapGeneration) {
      s.retireWrappers();  // deleted in afterFrameEnd, after the frame that switched
      s.wrapGeneration = f.generation;
    }
    QSGTexture*& w = s.wrapped[f.slot];
    if (w && s.wrappedNative[f.slot] != f.texture) {
      s.retired.push_back(w);
      w = nullptr;
    }
    if (!w) {
      w = QNativeInterface::QSGD3D11Texture::fromNative(f.texture, window, f.textureSize, {});  // no mipmaps
      if (!w) {
        s.failed = true;
        return nullptr;
      }
      w->setMipmapFiltering(QSGTexture::None);
      s.wrappedNative[f.slot] = f.texture;
    }
    if (contentSize) {
      *contentSize = f.contentSize;
    }
    return w;
  }

  bool failed() const override { return s_->failed.load() || s_->consumer->failed(); }

 private:
  // First texture() of the first window (s_->m held): D3D11 check, Qt's device and adapter, connections.
  bool attach(QQuickWindow* window) {
    RenderState& s = *s_;
    QSGRendererInterface* ri = window->rendererInterface();
    if (!ri) {
      return false;  // scene graph not ready yet; ask again
    }
    const QSGRendererInterface::GraphicsApi api = ri->graphicsApi();
    if (api != QSGRendererInterface::Direct3D11) {
      s.consumer->setUnavailable(QStringLiteral("Qt Quick renders with %1").arg(apiName(api)));
      s.disabled = true;
      return false;
    }
    void* dev = ri->getResource(window, QSGRendererInterface::DeviceResource);
    quint64 luid = 0;
    if (dev) {
      ComPtr<IDXGIDevice> dxgiDevice;
      ComPtr<IDXGIAdapter> adapter;
      DXGI_ADAPTER_DESC desc = {};
      if (SUCCEEDED(static_cast<ID3D11Device*>(dev)->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
          SUCCEEDED(adapter->GetDesc(&desc))) {
        luid = (static_cast<quint64>(static_cast<quint32>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
      }
    }
    if (!dev || luid == 0) {
      s.consumer->setUnavailable(QStringLiteral("Qt Quick's D3D11 device or adapter is not available"));
      s.disabled = true;
      return false;
    }
    s.window = window;
    s.device = dev;
    s.consumer->setTargetAdapter(luid);
    std::shared_ptr<RenderState> keep = s_;
    // Both signals are emitted on the render thread: DirectConnection, so frameEnd() runs right after Qt executed the frame.
    s.endConn = QObject::connect(window, &QQuickWindow::afterFrameEnd, window,
                                 [keep]() {
                                   keep->consumer->frameEnd();
                                   std::lock_guard lock(keep->m);
                                   keep->deleteRetired();
                                 },
                                 Qt::DirectConnection);
    s.invalidatedConn = QObject::connect(window, &QQuickWindow::sceneGraphInvalidated, window, [keep]() { keep->cleanup(true); },
                                         Qt::DirectConnection);
    return true;
  }

  std::shared_ptr<RenderState> s_;
};

}  // namespace

PlatformDisplay makePlatformDisplay(QString* reason) {
  if (uzume::env::bytes("UZUME_DISABLE_ZERO_COPY") == "1") {
    if (reason) *reason = QStringLiteral("UZUME_DISABLE_ZERO_COPY=1");
    return {};
  }
  auto bridge = std::make_shared<GlD3d11Bridge>();  // no D3D/GL work until the emulation thread calls open()
  D3d11Display ring = makeD3d11Display(bridge);
  PlatformDisplay out;
  out.producer = std::move(ring.producer);
  out.source = std::make_unique<D3d11TextureSource>(bridge, std::move(ring.consumer));
  out.shared = bridge;  // a std::shared_ptr<GlD3d11Bridge>, see d3d11glbridge.h
  return out;
}

}  // namespace uzume::ui

#endif  // Q_OS_WIN
