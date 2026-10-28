// Diagnostics overlay model (0.6 D5-D11): formatting and persisted states, without Qt Quick.
#include <QtTest>

#include "diagnosticsmodel.h"
#include "gpudisplaybridge.h"
#include "gpuencodebridge.h"

using namespace uzume;
using namespace uzume::ui;

namespace {
EmulationDiagnostics sw() {
  EmulationDiagnostics d;
  d.valid = true;
  d.core = QStringLiteral("melonDS DS 1.4.0");
  d.frameSize = QSize(256, 384);
  d.baseSize = QSize(256, 384);
  d.screens = 2;
  d.cpuThreads = 8;
  d.fps = 59.9;
  d.targetFps = 59.83;
  d.frameMs = 6.2;
  d.emuMs = 6.2;
  d.audioActive = true;
  d.audioBufferMs = 82;
  return d;
}
}  // namespace

class DiagnosticsTest : public QObject {
  Q_OBJECT
 private slots:
  void softwareRows() {
    const QVariantMap m = DiagnosticsModel::emulationMap(sw());
    QCOMPARE(m.value("renderer").toString(), QStringLiteral("Software"));
    QCOMPARE(m.value("rendererSub").toString(), QStringLiteral("CPU · 8 threads"));
    QVERIFY(!m.value("fallback").toBool());
    QVERIFY(m.value("frame").toString().contains(QStringLiteral("no readback")));
    QVERIFY(m.value("fps").toString().startsWith(QStringLiteral("59.9 / 59.83")));
    QVERIFY(m.value("audio").toString().contains(QStringLiteral("0 underruns")));
  }

  void screenSizesSub() {
    // Equal screens: one size; the 3DS has a narrower bottom screen: each size is named (scaled like the frame).
    EmulationDiagnostics d = sw();
    d.screenSizes = {QSize(256, 192), QSize(256, 192)};
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("resolutionSub").toString(), QStringLiteral("2 screens of 256×192"));
    d.frameSize = d.baseSize = QSize(400, 480);
    d.screenSizes = {QSize(400, 240), QSize(320, 240)};
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("resolutionSub").toString(), QStringLiteral("Top 400×240 · bottom 320×240"));
    d.frameSize = QSize(800, 960);
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("resolutionSub").toString(), QStringLiteral("Top 800×480 · bottom 640×480"));
  }

  void hardwareRows() {
    EmulationDiagnostics d = sw();
    d.hwRequested = d.hwActive = true;
    d.api = QStringLiteral("OpenGL 4.6 Core");
    d.gpu = QStringLiteral("Example GPU · Driver 1.2.3");
    d.frameSize = QSize(768, 1152);
    d.readbackMs = 1.4;
    const QVariantMap m = DiagnosticsModel::emulationMap(d);
    QCOMPARE(m.value("renderer").toString(), d.api);
    QCOMPARE(m.value("rendererSub").toString(), d.gpu);
    QVERIFY(m.value("resolution").toString().startsWith(QStringLiteral("3×")));
    QVERIFY(m.value("frame").toString().contains(QStringLiteral("readback 1.4")));
  }

  void vulkanFallbackShowsInRendererSub() {
    EmulationDiagnostics d = sw();
    d.hwRequested = d.hwActive = true;
    d.api = QStringLiteral("OpenGL 4.5 Core");
    d.gpu = QStringLiteral("Example GPU");
    d.vulkanFallback = QStringLiteral("no Vulkan device");
    const QString sub = DiagnosticsModel::emulationMap(d).value("rendererSub").toString();
    QVERIFY2(sub.contains(QStringLiteral("Example GPU")), qPrintable(sub));
    QVERIFY2(sub.contains(QStringLiteral("Vulkan requested, not used (no Vulkan device)")), qPrintable(sub));
  }

  void fallbackRows() {
    EmulationDiagnostics d = sw();
    d.hwRequested = true;
    d.requestedScale = 3;
    d.fallbackReason = QStringLiteral("no context");
    const QVariantMap m = DiagnosticsModel::emulationMap(d);
    QVERIFY(m.value("fallback").toBool());
    QVERIFY(m.value("fallbackText").toString().contains(QStringLiteral("no context")));
    QVERIFY(m.value("resolutionSub").toString().contains(QStringLiteral("3× requested")));
  }

  void underrunsAreShown() {
    EmulationDiagnostics d = sw();
    d.underruns = 1;
    QVERIFY(DiagnosticsModel::emulationMap(d).value("audio").toString().contains(QStringLiteral("1 underrun")));
    QVERIFY(!DiagnosticsModel::emulationMap(d).value("audioSub").toString().isEmpty());
    d.audioActive = false;
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("audio").toString(), QStringLiteral("No audio output"));
  }

  void pausedShowsDashes() {
    EmulationDiagnostics d = sw();
    d.fps = 0;
    const QVariantMap m = DiagnosticsModel::emulationMap(d);
    QCOMPARE(m.value("frame").toString(), QStringLiteral("—"));
    QCOMPARE(m.value("summary").toString(), QStringLiteral("not running"));
  }

  void connectionPills() {
    using P = DiagnosticsModel;
    QCOMPARE(P::connectionPill(QString()).text, QStringLiteral("Connecting"));
    QCOMPARE(P::connectionPill(QStringLiteral("direct (host)")).tone, QStringLiteral("ok"));
    QCOMPARE(P::connectionPill(QStringLiteral("relay (udp)")).text, QStringLiteral("Relayed (TURN)"));
    QCOMPARE(P::connectionPill(QStringLiteral("relay (tcp)")).text, QStringLiteral("Relayed (TURN/TCP)"));
    QVERIFY(P::isRelayed(QStringLiteral("relay (udp)")));
    QVERIFY(!P::isRelayed(QStringLiteral("direct (srflx)")));
  }

  void hostAndViewerLines() {
    SessionStats s;
    QCOMPARE(DiagnosticsModel::hostLine(s), QStringLiteral("Encoder starts with the first viewer"));
    s.encoderName = QStringLiteral("h264_nvenc");
    s.videoBitrateKbps = 6000;
    s.targetBitrateKbps = 6500;
    s.fps = 60;
    QCOMPARE(DiagnosticsModel::hostLine(s), QStringLiteral("Encoder h264_nvenc · H.264 · 6.0 / target 6.5 Mbit/s · 60.0 fps"));
    QCOMPARE(DiagnosticsModel::hostSendingLine(1), QStringLiteral("Sending to 1 viewer · adaptive bitrate"));
    QCOMPARE(DiagnosticsModel::decoderLine(QStringLiteral("h264"), 3900.0, 59.7),
             QStringLiteral("Decoder h264 · H.264 · 3.9 Mbit/s · 59.7 fps"));
    QCOMPARE(DiagnosticsModel::decoderLine(QString(), std::nullopt, std::nullopt), QStringLiteral("Decoder — · H.264 · — · —"));
  }

  // ADR 0033 D5: the Stream row names the tiers in use and the viewers; the viewer side says what the host serves.
  void streamRowsNameTiersAndViewers() {
    SessionStats h;
    h.encoderName = QStringLiteral("libx264");
    QCOMPARE(DiagnosticsModel::hostDetailRows(h).size(), 1);  // no encoders counted yet: no Stream row
    h.encoders = 2;
    h.tiersInUse = QStringLiteral("Auto, Low");
    h.viewers = 2;
    const QVariantList rows = DiagnosticsModel::hostDetailRows(h);
    QCOMPARE(rows.last().toList().first().toString(), QStringLiteral("Stream"));
    QCOMPARE(rows.last().toList().last().toString(), QStringLiteral("Auto, Low · 2 viewers"));
    QCOMPARE(DiagnosticsModel::hostSendingLine(2, 2), QStringLiteral("Sending to 2 viewers · 2 streams"));
    QCOMPARE(DiagnosticsModel::hostSendingLine(2, 1), QStringLiteral("Sending to 2 viewers · adaptive bitrate"));
    SessionStats v;
    v.hasServedTier = true;
    v.servedTier = Quality::Medium;
    v.servedKbps = 1500;
    v.pick = Quality::Medium;
    QCOMPARE(DiagnosticsModel::viewerDetailRows(v).last().toList().last().toString(), QStringLiteral("picked Medium · gets Medium · 1.5 Mbit/s"));
    v.pick = Quality::Auto;
    v.servedTier = Quality::Auto;
    v.servedKbps = 4000;
    QCOMPARE(DiagnosticsModel::viewerDetailRows(v).last().toList().last().toString(), QStringLiteral("Auto · gets High · 4.0 Mbit/s"));
  }

  // ADR 0019: the host line names the GPU-direct path, or says that it was tried and is off. Without either, the string is
  // exactly the one above.
  void hostLineNamesTheGpuPath() {
    SessionStats s;
    s.encoderName = QStringLiteral("h264_nvenc");
    s.videoBitrateKbps = 6000;
    s.targetBitrateKbps = 6500;
    s.fps = 60;
    const QString base = QStringLiteral("Encoder h264_nvenc · H.264 · 6.0 / target 6.5 Mbit/s · 60.0 fps");
    QCOMPARE(DiagnosticsModel::hostLine(s), base);
    s.gpuInput = true;
    QCOMPARE(DiagnosticsModel::hostLine(s), base + QStringLiteral(" · GPU-direct"));
    s.gpuInput = false;
    s.gpuInputFailed = true;
    QCOMPARE(DiagnosticsModel::hostLine(s), base + QStringLiteral(" · readback (GPU-direct off)"));
    s.gpuInput = true;  // cannot be both in practice; GPU-direct wins
    QCOMPARE(DiagnosticsModel::hostLine(s), base + QStringLiteral(" · GPU-direct"));
    SessionStats idle;  // no encoder yet: the suffixes do not apply
    idle.gpuInputFailed = true;
    QCOMPARE(DiagnosticsModel::hostLine(idle), QStringLiteral("Encoder starts with the first viewer"));
  }

  // The frame row gets the copy into the Session encode texture only while frames are captured.
  void frameRowShowsTheGpuCopy() {
    EmulationDiagnostics d = sw();
    d.hwRequested = d.hwActive = true;
    d.api = QStringLiteral("OpenGL 4.6 Core");
    d.gpu = QStringLiteral("Example GPU · Driver 1.2.3");
    d.frameMs = 9.4;
    d.emuMs = 7.1;
    d.readbackMs = 0.4;
    d.readbacksPerSec = 60;
    const QString base = QStringLiteral("9.4 ms · emu 7.1 · readback 0.4 (60/s)");
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("frame").toString(), base);
    d.gpuCopyMs = 0.3;
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("frame").toString(), base);  // copy time without captures: unchanged
    d.gpuCopiesPerSec = 60;
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("frame").toString(), base + QStringLiteral(" · GPU copy 0.3 (60/s)"));
    d.gpuCopiesPerSec = 29.6;
    QVERIFY(DiagnosticsModel::emulationMap(d).value("frame").toString().endsWith(QStringLiteral(" · GPU copy 0.3 (30/s)")));
    d.fps = 0;  // paused: the dash stays
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("frame").toString(), QStringLiteral("—"));
  }

  // Zero-copy display (ADR 0027, D7): the zero-copy part replaces the readback part while none runs.
  void frameRowShowsZeroCopy() {
    EmulationDiagnostics d = sw();
    d.hwRequested = d.hwActive = true;
    d.api = QStringLiteral("OpenGL 4.6 Core");
    d.gpu = QStringLiteral("Example GPU · Driver 1.2.3");
    d.frameMs = 9.4;
    d.emuMs = 7.1;
    d.displayMs = 0.2;
    d.displaysPerSec = 60;
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("frame").toString(), QStringLiteral("9.4 ms · emu 7.1 · zero-copy 0.2 (60/s)"));
    d.readbackMs = 1.1;  // both (not yet confirmed, or a shared Session without GPU-direct encoding)
    d.readbacksPerSec = 60;
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("frame").toString(),
             QStringLiteral("9.4 ms · emu 7.1 · readback 1.1 (60/s) · zero-copy 0.2 (60/s)"));
    d.gpuCopyMs = 0.3;  // the GPU copy part stays where it was
    d.gpuCopiesPerSec = 60;
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("frame").toString(),
             QStringLiteral("9.4 ms · emu 7.1 · readback 1.1 (60/s) · zero-copy 0.2 (60/s) · GPU copy 0.3 (60/s)"));
    d.readbacksPerSec = 0;
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("frame").toString(),
             QStringLiteral("9.4 ms · emu 7.1 · zero-copy 0.2 (60/s) · GPU copy 0.3 (60/s)"));
    d.displaysPerSec = 0;  // no zero-copy frames: the old row, bit for bit
    d.readbacksPerSec = 60;
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("frame").toString(),
             QStringLiteral("9.4 ms · emu 7.1 · readback 1.1 (60/s) · GPU copy 0.3 (60/s)"));
  }

  void resolutionRowShowsTheZeroCopySize() {
    EmulationDiagnostics d = sw();
    d.hwRequested = d.hwActive = true;
    d.frameSize = QSize(768, 1152);
    const QString base = QStringLiteral("3× · 768×1152");
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("resolution").toString(), base);
    d.readbackSize = QSize(512, 768);  // a readback ran
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("resolution").toString(), base + QStringLiteral(" → read back 512×768"));
    d.zeroCopy = true;
    d.displaySize = QSize(640, 960);  // both: the readback size is the one shown
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("resolution").toString(), base + QStringLiteral(" → read back 512×768"));
    d.readbackSize = QSize();         // zero-copy without readback
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("resolution").toString(), base + QStringLiteral(" → shown 640×960"));
    d.displaySize = QSize();          // shown at full size: nothing to add
    QCOMPARE(DiagnosticsModel::emulationMap(d).value("resolution").toString(), base);
  }

  // planReadback: written out by cases (view limit, share size) x display active.
  void planReadbackTruthTable() {
    const QSize none;
    const QSize view(640, 480), shareBig(1280, 1920), shareSmall(320, 240);
    struct Row {
      bool active;
      QSize view, share;
      bool readback;
      QSize readbackLimit;
    };
    const Row rows[] = {
        // display not (yet) confirmed: today's rule - the max of the views, expanded to the share size when a view exists
        {false, none, none, true, none},
        {false, none, shareBig, true, none},  // no view reported: unlimited
        {false, view, none, true, view},
        {false, view, shareBig, true, QSize(1280, 1920)},
        {false, view, shareSmall, true, view},
        {false, QSize(2000, 100), shareBig, true, QSize(2000, 1920)},  // component-wise
        // display active, nobody needs pixels: no readback
        {true, none, none, false, none},
        {true, view, none, false, none},
        // display active, shared without GPU-direct encoding: readback for the Session at the share size
        {true, view, shareBig, true, shareBig},
        {true, view, shareSmall, true, shareSmall},  // the view needs none of it: share size only
        {true, none, shareBig, true, shareBig},
    };
    int n = 0;
    for (const Row& r : rows) {
      const ReadbackPlan p = planReadback(r.active, r.view, r.share);
      const QString row = QStringLiteral("row %1: active=%2 view=%3x%4 share=%5x%6").arg(n).arg(r.active).arg(r.view.width()).arg(r.view.height())
                              .arg(r.share.width()).arg(r.share.height());
      QVERIFY2(p.readback == r.readback, qPrintable(row));
      QVERIFY2(p.readbackLimit == r.readbackLimit, qPrintable(row));
      QVERIFY2(p.displayLimit == r.view, qPrintable(row));  // the display blit follows the views, always
      ++n;
    }
    QCOMPARE(n, 11);
  }

  // planGpuEncode over all 32 combinations: written out by cases, not by the formula of the implementation.
  void planGpuEncodeTruthTable() {
    int rows = 0, keep = 0, create = 0, wanted = 0, emptyShare = 0, maxShare = 0;
    for (int bits = 0; bits < 32; ++bits) {
      const bool shared = bits & 16, hw = bits & 8, running = bits & 4, allowed = bits & 2, active = bits & 1;
      const GpuEncodePlan p = planGpuEncode(shared, hw, running, allowed, active);
      const QString row = QStringLiteral("shared=%1 hw=%2 running=%3 allowed=%4 active=%5").arg(shared).arg(hw).arg(running).arg(allowed).arg(active);
      ++rows;
      bool expectKeep = false, expectCreate = false;
      QSize expectSize;  // not shared: no share size at all
      if (shared) {
        expectSize = active ? QSize() : kShareEncodeMax;  // GPU-direct active: nothing needs reading back for the encoder
        if (hw && allowed) {
          expectKeep = true;
          expectCreate = running;  // the bridge exists only while the encoder runs ...
        }
      }
      QVERIFY2(p.keep == expectKeep, qPrintable(row));
      QVERIFY2(p.create == expectCreate, qPrintable(row));
      QVERIFY2(p.wanted == expectCreate, qPrintable(row));  // ... and produces frames exactly then
      QVERIFY2(p.shareSize == expectSize, qPrintable(row));
      keep += p.keep;
      create += p.create;
      wanted += p.wanted;
      (p.shareSize.isEmpty() ? emptyShare : maxShare) += 1;
    }
    QCOMPARE(rows, 32);
    QCOMPARE(keep, 4);
    QCOMPARE(create, 2);
    QCOMPARE(wanted, 2);
    QCOMPARE(emptyShare, 24);  // 16 unshared + 8 shared with GPU-direct active
    QCOMPARE(maxShare, 8);
    QCOMPARE(kShareEncodeMax, QSize(1280, 1920));
  }

  void linkLineAndTurn() {
    QCOMPARE(DiagnosticsModel::linkLine(48.0, 0.4, QStringLiteral("direct (host)"), QString()), QStringLiteral("RTT 48 ms · Loss 0.4 %"));
    QCOMPARE(DiagnosticsModel::linkLine(std::nullopt, std::nullopt, QStringLiteral("relay (udp)"), QStringLiteral("hub.example.com:3478")),
             QStringLiteral("RTT — · Loss — · via hub.example.com:3478"));
    // A relayed link names the reason; direct links and RelayReason::None stay unchanged.
    QCOMPARE(DiagnosticsModel::linkLine(10.0, 0.0, QStringLiteral("relay (udp)"), QStringLiteral("hub.example.com:3478"), RelayReason::NoLocalSrflx),
             QStringLiteral("RTT 10 ms · Loss 0.0 % · via hub.example.com:3478 · no STUN reachable on this side"));
    QCOMPARE(DiagnosticsModel::linkLine(10.0, 0.0, QStringLiteral("relay (tcp)"), QString(), RelayReason::Forced),
             QStringLiteral("RTT 10 ms · Loss 0.0 % · relay forced"));
    QCOMPARE(DiagnosticsModel::linkLine(10.0, 0.0, QStringLiteral("direct (srflx)"), QString(), RelayReason::DirectFailed),
             QStringLiteral("RTT 10 ms · Loss 0.0 %"));
    QVERIFY(DiagnosticsModel::relayReasonText(RelayReason::None).isEmpty());
    QVERIFY(!DiagnosticsModel::relayReasonText(RelayReason::NoRemoteSrflx).isEmpty());
    QVERIFY(!DiagnosticsModel::relayReasonText(RelayReason::DirectFailed).isEmpty());
    TurnServer t;
    t.urls = {QStringLiteral("turn:hub.example.com:3478?transport=udp")};
    QCOMPARE(DiagnosticsModel::turnEndpoint({t}), QStringLiteral("hub.example.com:3478"));
    QVERIFY(DiagnosticsModel::turnEndpoint({}).isEmpty());
  }

  void detailsFieldsAndRows() {
    // Link values of the Details table exist only where they are measured
    const QVariantMap relay = DiagnosticsModel::linkFields(64.0, 1.2, QStringLiteral("relay (udp)"), RelayReason::DirectFailed);
    QCOMPARE(relay.value("path").toString(), QStringLiteral("Relay"));
    QCOMPARE(relay.value("rtt").toString(), QStringLiteral("64 ms"));
    QCOMPARE(relay.value("loss").toString(), QStringLiteral("1.2 %"));
    QVERIFY(relay.value("reason").toString().startsWith(QStringLiteral("Relay because")));
    QVERIFY(relay.value("reason").toString().endsWith(QLatin1Char('.')));
    const QVariantMap direct = DiagnosticsModel::linkFields(18.0, 0.1, QStringLiteral("direct (srflx)"), RelayReason::DirectFailed);
    QCOMPARE(direct.value("path").toString(), QStringLiteral("Direct"));
    QVERIFY(direct.value("reason").toString().isEmpty());   // a direct path has no relay reason
    const QVariantMap unknown = DiagnosticsModel::linkFields(std::nullopt, std::nullopt, QString());
    QVERIFY(unknown.value("path").toString().isEmpty() && unknown.value("rtt").toString().isEmpty() && unknown.value("loss").toString().isEmpty());
    QVERIFY(DiagnosticsModel::relayReasonSentence(RelayReason::None).isEmpty());
    for (RelayReason r : {RelayReason::Forced, RelayReason::NoLocalSrflx, RelayReason::NoRemoteSrflx, RelayReason::DirectFailed})
      QVERIFY(DiagnosticsModel::relayReasonSentence(r).endsWith(QLatin1Char('.')));

    SessionStats host;
    host.encoderName = QStringLiteral("h264_nvenc");
    host.gpuInput = true;
    host.videoBitrateKbps = 8200;
    host.width = 1024;
    host.height = 768;
    host.fps = 60.0;
    const QVariantList rows = DiagnosticsModel::hostDetailRows(host);
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows.at(0).toList(), (QVariantList{QStringLiteral("Encoder"), QStringLiteral("h264_nvenc · H.264 · GPU-direct")}));
    QCOMPARE(rows.at(1).toList(), (QVariantList{QStringLiteral("Bitrate"), QStringLiteral("8.2 Mbit/s")}));
    QCOMPARE(rows.at(2).toList(), (QVariantList{QStringLiteral("Resolution"), QStringLiteral("1024 × 768 · 60 fps")}));
    QVERIFY(DiagnosticsModel::hostDetailRows(SessionStats()).isEmpty());   // nothing measured: no rows
    SessionStats viewer;
    viewer.decoderName = QStringLiteral("h264");
    QCOMPARE(DiagnosticsModel::viewerDetailRows(viewer).size(), 1);
    // participant() merges the link values
    const QVariantMap p = DiagnosticsModel::participant(QStringLiteral("Jonas"), QStringLiteral("viewer"), {QStringLiteral("Relayed (TURN)"), QStringLiteral("warn")}, {}, {}, relay);
    QCOMPARE(p.value("path").toString(), QStringLiteral("Relay"));
    QCOMPARE(p.value("name").toString(), QStringLiteral("Jonas"));
  }

  void streamingSummaryCountsRelayed() {
    DiagnosticsModel m;
    QCOMPARE(m.participantCount(), 0);
    const auto ok = DiagnosticsModel::participant(QStringLiteral("A"), QStringLiteral("viewer"), {QStringLiteral("Direct"), QStringLiteral("ok")}, {}, {});
    const auto rl = DiagnosticsModel::participant(QStringLiteral("B"), QStringLiteral("viewer"), {QStringLiteral("Relayed (TURN)"), QStringLiteral("warn")}, {}, {});
    m.setStreaming({QVariantMap{{"participants", QVariantList{ok, rl}}}});
    QCOMPARE(m.participantCount(), 2);
    QCOMPARE(m.streamingSummary(), QStringLiteral("2 · 1 relayed"));
  }

  void statesToggle() {
    DiagnosticsModel m;
    QSignalSpy spy(&m, &DiagnosticsModel::statesChanged);
    QVERIFY(!m.isOpen());
    m.toggle();
    QVERIFY(m.isOpen());
    QVERIFY(m.emulationOpen());
    m.toggleEmulation();
    QVERIFY(!m.emulationOpen());
    QCOMPARE(spy.count(), 2);
  }
};

QTEST_GUILESS_MAIN(DiagnosticsTest)
#include "diagnostics_test.moc"
