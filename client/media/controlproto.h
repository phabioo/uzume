#pragma once

// Controller hand-over (ADR 0033 D8) wire formats, documented in protocol/datachannel.md. Pure data and parsers, no Qt event
// loop. Every parser returns nullopt for anything that is not a well-formed message of its type, so readers ignore garbage.
//   viewer -> host   {"t":"control_request","port":0|1}
//   host -> viewer   {"t":"control","port":n,"granted":bool,"reason":"denied|busy|timeout|unsupported|holding"?}
//   host -> viewer   {"t":"control_end","port":n,"reason":"host|viewer|left"}
//   viewer -> host   {"t":"control_release"}
//   host -> viewer   {"t":"control_caps","ports":n}  (what the host's system offers; older hosts never send it)
// and the binary input frames of the negotiated DataChannel `fb-input` (id 1, unordered, maxRetransmits 0).

#include <QByteArray>
#include <QString>
#include <optional>

namespace uzume {

constexpr int kInputChannelId = 1;
constexpr int kInputFrameVersion = 1;
constexpr int kInputFrameSize = 21;
constexpr int kInputKeepAliveMs = 100;  // the viewer repeats the current state at least this often
constexpr int kInputStaleMs = 250;      // the host drops frames that arrive later than this behind the best path seen
constexpr int kInputNeutralMs = 500;    // no frame for this long: the port goes neutral
constexpr int kControlAskTimeoutMs = 20000;  // an unanswered request is denied
constexpr qint16 kTouchMax = 32767;     // touch x/y are 0..kTouchMax over the whole video frame

// Frame layout (little endian): version u8, port u8, seq u16, ts u16 (sender ms, wraps), buttons u16 (libretro joypad bits
// 0..15), axes 4 x i16 (left x, left y, right x, right y), touch x i16, touch y i16, flags u8 (bit 0: touch pressed).
struct InputFrame {
  quint8 port = 0;
  quint16 seq = 0;
  quint16 tsMs = 0;
  quint16 buttons = 0;
  qint16 axes[4] = {0, 0, 0, 0};
  qint16 touchX = 0;
  qint16 touchY = 0;
  bool touching = false;
  bool operator==(const InputFrame& o) const {
    return port == o.port && seq == o.seq && tsMs == o.tsMs && buttons == o.buttons && touchX == o.touchX && touchY == o.touchY &&
           touching == o.touching && axes[0] == o.axes[0] && axes[1] == o.axes[1] && axes[2] == o.axes[2] && axes[3] == o.axes[3];
  }
  // Same content except the sequence and the timestamp (a change worth sending).
  bool sameInput(const InputFrame& o) const {
    return buttons == o.buttons && touchX == o.touchX && touchY == o.touchY && touching == o.touching && axes[0] == o.axes[0] &&
           axes[1] == o.axes[1] && axes[2] == o.axes[2] && axes[3] == o.axes[3];
  }
};
QByteArray encodeInputFrame(const InputFrame& f);
// nullopt: wrong size, wrong version, port above 1, touch outside 0..kTouchMax, unknown flag bits.
std::optional<InputFrame> decodeInputFrame(const QByteArray& bytes);
// The libretro mask the host injects: the 16 button bits plus the sticks as the digital direction bits the "3ds" profile
// reads (left stick bits 20..23 up/down/left/right, right stick bits 16..19) beyond half deflection.
quint32 inputFrameMask(const InputFrame& f);

QByteArray makeControlRequest(int port);
std::optional<int> parseControlRequest(const QByteArray& message);

struct ControlAnswer {
  int port = 0;
  bool granted = false;
  QString reason;  // only on a denial
};
QByteArray makeControlAnswer(int port, bool granted, const QString& reason = {});
std::optional<ControlAnswer> parseControlAnswer(const QByteArray& message);

struct ControlEnd {
  int port = 0;
  QString reason;  // host | viewer | left
};
QByteArray makeControlEnd(int port, const QString& reason);
std::optional<ControlEnd> parseControlEnd(const QByteArray& message);

QByteArray makeControlRelease();
bool parseControlRelease(const QByteArray& message);

QByteArray makeControlCaps(int ports);
std::optional<int> parseControlCaps(const QByteArray& message);

}  // namespace uzume
