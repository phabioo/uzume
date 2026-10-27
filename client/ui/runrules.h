#pragma once
// The core rules of a run as the UI shows and edits them (design 11e / 11f): Save states, Rewind, Cheats, Memory changes.
// Pure helpers (no I/O): the Start dialog and the Rules dialog keep their draft as a QVariantMap
// {states: bool, rewind: bool, cheats: "off"|"logged", memoryWrite: "off"|"logged"}.
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include "runtypes.h"

namespace uzume::ui::runrules {

QVariantMap toMap(const RunRules& rules);
// Unknown or missing keys keep the defaults of a new run (everything off); unknown level strings count as "off".
RunRules fromMap(const QVariantMap& map);
// Sets one rule of the draft: key states | rewind (bool) or cheats | memoryWrite ("off" | "logged"; true/false are accepted too).
// False and nothing changed for an unknown key or level.
bool setRule(RunRules& rules, const QString& key, const QVariant& value);
// "Off" / "Allowed, logged" for a level, "On" / "Off" for a switch.
QString levelText(const QString& level);
// One line for the run header: "Save states off · Rewind off · Cheats off · Memory changes off".
QString summaryLine(const RunRules& rules);
// "Cheats: Off → Allowed, logged" parts for every rule that differs, joined with "; "; empty when nothing differs.
QString changeText(const RunRules& before, const RunRules& after);
// Log row of the run event rules.changed: "Lena changed Cheats: Off → Allowed, logged".
QString changeEventText(const QString& who, const QJsonObject& payload);
// What the Rules dialog warns about for a draft against the saved rules ("" = nothing): turning cheats or memory changes off acts at once.
QString draftWarning(const RunRules& saved, const RunRules& draft);

}  // namespace uzume::ui::runrules
