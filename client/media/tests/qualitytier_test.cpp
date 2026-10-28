// Quality per viewer (ADR 0033 D5): DataChannel messages, RTT from pongs and the tier manager (fake clock).
#include <QtTest>

#include "qualitytier.h"
#include "tiermanager.h"

using namespace uzume;

class QualityTierTest : public QObject {
  Q_OBJECT
 private slots:
  // ---- messages
  void qualityPickRoundTrip() {
    for (const Quality q : {Quality::Auto, Quality::High, Quality::Medium, Quality::Low}) {
      const auto parsed = parseQualityPick(makeQualityPick(q));
      QVERIFY(parsed.has_value());
      QCOMPARE(*parsed, q);
    }
    QCOMPARE(makeQualityPick(Quality::Low), QByteArray(R"({"pick":"low","t":"quality"})"));
  }
  void qualityPickRejectsGarbage() {
    QVERIFY(!parseQualityPick("").has_value());
    QVERIFY(!parseQualityPick("not json").has_value());
    QVERIFY(!parseQualityPick("[1,2]").has_value());
    QVERIFY(!parseQualityPick(R"({"t":"quality"})").has_value());
    QVERIFY(!parseQualityPick(R"({"t":"quality","pick":"ultra"})").has_value());
    QVERIFY(!parseQualityPick(R"({"t":"quality","pick":3})").has_value());
    QVERIFY(!parseQualityPick(R"({"t":"quality","pick":"High"})").has_value());  // tokens are lower case
    QVERIFY(!parseQualityPick(R"({"t":"rx","pick":"low"})").has_value());
    QVERIFY(!parseQualityPick(QByteArray(600, 'x')).has_value());
    QVERIFY(parseQualityPick(R"({"t":"quality","pick":"low","future":{"a":1}})").has_value());  // unknown fields are ignored
  }
  void pingPong() {
    const auto ping = parsePingPong(makePing(7, 123456));
    QVERIFY(ping.has_value());
    QVERIFY(!ping->pong);
    QCOMPARE(ping->n, 7);
    QCOMPARE(ping->ts, qint64(123456));
    const auto pong = parsePingPong(makePong(7, 123456));
    QVERIFY(pong.has_value() && pong->pong);
    QVERIFY(!parsePingPong(R"({"t":"ping","n":"x","ts":1})").has_value());
    QVERIFY(!parsePingPong(R"({"t":"ping","n":1})").has_value());
    QVERIFY(!parsePingPong(R"({"t":"ping","n":1.5,"ts":1})").has_value());
    QVERIFY(!parsePingPong(R"({"t":"ping","n":-1,"ts":1})").has_value());
    QVERIFY(!parsePingPong(R"({"t":"pang","n":1,"ts":1})").has_value());
    QVERIFY(!parsePingPong("{").has_value());
  }
  void rttIsNowMinusStamp() {
    QCOMPARE(*rttFromPong(1018, 1000), 18.0);
    QCOMPARE(*rttFromPong(1000, 1000), 0.0);
    QVERIFY(!rttFromPong(900, 1000).has_value());      // stamp from the future
    QVERIFY(!rttFromPong(100000, 1000).has_value());   // stale
  }
  void tierMessage() {
    const auto t = parseTier(makeTier(Quality::Medium, 1500));
    QVERIFY(t.has_value());
    QCOMPARE(t->tier, Quality::Medium);
    QCOMPARE(t->kbps, 1500);
    QVERIFY(!parseTier(R"({"t":"tier","tier":"high"})").has_value());
    QVERIFY(!parseTier(R"({"t":"tier","tier":"x","kbps":1})").has_value());
    QVERIFY(!parseTier(R"({"t":"tier","tier":"low","kbps":-5})").has_value());
    QVERIFY(!parseTier(R"({"t":"state","paused":true})").has_value());
  }
  void unknownMessagesAreNotAnyKnownType() {
    const QByteArray unknown = R"({"t":"emote","v":1})";
    QVERIFY(!parseQualityPick(unknown) && !parsePingPong(unknown) && !parseTier(unknown));
  }
  void kbpsNames() {
    QCOMPARE(qualityKbps(Quality::High), 4000);
    QCOMPARE(qualityKbps(Quality::Medium), 1500);
    QCOMPARE(qualityKbps(Quality::Low), 600);
    QCOMPARE(qualityForKbps(4000), Quality::High);
    QCOMPARE(qualityForKbps(2000), Quality::Medium);
    QCOMPARE(qualityForKbps(300), Quality::Low);
  }

  // ---- tier manager
  void autoOnlyIsOneAdaptiveEncoder() {
    TierManager m;
    m.addViewer("a", 0);
    m.addViewer("b", 0);
    QCOMPARE(m.encoderCount(), 1);
    QVERIFY(m.adaptiveAlive());
    QCOMPARE(m.fixedMask(), 0);
    QCOMPARE(m.effective("a"), Quality::Auto);
    QVERIFY(!m.tick(60000));  // nothing idle
    QCOMPARE(m.encoderCount(), 1);
  }
  void fixedTierIsCreatedLazilyAndSwitchRequestsKeyframe() {
    TierManager m;
    m.addViewer("a", 0);
    m.addViewer("b", 0);
    QVERIFY(!m.alive(Quality::Low));
    const auto ch = m.setPick("b", Quality::Low, 1000);
    QVERIFY(ch.switched);
    QCOMPARE(ch.effective, Quality::Low);
    QVERIFY(m.alive(Quality::Low));
    QCOMPARE(m.encoderCount(), 2);
    QCOMPARE(m.fixedMask(), 1 << int(Quality::Low));
    QVERIFY(m.isAuto("a"));
    QVERIFY(!m.isAuto("b"));
    QVERIFY(!m.setPick("b", Quality::Low, 1100).switched);  // same pick again: nothing to do
  }
  void idleEncoderIsDroppedTenSecondsAfterTheLastViewerLeft() {
    TierManager m;
    m.addViewer("a", 0);
    m.addViewer("b", 0);
    m.setPick("b", Quality::Low, 1000);
    m.setPick("b", Quality::Auto, 5000);  // Low is empty from 5000
    QVERIFY(m.alive(Quality::Low));
    QVERIFY(!m.tick(14999));
    QVERIFY(m.alive(Quality::Low));
    QVERIFY(m.tick(15000));
    QVERIFY(!m.alive(Quality::Low));
    QCOMPARE(m.encoderCount(), 1);
  }
  void returningInsideTheGraceKeepsTheEncoder() {
    TierManager m;
    m.addViewer("a", 0);
    m.setPick("a", Quality::Medium, 0);
    QVERIFY(!m.tick(0));
    m.removeViewer("a", 1000);
    QVERIFY(m.alive(Quality::Medium));  // still alive, idle
    m.addViewer("c", 5000);
    m.setPick("c", Quality::Medium, 6000);
    m.tick(20000);  // in use again: not dropped (the idle Auto encoder is)
    QVERIFY(m.alive(Quality::Medium));
  }
  void adaptiveEncoderIdlesWhenNobodyIsOnAuto() {
    TierManager m;
    m.addViewer("a", 0);
    m.setPick("a", Quality::High, 0);
    QVERIFY(m.adaptiveAlive());  // grace
    QVERIFY(m.tick(10000));
    QVERIFY(!m.adaptiveAlive());
    QCOMPARE(m.encoderCount(), 1);
    QCOMPARE(m.fixedMask(), 1 << int(Quality::High));
  }
  void atMostThreeEncoders() {
    TierManager m;
    m.addViewer("auto", 0);
    m.addViewer("h", 0);
    m.addViewer("m", 0);
    m.addViewer("l", 0);
    m.setPick("h", Quality::High, 0);
    m.setPick("m", Quality::Medium, 0);
    QCOMPARE(m.encoderCount(), 3);  // Auto, High, Medium
    const auto ch = m.setPick("l", Quality::Low, 0);
    QCOMPARE(m.encoderCount(), 3);
    QVERIFY(!m.alive(Quality::Low));
    QCOMPARE(ch.effective, Quality::Medium);  // closest alive bitrate to 600 kbit/s among Auto (2000), High, Medium
    QCOMPARE(m.pick("l"), Quality::Low);
    QCOMPARE(m.effective("l"), Quality::Medium);
    // Room appears: the viewer moves to its pick.
    m.setPick("m", Quality::Auto, 1000);
    m.setPick("l", Quality::Low, 1000);  // m left Medium but l sits on it; Medium still alive
    m.removeViewer("h", 2000);
    QVERIFY(m.tick(12000));  // High idle for 10 s: dropped, room for Low
    QCOMPARE(m.effective("l"), Quality::Low);
    QVERIFY(m.encoderCount() <= 3);
  }
  void anIdleEncoderIsEvictedForANewTierAtTheCap() {
    TierManager m;
    m.addViewer("a", 0);
    m.addViewer("b", 0);
    m.setPick("b", Quality::High, 0);
    m.addViewer("c", 0);
    m.setPick("c", Quality::Medium, 0);
    QCOMPARE(m.encoderCount(), 3);
    m.setPick("c", Quality::Auto, 1000);  // Medium is idle now
    const auto ch = m.setPick("c", Quality::Low, 1100);
    QCOMPARE(ch.effective, Quality::Low);  // Medium (idle) made room
    QVERIFY(!m.alive(Quality::Medium));
    QCOMPARE(m.encoderCount(), 3);
  }
  void evictForgetsAFailedTier() {
    TierManager m;
    m.addViewer("a", 0);
    m.setPick("a", Quality::Low, 0);
    QVERIFY(m.alive(Quality::Low));
    m.setPick("a", Quality::Auto, 100);  // the host moves its viewers off a failed tier first
    m.evict(Quality::Low);
    QVERIFY(!m.alive(Quality::Low));
    QCOMPARE(m.fixedMask(), 0);
    QVERIFY(!m.tick(100'000));
    m.evict(Quality::Auto);  // never drops the adaptive encoder
    QVERIFY(m.alive(Quality::Auto));
  }
  void removingEveryViewerAndClear() {
    TierManager m;
    m.addViewer("a", 0);
    m.setPick("a", Quality::Low, 0);
    m.removeViewer("a", 10);
    m.clear();
    QCOMPARE(m.encoderCount(), 0);
    QVERIFY(!m.hasViewer("a"));
  }
};

QTEST_APPLESS_MAIN(QualityTierTest)
#include "qualitytier_test.moc"
