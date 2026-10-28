#pragma once
// AudioController (0.19 audio stage 1): master volume, mute, "Mute when in the background", volume steps, the on-screen
// volume notice and the output device choice of this PC. The state lives in PlayerSettings; the controller turns it into
// two software gains: the local game (silent in the background when the option is on) and watched sessions (always
// audible, only master volume and mute apply). Voice and plugin sound come with 0.21.

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace uzume {
class PlayerSettings;
}

namespace uzume::ui {

class AudioController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  Q_PROPERTY(int volume READ volume NOTIFY changed)                        // 0..100 %
  Q_PROPERTY(bool muted READ muted NOTIFY changed)                         // the mute flag
  Q_PROPERTY(bool effectiveMuted READ effectiveMuted NOTIFY changed)       // muted or volume 0: what the Volume button shows
  Q_PROPERTY(bool muteInBackground READ muteInBackground WRITE setMuteInBackground NOTIFY changed)
  Q_PROPERTY(int volumeStep READ volumeStep WRITE setVolumeStep NOTIFY changed)  // 5 | 10
  Q_PROPERTY(bool showVolume READ showVolume WRITE setShowVolume NOTIFY changed)
  Q_PROPERTY(QString outputDevice READ outputDevice WRITE setOutputDevice NOTIFY changed)  // "" = system default
  Q_PROPERTY(QVariantList outputDevices READ outputDevices NOTIFY devicesChanged)         // [{value, label}], default first
  Q_PROPERTY(bool appActive READ appActive NOTIFY changed)
  // 0.23: channel gains of the Sound popover next to the master volume ("My game"), 0..100 %.
  Q_PROPERTY(int voiceGain READ voiceGain NOTIFY changed)
  Q_PROPERTY(int pluginGain READ pluginGain NOTIFY changed)

 public:
  explicit AudioController(PlayerSettings* settings, QObject* parent = nullptr);

  int volume() const { return volume_; }
  bool muted() const { return muted_; }
  bool effectiveMuted() const { return muted_ || volume_ <= 0; }
  bool muteInBackground() const { return muteInBackground_; }
  void setMuteInBackground(bool on);
  int volumeStep() const { return step_; }
  void setVolumeStep(int percent);
  bool showVolume() const { return showVolume_; }
  void setShowVolume(bool on);
  QString outputDevice() const { return device_; }
  void setOutputDevice(const QString& id);
  QVariantList outputDevices() const;
  bool appActive() const { return appActive_; }
  int voiceGain() const { return voiceGain_; }
  int pluginGain() const { return pluginGain_; }
  // The local game is silent: "Muted in the background" and the window is inactive.
  bool gameSilenced() const { return muteInBackground_ && !appActive_; }

  // Gains 0..1: the local game (also silent in the background) and watched sessions.
  double localGain() const;
  double remoteGain() const;
  // Output device id as QAudioDevice::id() bytes (decoded from the stored text); empty = system default.
  QByteArray deviceIdBytes() const;

  Q_INVOKABLE void volumeUp();     // hotkey: one step up, unmutes; shows the notice
  Q_INVOKABLE void volumeDown();   // hotkey: one step down; shows the notice
  Q_INVOKABLE void toggleMute();   // hotkey: shows the notice
  Q_INVOKABLE void setVolume(int percent);  // slider: no notice
  Q_INVOKABLE void setMuted(bool muted);    // popover button: no notice
  Q_INVOKABLE void setVoiceGain(int percent);
  Q_INVOKABLE void setPluginGain(int percent);
  // The window became (in)active; connected to QGuiApplication::applicationStateChanged, callable from tests.
  void setAppActive(bool active);

 signals:
  void changed();
  void devicesChanged();
  void gainsChanged();
  void volumeNotice(const QString& text);

 private:
  void announce(const QString& text);
  void apply(bool notifyChanged = true);
  QString volumeText() const;

  PlayerSettings* settings_;
  int volume_ = 100;
  bool muted_ = false;
  bool muteInBackground_ = true;
  int step_ = 5;
  bool showVolume_ = true;
  QString device_;
  bool appActive_ = true;
  int voiceGain_ = 100;
  int pluginGain_ = 100;
};

}  // namespace uzume::ui
