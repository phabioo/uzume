#pragma once
// ErrorSender (ADR 0035 D1): sends the queued error events to the connected server (POST /reports/errors, handshake feature
// reports_v2) in batches of at most 20. Only while enabled (the person's opt-in) and connected to a server that advertises
// reports_v2; events stay in the queue otherwise and go out when the connection is back. A batch leaves the queue when the server
// answered 202 (accepted or dropped by its per-device limit) or refused its shape (400, retrying would never help). Network
// errors, 401, 429 and 5xx keep it; the next try comes with the next event, the next connect or after a minute.

#include <QObject>
#include <QPointer>
#include <QTimer>

#include "errorevents.h"
#include "hubconnection.h"

namespace uzume {

class ErrorSender : public QObject {
  Q_OBJECT
 public:
  static constexpr const char* kFeature = "reports_v2";

  ErrorSender(HubConnection* connection, errors::ErrorQueue* queue, QObject* parent = nullptr);

  bool enabled() const { return enabled_; }
  void setEnabled(bool on);  // turning it on sends what is queued
  bool supported() const;    // connected and the server advertises reports_v2
  bool sending() const { return reply_ != nullptr; }
  void setRetryMs(int ms) { retryMs_ = ms; }  // tests

  // Sends the next batch if enabled, supported and not busy; continues until the queue is empty.
  void flush();

 signals:
  void batchSent(int accepted, int dropped);
  void batchFailed(const QString& code);  // network | auth | rate_limited | rejected | server

 private:
  void onFinished(QNetworkReply* reply, int count);
  void scheduleRetry();

  QPointer<HubConnection> conn_;
  errors::ErrorQueue* queue_;
  QPointer<QNetworkReply> reply_;
  QTimer retry_;
  int retryMs_ = 60000;
  bool enabled_ = false;
};

}  // namespace uzume
