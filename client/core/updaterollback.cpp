#include "updaterollback.h"

#include <algorithm>

namespace uzume::update {

QList<HistoryEntry> parseHistory(const QJsonArray& a) {
  QList<HistoryEntry> out;
  for (const QJsonValue& v : a) {
    const QJsonObject o = v.toObject();
    HistoryEntry e{o.value(QLatin1String("version")).toString(), o.value(QLatin1String("first")).toString(),
                   o.value(QLatin1String("last")).toString()};
    if (!SemVer::parse(e.version)) continue;
    const bool dup = std::any_of(out.cbegin(), out.cend(), [&](const HistoryEntry& x) { return x.version == e.version; });
    if (!dup) out.append(e);
  }
  return out;
}

QJsonArray historyToJson(const QList<HistoryEntry>& h) {
  QJsonArray a;
  for (const HistoryEntry& e : h) {
    a.append(QJsonObject{{"version", e.version}, {"first", e.first}, {"last", e.last}});
  }
  return a;
}

bool recordVersion(QList<HistoryEntry>& history, const QString& version, const QDateTime& now) {
  if (!SemVer::parse(version) || !now.isValid()) return false;
  const QString stamp = now.toUTC().toString(Qt::ISODate);
  for (HistoryEntry& e : history) {
    if (e.version != version) continue;
    const QDateTime last = QDateTime::fromString(e.last, Qt::ISODate);
    if (last.isValid() && last <= now && last.secsTo(now) < 3600) return false;
    e.last = stamp;
    if (e.first.isEmpty()) e.first = stamp;
    return true;
  }
  history.append({version, stamp, stamp});
  while (history.size() > kMaxHistory) history.removeFirst();
  return true;
}

Pause parsePause(const QJsonObject& o) {
  Pause p;
  const QString from = o.value(QLatin1String("from")).toString();
  if (!SemVer::parse(from)) return p;
  p.from = from;
  p.to = o.value(QLatin1String("to")).toString();
  p.at = o.value(QLatin1String("at")).toString();
  const QJsonValue pa = o.value(QLatin1String("prev_auto"));
  p.prevAuto = pa.isBool() ? (pa.toBool() ? 1 : 0) : -1;
  return p;
}

QJsonObject pauseToJson(const Pause& p) {
  if (!p.active()) return {};
  QJsonObject o{{"from", p.from}, {"to", p.to}, {"at", p.at}};
  if (p.prevAuto >= 0) o.insert(QStringLiteral("prev_auto"), p.prevAuto == 1);
  return o;
}

bool pauseEffective(const Pause& p, const QString& runningVersion) {
  if (!p.active()) return false;
  const auto cmp = compareVersions(runningVersion, p.from);
  return !cmp || *cmp < 0;  // an unparsable running version stays paused
}

bool skipEffective(const QString& skip, const QString& runningVersion) {
  if (skip.isEmpty()) return false;
  const auto cmp = compareVersions(runningVersion, skip);
  return !cmp || *cmp < 0;
}

QList<RollbackCandidate> rollbackCandidates(const Index& index, const RollbackQuery& q) {
  QList<RollbackCandidate> out;
  const auto cur = SemVer::parse(q.currentVersion);
  if (!cur) return out;
  auto channelAllowed = [&](const QString& ch) {
    if (q.channel == Channel::Beta) return ch == QLatin1String("beta") || ch == QLatin1String("stable");
    if (q.channel == Channel::Stable) return ch == QLatin1String("stable");
    return false;
  };
  for (const Release& r : index.releases) {
    if (r.product != QLatin1String("player") || SemVer::compare(r.semver, *cur) >= 0) continue;
    const QString version = r.semver.toString();
    const auto hist = std::find_if(q.history.cbegin(), q.history.cend(), [&](const HistoryEntry& h) { return h.version == version; });
    if (!channelAllowed(r.channel) && hist == q.history.cend()) continue;
    if (q.kind == QLatin1String(kKindMsi) && !r.msiDowngrade) continue;  // an MSI built without AllowDowngrades refuses to install over a newer version
    if (q.protocolVersion > 0 && !(r.minProtocolVersion <= q.protocolVersion && q.protocolVersion <= r.protocolVersion)) continue;
    const auto art = r.artifact(q.platform, q.kind);
    if (!art) continue;
    RollbackCandidate c{r, *art, {}, {}, false};
    if (hist != q.history.cend()) {
      c.first = hist->first;
      c.last = hist->last;
      c.ran = true;
    }
    auto same = std::find_if(out.begin(), out.end(), [&](const RollbackCandidate& x) { return x.release.semver.toString() == version; });
    if (same != out.end()) {
      if (r.channel == QLatin1String("stable") && same->release.channel != QLatin1String("stable")) *same = c;  // stable wins
      continue;
    }
    out.append(c);
  }
  std::sort(out.begin(), out.end(), [](const RollbackCandidate& a, const RollbackCandidate& b) {
    return SemVer::compare(a.release.semver, b.release.semver) > 0;
  });
  while (out.size() > kMaxRollbackCandidates) out.removeLast();
  return out;
}

}  // namespace uzume::update
