#pragma once
// Plugin sound bus (0.23, ADR 0033 D6): the SoundSink behind uz.sound. Voices (decoded WAV clips) are mixed by a generator
// that the AudioMixer pulls with its sink, so there is no timer on the playing path. The sink is held open (mixer reference) only
// while sounds play. play() and stopAll() are called from the plugin thread.

#include <QMutex>
#include <QObject>
#include <QTimer>
#include <vector>

#include "audiomixer.h"
#include "plugin_sound.h"

namespace uzume::ui {

class PluginSoundBus : public QObject, public plugins::SoundSink {
  Q_OBJECT
 public:
  explicit PluginSoundBus(AudioMixer* mixer, QObject* parent = nullptr);
  ~PluginSoundBus() override;

  quint64 play(const QString& pluginId, std::shared_ptr<const plugins::SoundClip> clip, double volume) override;
  void stopAll(const QString& pluginId) override;
  int activeVoices(const QString& pluginId) const override;
  int totalVoices() const;

 private slots:
  void ensureOpen();
  void idleCheck();

 private:
  struct Voice {
    quint64 handle = 0;
    QString plugin;
    std::shared_ptr<const plugins::SoundClip> clip;
    float volume = 1.0f;
    double pos = 0.0;  // in clip frames
  };
  int generate(float* acc, int frames, int outRate);

  AudioMixer* mixer_;
  int genId_ = 0;
  mutable QMutex mutex_;
  std::vector<Voice> voices_;
  quint64 nextHandle_ = 1;
  bool acquired_ = false;  // UI thread
  QTimer idle_;
};

}  // namespace uzume::ui
