// Controller hand-over (ADR 0033 D8): input frames, DataChannel messages and the host arbitration with a fake clock.
#include <QtTest>

#include "controlarbiter.h"
#include "controlproto.h"

using namespace uzume;

namespace {
InputFrame frame(int port, quint16 seq, quint16 ts, quint16 buttons = 0) {
  InputFrame f;
  f.port = static_cast<quint8>(port);
  f.seq = seq;
  f.tsMs = ts;
  f.buttons = buttons;
  return f;
}
}  // namespace

class ControlTest : public QObject {
  Q_OBJECT
 private slots:
  // ---- frames
  void frameRoundTrip() {
    InputFrame f = frame(1, 65535, 65530, 0xA5C3);
    f.axes[0] = -32768;
    f.axes[1] = 32767;
    f.axes[2] = -1;
    f.axes[3] = 1234;
    f.touchX = 32767;
    f.touchY = 17;
    f.touching = true;
    const QByteArray b = encodeInputFrame(f);
    QCOMPARE(b.size(), kInputFrameSize);
    const auto back = decodeInputFrame(b);
    QVERIFY(back.has_value());
    QVERIFY(*back == f);
    const auto zero = decodeInputFrame(encodeInputFrame(InputFrame()));
    QVERIFY(zero && *zero == InputFrame());
  }
  void garbageFramesAreRejected() {
    const QByteArray good = encodeInputFrame(frame(0, 1, 2, 3));
    QVERIFY(!decodeInputFrame({}));
    QVERIFY(!decodeInputFrame(good.left(kInputFrameSize - 1)));
    QVERIFY(!decodeInputFrame(good + 'x'));
    QByteArray b = good;
    b[0] = 2;  // version
    QVERIFY(!decodeInputFrame(b));
    b = good;
    b[1] = 2;  // port
    QVERIFY(!decodeInputFrame(b));
    b = good;
    b[20] = 2;  // unknown flag
    QVERIFY(!decodeInputFrame(b));
    b = good;
    b[17] = static_cast<char>(0x80);  // negative touch x
    QVERIFY(!decodeInputFrame(b));
    QVERIFY(!decodeInputFrame(QByteArray(kInputFrameSize, '\xff')));
  }
  void maskCarriesSticks() {
    InputFrame f = frame(0, 1, 1, 0x0008);
    f.axes[0] = 30000;   // left stick right
    f.axes[3] = -30000;  // right stick up
    QCOMPARE(inputFrameMask(f), quint32(0x0008 | (1u << 23) | (1u << 16)));
    f.axes[0] = 1000;
    QCOMPARE(inputFrameMask(f) & (1u << 23), 0u);
  }

  // ---- messages
  void messagesRoundTripAndRejectGarbage() {
    QCOMPARE(parseControlRequest(makeControlRequest(1)), std::optional<int>(1));
    QVERIFY(!parseControlRequest(R"({"t":"control_request","port":2})"));
    QVERIFY(!parseControlRequest(R"({"t":"control_request","port":0.5})"));
    QVERIFY(!parseControlRequest(R"({"t":"control_request","port":"0"})"));
    QVERIFY(!parseControlRequest("not json"));
    QVERIFY(!parseControlRequest(makeControlRelease()));
    const auto a = parseControlAnswer(makeControlAnswer(0, false, QStringLiteral("busy")));
    QVERIFY(a && a->port == 0 && !a->granted && a->reason == QLatin1String("busy"));
    const auto g = parseControlAnswer(makeControlAnswer(1, true));
    QVERIFY(g && g->granted && g->reason.isEmpty());
    QVERIFY(!parseControlAnswer(R"({"t":"control","port":0,"granted":"yes"})"));
    const auto e = parseControlEnd(makeControlEnd(0, QStringLiteral("left")));
    QVERIFY(e && e->port == 0 && e->reason == QLatin1String("left"));
    QVERIFY(!parseControlEnd(makeControlEnd(0, QStringLiteral("whatever"))));
    QVERIFY(parseControlRelease(makeControlRelease()));
    QVERIFY(!parseControlRelease(R"({"t":"control"})"));
    QCOMPARE(parseControlCaps(makeControlCaps(2)), std::optional<int>(2));
    QVERIFY(!parseControlCaps(R"({"t":"control_caps","ports":3})"));
  }

  // ---- arbitration
  void grantDenyAndTimeout() {
    ControlArbiter a;
    a.setPortCount(2);
    QCOMPARE(a.request(QStringLiteral("v1"), 1, 1000).kind, ControlArbiter::Request::Pending);
    QCOMPARE(a.request(QStringLiteral("v2"), 1, 1100).reason, QStringLiteral("busy"));  // somebody asked first
    QCOMPARE(a.request(QStringLiteral("v1"), 0, 1100).reason, QStringLiteral("holding"));
    const auto ans = a.answer(QStringLiteral("v1"), true, 2000);
    QVERIFY(ans.known && ans.granted);
    QCOMPARE(ans.port, 1);
    QCOMPARE(a.holder(1), QStringLiteral("v1"));
    QCOMPARE(a.request(QStringLiteral("v2"), 1, 2100).reason, QStringLiteral("busy"));  // held
    QCOMPARE(a.request(QStringLiteral("v2"), 0, 2100).kind, ControlArbiter::Request::Pending);
    const auto deny = a.answer(QStringLiteral("v2"), false, 2200);
    QVERIFY(deny.known && !deny.granted);
    QVERIFY(!a.answer(QStringLiteral("v2"), true, 2300).known);  // already answered
    // timeout
    QCOMPARE(a.request(QStringLiteral("v3"), 0, 10000).kind, ControlArbiter::Request::Pending);
    QVERIFY(a.expire(10000 + kControlAskTimeoutMs - 1).isEmpty());
    const auto ex = a.expire(10000 + kControlAskTimeoutMs);
    QCOMPARE(ex.size(), 1);
    QCOMPARE(ex.first().viewerId, QStringLiteral("v3"));
    QVERIFY(a.pending().isEmpty());
  }
  void unsupportedPorts() {
    ControlArbiter a;
    QCOMPARE(a.request(QStringLiteral("v"), 0, 0).reason, QStringLiteral("unsupported"));  // no hand-over
    a.setPortCount(1);
    QCOMPARE(a.request(QStringLiteral("v"), 1, 0).reason, QStringLiteral("unsupported"));  // one-port system: no P2
    QCOMPARE(a.request(QStringLiteral("v"), 0, 0).kind, ControlArbiter::Request::Pending);
  }
  void releaseOnDisconnectAndTakeBack() {
    ControlArbiter a;
    a.setPortCount(2);
    a.request(QStringLiteral("v1"), 0, 0);
    a.answer(QStringLiteral("v1"), true, 0);
    const auto e = a.release(QStringLiteral("v1"), QStringLiteral("left"));
    QVERIFY(e && e->port == 0 && e->reason == QLatin1String("left"));
    QVERIFY(a.holder(0).isEmpty());
    QVERIFY(!a.release(QStringLiteral("v1"), QStringLiteral("left")));
    a.request(QStringLiteral("v2"), 1, 0);
    a.answer(QStringLiteral("v2"), true, 0);
    const auto t = a.releasePort(1, QStringLiteral("host"));
    QVERIFY(t && t->viewerId == QLatin1String("v2"));
    a.request(QStringLiteral("v3"), 0, 0);  // pending request withdrawn by leaving
    QVERIFY(!a.release(QStringLiteral("v3"), QStringLiteral("left")));
    QVERIFY(a.pending().isEmpty());
    a.request(QStringLiteral("v4"), 0, 0);
    a.answer(QStringLiteral("v4"), true, 0);
    QCOMPARE(a.setPortCount(1).size(), 0);
    QCOMPARE(a.releaseAll(QStringLiteral("host")).size(), 1);
  }
  void staleAndOutOfOrderFrames() {
    ControlArbiter a;
    a.setPortCount(2);
    a.request(QStringLiteral("v"), 0, 0);
    a.answer(QStringLiteral("v"), true, 0);
    QCOMPARE(a.accept(QStringLiteral("nobody"), frame(0, 1, 100), 100), ControlArbiter::Verdict::NoGrant);
    QCOMPARE(a.accept(QStringLiteral("v"), frame(1, 1, 100), 100), ControlArbiter::Verdict::WrongPort);
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 10, 100, 0x10), 105), ControlArbiter::Verdict::Applied);
    QCOMPARE(a.input(0).buttons, quint16(0x10));
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 10, 110, 0x20), 115), ControlArbiter::Verdict::OutOfOrder);  // same seq
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 9, 110, 0x20), 115), ControlArbiter::Verdict::OutOfOrder);   // older
    QCOMPARE(a.input(0).buttons, quint16(0x10));
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 11, 120, 0x20), 125), ControlArbiter::Verdict::Applied);
    // a frame that spent more than 250 ms in the network (its sender stamp is 300 ms behind the path seen so far)
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 12, 130, 0x40), 130 + 300), ControlArbiter::Verdict::Stale);
    QCOMPARE(a.input(0).buttons, quint16(0x20));
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 13, 500, 0x40), 500 + 200), ControlArbiter::Verdict::Applied);  // within the limit
    // sequence wrap-around
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 30000, 690, 1), 695), ControlArbiter::Verdict::Applied);
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 60000, 695, 1), 700), ControlArbiter::Verdict::Applied);
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 65535, 700, 1), 705), ControlArbiter::Verdict::Applied);
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 0, 710, 2), 715), ControlArbiter::Verdict::Applied);
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 65535, 720, 3), 725), ControlArbiter::Verdict::OutOfOrder);
  }
  // A delay that grew for good (longer route, clock drift) must not make every later frame stale: after 3 s the floor moves.
  void sustainedDelayGrowthRebases() {
    ControlArbiter a;
    a.setPortCount(1);
    a.request(QStringLiteral("v"), 0, 0);
    a.answer(QStringLiteral("v"), true, 0);
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 1, 100), 105), ControlArbiter::Verdict::Applied);  // path floor: 5 ms
    quint16 seq = 2;
    qint64 t = 200;
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, seq++, static_cast<quint16>(t)), t + 400), ControlArbiter::Verdict::Stale);
    for (; t < 3200; t += 100) {  // the run of stale frames started at arrival 600 and lasts 3 s
      QCOMPARE(a.accept(QStringLiteral("v"), frame(0, seq++, static_cast<quint16>(t)), t + 400), ControlArbiter::Verdict::Stale);
    }
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, seq++, static_cast<quint16>(t)), t + 400), ControlArbiter::Verdict::Applied);
    t += 100;
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, seq++, static_cast<quint16>(t)), t + 410), ControlArbiter::Verdict::Applied);
    // a real spike after that is still judged against the new floor
    t += 100;
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, seq++, static_cast<quint16>(t)), t + 900), ControlArbiter::Verdict::Stale);
  }
  void loweringPortsReportsDroppedRequests() {
    ControlArbiter a;
    a.setPortCount(2);
    a.request(QStringLiteral("v1"), 1, 0);
    a.request(QStringLiteral("v2"), 0, 0);
    QList<ControlArbiter::Pending> dropped;
    a.setPortCount(1, &dropped);
    QCOMPARE(dropped.size(), 1);
    QCOMPARE(dropped.first().viewerId, QStringLiteral("v1"));
    QVERIFY(a.hasPending(QStringLiteral("v2")));
    a.setPortCount(0, &dropped);
    QCOMPARE(dropped.size(), 2);
    QVERIFY(a.pending().isEmpty());
  }
  void timestampWrapIsNotStale() {
    ControlArbiter a;
    a.setPortCount(1);
    a.request(QStringLiteral("v"), 0, 0);
    a.answer(QStringLiteral("v"), true, 0);
    const qint64 t0 = 65536 * 3 - 20;  // sender stamps wrap while the host clock keeps counting
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 1, static_cast<quint16>(t0 & 0xFFFF)), t0 + 5), ControlArbiter::Verdict::Applied);
    QCOMPARE(a.accept(QStringLiteral("v"), frame(0, 2, static_cast<quint16>((t0 + 40) & 0xFFFF)), t0 + 45), ControlArbiter::Verdict::Applied);
  }
  void neutralAfterHalfASecond() {
    ControlArbiter a;
    a.setPortCount(2);
    a.request(QStringLiteral("v"), 1, 0);
    a.answer(QStringLiteral("v"), true, 1000);
    QVERIFY(a.neutral(1));  // a fresh grant starts neutral
    QCOMPARE(a.accept(QStringLiteral("v"), frame(1, 1, 1000, 0xFF), 1010), ControlArbiter::Verdict::Applied);
    QVERIFY(!a.neutral(1));
    QVERIFY(a.expireNeutral(1010 + kInputNeutralMs - 1).isEmpty());
    QCOMPARE(a.expireNeutral(1010 + kInputNeutralMs), QList<int>{1});
    QVERIFY(a.neutral(1));
    QCOMPARE(a.input(1).buttons, quint16(0));
    QVERIFY(a.expireNeutral(5000).isEmpty());  // reported once
    QCOMPARE(a.holder(1), QStringLiteral("v"));  // the grant stays
    QCOMPARE(a.accept(QStringLiteral("v"), frame(1, 2, 3000, 0x1), 3005), ControlArbiter::Verdict::Applied);  // and works again
    QVERIFY(!a.neutral(1));
  }
};

QTEST_APPLESS_MAIN(ControlTest)
#include "control_test.moc"
