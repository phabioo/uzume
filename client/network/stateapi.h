#pragma once
// Server API for save states (protocol/openapi/uzume.yaml, tag `saves`, handshake feature `states_v1`, 0.20 package 4):
//   GET /games/{id}/states[?slot=], GET/PUT/DELETE /games/{id}/states/{slot}/{n}, GET .../preview.
// A state is the core's serialization of a running game; the server never reads it. Thin async client over HubConnection like
// SaveApi: the callback runs on the event loop (never synchronously) and is dropped if the StateApi is destroyed.
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

#include "hubconnection.h"
#include "statestore.h"

namespace uzume {

inline constexpr const char* kStatesFeature = "states_v1";

struct StateApiResult {
  enum class Kind {
    Ok,
    NotFound,     // 404
    Offline,      // no connection, 5xx, 429, token problem: retry later
    QuotaFull,    // 413 state_quota_exceeded
    TooLarge,     // 413 payload_too_large
    Rejected,     // other 4xx
    BadResponse   // malformed or verification failed
  };
  Kind kind = Kind::Offline;
  int status = 0;
  QString errorCode;
  QString errorMessage;
  QList<StateRecord> states;   // list: on_server = true
  qint64 usedBytes = 0;        // list: everything the person stores as states on this server
  qint64 quotaBytes = 0;
  std::optional<StateRecord> state;  // put
  QByteArray content;          // get (verified against the ETag) / preview
  QString coreId;              // get: X-Uzume-Core-Id
  QString coreVersion;         // get: X-Uzume-Core-Version
  bool ok() const { return kind == Kind::Ok; }
};

std::optional<StateRecord> parseServerState(const QJsonObject& o);

class StateApi : public QObject {
  Q_OBJECT
 public:
  using Callback = std::function<void(const StateApiResult&)>;
  explicit StateApi(HubConnection* connection, QObject* parent = nullptr);

  bool connected() const { return conn_ && conn_->state() == HubConnection::State::Connected; }
  // Connected to a server that does not know save states: they stay on this PC and nothing is retried.
  bool featureMissing() const { return connected() && !conn_->hubHasFeature(QLatin1String(kStatesFeature)); }
  bool available() const { return conn_ && conn_->state() == HubConnection::State::Connected && conn_->hubHasFeature(QLatin1String(kStatesFeature)); }

  void list(const QString& gameId, const QString& saveSlot, Callback cb);  // saveSlot empty = every slot of the game
  void put(const StateRecord& meta, const QByteArray& state, const QByteArray& previewPng, Callback cb);
  void get(const QString& gameId, const QString& saveSlot, int n, Callback cb);
  void preview(const QString& gameId, const QString& saveSlot, int n, Callback cb);
  void remove(const QString& gameId, const QString& saveSlot, int n, Callback cb);

  // The multipart/form-data body of put (meta, state, preview in this order); exposed for tests.
  static QByteArray multipartBody(const QByteArray& boundary, const QString& coreId, const QString& coreVersion, const QByteArray& state,
                                  const QByteArray& previewPng);

 private:
  enum class Expect { List, Put, Content, Preview, None };
  void run(QNetworkReply* reply, Expect expect, Callback cb);
  void immediate(StateApiResult::Kind kind, const QString& code, Callback cb);
  static QString statePath(const QString& gameId, const QString& saveSlot, int n);

  QPointer<HubConnection> conn_;
};

}  // namespace uzume
