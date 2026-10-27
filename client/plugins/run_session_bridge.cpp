#include "run_session_bridge.h"

#include <QJsonDocument>
#include <QMetaObject>
#include <QUuid>
#include <algorithm>

namespace uzume::plugins {

namespace {
constexpr int kMaxEvents = 500;
constexpr int kMaxQueuedEvents = 200;
constexpr int kMaxNotes = 2000;
QJsonValue nullable(const QString& s) { return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s); }
}  // namespace

std::shared_ptr<RunSessionBridge> RunSessionBridge::create(RunClient* client, const QString& runId) {
  return std::shared_ptr<RunSessionBridge>(new RunSessionBridge(client, runId), [](RunSessionBridge* b) { b->deleteLater(); });
}

RunSessionBridge::RunSessionBridge(RunClient* client, const QString& runId) : client_(client), runId_(runId.toLower()) {
  connect(client, &RunClient::runChanged, this, [this](const QString& id) {
    if (id == runId_) rebuild();
  });
  connect(client, &RunClient::statusChanged, this, [this](const QString& id, const QString& status) {
    if (id != runId_) return;
    const QString previous = std::exchange(notifiedStatus_, status);  // status_ already follows runChanged, which comes first
    rebuild();
    note(QStringLiteral("status"), {{QStringLiteral("status"), status}, {QStringLiteral("previous"), previous}});
  });
  connect(client, &RunClient::participantsChanged, this, [this](const QString& id) {
    if (id != runId_) return;
    rebuild();
    note(QStringLiteral("participants"), {{QStringLiteral("participants"), current().value(QStringLiteral("participants"))}});
  });
  connect(client, &RunClient::stateChanged, this, [this](const QString& id, const QString& userId, const QString& pluginId) {
    if (id != runId_) return;
    QJsonObject before;
    {
      std::lock_guard<std::mutex> l(m_);
      before = states_.value(userId).toObject().value(pluginId).toObject();
    }
    rebuild();
    bool found = false;
    const QJsonObject after = stateOf(userId, pluginId, &found);
    if (found && after != before)
      note(QStringLiteral("state"), {{QStringLiteral("user_id"), userId}, {QStringLiteral("plugin_id"), pluginId}, {QStringLiteral("state"), after}});
  });
  connect(client, &RunClient::eventAdded, this, [this](const QString& id, const RunEvent& e) {
    if (id != runId_) return;
    const QJsonObject j = eventJson(e);
    {
      std::lock_guard<std::mutex> l(m_);
      events_.append(j);
      while (events_.size() > kMaxEvents) events_.removeFirst();
    }
    note(QStringLiteral("event"), j);
  });
  for (const RunEvent& e : client->events(runId_, 0, kMaxEvents)) events_.append(eventJson(e));
  rebuild();
  {
    std::lock_guard<std::mutex> l(m_);
    notes_.clear();
    notifiedStatus_ = status_;
  }
}

QJsonObject RunSessionBridge::eventJson(const RunEvent& e) {
  return {{QStringLiteral("seq"), static_cast<double>(e.seq)},
          {QStringLiteral("time"), e.createdAt},
          {QStringLiteral("user_id"), nullable(e.userId)},
          {QStringLiteral("user_name"), e.userName},
          {QStringLiteral("plugin_id"), nullable(e.pluginId)},
          {QStringLiteral("source"), e.source},
          {QStringLiteral("type"), e.type},
          {QStringLiteral("text"), e.text},
          {QStringLiteral("payload"), e.payload},
          {QStringLiteral("client_id"), e.clientId}};
}

void RunSessionBridge::rebuild() {
  if (!client_) return;
  const auto run = client_->run(runId_);
  std::lock_guard<std::mutex> l(m_);
  if (!run) {
    current_ = QJsonObject();
    joined_ = writable_ = false;
    return;
  }
  me_ = client_->myUserId();
  QJsonArray plugins;
  pluginIds_.clear();
  for (const RunPluginRef& p : run->plugins) {
    plugins.append(QJsonObject{{QStringLiteral("id"), p.pluginId}, {QStringLiteral("version"), p.version}});
    pluginIds_.insert(p.pluginId);
  }
  QJsonArray parts;
  QJsonObject states;
  for (const RunParticipant& p : run->participants) {
    parts.append(QJsonObject{{QStringLiteral("user_id"), p.userId},
                             {QStringLiteral("name"), p.userName},
                             {QStringLiteral("color"), p.color < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(p.color)},
                             {QStringLiteral("status"), p.status},
                             {QStringLiteral("game_id"), nullable(p.gameId)}});
    if (!p.variantKind.isEmpty()) {
      QJsonObject last = parts.last().toObject();
      last.insert(QStringLiteral("variant_kind"), p.variantKind);
      parts.replace(parts.size() - 1, last);
    }
    QJsonObject byPlugin;
    for (const RunPluginRef& ref : run->plugins) {
      QJsonObject st = p.stateOf(ref.pluginId).state;
      if (p.userId == me_) {
        const auto pend = pendingStates_.find(ref.pluginId);
        st = pend != pendingStates_.end() ? pend->second : client_->ownState(runId_, ref.pluginId);
      }
      byPlugin.insert(ref.pluginId, st);
    }
    states.insert(p.userId, byPlugin);
  }
  const RunParticipant* mine = run->participant(me_);
  joined_ = mine != nullptr && mine->status == QLatin1String("joined");
  writable_ = run->acceptsWrites();
  status_ = run->status;
  states_ = states;
  const bool rulesDiffer = !(rules_ == run->rules);
  rules_ = run->rules;
  current_ = {{QStringLiteral("rules"), runRulesToJson(run->rules)}, {QStringLiteral("id"), run->runId},     {QStringLiteral("name"), run->name}, {QStringLiteral("status"), run->status},
              {QStringLiteral("me"), me_},            {QStringLiteral("settings"), run->settings}, {QStringLiteral("plugins"), plugins},
              {QStringLiteral("participants"), parts}};
  // Public facts of the randomizer only (allow-list): never seed, settings or base_sha256.
  if (run->randomizerMode() != QLatin1String("off")) {
    const QJsonObject& r = run->randomizer;
    current_.insert(QStringLiteral("randomizer"),
                    QJsonObject{{QStringLiteral("mode"), run->randomizerMode()},
                                {QStringLiteral("preset_id"), r.value(QLatin1String("preset_id")).toString()},
                                {QStringLiteral("tool_version"), r.value(QLatin1String("tool_version")).toString()},
                                {QStringLiteral("allow_own"), r.value(QLatin1String("allow_own")).toBool()}});
  }
  if (rulesDiffer) QMetaObject::invokeMethod(this, [this] { emit rulesChanged(); }, Qt::QueuedConnection);
}

void RunSessionBridge::note(const QString& kind, const QJsonObject& data) {
  std::lock_guard<std::mutex> l(m_);
  if (notes_.size() >= kMaxNotes) notes_.removeFirst();
  notes_.append({kind, data});
}

QList<RunNotification> RunSessionBridge::takeNotifications() {
  std::lock_guard<std::mutex> l(m_);
  return std::exchange(notes_, {});
}

QJsonObject RunSessionBridge::current() const {
  std::lock_guard<std::mutex> l(m_);
  return current_;
}

QJsonObject RunSessionBridge::stateOf(const QString& userId, const QString& pluginId, bool* found) const {
  std::lock_guard<std::mutex> l(m_);
  const QJsonObject byPlugin = states_.value(userId).toObject();
  const bool has = byPlugin.contains(pluginId);
  if (found != nullptr) *found = has;
  return byPlugin.value(pluginId).toObject();
}

QJsonObject RunSessionBridge::states(const QString& pluginId) const {
  std::lock_guard<std::mutex> l(m_);
  QJsonObject out;
  for (auto it = states_.begin(); it != states_.end(); ++it) {
    const QJsonObject byPlugin = it.value().toObject();
    if (byPlugin.contains(pluginId)) out.insert(it.key(), byPlugin.value(pluginId));
  }
  return out;
}

QJsonArray RunSessionBridge::events(qint64 afterSeq, int limit) const {
  std::lock_guard<std::mutex> l(m_);
  QJsonArray out;
  for (const QJsonObject& e : events_) {
    if (static_cast<qint64>(e.value(QStringLiteral("seq")).toDouble()) <= afterSeq) continue;
    out.append(e);
    if (limit > 0 && out.size() >= limit) break;
  }
  return out;
}

QString RunSessionBridge::refusal(const QString& what) const {
  if (current_.isEmpty()) return QStringLiteral("the run is not available");
  if (!joined_) return QStringLiteral("you have not joined this run");
  if (!writable_)
    return QStringLiteral("the run is %1; %2 can only be written while it is starting or active").arg(status_, what);
  return {};
}

QString RunSessionBridge::setState(const QString& pluginId, const QJsonObject& state) {
  if (QJsonDocument(state).toJson(QJsonDocument::Compact).size() > kRunMaxStateBytes) return QStringLiteral("state is larger than 64 KiB");
  bool queue = false;
  {
    std::lock_guard<std::mutex> l(m_);
    const QString why = refusal(QStringLiteral("state"));
    if (!why.isEmpty()) return why;
    if (!pluginIds_.contains(pluginId)) return QStringLiteral("plugin %1 is not part of this run").arg(pluginId);
    QJsonObject mine = states_.value(me_).toObject();
    mine.insert(pluginId, state);
    states_.insert(me_, mine);  // the plugin reads its own write back at once
    pendingStates_[pluginId] = state;
    queue = !statesQueued_;
    statesQueued_ = true;
  }
  if (queue) QMetaObject::invokeMethod(this, [this] { flushStates(); }, Qt::QueuedConnection);
  return {};
}

void RunSessionBridge::flushStates() {
  std::map<QString, QJsonObject> pending;
  {
    std::lock_guard<std::mutex> l(m_);
    pending = std::exchange(pendingStates_, {});
    statesQueued_ = false;
  }
  if (!client_) return;
  for (const auto& [pluginId, st] : pending) client_->setOwnState(runId_, pluginId, st);
}

QString RunSessionBridge::postEvent(const QString& pluginId, const QString& source, const QString& type, const QString& text,
                                    const QJsonObject& payload, QString* clientId) {
  {
    std::lock_guard<std::mutex> l(m_);
    const QString why = refusal(QStringLiteral("events"));
    if (!why.isEmpty()) return why;
    if (!pluginIds_.contains(pluginId)) return QStringLiteral("plugin %1 is not part of this run").arg(pluginId);
  }
  if (source != QLatin1String("automatic") && source != QLatin1String("manual")) return QStringLiteral("source must be \"automatic\" or \"manual\"");
  if (text.size() > kRunMaxEventText) return QStringLiteral("text is longer than 280 characters");
  if (QJsonDocument(payload).toJson(QJsonDocument::Compact).size() > kRunMaxEventPayloadBytes) return QStringLiteral("payload is larger than 4 KiB");
  if (eventsQueued_.load() >= kMaxQueuedEvents) return QStringLiteral("too many events waiting to be sent");
  const QString cid = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (clientId != nullptr) *clientId = cid;
  ++eventsQueued_;
  QMetaObject::invokeMethod(
      this,
      [this, pluginId, source, type, text, payload, cid] {
        --eventsQueued_;
        QString id = cid;
        if (client_) {
          const QString err = client_->postEvent(runId_, pluginId, source, type, text, payload, &id);
          if (!err.isEmpty()) emit client_->writeFailed(runId_, QStringLiteral("event"), QStringLiteral("refused"), err);
        }
      },
      Qt::QueuedConnection);
  return {};
}

QString RunSessionBridge::postCoreEvent(const QString& type, const QString& text, const QJsonObject& payload) {
  {
    std::lock_guard<std::mutex> l(m_);
    const QString why = refusal(QStringLiteral("events"));
    if (!why.isEmpty()) return why;
  }
  if (text.size() > kRunMaxEventText) return QStringLiteral("text is longer than 280 characters");
  if (QJsonDocument(payload).toJson(QJsonDocument::Compact).size() > kRunMaxEventPayloadBytes) return QStringLiteral("payload is larger than 4 KiB");
  if (eventsQueued_.load() >= kMaxQueuedEvents) return QStringLiteral("too many events waiting to be sent");
  ++eventsQueued_;
  QMetaObject::invokeMethod(
      this,
      [this, type, text, payload] {
        --eventsQueued_;
        if (!client_) return;
        const QString pluginId = payload.value(QStringLiteral("plugin_id")).toString();
        const QString err = client_->postEvent(runId_, pluginId, QStringLiteral("automatic"), type, text, payload);
        if (!err.isEmpty()) emit client_->writeFailed(runId_, QStringLiteral("event"), QStringLiteral("refused"), err);
      },
      Qt::QueuedConnection);
  return {};
}

QString RunSessionBridge::setBoard(const QString& pluginId, const QJsonObject& tree) {
  if (QJsonDocument(tree).toJson(QJsonDocument::Compact).size() > kRunMaxBoardBytes) return QStringLiteral("board is larger than 64 KiB");
  bool queue = false;
  {
    std::lock_guard<std::mutex> l(m_);
    const QString why = refusal(QStringLiteral("the board"));
    if (!why.isEmpty()) return why;
    if (!pluginIds_.contains(pluginId)) return QStringLiteral("plugin %1 is not part of this run").arg(pluginId);
    pendingBoards_[pluginId] = tree;
    queue = !boardsQueued_;
    boardsQueued_ = true;
  }
  if (queue) QMetaObject::invokeMethod(this, [this] { flushBoards(); }, Qt::QueuedConnection);
  return {};
}

void RunSessionBridge::flushBoards() {
  std::map<QString, QJsonObject> boards;
  {
    std::lock_guard<std::mutex> l(m_);
    boards = std::exchange(pendingBoards_, {});
    boardsQueued_ = false;
  }
  if (!client_) return;
  for (const auto& [pluginId, tree] : boards) client_->setBoard(runId_, pluginId, tree);
}

}  // namespace uzume::plugins
