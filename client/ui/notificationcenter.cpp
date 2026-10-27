#include "notificationcenter.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QDateTime>
#include <QMediaDevices>
#include <QtMath>
#include <algorithm>

namespace uzume::ui {

NotificationCenter::NotificationCenter(PlayerSettings* settings, QObject* parent) : QObject(parent), settings_(settings) {
  timer_.setInterval(250);
  connect(&timer_, &QTimer::timeout, this, &NotificationCenter::tick);
}

qint64 NotificationCenter::now() const { return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch(); }

QVariantList NotificationCenter::entries() const {
  QVariantList out;
  for (int i = static_cast<int>(entries_.size()) - 1; i >= 0; --i) {
    const Entry& e = entries_[i];
    out.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(entries_.size()) - 1 - i},
                           {QStringLiteral("kind"), e.kind},
                           {QStringLiteral("text"), e.text},
                           {QStringLiteral("atMs"), e.atMs},
                           {QStringLiteral("actionLabel"), e.actionLabel},
                           {QStringLiteral("read"), e.read},
                           {QStringLiteral("mention"), e.mention}});
  }
  return out;
}

int NotificationCenter::unread() const {
  return static_cast<int>(std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) { return !e.read; }));
}

QVariantList NotificationCenter::toasts() const {
  QVariantList out;
  for (const Toast& t : toasts_) {
    if (t.shownAtMs == 0) continue;
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == t.id; });
    if (it == entries_.end()) continue;
    out.append(QVariantMap{{QStringLiteral("id"), t.id},
                           {QStringLiteral("kind"), it->kind},
                           {QStringLiteral("text"), it->text},
                           {QStringLiteral("actionLabel"), it->actionLabel}});
  }
  return out;
}

void NotificationCenter::setFullscreen(bool on) {
  if (fullscreen_ == on) return;
  fullscreen_ = on;
  emit settingsChanged();
}

QVariantMap NotificationCenter::modes() const {
  QVariantMap m;
  for (const char* k : {"invite", "session_started", "run_event", "update", "server_problem"}) {
    const QString kind = QLatin1String(k);
    m.insert(kind, settings_->notificationMode(kind));
  }
  return m;
}

bool NotificationCenter::quietFullscreen() const { return settings_->notifyQuietFullscreen(); }
void NotificationCenter::setQuietFullscreen(bool on) {
  if (settings_->notifyQuietFullscreen() == on) return;
  settings_->setNotifyQuietFullscreen(on);
  emit settingsChanged();
}
bool NotificationCenter::sound() const { return settings_->notifySound(); }
void NotificationCenter::setSound(bool on) {
  if (settings_->notifySound() == on) return;
  settings_->setNotifySound(on);
  emit settingsChanged();
}

void NotificationCenter::setMode(const QString& kind, const QString& mode) {
  if (!settings_->setNotificationMode(kind, mode)) return;
  emit settingsChanged();
}

void NotificationCenter::notify(const QString& kind, const QString& text, const QString& actionLabel, const QString& actionData, bool isMention) {
  if (text.trimmed().isEmpty()) return;
  Entry e;
  e.id = nextId_++;
  e.kind = kind;
  e.text = text;
  e.atMs = now();
  e.actionLabel = actionLabel;
  e.actionData = actionData;
  e.mention = isMention;
  entries_.append(e);
  while (entries_.size() > kMaxEntries) {
    const int dropped = entries_.takeFirst().id;
    toasts_.removeIf([&](const Toast& t) { return t.id == dropped; });
  }
  const bool wanted = isMention || (settings_->notificationMode(kind) == QLatin1String("toast") && !(fullscreen_ && settings_->notifyQuietFullscreen()));
  if (wanted) {
    toasts_.append({e.id, 0});
    promoteToasts();
  }
  emit entriesChanged();
  emit toastsChanged();
}

void NotificationCenter::promoteToasts() {
  int visible = static_cast<int>(std::count_if(toasts_.begin(), toasts_.end(), [](const Toast& t) { return t.shownAtMs != 0; }));
  bool shown = false;
  for (Toast& t : toasts_) {
    if (visible >= kMaxToasts) break;
    if (t.shownAtMs != 0) continue;
    t.shownAtMs = now();
    ++visible;
    shown = true;
  }
  if (shown && settings_->notifySound()) {
    if (soundHook_) soundHook_();
    else playTone();
  }
  rearm();
}

void NotificationCenter::rearm() {
  if (toasts_.isEmpty()) timer_.stop();
  else if (!timer_.isActive()) timer_.start();
}

void NotificationCenter::tick() {
  const qint64 t = now();
  const qsizetype before = toasts_.size();
  toasts_.removeIf([&](const Toast& x) { return x.shownAtMs != 0 && t - x.shownAtMs >= kToastMs; });
  if (toasts_.size() != before) {
    promoteToasts();
    emit toastsChanged();
  }
  rearm();
}

void NotificationCenter::dismissToast(int toastId) {
  if (toasts_.removeIf([&](const Toast& t) { return t.id == toastId; }) == 0) return;
  promoteToasts();
  emit toastsChanged();
}

void NotificationCenter::markAllRead() {
  bool any = false;
  for (Entry& e : entries_) {
    if (!e.read) {
      e.read = true;
      any = true;
    }
  }
  if (any) emit entriesChanged();
}

void NotificationCenter::run(const Entry& e) { emit actionRequested(e.kind, e.actionData); }

bool NotificationCenter::act(int index) {
  const int i = static_cast<int>(entries_.size()) - 1 - index;
  if (index < 0 || i < 0 || i >= entries_.size()) return false;
  const Entry e = entries_[i];
  if (!entries_[i].read) {
    entries_[i].read = true;
    emit entriesChanged();
  }
  dismissToast(e.id);
  run(e);
  return true;
}

bool NotificationCenter::actToast(int toastId) {
  const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == toastId; });
  if (it == entries_.end()) return false;
  return act(static_cast<int>(entries_.end() - it) - 1);
}

void NotificationCenter::reset() {
  if (entries_.isEmpty() && toasts_.isEmpty()) return;
  entries_.clear();
  toasts_.clear();
  timer_.stop();
  emit entriesChanged();
  emit toastsChanged();
}

// A short two-note chime (A5, E6) through the default output device; silent when there is none.
void NotificationCenter::playTone() {
  static constexpr int kRate = 44100;
  QAudioFormat fmt;
  fmt.setSampleRate(kRate);
  fmt.setChannelCount(1);
  fmt.setSampleFormat(QAudioFormat::Int16);
  const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
  if (dev.isNull() || !dev.isFormatSupported(fmt)) return;
  QByteArray pcm;
  const int n = kRate * 22 / 100;
  pcm.resize(n * 2);
  auto* out = reinterpret_cast<qint16*>(pcm.data());
  for (int i = 0; i < n; ++i) {
    const double f = i < n / 2 ? 880.0 : 1318.5;
    const double env = qMin(1.0, (n - i) / double(kRate / 25)) * qMin(1.0, i / 200.0);
    out[i] = static_cast<qint16>(qSin(2 * M_PI * f * i / kRate) * 0.18 * 32767 * env);
  }
  auto* buf = new QBuffer;
  buf->setData(pcm);
  buf->open(QIODevice::ReadOnly);
  auto* sink = new QAudioSink(dev, fmt);
  buf->setParent(sink);
  QObject::connect(sink, &QAudioSink::stateChanged, sink, [sink](QAudio::State s) {
    if (s == QAudio::IdleState || s == QAudio::StoppedState) sink->deleteLater();
  });
  sink->start(buf);
}

}  // namespace uzume::ui
