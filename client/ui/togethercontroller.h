#pragma once
// TogetherController (ADR 0033 D3): ready check and shared pause of the scope the running game belongs to. Scope = the run the game was
// started for, else my shared Session (owner), else the Session I watch (viewer). Holds the open check (members with names and colours,
// my answer, seconds left) and the shared pause state; a shared pause pauses my local game only when the scope is hosted by my game
// (run member, Session owner) and resumes it only if the pause came from here. Everything is derived from Hub messages, nothing is stored.

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <functional>

#include "hubsocket.h"

namespace uzume::ui {

class TogetherController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  Q_PROPERTY(bool available READ available NOTIFY scopeChanged)    // a scope exists and the Hub offers together_v1
  Q_PROPERTY(QString scopeKind READ scopeKind NOTIFY scopeChanged)  // "run" | "session" | ""
  Q_PROPERTY(QString scopeId READ scopeId NOTIFY scopeChanged)
  // Ready check
  Q_PROPERTY(bool checkOpen READ checkOpen NOTIFY checkChanged)
  Q_PROPERTY(QString checkTitle READ checkTitle NOTIFY checkChanged)         // "Ready check · Lena"
  Q_PROPERTY(QVariantList checkMembers READ checkMembers NOTIFY checkChanged)  // [{userId, name, color, state, stateText, me}]
  Q_PROPERTY(QString myState READ myState NOTIFY checkChanged)                // pending | ready | not_ready | ""
  Q_PROPERTY(bool iStarted READ iStarted NOTIFY checkChanged)
  Q_PROPERTY(bool awaitsAnswer READ awaitsAnswer NOTIFY checkChanged)  // open and I have not answered
  Q_PROPERTY(int secondsLeft READ secondsLeft NOTIFY secondsLeftChanged)
  // Short result line ("Everyone is ready"), cleared after kNoticeMs
  Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
  // Shared pause
  Q_PROPERTY(bool paused READ paused NOTIFY pauseChanged)
  Q_PROPERTY(QString pausedText READ pausedText NOTIFY pauseChanged)  // "Lena paused for everyone"
  Q_PROPERTY(bool holdActive READ holdActive NOTIFY pauseChanged)     // my game is paused by the shared pause

 public:
  static constexpr int kNoticeMs = 3000;
  static constexpr int kCheckMs = 30000;

  struct Scope {
    QString kind;  // "run" | "session"; empty = none
    QString id;
    bool hostsGame = false;  // my local game is the one paused by a shared pause (run member, Session owner)
  };
  // What decides the scope; the PlayerController fills it from the run list, the game and the Sessions.
  struct ScopeInputs {
    QString boardRunId;  // joined, active run open on the Runs screen
    QString gameRunId;   // active run of the running game ("" = not a run game)
    bool gameActive = false;
    QString ownSessionId;  // my shared Session
    QStringList watched;   // Sessions I watch
    QString focus;         // audio focus surface: a watched Session id or "local"
  };
  // Board of a run > run of the game > my shared Session > the Session I watch (the focused one first).
  static Scope pickScope(const ScopeInputs& in);

  struct GameHooks {
    std::function<bool()> isPaused;
    std::function<void()> pause;
    std::function<void()> resume;
  };

  explicit TogetherController(HubSocket* socket, QObject* parent = nullptr);

  // Providers set by the PlayerController (tests set their own).
  void setScopeProvider(std::function<Scope()> f) { scopeFn_ = std::move(f); }
  void setNameResolver(std::function<QString(const QString&)> f) { nameFn_ = std::move(f); }  // user id -> display name
  void setSelf(std::function<QString()> userId) { selfFn_ = std::move(userId); }
  void setGameHooks(GameHooks h) { game_ = std::move(h); }
  void setClock(std::function<qint64()> f) { clock_ = std::move(f); }
  void setFeatureCheck(std::function<bool()> f) { featureFn_ = std::move(f); }  // default: the Hub offers together_v1

  bool available() const { return !scope_.kind.isEmpty() && featureOn(); }
  bool featureAvailable() const { return featureOn(); }  // the Hub offers together_v1
  QString scopeKind() const { return scope_.kind; }
  QString scopeId() const { return scope_.id; }
  bool checkOpen() const { return check_.open; }
  QString checkTitle() const;
  QVariantList checkMembers() const;
  QString myState() const;
  bool iStarted() const { return check_.open && check_.by == self(); }
  int secondsLeft() const;
  QString notice() const { return notice_; }
  bool paused() const { return paused_; }
  QString pausedText() const;
  bool holdActive() const { return hold_; }
  // Who the Session's viewers see as the one who paused the host's game: the requester while a shared pause holds it, else empty (the host).
  QString hostPauseName() const { return hold_ && !pausedBy_.isEmpty() ? nameOf(pausedBy_) : QString(); }
  // True while the card waits for my answer by key or pad (open and I have not answered).
  bool awaitsAnswer() const { return check_.open && myState() == QLatin1String("pending"); }

  Q_INVOKABLE void startCheck();
  Q_INVOKABLE void answer(bool ready);
  Q_INVOKABLE void cancelCheck();
  Q_INVOKABLE void setPaused(bool paused);
  Q_INVOKABLE void togglePause() { setPaused(!paused_); }

  // The scope may have changed (game, Session, run, Hub link): recomputes it and drops what belongs to a scope that is gone.
  void refresh();
  void holdAndPause();
  // The local game was paused or resumed by someone else than the shared pause.
  void noteLocalPauseChanged();
  void tick();  // 250 ms timer; tests call it

 signals:
  void scopeChanged();
  void checkChanged();
  void secondsLeftChanged();
  void noticeChanged();
  void pauseChanged();

 private:
  struct Member {
    QString userId;
    QString state;
  };
  struct Check {
    bool open = false;
    QString checkId;
    QString by;
    qint64 endAtMs = 0;
    QList<Member> members;
  };
  QString self() const { return selfFn_ ? selfFn_() : QString(); }
  QString nameOf(const QString& userId) const;
  bool featureOn() const;
  qint64 now() const;
  bool inScope(const QString& kind, const QString& id) const { return !scope_.kind.isEmpty() && kind == scope_.kind && id == scope_.id; }
  void onReadyCheck(const ReadyCheckMessage& m);
  void onPauseState(const PauseStateMessage& m);
  void onSocketState(HubSocket::State s);
  void showNotice(const QString& text);
  void clearCheck();
  void releaseHold(bool resume);

  HubSocket* socket_;
  std::function<Scope()> scopeFn_;
  std::function<QString(const QString&)> nameFn_;
  std::function<QString()> selfFn_;
  std::function<bool()> featureFn_;
  std::function<qint64()> clock_;
  GameHooks game_;
  Scope scope_;
  Check check_;
  QString notice_;
  qint64 noticeUntilMs_ = 0;
  bool paused_ = false;
  QString pausedBy_;  // user id, may be empty
  bool hold_ = false;
  int lastSeconds_ = 0;
  QTimer timer_;
};

}  // namespace uzume::ui
