#include "chatapi.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QTimer>
#include <QUrl>

#include "hubhttp.h"

namespace uzume {

void ChatApi::fetch(const QString& scope, const QString& id, qint64 afterSeq, int limit, Callback cb) {
  QNetworkReply* reply = nullptr;
  if (conn_ && !id.isEmpty()) {
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(id));
    const QString path = scope == QLatin1String("run")
                             ? QStringLiteral("/runs/%1/chat?after_seq=%2&limit=%3").arg(enc).arg(afterSeq).arg(limit)
                             : QStringLiteral("/sessions/%1/chat").arg(enc);
    reply = conn_->authorizedGet(path);
  }
  if (reply == nullptr) {
    QTimer::singleShot(0, this, [cb = std::move(cb)]() { cb(ChatPage{}); });
    return;
  }
  connect(reply, &QNetworkReply::finished, this, [this, reply, cb = std::move(cb)]() {
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    ChatPage page;
    if (r.status == 401 && conn_) conn_->noteUnauthorized();
    if (r.ok()) {
      const QJsonObject o = r.json();
      page.ok = true;
      for (const QJsonValue& v : o.value(QStringLiteral("messages")).toArray()) page.messages.append(parseChatMessage(v.toObject()));
      page.nextAfterSeq = static_cast<qint64>(o.value(QStringLiteral("next_after_seq")).toDouble());
    }
    cb(page);
  });
}

}  // namespace uzume
