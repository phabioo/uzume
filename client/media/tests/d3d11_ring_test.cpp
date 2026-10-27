// Zero-copy display ring (ADR 0027, design D4) on two WARP devices: the producer device A writes a colour into the
// shared NT-handle keyed-mutex textures (ClearRenderTargetView instead of GL, test seam makeD3d11DisplayForTest), the
// consumer on device B reads it back through a staging copy. Covers the keyed-mutex protocol, newest-wins, skip, growth
// and failure propagation. The GL interop half (wglDX*) is verified only on real GPUs. Windows only; SKIP/77 when WARP
// or shared keyed-mutex resources are not available (e.g. a CI VM).
#include <QCoreApplication>
#include <QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "d3d11displayring.h"
#include "processguard.h"

using Microsoft::WRL::ComPtr;
using namespace uzume;

namespace {

struct Warp {
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  bool create() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                       D3D11_SDK_VERSION, &dev, nullptr, &ctx));
  }
};

Warp g_a;  // producer
Warp g_b;  // consumer (stands in for Qt's device)

struct Rgba {
  quint8 r, g, b, a;
  bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};
constexpr Rgba kRed{255, 0, 0, 255};
constexpr Rgba kGreen{0, 255, 0, 255};
constexpr Rgba kBlue{0, 0, 255, 255};

// Producer side: clear the slot of the current acquire() to `c` (stands in for the GL blit).
void paint(D3d11DisplayProducer* p, const Rgba& c) {
  auto* tex = static_cast<ID3D11Texture2D*>(p->testTexture());
  QVERIFY(tex);
  ComPtr<ID3D11RenderTargetView> rtv;
  QVERIFY(SUCCEEDED(g_a.dev->CreateRenderTargetView(tex, nullptr, &rtv)));
  const float color[4] = {c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
  g_a.ctx->ClearRenderTargetView(rtv.Get(), color);
  g_a.ctx->Flush();
}

// Consumer side: reads pixel (x, y) of a texture on device B through a staging copy.
bool readPixel(ID3D11Texture2D* tex, int x, int y, Rgba* out) {
  D3D11_TEXTURE2D_DESC desc = {};
  tex->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(g_b.dev->CreateTexture2D(&desc, nullptr, &staging))) {
    return false;
  }
  g_b.ctx->CopyResource(staging.Get(), tex);
  D3D11_MAPPED_SUBRESOURCE m = {};
  if (FAILED(g_b.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
    return false;
  }
  const auto* px = static_cast<const quint8*>(m.pData) + static_cast<size_t>(y) * m.RowPitch + static_cast<size_t>(x) * 4;
  *out = {px[0], px[1], px[2], px[3]};
  g_b.ctx->Unmap(staging.Get(), 0);
  return true;
}

D3d11DisplayProducer* prod(D3d11Display& d) { return static_cast<D3d11DisplayProducer*>(d.producer.get()); }

// One produced frame: acquire, paint, present.
bool produce(D3d11Display& d, int w, int h, const Rgba& c) {
  unsigned gl = 99;
  QSize tex;
  if (d.producer->acquire(w, h, &gl, &tex) != DisplayProducer::Status::Ok) {
    return false;
  }
  paint(prod(d), c);
  d.producer->present(w, h);
  return true;
}

}  // namespace

class D3d11RingTest : public QObject {
  Q_OBJECT
 private slots:
  void colourCrossesDevices() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    unsigned gl = 99;
    QSize tex;
    QCOMPARE(d.producer->acquire(100, 50, &gl, &tex), DisplayProducer::Status::Ok);
    QCOMPARE(gl, 0u);  // no GL in the test seam
    QCOMPARE(tex, QSize(128, 64));
    paint(prod(d), kRed);
    d.producer->present(100, 50);

    ConsumerFrame f;
    QString err;
    QVERIFY2(d.consumer->frame(g_b.dev.Get(), &f, &err), qPrintable(err));
    QVERIFY(f.texture);
    QCOMPARE(f.textureSize, QSize(128, 64));
    QCOMPARE(f.contentSize, QSize(100, 50));
    QCOMPARE(f.generation, quint64(1));
    Rgba px{};
    QVERIFY(readPixel(static_cast<ID3D11Texture2D*>(f.texture), 10, 10, &px));
    QVERIFY(px == kRed);
    d.consumer->frameEnd();
    d.consumer->close();
    d.producer->release(true);
  }

  // Vulkan mode (ADR 0034 D8): the producer hands out the NT handle and makes no keyed-mutex call; the Vulkan submit would
  // AcquireSync(0) / ReleaseSync(1) itself. Here device A's own COM view of the handle plays the Vulkan side.
  void vulkanModeHandsOutHandlesAndOnlyPublishes() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    ExternalDisplayImage img;
    QCOMPARE(d.producer->acquireExternal(100, 50, &img), DisplayProducer::Status::Ok);
    QCOMPARE(img.handleType, ExternalDisplayImage::HandleType::D3D11TextureNt);
    QVERIFY(img.handle != 0);
    QCOMPARE(img.texSize, QSize(128, 64));
    QVERIFY(img.keyedMutex);
    QCOMPARE(img.acquireKey, quint64(0));
    QCOMPARE(img.releaseKey, quint64(1));
    QCOMPARE(img.generation, quint64(1));
    // The "Vulkan side": open the handle on a device, take key 0, paint, give key 1.
    ComPtr<ID3D11Device1> dev1;
    QVERIFY(SUCCEEDED(g_a.dev.As(&dev1)));
    ComPtr<ID3D11Texture2D> tex;
    QVERIFY(SUCCEEDED(dev1->OpenSharedResource1(reinterpret_cast<HANDLE>(img.handle), IID_PPV_ARGS(&tex))));
    ComPtr<IDXGIKeyedMutex> mtx;
    QVERIFY(SUCCEEDED(tex.As(&mtx)));
    QCOMPARE(mtx->AcquireSync(0, 0), S_OK);
    ComPtr<ID3D11RenderTargetView> rtv;
    QVERIFY(SUCCEEDED(g_a.dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv)));
    const float green[4] = {0.f, 1.f, 0.f, 1.f};
    g_a.ctx->ClearRenderTargetView(rtv.Get(), green);
    g_a.ctx->Flush();
    QCOMPARE(mtx->ReleaseSync(1), S_OK);
    d.producer->present(100, 50);  // publishes only

    ConsumerFrame f;
    QString err;
    QVERIFY2(d.consumer->frame(g_b.dev.Get(), &f, &err), qPrintable(err));
    QVERIFY(f.texture);
    QCOMPARE(f.contentSize, QSize(100, 50));
    Rgba px{};
    QVERIFY(readPixel(static_cast<ID3D11Texture2D*>(f.texture), 10, 10, &px));
    QVERIFY(px == kGreen);
    d.consumer->frameEnd();
    unsigned gl = 0;
    QSize glTex;
    QCOMPARE(d.producer->acquire(64, 64, &gl, &glTex), DisplayProducer::Status::Unavailable);  // the mode is fixed by the first call
    d.consumer->close();
    d.producer->release(true);
  }

  void nothingPublishedYieldsNoTexture() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    ConsumerFrame f;
    QString err;
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f, &err));
    QVERIFY(!f.texture);
    d.consumer->frameEnd();
  }

  void newestWinsAndTheOlderSlotIsRecycled() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    QVERIFY(produce(d, 64, 64, kRed));
    QVERIFY(produce(d, 64, 64, kGreen));  // supersedes red before the consumer looked
    ConsumerFrame f;
    QString err;
    QVERIFY2(d.consumer->frame(g_b.dev.Get(), &f, &err), qPrintable(err));
    QCOMPARE(f.sequence, quint64(2));
    Rgba px{};
    QVERIFY(readPixel(static_cast<ID3D11Texture2D*>(f.texture), 0, 0, &px));
    QVERIFY(px == kGreen);
    d.consumer->frameEnd();
    // Red's slot went back to the producer; the shown (green) slot stays acquired by the consumer.
    QVERIFY(produce(d, 64, 64, kBlue));
    QVERIFY(produce(d, 64, 64, kRed));
    QCOMPARE(prod(d)->skippedFrames(), quint64(0));
    d.consumer->close();
    d.producer->release(true);
  }

  void frameIsIdempotentUntilFrameEnd() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    QVERIFY(produce(d, 64, 64, kRed));
    ConsumerFrame f1;
    ConsumerFrame f2;
    QString err;
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f1, &err));
    QVERIFY(produce(d, 64, 64, kGreen));  // published while the Qt frame is still running
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f2, &err));
    QCOMPARE(f2.texture, f1.texture);
    QCOMPARE(f2.sequence, f1.sequence);
    d.consumer->frameEnd();
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f2, &err));
    QCOMPARE(f2.sequence, quint64(2));  // the next window frame picks the newer one
    QVERIFY(f2.texture != f1.texture);
    d.consumer->frameEnd();
    d.consumer->close();
    d.producer->release(true);
  }

  void skipsWhenEverySlotIsInUse() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    QVERIFY(produce(d, 64, 64, kRed));
    QVERIFY(produce(d, 64, 64, kGreen));
    QVERIFY(produce(d, 64, 64, kBlue));  // three published, the consumer never looked
    unsigned gl = 0;
    QSize tex;
    QCOMPARE(d.producer->acquire(64, 64, &gl, &tex), DisplayProducer::Status::Skip);
    QCOMPARE(prod(d)->skippedFrames(), quint64(1));
    // The consumer catches up: newest (blue) shown, the two older ones recycled, the producer continues.
    ConsumerFrame f;
    QString err;
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f, &err));
    QCOMPARE(f.sequence, quint64(3));
    d.consumer->frameEnd();
    QVERIFY(produce(d, 64, 64, kRed));
    d.consumer->close();
    d.producer->release(true);
  }

  void growthSwitchesGeneration() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    QVERIFY(produce(d, 64, 64, kRed));
    ConsumerFrame f;
    QString err;
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f, &err));
    QCOMPARE(f.generation, quint64(1));
    d.consumer->frameEnd();

    unsigned gl = 0;
    QSize tex;
    QCOMPARE(d.producer->acquire(200, 100, &gl, &tex), DisplayProducer::Status::Ok);
    QCOMPARE(tex, QSize(256, 128));
    paint(prod(d), kGreen);
    d.producer->present(200, 100);
    // A smaller request later does not shrink the textures.
    QVERIFY(produce(d, 64, 64, kBlue));
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f, &err));
    QCOMPARE(f.generation, quint64(2));
    QCOMPARE(f.textureSize, QSize(256, 128));
    Rgba px{};
    QVERIFY(readPixel(static_cast<ID3D11Texture2D*>(f.texture), 0, 0, &px));
    QVERIFY(px == kBlue);
    d.consumer->frameEnd();
    d.consumer->close();
    d.producer->release(true);
  }

  void abortDoesNotPublish() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    unsigned gl = 0;
    QSize tex;
    QCOMPARE(d.producer->acquire(64, 64, &gl, &tex), DisplayProducer::Status::Ok);
    d.producer->abort();
    ConsumerFrame f;
    QString err;
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f, &err));
    QVERIFY(!f.texture);
    d.consumer->frameEnd();
    QVERIFY(produce(d, 64, 64, kRed));  // the slot is usable again
    d.consumer->close();
    d.producer->release(true);
  }

  void producerFailureReachesTheConsumer() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    unsigned gl = 0;
    QSize tex;
    QCOMPARE(d.producer->acquire(40000, 64, &gl, &tex), DisplayProducer::Status::Failed);
    QVERIFY(!d.producer->reason().isEmpty());
    QVERIFY(d.consumer->failed());
    ConsumerFrame f;
    QString err;
    QVERIFY(!d.consumer->frame(g_b.dev.Get(), &f, &err));
    QVERIFY(!err.isEmpty());
    QCOMPARE(d.producer->acquire(64, 64, &gl, &tex), DisplayProducer::Status::Failed);
  }

  void consumerFailureReachesTheProducer() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    QVERIFY(produce(d, 64, 64, kRed));
    ConsumerFrame f;
    QString err;
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f, &err));
    d.consumer->frameEnd();
    QVERIFY(!d.consumer->frame(g_a.dev.Get(), &f, &err));  // Qt's device changed
    unsigned gl = 0;
    QSize tex;
    QCOMPARE(d.producer->acquire(64, 64, &gl, &tex), DisplayProducer::Status::Failed);
    QVERIFY(d.producer->reason().contains(QLatin1String("device")));
  }

  void consumerOutlivesTheProducer() {
    D3d11Display d = makeD3d11DisplayForTest(g_a.dev.Get());
    QVERIFY(produce(d, 64, 64, kGreen));
    ConsumerFrame f;
    QString err;
    QVERIFY(d.consumer->frame(g_b.dev.Get(), &f, &err));
    d.producer->release(true);
    d.producer.reset();
    Rgba px{};
    QVERIFY(readPixel(static_cast<ID3D11Texture2D*>(f.texture), 5, 5, &px));  // the frame in flight is still valid
    QVERIFY(px == kGreen);
    d.consumer->frameEnd();
    d.consumer->close();
  }
};

int main(int argc, char** argv) {
  fbtest::prepareProcess();
  QCoreApplication app(argc, argv);
  if (!g_a.create() || !g_b.create()) {
    qWarning("No WARP D3D11 device available");
    return 77;
  }
  {  // Probe: shared NT-handle keyed-mutex textures, opened on the second device
    D3d11Display probe = makeD3d11DisplayForTest(g_a.dev.Get());
    unsigned gl = 0;
    QSize tex;
    ConsumerFrame f;
    QString err;
    bool ok = probe.producer->acquire(64, 64, &gl, &tex) == DisplayProducer::Status::Ok;
    if (ok) {
      probe.producer->present(64, 64);
      ok = probe.consumer->frame(g_b.dev.Get(), &f, &err) && f.texture;
      probe.consumer->frameEnd();
      probe.consumer->close();
      probe.producer->release(true);
    }
    if (!ok) {
      qWarning("WARP lacks shared keyed-mutex resources: %s", qPrintable(probe.producer->reason() + err));
      return 77;
    }
  }
  D3d11RingTest t;
  return QTest::qExec(&t, argc, argv);
}

#include "d3d11_ring_test.moc"
