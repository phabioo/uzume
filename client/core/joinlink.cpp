#include "joinlink.h"

#include <QRegularExpression>
#include <QUrl>

#include "profilestore.h"
#include "securitycode.h"

namespace uzume {

namespace {

constexpr auto kLinkPrefix = "uzume://join/";

struct HostPort {
  QString host;  // as typed, IPv6 literals in brackets
  int port = 0;  // 0 = none
  bool ok = false;
};

// "host", "host:port", "[v6]" or "[v6]:port"; validated with the same rules as the hub edit fields.
HostPort splitHostPort(const QString& text) {
  HostPort r;
  QString host = text;
  QString port;
  if (text.startsWith(QLatin1Char('['))) {
    const qsizetype close = text.indexOf(QLatin1Char(']'));
    if (close < 0) {
      return r;
    }
    host = text.left(close + 1);
    const QString rest = text.mid(close + 1);
    if (!rest.isEmpty()) {
      if (!rest.startsWith(QLatin1Char(':'))) {
        return r;
      }
      port = rest.mid(1);
    }
  } else {
    const qsizetype colon = text.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
      host = text.left(colon);
      port = text.mid(colon + 1);
    }
  }
  if (!text.startsWith(QLatin1Char('[')) && host.contains(QLatin1Char(':'))) {
    return r;
  }
  bool numeric = true;
  int n = 0;
  if (!port.isEmpty()) {
    n = port.toInt(&numeric);
    if (!numeric) {
      return r;
    }
  }
  const QString checkPort = port.isEmpty() ? QString::number(kDefaultHubPort) : port;
  if (!ProfileStore::validateHubAddress(host, checkPort).isEmpty()) {
    return r;
  }
  r.host = host;
  r.port = port.isEmpty() ? 0 : n;
  r.ok = true;
  return r;
}

QString joinHostPort(const HostPort& hp) {
  return hp.host + QLatin1Char(':') + QString::number(hp.port > 0 ? hp.port : kDefaultHubPort);
}

JoinInput broken() {
  JoinInput in;
  in.kind = JoinInput::Kind::BrokenLink;
  in.error = QStringLiteral("broken_link");
  return in;
}

JoinInput parseLink(const QString& text) {
  QString rest = text.mid(QString::fromLatin1(kLinkPrefix).size());
  QString fragment;
  const qsizetype hash = rest.indexOf(QLatin1Char('#'));
  if (hash >= 0) {
    fragment = rest.mid(hash + 1);
    rest = rest.left(hash);
  }
  while (rest.endsWith(QLatin1Char('/'))) {
    rest.chop(1);
  }
  const QStringList parts = rest.split(QLatin1Char('/'));
  if (parts.size() != 2) {
    return broken();
  }
  const HostPort hp = splitHostPort(QUrl::fromPercentEncoding(parts.at(0).toUtf8()).trimmed());
  const QString invite = normalizeInviteCode(QUrl::fromPercentEncoding(parts.at(1).toUtf8()));
  const QString fp = normalizeFingerprint(QUrl::fromPercentEncoding(fragment.toUtf8()));
  if (!hp.ok || invite.isEmpty() || fp.isEmpty()) {
    return broken();
  }
  JoinInput in;
  in.kind = JoinInput::Kind::Link;
  in.address = joinHostPort(hp);
  in.inviteCode = invite;
  in.fingerprint = fp;
  return in;
}

}  // namespace

QString normalizeInviteCode(const QString& text) {
  const QString n = normalizeCode(text);
  if (n.size() != 10 || !n.startsWith(QLatin1String("UZ")) || !isCrockford(n.mid(2))) {
    return {};
  }
  return QStringLiteral("UZ-") + n.mid(2, 4) + QLatin1Char('-') + n.mid(6, 4);
}

QString normalizeFingerprint(const QString& text) {
  QString hex;
  for (const QChar c : text) {
    if (c != QLatin1Char(':') && !c.isSpace()) {
      hex.append(c.toUpper());
    }
  }
  static const QRegularExpression re(QStringLiteral("^[0-9A-F]{64}$"));
  if (!re.match(hex).hasMatch()) {
    return {};
  }
  QString out;
  out.reserve(95);
  for (qsizetype i = 0; i < hex.size(); i += 2) {
    if (i > 0) {
      out.append(QLatin1Char(':'));
    }
    out.append(hex.mid(i, 2));
  }
  return out;
}

JoinInput classifyJoinInput(const QString& input) {
  JoinInput in;
  const QString text = input.trimmed();
  if (text.isEmpty()) {
    in.error = QStringLiteral("empty");
    return in;
  }
  if (text.startsWith(QLatin1String("uzume:"), Qt::CaseInsensitive)) {
    if (text.startsWith(QString::fromLatin1(kLinkPrefix), Qt::CaseInsensitive)) {
      return parseLink(QString::fromLatin1(kLinkPrefix) + text.mid(QString::fromLatin1(kLinkPrefix).size()));
    }
    return broken();
  }
  if (const QString invite = normalizeInviteCode(text); !invite.isEmpty()) {
    in.kind = JoinInput::Kind::InviteCode;
    in.inviteCode = invite;
    return in;
  }
  // "UZ-..." that is not a complete code is most likely a mistyped invite code, not a host name.
  const QString codeLike = normalizeCode(text);
  const bool uzSeparated = text.size() > 2 && text.startsWith(QLatin1String("uz"), Qt::CaseInsensitive) &&
                           (text.at(2) == QLatin1Char('-') || text.at(2).isSpace());
  static const QRegularExpression alnum(QStringLiteral("^[A-Z0-9]+$"));
  const bool codeShaped = codeLike.size() <= 12 && codeLike.startsWith(QLatin1String("UZ")) &&
                          (isCrockford(codeLike.mid(2)) || (codeLike.size() == 10 && alnum.match(codeLike).hasMatch()));
  if (uzSeparated && codeShaped && !text.contains(QLatin1Char('.')) && !text.contains(QLatin1Char(':'))) {
    in.kind = JoinInput::Kind::Invalid;
    in.error = QStringLiteral("invalid_invite_code");
    return in;
  }
  // A server address, with or without scheme.
  QString scheme;
  QString hostPart = text;
  const qsizetype sep = text.indexOf(QLatin1String("://"));
  if (sep >= 0) {
    scheme = text.left(sep).toLower();
    hostPart = text.mid(sep + 3);
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) {
      in.kind = JoinInput::Kind::Invalid;
      in.error = QStringLiteral("invalid_address");
      return in;
    }
  }
  while (hostPart.endsWith(QLatin1Char('/'))) {
    hostPart.chop(1);
  }
  const HostPort hp = splitHostPort(hostPart);
  if (!hp.ok) {
    in.kind = JoinInput::Kind::Invalid;
    in.error = QStringLiteral("invalid_address");
    return in;
  }
  in.kind = JoinInput::Kind::Address;
  in.address = (scheme.isEmpty() ? QString() : scheme + QStringLiteral("://")) + joinHostPort(hp);
  return in;
}

QString makeJoinLink(const QString& address, const QString& inviteCode, const QString& fingerprint) {
  QString text = address.trimmed();
  if (const qsizetype sep = text.indexOf(QLatin1String("://")); sep >= 0) {
    text = text.mid(sep + 3);
  }
  while (text.endsWith(QLatin1Char('/'))) {
    text.chop(1);
  }
  const HostPort hp = splitHostPort(text);
  const QString invite = normalizeInviteCode(inviteCode);
  QString fp = normalizeFingerprint(fingerprint);
  if (!hp.ok || invite.isEmpty() || fp.isEmpty()) {
    return {};
  }
  fp.remove(QLatin1Char(':'));
  return QString::fromLatin1(kLinkPrefix) + joinHostPort(hp) + QLatin1Char('/') + invite + QLatin1Char('#') + fp;
}

namespace {

constexpr auto kRunLinkPrefix = "uzume://run/";

QString normalizeRunId(const QString& text) {
  static const QRegularExpression re(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
  const QString t = text.trimmed().toLower();
  return re.match(t).hasMatch() ? t : QString();
}

}  // namespace

RunLink parseRunLink(const QString& input) {
  RunLink out;
  const QString text = input.trimmed();
  if (!text.startsWith(QString::fromLatin1(kRunLinkPrefix), Qt::CaseInsensitive)) {
    return out;
  }
  QString rest = text.mid(QString::fromLatin1(kRunLinkPrefix).size());
  if (const qsizetype cut = rest.indexOf(QRegularExpression(QStringLiteral("[?#]"))); cut >= 0) {
    rest = rest.left(cut);
  }
  while (rest.endsWith(QLatin1Char('/'))) {
    rest.chop(1);
  }
  const QStringList parts = rest.split(QLatin1Char('/'));
  if (parts.size() != 3 || parts.at(2).toLower() != QLatin1String("recap")) {
    return out;
  }
  const HostPort hp = splitHostPort(QUrl::fromPercentEncoding(parts.at(0).toUtf8()).trimmed());
  const QString id = normalizeRunId(parts.at(1));
  if (!hp.ok || id.isEmpty()) {
    return out;
  }
  out.ok = true;
  out.address = joinHostPort(hp);
  out.runId = id;
  return out;
}

QString makeRunLink(const QString& address, const QString& runId) {
  QString text = address.trimmed();
  if (const qsizetype sep = text.indexOf(QLatin1String("://")); sep >= 0) {
    text = text.mid(sep + 3);
  }
  while (text.endsWith(QLatin1Char('/'))) {
    text.chop(1);
  }
  const HostPort hp = splitHostPort(text);
  const QString id = normalizeRunId(runId);
  if (!hp.ok || id.isEmpty()) {
    return {};
  }
  return QString::fromLatin1(kRunLinkPrefix) + joinHostPort(hp) + QLatin1Char('/') + id + QStringLiteral("/recap");
}

}  // namespace uzume
