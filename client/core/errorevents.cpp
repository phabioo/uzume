#include "errorevents.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

#include "diagbundle.h"
#include "errorfingerprint.h"

namespace uzume::errors {

namespace {

constexpr qint64 kLogTailBytes = 256 * 1024;

QString oneLine(const QString& s, int max) {
  QString t = s;
  const qsizetype nl = t.indexOf(QRegularExpression(QStringLiteral("[\r\n]")));
  if (nl >= 0) t.truncate(nl);
  t = t.trimmed();
  if (t.size() > max) t = t.left(max);
  return t;
}

QString redacted(const QString& s) { return QString::fromUtf8(diag::redact(s.toUtf8())); }

}  // namespace

QJsonObject ErrorEvent::toJson() const {
  QJsonObject o;
  o.insert(QStringLiteral("kind"), kind);
  o.insert(QStringLiteral("component"), component);
  o.insert(QStringLiteral("fingerprint"), fingerprint);
  o.insert(QStringLiteral("title"), title);
  o.insert(QStringLiteral("message"), message);
  o.insert(QStringLiteral("app_version"), appVersion);
  o.insert(QStringLiteral("os"), os);
  o.insert(QStringLiteral("occurred_at"), occurredAt.toUTC().toString(Qt::ISODate));
  return o;
}

std::optional<ErrorEvent> ErrorEvent::fromJson(const QJsonObject& o) {
  ErrorEvent e;
  e.kind = o.value(QStringLiteral("kind")).toString();
  e.component = o.value(QStringLiteral("component")).toString();
  e.fingerprint = o.value(QStringLiteral("fingerprint")).toString();
  e.title = o.value(QStringLiteral("title")).toString();
  e.message = o.value(QStringLiteral("message")).toString();
  e.appVersion = o.value(QStringLiteral("app_version")).toString();
  e.os = o.value(QStringLiteral("os")).toString();
  e.occurredAt = QDateTime::fromString(o.value(QStringLiteral("occurred_at")).toString(), Qt::ISODate);
  if (!validKind(e.kind) || !validComponent(e.component) || !validFingerprint(e.fingerprint) || e.title.isEmpty() || !e.occurredAt.isValid()) {
    return std::nullopt;
  }
  return e;
}

ErrorEvent makeEvent(const QString& kind, const QString& component, const QString& title, const QString& message,
                     const EventSource& source, const QDateTime& now) {
  ErrorEvent e;
  e.kind = kind;
  e.component = component;
  e.title = oneLine(redacted(title), kMaxTitle);
  if (e.title.isEmpty()) e.title = kind;
  e.message = redacted(message).trimmed().left(kMaxMessage);
  e.appVersion = source.appVersion.left(40);
  e.os = source.os.left(80);
  e.occurredAt = (now.isValid() ? now : QDateTime::currentDateTimeUtc()).toUTC();
  if (validKind(kind) && validComponent(component)) e.fingerprint = fingerprint(kind, component, e.message);
  return e;
}

// ---------------------------------------------------------------- queue

ErrorQueue::ErrorQueue(const QString& dataDir) : path_(fileName(dataDir)) { load(); }

QString ErrorQueue::fileName(const QString& dataDir) { return QDir(dataDir).filePath(QStringLiteral("errors-pending.json")); }

void ErrorQueue::load() {
  events_.clear();
  QFile f(path_);
  if (!f.open(QIODevice::ReadOnly)) return;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  for (const QJsonValue& v : doc.object().value(QStringLiteral("events")).toArray()) {
    if (const auto e = ErrorEvent::fromJson(v.toObject())) events_.append(*e);
  }
  while (events_.size() > kMaxQueued) events_.removeFirst();
}

void ErrorQueue::save() const {
  if (events_.isEmpty()) {
    QFile::remove(path_);
    return;
  }
  QDir().mkpath(QFileInfo(path_).absolutePath());
  QJsonArray arr;
  for (const ErrorEvent& e : events_) arr.append(e.toJson());
  QJsonObject root;
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("events"), arr);
  QSaveFile f(path_);
  if (!f.open(QIODevice::WriteOnly)) return;
  f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
  f.commit();
}

bool ErrorQueue::add(const ErrorEvent& event) {
  if (!validFingerprint(event.fingerprint) || !validKind(event.kind) || !validComponent(event.component)) return false;
  for (const ErrorEvent& q : std::as_const(events_)) {
    if (q.fingerprint == event.fingerprint && qAbs(q.occurredAt.secsTo(event.occurredAt)) < kMergeSeconds) return false;
  }
  events_.append(event);
  while (events_.size() > kMaxQueued) events_.removeFirst();
  save();
  return true;
}

QList<ErrorEvent> ErrorQueue::front(int count) const { return events_.mid(0, count); }

void ErrorQueue::removeFront(int count) {
  if (count <= 0) return;
  events_.remove(0, qMin<qsizetype>(count, events_.size()));
  save();
}

void ErrorQueue::clear() {
  events_.clear();
  save();
}

// ---------------------------------------------------------------- previous run

QString lastErrorLine(const QString& logPath) {
  QFile f(logPath);
  if (!f.open(QIODevice::ReadOnly)) return {};
  if (f.size() > kLogTailBytes) f.seek(f.size() - kLogTailBytes);
  const QList<QByteArray> lines = f.readAll().split('\n');
  for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
    const QByteArray line = it->trimmed();
    for (const char* tag : {" [E] ", " [F] "}) {
      const qsizetype at = line.indexOf(tag);
      if (at > 0 && at <= 40) {  // after the timestamp
        return QString::fromUtf8(diag::redact(line.mid(at + 5))).trimmed().left(kMaxMessage);
      }
    }
  }
  return {};
}

QString RunMarker::fileName(const QString& dataDir) { return QDir(dataDir).filePath(QStringLiteral("running.marker")); }

bool RunMarker::exists(const QString& dataDir) { return !dataDir.isEmpty() && QFile::exists(fileName(dataDir)); }

RunMarker::RunMarker(const QString& dataDir) {
  if (dataDir.isEmpty()) return;
  QDir().mkpath(dataDir);
  path_ = fileName(dataDir);
  QFile f(path_);
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    f.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
  }
}

RunMarker::~RunMarker() { release(); }

void RunMarker::release() {
  if (path_.isEmpty()) return;
  QFile::remove(path_);
  path_.clear();
}

}  // namespace uzume::errors
