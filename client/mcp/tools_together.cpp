// server_sign_in (ADR 0032, live tests): adds a server and signs in with a password through the same onboarding flow as the
// "Join a server" screen (security check, then "sign in on a new device"). The password is neither logged nor returned.
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>

#include "hubconnection.h"
#include "hubhttp.h"
#include "mcp_tools.h"
#include "notificationcenter.h"
#include "onboardingcontroller.h"
#include "playercontroller.h"
#include "sessioncontroller.h"
#include "togethercontroller.h"
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

  // together_state (0.23): what the app shows in the ONLINE list and the bell.
  ToolDef state;
  state.name = QStringLiteral("together_state");
  state.description = QStringLiteral(
      "The online list and the notifications of the current server as the app shows them: roster (people other than me: name, state "
      "online|in_game, game title, system, session_id when watchable, invite_kind session|run|none), notifications (newest first: index, "
      "kind, text, time, action label, read), the unread count, the scope of the running game (run or Session, `available`), the open ready check (title, seconds left, my state, members with state, the last result notice) and the shared pause. Read-only; opening the bell is what marks entries read, this does not.");
  state.inputSchema = schema();
  state.handler = [pc](const QJsonObject&, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    QJsonArray roster;
    for (const QVariant& v : pc->onlinePeople()) {
      const QVariantMap m = v.toMap();
      roster.append(QJsonObject{{QStringLiteral("user_id"), m.value(QStringLiteral("userId")).toString()},
                                {QStringLiteral("name"), m.value(QStringLiteral("name")).toString()},
                                {QStringLiteral("state"), m.value(QStringLiteral("state")).toString()},
                                {QStringLiteral("game_title"), m.value(QStringLiteral("gameTitle")).toString()},
                                {QStringLiteral("system"), m.value(QStringLiteral("system")).toString()},
                                {QStringLiteral("session_id"), m.value(QStringLiteral("sessionId")).toString()},
                                {QStringLiteral("invite_kind"), m.value(QStringLiteral("inviteKind")).toString().isEmpty() ? QStringLiteral("none") : m.value(QStringLiteral("inviteKind")).toString()}});
    }
    QJsonArray notes;
    for (const QVariant& v : pc->notifications()->entries()) {
      const QVariantMap m = v.toMap();
      notes.append(QJsonObject{{QStringLiteral("index"), m.value(QStringLiteral("index")).toInt()},
                               {QStringLiteral("kind"), m.value(QStringLiteral("kind")).toString()},
                               {QStringLiteral("text"), m.value(QStringLiteral("text")).toString()},
                               {QStringLiteral("time"), QDateTime::fromMSecsSinceEpoch(m.value(QStringLiteral("atMs")).toLongLong(), Qt::UTC).toString(Qt::ISODate)},
                               {QStringLiteral("action"), m.value(QStringLiteral("actionLabel")).toString()},
                               {QStringLiteral("read"), m.value(QStringLiteral("read")).toBool()}});
    }
    ui::TogetherController* tg = pc->together();
    QJsonArray members;
    for (const QVariant& v : tg->checkMembers()) {
      const QVariantMap m = v.toMap();
      members.append(QJsonObject{{QStringLiteral("user_id"), m.value(QStringLiteral("userId")).toString()},
                                 {QStringLiteral("name"), m.value(QStringLiteral("name")).toString()},
                                 {QStringLiteral("state"), m.value(QStringLiteral("state")).toString()}});
    }
    reply(ToolResult::json(QJsonObject{
        {QStringLiteral("roster"), roster},
        {QStringLiteral("notifications"), notes},
        {QStringLiteral("unread"), pc->notifications()->unread()},
        {QStringLiteral("scope"), QJsonObject{{QStringLiteral("kind"), tg->scopeKind()}, {QStringLiteral("id"), tg->scopeId()}, {QStringLiteral("available"), tg->available()}}},
        {QStringLiteral("ready_check"), QJsonObject{{QStringLiteral("open"), tg->checkOpen()},
                                                    {QStringLiteral("title"), tg->checkTitle()},
                                                    {QStringLiteral("seconds_left"), tg->secondsLeft()},
                                                    {QStringLiteral("my_state"), tg->myState()},
                                                    {QStringLiteral("members"), members},
                                                    {QStringLiteral("notice"), tg->notice()}}},
        {QStringLiteral("pause"), QJsonObject{{QStringLiteral("paused"), tg->paused()}, {QStringLiteral("text"), tg->pausedText()}, {QStringLiteral("holding_my_game"), tg->holdActive()}}}}));
  };
  registry.add(std::move(state));

  // notifications_act {index}: the click on an entry of the bell (WATCH, OPEN RUN, WHAT'S NEW, OPEN STATUS).
  ToolDef act;
  act.name = QStringLiteral("notifications_act");
  act.description = QStringLiteral("Runs the action of a notification (index from together_state, 0 = newest), as a click in the bell does: marks it read and "
                                   "watches the Session, opens the run, Settings > Updates or the server status.");
  act.inputSchema = schema({{QStringLiteral("index"), typeInt(QStringLiteral("Index of the notification, 0 = newest."))}}, {QStringLiteral("index")});
  act.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    const QJsonValue idx = a.value(QStringLiteral("index"));
    if (!idx.isDouble()) return reply(ToolResult::error(QStringLiteral("index is required (an integer)")));
    if (!pc->notifications()->act(idx.toInt())) return reply(ToolResult::error(QStringLiteral("no_such_notification")));
    reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}}));
  };
  registry.add(std::move(act));

  // session_share / session_stop_sharing (0.23, ADR 0032 D4 amendment): the Share button's path for the running game's Session. The
  // server-level "Share with others" of the local server stays person-only.
  ToolDef share;
  share.name = QStringLiteral("session_share");
  share.description = QStringLiteral(
      "Shares the running game as a Session like the Share button (visibility private | home_server = everyone on the server | invite_only), "
      "or changes the visibility of the Session that is already shared. Answers no_game without a running game. Never touches the sharing "
      "of the local server (\"Share with others\").");
  share.inputSchema = schema({{QStringLiteral("visibility"), typeStr(QStringLiteral("private | home_server | invite_only"))}}, {QStringLiteral("visibility")});
  share.timeoutMs = 10000;
  share.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    const QString given = a.value(QStringLiteral("visibility")).toString();
    const QString vis = given == QLatin1String("home_server") ? QStringLiteral("hub_users") : given;
    if (vis != QLatin1String("private") && vis != QLatin1String("hub_users") && vis != QLatin1String("invite_only"))
      return reply(ToolResult::error(QStringLiteral("visibility must be private, home_server or invite_only")));
    ui::SessionController* sc = pc->sessions();
    if (!sc->hasLocalGame()) return reply(ToolResult::error(QStringLiteral("no_game")));
    sc->setVisibility(vis);
    if (!sc->shared()) sc->shareSession();
    auto elapsed = std::make_shared<int>(0);
    auto poll = std::make_shared<std::function<void()>>();
    QPointer<ui::SessionController> scp(sc);
    auto shared = std::make_shared<ToolReply>(std::move(reply));
    *poll = [scp, elapsed, poll, shared, given]() {
      if (!scp) return (*shared)(ToolResult::error(QStringLiteral("app_closing")));
      if (scp->shared() && !scp->shareBusy())
        return (*shared)(ToolResult::json(QJsonObject{{QStringLiteral("shared"), true}, {QStringLiteral("visibility"), given}}));
      *elapsed += 100;
      if (*elapsed > 8000) return (*shared)(ToolResult::error(scp->message().isEmpty() ? QStringLiteral("share_timeout") : scp->message()));
      QTimer::singleShot(100, qApp, [poll] { (*poll)(); });
    };
    (*poll)();
  };
  registry.add(std::move(share));

  ToolDef stop;
  stop.name = QStringLiteral("session_stop_sharing");
  stop.description = QStringLiteral("Stops sharing the running game's Session (the Stop sharing button). Does nothing when nothing is shared.");
  stop.inputSchema = schema();
  stop.handler = [pc](const QJsonObject&, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    const bool was = pc->sessions()->shared();
    pc->sessions()->stopSharing();
    reply(ToolResult::json(QJsonObject{{QStringLiteral("was_shared"), was}}));
  };
  registry.add(std::move(stop));

  // together_ready_check / together_pause (0.23, ADR 0033 D3): the "Ready check…" and "Pause for everyone" entries of the ⋯ menu.
  ToolDef rc;
  rc.name = QStringLiteral("together_ready_check");
  rc.description = QStringLiteral(
      "Ready check of the scope of the running game (its run, else the shared or watched Session), like the \"Ready check…\" menu entry and the "
      "card buttons. action: start | ready | not_ready | cancel (cancel only for the starter). Answers no_scope, no_open_check or not_starter. "
      "Read the result with together_state.");
  rc.inputSchema = schema({{QStringLiteral("action"), typeStr(QStringLiteral("start | ready | not_ready | cancel"))}}, {QStringLiteral("action")});
  rc.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    ui::TogetherController* tg = pc->together();
    const QString action = a.value(QStringLiteral("action")).toString();
    if (action == QLatin1String("start")) {
      if (!tg->available()) return reply(ToolResult::error(QStringLiteral("no_scope")));
      tg->startCheck();
    } else if (action == QLatin1String("ready") || action == QLatin1String("not_ready")) {
      if (!tg->checkOpen()) return reply(ToolResult::error(QStringLiteral("no_open_check")));
      tg->answer(action == QLatin1String("ready"));
    } else if (action == QLatin1String("cancel")) {
      if (!tg->checkOpen()) return reply(ToolResult::error(QStringLiteral("no_open_check")));
      if (!tg->iStarted()) return reply(ToolResult::error(QStringLiteral("not_starter")));
      tg->cancelCheck();
    } else {
      return reply(ToolResult::error(QStringLiteral("action must be start, ready, not_ready or cancel")));
    }
    reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}}));
  };
  registry.add(std::move(rc));

  ToolDef tp;
  tp.name = QStringLiteral("together_pause");
  tp.description = QStringLiteral(
      "Pauses (paused true) or resumes (false) the game for everyone in the scope of the running game (Shift+Esc). Answers no_scope without a "
      "run or Session. The Hub answers every member; read the result with together_state or game_state.");
  tp.inputSchema = schema({{QStringLiteral("paused"), typeBool(QStringLiteral("true = pause for everyone, false = resume"))}}, {QStringLiteral("paused")});
  tp.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    if (!a.value(QStringLiteral("paused")).isBool()) return reply(ToolResult::error(QStringLiteral("paused is required (true or false)")));
    ui::TogetherController* tg = pc->together();
    if (!tg->available()) return reply(ToolResult::error(QStringLiteral("no_scope")));
    tg->setPaused(a.value(QStringLiteral("paused")).toBool());
    reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}}));
  };
  registry.add(std::move(tp));
}

}  // namespace uzume::mcp
