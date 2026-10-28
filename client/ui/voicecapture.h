#pragma once
// Microphone capture of the voice chat (ADR 0033 D7): a QAudioSource on the chosen device, converted to 48 kHz mono int16 and cut into
// 20 ms frames. The device is opened in that format when it supports it, else in its preferred format and converted.

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <memory>

class QAudioSource;
class QIODevice;

namespace uzume::ui {

class VoiceCapture : public QObject {
  Q_OBJECT
 public:
  enum class Status { Stopped, Running, NoDevice, Blocked };
  explicit VoiceCapture(QObject* parent = nullptr);
  ~VoiceCapture() override;

  // `deviceId` is QAudioDevice::id() bytes; empty or gone = the system default.
  Status start(const QByteArray& deviceId);
  void stop();
  Status status() const { return status_; }
  QString detail() const { return detail_; }
  bool running() const { return status_ == Status::Running; }

  // [{value, label}] of the input devices, default first; value = base64 of the id ("" = system default).
  static QVariantList devices();
  static QByteArray decodeId(const QString& value) { return QByteArray::fromBase64(value.toLatin1()); }

  // Conversion helper (tests): raw device bytes -> 48 kHz mono int16 appended to `out`. State is kept between calls.
  void convert(const char* data, qsizetype bytes, QByteArray& out);
  void setConvertFormat(const QAudioFormat& fmt);

 signals:
  void frame(const QByteArray& mono16);  // 960 samples
  void statusChanged();

 private:
  void onReadyRead();
  void setStatus(Status s, const QString& detail);

  std::unique_ptr<QAudioSource> source_;
  QIODevice* io_ = nullptr;
  Status status_ = Status::Stopped;
  QString detail_;
  QAudioFormat fmt_;
  QByteArray pending_;   // converted samples not yet a full frame
  QByteArray carry_;     // incomplete input sample/frame between reads
  double pos_ = 0.0;     // resampler read position (input samples)
  double prev_ = 0.0;    // last input sample (mono, -1..1)
  bool hasPrev_ = false;
};

}  // namespace uzume::ui
