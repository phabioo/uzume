#include "plugin_engine.h"

#include "plugin_host.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstring>

#include "format_helpers.h"
#include "lauxlib.h"
#include "lua.h"

namespace uzume::plugins {

using Clock = std::chrono::steady_clock;

namespace {
constexpr qint64 kMaxEntryBytes = 4 * 1024 * 1024;
constexpr int kMaxSlowCalls = 5;

QString qs(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
}  // namespace

// ---------------------------------------------------------------- ConsoleStore

void ConsoleStore::reset(const QStringList& ids, PluginStatus status) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_map.clear();
  m_dirty.clear();
  m_order = ids;
  for (const QString& id : ids) {
    ConsoleSnapshot s;
    s.status = status;
    m_map.insert(id, s);
  }
}

void ConsoleStore::setStatus(const QString& id, PluginStatus status, const QString& error) {
  std::lock_guard<std::mutex> lock(m_mutex);
  ConsoleSnapshot& s = m_map[id];
  s.status = status;
  s.error = error;
  if (status != PluginStatus::Running) s.watched.clear();
  if (!m_order.contains(id)) m_order.append(id);
  m_dirty.insert(id);
}

void ConsoleStore::addLog(const QString& id, LogLevel level, const QString& text) {
  std::lock_guard<std::mutex> lock(m_mutex);
  ConsoleSnapshot& s = m_map[id];
  s.log.append({QDateTime::currentMSecsSinceEpoch(), level, text, ++s.lastSeq});
  while (s.log.size() > kLogRing) s.log.removeFirst();
  m_dirty.insert(id);
}

void ConsoleStore::addEvent(const QString& id, const EventLine& line) {
  std::lock_guard<std::mutex> lock(m_mutex);
  ConsoleSnapshot& s = m_map[id];
  s.events.append(line);
  while (s.events.size() > kEventRing) s.events.removeFirst();
  m_dirty.insert(id);
}

bool ConsoleStore::setWatched(const QString& id, const QList<WatchedRow>& rows) {
  std::lock_guard<std::mutex> lock(m_mutex);
  ConsoleSnapshot& s = m_map[id];
  if (s.watched.size() == rows.size()) {
    bool same = true;
    for (qsizetype i = 0; i < rows.size() && same; ++i)
      same = s.watched[i].label == rows[i].label && s.watched[i].address == rows[i].address && s.watched[i].value == rows[i].value;
    if (same) return false;
  }
  s.watched = rows;
  m_dirty.insert(id);
  return true;
}

void ConsoleStore::noteDenied(const QString& id, const QString& permission) {
  std::lock_guard<std::mutex> lock(m_mutex);
  ConsoleSnapshot& s = m_map[id];
  if (s.notGranted.contains(permission)) return;
  s.notGranted.append(permission);
  m_dirty.insert(id);
}

void ConsoleStore::noteError(const QString& id) {
  std::lock_guard<std::mutex> lock(m_mutex);
  ++m_map[id].errorCount;
  m_dirty.insert(id);
}

void ConsoleStore::setReloaded(const QString& id, qint64 ms) {
  std::lock_guard<std::mutex> lock(m_mutex);
  ConsoleSnapshot& s = m_map[id];
  s.reloadMs = ms;
  ++s.reloadCount;
  m_dirty.insert(id);
}

ConsoleSnapshot ConsoleStore::snapshot(const QString& id) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_map.value(id);
}

QStringList ConsoleStore::ids() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_order;
}

QStringList ConsoleStore::takeDirty() {
  std::lock_guard<std::mutex> lock(m_mutex);
  const QStringList out(m_dirty.begin(), m_dirty.end());
  m_dirty.clear();
  return out;
}

// ---------------------------------------------------------------- PluginEngine

PluginEngine::PluginEngine(ConsoleStore& s, const std::atomic<int>& tickHz, std::function<void(const QString&)> statusChanged)
    : store(s), m_tickHz(tickHz), m_statusChanged(std::move(statusChanged)) {
  started = Clock::now();
}

PluginEngine::~PluginEngine() { stop(); }

Plugin* PluginEngine::find(const QString& id) {
  for (auto& p : m_plugins)
    if (p->id() == id) return p.get();
  return nullptr;
}

void PluginEngine::setStatus(Plugin& p, PluginStatus s, const QString& error) {
  p.status = s;
  store.setStatus(p.id(), s, error);
  if (m_statusChanged) m_statusChanged(p.id());
}

void PluginEngine::log(Plugin& p, LogLevel level, const std::string& text) {
  QString t = qs(text);
  if (t.size() > 4000) t = t.left(4000) + QStringLiteral("...");
  if (m_captureLog != nullptr && m_captureLog->size() < 500) m_captureLog->append({QDateTime::currentMSecsSinceEpoch(), level, t});
  store.addLog(p.id(), level, t);
}

void PluginEngine::emitEvent(Plugin& p, const std::string& name, Value value) {
  EventLine line;
  line.timeMs = QDateTime::currentMSecsSinceEpoch();
  line.plugin = p.id();
  line.name = qs(name);
  line.text = qs(shortText(value));
  store.addEvent(p.id(), line);
  QueuedEvent ev{p.id(), name, std::move(value), QString(), 0};
  pushEvent(std::move(ev));
}

bool PluginEngine::eventFits(const QString& from, size_t bytes) const {
  if (events.size() >= kMaxQueuedEventCount || queuedBytes + bytes > kMaxQueuedEventBytes) return false;
  const auto it = queuedBy.find(from);
  return (it == queuedBy.end() ? 0 : it->second) + bytes <= kMaxQueuedEventBytesPerPlugin;
}

void PluginEngine::pushEvent(QueuedEvent ev) {
  ev.bytes = valueBytes(ev.value) + ev.name.size() + sizeof(QueuedEvent);
  queuedBytes += ev.bytes;
  queuedBy[ev.from] += ev.bytes;
  events.push_back(std::move(ev));
}

QueuedEvent PluginEngine::popEvent() {
  QueuedEvent ev = std::move(events.front());
  events.pop_front();
  queuedBytes -= ev.bytes;
  auto it = queuedBy.find(ev.from);
  if (it != queuedBy.end()) {
    it->second -= ev.bytes;
    if (it->second == 0) queuedBy.erase(it);
  }
  return ev;
}

void PluginEngine::syncWatches(Plugin& p) {
  if (!provider) return;
  std::vector<WatchRange> ranges;
  ranges.reserve(p.watches.size());
  for (const auto& kv : p.watches) ranges.push_back(kv.second);
  provider->setWatches(p.id().toStdString(), std::move(ranges));
}

void PluginEngine::releaseState(Plugin& p) {
  // Order matters: the state owns the registry refs, so drop the refs' bookkeeping with it.
  p.subs.clear();
  p.provided.clear();
  p.tickRef = LUA_NOREF;
  p.lua.reset();
  p.rom.reset();
  p.watches.clear();
  p.devWatches.clear();
  p.lastRows.clear();
  p.actionRef = LUA_NOREF;
  p.runSubs.clear();
  p.pickRefs.clear();
  p.hasBoardPending = false;
  p.capture.reset();
  p.uiTrees.clear();
  p.soundClips.clear();
  if (ctx.sound) ctx.sound->stopAll(p.id());
  syncWatches(p);
}

void PluginEngine::pause(Plugin& p, const std::string& error) {
  if (p.status == PluginStatus::Paused) return;
  const std::string first = LuaSandbox::firstLine(error);
  log(p, LogLevel::Error, "Paused: " + first);
  releaseState(p);
  store.ui.markPaused(p.id());
  setStatus(p, PluginStatus::Paused, qs(error));
}

void PluginEngine::handlerError(Plugin& p, const std::string& error) {
  // Budget and memory overruns keep their behaviour: the state is not trustworthy any more, so the plugin pauses at once.
  if (p.lua && p.lua->trip() != LuaSandbox::Trip::None) {
    pause(p, error);
    return;
  }
  store.noteError(p.id());
  const bool pauseNow = p.errors.record(Clock::now());
  log(p, LogLevel::Error, LuaSandbox::firstLine(error));
  if (pauseNow) {
    pause(p, error + "\n(" + std::to_string(ErrorBurst::kLimit) + " errors within " + std::to_string(ErrorBurst::kWindowMs / 1000) + " s)");
  }
}

void PluginEngine::callRef(Plugin& p, int ref, const std::function<int(lua_State*)>& pushArgs) {
  if (p.status != PluginStatus::Running || !p.lua) return;
  std::string err;
  const auto t0 = Clock::now();
  const bool ok = p.lua->run(
      [&](lua_State* L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        const int n = pushArgs(L);
        lua_call(L, n, 0);
      },
      err);
  if (!ok) {
    handlerError(p, err);
    return;
  }
  // A handler that stays just below the wall limit every time would still starve everything: repeated slow calls pause it.
  if (Clock::now() - t0 > std::chrono::milliseconds(limits.wallMs / 2)) {
    if (++p.slowCalls >= kMaxSlowCalls) pause(p, "Handlers repeatedly used more than half of the time budget (" + std::to_string(limits.wallMs) + " ms)");
  } else {
    p.slowCalls = 0;
  }
}

namespace {
constexpr size_t kProbeMaxResultBytes = 32u * 1024 * 1024;
}

ProbeResult PluginEngine::probe(const QString& id, const std::function<void(lua_State*, std::vector<Value>&)>& body) {
  ProbeResult r;
  Plugin* p = find(id);
  if (p == nullptr) {
    r.error = QStringLiteral("plugin_not_found");
    return r;
  }
  if (p->status != PluginStatus::Running || !p->lua) {
    r.error = QStringLiteral("plugin_not_running");
    return r;
  }
  QList<LogLine> out;
  m_captureLog = &out;
  std::string err;
  const bool ok = p->lua->run([&](lua_State* L) { body(L, r.values); }, err);
  m_captureLog = nullptr;
  r.output = out;
  r.ok = ok;
  if (!ok) {
    r.values.clear();
    const std::string first = LuaSandbox::firstLine(err);
    r.error = qs(first);
    if (err.size() > first.size()) r.trace = qs(err.substr(first.size() + 1)).left(4000);
    if (p->lua->trip() != LuaSandbox::Trip::None) {  // same consequence as for any handler: the plugin is paused
      r.paused = true;
      pause(*p, err);
    }
  }
  return r;
}

ProbeResult PluginEngine::probeCall(const QString& id, const std::string& iface, const std::string& fn, const std::vector<Value>& args) {
  Plugin* p = find(id);
  int ref = LUA_NOREF;
  if (p != nullptr) {
    const auto it = p->provided.find(iface);
    if (it == p->provided.end() && p->status == PluginStatus::Running) {
      ProbeResult r;
      r.error = QStringLiteral("interface_not_provided");
      return r;
    }
    if (it != p->provided.end()) ref = it->second;
  }
  return probe(id, [&](lua_State* L, std::vector<Value>& results) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_getfield(L, -1, fn.c_str());
    if (!lua_isfunction(L, -1)) luaL_error(L, "'%s.%s' is not a function", iface.c_str(), fn.c_str());
    const int fidx = lua_gettop(L);
    luaL_checkstack(L, static_cast<int>(args.size()) + 4, "too many arguments");
    for (const Value& a : args) pushValue(L, a);
    lua_call(L, static_cast<int>(args.size()), LUA_MULTRET);
    const int nres = lua_gettop(L) - fidx + 1;
    for (int i = 0; i < nres; ++i) {
      Value v;
      std::string e;
      if (!readValue(L, fidx + i, v, e, kMaxValueDepth, kProbeMaxResultBytes)) luaL_error(L, "result %d: %s", i + 1, e.c_str());
      results.push_back(std::move(v));
    }
  });
}

ProbeResult PluginEngine::probeEval(const QString& id, const std::string& code) {
  return probe(id, [&](lua_State* L, std::vector<Value>& results) {
    // Host-side compile (the plugin's own environment has no load): first as an expression, then as statements.
    // Text chunks only, never bytecode.
    const std::string asExpr = "return " + code;
    if (luaL_loadbufferx(L, asExpr.data(), asExpr.size(), "=eval", "t") != LUA_OK) {
      lua_pop(L, 1);
      if (luaL_loadbufferx(L, code.data(), code.size(), "=eval", "t") != LUA_OK) lua_error(L);
    }
    const int base = lua_gettop(L) - 1;
    lua_call(L, 0, LUA_MULTRET);
    const int nres = lua_gettop(L) - base;
    luaL_checkstack(L, 4, "too many results");
    for (int i = 0; i < nres; ++i) {
      Value v;
      std::string e;
      if (!readValue(L, base + 1 + i, v, e, kMaxValueDepth, kProbeMaxResultBytes)) luaL_error(L, "result %d: %s", i + 1, e.c_str());
      results.push_back(std::move(v));
    }
  });
}

void PluginEngine::load(Plugin& p) {
  setStatus(p, PluginStatus::Loading);
  SandboxLimits sl;
  sl.memoryBytes = limits.memoryBytes;
  sl.instructions = limits.instructions;
  sl.wallMs = limits.wallMs;
  p.lua = std::make_unique<LuaSandbox>(sl);
  if (!p.lua->valid()) {
    pause(p, "Lua state could not be created");
    return;
  }
  p.lua->setOwner(&p);
  p.lua->setPrintSink([this, &p](LogLevel lv, const std::string& t) { log(p, lv, t); });

  std::string err;
  bool ok = p.lua->run([&](lua_State*) { registerFbApi(*this, p); }, err);
  if (!ok) {
    pause(p, "Could not set up the plugin API: " + err);
    return;
  }
  QFile f(QDir(p.info.path).filePath(p.info.manifest.entry));
  if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxEntryBytes) {
    pause(p, "Cannot read entry file '" + p.info.manifest.entry.toStdString() + "' (missing or larger than 4 MiB)");
    return;
  }
  const std::string code = f.readAll().toStdString();
  f.close();
  if (!p.lua->runChunk(code, "@" + p.info.manifest.entry.toStdString(), err)) {
    pause(p, err);
    return;
  }
  setStatus(p, PluginStatus::Running);
  log(p, LogLevel::Info, "Loaded " + p.info.manifest.name.toStdString() + " " + p.info.manifest.version.toString().toStdString());
}

void PluginEngine::start(const GameContext& c, std::shared_ptr<MemoryProvider> prov, const QList<PluginInfo>& list, const HostLimits& lim) {
  stop();
  formats::romfsCacheClear();  // a new game: never serve the previous game's RomFS index
  ctx = c;
  provider = std::move(prov);
  limits = lim;
  started = Clock::now();
  m_lastGen.reset();
  m_lastTick = Clock::time_point();
  m_lastWatched = Clock::time_point();
  QStringList ids;
  for (const PluginInfo& info : list) ids << info.manifest.id;
  store.reset(ids, PluginStatus::Loading);
  store.ui.reset(ids);
  for (const PluginInfo& info : list) {
    auto p = std::make_unique<Plugin>();
    p->info = info;
    p->engine = this;
    m_plugins.push_back(std::move(p));
  }
  for (auto& p : m_plugins) load(*p);
}

void PluginEngine::stop() {
  for (auto& p : m_plugins) {
    releaseState(*p);
    store.ui.clearPlugin(p->id());
    if (p->status != PluginStatus::Stopped) setStatus(*p, PluginStatus::Stopped, p->status == PluginStatus::Paused ? store.snapshot(p->id()).error : QString());
  }
  m_plugins.clear();
  formats::romfsCacheClear();
  events.clear();
  queuedBytes = 0;
  queuedBy.clear();
  provider.reset();
  ctx.video.reset();  // the Player releases its side on the UI thread after stop() returns
}

void PluginEngine::reload(const QString& id) {
  Plugin* p = find(id);
  if (!p) return;
  const auto t0 = Clock::now();
  releaseState(*p);
  store.ui.clearPlugin(p->id());
  p->status = PluginStatus::Loading;
  p->i18n.clear();
  const ManifestResult m = loadManifest(p->info.path);
  if (!m.ok) {
    pause(*p, "Manifest: " + m.error.toStdString());
    return;
  }
  p->info.manifest = m.manifest;
  log(*p, LogLevel::Info, "Reloading");
  p->errors.reset();
  p->slowCalls = 0;
  load(*p);
  const qint64 ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  if (p->status == PluginStatus::Running) {
    log(*p, LogLevel::Info, "Reloaded in " + std::to_string(ms) + " ms");
    // What survives a reload: uz.store, uz.cache, the run state on the server, per-game consent. The Lua state does not, so the
    // plugin hears about it (uz.events.on("uz", "reloaded", fn)) and can restore from uz.store.
    postHostEvent(QStringLiteral("reloaded"), valueFromVariant(QVariantMap{{QStringLiteral("ms"), ms}}), p->id());
  }
  store.setReloaded(p->id(), ms);
}

void PluginEngine::deliverEvents() {
  size_t n = (std::min<size_t>)(events.size(), 1000);
  while (n-- > 0 && !events.empty()) {
    // Aggregate budget: the handlers of all plugins together may use kIterateBudgetMs per iteration; the rest stays queued.
    if (Clock::now() > m_iterDeadline) break;
    QueuedEvent ev = popEvent();
    for (auto& pp : m_plugins) {
      Plugin& p = *pp;
      if (p.status != PluginStatus::Running) continue;
      std::vector<int> refs;
      for (const EventSub& s : p.subs)
        if ((ev.target.isEmpty() || ev.target == p.id()) && qs(ev.name) == s.name && (s.plugin == QLatin1String("*") || s.plugin == ev.from)) refs.push_back(s.ref);
      for (int ref : refs) {
        callRef(p, ref, [&](lua_State* L) {
          pushValue(L, ev.value);
          lua_pushstring(L, ev.from.toUtf8().constData());
          lua_pushlstring(L, ev.name.data(), ev.name.size());
          return 3;
        });
        if (p.status != PluginStatus::Running) break;
      }
    }
  }
}

void PluginEngine::deliverAction(const QString& pluginId, const QString& slot, const QString& action, const Value& value) {
  Plugin* p = find(pluginId);
  if (!p || p->status != PluginStatus::Running || p->actionRef == LUA_NOREF) return;
  const QByteArray a = action.toUtf8();
  const QByteArray sl = slot.toUtf8();
  callRef(*p, p->actionRef, [&](lua_State* L) {
    lua_pushlstring(L, a.constData(), static_cast<size_t>(a.size()));
    pushValue(L, value);
    lua_pushlstring(L, sl.constData(), static_cast<size_t>(sl.size()));
    return 3;
  });
}

void PluginEngine::postHostEvent(const QString& name, Value value, const QString& target) {
  const std::string n = name.toStdString();
  if (!eventFits(QStringLiteral("uz"), valueBytes(value) + n.size() + sizeof(QueuedEvent))) return;
  pushEvent({QStringLiteral("uz"), n, std::move(value), target, 0});
}

void PluginEngine::postAppEvent(const QString& name, Value value, const QString& target) {
  const std::string n = name.toStdString();
  if (!eventFits(QStringLiteral("app"), valueBytes(value) + n.size() + sizeof(QueuedEvent))) return;
  pushEvent({QStringLiteral("app"), n, std::move(value), target, 0});
}

void PluginEngine::deliverFile(const QString& pluginId, uint64_t requestId, const std::string& content, const std::string& name, const std::string& error) {
  Plugin* p = find(pluginId);
  if (!p || p->status != PluginStatus::Running || !p->lua) return;
  auto it = p->pickRefs.find(requestId);
  if (it == p->pickRefs.end()) return;
  const int ref = it->second;
  p->pickRefs.erase(it);
  callRef(*p, ref, [&](lua_State* L) {
    if (error.empty()) {
      lua_pushlstring(L, content.data(), content.size());
      lua_createtable(L, 0, 2);
      lua_pushlstring(L, name.data(), name.size());
      lua_setfield(L, -2, "name");
      lua_pushinteger(L, static_cast<lua_Integer>(content.size()));
      lua_setfield(L, -2, "size");
      return 2;
    }
    lua_pushnil(L);
    if (error == "cancelled") return 1;  // cancel = callback(nil)
    lua_pushlstring(L, error.data(), error.size());
    return 2;
  });
  if (p->lua) luaL_unref(p->lua->L(), LUA_REGISTRYINDEX, ref);
}

void PluginEngine::clearSlotEverywhere(const QString& slot) {
  for (auto& p : m_plugins) {
    p->uiTrees.erase(slot);
    store.ui.clearSlot(p->id(), slot);
  }
}

void PluginEngine::tick() {
  if (!provider) return;
  const std::optional<uint64_t> frame = provider->frame();
  if (!frame) return;
  const uint64_t gen = provider->generation();
  if (m_lastGen && *m_lastGen == gen) return;
  const int hz = (std::max)(1, m_tickHz.load());
  const auto now = Clock::now();
  if (m_lastTick != Clock::time_point() && now - m_lastTick < std::chrono::microseconds(1000000 / hz)) return;
  m_lastGen = gen;
  m_lastTick = now;
  for (auto& pp : m_plugins) {
    Plugin& p = *pp;
    if (p.status != PluginStatus::Running || p.tickRef == LUA_NOREF) continue;
    callRef(p, p.tickRef, [&](lua_State* L) {
      lua_pushinteger(L, static_cast<lua_Integer>(*frame));
      return 1;
    });
  }
}

std::optional<QString> formatWatched(const MemoryProvider& mem, const DevWatch& w) {
  unsigned char buf[64] = {};
  if (w.size == 0 || w.size > sizeof buf || !mem.read(w.addr, buf, w.size)) return std::nullopt;
  uint64_t u = 0;
  for (uint32_t i = 0; i < w.size && i < 8; ++i) u |= static_cast<uint64_t>(buf[i]) << (8 * i);
  if (w.kind == QLatin1String("s32")) return QString::number(static_cast<int32_t>(u));
  if (w.kind == QLatin1String("hex")) {
    QString s;
    for (uint32_t i = 0; i < w.size; ++i) s += QString::number(buf[i], 16).toUpper().rightJustified(2, QLatin1Char('0')) + (i + 1 < w.size ? QStringLiteral(" ") : QString());
    return s;
  }
  return QString::number(u);
}

void PluginEngine::updateWatched() {
  if (!provider) return;
  const auto now = Clock::now();
  if (now - m_lastWatched < std::chrono::milliseconds(100)) return;
  m_lastWatched = now;
  for (auto& pp : m_plugins) {
    Plugin& p = *pp;
    if (p.status != PluginStatus::Running && p.devWatches.empty()) continue;
    QList<WatchedRow> rows;
    for (const DevWatch& w : p.devWatches) {
      const std::optional<QString> v = formatWatched(*provider, w);
      const QString addr = QStringLiteral("0x") + QString::number(w.addr, 16).toUpper().rightJustified(w.addr > 0xFFFFFFFFull ? 16 : 8, QLatin1Char('0'));
      rows.append({w.label, addr, v ? *v : QStringLiteral("-")});
    }
    store.setWatched(p.id(), rows);
  }
}

// uz.run: hands what happened in the run to the subscribed plugins, and sends boards that waited for the 1/s limit.
void PluginEngine::pollRun() {
  if (!ctx.run) return;
  const auto now = Clock::now();
  for (auto& pp : m_plugins) {
    Plugin& p = *pp;
    if (p.hasBoardPending && now - p.boardLast >= std::chrono::milliseconds(1000)) {
      p.hasBoardPending = false;
      p.boardLast = now;
      const QString e = ctx.run->setBoard(p.id(), p.boardPending);
      if (!e.isEmpty()) log(p, LogLevel::Warn, "uz.run.setBoard: " + e.toStdString());
    }
  }
  for (const RunNotification& n : ctx.run->takeNotifications()) {
    const std::string kind = n.kind.toStdString();
    const Value data = valueFromJson(n.data);
    for (auto& pp : m_plugins) {
      Plugin& p = *pp;
      if (p.status != PluginStatus::Running) continue;
      std::vector<int> refs;
      for (const RunSub& s : p.runSubs)
        if (s.kind == kind) refs.push_back(s.ref);
      for (int ref : refs) {
        callRef(p, ref, [&](lua_State* L) {
          // event: (event); state: (user_id, state, plugin_id); status: (status, previous); participants: (list)
          const auto field = [&](const char* name) {
            for (size_t i = 0; i < data.keys.size(); ++i)
              if (data.keys[i].type == Value::Type::String && data.keys[i].s == name) return data.vals[i];
            return Value();
          };
          if (kind == "state") {
            pushValue(L, field("user_id"));
            pushValue(L, field("state"));
            pushValue(L, field("plugin_id"));
            return 3;
          }
          if (kind == "status") {
            pushValue(L, field("status"));
            pushValue(L, field("previous"));
            return 2;
          }
          if (kind == "participants") {
            pushValue(L, field("participants"));
            return 1;
          }
          pushValue(L, data);
          return 1;
        });
        if (p.status != PluginStatus::Running) break;
      }
    }
  }
}

namespace {
constexpr size_t kMaxCapturePng = 2u * 1024 * 1024;
constexpr int kMaxCaptureSide = 4096;
constexpr auto kCaptureMinInterval = std::chrono::seconds(1);
constexpr auto kCaptureTimeout = std::chrono::seconds(5);

void putField(Value& t, const char* key, Value v) {
  Value k;
  k.type = Value::Type::String;
  k.s = key;
  t.keys.push_back(std::move(k));
  t.vals.push_back(std::move(v));
}
Value intValue(int64_t i) {
  Value v;
  v.type = Value::Type::Int;
  v.i = i;
  return v;
}
Value strValue(std::string s) {
  Value v;
  v.type = Value::Type::String;
  v.s = std::move(s);
  return v;
}
Value boolValue(bool b) {
  Value v;
  v.type = Value::Type::Bool;
  v.b = b;
  return v;
}
}  // namespace

uint64_t PluginEngine::requestCapture(Plugin& p, const std::string& screen, int x, int y, int w, int h) {
  const uint64_t id = p.captureNextId++;
  auto fail = [&](const char* err) {
    finishCapture(p, id, nullptr, nullptr, err);
    return id;
  };
  if (p.capture) return fail("busy");
  const auto now = Clock::now();
  if (p.captureLast && now - *p.captureLast < kCaptureMinInterval) return fail("rate_limited");
  if (!ctx.video) return fail("unsupported");
  PendingCapture cap;
  cap.id = id;
  cap.at = now;
  if (!screen.empty()) {
    const std::vector<ScreenInfo> screens = ctx.video->screens();
    if (screens.empty()) return fail("no_frame");
    const ScreenInfo* found = nullptr;
    for (const ScreenInfo& s : screens)
      if (s.name == screen) found = &s;
    if (!found) return fail("unknown_screen");
    cap.x = found->x;
    cap.y = found->y;
    cap.w = found->w;
    cap.h = found->h;
  } else {
    if (x < 0 || y < 0 || w < 1 || h < 1 || w > kMaxCaptureSide || h > kMaxCaptureSide) return fail("invalid_rect");
    cap.x = x;
    cap.y = y;
    cap.w = w;
    cap.h = h;
  }
  p.captureLast = now;
  p.capture = cap;
  ctx.video->captureFrame([inbox = m_captureInbox, plugin = p.id(), id](RawFrame f) {
    std::lock_guard<std::mutex> l(inbox->mutex);
    inbox->items.push_back({plugin, id, std::move(f)});
  });
  return id;
}

// Queues the host event "video.captured" for `p`. Without `frame` (or with an error) the event carries ok=false.
void PluginEngine::finishCapture(Plugin& p, uint64_t id, const RawFrame* frame, const PendingCapture* cap, const std::string& error) {
  Value t;
  t.type = Value::Type::Table;
  putField(t, "id", intValue(static_cast<int64_t>(id)));
  std::string err = error;
  std::string png;
  int w = 0, h = 0;
  if (err.empty() && frame && cap) {
    if (frame->w < 1 || frame->h < 1 || frame->rgba.size() != static_cast<size_t>(frame->w) * frame->h * 4) {
      err = "no_frame";
    } else if (cap->x + static_cast<int64_t>(cap->w) > frame->w || cap->y + static_cast<int64_t>(cap->h) > frame->h) {
      err = "invalid_rect";
    } else {
      formats::Image img;
      img.ok = true;
      img.width = cap->w;
      img.height = cap->h;
      img.rgba.resize(static_cast<size_t>(cap->w) * cap->h * 4);
      for (int row = 0; row < cap->h; ++row)
        std::memcpy(&img.rgba[static_cast<size_t>(row) * cap->w * 4], &frame->rgba[(static_cast<size_t>(cap->y + row) * frame->w + cap->x) * 4],
                    static_cast<size_t>(cap->w) * 4);
      formats::Result r = formats::pngEncode(img);
      if (!r.ok) err = "encode_failed";
      else if (r.data.size() > kMaxCapturePng) err = "too_large";
      else {
        png = std::move(r.data);
        w = cap->w;
        h = cap->h;
      }
    }
  } else if (err.empty()) {
    err = "no_frame";
  }
  putField(t, "ok", boolValue(err.empty()));
  if (err.empty()) {
    putField(t, "png", strValue(std::move(png)));
    putField(t, "w", intValue(w));
    putField(t, "h", intValue(h));
  } else {
    putField(t, "error", strValue(err));
  }
  postHostEvent(QStringLiteral("video.captured"), std::move(t), p.id());
}

void PluginEngine::pollCaptures() {
  std::vector<CaptureInbox::Item> items;
  {
    std::lock_guard<std::mutex> l(m_captureInbox->mutex);
    items.swap(m_captureInbox->items);
  }
  for (CaptureInbox::Item& it : items) {
    Plugin* p = find(it.plugin);
    if (!p || !p->capture || p->capture->id != it.id) continue;  // reloaded, stopped or timed out meanwhile
    const PendingCapture cap = *p->capture;
    p->capture.reset();
    finishCapture(*p, it.id, &it.frame, &cap, it.frame.error);
  }
  const auto now = Clock::now();
  for (auto& pp : m_plugins) {
    if (pp->capture && now - pp->capture->at > kCaptureTimeout) {
      const uint64_t id = pp->capture->id;
      pp->capture.reset();
      finishCapture(*pp, id, nullptr, nullptr, "no_frame");
    }
  }
}

void PluginEngine::iterate() {
  m_iterDeadline = Clock::now() + std::chrono::milliseconds(kIterateBudgetMs);
  pollCaptures();
  pollRun();
  deliverEvents();
  tick();
  updateWatched();
}

}  // namespace uzume::plugins
