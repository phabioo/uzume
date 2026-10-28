// Voice chat mic logic (0.23 package 7, ADR 0033 D7): the six states of the header mic control, what is sent when, the wire mode and
// voice activity with its hangover. Pure; no audio.
#include <QtTest>

#include "voicemiclogic.h"

using namespace uzume::ui;

class VoiceMicTest : public QObject {
  Q_OBJECT
 private slots:
  void pushToTalkStates() {
    MicInputs in;  // push-to-talk, mic on
    QCOMPARE(micStateOf(in), MicState::PttIdle);
    QVERIFY(!micTransmits(in));
    in.pttHeld = true;
    QCOMPARE(micStateOf(in), MicState::Live);
    QVERIFY(micTransmits(in));
    QCOMPARE(micWireMode(in), QStringLiteral("ptt"));  // the key press does not change what the others see
    in.pttHeld = false;
    QCOMPARE(micWireMode(in), QStringLiteral("ptt"));
  }
  void voiceActivationStates() {
    MicInputs in;
    in.vad = true;
    QCOMPARE(micStateOf(in), MicState::Live);  // live while on, even in silence
    QVERIFY(!micTransmits(in));                 // but nothing is sent until there is voice
    in.activityOpen = true;
    QVERIFY(micTransmits(in));
    QCOMPARE(micWireMode(in), QStringLiteral("on"));
    in.pttHeld = true;  // the key does nothing to the state in this mode
    in.activityOpen = false;
    QVERIFY(!micTransmits(in));
  }
  void micOffAndPriorities() {
    MicInputs in;
    in.pttHeld = true;
    in.micOn = false;
    QCOMPARE(micStateOf(in), MicState::Off);
    QVERIFY(!micTransmits(in));
    QCOMPARE(micWireMode(in), QStringLiteral("off"));
    in.mutedByHost = true;  // muted by host beats the person's own switch: only the host can undo it
    QCOMPARE(micStateOf(in), MicState::MutedByHost);
    in.micOn = true;
    QCOMPARE(micStateOf(in), MicState::MutedByHost);
    QVERIFY(!micTransmits(in));
    QCOMPARE(micWireMode(in), QStringLiteral("ptt"));  // the person's own choice stays what the others see
    in.deviceAvailable = false;
    QCOMPARE(micStateOf(in), MicState::NoDevice);  // nothing to hear: the machine says so first
    QCOMPARE(micWireMode(in), QStringLiteral("off"));
    in.blocked = true;
    QCOMPARE(micStateOf(in), MicState::Blocked);
    in.blocked = false;
    in.deviceAvailable = true;
    in.mutedByHost = false;
    QCOMPARE(micStateOf(in), MicState::Live);
  }
  void stateNames() {
    QCOMPARE(micStateName(MicState::Off), QStringLiteral("off"));
    QCOMPARE(micStateName(MicState::PttIdle), QStringLiteral("ptt"));
    QCOMPARE(micStateName(MicState::Live), QStringLiteral("live"));
    QCOMPARE(micStateName(MicState::MutedByHost), QStringLiteral("mutedByHost"));
    QCOMPARE(micStateName(MicState::NoDevice), QStringLiteral("noDevice"));
    QCOMPARE(micStateName(MicState::Blocked), QStringLiteral("blocked"));
  }
  void activityOpensHoldsAndCloses() {
    VoiceActivity a;
    const double thr = -42;
    QVERIFY(!a.update(-60, thr, 0));
    QVERIFY(!a.update(-43, thr, 20));   // just below: closed
    QVERIFY(a.update(-40, thr, 40));    // above: open
    QVERIFY(a.update(-44, thr, 60));    // within the 3 dB hysteresis: stays open
    QVERIFY(a.update(-70, thr, 100));   // below, but the hangover holds it
    QVERIFY(a.update(-70, thr, 60 + VoiceActivity::kHangoverMs));
    QVERIFY(!a.update(-70, thr, 61 + VoiceActivity::kHangoverMs));  // hangover over
    QVERIFY(!a.update(-44, thr, 500));  // reopening needs the real threshold, not the hysteresis edge
    QVERIFY(a.update(-41, thr, 520));
    a.reset();
    QVERIFY(!a.open());
  }
};

QTEST_GUILESS_MAIN(VoiceMicTest)
#include "voicemic_test.moc"
