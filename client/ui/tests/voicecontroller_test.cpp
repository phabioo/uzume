// VoiceController (0.23 package 7, ADR 0033 D7) without QML and without a microphone: the room from voice_room messages (join, members,
// leave, removal), toast for a newcomer, the mic states with push-to-talk / Shift+M / voice activation, muted by host, the synthetic source
// (voice_inject), per-person volume, ducking, and the People marks. The Hub socket is never opened; messages are emitted directly.
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "playersettings.h"
#include "voicecontroller.h"

using namespace uzume;
using namespace uzume::ui;
using Scope = TogetherController::Scope;

namespace {
VoiceRoomMessage room(const QString& scope, const QString& id, const QList<VoiceMember>& members) { return {scope, id, members}; }
VoiceMember member(const QString& user, const QString& device, const QString& mic = QStringLiteral("ptt"), bool muted = false) {
  return {user, device, mic, muted};
}
}  // namespace

class VoiceControllerTest : public QObject {
  Q_OBJECT

  struct Fixture {
    QTemporaryDir dir;
    PlayerSettings settings{dir.path()};
    HubSocket socket{nullptr};
    AudioMixer mixer;
    VoiceController vc{&socket, &mixer, &settings};
    Scope scope{QStringLiteral("session"), QStringLiteral("s1"), true};
    bool owner = false;
    Fixture() {
      mixer.openHeadless(48000);
      vc.setCaptureEnabled(false);
      vc.setFeatureCheck([] { return true; });
      vc.setScopeProvider([this] { return scope; });
      vc.setSelf([] { return QStringLiteral("u_max"); }, [] { return QStringLiteral("dev-max"); });
      vc.setNameResolver([](const QString& id) { return id == QLatin1String("u_lena") ? QStringLiteral("Lena") : id == QLatin1String("u_max") ? QStringLiteral("Max") : id == QLatin1String("u_sam") ? QStringLiteral("Sam") : QString(); });
      vc.setOwnerCheck([this](const QString&) { return owner; });
    }
    void inRoom(const QList<VoiceMember>& extra = {member(QStringLiteral("u_lena"), QStringLiteral("dev-lena"))}) {
      vc.join();
      QList<VoiceMember> all{member(QStringLiteral("u_max"), QStringLiteral("dev-max"))};
      all.append(extra);
      emit socket.voiceRoomReceived(room(QStringLiteral("session"), QStringLiteral("s1"), all));
    }
  };

  static QVariantMap memberOf(VoiceController& vc, const QString& user) {
    for (const QVariant& v : vc.members())
      if (v.toMap().value("userId").toString() == user) return v.toMap();
    return {};
  }

 private slots:
  void availabilityAndJoinLeave() {
    Fixture f;
    QVERIFY(f.vc.available());
    f.scope = Scope{};
    QVERIFY(!f.vc.available());
    f.vc.join();
    QVERIFY(!f.vc.wantJoined());  // no scope, nothing to join
    f.scope = Scope{QStringLiteral("session"), QStringLiteral("s1"), true};
    f.vc.setFeatureCheck([] { return false; });
    QVERIFY(!f.vc.available());
    f.vc.join();
    QVERIFY(!f.vc.wantJoined());
    f.vc.setFeatureCheck([] { return true; });
    f.vc.join();
    QVERIFY(f.vc.wantJoined());
    QVERIFY(!f.vc.joined());  // the Hub has not listed me yet
    QCOMPARE(f.vc.roomScope(), QStringLiteral("session"));
    QCOMPARE(f.vc.roomId(), QStringLiteral("s1"));
    emit f.socket.voiceRoomReceived(room(QStringLiteral("session"), QStringLiteral("s1"), {member("u_max", "dev-max")}));
    QVERIFY(f.vc.joined());
    f.vc.leave();
    QVERIFY(!f.vc.wantJoined() && !f.vc.joined());
    QVERIFY(f.vc.members().isEmpty());
  }

  void membersMarksAndToast() {
    Fixture f;
    QSignalSpy joinedSpy(&f.vc, &VoiceController::memberJoined);
    f.inRoom({member("u_lena", "dev-lena", "off")});
    QCOMPARE(f.vc.members().size(), 2);
    QCOMPARE(joinedSpy.size(), 0);  // who is already there when I arrive is not news
    QCOMPARE(memberOf(f.vc, "u_lena").value("voice").toString(), QStringLiteral("micOff"));
    QCOMPARE(memberOf(f.vc, "u_lena").value("name").toString(), QStringLiteral("Lena"));
    QVERIFY(memberOf(f.vc, "u_max").value("me").toBool());
    // A third person joins: the toast "<name> joined voice"
    emit f.socket.voiceRoomReceived(room("session", "s1", {member("u_max", "dev-max"), member("u_lena", "dev-lena", "off"), member("u_sam", "dev-sam")}));
    QCOMPARE(joinedSpy.size(), 1);
    QCOMPARE(joinedSpy.first().first().toString(), QStringLiteral("Sam"));
    // Muted by host beats mic off; the marks map feeds the Multiview tiles
    emit f.socket.voiceRoomReceived(room("session", "s1", {member("u_max", "dev-max"), member("u_lena", "dev-lena", "off", true), member("u_sam", "dev-sam")}));
    QCOMPARE(memberOf(f.vc, "u_lena").value("voice").toString(), QStringLiteral("mutedByHost"));
    QCOMPARE(f.vc.marks().value("u_lena").toString(), QStringLiteral("mutedByHost"));
    QVERIFY(!f.vc.marks().contains("u_sam"));
    // A room of another scope is not mine
    emit f.socket.voiceRoomReceived(room("session", "other", {member("u_max", "dev-max")}));
    QCOMPARE(f.vc.members().size(), 3);
    // The Hub removes me (or closes the room): I am out
    emit f.socket.voiceRoomReceived(room("session", "s1", {}));
    QVERIFY(!f.vc.joined() && !f.vc.wantJoined());
  }

  void mutedByHostIsMyHeaderState() {
    Fixture f;
    f.inRoom();
    f.vc.talkKey(true);
    QCOMPARE(f.vc.micState(), MicState::Live);
    emit f.socket.voiceRoomReceived(room("session", "s1", {member("u_max", "dev-max", "ptt", true), member("u_lena", "dev-lena")}));
    QCOMPARE(f.vc.micStateText(), QStringLiteral("mutedByHost"));
    QCOMPARE(f.vc.micWord(), QStringLiteral("Muted by host"));
    QVERIFY(f.vc.mutedByHost());
    f.vc.toggleMic();  // only the host can undo it
    QVERIFY(f.vc.micOn());
    QVERIFY(!f.vc.notice().isEmpty());
    emit f.socket.voiceRoomReceived(room("session", "s1", {member("u_max", "dev-max"), member("u_lena", "dev-lena")}));
    QCOMPARE(f.vc.micStateText(), QStringLiteral("live"));
  }

  void micStatesPushToTalkToggleAndVoiceActivation() {
    Fixture f;
    f.vc.setPttKeyLabel(QStringLiteral("V"));
    QCOMPARE(f.vc.micStateText(), QStringLiteral("ptt"));
    QCOMPARE(f.vc.micWord(), QStringLiteral("Hold V"));
    f.vc.talkKey(true);
    QCOMPARE(f.vc.micStateText(), QStringLiteral("live"));
    QCOMPARE(f.vc.micWord(), QStringLiteral("Live"));
    f.vc.talkKey(false);
    QCOMPARE(f.vc.micStateText(), QStringLiteral("ptt"));
    f.vc.toggleMic();  // Shift+M
    QCOMPARE(f.vc.micStateText(), QStringLiteral("off"));
    QCOMPARE(f.vc.micMode(), QStringLiteral("off"));
    f.vc.talkKey(true);  // the key does nothing while the mic is off
    QCOMPARE(f.vc.micStateText(), QStringLiteral("off"));
    f.vc.talkKey(false);
    f.vc.toggleMic();
    QCOMPARE(f.vc.micStateText(), QStringLiteral("ptt"));
    // Voice activation: live while on; the same key now switches the mic
    f.vc.setTalkMode(QStringLiteral("vad"));
    QCOMPARE(f.vc.micStateText(), QStringLiteral("live"));
    QCOMPARE(f.vc.micMode(), QStringLiteral("on"));
    f.vc.talkKey(true);
    QCOMPARE(f.vc.micStateText(), QStringLiteral("off"));
    f.vc.talkKey(false);
    f.vc.setMicMode(QStringLiteral("ptt"));  // MCP
    QCOMPARE(f.vc.talkMode(), QStringLiteral("ptt"));
    QCOMPARE(f.vc.micStateText(), QStringLiteral("ptt"));
    f.vc.setMicMode(QStringLiteral("off"));
    QCOMPARE(f.vc.micStateText(), QStringLiteral("off"));
    f.vc.setMicMode(QStringLiteral("bogus"));
    QCOMPARE(f.vc.micStateText(), QStringLiteral("off"));
    // The settings survive in the PC's settings
    QVERIFY(!f.settings.voiceMicOn());
  }

  void injectedSignalStandsInForTheMicrophone() {
    Fixture f;
    f.inRoom();
    QVERIFY(f.vc.injectTone(440, 0.2));
    QVERIFY(f.vc.injecting());
    QCOMPARE(f.vc.micStateText(), QStringLiteral("live"));  // counts as the key held
    for (int i = 0; i < 3; ++i) f.vc.tickForTests();
    QVERIFY(f.vc.selfSpeaking());
    QCOMPARE(memberOf(f.vc, "u_max").value("voice").toString(), QStringLiteral("speaking"));
    QVERIFY(f.vc.levelDb() > -30 && f.vc.levelDb() < -5);
    // The person's own switch still applies
    f.vc.setMicMode(QStringLiteral("off"));
    f.vc.tickForTests();
    QVERIFY(!f.vc.selfSpeaking());
    // Bad input
    QVERIFY(!f.vc.injectTone(0, 1));
    QVERIFY(!f.vc.injectWav(QStringLiteral("/nonexistent.wav")));
    // The queue ends by itself
    f.vc.setMicMode(QStringLiteral("ptt"));
    for (int i = 0; i < 20; ++i) f.vc.tickForTests();
    QVERIFY(!f.vc.injecting());
    QVERIFY(!f.vc.selfSpeaking());
  }

  void injectedWavFile() {
    Fixture f;
    QTemporaryDir dir;
    QFile w(dir.filePath("t.wav"));
    QVERIFY(w.open(QIODevice::WriteOnly));
    const int n = 48000 / 5;  // 0.2 s mono 16-bit 48 kHz
    QByteArray data(n * 2, 0);
    for (int i = 0; i < n; ++i) {
      const qint16 v = qint16(8000 * std::sin(2 * M_PI * 300 * i / 48000.0));
      memcpy(data.data() + 2 * i, &v, 2);
    }
    auto le32 = [](quint32 v) { return QByteArray(reinterpret_cast<const char*>(&v), 4); };
    auto le16 = [](quint16 v) { return QByteArray(reinterpret_cast<const char*>(&v), 2); };
    w.write("RIFF" + le32(36 + data.size()) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(48000) + le32(96000) + le16(2) + le16(16) + "data" + le32(data.size()) + data);
    w.close();
    QVERIFY(f.vc.injectWav(w.fileName()));
    QVERIFY(f.vc.injecting());
  }

  void duckingFollowsSomeoneElseSpeaking() {
    Fixture f;
    f.inRoom();
    QVERIFY(!f.mixer.ducking());
    emit f.vc.engine()->speakingChanged(QStringLiteral("dev-lena"), true);
    QVERIFY(f.vc.someoneSpeaking());
    QVERIFY(f.mixer.ducking());
    QCOMPARE(memberOf(f.vc, "u_lena").value("voice").toString(), QStringLiteral("speaking"));
    f.vc.setDucking(false);  // Settings: "Lower game volume while someone speaks"
    QVERIFY(!f.mixer.ducking());
    f.vc.setDucking(true);
    QVERIFY(f.mixer.ducking());
    emit f.vc.engine()->speakingChanged(QStringLiteral("dev-lena"), false);
    QVERIFY(!f.mixer.ducking());
    // Leaving the room never leaves the game ducked
    emit f.vc.engine()->speakingChanged(QStringLiteral("dev-lena"), true);
    QVERIFY(f.mixer.ducking());
    f.vc.leave();
    QVERIFY(!f.mixer.ducking());
  }

  void decodedVoiceReachesTheMixerWithAPersonGain() {
    Fixture f;
    f.inRoom();
    emit f.vc.engine()->pcmDecoded(QStringLiteral("dev-lena"), QByteArray(960 * 2, '\x10'));
    const QVariantList inputs = f.mixer.state().value("inputs").toList();
    QCOMPARE(inputs.size(), 1);
    QCOMPARE(inputs.first().toMap().value("channel").toString(), QStringLiteral("voice"));
    QCOMPARE(inputs.first().toMap().value("key").toString(), QStringLiteral("u_lena"));
    QVERIFY(inputs.first().toMap().value("queued_frames").toLongLong() > 0);
    f.vc.setPersonGain(QStringLiteral("u_lena"), 40);
    QCOMPARE(f.vc.personGain(QStringLiteral("u_lena")), 40);
    QCOMPARE(f.mixer.personGain(AudioMixer::Voice, QStringLiteral("u_lena")), 0.4);
    QCOMPARE(f.settings.voicePersonGain(QStringLiteral("u_lena")), 40);  // remembered per user id
    QCOMPARE(memberOf(f.vc, "u_lena").value("gain").toInt(), 40);
    // A person who leaves loses the mixer input
    emit f.socket.voiceRoomReceived(room("session", "s1", {member("u_max", "dev-max")}));
    QCOMPARE(f.mixer.state().value("inputs").toList().size(), 0);
  }

  void hostMutesOnlyInASessionIOwn() {
    Fixture f;
    f.inRoom();
    QVERIFY(!f.vc.canMuteOthers());
    f.owner = true;
    QVERIFY(f.vc.canMuteOthers());
    f.vc.mute(QStringLiteral("u_lena"), true);  // goes to the Hub as voice_mute (the socket is closed here: nothing to observe)
    f.vc.leave();
    f.scope = Scope{QStringLiteral("run"), QStringLiteral("r1"), true};
    f.vc.join();
    QVERIFY(!f.vc.canMuteOthers());  // runs have no host
  }

  void scopeChangeLeavesTheRoom() {
    Fixture f;
    f.inRoom();
    QVERIFY(f.vc.joined());
    f.scope = Scope{QStringLiteral("session"), QStringLiteral("s2"), true};
    f.vc.refresh();
    QVERIFY(!f.vc.wantJoined());
    f.scope = Scope{};
    f.vc.refresh();
    QVERIFY(!f.vc.available());
  }

  void hubErrorsOfAJoinAreExplained() {
    Fixture f;
    f.vc.join();
    emit f.socket.hubError(QStringLiteral("conflict"), QStringLiteral("room full"));
    QVERIFY(!f.vc.wantJoined());
    QVERIFY(f.vc.notice().contains(QStringLiteral("full")));
    // An error while I am in the room (some other feature) leaves it alone
    f.inRoom();
    emit f.socket.hubError(QStringLiteral("conflict"), QStringLiteral("something else"));
    QVERIFY(f.vc.wantJoined());
  }

  void settingsPage() {
    Fixture f;
    QCOMPARE(f.vc.talkMode(), QStringLiteral("ptt"));
    QVERIFY(f.vc.noiseSuppression() && f.vc.ducking() && f.vc.autoJoin());
    f.vc.setThresholdFraction(0.5);
    QCOMPARE(f.vc.thresholdDb(), -30);
    f.vc.setThresholdDb(-200);
    QCOMPARE(f.vc.thresholdDb(), -70);  // clamped
    f.vc.setNoiseSuppression(false);
    f.vc.setAutoJoin(false);
    QVERIFY(!f.settings.voiceNoiseSuppression() && !f.settings.voiceAutoJoin());
    PlayerSettings again(f.dir.path());
    QCOMPARE(again.voiceThresholdDb(), -70);
    QVERIFY(!again.voiceNoiseSuppression());
    QVERIFY(!f.settings.setVoiceTalkMode(QStringLiteral("loud")));
  }
};

QTEST_GUILESS_MAIN(VoiceControllerTest)
#include "voicecontroller_test.moc"
