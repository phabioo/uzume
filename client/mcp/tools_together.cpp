// server_sign_in (ADR 0032, live tests): adds a server and signs in with a password through the same onboarding flow as the
// "Join a server" screen (security check, then "sign in on a new device"). The password is neither logged nor returned.
#include <QCoreApplication>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <memory>

#include "hubconnection.h"
#include "hubhttp.h"
#include "mcp_tools.h"
#include "onboardingcontroller.h"
#include "playercontroller.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;

QString validateServerSignInArgs(const QJsonObject& a) {
  for (const char* key : {"url", "username", "password"}) {
    const QJsonValue v = a.value(QLatin1String(key));
    if (!v.isString() || v.toString().trimmed().isEmpty()) return QStringLiteral("%1 is required (a non-empty string)").arg(QLatin1String(key));
  }
  if (a.contains(QStringLiteral("trust_fingerprint")) && !a.value(QStringLiteral("trust_fingerprint")).isString())
    return QStringLiteral("trust_fingerprint must be a string");
  return {};
}

bool serverFingerprintTrusted(const QString& observed, const QString& trusted) {
  return !trusted.trimmed().isEmpty() && HubHttp::fingerprintsEqual(observed, trusted);
}

namespace {

constexpr int kStepMs = 100;
constexpr int kSignInTimeoutMs = 30000;

struct Flow {
  QPointer<ui::PlayerController> pc;
  QString username, password, trust;
  ToolReply reply;
  bool signInSent = false;
  bool done = false;
  int elapsedMs = 0;
};

void finish(const std::shared_ptr<Flow>& f, ToolResult r) {
  if (f->done) return;
  f->done = true;
  f->password.clear();
  f->reply(std::move(r));
}

void step(const std::shared_ptr<Flow>& f) {
  if (f->done) return;
  if (!f->pc) return finish(f, ToolResult::error(QStringLiteral("app_closing")));
  using State = HubConnection::State;
  HubConnection* conn = f->pc->connection();
  auto* onboarding = f->pc->onboarding();
  const State s = conn->state();
  const auto fail = [&](const QString& code, const QString& extra = QString()) {
    QJsonObject o{{QStringLiteral("error"), code}};
    if (!extra.isEmpty()) o.insert(QStringLiteral("detail"), extra);
    onboarding->cancel();
    ToolResult r = ToolResult::json(o);
    r.isError = true;
    finish(f, std::move(r));
  };
  switch (s) {
    case State::Connected: {
      const QString user = conn->userDisplayName();
      if (!f->signInSent && user.compare(f->username, Qt::CaseInsensitive) != 0)
        return finish(f, ToolResult::error(QStringLiteral("already signed in as '%1' on this server; use a fresh --data-dir for another user").arg(user)));
      return finish(f, ToolResult::json(QJsonObject{{QStringLiteral("ok"), true},
                                                    {QStringLiteral("server"), onboarding->serverName()},
                                                    {QStringLiteral("address"), onboarding->serverAddress()},
                                                    {QStringLiteral("user"), user},
                                                    {QStringLiteral("role"), conn->userRole()}}));
    }
    case State::NeedsTrustConfirmation: {
      const QString observed = conn->observedFingerprint();
      if (!serverFingerprintTrusted(observed, f->trust)) {
        onboarding->securityDiffers();  // nothing is saved
        onboarding->dismissMismatch();
        ToolResult r = ToolResult::json(QJsonObject{{QStringLiteral("error"), QStringLiteral("untrusted_certificate")},
                                                    {QStringLiteral("fingerprint"), observed},
                                                    {QStringLiteral("hint"), QStringLiteral("Call again with trust_fingerprint set to this value if it is the expected server.")}});
        r.isError = true;
        return finish(f, std::move(r));
      }
      onboarding->securityMatches();
      break;
    }
    case State::NeedsPairing:
    case State::Denied:
    case State::Expired:
      if (!f->signInSent) {
        f->signInSent = true;
        onboarding->signIn(f->username, f->password);
        f->password.clear();
      } else if (!onboarding->busy() && !onboarding->errorCode().isEmpty()) {
        return fail(onboarding->errorCode());
      }
      break;
    case State::Unreachable: return fail(QStringLiteral("unreachable"));
    case State::Incompatible: return fail(QStringLiteral("incompatible"), onboarding->phase());
    case State::CertificateChanged: return fail(QStringLiteral("certificate_changed"));
    case State::UserDisabled: return fail(QStringLiteral("user_disabled"));
    case State::AwaitingApproval: return fail(QStringLiteral("awaiting_approval"));
    case State::Disconnected:
      if (!onboarding->errorCode().isEmpty()) return fail(onboarding->errorCode());
      break;
    default: break;
  }
  f->elapsedMs += kStepMs;
  if (f->elapsedMs > kSignInTimeoutMs) return fail(QStringLiteral("timeout"), onboarding->phase());
  QTimer::singleShot(kStepMs, qApp, [f] { step(f); });
}

}  // namespace

void installTogetherTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);
  ToolDef t;
  t.name = QStringLiteral("server_sign_in");
  t.description = QStringLiteral(
      "Add a server and sign in with user name and password, like \"Join a server\" > \"Sign in\" (live tests). A self-signed certificate "
      "is accepted only when trust_fingerprint (SHA-256 as the app shows it, colons optional) equals the server's; otherwise the call "
      "fails and returns the fingerprint. The password is never logged or returned.");
  t.inputSchema = schema({{QStringLiteral("url"), typeStr(QStringLiteral("Server address, e.g. https://127.0.0.1:8443."))},
                          {QStringLiteral("username"), typeStr(QStringLiteral("User name."))},
                          {QStringLiteral("password"), typeStr(QStringLiteral("Password (not logged, not returned)."))},
                          {QStringLiteral("trust_fingerprint"), typeStr(QStringLiteral("Certificate fingerprint to trust on first contact."))}},
                         {QStringLiteral("url"), QStringLiteral("username"), QStringLiteral("password")});
  t.timeoutMs = kSignInTimeoutMs + 5000;
  t.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    const QString bad = validateServerSignInArgs(a);
    if (!bad.isEmpty()) return reply(ToolResult::error(bad));
    auto f = std::make_shared<Flow>();
    f->pc = pc;
    f->username = a.value(QStringLiteral("username")).toString().trimmed();
    f->password = a.value(QStringLiteral("password")).toString();
    f->trust = a.value(QStringLiteral("trust_fingerprint")).toString();
    f->reply = std::move(reply);
    auto* onboarding = pc->onboarding();
    onboarding->beginJoin();
    onboarding->submitJoinInput(a.value(QStringLiteral("url")).toString().trimmed());
    if (!onboarding->errorCode().isEmpty() && pc->connection()->state() == HubConnection::State::Disconnected) {
      const QString code = onboarding->errorCode();
      onboarding->cancel();
      return finish(f, ToolResult::error(code));
    }
    QTimer::singleShot(kStepMs, qApp, [f] { step(f); });
  };
  registry.add(std::move(t));
}

}  // namespace uzume::mcp
