#include "core_log_filter.h"

namespace uzume::emu {

namespace {
// A core whose text keeps changing (counters, timestamps) produces endless "new" messages: at most this many distinct
// new messages are reported per second, the rest is counted and announced with the next reported one. Errors are exempt.
constexpr int kMaxNewPerSecond = 20;
}  // namespace

CoreLogFilter::CoreLogFilter(bool debugEnabled, qint64 repeatWindowMs, int maxKnown)
    : m_debug(debugEnabled), m_windowMs(repeatWindowMs), m_maxKnown(maxKnown) {}

QString CoreLogFilter::repeatKey(const QString& msg) {
  QString key;
  key.reserve(msg.size());
  bool inDigits = false;
  for (const QChar c : msg) {
    if (c.isDigit()) {
      if (!inDigits) key += QLatin1Char('#');
      inDigits = true;
    } else {
      key += c;
      inDigits = false;
    }
  }
  return key;
}

bool CoreLogFilter::accept(CoreLogLevel level, QString& msg, qint64 nowMs) {
  if (!wantsLevel(level)) return false;
  const QString key = repeatKey(msg);
  std::lock_guard<std::mutex> lock(m_mutex);
  if (auto it = m_seen.find(key); it != m_seen.end()) {
    Entry& e = it.value();
    if (nowMs - e.lastReportedMs < m_windowMs) {
      ++e.suppressed;
      return false;
    }
    if (e.suppressed > 0)
      msg += QStringLiteral(" (repeated %1 times in the last %2 s)").arg(e.suppressed).arg(m_windowMs / 1000);
    e.suppressed = 0;
    e.lastReportedMs = nowMs;
    return true;
  }
  if (nowMs - m_newWindowStartMs >= 1000) {
    m_newWindowStartMs = nowMs;
    m_newInWindow = 0;
  }
  if (level != CoreLogLevel::Error) {
    if (m_newInWindow >= kMaxNewPerSecond) {
      ++m_suppressedNew;
      return false;
    }
    ++m_newInWindow;
  }
  if (m_seen.size() >= m_maxKnown && !m_order.isEmpty()) m_seen.remove(m_order.takeFirst());
  m_seen.insert(key, Entry{nowMs, 0});
  m_order.append(key);
  if (m_suppressedNew > 0) {
    msg += QStringLiteral(" (%1 other core messages suppressed)").arg(m_suppressedNew);
    m_suppressedNew = 0;
  }
  return true;
}

}  // namespace uzume::emu
