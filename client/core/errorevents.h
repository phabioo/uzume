#pragma once
// Error events of the app (ADR 0035 D1): what is recorded, the persisted queue and the unclean-exit marker.
//
//   makeEvent()     builds one event: title one line (<= 200), message redacted (diag::redact) and cut (<= 2000), fingerprint
//   ErrorQueue      <data>/errors-pending.json: at most 100 events (the oldest are dropped), an event with the same
//                   fingerprint as a queued one within 10 minutes is merged into it (dropped); sent in batches of <= 20
//   RunMarker       <data>/running.marker exists while the app runs; found at the next start it means the app did not exit
//                   cleanly (crash, kill, power loss). lastErrorLine() reads the ERROR/FATAL line of the previous log.
// No network and no UI here; sending is network/errorsender.h, the opt-in and the views are ui/errorscontroller.h.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <optional>

namespace uzume::errors {

inline constexpr int kMaxTitle = 200;
inline constexpr int kMaxMessage = 2000;
inline constexpr int kMaxQueued = 100;
inline constexpr int kBatchSize = 20;
inline constexpr int kMergeSeconds = 600;

struct ErrorEvent {
  QString kind, component, fingerprint, title, message, appVersion, os;
  QDateTime occurredAt;

  // The ErrorEvent object of POST /reports/errors (occurred_at as RFC 3339 UTC).
  QJsonObject toJson() const;
  static std::optional<ErrorEvent> fromJson(const QJsonObject& o);  // nullopt: a field is missing or malformed
};

struct EventSource {
  QString appVersion;
  QString os;
};

// `kind` and `component` must be valid (validKind / validComponent); an invalid one yields an event with an empty fingerprint.
ErrorEvent makeEvent(const QString& kind, const QString& component, const QString& title, const QString& message,
                     const EventSource& source, const QDateTime& now = {});

class ErrorQueue {
 public:
  explicit ErrorQueue(const QString& dataDir);

  static QString fileName(const QString& dataDir);  // <data>/errors-pending.json

  // True when queued; false when merged into a queued event of the same fingerprint (within kMergeSeconds) or invalid.
  bool add(const ErrorEvent& event);
  QList<ErrorEvent> front(int count = kBatchSize) const;  // oldest first
  void removeFront(int count);
  int size() const { return events_.size(); }
  bool isEmpty() const { return events_.isEmpty(); }
  void clear();
  const QList<ErrorEvent>& events() const { return events_; }

 private:
  void load();
  void save() const;

  QString path_;
  QList<ErrorEvent> events_;
};

// ERROR or FATAL line of a player log (filelog format `<time> [E] <category>: <text>`), the last one in the file, without the
// time and level and redacted with diag::redact. Empty when there is none or the file cannot be read. Only the file's tail is read.
QString lastErrorLine(const QString& logPath);

class RunMarker {
 public:
  static QString fileName(const QString& dataDir);  // <data>/running.marker
  static bool exists(const QString& dataDir);       // the previous run did not exit cleanly

  // Writes the marker (start time inside); the destructor removes it. Not created for an empty dataDir.
  explicit RunMarker(const QString& dataDir);
  ~RunMarker();
  RunMarker(const RunMarker&) = delete;
  RunMarker& operator=(const RunMarker&) = delete;
  void release();  // clean exit now

 private:
  QString path_;
};

}  // namespace uzume::errors
