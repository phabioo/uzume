// GPU-direct Session encoding for Vulkan games (ADR 0034 D9) on Linux: the "vulkan" render mode of the Uzume test core
// through LibretroBackend and VkRenderContext, with a fake emu::GpuEncodeTarget that owns an exportable B8G8R8A8 image
// (opaque fd, VK_KHR_external_memory_fd) on its own Vulkan device. The frame must land fitted in the target's image, the
// target's capture() runs right after the submit and reads the image back; the fake compares content, orientation and size
// with the test core's known frame. The Windows handle type (D3D11 NT handle + keyed mutex) is only compiled there.
// Needs a Vulkan device with external-memory fd support (lavapipe is enough) and a QGuiApplication; without one the test
// returns 77 (ctest: SKIP), except with UZUME_REQUIRE_VULKAN=1 (Linux CI).
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QtTest>

#include <cstring>
#include <memory>

#include "emulation_runner.h"
#include "gpu_display_target.h"
#include "gpu_encode_target.h"
#include "libretro_backend.h"
#include "test_core_layout.h"
#include "vk_render.h"

#if UZUME_HAVE_VULKAN && defined(__unix__) && !defined(__APPLE__)
#define UZUME_VK_ENCODE_TEST 1
#include "vk_fake_exporter.h"
#else
#define UZUME_VK_ENCODE_TEST 0
#endif

using namespace uzume::emu;
namespace tc = uzume::testcore;

#if UZUME_VK_ENCODE_TEST

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

// The fake encode target: one exportable BGRA image of exactly the size VkRenderContext asks for, read back in capture().
class FakeEncodeTarget final : public GpuEncodeTarget {
 public:
  enum class Next { Ok, NotReady, Unavailable, Failed };

  bool init(QString* why) { return vk.init(why); }
  ~FakeEncodeTarget() override { vk.destroy(&img); }

  // ---- knobs and results (the test thread reads after runFrame(), which runs inline)
  bool wantedFlag = true;
  QSize max = QSize(1280, 1920);
  bool luidValid = false;
  quint8 luid[8] = {};
  Next next = Next::Ok;
  int notReadyFrames = 0;     // attachExternal answers NotReady this many times first
  bool failCapture = false;
  int attaches = 0, captures = 0, detaches = 0;
  quint64 generation = 0;
  QString failReason, unavailableReason;
  QImage lastImage;           // RGB32, the image as read back after the last capture()
  QSize lastAttachSize;
  VulkanDevice lastDevice;

  // ---- GpuEncodeTarget
  bool wanted() const override { return wantedFlag && failReason.isEmpty() && unavailableReason.isEmpty(); }
  QSize maxSize() const override { return max; }
  Attach attach(unsigned, int, int) override { return Attach::Unavailable; }
  Attach attachExternal(int w, int h, const VulkanDevice& device, ExternalImage* out) override {
    ++attaches;
    lastDevice = device;
    lastAttachSize = QSize(w, h);
    if (notReadyFrames > 0) {
      --notReadyFrames;
      return Attach::NotReady;
    }
    switch (next) {
      case Next::NotReady: return Attach::NotReady;
      case Next::Unavailable: return Attach::Unavailable;
      case Next::Failed: return Attach::Failed;
      case Next::Ok: break;
    }
    vk.destroy(&img);  // a new size supersedes the old texture
    if (!vk.create(&img, w, h, /*bgra=*/true)) return Attach::Failed;
    out->handleType = ExternalImage::HandleType::OpaqueFd;
    out->handle = img.fd;
    out->texSize = QSize(w, h);
    out->memorySize = img.memSize;
    out->generation = ++generation;
    out->slot = 0;
    out->bgra = true;
    out->keyedMutex = false;
    return Attach::Ok;
  }
  AdapterLuid adapterLuid() const override {
    AdapterLuid l;
    l.valid = luidValid;
    std::memcpy(l.bytes, luid, 8);
    return l;
  }
  void markUnavailable(const QString& reason) override {
    if (unavailableReason.isEmpty()) unavailableReason = reason;
  }
  void detach(bool) override {
    ++detaches;
    vk.destroy(&img);
  }
  bool capture() override {
    ++captures;
    if (failCapture) return false;
    lastImage = vk.readBack(img, img.w, img.h);
    return true;
  }
  void fail(const QString& reason) override {
    if (failReason.isEmpty()) failReason = reason;
  }

  testing::FakeVkExporter vk;
  testing::FakeVkExporter::Image img;
};

// A display target next to it: one RGBA image of the frame size (single slot), read back at present().
class FakeDisplayTarget final : public GpuDisplayTarget {
 public:
  bool init(QString* why) { return vk.init(why); }
  ~FakeDisplayTarget() override { vk.destroy(&img); }
  int presented = 0;
  QImage lastImage;
  bool wanted() const override { return true; }
  Acquire acquire(int, int, unsigned*, QSize*) override { return Acquire::Unavailable; }
  Acquire acquireExternal(int w, int h, ExternalImage* out) override {
    if (img.w != w || img.h != h) {
      vk.destroy(&img);
      if (!vk.create(&img, w, h, false)) return Acquire::Failed;
      ++generation;
    }
    out->handleType = ExternalImage::HandleType::OpaqueFd;
    out->handle = img.fd;
    out->texSize = QSize(w, h);
    out->memorySize = img.memSize;
    out->generation = generation;
    out->slot = 0;
    return Acquire::Ok;
  }
  void present(int w, int h) override {
    ++presented;
    lastImage = vk.readBack(img, w, h);
  }
  void abort() override {}
  void release(bool) override { vk.destroy(&img); }
  void fail(const QString&) override {}

  testing::FakeVkExporter vk;
  testing::FakeVkExporter::Image img;
  quint64 generation = 0;
};

}  // namespace

class VkEncodeTest : public QObject {
  Q_OBJECT
  QTemporaryDir m_dirs;

  // Starts the test core in Vulkan mode with the targets set before the game loads (GameSession order).
  bool start(LibretroBackend& be, const std::shared_ptr<FakeEncodeTarget>& encode, QString* err,
             const std::shared_ptr<FakeDisplayTarget>& display = nullptr) {
    be.prepareForStart();
    be.setPreferredRenderer(QStringLiteral("vulkan"));
    be.setSystemDirectory(m_dirs.filePath(QStringLiteral("system")));
    be.setSaveDirectory(m_dirs.filePath(QStringLiteral("save")));
    if (!be.loadCore(QStringLiteral(FB_TEST_CORE_PATH), err)) return false;
    be.setCoreOption(QStringLiteral("test_render_mode"), QStringLiteral("vulkan"));
    if (encode) be.setGpuEncodeTarget(encode);
    if (display) be.setGpuDisplayTarget(display);
    return be.loadGame(QStringLiteral(FB_TEST_ROM_PATH), err);
  }

  std::shared_ptr<FakeEncodeTarget> makeEncode() {
    auto t = std::make_shared<FakeEncodeTarget>();
    QString why;
    if (!t->init(&why)) {
      if (qEnvironmentVariable("UZUME_REQUIRE_VULKAN") == QLatin1String("1")) QTest::qFail(qPrintable(why), __FILE__, __LINE__);
      else QTest::qSkip(qPrintable(why), __FILE__, __LINE__);
      return nullptr;
    }
    return t;
  }

  // Content and orientation of the test core's frame at any fitted size (aspect within rounding), marker only at scale 1.
  void checkColors(const QImage& f) {
    QCOMPARE(f.format(), QImage::Format_RGB32);
    const int W = f.width(), H = f.height();
    auto at = [&](double fx, double fy) { return f.pixel(int(fx * W), int(fy * H)); };
    QCOMPARE(at(0.25, 48.0 / 384), kRed);  // red top left, green top right: no mirror, no flip
    QCOMPARE(at(0.75, 48.0 / 384), kGreen);
    QCOMPARE(at(0.25, 144.0 / 384), kBlue);
    QCOMPARE(at(0.75, 144.0 / 384), kWhite);
    QCOMPARE(at(0.5, 288.0 / 384), kMagenta);
  }
  void checkPattern(const QImage& f, int frames) {
    QCOMPARE(f.size(), QSize(tc::kWidth, tc::kHeight));
    checkColors(f);
    const int mx = tc::vkMarkerX(static_cast<uint64_t>(frames), 1);
    QCOMPARE(f.pixel(mx + 1, 1), kCyan);
    QCOMPARE(f.pixel(mx + 8 - 1, 8 - 1), kCyan);
    QCOMPARE(f.pixel(mx + 8, 1) == kCyan, false);
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

  // The frame lands in the encode image, orientation and content as the test core drew it; the capture follows every frame.
  void frameLandsInTheEncodeImage_data() {
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
  void frameLandsInTheEncodeImage() {
    QFETCH(QByteArray, neg);
    QFETCH(QByteArray, cmdbufs);
    QFETCH(QByteArray, layout);
    QFETCH(QByteArray, format);
    EnvGuard g1("FB_TEST_CORE_VK_NEG", neg.constData());
    EnvGuard g2("FB_TEST_CORE_VK_CMDBUFS", cmdbufs.constData());
    EnvGuard g3("FB_TEST_CORE_VK_LAYOUT", layout.constData());
    EnvGuard g4("FB_TEST_CORE_VK_FORMAT", format.constData());
    auto t = makeEncode();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);  // the Session is the only consumer
    const int n = 12;
    for (int i = 0; i < n; ++i) {
      QVERIFY(be.runFrame());
      QVERIFY(be.lastGpuCaptured());
    }
    QVERIFY(be.renderInfo().vulkan);
    QCOMPARE(t->captures, n);
    QCOMPARE(t->attaches, 1);  // attached once, the size did not change
    QCOMPARE(t->detaches, 0);
    QVERIFY2(t->failReason.isEmpty() && t->unavailableReason.isEmpty(), qPrintable(t->failReason + t->unavailableReason));
    QCOMPARE(be.hwReadbackCount(), quint64(0));  // no readback anywhere
    QVERIFY(be.videoFrame().isNull());
    QVERIFY(be.lastGpuCopyMs() >= 0.0);
    QCOMPARE(t->lastAttachSize, QSize(tc::kWidth, tc::kHeight));  // never upscaled, here the frame itself
    checkPattern(t->lastImage, n);
    be.runFrame();
    checkPattern(t->lastImage, n + 1);  // successive frames differ (the marker moves)
    be.unloadCore();
    QVERIFY(t->detaches >= 1);  // the context hands the target back when it goes away
  }

  void theDeviceFactsReachTheTarget() {
    auto t = makeEncode();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    const QByteArray luid("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    VkRenderContext::setDeviceLuidForTest(luid);
    QVERIFY(be.runFrame());
    QVERIFY(t->lastDevice.luid.valid);
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(t->lastDevice.luid.bytes), 8), luid);
    QVERIFY(t->lastDevice.vendorId != 0);
    be.unloadCore();
  }

  // The encode image is the frame fitted into the target's maxSize: aspect kept, never upscaled, even.
  void frameIsFittedIntoMaxSize() {
    auto t = makeEncode();
    if (!t) return;
    t->max = QSize(128, 192);
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    for (int i = 0; i < 4; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->lastAttachSize, QSize(128, 192));
    QCOMPARE(t->lastImage.size(), QSize(128, 192));
    checkColors(t->lastImage);  // linear filter, but the quads are large
    QCOMPARE(t->generation, quint64(1));
    // A limit larger than the frame never upscales; the size change is a new image (generation) that is imported again.
    t->max = QSize(1024, 1024);
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->lastAttachSize, QSize(tc::kWidth, tc::kHeight));
    QCOMPARE(t->generation, quint64(2));
    QCOMPARE(t->attaches, 2);
    QCOMPARE(t->lastImage.size(), QSize(tc::kWidth, tc::kHeight));
    checkPattern(t->lastImage, 7);
    be.unloadCore();
  }

  void anOddFitIsRoundedDownToEven() {
    auto t = makeEncode();
    if (!t) return;
    t->max = QSize(101, 101);  // 256x384 -> 67.4 x 101: even width 66 (or less), height 100
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->lastAttachSize.width() % 2, 0);
    QCOMPARE(t->lastAttachSize.height() % 2, 0);
    QVERIFY(t->lastAttachSize.width() <= 101 && t->lastAttachSize.height() <= 101);
    QCOMPARE(t->lastImage.size(), t->lastAttachSize);
    checkColors(t->lastImage);
    be.unloadCore();
  }

  void notWantedMeansNoBlitAndNoCapture() {
    auto t = makeEncode();
    if (!t) return;
    t->wantedFlag = false;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    for (int i = 0; i < 4; ++i) {
      QVERIFY(be.runFrame());
      QVERIFY(!be.lastGpuCaptured());
    }
    QCOMPARE(t->attaches, 0);  // not even attached
    QCOMPARE(t->captures, 0);
    QVERIFY(!be.videoFrame().isNull());  // readback went on
    t->wantedFlag = true;
    for (int i = 0; i < 3; ++i) {
      QVERIFY(be.runFrame());
      QVERIFY(be.lastGpuCaptured());
    }
    QCOMPARE(t->captures, 3);
    checkPattern(t->lastImage, 7);
    t->wantedFlag = false;
    QVERIFY(be.runFrame());
    QCOMPARE(t->captures, 3);  // switched off again: no more blits
    be.unloadCore();
  }

  void notReadyIsAskedAgainOnLaterFrames() {
    auto t = makeEncode();
    if (!t) return;
    t->notReadyFrames = 2;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(true);
    for (int i = 0; i < 2; ++i) {
      QVERIFY(be.runFrame());
      QVERIFY(!be.lastGpuCaptured());
      QCOMPARE(t->captures, 0);
    }
    QVERIFY(be.runFrame());
    QVERIFY(be.lastGpuCaptured());
    QCOMPARE(t->captures, 1);
    QVERIFY(t->failReason.isEmpty());
    be.unloadCore();
  }

  // The target gives up in the middle of the game (consumer side): no more copies; the game and the readback go on.
  void targetFailureMidGameKeepsTheGameAndReadback() {
    auto t = makeEncode();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    for (int i = 0; i < 4; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->captures, 4);
    QVERIFY(be.videoFrame().isNull());
    t->fail(QStringLiteral("consumer gave up"));  // wanted() is false from now on
    be.setReadbackEnabled(true);                  // the Session's re-plan after the state change
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->captures, 4);
    checkPattern(be.videoFrame(), 7);
    QVERIFY(be.renderInfo().vulkan);
    be.unloadCore();
  }

  // A capture failure drops the target (fail + detach), is never retried, and the game keeps running with readback frames.
  void captureFailureDropsTheTargetButNotTheGame() {
    auto t = makeEncode();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(true);
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->captures, 3);
    t->failCapture = true;
    QVERIFY(be.runFrame());
    QVERIFY(!be.lastGpuCaptured());
    QCOMPARE(t->captures, 4);
    QVERIFY(!t->failReason.isEmpty());
    QVERIFY(t->detaches >= 1);
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->captures, 4);  // forgotten: never asked again in this game
    QCOMPARE(t->attaches, 1);
    checkPattern(be.videoFrame(), 7);
    QVERIFY(be.renderInfo().vulkan);
    be.unloadCore();
  }

  void attachFailureDropsTheTarget() {
    auto t = makeEncode();
    if (!t) return;
    t->next = FakeEncodeTarget::Next::Failed;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QVERIFY(!t->failReason.isEmpty());
    QVERIFY(t->detaches >= 1);
    QCOMPARE(t->attaches, 1);
    QCOMPARE(t->captures, 0);
    QVERIFY(!be.videoFrame().isNull());
    be.unloadCore();
  }

  void targetThatIsNotApplicableIsReleasedQuietly() {
    auto t = makeEncode();
    if (!t) return;
    t->next = FakeEncodeTarget::Next::Unavailable;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QVERIFY(t->failReason.isEmpty());
    QVERIFY(t->unavailableReason.isEmpty());  // it reported itself; the Vulkan side adds nothing
    QVERIFY(t->detaches >= 1);
    QCOMPARE(t->attaches, 1);
    QVERIFY(!be.videoFrame().isNull());
    be.unloadCore();
  }

  void otherAdapterIsUnavailableAndReadbackContinues() {
    auto t = makeEncode();
    if (!t) return;
    t->luidValid = true;
    for (int i = 0; i < 8; ++i) t->luid[i] = static_cast<quint8>(0x10 + i);
    VkRenderContext::setDeviceLuidForTest(QByteArray(8, '\x7f'));
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->unavailableReason, QStringLiteral("Vulkan device is on another adapter"));
    QVERIFY(t->failReason.isEmpty());
    QCOMPARE(t->attaches, 0);  // known before the target creates anything
    QCOMPARE(t->captures, 0);
    QVERIFY(t->detaches >= 1);
    checkPattern(be.videoFrame(), 3);
    be.unloadCore();
  }

  void sameAdapterIsAccepted() {
    auto t = makeEncode();
    if (!t) return;
    t->luidValid = true;
    for (int i = 0; i < 8; ++i) t->luid[i] = static_cast<quint8>(0x10 + i);
    VkRenderContext::setDeviceLuidForTest(QByteArray(reinterpret_cast<const char*>(t->luid), 8));
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QCOMPARE(t->captures, 3);
    be.unloadCore();
  }

  void missingExtensionIsUnavailableNotAFailedGame() {
    VkRenderContext::setExternalMemoryMissingForTest(true);
    auto t = makeEncode();
    if (!t) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, t, &err), qPrintable(err));
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    QVERIFY2(t->unavailableReason.contains(QLatin1String("VK_KHR_external_memory")), qPrintable(t->unavailableReason));
    QVERIFY(t->failReason.isEmpty());
    QCOMPARE(t->captures, 0);
    QVERIFY(be.renderInfo().vulkan);
    checkPattern(be.videoFrame(), 3);
    be.unloadCore();
  }

  void targetThatArrivesAfterCreateIsServed() {
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, nullptr, &err), qPrintable(err));  // no target at device creation: the interop extensions are enabled anyway
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    auto t = makeEncode();
    if (!t) return;
    be.setGpuEncodeTarget(t);
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    QVERIFY2(t->unavailableReason.isEmpty(), qPrintable(t->unavailableReason));
    QVERIFY2(t->captures > 0, "a late target gets the frames (a Session started after the game)");
    QVERIFY(!be.videoFrame().isNull());
    be.unloadCore();
  }

  void replacingTheTargetDetachesTheOldOne() {
    auto a = makeEncode();
    if (!a) return;
    auto b = makeEncode();
    if (!b) return;
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, a, &err), qPrintable(err));
    be.setReadbackEnabled(false);
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    QCOMPARE(a->captures, 2);
    be.setGpuEncodeTarget(b);
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    QVERIFY(a->detaches >= 1);
    QCOMPARE(a->captures, 2);
    QCOMPARE(b->captures, 2);
    checkPattern(b->lastImage, 4);
    be.setGpuEncodeTarget(nullptr);
    QVERIFY(be.runFrame());
    QVERIFY(b->detaches >= 1);
    QVERIFY(!be.lastGpuCaptured());
    be.unloadCore();
  }

  // Display and encode targets (and the readback) are served by the same frame: each gets its own copy, sized by its own
  // limit, none of them disturbs the others.
  void displayEncodeAndReadbackShareOneFrame() {
    auto e = makeEncode();
    if (!e) return;
    auto d = std::make_shared<FakeDisplayTarget>();
    QString why;
    if (!d->init(&why)) {
      if (qEnvironmentVariable("UZUME_REQUIRE_VULKAN") == QLatin1String("1")) QTest::qFail(qPrintable(why), __FILE__, __LINE__);
      else QTest::qSkip(qPrintable(why), __FILE__, __LINE__);
      return;
    }
    e->max = QSize(128, 192);
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, e, &err, d), qPrintable(err));  // readback stays on
    for (int i = 0; i < 6; ++i) QVERIFY(be.runFrame());
    QCOMPARE(d->presented, 6);
    QCOMPARE(e->captures, 6);
    QVERIFY(be.lastDisplayed());
    QVERIFY(be.lastGpuCaptured());
    QVERIFY(be.hwReadbackCount() >= 6);
    checkPattern(be.videoFrame(), 6);
    checkPattern(d->lastImage, 6);
    QCOMPARE(be.videoFrame(), d->lastImage);
    QCOMPARE(e->lastImage.size(), QSize(128, 192));
    checkColors(e->lastImage);
    // The encode target going away costs the display nothing.
    e->fail(QStringLiteral("gone"));
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    QCOMPARE(d->presented, 8);
    QCOMPARE(e->captures, 6);
    checkPattern(d->lastImage, 8);
    be.unloadCore();
  }

  void restartInTheSameProcessAttachesAgain() {
    for (int round = 0; round < 3; ++round) {
      auto t = makeEncode();
      if (!t) return;
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, t, &err), qPrintable(err));
      be.setReadbackEnabled(false);
      for (int i = 0; i < 4; ++i) QVERIFY(be.runFrame());
      QCOMPARE(t->captures, 4);
      checkPattern(t->lastImage, 4);
      be.unloadCore();
      QVERIFY(t->detaches >= 1);
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
  VkEncodeTest t;
  return QTest::qExec(&t, argc, argv);
}

#include "vk_encode_test.moc"

#else  // no Vulkan headers or not a Unix: the fake target needs opaque fds

int main() {
  qWarning("vk_encode_test: needs the Vulkan headers and Linux - skipped");
  return 77;
}

#endif
