#include "runclient.h"

#include <QDir>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <QtGlobal>
#include <algorithm>
#include <chrono>

namespace uzume {

Q_LOGGING_CATEGORY(lcRuns, "uzume.runs")

namespace {

constexpr int kEventCache = 500;      // events kept per run (memory and disk)
constexpr int kPageSize = 500;        // GET .../events limit
constexpr int kMaxPages = 20;         // a catch-up never pages more than this in one go
constexpr int kMaxQueuedEvents = 200;  // not yet confirmed events per run
constexpr int kMaxRetryMs = 15000;

QString safeName(const QString& s) {
  QString out;
  for (const QChar c : s) out += (c.isLetterOrNumber() && c.unicode() < 128) || c == QLatin1Char('-') || c == QLatin1Char('_') ? c : QLatin1Char('_');
  return out.isEmpty() ? QStringLiteral("_") : out;
}

QByteArray compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

bool transient(const HttpResult& r) { return r.networkError || r.status == 0 || r.status == 429 || r.status >= 500; }

QString messageOf(const HttpResult& r) {
  if (!r.apiErrorMessage.isEmpty()) return r.apiErrorMessage;
  if (r.networkError) return r.errorString;
  return QStringLiteral("HTTP %1").arg(r.status);
}

bool validEventType(const QString& t) {
  if (t.isEmpty() || t.size() > 64) return false;
  for (const QChar c : t)
    if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('_') ||
          c == QLatin1Char('.') || c == QLatin1Char('-')))
      return false;
  return true;
}

}  // namespace

qint64 RunClient::nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

RunClient::RunClient(HubConnection* connection, HubSocket* socket, const QString& dataDir, QObject* parent)
    : QObject(parent), conn_(connection), socket_(socket), dataDir_(dataDir) {
  qRegisterMetaType<uzume::RunEvent>("uzume::RunEvent");
  saveTimer_.setSingleShot(true);
  saveTimer_.setInterval(500);
  connect(&saveTimer_, &QTimer::timeout, this, &RunClient::saveCache);
  connect(connection, &HubConnection::stateChanged, this, &RunClient::onConnectionState);
  if (socket != nullptr) {
    socket->setHelloRunsProvider([this]() { return helloRuns(); });
    connect(socket, &HubSocket::runUpdated, this, &RunClient::applyRunJson);
    connect(socket, &HubSocket::runStateReceived, this, &RunClient::onRunState);
    connect(socket, &HubSocket::runEventReceived, this, &RunClient::onRunEvent);
  }
  if (connection->state() == HubConnection::State::Connected) onConnectionState(HubConnection::State::Connected);
}

RunClient::~RunClient() {
  if (socket_) socket_->setHelloRunsProvider({});
  saveCache();
}

bool RunClient::supported() const {
  return conn_ && conn_->state() == HubConnection::State::Connected && conn_->hubHasFeature(QStringLiteral("runs_v1"));
}

bool RunClient::supportsPluginless() const { return supported() && conn_->hubHasFeature(QStringLiteral("runs_v2")); }

bool RunClient::supportsRules() const { return supported() && conn_->hubHasFeature(QStringLiteral("run_rules_v1")); }

QString RunClient::myUserId() const { return conn_ ? conn_->hubUserId() : QString(); }

void RunClient::onConnectionState(HubConnection::State s) {
  if (s == HubConnection::State::Connected) {
    ensureCacheLoaded();
    if (supported()) refresh();
    return;
  }
  reset();
}

// {user id: {plugin id: {state, state_rev}}} of every participant (the persisted form).
static QJsonObject statesJson(const RunInfo& info) {
  QJsonObject out;
  for (const RunParticipant& p : info.participants) {
    QJsonObject byPlugin;
    for (auto it = p.states.begin(); it != p.states.end(); ++it)
      byPlugin.insert(it.key(), QJsonObject{{QStringLiteral("state"), it->state}, {QStringLiteral("state_rev"), static_cast<double>(it->rev)}});
    if (!byPlugin.isEmpty()) out.insert(p.userId, byPlugin);
  }
  return out;
}

void RunClient::reset() {
  ++gen_;
  saveCache();
  for (auto it = runs_.begin(); it != runs_.end(); ++it) {
    cachedSeq_[it.key()] = it.value()->lastSeq;
    cachedEvents_[it.key()] = it.value()->events;
    cachedStates_[it.key()] = statesJson(it.value()->info);
  }
  const bool had = !runs_.isEmpty() || loaded_;
  runs_.clear();
  loaded_ = false;
  if (had) emit runsChanged();
}

// ------------------------------------------------------------------ queries

RunClient::RunData* RunClient::data(const QString& runId) {
  const auto it = runs_.find(runId.toLower());
  return it == runs_.end() ? nullptr : it->get();
}
const RunClient::RunData* RunClient::data(const QString& runId) const {
  const auto it = runs_.constFind(runId.toLower());
  return it == runs_.constEnd() ? nullptr : it->get();
}

QList<RunInfo> RunClient::runs() const {
  QList<RunInfo> out;
  for (const auto& d : runs_) out.append(d->info);
  std::sort(out.begin(), out.end(), [](const RunInfo& a, const RunInfo& b) {
    return a.updatedAt != b.updatedAt ? a.updatedAt > b.updatedAt : a.runId < b.runId;
  });
  return out;
}

std::optional<RunInfo> RunClient::run(const QString& runId) const {
  const RunData* d = data(runId);
  if (d == nullptr) return std::nullopt;
  return d->info;
}

int RunClient::pendingInviteCount() const {
  const QString me = myUserId();
  int n = 0;
  for (const auto& d : runs_) {
    const RunParticipant* p = d->info.participant(me);
    if (p != nullptr && p->status == QLatin1String("invited") && !d->info.isOver()) ++n;
  }
  return n;
}

qint64 RunClient::lastSeq(const QString& runId) const {
  const RunData* d = data(runId);
  return d != nullptr ? d->lastSeq : cachedSeq_.value(runId.toLower(), 0);
}

int RunClient::pendingEvents(const QString& runId) const {
  const RunData* d = data(runId);
  return d != nullptr ? static_cast<int>(d->queue.size()) : 0;
}

QList<RunEvent> RunClient::events(const QString& runId, qint64 afterSeq, int limit) const {
  const RunData* d = data(runId);
  const QList<RunEvent>& all = d != nullptr ? d->events : cachedEvents_.value(runId.toLower());
  QList<RunEvent> out;
  for (const RunEvent& e : all) {
    if (e.seq <= afterSeq) continue;
    out.append(e);
    if (limit > 0 && out.size() >= limit) break;
  }
  return out;
}

QJsonObject RunClient::ownState(const QString& runId, const QString& pluginId) const {
  const RunData* d = data(runId);
  if (d == nullptr) return {};
  const auto o = d->own.find(pluginId);
  if (o != d->own.end() && (o->second.dirty || o->second.inFlight)) return o->second.desired;
  const RunParticipant* p = d->info.participant(myUserId());
  return p != nullptr ? p->stateOf(pluginId).state : QJsonObject();
}

std::optional<QJsonObject> RunClient::participantState(const QString& runId, const QString& userId, const QString& pluginId) const {
  const RunData* d = data(runId);
  if (d == nullptr) return std::nullopt;
  const RunParticipant* p = d->info.participant(userId);
  if (p == nullptr) return std::nullopt;
  if (userId == myUserId()) return ownState(runId, pluginId);
  return p->stateOf(pluginId).state;
}

// ------------------------------------------------------------------ disk cache

QString RunClient::cacheDir() const {
  if (!conn_ || dataDir_.isEmpty() || conn_->hubId().isEmpty() || conn_->hubUserId().isEmpty()) return {};
  return dataDir_ + QStringLiteral("/runs/") + safeName(conn_->hubId()) + QLatin1Char('/') + safeName(conn_->hubUserId());
}

void RunClient::ensureCacheLoaded() {
  const QString owner = conn_ ? conn_->hubId() + QLatin1Char('/') + conn_->hubUserId() : QString();
  if (owner == cacheOwner_) return;
  cacheOwner_ = owner;
  cachedSeq_.clear();
  cachedEvents_.clear();
  cachedStates_.clear();
  loadCache();
}

void RunClient::loadCache() {
  const QString dir = cacheDir();
  if (dir.isEmpty()) return;
  const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files);
  for (const QFileInfo& fi : files) {
    const QString id = fi.completeBaseName();
    if (QUuid::fromString(id).isNull()) continue;
    QFile f(fi.absoluteFilePath());
    if (!f.open(QIODevice::ReadOnly) || f.size() > 8 * 1024 * 1024) continue;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    QList<RunEvent> evs;
    qint64 last = 0;
    for (const QJsonValue& v : o.value(QStringLiteral("events")).toArray()) {
      const auto e = parseRunEvent(v.toObject());
      if (e && e->seq > last) {
        evs.append(*e);
        last = e->seq;
      }
    }
    const qint64 declared = static_cast<qint64>(o.value(QStringLiteral("last_seq")).toDouble(0));
    cachedSeq_.insert(id, (std::max)(declared, last));
    cachedEvents_.insert(id, evs);
    cachedStates_.insert(id, o.value(QStringLiteral("states")).toObject());
  }
}

void RunClient::markCacheDirty(RunData& d) {
  d.cacheDirty = true;
  if (!saveTimer_.isActive()) saveTimer_.start();
}

void RunClient::saveCache() {
  saveTimer_.stop();
  const QString dir = cacheDir();
  if (dir.isEmpty()) return;
  for (auto it = runs_.begin(); it != runs_.end(); ++it) {
    RunData& d = *it.value();
    if (!d.cacheDirty) continue;
    d.cacheDirty = false;
    if (!QDir().mkpath(dir)) return;
    QJsonArray evs;
    for (const RunEvent& e : d.events) evs.append(runEventToJson(e));
    QSaveFile f(dir + QLatin1Char('/') + it.key() + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly)) continue;
    f.write(compact({{QStringLiteral("last_seq"), static_cast<double>(d.lastSeq)}, {QStringLiteral("events"), evs}, {QStringLiteral("states"), statesJson(d.info)}}));
    f.commit();
  }
}

// ------------------------------------------------------------------ HTTP

RunResult RunClient::toResult(const HttpResult& r) {
  RunResult res;
  res.ok = r.ok();
  res.status = r.status;
  res.networkError = r.networkError;
  res.errorCode = r.apiErrorCode;
  res.message = res.ok ? QString() : messageOf(r);
  if (!r.networkError) res.json = r.json();
  return res;
}

void RunClient::send(const QByteArray& method, const QString& path, const std::optional<QJsonObject>& body, std::function<void(const HttpResult&)> cb) {
  QNetworkReply* reply = nullptr;
  if (conn_) {
    reply = method == "GET" ? conn_->authorizedGet(path) : conn_->authorizedSend(method, path, body ? compact(*body) : QByteArray(), {}, "application/json");
  }
  if (reply == nullptr) {
    HttpResult r;
    r.networkError = true;
    r.errorString = QStringLiteral("Not connected to the server");
    QTimer::singleShot(0, this, [cb = std::move(cb), r]() { cb(r); });
    return;
  }
  const quint64 gen = gen_;
  connect(reply, &QNetworkReply::finished, this, [this, reply, gen, cb = std::move(cb)]() {
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    if (gen != gen_) return;  // the connection changed: stale answer
    if (r.status == 401 && conn_) conn_->noteUnauthorized();
    cb(r);
  });
}

void RunClient::sendRun(const QByteArray& method, const QString& path, const std::optional<QJsonObject>& body, RunCallback cb) {
  send(method, path, body, [this, cb = std::move(cb)](const HttpResult& r) {
    RunResult res = toResult(r);
    if (res.ok) {
      if (const auto run = parseRun(res.json)) {
        applyRun(*run);
        res.run = run;
      }
    }
    if (cb) cb(res);
  });
}

void RunClient::refresh() {
  if (!supported()) return;
  // The answer is a snapshot from the time the Hub served it: a run that was updated here meanwhile (join / leave
  // answer, run_updated push) is newer and is neither overwritten nor dropped by it.
  const quint64 started = applySerial_;
  send("GET", QStringLiteral("/runs"), std::nullopt, [this, started](const HttpResult& r) {
    if (!r.ok()) {
      qCWarning(lcRuns) << "list failed:" << messageOf(r);
      return;
    }
    QSet<QString> seen;
    for (const QJsonValue& v : r.json().value(QStringLiteral("runs")).toArray()) {
      if (const auto run = parseRun(v.toObject())) {
        seen.insert(run->runId);
        const RunData* known = data(run->runId);
        if (known != nullptr && known->appliedSerial > started) continue;  // updated after this list was served
        applyRun(*run);
      }
    }
    bool removed = false;
    for (auto it = runs_.begin(); it != runs_.end();) {
      if (!seen.contains(it.key()) && (*it)->appliedSerial <= started) {
        it = runs_.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }
    // Caches of runs that no longer exist for this user (archived, removed) go away.
    const QString dir = cacheDir();
    for (auto it = cachedSeq_.begin(); it != cachedSeq_.end();) {
      if (!seen.contains(it.key()) && !runs_.contains(it.key())) {
        if (!dir.isEmpty()) QFile::remove(dir + QLatin1Char('/') + it.key() + QStringLiteral(".json"));
        cachedEvents_.remove(it.key());
        cachedStates_.remove(it.key());
        it = cachedSeq_.erase(it);
      } else {
        ++it;
      }
    }
    loaded_ = true;
    if (removed) emit runsChanged();
    emit runsChanged();
  });
}

// ------------------------------------------------------------------ REST actions

static QString enc(const QString& s) { return QString::fromLatin1(QUrl::toPercentEncoding(s)); }

void RunClient::createRun(const QString& name, const QList<RunPluginRef>& plugins, const QString& gameId, const QStringList& inviteUserIds,
                          const QJsonObject& settings, RunCallback cb) {
  createRunBody(name, plugins, gameId, inviteUserIds, settings, nullptr, std::move(cb));
}

void RunClient::createRun(const QString& name, const QList<RunPluginRef>& plugins, const QString& gameId, const QStringList& inviteUserIds,
                          const QJsonObject& settings, const RunRules& rules, RunCallback cb) {
  createRunBody(name, plugins, gameId, inviteUserIds, settings, &rules, std::move(cb));
}

void RunClient::setRules(const QString& runId, const RunRules& rules, RunCallback cb) {
  sendRun("PUT", QStringLiteral("/runs/%1/rules").arg(enc(runId)), runRulesToJson(rules), std::move(cb));
}

void RunClient::createRunBody(const QString& name, const QList<RunPluginRef>& plugins, const QString& gameId, const QStringList& inviteUserIds,
                              const QJsonObject& settings, const RunRules* rules, RunCallback cb) {
  QJsonArray pl;
  for (const RunPluginRef& p : plugins) pl.append(QJsonObject{{QStringLiteral("plugin_id"), p.pluginId}, {QStringLiteral("version"), p.version}});
  QJsonObject body{{QStringLiteral("name"), name}, {QStringLiteral("plugins"), pl}, {QStringLiteral("game_id"), gameId}, {QStringLiteral("settings"), settings}};
  if (!inviteUserIds.isEmpty()) body.insert(QStringLiteral("invite_user_ids"), QJsonArray::fromStringList(inviteUserIds));
  if (rules != nullptr && supportsRules()) body.insert(QStringLiteral("rules"), runRulesToJson(*rules));
  sendRun("POST", QStringLiteral("/runs"), body, std::move(cb));
}

void RunClient::joinRun(const QString& runId, const QString& gameId, RunCallback cb) {
  sendRun("POST", QStringLiteral("/runs/%1/join").arg(enc(runId)), QJsonObject{{QStringLiteral("game_id"), gameId}}, std::move(cb));
}
void RunClient::declineRun(const QString& runId, RunCallback cb) {
  sendRun("POST", QStringLiteral("/runs/%1/decline").arg(enc(runId)), QJsonObject(), std::move(cb));
}
void RunClient::leaveRun(const QString& runId, RunCallback cb) {
  sendRun("POST", QStringLiteral("/runs/%1/leave").arg(enc(runId)), QJsonObject(), std::move(cb));
}
void RunClient::setStatus(const QString& runId, const QString& status, RunCallback cb) {
  sendRun("POST", QStringLiteral("/runs/%1/status").arg(enc(runId)), QJsonObject{{QStringLiteral("status"), status}}, std::move(cb));
}
void RunClient::inviteUsers(const QString& runId, const QStringList& userIds, RunCallback cb) {
  sendRun("POST", QStringLiteral("/runs/%1/invite").arg(enc(runId)), QJsonObject{{QStringLiteral("user_ids"), QJsonArray::fromStringList(userIds)}}, std::move(cb));
}

void RunClient::loadUsers(std::function<void(bool, const QList<HubUser>&, const QString&)> cb) {
  send("GET", QStringLiteral("/users"), std::nullopt, [cb = std::move(cb)](const HttpResult& r) {
    QList<HubUser> users;
    if (r.ok())
      for (const QJsonValue& v : r.json().value(QStringLiteral("users")).toArray())
        if (const auto u = parseHubUser(v.toObject())) users.append(*u);
    cb(r.ok(), users, r.ok() ? QString() : messageOf(r));
  });
}

// ------------------------------------------------------------------ WSS

void RunClient::applyRunJson(const QJsonObject& o) {
  if (const auto run = parseRun(o)) applyRun(*run);
}

void RunClient::applyRun(const RunInfo& incoming) {
  ensureCacheLoaded();
  const QString me = myUserId();
  auto it = runs_.find(incoming.runId);
  const bool isNew = it == runs_.end();
  if (isNew) {
    auto d = std::make_shared<RunData>();
    d->lastSeq = cachedSeq_.value(incoming.runId, 0);
    d->events = cachedEvents_.value(incoming.runId);
    for (const RunEvent& e : d->events) d->clientIds.insert(e.clientId);
    if (!d->events.isEmpty()) d->lastSeq = (std::max)(d->lastSeq, d->events.last().seq);
    d->knownSeq = d->lastSeq;
    it = runs_.insert(incoming.runId, d);
  }
  RunData& d = **it;
  RunInfo next = incoming;
  if (isNew) {
    // States from the disk cache fill in what the Hub's answer does not carry newer (revisions only ever grow).
    const QJsonObject cached = cachedStates_.value(incoming.runId);
    for (RunParticipant& p : next.participants) {
      const QJsonObject byPlugin = cached.value(p.userId).toObject();
      for (auto c = byPlugin.begin(); c != byPlugin.end(); ++c) {
        const QJsonObject so = c.value().toObject();
        const qint64 rev = static_cast<qint64>(so.value(QStringLiteral("state_rev")).toDouble(0));
        if (rev > p.stateOf(c.key()).rev) p.states[c.key()] = {so.value(QStringLiteral("state")).toObject(), rev, QString()};
      }
    }
  }
  const RunInfo old = d.info;
  QList<QPair<QString, QString>> stateChangedFor;  // (user id, plugin id)
  if (!isNew) {
    // A newer state (run_state arrived before this run_updated was built) wins; my own unsent value always wins.
    for (RunParticipant& p : next.participants) {
      const RunParticipant* o = old.participant(p.userId);
      QSet<QString> plugins;
      for (auto it2 = p.states.begin(); it2 != p.states.end(); ++it2) plugins.insert(it2.key());
      if (o != nullptr)
        for (auto it2 = o->states.begin(); it2 != o->states.end(); ++it2) plugins.insert(it2.key());
      for (const QString& pid : std::as_const(plugins)) {
        const RunPluginState have = o != nullptr ? o->stateOf(pid) : RunPluginState();
        RunPluginState& cur = p.states[pid];
        const auto own = d.own.find(pid);
        const bool mineOpen = p.userId == me && own != d.own.end() && (own->second.dirty || own->second.inFlight);
        if (o == nullptr) {
          if (!cur.state.isEmpty()) stateChangedFor << qMakePair(p.userId, pid);
        } else if (mineOpen || have.rev > cur.rev) {
          const qint64 rev = (std::max)(have.rev, cur.rev);
          cur = have;
          cur.rev = rev;
        } else if (have.state != cur.state || have.rev != cur.rev) {
          stateChangedFor << qMakePair(p.userId, pid);
        }
        if (cur.state.isEmpty() && cur.rev == 0 && cur.updatedAt.isEmpty()) p.states.remove(pid);
      }
    }
  } else {
    for (const RunParticipant& p : next.participants)
      for (auto it2 = p.states.begin(); it2 != p.states.end(); ++it2)
        if (!it2->state.isEmpty()) stateChangedFor << qMakePair(p.userId, it2.key());
  }
  d.info = next;
  d.appliedSerial = ++applySerial_;
  d.knownSeq = (std::max)(d.knownSeq, next.lastSeq);

  bool partsChanged = old.participants.size() != next.participants.size();
  for (int i = 0; !partsChanged && i < next.participants.size(); ++i) {
    const RunParticipant& a = old.participants[i];
    const RunParticipant& b = next.participants[i];
    partsChanged = a.userId != b.userId || a.status != b.status || a.gameId != b.gameId || a.color != b.color || a.userName != b.userName;
  }
  emit runChanged(next.runId);
  if (!isNew && old.status != next.status) emit statusChanged(next.runId, next.status);
  if (isNew || partsChanged) emit participantsChanged(next.runId);
  for (const auto& u : std::as_const(stateChangedFor)) emit stateChanged(next.runId, u.first, u.second);
  if (!stateChangedFor.isEmpty()) markCacheDirty(d);
  emit runsChanged();

  const RunParticipant* mine = next.participant(me);
  if (mine != nullptr && (mine->status == QLatin1String("joined") || mine->status == QLatin1String("left")) && d.lastSeq < d.knownSeq)
    scheduleCatchUp(next.runId);
  // Writes that waited for a run that could not take them are not retried; a run that accepts writes again resumes them.
  if (next.acceptsWrites()) {
    std::vector<QString> dirty;
    for (const auto& [pid, os] : d.own)
      if (os.dirty) dirty.push_back(pid);
    for (const QString& pid : dirty) flushState(next.runId, pid);
    if (!d.queue.isEmpty()) flushEvents(next.runId);
    flushBoards(next.runId);
  }
}

void RunClient::onRunState(const QJsonObject& payload) {
  RunData* d = data(payload.value(QStringLiteral("run_id")).toString());
  if (d == nullptr) return;
  const QString userId = payload.value(QStringLiteral("user_id")).toString();
  RunParticipant* p = d->info.participant(userId);
  if (p == nullptr) return;
  const QString pluginId = payload.value(QStringLiteral("plugin_id")).toString();
  if (pluginId.isEmpty() || !d->info.hasPlugin(pluginId)) return;
  const qint64 rev = static_cast<qint64>(payload.value(QStringLiteral("state_rev")).toDouble(0));
  RunPluginState& ps = p->states[pluginId];
  if (rev <= ps.rev) return;  // older or already known
  const auto own = d->own.find(pluginId);
  if (userId == myUserId() && own != d->own.end() && (own->second.dirty || own->second.inFlight)) {
    ps.rev = rev;  // another device of mine wrote; my unsent value wins and goes out on this revision
    return;
  }
  ps.rev = rev;
  ps.state = payload.value(QStringLiteral("state")).toObject();
  markCacheDirty(*d);
  emit stateChanged(d->info.runId, userId, pluginId);
}

void RunClient::onRunEvent(const QJsonObject& payload) {
  RunData* d = data(payload.value(QStringLiteral("run_id")).toString());
  if (d == nullptr) return;
  const auto e = parseRunEvent(payload.value(QStringLiteral("event")).toObject());
  if (!e) return;
  acceptEvent(*d, *e, false);
}

void RunClient::noteSeq(RunData& d, qint64 seq) {
  if (seq > d.knownSeq) d.knownSeq = seq;
  if (seq > d.lastSeq) scheduleCatchUp(d.info.runId);
}

bool RunClient::acceptEvent(RunData& d, const RunEvent& e, bool contiguousPage) {
  if (e.seq <= d.lastSeq || d.clientIds.contains(e.clientId)) return false;  // dedup: seen via WSS, REST or catch-up
  if (!contiguousPage && e.seq != d.lastSeq + 1) {
    noteSeq(d, e.seq);  // a gap: the missing events come with the REST page
    return false;
  }
  d.events.append(e);
  d.clientIds.insert(e.clientId);
  d.lastSeq = e.seq;
  d.knownSeq = (std::max)(d.knownSeq, e.seq);
  while (d.events.size() > kEventCache) {
    d.clientIds.remove(d.events.first().clientId);
    d.events.removeFirst();
  }
  markCacheDirty(d);
  emit eventAdded(d.info.runId, e);
  return true;
}

void RunClient::scheduleCatchUp(const QString& runId) {
  RunData* d = data(runId);
  if (d == nullptr || d->catchUpScheduled || d->paging) return;
  d->catchUpScheduled = true;
  const quint64 gen = gen_;
  QTimer::singleShot(catchUpDelayMs_, this, [this, runId, gen]() {
    if (gen != gen_) return;
    RunData* r = data(runId);
    if (r == nullptr) return;
    r->catchUpScheduled = false;
    if (r->lastSeq < r->knownSeq) pageEvents(runId, kMaxPages);
  });
}

void RunClient::catchUp(const QString& runId) {
  RunData* d = data(runId);
  if (d == nullptr || d->paging) return;
  if (d->knownSeq <= d->lastSeq) d->knownSeq = d->lastSeq + 1;  // ask once: the Hub's answer says where it ends
  pageEvents(runId, kMaxPages);
}

void RunClient::pageEvents(const QString& runId, int pagesLeft) {
  RunData* d = data(runId);
  if (d == nullptr || d->paging || pagesLeft <= 0 || !supported()) return;
  d->paging = true;
  const qint64 after = d->lastSeq;
  send("GET", QStringLiteral("/runs/%1/events?after=%2&limit=%3").arg(enc(runId)).arg(after).arg(kPageSize), std::nullopt,
       [this, runId, after, pagesLeft](const HttpResult& r) {
         RunData* x = data(runId);
         if (x == nullptr) return;
         x->paging = false;
         if (!r.ok()) {
           qCWarning(lcRuns) << "events page failed:" << messageOf(r);
           if (transient(r)) {
             x->catchUpScheduled = false;
             QTimer::singleShot(retryDelayMs(1), this, [this, runId, gen = gen_]() {
               if (gen == gen_) scheduleCatchUp(runId);
             });
           }
           return;
         }
         const QJsonObject o = r.json();
         const QJsonArray arr = o.value(QStringLiteral("events")).toArray();
         int got = 0;
         for (const QJsonValue& v : arr) {
           if (const auto e = parseRunEvent(v.toObject())) {
             if (e->seq > x->lastSeq) acceptEvent(*x, *e, true);
             ++got;
           }
         }
         const qint64 hubLast = static_cast<qint64>(o.value(QStringLiteral("last_seq")).toDouble(0));
         if (hubLast > x->knownSeq) x->knownSeq = hubLast;
         if (x->lastSeq < x->knownSeq && got > 0 && x->lastSeq > after) pageEvents(runId, pagesLeft - 1);
       });
}

// ------------------------------------------------------------------ writes of the game

int RunClient::retryDelayMs(int failures) const {
  const int shift = (std::min)((std::max)(failures, 1) - 1, 5);
  return (std::min)(retryBaseMs_ << shift, kMaxRetryMs);
}

QString RunClient::setOwnState(const QString& runId, const QString& pluginId, const QJsonObject& state) {
  RunData* d = data(runId);
  if (d == nullptr) return QStringLiteral("this run is not available");
  RunParticipant* me = d->info.participant(myUserId());
  if (me == nullptr || me->status != QLatin1String("joined")) return QStringLiteral("you have not joined this run");
  if (!d->info.hasPlugin(pluginId)) return QStringLiteral("plugin %1 is not part of this run").arg(pluginId);
  if (!d->info.acceptsWrites()) return QStringLiteral("the run is %1; state can only be written while it is starting or active").arg(d->info.status);
  if (compact(state).size() > kRunMaxStateBytes) return QStringLiteral("state is larger than 64 KiB");
  OwnState& os = d->own[pluginId];
  os.desired = state;
  os.dirty = true;
  me->states[pluginId].state = state;
  emit stateChanged(d->info.runId, me->userId, pluginId);
  flushState(d->info.runId, pluginId);
  return {};
}

void RunClient::flushState(const QString& runId, const QString& pluginId) {
  RunData* d = data(runId);
  if (d == nullptr) return;
  const auto found = d->own.find(pluginId);
  if (found == d->own.end()) return;
  OwnState& os = found->second;
  if (os.inFlight || !os.dirty || !d->info.acceptsWrites() || !supported()) return;
  const qint64 wait = os.lastSendMs + stateIntervalMs_ - nowMs();
  if (wait > 0) {
    if (!os.timerScheduled) {
      os.timerScheduled = true;
      QTimer::singleShot(static_cast<int>(wait), this, [this, runId, pluginId, gen = gen_]() {
        if (gen != gen_) return;
        if (RunData* r = data(runId)) r->own[pluginId].timerScheduled = false;
        flushState(runId, pluginId);
      });
    }
    return;
  }
  const RunParticipant* me = d->info.participant(myUserId());
  const qint64 baseRev = me != nullptr ? me->stateOf(pluginId).rev : 0;
  os.dirty = false;
  os.inFlight = true;
  os.lastSendMs = nowMs();
  send("PUT", QStringLiteral("/runs/%1/state/%2").arg(enc(runId), enc(pluginId)),
       QJsonObject{{QStringLiteral("base_rev"), static_cast<double>(baseRev)}, {QStringLiteral("state"), os.desired}},
       [this, runId, pluginId](const HttpResult& r) {
         RunData* x = data(runId);
         if (x == nullptr) return;
         OwnState& o = x->own[pluginId];
         o.inFlight = false;
         RunParticipant* me = x->info.participant(myUserId());
         const auto retryLater = [&]() {
           o.dirty = true;
           ++o.failures;
           if (!o.timerScheduled) {
             o.timerScheduled = true;
             QTimer::singleShot(retryDelayMs(o.failures), this, [this, runId, pluginId, gen = gen_]() {
               if (gen != gen_) return;
               if (RunData* q = data(runId)) q->own[pluginId].timerScheduled = false;
               flushState(runId, pluginId);
             });
           }
         };
         if (r.ok()) {
           o.failures = 0;
           const qint64 rev = static_cast<qint64>(r.json().value(QStringLiteral("state_rev")).toDouble(0));
           if (me != nullptr && rev > me->stateOf(pluginId).rev) me->states[pluginId].rev = rev;
           markCacheDirty(*x);
           if (o.dirty) flushState(runId, pluginId);
         } else if (r.status == 409 && r.apiErrorCode == QLatin1String("state_conflict")) {
           // Someone (another device of mine) wrote in between: adopt the Hub's revision and send the latest local value.
           const qint64 rev = static_cast<qint64>(r.json().value(QStringLiteral("state_rev")).toDouble(-1));
           if (me != nullptr && rev >= 0) me->states[pluginId].rev = rev;
           o.dirty = true;
           o.failures = 0;
           flushState(runId, pluginId);
         } else if (transient(r)) {
           retryLater();
         } else {
           o.failures = 0;
           o.dirty = false;
           qCWarning(lcRuns) << "state write refused:" << r.status << r.apiErrorCode;
           emit writeFailed(runId, QStringLiteral("state"), r.apiErrorCode, messageOf(r));
         }
       });
}

QString RunClient::postEvent(const QString& runId, const QString& pluginId, const QString& source, const QString& type, const QString& text,
                             const QJsonObject& payload, QString* clientIdOut) {
  RunData* d = data(runId);
  if (d == nullptr) return QStringLiteral("this run is not available");
  const RunParticipant* me = d->info.participant(myUserId());
  if (me == nullptr || me->status != QLatin1String("joined")) return QStringLiteral("you have not joined this run");
  if (!d->info.acceptsWrites()) return QStringLiteral("the run is %1; events can only be written while it is starting or active").arg(d->info.status);
  if (source != QLatin1String("automatic") && source != QLatin1String("manual")) return QStringLiteral("source must be \"automatic\" or \"manual\"");
  if (!validEventType(type)) return QStringLiteral("type must match [a-z0-9_.-]{1,64}");
  if (text.size() > kRunMaxEventText) return QStringLiteral("text is longer than 280 characters");
  if (compact(payload).size() > kRunMaxEventPayloadBytes) return QStringLiteral("payload is larger than 4 KiB");
  // The events the app writes itself (cheats, memory writes, state loads; run_rules_v1) come without a plugin, or name the plugin
  // that caused them when it is part of the run.
  static const QSet<QString> coreTypes = {QStringLiteral("cheat.enabled"), QStringLiteral("cheat.disabled"), QStringLiteral("memory.write"),
                                          QStringLiteral("state.loaded")};
  const bool core = coreTypes.contains(type);
  if (!core && !d->info.hasPlugin(pluginId)) return QStringLiteral("plugin %1 is not part of this run").arg(pluginId);
  if (d->queue.size() >= kMaxQueuedEvents) return QStringLiteral("too many events waiting to be sent");
  const QString clientId = clientIdOut != nullptr && !clientIdOut->isEmpty() ? *clientIdOut : QUuid::createUuid().toString(QUuid::WithoutBraces);
  PendingEvent pe;
  pe.body = {{QStringLiteral("client_id"), clientId}, {QStringLiteral("source"), source}, {QStringLiteral("type"), type},
             {QStringLiteral("text"), text},          {QStringLiteral("payload"), payload}};
  if (!pluginId.isEmpty() && d->info.hasPlugin(pluginId)) pe.body.insert(QStringLiteral("plugin_id"), pluginId);
  d->queue.append(pe);
  if (clientIdOut != nullptr) *clientIdOut = clientId;
  flushEvents(runId);
  return {};
}

void RunClient::flushEvents(const QString& runId) {
  RunData* d = data(runId);
  if (d == nullptr || d->eventInFlight || d->queue.isEmpty() || !supported()) return;
  if (!d->info.acceptsWrites()) return;  // resumes when the run accepts writes again (run_updated)
  d->eventInFlight = true;
  const QJsonObject body = d->queue.first().body;
  const QString clientId = body.value(QStringLiteral("client_id")).toString();
  send("POST", QStringLiteral("/runs/%1/events").arg(enc(runId)), body, [this, runId, clientId](const HttpResult& r) {
    RunData* x = data(runId);
    if (x == nullptr) return;
    x->eventInFlight = false;
    if (x->queue.isEmpty() || x->queue.first().body.value(QStringLiteral("client_id")).toString() != clientId) return;
    if (r.ok()) {
      x->queue.removeFirst();
      noteSeq(*x, static_cast<qint64>(r.json().value(QStringLiteral("seq")).toDouble(0)));
      flushEvents(runId);
    } else if (transient(r)) {
      // Same client_id again: the Hub answers 200 with the original seq when the first try did arrive.
      const int attempts = ++x->queue.first().attempts;
      if (!x->eventTimerScheduled) {
        x->eventTimerScheduled = true;
        QTimer::singleShot(retryDelayMs(attempts), this, [this, runId, gen = gen_]() {
          if (gen != gen_) return;
          if (RunData* q = data(runId)) q->eventTimerScheduled = false;
          flushEvents(runId);
        });
      }
    } else {
      x->queue.removeFirst();
      qCWarning(lcRuns) << "event refused:" << r.status << r.apiErrorCode;
      emit writeFailed(runId, QStringLiteral("event"), r.apiErrorCode, messageOf(r));
      flushEvents(runId);
    }
  });
}

QString RunClient::setBoard(const QString& runId, const QString& pluginId, const QJsonObject& tree) {
  RunData* d = data(runId);
  if (d == nullptr) return QStringLiteral("this run is not available");
  const RunParticipant* me = d->info.participant(myUserId());
  if (me == nullptr || me->status != QLatin1String("joined")) return QStringLiteral("you have not joined this run");
  if (!d->info.acceptsWrites()) return QStringLiteral("the run is %1; the board can only be written while it is starting or active").arg(d->info.status);
  if (!d->info.hasPlugin(pluginId)) return QStringLiteral("plugin %1 is not part of this run").arg(pluginId);
  if (compact(tree).size() > kRunMaxBoardBytes) return QStringLiteral("board is larger than 64 KiB");
  Board& b = d->boards[pluginId];
  b.tree = tree;
  b.dirty = true;
  flushBoards(runId);
  return {};
}

void RunClient::flushBoards(const QString& runId) {
  RunData* d = data(runId);
  if (d == nullptr || !d->info.acceptsWrites() || !supported()) return;
  for (auto& [pluginId, b] : d->boards) {
    if (!b.dirty || b.inFlight) continue;
    const qint64 wait = b.lastSendMs + boardIntervalMs_ - nowMs();
    if (wait > 0) {
      QTimer::singleShot(static_cast<int>(wait), this, [this, runId, gen = gen_]() {
        if (gen == gen_) flushBoards(runId);
      });
      continue;
    }
    b.dirty = false;
    b.inFlight = true;
    b.lastSendMs = nowMs();
    const QString pid = pluginId;
    send("PUT", QStringLiteral("/runs/%1/boards/%2").arg(enc(runId), enc(pid)), QJsonObject{{QStringLiteral("tree"), b.tree}}, [this, runId, pid](const HttpResult& r) {
      RunData* x = data(runId);
      if (x == nullptr) return;
      Board& bb = x->boards[pid];
      bb.inFlight = false;
      if (r.ok()) {
        bb.failures = 0;
      } else if (transient(r)) {
        bb.dirty = true;
        const int delay = retryDelayMs(++bb.failures);
        bb.lastSendMs = nowMs() + delay - boardIntervalMs_;
      } else {
        bb.failures = 0;
        qCWarning(lcRuns) << "board write refused:" << r.status << r.apiErrorCode;
        emit writeFailed(runId, QStringLiteral("board"), r.apiErrorCode, messageOf(r));
      }
      flushBoards(runId);
    });
  }
}

QJsonArray RunClient::helloRuns() {
  ensureCacheLoaded();
  QHash<QString, qint64> seqs = cachedSeq_;
  for (auto it = runs_.begin(); it != runs_.end(); ++it) {
    const RunParticipant* p = it.value()->info.participant(myUserId());
    if (p != nullptr && p->status == QLatin1String("joined")) seqs[it.key()] = it.value()->lastSeq;
  }
  QJsonArray out;
  QStringList ids = seqs.keys();
  ids.sort();
  for (const QString& id : std::as_const(ids))
    out.append(QJsonObject{{QStringLiteral("run_id"), id}, {QStringLiteral("last_seq"), static_cast<double>(seqs.value(id))}});
  return out;
}

}  // namespace uzume
