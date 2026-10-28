// Controller hand-over tools (0.23 package 8, ADR 0033 D8): control_request (viewer), control_answer (host), control_release (viewer),
// control_state (both) and remote_input (viewer, tests). They do what the People tab buttons, the confirm card and the keyboard do.
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include "controlcontroller.h"
#include "envvar.h"
#include "inputmap.h"
#include "mcp_tools.h"
#include "playercontroller.h"
#include "sessioncontroller.h"
#include "tools_util.h"

namespace uzume::mcp {

using namespace tools;

namespace {
constexpr int kMaxRemoteFrames = 600;

// Same switch as tools_ui.cpp: confirm: true counts only when the app was started with UZUME_MCP_CONFIRM=1.
bool confirmed(const QJsonObject& a) {
  return a.value(QStringLiteral("confirm")).toBool(false) && uzume::env::get("UZUME_MCP_CONFIRM") == QLatin1String("1");
}

QString watchedSession(ui::SessionController* sc, const QJsonObject& a) {
  QString sid = a.value(QStringLiteral("session_id")).toString();
  if (sid.isEmpty() && !sc->shownSessionIds().isEmpty()) sid = sc->shownSessionIds().first();
  return sid;
}
}  // namespace

void installControlTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);

  ToolDef rq;
  rq.name = QStringLiteral("control_request");
  rq.description = QStringLiteral(
      "Viewer side: asks the host of a watched Session for a controller, like the People tab buttons: port 0 = \"Take over P1\", port 1 = \"Play as P2\" "
      "(only when the host's system has two ports). Answers at once with state asking; the host decides (control_state shows playing, or idle with a reason: "
      "denied | busy | timeout | unsupported). Errors: not_watching, host_offers_no_controller, port_not_available, already_asking_or_playing.");
  rq.inputSchema = schema({{QStringLiteral("port"), typeInt(QStringLiteral("0 (P1) or 1 (P2)"))},
                           {QStringLiteral("session_id"), typeStr(QStringLiteral("Watched Session (default: the first one)"))}},
                          {QStringLiteral("port")});
  rq.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    const QJsonValue p = a.value(QStringLiteral("port"));
    if (!p.isDouble() || (p.toInt(-1) != 0 && p.toInt(-1) != 1)) return reply(ToolResult::error(QStringLiteral("port must be 0 or 1")));
    const QString sid = watchedSession(pc->sessions(), a);
    QString err;
    if (!pc->control()->request(sid, p.toInt(), &err)) return reply(ToolResult::error(err));
    reply(ToolResult::json(QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("port"), p.toInt()}, {QStringLiteral("state"), QStringLiteral("asking")}}));
  };
  registry.add(std::move(rq));

  ToolDef an;
  an.name = QStringLiteral("control_answer");
  an.description = QStringLiteral(
      "Host side: answers the pending controller request of a viewer like the Allow / Deny card on the stage (keys 1 / 2). viewer is the viewer_id from "
      "control_state or session_viewers (default: the oldest pending request). Allow needs confirm: true and an app started with UZUME_MCP_CONFIRM=1 (else needs_confirmation_in_app); Deny is free. Errors: no_pending_request.");
  an.inputSchema = schema({{QStringLiteral("allow"), typeBool(QStringLiteral("true = Allow, false = Deny"))},
                           {QStringLiteral("viewer"), typeStr(QStringLiteral("viewer_id (default: the oldest pending request)"))},
                           {QStringLiteral("confirm"), typeBool(QStringLiteral("Allow only: test runs started with UZUME_MCP_CONFIRM=1"))}},
                          {QStringLiteral("allow")});
  an.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    if (!a.value(QStringLiteral("allow")).isBool()) return reply(ToolResult::error(QStringLiteral("allow must be true or false")));
    // Allow hands the game's controller to another person: like a destructive click it needs the test-run switch (UZUME_MCP_CONFIRM=1)
    // and confirm: true. Deny stays free.
    if (a.value(QStringLiteral("allow")).toBool() && !confirmed(a)) {
      ToolResult r = ToolResult::error(QStringLiteral("needs_confirmation_in_app"));
      r.addText(QStringLiteral("Allowing hands the controller to another person. Ask the person to press Allow in the app themselves. Test runs only: start "
                               "the app with UZUME_MCP_CONFIRM=1 and pass confirm: true."));
      return reply(r);
    }
    QString viewer = a.value(QStringLiteral("viewer")).toString();
    if (viewer.isEmpty()) {
      const auto pending = pc->sessions()->host()->controlPending();
      if (!pending.isEmpty()) viewer = pending.first().viewerId;
    }
    if (viewer.isEmpty() || !pc->control()->answerViewer(viewer, a.value(QStringLiteral("allow")).toBool()))
      return reply(ToolResult::error(QStringLiteral("no_pending_request")));
    reply(ToolResult::json(QJsonObject{{QStringLiteral("viewer"), viewer}, {QStringLiteral("allowed"), a.value(QStringLiteral("allow")).toBool()}}));
  };
  registry.add(std::move(an));

  ToolDef rl;
  rl.name = QStringLiteral("control_release");
  rl.description = QStringLiteral(
      "Hands the controller back. Viewer side (default): \"Stop playing\" / cancels an ask in a watched Session. Host side with take_back = true: \"Take back\", "
      "ends every hold (port optional: 0 or 1 for one port). Errors: nothing_to_release.");
  rl.inputSchema = schema({{QStringLiteral("session_id"), typeStr(QStringLiteral("Watched Session (default: the first one)"))},
                           {QStringLiteral("take_back"), typeBool(QStringLiteral("Host: take the controller back from the viewer(s)"))},
                           {QStringLiteral("port"), typeInt(QStringLiteral("Host take_back: only this port"))}});
  rl.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    ui::SessionController* sc = pc->sessions();
    if (a.value(QStringLiteral("take_back")).toBool(false)) {
      SessionHost* host = sc->host();
      bool any = false;
      for (int p = 0; p < 2; ++p) {
        if (a.contains(QStringLiteral("port")) && a.value(QStringLiteral("port")).toInt(-1) != p) continue;
        any = host->takeBack(p) || any;
      }
      if (!any) return reply(ToolResult::error(QStringLiteral("nothing_to_release")));
      return reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("side"), QStringLiteral("host")}}));
    }
    const QString sid = watchedSession(sc, a);
    if (!pc->control()->release(sid)) return reply(ToolResult::error(QStringLiteral("nothing_to_release")));
    reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("side"), QStringLiteral("viewer")}, {QStringLiteral("session_id"), sid}}));
  };
  registry.add(std::move(rl));

  ToolDef st;
  st.name = QStringLiteral("control_state");
  st.description = QStringLiteral(
      "Controller hand-over state. Host: ports (what the running system offers, 0 = no game or not shared), holders [{port, viewer_id, name, buttons, neutral}], "
      "pending requests [{viewer_id, name, port}], and game {p1_remote, p1_mask, p2_remote, p2_mask} (what is injected into the core). Viewer: watching "
      "[{session_id, state: idle | asking | playing, port, host_ports, reason, frames_sent}]. Read-only.");
  st.inputSchema = schema();
  st.handler = [pc](const QJsonObject&, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    reply(ToolResult::json(QJsonObject::fromVariantMap(pc->control()->state())));
  };
  registry.add(std::move(st));

  ToolDef ri;
  ri.name = QStringLiteral("remote_input");
  ri.description = QStringLiteral(
      "Viewer side, for tests: presses buttons for N frames on the fb-input channel of the Session you play in (granted by the host), then releases them. "
      "Button names are the Uzume inputs (a, b, x, y, l, r, start, select, up, down, left, right). Errors: not_playing, unknown_button.");
  ri.inputSchema = schema({{QStringLiteral("buttons"), typeArray(typeStr(QString()), QStringLiteral("Button names."))},
                           {QStringLiteral("frames"), typeInt(QStringLiteral("Frames to hold, 1 to 600 (default 6)."))},
                           {QStringLiteral("session_id"), typeStr(QStringLiteral("Session you play in (default: that one)"))}});
  ri.timeoutMs = 30000;
  ri.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    ui::ControlController* cc = pc->control();
    QString sid = a.value(QStringLiteral("session_id")).toString();
    if (sid.isEmpty()) sid = cc->playingSession();
    if (sid.isEmpty() || !cc->controlling(sid)) return reply(ToolResult::error(QStringLiteral("not_playing")));
    const int frames = a.value(QStringLiteral("frames")).isDouble() ? a.value(QStringLiteral("frames")).toInt(6) : 6;
    if (frames < 1 || frames > kMaxRemoteFrames) return reply(ToolResult::error(QStringLiteral("frames must be 1 to 600")));
    quint32 mask = 0;
    for (const QJsonValue& b : a.value(QStringLiteral("buttons")).toArray()) {
      const QString name = b.toString().toLower();
      const quint32 bit = input::inputBit(name);
      const quint32 lib = bit == 0 ? 0 : input::libretroMaskFor(QStringLiteral("nds"), bit);
      if (lib == 0) return reply(ToolResult::error(QStringLiteral("unknown_button: ") + name.left(24)));
      mask |= lib;
    }
    cc->remoteInput(sid, mask);
    QTimer::singleShot(frames * 1000 / 60, cc, [pc, sid, reply, frames]() {
      if (pc) pc->control()->remoteInput(sid, 0);
      reply(ToolResult::json(QJsonObject{{QStringLiteral("held_frames"), frames}, {QStringLiteral("session_id"), sid}}));
    });
  };
  registry.add(std::move(ri));
}

}  // namespace uzume::mcp
