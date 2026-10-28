#pragma once
// Seam between the plugin runtime and the app for uz.memory.write, uz.cheats and uz.save.snapshot (0.20, permissions
// `memory.write` and `cheats`). The plugin thread only calls this interface; every method is thread-safe and never waits for
// the GUI thread, the emulation thread or the network. The app (ui/pluginemulation.*) owns the consent per game, the
// "Before ..." safety copies, the run rules and the run log, so no plugin can skip them.
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace uzume::plugins {

// One entry of a cheat list handed to uz.cheats.apply.
struct CheatItem {
  QString id;
  QString name;
  QString format;  // raw | gsa | cb | ar_ds | ar_gba | game_genie | gateway
  QString code;
  bool enabled = true;
};
struct CheatReject {
  QString id;
  QString reason;  // "unsupported_format" | "invalid_code" | "invalid"
};
struct CheatApplyResult {
  QString error;  // not empty: the whole call was refused ("no_game", "blocked_by_run_rule", "consent_required", "consent_denied", ...)
  int applied = 0;
  QList<CheatReject> rejected;
};
struct CheatDatabaseResult {
  // Becomes the second result of uz.cheats.database() after nil: "off" (the cheat database is off on the server), "unavailable" (it is on, but the server holds no lists yet: not loaded, or the update failed), "offline",
  // "not_found" (no such game on the server) or "error: <text>". While the first fetch is under way: "error: loading" (the app
  // event cheats.database follows).
  QString error;
  QJsonObject data;   // {source, updated_at, cheats:[{name, code, format_guess}], ...} of GET /games/{id}/cheats
};

class EmulationBridge {
 public:
  virtual ~EmulationBridge() = default;

  // Queues a write of `bytes` at `addr` (the core's address space). "" = accepted; otherwise "no_game", "consent_required"
  // (the app asks the person; the plugin may try again), "consent_denied", "blocked_by_run_rule", "unmapped", "queue_full",
  // "invalid".
  virtual QString writeMemory(const QString& pluginId, quint64 addr, const QByteArray& bytes) = 0;

  // Cheat formats of the running core and system (empty without a game or a core with cheat support).
  virtual QStringList cheatFormats() const = 0;
  // Display name of the running core ("mGBA"), second result of uz.cheats.formats(); empty = none.
  virtual QString coreName() const { return QString(); }
  // Why cheats cannot be turned on right now: "" = they can, "run_rule" = the run's rule is Off (uz.cheats.allowed).
  virtual QString cheatsBlockedReason() const { return QString(); }
  // Replaces the whole active list. Entries the core cannot use come back in `rejected`.
  virtual CheatApplyResult applyCheats(const QString& pluginId, const QList<CheatItem>& list) = 0;
  // Turns every active cheat off ("" = done, else the error).
  virtual QString clearCheats(const QString& pluginId) = 0;
  // GET /games/{id}/cheats with a local cache: the cached copy at once (also offline), refreshed in the background.
  virtual CheatDatabaseResult cheatDatabase() = 0;

  // Manual save snapshot "<plugin name>: <reason>". "" = accepted (done asynchronously; the person gets a toast), else why not
  // ("no_game", "unsupported", ...). Rate limit and reason length are checked by the caller.
  virtual QString snapshot(const QString& pluginId, const QString& pluginName, const QString& reason) = 0;
};

}  // namespace uzume::plugins
