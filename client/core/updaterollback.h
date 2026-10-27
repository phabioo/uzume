#pragma once
// Update rollback (ADR 0029 D4), pure logic without I/O: the small history of versions this device ran, the pause that
// follows going back, and the choice of the older releases that may be offered ("Go back to ...").

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "updateindex.h"

namespace uzume::update {

inline constexpr int kMaxRollbackCandidates = 5;  // the list offers at most the last 5 releases
inline constexpr int kMaxHistory = 20;

// A version this device actually ran, with the first and last time (ISO 8601 UTC) it was seen running.
struct HistoryEntry {
  QString version;
  QString first;
  QString last;
};
QList<HistoryEntry> parseHistory(const QJsonArray& a);
QJsonArray historyToJson(const QList<HistoryEntry>& h);
// Notes `version` as running at `now`. `last` is only touched when it is at least an hour old. Returns true when the
// history changed (and has to be saved). The newest kMaxHistory entries are kept.
bool recordVersion(QList<HistoryEntry>& history, const QString& version, const QDateTime& now);

// "Paused after going back": automatic updates stay off until "Resume updates". It ends by itself once the running
// version is no longer older than the version the user went back from.
struct Pause {
  QString from;       // the version the user went back from (SemVer)
  QString to;         // the version installed by the rollback
  QString at;         // ISO 8601 UTC
  int prevAuto = -1;  // update_auto_install before the pause: -1 unset, 0 off, 1 on
  bool active() const { return !from.isEmpty(); }
};
Pause parsePause(const QJsonObject& o);
QJsonObject pauseToJson(const Pause& p);
// The pause holds while the running version is older than `from`.
bool pauseEffective(const Pause& p, const QString& runningVersion);
// The skipped version (the one the user went back from) is not installed automatically while the running version
// is older than it. Once the running version has reached it the skip is moot.
bool skipEffective(const QString& skip, const QString& runningVersion);

struct RollbackQuery {
  QString currentVersion;
  Channel channel = Channel::Off;  // the install's channel: beta sees beta + stable, stable only stable, off none
  QList<HistoryEntry> history;     // versions that were run are offered whatever their channel
  int protocolVersion = 0;         // this Player's protocol; the target must speak it (min <= it <= protocol); 0 = not checked
  QString platform = QLatin1String(playerPlatform());
  QString kind = QLatin1String(playerKind());
};

struct RollbackCandidate {
  Release release;
  Artifact artifact;
  QString first;  // when this device first / last ran the version (empty when it never did)
  QString last;
  bool ran = false;
};

// Older releases of the same product/platform from the signed index, newest first, at most kMaxRollbackCandidates.
// For the MSI kind only a release whose MSI installs over a newer version (msiDowngrade) is offered; an AppImage is a plain file and can always be replaced by an older one. A version listed in several
// channels counts once (stable wins).
QList<RollbackCandidate> rollbackCandidates(const Index& index, const RollbackQuery& q);

}  // namespace uzume::update
