#pragma once
// Chat history over REST (ADR 0033 D4, together_v1): GET /runs/{id}/chat?after_seq=&limit= (stored, joined participants) and
// GET /sessions/{id}/chat (the in-memory ring of 200, owner and viewers).

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

#include "chattypes.h"
#include "hubconnection.h"

namespace uzume {

struct ChatPage {
  bool ok = false;
  QList<ChatMessage> messages;  // oldest first
  qint64 nextAfterSeq = 0;      // > 0: more messages follow (runs only)
};

class ChatApi : public QObject {
  Q_OBJECT
 public:
  using Callback = std::function<void(const ChatPage&)>;
  explicit ChatApi(HubConnection* connection, QObject* parent = nullptr) : QObject(parent), conn_(connection) {}

  // scope "run" | "session". The callback runs on the event loop and is dropped if the ChatApi is destroyed.
  void fetch(const QString& scope, const QString& id, qint64 afterSeq, int limit, Callback cb);

 private:
  QPointer<HubConnection> conn_;
};

}  // namespace uzume
