// Voice chat tools (0.23 package 7, ADR 0033 D7): voice_state, voice_join, voice_leave, voice_mic and voice_inject. The server only exists
// with --mcp (or the setting), so voice_inject, which feeds a synthetic signal instead of the microphone, is for tests only.
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>

#include "mcp_tools.h"
#include "playercontroller.h"
#include "tools_util.h"
#include "voicecontroller.h"

namespace uzume::mcp {

using namespace tools;

QString validateVoiceInjectArgs(const QJsonObject& a) {
  const bool wav = a.contains(QStringLiteral("wav_path"));
  const bool tone = a.contains(QStringLiteral("tone_hz"));
  if (wav == tone) return QStringLiteral("give either wav_path or tone_hz");
  if (wav && (!a.value(QStringLiteral("wav_path")).isString() || a.value(QStringLiteral("wav_path")).toString().isEmpty()))
    return QStringLiteral("wav_path must be a non-empty string");
  if (tone) {
    const double hz = a.value(QStringLiteral("tone_hz")).toDouble(-1);
    if (hz < 50 || hz > 8000) return QStringLiteral("tone_hz must be 50..8000");
    const double s = a.value(QStringLiteral("seconds")).toDouble(-1);
    if (s <= 0 || s > 60) return QStringLiteral("seconds must be 0..60 with tone_hz");
  }
  return {};
}

void installVoiceTools(McpToolRegistry& registry, ui::PlayerController& player) {
  QPointer<ui::PlayerController> pc(&player);

  ToolDef st;
  st.name = QStringLiteral("voice_state");
  st.description = QStringLiteral(
      "The voice chat as the header mic control and the People tab show it: available (a run or Session is the scope and the server offers voice), "
      "joined, room and scope {scope, id}, my mic_state (off | ptt | live | mutedByHost | noDevice | blocked), mic_mode (what others see: off | ptt | on), "
      "talk_mode (ptt | vad), level_db (post noise suppression), threshold_db, self_speaking, muted_by_host, injecting, ducking_active, capture "
      "(running | stopped | no_device | blocked), and members [{user_id, name, device_id, me, mic, muted_by_host, speaking, connection, link}].");
  st.inputSchema = schema();
  st.handler = [pc](const QJsonObject&, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    reply(ToolResult::json(QJsonObject::fromVariantMap(pc->voice()->debugState())));
  };
  registry.add(std::move(st));

  ToolDef jn;
  jn.name = QStringLiteral("voice_join");
  jn.description = QStringLiteral(
      "Joins the voice room of the scope of the running game (its run, else the shared or watched Session), like the Join voice button. Answers "
      "no_scope without a run or Session or when the server has no voice. The room list arrives from the server: poll voice_state.");
  jn.inputSchema = schema();
  jn.handler = [pc](const QJsonObject&, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    ui::VoiceController* v = pc->voice();
    if (v->wantJoined()) return reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("already"), true}}));
    if (!v->available()) return reply(ToolResult::error(QStringLiteral("no_scope")));
    v->join();
    reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), v->wantJoined()}}));
  };
  registry.add(std::move(jn));

  ToolDef lv;
  lv.name = QStringLiteral("voice_leave");
  lv.description = QStringLiteral("Leaves the voice room (Leave voice). No automatic join into the same scope follows until the scope changes.");
  lv.inputSchema = schema();
  lv.handler = [pc](const QJsonObject&, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    pc->voice()->leave();
    reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}}));
  };
  registry.add(std::move(lv));

  ToolDef mc;
  mc.name = QStringLiteral("voice_mic");
  mc.description = QStringLiteral(
      "Sets my microphone: mode on (voice activation: sends while someone speaks above the threshold), ptt (push-to-talk) or off (Shift+M). Also the "
      "Talk setting. With ptt a synthetic voice_inject counts as the key held. Answers the new mic_state and mic_mode.");
  mc.inputSchema = schema({{QStringLiteral("mode"), typeStr(QStringLiteral("on | off | ptt"))}}, {QStringLiteral("mode")});
  mc.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    const QString mode = a.value(QStringLiteral("mode")).toString();
    if (mode != QLatin1String("on") && mode != QLatin1String("off") && mode != QLatin1String("ptt")) return reply(ToolResult::error(QStringLiteral("mode must be on, off or ptt")));
    ui::VoiceController* v = pc->voice();
    v->setMicMode(mode);
    reply(ToolResult::json(QJsonObject{{QStringLiteral("mic_state"), v->micStateText()}, {QStringLiteral("mic_mode"), v->micMode()}}));
  };
  registry.add(std::move(mc));

  ToolDef inj;
  inj.name = QStringLiteral("voice_inject");
  inj.description = QStringLiteral(
      "Tests without a microphone: feeds a synthetic signal into the voice chat instead of the microphone, as 20 ms frames in real time. Either "
      "tone_hz (50..8000) with seconds (up to 60), or wav_path (16-bit PCM WAV, up to 60 s). It skips noise suppression, counts as the key held in "
      "push-to-talk and still obeys mic off and muted by host. Needs the app started with --mcp.");
  inj.inputSchema = schema({{QStringLiteral("tone_hz"), typeInt(QStringLiteral("Sine frequency in Hz"))},
                            {QStringLiteral("seconds"), typeAny(QStringLiteral("Length of the tone in seconds (number)"))},
                            {QStringLiteral("wav_path"), typeStr(QStringLiteral("Absolute path of a 16-bit PCM WAV file"))}});
  inj.handler = [pc](const QJsonObject& a, ToolReply reply) {
    if (!pc) return reply(ToolResult::error(QStringLiteral("app_closing")));
    const QString bad = validateVoiceInjectArgs(a);
    if (!bad.isEmpty()) return reply(ToolResult::error(bad));
    ui::VoiceController* v = pc->voice();
    const bool ok = a.contains(QStringLiteral("wav_path")) ? v->injectWav(a.value(QStringLiteral("wav_path")).toString())
                                                            : v->injectTone(a.value(QStringLiteral("tone_hz")).toDouble(), a.value(QStringLiteral("seconds")).toDouble());
    if (!ok) return reply(ToolResult::error(QStringLiteral("unreadable_wav")));
    reply(ToolResult::json(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("mic_state"), v->micStateText()}, {QStringLiteral("joined"), v->joined()}}));
  };
  registry.add(std::move(inj));
}

}  // namespace uzume::mcp
