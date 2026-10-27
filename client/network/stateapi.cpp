#include "stateapi.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace uzume {

std::optional<StateRecord> parseServerState(const QJsonObject& o) {
  StateRecord r;
  r.gameId = o.value(QStringLiteral("game_id")).toString();
  r.saveSlot = o.value(QStringLiteral("slot")).toString();
  r.n = o.value(QStringLiteral("n")).toInt(-1);
  r.coreId = o.value(QStringLiteral("core_id")).toString();
  r.coreVersion = o.value(QStringLiteral("core_version")).toString();
  r.sizeBytes = o.value(QStringLiteral("size_bytes")).toVariant().toLongLong();
  r.sha256 = o.value(QStringLiteral("sha256")).toString();
  r.createdAt = o.value(QStringLiteral("created_at")).toString();
  r.deviceName = o.value(QStringLiteral("device_name")).toString();
  r.hasPreview = o.value(QStringLiteral("has_preview")).toBool();
  r.onServer = true;
  if (r.gameId.isEmpty() || r.saveSlot.isEmpty() || r.n < 0 || r.n > 10 || r.sha256.size() != 64) return std::nullopt;
  return r;
}

StateApi::StateApi(HubConnection* connection, QObject* parent) : QObject(parent), conn_(connection) {}

QString StateApi::statePath(const QString& gameId, const QString& saveSlot, int n) {
  return QStringLiteral("/games/%1/states/%2/%3")
      .arg(QString::fromLatin1(QUrl::toPercentEncoding(gameId)), QString::fromLatin1(QUrl::toPercentEncoding(saveSlot))).arg(n);
}

void StateApi::immediate(StateApiResult::Kind kind, const QString& code, Callback cb) {
  QTimer::singleShot(0, this, [kind, code, cb = std::move(cb)]() {
    StateApiResult r;
    r.kind = kind;
    r.errorCode = code;
    cb(r);
  });
}

void StateApi::run(QNetworkReply* reply, Expect expect, Callback cb) {
  connect(reply, &QNetworkReply::finished, this, [this, reply, expect, cb = std::move(cb)]() {
    StateApiResult res;
    const QByteArray etag = reply->rawHeader("ETag");
    const QString coreId = QString::fromLatin1(reply->rawHeader("X-Uzume-Core-Id"));
    const QString coreVersion = QString::fromLatin1(reply->rawHeader("X-Uzume-Core-Version"));
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    res.status = r.status;
    res.errorCode = r.apiErrorCode;
    res.errorMessage = r.apiErrorMessage;
    using K = StateApiResult::Kind;
    if (r.networkError || r.certMismatch) {
      res.kind = K::Offline;
      res.errorCode = QStringLiteral("unreachable");
      res.errorMessage = r.errorString;
    } else if (r.status == 401) {
      if (conn_) conn_->noteUnauthorized();
      res.kind = K::Offline;
    } else if (r.status >= 500 || r.status == 429) {
      res.kind = K::Offline;
    } else if (r.status == 404) {
      res.kind = K::NotFound;
    } else if (r.status == 413) {
      res.kind = r.apiErrorCode == QLatin1String("state_quota_exceeded") ? K::QuotaFull : K::TooLarge;
    } else if (!r.ok()) {
      res.kind = K::Rejected;
    } else {
      res.kind = K::Ok;
      switch (expect) {
        case Expect::List: {
          const QJsonObject o = r.json();
          for (const QJsonValue& v : o.value(QStringLiteral("states")).toArray()) {
            if (const auto s = parseServerState(v.toObject())) res.states.append(*s);
          }
          res.usedBytes = o.value(QStringLiteral("used_bytes")).toVariant().toLongLong();
          res.quotaBytes = o.value(QStringLiteral("quota_bytes")).toVariant().toLongLong();
          break;
        }
        case Expect::Put:
          res.state = parseServerState(r.json());
          if (!res.state) res.kind = K::BadResponse;
          break;
        case Expect::Content: {
          QByteArray tag = etag.trimmed();
          if (tag.startsWith('"') && tag.endsWith('"') && tag.size() >= 2) tag = tag.mid(1, tag.size() - 2);
          const QByteArray actual = QCryptographicHash::hash(r.body, QCryptographicHash::Sha256).toHex();
          if (tag.isEmpty() || tag.toLower() != actual) {
            res.kind = K::BadResponse;
            res.errorCode = QStringLiteral("verification_failed");
          } else {
            res.content = r.body;
            res.coreId = coreId;
            res.coreVersion = coreVersion;
          }
          break;
        }
        case Expect::Preview:
          res.content = r.body;
          break;
        case Expect::None:
          break;
      }
    }
    cb(res);
  });
}

void StateApi::list(const QString& gameId, const QString& saveSlot, Callback cb) {
  QString path = QStringLiteral("/games/%1/states").arg(QString::fromLatin1(QUrl::toPercentEncoding(gameId)));
  if (!saveSlot.isEmpty()) path += QStringLiteral("?slot=") + QString::fromLatin1(QUrl::toPercentEncoding(saveSlot));
  QNetworkReply* reply = available() ? conn_->authorizedGet(path) : nullptr;
  if (reply == nullptr) return immediate(StateApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
  run(reply, Expect::List, std::move(cb));
}

QByteArray StateApi::multipartBody(const QByteArray& boundary, const QString& coreId, const QString& coreVersion, const QByteArray& state,
                                   const QByteArray& previewPng) {
  const QByteArray meta = QJsonDocument(QJsonObject{{QStringLiteral("core_id"), coreId},
                                                          // The server requires a version; a core from UZUME_*_CORE or the app folder has none.
                                                          {QStringLiteral("core_version"), coreVersion.isEmpty() ? QStringLiteral("unknown") : coreVersion}})
                              .toJson(QJsonDocument::Compact);
  QByteArray b;
  const auto part = [&](const char* name, const char* type, const QByteArray& data, const char* file) {
    b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"";
    if (file != nullptr) b += QByteArray("; filename=\"") + file + "\"";
    b += QByteArray("\r\nContent-Type: ") + type + "\r\n\r\n" + data + "\r\n";
  };
  part("meta", "application/json", meta, nullptr);
  part("state", "application/octet-stream", state, "state.bin");
  if (!previewPng.isEmpty()) part("preview", "image/png", previewPng, "preview.png");
  b += "--" + boundary + "--\r\n";
  return b;
}

void StateApi::put(const StateRecord& meta, const QByteArray& state, const QByteArray& previewPng, Callback cb) {
  if (state.isEmpty() || state.size() > kMaxStateBytes) return immediate(StateApiResult::Kind::TooLarge, QStringLiteral("payload_too_large"), std::move(cb));
  const QByteArray boundary = "uzume" + QUuid::createUuid().toByteArray(QUuid::Id128);
  const QByteArray preview = previewPng.size() <= kMaxStatePreviewBytes ? previewPng : QByteArray();
  const QByteArray body = multipartBody(boundary, meta.coreId, meta.coreVersion, state, preview);
  QNetworkReply* reply = available() ? conn_->authorizedSend("PUT", statePath(meta.gameId, meta.saveSlot, meta.n), body, {},
                                                            "multipart/form-data; boundary=" + boundary)
                                     : nullptr;
  if (reply == nullptr) return immediate(StateApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
  run(reply, Expect::Put, std::move(cb));
}

void StateApi::get(const QString& gameId, const QString& saveSlot, int n, Callback cb) {
  QNetworkReply* reply = available() ? conn_->authorizedGet(statePath(gameId, saveSlot, n)) : nullptr;
  if (reply == nullptr) return immediate(StateApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
  run(reply, Expect::Content, std::move(cb));
}

void StateApi::preview(const QString& gameId, const QString& saveSlot, int n, Callback cb) {
  QNetworkReply* reply = available() ? conn_->authorizedGet(statePath(gameId, saveSlot, n) + QStringLiteral("/preview")) : nullptr;
  if (reply == nullptr) return immediate(StateApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
  run(reply, Expect::Preview, std::move(cb));
}

void StateApi::remove(const QString& gameId, const QString& saveSlot, int n, Callback cb) {
  QNetworkReply* reply = available() ? conn_->authorizedSend("DELETE", statePath(gameId, saveSlot, n), QByteArray()) : nullptr;
  if (reply == nullptr) return immediate(StateApiResult::Kind::Offline, QStringLiteral("not_connected"), std::move(cb));
  run(reply, Expect::None, std::move(cb));
}

}  // namespace uzume
