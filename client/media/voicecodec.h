#pragma once
// Voice chat Opus (ADR 0033 D7): mono, 48 kHz, 20 ms, VOIP application, 24 kbit/s, in-band FEC.

#include <cstdint>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace uzume {

class VoiceEncoder {
 public:
  VoiceEncoder();
  ~VoiceEncoder();
  VoiceEncoder(const VoiceEncoder&) = delete;
  VoiceEncoder& operator=(const VoiceEncoder&) = delete;
  bool open(int bitrate = 24000, int expectedLossPercent = 10);
  void close();
  bool isOpen() const { return enc_ != nullptr; }
  // 960 samples -> one packet; empty on error.
  std::vector<uint8_t> encode(const int16_t* pcm);

 private:
  OpusEncoder* enc_ = nullptr;
};

class VoiceDecoder {
 public:
  VoiceDecoder();
  ~VoiceDecoder();
  VoiceDecoder(const VoiceDecoder&) = delete;
  VoiceDecoder& operator=(const VoiceDecoder&) = delete;
  bool open();
  void close();
  bool isOpen() const { return dec_ != nullptr; }
  // Decodes a packet to 960 samples (false on error). data == nullptr: packet loss concealment; `fec` with the NEXT packet:
  // recovers the lost frame before it from the in-band redundancy.
  bool decode(const uint8_t* data, size_t size, int16_t* out, bool fec = false);

 private:
  OpusDecoder* dec_ = nullptr;
};

}  // namespace uzume
