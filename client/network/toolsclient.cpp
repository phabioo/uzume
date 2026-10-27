#include "toolsclient.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QTimer>
#include <memory>

#include "corecache.h"

namespace uzume {

Q_LOGGING_CATEGORY(lcTools, "uzume.tools")

namespace {
struct HashState {
  QCryptographicHash h{QCryptographicHash::Sha256};
  qint64 got = 0;
};
}  // namespace

const ToolVersionInfo* ToolInfo::versionInfo(const QString& v) const {
  for (const ToolVersionInfo& x : versions)
    if (x.version == v) return &x;
  return nullptr;
}

QList<ToolInfo> parseTools(const QByteArray& body) {
  QList<ToolInfo> out;
  const QJsonDocument doc = QJsonDocument::fromJson(body);
  const QJsonArray arr = doc.isArray() ? doc.array() : doc.object().value(QStringLiteral("tools")).toArray();
  for (const QJsonValue& tv : arr) {
    const QJsonObject o = tv.toObject();
    ToolInfo t;
    t.id = o.value(QStringLiteral("id")).toString();
    if (!ToolCache::isValidId(t.id)) continue;
    t.name = o.value(QStringLiteral("name")).toString(t.id);
    t.offered = o.value(QStringLiteral("offered")).toBool(false);
    t.version = o.value(QStringLiteral("version")).toString();
    t.license = o.value(QStringLiteral("license")).toString();
    t.sourceUrl = o.value(QStringLiteral("source_url")).toString();
    for (const QJsonValue& vv : o.value(QStringLiteral("versions")).toArray()) {
      const QJsonObject vo = vv.toObject();
      ToolVersionInfo v;
      v.version = vo.value(QStringLiteral("version")).toString();
      if (!ToolCache::isValidId(v.version)) continue;
      for (const QJsonValue& pv : vo.value(QStringLiteral("platforms")).toArray()) {
        const QJsonObject po = pv.toObject();
        ToolPlatformInfo p;
        p.platform = po.value(QStringLiteral("platform")).toString();
        p.size = static_cast<qint64>(po.value(QStringLiteral("size")).toDouble());
        p.sha256 = po.value(QStringLiteral("sha256")).toString().toLower();
        p.cached = po.value(QStringLiteral("cached")).toBool(true);
        if (ToolCache::isValidId(p.platform)) v.platforms.append(p);
      }
      t.versions.append(v);
    }
    if (t.version.isEmpty() && !t.versions.isEmpty()) {
      t.version = t.versions.first().version;
      for (const ToolVersionInfo& v : std::as_const(t.versions))
        if (CoreCache::compareVersions(v.version, t.version) > 0) t.version = v.version;
    }
    out.append(t);
  }
  return out;
}

ToolsClient::ToolsClient(HubConnection* connection, ToolCache* cache, QObject* parent)
    : QObject(parent), conn_(connection), cache_(cache), platform_(CoreCache::currentPlatform()) {
  qRegisterMetaType<uzume::ToolPrepareResult>("uzume::ToolPrepareResult");
  if (connection != nullptr) {
    connect(connection, &HubConnection::stateChanged, this, [this](HubConnection::State s) {
      if (s != HubConnection::State::Connected && busy_) {
        ++generation_;
        fail(ToolProblem::DownloadFailed, QStringLiteral("Connection to the server lost"));
      }
    });
  }
}

bool ToolsClient::available() const {
  return conn_ && conn_->state() == HubConnection::State::Connected && conn_->hubHasFeature(QStringLiteral("tools_v1"));
}

const ToolInfo* ToolsClient::tool(const QString& id) const {
  for (const ToolInfo& t : tools_)
    if (t.id == id) return &t;
  return nullptr;
}

const ToolPlatformInfo* ToolsClient::platformFor(const ToolVersionInfo& v) const {
  const QString want = ToolCache::normalizePlatform(platform_);
  for (const ToolPlatformInfo& p : v.platforms)
    if (ToolCache::normalizePlatform(p.platform) == want) return &p;
  return nullptr;
}

QString ToolsClient::offeredVersion(const QString& toolId, ToolPlatformInfo* platformOut) const {
  const ToolInfo* t = tool(toolId);
  if (!t || !t->offered) return {};
  const ToolVersionInfo* v = t->versionInfo(t->version);
  if (!v) return {};
  const ToolPlatformInfo* p = platformFor(*v);
  if (!p) return {};
  if (platformOut) *platformOut = *p;
  return v->version;
}

void ToolsClient::refresh() {
  if (!available()) return;
  QNetworkReply* reply = conn_->authorizedGet(QStringLiteral("/tools"));
  if (!reply) {
    emit refreshFailed(QStringLiteral("not connected"));
    return;
  }
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    if (r.status == 401 && conn_) conn_->noteUnauthorized();
    if (!r.ok()) {
      emit refreshFailed(r.networkError ? r.errorString : (r.apiErrorCode.isEmpty() ? QStringLiteral("HTTP %1").arg(r.status) : r.apiErrorCode));
      return;
    }
    tools_ = parseTools(r.body);
    loaded_ = true;
    emit toolsChanged();
  });
}

void ToolsClient::cancel() {
  if (!busy_) return;
  ++generation_;
  if (reply_) reply_->abort();
  fail(ToolProblem::Cancelled, QString());
}

void ToolsClient::prepare(const QString& toolId, const QString& version) {
  if (busy_) return;
  busy_ = true;
  ++generation_;
  result_ = {};
  result_.toolId = toolId;
  result_.version = version;
  const quint64 gen = generation_;
  QTimer::singleShot(0, this, [this, gen, toolId, version]() {
    if (gen != generation_ || !busy_) return;
    if (!available()) return fail(ToolProblem::DownloadFailed, QStringLiteral("Not connected to the server"));
    if (!ToolCache::isValidId(toolId) || !ToolCache::isValidId(version)) return fail(ToolProblem::NotOnServer, QStringLiteral("invalid tool or version"));
    if (cache_->isReady(toolId, version, ToolCache::normalizePlatform(platform_))) {
      result_.ok = true;
      busy_ = false;
      emit prepared(result_);
      return;
    }
    // Always against a fresh catalog: the sizes and hashes come from it.
    QNetworkReply* reply = conn_->authorizedGet(QStringLiteral("/tools"));
    if (!reply) return fail(ToolProblem::DownloadFailed, QStringLiteral("Not connected to the server"));
    reply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, toolId, version]() {
      const HttpResult r = HubHttp::resultOf(reply);
      reply->deleteLater();
      if (gen != generation_ || !busy_) return;
      if (r.status == 401 && conn_) conn_->noteUnauthorized();
      if (!r.ok()) return fail(ToolProblem::DownloadFailed, r.networkError ? r.errorString : QStringLiteral("HTTP %1").arg(r.status));
      tools_ = parseTools(r.body);
      loaded_ = true;
      emit toolsChanged();
      const ToolInfo* t = tool(toolId);
      if (!t) return fail(ToolProblem::NotOnServer, QStringLiteral("the server has no tool %1").arg(toolId));
      const ToolVersionInfo* v = t->versionInfo(version);
      if (!v) return fail(ToolProblem::NotOnServer, QStringLiteral("the server does not offer %1 %2").arg(toolId, version));
      if (!t->offered) return fail(ToolProblem::NotOffered, QStringLiteral("the server does not offer %1 to apps").arg(toolId));
      const ToolPlatformInfo* p = platformFor(*v);
      if (!p) return fail(ToolProblem::Incompatible, QStringLiteral("no package of %1 %2 for %3").arg(toolId, version, platform_));
      startDownload(*t, *v, *p);
    });
  });
}

void ToolsClient::startDownload(const ToolInfo& tool, const ToolVersionInfo& ver, const ToolPlatformInfo& plat) {
  const QString dir = cache_->root() + QStringLiteral("/.downloads");
  QDir().mkpath(dir);
  tmpPath_ = dir + QStringLiteral("/%1-%2-%3.zip").arg(tool.id, ver.version, plat.platform);
  tmp_ = std::make_unique<QFile>(tmpPath_);
  if (!tmp_->open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail(ToolProblem::InstallFailed, QStringLiteral("cannot write the download"));
  QNetworkReply* reply = conn_ ? conn_->authorizedGet(QStringLiteral("/tools/%1/packages/%2/%3").arg(tool.id, ver.version, plat.platform)) : nullptr;
  if (!reply) return fail(ToolProblem::DownloadFailed, QStringLiteral("Not connected to the server"));
  reply_ = reply;
  const quint64 gen = generation_;
  const qint64 expectedSize = plat.size;
  const QString expectedSha = plat.sha256;
  auto hash = std::make_shared<HashState>();
  const QString toolId = tool.id;
  connect(reply, &QNetworkReply::readyRead, this, [this, reply, hash, gen, expectedSize, toolId]() {
    if (gen != generation_ || !busy_ || !tmp_) return;
    const QByteArray chunk = reply->readAll();
    hash->h.addData(chunk);
    hash->got += chunk.size();
    if (expectedSize > 0 && hash->got > expectedSize) {
      reply->abort();  // more than the catalog announced: not the file we verify against
      return;
    }
    if (tmp_->write(chunk) != chunk.size()) reply->abort();
  });
  connect(reply, &QNetworkReply::downloadProgress, this, [this, gen, toolId, expectedSize](qint64 got, qint64 total) {
    if (gen == generation_ && busy_) emit progress(toolId, got, total > 0 ? total : expectedSize);
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, hash, gen, expectedSize, expectedSha, tool, ver, plat]() {
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    if (gen != generation_ || !busy_) return;
    if (r.status == 401 && conn_) conn_->noteUnauthorized();
    if (tmp_) tmp_->close();
    if (!r.ok()) {
      if (r.status == 404) return fail(ToolProblem::NotOnServer, QStringLiteral("the package is not on the server"));
      return fail(ToolProblem::DownloadFailed, r.networkError ? r.errorString : (r.apiErrorCode.isEmpty() ? QStringLiteral("HTTP %1").arg(r.status) : r.apiErrorCode));
    }
    if (expectedSize > 0 && hash->got != expectedSize) return fail(ToolProblem::InvalidSize, QStringLiteral("size differs from the catalog"));
    if (QString::fromLatin1(hash->h.result().toHex()).compare(expectedSha, Qt::CaseInsensitive) != 0)
      return fail(ToolProblem::InvalidHash, QStringLiteral("checksum differs from the catalog"));
    QString err;
    const auto res = cache_->installZip(tool.id, ver.version, ToolCache::normalizePlatform(plat.platform), tmpPath_, expectedSha, &err);
    if (res != ToolCache::InstallResult::Ok) return fail(res == ToolCache::InstallResult::HashMismatch ? ToolProblem::InvalidHash : ToolProblem::InstallFailed, err);
    qCInfo(lcTools) << "installed" << tool.id << ver.version << plat.platform << expectedSha;
    result_.ok = true;
    cleanup();
    busy_ = false;
    emit prepared(result_);
  });
}

void ToolsClient::cleanup() {
  if (tmp_) tmp_->close();
  tmp_.reset();
  if (!tmpPath_.isEmpty()) QFile::remove(tmpPath_);
  tmpPath_.clear();
  reply_.clear();
}

void ToolsClient::fail(ToolProblem p, const QString& detail) {
  qCWarning(lcTools) << "problem" << result_.toolId << result_.version << static_cast<int>(p);
  cleanup();
  result_.ok = false;
  result_.problem = p;
  result_.detail = detail;
  busy_ = false;
  emit prepared(result_);
}

}  // namespace uzume
