#pragma once

// Quality per viewer (ADR 0033 D5) on the fb-diag DataChannel; documented in protocol/datachannel.md. Pure data and
// parsers, no Qt event loop. Every parser returns nullopt for anything that is not a well-formed message of its type
// (other types, bad JSON, wrong field types): readers ignore those, so unknown messages never break a connection.
//   viewer -> host   {"t":"quality","pick":"auto|high|medium|low"}
//   both directions  {"t":"ping","n":<int>,"ts":<ms>} / {"t":"pong","n":<int>,"ts":<ms>}  (pong echoes n and ts)
//   host -> viewer   {"t":"tier","tier":"high|medium|low|auto","kbps":N}

#include <QByteArray>
#include <QString>
#include <optional>

namespace uzume {

enum class Quality { Auto = 0, High, Medium, Low };

constexpr int kQualityHighKbps = 4000;
constexpr int kQualityMediumKbps = 1500;
constexpr int kQualityLowKbps = 600;

QString qualityToken(Quality q);                       // "auto" | "high" | "medium" | "low"
std::optional<Quality> parseQualityToken(const QString& s);  // case-sensitive lower-case tokens only
QString qualityLabel(Quality q);                       // "Auto" | "High" | "Medium" | "Low" (UI)
int qualityKbps(Quality q);                            // fixed tiers; 0 for Auto
// The tier a bitrate is closest to (>= 3000 High, >= 1000 Medium, else Low): how an Auto viewer's "gets High" is named.
Quality qualityForKbps(double kbps);

QByteArray makeQualityPick(Quality pick);
std::optional<Quality> parseQualityPick(const QByteArray& message);

struct PingMessage {
  bool pong = false;
  int n = 0;
  qint64 ts = 0;  // the sender's own monotonic milliseconds; only the sender interprets it
};
QByteArray makePing(int n, qint64 tsMs);
QByteArray makePong(int n, qint64 tsMs);
std::optional<PingMessage> parsePingPong(const QByteArray& message);
// RTT of a pong that arrives at nowMs for a ping stamped tsMs by the same clock; nullopt for a stamp from the future or
// older than a minute.
std::optional<double> rttFromPong(qint64 nowMs, qint64 tsMs);

struct TierMessage {
  Quality tier = Quality::Auto;
  int kbps = 0;
};
QByteArray makeTier(Quality tier, int kbps);
std::optional<TierMessage> parseTier(const QByteArray& message);

}  // namespace uzume
