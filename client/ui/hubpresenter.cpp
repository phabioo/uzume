#include "hubpresenter.h"
#include "timeformat.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <algorithm>

#include "core_options.h"
#include "firmware_materializer.h"
#include "libretro_backend.h"
#include "semver.h"
#include "version.h"

namespace uzume::ui {

QString trimmedScheme(const QString& address) {
  QString a = address;
  a.remove(QRegularExpression(QStringLiteral("^https?://")));
  return a;
}

namespace {

// A sentence for the common TLS failures instead of the raw OpenSSL text (R-P20). `detail` keeps the raw text for "Details".
QString friendlyConnectError(const QString& raw, QString* detail) {
  detail->clear();
  const QString lower = raw.toLower();
  QString friendly;
  if (lower.contains(QLatin1String("wrong version number")) || lower.contains(QLatin1String("http request"))) {
    friendly = QCoreApplication::translate("uzume::ui::HubPresenter",
                                           "The server does not answer with HTTPS on this port. Check the address and the port.");
  } else if (lower.contains(QLatin1String("ssl handshake failed")) || lower.contains(QLatin1String("ssl routines"))) {
    friendly = QCoreApplication::translate("uzume::ui::HubPresenter",
                                           "A secure connection to the server could not be set up. Check the address and the port.");
  }
  if (friendly.isEmpty()) return raw;
  *detail = raw;
  return friendly;
}

}  // namespace

HubPresenter::HubPresenter(const Deps& d, QObject* parent)
    : QObject(parent), conn_(d.conn), profiles_(d.profiles), lastError_(d.lastError), inviteBusy_(d.inviteBusy) {}

QVariantMap HubPresenter::hubCard(const HubProfile& p) const {
  using S = HubConnection::State;
  QVariantMap m;
  m.insert(QStringLiteral("hubId"), p.hubId);
  m.insert(QStringLiteral("name"), p.name.isEmpty() ? trimmedScheme(p.address) : p.name);
  m.insert(QStringLiteral("saved"), true);
  {
    QString host;
    int port = 0;
    ProfileStore::splitAddress(p.address, &host, &port);
    m.insert(QStringLiteral("host"), host);
    m.insert(QStringLiteral("port"), port > 0 ? QString::number(port) : QStringLiteral("8443"));
  }
  m.insert(QStringLiteral("isLast"), p.hubId == profiles_->lastHubId());
  const bool lost = p.credentialRef.isEmpty() && p.lastConnected.isValid();   // connected before, pairing gone
  m.insert(QStringLiteral("pairingLost"), lost);
  const QString lastWhen = p.lastConnected.isValid() ? TimeFormat::when(p.lastConnected) : QString();
  m.insert(QStringLiteral("lastConnectedText"), lastWhen);
  m.insert(QStringLiteral("lastConnectedFull"), TimeFormat::full(p.lastConnected));
  const bool connectedNow = conn_->state() == S::Connected && (conn_->address() == p.address || (conn_->profile() && conn_->profile()->hubId == p.hubId));
  const QString last = !p.lastConnected.isValid() ? tr("never connected")
                       : connectedNow ? tr("since %1").arg(lastWhen) : tr("last %1").arg(lastWhen);
  m.insert(QStringLiteral("detail"), trimmedScheme(p.address) + QStringLiteral(" · ") + last);
  m.insert(QStringLiteral("addressText"), trimmedScheme(p.address));

  const bool current = conn_->state() != S::Disconnected &&
                       (conn_->address() == p.address || (conn_->profile() && conn_->profile()->hubId == p.hubId));
  QString status = QStringLiteral("idle");
  QString text = tr("Ready");
  QString tone = QStringLiteral("neutral");
  QString message;
  QString messageDetail;  // raw technical text behind a "Details" link
  if (current) {
    switch (conn_->state()) {
      case S::Identifying:
      case S::Authenticating:
        status = QStringLiteral("connecting");
        text = tr("Connecting…");
        break;
      case S::CertificateChanged:
        status = QStringLiteral("certChanged");
        text = tr("Certificate changed");
        tone = QStringLiteral("error");
        message = tr("The server's certificate changed. The connection is blocked and nothing was sent to the server. "
                     "Compare the new fingerprint with the one on the server's web Settings page. Trust it only if they match.");
        m.insert(QStringLiteral("expectedFingerprint"), formatFingerprint(conn_->expectedFingerprint().isEmpty() ? p.pinnedFingerprint : conn_->expectedFingerprint()));
        m.insert(QStringLiteral("observedFingerprint"), formatFingerprint(conn_->observedFingerprint()));
        m.insert(QStringLiteral("observedFingerprintRaw"), conn_->observedFingerprint());
        break;
      case S::Incompatible: {
        status = QStringLiteral("incompatible");
        const bool hubOld = conn_->incompatibleReason() == HubConnection::IncompatibleReason::HubTooOld;
        text = hubOld ? tr("Server too old") : tr("App too old");
        tone = QStringLiteral("warn");
        const HubInfo& hi = conn_->hubInfo();
        message = hubOld ? tr("Server speaks protocol v%1, app requires at least v%2").arg(hi.protocolVersion).arg(kMinProtocolVersion)
                         : tr("Server requires at least protocol v%1, app speaks v%2").arg(hi.minProtocolVersion).arg(kProtocolVersion);
        break;
      }
      case S::UserDisabled:
        status = QStringLiteral("userDisabled");
        text = tr("User disabled");
        tone = QStringLiteral("error");
        message = tr("This user is disabled on the server. Ask the server admin to enable it again. "
                     "Uzume does not retry on its own; use Retry once it is enabled.");
        break;
      case S::Unreachable:
        status = QStringLiteral("unreachable");
        text = tr("Not reachable");
        tone = QStringLiteral("error");
        message = friendlyConnectError(conn_->errorMessage(), &messageDetail);
        break;
      default:
        break;
    }
  }
  m.insert(QStringLiteral("status"), status);
  m.insert(QStringLiteral("statusText"), text);
  m.insert(QStringLiteral("tone"), tone);
  m.insert(QStringLiteral("message"), message);
  m.insert(QStringLiteral("messageDetail"), messageDetail);
  m.insert(QStringLiteral("current"), current);
  m.insert(QStringLiteral("connected"), current && conn_->state() == S::Connected);
  return m;
}

QVariantList HubPresenter::hubs() const {
  using S = HubConnection::State;
  QVariantList list;
  bool matched = false;
  for (const HubProfile& p : profiles_->profiles()) {
    QVariantMap card = hubCard(p);
    matched = matched || card.value(QStringLiteral("current")).toBool();
    list.append(card);
  }
  const S s = conn_->state();
  if (!matched && (s == S::Unreachable || s == S::Incompatible || s == S::CertificateChanged || s == S::UserDisabled)) {
    // Attempt with an address not saved yet: show as a card with the result.
    HubProfile p;
    p.address = conn_->address();
    p.name = conn_->hubInfo().name;
    QVariantMap card = hubCard(p);
    card.insert(QStringLiteral("saved"), false);
    card.insert(QStringLiteral("hubId"), QString());
    card.insert(QStringLiteral("detail"), trimmedScheme(p.address));
    list.append(card);
  }
  return list;
}

QString HubPresenter::formatFingerprint(const QString& fp) {
  QString hex;
  for (const QChar c : fp) {
    if (c.isDigit() || (c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f'))) {
      hex.append(c.toUpper());
    }
  }
  QStringList pairs;
  for (qsizetype i = 0; i + 1 < hex.size(); i += 2) {
    pairs.append(hex.mid(i, 2));
  }
  if (pairs.isEmpty()) {
    return fp;
  }
  const qsizetype half = (pairs.size() + 1) / 2;
  const QString first = pairs.mid(0, half).join(QLatin1Char(':'));
  const QString second = pairs.mid(half).join(QLatin1Char(':'));
  return second.isEmpty() ? first : first + QLatin1Char('\n') + second;
}

QVariantMap HubPresenter::pairing() const {
  using S = HubConnection::State;
  const S s = conn_->state();
  const HubInfo& hi = conn_->hubInfo();
  const HandshakeInfo h = HandshakeInfo::detect();
  QVariantMap m;
  QString phase = QStringLiteral("identifying");
  switch (s) {
    case S::NeedsTrustConfirmation: phase = QStringLiteral("trust"); break;
    case S::NeedsPairing: phase = QStringLiteral("needsPairing"); break;
    case S::AwaitingApproval: phase = QStringLiteral("awaiting"); break;
    case S::Denied: phase = QStringLiteral("denied"); break;
    case S::Expired: phase = QStringLiteral("expired"); break;
    case S::Authenticating: phase = QStringLiteral("authenticating"); break;
    default: break;
  }
  const bool tls = conn_->address().startsWith(QLatin1String("https://"));
  QString fp = conn_->observedFingerprint();
  if (fp.isEmpty() && conn_->profile()) {
    fp = conn_->profile()->pinnedFingerprint;
  }
  m.insert(QStringLiteral("phase"), phase);
  m.insert(QStringLiteral("address"), trimmedScheme(conn_->address()));
  m.insert(QStringLiteral("hubName"), hi.name);
  m.insert(QStringLiteral("hubVersion"), hi.hubVersion);
  m.insert(QStringLiteral("protocol"), hi.protocolVersion);
  m.insert(QStringLiteral("hubKnown"), !hi.hubId.isEmpty());
  // "Pair again": this Hub was connected before (a saved profile with a last connection) and its pairing is gone.
  m.insert(QStringLiteral("pairAgain"), conn_->profile().has_value() && conn_->profile()->lastConnected.isValid() &&
                                            (phase == QLatin1String("needsPairing") || phase == QLatin1String("awaiting") ||
                                             phase == QLatin1String("denied") || phase == QLatin1String("expired")));
  m.insert(QStringLiteral("tls"), tls);
  m.insert(QStringLiteral("fingerprint"), formatFingerprint(fp));
  m.insert(QStringLiteral("deviceName"), profiles_->deviceName());
  m.insert(QStringLiteral("platform"), platformLabel(h.platform, h.arch));
  m.insert(QStringLiteral("playerVersion"), h.playerVersion);
  m.insert(QStringLiteral("error"), lastError_);
  m.insert(QStringLiteral("inviteBusy"), inviteBusy_);
  return m;
}

QString HubPresenter::platformLabel(const QString& platform, const QString& arch) {
  QString p = platform;
  if (p == QLatin1String("windows")) p = QStringLiteral("Windows");
  else if (p == QLatin1String("macos")) p = QStringLiteral("macOS");
  else if (p == QLatin1String("linux")) p = QStringLiteral("Linux");
  QString a = arch;
  if (a == QLatin1String("x86_64")) a = QStringLiteral("x86-64");
  else if (a == QLatin1String("arm64") || a == QLatin1String("aarch64")) a = QStringLiteral("ARM64");
  return a.isEmpty() ? p : p + QLatin1Char(' ') + a;
}

}  // namespace uzume::ui
