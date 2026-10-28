#include "d3d11displayring.h"

#include <QLoggingCategory>
#include <atomic>
#include <mutex>

#include "displayslots.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h
#include <d3d10_1.h>  // ID3D10Multithread; d3d10_1.h must come before any d3d10.h (SAL check in the Windows SDK)
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace uzume {

Q_LOGGING_CATEGORY(lcRing, "uzume.gpudisplay")

namespace {
constexpr int kMaxTextureSize = 16384;  // D3D11 feature level 11_0 limit
}

struct RingGen;

// State shared by producer and consumer (and the platform source): the failed flag, the newest generation.
struct D3d11RingShared {
  std::shared_ptr<GlD3d11Bridge> bridge;  // null in tests
  void* testDevice = nullptr;             // test seam: producer device without GL
  std::mutex m;
  std::shared_ptr<RingGen> latest;  // the producer's current generation (the consumer follows it)
  QString failReason;
  std::atomic<bool> failed{false};
  std::atomic<quint64> skipped{0};
  std::atomic<quint64> nextGeneration{1};
  std::atomic<quint64> targetLuid{0};  // Qt Quick's adapter once the consumer side knows it (Vulkan mode waits for it)
  QString unavailableReason;           // m; Qt Quick does not render with D3D11 etc.
  std::atomic<bool> unavailable{false};

  void fail(const QString& why) {
    bool first = false;
    {
      std::lock_guard lock(m);
      if (!failed.load()) {
        failReason = why;
        failed = true;
        first = true;
      }
    }
    if (first) {
      qCWarning(lcRing).noquote() << QStringLiteral("zero-copy display failed: %1").arg(why);
    }
  }
  QString failure() {
    std::lock_guard lock(m);
    return failReason;
  }
};

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

namespace {

QString hex(HRESULT hr) { return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')); }

// IDXGIKeyedMutex::AcquireSync with a zero timeout: S_OK = acquired, WAIT_TIMEOUT (a SUCCESS code!) = busy, anything
// else is an error (including WAIT_ABANDONED). Never use SUCCEEDED() on the result.
enum class Acq { Got, Busy, Error };
Acq acquireSync(IDXGIKeyedMutex* mtx, UINT64 key, HRESULT* hrOut = nullptr) {
  const HRESULT hr = mtx->AcquireSync(key, 0);
  if (hrOut) {
    *hrOut = hr;
  }
  if (hr == S_OK) {
    return Acq::Got;
  }
  if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
    return Acq::Busy;
  }
  return Acq::Error;
}

}  // namespace

constexpr int kMaxBusyFrames = 30;  // consecutive Busy answers of the consumer before the ring is given up

// One generation: 3 shared textures of one size. The NT handles stay valid until the last owner dropped the object.
struct RingGen {
  quint64 id = 0;
  int w = 0;
  int h = 0;
  DisplaySlots table;  // not "slots": a Qt keyword macro
  HANDLE handles[DisplaySlots::kCount] = {};
  ~RingGen() {
    for (HANDLE h : handles) {
      if (h) {
        CloseHandle(h);
      }
    }
  }
};

// ---- producer -------------------------------------------------------------------------------------------------

struct D3d11DisplayProducer::Impl {
  std::shared_ptr<D3d11RingShared> sh;
  void* testDevice = nullptr;
  bool released = false;

  // The producer's side of the current generation (emulation thread only).
  struct Side {
    std::shared_ptr<RingGen> gen;
    ComPtr<ID3D11Texture2D> tex[DisplaySlots::kCount];
    ComPtr<IDXGIKeyedMutex> mtx[DisplaySlots::kCount];
    void* reg[DisplaySlots::kCount] = {};
    unsigned glName[DisplaySlots::kCount] = {};
  } side;
  int writing = -1;

  // Vulkan mode (first call was acquireExternal): own D3D11 device on Qt Quick's adapter, no GL, no bridge.
  enum class Mode { Undecided, Gl, Vulkan } mode = Mode::Undecided;
  ComPtr<ID3D11Device> vkDevice;
  QString vkAdapterName;  // m

  mutable std::mutex m;  // reason, vkAdapterName
  QString reason;

  void setReason(const QString& why) {
    std::lock_guard lock(m);
    reason = why;
  }

  bool test() const { return testDevice != nullptr; }
  bool vulkan() const { return mode == Mode::Vulkan; }
  ID3D11Device* device() const {
    if (test()) return static_cast<ID3D11Device*>(testDevice);
    if (vulkan()) return vkDevice.Get();
    return static_cast<ID3D11Device*>(sh->bridge->d3dDevice());
  }

  // Vulkan mode: the D3D11 device on the adapter with `luid` (not Qt Quick's device, not the bridge's).
  bool openVulkanDevice(quint64 luid, QString* why) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      *why = QStringLiteral("CreateDXGIFactory1 failed");
      return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc = {};
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> a;
      if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 dd = {};
      if (SUCCEEDED(a->GetDesc1(&dd))) {
        const quint64 l = (static_cast<quint64>(static_cast<quint32>(dd.AdapterLuid.HighPart)) << 32) | dd.AdapterLuid.LowPart;
        if (l == luid) {
          adapter = a;
          desc = dd;
          break;
        }
      }
    }
    if (!adapter) {
      *why = QStringLiteral("the display adapter (LUID 0x%1) was not found").arg(luid, 0, 16);
      return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &vkDevice, nullptr, nullptr);
    if (FAILED(hr)) {
      *why = QStringLiteral("D3D11CreateDevice failed (%1)").arg(hex(hr));
      return false;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(vkDevice.As(&mt))) mt->SetMultithreadProtected(TRUE);
    const QString name = QString::fromWCharArray(desc.Description);
    {
      std::lock_guard lock(m);
      vkAdapterName = name;
    }
    qCInfo(lcRing).noquote() << QStringLiteral("zero-copy display: D3D11 device for Vulkan frames on \"%1\"").arg(name);
    return true;
  }

  // Unregisters and frees the producer side of `s`. Needs GL current for the interop calls; without it the
  // registrations are leaked (the bridge's device handle is leaked the same way).
  void retire(Side& s, bool glCurrent) {
    if (!test() && sh->bridge) {
      const GlD3d11Bridge::GlFunctions* gl = sh->bridge->gl();
      for (int i = 0; i < DisplaySlots::kCount; ++i) {
        if (s.reg[i] && glCurrent) {
          sh->bridge->unregister(s.reg[i]);
        }
        if (s.glName[i] && glCurrent && gl) {
          gl->deleteTextures(1, &s.glName[i]);
        }
      }
    }
    s = Side();
  }

  bool createGeneration(int w, int h, QString* why) {
    ID3D11Device* dev = device();
    if (!dev) {
      *why = QStringLiteral("the D3D11 device is gone");
      return false;
    }
    Side n;
    n.gen = std::make_shared<RingGen>();
    n.gen->id = sh->nextGeneration.fetch_add(1);
    n.gen->w = w;
    n.gen->h = h;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // Qt's fromNative always views RGBA8
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    const GlD3d11Bridge::GlFunctions* gl = (test() || vulkan()) ? nullptr : sh->bridge->gl();
    if (!test() && !vulkan() && !gl) {
      *why = QStringLiteral("the GL-D3D11 bridge is closed");
      return false;
    }
    bool ok = true;
    for (int i = 0; i < DisplaySlots::kCount && ok; ++i) {
      HRESULT hr = dev->CreateTexture2D(&desc, nullptr, &n.tex[i]);
      if (FAILED(hr)) {
        *why = QStringLiteral("CreateTexture2D %1x%2 failed (%3)").arg(w).arg(h).arg(hex(hr));
        ok = false;
        break;
      }
      ComPtr<IDXGIResource1> res;
      hr = n.tex[i].As(&res);
      if (SUCCEEDED(hr)) {
        hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &n.gen->handles[i]);
      }
      if (FAILED(hr)) {
        *why = QStringLiteral("CreateSharedHandle failed (%1)").arg(hex(hr));
        ok = false;
        break;
      }
      if (FAILED(n.tex[i].As(&n.mtx[i]))) {
        *why = QStringLiteral("the shared texture has no keyed mutex");
        ok = false;
        break;
      }
      if (gl) {
        gl->genTextures(1, &n.glName[i]);
        n.reg[i] = sh->bridge->registerTexture(n.tex[i].Get(), n.glName[i], true);
        if (!n.reg[i]) {
          *why = QStringLiteral("wglDXRegisterObjectNV failed (error %1)").arg(sh->bridge->lastWin32Error());
          ok = false;
          break;
        }
      }
    }
    if (!ok) {
      retire(n, true);
      return false;
    }
    {
      std::lock_guard lock(sh->m);
      sh->latest = n.gen;
    }
    Side old = std::move(side);
    side = std::move(n);
    retire(old, true);  // the consumer keeps the old generation (its own views) until it has switched
    qCInfo(lcRing).noquote() << QStringLiteral("zero-copy display: generation %1, 3 slots of %2x%3").arg(side.gen->id).arg(w).arg(h);
    return true;
  }

  bool fail(const QString& why) {
    setReason(why);
    sh->fail(why);
    return false;
  }
};

D3d11DisplayProducer::D3d11DisplayProducer(std::shared_ptr<D3d11RingShared> shared, void* testDevice) : d_(std::make_unique<Impl>()) {
  d_->sh = std::move(shared);
  d_->testDevice = testDevice;
  if (d_->sh->bridge) d_->sh->bridge->addUser();  // released in release(): the last user closes the bridge
}

D3d11DisplayProducer::~D3d11DisplayProducer() { release(false); }

DisplayProducer::Status D3d11DisplayProducer::acquire(int w, int h, unsigned* glTexture, QSize* texSize) {
  Impl& d = *d_;
  D3d11RingShared& sh = *d.sh;
  if (sh.failed.load()) {
    d.setReason(sh.failure());
    return Status::Failed;
  }
  if (d.released) {
    d.setReason(QStringLiteral("the display ring was released"));
    return Status::Unavailable;
  }
  if (w <= 0 || h <= 0) {
    return Status::Skip;
  }
  if (d.vulkan()) {
    d.setReason(QStringLiteral("the display ring is used by a Vulkan game"));
    return Status::Unavailable;
  }
  d.mode = Impl::Mode::Gl;
  if (d.writing >= 0) {
    abort();  // acquire() twice without present(): the earlier image is dropped
  }
  if (!d.test()) {
    switch (sh.bridge->open()) {
      case GlD3d11Bridge::Status::Ok:
        break;
      case GlD3d11Bridge::Status::NotReady:
        return Status::NotReady;
      case GlD3d11Bridge::Status::Unavailable:
        d.setReason(sh.bridge->reason());
        return Status::Unavailable;
      case GlD3d11Bridge::Status::Failed:
        d.fail(sh.bridge->reason());
        return Status::Failed;
    }
  }

  int gw = 0;
  int gh = 0;
  growSize(d.side.gen ? d.side.gen->w : 0, d.side.gen ? d.side.gen->h : 0, w, h, &gw, &gh);
  if (gw > kMaxTextureSize || gh > kMaxTextureSize) {
    d.fail(QStringLiteral("the frame (%1x%2) is larger than a D3D11 texture").arg(w).arg(h));
    return Status::Failed;
  }
  if (!d.side.gen || gw != d.side.gen->w || gh != d.side.gen->h) {
    QString why;
    if (!d.createGeneration(gw, gh, &why)) {
      d.fail(why);
      return Status::Failed;
    }
  }

  const int slot = d.side.gen->table.beginWrite();
  if (slot < 0) {
    sh.skipped.fetch_add(1);
    return Status::Skip;
  }
  HRESULT hr = S_OK;
  switch (acquireSync(d.side.mtx[slot].Get(), 0, &hr)) {
    case Acq::Got:
      break;
    case Acq::Busy:  // the consumer still holds it (cannot happen for a Free slot unless Qt is slow to release)
      d.side.gen->table.abortWrite(slot);
      sh.skipped.fetch_add(1);
      return Status::Skip;
    case Acq::Error:
      d.side.gen->table.abortWrite(slot);
      d.fail(QStringLiteral("AcquireSync(0) failed (%1)").arg(hex(hr)));
      return Status::Failed;
  }
  if (!d.test()) {
    void* reg = d.side.reg[slot];
    if (!sh.bridge->lock(&reg, 1)) {
      const quint32 err = sh.bridge->lastWin32Error();
      d.side.mtx[slot]->ReleaseSync(0);
      d.side.gen->table.abortWrite(slot);
      d.fail(QStringLiteral("wglDXLockObjectsNV failed (error %1)").arg(err));
      return Status::Failed;
    }
  }
  d.writing = slot;
  *glTexture = d.side.glName[slot];
  *texSize = QSize(d.side.gen->w, d.side.gen->h);
  return Status::Ok;
}

DisplayProducer::Status D3d11DisplayProducer::acquireExternal(int w, int h, ExternalDisplayImage* out) {
  Impl& d = *d_;
  D3d11RingShared& sh = *d.sh;
  if (sh.failed.load()) {
    d.setReason(sh.failure());
    return Status::Failed;
  }
  if (d.released) {
    d.setReason(QStringLiteral("the display ring was released"));
    return Status::Unavailable;
  }
  if (w <= 0 || h <= 0) {
    return Status::Skip;
  }
  if (d.mode == Impl::Mode::Gl) {
    d.setReason(QStringLiteral("the display ring is used by an OpenGL game"));
    return Status::Unavailable;
  }
  d.mode = Impl::Mode::Vulkan;
  if (d.writing >= 0) {
    abort();
  }
  if (!d.test() && !d.vkDevice) {
    if (sh.unavailable.load()) {
      std::lock_guard lock(sh.m);
      d.setReason(sh.unavailableReason);
      return Status::Unavailable;
    }
    const quint64 luid = sh.targetLuid.load();
    if (luid == 0) {
      return Status::NotReady;  // Qt Quick's adapter is not known yet
    }
    QString why;
    if (!d.openVulkanDevice(luid, &why)) {
      d.fail(why);
      return Status::Failed;
    }
  }
  int gw = 0;
  int gh = 0;
  growSize(d.side.gen ? d.side.gen->w : 0, d.side.gen ? d.side.gen->h : 0, w, h, &gw, &gh);
  if (gw > kMaxTextureSize || gh > kMaxTextureSize) {
    d.fail(QStringLiteral("the frame (%1x%2) is larger than a D3D11 texture").arg(w).arg(h));
    return Status::Failed;
  }
  if (!d.side.gen || gw != d.side.gen->w || gh != d.side.gen->h) {
    QString why;
    if (!d.createGeneration(gw, gh, &why)) {
      d.fail(why);
      return Status::Failed;
    }
  }
  const int slot = d.side.gen->table.beginWrite();
  if (slot < 0) {
    sh.skipped.fetch_add(1);
    return Status::Skip;
  }
  d.writing = slot;  // no keyed-mutex call here: the Vulkan submit acquires key 0 and releases key 1
  out->handleType = ExternalDisplayImage::HandleType::D3D11TextureNt;
  out->handle = reinterpret_cast<qintptr>(d.side.gen->handles[slot]);
  out->texSize = QSize(d.side.gen->w, d.side.gen->h);
  out->memorySize = 0;
  out->generation = d.side.gen->id;
  out->slot = slot;
  out->keyedMutex = true;
  out->acquireKey = 0;
  out->releaseKey = 1;
  return Status::Ok;
}

bool D3d11DisplayProducer::adapterLuid(quint8 out[8]) const {
  const quint64 l = d_->sh->targetLuid.load();
  if (l == 0) return false;
  for (int i = 0; i < 8; ++i) out[i] = static_cast<quint8>((l >> (8 * i)) & 0xff);  // LUID memory order: low part, high part
  return true;
}

void D3d11DisplayProducer::present(int w, int h) {
  Impl& d = *d_;
  if (d.writing < 0) {
    return;
  }
  const int slot = d.writing;
  d.writing = -1;
  if (d.vulkan()) {
    d.side.gen->table.endWrite(slot, w, h);  // the submit already released key 1; newest wins
    return;
  }
  if (!d.test()) {
    void* reg = d.side.reg[slot];
    if (!d.sh->bridge->unlock(&reg, 1)) {
      d.side.gen->table.abortWrite(slot);
      d.fail(QStringLiteral("wglDXUnlockObjectsNV failed (error %1)").arg(d.sh->bridge->lastWin32Error()));
      return;
    }
  }
  const HRESULT hr = d.side.mtx[slot]->ReleaseSync(1);
  if (FAILED(hr)) {
    d.side.gen->table.abortWrite(slot);
    d.fail(QStringLiteral("ReleaseSync(1) failed (%1)").arg(hex(hr)));
    return;
  }
  d.side.gen->table.endWrite(slot, w, h);  // newest wins
}

void D3d11DisplayProducer::abort() {
  Impl& d = *d_;
  if (d.writing < 0) {
    return;
  }
  const int slot = d.writing;
  d.writing = -1;
  if (d.vulkan()) {  // also called after a failed submit: no key is touched here, the submit took or released them (or not) by itself
    d.side.gen->table.abortWrite(slot);
    return;
  }
  if (!d.test()) {
    void* reg = d.side.reg[slot];
    d.sh->bridge->unlock(&reg, 1);
  }
  d.side.mtx[slot]->ReleaseSync(0);
  d.side.gen->table.abortWrite(slot);
}

void D3d11DisplayProducer::release(bool glCurrent) {
  Impl& d = *d_;
  if (d.released) {
    return;
  }
  d.released = true;
  if (d.writing >= 0) {
    if (glCurrent) {
      abort();
    } else {
      d.writing = -1;  // GL is gone: nothing to unlock
    }
  }
  d.retire(d.side, glCurrent);
  d.vkDevice.Reset();
  {
    std::lock_guard lock(d.sh->m);
    d.sh->latest.reset();
  }
  const quint64 skipped = d.sh->skipped.load();
  if (skipped > 0) {
    qCInfo(lcRing).noquote() << QStringLiteral("zero-copy display: %1 display frames skipped").arg(skipped);
  }
  if (d.sh->bridge) d.sh->bridge->releaseUser(glCurrent);  // after the registrations are gone; the last user closes
}

QString D3d11DisplayProducer::reason() const {
  std::lock_guard lock(d_->m);
  return d_->reason;
}

QString D3d11DisplayProducer::adapterName() const {
  {
    std::lock_guard lock(d_->m);
    if (!d_->vkAdapterName.isEmpty()) return d_->vkAdapterName;
  }
  return d_->sh->bridge ? d_->sh->bridge->adapterName() : QString();
}
quint64 D3d11DisplayProducer::skippedFrames() const { return d_->sh->skipped.load(); }

void* D3d11DisplayProducer::testTexture() const {
  const Impl& d = *d_;
  return d.writing >= 0 ? d.side.tex[d.writing].Get() : nullptr;
}

// ---- consumer -------------------------------------------------------------------------------------------------

namespace {
// The consumer's views of one generation on Qt's device.
struct ConsGen {
  std::shared_ptr<RingGen> gen;
  ComPtr<ID3D11Texture2D> tex[DisplaySlots::kCount];
  ComPtr<IDXGIKeyedMutex> mtx[DisplaySlots::kCount];
};
}  // namespace

struct D3d11DisplayConsumer::Impl {
  std::shared_ptr<D3d11RingShared> sh;
  ComPtr<ID3D11Device1> dev;
  void* devRaw = nullptr;
  std::shared_ptr<ConsGen> cur;  // generation of the shown slot
  int shown = -1;
  int shownW = 0;
  int shownH = 0;
  quint64 shownSeq = 0;
  std::shared_ptr<ConsGen> prevGen;  // the slot replaced in this frame, released in frameEnd()
  int prev = -1;
  bool inFrame = false;
  bool closed = false;
  int busyFrames = 0;  // consecutive frames whose newest slot was still held by the producer's side
  ConsumerFrame cached;

  std::shared_ptr<ConsGen> open(const std::shared_ptr<RingGen>& g, QString* why) {
    auto cg = std::make_shared<ConsGen>();
    cg->gen = g;
    for (int i = 0; i < DisplaySlots::kCount; ++i) {
      HRESULT hr = dev->OpenSharedResource1(g->handles[i], IID_PPV_ARGS(&cg->tex[i]));
      if (FAILED(hr)) {
        *why = QStringLiteral("OpenSharedResource1 failed (%1)").arg(hex(hr));
        return nullptr;
      }
      if (FAILED(cg->tex[i].As(&cg->mtx[i]))) {
        *why = QStringLiteral("the shared texture has no keyed mutex");
        return nullptr;
      }
    }
    return cg;
  }

  void releasePrev() {
    if (prev >= 0 && prevGen) {
      prevGen->mtx[prev]->ReleaseSync(0);
      prevGen->gen->table.releaseShown(prev);
    }
    prev = -1;
    prevGen.reset();
  }
};

D3d11DisplayConsumer::D3d11DisplayConsumer(std::shared_ptr<D3d11RingShared> shared) : d_(std::make_unique<Impl>()) {
  d_->sh = std::move(shared);
}

D3d11DisplayConsumer::~D3d11DisplayConsumer() { close(); }

bool D3d11DisplayConsumer::frame(void* qtD3d11Device, ConsumerFrame* out, QString* error) {
  Impl& d = *d_;
  D3d11RingShared& sh = *d.sh;
  if (d.closed) {
    *error = QStringLiteral("the display ring is closed");
    return false;
  }
  if (sh.failed.load()) {
    *error = sh.failure();
    return false;
  }
  if (d.inFrame) {
    *out = d.cached;
    return true;
  }
  if (!qtD3d11Device) {
    sh.fail(QStringLiteral("Qt Quick has no D3D11 device"));
    *error = sh.failure();
    return false;
  }
  if (d.devRaw != qtD3d11Device) {
    if (d.devRaw) {  // Qt recreated its device: our views and the keys we hold are gone with it
      sh.fail(QStringLiteral("Qt Quick's D3D11 device changed"));
      *error = sh.failure();
      return false;
    }
    ComPtr<ID3D11Device> base(static_cast<ID3D11Device*>(qtD3d11Device));
    if (FAILED(base.As(&d.dev))) {
      sh.fail(QStringLiteral("Qt Quick's D3D11 device has no ID3D11Device1 (Windows 8 or later is required)"));
      *error = sh.failure();
      return false;
    }
    d.devRaw = qtD3d11Device;
  }

  std::shared_ptr<RingGen> latest;
  {
    std::lock_guard lock(sh.m);
    latest = sh.latest;
  }
  if (latest) {
    const DisplaySlots::Frame newest = latest->table.newestReady();
    if (newest.slot >= 0) {
      std::shared_ptr<ConsGen> cg = (d.cur && d.cur->gen == latest) ? d.cur : nullptr;
      QString why;
      if (!cg) {
        cg = d.open(latest, &why);
        if (!cg) {
          sh.fail(why);
          *error = sh.failure();
          return false;
        }
      }
      HRESULT hr = S_OK;
      const Acq a = acquireSync(cg->mtx[newest.slot].Get(), 1, &hr);
      if (a == Acq::Error) {
        sh.fail(QStringLiteral("AcquireSync(1) failed (%1)").arg(hex(hr)));
        *error = sh.failure();
        return false;
      }
      if (a == Acq::Got) {
        d.busyFrames = 0;
        // Superseded pending slots: acquire and give back with key 0 at once.
        for (int s : latest->table.readyOlderThan(newest.sequence)) {
          if (acquireSync(cg->mtx[s].Get(), 1) == Acq::Got) {
            cg->mtx[s]->ReleaseSync(0);
            latest->table.recycle(s);
          }
        }
        latest->table.takeShown(newest.slot);
        d.prev = d.shown;  // the slot being replaced (if any) is released in frameEnd()
        d.prevGen = d.shown >= 0 ? d.cur : nullptr;
        d.cur = cg;
        d.shown = newest.slot;
        d.shownW = newest.width;
        d.shownH = newest.height;
        d.shownSeq = newest.sequence;
      }
      // Busy: keep showing the old slot, ask again next frame. A key 1 that is never released (a failed Vulkan submit after
      // the slot was marked ready) would freeze the view for good: after kMaxBusyFrames the ring fails and readback takes over.
      if (a == Acq::Busy && ++d.busyFrames >= kMaxBusyFrames) {
        sh.fail(QStringLiteral("a display slot stayed busy for %1 frames").arg(kMaxBusyFrames));
        *error = sh.failure();
        return false;
      }
    }
  }

  ConsumerFrame f;
  if (d.cur && d.shown >= 0) {
    f.texture = d.cur->tex[d.shown].Get();
    f.textureSize = QSize(d.cur->gen->w, d.cur->gen->h);
    f.contentSize = QSize(d.shownW, d.shownH);
    f.generation = d.cur->gen->id;
    f.sequence = d.shownSeq;
    f.slot = d.shown;
  }
  d.cached = f;
  d.inFrame = true;
  *out = f;
  return true;
}

void D3d11DisplayConsumer::frameEnd() {
  Impl& d = *d_;
  d.releasePrev();
  d.inFrame = false;
}

void D3d11DisplayConsumer::close() {
  Impl& d = *d_;
  if (d.closed) {
    return;
  }
  d.closed = true;
  d.releasePrev();
  if (d.cur && d.shown >= 0) {
    d.cur->mtx[d.shown]->ReleaseSync(0);
    d.cur->gen->table.releaseShown(d.shown);
  }
  d.shown = -1;
  d.cur.reset();
  d.dev.Reset();
  d.devRaw = nullptr;
  d.cached = ConsumerFrame();
}

#else  // !_WIN32

struct RingGen {};

struct D3d11DisplayProducer::Impl {
  std::shared_ptr<D3d11RingShared> sh;
};

D3d11DisplayProducer::D3d11DisplayProducer(std::shared_ptr<D3d11RingShared> shared, void*) : d_(std::make_unique<Impl>()) {
  d_->sh = std::move(shared);
}
D3d11DisplayProducer::~D3d11DisplayProducer() = default;
DisplayProducer::Status D3d11DisplayProducer::acquire(int, int, unsigned*, QSize*) { return Status::Unavailable; }
DisplayProducer::Status D3d11DisplayProducer::acquireExternal(int, int, ExternalDisplayImage*) { return Status::Unavailable; }
bool D3d11DisplayProducer::adapterLuid(quint8[8]) const { return false; }
void D3d11DisplayProducer::present(int, int) {}
void D3d11DisplayProducer::abort() {}
void D3d11DisplayProducer::release(bool) {}
QString D3d11DisplayProducer::reason() const { return QStringLiteral("not Windows"); }
QString D3d11DisplayProducer::adapterName() const { return {}; }
quint64 D3d11DisplayProducer::skippedFrames() const { return 0; }
void* D3d11DisplayProducer::testTexture() const { return nullptr; }

struct D3d11DisplayConsumer::Impl {
  std::shared_ptr<D3d11RingShared> sh;
};

D3d11DisplayConsumer::D3d11DisplayConsumer(std::shared_ptr<D3d11RingShared> shared) : d_(std::make_unique<Impl>()) {
  d_->sh = std::move(shared);
}
D3d11DisplayConsumer::~D3d11DisplayConsumer() = default;
bool D3d11DisplayConsumer::frame(void*, ConsumerFrame*, QString* error) {
  *error = QStringLiteral("not Windows");
  return false;
}
void D3d11DisplayConsumer::frameEnd() {}
void D3d11DisplayConsumer::close() {}

#endif  // _WIN32

void D3d11DisplayConsumer::setTargetAdapter(quint64 luid) {
  d_->sh->targetLuid = luid;
  if (d_->sh->bridge) d_->sh->bridge->setTargetAdapter(luid);
}

void D3d11DisplayConsumer::setUnavailable(const QString& reason) {
  {
    std::lock_guard lock(d_->sh->m);
    if (!d_->sh->unavailable.load()) d_->sh->unavailableReason = reason;
    d_->sh->unavailable = true;
  }
  if (d_->sh->bridge) d_->sh->bridge->setUnavailable(reason);
}

bool D3d11DisplayConsumer::failed() const { return d_->sh->failed.load(); }
QString D3d11DisplayConsumer::failureReason() const { return d_->sh->failure(); }

D3d11Display makeD3d11Display(std::shared_ptr<GlD3d11Bridge> bridge) {
  auto shared = std::make_shared<D3d11RingShared>();
  shared->bridge = std::move(bridge);
  D3d11Display out;
  out.producer = std::make_unique<D3d11DisplayProducer>(shared, nullptr);
  out.consumer = std::make_shared<D3d11DisplayConsumer>(shared);
  return out;
}

D3d11Display makeD3d11DisplayForTest(void* producerD3dDevice) {
  auto shared = std::make_shared<D3d11RingShared>();
  shared->testDevice = producerD3dDevice;
  D3d11Display out;
  out.producer = std::make_unique<D3d11DisplayProducer>(shared, producerD3dDevice);
  out.consumer = std::make_shared<D3d11DisplayConsumer>(shared);
  return out;
}

}  // namespace uzume
