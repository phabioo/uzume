#include "runrecap.h"

#include <QCoreApplication>
#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <set>

namespace uzume {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("RunRecap", text); }

QDateTime parseTime(const QString& iso) { return QDateTime::fromString(iso, Qt::ISODateWithMs); }

QString fallbackText(const QString& type) {
  if (type == QLatin1String("rules.changed")) return tr("Rules changed");
  if (type == QLatin1String("cheat.enabled")) return tr("Cheats turned on");
  if (type == QLatin1String("cheat.disabled")) return tr("Cheats turned off");
  if (type == QLatin1String("memory.write")) return tr("Memory written");
  if (type == QLatin1String("state.loaded")) return tr("Save state loaded");
  return type;
}

QString daysText(int days) { return days == 1 ? tr("1 day") : tr("%1 days").arg(days); }

QString splitText(const RecapModel& m, int RecapLane::*field) {
  QStringList parts;
  for (const RecapLane& l : m.lanes)
    if (l.*field > 0) parts << QStringLiteral("%1 %2").arg(l.name).arg(l.*field);
  return parts.join(QStringLiteral(" · "));
}

QJsonObject eventJson(const RecapEvent& e) {
  QJsonObject o{{QStringLiteral("glyph"), recapGlyphName(e.glyph)},
                {QStringLiteral("type"), e.type},
                {QStringLiteral("text"), e.text},
                {QStringLiteral("created_at"), e.createdAt},
                {QStringLiteral("day"), e.day}};
  if (e.seq > 0) o.insert(QStringLiteral("seq"), double(e.seq));
  if (!e.pluginId.isEmpty()) o.insert(QStringLiteral("plugin_id"), e.pluginId);
  if (!e.screenshotId.isEmpty()) o.insert(QStringLiteral("screenshot_id"), e.screenshotId);
  return o;
}

// Axis labels: day 1 and every `step`-th day after it (1 3 5 7 9 on the board), each at the middle of its day.
struct AxisLabel {
  int day;
  double pos;
};
QList<AxisLabel> axisLabels(int days) {
  QList<AxisLabel> out;
  const int step = std::max(1, int(std::ceil((days - 1) / 4.0)));
  for (int d = 1; d <= days; d += step) out.append({d, (d - 0.5) / days});
  return out;
}

}  // namespace

QString recapGlyphName(RecapGlyph g) {
  switch (g) {
    case RecapGlyph::Catch: return QStringLiteral("catch");
    case RecapGlyph::Lost: return QStringLiteral("lost");
    case RecapGlyph::Badge: return QStringLiteral("badge");
    case RecapGlyph::Note: return QStringLiteral("note");
    case RecapGlyph::Screenshot: return QStringLiteral("screenshot");
  }
  return QStringLiteral("note");
}

QString recapGlyphChar(RecapGlyph g) {
  switch (g) {
    case RecapGlyph::Catch: return QStringLiteral("■");       // ■
    case RecapGlyph::Lost: return QStringLiteral("✕");        // ✕
    case RecapGlyph::Badge: return QStringLiteral("◆");       // ◆
    case RecapGlyph::Note: return QStringLiteral("□");        // □
    case RecapGlyph::Screenshot: return QStringLiteral("▣");  // ▣
  }
  return QStringLiteral("□");
}

std::optional<RecapGlyph> parseRecapGlyph(const QString& name) {
  for (RecapGlyph g : {RecapGlyph::Catch, RecapGlyph::Lost, RecapGlyph::Badge, RecapGlyph::Note, RecapGlyph::Screenshot})
    if (recapGlyphName(g) == name) return g;
  return std::nullopt;
}

bool isCoreRecapEvent(const QString& type) {
  return type == QLatin1String("rules.changed") || type == QLatin1String("cheat.enabled") || type == QLatin1String("cheat.disabled") ||
         type == QLatin1String("memory.write") || type == QLatin1String("state.loaded");
}

QString recapDurationText(qint64 seconds) {
  if (seconds >= 3600) return tr("%1 h").arg(qint64(std::llround(seconds / 3600.0)));
  return tr("%1 min").arg(seconds <= 0 ? 0 : std::max<qint64>(1, seconds / 60));
}

// ------------------------------------------------------------------ the model

RecapModel buildRecap(const RecapInput& in) {
  RecapModel m;
  m.runId = in.run.runId;
  m.runName = in.run.name;
  m.status = in.run.status;
  m.playSeconds = in.run.playSeconds;
  m.hasPlayTime = in.run.playSeconds > 0;

  const QTimeZone zone = in.zone.isValid() ? in.zone : QTimeZone::utc();
  QDateTime created = parseTime(in.run.createdAt);
  if (!created.isValid()) {
    // No usable start: day 1 is the first thing that happened (or now).
    created = in.now;
    for (const RunEvent& e : in.events) {
      const QDateTime t = parseTime(e.createdAt);
      if (t.isValid() && t < created) created = t;
    }
  }
  const QDate startDate = created.toTimeZone(zone).date();
  const QDateTime axisStart = QDateTime(startDate, QTime(0, 0), zone);
  m.startDate = startDate.toString(Qt::ISODate);

  // Lanes: the people who took part (in join order), others only when they have something on the board.
  QList<RecapLane> lanes;
  QHash<QString, int> laneIndex;
  std::set<QString> withLane;
  for (const RunParticipant& p : in.run.participants) {
    if (p.status != QLatin1String("joined") && p.status != QLatin1String("left")) continue;
    RecapLane l;
    l.userId = p.userId;
    l.name = p.userName.isEmpty() ? p.userId : p.userName;
    l.color = p.color;
    laneIndex.insert(p.userId, int(lanes.size()));
    lanes.append(l);
  }
  auto laneFor = [&](const QString& userId, const QString& name) -> RecapLane* {
    if (userId.isEmpty()) return nullptr;
    auto it = laneIndex.constFind(userId);
    if (it == laneIndex.constEnd()) {
      RecapLane l;
      l.userId = userId;
      l.name = name;
      if (const RunParticipant* p = in.run.participant(userId)) {
        l.color = p->color;
        if (!p->userName.isEmpty()) l.name = p->userName;
      }
      if (l.name.isEmpty()) l.name = userId;
      laneIndex.insert(userId, int(lanes.size()));
      lanes.append(l);
      return &lanes.last();
    }
    return &lanes[it.value()];
  };

  struct Placed {
    RecapEvent ev;
    QDateTime when;
    QString userId;
    QString userName;
  };
  QList<Placed> placed;
  auto glyphOf = [&](const RunEvent& e) -> std::optional<RecapGlyph> {
    if (isCoreRecapEvent(e.type)) return RecapGlyph::Note;
    if (e.pluginId.isEmpty()) return std::nullopt;
    const QString g = in.mappings.value(e.pluginId).value(e.type);
    return g.isEmpty() ? std::nullopt : parseRecapGlyph(g);
  };
  for (const RunEvent& e : in.events) {
    if (e.source == QLatin1String("hub") && !isCoreRecapEvent(e.type)) continue;  // run.created, run.joined, ...
    const auto glyph = glyphOf(e);
    if (!glyph) continue;
    QString userId = e.userId;
    QString userName = e.userName;
    if (e.type == QLatin1String("rules.changed") || userId.isEmpty()) {
      const QString by = e.payload.value(QStringLiteral("by_user_id")).toString();
      if (!by.isEmpty()) {
        userId = by;
        userName = e.payload.value(QStringLiteral("by_user_name")).toString();
      }
    }
    const QDateTime when = parseTime(e.createdAt);
    if (userId.isEmpty() || !when.isValid()) continue;
    Placed p;
    p.ev.glyph = *glyph;
    p.ev.type = e.type;
    p.ev.text = e.text.isEmpty() ? fallbackText(e.type) : e.text;
    p.ev.pluginId = e.pluginId;
    p.ev.createdAt = e.createdAt;
    p.ev.seq = e.seq;
    p.when = when;
    p.userId = userId;
    p.userName = userName;
    placed.append(p);
  }
  for (const ScreenshotInfo& s : in.screenshots) {
    const QDateTime when = parseTime(s.createdAt);
    if (s.userId.isEmpty() || !when.isValid()) continue;
    Placed p;
    p.ev.glyph = RecapGlyph::Screenshot;
    p.ev.type = QStringLiteral("screenshot");
    p.ev.text = tr("Screenshot");
    p.ev.createdAt = s.createdAt;
    p.ev.screenshotId = s.id;
    p.when = when;
    p.userId = s.userId;
    p.userName = s.userName;
    placed.append(p);
  }

  // Axis: day 1 .. the day of the last event or of the end of the run's activity.
  QDateTime end = in.run.acceptsWrites() ? in.now : parseTime(in.run.updatedAt);
  if (!end.isValid()) end = in.now;
  int days = std::max(1, int(startDate.daysTo(end.toTimeZone(zone).date())) + 1);
  for (Placed& p : placed) {
    p.ev.day = std::max(1, int(startDate.daysTo(p.when.toTimeZone(zone).date())) + 1);
    days = std::max(days, p.ev.day);
  }
  m.days = days;
  const double span = double(axisStart.msecsTo(axisStart.addDays(days)));
  for (Placed& p : placed) {
    p.ev.pos = std::clamp(double(axisStart.msecsTo(p.when)) / span, 0.0, 1.0);
  }

  std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) {
    if (a.when != b.when) return a.when < b.when;
    return a.ev.seq < b.ev.seq;
  });
  for (const Placed& p : placed) {
    RecapLane* lane = laneFor(p.userId, p.userName);
    if (lane == nullptr) continue;
    lane->events.append(p.ev);
    switch (p.ev.glyph) {
      case RecapGlyph::Catch: ++lane->catches; break;
      case RecapGlyph::Lost: ++lane->lost; break;
      case RecapGlyph::Badge: ++lane->badges; break;
      default: break;
    }
  }
  // The pointer into `lanes` above is only used inside the loop (appends can move it, it is looked up again each time).
  m.lanes = lanes;
  for (const RecapLane& l : std::as_const(m.lanes)) {
    m.catches += l.catches;
    m.lost += l.lost;
    m.badges += l.badges;
    m.shownEvents += int(l.events.size());
  }
  return m;
}

QList<RecapTile> RecapModel::tiles() const {
  QList<RecapTile> t;
  t.append({QStringLiteral("catches"), tr("CATCHES"), QString::number(catches), splitText(*this, &RecapLane::catches)});
  t.append({QStringLiteral("lost"), tr("LOST"), QString::number(lost), splitText(*this, &RecapLane::lost)});
  t.append({QStringLiteral("badges"), tr("BADGES"), QString::number(badges), splitText(*this, &RecapLane::badges)});
  if (hasPlayTime)
    t.append({QStringLiteral("playtime"), tr("PLAY TIME"), recapDurationText(playSeconds), tr("over %1").arg(daysText(days))});
  else  // the server sends no play time (older server or none played): the elapsed days instead, without a second line
    t.append({QStringLiteral("playtime"), tr("ELAPSED"), daysText(days), QString()});
  return t;
}

QVariantMap RecapModel::toVariant() const {
  QVariantList tl;
  for (const RecapTile& t : tiles())
    tl.append(QVariantMap{{QStringLiteral("id"), t.id}, {QStringLiteral("label"), t.label}, {QStringLiteral("value"), t.value}, {QStringLiteral("sub"), t.sub}});
  QVariantList ll;
  for (const RecapLane& l : lanes) {
    QVariantList el;
    for (const RecapEvent& e : l.events)
      el.append(QVariantMap{{QStringLiteral("glyph"), recapGlyphName(e.glyph)},
                            {QStringLiteral("glyphChar"), recapGlyphChar(e.glyph)},
                            {QStringLiteral("type"), e.type},
                            {QStringLiteral("text"), e.text},
                            {QStringLiteral("createdAt"), e.createdAt},
                            {QStringLiteral("day"), e.day},
                            {QStringLiteral("pos"), e.pos},
                            {QStringLiteral("screenshotId"), e.screenshotId}});
    ll.append(QVariantMap{{QStringLiteral("userId"), l.userId}, {QStringLiteral("name"), l.name}, {QStringLiteral("color"), l.color},
                          {QStringLiteral("catches"), l.catches}, {QStringLiteral("lost"), l.lost}, {QStringLiteral("badges"), l.badges},
                          {QStringLiteral("events"), el}});
  }
  QVariantList axis;
  for (const AxisLabel& a : axisLabels(days))
    axis.append(QVariantMap{{QStringLiteral("day"), a.day}, {QStringLiteral("label"), tr("DAY %1").arg(a.day)}, {QStringLiteral("pos"), a.pos}});
  return {{QStringLiteral("runId"), runId}, {QStringLiteral("runName"), runName}, {QStringLiteral("status"), status},
          {QStringLiteral("startDate"), startDate}, {QStringLiteral("days"), days}, {QStringLiteral("empty"), isEmpty()},
          {QStringLiteral("shownEvents"), shownEvents}, {QStringLiteral("tiles"), tl}, {QStringLiteral("lanes"), ll}, {QStringLiteral("axis"), axis}};
}

QJsonObject RecapModel::toJson() const {
  QJsonArray tl;
  for (const RecapTile& t : tiles())
    tl.append(QJsonObject{{QStringLiteral("id"), t.id}, {QStringLiteral("label"), t.label}, {QStringLiteral("value"), t.value}, {QStringLiteral("sub"), t.sub}});
  QJsonArray ll;
  for (const RecapLane& l : lanes) {
    QJsonArray el;
    QMap<int, QJsonArray> byDay;
    for (const RecapEvent& e : l.events) {
      el.append(eventJson(e));
      byDay[e.day].append(eventJson(e));
    }
    QJsonArray dl;
    for (auto it = byDay.cbegin(); it != byDay.cend(); ++it)
      dl.append(QJsonObject{{QStringLiteral("day"), it.key()}, {QStringLiteral("events"), it.value()}});
    ll.append(QJsonObject{{QStringLiteral("user_id"), l.userId}, {QStringLiteral("name"), l.name}, {QStringLiteral("catches"), l.catches},
                          {QStringLiteral("lost"), l.lost}, {QStringLiteral("badges"), l.badges}, {QStringLiteral("events"), el},
                          {QStringLiteral("per_day"), dl}});
  }
  return {{QStringLiteral("run_id"), runId}, {QStringLiteral("name"), runName}, {QStringLiteral("status"), status},
          {QStringLiteral("start_date"), startDate}, {QStringLiteral("days"), days}, {QStringLiteral("empty"), isEmpty()},
          {QStringLiteral("play_seconds"), double(playSeconds)}, {QStringLiteral("has_play_time"), hasPlayTime},
          {QStringLiteral("catches"), catches}, {QStringLiteral("lost"), lost}, {QStringLiteral("badges"), badges},
          {QStringLiteral("shown_events"), shownEvents}, {QStringLiteral("tiles"), tl}, {QStringLiteral("lanes"), ll}};
}

// ------------------------------------------------------------------ HTML export

QString recapHtml(const RecapModel& m, const QString& serverName) {
  auto esc = [](const QString& s) { return s.toHtmlEscaped(); };
  QString h;
  h += QStringLiteral(
      "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
      "<title>Run recap - %1</title>\n<style>\n"
      ":root{--ground:#fbfaf5;--ink:#1c1b19;--muted:#5f5c55;--dim:#6a665f;--rule:#d6d2c6;--rule2:#e9e6dd}\n"
      "@media (prefers-color-scheme:dark){:root{--ground:#1c1b19;--ink:#efece4;--muted:#a19d94;--dim:#96928a;--rule:#3a3833;--rule2:#2b2a27}}\n"
      "body{margin:0;background:var(--ground);color:var(--ink);font:14px/20px 'IBM Plex Sans',system-ui,sans-serif}\n"
      "main{max-width:960px;margin:0 auto;padding:32px 24px}\n"
      ".mono{font-family:'IBM Plex Mono',ui-monospace,monospace}\n"
      ".label{font:500 10px/16px 'IBM Plex Mono',ui-monospace,monospace;letter-spacing:.14em;color:var(--dim);text-transform:uppercase}\n"
      "h1{margin:8px 0 24px;font-weight:300;font-size:40px;line-height:40px;letter-spacing:-.02em}\n"
      ".tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));border-top:1px solid var(--rule);border-bottom:1px solid var(--rule2);margin-bottom:24px}\n"
      ".tile{display:flex;flex-direction:column;gap:6px;padding:14px 16px;border-left:1px solid var(--rule2)}.tile:first-child{border-left:0;padding-left:0}\n"
      ".tile b{font-weight:300;font-size:28px;line-height:32px}.tile span.sub{font-size:12px;line-height:18px;color:var(--muted)}\n"
      ".row{display:grid;grid-template-columns:120px minmax(0,1fr);column-gap:16px;align-items:center}\n"
      ".axis{padding:6px 0;border-bottom:1px solid var(--rule);position:relative;height:16px}.axis span{position:absolute;transform:translateX(-50%);font:400 10px/16px 'IBM Plex Mono',ui-monospace,monospace;letter-spacing:.06em;color:var(--dim)}\n"
      ".lane{height:48px;border-bottom:1px solid var(--rule2)}.track{position:relative;height:48px;background:linear-gradient(var(--rule2),var(--rule2)) center/100% 1px no-repeat}\n"
      ".g{position:absolute;top:50%;transform:translate(-50%,-50%);font:500 12px/12px 'IBM Plex Mono',ui-monospace,monospace}.g.shot{color:var(--muted)}\n"
      ".legend{display:flex;gap:16px;flex-wrap:wrap;margin:12px 0;font:400 12px/16px 'IBM Plex Mono',ui-monospace,monospace;color:var(--muted)}\n"
      ".foot{margin-top:24px;font-size:12px;color:var(--muted)}\n"
      "</style></head><body><main>\n")
           .arg(esc(m.runName));
  h += QStringLiteral("<div class=\"label\">Run recap%1</div>\n<h1>%2</h1>\n")
           .arg(serverName.isEmpty() ? QString() : QStringLiteral(" · ") + esc(serverName), esc(m.runName));
  h += QStringLiteral("<div class=\"tiles\">\n");
  for (const RecapTile& t : m.tiles()) {
    h += QStringLiteral("<div class=\"tile\"><span class=\"label\">%1</span><b>%2</b>").arg(esc(t.label), esc(t.value));
    if (!t.sub.isEmpty()) h += QStringLiteral("<span class=\"sub\">%1</span>").arg(esc(t.sub));
    h += QStringLiteral("</div>\n");
  }
  h += QStringLiteral("</div>\n<div class=\"label\">Lanes · DAY 1 – DAY %1</div>\n").arg(m.days);
  if (m.isEmpty()) {
    h += QStringLiteral("<p class=\"foot\">Nothing to show yet.</p>\n");
  } else {
    h += QStringLiteral("<div class=\"row\"><span></span><div class=\"axis\">");
    for (const AxisLabel& a : axisLabels(m.days))
      h += QStringLiteral("<span style=\"left:%1%\">DAY %2</span>").arg(a.pos * 100.0, 0, 'f', 2).arg(a.day);
    h += QStringLiteral("</div></div>\n");
    for (const RecapLane& l : m.lanes) {
      h += QStringLiteral("<div class=\"row lane\"><span>%1</span><div class=\"track\">").arg(esc(l.name));
      for (const RecapEvent& e : l.events) {
        h += QStringLiteral("<span class=\"g%1\" style=\"left:%2%\" title=\"%3\">%4</span>")
                 .arg(e.glyph == RecapGlyph::Screenshot ? QStringLiteral(" shot") : QString())
                 .arg(e.pos * 100.0, 0, 'f', 2)
                 .arg(esc(QStringLiteral("%1 · %2 · day %3").arg(recapGlyphName(e.glyph), e.text).arg(e.day)), recapGlyphChar(e.glyph));
      }
      h += QStringLiteral("</div></div>\n");
    }
  }
  h += QStringLiteral("<div class=\"legend\"><span>%1 catch</span><span>%2 lost</span><span>%3 badge</span><span>%4 note</span><span>%5 screenshot</span></div>\n")
           .arg(recapGlyphChar(RecapGlyph::Catch), recapGlyphChar(RecapGlyph::Lost), recapGlyphChar(RecapGlyph::Badge),
                recapGlyphChar(RecapGlyph::Note), recapGlyphChar(RecapGlyph::Screenshot));
  h += QStringLiteral("<div class=\"foot\">Exported from Uzume%1.</div>\n</main></body></html>\n")
           .arg(serverName.isEmpty() ? QString() : QStringLiteral(" (") + esc(serverName) + QLatin1Char(')'));
  return h;
}

// ------------------------------------------------------------------ loader

RunRecapLoader::RunRecapLoader(HubConnection* connection, QObject* parent) : QObject(parent), conn_(connection) {}

void RunRecapLoader::finish(quint64 gen, bool ok, const QString& error) {
  if (gen != gen_ || !done_) return;
  result_.ok = ok;
  result_.error = error;
  auto cb = std::move(done_);
  done_ = nullptr;
  cb(result_);
}

void RunRecapLoader::load(const QString& runId, std::function<void(const RecapFetch&)> done) {
  ++gen_;
  done_ = std::move(done);
  result_ = {};
  page(gen_, runId, 0, kMaxPages);
}

void RunRecapLoader::page(quint64 gen, const QString& runId, qint64 after, int pagesLeft) {
  QNetworkReply* reply = conn_ ? conn_->authorizedGet(QStringLiteral("/runs/%1/events?after=%2&limit=%3")
                                                          .arg(QString::fromLatin1(QUrl::toPercentEncoding(runId)))
                                                          .arg(after)
                                                          .arg(kPageSize))
                               : nullptr;
  if (reply == nullptr) {
    QTimer::singleShot(0, this, [this, gen] { finish(gen, false, tr("Not connected to the server")); });
    return;
  }
  connect(reply, &QNetworkReply::finished, this, [this, reply, gen, runId, after, pagesLeft] {
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    if (gen != gen_) return;
    if (r.status == 401 && conn_) conn_->noteUnauthorized();
    if (!r.ok()) {
      finish(gen, false, r.status == 404 ? tr("This run is not on the server any more.")
                                          : (r.apiErrorMessage.isEmpty() ? (r.errorString.isEmpty() ? tr("The server did not answer.") : r.errorString)
                                                                          : r.apiErrorMessage));
      return;
    }
    const QJsonObject o = r.json();
    qint64 last = after;
    int got = 0;
    for (const QJsonValue& v : o.value(QStringLiteral("events")).toArray()) {
      if (const auto e = parseRunEvent(v.toObject())) {
        if (e->seq > after) {
          result_.events.append(*e);
          last = std::max(last, e->seq);
        }
        ++got;
      }
    }
    const qint64 hubLast = qint64(o.value(QStringLiteral("last_seq")).toDouble(0));
    const bool more = hubLast > 0 ? last < hubLast : got >= kPageSize;
    if (got > 0 && last > after && more && pagesLeft > 1) {
      page(gen, runId, last, pagesLeft - 1);
      return;
    }
    // Screenshots of the run (all members); a server without them leaves the glyphs out.
    auto* api = new ScreenshotApi(conn_, this);
    api->listRun(runId, [this, gen, api](const ScreenshotResult& s) {
      if (s.ok) result_.screenshots = s.screenshots;
      api->deleteLater();
      finish(gen, true, QString());
    });
  });
}

}  // namespace uzume
