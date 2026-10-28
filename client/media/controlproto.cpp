#include "controlproto.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace uzume {

namespace {
constexpr int kMaxMessage = 512;

std::optional<QJsonObject> parseObject(const QByteArray& message, const char* type) {
  if (message.isEmpty() || message.size() > kMaxMessage) return std::nullopt;
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(message, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
  const QJsonObject o = doc.object();
  if (o.value(QStringLiteral("t")).toString() != QLatin1String(type)) return std::nullopt;
  return o;
}

std::optional<int> port01(const QJsonValue& v) {
  if (!v.isDouble()) return std::nullopt;
  const double d = v.toDouble();
  if (d != 0.0 && d != 1.0) return std::nullopt;
  return static_cast<int>(d);
}

QByteArray json(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

void put16(QByteArray& b, quint16 v) {
  b.append(static_cast<char>(v & 0xFF));
  b.append(static_cast<char>(v >> 8));
}
quint16 get16(const QByteArray& b, int at) {
  return static_cast<quint16>(static_cast<quint8>(b.at(at)) | (static_cast<quint8>(b.at(at + 1)) << 8));
}
}  // namespace

QByteArray encodeInputFrame(const InputFrame& f) {
  QByteArray b;
  b.reserve(kInputFrameSize);
  b.append(static_cast<char>(kInputFrameVersion));
  b.append(static_cast<char>(f.port));
  put16(b, f.seq);
  put16(b, f.tsMs);
  put16(b, f.buttons);
  for (const qint16 a : f.axes) put16(b, static_cast<quint16>(a));
  put16(b, static_cast<quint16>(f.touchX));
  put16(b, static_cast<quint16>(f.touchY));
  b.append(static_cast<char>(f.touching ? 1 : 0));
  return b;
}

std::optional<InputFrame> decodeInputFrame(const QByteArray& b) {
  if (b.size() != kInputFrameSize || static_cast<quint8>(b.at(0)) != kInputFrameVersion) return std::nullopt;
  InputFrame f;
  f.port = static_cast<quint8>(b.at(1));
  if (f.port > 1) return std::nullopt;
  f.seq = get16(b, 2);
  f.tsMs = get16(b, 4);
  f.buttons = get16(b, 6);
  for (int i = 0; i < 4; ++i) f.axes[i] = static_cast<qint16>(get16(b, 8 + 2 * i));
  f.touchX = static_cast<qint16>(get16(b, 16));
  f.touchY = static_cast<qint16>(get16(b, 18));
  const quint8 flags = static_cast<quint8>(b.at(20));
  if ((flags & ~1u) != 0) return std::nullopt;
  f.touching = (flags & 1u) != 0;
  if (f.touchX < 0 || f.touchY < 0) return std::nullopt;  // kTouchMax is the i16 maximum
  return f;
}

quint32 inputFrameMask(const InputFrame& f) {
  quint32 m = f.buttons;
  constexpr int kHalf = 16384;
  const auto stick = [&m](int x, int y, int shift) {
    if (y < -kHalf) m |= 1u << (shift + 0);
    if (y > kHalf) m |= 1u << (shift + 1);
    if (x < -kHalf) m |= 1u << (shift + 2);
    if (x > kHalf) m |= 1u << (shift + 3);
  };
  stick(f.axes[0], f.axes[1], 20);
  stick(f.axes[2], f.axes[3], 16);
  return m;
}

QByteArray makeControlRequest(int port) { return json({{QStringLiteral("t"), QStringLiteral("control_request")}, {QStringLiteral("port"), port}}); }

std::optional<int> parseControlRequest(const QByteArray& message) {
  const auto o = parseObject(message, "control_request");
  return o ? port01(o->value(QStringLiteral("port"))) : std::nullopt;
}

QByteArray makeControlAnswer(int port, bool granted, const QString& reason) {
  QJsonObject o{{QStringLiteral("t"), QStringLiteral("control")}, {QStringLiteral("port"), port}, {QStringLiteral("granted"), granted}};
  if (!granted && !reason.isEmpty()) o.insert(QStringLiteral("reason"), reason);
  return json(o);
}

std::optional<ControlAnswer> parseControlAnswer(const QByteArray& message) {
  const auto o = parseObject(message, "control");
  if (!o) return std::nullopt;
  const auto port = port01(o->value(QStringLiteral("port")));
  if (!port || !o->value(QStringLiteral("granted")).isBool()) return std::nullopt;
  const QJsonValue r = o->value(QStringLiteral("reason"));
  if (!r.isUndefined() && (!r.isString() || r.toString().size() > 32)) return std::nullopt;
  return ControlAnswer{*port, o->value(QStringLiteral("granted")).toBool(), r.toString()};
}

QByteArray makeControlEnd(int port, const QString& reason) {
  return json({{QStringLiteral("t"), QStringLiteral("control_end")}, {QStringLiteral("port"), port}, {QStringLiteral("reason"), reason}});
}

std::optional<ControlEnd> parseControlEnd(const QByteArray& message) {
  const auto o = parseObject(message, "control_end");
  if (!o) return std::nullopt;
  const auto port = port01(o->value(QStringLiteral("port")));
  const QString reason = o->value(QStringLiteral("reason")).toString();
  if (!port || (reason != QLatin1String("host") && reason != QLatin1String("viewer") && reason != QLatin1String("left"))) return std::nullopt;
  return ControlEnd{*port, reason};
}

QByteArray makeControlRelease() { return json({{QStringLiteral("t"), QStringLiteral("control_release")}}); }
bool parseControlRelease(const QByteArray& message) { return parseObject(message, "control_release").has_value(); }

QByteArray makeControlCaps(int ports) { return json({{QStringLiteral("t"), QStringLiteral("control_caps")}, {QStringLiteral("ports"), ports}}); }

std::optional<int> parseControlCaps(const QByteArray& message) {
  const auto o = parseObject(message, "control_caps");
  if (!o || !o->value(QStringLiteral("ports")).isDouble()) return std::nullopt;
  const double d = o->value(QStringLiteral("ports")).toDouble();
  if (d != std::floor(d) || d < 0 || d > 2) return std::nullopt;
  return static_cast<int>(d);
}

}  // namespace uzume
