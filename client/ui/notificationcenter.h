#pragma once
// NotificationCenter (ADR 0033 D2): the app-side notifications of one server, in memory only (last kMaxEntries). Producers call
// notify(): the PlayerController derives them from existing WSS events (Session invites and starts, run invites and run status events,
// app update, server problems for admins) and the chat package will use it for @mentions. Each entry has a text, a time, a kind, an
// action label (WATCH, OPEN RUN, WHAT'S NEW, OPEN STATUS) and an opaque `actionData` that the PlayerController turns into a
// navigation or action when act() is called. Per kind the settings of this PC decide whether an entry also shows as a toast or only
// waits in the bell; "Quiet in fullscreen" lets only mentions toast while the game is fullscreen. At most kMaxToasts toasts are
// visible, each for kToastMs; the others wait their turn.

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QList>
#include <functional>
#include <QtQml/qqmlregistration.h>

#include "playersettings.h"

namespace uzume::ui {

class NotificationCenter : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  // Newest first: [{index, kind, text, atMs, time, actionLabel, read, mention}]
  Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
  Q_PROPERTY(int unread READ unread NOTIFY entriesChanged)
  // Toasts that are visible now: [{id, kind, text, actionLabel}], at most kMaxToasts
  Q_PROPERTY(QVariantList toasts READ toasts NOTIFY toastsChanged)
  Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY settingsChanged)
  // Settings of this PC: {kind: "toast" | "list"}, quiet in fullscreen, sound for toasts
  Q_PROPERTY(QVariantMap modes READ modes NOTIFY settingsChanged)
  Q_PROPERTY(bool quietFullscreen READ quietFullscreen WRITE setQuietFullscreen NOTIFY settingsChanged)
  Q_PROPERTY(bool sound READ sound WRITE setSound NOTIFY settingsChanged)

 public:
  static constexpr int kMaxEntries = 50;
  static constexpr int kMaxToasts = 3;
  static constexpr qint64 kToastMs = 6000;

  explicit NotificationCenter(PlayerSettings* settings, QObject* parent = nullptr);

  QVariantList entries() const;
  int unread() const;
  QVariantList toasts() const;
  bool fullscreen() const { return fullscreen_; }
  void setFullscreen(bool on);
  QVariantMap modes() const;
  bool quietFullscreen() const;
  void setQuietFullscreen(bool on);
  bool sound() const;
  void setSound(bool on);

  // Producers. `kind` is one of the kinds of Settings > Notifications (invite, session_started, run_event, update, server_problem)
  // or another one (chat); `actionData` is opaque here. A mention always toasts, also in fullscreen.
  Q_INVOKABLE void notify(const QString& kind, const QString& text, const QString& actionLabel = {}, const QString& actionData = {}, bool isMention = false);
  Q_INVOKABLE void markAllRead();
  // Runs the action of entry `index` (as in `entries`): marks it read and emits actionRequested. False: no such entry.
  Q_INVOKABLE bool act(int index);
  Q_INVOKABLE bool actToast(int toastId);
  Q_INVOKABLE void dismissToast(int toastId);
  Q_INVOKABLE void setMode(const QString& kind, const QString& mode);
  Q_INVOKABLE void reset();  // another server: the list and the toasts go away

  // Tests: clock in ms, the tone, and the toast timer step (the timer calls it every 250 ms).
  void setClock(std::function<qint64()> clock) { clock_ = std::move(clock); }
  void setSoundHook(std::function<void()> hook) { soundHook_ = std::move(hook); }
  void tick();

 signals:
  void entriesChanged();
  void toastsChanged();
  void settingsChanged();
  void actionRequested(const QString& kind, const QString& actionData);

 private:
  struct Entry {
    int id = 0;
    QString kind;
    QString text;
    qint64 atMs = 0;
    QString actionLabel;
    QString actionData;
    bool read = false;
    bool mention = false;
  };
  struct Toast {
    int id = 0;  // the entry id
    qint64 shownAtMs = 0;  // 0 = waiting for a free place
  };
  qint64 now() const;
  void promoteToasts();
  void rearm();
  void run(const Entry& e);
  static void playTone();

  PlayerSettings* settings_;
  QList<Entry> entries_;  // oldest first
  QList<Toast> toasts_;
  int nextId_ = 1;
  bool fullscreen_ = false;
  QTimer timer_;
  std::function<qint64()> clock_;
  std::function<void()> soundHook_;
};

}  // namespace uzume::ui
