#pragma once
// AudioMixer (0.23 audio stage 2, ADR 0033 D6): the one output sink of the app. Channels: game (the local game), tiles (one
// input per watched Session), voice (one input per person, filled by the voice chat of 0.23 package 7) and plugin sounds (one
// bus of generators). Sources push interleaved stereo int16 at outputRate() into their input; the sink pulls (pull mode, same
// short-read rule as before: a read returns only what is queued, silence only while everything is dry), mixes in float and
// soft-clips. Gains: master x mute x channel x person x input; ducking halves game and tiles. Routing: exclusive (only the
// focused game/tile input is audible, as before) or "Mix all tiles" (every game/tile input, each with its own gain).
// Thread-safe: sources push from the UI thread, the sink pulls from Qt's audio thread. Without an output device (or headless in
// tests) the class mixes all the same, only the sink is missing.

#include <QAudioFormat>
#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

class QAudioSink;

namespace uzume::ui {

class MixerDevice;

class AudioMixer : public QObject {
  Q_OBJECT
 public:
  enum Channel { Game = 0, Tiles = 1, Voice = 2, Plugin = 3 };
  static constexpr int kChannels = 4;
  using InputId = int;  // > 0; 0 = none
  // Plugin sound bus: adds up to `frames` frames of stereo float at `outRate` into `acc` (zeroed by the mixer) and returns how
  // many it wrote (0 = idle). Called from the audio thread under the mixer lock: it must not call back into the mixer.
  using Generator = std::function<int(float* acc, int frames, int outRate)>;

  explicit AudioMixer(QObject* parent = nullptr);
  ~AudioMixer() override;

  static QString channelName(Channel c);
  static bool channelFromName(const QString& name, Channel& out);

  // ---- output (UI thread) ----
  // Reference counted: the first acquire opens the sink on the chosen device (rateHint = fallback rate when the device has no
  // usable preferred format); false = no device, the sources stay silent. release() closes the sink with the last reference.
  bool acquire(int rateHint = 48000);
  void release();
  bool isOpen() const { return refs_ > 0 && (sink_ != nullptr || headless_); }
  // Output device id (QAudioDevice::id()); empty or gone = system default. Applies when the sink is opened next.
  void setDeviceId(const QByteArray& id) { deviceId_ = id; }
  QByteArray deviceId() const { return deviceId_; }
  // Tests: no sink, the mixer is read by hand with mix(). Counts as a reference.
  void openHeadless(int rate);
  int outputRate() const { return rate_; }
  QAudioFormat outputFormat() const { return fmt_; }
  double sinkBufferedMs() const;  // what the sink still holds

  // ---- inputs ----
  // `key` names the person (user id) for per-person gains; empty = none.
  InputId addInput(Channel channel, const QString& key = QString());
  void removeInput(InputId id);
  // Voice chat (package 7): one input per person, filled with that person's decoded voice.
  InputId addVoiceInput(const QString& userId) { return addInput(Voice, userId); }
  void removeVoiceInput(InputId id) { removeInput(id); }
  // Plugin sound bus: a generator pulled with the sink. Returns the generator id (removeGenerator).
  int addGenerator(const QString& key, Generator gen);
  void removeGenerator(int genId);
  // Interleaved stereo int16 at outputRate(). Dropped while the input is not audible; returns the bytes dropped over the cap.
  qint64 push(InputId id, const QByteArray& pcm16);
  qint64 queuedFrames(InputId id) const;
  void clearInput(InputId id);
  int underruns(InputId id) const;
  void setUnderrunCounting(InputId id, bool on);

  // ---- gains and routing (any thread) ----
  void setMaster(double gain);  // 0..1
  double master() const;
  void setMuted(bool muted);
  bool muted() const;
  void setGameSilenced(bool silenced);  // "Muted in the background": the local game only
  bool gameSilenced() const;
  void setChannelGain(Channel c, double gain);  // 0..1
  double channelGain(Channel c) const;
  void setPersonGain(Channel c, const QString& key, double gain);  // 0..1, by person key; unset = 1
  double personGain(Channel c, const QString& key) const;
  void setInputGain(InputId id, double gain);
  void setDucking(bool on);  // halves game and tiles (voice chat, package 7)
  bool ducking() const;
  void setMixAll(bool on);   // off: only the focused input of game/tiles is audible
  bool mixAll() const;
  void setFocus(InputId id); // the audible game/tile input while mix-all is off
  InputId focus() const;
  // Whether the input is mixed at all (routing and a gain above zero). Voice and plugin inputs are always routed.
  bool isAudible(InputId id) const;
  bool isRouted(InputId id) const;
  double effectiveGain(InputId id) const;  // master x mute x channel x person x input x ducking (0 when not routed)

  // ---- mixing ----
  // Fills up to `frames` stereo float frames and returns how many were produced: the most any audible input has queued
  // (at most `frames`), or `frames` of silence while everything is dry. The sink calls this; tests call it directly.
  int mix(float* out, int frames);
  static float softClip(float x);

  // State for the MCP audio_mixer tool: master, muted, ducking, mix_all, channels{gain}, inputs[{id, channel, key, gain, audible, queued_frames}].
  QVariantMap state() const;

 signals:
  void routingChanged();  // focus, mix-all or a gain changed

 private:
  struct Input {
    InputId id = 0;
    Channel channel = Game;
    QString key;
    double gain = 1.0;
    std::deque<float> fifo;  // interleaved stereo float
    bool hadData = false;
    bool counting = true;
    int underruns = 0;
  };
  struct Gen {
    int id = 0;
    QString key;
    Generator fn;
  };
  Input* find(InputId id);
  const Input* find(InputId id) const;
  bool routedLocked(const Input& in) const;
  double gainLocked(const Input& in) const;
  double channelGainLocked(Channel c) const;
  qint64 maxQueuedFrames() const;
  void openSink(int rateHint);
  void closeSink();

  mutable QMutex mutex_;
  std::vector<std::unique_ptr<Input>> inputs_;
  std::vector<Gen> gens_;
  InputId nextId_ = 1;
  int nextGen_ = 1;
  double master_ = 1.0;
  bool muted_ = false;
  bool gameSilenced_ = false;
  bool ducking_ = false;
  bool mixAll_ = false;
  InputId focus_ = 0;
  std::array<double, kChannels> channelGain_{1.0, 1.0, 1.0, 1.0};
  std::array<QHash<QString, double>, kChannels> personGain_;
  std::vector<float> scratch_;
  std::vector<float> genBuf_;
  int rate_ = 48000;

  int refs_ = 0;
  bool headless_ = false;
  QByteArray deviceId_;
  QAudioFormat fmt_;
  std::unique_ptr<MixerDevice> device_;  // before sink_: the sink is destroyed first
  std::unique_ptr<QAudioSink> sink_;
  friend class MixerDevice;
};

}  // namespace uzume::ui
