#pragma once
// Microphone logic of the voice chat (ADR 0033 D7), pure and header-only so it is tested without audio: the six states of the header
// mic control (board 6w-2), what is sent when, the wire mode of voice_state, and voice activity with a hangover.

#include <QString>
#include <QtGlobal>

namespace uzume::ui {

enum class MicState { Off, PttIdle, Live, MutedByHost, NoDevice, Blocked };

inline QString micStateName(MicState s) {
  switch (s) {
    case MicState::Off: return QStringLiteral("off");
    case MicState::PttIdle: return QStringLiteral("ptt");
    case MicState::Live: return QStringLiteral("live");
    case MicState::MutedByHost: return QStringLiteral("mutedByHost");
    case MicState::NoDevice: return QStringLiteral("noDevice");
    case MicState::Blocked: return QStringLiteral("blocked");
  }
  return QStringLiteral("off");
}

struct MicInputs {
  bool micOn = true;            // the person's mic switch (Shift+M); off = "mic off" for everyone else
  bool vad = false;             // Talk: voice activation (else push-to-talk)
  bool pttHeld = false;         // the push-to-talk key (V, Select + R3) is down
  bool mutedByHost = false;
  bool deviceAvailable = true;  // a microphone (or the synthetic test source) exists
  bool blocked = false;         // the OS refuses the microphone
  bool activityOpen = false;    // voice activity above the threshold (with hangover)
};

// Blocked and no-device win over everything (nothing can be heard anyway), then muted by host (only the host can undo it), then the
// person's switch; with push-to-talk the key decides, with voice activation the mic is live while it is on.
inline MicState micStateOf(const MicInputs& in) {
  if (in.blocked) return MicState::Blocked;
  if (!in.deviceAvailable) return MicState::NoDevice;
  if (in.mutedByHost) return MicState::MutedByHost;
  if (!in.micOn) return MicState::Off;
  if (in.vad) return MicState::Live;
  return in.pttHeld ? MicState::Live : MicState::PttIdle;
}

// Frames go out only while live: with push-to-talk while the key is held, with voice activation while there is activity.
inline bool micTransmits(const MicInputs& in) {
  return micStateOf(in) == MicState::Live && (!in.vad || in.activityOpen);
}

// The `mic` of voice_join / voice_state: what the others see. A mic that cannot work counts as off; muted by host keeps the person's own choice.
inline QString micWireMode(const MicInputs& in) {
  if (in.blocked || !in.deviceAvailable || !in.micOn) return QStringLiteral("off");
  return in.vad ? QStringLiteral("on") : QStringLiteral("ptt");
}

// Voice activity: opens when the level crosses the threshold, stays open while the level is within 3 dB below it and for kHangoverMs
// after the last such frame (no chopped word endings).
class VoiceActivity {
 public:
  static constexpr qint64 kHangoverMs = 300;
  static constexpr double kHysteresisDb = 3.0;
  bool update(double levelDb, double thresholdDb, qint64 nowMs) {
    const double edge = open_ ? thresholdDb - kHysteresisDb : thresholdDb;
    if (levelDb >= edge) {
      open_ = true;
      lastAboveMs_ = nowMs;
    } else if (open_ && nowMs - lastAboveMs_ > kHangoverMs) {
      open_ = false;
    }
    return open_;
  }
  bool open() const { return open_; }
  void reset() {
    open_ = false;
    lastAboveMs_ = 0;
  }

 private:
  bool open_ = false;
  qint64 lastAboveMs_ = 0;
};

}  // namespace uzume::ui
