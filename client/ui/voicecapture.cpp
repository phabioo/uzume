#include "voicecapture.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <algorithm>
#include <cstring>

#include "voicedsp.h"

namespace uzume::ui {

VoiceCapture::VoiceCapture(QObject* parent) : QObject(parent) {
  fmt_.setSampleRate(uzume::kVoiceRate);
  fmt_.setChannelCount(1);
  fmt_.setSampleFormat(QAudioFormat::Int16);
}

VoiceCapture::~VoiceCapture() { stop(); }

QVariantList VoiceCapture::devices() {
  QVariantList out;
  const QAudioDevice def = QMediaDevices::defaultAudioInput();
  if (!def.isNull()) out.append(QVariantMap{{QStringLiteral("value"), QString()}, {QStringLiteral("label"), QObject::tr("System default (%1)").arg(def.description())}});
  for (const QAudioDevice& d : QMediaDevices::audioInputs()) {
    out.append(QVariantMap{{QStringLiteral("value"), QString::fromLatin1(d.id().toBase64())}, {QStringLiteral("label"), d.description()}});
  }
  if (out.isEmpty()) out.append(QVariantMap{{QStringLiteral("value"), QString()}, {QStringLiteral("label"), QObject::tr("No microphone found")}});
  return out;
}

void VoiceCapture::setStatus(Status s, const QString& detail) {
  if (status_ == s && detail_ == detail) return;
  status_ = s;
  detail_ = detail;
  emit statusChanged();
}

void VoiceCapture::setConvertFormat(const QAudioFormat& fmt) {
  fmt_ = fmt;
  pending_.clear();
  carry_.clear();
  pos_ = 0.0;
  hasPrev_ = false;
}

VoiceCapture::Status VoiceCapture::start(const QByteArray& deviceId) {
  stop();
  QAudioDevice dev = QMediaDevices::defaultAudioInput();
  if (!deviceId.isEmpty()) {
    for (const QAudioDevice& d : QMediaDevices::audioInputs()) {
      if (d.id() == deviceId) {
        dev = d;
        break;
      }
    }
  }
  if (dev.isNull()) {
    setStatus(Status::NoDevice, tr("No microphone found"));
    return status_;
  }
  QAudioFormat want;
  want.setSampleRate(uzume::kVoiceRate);
  want.setChannelCount(1);
  want.setSampleFormat(QAudioFormat::Int16);
  setConvertFormat(dev.isFormatSupported(want) ? want : dev.preferredFormat());
  source_ = std::make_unique<QAudioSource>(dev, fmt_);
  source_->setBufferSize(fmt_.bytesForDuration(100000));
  connect(source_.get(), &QAudioSource::stateChanged, this, [this](QAudio::State st) {
    if (st == QAudio::StoppedState && source_ && source_->error() != QAudio::NoError) {
      setStatus(Status::Blocked, tr("The system does not let Uzume use the microphone"));
    }
  });
  io_ = source_->start();
  if (!io_ || source_->error() != QAudio::NoError) {
    source_.reset();
    io_ = nullptr;
    setStatus(Status::Blocked, tr("The system does not let Uzume use the microphone"));
    return status_;
  }
  connect(io_, &QIODevice::readyRead, this, &VoiceCapture::onReadyRead);
  setStatus(Status::Running, dev.description());
  return status_;
}

void VoiceCapture::stop() {
  if (source_) {
    source_->stop();
    source_.reset();
  }
  io_ = nullptr;
  pending_.clear();
  carry_.clear();
  if (status_ != Status::Stopped) setStatus(Status::Stopped, QString());
}

void VoiceCapture::onReadyRead() {
  if (!io_) return;
  const QByteArray raw = io_->readAll();
  if (raw.isEmpty()) return;
  convert(raw.constData(), raw.size(), pending_);
  constexpr int kBytes = uzume::kVoiceFrame * int(sizeof(int16_t));
  while (pending_.size() >= kBytes) {
    emit frame(pending_.left(kBytes));
    pending_.remove(0, kBytes);
  }
}

void VoiceCapture::convert(const char* data, qsizetype bytes, QByteArray& out) {
  const int ch = std::max(1, fmt_.channelCount());
  const int bps = std::max(1, fmt_.bytesPerSample());
  const qsizetype frameBytes = static_cast<qsizetype>(ch) * bps;
  QByteArray in = carry_;
  in.append(data, bytes);
  const qsizetype frames = in.size() / frameBytes;
  carry_ = in.mid(frames * frameBytes);
  // Downmix to mono float.
  std::vector<float> mono(static_cast<size_t>(frames));
  const char* p = in.constData();
  for (qsizetype i = 0; i < frames; ++i) {
    double sum = 0;
    for (int c = 0; c < ch; ++c) {
      const char* s = p + (i * ch + c) * bps;
      switch (fmt_.sampleFormat()) {
        case QAudioFormat::UInt8: sum += (double(*reinterpret_cast<const quint8*>(s)) - 128.0) / 128.0; break;
        case QAudioFormat::Int16: { qint16 v; std::memcpy(&v, s, 2); sum += v / 32768.0; break; }
        case QAudioFormat::Int32: { qint32 v; std::memcpy(&v, s, 4); sum += v / 2147483648.0; break; }
        case QAudioFormat::Float: { float v; std::memcpy(&v, s, 4); sum += v; break; }
        default: break;
      }
    }
    mono[static_cast<size_t>(i)] = static_cast<float>(sum / ch);
  }
  // Resample to 48 kHz (linear, stateful) and convert to int16.
  const double step = double(std::max(1, fmt_.sampleRate())) / double(uzume::kVoiceRate);
  const bool pass = fmt_.sampleRate() == uzume::kVoiceRate;
  auto put = [&out](double v) {
    const int s = std::clamp(int(std::lround(v * 32767.0)), -32768, 32767);
    const qint16 q = static_cast<qint16>(s);
    out.append(reinterpret_cast<const char*>(&q), 2);
  };
  if (pass) {
    for (float v : mono) put(v);
    return;
  }
  // Position 0 of the buffer is `prev_` (the last sample of the previous chunk), index i + 1 is mono[i].
  const double have = double(mono.size());
  if (!hasPrev_ && !mono.empty()) {
    prev_ = mono[0];
    hasPrev_ = true;
    pos_ = 1.0;
  }
  while (pos_ < have) {
    const double i0 = std::floor(pos_);
    const double frac = pos_ - i0;
    const double a = i0 <= 0.0 ? prev_ : mono[static_cast<size_t>(i0) - 1];
    const double b = mono[static_cast<size_t>(i0)];
    put(a + (b - a) * frac);
    pos_ += step;
  }
  pos_ -= have;
  if (!mono.empty()) prev_ = mono.back();
}

}  // namespace uzume::ui
