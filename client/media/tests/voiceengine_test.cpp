// VoiceEngine: the offerer rule and two engines in one process (in-memory signaling, host candidates, no network): a tone in A comes
// out of B decoded, B sees A speaking, and the same the other way round (the answerer sends too). Three engines form a mesh.
#include <QSignalSpy>
#include <QtTest>
#include <cmath>
#include <vector>

#include "processguard.h"
#include "voicedsp.h"
#include "voiceengine.h"

using namespace uzume;

namespace {
// The Hub's relay: a signal goes to the device it names and arrives with the sender's id.
void wire(VoiceEngine& from, const QString& fromDevice, VoiceEngine& to, const QString& toDevice) {
  QObject::connect(&from, &VoiceEngine::signalOut, &to, [&to, fromDevice, toDevice](const QString& target, const QJsonObject& body) {
    if (target == toDevice) to.handleSignal(fromDevice, body);
  }, Qt::QueuedConnection);
}
struct Level {
  double maxDb = -120;
  int frames = 0;
};
std::vector<int16_t> tone(int frame, double hz = 440.0, double amp = 8000.0) {
  std::vector<int16_t> v(kVoiceFrame);
  for (int i = 0; i < kVoiceFrame; ++i) v[size_t(i)] = int16_t(amp * std::sin(2 * M_PI * hz * double(frame * kVoiceFrame + i) / kVoiceRate));
  return v;
}
}  // namespace

class VoiceEngineTest : public QObject {
  Q_OBJECT
 private slots:
  void offererIsTheLowerDeviceId() {
    QVERIFY(VoiceEngine::isOfferer(QStringLiteral("dev-a"), QStringLiteral("dev-b")));
    QVERIFY(!VoiceEngine::isOfferer(QStringLiteral("dev-b"), QStringLiteral("dev-a")));
    QVERIFY(VoiceEngine::isOfferer(QStringLiteral("a"), QStringLiteral("aa")));
    // Exactly one of two devices offers.
    QVERIFY(VoiceEngine::isOfferer(QStringLiteral("x1"), QStringLiteral("x2")) != VoiceEngine::isOfferer(QStringLiteral("x2"), QStringLiteral("x1")));
  }

  void toneTravelsBothWays() {
    VoiceEngine a, b;
    a.setSelf(QStringLiteral("u-a"), QStringLiteral("dev-a"));
    b.setSelf(QStringLiteral("u-b"), QStringLiteral("dev-b"));
    wire(a, QStringLiteral("dev-a"), b, QStringLiteral("dev-b"));
    wire(b, QStringLiteral("dev-b"), a, QStringLiteral("dev-a"));
    Level atB, atA;
    QObject::connect(&b, &VoiceEngine::pcmDecoded, &b, [&](const QString& dev, const QByteArray& pcm) {
      QCOMPARE(dev, QStringLiteral("dev-a"));
      QCOMPARE(pcm.size(), kVoiceFrame * 2);
      atB.maxDb = std::max<double>(atB.maxDb, VoiceDsp::levelDb(reinterpret_cast<const int16_t*>(pcm.constData()), kVoiceFrame));
      ++atB.frames;
    });
    QObject::connect(&a, &VoiceEngine::pcmDecoded, &a, [&](const QString&, const QByteArray& pcm) {
      atA.maxDb = std::max<double>(atA.maxDb, VoiceDsp::levelDb(reinterpret_cast<const int16_t*>(pcm.constData()), kVoiceFrame));
      ++atA.frames;
    });
    QSignalSpy speakB(&b, &VoiceEngine::speakingChanged);
    const QList<VoiceEngine::Member> room{{QStringLiteral("u-a"), QStringLiteral("dev-a")}, {QStringLiteral("u-b"), QStringLiteral("dev-b")}};
    a.setMembers(room);
    b.setMembers(room);
    const auto connected = [](VoiceEngine& e) {
      const auto peers = e.peers();
      return peers.size() == 1 && peers.first().state == QLatin1String("connected");
    };
    QTRY_VERIFY_WITH_TIMEOUT(connected(a) && connected(b), 20000);

    // A speaks: 60 frames of tone (1.2 s), paced like a microphone.
    int sentTo = 0;
    for (int f = 0; f < 60; ++f) {
      sentTo += a.pushFrame(tone(f).data(), true);
      QTest::qWait(20);
    }
    QVERIFY2(sentTo >= 50, qPrintable(QString::number(sentTo)));
    QTRY_VERIFY_WITH_TIMEOUT(atB.frames >= 40, 3000);
    qInfo("A -> B: %d frames decoded, peak %.1f dBFS", atB.frames, atB.maxDb);
    QVERIFY2(atB.maxDb > -25.0 && atB.maxDb < -10.0, qPrintable(QString::number(atB.maxDb)));
    QVERIFY(!speakB.isEmpty());
    QCOMPARE(speakB.first().at(0).toString(), QStringLiteral("dev-a"));
    QVERIFY(speakB.first().at(1).toBool());
    // A is silent again: B stops seeing speech.
    for (int f = 0; f < 30; ++f) {
      a.pushFrame(tone(f).data(), false);
      QTest::qWait(20);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!b.peers().first().speaking, 2000);
    QCOMPARE(speakB.last().at(1).toBool(), false);

    // B speaks (B is the answerer): A decodes it.
    for (int f = 0; f < 40; ++f) {
      b.pushFrame(tone(f, 880.0).data(), true);
      QTest::qWait(20);
    }
    QTRY_VERIFY_WITH_TIMEOUT(atA.frames >= 25, 3000);
    QVERIFY2(atA.maxDb > -25.0, qPrintable(QString::number(atA.maxDb)));
    QVERIFY(a.peers().first().framesReceived > 20);
  }

  // Only the offerer rebuilds a failed or closed connection; the answerer waits for the new offer.
  void onlyTheOffererRebuilds() {
    constexpr int kFailed = 4, kClosed = 5;
    for (const int dead : {kFailed, kClosed}) {
      VoiceEngine offerer, answerer;
      offerer.setSelf(QStringLiteral("u-a"), QStringLiteral("dev-a"));
      answerer.setSelf(QStringLiteral("u-b"), QStringLiteral("dev-b"));
      const QList<VoiceEngine::Member> room{{QStringLiteral("u-a"), QStringLiteral("dev-a")}, {QStringLiteral("u-b"), QStringLiteral("dev-b")}};
      offerer.setMembers(room);
      answerer.setMembers(room);
      const int offererBefore = offerer.buildsForTests(QStringLiteral("dev-b"));
      const int answererBefore = answerer.buildsForTests(QStringLiteral("dev-a"));
      offerer.simulateStateForTests(QStringLiteral("dev-b"), dead);
      answerer.simulateStateForTests(QStringLiteral("dev-a"), dead);
      QTRY_VERIFY_WITH_TIMEOUT(offerer.buildsForTests(QStringLiteral("dev-b")) > offererBefore, 4000);
      QTest::qWait(300);
      QCOMPARE(answerer.buildsForTests(QStringLiteral("dev-a")), answererBefore);
    }
  }

  void meshOfThree() {
    VoiceEngine a, b, c;
    VoiceEngine* all[] = {&a, &b, &c};
    const QStringList dev{QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3")};
    for (int i = 0; i < 3; ++i) all[i]->setSelf(QStringLiteral("u%1").arg(i), dev[i]);
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        if (i != j) wire(*all[i], dev[i], *all[j], dev[j]);
    QList<VoiceEngine::Member> room;
    for (int i = 0; i < 3; ++i) room.append({QStringLiteral("u%1").arg(i), dev[i]});
    for (auto* e : all) e->setMembers(room);
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(std::begin(all), std::end(all), [](VoiceEngine* e) {
      const auto p = e->peers();
      return p.size() == 2 && p[0].state == QLatin1String("connected") && p[1].state == QLatin1String("connected");
    }), 25000);
    // One leaves: the others drop it.
    room.removeLast();
    a.setMembers(room);
    b.setMembers(room);
    QCOMPARE(a.peers().size(), 1);
    QCOMPARE(b.peers().size(), 1);
  }
};

FB_TEST_MAIN(VoiceEngineTest)
#include "voiceengine_test.moc"
