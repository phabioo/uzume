#pragma once
// Filter between the libretro log callback and the app log (issue 435): drops core debug lines unless opted in
// (UZUME_CORE_DEBUG_LOG=1) and collapses identical repeated messages. Thread-safe: cores may log from any thread.

#include <QHash>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <mutex>

namespace uzume::emu {

enum class CoreLogLevel { Debug, Info, Warn, Error };

class CoreLogFilter {
 public:
  // debugEnabled: keep debug lines. repeatWindowMs: an identical message is reported again (with the count of the
  // suppressed repeats) at most this often. maxKnown: size of the table of known messages (oldest forgotten first).
  explicit CoreLogFilter(bool debugEnabled = false, qint64 repeatWindowMs = 60000, int maxKnown = 256);

  // Cheap check before the message is even formatted: false = every line of this level is dropped.
  bool wantsLevel(CoreLogLevel level) const { return level != CoreLogLevel::Debug || m_debug; }
  bool debugEnabled() const { return m_debug; }

  // Decides about one formatted message. true = report it (msg may get a "repeated N times" suffix). nowMs is
  // a monotonic or wall clock in milliseconds.
  bool accept(CoreLogLevel level, QString& msg, qint64 nowMs);

  // The key under which repeats are counted: every run of digits becomes '#', so lines that differ only in a
  // timestamp, address or counter (Azahar prefixes each line with its uptime) count as the same message.
  static QString repeatKey(const QString& msg);

 private:
  struct Entry {
    qint64 lastReportedMs = 0;
    int suppressed = 0;  // identical repeats since the last report
  };
  const bool m_debug;
  const qint64 m_windowMs;
  const int m_maxKnown;
  std::mutex m_mutex;
  QHash<QString, Entry> m_seen;
  QList<QString> m_order;  // insertion order, oldest first
  qint64 m_newWindowStartMs = 0;
  int m_newInWindow = 0;
  int m_suppressedNew = 0;
};

}  // namespace uzume::emu
