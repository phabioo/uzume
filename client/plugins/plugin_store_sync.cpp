#include "plugin_store_sync.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace uzume::plugins {

namespace {
QString enc(const QString& s) { return QString::fromLatin1(QUrl::toPercentEncoding(s)); }
QString keyPath(const QString& plugin, const QString& key) { return QStringLiteral("/plugins/%1/store/%2").arg(enc(plugin), enc(key)); }
}  // namespace

struct PluginStoreSync::Work {
  QList<PluginStore::Dirty> pushes;
  QStringList pulls;                          // plugins still to pull
  QString pullPlugin;                         // the plugin whose keys are being pulled
  QList<std::pair<QString, qint64>> pullKeys; // (key, server revision) still to fetch
  bool ok = true;
  int conflictRetries = 0;
};

PluginStoreSync::PluginStoreSync(std::shared_ptr<PluginStore> store, HubConnection* connection, QObject* parent)
    : QObject(parent), store_(std::move(store)), conn_(connection) {
  debounce_.setSingleShot(true);
  debounce_.setInterval(1500);
  connect(&debounce_, &QTimer::timeout, this, [this]() { startRound({}); });
  retry_.setSingleShot(true);
  connect(&retry_, &QTimer::timeout, this, [this]() { startRound({}); });
  store_->setChangedCallback([this](const QString&) { QMetaObject::invokeMethod(this, [this] { pushSoon(); }, Qt::QueuedConnection); });
  if (conn_)
    connect(conn_, &HubConnection::stateChanged, this, [this](HubConnection::State s) {
      if (s == HubConnection::State::Connected) pushSoon();
    });
}

bool PluginStoreSync::supported() const {
  return conn_ && conn_->state() == HubConnection::State::Connected && conn_->hubHasFeature(QLatin1String(kFeature));
}

void PluginStoreSync::pullPlugin(const QString& pluginId) {
  if (!PluginStore::validPluginId(pluginId)) return;
  pullWanted_.insert(pluginId);
  pushSoon();
}

void PluginStoreSync::pushSoon() { debounce_.start(); }

void PluginStoreSync::syncNow(std::function<void()> done) { startRound(std::move(done)); }

void PluginStoreSync::request(const QByteArray& method, const QString& path, const QByteArray& body, std::function<void(const HttpResult&)> cb) {
  QNetworkReply* reply = nullptr;
  if (conn_) reply = method == "GET" ? conn_->authorizedGet(path) : conn_->authorizedSend(method, path, body, {}, "application/json");
  if (reply == nullptr) {
    QMetaObject::invokeMethod(this, [cb = std::move(cb)] {
      HttpResult r;
      r.networkError = true;
      cb(r);
    }, Qt::QueuedConnection);
    return;
  }
  connect(reply, &QNetworkReply::finished, this, [this, reply, cb = std::move(cb)]() {
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    if (r.status == 401 && conn_) conn_->noteUnauthorized();
    cb(r);
  });
}

void PluginStoreSync::startRound(std::function<void()> done) {
  if (done) done_ = std::move(done);
  if (busy_) {
    again_ = true;
    return;
  }
  if (!supported()) {
    if (done_) std::exchange(done_, {})();
    return;
  }
  busy_ = true;
  auto w = std::make_shared<Work>();
  w->pushes = store_->dirtyKeys();
  w->pulls = QStringList(pullWanted_.begin(), pullWanted_.end());
  pullWanted_.clear();
  pushNext(w);
}

void PluginStoreSync::pushNext(std::shared_ptr<Work> w) {
  if (w->pushes.isEmpty()) {
    pullNext(w);
    return;
  }
  const PluginStore::Dirty d = w->pushes.first();
  const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("base_revision"), static_cast<double>(d.serverRev)}, {QStringLiteral("value"), d.value}})
                              .toJson(QJsonDocument::Compact);
  // QJsonDocument cannot hold a bare scalar: build the body by hand when the value is not an object or array.
  QByteArray raw = body;
  if (!d.value.isObject() && !d.value.isArray()) {
    QByteArray v;
    switch (d.value.type()) {
      case QJsonValue::Bool: v = d.value.toBool() ? "true" : "false"; break;
      case QJsonValue::Double: v = QByteArray::number(d.value.toDouble(), 'g', 17); break;
      case QJsonValue::String: v = QJsonDocument(QJsonArray{d.value}).toJson(QJsonDocument::Compact); v = v.mid(1, v.size() - 2); break;
      default: v = "null"; break;
    }
    raw = "{\"base_revision\":" + QByteArray::number(d.serverRev) + ",\"value\":" + v + "}";
  }
  request("PUT", keyPath(d.pluginId, d.key), raw, [this, w, d](const HttpResult& r) {
    if (r.ok()) {
      store_->markPushed(d.pluginId, d.key, d.localRev, static_cast<qint64>(r.json().value(QStringLiteral("revision")).toDouble(d.serverRev + 1)));
      w->pushes.removeFirst();
    } else if (r.status == 409) {
      const QJsonObject o = r.json();
      const qint64 rev = static_cast<qint64>(o.value(QStringLiteral("revision")).toDouble(0));
      if (rev == 0 && !o.contains(QStringLiteral("value")) && w->conflictRetries++ < 2) {
        store_->markPushed(d.pluginId, d.key, -1, 0);  // the key is gone on the server: push again as a new key
        w->pushes.first().serverRev = 0;
      } else {
        store_->markServerConflict(d.pluginId, d.key, rev, o.value(QStringLiteral("value")));
        emit keyConflicted(d.pluginId, d.key);
        w->pushes.removeFirst();
      }
    } else if (r.status == 413 || r.status == 400) {
      w->pushes.removeFirst();  // the server will never take this value; keep it local, try the next
    } else {
      w->ok = false;  // offline or server trouble: stop and retry later
      w->pushes.clear();
      w->pulls.clear();
    }
    pushNext(w);
  });
}

void PluginStoreSync::pullNext(std::shared_ptr<Work> w) {
  if (w->pulls.isEmpty()) {
    finishRound(w, w->ok);
    return;
  }
  w->pullPlugin = w->pulls.takeFirst();
  request("GET", QStringLiteral("/plugins/%1/store").arg(enc(w->pullPlugin)), {}, [this, w](const HttpResult& r) {
    if (!r.ok()) {
      w->ok = w->ok && r.status == 404;
      pullNext(w);
      return;
    }
    QHash<QString, qint64> local;
    for (const auto& kv : store_->serverRevisions(w->pullPlugin)) local.insert(kv.first, kv.second);
    w->pullKeys.clear();
    for (const QJsonValue& v : r.json().value(QStringLiteral("keys")).toArray()) {
      const QJsonObject o = v.toObject();
      const QString key = o.value(QStringLiteral("key")).toString();
      const qint64 rev = static_cast<qint64>(o.value(QStringLiteral("revision")).toDouble(0));
      if (PluginStore::validKey(key) && rev > local.value(key, 0)) w->pullKeys.append({key, rev});
    }
    pullKeys(w);
  });
}

void PluginStoreSync::pullKeys(std::shared_ptr<Work> w) {
  if (w->pullKeys.isEmpty()) {
    pullNext(w);
    return;
  }
  const auto [key, rev] = w->pullKeys.takeFirst();
  const QString plugin = w->pullPlugin;
  request("GET", keyPath(plugin, key), {}, [this, w, plugin, key, rev](const HttpResult& r) {
    if (!r.ok()) {
      w->ok = w->ok && r.status == 404;
      pullKeys(w);
      return;
    }
    const QJsonObject o = r.json();
    const qint64 serverRev = static_cast<qint64>(o.value(QStringLiteral("revision")).toDouble(rev));
    if (store_->isDirty(plugin, key)) {
      store_->markServerConflict(plugin, key, serverRev, o.value(QStringLiteral("value")));
      emit keyConflicted(plugin, key);
    } else {
      store_->adoptServer(plugin, key, serverRev, o.value(QStringLiteral("value")));
      emit keyAdopted(plugin, key);
    }
    pullKeys(w);
  });
}

void PluginStoreSync::finishRound(std::shared_ptr<Work>, bool ok) {
  busy_ = false;
  auto done = std::exchange(done_, {});
  if (!ok) retry_.start(retryMs_);
  if (again_) {
    again_ = false;
    startRound({});
  }
  if (done) done();
}

}  // namespace uzume::plugins
