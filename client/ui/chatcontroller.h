#pragma once
// ChatController (ADR 0033 D4): the chat of the scope the running game belongs to (the TogetherController's scope: run > my shared Session >
// the Session I watch). Per scope it keeps the messages in seq order (dedupe by seq and client_id), the messages I sent that the Hub has
// not echoed yet (pending, failed after kPendingMs, resent with the same client_id), the unread count of the closed Chat tab and, for a
// run, the run events as dimmed lines between the messages. History comes from the Hub's REST lists when a scope appears and after a
// reconnect; live messages come from `chat_message`. A message that mentions me while the Chat tab is not visible raises `mentioned`.
// Everything is in memory; the Hub is the store.

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <limits>

#include "chatapi.h"
#include "hubsocket.h"
#include "togethercontroller.h"

namespace uzume::ui {

class ChatController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  Q_PROPERTY(int maxText READ maxText CONSTANT)
  Q_PROPERTY(bool available READ available NOTIFY scopeChanged)  // a scope exists, the Hub offers together_v1
  Q_PROPERTY(QString scopeKind READ scopeKind NOTIFY scopeChanged)
  Q_PROPERTY(QString scopeId READ scopeId NOTIFY scopeChanged)
  Q_PROPERTY(QString header READ header NOTIFY rowsChanged)  // "RUN CHAT · EMBER ROUTE · 4 PEOPLE" | "SESSION CHAT · 2 PEOPLE"
  // Newest first (the list is drawn bottom to top): [{type: "msg"|"event"|"day", ...}], see rows()
  Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
  Q_PROPERTY(QVariantList people READ people NOTIFY peopleChanged)  // the people of the scope (also for the People tab)
  Q_PROPERTY(int unread READ unread NOTIFY unreadChanged)
  Q_PROPERTY(bool panelVisible READ panelVisible WRITE setPanelVisible NOTIFY visibleChanged)
  Q_PROPERTY(bool loading READ loading NOTIFY rowsChanged)
  Q_PROPERTY(bool canSend READ canSend NOTIFY rowsChanged)
  Q_PROPERTY(QVariantList quickReplies READ quickReplies CONSTANT)  // [{label, text, key}]

 public:
  static constexpr int kMaxText = 500;
  static constexpr int kHistoryRun = 100;  // messages of a run kept from the history of a scope
  static constexpr int kMaxKept = 400;     // per scope in memory
  static constexpr int kMaxRows = 300;
  static constexpr int kMaxScopes = 12;
  static constexpr qint64 kPendingMs = 15000;
  static constexpr int kPageSize = 500;

  struct EventLine {
    qint64 atMs = 0;
    qint64 seq = 0;  // run event seq, for dedupe
    QString text;
  };
  using Fetch = std::function<void(const QString& scope, const QString& id, qint64 afterSeq, int limit, std::function<void(const ChatPage&)>)>;

  ChatController(TogetherController* together, HubSocket* socket, QObject* parent = nullptr);

  // Providers set by the PlayerController (tests set their own).
  void setSelf(std::function<QString()> userId, std::function<QString()> displayName) {
    selfFn_ = std::move(userId);
    selfNameFn_ = std::move(displayName);
  }
  // The scope of the chat; default: the TogetherController's. The PlayerController lets a run that is still starting count.
  void setScopeProvider(std::function<TogetherController::Scope()> f) { scopeFn_ = std::move(f); }
  void setNameResolver(std::function<QString(const QString&)> f) { nameFn_ = std::move(f); }
  // Run name for the header, and the people of a scope ([{userId, name, role, me, color, connection, tone}], shown by the People tab).
  void setScopeInfo(std::function<QString(const QString&, const QString&)> title, std::function<QVariantList(const QString&, const QString&)> people) {
    titleFn_ = std::move(title);
    peopleFn_ = std::move(people);
  }
  void setRunEvents(std::function<QList<EventLine>(const QString&)> f) { runEventsFn_ = std::move(f); }
  void setFetch(Fetch f) { fetch_ = std::move(f); }
  void setSender(std::function<bool(const QString&, const QString&, const QString&, const QString&)> f) { sender_ = std::move(f); }
  void setClock(std::function<qint64()> f) { clock_ = std::move(f); }

  int maxText() const { return kMaxText; }
  bool available() const;
  QString scopeKind() const { return kind_; }
  QString scopeId() const { return id_; }
  QString header() const;
  QVariantList rows() const;
  QVariantList people() const { return peopleFn_ && !kind_.isEmpty() ? peopleFn_(kind_, id_) : QVariantList(); }
  int unread() const;
  bool panelVisible() const { return visible_; }
  void setPanelVisible(bool on);
  bool loading() const;
  bool canSend() const;
  QVariantList quickReplies() const;

  // Sends `text` (1..kMaxText characters after trimming) to the current scope; false = nothing was sent (empty, too long, no scope, no link).
  Q_INVOKABLE bool send(const QString& text);
  Q_INVOKABLE bool sendQuick(int index);  // 0..2
  Q_INVOKABLE void retry(const QString& clientId);
  // Text of the last messages of the current scope, oldest first: [{name, text, time}] (MCP chat_history)
  QVariantList history(int limit) const;
  // Numbers for tests and the MCP state.
  Q_INVOKABLE static int charCount(const QString& text);  // characters as the Hub counts them (code points)
  Q_INVOKABLE int messageCount() const;
  Q_INVOKABLE bool isPending(const QString& clientId) const;

  // Shortcodes :gg: :wave: :lol: :hype: :sad: :ok: as unicode; everything else is left as typed.
  Q_INVOKABLE static QString renderEmotes(const QString& text);
  // The display form of a message: HTML-escaped, emotes replaced, the @mention of me in bold when `mentionsMe`.
  static QString renderHtml(const QString& text, const QString& myName, bool mentionsMe);

  // Run events (existing run_event stream) of a run; shown inline while that run is the scope.
  void noteRunEvent(const QString& runId);  // an event of that run arrived: the rows are rebuilt from the provider
  void refresh();  // the scope or the feature may have changed
  void reset();    // another server or person: everything goes away
  void tick();     // 1 s timer; tests call it

 signals:
  void scopeChanged();
  void rowsChanged();
  void peopleChanged();  // the Hub's people lists or the links changed (not on every message)
  void unreadChanged();
  void visibleChanged();
  // A message that names me arrived while its chat was not on screen.
  void mentioned(const QString& scopeKind, const QString& scopeId, const QString& fromName, const QString& text);

 private:
  struct Msg {
    qint64 seq = 0;  // 0 = pending
    QString clientId;
    QString userId;
    QString text;
    qint64 atMs = 0;
    QStringList mentions;
    bool pending = false;
    bool failed = false;
    qint64 sentLocalMs = 0;
  };
  struct Chat {
    QString kind, id;
    QList<Msg> msgs;  // confirmed in seq order, then pending
    qint64 lastSeq = 0;
    qint64 liveAfter = -1;  // while a catch-up reload runs: messages above this seq arrived while I was away and count as new
    int unread = 0;
    bool loaded = false;
    bool loading = false;
  };
  static QString keyOf(const QString& kind, const QString& id) { return kind + QLatin1Char(':') + id; }
  QString self() const { return selfFn_ ? selfFn_() : QString(); }
  QString nameOf(const QString& userId) const;
  qint64 now() const;
  Chat& chatFor(const QString& kind, const QString& id);
  Chat* current();
  const Chat* current() const;
  void onMessage(const ChatMessage& m, bool live);
  void load(const QString& kind, const QString& id, bool initial);
  void loadPage(const QString& key, qint64 afterSeq, bool initial, int page);
  void sortMsgs(Chat& c);
  void trim(Chat& c);
  void onSocketState(HubSocket::State s);
  void markSeen(Chat& c);
  void emitChanged(const Chat& c);

  TogetherController* together_;
  HubSocket* socket_;
  std::function<TogetherController::Scope()> scopeFn_;
  std::function<QString()> selfFn_, selfNameFn_;
  std::function<QString(const QString&)> nameFn_;
  std::function<QString(const QString&, const QString&)> titleFn_;
  std::function<QVariantList(const QString&, const QString&)> peopleFn_;
  std::function<QList<EventLine>(const QString&)> runEventsFn_;
  Fetch fetch_;
  std::function<bool(const QString&, const QString&, const QString&, const QString&)> sender_;
  std::function<qint64()> clock_;
  QHash<QString, Chat> chats_;
  QStringList order_;  // keys, oldest first (for the kMaxScopes limit)
  QString kind_, id_;
  bool visible_ = false;
  int bulk_ = 0;  // > 0 while a history page is merged: one change signal at the end
  QTimer timer_;
};

}  // namespace uzume::ui
