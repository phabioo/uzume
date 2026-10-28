#include "qualitytier.h"

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

// A JSON number that is a whole value inside [lo, hi].
std::optional<qint64> wholeNumber(const QJsonValue& v, qint64 lo, qint64 hi) {
  if (!v.isDouble()) return std::nullopt;
  const double d = v.toDouble();
  if (!std::isfinite(d) || d != std::floor(d) || d < static_cast<double>(lo) || d > static_cast<double>(hi)) return std::nullopt;
  return static_cast<qint64>(d);
}
}  // namespace

QString qualityToken(Quality q) {
  switch (q) {
    case Quality::High: return QStringLiteral("high");
    case Quality::Medium: return QStringLiteral("medium");
    case Quality::Low: return QStringLiteral("low");
    case Quality::Auto: break;
  }
  return QStringLiteral("auto");
}

std::optional<Quality> parseQualityToken(const QString& s) {
  if (s == QLatin1String("auto")) return Quality::Auto;
  if (s == QLatin1String("high")) return Quality::High;
  if (s == QLatin1String("medium")) return Quality::Medium;
  if (s == QLatin1String("low")) return Quality::Low;
  return std::nullopt;
}

QString qualityLabel(Quality q) {
  switch (q) {
    case Quality::High: return QStringLiteral("High");
    case Quality::Medium: return QStringLiteral("Medium");
    case Quality::Low: return QStringLiteral("Low");
    case Quality::Auto: break;
  }
  return QStringLiteral("Auto");
}

int qualityKbps(Quality q) {
  switch (q) {
    case Quality::High: return kQualityHighKbps;
    case Quality::Medium: return kQualityMediumKbps;
    case Quality::Low: return kQualityLowKbps;
    case Quality::Auto: break;
  }
  return 0;
}

Quality qualityForKbps(double kbps) {
  if (kbps >= 3000.0) return Quality::High;
  if (kbps >= 1000.0) return Quality::Medium;
  return Quality::Low;
}

QByteArray makeQualityPick(Quality pick) {
  return QJsonDocument(QJsonObject{{QStringLiteral("t"), QStringLiteral("quality")}, {QStringLiteral("pick"), qualityToken(pick)}})
      .toJson(QJsonDocument::Compact);
}

std::optional<Quality> parseQualityPick(const QByteArray& message) {
  const auto o = parseObject(message, "quality");
  if (!o) return std::nullopt;
  const QJsonValue v = o->value(QStringLiteral("pick"));
  if (!v.isString()) return std::nullopt;
  return parseQualityToken(v.toString());
}

static QByteArray makePingLike(const char* type, int n, qint64 tsMs) {
  return QJsonDocument(QJsonObject{{QStringLiteral("t"), QLatin1String(type)}, {QStringLiteral("n"), n}, {QStringLiteral("ts"), static_cast<double>(tsMs)}})
      .toJson(QJsonDocument::Compact);
}
QByteArray makePing(int n, qint64 tsMs) { return makePingLike("ping", n, tsMs); }
QByteArray makePong(int n, qint64 tsMs) { return makePingLike("pong", n, tsMs); }

std::optional<PingMessage> parsePingPong(const QByteArray& message) {
  for (const char* type : {"ping", "pong"}) {
    const auto o = parseObject(message, type);
    if (!o) continue;
    const auto n = wholeNumber(o->value(QStringLiteral("n")), 0, 2'000'000'000LL);
    const auto ts = wholeNumber(o->value(QStringLiteral("ts")), 0, 1LL << 52);
    if (!n || !ts) return std::nullopt;
    PingMessage m;
    m.pong = QLatin1String(type) == QLatin1String("pong");
    m.n = static_cast<int>(*n);
    m.ts = *ts;
    return m;
  }
  return std::nullopt;
}

std::optional<double> rttFromPong(qint64 nowMs, qint64 tsMs) {
  const qint64 d = nowMs - tsMs;
  if (d < 0 || d > 60'000) return std::nullopt;
  return static_cast<double>(d);
}

QByteArray makeTier(Quality tier, int kbps) {
  return QJsonDocument(QJsonObject{{QStringLiteral("t"), QStringLiteral("tier")}, {QStringLiteral("tier"), qualityToken(tier)}, {QStringLiteral("kbps"), kbps}})
      .toJson(QJsonDocument::Compact);
}

std::optional<TierMessage> parseTier(const QByteArray& message) {
  const auto o = parseObject(message, "tier");
  if (!o) return std::nullopt;
  const QJsonValue t = o->value(QStringLiteral("tier"));
  if (!t.isString()) return std::nullopt;
  const auto q = parseQualityToken(t.toString());
  const auto kbps = wholeNumber(o->value(QStringLiteral("kbps")), 0, 100'000);
  if (!q || !kbps) return std::nullopt;
  TierMessage m;
  m.tier = *q;
  m.kbps = static_cast<int>(*kbps);
  return m;
}

}  // namespace uzume
