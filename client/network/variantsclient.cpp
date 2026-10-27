#include "variantsclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>

namespace uzume {

namespace {
QString seedText(const QJsonValue& v) {
  if (v.isString()) return v.toString();
  if (v.isDouble()) return QString::number(static_cast<qint64>(v.toDouble()));
  return {};
}
}  // namespace

QJsonObject parseVariantJson(const QByteArray& body) {
  // Quote the digits of seed / run_seed: a QJsonDocument would round an int63 to a double.
  static const QRegularExpression re(QStringLiteral("(\"(?:seed|run_seed)\"\\s*:\\s*)(-?\\d{10,})"));
  QString text = QString::fromUtf8(body);
  text.replace(re, QStringLiteral("\\1\"\\2\""));
  const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
  if (doc.isArray()) return QJsonObject{{QStringLiteral("variants"), doc.array()}};
  return doc.object();
}

VariantInfo parseVariant(const QJsonObject& o) {
  VariantInfo v;
  v.id = o.value(QStringLiteral("id")).toString();
  v.gameId = o.value(QStringLiteral("game_id")).toString();
  v.name = o.value(QStringLiteral("name")).toString();
  v.slot = o.value(QStringLiteral("slot")).toString();
  v.toolId = o.value(QStringLiteral("tool_id")).toString(QStringLiteral("randomizer"));
  v.toolVersion = o.value(QStringLiteral("tool_version")).toString();
  v.settings = o.value(QStringLiteral("settings")).toString();
  v.seed = seedText(o.value(QStringLiteral("seed")));
  v.baseSha256 = o.value(QStringLiteral("base_sha256")).toString();
  v.titleId = o.value(QStringLiteral("title_id")).toString();
  v.updateVersion = o.value(QStringLiteral("update_version")).toString();
  v.spoilerLog = o.value(QStringLiteral("spoiler_log")).toBool();
  v.runId = o.value(QStringLiteral("run_id")).toString();
  v.createdAt = o.value(QStringLiteral("created_at")).toString();
  v.deletedAt = o.value(QStringLiteral("deleted_at")).toString();
  return v;
}

RunVariantRecipe parseRunVariant(const QJsonObject& o) {
  RunVariantRecipe r;
  r.toolId = o.value(QStringLiteral("tool_id")).toString(QStringLiteral("randomizer"));
  r.toolVersion = o.value(QStringLiteral("tool_version")).toString();
  r.settings = o.value(QStringLiteral("settings")).toString();
  r.seed = seedText(o.value(QStringLiteral("seed")));
  r.baseSha256 = o.value(QStringLiteral("base_sha256")).toString();
  r.titleId = o.value(QStringLiteral("title_id")).toString();
  r.updateVersion = o.value(QStringLiteral("update_version")).toString();
  r.slot = o.value(QStringLiteral("slot")).toString();
  return r;
}

bool VariantsClient::available() const {
  return conn_ && conn_->state() == HubConnection::State::Connected && conn_->hubHasFeature(QStringLiteral("variants_v1"));
}

VariantCall VariantsClient::callOf(const HttpResult& r) {
  VariantCall c;
  c.ok = r.ok();
  c.status = r.status;
  if (!c.ok) c.error = r.networkError ? QStringLiteral("network") : (r.apiErrorCode.isEmpty() ? QStringLiteral("http_%1").arg(r.status) : r.apiErrorCode);
  return c;
}

void VariantsClient::send(const QByteArray& method, const QString& path, const QByteArray& body, std::function<void(const HttpResult&)> cb) {
  QNetworkReply* reply = nullptr;
  if (conn_) {
    if (method == "GET") reply = conn_->authorizedGet(path);
    else reply = conn_->authorizedSend(method, path, body, {}, "application/json");
  }
  if (!reply) {
    HttpResult r;
    r.networkError = true;
    r.errorString = QStringLiteral("Not connected to the server");
    QMetaObject::invokeMethod(this, [cb, r]() { cb(r); }, Qt::QueuedConnection);
    return;
  }
  connect(reply, &QNetworkReply::finished, this, [this, reply, cb]() {
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    if (r.status == 401 && conn_) conn_->noteUnauthorized();
    cb(r);
  });
}

void VariantsClient::list(const QString& gameId, ListCb cb) {
  const QString path = gameId.isEmpty() ? QStringLiteral("/variants") : QStringLiteral("/games/%1/variants").arg(QString::fromLatin1(QUrl::toPercentEncoding(gameId)));
  send("GET", path, {}, [cb](const HttpResult& r) {
    QList<VariantInfo> out;
    const VariantCall c = callOf(r);
    if (c.ok) {
      const QJsonObject o = parseVariantJson(r.body);
      for (const QJsonValue& v : o.value(QStringLiteral("variants")).toArray()) {
        const VariantInfo vi = parseVariant(v.toObject());
        if (vi.valid()) out.append(vi);
      }
    }
    cb(out, c);
  });
}

void VariantsClient::create(const QString& gameId, const NewVariant& v, OneCb cb) {
  QJsonObject o{{QStringLiteral("name"), v.name},
                {QStringLiteral("tool_id"), v.toolId},
                {QStringLiteral("tool_version"), v.toolVersion},
                {QStringLiteral("settings"), v.settings},
                {QStringLiteral("spoiler_log"), v.spoilerLog}};
  if (!v.baseSha256.isEmpty()) o.insert(QStringLiteral("base_sha256"), v.baseSha256);
  if (!v.titleId.isEmpty()) o.insert(QStringLiteral("title_id"), v.titleId);
  if (!v.updateVersion.isEmpty()) o.insert(QStringLiteral("update_version"), v.updateVersion);
  QByteArray body = QJsonDocument(o).toJson(QJsonDocument::Compact);
  // The seed goes out as an exact integer literal.
  bool isNumber = false;
  v.seed.toLongLong(&isNumber);
  if (isNumber) {
    body.chop(1);
    body += ",\"seed\":" + v.seed.toLatin1() + "}";
  }
  send("POST", QStringLiteral("/games/%1/variants").arg(QString::fromLatin1(QUrl::toPercentEncoding(gameId))), body, [cb](const HttpResult& r) {
    cb(callOf(r).ok ? parseVariant(parseVariantJson(r.body)) : VariantInfo(), callOf(r));
  });
}

void VariantsClient::rename(const QString& id, const QString& name, OneCb cb) {
  send("PATCH", QStringLiteral("/variants/%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(id))),
       QJsonDocument(QJsonObject{{QStringLiteral("name"), name}}).toJson(QJsonDocument::Compact), [cb](const HttpResult& r) {
         cb(callOf(r).ok ? parseVariant(parseVariantJson(r.body)) : VariantInfo(), callOf(r));
       });
}

void VariantsClient::remove(const QString& id, bool keepSaves, DoneCb cb) {
  send("DELETE", QStringLiteral("/variants/%1?saves=%2").arg(QString::fromLatin1(QUrl::toPercentEncoding(id)), keepSaves ? QStringLiteral("keep") : QStringLiteral("delete")), {},
       [cb](const HttpResult& r) { cb(callOf(r)); });
}

void VariantsClient::runVariant(const QString& runId, RunCb cb) {
  send("GET", QStringLiteral("/runs/%1/variant").arg(QString::fromLatin1(QUrl::toPercentEncoding(runId))), {}, [cb](const HttpResult& r) {
    const VariantCall c = callOf(r);
    cb(c.ok ? parseRunVariant(parseVariantJson(r.body)) : RunVariantRecipe(), c);
  });
}

void VariantsClient::setRunVariantState(const QString& runId, const QString& state, DoneCb cb) {
  send("POST", QStringLiteral("/runs/%1/variant-state").arg(QString::fromLatin1(QUrl::toPercentEncoding(runId))),
       QJsonDocument(QJsonObject{{QStringLiteral("state"), state}}).toJson(QJsonDocument::Compact), [cb](const HttpResult& r) { cb(callOf(r)); });
}

}  // namespace uzume
