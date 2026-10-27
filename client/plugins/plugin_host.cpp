#include "plugin_host.h"

#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <chrono>

#include "plugin_engine.h"

namespace uzume::plugins {

// Lives on the plugin thread; owns the engine and drives it from a timer.
class HostWorker : public QObject {
  Q_OBJECT
 public:
  HostWorker(ConsoleStore& store, const std::atomic<int>& tickHz) : m_store(store), m_tickHz(tickHz) {}
  ~HostWorker() override = default;

  // Called on the plugin thread.
  void initOnThread() {
    m_engine = std::make_unique<PluginEngine>(m_store, m_tickHz, [this](const QString& id) { emit statusChanged(id); });
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(4);
    connect(m_timer, &QTimer::timeout, this, &HostWorker::onTimer);
  }
  void shutdownOnThread() {
    if (m_timer) m_timer->stop();
    m_engine.reset();
  }
  void start(const GameContext& ctx, const std::shared_ptr<MemoryProvider>& mem, const QList<PluginInfo>& list, const HostLimits& lim) {
    m_engine->start(ctx, mem, list, lim);
    m_timer->start();
    flushConsole(true);
    flushUi(true);
  }
  void stop() {
    m_timer->stop();
    m_engine->stop();
    flushConsole(true);
    flushUi(true);
  }
  void reload(const QString& id) {
    m_engine->reload(id);
    flushConsole(true);
    flushUi(true);
  }
  void sendAction(const QString& id, const QString& slot, const QString& action, const Value& value) {
    if (!m_engine) return;
    m_engine->deliverAction(id, slot, action, value);
    flushUi(false);
  }
  void hostEvent(const QString& name, const Value& value, const QString& target) {
    if (m_engine) m_engine->postHostEvent(name, value, target);
  }
  void appEvent(const QString& name, const Value& value, const QString& target) {
    if (m_engine) m_engine->postAppEvent(name, value, target);
  }
  void deliverFile(const QString& id, uint64_t reqId, const std::string& content, const std::string& name, const std::string& error) {
    if (!m_engine) return;
    m_engine->deliverFile(id, reqId, content, name, error);
    flushUi(false);
  }
  void clearSlot(const QString& slot) {
    if (!m_engine) return;
    m_engine->clearSlotEverywhere(slot);
    flushUi(true);
  }
  ProbeResult callProvided(const QString& id, const QString& iface, const QString& fn, const std::vector<Value>& args) {
    if (!m_engine) return failed(QStringLiteral("plugin_not_running"));
    ProbeResult r = m_engine->probeCall(id, iface.toStdString(), fn.toStdString(), args);
    flushConsole(true);
    flushUi(true);
    return r;
  }
  ProbeResult evalChunk(const QString& id, const QString& code) {
    if (!m_engine) return failed(QStringLiteral("plugin_not_running"));
    ProbeResult r = m_engine->probeEval(id, code.toStdString());
    flushConsole(true);
    flushUi(true);
    return r;
  }

 signals:
  void statusChanged(const QString& id);
  void consoleChanged(const QString& id);
  void uiChanged(const QString& id, const QString& what);

 private:
  static ProbeResult failed(const QString& why) {
    ProbeResult r;
    r.error = why;
    return r;
  }
  void onTimer() {
    m_engine->iterate();
    flushConsole(false);
    flushUi(false);
  }
  // Dirty slots are emitted at most every 33 ms; everything written in between collapses into one emission.
  void flushUi(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - m_lastUiFlush < std::chrono::milliseconds(33)) return;
    m_lastUiFlush = now;
    for (const auto& d : m_store.ui.takeDirty()) emit uiChanged(d.first, d.second);
  }
  void flushConsole(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - m_lastFlush < std::chrono::milliseconds(100)) return;
    m_lastFlush = now;
    for (const QString& id : m_store.takeDirty()) emit consoleChanged(id);
  }

  ConsoleStore& m_store;
  const std::atomic<int>& m_tickHz;
  std::unique_ptr<PluginEngine> m_engine;
  QTimer* m_timer = nullptr;
  std::chrono::steady_clock::time_point m_lastFlush;
  std::chrono::steady_clock::time_point m_lastUiFlush;
};

PluginHost::PluginHost(QObject* parent) : QObject(parent), m_store(std::make_unique<ConsoleStore>()) {
  m_thread = new QThread(this);
  m_thread->setObjectName(QStringLiteral("uzume-plugins"));
  m_worker = new HostWorker(*m_store, m_tickHz);
  m_worker->moveToThread(m_thread);
  connect(m_worker, &HostWorker::statusChanged, this, &PluginHost::statusChanged);
  connect(m_worker, &HostWorker::consoleChanged, this, &PluginHost::consoleChanged);
  connect(m_worker, &HostWorker::uiChanged, this, &PluginHost::uiChanged);
  connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
  m_thread->start();
  QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->initOnThread(); }, Qt::BlockingQueuedConnection);
}

PluginHost::~PluginHost() {
  QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->shutdownOnThread(); }, Qt::BlockingQueuedConnection);
  m_thread->quit();
  m_thread->wait();
}

void PluginHost::start(const GameContext& ctx, std::shared_ptr<MemoryProvider> memory, const QList<PluginInfo>& plugins) {
  QStringList ids;
  for (const PluginInfo& p : plugins) ids << p.manifest.id;
  m_store->reset(ids, PluginStatus::Loading);
  const HostLimits lim = m_limits;
  QMetaObject::invokeMethod(m_worker, [w = m_worker, ctx, memory = std::move(memory), plugins, lim] { w->start(ctx, memory, plugins, lim); },
                            Qt::QueuedConnection);
}

void PluginHost::stop() {
  QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->stop(); }, Qt::BlockingQueuedConnection);
}

void PluginHost::reload(const QString& id) {
  QMetaObject::invokeMethod(m_worker, [w = m_worker, id] { w->reload(id); }, Qt::QueuedConnection);
}

void PluginHost::setTickHz(int hz) { m_tickHz = std::clamp(hz, 1, 240); }

void PluginHost::setLimits(const HostLimits& limits) { m_limits = limits; }

void PluginHost::sendAction(const QString& pluginId, const QString& slot, const QString& action, const QVariant& value) {
  const Value v = valueFromVariant(value);
  QMetaObject::invokeMethod(m_worker, [w = m_worker, pluginId, slot, action, v] { w->sendAction(pluginId, slot, action, v); }, Qt::QueuedConnection);
}

void PluginHost::postHostEvent(const QString& name, const QVariant& value, const QString& targetPlugin) {
  const Value v = valueFromVariant(value);
  QMetaObject::invokeMethod(m_worker, [w = m_worker, name, v, targetPlugin] { w->hostEvent(name, v, targetPlugin); }, Qt::QueuedConnection);
}

void PluginHost::postAppEvent(const QString& name, const QVariant& value, const QString& targetPlugin) {
  const Value v = valueFromVariant(value);
  QMetaObject::invokeMethod(m_worker, [w = m_worker, name, v, targetPlugin] { w->appEvent(name, v, targetPlugin); }, Qt::QueuedConnection);
}

void PluginHost::deliverFile(const QString& pluginId, quint64 requestId, const QByteArray& content, const QString& name, const QString& error) {
  const std::string c(content.constData(), static_cast<size_t>(content.size()));
  const std::string n = name.toStdString();
  const std::string e = error.toStdString();
  QMetaObject::invokeMethod(m_worker, [w = m_worker, pluginId, requestId, c, n, e] { w->deliverFile(pluginId, requestId, c, n, e); }, Qt::QueuedConnection);
}

QList<PickRequest> PluginHost::takePickRequests() { return m_store->ui.takePickRequests(); }

void PluginHost::clearSlot(const QString& slot) {
  QMetaObject::invokeMethod(m_worker, [w = m_worker, slot] { w->clearSlot(slot); }, Qt::QueuedConnection);
}

void PluginHost::callProvided(const QString& pluginId, const QString& iface, const QString& fn, const std::vector<Value>& args, ProbeCallback done) {
  QPointer<PluginHost> self(this);
  QMetaObject::invokeMethod(
      m_worker,
      [w = m_worker, self, pluginId, iface, fn, args, done = std::move(done)]() {
        ProbeResult r = w->callProvided(pluginId, iface, fn, args);
        if (self.isNull()) return;
        QMetaObject::invokeMethod(self.data(), [self, done, r = std::move(r)]() mutable { if (self) done(std::move(r)); }, Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}

void PluginHost::evalChunk(const QString& pluginId, const QString& code, ProbeCallback done) {
  QPointer<PluginHost> self(this);
  QMetaObject::invokeMethod(
      m_worker,
      [w = m_worker, self, pluginId, code, done = std::move(done)]() {
        ProbeResult r = w->evalChunk(pluginId, code);
        if (self.isNull()) return;
        QMetaObject::invokeMethod(self.data(), [self, done, r = std::move(r)]() mutable { if (self) done(std::move(r)); }, Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}

UiPluginState PluginHost::uiState(const QString& pluginId) const { return m_store->ui.state(pluginId); }
QVariantMap PluginHost::uiTree(const QString& pluginId, const QString& slot) const { return m_store->ui.tree(pluginId, slot); }
QList<QVariantMap> PluginHost::takeToasts() { return m_store->ui.takeToasts(); }

QList<QPair<QString, QString>> PluginHost::takeOpenRequests() { return m_store->ui.takeOpenRequests(); }
QList<QPair<QString, QString>> PluginHost::takeCloseRequests() { return m_store->ui.takeCloseRequests(); }

QStringList PluginHost::pluginIds() const { return m_store->ids(); }

ConsoleSnapshot PluginHost::consoleSnapshot(const QString& id) const { return m_store->snapshot(id); }

}  // namespace uzume::plugins

#include "plugin_host.moc"
