// Vulkan hardware rendering (ADR 0034): the "vulkan" render mode of the Uzume test core through LibretroBackend and
// VkRenderContext, frames read back into the XRGB8888 path. Needs a Vulkan device (lavapipe is enough) and a
// QGuiApplication; without one the test returns 77 (ctest: SKIP), except with UZUME_REQUIRE_VULKAN=1 (Linux CI).
#include <QGuiApplication>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>

#include "emulation_runner.h"
#include "hw_render.h"
#include "libretro_backend.h"
#include "third_party/libretro/libretro.h"
#include "test_core_layout.h"
#include "vk_render.h"

using namespace uzume::emu;
namespace tc = uzume::testcore;

namespace {
constexpr QRgb kRed = 0xFFFF0000, kGreen = 0xFF00FF00, kBlue = 0xFF0000FF, kWhite = 0xFFFFFFFF, kMagenta = 0xFFFF00FF,
               kCyan = 0xFF00FFFF;

struct EnvGuard {  // sets an environment variable for the lifetime of the guard
  QByteArray name, old;
  bool had;
  EnvGuard(const char* n, const char* v) : name(n), old(qgetenv(n)), had(qEnvironmentVariableIsSet(n)) { qputenv(n, v); }
  ~EnvGuard() {
    if (had) qputenv(name.constData(), old);
    else qunsetenv(name.constData());
  }
};

std::atomic<int> g_retries{0};  // "starting again with OpenGL" lines of the runner
QtMessageHandler g_prevHandler = nullptr;
void countingHandler(QtMsgType t, const QMessageLogContext& c, const QString& m) {
  if (m.contains(QLatin1String("starting again with OpenGL"))) ++g_retries;
  if (g_prevHandler) g_prevHandler(t, c, m);
}

int coreState(int what) {  // the test core's own counters (the library is already loaded in this process)
  QLibrary lib(QStringLiteral(FB_TEST_CORE_PATH));
  using Fn = int (*)(int);
  auto fn = reinterpret_cast<Fn>(lib.resolve("uzume_test_core_vk_state"));
  return fn ? fn(what) : -2;
}
}  // namespace

class VkRenderTest : public QObject {
  Q_OBJECT
  QTemporaryDir m_dirs;

  bool start(LibretroBackend& be, const QString& mode, QString* err, const QString& scale = QStringLiteral("1"),
             const QString& preferred = QStringLiteral("opengl")) {
    be.prepareForStart();
    be.setPreferredRenderer(preferred);
    be.setSystemDirectory(m_dirs.filePath(QStringLiteral("system")));
    be.setSaveDirectory(m_dirs.filePath(QStringLiteral("save")));
    if (!be.loadCore(QStringLiteral(FB_TEST_CORE_PATH), err)) return false;
    be.setCoreOption(QStringLiteral("test_render_mode"), mode);
    be.setCoreOption(QStringLiteral("test_gl_scale"), scale);
    return be.loadGame(QStringLiteral(FB_TEST_ROM_PATH), err);
  }

  // The deterministic pattern of the core after `frames` frames at the given scale.
  void checkPattern(const QImage& f, int frames, int k) {
    QCOMPARE(f.format(), QImage::Format_RGB32);
    QCOMPARE(f.size(), QSize(tc::kWidth * k, tc::kHeight * k));
    const int W = tc::kWidth * k;
    QCOMPARE(f.pixel(W / 4, 48 * k), kRed);
    QCOMPARE(f.pixel(3 * W / 4, 48 * k), kGreen);
    QCOMPARE(f.pixel(W / 4, 144 * k), kBlue);
    QCOMPARE(f.pixel(3 * W / 4, 144 * k), kWhite);
    QCOMPARE(f.pixel(W / 2, 288 * k), kMagenta);
    const int mx = tc::vkMarkerX(static_cast<uint64_t>(frames), k);
    QCOMPARE(f.pixel(mx + 1, 1), kCyan);
    QCOMPARE(f.pixel(mx + 8 * k - 1, 8 * k - 1), kCyan);
    QCOMPARE(f.pixel(mx + 8 * k, 1) == kCyan, false);
  }

 private slots:
  void initTestCase() {
    QVERIFY(QDir().mkpath(m_dirs.filePath(QStringLiteral("system"))));
    QVERIFY(QDir().mkpath(m_dirs.filePath(QStringLiteral("save"))));
    VkRenderContext::setUnavailableForTest(false);
    g_prevHandler = qInstallMessageHandler(countingHandler);
  }
  void cleanup() {
    VkRenderContext::setUnavailableForTest(false);
    VkRenderContext::setHighestApiForTest(0);
    VkRenderContext::setCreateFailsForTest(false);
    EmulationRunner::resetVulkanFailuresForTest();
  }

  void apiVersionHelpers() {
    const unsigned v10 = 1u << 22, v11 = v10 | (1u << 12), v13 = v10 | (3u << 12);
    QCOMPARE(VkRenderContext::normalizeApiVersion(0), v10);
    QCOMPARE(VkRenderContext::normalizeApiVersion(1), v10);
    QCOMPARE(VkRenderContext::normalizeApiVersion(2), 2u << 22);
    QCOMPARE(VkRenderContext::normalizeApiVersion(v13), v13);
    QVERIFY(VkRenderContext::apiSatisfied(v11, v13));
    QVERIFY(VkRenderContext::apiSatisfied(v13, v13));
    QVERIFY(!VkRenderContext::apiSatisfied(v13, v11));
    QVERIFY(!VkRenderContext::apiSatisfied(2, v13));  // plain major 2
    QVERIFY(VkRenderContext::apiSatisfied(1, v11));
    const unsigned v10p18 = v10 | 18u;  // VK_MAKE_VERSION(1, 0, 18): the patch bits never count
    QCOMPARE(VkRenderContext::normalizeApiVersion(v10p18), v10);
    QVERIFY(VkRenderContext::apiSatisfied(v10p18, v10));
    QVERIFY(VkRenderContext::apiSatisfied(v11 | 5u, v11));
    QVERIFY(!VkRenderContext::apiSatisfied(v11 | 5u, v10));
    QVERIFY(VkRenderContext::apiSatisfied(v13, 0));  // unknown highest never refuses
    VkRenderContext::setHighestApiForTest(v11);
    QCOMPARE(VkRenderContext::highestUsableApi(), v11);
    VkRenderContext::setHighestApiForTest(0);
    QVERIFY(VkRenderContext::highestUsableApi() >= v10);  // probed in main()
  }

  // ---- Vulkan start fails, OpenGL takes over (ADR 0034)
 private:
  // Runs the game through EmulationRunner like the app: renderer vulkan, the core requires hardware rendering.
  struct RunnerResult {
    bool started = false;
    QString error;
    RenderInfo ri;
    QSize frame;
  };
  RunnerResult runViaRunner(bool withFallback = true) {
    RunnerResult out;
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QSignalSpy started(&runner, &EmulationRunner::started);
    QSignalSpy failed(&runner, &EmulationRunner::startFailed);
    QSignalSpy frames(&runner, &EmulationRunner::frameReady);
    EmulationRunner::StartRequest req;
    req.corePath = QStringLiteral(FB_TEST_CORE_PATH);
    req.gamePath = QStringLiteral(FB_TEST_ROM_PATH);
    req.systemDir = m_dirs.filePath(QStringLiteral("system"));
    req.saveDir = m_dirs.filePath(QStringLiteral("save"));
    req.coreOptions = {{QStringLiteral("test_render_mode"), QStringLiteral("vulkan")}};
    req.requireHwRender = true;
    req.preferredRenderer = QStringLiteral("vulkan");
    if (withFallback) req.openglFallbackOptions = {{QStringLiteral("test_render_mode"), QStringLiteral("opengl")}};
    runner.start(req);
    QTest::qWait(50);
    for (int i = 0; i < 400 && started.isEmpty() && failed.isEmpty(); ++i) QTest::qWait(50);
    out.started = !started.isEmpty();
    if (!failed.isEmpty()) out.error = failed.first().first().toString();
    if (out.started) {
      for (int i = 0; i < 100 && frames.count() < 3; ++i) QTest::qWait(20);
      out.ri = runner.renderInfo();
      if (!frames.isEmpty()) out.frame = frames.last().first().value<QImage>().size();
    }
    runner.stop();
    return out;
  }
  bool glUsable() {
    LibretroBackend be;
    QString err;
    const bool ok = start(be, QStringLiteral("opengl"), &err) && be.renderInfo().hwActive;
    be.unloadCore();
    return ok;
  }

 private slots:
  void impossibleVersionStartsWithOpenGl() {
    if (!glUsable()) QSKIP("no OpenGL context here");
    EnvGuard g("FB_TEST_CORE_VK_VERSION", "1.99");
    {  // without fallback options the start fails as before
      const RunnerResult none = runViaRunner(false);
      QVERIFY(!none.started);
      EmulationRunner::resetVulkanFailuresForTest();
    }
    g_retries = 0;
    const RunnerResult r = runViaRunner();
    QVERIFY2(r.started, qPrintable(r.error));
    QCOMPARE(g_retries.load(), 1);
    QVERIFY(r.ri.hwActive);
    QVERIFY(!r.ri.vulkan);
    QVERIFY(r.ri.api.startsWith(QLatin1String("OpenGL")));
    QVERIFY2(r.ri.vulkanFallback.contains(QLatin1String("Vulkan 1.99 needed")), qPrintable(r.ri.vulkanFallback));
    QVERIFY(r.ri.vulkanFallback.contains(QLatin1String("available")));
    QCOMPARE(r.frame, QSize(tc::kWidth, tc::kHeight));
  }

  void failedContextCreationStartsWithOpenGl() {
    if (!glUsable()) QSKIP("no OpenGL context here");
    VkRenderContext::setCreateFailsForTest(true);
    const RunnerResult r = runViaRunner();
    QVERIFY2(r.started, qPrintable(r.error));
    QVERIFY(r.ri.hwActive && !r.ri.vulkan);
    QVERIFY(!r.ri.vulkanFallback.isEmpty());
  }

  void secondStartSkipsVulkan() {
    if (!glUsable()) QSKIP("no OpenGL context here");
    EnvGuard g("FB_TEST_CORE_VK_VERSION", "1.99");
    g_retries = 0;
    QVERIFY(runViaRunner().started);
    QCOMPARE(g_retries.load(), 1);  // the failed Vulkan load, then the OpenGL load
    g_retries = 0;
    // The second start loads the core once: no failed Vulkan load in front of it.
    const RunnerResult r = runViaRunner();
    QVERIFY2(r.started, qPrintable(r.error));
    QVERIFY(r.ri.hwActive && !r.ri.vulkan);
    QVERIFY(r.ri.vulkanFallback.contains(QLatin1String("Vulkan 1.99 needed")));
    QCOMPARE(g_retries.load(), 0);  // straight to OpenGL: no failed Vulkan load
  }

  void coreRetryingWithOpenGlNeedsNoRunnerRetry() {
    if (!glUsable()) QSKIP("no OpenGL context here");
    EnvGuard g("FB_TEST_CORE_VK_VERSION", "1.99");
    EnvGuard g2("FB_TEST_CORE_VK_RETRY_GL", "1");
    g_retries = 0;
    const RunnerResult r = runViaRunner();
    QVERIFY2(r.started, qPrintable(r.error));
    QVERIFY(r.ri.hwActive && !r.ri.vulkan);
    QCOMPARE(g_retries.load(), 0);  // one load only
    QVERIFY(r.ri.vulkanFallback.contains(QLatin1String("Vulkan 1.99 needed")));
    // nothing was remembered: the next start tries Vulkan again (and is refused again, cheaply)
    const RunnerResult r2 = runViaRunner();
    QVERIFY2(r2.started, qPrintable(r2.error));
    QCOMPARE(g_retries.load(), 0);
  }

  void coreRetryingWithOpenGlThenFailingLoadIsNotRetried() {
    if (!glUsable()) QSKIP("no OpenGL context here");
    EnvGuard g("FB_TEST_CORE_VK_VERSION", "1.99");  // Vulkan refused, the core takes OpenGL itself ...
    EnvGuard g2("FB_TEST_CORE_VK_RETRY_GL", "1");
    EnvGuard g3("FB_TEST_CORE_FAIL_LOAD_AFTER_HW", "1");  // ... and then refuses the game for another reason
    g_retries = 0;
    const RunnerResult r = runViaRunner();
    QVERIFY(!r.started);
    QCOMPARE(g_retries.load(), 0);  // Vulkan was not the cause: no second load, no remembered failure
  }

  void negotiationSupportQuery() {
    EnvGuard g("FB_TEST_CORE_VK_NEG", "none");
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, QStringLiteral("vulkan"), &err), qPrintable(err));
    QCOMPARE(coreState(5), 2);  // the frontend's maximum for Vulkan, whatever the core announced
    QCOMPARE(coreState(6), 0);  // an unknown type: answered, version 0
    be.unloadCore();
  }

  void usableVulkanVersionStillUsesVulkan() {
    EnvGuard g("FB_TEST_CORE_VK_VERSION", "1.1");
    const RunnerResult r = runViaRunner();
    QVERIFY2(r.started, qPrintable(r.error));
    QVERIFY(r.ri.vulkan);
    QVERIFY(r.ri.vulkanFallback.isEmpty());
  }

  void describeHelpers() {
    QCOMPARE(VkRenderContext::describeApi((1u << 22) | (3u << 12) | 275u), QStringLiteral("Vulkan 1.3"));
    QCOMPARE(VkRenderContext::describeGpu(QStringLiteral("llvmpipe (LLVM 17)"), 0x10005, (24u << 22) | (0u << 12) | 5u),
             QStringLiteral("llvmpipe (LLVM 17) · Driver 24.0.5"));
    // NVIDIA packs 10.8.8.6 bits: 555.58.2
    QCOMPARE(VkRenderContext::describeGpu(QStringLiteral("GeForce"), 0x10DE, (555u << 22) | (58u << 14) | (2u << 6)),
             QStringLiteral("GeForce · Driver 555.58.2"));
  }

  void framesArriveWithExpectedPixels() {
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, QStringLiteral("vulkan"), &err), qPrintable(err));
    const int n = 12;
    for (int i = 0; i < n; ++i) QVERIFY(be.runFrame());
    checkPattern(be.videoFrame(), n, 1);
    const RenderInfo ri = be.renderInfo();
    QVERIFY(ri.hwRequested);
    QVERIFY(ri.hwActive);
    QVERIFY(ri.vulkan);
    QVERIFY2(ri.api.startsWith(QStringLiteral("Vulkan 1.")), qPrintable(ri.api));
    QVERIFY2(ri.gpu.contains(QStringLiteral(" · Driver ")), qPrintable(ri.gpu));
    QVERIFY(ri.fallbackReason.isEmpty());
    QCOMPARE(be.sourceFrameSize(), QSize(tc::kWidth, tc::kHeight));
    QVERIFY(be.hwReadbackCount() >= quint64(n));
    QVERIFY(be.frameCount() >= quint64(n));
    // Successive frames differ (the marker moves).
    be.runFrame();
    checkPattern(be.videoFrame(), n + 1, 1);
    be.unloadCore();
  }

  void negotiationInterfaces_data() {
    QTest::addColumn<QByteArray>("neg");
    QTest::newRow("v1 create_device") << QByteArray("v1");
    QTest::newRow("v2 create_instance + create_device2") << QByteArray("v2");
    QTest::newRow("none") << QByteArray("none");
  }
  void negotiationInterfaces() {
    QFETCH(QByteArray, neg);
    EnvGuard g("FB_TEST_CORE_VK_NEG", neg.constData());
    const int d1 = coreState(0), d2 = coreState(1), ci = coreState(2);
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, QStringLiteral("vulkan"), &err), qPrintable(err));
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    checkPattern(be.videoFrame(), 3, 1);
    if (neg == "v1") {
      QVERIFY(coreState(0) > d1);
      QCOMPARE(coreState(1), d2);
    } else if (neg == "v2") {
      QVERIFY(coreState(1) > d2);
      QVERIFY(coreState(2) > ci);
    } else {
      QCOMPARE(coreState(0), d1);
      QCOMPARE(coreState(1), d2);
    }
    be.unloadCore();
  }

  void variants_data() {
    QTest::addColumn<QByteArray>("name");
    QTest::addColumn<QByteArray>("value");
    QTest::newRow("rgba format") << QByteArray("FB_TEST_CORE_VK_FORMAT") << QByteArray("rgba");
    QTest::newRow("core command buffers") << QByteArray("FB_TEST_CORE_VK_CMDBUFS") << QByteArray("1");
    QTest::newRow("general layout") << QByteArray("FB_TEST_CORE_VK_LAYOUT") << QByteArray("general");
  }
  void variants() {
    QFETCH(QByteArray, name);
    QFETCH(QByteArray, value);
    EnvGuard g(name.constData(), value.constData());
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, QStringLiteral("vulkan"), &err), qPrintable(err));
    for (int i = 0; i < 6; ++i) QVERIFY(be.runFrame());
    checkPattern(be.videoFrame(), 6, 1);
    be.unloadCore();
  }

  void scaledSourceAndReadbackLimit() {
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, QStringLiteral("vulkan"), &err, QStringLiteral("2")), qPrintable(err));
    for (int i = 0; i < 4; ++i) QVERIFY(be.runFrame());
    checkPattern(be.videoFrame(), 4, 2);  // 512x768
    be.setReadbackLimit(QSize(256, 256));  // aspect 2:3 kept -> 171x256
    QVERIFY(be.runFrame());
    const QImage f = be.videoFrame();
    QCOMPARE(f.size(), HwRenderContext::scaledReadbackSize(512, 768, QSize(256, 256)));
    QVERIFY(f.width() <= 256 && f.height() <= 256);
    QCOMPARE(be.sourceFrameSize(), QSize(512, 768));
    QCOMPARE(f.pixel(f.width() / 4, f.height() / 8), kRed);  // box-filtered areas of one colour stay exact
    be.unloadCore();
  }

  void preferenceDecidesThePreferredApi() {
    {
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, QStringLiteral("auto"), &err, QStringLiteral("1"), QStringLiteral("vulkan")), qPrintable(err));
      QCOMPARE(coreState(4), 1 + int(RETRO_HW_CONTEXT_VULKAN));
      for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
      QVERIFY(be.renderInfo().vulkan);
      checkPattern(be.videoFrame(), 3, 1);
      be.unloadCore();
    }
    {  // default: OpenGL Core when a GL context exists, else no answer; never Vulkan
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, QStringLiteral("auto"), &err, QStringLiteral("1"), QStringLiteral("opengl")), qPrintable(err));
      QVERIFY(coreState(4) != 1 + int(RETRO_HW_CONTEXT_VULKAN));
      for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
      QVERIFY(!be.renderInfo().vulkan);
      be.unloadCore();
    }
    {  // UZUME_RENDERER wins over the launch preference, both ways
      EnvGuard g("UZUME_RENDERER", "vulkan");
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, QStringLiteral("auto"), &err, QStringLiteral("1"), QStringLiteral("opengl")), qPrintable(err));
      QCOMPARE(coreState(4), 1 + int(RETRO_HW_CONTEXT_VULKAN));
      be.unloadCore();
    }
    {
      EnvGuard g("UZUME_RENDERER", "opengl");
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, QStringLiteral("auto"), &err, QStringLiteral("1"), QStringLiteral("vulkan")), qPrintable(err));
      QVERIFY(coreState(4) != 1 + int(RETRO_HW_CONTEXT_VULKAN));
      be.unloadCore();
    }
    {  // UZUME_DISABLE_HW_RENDER=1 disables Vulkan too: no answer at all
      EnvGuard g("UZUME_DISABLE_HW_RENDER", "1");
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, QStringLiteral("auto"), &err, QStringLiteral("1"), QStringLiteral("vulkan")), qPrintable(err));
      QCOMPARE(coreState(4), 0);
      be.unloadCore();
    }
  }

  void noVulkanDeviceFallsBackToSoftware() {
    VkRenderContext::setUnavailableForTest(true);
    {  // the preference cannot be honoured
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, QStringLiteral("auto"), &err, QStringLiteral("1"), QStringLiteral("vulkan")), qPrintable(err));
      QVERIFY(coreState(4) != 1 + int(RETRO_HW_CONTEXT_VULKAN));
      be.unloadCore();
    }
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, QStringLiteral("vulkan"), &err), qPrintable(err));  // the core asks for Vulkan anyway
    for (int i = 0; i < 3; ++i) QVERIFY(be.runFrame());
    const RenderInfo ri = be.renderInfo();
    QVERIFY(ri.hwRequested);
    QVERIFY(!ri.hwActive);
    QVERIFY(!ri.vulkan);
    QCOMPARE(ri.fallbackReason, QString::fromLatin1(kFallbackNoVulkan));
    QCOMPARE(be.videoFrame().size(), QSize(tc::kWidth, tc::kHeight));  // software path: strips and data row
    QCOMPARE(be.videoFrame().pixel(tc::kCellWidth / 2, tc::kStripY0 + 2), kMagenta);
    be.unloadCore();
  }

  void invalidIcdFallsBackToSoftware() {
    EnvGuard g("VK_ICD_FILENAMES", "/nonexistent/uzume_no_icd.json");
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, QStringLiteral("vulkan"), &err), qPrintable(err));
    for (int i = 0; i < 2; ++i) QVERIFY(be.runFrame());
    const RenderInfo ri = be.renderInfo();
    QVERIFY(ri.hwRequested);
    QVERIFY2(!ri.hwActive, "the Vulkan loader still found a driver with an invalid VK_ICD_FILENAMES");
    QCOMPARE(ri.fallbackReason, QString::fromLatin1(kFallbackNoVulkan));
    be.unloadCore();
  }

  void disabledHwRenderGivesDisabledReason() {
    EnvGuard g("UZUME_DISABLE_HW_RENDER", "1");
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, QStringLiteral("vulkan"), &err), qPrintable(err));
    QVERIFY(be.runFrame());
    QVERIFY(!be.renderInfo().hwActive);
    QCOMPARE(be.renderInfo().fallbackReason, QString::fromLatin1(kFallbackDisabled));
    be.unloadCore();
  }

  void repeatedStartStop() {
    for (int i = 0; i < 25; ++i) {
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, QStringLiteral("vulkan"), &err), qPrintable(QStringLiteral("round %1: %2").arg(i).arg(err)));
      for (int f = 0; f < 3; ++f) QVERIFY(be.runFrame());
      QVERIFY(be.renderInfo().vulkan);
      if (i % 2) {
        be.unloadGame();  // game unloaded, core kept; loading again re-creates the context
        QVERIFY2(be.loadGame(QStringLiteral(FB_TEST_ROM_PATH), &err), qPrintable(err));
        QVERIFY(be.runFrame());
      }
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
  VkRenderTest t;
  return QTest::qExec(&t, argc, argv);
}

#include "vk_render_test.moc"
