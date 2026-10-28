// Loopback of the Session media path: SessionHost and SessionViewer(s) in one process with in-memory signaling.
// Synthetic moving frames and a 440 Hz tone go through encoder, libdatachannel, decoder. No network, no Hub.
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QtTest>

#include "processguard.h"
#include <cmath>
#include <memory>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mediacaps.h"
#include "sessionhost.h"
#include "sessionviewer.h"

using namespace uzume;

namespace {
constexpr int kW = 256;
constexpr int kH = 384;
constexpr int kCoreRate = 32768;  // typical DS rate: exercises the 48 kHz resampler
const QColor kBackground(200, 60, 40);

// Solid colour with a moving white bar: average colour is stable, content changes every frame.
QImage syntheticFrame(int n, QImage::Format format = QImage::Format_RGB32) {
  QImage img(kW, kH, QImage::Format_RGB32);
  img.fill(kBackground);
  QPainter p(&img);
  p.fillRect((n * 4) % (kW - 16), (n * 3) % (kH - 16), 16, 16, Qt::white);
  p.end();
  return format == QImage::Format_RGB32 ? img : img.convertToFormat(format);
}

QColor averageColor(const QImage& img) {
  qint64 r = 0, g = 0, b = 0;
  for (int y = 0; y < img.height(); y += 4) {
    for (int x = 0; x < img.width(); x += 4) {
      const QRgb c = img.pixel(x, y);
      r += qRed(c);
      g += qGreen(c);
      b += qBlue(c);
    }
  }
  const qint64 n = (img.width() / 4) * (img.height() / 4);
  return QColor(static_cast<int>(r / n), static_cast<int>(g / n), static_cast<int>(b / n));
}

// A GPU frame the encoder cannot use: format CUDA, a 16-byte buffer, and a zeroed frames context (so openGpu() rejects it
// deterministically without touching NVENC, and cudaHealthCheck() has nothing to check). Stands in for a CudaGlCapture frame.
std::shared_ptr<AVFrame> fakeGpuFrame() {
  AVFrame* f = av_frame_alloc();
  f->format = AV_PIX_FMT_CUDA;
  f->width = kW;
  f->height = kH;
  f->buf[0] = av_buffer_alloc(16);
  f->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
  return std::shared_ptr<AVFrame>(f, [](AVFrame* p) { av_frame_free(&p); });
}

// Sets (or removes, value == nullptr) an environment variable for one scope.
struct EnvScope {
  EnvScope(const char* name, const char* value) : name_(name) {
    if (value) qputenv(name, value);
    else qunsetenv(name);
  }
  ~EnvScope() { qunsetenv(name_); }
  const char* name_;
};

// One Session with a host and N viewers, wired in memory the way the Hub would relay.
struct Rig {
  SessionHost host;
  std::vector<std::unique_ptr<SessionViewer>> viewers;
  QStringList ids;
  QTimer source;
  int frameNo = 0;
  double phase = 0.0;
  QImage::Format format = QImage::Format_RGB32;
  const QString sessionId = QStringLiteral("00000000-0000-4000-8000-000000000001");

  Rig() {
    host.open(sessionId, {});
    source.setInterval(16);
    QObject::connect(&source, &QTimer::timeout, &host, [this]() {
      host.pushFrame(syntheticFrame(frameNo++, format));
      const int frames = kCoreRate / 60;
      QByteArray pcm(frames * 4, 0);
      auto* s = reinterpret_cast<qint16*>(pcm.data());
      for (int i = 0; i < frames; ++i) {
        const auto v = static_cast<qint16>(12000.0 * std::sin(phase));
        phase += 2.0 * M_PI * 440.0 / kCoreRate;
        s[2 * i] = s[2 * i + 1] = v;
      }
      host.pushAudio(pcm, kCoreRate);
    });
  }
  ~Rig() { source.stop(); }

  SessionViewer* addViewer() {
    const QString id = QStringLiteral("viewer-%1").arg(viewers.size() + 1);
    auto v = std::make_unique<SessionViewer>();
    QObject::connect(v.get(), &SessionViewer::signalOut, &host, &SessionHost::handleSignal);
    QObject::connect(&host, &SessionHost::signalOut, v.get(), [vp = v.get()](const SessionSignal& s) { vp->handleSignal(s); });
    v->open(sessionId, id, {});
    host.addViewer(id);  // = viewer_joined from the Hub
    ids << id;
    viewers.push_back(std::move(v));
    return viewers.back().get();
  }
};
}  // namespace

class LoopbackTest : public QObject {
  Q_OBJECT
 private slots:
  void capabilitiesAreTruthful() {
    const MediaCapabilities c = detectMediaCapabilities();
    QVERIFY(c.h264Decode);
    QVERIFY(c.opus);
    QVERIFY2(c.h264Encode && !c.encoders.isEmpty(), "no H.264 encoder opens");
    qInfo().noquote() << "[loopback] encoders that open:" << c.encoders.join(QLatin1Char(','));
    HandshakeInfo h;
    applyMediaCapabilities(&h);
    QCOMPARE(h.h264Encode, c.h264Encode);
    QCOMPARE(h.encoders, c.encoders);
  }

  void noEncodingWithoutViewers() {
    Rig rig;
    for (int i = 0; i < 20; ++i) {
      rig.host.pushFrame(syntheticFrame(i));
    }
    QVERIFY(!rig.host.encoderRunning());
    QCOMPARE(rig.host.viewerCount(), 0);
  }

  void videoAndAudioArriveAndViewerRemovalStopsEverything() {
    Rig rig;
    QSignalSpy encoderSpy(&rig.host, &SessionHost::encoderRunningChanged);
    QSignalSpy closedSpy(&rig.host, &SessionHost::viewerClosed);
    QSignalSpy rxSpy(&rig.host, &SessionHost::rxReportReceived);
    SessionViewer* viewer = rig.addViewer();

    int frames = 0, wrongSize = 0, nonSilentPulls = 0;
    QColor lastColor;
    QObject::connect(viewer, &SessionViewer::frameReady, this, [&](const QImage& f) {
      ++frames;
      if (f.width() != kW || f.height() != kH) {
        ++wrongSize;
      }
      lastColor = averageColor(f);
    });
    // Audio output stand-in: pulls 10 ms every 10 ms.
    double energy = 0;
    qint64 samples = 0;
    QTimer sink;
    sink.setInterval(10);
    QObject::connect(&sink, &QTimer::timeout, this, [&]() {
      const QByteArray pcm = viewer->pullAudio(480);
      const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
      bool nonSilent = false;
      for (int i = 0; i < 960; ++i) {
        energy += double(s[i]) * s[i];
        ++samples;
        nonSilent = nonSilent || s[i] != 0;
      }
      nonSilentPulls += nonSilent ? 1 : 0;
    });
    sink.start();
    rig.source.start();

    QVERIFY(QTest::qWaitFor([&]() { return viewer->isConnected(); }, 15000));
    QVERIFY2(QTest::qWaitFor([&]() { return frames >= 60 && nonSilentPulls >= 20; }, 15000),
             qPrintable(QStringLiteral("frames=%1 nonSilentPulls=%2").arg(frames).arg(nonSilentPulls)));
    qInfo().noquote() << "[loopback] encoder" << rig.host.stats().encoderName << "frames" << frames << "last color" << lastColor.name();
    QVERIFY(rig.host.encoderRunning());
    QCOMPARE(encoderSpy.count(), 1);
    QCOMPARE(wrongSize, 0);
    QVERIFY2(std::abs(lastColor.red() - kBackground.red()) < 30 && std::abs(lastColor.green() - kBackground.green()) < 30 &&
                 std::abs(lastColor.blue() - kBackground.blue()) < 30,
             qPrintable(lastColor.name()));
    const double rms = std::sqrt(energy / double(std::max<qint64>(1, samples)));
    QVERIFY2(rms > 1500.0, qPrintable(QString::number(rms)));  // tone amplitude 12000 -> RMS ~8500, minus silence while priming
    QVERIFY2(viewer->audioPeak() > 8000, qPrintable(QString::number(viewer->audioPeak())));

    // Stats on both sides (the host/viewer refresh them once a second).
    QVERIFY2(QTest::qWaitFor([&]() { return rig.host.stats().viewers == 1 && rig.host.stats().videoBitrateKbps > 0; }, 15000),
             qPrintable(QStringLiteral("viewers=%1 video_kbps=%2").arg(rig.host.stats().viewers).arg(rig.host.stats().videoBitrateKbps)));
    // Stats refresh once a second: wait until every value checked below has been computed at least once.
    QVERIFY2(QTest::qWaitFor([&]() {
      const SessionStats h = rig.host.stats(), v = viewer->stats();
      return h.fps > 20.0 && h.videoBitrateKbps > 5.0 && h.audioBitrateKbps > 20.0 && v.fps > 20.0 && v.audioFrames > 0 && v.width == kW &&
             !v.connectionType.isEmpty() && !rig.host.viewerLinks().isEmpty() && !rig.host.viewerLinks().first().connectionType.isEmpty();
    }, 15000), "stats did not settle");
    const SessionStats hs = rig.host.stats();
    const SessionStats vs = viewer->stats();
    QVERIFY(!hs.encoderName.isEmpty());
    QCOMPARE(hs.codec, QStringLiteral("H264 + Opus"));
    QCOMPARE(hs.width, kW);
    QVERIFY2(hs.fps > 20.0 && hs.fps < 70.0, qPrintable(QString::number(hs.fps)));
    QVERIFY2(hs.videoBitrateKbps > 5.0,  // encoders differ a lot (libopenh264 ~30 kbit/s here, x264 ~2000)
              qPrintable(QStringLiteral("encoder %1 video_kbps %2").arg(hs.encoderName).arg(hs.videoBitrateKbps)));
    QVERIFY2(hs.audioBitrateKbps > 20.0 && hs.audioBitrateKbps < 200.0, qPrintable(QString::number(hs.audioBitrateKbps)));
    QVERIFY(!hs.packetLossPercent.has_value());
    QVERIFY2(vs.fps > 20.0, qPrintable(QString::number(vs.fps)));
    QCOMPARE(vs.width, kW);
    QVERIFY2(vs.connectionType.startsWith(QLatin1String("direct (")), qPrintable(vs.connectionType));
    QVERIFY2(rig.host.viewerLinks().first().connectionType.startsWith(QLatin1String("direct (")),
             qPrintable(rig.host.viewerLinks().first().connectionType));
    // Starts at 2000 kbit/s; a lossy rx report from a loaded CI runner may already have lowered it by the adaptation
    // (x0.7 per step, floor 300). The exact steps are covered by bitratecontroller_test.
    QVERIFY2(hs.targetBitrateKbps >= 300.0 && hs.targetBitrateKbps <= 2000.0, qPrintable(QString::number(hs.targetBitrateKbps)));
    QVERIFY(vs.audioFrames > 0);
    QVERIFY(rig.host.viewerLinks().size() == 1);

    // RTT needs an SCTP association: the negotiated "fb-diag" data channel provides it, on both sides.
    QVERIFY2(QTest::qWaitFor([&]() { return rig.host.stats().rttMs.has_value() && viewer->stats().rttMs.has_value(); }, 20000),
             qPrintable(QStringLiteral("host rtt %1, viewer rtt %2")
                            .arg(rig.host.stats().rttMs ? QString::number(*rig.host.stats().rttMs) : QStringLiteral("n/a"))
                            .arg(viewer->stats().rttMs ? QString::number(*viewer->stats().rttMs) : QStringLiteral("n/a"))));
    QVERIFY(rig.host.viewerLinks().first().rttMs.has_value());
    // Bitrate adaptation input: the viewer reports loss and received rate once a second over fb-diag.
    QVERIFY2(QTest::qWaitFor([&]() { return rxSpy.count() >= 1; }, 5000), "no rx report reached the host");
    QVERIFY(rxSpy.first().at(0).toString() == rig.ids.first());
    QVERIFY(rxSpy.first().at(1).toDouble() >= 0.0 && rxSpy.first().at(1).toDouble() <= 1.0);
    QVERIFY(rig.host.stats().targetBitrateKbps >= 300.0);
    // 0.6 D7/D8: the host keeps the latest report per viewer (fps and decoder included); the viewer names its real decoder.
    QCOMPARE(vs.decoderName, QStringLiteral("h264"));
    QVERIFY2(QTest::qWaitFor([&]() {
      const auto l = rig.host.viewerLinks();
      return l.size() == 1 && l.first().hasReport && l.first().reportFps.has_value() && !l.first().reportDecoder.isEmpty();
    }, 5000), "host did not keep fps/decoder of the viewer report");
    {
      const ViewerLinkStats l = rig.host.viewerLinks().first();
      QCOMPARE(l.reportDecoder, QStringLiteral("h264"));
      QVERIFY(l.reportFps && *l.reportFps >= 0.0 && *l.reportFps < 200.0);
      QVERIFY(l.reportLoss >= 0.0 && l.reportLoss <= 1.0);
      QVERIFY(l.reportKbps >= 0.0);
    }
    QVERIFY(viewer->link().rttMs.has_value());
    QVERIFY2(*rig.host.stats().rttMs >= 0.0 && *rig.host.stats().rttMs < 1000.0, qPrintable(QString::number(*rig.host.stats().rttMs)));

    // Viewer removal (viewer_left): the PeerConnection closes immediately, the encoder stops.
    rig.host.removeViewer(rig.ids.first());
    QCOMPARE(rig.host.viewerCount(), 0);
    QVERIFY(!rig.host.encoderRunning());
    QCOMPARE(encoderSpy.count(), 2);
    QCOMPARE(closedSpy.count(), 1);
    QVERIFY2(closedSpy.first().at(1).toString().endsWith(QStringLiteral(":closed")), qPrintable(closedSpy.first().at(1).toString()));
    // The Hub sends viewer_left to the viewer side as well; it closes its PeerConnection immediately.
    viewer->close();
    QVERIFY(!viewer->isConnected());
    QVERIFY(!viewer->isOpen());
    // Frames pushed with nobody watching are dropped without restarting the encoder.
    rig.host.pushFrame(syntheticFrame(1));
    QVERIFY(!rig.host.encoderRunning());
  }

  void encodingRunsOffTheCallingThread() {
    Rig rig;
    SessionViewer* viewer = rig.addViewer();
    int frames = 0;
    QObject::connect(viewer, &SessionViewer::frameReady, this, [&](const QImage&) { ++frames; });
    QVERIFY(QTest::qWaitFor([&]() { return viewer->isConnected(); }, 15000));
    // Feed from the test (UI) thread without ever yielding to the event loop: the pushes must return immediately
    // (copy into the bounded queue), whatever the encoder does.
    QElapsedTimer t;
    t.start();
    qint64 worst = 0;
    for (int i = 0; i < 300; ++i) {
      QElapsedTimer one;
      one.start();
      rig.host.pushFrame(syntheticFrame(i));
      worst = std::max<qint64>(worst, one.nsecsElapsed());
      QTest::qSleep(2);
    }
    qInfo().noquote() << "[loopback] 300 pushes in" << t.elapsed() << "ms, worst push" << worst / 1000 << "us, dropped"
                      << rig.host.stats().droppedFrames;
    QVERIFY(rig.host.encoderRunning());
    // The viewer's UI thread did not run during the burst: its backlog was dropped (PN-6) and it asks for a keyframe once
    // the event loop runs again. A live source (as in a real Session) lets it resync and play on.
    rig.source.start();
    QVERIFY2(QTest::qWaitFor([&]() { return frames >= 10; }, 15000), qPrintable(QString::number(frames)));
    rig.source.stop();
    qInfo().noquote() << "[loopback] viewer dropped" << viewer->droppedVideoFrames() << "video frames during the stall";
    QVERIFY(!rig.host.stats().encoderName.isEmpty());
    QCOMPARE(rig.host.encoderName(), rig.host.stats().encoderName);
    // Stopping while frames are queued joins the worker cleanly; a second start works again.
    for (int i = 0; i < 5; ++i) {
      rig.host.pushFrame(syntheticFrame(i));
    }
    rig.host.close();
    QVERIFY(!rig.host.encoderRunning());
    QCOMPARE(rig.host.stats().videoFrames, 0);
    rig.host.pushFrame(syntheticFrame(1));  // closed: ignored
    QVERIFY(!rig.host.encoderRunning());
  }

  void oneEncoderForSeveralViewersRgb565() {
    Rig rig;
    rig.format = QImage::Format_RGB16;  // raw RGB565 path
    SessionViewer* v1 = rig.addViewer();
    SessionViewer* v2 = rig.addViewer();
    int f1 = 0, f2 = 0;
    QColor c2;
    QObject::connect(v1, &SessionViewer::frameReady, this, [&](const QImage&) { ++f1; });
    QObject::connect(v2, &SessionViewer::frameReady, this, [&](const QImage& f) {
      ++f2;
      c2 = averageColor(f);
    });
    QSignalSpy encoderSpy(&rig.host, &SessionHost::encoderRunningChanged);
    rig.source.start();
    QVERIFY2(QTest::qWaitFor([&]() { return f1 >= 40 && f2 >= 40; }, 20000), qPrintable(QStringLiteral("%1/%2").arg(f1).arg(f2)));
    QCOMPARE(encoderSpy.count(), 1);  // one encoder start for both viewers
    QVERIFY(std::abs(c2.red() - kBackground.red()) < 30 && std::abs(c2.green() - kBackground.green()) < 30 &&
            std::abs(c2.blue() - kBackground.blue()) < 30);

    rig.host.removeViewer(rig.ids.at(0));
    QCOMPARE(rig.host.viewerCount(), 1);
    QVERIFY(rig.host.encoderRunning());  // the second viewer keeps it alive
    const int before = f2;
    QVERIFY(QTest::qWaitFor([&]() { return f2 >= before + 20; }, 10000));

    // Session end: everything closes at once.
    rig.host.close();
    QCOMPARE(rig.host.viewerCount(), 0);
    QVERIFY(!rig.host.encoderRunning());
    v2->close();  // session_ended reaches the viewer side
    QVERIFY(!v2->isConnected());
  }

  // ADR 0033 D5: one viewer stays on Auto, the other picks Low. The host keeps two encoders, both viewers get frames, the
  // Low viewer's stream is ~30 fps (every 2nd frame of the 60 fps source is skipped), ping/pong gives both sides a latency.
  void viewerPicksLowWhileAnotherStaysAuto() {
    Rig rig;
    SessionViewer* vAuto = rig.addViewer();
    SessionViewer* vLow = rig.addViewer();
    int fAuto = 0, fLow = 0;
    QObject::connect(vAuto, &SessionViewer::frameReady, this, [&](const QImage&) { ++fAuto; });
    QObject::connect(vLow, &SessionViewer::frameReady, this, [&](const QImage&) { ++fLow; });
    QCOMPARE(rig.host.encoderCount(), 1);  // everyone on Auto: the adaptive encoder only
    vLow->setQuality(Quality::Low);        // before the channel is open: sent when it opens
    rig.source.start();
    QVERIFY2(QTest::qWaitFor([&]() { return fAuto >= 30 && fLow >= 30; }, 20000), qPrintable(QStringLiteral("%1/%2").arg(fAuto).arg(fLow)));
    QVERIFY2(QTest::qWaitFor([&]() { return rig.host.encoderCount() == 2; }, 10000), qPrintable(QString::number(rig.host.encoderCount())));
    QCOMPARE(rig.host.viewerPick(rig.ids.at(1)), Quality::Low);
    QCOMPARE(rig.host.viewerTier(rig.ids.at(1)), Quality::Low);
    QCOMPARE(rig.host.viewerTier(rig.ids.at(0)), Quality::Auto);
    // The viewer learns what it gets; the Auto viewer is told "auto" with the adaptive target.
    QVERIFY2(QTest::qWaitFor([&]() { return vLow->hasServedTier() && vAuto->hasServedTier(); }, 10000), "no tier message");
    QCOMPARE(vLow->servedTier(), Quality::Low);
    QCOMPARE(vLow->servedKbps(), 600);
    QCOMPARE(vAuto->servedTier(), Quality::Auto);
    // Frame rates over a window once both streams run: Low ~30, Auto ~60 (the sender is a 16 ms timer, so allow a wide band).
    QTest::qWait(500);
    const int a0 = fAuto, l0 = fLow;
    QElapsedTimer t;
    t.start();
    QTest::qWait(3000);
    const double secs = t.elapsed() / 1000.0;
    const double rateAuto = (fAuto - a0) / secs, rateLow = (fLow - l0) / secs;
    qInfo().noquote() << "[loopback] auto" << rateAuto << "fps, low" << rateLow << "fps";
    QVERIFY2(rateLow > 12.0 && rateLow < 36.0, qPrintable(QString::number(rateLow)));
    QVERIFY2(rateAuto > rateLow * 1.3, qPrintable(QStringLiteral("auto %1 low %2").arg(rateAuto).arg(rateLow)));
    // Latency from the 2 s ping/pong, on both sides and in the host's per-viewer links.
    QVERIFY2(QTest::qWaitFor([&]() {
      const auto links = rig.host.viewerLinks();
      return vLow->latencyMs().has_value() && links.size() == 2 && links.at(0).latencyMs.has_value() && links.at(1).latencyMs.has_value();
    }, 10000), "no latency from ping/pong");
    for (const ViewerLinkStats& l : rig.host.viewerLinks()) {
      QVERIFY(*l.latencyMs >= 0.0 && *l.latencyMs < 1000.0);
      QCOMPARE(l.tier, l.viewerId == rig.ids.at(1) ? Quality::Low : Quality::Auto);
    }
    QVERIFY2(QTest::qWaitFor([&]() { return rig.host.stats().encoders == 2; }, 3000), "stats do not show two encoders");
    // Back to Auto: one stream again; the Low encoder is dropped after its idle time (fake clock).
    vLow->setQuality(Quality::Auto);
    QVERIFY2(QTest::qWaitFor([&]() { return rig.host.viewerTier(rig.ids.at(1)) == Quality::Auto; }, 10000), "pick did not arrive");
    rig.host.advanceTierClockForTest(11000);
    QCOMPARE(rig.host.encoderCount(), 1);
    const int l1 = fLow;
    QVERIFY2(QTest::qWaitFor([&]() { return fLow >= l1 + 20; }, 10000), "viewer got no frames after switching back to Auto");
  }

  void lateJoinerGetsKeyframe() {
    Rig rig;
    SessionViewer* v1 = rig.addViewer();
    int f1 = 0;
    QObject::connect(v1, &SessionViewer::frameReady, this, [&](const QImage&) { ++f1; });
    rig.source.start();
    QVERIFY(QTest::qWaitFor([&]() { return f1 >= 30; }, 15000));
    SessionViewer* v2 = rig.addViewer();  // joins mid-GOP: must start decoding quickly (keyframe on join / PLI)
    int f2 = 0;
    QObject::connect(v2, &SessionViewer::frameReady, this, [&](const QImage&) { ++f2; });
    QVERIFY(QTest::qWaitFor([&]() { return f2 >= 20; }, 15000));
  }

  // ADR 0026 D3: pause and resume of the host reach every viewer over fb-diag; a viewer that joins while paused is told
  // as soon as its channel opens; closing the connection clears the state.
  void hostPausedSignalReachesViewers() {
    Rig rig;
    SessionViewer* v1 = rig.addViewer();
    QSignalSpy s1(v1, &SessionViewer::hostStateChanged);
    QVERIFY(!v1->hostPaused());
    rig.source.start();
    QVERIFY(QTest::qWaitFor([&]() { return v1->isConnected(); }, 15000));
    // The channel opens a moment after the media path: the state is sent once it is open.
    rig.host.setHostState(true, QStringLiteral("Max"));
    QVERIFY(QTest::qWaitFor([&]() { return v1->hostPaused(); }, 15000));
    QCOMPARE(v1->hostPausedBy(), QStringLiteral("Max"));
    QCOMPARE(s1.count(), 1);
    QCOMPARE(s1.last().at(0).toBool(), true);
    QCOMPARE(s1.last().at(1).toString(), QStringLiteral("Max"));

    // A second viewer joining while the host is paused
    SessionViewer* v2 = rig.addViewer();
    QVERIFY(QTest::qWaitFor([&]() { return v2->hostPaused(); }, 15000));
    QCOMPARE(v2->hostPausedBy(), QStringLiteral("Max"));

    rig.host.setHostState(false, QString());
    QVERIFY(QTest::qWaitFor([&]() { return !v1->hostPaused() && !v2->hostPaused(); }, 15000));
    QCOMPARE(s1.count(), 2);
    QVERIFY(v1->hostPausedBy().isEmpty());

    // The same state again sends nothing new
    rig.host.setHostState(false, QString());
    QTest::qWait(300);
    QCOMPARE(s1.count(), 2);

    // Closing the connection clears a paused state on the viewer
    rig.host.setHostState(true, QStringLiteral("Max"));
    QVERIFY(QTest::qWaitFor([&]() { return v1->hostPaused(); }, 15000));
    v1->close();
    QVERIFY(!v1->hostPaused());
    QCOMPARE(s1.last().at(0).toBool(), false);
  }

  // ADR 0033 D8: controller hand-over over the real fb-diag / fb-input channels: ask, host allows, frames arrive and are applied,
  // a second viewer is refused, the host takes back, a viewer that leaves releases the port.
  void controllerHandOver() {
    Rig rig;
    rig.host.setControlPorts(2);
    SessionViewer* v1 = rig.addViewer();
    SessionViewer* v2 = rig.addViewer();
    rig.source.start();
    QVERIFY(QTest::qWaitFor([&]() { return v1->isConnected() && v2->isConnected(); }, 15000));
    QVERIFY(QTest::qWaitFor([&]() { return v1->hostPorts() == 2 && v2->hostPorts() == 2; }, 15000));  // control_caps
    QVERIFY(!v1->requestControl(2));
    // the host sees the request, a deny comes back with a reason
    QSignalSpy asked(&rig.host, &SessionHost::controlRequested);
    QVERIFY(v1->requestControl(0));
    QCOMPARE(v1->controlState(), SessionViewer::ControlState::Asking);
    QVERIFY(QTest::qWaitFor([&]() { return asked.count() == 1; }, 10000));
    QCOMPARE(asked.last().at(0).toString(), rig.ids.at(0));
    QCOMPARE(asked.last().at(1).toInt(), 0);
    QVERIFY(rig.host.answerControl(rig.ids.at(0), false));
    QVERIFY(QTest::qWaitFor([&]() { return v1->controlState() == SessionViewer::ControlState::Idle; }, 10000));
    QCOMPARE(v1->controlReason(), QStringLiteral("denied"));

    QVERIFY(v1->requestControl(0));
    QVERIFY(QTest::qWaitFor([&]() { return asked.count() == 2; }, 10000));
    QVERIFY(rig.host.answerControl(rig.ids.at(0), true));
    QVERIFY(QTest::qWaitFor([&]() { return v1->controlState() == SessionViewer::ControlState::Granted; }, 10000));
    QCOMPARE(rig.host.controlHolder(0), rig.ids.at(0));
    // input frames reach the host and are applied
    QVERIFY(QTest::qWaitFor([&]() { return rig.host.controlInputFrame(0).buttons == 0 && rig.host.controlPortOf(rig.ids.at(0)) == 0; }, 1000));
    v1->setControlButtons(0x0081);
    QVERIFY(QTest::qWaitFor([&]() { return rig.host.controlInputFrame(0).buttons == 0x0081; }, 10000));
    // no frame for half a second (the clock jumps ahead): the port is neutral, the grant stays
    {
      QSignalSpy inputs(&rig.host, &SessionHost::controlInput);
      rig.host.advanceControlClockForTest(kInputNeutralMs + 100);
      QVERIFY(inputs.count() >= 1);
      QCOMPARE(inputs.first().at(0).toInt(), 0);
      QCOMPARE(inputs.first().at(2).toBool(), true);
      QCOMPARE(rig.host.controlInputFrame(0).buttons, quint16(0));
      QCOMPARE(rig.host.controlHolder(0), rig.ids.at(0));
    }
    // a second viewer cannot take the held port
    QVERIFY(v2->requestControl(0));
    QVERIFY(QTest::qWaitFor([&]() { return v2->controlState() == SessionViewer::ControlState::Idle; }, 10000));
    QCOMPARE(v2->controlReason(), QStringLiteral("busy"));
    // P2 for the second viewer
    QVERIFY(v2->requestControl(1));
    QVERIFY(QTest::qWaitFor([&]() { return asked.count() == 3; }, 10000));
    QVERIFY(rig.host.answerControl(rig.ids.at(1), true));
    QVERIFY(QTest::qWaitFor([&]() { return v2->controlState() == SessionViewer::ControlState::Granted; }, 10000));
    // the host takes P1 back
    QVERIFY(rig.host.takeBack(0));
    QVERIFY(QTest::qWaitFor([&]() { return v1->controlState() == SessionViewer::ControlState::Idle; }, 10000));
    QCOMPARE(v1->controlReason(), QStringLiteral("host"));
    QVERIFY(rig.host.controlHolder(0).isEmpty());
    // the P2 viewer stops playing
    v2->releaseControl();
    QVERIFY(QTest::qWaitFor([&]() { return rig.host.controlHolder(1).isEmpty(); }, 10000));
    // a viewer that disappears releases its port
    QVERIFY(v1->requestControl(1));
    QVERIFY(QTest::qWaitFor([&]() { return asked.count() == 4; }, 10000));
    QVERIFY(rig.host.answerControl(rig.ids.at(0), true));
    QVERIFY(QTest::qWaitFor([&]() { return v1->controlState() == SessionViewer::ControlState::Granted; }, 10000));
    rig.host.removeViewer(rig.ids.at(0));
    QVERIFY(rig.host.controlHolder(1).isEmpty());
  }

  // ADR 0019: a CUDA frame that h264_nvenc cannot take (here: a frames context that is no CUDA context) sends the Session
  // back to readback frames without an error, without stopping the encoder and without a second keyframe storm.
  void gpuFrameThatCannotOpenFallsBackToReadback() {
    EnvScope noKill("UZUME_DISABLE_GPU_ENCODE", nullptr);
    EnvScope noForce("UZUME_H264_ENCODER", nullptr);
    Rig rig;
    SessionViewer* viewer = rig.addViewer();
    int frames = 0;
    QObject::connect(viewer, &SessionViewer::frameReady, this, [&](const QImage&) { ++frames; });
    QSignalSpy gpuSpy(&rig.host, &SessionHost::gpuInputChanged);
    QSignalSpy errorSpy(&rig.host, &SessionHost::errorOccurred);
    rig.source.start();
    QVERIFY2(QTest::qWaitFor([&]() { return frames >= 20; }, 15000), qPrintable(QString::number(frames)));
    QVERIFY(rig.host.encoderRunning());
    const QString cpuEncoder = rig.host.stats().encoderName;
    QVERIFY(!cpuEncoder.isEmpty());

    // A GPU frame that follows a readback frame closely is dropped by the shared 1/(2 fps) gap: push until one gets through.
    QVERIFY2(QTest::qWaitFor([&]() {
      rig.host.pushGpuFrame(fakeGpuFrame());
      return gpuSpy.count() >= 1;
    }, 10000), "the GPU frame did not reach the worker");
    QCOMPARE(gpuSpy.count(), 1);
    QVERIFY(rig.host.gpuInputFailed());
    QVERIFY(!rig.host.gpuInputActive());
    QVERIFY(!rig.host.stats().gpuInput);
    QVERIFY(rig.host.stats().gpuInputFailed);
    QCOMPARE(rig.host.stats().encoderName, cpuEncoder);  // the CPU encoder was never touched
    QVERIFY(rig.host.encoderRunning());
    QVERIFY(!rig.host.gpuInputAllowed());

    const int before = frames;
    QVERIFY2(QTest::qWaitFor([&]() { return frames >= before + 20; }, 10000), "readback frames stopped arriving");
    rig.host.pushGpuFrame(fakeGpuFrame());  // a second GPU frame is ignored
    QTest::qWait(100);
    QCOMPARE(gpuSpy.count(), 1);
    QVERIFY(rig.host.encoderRunning());
    QCOMPARE(errorSpy.count(), 0);  // no toast: this is not an encoder failure
    QCOMPARE(rig.host.stats().encoderName, cpuEncoder);
  }

  // GPU frames never start the encoder: only a readback frame of a connected viewer does.
  void gpuFrameNeverStartsEncoder() {
    EnvScope noKill("UZUME_DISABLE_GPU_ENCODE", nullptr);
    EnvScope noForce("UZUME_H264_ENCODER", nullptr);
    Rig rig;
    SessionViewer* viewer = rig.addViewer();
    QVERIFY(QTest::qWaitFor([&]() { return viewer->isConnected(); }, 15000));
    QSignalSpy encoderSpy(&rig.host, &SessionHost::encoderRunningChanged);
    QSignalSpy gpuSpy(&rig.host, &SessionHost::gpuInputChanged);
    for (int i = 0; i < 10; ++i) {
      rig.host.pushGpuFrame(fakeGpuFrame());
      QTest::qWait(5);
    }
    QVERIFY(!rig.host.encoderRunning());
    QCOMPARE(encoderSpy.count(), 0);
    QCOMPARE(gpuSpy.count(), 0);
    QVERIFY(!rig.host.gpuInputFailed());
    rig.host.pushFrame(syntheticFrame(0));  // a readback frame starts it
    QVERIFY(rig.host.encoderRunning());
    QCOMPARE(encoderSpy.count(), 1);
  }

  // The watchdog: GPU input is "active" but only readback frames arrive. After 60 of them and a second without a GPU
  // frame it is turned off and the 60th is encoded.
  void watchdogTurnsGpuOff() {
    EnvScope noKill("UZUME_DISABLE_GPU_ENCODE", nullptr);
    EnvScope noForce("UZUME_H264_ENCODER", nullptr);
    Rig rig;
    SessionViewer* viewer = rig.addViewer();
    int frames = 0;
    QObject::connect(viewer, &SessionViewer::frameReady, this, [&](const QImage&) { ++frames; });
    rig.source.start();
    QVERIFY2(QTest::qWaitFor([&]() { return frames >= 20 && rig.host.encoderRunning(); }, 15000), qPrintable(QString::number(frames)));
    rig.source.stop();
    QSignalSpy gpuSpy(&rig.host, &SessionHost::gpuInputChanged);

    rig.host.simulateGpuActiveForTest();
    QVERIFY(rig.host.gpuInputActive());
    QVERIFY(rig.host.stats().gpuInput);
    for (int i = 0; i < 59; ++i) {
      rig.host.pushFrame(syntheticFrame(i));  // skipped: the encoder is supposed to get GPU frames
    }
    QCOMPARE(gpuSpy.count(), 0);
    QVERIFY(rig.host.gpuInputActive());
    QTest::qWait(1100);  // the watchdog also wants a second without a GPU frame on the host clock
    rig.host.pushFrame(syntheticFrame(59));
    QCOMPARE(gpuSpy.count(), 1);
    QVERIFY(rig.host.gpuInputFailed());
    QVERIFY(!rig.host.gpuInputActive());
    QVERIFY(!rig.host.stats().gpuInput);
    QVERIFY(rig.host.stats().gpuInputFailed);

    const int before = frames;
    rig.source.start();  // readback frames are encoded again
    QVERIFY2(QTest::qWaitFor([&]() { return frames >= before + 20; }, 10000), qPrintable(QString::number(frames - before)));
    QVERIFY(rig.host.encoderRunning());
    QCOMPARE(gpuSpy.count(), 1);
  }

  // A UI stall queues a burst of frame events that the mailbox answers with one GPU frame and many empty takes: 120
  // readback frames right after a GPU frame are no failure. Only 60 frames AND a second without a GPU frame are.
  void watchdogIgnoresABurstRightAfterAGpuFrame() {
    EnvScope noKill("UZUME_DISABLE_GPU_ENCODE", nullptr);
    EnvScope noForce("UZUME_H264_ENCODER", nullptr);
    Rig rig;
    SessionViewer* viewer = rig.addViewer();
    int frames = 0;
    QObject::connect(viewer, &SessionViewer::frameReady, this, [&](const QImage&) { ++frames; });
    rig.source.start();
    QVERIFY2(QTest::qWaitFor([&]() { return frames >= 20 && rig.host.encoderRunning(); }, 15000), qPrintable(QString::number(frames)));
    rig.source.stop();
    QSignalSpy gpuSpy(&rig.host, &SessionHost::gpuInputChanged);

    rig.host.simulateGpuActiveForTest();  // "a GPU frame was just encoded"
    QVERIFY(rig.host.gpuInputActive());
    for (int i = 0; i < 120; ++i) {
      rig.host.pushFrame(syntheticFrame(i));  // skipped, and no trip: the last GPU frame is milliseconds old
    }
    QCOMPARE(gpuSpy.count(), 0);
    QVERIFY(rig.host.gpuInputActive());
    QVERIFY(!rig.host.gpuInputFailed());

    QTest::qWait(1100);  // no GPU frame for a second now, and far more than 60 readback frames counted: the next one trips
    rig.host.pushFrame(syntheticFrame(120));
    QCOMPARE(gpuSpy.count(), 1);
    QVERIFY(rig.host.gpuInputFailed());
    QVERIFY(!rig.host.gpuInputActive());
  }

  // GPU input is configured at open(): the kill switch and a forced encoder (also h264_nvenc) keep the readback path.
  void gpuConfigSwitches() {
    {
      EnvScope kill("UZUME_DISABLE_GPU_ENCODE", "1");
      EnvScope noForce("UZUME_H264_ENCODER", nullptr);
      Rig rig;
      QVERIFY(!rig.host.gpuInputAllowed());
      SessionViewer* viewer = rig.addViewer();
      int frames = 0;
      QObject::connect(viewer, &SessionViewer::frameReady, this, [&](const QImage&) { ++frames; });
      QSignalSpy gpuSpy(&rig.host, &SessionHost::gpuInputChanged);
      rig.source.start();
      QVERIFY2(QTest::qWaitFor([&]() { return frames >= 10; }, 15000), qPrintable(QString::number(frames)));
      for (int i = 0; i < 20; ++i) {
        rig.host.pushGpuFrame(fakeGpuFrame());  // ignored: had the worker seen it, GPU input would have failed
        QTest::qWait(5);
      }
      QCOMPARE(gpuSpy.count(), 0);
      QVERIFY(!rig.host.gpuInputFailed());
      QVERIFY(!rig.host.gpuInputAllowed());
    }
    {
      EnvScope noKill("UZUME_DISABLE_GPU_ENCODE", nullptr);
      EnvScope force("UZUME_H264_ENCODER", "libx264");
      Rig rig;  // no frames are encoded here: the forced encoder may not exist in this FFmpeg
      QVERIFY(!rig.host.gpuInputAllowed());
      SessionViewer* viewer = rig.addViewer();
      QVERIFY(QTest::qWaitFor([&]() { return viewer->isConnected(); }, 15000));
      rig.host.pushGpuFrame(fakeGpuFrame());
      QVERIFY(!rig.host.encoderRunning());
    }
  }
};

FB_TEST_MAIN(LoopbackTest)
#include "loopback_test.moc"
