#pragma once
// Voice chat DSP (ADR 0033 D7): speexdsp preprocessor (noise suppression, AGC) and echo canceller on 48 kHz mono 20 ms
// frames (960 samples). The far-end reference for the echo canceller is the mixer output (what the speakers play).

#include <cstdint>
#include <vector>

struct SpeexPreprocessState_;
struct SpeexEchoState_;

namespace uzume {

inline constexpr int kVoiceRate = 48000;
inline constexpr int kVoiceFrame = 960;  // 20 ms mono

class VoiceDsp {
 public:
  struct Options {
    bool denoise = true;
    bool agc = true;
    bool echoCancel = true;
    int echoFilterMs = 100;  // length of the echo path model
    int noiseSuppressDb = -25;
  };
  VoiceDsp();
  ~VoiceDsp();
  VoiceDsp(const VoiceDsp&) = delete;
  VoiceDsp& operator=(const VoiceDsp&) = delete;

  bool open(const Options& o);
  bool open() { return open(Options{}); }
  void close();
  bool isOpen() const { return pp_ != nullptr; }

  // What the speakers play, mono 48 kHz (any chunk size; consumed in frames of kVoiceFrame). Only used while echoCancel is on.
  void pushFarEnd(const int16_t* pcm, int samples);
  // One microphone frame (kVoiceFrame samples) -> out (kVoiceFrame samples). Voice activity is decided by the caller from the
  // processed level (speexdsp's own VAD is a placeholder hack in 1.2.1).
  void process(const int16_t* mic, int16_t* out);
  // Frames of far end waiting (tests).
  int farEndFrames() const { return int(far_.size() / kVoiceFrame); }

  static float levelDb(const int16_t* pcm, int samples);  // RMS in dBFS, -120 for silence

 private:
  SpeexPreprocessState_* pp_ = nullptr;
  SpeexEchoState_* echo_ = nullptr;
  Options opt_;
  std::vector<int16_t> far_;
  std::vector<int16_t> scratch_;
};

}  // namespace uzume
