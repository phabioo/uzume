#include "runtypes.h"

#include <QUuid>

namespace uzume {

namespace {

QString str(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString(); }

bool validUuid(const QString& s) {
  const QUuid u = QUuid::fromString(s);
  return !u.isNull() && s.size() == 36;
}

bool validStatus(const QString& s) {
  return s == QLatin1String("starting") || s == QLatin1String("active") || s == QLatin1String("paused") ||
         s == QLatin1String("finished") || s == QLatin1String("abandoned");
}

}  // namespace

const RunParticipant* RunInfo::participant(const QString& userId) const {
  for (const RunParticipant& p : participants)
    if (p.userId == userId) return &p;
  return nullptr;
}

RunParticipant* RunInfo::participant(const QString& userId) {
  for (RunParticipant& p : participants)
    if (p.userId == userId) return &p;
  return nullptr;
}

bool RunInfo::hasPlugin(const QString& pluginId) const {
  for (const RunPluginRef& p : plugins)
    if (p.pluginId == pluginId) return true;
  return false;
}

std::optional<RunInfo> parseRun(const QJsonObject& o) {
  RunInfo r;
  r.runId = str(o, "run_id").toLower();
  r.name = str(o, "name");
  r.status = str(o, "status");
  if (!validUuid(r.runId) || r.name.isEmpty() || !validStatus(r.status)) return std::nullopt;
  r.createdBy = str(o, "created_by");
  r.createdAt = str(o, "created_at");
  r.updatedAt = str(o, "updated_at");
  r.statusBy = str(o, "status_by");
  r.saveSlot = str(o, "save_slot");
  if (r.saveSlot.isEmpty()) r.saveSlot = runSaveSlot(r.runId);
  r.settings = o.value(QLatin1String("settings")).toObject();
  r.randomizer = o.value(QLatin1String("randomizer")).toObject();
  r.lastSeq = static_cast<qint64>(o.value(QLatin1String("last_seq")).toDouble(0));
  r.playSeconds = qMax<qint64>(0, static_cast<qint64>(o.value(QLatin1String("play_seconds")).toDouble(0)));
  for (const QJsonValue& v : o.value(QLatin1String("plugins")).toArray()) {
    const QJsonObject p = v.toObject();
    RunPluginRef ref{str(p, "plugin_id"), str(p, "version"), str(p, "sha256").toLower()};
    if (!ref.pluginId.isEmpty() && !ref.version.isEmpty()) r.plugins.append(ref);
  }
  for (const QJsonValue& v : o.value(QLatin1String("participants")).toArray()) {
    const QJsonObject p = v.toObject();
    RunParticipant part;
    part.userId = str(p, "user_id");
    if (part.userId.isEmpty()) continue;
    part.userName = str(p, "user_name");
    part.status = str(p, "status");
    part.gameId = str(p, "game_id");
    part.playSeconds = qMax<qint64>(0, static_cast<qint64>(p.value(QLatin1String("play_seconds")).toDouble(0)));
    part.lastPlayedAt = str(p, "last_played_at");
    part.variantKind = str(p, "variant_kind");
    part.variantState = str(p, "variant_state");
    part.color = p.value(QLatin1String("color")).isDouble() ? p.value(QLatin1String("color")).toInt(-1) : -1;
    const QJsonObject sts = p.value(QLatin1String("states")).toObject();
    for (auto it = sts.begin(); it != sts.end(); ++it) {
      const QJsonObject so = it.value().toObject();
      RunPluginState ps;
      ps.state = so.value(QLatin1String("state")).toObject();
      ps.rev = static_cast<qint64>(so.value(QLatin1String("state_rev")).toDouble(0));
      ps.updatedAt = str(so, "updated_at");
      part.states.insert(it.key(), ps);
    }
    r.participants.append(part);
  }
  return r;
}

QJsonObject participantToJson(const RunParticipant& p) {
  QJsonObject sts;
  for (auto it = p.states.begin(); it != p.states.end(); ++it)
    sts.insert(it.key(), QJsonObject{{QStringLiteral("state"), it->state},
                                     {QStringLiteral("state_rev"), static_cast<double>(it->rev)},
                                     {QStringLiteral("updated_at"), it->updatedAt.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(it->updatedAt)}});
  return {{QStringLiteral("user_id"), p.userId},
          {QStringLiteral("user_name"), p.userName},
          {QStringLiteral("status"), p.status},
          {QStringLiteral("game_id"), p.gameId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(p.gameId)},
          {QStringLiteral("color"), p.color < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(p.color)},
          {QStringLiteral("play_seconds"), static_cast<double>(p.playSeconds)},
          {QStringLiteral("last_played_at"), p.lastPlayedAt.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(p.lastPlayedAt)},
          {QStringLiteral("variant_kind"), p.variantKind},
          {QStringLiteral("variant_state"), p.variantState},
          {QStringLiteral("states"), sts}};
}

QJsonObject runToJson(const RunInfo& r) {
  QJsonArray plugins;
  for (const RunPluginRef& p : r.plugins)
    plugins.append(QJsonObject{{QStringLiteral("plugin_id"), p.pluginId}, {QStringLiteral("version"), p.version}, {QStringLiteral("sha256"), p.sha256}});
  QJsonArray parts;
  for (const RunParticipant& p : r.participants) parts.append(participantToJson(p));
  return {{QStringLiteral("run_id"), r.runId},
          {QStringLiteral("name"), r.name},
          {QStringLiteral("status"), r.status},
          {QStringLiteral("created_by"), r.createdBy},
          {QStringLiteral("created_at"), r.createdAt},
          {QStringLiteral("updated_at"), r.updatedAt},
          {QStringLiteral("status_by"), r.statusBy},
          {QStringLiteral("save_slot"), r.saveSlot.isEmpty() ? runSaveSlot(r.runId) : r.saveSlot},
          {QStringLiteral("settings"), r.settings},
          {QStringLiteral("randomizer"), r.randomizer},
          {QStringLiteral("plugins"), plugins},
          {QStringLiteral("participants"), parts},
          {QStringLiteral("play_seconds"), static_cast<double>(r.playSeconds)},
          {QStringLiteral("last_seq"), static_cast<double>(r.lastSeq)}};
}

std::optional<RunEvent> parseRunEvent(const QJsonObject& o) {
  RunEvent e;
  e.seq = static_cast<qint64>(o.value(QLatin1String("seq")).toDouble(0));
  e.clientId = str(o, "client_id").toLower();
  e.type = str(o, "type");
  e.source = str(o, "source");
  if (e.seq < 1 || e.type.isEmpty() || e.clientId.isEmpty()) return std::nullopt;
  e.createdAt = str(o, "created_at");
  e.userId = str(o, "user_id");
  e.userName = str(o, "user_name");
  e.deviceId = str(o, "device_id");
  e.pluginId = str(o, "plugin_id");
  e.text = str(o, "text");
  e.payload = o.value(QLatin1String("payload")).toObject();
  return e;
}

QJsonObject runEventToJson(const RunEvent& e) {
  const auto nullable = [](const QString& s) { return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s); };
  return {{QStringLiteral("seq"), static_cast<double>(e.seq)},
          {QStringLiteral("created_at"), e.createdAt},
          {QStringLiteral("user_id"), nullable(e.userId)},
          {QStringLiteral("user_name"), e.userName},
          {QStringLiteral("device_id"), nullable(e.deviceId)},
          {QStringLiteral("plugin_id"), nullable(e.pluginId)},
          {QStringLiteral("source"), e.source},
          {QStringLiteral("type"), e.type},
          {QStringLiteral("text"), e.text},
          {QStringLiteral("payload"), e.payload},
          {QStringLiteral("client_id"), e.clientId}};
}

QString runSaveSlot(const QString& runId) {
  QString hex = runId;
  hex.remove(QLatin1Char('-'));
  return QStringLiteral("run-") + hex.left(8).toLower();
}

}  // namespace uzume
