#include "pluginsoundbus.h"

#include <QMetaObject>
#include <algorithm>

namespace uzume::ui {

PluginSoundBus::PluginSoundBus(AudioMixer* mixer, QObject* parent) : QObject(parent), mixer_(mixer) {
  genId_ = mixer_->addGenerator(QStringLiteral("plugin"), [this](float* acc, int frames, int rate) { return generate(acc, frames, rate); });
  idle_.setInterval(2000);
  connect(&idle_, &QTimer::timeout, this, &PluginSoundBus::idleCheck);
}

PluginSoundBus::~PluginSoundBus() {
  mixer_->removeGenerator(genId_);
  if (acquired_) mixer_->release();
}

quint64 PluginSoundBus::play(const QString& pluginId, std::shared_ptr<const plugins::SoundClip> clip, double volume) {
  if (!clip || clip->frames() <= 0) return 0;
  quint64 h;
  {
    QMutexLocker lock(&mutex_);
    int mine = 0;
    for (const Voice& v : voices_) mine += v.plugin == pluginId;
    if (mine >= plugins::kMaxSoundVoices) return 0;
    h = nextHandle_++;
    voices_.push_back({h, pluginId, std::move(clip), static_cast<float>(std::clamp(volume, 0.0, 1.0)), 0.0});
  }
  QMetaObject::invokeMethod(this, "ensureOpen", Qt::QueuedConnection);  // the sink is opened on the UI thread
  return h;
}

void PluginSoundBus::stopAll(const QString& pluginId) {
  QMutexLocker lock(&mutex_);
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.plugin == pluginId; }), voices_.end());
}

int PluginSoundBus::activeVoices(const QString& pluginId) const {
  QMutexLocker lock(&mutex_);
  return static_cast<int>(std::count_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.plugin == pluginId; }));
}

int PluginSoundBus::totalVoices() const {
  QMutexLocker lock(&mutex_);
  return static_cast<int>(voices_.size());
}

void PluginSoundBus::ensureOpen() {
  if (!acquired_) {
    acquired_ = mixer_->acquire(48000);
    if (!acquired_) {  // no output device: nothing would ever consume the voices
      QMutexLocker lock(&mutex_);
      voices_.clear();
      return;
    }
  }
  if (!idle_.isActive()) idle_.start();
}

void PluginSoundBus::idleCheck() {
  if (totalVoices() > 0) return;
  idle_.stop();
  if (acquired_) {
    acquired_ = false;
    mixer_->release();
  }
}

// Audio thread, under the mixer lock: linear resampling of every voice to the output rate, summed into acc.
int PluginSoundBus::generate(float* acc, int frames, int outRate) {
  QMutexLocker lock(&mutex_);
  if (voices_.empty() || outRate <= 0) return 0;
  for (Voice& v : voices_) {
    const auto* s = reinterpret_cast<const qint16*>(v.clip->pcm16.constData());
    const qint64 n = v.clip->frames();
    const double step = static_cast<double>(v.clip->sampleRate) / outRate;
    for (int i = 0; i < frames && v.pos < static_cast<double>(n); ++i, v.pos += step) {
      const qint64 i0 = static_cast<qint64>(v.pos);
      const qint64 i1 = std::min(i0 + 1, n - 1);
      const float f = static_cast<float>(v.pos - static_cast<double>(i0));
      for (int c = 0; c < 2; ++c) {
        const float a = s[2 * i0 + c], b = s[2 * i1 + c];
        acc[2 * i + c] += (a + (b - a) * f) / 32768.0f * v.volume;
      }
    }
  }
  voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.pos >= static_cast<double>(v.clip->frames()); }), voices_.end());
  return frames;
}

}  // namespace uzume::ui
