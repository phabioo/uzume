#pragma once
// together_v1 chat (ADR 0033 D4): one message as the Hub sends it (WSS `chat_message` and the REST lists share the shape).

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace uzume {

struct ChatMessage {
  QString scope;  // "run" | "session" (empty in REST lists: the caller knows it)
  QString id;     // run or Session id (empty in REST lists)
  qint64 seq = 0;
  QString userId;
  QString text;
  QString sentAt;  // RFC 3339
  QStringList mentions;  // user ids
  QString clientId;
};

inline ChatMessage parseChatMessage(const QJsonObject& p) {
  ChatMessage m;
  m.scope = p.value(QStringLiteral("scope")).toString();
  m.id = p.value(QStringLiteral("id")).toString();
  m.seq = static_cast<qint64>(p.value(QStringLiteral("seq")).toDouble());
  m.userId = p.value(QStringLiteral("user_id")).toString();
  m.text = p.value(QStringLiteral("text")).toString();
  m.sentAt = p.value(QStringLiteral("sent_at")).toString();
  m.clientId = p.value(QStringLiteral("client_id")).toString();
  for (const QJsonValue& v : p.value(QStringLiteral("mentions")).toArray()) m.mentions.append(v.toString());
  return m;
}

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::ChatMessage)
