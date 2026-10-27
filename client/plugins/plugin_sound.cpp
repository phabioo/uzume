#include "plugin_sound.h"

#include <cstring>

namespace uzume::plugins {

namespace {
quint32 rd32(const char* p) {
  const auto* u = reinterpret_cast<const unsigned char*>(p);
  return u[0] | (u[1] << 8) | (u[2] << 16) | (static_cast<quint32>(u[3]) << 24);
}
quint16 rd16(const char* p) {
  const auto* u = reinterpret_cast<const unsigned char*>(p);
  return static_cast<quint16>(u[0] | (u[1] << 8));
}
}  // namespace

std::shared_ptr<const SoundClip> decodeWav(const QByteArray& f, QString* error) {
  auto fail = [&](const char* e) -> std::shared_ptr<const SoundClip> {
    if (error) *error = QLatin1String(e);
    return nullptr;
  };
  if (f.size() < 12 || std::memcmp(f.constData(), "RIFF", 4) != 0 || std::memcmp(f.constData() + 8, "WAVE", 4) != 0)
    return fail("unsupported_format");
  int channels = 0, rate = 0, bits = 0;
  bool haveFmt = false;
  const char* data = nullptr;
  qint64 dataLen = 0;
  qint64 pos = 12;
  while (pos + 8 <= f.size()) {
    const char* h = f.constData() + pos;
    const qint64 len = rd32(h + 4);
    const qint64 body = pos + 8;
    if (std::memcmp(h, "fmt ", 4) == 0) {
      if (len < 16 || body + 16 > f.size()) return fail("unsupported_format");
      const quint16 tag = rd16(f.constData() + body);
      if (tag != 1) return fail("unsupported_format");  // PCM only
      channels = rd16(f.constData() + body + 2);
      rate = static_cast<int>(rd32(f.constData() + body + 4));
      bits = rd16(f.constData() + body + 14);
      haveFmt = true;
    } else if (std::memcmp(h, "data", 4) == 0) {
      data = f.constData() + body;
      dataLen = std::min<qint64>(len, f.size() - body);
      break;
    }
    pos = body + len + (len & 1);
  }
  if (!haveFmt || !data || bits != 16 || (channels != 1 && channels != 2) || rate < 8000 || rate > 192000)
    return fail("unsupported_format");
  const qint64 frames = dataLen / (2 * channels);
  if (frames <= 0) return fail("unsupported_format");
  if (static_cast<double>(frames) / rate > kMaxSoundSeconds) return fail("too_long");
  auto clip = std::make_shared<SoundClip>();
  clip->sampleRate = rate;
  clip->pcm16.resize(static_cast<qsizetype>(frames * 4));
  if (channels == 2) {
    std::memcpy(clip->pcm16.data(), data, static_cast<size_t>(frames * 4));
  } else {
    char* o = clip->pcm16.data();
    for (qint64 i = 0; i < frames; ++i) {
      std::memcpy(o + 4 * i, data + 2 * i, 2);
      std::memcpy(o + 4 * i + 2, data + 2 * i, 2);
    }
  }
  return clip;
}

}  // namespace uzume::plugins
