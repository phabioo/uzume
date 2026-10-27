#include "runrules.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QStringList>

namespace uzume::ui::runrules {

namespace {
QString tr(const char* s) { return QCoreApplication::translate("RunRules", s); }
QString onOff(bool on) { return on ? tr("On") : tr("Off"); }
QString lowerFirst(QString s) {
  if (!s.isEmpty()) s[0] = s.at(0).toLower();
  return s;
}
}  // namespace

QVariantMap toMap(const RunRules& r) {
  return {{QStringLiteral("states"), r.states}, {QStringLiteral("rewind"), r.rewind}, {QStringLiteral("cheats"), r.cheats}, {QStringLiteral("memoryWrite"), r.memoryWrite}};
}

RunRules fromMap(const QVariantMap& m) {
  RunRules r;
  for (auto it = m.begin(); it != m.end(); ++it) setRule(r, it.key(), it.value());
  return r;
}

bool setRule(RunRules& r, const QString& key, const QVariant& value) {
  if (key == QLatin1String("states")) {
    r.states = value.toBool();
    return true;
  }
  if (key == QLatin1String("rewind")) {
    r.rewind = value.toBool();
    return true;
  }
  const bool cheats = key == QLatin1String("cheats");
  if (!cheats && key != QLatin1String("memoryWrite") && key != QLatin1String("memory_write")) return false;
  QString level;
  if (value.typeId() == QMetaType::Bool) level = value.toBool() ? QStringLiteral("logged") : QStringLiteral("off");
  else level = value.toString();
  if (level != QLatin1String("off") && level != QLatin1String("logged")) return false;
  (cheats ? r.cheats : r.memoryWrite) = level;
  return true;
}

QString levelText(const QString& level) { return level == QLatin1String("logged") ? tr("Allowed, logged") : tr("Off"); }

QString summaryLine(const RunRules& r) {
  return tr("Save states %1 · Rewind %2 · Cheats %3 · Memory changes %4")
      .arg(lowerFirst(onOff(r.states)), lowerFirst(onOff(r.rewind)), lowerFirst(levelText(r.cheats)), lowerFirst(levelText(r.memoryWrite)));
}

QString changeText(const RunRules& a, const RunRules& b) {
  QStringList parts;
  if (a.states != b.states) parts << tr("Save states: %1 → %2").arg(onOff(a.states), onOff(b.states));
  if (a.rewind != b.rewind) parts << tr("Rewind: %1 → %2").arg(onOff(a.rewind), onOff(b.rewind));
  if (a.cheats != b.cheats) parts << tr("Cheats: %1 → %2").arg(levelText(a.cheats), levelText(b.cheats));
  if (a.memoryWrite != b.memoryWrite) parts << tr("Memory changes: %1 → %2").arg(levelText(a.memoryWrite), levelText(b.memoryWrite));
  return parts.join(QStringLiteral("; "));
}

QString changeEventText(const QString& who, const QJsonObject& payload) {
  const QString diff = changeText(parseRunRules(payload.value(QStringLiteral("before")).toObject()), parseRunRules(payload.value(QStringLiteral("after")).toObject()));
  return diff.isEmpty() ? tr("%1 changed the run rules").arg(who) : tr("%1 changed %2").arg(who, diff);
}

QString draftWarning(const RunRules& saved, const RunRules& draft) {
  const bool cheatsOff = saved.cheatsAllowed() && !draft.cheatsAllowed();
  const bool memOff = saved.memoryWriteAllowed() && !draft.memoryWriteAllowed();
  if (cheatsOff && memOff) return tr("Everyone's active cheats turn off and memory changes stop as soon as you save.");
  if (cheatsOff) return tr("Everyone's active cheats turn off as soon as you save. The cheats stay in their lists.");
  if (memOff) return tr("Memory changes stop for everyone as soon as you save.");
  if ((!saved.cheatsAllowed() && draft.cheatsAllowed()) || (!saved.memoryWriteAllowed() && draft.memoryWriteAllowed()))
    return tr("Everyone sees each cheat and memory change in the run log.");
  return QString();
}

}  // namespace uzume::ui::runrules
