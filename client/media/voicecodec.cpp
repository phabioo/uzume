#include "voicecodec.h"

#include <opus/opus.h>

#include "voicedsp.h"

namespace uzume {

VoiceEncoder::VoiceEncoder() = default;
VoiceEncoder::~VoiceEncoder() { close(); }

bool VoiceEncoder::open(int bitrate, int lossPercent) {
  close();
  int err = 0;
  enc_ = opus_encoder_create(kVoiceRate, 1, OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK || !enc_) {
    enc_ = nullptr;
    return false;
  }
  opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate));
  opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
  opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(lossPercent));
  return true;
}

void VoiceEncoder::close() {
  if (enc_) {
    opus_encoder_destroy(enc_);
    enc_ = nullptr;
  }
}

std::vector<uint8_t> VoiceEncoder::encode(const int16_t* pcm) {
  if (!enc_) return {};
  uint8_t buf[400];
  const int n = opus_encode(enc_, pcm, kVoiceFrame, buf, sizeof(buf));
  if (n <= 0) return {};
  return {buf, buf + n};
}

VoiceDecoder::VoiceDecoder() = default;
VoiceDecoder::~VoiceDecoder() { close(); }

bool VoiceDecoder::open() {
  close();
  int err = 0;
  dec_ = opus_decoder_create(kVoiceRate, 1, &err);
  if (err != OPUS_OK) dec_ = nullptr;
  return dec_ != nullptr;
}

void VoiceDecoder::close() {
  if (dec_) {
    opus_decoder_destroy(dec_);
    dec_ = nullptr;
  }
}

bool VoiceDecoder::decode(const uint8_t* data, size_t size, int16_t* out, bool fec) {
  if (!dec_) return false;
  const int n = opus_decode(dec_, data, data ? static_cast<opus_int32>(size) : 0, out, kVoiceFrame, fec ? 1 : 0);
  return n == kVoiceFrame;
}

}  // namespace uzume
