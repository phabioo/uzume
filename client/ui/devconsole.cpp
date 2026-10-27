#include "devconsole.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
#include <algorithm>

#include "emulator_backend.h"
#include "gamesession.h"

namespace uzume::ui {

using plugins::LogLevel;
using plugins::PluginStatus;

namespace {

const QStringList& allLevels() {
  static const QStringList l{QStringLiteral("error"), QStringLiteral("warn"), QStringLiteral("info"), QStringLiteral("debug")};
  return l;
}

QString tr(const char* text) { return QCoreApplication::translate("DevConsole", text); }

class GameMemorySource : public MemorySource {
 public:
  explicit GameMemorySource(GameSession* gs) : gs_(gs) {}
  bool available() const override { return gs_ && gs_->isActive() && !gs_->memoryRegions().empty(); }
  std::vector<emu::MemoryMap::RegionInfo> regions() const override {
    return gs_ && gs_->isActive() ? gs_->memoryRegions() : std::vector<emu::MemoryMap::RegionInfo>();
  }
  bool read(quint64 addr, quint64 len, QByteArray* out, QString* error) override {
    if (!gs_ || !gs_->isActive()) return fail(error, QStringLiteral("no_game"));
    bool noMap = false;
    const bool ran = gs_->runOnEmuThread([&](emu::EmulatorBackend& be) {
      const emu::MemoryMap* map = be.memoryMap();
      if (map == nullptr) {
        noMap = true;
        return;
      }
      *out = emu::memaccess::read(*map, addr, len);
    });
    if (!ran) return fail(error, QStringLiteral("emulation_busy"));
    if (noMap) return fail(error, QStringLiteral("no_memory_map"));
    return true;
  }
  bool search(quint64 start, quint64 end, const QByteArray& pattern, int max, emu::memaccess::SearchResult* out, QString* error) override {
    if (!gs_ || !gs_->isActive()) return fail(error, QStringLiteral("no_game"));
    bool noMap = false;
    const bool ran = gs_->runOnEmuThread(
        [&](emu::EmulatorBackend& be) {
          const emu::MemoryMap* map = be.memoryMap();
          if (map == nullptr) {
            noMap = true;
            return;
          }
          *out = emu::memaccess::search(*map, start, end, pattern, max);
        },
        15000);
    if (!ran) return fail(error, QStringLiteral("emulation_busy"));
    if (noMap) return fail(error, QStringLiteral("no_memory_map"));
    return true;
  }

 private:
  static bool fail(QString* error, const QString& why) {
    if (error != nullptr) *error = why;
    return false;
  }
  QPointer<GameSession> gs_;
};

}  // namespace

std::unique_ptr<MemorySource> makeGameMemorySource(GameSession* session) { return std::make_unique<GameMemorySource>(session); }

int ConsoleLogView::countOf(const QString& level) const {
  if (level == QLatin1String("error")) return error;
  if (level == QLatin1String("warn")) return warn;
  if (level == QLatin1String("info")) return info;
  if (level == QLatin1String("debug")) return debug;
  return 0;
}

// ---------------------------------------------------------------- pure pieces

ConsoleLogView DevConsole::buildLogView(const QList<ConsoleLogEntry>& entries, const ConsoleLogFilter& filter) {
  ConsoleLogView v;
  QList<ConsoleLogEntry> kept;
  for (const ConsoleLogEntry& e : entries) {
    v.lastSeq = (std::max)(v.lastSeq, e.seq);
    if (filter.clearedBeforeMs > 0 && e.timeMs <= filter.clearedBeforeMs) continue;
    if (filter.maxSeq >= 0 && e.seq > filter.maxSeq) {
      ++v.hiddenNew;
      continue;
    }
    ++v.total;
    if (e.level == QLatin1String("error")) ++v.error;
    else if (e.level == QLatin1String("warn")) ++v.warn;
    else if (e.level == QLatin1String("debug")) ++v.debug;
    else ++v.info;
    if (!filter.levels.isEmpty() && !filter.levels.contains(e.level)) continue;
    if (e.seq <= filter.sinceSeq) continue;
    kept.append(e);
  }
  for (auto it = kept.crbegin(); it != kept.crend() && v.lines.size() < filter.limit; ++it) v.lines.append(*it);
  return v;
}

QList<ConsoleLogEntry> DevConsole::entriesOf(const QList<plugins::LogLine>& log) {
  QList<ConsoleLogEntry> out;
  out.reserve(log.size());
  for (const plugins::LogLine& l : log) out.append({l.seq, l.timeMs, levelName(l.level), l.text});
  return out;
}

QString DevConsole::levelName(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return QStringLiteral("debug");
    case LogLevel::Info: return QStringLiteral("info");
    case LogLevel::Warn: return QStringLiteral("warn");
    case LogLevel::Error: return QStringLiteral("error");
  }
  return QStringLiteral("info");
}

QString DevConsole::statusKey(const plugins::ConsoleSnapshot& s) {
  switch (s.status) {
    case PluginStatus::Running: return QStringLiteral("running");
    case PluginStatus::Loading: return QStringLiteral("loading");
    case PluginStatus::Paused: return s.error.isEmpty() ? QStringLiteral("paused") : QStringLiteral("error");
    case PluginStatus::Stopped: return QStringLiteral("stopped");
  }
  return QStringLiteral("stopped");
}

QString DevConsole::timeText(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm:ss.zzz")); }

namespace {
QString statusText(const QString& key, bool running) {
  if (key == QLatin1String("running")) return tr("Running");
  if (key == QLatin1String("loading")) return tr("Starting");
  if (key == QLatin1String("paused")) return tr("Paused");
  if (key == QLatin1String("error")) return tr("Paused after an error");
  return running ? tr("Stopped") : tr("Not running");
}

QVariantMap logLine(const ConsoleLogEntry& e) {
  return {{QStringLiteral("seq"), e.seq}, {QStringLiteral("time"), DevConsole::timeText(e.timeMs)}, {QStringLiteral("level"), e.level}, {QStringLiteral("text"), e.text}};
}
}  // namespace

// ---------------------------------------------------------------- construction

DevConsole::DevConsole(Sources sources, QObject* parent) : QObject(parent), src_(std::move(sources)) {
  for (const QString& l : allLevels()) levels_.insert(l);
  timer_.setSingleShot(true);
  timer_.setInterval(100);  // about 10 Hz at most
  connect(&timer_, &QTimer::timeout, this, [this]() {
    model_ = buildModel();
    emit modelChanged();
  });
  memory_ = QVariantMap{{QStringLiteral("available"), false}};
}

void DevConsole::setReloadOnChange(bool on) {
  if (reloadOnChange_ == on) return;
  reloadOnChange_ = on;
  if (src_.setWatching) src_.setWatching(on);
  emit reloadOnChangeChanged();
  schedule();
}

void DevConsole::setMemorySource(std::shared_ptr<MemorySource> source) {
  mem_ = std::move(source);
  baseSet_ = false;
  regionIndex_ = 0;
  search_.clear();
  rebuildMemory();
}

QString DevConsole::nameOf(const QString& id) const {
  const auto info = src_.info ? src_.info(id) : std::nullopt;
  return info ? info->manifest.name : id;
}

QStringList DevConsole::consoleIds() const {
  QStringList ids = src_.activeIds ? src_.activeIds() : QStringList();
  if (src_.unpacked)
    for (const plugins::PluginInfo& p : src_.unpacked())
      if (!ids.contains(p.manifest.id)) ids << p.manifest.id;
  return ids;
}

ConsoleLogFilter DevConsole::filterFor(const QString& id) const {
  ConsoleLogFilter f;
  f.levels = levels_;
  f.clearedBeforeMs = clearedAt_.value(id, 0);
  if (logPaused_) {
    const auto it = pausedAtSeq_.constFind(id);
    f.maxSeq = it != pausedAtSeq_.constEnd() ? it.value() : (src_.snapshot ? src_.snapshot(id).lastSeq : 0);
  }
  return f;
}

ConsoleLogView DevConsole::logView(const QString& pluginId, const ConsoleLogFilter& filter) const {
  const plugins::ConsoleSnapshot snap = src_.snapshot ? src_.snapshot(pluginId) : plugins::ConsoleSnapshot();
  return buildLogView(entriesOf(snap.log), filter);
}

QVariantMap DevConsole::permissionsOf(const plugins::PluginInfo& info, const plugins::ConsoleSnapshot& snap) const {
  const auto label = [this](const QString& p) { return src_.permissionLabel ? src_.permissionLabel(p) : p; };
  QVariantList granted, notGranted;
  for (const QString& p : info.manifest.permissions) granted.append(QVariantMap{{QStringLiteral("id"), p}, {QStringLiteral("label"), label(p)}});
  for (const QString& p : snap.notGranted) notGranted.append(QVariantMap{{QStringLiteral("id"), p}, {QStringLiteral("label"), label(p)}});
  return {{QStringLiteral("granted"), granted}, {QStringLiteral("notGranted"), notGranted}};
}

// ---------------------------------------------------------------- model

QVariantMap DevConsole::buildModel() {
  if (hasPreview_) {
    QVariantMap m = preview_;
    QList<ConsoleLogEntry> entries;
    const QVariantList log = preview_.value(QStringLiteral("log")).toList();
    qint64 seq = 0;
    for (auto it = log.crbegin(); it != log.crend(); ++it) {  // the preview lists newest first
      const QVariantMap l = it->toMap();
      entries.append({++seq, seq, l.value(QStringLiteral("level")).toString(), l.value(QStringLiteral("text")).toString()});
    }
    // Preview times are texts: keep them next to the entries by index.
    ConsoleLogFilter f;
    f.levels = levels_;
    const ConsoleLogView v = buildLogView(entries, f);
    QVariantList out;
    for (const ConsoleLogEntry& e : v.lines) {
      QVariantMap row = logLine(e);
      row.insert(QStringLiteral("time"), log.value(static_cast<int>(log.size() - e.seq)).toMap().value(QStringLiteral("time")));
      out.append(row);
    }
    m.insert(QStringLiteral("log"), out);
    m.insert(QStringLiteral("counts"), QVariantMap{{QStringLiteral("error"), v.error}, {QStringLiteral("warn"), v.warn}, {QStringLiteral("info"), v.info},
                                                   {QStringLiteral("debug"), v.debug}, {QStringLiteral("total"), v.total}});
    QVariantMap filters;
    for (const QString& l : allLevels()) filters.insert(l, levels_.contains(l));
    m.insert(QStringLiteral("filters"), filters);
    m.insert(QStringLiteral("logPaused"), logPaused_);
    m.insert(QStringLiteral("developerMode"), true);
    const QString st = m.value(QStringLiteral("status")).toString();
    m.insert(QStringLiteral("paused"), st == QLatin1String("error") || st == QLatin1String("paused"));
    m.insert(QStringLiteral("running"), st == QLatin1String("running") || st == QLatin1String("error") || st == QLatin1String("loading") || st == QLatin1String("paused"));
    m.insert(QStringLiteral("errorLine"), tr("Paused after an error. Your game keeps running."));
    return m;
  }

  QVariantMap m;
  m.insert(QStringLiteral("developerMode"), src_.developerMode && src_.developerMode());
  const QStringList ids = consoleIds();
  const QStringList active = src_.activeIds ? src_.activeIds() : QStringList();
  if (!ids.contains(selected_)) selected_ = ids.value(0);

  QHash<QString, plugins::PluginInfo> unpacked;
  if (src_.unpacked)
    for (const plugins::PluginInfo& p : src_.unpacked()) unpacked.insert(p.manifest.id, p);

  QVariantList plist;
  for (const QString& id : ids) {
    const bool run = active.contains(id);
    const plugins::ConsoleSnapshot s = run && src_.snapshot ? src_.snapshot(id) : plugins::ConsoleSnapshot();
    const QString key = run ? statusKey(s) : QStringLiteral("stopped");
    const auto info = src_.info ? src_.info(id) : std::nullopt;
    const bool isUnpacked = unpacked.contains(id) || (info && info->devLinked);
    plist.append(QVariantMap{{QStringLiteral("id"), id},
                             {QStringLiteral("name"), info ? info->manifest.name : id},
                             {QStringLiteral("status"), key},
                             {QStringLiteral("statusText"), statusText(key, run)},
                             {QStringLiteral("running"), run},
                             {QStringLiteral("unpacked"), isUnpacked},
                             {QStringLiteral("folder"), info ? info->path : QString()}});
  }
  m.insert(QStringLiteral("plugins"), plist);
  m.insert(QStringLiteral("selected"), selected_);
  m.insert(QStringLiteral("name"), selected_.isEmpty() ? QString() : nameOf(selected_));

  const bool run = active.contains(selected_);
  const plugins::ConsoleSnapshot snap = run && src_.snapshot ? src_.snapshot(selected_) : plugins::ConsoleSnapshot();
  const QString key = selected_.isEmpty() || !run ? QStringLiteral("stopped") : statusKey(snap);
  const auto info = selected_.isEmpty() || !src_.info ? std::nullopt : src_.info(selected_);
  const bool paused = key == QLatin1String("error") || key == QLatin1String("paused");
  m.insert(QStringLiteral("running"), run);
  m.insert(QStringLiteral("unpacked"), info && info->devLinked);
  m.insert(QStringLiteral("folder"), info ? info->path : QString());
  m.insert(QStringLiteral("status"), key);
  m.insert(QStringLiteral("statusText"), statusText(key, run));
  m.insert(QStringLiteral("paused"), paused);
  m.insert(QStringLiteral("error"), snap.error);
  m.insert(QStringLiteral("errorFirst"), snap.error.section(QLatin1Char('\n'), 0, 0));
  m.insert(QStringLiteral("errorLine"), tr("Paused after an error. Your game keeps running."));
  m.insert(QStringLiteral("permissions"), info ? permissionsOf(*info, snap) : QVariantMap{{QStringLiteral("granted"), QVariantList()}, {QStringLiteral("notGranted"), QVariantList()}});

  QVariantList watched;
  for (const auto& w : snap.watched)
    watched.append(QVariantMap{{QStringLiteral("address"), w.address}, {QStringLiteral("value"), w.value}, {QStringLiteral("label"), w.label}});
  QVariantList events;
  for (auto it = snap.events.crbegin(); it != snap.events.crend() && events.size() < 100; ++it)  // newest first
    events.append(QVariantMap{{QStringLiteral("time"), timeText(it->timeMs)}, {QStringLiteral("plugin"), it->plugin}, {QStringLiteral("kind"), it->name}, {QStringLiteral("text"), it->text}});
  m.insert(QStringLiteral("watched"), watched);
  m.insert(QStringLiteral("events"), events);

  if (logPaused_ && !pausedAtSeq_.contains(selected_)) pausedAtSeq_.insert(selected_, snap.lastSeq);
  const ConsoleLogView v = buildLogView(entriesOf(snap.log), filterFor(selected_));
  QVariantList lines;
  for (const ConsoleLogEntry& e : v.lines) lines.append(logLine(e));
  m.insert(QStringLiteral("log"), lines);
  m.insert(QStringLiteral("counts"), QVariantMap{{QStringLiteral("error"), v.error}, {QStringLiteral("warn"), v.warn}, {QStringLiteral("info"), v.info},
                                                 {QStringLiteral("debug"), v.debug}, {QStringLiteral("total"), v.total}});
  QVariantMap filters;
  for (const QString& l : allLevels()) filters.insert(l, levels_.contains(l));
  m.insert(QStringLiteral("filters"), filters);
  m.insert(QStringLiteral("logPaused"), logPaused_);
  m.insert(QStringLiteral("logPending"), v.hiddenNew);

  QVariantList regions;
  if (mem_ != nullptr) {
    for (const auto& r : mem_->regions())
      regions.append(QVariantMap{{QStringLiteral("start"), emu::memaccess::hexAddress(r.start)},
                                 {QStringLiteral("length"), QStringLiteral("0x") + QString::number(r.len, 16).toUpper()},
                                 {QStringLiteral("addrspace"), QString::fromStdString(r.addrspace)}});
  }
  m.insert(QStringLiteral("regions"), regions);
  m.insert(QStringLiteral("regionsText"), regions.isEmpty() ? tr("The core exposes no memory map") : QString());

  QVariantMap inspect = src_.inspect && !selected_.isEmpty() && run ? src_.inspect(selected_) : QVariantMap();
  inspect.insert(QStringLiteral("watched"), watched);
  m.insert(QStringLiteral("inspect"), inspect);

  m.insert(QStringLiteral("reload"), QVariantMap{{QStringLiteral("count"), snap.reloadCount},
                                                 {QStringLiteral("ms"), snap.reloadMs},
                                                 {QStringLiteral("text"), snap.reloadMs >= 0 ? tr("Reloaded · %1 ms").arg(snap.reloadMs) : QString()}});
  m.insert(QStringLiteral("reloadOnChange"), reloadOnChange_);
  m.insert(QStringLiteral("watching"), reloadOnChange_ && info && info->devLinked);
  return m;
}

void DevConsole::schedule() {
  if (hasPreview_) return;
  if (!timer_.isActive()) timer_.start();
}

void DevConsole::noteChanged(const QString& id) {
  const plugins::ConsoleSnapshot s = src_.snapshot ? src_.snapshot(id) : plugins::ConsoleSnapshot();
  Seen& st = seen_[id];
  const bool pausedByError = s.status == PluginStatus::Paused && !s.error.isEmpty();
  if (pausedByError && !st.paused) {
    st.paused = true;
    emit pausedAfterError(id, nameOf(id));
  } else if (!pausedByError) {
    st.paused = false;
  }
  if (s.reloadCount > st.reloadCount && s.status == PluginStatus::Running) emit reloaded(id, nameOf(id), s.reloadMs);
  st.reloadCount = s.reloadCount;
  schedule();
}

void DevConsole::resetSeen() {
  seen_.clear();
  pausedAtSeq_.clear();
  clearedAt_.clear();
  schedule();
}

void DevConsole::setPreview(const QVariantMap& model) {
  hasPreview_ = !model.isEmpty();
  preview_ = model;
  model_ = hasPreview_ ? buildModel() : QVariantMap();
  if (!hasPreview_) {
    model_.clear();
    schedule();
  }
  emit modelChanged();
}

// ---------------------------------------------------------------- state changes

void DevConsole::selectPlugin(const QString& id) {
  if (hasPreview_) {
    preview_.insert(QStringLiteral("selected"), id);
    model_ = buildModel();
    emit modelChanged();
    return;
  }
  if (!consoleIds().contains(id) || selected_ == id) return;
  selected_ = id;
  model_ = buildModel();
  emit modelChanged();
}

void DevConsole::setLevel(const QString& level, bool shown) {
  if (!allLevels().contains(level)) return;
  if (shown) levels_.insert(level);
  else levels_.remove(level);
  if (levels_.isEmpty()) levels_.insert(level);  // never an empty table: the last chip stays on
  model_ = buildModel();
  emit modelChanged();
}

void DevConsole::setLogPaused(bool paused) {
  if (logPaused_ == paused) return;
  logPaused_ = paused;
  pausedAtSeq_.clear();
  if (paused && src_.snapshot)
    for (const QString& id : consoleIds()) pausedAtSeq_.insert(id, src_.snapshot(id).lastSeq);
  model_ = buildModel();
  emit modelChanged();
}

void DevConsole::clearLog(const QString& id) {
  if (hasPreview_) {
    preview_.insert(QStringLiteral("log"), QVariantList());
    model_ = buildModel();
    emit modelChanged();
    return;
  }
  qint64 latest = QDateTime::currentMSecsSinceEpoch();
  if (src_.snapshot)
    for (const auto& l : src_.snapshot(id).log) latest = (std::max)(latest, l.timeMs);
  clearedAt_.insert(id, latest);
  model_ = buildModel();
  emit modelChanged();
}

QString DevConsole::logText(const QString& id) const {
  QStringList lines;
  if (hasPreview_) {
    const QVariantList log = preview_.value(QStringLiteral("log")).toList();
    for (auto it = log.crbegin(); it != log.crend(); ++it) {
      const QVariantMap m = it->toMap();
      lines << m.value(QStringLiteral("time")).toString() + QLatin1Char(' ') + m.value(QStringLiteral("level")).toString().toUpper() + QLatin1Char(' ') +
                   m.value(QStringLiteral("text")).toString();
    }
    return lines.join(QLatin1Char('\n')) + (lines.isEmpty() ? QString() : QStringLiteral("\n"));
  }
  const qint64 cut = clearedAt_.value(id, 0);
  const plugins::ConsoleSnapshot snap = src_.snapshot ? src_.snapshot(id) : plugins::ConsoleSnapshot();
  for (const auto& l : snap.log)
    if (l.timeMs > cut) lines << QStringLiteral("%1 %2 %3").arg(timeText(l.timeMs), levelName(l.level).toUpper(), l.text);
  return lines.join(QLatin1Char('\n'));
}

void DevConsole::reloadSelected() {
  const QStringList ids = consoleIds();
  if (!ids.contains(selected_)) selected_ = ids.value(0);
  if (!selected_.isEmpty() && src_.reload) src_.reload(selected_);
}

// ---------------------------------------------------------------- memory viewer

void DevConsole::setMemoryError(const QString& text) {
  memError_ = text;
  rebuildMemory();
}

void DevConsole::viewMemory() {
  rows_.clear();
  memError_.clear();
  if (mem_ == nullptr || !mem_->available()) return;
  QByteArray data;
  QString err;
  if (!mem_->read(base_, kViewRows * kRowBytes, &data, &err)) {
    memError_ = err == QLatin1String("emulation_busy") ? tr("The game did not answer. Try again.") : tr("No game memory to show.");
    return;
  }
  if (data.isEmpty()) {
    memError_ = tr("Nothing is mapped at %1.").arg(emu::memaccess::hexAddress(base_));
    return;
  }
  for (const emu::memaccess::HexRow& r : emu::memaccess::hexRows(data, base_, kRowBytes))
    rows_.append(QVariantMap{{QStringLiteral("addr"), r.addrText}, {QStringLiteral("hex"), r.hex}, {QStringLiteral("ascii"), r.ascii}});
}

void DevConsole::rebuildMemory() {
  QVariantMap m;
  const bool avail = mem_ != nullptr && mem_->available();
  m.insert(QStringLiteral("available"), avail);
  QVariantList regions;
  if (avail) {
    int i = 0;
    for (const auto& r : mem_->regions()) {
      const QString space = QString::fromStdString(r.addrspace);
      regions.append(QVariantMap{{QStringLiteral("index"), i},
                                 {QStringLiteral("value"), QString::number(i)},
                                 {QStringLiteral("label"), emu::memaccess::hexAddress(r.start) + (space.isEmpty() ? QString() : QStringLiteral(" · ") + space)},
                                 {QStringLiteral("start"), emu::memaccess::hexAddress(r.start)},
                                 {QStringLiteral("length"), QStringLiteral("0x") + QString::number(r.len, 16).toUpper()},
                                 {QStringLiteral("addrspace"), space}});
      ++i;
    }
  }
  m.insert(QStringLiteral("regions"), regions);
  m.insert(QStringLiteral("regionIndex"), regionIndex_);
  m.insert(QStringLiteral("baseText"), avail ? emu::memaccess::hexAddress(base_) : QString());
  m.insert(QStringLiteral("rows"), rows_);
  m.insert(QStringLiteral("error"), memError_);
  m.insert(QStringLiteral("search"), search_);
  memory_ = m;
  emit memoryChanged();
}

void DevConsole::memorySelectRegion(int index) {
  if (mem_ == nullptr || !mem_->available()) return;
  const auto regions = mem_->regions();
  if (index < 0 || index >= static_cast<int>(regions.size())) return;
  regionIndex_ = index;
  base_ = regions[static_cast<size_t>(index)].start;
  baseSet_ = true;
  search_.clear();
  viewMemory();
  rebuildMemory();
}

void DevConsole::memoryGoto(const QString& addressText) {
  quint64 addr = 0;
  if (!emu::memaccess::parseAddress(addressText, &addr)) {
    setMemoryError(tr("Not an address. Use 0x08001A10 or a decimal number."));
    return;
  }
  base_ = addr & ~quint64(kRowBytes - 1);
  baseSet_ = true;
  if (mem_ != nullptr) {
    int i = 0;
    for (const auto& r : mem_->regions()) {
      if (addr >= r.start && addr - r.start < r.len) {
        regionIndex_ = i;
        break;
      }
      ++i;
    }
  }
  viewMemory();
  rebuildMemory();
}

void DevConsole::memoryScroll(int rows) {
  if (!baseSet_) return;
  const qint64 next = static_cast<qint64>(base_) + static_cast<qint64>(rows) * kRowBytes;
  base_ = next < 0 ? 0 : static_cast<quint64>(next);
  viewMemory();
  rebuildMemory();
}

void DevConsole::memoryRefresh() {
  if (mem_ == nullptr) return;
  if (!mem_->available()) {
    rows_.clear();
    rebuildMemory();
    return;
  }
  if (!baseSet_) {
    const auto regions = mem_->regions();
    if (regions.empty()) {
      rebuildMemory();
      return;
    }
    regionIndex_ = (std::min)(regionIndex_, static_cast<int>(regions.size()) - 1);
    base_ = regions[static_cast<size_t>(regionIndex_)].start;
    baseSet_ = true;
  }
  viewMemory();
  rebuildMemory();
}

void DevConsole::memorySearch(const QString& kind, const QString& text, const QString& type) {
  QVariantMap s{{QStringLiteral("kind"), kind}, {QStringLiteral("text"), text}, {QStringLiteral("type"), type}, {QStringLiteral("matches"), QVariantList()},
                {QStringLiteral("truncated"), false}, {QStringLiteral("scanned"), 0}, {QStringLiteral("error"), QString()}, {QStringLiteral("active"), true}};
  const auto finish = [&](const QString& err) {
    s.insert(QStringLiteral("error"), err);
    search_ = s;
    rebuildMemory();
  };
  if (mem_ == nullptr || !mem_->available()) return finish(tr("No game memory to search."));
  const auto regions = mem_->regions();
  if (regions.empty()) return finish(tr("The core exposes no memory map"));
  QByteArray pattern;
  if (kind == QLatin1String("bytes")) {
    pattern = emu::memaccess::parseHex(text);
    if (pattern.isEmpty()) return finish(tr("Enter bytes as hex, for example 46 42 4D 4D."));
  } else {
    quint64 v = 0;
    if (!emu::memaccess::parseAddress(text, &v)) return finish(tr("Enter a number (decimal or 0x hex)."));
    pattern = emu::memaccess::valueBytes(v, type.isEmpty() ? QStringLiteral("u8") : type);
    if (pattern.isEmpty()) return finish(tr("Choose u8, u16 or u32."));
  }
  const auto& r = regions[static_cast<size_t>((std::min)(regionIndex_, static_cast<int>(regions.size()) - 1))];
  emu::memaccess::SearchResult found;
  QString err;
  if (!mem_->search(r.start, r.start + r.len, pattern, 100, &found, &err)) return finish(err == QLatin1String("emulation_busy") ? tr("The game did not answer. Try again.") : tr("No game memory to search."));
  QVariantList matches;
  for (const quint64 a : found.matches) matches.append(emu::memaccess::hexAddress(a));
  s.insert(QStringLiteral("matches"), matches);
  s.insert(QStringLiteral("truncated"), found.truncated);
  s.insert(QStringLiteral("scanned"), static_cast<double>(found.scanned));
  finish(QString());
}

void DevConsole::memoryClearSearch() {
  search_.clear();
  rebuildMemory();
}

}  // namespace uzume::ui
