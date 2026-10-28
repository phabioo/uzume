#include "errorsender.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace uzume {

ErrorSender::ErrorSender(HubConnection* connection, errors::ErrorQueue* queue, QObject* parent)
    : QObject(parent), conn_(connection), queue_(queue) {
  retry_.setSingleShot(true);
  connect(&retry_, &QTimer::timeout, this, &ErrorSender::flush);
  if (conn_) {
    connect(conn_, &HubConnection::stateChanged, this, [this](HubConnection::State s) {
      if (s == HubConnection::State::Connected) flush();
    });
  }
}

void ErrorSender::setEnabled(bool on) {
  if (enabled_ == on) return;
  enabled_ = on;
  if (on) flush();
}

bool ErrorSender::supported() const {
  return conn_ && conn_->state() == HubConnection::State::Connected && conn_->hubHasFeature(QString::fromLatin1(kFeature));
}

void ErrorSender::flush() {
  if (!enabled_ || reply_ || queue_ == nullptr || queue_->isEmpty() || !supported()) return;
  const QList<errors::ErrorEvent> batch = queue_->front(errors::kBatchSize);
  QJsonArray events;
  for (const errors::ErrorEvent& e : batch) events.append(e.toJson());
  QJsonObject body;
  body.insert(QStringLiteral("events"), events);
  QNetworkReply* reply = conn_->authorizedSend("POST", QStringLiteral("/reports/errors"),
                                               QJsonDocument(body).toJson(QJsonDocument::Compact), {}, "application/json");
  if (reply == nullptr) return;  // not connected after all: the next connect sends it
  reply_ = reply;
  const int count = static_cast<int>(batch.size());
  connect(reply, &QNetworkReply::finished, this, [this, reply, count]() { onFinished(reply, count); });
}

void ErrorSender::scheduleRetry() {
  if (!retry_.isActive()) retry_.start(retryMs_);
}

void ErrorSender::onFinished(QNetworkReply* reply, int count) {
  const HttpResult r = HubHttp::resultOf(reply);
  reply->deleteLater();
  if (reply_ != reply) return;
  reply_.clear();
  if (r.status == 202 || (r.ok() && r.status == 200)) {
    queue_->removeFront(count);
    const QJsonObject j = r.json();
    emit batchSent(j.value(QStringLiteral("accepted")).toInt(count), j.value(QStringLiteral("dropped")).toInt(0));
    flush();
    return;
  }
  if (r.status == 400) {  // the shape was refused: this batch can never go through
    queue_->removeFront(count);
    emit batchFailed(QStringLiteral("rejected"));
    flush();
    return;
  }
  if (r.status == 401 && conn_) {
    conn_->noteUnauthorized();
    emit batchFailed(QStringLiteral("auth"));
  } else if (r.status == 429) {
    emit batchFailed(QStringLiteral("rate_limited"));
  } else if (r.status >= 500) {
    emit batchFailed(QStringLiteral("server"));
  } else {
    emit batchFailed(QStringLiteral("network"));
  }
  scheduleRetry();
}

}  // namespace uzume
