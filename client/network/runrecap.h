#pragma once
// Run recap (Uzume 0.22 package 2, design board 4jh-app-2): the model behind the Recap tab of a run. Pure data: it is built from
// the run, its complete event log, its screenshots and the `recap` mappings of the manifests of the run's plugins; no network,
// no UI. The loader (RunRecapLoader) fetches the inputs, the UI and the debug MCP tool present the model, recapHtml() exports it.
//   * Glyphs: catch, lost, badge, note, screenshot (the board's ■ ✕ ◆ □ ▣). Only catch, lost and badge are counted.
//   * An event of a plugin is shown when the plugin's `recap` block maps its type; events without a mapping, and events of plugins
//     that are not installed (no mapping known), are not shown. The core events rules.changed, cheat.enabled, cheat.disabled,
//     memory.write and state.loaded are notes. Run screenshots are screenshot glyphs.
//   * Day 1 is the day the run was created (in the time zone of the viewer); the axis ends with the day of the last event or of
//     now (running) / the last update (paused, finished), whichever is later.
#include <QDateTime>
#include <QJsonObject>
#include <QPointer>
#include <QList>
#include <QMap>
#include <QString>
#include <QTimeZone>
#include <QVariantMap>
#include <functional>
#include <optional>

#include "hubconnection.h"
#include "runtypes.h"
#include "screenshotapi.h"

namespace uzume {

enum class RecapGlyph { Catch, Lost, Badge, Note, Screenshot };
QString recapGlyphName(RecapGlyph g);   // "catch" | "lost" | "badge" | "note" | "screenshot"
QString recapGlyphChar(RecapGlyph g);   // ■ ✕ ◆ □ ▣
std::optional<RecapGlyph> parseRecapGlyph(const QString& name);
bool isCoreRecapEvent(const QString& type);  // rules.changed, cheat.enabled, cheat.disabled, memory.write, state.loaded

// plugin id -> (run event type -> glyph name) from the `recap` blocks of the installed manifests.
using RecapMappings = QMap<QString, QMap<QString, QString>>;

struct RecapInput {
  RunInfo run;
  QList<RunEvent> events;                // the whole log, ascending
  QList<ScreenshotInfo> screenshots;     // GET /runs/{id}/screenshots
  RecapMappings mappings;
  QTimeZone zone = QTimeZone::systemTimeZone();
  QDateTime now = QDateTime::currentDateTimeUtc();
};

struct RecapEvent {
  RecapGlyph glyph = RecapGlyph::Note;
  QString type;          // run event type, "screenshot" for a screenshot
  QString text;          // the event text, else the type
  QString pluginId;      // empty for core events and screenshots
  QString createdAt;     // RFC 3339
  qint64 seq = 0;        // 0 for a screenshot
  QString screenshotId;  // screenshots only
  int day = 1;           // 1-based
  double pos = 0;        // 0..1 along the lane (time over the whole axis)
};

struct RecapLane {
  QString userId;
  QString name;
  int color = -1;        // join order 0..7, -1 unknown
  int catches = 0;
  int lost = 0;
  int badges = 0;
  QList<RecapEvent> events;  // ascending by time
};

struct RecapTile {
  QString id;     // catches | lost | badges | playtime
  QString label;  // CATCHES, LOST, BADGES, PLAY TIME (ELAPSED without play time)
  QString value;  // "41", "62 h", "9 days"
  QString sub;    // "Max 16 · Lena 14", "over 9 days"; may be empty
};

struct RecapModel {
  QString runId;
  QString runName;
  QString status;
  QString startDate;    // yyyy-MM-dd of day 1 in the viewer's zone
  int days = 1;
  int catches = 0;
  int lost = 0;
  int badges = 0;
  qint64 playSeconds = 0;
  bool hasPlayTime = false;
  int shownEvents = 0;  // glyphs on all lanes
  QList<RecapLane> lanes;

  bool isEmpty() const { return shownEvents == 0; }
  QList<RecapTile> tiles() const;
  QVariantMap toVariant() const;  // QML: tiles, lanes (events with glyph/char/day/pos/...), days, axis labels
  QJsonObject toJson() const;     // MCP: tiles, lanes with events (and perDay), counts
};

RecapModel buildRecap(const RecapInput& in);

// "62 h", "45 min", "1 h 30 min" is not used: hours are rounded, below an hour minutes are shown.
QString recapDurationText(qint64 seconds);
// One self-contained HTML file: inline CSS (follows prefers-color-scheme), glyphs as text, no external resource and no script.
QString recapHtml(const RecapModel& model, const QString& serverName);

// Fetches the complete input of a run: GET /runs/{id}/events in pages of 500 (until the log ends) and the run screenshots.
// One loader per request; the callback runs once, on the event loop, never synchronously. A server that does not know
// screenshots only leaves them out; a failed event page fails the whole load.
struct RecapFetch {
  bool ok = false;
  QString error;                         // English text when !ok
  QList<RunEvent> events;
  QList<ScreenshotInfo> screenshots;
};
class RunRecapLoader : public QObject {
  Q_OBJECT
 public:
  static constexpr int kPageSize = 500;
  static constexpr int kMaxPages = 200;  // 100 000 events
  RunRecapLoader(HubConnection* connection, QObject* parent = nullptr);
  void load(const QString& runId, std::function<void(const RecapFetch&)> done);

 private:
  void page(quint64 gen, const QString& runId, qint64 after, int pagesLeft);
  void finish(quint64 gen, bool ok, const QString& error);
  QPointer<HubConnection> conn_;
  std::function<void(const RecapFetch&)> done_;
  RecapFetch result_;
  quint64 gen_ = 0;
};

}  // namespace uzume
