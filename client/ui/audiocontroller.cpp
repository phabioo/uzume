#include "audiocontroller.h"

#include <QAudioDevice>
#include <QGuiApplication>
#include <QMediaDevices>
#include <algorithm>

#include "playersettings.h"

namespace uzume::ui {

AudioController::AudioController(PlayerSettings* settings, QObject* parent) : QObject(parent), settings_(settings) {
  if (settings_) {
    volume_ = settings_->audioVolume();
    muted_ = settings_->audioMuted();
    muteInBackground_ = settings_->audioMuteInBackground();
    step_ = settings_->audioVolumeStep();
    showVolume_ = settings_->audioShowVolume();
    device_ = settings_->audioOutputDevice();
    voiceGain_ = settings_->audioVoiceGain();
    pluginGain_ = settings_->audioPluginGain();
  }
  if (auto* app = qGuiApp) {
    connect(app, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState s) { setAppActive(s == Qt::ApplicationActive); });
  }
  // Device list is read when asked; a hot-plug refreshes the Settings row.
  auto* devices = new QMediaDevices(this);
  connect(devices, &QMediaDevices::audioOutputsChanged, this, &AudioController::devicesChanged);
}

double AudioController::localGain() const {
  if (muted_ || (muteInBackground_ && !appActive_)) return 0.0;
  return volume_ / 100.0;
}

double AudioController::remoteGain() const { return muted_ ? 0.0 : volume_ / 100.0; }

QByteArray AudioController::deviceIdBytes() const { return QByteArray::fromBase64(device_.toLatin1()); }

QVariantList AudioController::outputDevices() const {
  QVariantList l;
  l.append(QVariantMap{{QStringLiteral("value"), QString()}, {QStringLiteral("label"), tr("System default")}});
  for (const QAudioDevice& d : QMediaDevices::audioOutputs()) {
    l.append(QVariantMap{{QStringLiteral("value"), QString::fromLatin1(d.id().toBase64())}, {QStringLiteral("label"), d.description()}});
  }
  return l;
}

void AudioController::apply(bool notifyChanged) {
  if (notifyChanged) emit changed();
  emit gainsChanged();
}

QString AudioController::volumeText() const { return tr("Volume %1 %").arg(volume_); }

void AudioController::announce(const QString& text) {
  if (showVolume_) emit volumeNotice(text);
}

void AudioController::setVolume(int percent) {
  percent = std::clamp(percent, 0, 100);
  if (percent == volume_) return;
  volume_ = percent;
  if (settings_) settings_->setAudioVolume(volume_);
  apply();
}

void AudioController::setVoiceGain(int percent) {
  percent = std::clamp(percent, 0, 100);
  if (percent == voiceGain_) return;
  voiceGain_ = percent;
  if (settings_) settings_->setAudioVoiceGain(percent);
  apply();
}

void AudioController::setPluginGain(int percent) {
  percent = std::clamp(percent, 0, 100);
  if (percent == pluginGain_) return;
  pluginGain_ = percent;
  if (settings_) settings_->setAudioPluginGain(percent);
  apply();
}

void AudioController::setMuted(bool muted) {
  if (muted == muted_) return;
  muted_ = muted;
  if (settings_) settings_->setAudioMuted(muted_);
  apply();
}

void AudioController::volumeUp() {
  const bool wasMuted = muted_;
  setMuted(false);
  setVolume(volume_ + step_);
  announce(wasMuted ? tr("Sound on · %1 %").arg(volume_) : volumeText());
}

void AudioController::volumeDown() {
  setVolume(volume_ - step_);
  announce(muted_ ? tr("Muted") : volumeText());
}

void AudioController::toggleMute() {
  setMuted(!muted_);
  announce(muted_ ? tr("Muted") : tr("Sound on · %1 %").arg(volume_));
}

void AudioController::setMuteInBackground(bool on) {
  if (on == muteInBackground_) return;
  muteInBackground_ = on;
  if (settings_) settings_->setAudioMuteInBackground(on);
  apply();
}

void AudioController::setVolumeStep(int percent) {
  if ((percent != 5 && percent != 10) || percent == step_) return;
  step_ = percent;
  if (settings_) settings_->setAudioVolumeStep(percent);
  emit changed();
}

void AudioController::setShowVolume(bool on) {
  if (on == showVolume_) return;
  showVolume_ = on;
  if (settings_) settings_->setAudioShowVolume(on);
  emit changed();
}

void AudioController::setOutputDevice(const QString& id) {
  if (id == device_) return;
  device_ = id;
  if (settings_) settings_->setAudioOutputDevice(id);
  emit changed();
}

void AudioController::setAppActive(bool active) {
  if (active == appActive_) return;
  appActive_ = active;
  apply();
}

}  // namespace uzume::ui
