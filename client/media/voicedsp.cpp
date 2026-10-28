#include "voicedsp.h"

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include <algorithm>
#include <cmath>

namespace uzume {

namespace {
constexpr size_t kMaxFarEndFrames = 25;  // 500 ms: further behind the playback is dropped
}

VoiceDsp::VoiceDsp() = default;
VoiceDsp::~VoiceDsp() { close(); }

bool VoiceDsp::open(const Options& o) {
  close();
  opt_ = o;
  pp_ = speex_preprocess_state_init(kVoiceFrame, kVoiceRate);
  if (!pp_) return false;
  auto* pp = pp_;
  int on = o.denoise ? 1 : 0;
  speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_DENOISE, &on);
  int db = o.noiseSuppressDb;
  speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &db);
  on = o.agc ? 1 : 0;
  speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_AGC, &on);
  float level = 20000.f;  // target level for the AGC
  speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_AGC_LEVEL, &level);
  if (o.echoCancel) {
    const int taps = std::max(kVoiceFrame, kVoiceRate / 1000 * o.echoFilterMs);
    echo_ = speex_echo_state_init(kVoiceFrame, taps);
    if (echo_) {
      int rate = kVoiceRate;
      speex_echo_ctl(echo_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
      speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_ECHO_STATE, echo_);
    }
  }
  scratch_.assign(kVoiceFrame, 0);
  far_.clear();
  return true;
}

void VoiceDsp::close() {
  if (pp_) {
    speex_preprocess_state_destroy(pp_);
    pp_ = nullptr;
  }
  if (echo_) {
    speex_echo_state_destroy(echo_);
    echo_ = nullptr;
  }
  far_.clear();
}

void VoiceDsp::pushFarEnd(const int16_t* pcm, int samples) {
  if (!echo_ || samples <= 0) return;
  far_.insert(far_.end(), pcm, pcm + samples);
  const size_t cap = kMaxFarEndFrames * kVoiceFrame;
  if (far_.size() > cap) far_.erase(far_.begin(), far_.begin() + (far_.size() - cap));
}

void VoiceDsp::process(const int16_t* mic, int16_t* out) {
  if (!pp_) {
    std::copy(mic, mic + kVoiceFrame, out);
    return;
  }
  auto* pp = pp_;
  if (echo_) {
    auto* ec = echo_;
    // Playback and capture are paired frame by frame (speex_echo_playback / speex_echo_capture): without far end the reference is silence.
    if (far_.size() >= size_t(kVoiceFrame)) {
      speex_echo_playback(ec, far_.data());
      far_.erase(far_.begin(), far_.begin() + kVoiceFrame);
    } else {
      std::fill(scratch_.begin(), scratch_.end(), int16_t(0));
      speex_echo_playback(ec, scratch_.data());
    }
    speex_echo_capture(ec, mic, out);
  } else {
    std::copy(mic, mic + kVoiceFrame, out);
  }
  speex_preprocess_run(pp, out);
}

float VoiceDsp::levelDb(const int16_t* pcm, int samples) {
  if (samples <= 0) return -120.f;
  double sum = 0;
  for (int i = 0; i < samples; ++i) sum += double(pcm[i]) * pcm[i];
  const double rms = std::sqrt(sum / samples) / 32768.0;
  return rms < 1e-6 ? -120.f : float(20.0 * std::log10(rms));
}

}  // namespace uzume
