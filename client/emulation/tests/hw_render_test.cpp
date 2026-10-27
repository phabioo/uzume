// OpenGL hardware rendering of LibretroBackend with a fake HW core (no ROM). Needs a GUI application and a
// working OpenGL 3.3 core context. Without one the test returns 77 (ctest: SKIP) - except on Linux CI
// (environment variable CI set), where a missing context is a failure so the test cannot silently vanish.
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMutex>
#include <QTemporaryFile>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QSignalSpy>
#include <QSurfaceFormat>
#include <QtTest>

#include <atomic>
#include <functional>
#include <memory>

#include "emulation_runner.h"
#include "fake_gl_display.h"
#include "hw_render.h"
#include "libretro_backend.h"

using namespace uzume::emu;

namespace {
bool canCreateGl33Core() {
  QOffscreenSurface surface;
  surface.create();
  if (!surface.isValid()) return false;
  QSurfaceFormat fmt;
  fmt.setRenderableType(QSurfaceFormat::OpenGL);
  fmt.setVersion(3, 3);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext ctx;
  ctx.setFormat(fmt);
  return ctx.create() && ctx.makeCurrent(&surface);
}

QRgb px(const QImage& img, int x, int y) { return img.pixel(x, y); }

constexpr int kRgba8 = 0x8058;  // GL_RGBA8

// GpuEncodeTarget double (ADR 0019): records every call, checks the contract (GL current on the emulation thread) and
// reads the encode texture with its own FBO, so it needs nothing but GL 3.3 core functions. Results are scriptable.
// Thread-safe: the runner tests call it from the emulation thread.
class FakeTarget : public GpuEncodeTarget {
 public:
  struct Call {
    QString name;               // attach | attach (not ready) | detach | capture | fail
    unsigned tex = 0;           // the texture of attach / detach / capture
    QSize size;                 // attach / detach / capture
    bool glCurrentArg = false;  // detach
    bool isTexture = false;     // detach: glIsTexture(tex) at that moment
    int internalFormat = 0;     // attach / detach: GL_TEXTURE_INTERNAL_FORMAT of level 0
    QString reason;             // fail
  };

  // Scripting. wanted and the size limit may change while the Session runs.
  std::atomic<bool> isWanted{true};
  std::atomic<int> maxW{1280}, maxH{1920};
  int notReady = 0;          // attach() answers NotReady this many times first
  bool attachFails = false;        // attach() answers Failed
  bool attachUnavailable = false;  // attach() answers Unavailable (not applicable here, e.g. a non-NVIDIA GL context)
  int failCaptureAt = 0;     // the n-th capture() (1-based) returns false
  bool keepImages = true;

  bool wanted() const override { return isWanted.load(); }
  QSize maxSize() const override { return QSize(maxW.load(), maxH.load()); }

  Attach attach(unsigned texture, int width, int height) override {
    QMutexLocker l(&m_);
    if (!glCurrent()) violations_.append(QStringLiteral("attach without a current GL context"));
    if (notReady > 0) {
      --notReady;
      calls_.append(makeCall(QStringLiteral("attach (not ready)"), texture, QSize(width, height)));
      return Attach::NotReady;
    }
    Call c = makeCall(QStringLiteral("attach"), texture, QSize(width, height));
    c.internalFormat = internalFormatOf(texture);
    calls_.append(c);
    if (attachUnavailable) return Attach::Unavailable;
    if (attachFails) return Attach::Failed;
    tex_ = texture;
    size_ = QSize(width, height);
    return Attach::Ok;
  }

  void detach(bool glCurrentArg) override {
    QMutexLocker l(&m_);
    Call c = makeCall(QStringLiteral("detach"), tex_, size_);
    c.glCurrentArg = glCurrentArg;
    if (glCurrentArg && !glCurrent()) violations_.append(QStringLiteral("detach(true) without a current GL context"));
    if (QOpenGLContext* ctx = QOpenGLContext::currentContext()) {
      c.isTexture = ctx->functions()->glIsTexture(tex_) != GL_FALSE;
      if (c.isTexture) c.internalFormat = internalFormatOf(tex_);
    }
    calls_.append(c);
    tex_ = 0;  // idempotent
  }

  bool capture() override {
    QMutexLocker l(&m_);
    calls_.append(makeCall(QStringLiteral("capture"), tex_, size_));
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx) {
      violations_.append(QStringLiteral("capture without a current GL context"));
      return false;
    }
    if (tex_ == 0) {
      violations_.append(QStringLiteral("capture while detached"));
      return false;
    }
    ++captures_;
    if (failCaptureAt > 0 && captures_ == failCaptureAt) return false;
    QOpenGLFunctions* f = ctx->functions();
    GLint readFbo = 0, drawFbo = 0;
    f->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    f->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    GLuint fbo = 0;
    f->glGenFramebuffers(1, &fbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    QImage img(size_, QImage::Format_RGBA8888);  // row 0 = GL row 0 = top row of the image (the interface contract)
    const bool ok = f->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) f->glReadPixels(0, 0, size_.width(), size_.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));  // the core's state stays as it was
    f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
    f->glDeleteFramebuffers(1, &fbo);
    if (!ok) {
      violations_.append(QStringLiteral("the encode texture cannot be read through an FBO"));
      return false;
    }
    if (keepImages) images_.append(img);
    return true;
  }

  void fail(const QString& reason) override {
    QMutexLocker l(&m_);
    Call c = makeCall(QStringLiteral("fail"), 0, QSize());
    c.reason = reason;
    calls_.append(c);
  }

  // Inspection (any thread).
  QList<Call> calls() const {
    QMutexLocker l(&m_);
    return calls_;
  }
  QStringList names() const {
    QStringList out;
    for (const Call& c : calls()) out.append(c.name);
    return out;
  }
  int count(const QString& name) const { return static_cast<int>(names().count(name)); }
  QList<QImage> images() const {
    QMutexLocker l(&m_);
    return images_;
  }
  QStringList violations() const {
    QMutexLocker l(&m_);
    return violations_;
  }

 private:
  static Call makeCall(const QString& name, unsigned tex, const QSize& size) {
    Call c;
    c.name = name;
    c.tex = tex;
    c.size = size;
    return c;
  }
  static bool glCurrent() { return QOpenGLContext::currentContext() != nullptr; }
  static int internalFormatOf(unsigned texture) {
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx) return 0;
    QOpenGLExtraFunctions* fx = ctx->extraFunctions();
    GLint prev = 0, v = 0;
    fx->glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    fx->glBindTexture(GL_TEXTURE_2D, texture);
    fx->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1003 /* GL_TEXTURE_INTERNAL_FORMAT */, &v);
    fx->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
    return v;
  }

  mutable QMutex m_;
  QList<Call> calls_;
  QList<QImage> images_;
  QStringList violations_;
  unsigned tex_ = 0;
  QSize size_;
  int captures_ = 0;
};
}  // namespace

// GpuDisplayTarget double (ADR 0027) around the shared GL fake of testutil/fake_gl_display.h.
class FakeDisplay : public GpuDisplayTarget {
 public:
  fbtest::FakeGlDisplay gl;
  std::function<void()> onRelease;  // called inside release(), before the fake frees its texture

  static Acquire map(fbtest::FakeGlDisplay::Status s) {
    switch (s) {
      case fbtest::FakeGlDisplay::Status::Ok: return Acquire::Ok;
      case fbtest::FakeGlDisplay::Status::Skip: return Acquire::Skip;
      case fbtest::FakeGlDisplay::Status::NotReady: return Acquire::NotReady;
      case fbtest::FakeGlDisplay::Status::Unavailable: return Acquire::Unavailable;
      case fbtest::FakeGlDisplay::Status::Failed: return Acquire::Failed;
    }
    return Acquire::Failed;
  }
  bool wanted() const override { return gl.isWanted.load(); }
  Acquire acquire(int w, int h, unsigned* tex, QSize* texSize) override { return map(gl.acquire(w, h, tex, texSize)); }
  void present(int w, int h) override { gl.present(w, h); }
  void abort() override { gl.abort(); }
  void release(bool glCurrent) override {
    if (onRelease) onRelease();
    gl.release(glCurrent);
  }
  void fail(const QString& reason) override { gl.fail(reason); }
};

class HwRenderTest : public QObject {
  Q_OBJECT
  QTemporaryFile m_log;  // events written by the fake core (FB_FAKE_HW_LOG)
  QStringList events() {
    QFile f(m_log.fileName());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  }
  int count(const QString& prefix) {
    int n = 0;
    for (const QString& e : events()) n += e.startsWith(prefix) ? 1 : 0;
    return n;
  }
  void clearLog() { QVERIFY(QFile::resize(m_log.fileName(), 0)); }

 private slots:
  void init() { clearLog(); }
  // Every test starts from a clean fake-core mode, also after a failed QCOMPARE returned early.
  void cleanup() {
    for (const char* k : {"FB_FAKE_HW_CTX", "FB_FAKE_HW_MAJOR", "FB_FAKE_HW_MINOR", "FB_FAKE_HW_SCALE", "FB_FAKE_HW_STRIPES",
                          "FB_FAKE_HW_BOTTOM_LEFT", "FB_FAKE_HW_COUNTER", "FB_FAKE_HW_LOGAV", "FB_FAKE_HW_READFBO",
                          "FB_FAKE_HW_GLERROR", "FB_FAKE_HW_DUPE_EVERY", "UZUME_SYNC_READBACK", "UZUME_DISABLE_HW_RENDER"})
      qunsetenv(k);
  }
  void initTestCase() {
    QVERIFY(m_log.open());
    qputenv("FB_FAKE_HW_LOG", m_log.fileName().toUtf8());
    qunsetenv("UZUME_DISABLE_HW_RENDER");
    qunsetenv("FB_FAKE_HW_BOTTOM_LEFT");
    qunsetenv("FB_FAKE_HW_CTX");
  }

  void hwFrameMatchesOrientation() {
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    LibretroBackend be;
    be.prepareForStart();
    QString err;
    QVERIFY2(be.loadCore(QStringLiteral(FB_FAKE_HW_CORE_PATH), &err), qPrintable(err));
    QVERIFY2(be.loadGame(rom.fileName(), &err), qPrintable(err));
    QCOMPARE(count(QStringLiteral("accepted")), 1);
    QCOMPARE(count(QStringLiteral("reset")), 1);
    QCOMPARE(count(QStringLiteral("destroy")), 0);
    {  // diagnostics: hardware path active, GPU strings from the live context, no fallback reason
      const RenderInfo ri = be.renderInfo();
      QVERIFY(ri.hwRequested && ri.hwActive);
      QVERIFY(ri.fallbackReason.isEmpty());
      QVERIFY2(ri.api.startsWith(QStringLiteral("OpenGL 3.")) || ri.api.startsWith(QStringLiteral("OpenGL 4.")), qPrintable(ri.api));
      QVERIFY(!ri.gpu.isEmpty());
    }
    {
      const QStringList ver = events().filter(QStringLiteral("reset ")).value(0).mid(6).split(QLatin1Char('.'));
      QVERIFY(ver.size() == 2 && ver.at(0).toInt() * 10 + ver.at(1).toInt() >= 33);  // core asked for 3.3
    }
    QVERIFY(be.runFrame());
    QCOMPARE(count(QStringLiteral("frame without fbo")), 0);
    const QImage f = be.videoFrame();
    QCOMPARE(f.size(), QSize(64, 48));
    QCOMPARE(f.format(), QImage::Format_RGB32);
    QCOMPARE(be.frameCount(), quint64(1));
    QCOMPARE(px(f, 2, 45), qRgb(255, 0, 0));    // GL bottom-left red -> bottom-left of the image
    QCOMPARE(px(f, 61, 2), qRgb(0, 255, 0));    // GL top-right green -> top-right of the image
    QCOMPARE(px(f, 2, 2), qRgb(0, 0, 255));     // top-left stays blue
    QCOMPARE(px(f, 61, 45), qRgb(0, 0, 255));   // bottom-right stays blue
    QCOMPARE(px(f, 32, 24), qRgb(0, 0, 255));
    QCOMPARE(qAlpha(px(f, 0, 0)), 255);
    QVERIFY(be.lastReadbackMs() >= 0.0 && be.lastReadbackMs() < 1000.0);  // timed around the GPU readback
    be.unloadGame();
    QCOMPARE(count(QStringLiteral("destroy")), 1);  // context_destroy before retro_unload_game
    be.unloadCore();
  }

  void topLeftOriginIsNotFlipped() {
    qputenv("FB_FAKE_HW_BOTTOM_LEFT", "0");
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    LibretroBackend be;
    be.prepareForStart();
    QString err;
    QVERIFY2(be.loadCore(QStringLiteral(FB_FAKE_HW_CORE_PATH), &err), qPrintable(err));
    QVERIFY2(be.loadGame(rom.fileName(), &err), qPrintable(err));
    QVERIFY(be.runFrame());
    const QImage f = be.videoFrame();
    qunsetenv("FB_FAKE_HW_BOTTOM_LEFT");
    QCOMPARE(px(f, 2, 2), qRgb(255, 0, 0));
    QCOMPARE(px(f, 61, 45), qRgb(0, 255, 0));
  }

  void disabledByEnvironment() {
    qputenv("UZUME_DISABLE_HW_RENDER", "1");
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    LibretroBackend be;
    be.prepareForStart();
    QString err;
    QVERIFY2(be.loadCore(QStringLiteral(FB_FAKE_HW_CORE_PATH), &err), qPrintable(err));
    QVERIFY(!be.loadGame(rom.fileName(), &err));  // the fake core needs HW rendering and gives up
    QCOMPARE(count(QStringLiteral("rejected")), 1);
    QCOMPARE(count(QStringLiteral("reset")), 0);
    {  // diagnostics: requested but software, with the reason (0.6 D10)
      const RenderInfo ri = be.renderInfo();
      QVERIFY(ri.hwRequested && !ri.hwActive);
      QCOMPARE(ri.fallbackReason, QString::fromLatin1(kFallbackDisabled));
      QVERIFY(ri.api.isEmpty() && ri.gpu.isEmpty());
    }
    qunsetenv("UZUME_DISABLE_HW_RENDER");
  }

  void glDescriptionHelpers() {
    using uzume::emu::HwRenderContext;
    QCOMPARE(HwRenderContext::describeApi(QStringLiteral("4.6.0 NVIDIA 555.42"), true), QStringLiteral("OpenGL 4.6 Core"));
    QCOMPARE(HwRenderContext::describeApi(QStringLiteral("3.3 (Compatibility Profile) Mesa 23.2.1"), false), QStringLiteral("OpenGL 3.3"));
    QCOMPARE(HwRenderContext::describeApi(QString(), true), QStringLiteral("OpenGL Core"));
    QCOMPARE(HwRenderContext::describeGpu(QStringLiteral("Example GPU"), QStringLiteral("4.6.0 NVIDIA 555.42")),
             QStringLiteral("Example GPU · Driver NVIDIA 555.42"));
    QCOMPARE(HwRenderContext::describeGpu(QStringLiteral("llvmpipe (LLVM 15.0.7, 256 bits)"), QStringLiteral("4.5 (Core Profile) Mesa 23.2.1")),
             QStringLiteral("llvmpipe (LLVM 15.0.7, 256 bits) · Driver Mesa 23.2.1"));
    QCOMPARE(HwRenderContext::describeGpu(QString(), QStringLiteral("4.1")), QStringLiteral("GPU"));
  }

  void otherApisAreRejected() {
    qputenv("FB_FAKE_HW_CTX", "7");  // D3D11: Vulkan is supported since ADR 0034 (vk_render_test), D3D stays unsupported
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    LibretroBackend be;
    be.prepareForStart();
    QString err;
    QVERIFY2(be.loadCore(QStringLiteral(FB_FAKE_HW_CORE_PATH), &err), qPrintable(err));
    QVERIFY(!be.loadGame(rom.fileName(), &err));
    QCOMPARE(count(QStringLiteral("rejected")), 1);
    QCOMPARE(be.renderInfo().fallbackReason, QString::fromLatin1(kFallbackUnsupported));
    QVERIFY(be.renderInfo().hwRequested && !be.renderInfo().hwActive);
    qunsetenv("FB_FAKE_HW_CTX");
  }

  void unavailableVersionIsRejected() {
    qputenv("FB_FAKE_HW_MAJOR", "9");
    qputenv("FB_FAKE_HW_MINOR", "9");
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    LibretroBackend be;
    be.prepareForStart();
    QString err;
    QVERIFY2(be.loadCore(QStringLiteral(FB_FAKE_HW_CORE_PATH), &err), qPrintable(err));
    QVERIFY(!be.loadGame(rom.fileName(), &err));
    QCOMPARE(count(QStringLiteral("rejected")), 1);
    qunsetenv("FB_FAKE_HW_MAJOR");
    qunsetenv("FB_FAKE_HW_MINOR");
  }

  // Loads the fake HW core into `be`; the ROM file is a dummy.
  void loadFake(LibretroBackend& be, QTemporaryFile& rom) {
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    be.prepareForStart();
    QString err;
    QVERIFY2(be.loadCore(QStringLiteral(FB_FAKE_HW_CORE_PATH), &err), qPrintable(err));
    QVERIFY2(be.loadGame(rom.fileName(), &err), qPrintable(err));
  }

  void videoNotWantedSkipsReadback() {
    qputenv("FB_FAKE_HW_LOGAV", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    QCOMPARE(be.hwReadbackCount(), quint64(0));
    be.setVideoWanted(true);
    QVERIFY(be.runFrame());
    QCOMPARE(be.hwReadbackCount(), quint64(1));
    const QImage shown = be.videoFrame();
    QVERIFY(!shown.isNull());
    for (int i = 0; i < 3; ++i) {
      be.setVideoWanted(false);
      QVERIFY(be.runFrame());
    }
    QCOMPARE(be.hwReadbackCount(), quint64(1));  // no readback for frames nobody sees
    QCOMPARE(be.frameCount(), quint64(4));       // but they are still emulated
    QCOMPARE(be.videoFrame(), shown);            // the last frame stays
    be.setVideoWanted(true);
    QVERIFY(be.runFrame());
    QCOMPARE(be.hwReadbackCount(), quint64(2));
    // GET_AUDIO_VIDEO_ENABLE reached the core with the right bits: 3 = video + audio, 2 = audio only.
    const QStringList av = events().filter(QStringLiteral("av "));
    QCOMPARE(av, (QStringList{QStringLiteral("av 3"), QStringLiteral("av 2"), QStringLiteral("av 2"), QStringLiteral("av 2"),
                              QStringLiteral("av 3")}));
    qunsetenv("FB_FAKE_HW_LOGAV");
  }

  // Red value of the fake core's frame counter (GL (28..36, 20..28) -> image row 23).
  static int counterOf(const QImage& img) { return qRed(px(img, 32, 23)); }

  void pboReadbackHasOneFrameLatency() {
    qputenv("FB_FAKE_HW_COUNTER", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    QVERIFY(be.runFrame());
    QCOMPARE(counterOf(be.videoFrame()), 1);           // the first frame is valid at once
    QCOMPARE(px(be.videoFrame(), 2, 45), qRgb(255, 0, 0));
    for (int k = 2; k <= 8; ++k) {
      QVERIFY(be.runFrame());
      const QImage f = be.videoFrame();
      QCOMPARE(f.size(), QSize(64, 48));
      QCOMPARE(counterOf(f), k - 1);                    // asynchronous: the previous frame
      QCOMPARE(px(f, 2, 45), qRgb(255, 0, 0));          // orientation and pixels intact
      QCOMPARE(px(f, 61, 2), qRgb(0, 255, 0));
    }
    qunsetenv("FB_FAKE_HW_COUNTER");
  }

  void syncFallbackIsCurrentFrame() {
    qputenv("FB_FAKE_HW_COUNTER", "1");
    qputenv("UZUME_SYNC_READBACK", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    for (int k = 1; k <= 4; ++k) {
      QVERIFY(be.runFrame());
      QCOMPARE(counterOf(be.videoFrame()), k);
      QCOMPARE(px(be.videoFrame(), 2, 45), qRgb(255, 0, 0));
    }
    qunsetenv("UZUME_SYNC_READBACK");
    qunsetenv("FB_FAKE_HW_COUNTER");
  }

  void readbackTimingSyncVsPbo() {  // informational measurement (llvmpipe): ms per readback on the emulation thread
    QTemporaryFile rom;
    double ms[2] = {0, 0};
    for (int mode = 0; mode < 2; ++mode) {
      if (mode == 1) qputenv("UZUME_SYNC_READBACK", "1");
      LibretroBackend be;
      QTemporaryFile r;
      loadFake(be, r);
      QVERIFY(be.runFrame());
      double sum = 0;
      for (int i = 0; i < 100; ++i) {
        QVERIFY(be.runFrame());
        sum += be.lastReadbackMs();
      }
      ms[mode] = sum / 100;
      qunsetenv("UZUME_SYNC_READBACK");
    }
    qInfo("readback avg: PBO %.3f ms, sync %.3f ms (64x48)", ms[0], ms[1]);
  }

  void runnerRequestsVideoOnlyAtBaseRateWhileFast() {
    qputenv("FB_FAKE_HW_LOGAV", "1");
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    auto owned = std::make_unique<LibretroBackend>();
    LibretroBackend* be = owned.get();
    EmulationRunner runner(std::move(owned));
    std::atomic<int> ui{0};
    QObject::connect(&runner, &EmulationRunner::frameReady, &runner, [&ui] { ++ui; }, Qt::DirectConnection);
    EmulationRunner::StartRequest req;
    req.corePath = QStringLiteral(FB_FAKE_HW_CORE_PATH);
    req.gamePath = rom.fileName();
    req.speedUpRatio = 4.0;
    QSignalSpy started(&runner, &EmulationRunner::started);
    runner.start(req);
    QTRY_VERIFY2(started.count() == 1, "not started");
    QTest::qWait(300);
    // Normal speed: every frame is wanted.
    const quint64 rb0 = be->hwReadbackCount();
    const quint64 fr0 = be->frameCount();
    QTest::qWait(500);
    const quint64 rbNormal = be->hwReadbackCount() - rb0;
    const quint64 frNormal = be->frameCount() - fr0;
    QVERIFY2(frNormal > 15, qPrintable(QString::number(frNormal)));
    QVERIFY2(rbNormal >= frNormal - 2, qPrintable(QStringLiteral("normal: %1 readbacks for %2 frames").arg(rbNormal).arg(frNormal)));

    runner.setFastForward(true);
    QTest::qWait(300);
    const quint64 rb1 = be->hwReadbackCount();
    const quint64 fr1 = be->frameCount();
    const int ui1 = ui.load();
    QElapsedTimer t;
    t.start();
    QTest::qWait(1500);
    const double secs = t.elapsed() / 1000.0;
    const quint64 rbFast = be->hwReadbackCount() - rb1;
    const quint64 frFast = be->frameCount() - fr1;
    const int uiFast = ui.load() - ui1;
    qInfo("4x: %.1f emu frames/s, %.1f readbacks/s, %.1f UI frames/s", frFast / secs, rbFast / secs, uiFast / secs);
    QVERIFY2(frFast / secs > 60 * 2.0, qPrintable(QStringLiteral("emu %1/s").arg(frFast / secs)));  // actually faster
    QVERIFY2(rbFast / secs < 60 * 1.3, qPrintable(QStringLiteral("readbacks %1/s").arg(rbFast / secs)));  // ~base fps
    QVERIFY2(rbFast / secs > 60 * 0.5, qPrintable(QStringLiteral("readbacks %1/s").arg(rbFast / secs)));
    QVERIFY2(rbFast <= static_cast<quint64>(uiFast) + 3, "readback without a shown frame");
    runner.stop();
    // The core saw video on/off: both answers occurred while fast.
    QVERIFY(count(QStringLiteral("av 2")) > 0);
    QVERIFY(count(QStringLiteral("av 3")) > 0);
    qunsetenv("FB_FAKE_HW_LOGAV");
  }

  // ---- readback size limit (GPU downscale before readback)

  // Runs `n` frames and returns the last video frame (the PBO path lags one frame behind a limit change).
  static QImage runFrames(LibretroBackend& be, int n) {
    for (int i = 0; i < n; ++i)
      if (!be.runFrame()) return {};
    return be.videoFrame();
  }

  void scaledReadbackSizeRules() {
    QCOMPARE(HwRenderContext::scaledReadbackSize(2048, 3072, {}), QSize(2048, 3072));
    QCOMPARE(HwRenderContext::scaledReadbackSize(2048, 3072, QSize(0, 0)), QSize(2048, 3072));
    QCOMPARE(HwRenderContext::scaledReadbackSize(2048, 3072, QSize(4096, 4096)), QSize(2048, 3072));  // never upscale
    QCOMPARE(HwRenderContext::scaledReadbackSize(2048, 3072, QSize(2048, 3072)), QSize(2048, 3072));
    QCOMPARE(HwRenderContext::scaledReadbackSize(2048, 3072, QSize(1312, 1968)), QSize(1312, 1968));
    QCOMPARE(HwRenderContext::scaledReadbackSize(2048, 3072, QSize(1312, 5000)), QSize(1312, 1968));  // width limits
    QCOMPARE(HwRenderContext::scaledReadbackSize(2048, 3072, QSize(5000, 1000)), QSize(667, 1000));   // height limits
    QCOMPARE(HwRenderContext::scaledReadbackSize(256, 192, QSize(1, 1)), QSize(1, 1));
  }

  void stripesAverageToGrey_data() {
    QTest::addColumn<int>("stripes");
    QTest::addColumn<bool>("bottomLeft");
    QTest::newRow("vertical, bottom-left") << 1 << true;
    QTest::newRow("vertical, top-left") << 1 << false;
    QTest::newRow("horizontal, bottom-left") << 2 << true;
    QTest::newRow("horizontal, top-left") << 2 << false;
  }

  // 1-pixel black/white stripes downscaled 4:1 must come out as uniform mid-grey (box filter), not aliased.
  void stripesAverageToGrey() {
    QFETCH(int, stripes);
    QFETCH(bool, bottomLeft);
    qputenv("FB_FAKE_HW_SCALE", "4");  // 256x192
    qputenv("FB_FAKE_HW_STRIPES", QByteArray::number(stripes));
    qputenv("FB_FAKE_HW_BOTTOM_LEFT", bottomLeft ? "1" : "0");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    be.setReadbackLimit(QSize(64, 48));  // 4:1 -> 64x48
    const QImage f = runFrames(be, 4);
    QCOMPARE(f.size(), QSize(64, 48));
    for (int y = 0; y < f.height(); ++y)
      for (int x = 0; x < f.width(); ++x) {
        const int g = qRed(px(f, x, y));
        QVERIFY2(g >= 126 && g <= 129, qPrintable(QStringLiteral("(%1,%2) = %3").arg(x).arg(y).arg(g)));
      }
    qunsetenv("FB_FAKE_HW_SCALE");
    qunsetenv("FB_FAKE_HW_STRIPES");
    qunsetenv("FB_FAKE_HW_BOTTOM_LEFT");
  }

  // 1-pixel stripes through a halving plus a final non-2:1 blit (256x192 -> 100x75): smoothed, never black/white.
  void stripesStayFilteredOnNonPowerOfTwoTarget() {
    qputenv("FB_FAKE_HW_SCALE", "4");
    qputenv("FB_FAKE_HW_STRIPES", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    be.setReadbackLimit(QSize(100, 75));
    const QImage f = runFrames(be, 4);
    QCOMPARE(f.size(), QSize(100, 75));
    for (int y = 0; y < f.height(); ++y)
      for (int x = 0; x < f.width(); ++x) {
        const int g = qRed(px(f, x, y));
        QVERIFY2(g >= 100 && g <= 155, qPrintable(QStringLiteral("(%1,%2) = %3").arg(x).arg(y).arg(g)));
      }
    qunsetenv("FB_FAKE_HW_SCALE");
    qunsetenv("FB_FAKE_HW_STRIPES");
  }

  void checkPattern(const QImage& f, int w, int h) {  // red bottom-left, green top-right, blue elsewhere
    QCOMPARE(f.size(), QSize(w, h));
    QCOMPARE(f.format(), QImage::Format_RGB32);
    const int m = std::max(2, w / 32);  // well inside the squares (w/8 wide)
    QCOMPARE(px(f, m, h - 1 - m), qRgb(255, 0, 0));
    QCOMPARE(px(f, w - 1 - m, m), qRgb(0, 255, 0));
    QCOMPARE(px(f, m, m), qRgb(0, 0, 255));
    QCOMPARE(px(f, w - 1 - m, h - 1 - m), qRgb(0, 0, 255));
    QCOMPARE(px(f, w / 2, h / 2), qRgb(0, 0, 255));
    QCOMPARE(qAlpha(px(f, 0, 0)), 255);
  }

  void limitedReadbackKeepsAspectOrientationAndColors_data() {
    QTest::addColumn<bool>("bottomLeft");
    QTest::addColumn<bool>("syncReadback");
    QTest::newRow("bottom-left, PBO") << true << false;
    QTest::newRow("top-left, PBO") << false << false;
    QTest::newRow("bottom-left, sync") << true << true;
  }

  void limitedReadbackKeepsAspectOrientationAndColors() {
    QFETCH(bool, bottomLeft);
    QFETCH(bool, syncReadback);
    qputenv("FB_FAKE_HW_SCALE", "4");  // 256x192 frame, the core leaves GL_SCISSOR_TEST enabled
    qputenv("FB_FAKE_HW_BOTTOM_LEFT", bottomLeft ? "1" : "0");
    if (syncReadback) qputenv("UZUME_SYNC_READBACK", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    QVERIFY(be.sourceFrameSize().isEmpty());
    be.setReadbackLimit(QSize(128, 96));
    // 256x192 -> one halving: 128x96 (aspect 4:3 kept)
    const QImage f = runFrames(be, 4);
    if (bottomLeft) checkPattern(f, 128, 96);
    else {  // top-left origin: the pattern is not flipped, so red is top-left and green bottom-right
      QCOMPARE(f.size(), QSize(128, 96));
      QCOMPARE(px(f, 4, 4), qRgb(255, 0, 0));
      QCOMPARE(px(f, 123, 91), qRgb(0, 255, 0));
      QCOMPARE(px(f, 4, 91), qRgb(0, 0, 255));
    }
    QCOMPARE(be.sourceFrameSize(), QSize(256, 192));  // diagnostics still know the real frame size
    qunsetenv("FB_FAKE_HW_SCALE");
    qunsetenv("FB_FAKE_HW_BOTTOM_LEFT");
    qunsetenv("UZUME_SYNC_READBACK");
  }

  void limitClearedAndNeverUpscaled() {
    qputenv("FB_FAKE_HW_SCALE", "4");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    QCOMPARE(runFrames(be, 3).size(), QSize(256, 192));  // no limit: unchanged
    be.setReadbackLimit(QSize(1000, 1000));
    QCOMPARE(runFrames(be, 3).size(), QSize(256, 192));  // limit above the frame: never upscaled
    be.setReadbackLimit(QSize(256, 192));
    checkPattern(runFrames(be, 3), 256, 192);
    qunsetenv("FB_FAKE_HW_SCALE");
  }

  void limitChangesMidRun() {
    qputenv("FB_FAKE_HW_SCALE", "4");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    checkPattern(runFrames(be, 3), 256, 192);
    be.setReadbackLimit(QSize(128, 96));
    checkPattern(runFrames(be, 3), 128, 96);
    be.setReadbackLimit(QSize(64, 48));  // two halvings
    checkPattern(runFrames(be, 3), 64, 48);
    be.setReadbackLimit(QSize(64, 64));  // width limits: 64x48
    checkPattern(runFrames(be, 3), 64, 48);
    be.setReadbackLimit(QSize(200, 100));  // height limits: 133x100 (one non-2:1 blit)
    const QImage f = runFrames(be, 3);
    QCOMPARE(f.size(), QSize(133, 100));
    QCOMPARE(px(f, 3, 96), qRgb(255, 0, 0));
    QCOMPARE(px(f, 129, 3), qRgb(0, 255, 0));
    be.setReadbackLimit(QSize());  // cleared: full size again
    checkPattern(runFrames(be, 3), 256, 192);
    qunsetenv("FB_FAKE_HW_SCALE");
  }

  void skippedFramesReportNoReadbackTime() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    be.setVideoWanted(true);
    QVERIFY(be.runFrame());
    QVERIFY(be.lastReadbackMs() > 0.0);
    be.setVideoWanted(false);
    QVERIFY(be.runFrame());
    QCOMPARE(be.lastReadbackMs(), 0.0);  // not the stale value of the previous frame
  }

  void readbackTimingFullVsLimited() {  // informational (llvmpipe): ms per readback of a large frame
    qputenv("FB_FAKE_HW_SCALE", "32");  // 2048x1536
    const QSize limits[] = {QSize(), QSize(1312, 984), QSize(656, 492)};
    for (const QSize& lim : limits) {
      QTemporaryFile rom;
      LibretroBackend be;
      loadFake(be, rom);
      be.setReadbackLimit(lim);
      runFrames(be, 3);
      double sum = 0;
      for (int i = 0; i < 20; ++i) {
        QVERIFY(be.runFrame());
        sum += be.lastReadbackMs();
      }
      qInfo("readback 2048x1536 -> %dx%d: %.2f ms avg", be.videoFrame().width(), be.videoFrame().height(), sum / 20);
    }
    qunsetenv("FB_FAKE_HW_SCALE");
  }

  void runnerAppliesReadbackLimit() {
    qputenv("FB_FAKE_HW_SCALE", "4");
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QSignalSpy frames(&runner, &EmulationRunner::frameReady);
    QSignalSpy failed(&runner, &EmulationRunner::startFailed);
    runner.setReadbackLimit(QSize(128, 96));  // before start: applies from the first frame
    EmulationRunner::StartRequest req;
    req.corePath = QStringLiteral(FB_FAKE_HW_CORE_PATH);
    req.gamePath = rom.fileName();
    runner.start(req);
    QTRY_VERIFY2(frames.count() >= 3 || failed.count() > 0, "no frames");
    QCOMPARE(failed.count(), 0);
    QCOMPARE(frames.last().at(0).value<QImage>().size(), QSize(128, 96));
    QCOMPARE(runner.sourceFrameSize(), QSize(256, 192));
    runner.setReadbackLimit(QSize());  // thread-safe change while running
    const int before = frames.count();
    QTRY_VERIFY(frames.count() >= before + 4);
    QCOMPARE(frames.last().at(0).value<QImage>().size(), QSize(256, 192));
    runner.stop();
    qunsetenv("FB_FAKE_HW_SCALE");
  }

  void runnerUsesEmulationThread() {
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    EmulationRunner runner(std::make_unique<LibretroBackend>());
    QSignalSpy frames(&runner, &EmulationRunner::frameReady);
    QSignalSpy failed(&runner, &EmulationRunner::startFailed);
    EmulationRunner::StartRequest req;
    req.corePath = QStringLiteral(FB_FAKE_HW_CORE_PATH);
    req.gamePath = rom.fileName();
    runner.start(req);
    QTRY_VERIFY2(frames.count() >= 3 || failed.count() > 0, "no frames");
    QCOMPARE(failed.count(), 0);
    const QImage f = frames.last().at(0).value<QImage>();
    QCOMPARE(f.size(), QSize(64, 48));
    QCOMPARE(px(f, 2, 45), qRgb(255, 0, 0));
    runner.stop();
    QCOMPARE(count(QStringLiteral("reset")), 1);
    QCOMPARE(count(QStringLiteral("destroy")), 1);
  }

  // ---- GL state left to the core

  // Integer values of the fake core's log lines "<key> <n>", in order.
  QList<int> logValues(const QString& key) {
    QList<int> out;
    for (const QString& e : events())
      if (e.startsWith(key + QLatin1Char(' '))) out.append(e.mid(key.size() + 1).toInt());
    return out;
  }

  void readAndDrawBindingsAreRestoredSeparately_data() {
    QTest::addColumn<QSize>("limit");
    QTest::newRow("full size readback") << QSize();
    QTest::newRow("GPU downscale") << QSize(32, 24);
  }

  // The fake core leaves GL_READ_FRAMEBUFFER at 0 and GL_DRAW_FRAMEBUFFER on its FBO. After the readback it must
  // find exactly that again (the guard used to restore both bindings from the draw binding).
  void readAndDrawBindingsAreRestoredSeparately() {
    QFETCH(QSize, limit);
    qputenv("FB_FAKE_HW_READFBO", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    be.setReadbackLimit(limit);
    QVERIFY(!runFrames(be, 5).isNull());
    qunsetenv("FB_FAKE_HW_READFBO");
    const QList<int> readFbo = logValues(QStringLiteral("readfbo")), drawFbo = logValues(QStringLiteral("drawfbo"));
    const QList<int> scissor = logValues(QStringLiteral("scissor")), fbo = logValues(QStringLiteral("fbo"));
    QCOMPARE(readFbo.size(), 5);
    QCOMPARE(drawFbo.size(), 5);
    QCOMPARE(scissor.size(), 5);
    QCOMPARE(fbo.size(), 5);
    QVERIFY(fbo.at(0) != 0);
    for (int k = 1; k < 5; ++k) {  // the state at the start of frame k is what frame k-1 and the readback left
      QCOMPARE(readFbo.at(k), 0);
      QCOMPARE(drawFbo.at(k), fbo.at(k - 1));
      QCOMPARE(scissor.at(k), 1);  // the core leaves GL_SCISSOR_TEST on
    }
  }

  // ---- Session encode texture (GpuEncodeTarget, ADR 0019)

  // Red value of the fake core's frame counter in a capture (GL (28..36, 20..28) -> image row 23).
  static int captureCounter(const QImage& img) { return qRed(px(img, 32, 23)); }
  // A capture as the RGB32 image checkPattern() understands (the encode texture is RGBA).
  static QImage asRgb32(const QImage& img) { return img.convertToFormat(QImage::Format_RGB32); }

  void encodeSizeForRules() {
    const QSize full(1280, 1920);
    QCOMPARE(HwRenderContext::encodeSizeFor(2048, 3072, full), QSize(1280, 1920));  // DS at 8x
    QCOMPARE(HwRenderContext::encodeSizeFor(1024, 1536, full), QSize(1024, 1536));  // DS at 4x: unchanged
    QCOMPARE(HwRenderContext::encodeSizeFor(256, 384, full), QSize(256, 384));      // DS at 1x: never upscaled
    QCOMPARE(HwRenderContext::encodeSizeFor(255, 383, full), QSize(254, 382));      // odd: rounded down to even
    QCOMPARE(HwRenderContext::encodeSizeFor(255, 383, {}), QSize(254, 382));        // no limit: the frame, even
    QCOMPARE(HwRenderContext::encodeSizeFor(256, 192, {}), QSize(256, 192));
    QCOMPARE(HwRenderContext::encodeSizeFor(256, 192, QSize(100, 100)), QSize(100, 74));  // 100 x 75 -> even
    QCOMPARE(HwRenderContext::encodeSizeFor(2048, 3072, QSize(1312, 5000)), QSize(1312, 1968));
    QCOMPARE(HwRenderContext::encodeSizeFor(1, 1, full), QSize(2, 2));  // at least 2 x 2
    QCOMPARE(HwRenderContext::encodeSizeFor(3, 3, full), QSize(2, 2));
    QCOMPARE(HwRenderContext::encodeSizeFor(64, 48, QSize(1, 1)), QSize(2, 2));
    const QSize s = HwRenderContext::encodeSizeFor(2048, 3072, QSize(1000, 1000));  // aspect kept: 2:3
    QCOMPARE(s.width() % 2 + s.height() % 2, 0);
    QVERIFY(qAbs(static_cast<double>(s.width()) / s.height() - 2.0 / 3.0) < 0.01);
    QVERIFY(s.width() <= 1000 && s.height() <= 1000);
  }

  void encodeTextureIsTopDownRgb() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    QCOMPARE(be.lastGpuCopyMs(), 0.0);  // no target: no GPU copy time
    QVERIFY(!be.lastGpuCaptured());
    be.setGpuEncodeTarget(t);
    QTest::ignoreMessage(QtInfoMsg, "Session encode texture 64x48 for a 64x48 frame");
    QVERIFY(be.runFrame());  // frame 1 is blitted, nothing to capture yet
    QCOMPARE(t->names(), (QStringList{QStringLiteral("attach")}));
    QVERIFY(!be.lastGpuCaptured());
    QVERIFY(be.runFrame());  // frame 2 starts by capturing frame 1
    QCOMPARE(t->names(), (QStringList{QStringLiteral("attach"), QStringLiteral("capture")}));
    QVERIFY2(t->violations().isEmpty(), qPrintable(t->violations().join(QLatin1Char(';'))));
    const FakeTarget::Call attach = t->calls().at(0);
    QCOMPARE(attach.size, QSize(64, 48));
    QCOMPARE(attach.internalFormat, kRgba8);  // sized GL_RGBA8
    QVERIFY(attach.tex != 0);
    QCOMPARE(t->images().size(), 1);
    const QImage img = t->images().at(0);
    QCOMPARE(img.size(), QSize(64, 48));
    // Row 0 is the top of the image: green top-right, red bottom-left (the core draws with a bottom-left origin).
    QCOMPARE(px(img, 61, 2), qRgb(0, 255, 0));
    QCOMPARE(px(img, 2, 45), qRgb(255, 0, 0));
    QCOMPARE(px(img, 2, 2), qRgb(0, 0, 255));
    QCOMPARE(px(img, 61, 45), qRgb(0, 0, 255));
    const uchar* bottom = img.constScanLine(45);  // bytes R, G, B (red = FF 00 00)
    QCOMPARE(int(bottom[2 * 4 + 0]), 0xFF);
    QCOMPARE(int(bottom[2 * 4 + 1]), 0x00);
    QCOMPARE(int(bottom[2 * 4 + 2]), 0x00);
    const uchar* top = img.constScanLine(2);
    QCOMPARE(int(top[61 * 4 + 0]), 0x00);
    QCOMPARE(int(top[61 * 4 + 1]), 0xFF);
    QCOMPARE(int(top[61 * 4 + 2]), 0x00);
    checkPattern(asRgb32(img), 64, 48);
    checkPattern(be.videoFrame(), 64, 48);  // the display readback is unaffected
  }

  void encodeTextureOfTopLeftCoreIsNotFlipped() {
    qputenv("FB_FAKE_HW_BOTTOM_LEFT", "0");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    qunsetenv("FB_FAKE_HW_BOTTOM_LEFT");
    auto t = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t);
    runFrames(be, 2);
    QCOMPARE(t->images().size(), 1);
    const QImage img = t->images().at(0);
    QCOMPARE(px(img, 2, 2), qRgb(255, 0, 0));     // GL row 0 is already the top row
    QCOMPARE(px(img, 61, 45), qRgb(0, 255, 0));
    QCOMPARE(px(img, 2, 45), qRgb(0, 0, 255));
  }

  void encodeTextureIsScaledIntoTheLimitAndEven() {
    qputenv("FB_FAKE_HW_SCALE", "4");  // 256x192; the core leaves GL_SCISSOR_TEST enabled
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    t->maxW = 100;
    t->maxH = 100;
    be.setGpuEncodeTarget(t);
    QTest::ignoreMessage(QtInfoMsg, "Session encode texture 100x74 for a 256x192 frame");
    runFrames(be, 3);
    QCOMPARE(t->calls().at(0).size, QSize(100, 74));  // 100 x 75, rounded down to even
    QVERIFY(t->images().size() >= 2);
    for (const QImage& img : t->images()) checkPattern(asRgb32(img), 100, 74);
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    QVERIFY(ctx);
    QVERIFY(ctx->functions()->glIsEnabled(GL_SCISSOR_TEST));  // the blits switched it off and restored it
    QCOMPARE(be.sourceFrameSize(), QSize(256, 192));
    checkPattern(be.videoFrame(), 256, 192);
    qunsetenv("FB_FAKE_HW_SCALE");
  }

  void encodeTextureAveragesStripes() {
    qputenv("FB_FAKE_HW_SCALE", "4");
    qputenv("FB_FAKE_HW_STRIPES", "1");  // 1-pixel black/white columns
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    t->maxW = 100;
    t->maxH = 100;
    be.setGpuEncodeTarget(t);
    runFrames(be, 3);
    QVERIFY(!t->images().isEmpty());
    const QImage img = t->images().last();
    QCOMPARE(img.size(), QSize(100, 74));
    for (int y = 0; y < img.height(); ++y)
      for (int x = 0; x < img.width(); ++x) {
        const int g = qRed(px(img, x, y));
        QVERIFY2(g >= 100 && g <= 155, qPrintable(QStringLiteral("(%1,%2) = %3").arg(x).arg(y).arg(g)));
      }
    qunsetenv("FB_FAKE_HW_SCALE");
    qunsetenv("FB_FAKE_HW_STRIPES");
  }

  void encodeCaptureIsOneFrameBehind() {
    qputenv("FB_FAKE_HW_COUNTER", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t);
    for (int k = 1; k <= 8; ++k) {
      QVERIFY(be.runFrame());
      QCOMPARE(t->images().size(), k - 1);  // no capture in the first runFrame, one in every later one
      QCOMPARE(be.lastGpuCaptured(), k > 1);
      if (k > 1) QCOMPARE(captureCounter(t->images().last()), k - 1);  // the frame of the previous runFrame
    }
    qunsetenv("FB_FAKE_HW_COUNTER");
  }

  void encodeNotWantedDoesNothing() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    t->isWanted = false;
    be.setGpuEncodeTarget(t);
    runFrames(be, 4);
    QVERIFY(t->calls().isEmpty());  // no attach, no capture
    QVERIFY(!be.lastGpuCaptured());
    checkPattern(be.videoFrame(), 64, 48);  // the display is unchanged
    t->isWanted = true;  // wanted again: attaches on the next frame, captures on the one after
    runFrames(be, 1);
    QCOMPARE(t->names(), (QStringList{QStringLiteral("attach")}));
    runFrames(be, 1);
    QCOMPARE(t->count(QStringLiteral("capture")), 1);
    t->isWanted = false;  // a frame that is pending when the target stops being wanted is discarded
    runFrames(be, 3);
    QCOMPARE(t->count(QStringLiteral("capture")), 1);
    QVERIFY(t->violations().isEmpty());
  }

  void encodeNotReadyAsksAgain() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    t->notReady = 2;
    be.setGpuEncodeTarget(t);
    const int expectedCaptures[] = {0, 0, 0, 1, 2};
    for (int k = 1; k <= 5; ++k) {
      QVERIFY(be.runFrame());
      QCOMPARE(t->count(QStringLiteral("capture")), expectedCaptures[k - 1]);
      checkPattern(be.videoFrame(), 64, 48);  // the readback keeps encoding meanwhile
    }
    QCOMPARE(t->count(QStringLiteral("attach (not ready)")), 2);
    QCOMPARE(t->count(QStringLiteral("attach")), 1);  // captures start after the third wanted frame
    QCOMPARE(t->count(QStringLiteral("fail")), 0);
  }

  void encodeFailureDropsTheTargetAndFails_data() {
    QTest::addColumn<bool>("attachFails");
    QTest::addColumn<bool>("attachUnavailable");
    QTest::addColumn<QString>("reason");
    QTest::addColumn<QStringList>("expectedCalls");
    QTest::newRow("capture fails") << false << false << QStringLiteral("capture failed")
                                   << QStringList{QStringLiteral("attach"), QStringLiteral("capture"), QStringLiteral("fail"),
                                                  QStringLiteral("detach")};
    QTest::newRow("attach fails") << true << false << QStringLiteral("attach failed")
                                  << QStringList{QStringLiteral("attach"), QStringLiteral("fail")};  // never attached: no detach
    // Not applicable (e.g. Optimus: the game's GL context runs on the iGPU): the target is released quietly, without a
    // fail() call (it reports Unavailable itself) and without the warning.
    QTest::newRow("attach unavailable") << false << true << QString() << QStringList{QStringLiteral("attach")};
  }

  void encodeFailureDropsTheTargetAndFails() {
    QFETCH(bool, attachFails);
    QFETCH(bool, attachUnavailable);
    QFETCH(QString, reason);
    QFETCH(QStringList, expectedCalls);
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    t->attachFails = attachFails;
    t->attachUnavailable = attachUnavailable;
    t->failCaptureAt = (attachFails || attachUnavailable) ? 0 : 1;
    be.setGpuEncodeTarget(t);
    if (attachUnavailable) {
      QTest::failOnWarning(QRegularExpression(QStringLiteral("Session encode target")));  // quiet: at most an info line
    } else {
      QTest::ignoreMessage(QtWarningMsg, qPrintable(QStringLiteral("Session encode target dropped (%1); the Session encoder gets "
                                                                   "readback frames").arg(reason)));
    }
    runFrames(be, 3);
    QCOMPARE(t->names(), expectedCalls);
    const QList<FakeTarget::Call> calls = t->calls();
    if (!attachUnavailable) {
      QCOMPARE(calls.at(expectedCalls.indexOf(QStringLiteral("fail"))).reason, reason);  // fail() first ...
    }
    if (!attachFails && !attachUnavailable) {  // ... then detach, GL current
      QVERIFY(calls.last().glCurrentArg);
      QVERIFY(calls.last().isTexture);  // detach comes before the texture is deleted
      QCOMPARE(calls.last().internalFormat, kRgba8);
    }
    const quint64 frames = be.frameCount();
    QCOMPARE(runFrames(be, 4).size(), QSize(64, 48));  // the Session keeps running ...
    QCOMPARE(be.frameCount(), frames + 4);
    checkPattern(be.videoFrame(), 64, 48);              // ... with display frames,
    QCOMPARE(t->names(), expectedCalls);                // and the target is not called any more
    QVERIFY(t->violations().isEmpty());
    QCOMPARE(t.use_count(), 1);                         // the dropped target is released (only the test holds it)
  }

  void encodeSizeChangeDetachesBeforeTheNewTexture() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t);
    runFrames(be, 3);
    QCOMPARE(t->calls().at(0).size, QSize(64, 48));
    t->maxW = 32;  // a smaller limit -> a new encode texture
    t->maxH = 24;
    runFrames(be, 3);
    // Frame 4 still captures the 64x48 frame 3 through the old registration; its own blit then detaches and re-attaches.
    QCOMPARE(t->names(), (QStringList{QStringLiteral("attach"), QStringLiteral("capture"), QStringLiteral("capture"),
                                      QStringLiteral("capture"), QStringLiteral("detach"), QStringLiteral("attach"),
                                      QStringLiteral("capture"), QStringLiteral("capture")}));
    const QList<FakeTarget::Call> calls = t->calls();
    const FakeTarget::Call detach = calls.at(4), attach = calls.at(5);
    QVERIFY(detach.glCurrentArg);
    QVERIFY(detach.isTexture);  // the old texture still exists while it is detached ...
    QCOMPARE(detach.internalFormat, kRgba8);
    QCOMPARE(detach.tex, calls.at(0).tex);
    QCOMPARE(attach.size, QSize(32, 24));
    QVERIFY(attach.tex != detach.tex);  // ... and the new size gets a new texture object, never a re-specified one
    QCOMPARE(attach.internalFormat, kRgba8);
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    QVERIFY(ctx);
    QVERIFY(ctx->functions()->glIsTexture(attach.tex));
    QVERIFY(!ctx->functions()->glIsTexture(detach.tex));  // deleted after the detach
    QCOMPARE(t->images().last().size(), QSize(32, 24));
    checkPattern(asRgb32(t->images().last()), 32, 24);
    QVERIFY(t->violations().isEmpty());
  }

  void encodeTargetLifecycle() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    runFrames(be, 3);
    // A target set mid-run applies at the start of the next runFrame, not before.
    auto t1 = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t1);
    QVERIFY(t1->calls().isEmpty());
    runFrames(be, 2);
    QCOMPARE(t1->names(), (QStringList{QStringLiteral("attach"), QStringLiteral("capture")}));
    // Replaced by another one: the old one is detached (GL current) at the next runFrame, the new one attaches.
    auto t2 = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t2);
    QCOMPARE(t1->names().size(), 2);
    runFrames(be, 1);
    QCOMPARE(t1->names(), (QStringList{QStringLiteral("attach"), QStringLiteral("capture"), QStringLiteral("detach")}));
    QVERIFY(t1->calls().last().glCurrentArg && t1->calls().last().isTexture);
    QCOMPARE(t2->names(), (QStringList{QStringLiteral("attach")}));
    QCOMPARE(t1.use_count(), 1);  // released by the backend
    // Two replacements before the emulation thread looks: only the last one is ever used.
    auto t3 = std::make_shared<FakeTarget>();
    auto t4 = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t3);
    be.setGpuEncodeTarget(t4);
    QCOMPARE(t3.use_count(), 1);
    runFrames(be, 2);
    QVERIFY(t3->calls().isEmpty());
    QCOMPARE(t2->count(QStringLiteral("detach")), 1);
    QCOMPARE(t4->names(), (QStringList{QStringLiteral("attach"), QStringLiteral("capture")}));
    // Removed: detached, nothing more happens.
    be.setGpuEncodeTarget(nullptr);
    runFrames(be, 3);
    QCOMPARE(t4->names(), (QStringList{QStringLiteral("attach"), QStringLiteral("capture"), QStringLiteral("detach")}));
    QCOMPARE(t4.use_count(), 1);
    // Game quit with a target: exactly one detach(true), with GL current, before the texture goes.
    auto t5 = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t5);
    runFrames(be, 3);
    QCOMPARE(t5->count(QStringLiteral("detach")), 0);
    be.unloadGame();
    QCOMPARE(t5->count(QStringLiteral("detach")), 1);
    QVERIFY(t5->calls().last().glCurrentArg && t5->calls().last().isTexture);
    QCOMPARE(t5->calls().last().internalFormat, kRgba8);
    QCOMPARE(t5.use_count(), 1);
    be.unloadCore();
    QCOMPARE(t5->count(QStringLiteral("detach")), 1);
    for (const auto& t : {t1, t2, t3, t4, t5}) QVERIFY2(t->violations().isEmpty(), qPrintable(t->violations().join(QLatin1Char(';'))));
  }

  void encodeDuplicateFramesRepeatTheLastCapture() {
    qputenv("FB_FAKE_HW_COUNTER", "1");
    qputenv("FB_FAKE_HW_DUPE_EVERY", "3");  // frames 3, 6, 9 are duplicates (the core renders nothing)
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t);
    runFrames(be, 9);
    qunsetenv("FB_FAKE_HW_COUNTER");
    qunsetenv("FB_FAKE_HW_DUPE_EVERY");
    const QList<QImage> images = t->images();
    QCOMPARE(images.size(), 8);  // one capture per displayed frame (the first runFrame has none to capture)
    QList<int> counters;
    for (const QImage& img : images) counters.append(captureCounter(img));
    QCOMPARE(counters, (QList<int>{1, 2, 2, 4, 5, 5, 7, 8}));  // a duplicate re-captures the previous frame
    QCOMPARE(images.at(2), images.at(1));
    QCOMPARE(images.at(5), images.at(4));
    QVERIFY(t->violations().isEmpty());
  }

  void encodeFollowsDisplayedFramesWhileSpedUp() {
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    auto owned = std::make_unique<LibretroBackend>();
    LibretroBackend* be = owned.get();
    EmulationRunner runner(std::move(owned));
    auto t = std::make_shared<FakeTarget>();
    t->keepImages = false;
    runner.setGpuEncodeTarget(t);  // before start: applies from the first frame
    EmulationRunner::StartRequest req;
    req.corePath = QStringLiteral(FB_FAKE_HW_CORE_PATH);
    req.gamePath = rom.fileName();
    req.speedUpRatio = 4.0;
    QSignalSpy started(&runner, &EmulationRunner::started);
    runner.start(req);
    QTRY_VERIFY2(started.count() == 1, "not started");
    QTest::qWait(300);
    const int cap0 = t->count(QStringLiteral("capture"));
    const quint64 rb0 = be->hwReadbackCount();
    QTest::qWait(500);
    const int capNormal = t->count(QStringLiteral("capture")) - cap0;
    const quint64 rbNormal = be->hwReadbackCount() - rb0;
    QVERIFY2(capNormal > 15, qPrintable(QString::number(capNormal)));
    QVERIFY2(qAbs(static_cast<qint64>(capNormal) - static_cast<qint64>(rbNormal)) <= 3,
             qPrintable(QStringLiteral("normal: %1 captures for %2 readbacks").arg(capNormal).arg(rbNormal)));

    runner.setFastForward(true);
    QTest::qWait(300);
    const int cap1 = t->count(QStringLiteral("capture"));
    const quint64 rb1 = be->hwReadbackCount();
    const quint64 fr1 = be->frameCount();
    QElapsedTimer timer;
    timer.start();
    QTest::qWait(1500);
    const double secs = timer.elapsed() / 1000.0;
    const double capFast = (t->count(QStringLiteral("capture")) - cap1) / secs;
    const double rbFast = static_cast<double>(be->hwReadbackCount() - rb1) / secs;
    const double frFast = static_cast<double>(be->frameCount() - fr1) / secs;
    qInfo("4x: %.1f emu frames/s, %.1f readbacks/s, %.1f captures/s", frFast, rbFast, capFast);
    QVERIFY2(frFast > 60 * 2.0, qPrintable(QStringLiteral("emu %1/s").arg(frFast)));            // actually faster
    QVERIFY2(capFast < 60 * 1.3, qPrintable(QStringLiteral("captures %1/s").arg(capFast)));      // ~ the base fps
    QVERIFY2(capFast > 60 * 0.5, qPrintable(QStringLiteral("captures %1/s").arg(capFast)));
    QVERIFY2(qAbs(capFast - rbFast) < 60 * 0.15, qPrintable(QStringLiteral("%1 captures/s vs %2 readbacks/s").arg(capFast).arg(rbFast)));
    runner.stop();
    QVERIFY(t->violations().isEmpty());
    QCOMPARE(t->count(QStringLiteral("detach")), 1);  // the game quit detached it
    QCOMPARE(t->count(QStringLiteral("fail")), 0);
  }

  void encodeKeepsTheCoresBindings_data() {
    QTest::addColumn<QSize>("limit");
    QTest::newRow("1:1 copy") << QSize(1280, 1920);
    QTest::newRow("scaled (halving)") << QSize(32, 24);
    QTest::newRow("scaled (final blit only)") << QSize(50, 37);
  }

  // The same state check as readAndDrawBindingsAreRestoredSeparately, now with the encode blit and the capture: read FBO 0,
  // draw FBO = the core's, scissor on, exactly as the core left them.
  void encodeKeepsTheCoresBindings() {
    QFETCH(QSize, limit);
    qputenv("FB_FAKE_HW_READFBO", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    t->maxW = limit.width();
    t->maxH = limit.height();
    be.setGpuEncodeTarget(t);
    QVERIFY(!runFrames(be, 6).isNull());
    qunsetenv("FB_FAKE_HW_READFBO");
    QVERIFY2(t->images().size() == 5, qPrintable(QString::number(t->images().size())));
    const QList<int> readFbo = logValues(QStringLiteral("readfbo")), drawFbo = logValues(QStringLiteral("drawfbo"));
    const QList<int> scissor = logValues(QStringLiteral("scissor")), fbo = logValues(QStringLiteral("fbo"));
    QCOMPARE(readFbo.size(), 6);
    QCOMPARE(fbo.size(), 6);
    for (int k = 1; k < 6; ++k) {
      QCOMPARE(readFbo.at(k), 0);
      QCOMPARE(drawFbo.at(k), fbo.at(k - 1));
      QCOMPARE(scissor.at(k), 1);
    }
    QVERIFY(t->violations().isEmpty());
  }

  void encodeTimingIsReported() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t);
    QVERIFY(be.runFrame());
    QVERIFY(be.lastGpuCopyMs() > 0.0 && be.lastGpuCopyMs() < 1000.0);  // the blit
    QVERIFY(!be.lastGpuCaptured());
    QVERIFY(be.runFrame());
    QVERIFY(be.lastGpuCopyMs() > 0.0 && be.lastGpuCopyMs() < 1000.0);  // capture + blit
    QVERIFY(be.lastGpuCaptured());
    be.setGpuEncodeTarget(nullptr);
    QVERIFY(be.runFrame());
    QCOMPARE(be.lastGpuCopyMs(), 0.0);  // removed: not a stale value
    QVERIFY(!be.lastGpuCaptured());
  }

  void encodeSurvivesAStaleGlErrorFromTheCore_data() {
    QTest::addColumn<bool>("syncReadback");
    QTest::newRow("PBO readback") << false;
    QTest::newRow("sync readback") << true;  // the PBO path drains errors itself; the sync path leaves them to the encode path
  }

  void encodeSurvivesAStaleGlErrorFromTheCore() {
    QFETCH(bool, syncReadback);
    qputenv("FB_FAKE_HW_GLERROR", "1");  // the core leaves GL_INVALID_ENUM pending after every frame
    if (syncReadback) qputenv("UZUME_SYNC_READBACK", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeTarget>();
    be.setGpuEncodeTarget(t);
    runFrames(be, 6);
    qunsetenv("UZUME_SYNC_READBACK");
    qunsetenv("FB_FAKE_HW_GLERROR");
    QCOMPARE(t->count(QStringLiteral("capture")), 5);  // neither the texture creation nor the blit saw the stale error
    QCOMPARE(t->count(QStringLiteral("fail")), 0);
    checkPattern(asRgb32(t->images().last()), 64, 48);
    checkPattern(be.videoFrame(), 64, 48);
  }

  // PE-15: a resize the GL implementation cannot do keeps the old, complete framebuffer; a later resize still works.
  void failedResizeKeepsTheWorkingFramebuffer() {
    HwRenderContext hw;
    hw.prepareSurface();
    QString err;
    QVERIFY2(hw.createContext(true, 3, 3, true, false, &err), qPrintable(err));
    QVERIFY(hw.ensureSize(64, 48));
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    const quintptr before = hw.framebuffer();
    QVERIFY(before != 0);
    QVERIFY(!hw.ensureSize(1 << 20, 1 << 20));  // far above GL_MAX_TEXTURE_SIZE
    QCOMPARE(hw.framebuffer(), before);
    f->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(hw.framebuffer()));
    QCOMPARE(f->glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    f->glClearColor(0.f, 0.f, 1.f, 1.f);
    f->glClear(GL_COLOR_BUFFER_BIT);
    quint8 pixel[4] = {0, 0, 0, 0};
    f->glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    QCOMPARE(int(pixel[2]), 255);
    QVERIFY(hw.ensureSize(128, 96));  // a later, valid resize works
    f->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(hw.framebuffer()));
    QCOMPARE(f->glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  // The public HwRenderContext API without a backend: the "core" is a glClear into the context's FBO.
  void encodeApiOnTheContext() {
    HwRenderContext hw;
    hw.prepareSurface();
    QString err;
    QVERIFY2(hw.createContext(true, 3, 3, false, false, &err), qPrintable(err));
    QVERIFY(hw.ensureSize(64, 48));
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    auto drawColor = [&](float r, float g, float b) {
      f->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(hw.framebuffer()));
      f->glClearColor(r, g, b, 1.f);
      f->glClear(GL_COLOR_BUFFER_BIT);
    };
    QVERIFY(!hw.hasEncodeTarget());
    QVERIFY(hw.encodeSize().isEmpty());
    drawColor(1.f, 0.f, 0.f);
    hw.encodeBlit(64, 48, true);  // no target: nothing happens
    QVERIFY(!hw.captureEncode());
    QVERIFY(hw.encodeSize().isEmpty());

    auto t = std::make_shared<FakeTarget>();
    t->maxW = 32;
    t->maxH = 24;
    hw.setEncodeTarget(t);
    QVERIFY(hw.hasEncodeTarget());
    QVERIFY(!hw.captureEncode());  // nothing blitted yet
    hw.encodeBlit(64, 48, true);
    QCOMPARE(hw.encodeSize(), QSize(32, 24));
    QVERIFY(hw.captureEncode());
    QVERIFY(!hw.captureEncode());  // captured once; the texture is not captured again by itself
    hw.encodeRepeat();             // duplicate frame
    drawColor(0.f, 1.f, 0.f);      // a core drawing the next frame does not touch the captured one
    QVERIFY(hw.captureEncode());
    hw.encodeBlit(64, 48, true);
    QVERIFY(hw.captureEncode());
    QCOMPARE(t->images().size(), 3);
    QCOMPARE(px(t->images().at(0), 16, 12), qRgb(255, 0, 0));
    QCOMPARE(px(t->images().at(1), 16, 12), qRgb(255, 0, 0));  // the repeat shows the old frame
    QCOMPARE(px(t->images().at(2), 16, 12), qRgb(0, 255, 0));
    hw.setEncodeTarget(nullptr);
    QVERIFY(!hw.hasEncodeTarget());
    QVERIFY(hw.encodeSize().isEmpty());
    QCOMPARE(t->count(QStringLiteral("detach")), 1);
    hw.encodeBlit(64, 48, true);  // removed again: nothing happens
    QVERIFY(!hw.captureEncode());
    hw.setEncodeTarget(t);        // a target set again attaches again ...
    hw.encodeBlit(64, 48, true);
    QCOMPARE(t->count(QStringLiteral("attach")), 2);
    hw.destroyContext();          // ... and the context's end detaches it with GL still alive
    QCOMPARE(t->count(QStringLiteral("detach")), 2);
    QVERIFY(t->calls().last().glCurrentArg && t->calls().last().isTexture);
    QVERIFY(!hw.hasEncodeTarget());
    QCOMPARE(t.use_count(), 1);
    QVERIFY(t->violations().isEmpty());
  }

  // ---------------------------------------------------------------- zero-copy display (ADR 0027)

  // The display texture holds the same image as a 1:1 readback: orientation, colours and the top-left placement inside the
  // larger texture. UZUME_SYNC_READBACK makes the readback the current frame, so the whole image can be compared.
  void displayTextureIsTopDownAndMatchesReadback_data() {
    QTest::addColumn<bool>("bottomLeft");
    QTest::newRow("bottom-left core") << true;
    QTest::newRow("top-left core") << false;
  }

  void displayTextureIsTopDownAndMatchesReadback() {
    QFETCH(bool, bottomLeft);
    qputenv("FB_FAKE_HW_BOTTOM_LEFT", bottomLeft ? "1" : "0");
    qputenv("UZUME_SYNC_READBACK", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    qunsetenv("FB_FAKE_HW_BOTTOM_LEFT");
    qunsetenv("UZUME_SYNC_READBACK");
    QCOMPARE(be.lastDisplayMs(), 0.0);
    QVERIFY(!be.lastDisplayed());
    auto t = std::make_shared<FakeDisplay>();
    be.setGpuDisplayTarget(t);
    QVERIFY(be.runFrame());
    QVERIFY(be.lastDisplayed());
    QVERIFY(be.lastDisplayMs() > 0.0 && be.lastDisplayMs() < 1000.0);
    QVERIFY(be.runFrame());
    QCOMPARE(t->gl.calls(), (QStringList{QStringLiteral("acquire"), QStringLiteral("present"), QStringLiteral("acquire"),
                                         QStringLiteral("present")}));
    QVERIFY2(t->gl.violations().isEmpty(), qPrintable(t->gl.violations().join(QLatin1Char(';'))));
    const auto presents = t->gl.presents();
    QCOMPARE(presents.size(), 2);
    QCOMPARE(presents.at(1).size, QSize(64, 48));
    QCOMPARE(t->gl.requests().last(), QSize(64, 48));
    QVERIFY(presents.at(1).texSize.width() > 64 && presents.at(1).texSize.height() > 48);  // top-left of a larger texture
    if (bottomLeft) {
      checkPattern(presents.at(1).image, 64, 48);
    } else {
      QCOMPARE(px(presents.at(1).image, 2, 2), qRgb(255, 0, 0));
      QCOMPARE(px(presents.at(1).image, 61, 45), qRgb(0, 255, 0));
    }
    QCOMPARE(presents.at(1).image, be.videoFrame());  // pixel for pixel what the readback shows
    QCOMPARE(be.sourceFrameSize(), QSize(64, 48));
    QCOMPARE(be.hwReadbackCount(), quint64(2));       // the readback is independent and unchanged
  }

  void displayScalesIntoTheLimitAndGrowsTheTexture() {
    qputenv("FB_FAKE_HW_SCALE", "4");  // 256x192, the core leaves GL_SCISSOR_TEST enabled
    qputenv("UZUME_SYNC_READBACK", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    qunsetenv("FB_FAKE_HW_SCALE");
    qunsetenv("UZUME_SYNC_READBACK");
    auto t = std::make_shared<FakeDisplay>();
    be.setGpuDisplayTarget(t);
    be.setDisplayLimit(QSize(64, 48));
    be.setReadbackLimit(QSize(64, 48));
    QVERIFY(!runFrames(be, 2).isNull());
    QCOMPARE(t->gl.presents().last().size, QSize(64, 48));
    checkPattern(t->gl.presents().last().image, 64, 48);
    QCOMPARE(t->gl.presents().last().image, be.videoFrame());
    be.setDisplayLimit(QSize(128, 96));  // changes mid-run: a bigger texture object, the cached FBO must follow
    be.setReadbackLimit(QSize(128, 96));
    QVERIFY(!runFrames(be, 2).isNull());
    QCOMPARE(t->gl.presents().last().size, QSize(128, 96));
    checkPattern(t->gl.presents().last().image, 128, 96);
    QCOMPARE(t->gl.presents().last().image, be.videoFrame());
    be.setDisplayLimit(QSize(100, 100));  // aspect kept, never above the limit: 100x75
    QVERIFY(!runFrames(be, 2).isNull());
    QCOMPARE(t->gl.presents().last().size, QSize(100, 75));
    checkPattern(t->gl.presents().last().image, 100, 75);
    be.setDisplayLimit(QSize());          // no limit: the full frame, no rounding to even either
    QVERIFY(!runFrames(be, 2).isNull());
    QCOMPARE(t->gl.presents().last().size, QSize(256, 192));
    checkPattern(t->gl.presents().last().image, 256, 192);
    QVERIFY(t->gl.textures() >= 2);
    QVERIFY2(t->gl.violations().isEmpty(), qPrintable(t->gl.violations().join(QLatin1Char(';'))));
    QCOMPARE(t->gl.count(QStringLiteral("abort")), 0);
    QCOMPARE(t->gl.count(QStringLiteral("fail")), 0);
    QCOMPARE(t->gl.count(QStringLiteral("acquire")), t->gl.count(QStringLiteral("present")));
    QCOMPARE(HwRenderContext::scaledReadbackSize(256, 192, QSize(100, 100)), QSize(100, 75));
  }

  void displaySkipAndNotReadyDrawNothing_data() {
    QTest::addColumn<int>("status");
    QTest::newRow("skip") << static_cast<int>(fbtest::FakeGlDisplay::Status::Skip);
    QTest::newRow("not ready") << static_cast<int>(fbtest::FakeGlDisplay::Status::NotReady);
  }

  void displaySkipAndNotReadyDrawNothing() {
    QFETCH(int, status);
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeDisplay>();
    t->gl.nextStatus = status;
    be.setGpuDisplayTarget(t);
    QTest::failOnWarning(QRegularExpression(QStringLiteral("Display target")));
    QVERIFY(!runFrames(be, 3).isNull());
    QCOMPARE(t->gl.count(QStringLiteral("acquire")), 3);  // asked again every frame
    QCOMPARE(t->gl.count(QStringLiteral("present")), 0);
    QCOMPARE(t->gl.count(QStringLiteral("fail")), 0);
    QCOMPARE(t->gl.count(QStringLiteral("release")), 0);  // not dropped
    QVERIFY(!be.lastDisplayed());
    t->gl.nextStatus = static_cast<int>(fbtest::FakeGlDisplay::Status::Ok);
    QVERIFY(be.runFrame());
    QVERIFY(be.lastDisplayed());
    QCOMPARE(t->gl.count(QStringLiteral("present")), 1);
    QVERIFY(t->gl.violations().isEmpty());
  }

  void displayUnavailableIsReleasedQuietly() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeDisplay>();
    t->gl.nextStatus = static_cast<int>(fbtest::FakeGlDisplay::Status::Unavailable);
    be.setGpuDisplayTarget(t);
    QTest::failOnWarning(QRegularExpression(QStringLiteral("Display target")));  // at most an info line
    QVERIFY(!runFrames(be, 4).isNull());
    QCOMPARE(t->gl.calls(), (QStringList{QStringLiteral("acquire (unavailable)"), QStringLiteral("release (GL current)")}));
    QCOMPARE(t->gl.count(QStringLiteral("fail")), 0);  // it reports Unavailable itself
    QCOMPARE(t.use_count(), 1);                         // forgotten
    checkPattern(be.videoFrame(), 64, 48);               // the game view keeps its readback frames
  }

  void displayFailureDropsTheTarget_data() {
    QTest::addColumn<bool>("acquireFails");
    QTest::addColumn<QString>("reason");
    QTest::addColumn<QStringList>("expectedCalls");
    QTest::newRow("acquire failed") << true << QStringLiteral("acquire failed")
                                    << QStringList{QStringLiteral("acquire (failed)"), QStringLiteral("fail"),
                                                   QStringLiteral("release (GL current)")};
    // The texture it hands out is unusable for the blit: the lock is given back with abort(), nothing is published.
    QTest::newRow("blit failed") << false << QStringLiteral("blit failed")
                                 << QStringList{QStringLiteral("acquire"), QStringLiteral("abort"), QStringLiteral("fail"),
                                                QStringLiteral("release (GL current)")};
  }

  void displayFailureDropsTheTarget() {
    QFETCH(bool, acquireFails);
    QFETCH(QString, reason);
    QFETCH(QStringList, expectedCalls);
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeDisplay>();
    if (acquireFails) t->gl.nextStatus = static_cast<int>(fbtest::FakeGlDisplay::Status::Failed);
    else t->gl.lieAboutSize = true;
    be.setGpuDisplayTarget(t);
    QTest::ignoreMessage(QtWarningMsg, qPrintable(QStringLiteral("Display target dropped (%1); the game view uses readback frames").arg(reason)));
    QVERIFY(!runFrames(be, 3).isNull());
    QCOMPARE(t->gl.calls(), expectedCalls);
    QCOMPARE(t->gl.failReason(), reason);
    QVERIFY(t->gl.violations().isEmpty());
    QVERIFY(!be.lastDisplayed());
    QCOMPARE(t.use_count(), 1);
    QCOMPARE(runFrames(be, 3).size(), QSize(64, 48));  // the game goes on, with readback frames
    QCOMPARE(t->gl.calls(), expectedCalls);             // and the target is not called any more
    checkPattern(be.videoFrame(), 64, 48);
  }

  void displayNotWantedDoesNothing() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeDisplay>();
    t->gl.isWanted = false;
    be.setGpuDisplayTarget(t);
    QVERIFY(!runFrames(be, 3).isNull());
    QVERIFY(t->gl.calls().isEmpty());
    QVERIFY(!be.lastDisplayed());
    t->gl.isWanted = true;
    QVERIFY(be.runFrame());
    QCOMPARE(t->gl.count(QStringLiteral("present")), 1);
  }

  void displayNotRunWhileVideoIsNotWanted() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeDisplay>();
    be.setGpuDisplayTarget(t);
    QVERIFY(be.runFrame());
    be.setVideoWanted(false);
    QVERIFY(be.runFrame());
    QVERIFY(be.runFrame());
    QCOMPARE(t->gl.count(QStringLiteral("present")), 1);  // frames nobody sees are not blitted either
    QCOMPARE(be.lastDisplayMs(), 0.0);
  }

  void readbackCanBeSwitchedOffAndOn() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeDisplay>();
    be.setGpuDisplayTarget(t);
    be.setReadbackEnabled(false);  // from the very first frame
    QVERIFY(be.sourceFrameSize().isEmpty());
    QVERIFY(be.runFrame());
    QCOMPARE(be.sourceFrameSize(), QSize(64, 48));  // known without any readback
    QVERIFY(be.videoFrame().isNull());              // nothing was ever read back
    QCOMPARE(be.hwReadbackCount(), quint64(0));
    QCOMPARE(be.lastReadbackMs(), 0.0);
    QVERIFY(be.lastDisplayed());
    QVERIFY(!runFrames(be, 5).isNull() || be.videoFrame().isNull());
    QCOMPARE(be.frameCount(), quint64(6));          // frames still count as delivered
    QCOMPARE(be.hwReadbackCount(), quint64(0));
    QCOMPARE(t->gl.count(QStringLiteral("present")), 6);
    be.setReadbackEnabled(true);
    QVERIFY(be.runFrame());
    QCOMPARE(be.hwReadbackCount(), quint64(1));
    QVERIFY(be.lastReadbackMs() > 0.0);
    // The first readback after the pause is the CURRENT frame, not one from before (the PBO pipeline was reset).
    const int presented = counterOf(t->gl.lastImage());
    QCOMPARE(counterOf(be.videoFrame()), presented);
    QVERIFY(be.runFrame());
    QCOMPARE(be.hwReadbackCount(), quint64(2));
    checkPattern(be.videoFrame(), 64, 48);
    be.setReadbackEnabled(false);
    const QImage kept = be.videoFrame();
    QVERIFY(be.runFrame());
    QCOMPARE(be.videoFrame(), kept);                // off again: the last image stays
    QCOMPARE(be.hwReadbackCount(), quint64(2));
    QVERIFY(t->gl.violations().isEmpty());
  }

  void readbackOffWithoutDisplayTargetStillWorks() {  // the switch is independent of the target
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    be.setReadbackEnabled(false);
    QVERIFY(!runFrames(be, 3).isNull() || be.videoFrame().isNull());
    QCOMPARE(be.hwReadbackCount(), quint64(0));
    QCOMPARE(be.sourceFrameSize(), QSize(64, 48));
    be.setReadbackEnabled(true);
    QVERIFY(be.runFrame());
    QCOMPARE(be.hwReadbackCount(), quint64(1));
    checkPattern(be.videoFrame(), 64, 48);
  }

  void runnerEmitsFramesWithoutReadbackAndReportsDisplayTiming() {
    QTemporaryFile rom;
    QVERIFY(rom.open());
    rom.write("x");
    rom.flush();
    auto owned = std::make_unique<LibretroBackend>();
    LibretroBackend* be = owned.get();
    EmulationRunner runner(std::move(owned));
    QSignalSpy frames(&runner, &EmulationRunner::frameReady);
    QSignalSpy failed(&runner, &EmulationRunner::startFailed);
    auto t = std::make_shared<FakeDisplay>();
    runner.setGpuDisplayTarget(t);
    runner.setReadbackEnabled(false);
    runner.setDisplayLimit(QSize(32, 24));
    EmulationRunner::StartRequest req;
    req.corePath = QStringLiteral(FB_FAKE_HW_CORE_PATH);
    req.gamePath = rom.fileName();
    runner.start(req);
    QTRY_VERIFY2(frames.count() >= 10 || failed.count() > 0, "no frames");
    QCOMPARE(failed.count(), 0);
    QCOMPARE(be->hwReadbackCount(), quint64(0));
    QVERIFY(frames.last().at(0).value<QImage>().isNull());  // the stale (here: never read) image, but the signal is there
    QCOMPARE(runner.sourceFrameSize(), QSize(64, 48));
    QTRY_VERIFY(t->gl.presents().size() >= 5);
    QCOMPARE(t->gl.presents().last().size, QSize(32, 24));
    QTRY_VERIFY2(runner.timing().valid && runner.timing().displaysPerSec > 10.0, "no display timing");
    const auto snap = runner.timing();
    QCOMPARE(snap.readbacksPerSec, 0.0);
    QCOMPARE(snap.readbackMs, 0.0);
    QVERIFY(snap.displayMs > 0.0);
    QVERIFY(snap.emuMs >= 0.0 && snap.emuMs <= snap.frameMs);
    runner.setReadbackEnabled(true);  // thread-safe while running
    const int before = frames.count();
    QTRY_VERIFY(frames.count() >= before + 4);
    QTRY_VERIFY(!frames.last().at(0).value<QImage>().isNull());
    QVERIFY(be->hwReadbackCount() > 0);
    runner.stop();
    QVERIFY(t->gl.violations().isEmpty());
    QCOMPARE(t->gl.count(QStringLiteral("release")), 1);  // the game quit released it, GL current
    QVERIFY(t->gl.lastReleaseGlCurrent());
  }

  void displayKeepsTheCoresBindings_data() {
    QTest::addColumn<QSize>("limit");
    QTest::newRow("1:1 copy") << QSize();
    QTest::newRow("scaled (halving)") << QSize(32, 24);
    QTest::newRow("scaled (final blit only)") << QSize(50, 37);
  }

  void displayKeepsTheCoresBindings() {
    QFETCH(QSize, limit);
    qputenv("FB_FAKE_HW_READFBO", "1");
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto t = std::make_shared<FakeDisplay>();
    be.setGpuDisplayTarget(t);
    be.setDisplayLimit(limit);
    QVERIFY(!runFrames(be, 6).isNull());
    qunsetenv("FB_FAKE_HW_READFBO");
    QCOMPARE(t->gl.presents().size(), 6);
    const QList<int> readFbo = logValues(QStringLiteral("readfbo")), drawFbo = logValues(QStringLiteral("drawfbo"));
    const QList<int> scissor = logValues(QStringLiteral("scissor")), fbo = logValues(QStringLiteral("fbo"));
    QCOMPARE(readFbo.size(), 6);
    for (int k = 1; k < 6; ++k) {
      QCOMPARE(readFbo.at(k), 0);
      QCOMPARE(drawFbo.at(k), fbo.at(k - 1));
      QCOMPARE(scissor.at(k), 1);
    }
    QVERIFY(t->gl.violations().isEmpty());
  }

  void displayAndEncodeShareTheFrame() {
    QTemporaryFile rom;
    LibretroBackend be;
    loadFake(be, rom);
    auto enc = std::make_shared<FakeTarget>();
    auto disp = std::make_shared<FakeDisplay>();
    be.setGpuEncodeTarget(enc);
    be.setGpuDisplayTarget(disp);
    be.setReadbackEnabled(false);
    QVERIFY(!runFrames(be, 4).isNull() || be.videoFrame().isNull());
    QCOMPARE(disp->gl.presents().size(), 4);
    QCOMPARE(enc->count(QStringLiteral("capture")), 3);
    checkPattern(asRgb32(enc->images().last()), 64, 48);
    checkPattern(disp->gl.lastImage(), 64, 48);
    QVERIFY(enc->violations().isEmpty() && disp->gl.violations().isEmpty());
  }

  void displayApiAndReleaseOrderOnTheContext() {
    HwRenderContext hw;
    hw.prepareSurface();
    QString err;
    QVERIFY2(hw.createContext(true, 3, 3, false, false, &err), qPrintable(err));
    QVERIFY(hw.ensureSize(64, 48));
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    auto drawColor = [&](float r, float g, float b) {
      f->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(hw.framebuffer()));
      f->glClearColor(r, g, b, 1.f);
      f->glClear(GL_COLOR_BUFFER_BIT);
    };
    QVERIFY(!hw.hasDisplayTarget());
    drawColor(1.f, 0.f, 0.f);
    QVERIFY(!hw.displayBlit(64, 48, true, QSize()));  // no target: nothing happens
    QCOMPARE(hw.displayBlitCount(), quint64(0));

    auto d = std::make_shared<FakeDisplay>();
    hw.setDisplayTarget(d);
    QVERIFY(hw.hasDisplayTarget());
    QVERIFY(!hw.displayBlit(0, 48, true, QSize()));    // nonsense sizes
    QVERIFY(!hw.displayBlit(641, 48, true, QSize()));  // larger than the FBO (it is at least 640 wide)
    QVERIFY(d->gl.calls().isEmpty());
    QVERIFY(hw.displayBlit(64, 48, true, QSize(32, 24)));
    QCOMPARE(hw.displayBlitCount(), quint64(1));
    QCOMPARE(d->gl.presents().last().size, QSize(32, 24));
    QCOMPARE(px(d->gl.presents().last().image, 16, 12), qRgb(255, 0, 0));
    drawColor(0.f, 1.f, 0.f);
    QVERIFY(hw.displayBlit(64, 48, true, QSize(32, 24)));
    QCOMPARE(px(d->gl.presents().last().image, 16, 12), qRgb(0, 255, 0));
    hw.dropDisplayTarget(QStringLiteral("software frame"));  // e.g. the core delivers CPU frames: fail + release + forget
    QVERIFY(!hw.hasDisplayTarget());
    QCOMPARE(d->gl.failReason(), QStringLiteral("software frame"));
    QCOMPARE(d->gl.releases(), 1);
    hw.setDisplayTarget(d);
    QVERIFY(hw.displayBlit(64, 48, true, QSize(32, 24)));
    hw.setDisplayTarget(nullptr);  // released with GL current, before its objects go
    QVERIFY(!hw.hasDisplayTarget());
    QCOMPARE(d->gl.releases(), 2);
    QVERIFY(d->gl.lastReleaseGlCurrent() && d->gl.textureAliveAtRelease());
    QVERIFY(!hw.displayBlit(64, 48, true, QSize()));
    hw.setDisplayTarget(d);  // set again: acquires again
    QVERIFY(hw.displayBlit(64, 48, true, QSize()));
    QCOMPARE(d->gl.presents().last().size, QSize(64, 48));

    // destroyContext: encode target first (detach), then the display target (release, GL alive), then the GL objects.
    auto enc = std::make_shared<FakeTarget>();
    hw.setEncodeTarget(enc);
    hw.encodeBlit(64, 48, true);
    int detachesAtDisplayRelease = -1;
    bool glCurrentAtRelease = false;
    d->onRelease = [&]() {
      detachesAtDisplayRelease = enc->count(QStringLiteral("detach"));
      glCurrentAtRelease = QOpenGLContext::currentContext() != nullptr;
    };
    hw.destroyContext();
    QCOMPARE(detachesAtDisplayRelease, 1);  // the encode target was already detached
    QVERIFY(glCurrentAtRelease);
    QCOMPARE(d->gl.releases(), 3);
    QVERIFY(d->gl.lastReleaseGlCurrent() && d->gl.textureAliveAtRelease());
    QVERIFY(!hw.hasDisplayTarget());
    QCOMPARE(d.use_count(), 1);
    QVERIFY(d->gl.violations().isEmpty());
  }
};

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  if (!canCreateGl33Core()) {
#ifdef Q_OS_LINUX
    const bool required = qEnvironmentVariableIsSet("CI");  // GitHub Actions sets CI=true
#else
    const bool required = false;
#endif

    qWarning("No OpenGL 3.3 core context available (platform %s)", qPrintable(QGuiApplication::platformName()));
    return required ? 1 : 77;
  }
  HwRenderTest t;
  return QTest::qExec(&t, argc, argv);
}

#include "hw_render_test.moc"
