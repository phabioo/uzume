#pragma once
// VoiceController (0.23 package 7, ADR 0033 D7): the voice chat of the scope the running game belongs to (the TogetherController's
// scope: a run, my shared Session or the Session I watch). Owns the microphone (capture, noise suppression and echo cancellation,
// voice activity or push-to-talk), the VoiceEngine (mesh of peer connections) and the room: join / leave, the members with their mic
// state and speaking marks, the host's mute, per-person volume, ducking of game and tiles, and the six states of the header mic
// control. Room membership lives on the Hub (voice_room); everything else is derived from it. Nothing is stored except the settings.

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "audiomixer.h"
#include "audioresampler.h"
#include "hubsocket.h"
#include "togethercontroller.h"
#include "voicedsp.h"
#include "voiceengine.h"
#include "voicemiclogic.h"

namespace uzume {
class PlayerSettings;
}

namespace uzume::ui {

class VoiceCapture;

class VoiceController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  Q_PROPERTY(bool available READ available NOTIFY stateChanged)  // the Hub offers voice (together_v1) and a scope exists
  Q_PROPERTY(bool joined READ joined NOTIFY stateChanged)        // I am in the voice room
  Q_PROPERTY(bool wantJoined READ wantJoined NOTIFY stateChanged)
  Q_PROPERTY(QString roomScope READ roomScope NOTIFY stateChanged)
  Q_PROPERTY(QString roomId READ roomId NOTIFY stateChanged)
  Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)  // "The voice room is full." and the like
  // The mic (board 6w-2)
  Q_PROPERTY(QString micState READ micStateText NOTIFY micChanged)  // off | ptt | live | mutedByHost | noDevice | blocked
  Q_PROPERTY(QString micWord READ micWord NOTIFY micChanged)        // "Hold V" | "Live" | "Off" | "Muted by host" | "No microphone" | "Blocked"
  Q_PROPERTY(QString micDetail READ micDetail NOTIFY micChanged)    // one line for the settings page and the tooltip
  Q_PROPERTY(QString micMode READ micMode NOTIFY micChanged)        // what the others see: off | ptt | on
  Q_PROPERTY(bool micOn READ micOn NOTIFY micChanged)
  Q_PROPERTY(bool selfSpeaking READ selfSpeaking NOTIFY micChanged)
  Q_PROPERTY(bool mutedByHost READ mutedByHost NOTIFY micChanged)
  Q_PROPERTY(double level READ level NOTIFY levelChanged)  // 0..1 (-60..0 dBFS) for the meter
  Q_PROPERTY(double levelDb READ levelDb NOTIFY levelChanged)
  Q_PROPERTY(double thresholdFraction READ thresholdFraction NOTIFY settingsChanged)
  Q_PROPERTY(bool monitoring READ monitoring WRITE setMonitoring NOTIFY settingsChanged)  // the Voice settings page shows the meter
  Q_PROPERTY(QString pttKeyLabel READ pttKeyLabel NOTIFY settingsChanged)
  // The room
  Q_PROPERTY(QVariantList members READ members NOTIFY membersChanged)
  Q_PROPERTY(QVariantMap marks READ marks NOTIFY membersChanged)  // userId -> speaking | micOff | mutedByHost | "" (Multiview tiles)
  Q_PROPERTY(QString selfUserId READ selfUserId CONSTANT)
  Q_PROPERTY(bool someoneSpeaking READ someoneSpeaking NOTIFY membersChanged)
  Q_PROPERTY(bool canMuteOthers READ canMuteOthers NOTIFY stateChanged)  // the owner of the Session
  // Settings (this PC)
  Q_PROPERTY(QString talkMode READ talkMode WRITE setTalkMode NOTIFY settingsChanged)  // ptt | vad
  Q_PROPERTY(int thresholdDb READ thresholdDb WRITE setThresholdDb NOTIFY settingsChanged)
  Q_PROPERTY(bool noiseSuppression READ noiseSuppression WRITE setNoiseSuppression NOTIFY settingsChanged)
  Q_PROPERTY(bool ducking READ ducking WRITE setDucking NOTIFY settingsChanged)
  Q_PROPERTY(bool autoJoin READ autoJoin WRITE setAutoJoin NOTIFY settingsChanged)
  Q_PROPERTY(QString micDevice READ micDevice WRITE setMicDevice NOTIFY settingsChanged)  // "" = system default
  Q_PROPERTY(QVariantList micDevices READ micDevices NOTIFY devicesChanged)
  Q_PROPERTY(QString serverName READ serverName NOTIFY settingsChanged)

 public:
  static constexpr int kMaxMembers = 8;
  static constexpr double kMinDb = -60.0;

  struct Member {
    QString userId, deviceId, mic;
    bool mutedByHost = false;
  };

  VoiceController(HubSocket* socket, AudioMixer* mixer, PlayerSettings* settings, QObject* parent = nullptr);
  ~VoiceController() override;

  // Providers set by the PlayerController (tests set their own).
  void setScopeProvider(std::function<TogetherController::Scope()> f) { scopeFn_ = std::move(f); }
  void setSelf(std::function<QString()> userId, std::function<QString()> deviceId) {
    selfFn_ = std::move(userId);
    deviceFn_ = std::move(deviceId);
  }
  void setNameResolver(std::function<QString(const QString&)> f) { nameFn_ = std::move(f); }
  void setFeatureCheck(std::function<bool()> f) { featureFn_ = std::move(f); }
  // Whether I own the Session of the room (only the owner mutes), and whether the scope joins by itself (a run I play, an invite-only Session).
  void setOwnerCheck(std::function<bool(const QString& sessionId)> f) { ownerFn_ = std::move(f); }
  void setAutoJoinCheck(std::function<bool(const TogetherController::Scope&)> f) { autoFn_ = std::move(f); }
  void setServerName(std::function<QString()> f) { serverNameFn_ = std::move(f); }
  void setClock(std::function<qint64()> f) { clock_ = std::move(f); }
  void setSystemSettingsOpener(std::function<void()> f) { openSystemFn_ = std::move(f); }
  void setCaptureEnabled(bool on) { captureEnabled_ = on; }  // tests: never open a real microphone

  bool available() const;
  bool joined() const { return joined_; }
  bool wantJoined() const { return want_; }
  QString roomScope() const { return roomKind_; }
  QString roomId() const { return roomId_; }
  QString notice() const { return notice_; }
  QString micStateText() const { return micStateName(micState()); }
  MicState micState() const { return micStateOf(inputs()); }
  QString micWord() const;
  QString micDetail() const;
  QString micMode() const { return micWireMode(inputs()); }
  bool micOn() const;
  bool selfSpeaking() const { return selfSpeaking_; }
  bool mutedByHost() const { return mutedByHost_; }
  double level() const;
  double levelDb() const { return levelDb_; }
  double thresholdFraction() const;
  bool monitoring() const { return monitoring_; }
  void setMonitoring(bool on);
  QString pttKeyLabel() const { return pttKeyLabel_; }
  void setPttKeyLabel(const QString& label);
  QVariantList members() const;
  QVariantMap marks() const;
  QString selfUserId() const { return selfFn_ ? selfFn_() : QString(); }
  bool someoneSpeaking() const;
  bool canMuteOthers() const;
  QString talkMode() const;
  void setTalkMode(const QString& mode);
  int thresholdDb() const;
  void setThresholdDb(int db);
  bool noiseSuppression() const;
  void setNoiseSuppression(bool on);
  bool ducking() const;
  void setDucking(bool on);
  bool autoJoin() const;
  void setAutoJoin(bool on);
  QString micDevice() const;
  void setMicDevice(const QString& id);
  QVariantList micDevices() const;
  QString serverName() const { return serverNameFn_ ? serverNameFn_() : QString(); }

  Q_INVOKABLE void join();
  Q_INVOKABLE void leave();
  Q_INVOKABLE void toggleJoin() { want_ ? leave() : join(); }
  Q_INVOKABLE void toggleMic();                     // Shift+M
  Q_INVOKABLE void setMicMode(const QString& mode); // on | off | ptt (MCP): on = voice activation, ptt = push-to-talk, off = mic off
  Q_INVOKABLE void talkKey(bool down);              // V, Select + R3: hold with push-to-talk, a press toggles with voice activation
  Q_INVOKABLE void mute(const QString& userId, bool muted);  // the Session owner mutes a person for everyone
  Q_INVOKABLE void setPersonGain(const QString& userId, int percent);
  Q_INVOKABLE int personGain(const QString& userId) const;
  Q_INVOKABLE void micAction();  // the header mic control: join, or switch the mic, or fix a blocked mic
  Q_INVOKABLE void retryMic();
  Q_INVOKABLE void openSystemSettings();
  Q_INVOKABLE void setThresholdFraction(double f);  // the meter: a click sets the voice-activation threshold

  // Tests and MCP voice_inject: a synthetic signal instead of the microphone. Returns false for an empty or unreadable WAV.
  bool injectTone(double hz, double seconds);
  bool injectWav(const QString& path);
  bool injecting() const { return !inject_.empty(); }
  // The state for the MCP voice_state tool.
  QVariantMap debugState() const;
  // The members of the room as the People tab needs them: userId -> {name, voice, gain, mutedByHost, speaking, micOff}.
  QVariantMap memberInfo() const;

  // The scope may have changed (game, Session, run, Hub link): leaves a room that is not the scope's any more, joins by itself when allowed.
  void refresh();

  // Test hooks.
  void feedMicFrameForTests(const int16_t* mono960) { onMicFrame(mono960); }
  VoiceEngine* engine() { return &engine_; }
  void tickForTests() { injectTick(); }
  QString joinRequestIdForTests() const { return joinRequestId_; }
  void setCaptureProblemForTests(bool noDevice, bool blocked) {
    captureNoDevice_ = noDevice;
    captureBlocked_ = blocked;
    emit micChanged();
  }

 signals:
  void stateChanged();
  void micChanged();
  void levelChanged();
  void membersChanged();
  void settingsChanged();
  void devicesChanged();
  void noticeChanged();
  void memberJoined(const QString& name);  // someone else joined the room (toast "<name> joined voice")

 private:
  MicInputs inputs() const;
  QString nameOf(const QString& userId) const;
  bool featureOn() const;
  qint64 now() const;
  QString scopeKey(const TogetherController::Scope& s) const { return s.kind + QLatin1Char(':') + s.id; }
  void onVoiceRoom(const VoiceRoomMessage& room);
  void onVoiceSignal(const QString& scope, const QString& id, const QString& from, const QJsonObject& body);
  void onSocketState(HubSocket::State s);
  void onHubError(const QString& requestId, const QString& code, const QString& message);
  void sendJoin();
  void onMicFrame(const int16_t* mono960);
  void onCaptureFrame(const QByteArray& mono16);
  void onCaptureStatus();
  void injectTick();
  void startCaptureIfNeeded();
  void stopCaptureIfIdle();
  void configureDsp();
  void updateTap();
  void sendWireState();
  void onPcm(const QString& deviceId, const QByteArray& mono16);
  void onSpeaking(const QString& deviceId, bool speaking);
  void updateDucking();
  void dropInputs();
  void showNotice(const QString& text);
  void leaveRoom(bool manual);
  void applyMembers(const QList<Member>& members);
  void tapFarEnd(const float* stereo, int frames, int rate);

  HubSocket* socket_;
  AudioMixer* mixer_;
  PlayerSettings* settings_;
  std::function<TogetherController::Scope()> scopeFn_;
  std::function<QString()> selfFn_, deviceFn_, serverNameFn_;
  std::function<QString(const QString&)> nameFn_;
  std::function<bool()> featureFn_;
  std::function<bool(const QString&)> ownerFn_;
  std::function<bool(const TogetherController::Scope&)> autoFn_;
  std::function<qint64()> clock_;
  std::function<void()> openSystemFn_;
  bool captureEnabled_ = true;

  QElapsedTimer elapsed_;
  VoiceEngine engine_;
  std::unique_ptr<VoiceCapture> capture_;
  VoiceDsp dsp_;
  VoiceActivity activity_;
  bool dspNoise_ = true;

  // The room
  bool want_ = false;    // I asked to be in the room (survives a reconnect)
  bool joined_ = false;  // the Hub lists me
  QString roomKind_, roomId_;
  QList<Member> members_;
  QSet<QString> manualLeft_;  // scopes the person left by hand: no automatic join there
  bool firstRoomSeen_ = false;
  QString joinRequestId_;  // the envelope id of my last voice_join: only an error answering it ends the join
  QString notice_;
  QTimer noticeTimer_;
  QString lastWireMode_;
  bool mutedByHost_ = false;
  bool monitoring_ = false;
  QString pttKeyLabel_ = QStringLiteral("V");

  // The mic
  bool pttHeld_ = false;
  bool selfSpeaking_ = false;
  double levelDb_ = -120;
  int levelFrames_ = 0;
  bool captureBlocked_ = false;
  bool captureNoDevice_ = false;

  // Synthetic source (MCP voice_inject): 20 ms frames, consumed by a timer
  std::deque<std::vector<int16_t>> inject_;
  QTimer injectTimer_;

  // Voice of others
  struct VoiceIn {
    AudioMixer::InputId input = 0;
    QString userId;
    ui::LinearResampler resampler;
    int resamplerRate = 0;  // output rate the resampler was set for (the mixer's rate can change with the device)
    bool speaking = false;
  };
  QHash<QString, VoiceIn> voiceIn_;  // by device id
  bool mixerHeld_ = false;

  // Echo reference: what the mixer plays, mono 48 kHz, written by the audio thread
  std::mutex farMutex_;
  std::deque<int16_t> far_;
  double farPos_ = 0.0;
  std::vector<int16_t> farFrame_;
};

}  // namespace uzume::ui
