#pragma once
// fb.sound (0.23, ADR 0033 D6): plugin sounds. The plugin runtime decodes a 16-bit PCM WAV from the plugin package and hands
// the clip to a SoundSink (the app's plugin sound bus in the audio mixer). No Qt Multimedia here.

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <memory>

namespace uzume::plugins {

inline constexpr qint64 kMaxSoundFileBytes = 2 * 1024 * 1024;
inline constexpr double kMaxSoundSeconds = 10.0;
inline constexpr int kMaxSoundVoices = 8;  // per plugin

struct SoundClip {
  int sampleRate = 0;
  QByteArray pcm16;  // interleaved stereo int16 (mono is doubled)
  qint64 frames() const { return pcm16.size() / 4; }
};

// Parses a RIFF/WAVE file: 16-bit PCM, mono or stereo, 8000..192000 Hz, at most 10 s. On failure returns null and sets
// `error` to a short code: "unsupported_format" or "too_long".
std::shared_ptr<const SoundClip> decodeWav(const QByteArray& file, QString* error);

class SoundSink {
 public:
  virtual ~SoundSink() = default;
  // Starts the clip at volume 0..1; returns a handle (> 0) or 0 when it cannot play.
  virtual quint64 play(const QString& pluginId, std::shared_ptr<const SoundClip> clip, double volume) = 0;
  virtual void stopAll(const QString& pluginId) = 0;
  virtual int activeVoices(const QString& pluginId) const = 0;
};

}  // namespace uzume::plugins
