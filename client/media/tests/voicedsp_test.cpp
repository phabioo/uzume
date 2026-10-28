// Voice DSP spike (ADR 0033 D7): speexdsp denoise / echo cancellation on synthetic signals, Opus voice codec, CPU cost per frame.
#include <QtTest>
#include <chrono>
#include <cmath>
#include <vector>

#include "processguard.h"
#include "voicecodec.h"
#include "voicedsp.h"

using namespace uzume;

namespace {
struct Lcg {
  uint32_t s = 12345;
  double next() {  // -1..1
    s = s * 1664525u + 1013904223u;
    return (double(s >> 8) / double(1 << 24)) * 2.0 - 1.0;
  }
};
double energy(const std::vector<int16_t>& v, size_t from, size_t to) {
  double e = 0;
  for (size_t i = from; i < to; ++i) e += double(v[i]) * v[i];
  return e / double(to - from);
}
// "Speech-like" far end: noise with a slow amplitude envelope.
std::vector<int16_t> farEnd(int frames, uint32_t seed) {
  Lcg r;
  r.s = seed;
  std::vector<int16_t> v(size_t(frames) * kVoiceFrame);
  double lp = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    lp = 0.7 * lp + 0.3 * r.next();
    const double env = 0.5 + 0.5 * std::sin(2 * M_PI * 3.0 * double(i) / kVoiceRate);
    v[i] = int16_t(lp * env * 12000);
  }
  return v;
}
}  // namespace

class VoiceDspTest : public QObject {
  Q_OBJECT
 private slots:
  void denoiseReducesStationaryNoise() {
    VoiceDsp dsp;
    VoiceDsp::Options o;
    o.agc = false;
    o.echoCancel = false;
    QVERIFY(dsp.open(o));
    Lcg r;
    const int frames = 300;  // 6 s
    std::vector<int16_t> in(size_t(frames) * kVoiceFrame), out(in.size());
    for (auto& s : in) s = int16_t(r.next() * 800);
    for (int f = 0; f < frames; ++f) dsp.process(&in[size_t(f) * kVoiceFrame], &out[size_t(f) * kVoiceFrame]);
    const size_t a = size_t(frames - 100) * kVoiceFrame;
    const double db = 10 * std::log10(energy(in, a, in.size()) / (std::max)(1.0, energy(out, a, out.size())));
    qInfo("denoise: stationary noise reduced by %.1f dB", db);
    QVERIFY2(db > 10.0, qPrintable(QString::number(db)));
  }
  void echoCancellerReducesDelayedCopy() {
    VoiceDsp dsp;
    VoiceDsp::Options o;
    o.denoise = false;
    o.agc = false;
    QVERIFY(dsp.open(o));
    const int frames = 500;  // 10 s
    const auto farSig = farEnd(frames, 99);
    const int delay = 1200;  // 25 ms room + device path
    std::vector<int16_t> mic(farSig.size()), out(farSig.size());
    for (size_t i = 0; i < mic.size(); ++i) mic[i] = i >= size_t(delay) ? int16_t(farSig[i - delay] * 0.6) : 0;
    for (int f = 0; f < frames; ++f) {
      dsp.pushFarEnd(&farSig[size_t(f) * kVoiceFrame], kVoiceFrame);
      dsp.process(&mic[size_t(f) * kVoiceFrame], &out[size_t(f) * kVoiceFrame]);
    }
    const size_t a = size_t(frames - 100) * kVoiceFrame;
    const double db = 10 * std::log10(energy(mic, a, mic.size()) / (std::max)(1.0, energy(out, a, out.size())));
    qInfo("echo canceller: delayed copy reduced by %.1f dB (ERLE, last 2 s of 10 s)", db);
    QVERIFY2(db > 15.0, qPrintable(QString::number(db)));
  }
  void echoCancellerKeepsNearEndSpeech() {
    VoiceDsp dsp;
    VoiceDsp::Options o;
    o.denoise = false;
    o.agc = false;
    QVERIFY(dsp.open(o));
    const int frames = 300;
    std::vector<int16_t> mic(size_t(frames) * kVoiceFrame), out(mic.size());
    for (size_t i = 0; i < mic.size(); ++i) mic[i] = int16_t(8000 * std::sin(2 * M_PI * 440.0 * double(i) / kVoiceRate));
    for (int f = 0; f < frames; ++f) dsp.process(&mic[size_t(f) * kVoiceFrame], &out[size_t(f) * kVoiceFrame]);  // no far end
    const size_t a = size_t(frames - 50) * kVoiceFrame;
    QVERIFY(energy(out, a, out.size()) > 0.5 * energy(mic, a, mic.size()));
  }
  void opusRoundTripAndFec() {
    VoiceEncoder enc;
    VoiceDecoder dec;
    QVERIFY(enc.open());
    QVERIFY(dec.open());
    std::vector<int16_t> pcm(kVoiceFrame), out(kVoiceFrame);
    size_t bytes = 0;
    int ok = 0;
    for (int f = 0; f < 50; ++f) {
      for (int i = 0; i < kVoiceFrame; ++i) pcm[size_t(i)] = int16_t(8000 * std::sin(2 * M_PI * 440.0 * double(f * kVoiceFrame + i) / kVoiceRate));
      const auto p = enc.encode(pcm.data());
      QVERIFY(!p.empty());
      bytes += p.size();
      if (f == 20) {  // lost frame: conceal
        QVERIFY(dec.decode(nullptr, 0, out.data()));
        continue;
      }
      if (f == 21) {  // recover frame 20 from the redundancy of 21, then decode 21
        QVERIFY(dec.decode(p.data(), p.size(), out.data(), true));
      }
      if (dec.decode(p.data(), p.size(), out.data())) ++ok;
    }
    QVERIFY(ok >= 49);
    qInfo("opus voice: %.1f kbit/s average on a 440 Hz tone", double(bytes) * 8 / 50 / 0.02 / 1000);
  }
  void cpuPerFrame() {
    VoiceDsp dsp;
    VoiceEncoder enc;
    VoiceDecoder dec;
    QVERIFY(dsp.open());
    QVERIFY(enc.open());
    QVERIFY(dec.open());
    const int frames = 500;
    const auto farSig = farEnd(frames, 7);
    std::vector<int16_t> mic(farSig.size()), out(kVoiceFrame), dec_out(kVoiceFrame);
    for (size_t i = 0; i < mic.size(); ++i) mic[i] = int16_t(farSig[i] / 2);
    using clk = std::chrono::steady_clock;
    double dspUs = 0, encUs = 0, decUs = 0;
    std::vector<uint8_t> pkt;
    for (int f = 0; f < frames; ++f) {
      dsp.pushFarEnd(&farSig[size_t(f) * kVoiceFrame], kVoiceFrame);
      auto t0 = clk::now();
      dsp.process(&mic[size_t(f) * kVoiceFrame], out.data());
      auto t1 = clk::now();
      pkt = enc.encode(out.data());
      auto t2 = clk::now();
      dec.decode(pkt.data(), pkt.size(), dec_out.data());
      auto t3 = clk::now();
      dspUs += std::chrono::duration<double, std::micro>(t1 - t0).count();
      encUs += std::chrono::duration<double, std::micro>(t2 - t1).count();
      decUs += std::chrono::duration<double, std::micro>(t3 - t2).count();
    }
    qInfo("cpu per 20 ms frame: speexdsp preprocess+AEC %.0f us, opus encode %.0f us, opus decode %.0f us (per remote talker)", dspUs / frames,
          encUs / frames, decUs / frames);
    QVERIFY((dspUs + encUs) / frames < 3000.0);  // far below the 20 ms budget even on a debug build
  }
};

FB_TEST_MAIN(VoiceDspTest)
#include "voicedsp_test.moc"
