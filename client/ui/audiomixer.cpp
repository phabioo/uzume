#include "audiomixer.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "audiooutput.h"  // lcAudio

namespace uzume::ui {

namespace {
constexpr int kBufferMs = 150;
constexpr int kFallbackRate = 48000;
constexpr float kKnee = 0.95f;
const char* const kChannelNames[AudioMixer::kChannels] = {"game", "tiles", "voice", "plugin"};

double clamp01(double g) { return std::clamp(g, 0.0, 1.0); }
bool convertible(QAudioFormat::SampleFormat f) {
  return f == QAudioFormat::UInt8 || f == QAudioFormat::Int16 || f == QAudioFormat::Int32 || f == QAudioFormat::Float;
}
}  // namespace

// Pull-mode device in front of the mixer: converts the float mix into the sink format.
class MixerDevice : public QIODevice {
 public:
  explicit MixerDevice(AudioMixer* m) : mixer_(m) {}
  bool isSequential() const override { return true; }
  qint64 bytesAvailable() const override {
    // Like AudioQueueDevice: what is queued, else a nominal amount so the sink keeps pulling.
    const qint64 bpf = mixer_->fmt_.bytesPerFrame();
    qint64 frames = mixer_->maxQueuedFrames();
    if (frames <= 0) frames = static_cast<qint64>(mixer_->rate_) * kBufferMs / 1000;
    return frames * bpf + QIODevice::bytesAvailable();
  }

 protected:
  qint64 readData(char* data, qint64 maxlen) override {
    const int bpf = mixer_->fmt_.bytesPerFrame();
    const qint64 frames = maxlen / bpf;
    if (frames <= 0) return 0;
    buf_.resize(static_cast<size_t>(frames) * 2);
    const int n = mixer_->mix(buf_.data(), static_cast<int>(frames));
    const size_t samples = static_cast<size_t>(n) * 2;
    switch (mixer_->fmt_.sampleFormat()) {
      case QAudioFormat::UInt8: {
        auto* o = reinterpret_cast<unsigned char*>(data);
        for (size_t i = 0; i < samples; ++i) o[i] = static_cast<unsigned char>(std::lround(buf_[i] * 127.0f) + 128);
        break;
      }
      case QAudioFormat::Int32: {
        for (size_t i = 0; i < samples; ++i) {
          const qint32 v = static_cast<qint32>(std::clamp(static_cast<double>(buf_[i]) * 2147483647.0, -2147483648.0, 2147483647.0));
          std::memcpy(data + 4 * i, &v, 4);
        }
        break;
      }
      case QAudioFormat::Float:
        std::memcpy(data, buf_.data(), samples * 4);
        break;
      default: {  // Int16
        for (size_t i = 0; i < samples; ++i) {
          const qint16 v = static_cast<qint16>(std::lround(std::clamp(buf_[i], -1.0f, 1.0f) * 32767.0f));
          std::memcpy(data + 2 * i, &v, 2);
        }
        break;
      }
    }
    return static_cast<qint64>(n) * bpf;
  }
  qint64 writeData(const char*, qint64) override { return -1; }

 private:
  AudioMixer* mixer_;
  std::vector<float> buf_;
};

AudioMixer::AudioMixer(QObject* parent) : QObject(parent) {}

AudioMixer::~AudioMixer() { closeSink(); }

QString AudioMixer::channelName(Channel c) { return QLatin1String(kChannelNames[c]); }

bool AudioMixer::channelFromName(const QString& name, Channel& out) {
  for (int i = 0; i < kChannels; ++i) {
    if (name == QLatin1String(kChannelNames[i])) {
      out = static_cast<Channel>(i);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- output

bool AudioMixer::acquire(int rateHint) {
  if (refs_ > 0) {
    ++refs_;
    return isOpen();
  }
  openSink(rateHint);
  if (!sink_) return false;
  refs_ = 1;
  return true;
}

void AudioMixer::release() {
  if (refs_ <= 0) return;
  if (--refs_ == 0) closeSink();
}

void AudioMixer::openHeadless(int rate) {
  headless_ = true;
  rate_ = rate;
  fmt_.setSampleRate(rate);
  fmt_.setChannelCount(2);
  fmt_.setSampleFormat(QAudioFormat::Float);
  ++refs_;
}

void AudioMixer::openSink(int rateHint) {
  if (rateHint <= 0) rateHint = kFallbackRate;
  QAudioDevice device;
  if (!deviceId_.isEmpty()) {
    for (const QAudioDevice& d : QMediaDevices::audioOutputs()) {
      if (d.id() == deviceId_) {
        device = d;
        break;
      }
    }
    if (device.isNull()) qCWarning(lcAudio) << "chosen output device is not available; using the system default";
  }
  if (device.isNull()) device = QMediaDevices::defaultAudioOutput();
  if (device.isNull()) {
    qCWarning(lcAudio) << "no default audio output device; the app runs without sound";
    return;
  }
  const QAudioFormat pref = device.preferredFormat();
  qCInfo(lcAudio) << "Mixer device:" << device.description() << "preferred:" << pref.sampleRate() << "Hz," << pref.channelCount()
                  << "ch, Format" << static_cast<int>(pref.sampleFormat());
  QAudioFormat fmt;
  fmt.setChannelCount(2);
  bool chosen = false;
  if (pref.sampleRate() > 0 && convertible(pref.sampleFormat())) {
    fmt.setSampleRate(pref.sampleRate());
    fmt.setSampleFormat(pref.sampleFormat());
    chosen = device.isFormatSupported(fmt);
  }
  if (!chosen) {
    fmt.setSampleFormat(QAudioFormat::Int16);
    fmt.setSampleRate(rateHint);
    if (!device.isFormatSupported(fmt)) {
      fmt.setSampleRate(kFallbackRate);
      if (!device.isFormatSupported(fmt)) {
        qCWarning(lcAudio) << "no supported output format; the app runs without sound";
        return;
      }
    }
  }
  fmt_ = fmt;
  rate_ = fmt.sampleRate();
  const int bufBytes = rate_ * fmt.bytesPerFrame() * kBufferMs / 1000;
  auto dev = std::make_unique<MixerDevice>(this);
  dev->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
  auto sink = std::make_unique<QAudioSink>(device, fmt);
  sink->setBufferSize(bufBytes);
  sink->start(dev.get());  // pull mode
  qCInfo(lcAudio) << "Mixer output:" << rate_ << "Hz, 2 ch, Format" << static_cast<int>(fmt.sampleFormat()) << "error:"
                  << static_cast<int>(sink->error());
  if (sink->error() != QAudio::NoError) {
    sink->stop();
    return;
  }
  device_ = std::move(dev);
  sink_ = std::move(sink);
}

void AudioMixer::closeSink() {
  if (sink_) {
    sink_->stop();
    sink_.reset();
  }
  if (device_) {
    device_->close();
    device_.reset();
  }
  QMutexLocker lock(&mutex_);
  for (auto& in : inputs_) {
    in->fifo.clear();
    in->hadData = false;
  }
}

double AudioMixer::sinkBufferedMs() const {
  if (!sink_ || rate_ <= 0 || fmt_.bytesPerFrame() <= 0) return 0.0;
  const qint64 filled = std::max<qint64>(0, sink_->bufferSize() - sink_->bytesFree());
  return static_cast<double>(filled) / fmt_.bytesPerFrame() * 1000.0 / rate_;
}

// ---------------------------------------------------------------- inputs

AudioMixer::Input* AudioMixer::find(InputId id) {
  for (auto& in : inputs_)
    if (in->id == id) return in.get();
  return nullptr;
}

const AudioMixer::Input* AudioMixer::find(InputId id) const {
  for (const auto& in : inputs_)
    if (in->id == id) return in.get();
  return nullptr;
}

AudioMixer::InputId AudioMixer::addInput(Channel channel, const QString& key) {
  QMutexLocker lock(&mutex_);
  auto in = std::make_unique<Input>();
  in->id = nextId_++;
  in->channel = channel;
  in->key = key;
  const InputId id = in->id;
  inputs_.push_back(std::move(in));
  return id;
}

void AudioMixer::removeInput(InputId id) {
  bool focusLost = false;
  {
    QMutexLocker lock(&mutex_);
    inputs_.erase(std::remove_if(inputs_.begin(), inputs_.end(), [id](const auto& in) { return in->id == id; }), inputs_.end());
    if (focus_ == id) {
      focus_ = 0;
      focusLost = true;
    }
  }
  if (focusLost) emit routingChanged();
}

int AudioMixer::addGenerator(const QString& key, Generator gen) {
  QMutexLocker lock(&mutex_);
  const int id = nextGen_++;
  gens_.push_back({id, key, std::move(gen)});
  return id;
}

void AudioMixer::removeGenerator(int genId) {
  QMutexLocker lock(&mutex_);
  gens_.erase(std::remove_if(gens_.begin(), gens_.end(), [genId](const Gen& g) { return g.id == genId; }), gens_.end());
}

qint64 AudioMixer::push(InputId id, const QByteArray& pcm16) {
  QMutexLocker lock(&mutex_);
  Input* in = find(id);
  if (!in || pcm16.isEmpty()) return 0;
  if (!routedLocked(*in) || gainLocked(*in) <= 0.0) return 0;  // silent on purpose: nothing piles up
  const qsizetype samples = pcm16.size() / 2;
  const auto* s = reinterpret_cast<const unsigned char*>(pcm16.constData());
  for (qsizetype i = 0; i < samples; ++i) {
    const qint16 v = static_cast<qint16>(static_cast<quint16>(s[2 * i] | (s[2 * i + 1] << 8)));
    in->fifo.push_back(static_cast<float>(v) / 32768.0f);
  }
  in->hadData = true;
  const size_t cap = static_cast<size_t>(rate_) * kBufferMs / 1000 * 2;  // same cap as the old queue: oldest frames go
  qint64 dropped = 0;
  if (in->fifo.size() > cap) {
    size_t drop = in->fifo.size() - cap;
    drop += drop % 2;
    in->fifo.erase(in->fifo.begin(), in->fifo.begin() + static_cast<std::ptrdiff_t>(drop));
    dropped = static_cast<qint64>(drop / 2) * 4;
  }
  return dropped;
}

qint64 AudioMixer::queuedFrames(InputId id) const {
  QMutexLocker lock(&mutex_);
  const Input* in = find(id);
  return in ? static_cast<qint64>(in->fifo.size() / 2) : 0;
}

qint64 AudioMixer::maxQueuedFrames() const {
  QMutexLocker lock(&mutex_);
  size_t best = 0;
  for (const auto& in : inputs_)
    if (routedLocked(*in)) best = std::max(best, in->fifo.size() / 2);
  return static_cast<qint64>(best);
}

void AudioMixer::clearInput(InputId id) {
  QMutexLocker lock(&mutex_);
  if (Input* in = find(id)) {
    in->fifo.clear();
    in->hadData = false;
  }
}

int AudioMixer::underruns(InputId id) const {
  QMutexLocker lock(&mutex_);
  const Input* in = find(id);
  return in ? in->underruns : 0;
}

void AudioMixer::setUnderrunCounting(InputId id, bool on) {
  QMutexLocker lock(&mutex_);
  if (Input* in = find(id)) {
    if (on && !in->counting) in->hadData = false;  // the dry period while counting was off is not an underrun
    in->counting = on;
  }
}

// ---------------------------------------------------------------- gains and routing

#define UZUME_MIXER_SET(expr) \
  {                           \
    {                         \
      QMutexLocker lock(&mutex_); \
      expr;                   \
    }                         \
    emit routingChanged();    \
  }

void AudioMixer::setMaster(double gain) UZUME_MIXER_SET(master_ = clamp01(gain))
double AudioMixer::master() const { QMutexLocker l(&mutex_); return master_; }
void AudioMixer::setMuted(bool muted) UZUME_MIXER_SET(muted_ = muted)
bool AudioMixer::muted() const { QMutexLocker l(&mutex_); return muted_; }
void AudioMixer::setGameSilenced(bool silenced) UZUME_MIXER_SET(gameSilenced_ = silenced)
bool AudioMixer::gameSilenced() const { QMutexLocker l(&mutex_); return gameSilenced_; }
void AudioMixer::setChannelGain(Channel c, double gain) UZUME_MIXER_SET(channelGain_[c] = clamp01(gain))
double AudioMixer::channelGain(Channel c) const { QMutexLocker l(&mutex_); return channelGain_[c]; }
void AudioMixer::setPersonGain(Channel c, const QString& key, double gain) UZUME_MIXER_SET(personGain_[c].insert(key, clamp01(gain)))
double AudioMixer::personGain(Channel c, const QString& key) const {
  QMutexLocker l(&mutex_);
  return personGain_[c].value(key, 1.0);
}
void AudioMixer::setInputGain(InputId id, double gain) UZUME_MIXER_SET(if (Input* in = find(id)) in->gain = clamp01(gain))
void AudioMixer::setDucking(bool on) UZUME_MIXER_SET(ducking_ = on)
bool AudioMixer::ducking() const { QMutexLocker l(&mutex_); return ducking_; }
void AudioMixer::setMixAll(bool on) {
  {
    QMutexLocker lock(&mutex_);
    if (mixAll_ == on) return;
    mixAll_ = on;
  }
  emit routingChanged();
}
bool AudioMixer::mixAll() const { QMutexLocker l(&mutex_); return mixAll_; }
void AudioMixer::setFocus(InputId id) {
  {
    QMutexLocker lock(&mutex_);
    if (focus_ == id) return;
    focus_ = id;
  }
  emit routingChanged();
}
AudioMixer::InputId AudioMixer::focus() const { QMutexLocker l(&mutex_); return focus_; }

#undef UZUME_MIXER_SET

bool AudioMixer::routedLocked(const Input& in) const {
  if (in.channel == Game || in.channel == Tiles) return mixAll_ || in.id == focus_;
  return true;
}

double AudioMixer::channelGainLocked(Channel c) const {
  double g = channelGain_[c];
  if (c == Game && gameSilenced_) g = 0.0;
  if (ducking_ && (c == Game || c == Tiles)) g *= 0.5;
  return g;
}

double AudioMixer::gainLocked(const Input& in) const {
  if (muted_) return 0.0;
  return master_ * channelGainLocked(in.channel) * personGain_[in.channel].value(in.key, 1.0) * in.gain;
}

bool AudioMixer::isRouted(InputId id) const {
  QMutexLocker lock(&mutex_);
  const Input* in = find(id);
  return in && routedLocked(*in);
}

bool AudioMixer::isAudible(InputId id) const {
  QMutexLocker lock(&mutex_);
  const Input* in = find(id);
  return in && routedLocked(*in) && gainLocked(*in) > 0.0;
}

double AudioMixer::effectiveGain(InputId id) const {
  QMutexLocker lock(&mutex_);
  const Input* in = find(id);
  return in && routedLocked(*in) ? gainLocked(*in) : 0.0;
}

// ---------------------------------------------------------------- mixing

float AudioMixer::softClip(float x) {
  const float a = std::fabs(x);
  if (a <= kKnee) return x;
  const float y = kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee));
  return x < 0 ? -y : y;
}

int AudioMixer::mix(float* out, int frames) {
  if (frames <= 0) return 0;
  QMutexLocker lock(&mutex_);
  const size_t want = static_cast<size_t>(frames) * 2;
  size_t produced = 0;  // samples
  std::fill(out, out + want, 0.0f);
  for (auto& in : inputs_) {
    if (!routedLocked(*in)) {
      in->fifo.clear();
      continue;
    }
    const float g = static_cast<float>(gainLocked(*in));
    const size_t avail = std::min(want, in->fifo.size() - in->fifo.size() % 2);
    if (avail == 0) {
      if (in->hadData) {
        if (in->counting) ++in->underruns;
        in->hadData = false;
      }
      continue;
    }
    for (size_t i = 0; i < avail; ++i) out[i] += in->fifo[i] * g;
    in->fifo.erase(in->fifo.begin(), in->fifo.begin() + static_cast<std::ptrdiff_t>(avail));
    produced = std::max(produced, avail);
  }
  if (!gens_.empty()) {
    // Generators never lengthen a buffer the routed inputs cut short (that would pad the game with silence mid-buffer);
    // they fill at most what the inputs provided, or the whole request when nothing else plays.
    const int genFrames = produced > 0 ? static_cast<int>(produced / 2) : frames;
    genBuf_.assign(want, 0.0f);
    const float g = muted_ ? 0.0f : static_cast<float>(master_ * channelGain_[Plugin]);
    for (const Gen& gen : gens_) {
      std::fill(genBuf_.begin(), genBuf_.end(), 0.0f);
      const int n = std::clamp(gen.fn(genBuf_.data(), genFrames, rate_), 0, genFrames);
      for (int i = 0; i < n * 2; ++i) out[i] += genBuf_[static_cast<size_t>(i)] * g;
      produced = std::max(produced, static_cast<size_t>(n) * 2);
    }
  }
  if (produced == 0) return frames;  // completely dry: silence for the whole request
  for (size_t i = 0; i < produced; ++i) out[i] = softClip(out[i]);
  if (tap_) tap_(out, static_cast<int>(produced / 2), rate_);
  return static_cast<int>(produced / 2);
}

void AudioMixer::setOutputTap(OutputTap tap) {
  QMutexLocker lock(&mutex_);
  tap_ = std::move(tap);
}

QVariantMap AudioMixer::state() const {
  QMutexLocker lock(&mutex_);
  QVariantMap channels;
  for (int i = 0; i < kChannels; ++i) channels.insert(QLatin1String(kChannelNames[i]), channelGain_[i]);
  QVariantList inputs;
  for (const auto& in : inputs_) {
    inputs.append(QVariantMap{{QStringLiteral("id"), in->id},
                              {QStringLiteral("channel"), channelName(in->channel)},
                              {QStringLiteral("key"), in->key},
                              {QStringLiteral("gain"), personGain_[in->channel].value(in->key, 1.0) * in->gain},
                              {QStringLiteral("audible"), routedLocked(*in) && gainLocked(*in) > 0.0},
                              {QStringLiteral("queued_frames"), static_cast<qint64>(in->fifo.size() / 2)}});
  }
  return QVariantMap{{QStringLiteral("master"), master_},          {QStringLiteral("muted"), muted_},
                     {QStringLiteral("game_silenced"), gameSilenced_}, {QStringLiteral("ducking"), ducking_},
                     {QStringLiteral("mix_all"), mixAll_},         {QStringLiteral("channels"), channels},
                     {QStringLiteral("inputs"), inputs},           {QStringLiteral("plugin_buses"), static_cast<int>(gens_.size())}};
}

}  // namespace uzume::ui
