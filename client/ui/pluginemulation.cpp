#include "pluginemulation.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>

#include "gamesession.h"
#include "hubconnection.h"
#include "run_session_bridge.h"

namespace uzume::ui {

Q_LOGGING_CATEGORY(lcEmu, "uzume.plugins.emulation")

using plugins::CheatApplyResult;
using plugins::CheatItem;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int kMaxPendingWrites = 256;
constexpr int kMaxWriteBytes = 64 * 1024;
constexpr auto kMemEventEvery = std::chrono::seconds(10);
constexpr auto kConflictWindow = std::chrono::seconds(1);
constexpr auto kConflictEvery = std::chrono::seconds(30);
constexpr auto kDbRefreshEvery = std::chrono::minutes(5);

int idx(PluginEmulationService::Kind k) { return static_cast<int>(k); }

QString hexAddr(quint64 a) { return QStringLiteral("0x%1").arg(a, 8, 16, QLatin1Char('0')).toUpper().replace(QLatin1String("0X"), QLatin1String("0x")); }

QString atomicName(const QString& s) {
  QString out = s;
  out.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
  return out;
}
}  // namespace

PluginEmulationService::PluginEmulationService(GameSession* session, const QString& dataDir, QObject* parent)
    : QObject(parent), session_(session), dataDir_(dataDir) {
  qRegisterMetaType<Notice>("uzume::ui::PluginEmulationService::Notice");
  rulesTimer_.setInterval(300);
  connect(&rulesTimer_, &QTimer::timeout, this, &PluginEmulationService::onRulesChanged);
  if (session_ != nullptr) connect(session_, &GameSession::memoryRegionsChanged, this, [this]() { refreshRegions(); });
}

PluginEmulationService::~PluginEmulationService() = default;

void PluginEmulationService::setConnection(HubConnection* c) { conn_ = c; }

// ------------------------------------------------------------------ game lifecycle

void PluginEmulationService::gameStarted(const GameInfo& info, std::shared_ptr<plugins::RunBridge> run) {
  gameStopped();
  {
    QMutexLocker l(&mu_);
    active_ = true;
    game_ = info;
    run_ = std::move(run);
    for (int i = 0; i < 2; ++i) {
      consent_[i] = Consent::Ask;
      sessionDenied_[i] = snapDone_[i] = snapBusy_[i] = false;
    }
    usedMarked_ = false;
    userActed_ = false;
    queuedConsentPlugin_.clear();
    memToldPlugins_.clear();
    lastMemEvent_.clear();
    lastWrite_.clear();
    cacheHubId_ = conn_ ? conn_->hubId() : QString();
    online_ = conn_ && conn_->state() == HubConnection::State::Connected;
  }
  loadConsent();
  refreshRegions();
  const Rules r = rulesNow();
  lastRules_ = r.rules;
  lastAllowed_ = !r.inRun || r.rules.cheatsAllowed();
  lastMemAllowed_ = !r.inRun || r.rules.memoryWriteAllowed();
  if (auto rs = std::dynamic_pointer_cast<plugins::RunSessionBridge>(run_)) runConn_ = connect(rs.get(), &plugins::RunSessionBridge::rulesChanged, this, &PluginEmulationService::onRulesChanged);
  if (run_) rulesTimer_.start();
  emit cheatsOnChanged();
}

void PluginEmulationService::gameStopped() {
  rulesTimer_.stop();
  if (runConn_) disconnect(runConn_);
  bool hadCheats = false;
  {
    QMutexLocker l(&mu_);
    hadCheats = !wanted_.isEmpty();
    active_ = false;
    run_.reset();
    pendingWrites_.clear();
    wanted_.clear();
    enabledPrev_.clear();
    applyPending_ = false;
    pendingApply_.clear();
    regions_.clear();
    game_ = GameInfo();
  }
  consentRequest_.clear();
  consentOpen_ = false;
  consentPlugins_.clear();
  emit consentRequestChanged();
  if (hadCheats) emit cheatsOnChanged();
}

bool PluginEmulationService::active() const {
  QMutexLocker l(&mu_);
  return active_;
}

QString PluginEmulationService::gameKey() const {
  QMutexLocker l(&mu_);
  if (!game_.id.isEmpty()) return QStringLiteral("game:") + game_.id;
  if (!game_.sha256.isEmpty()) return QStringLiteral("rom:") + game_.sha256;
  return QStringLiteral("title:") + game_.title;
}

int PluginEmulationService::cheatsOn() const {
  QMutexLocker l(&mu_);
  int n = 0;
  for (const CheatItem& c : wanted_) n += c.enabled;
  return n;
}

QList<CheatItem> PluginEmulationService::activeCheatList() const {
  QMutexLocker l(&mu_);
  return wanted_;
}

void PluginEmulationService::refreshRegions() {
  std::vector<std::pair<quint64, quint64>> spans;
  if (session_ != nullptr)
    for (const auto& r : session_->memoryRegions()) spans.push_back({r.start, r.start + r.len});
  std::sort(spans.begin(), spans.end());
  std::vector<std::pair<quint64, quint64>> merged;
  for (const auto& s : spans) {
    if (!merged.empty() && s.first <= merged.back().second) merged.back().second = (std::max)(merged.back().second, s.second);
    else merged.push_back(s);
  }
  QMutexLocker l(&mu_);
  regions_ = std::move(merged);
}

bool PluginEmulationService::mapped(quint64 addr, quint64 len) const {  // caller holds the lock
  if (regions_.empty()) return true;  // the map is not known yet: the emulation thread decides (and logs "unmapped")
  for (const auto& r : regions_)
    if (addr >= r.first && addr + len <= r.second) return true;
  return false;
}

// ------------------------------------------------------------------ run rules

PluginEmulationService::Rules PluginEmulationService::rulesNow() const {
  std::shared_ptr<plugins::RunBridge> run;
  {
    QMutexLocker l(&mu_);
    run = run_;
  }
  Rules r;
  if (!run) return r;
  r.inRun = true;
  r.rules = parseRunRules(run->current().value(QStringLiteral("rules")).toObject());
  return r;
}

QString PluginEmulationService::cheatsBlockedReason() const {
  const Rules r = rulesNow();
  return r.inRun && !r.rules.cheatsAllowed() ? QStringLiteral("run_rule") : QString();
}

void PluginEmulationService::onRulesChanged() {
  const Rules r = rulesNow();
  const bool cheatsOk = !r.inRun || r.rules.cheatsAllowed();
  const bool memOk = !r.inRun || r.rules.memoryWriteAllowed();
  lastRules_ = r.rules;
  if (!cheatsOk && (lastAllowed_ || cheatsOn() > 0)) {
    if (cheatsOn() > 0)
      dropCheatsBecauseOf(QStringLiteral("run_rule"), tr("Cheats are off again"), tr("Your active cheats were turned off."));
    else if (lastAllowed_)
      postApp(QStringLiteral("cheats.blocked"), QVariantMap{{QStringLiteral("reason"), QStringLiteral("run_rule")}});
  }
  if (cheatsOk && !lastAllowed_) postApp(QStringLiteral("cheats.allowed"), QVariantMap());
  const bool newCheats = r.inRun && cheatsOk && !lastAllowed_;
  const bool newMem = r.inRun && memOk && !lastMemAllowed_;
  if (newCheats || newMem) {
    // The rules.changed event can arrive just after run_updated: wait a moment so the actor is known.
    QTimer::singleShot(300, this, [this, newCheats, newMem]() { announceRulesAllowed(newCheats, newMem); });
  }
  lastAllowed_ = cheatsOk;
  if (!memOk && lastMemAllowed_) {
    QMutexLocker l(&mu_);
    pendingWrites_.clear();  // writes stop at once
  }
  lastMemAllowed_ = memOk;
}

// F25: somebody else allowed cheats (or memory changes) in the run while this game runs: say who, as an info notice.
void PluginEmulationService::announceRulesAllowed(bool cheats, bool memory) {
  std::shared_ptr<plugins::RunBridge> run;
  {
    QMutexLocker l(&mu_);
    run = run_;
  }
  if (!run) return;
  const QString me = run->current().value(QStringLiteral("me")).toString();
  QString who;
  QString byId;
  const QJsonArray events = run->events(0, 500);
  for (qsizetype i = events.size() - 1; i >= 0; --i) {
    const QJsonObject e = events.at(i).toObject();
    if (e.value(QStringLiteral("type")).toString() != QLatin1String("rules.changed")) continue;
    const QJsonObject p = e.value(QStringLiteral("payload")).toObject();
    byId = p.value(QStringLiteral("by_user_id")).toString();
    who = p.value(QStringLiteral("by_user_name")).toString();
    if (who.isEmpty()) who = e.value(QStringLiteral("user_name")).toString();
    break;
  }
  if (!me.isEmpty() && byId == me) return;  // the person who changed it knows
  if (who.isEmpty()) who = tr("Someone");
  Notice n;
  n.pluginName = tr("Uzume");
  n.tone = QStringLiteral("info");
  n.title = tr("Run rules");
  n.text = cheats ? tr("%1 allowed cheats in this run").arg(who) : tr("%1 allowed memory changes in this run").arg(who);
  Q_UNUSED(memory)
  raise(n);
}

void PluginEmulationService::logRun(const QString& type, const QString& text, const QJsonObject& payload) {
  std::shared_ptr<plugins::RunBridge> run;
  {
    QMutexLocker l(&mu_);
    run = run_;
  }
  if (!run) return;
  const QString err = run->postCoreEvent(type, text.left(280), payload);
  if (!err.isEmpty()) qCInfo(lcEmu) << "run log" << type << "not written:" << err;
}

// ------------------------------------------------------------------ helpers

QString PluginEmulationService::nameOf(const QString& pluginId) const { return nameOf_ ? nameOf_(pluginId) : pluginId; }

void PluginEmulationService::raise(const Notice& n) { emit noticeRaised(n); }

void PluginEmulationService::postApp(const QString& name, const QVariant& v) {
  if (appEvent_) appEvent_(name, v);
}

void PluginEmulationService::markUsed() {
  {
    QMutexLocker l(&mu_);
    if (usedMarked_) return;
    usedMarked_ = true;
  }
  if (markCheatsUsed_) markCheatsUsed_();
}

void PluginEmulationService::takeSnapshot(Kind kind, const QString& label, std::function<void(bool)> then) {
  Q_UNUSED(kind)
  if (!snapshotFn_) {
    QMetaObject::invokeMethod(this, [then] { then(true); }, Qt::QueuedConnection);
    return;
  }
  snapshotFn_(label, [this, then](SnapshotOutcome o, const QString& msg) {
    if (o == SnapshotOutcome::Failed)
      raise({QString(), tr("Uzume"), QStringLiteral("warning"), tr("could not save a safety copy"), msg.isEmpty() ? tr("The change was not made.") : msg});
    then(o != SnapshotOutcome::Failed);
  });
}

// ------------------------------------------------------------------ consent (11d)

void PluginEmulationService::loadConsent() {
  QFile f(QDir(dataDir_).filePath(QStringLiteral("plugins/consent.json")));
  if (!f.open(QIODevice::ReadOnly)) return;
  const QJsonObject mine = QJsonDocument::fromJson(f.readAll()).object().value(gameKey()).toObject();
  QMutexLocker l(&mu_);
  for (Kind k : {Kind::Cheats, Kind::MemoryWrite})
    if (mine.value(kindName(k)).toString() == QLatin1String("allow")) consent_[idx(k)] = Consent::Allowed;
}

PluginEmulationService::Consent PluginEmulationService::storedConsent(const QString& key, Kind kind) const {
  QFile f(QDir(dataDir_).filePath(QStringLiteral("plugins/consent.json")));
  if (!f.open(QIODevice::ReadOnly)) return Consent::Ask;
  return QJsonDocument::fromJson(f.readAll()).object().value(key).toObject().value(kindName(kind)).toString() == QLatin1String("allow") ? Consent::Allowed
                                                                                                                                      : Consent::Ask;
}

void PluginEmulationService::saveConsent() const {}

PluginEmulationService::Consent PluginEmulationService::consent(Kind kind) const {
  QMutexLocker l(&mu_);
  if (sessionDenied_[idx(kind)]) return Consent::Denied;
  return consent_[idx(kind)];
}

void PluginEmulationService::setConsent(Kind kind, Consent value, const QString& key) {
  const QString k = key.isEmpty() ? gameKey() : key;
  const QString path = QDir(dataDir_).filePath(QStringLiteral("plugins/consent.json"));
  QJsonObject all;
  {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) all = QJsonDocument::fromJson(f.readAll()).object();
  }
  QJsonObject mine = all.value(k).toObject();
  if (value == Consent::Allowed) mine.insert(kindName(kind), QStringLiteral("allow"));
  else mine.remove(kindName(kind));
  if (mine.isEmpty()) all.remove(k);
  else all.insert(k, mine);
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile out(path);
  if (out.open(QIODevice::WriteOnly)) {
    out.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
    out.commit();
  }
  if (!key.isEmpty() && k != gameKey()) return;  // another game: only the file changes
  QMutexLocker l(&mu_);
  sessionDenied_[idx(kind)] = value == Consent::Denied;
  consent_[idx(kind)] = value == Consent::Allowed ? Consent::Allowed : Consent::Ask;
}

void PluginEmulationService::noteUserAction() {
  QString queued;
  {
    QMutexLocker l(&mu_);
    if (!active_ || userActed_) return;
    userActed_ = true;
    queued = std::move(queuedConsentPlugin_);
    queuedConsentPlugin_.clear();
  }
  if (!queued.isEmpty()) requestConsent(Kind::Cheats, queued);
}

void PluginEmulationService::requestConsent(Kind kind, const QString& pluginId) {
  if (consent(kind) != Consent::Ask) return;
  const QString plugin = nameOf(pluginId);
  if (consentOpen_) {
    if (consentKind_ == kind && !consentPlugins_.contains(plugin)) consentPlugins_ << plugin;
    return;
  }
  QString title;
  {
    QMutexLocker l(&mu_);
    title = game_.title;
  }
  consentOpen_ = true;
  consentKind_ = kind;
  consentPlugins_ = QStringList{plugin};
  QVariantMap m;
  m.insert(QStringLiteral("kind"), kindName(kind));
  m.insert(QStringLiteral("plugins"), consentPlugins_);
  if (kind == Kind::Cheats) {
    m.insert(QStringLiteral("title"), tr("Turn on cheats for %1?").arg(title));
    m.insert(QStringLiteral("label"), tr("CHEATS · ONCE PER GAME"));
    m.insert(QStringLiteral("items"), QStringList{tr("Uzume first saves a safety copy of your save: it is kept as \u201cBefore cheats\u201d."),
                                                  tr("Later saves are marked \u201cMade with cheats\u201d."),
                                                  tr("In a run, everyone sees each cheat in the run log.")});
    m.insert(QStringLiteral("meta"), tr("Asked once per game. Codes run inside the emulator core."));
    m.insert(QStringLiteral("confirmLabel"), tr("Turn on cheats"));
  } else {
    m.insert(QStringLiteral("title"), tr("Let plugins change game memory in %1?").arg(title));
    m.insert(QStringLiteral("label"), tr("GAME MEMORY · ONCE PER GAME"));
    m.insert(QStringLiteral("items"), QStringList{tr("Uzume first saves a safety copy of your save: it is kept as \u201cBefore memory changes\u201d."),
                                                  tr("Later saves are marked \u201cMade with cheats\u201d."),
                                                  tr("In a run, everyone sees in the run log that memory was changed.")});
    m.insert(QStringLiteral("meta"), tr("Asked once per game. Asked by: %1.").arg(plugin));
    m.insert(QStringLiteral("confirmLabel"), tr("Allow changes"));
  }
  consentRequest_ = m;
  emit consentRequestChanged();
}

void PluginEmulationService::answerConsent(bool allow) {
  if (!consentOpen_) return;
  const Kind kind = consentKind_;
  consentOpen_ = false;
  consentRequest_.clear();
  consentPlugins_.clear();
  emit consentRequestChanged();
  finishConsent(kind, allow);
}

void PluginEmulationService::finishConsent(Kind kind, bool allow) {
  setConsent(kind, allow ? Consent::Allowed : Consent::Denied);
  if (kind == Kind::MemoryWrite) {
    if (allow) pumpWrites();
    else {
      QMutexLocker l(&mu_);
      pendingWrites_.clear();
    }
    return;
  }
  QList<CheatItem> list;
  QString plugin;
  bool had = false;
  {
    QMutexLocker l(&mu_);
    had = applyPending_;
    list = pendingApply_;
    plugin = pendingApplyPlugin_;
    applyPending_ = false;
    pendingApply_.clear();
  }
  if (allow && had) applyInternal(plugin, list, false, true);
  else if (!allow) postApp(QStringLiteral("cheats.blocked"), QVariantMap{{QStringLiteral("reason"), QStringLiteral("consent_denied")}});
}

// ------------------------------------------------------------------ memory writes

QString PluginEmulationService::writeMemory(const QString& pluginId, quint64 addr, const QByteArray& bytes) {
  bool ask = false;
  bool logEvent = false;
  QString pluginName;
  std::optional<std::pair<QString, QString>> conflict;
  {
    QMutexLocker l(&mu_);
    if (!active_) return QStringLiteral("no_game");
  }
  if (bytes.isEmpty() || bytes.size() > kMaxWriteBytes) return QStringLiteral("invalid");
  const Rules rules = rulesNow();
  if (rules.inRun && !rules.rules.memoryWriteAllowed()) return QStringLiteral("blocked_by_run_rule");
  {
    QMutexLocker l(&mu_);
    if (!mapped(addr, static_cast<quint64>(bytes.size()))) return QStringLiteral("unmapped");
    if (sessionDenied_[idx(Kind::MemoryWrite)]) return QStringLiteral("consent_denied");
    if (consent_[idx(Kind::MemoryWrite)] == Consent::Ask) ask = true;
    else if (pendingWrites_.size() >= kMaxPendingWrites) return QStringLiteral("queue_full");
    else {
      const auto now = Clock::now();
      LastWrite& lw = lastWrite_[addr];
      if (!lw.plugin.isEmpty() && lw.plugin != pluginId && now - lw.at < kConflictWindow) {
        if (++lw.hits >= 3 && (lw.reported.time_since_epoch().count() == 0 || now - lw.reported > kConflictEvery)) {
          lw.reported = now;
          conflict = {lw.plugin, pluginId};
        }
      }
      lw.plugin = pluginId;
      lw.at = now;
      if (rules.inRun) {
        auto it = lastMemEvent_.find(pluginId);
        if (it == lastMemEvent_.end() || now - it->second >= kMemEventEvery) {
          lastMemEvent_[pluginId] = now;
          logEvent = true;
        }
      }
      pendingWrites_.append({pluginId, addr, bytes});
    }
  }
  if (ask) {
    QMetaObject::invokeMethod(this, [this, pluginId] { requestConsent(Kind::MemoryWrite, pluginId); }, Qt::QueuedConnection);
    return QStringLiteral("consent_required");
  }
  if (logEvent) logRun(QStringLiteral("memory.write"), tr("%1 changed game memory").arg(nameOf(pluginId)), QJsonObject{{QStringLiteral("plugin_id"), pluginId}});
  if (conflict) {
    const auto [a, b] = *conflict;
    qCWarning(lcEmu) << "plugins" << a << "and" << b << "keep writing the same address" << hexAddr(addr);
    emit writeConflict(addr, a, b);
    raise({QString(), tr("Uzume"), QStringLiteral("warning"), tr("two plugins fight over game memory"),
           tr("%1 and %2 keep changing %3. The last write wins each time; turn one of them off.").arg(nameOf(a), nameOf(b), hexAddr(addr))});
  }
  QMetaObject::invokeMethod(this, [this] { pumpWrites(); }, Qt::QueuedConnection);
  return {};
}

void PluginEmulationService::pumpWrites() {
  bool needSnapshot = false;
  QList<PendingWrite> batch;
  {
    QMutexLocker l(&mu_);
    if (pendingWrites_.isEmpty() || !active_) return;
    if (!snapDone_[idx(Kind::MemoryWrite)]) {
      if (snapBusy_[idx(Kind::MemoryWrite)]) return;
      snapBusy_[idx(Kind::MemoryWrite)] = needSnapshot = true;
    } else {
      batch = std::exchange(pendingWrites_, {});
    }
  }
  if (needSnapshot) {
    takeSnapshot(Kind::MemoryWrite, tr("Before memory changes"), [this](bool ok) {
      {
        QMutexLocker l(&mu_);
        snapBusy_[idx(Kind::MemoryWrite)] = false;
        if (ok) snapDone_[idx(Kind::MemoryWrite)] = true;
        else pendingWrites_.clear();
      }
      if (ok) pumpWrites();
    });
    return;
  }
  if (session_ == nullptr || !session_->isActive()) return;
  markUsed();
  QStringList told;
  for (const PendingWrite& w : batch) {
    const quint64 id = session_->submitEmuCommand(emu::EmulationRunner::Command::writeMemory(w.addr, w.bytes),
                                                  [addr = w.addr, plugin = w.pluginId](const emu::EmulationRunner::CommandResult& r) {
                                                    if (!r.ok) qCInfo(lcEmu) << "memory write of" << plugin << "at" << hexAddr(addr) << "failed:" << r.error;
                                                  });
    if (id == 0) qCInfo(lcEmu) << "memory write dropped: the emulation is not running";
    bool first = false;
    {
      QMutexLocker l(&mu_);
      first = !memToldPlugins_.contains(w.pluginId);
      memToldPlugins_.insert(w.pluginId);
    }
    if (first) raise({w.pluginId, nameOf(w.pluginId), QStringLiteral("info"), tr("changed game memory"), QString()});
  }
}

// ------------------------------------------------------------------ cheats

QStringList PluginEmulationService::cheatFormats() const {
  QMutexLocker l(&mu_);
  return active_ ? game_.cheatFormats : QStringList();
}

QString PluginEmulationService::coreName() const {
  QMutexLocker l(&mu_);
  return active_ ? game_.coreName : QString();
}

bool PluginEmulationService::validCheatCode(const QString& format, const QString& code) {
  if (code.trimmed().isEmpty() || code.size() > 4096) return false;
  for (QChar c : code)
    if (c.unicode() < 0x20 && c != QLatin1Char('\n') && c != QLatin1Char('\r') && c != QLatin1Char('\t')) return false;
  if (format == QLatin1String("raw")) return true;
  static const QRegularExpression hex(QStringLiteral("^[0-9A-Fa-f +:\\-\\t\\r\\n]+$"));
  static const QRegularExpression genie(QStringLiteral("^[0-9A-Za-z +:\\-\\t\\r\\n]+$"));
  return (format == QLatin1String("game_genie") ? genie : hex).match(code).hasMatch();
}

CheatApplyResult PluginEmulationService::applyCheats(const QString& pluginId, const QList<CheatItem>& list) {
  return applyInternal(pluginId, list, false, false);
}

CheatApplyResult PluginEmulationService::applyCheatsAs(const QString& pluginId, const QList<CheatItem>& list, bool bypassConsent) {
  return applyInternal(pluginId, list, bypassConsent, false);
}

CheatApplyResult PluginEmulationService::applyInternal(const QString& pluginId, const QList<CheatItem>& list, bool bypassConsent, bool replay) {
  CheatApplyResult res;
  QStringList formats;
  {
    QMutexLocker l(&mu_);
    if (!active_) {
      res.error = QStringLiteral("no_game");
      return res;
    }
    formats = game_.cheatFormats;
  }
  QList<CheatItem> accepted;
  bool anyEnabled = false;
  for (const CheatItem& c : list) {
    if (!formats.contains(c.format)) res.rejected.append({c.id, QStringLiteral("unsupported_format")});
    else if (!validCheatCode(c.format, c.code)) res.rejected.append({c.id, QStringLiteral("invalid_code")});
    else {
      accepted.append(c);
      if (c.enabled) {
        anyEnabled = true;
        ++res.applied;
      }
    }
  }
  const Rules rules = rulesNow();
  if (anyEnabled && rules.inRun && !rules.rules.cheatsAllowed()) {
    postApp(QStringLiteral("cheats.blocked"), QVariantMap{{QStringLiteral("reason"), QStringLiteral("run_rule")}});
    return {QStringLiteral("blocked_by_run_rule"), 0, {}};
  }
  if (anyEnabled && !bypassConsent) {
    bool ask = false, denied = false;
    {
      QMutexLocker l(&mu_);
      denied = sessionDenied_[idx(Kind::Cheats)];
      ask = !denied && consent_[idx(Kind::Cheats)] == Consent::Ask;
      if (ask && !replay) {
        applyPending_ = true;
        pendingApply_ = list;
        pendingApplyPlugin_ = pluginId;
      }
    }
    if (denied) {
      postApp(QStringLiteral("cheats.blocked"), QVariantMap{{QStringLiteral("reason"), QStringLiteral("consent_denied")}});
      return {QStringLiteral("consent_denied"), 0, {}};
    }
    if (ask) {
      // Remembered cheats applied by the plugin at the start of the game: queue the question until the person acts in a plugin.
      bool acted = true;
      {
        QMutexLocker l(&mu_);
        acted = userActed_;
        if (!acted) queuedConsentPlugin_ = pluginId;
      }
      if (acted) QMetaObject::invokeMethod(this, [this, pluginId] { requestConsent(Kind::Cheats, pluginId); }, Qt::QueuedConnection);
      return {QStringLiteral("consent_required"), 0, {}};
    }
  }
  // Accepted: the run log gets the differences.
  QHash<QString, CheatItem> now;
  for (const CheatItem& c : accepted)
    if (c.enabled) now.insert(c.id, c);
  QHash<QString, CheatItem> before;
  {
    QMutexLocker l(&mu_);
    before = enabledPrev_;
    wanted_ = accepted;
    wantedBy_ = pluginId;
    enabledPrev_ = now;
  }
  if (rules.inRun) {
    for (auto it = now.begin(); it != now.end(); ++it)
      if (!before.contains(it.key()))
        logRun(QStringLiteral("cheat.enabled"), tr("turned on %1").arg(it->name.isEmpty() ? it->id : it->name),
               QJsonObject{{QStringLiteral("name"), it->name}, {QStringLiteral("format"), it->format}, {QStringLiteral("plugin_id"), pluginId}});
    for (auto it = before.begin(); it != before.end(); ++it)
      if (!now.contains(it.key()))
        logRun(QStringLiteral("cheat.disabled"), tr("turned off %1").arg(it->name.isEmpty() ? it->id : it->name),
               QJsonObject{{QStringLiteral("name"), it->name}, {QStringLiteral("format"), it->format}, {QStringLiteral("plugin_id"), pluginId}});
  }
  emit cheatsOnChanged();
  QMetaObject::invokeMethod(this, [this] { pumpCheats(); }, Qt::QueuedConnection);
  return res;
}

QString PluginEmulationService::clearCheats(const QString& pluginId) {
  const CheatApplyResult r = applyInternal(pluginId, {}, false, false);
  return r.error;
}

void PluginEmulationService::pumpCheats() {
  bool needSnapshot = false;
  {
    QMutexLocker l(&mu_);
    if (!active_) return;
    int on = 0;
    for (const CheatItem& c : wanted_) on += c.enabled;
    if (on > 0 && !snapDone_[idx(Kind::Cheats)]) {
      if (snapBusy_[idx(Kind::Cheats)]) return;
      snapBusy_[idx(Kind::Cheats)] = needSnapshot = true;
    }
  }
  if (needSnapshot) {
    takeSnapshot(Kind::Cheats, tr("Before cheats"), [this](bool ok) {
      {
        QMutexLocker l(&mu_);
        snapBusy_[idx(Kind::Cheats)] = false;
        if (ok) snapDone_[idx(Kind::Cheats)] = true;
      }
      if (ok) pumpCheats();
      else dropCheatsBecauseOf(QStringLiteral("snapshot_failed"), tr("Cheats were not turned on"), tr("Uzume could not save a safety copy first."));
    });
    return;
  }
  commitCheats();
}

void PluginEmulationService::commitCheats() {
  if (session_ == nullptr || !session_->isActive()) return;
  QList<emu::EmulationRunner::CheatEntry> entries;
  int on = 0;
  {
    QMutexLocker l(&mu_);
    for (const CheatItem& c : wanted_) {
      entries.append({c.id, c.format, c.code, c.enabled});
      on += c.enabled;
    }
  }
  if (on > 0) markUsed();
  session_->submitEmuCommand(emu::EmulationRunner::Command::setCheats(entries), [](const emu::EmulationRunner::CommandResult& r) {
    if (!r.ok) qCInfo(lcEmu) << "cheat list refused by the core:" << r.error;
  });
}

void PluginEmulationService::dropCheatsBecauseOf(const QString& reason, const QString& noticeTitle, const QString& noticeText) {
  QHash<QString, plugins::CheatItem> was;
  {
    QMutexLocker l(&mu_);
    was = enabledPrev_;
    wanted_.clear();
    enabledPrev_.clear();
    applyPending_ = false;
  }
  if (session_ != nullptr && session_->isActive()) session_->submitEmuCommand(emu::EmulationRunner::Command::setCheats({}));
  for (const plugins::CheatItem& it : std::as_const(was))  // the run log shows that they are off again
    logRun(QStringLiteral("cheat.disabled"), tr("turned off %1").arg(it.name.isEmpty() ? it.id : it.name),
           QJsonObject{{QStringLiteral("name"), it.name}, {QStringLiteral("format"), it.format}});
  emit cheatsOnChanged();
  postApp(QStringLiteral("cheats.blocked"), QVariantMap{{QStringLiteral("reason"), reason}});
  raise({QString(), tr("Uzume"), reason == QLatin1String("run_rule") ? QStringLiteral("info") : QStringLiteral("warning"), noticeTitle, noticeText});
}

// ------------------------------------------------------------------ cheat database

plugins::CheatDatabaseResult PluginEmulationService::cheatDatabase() {
  plugins::CheatDatabaseResult res;
  QString gameId, hubId, err;
  bool online = false;
  {
    QMutexLocker l(&mu_);
    if (!active_) {
      res.error = QStringLiteral("no_game");
      return res;
    }
    gameId = game_.id;
    hubId = cacheHubId_;
    online = online_;
    err = dbError_.value(game_.id);
  }
  if (gameId.isEmpty()) {
    res.error = QStringLiteral("not_found");
    return res;
  }
  const QString path = QDir(dataDir_).filePath(QStringLiteral("cheatdb/%1/%2.json").arg(atomicName(hubId), atomicName(gameId)));
  bool stale = true;
  {
    QMutexLocker l(&mu_);
    const auto it = dbFetched_.constFind(gameId);
    stale = it == dbFetched_.constEnd() || Clock::now() - it.value() > kDbRefreshEvery;
  }
  if (stale) QMetaObject::invokeMethod(this, [this, gameId] { refreshCheatDb(gameId); }, Qt::QueuedConnection);
  QFile f(path);
  if (f.open(QIODevice::ReadOnly)) {
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (!o.isEmpty()) {
      if (o.value(QStringLiteral("source")).toString() == QLatin1String("none")) {
        res.error = QStringLiteral("off");
        return res;
      }
      res.data = o;
      return res;
    }
  }
  if (!err.isEmpty()) res.error = err;
  else res.error = online ? QStringLiteral("error: loading") : QStringLiteral("offline");
  return res;
}

void PluginEmulationService::refreshCheatDb(const QString& gameId) {
  {
    QMutexLocker l(&mu_);
    if (dbBusy_.contains(gameId)) return;
    dbBusy_.insert(gameId);
    dbFetched_[gameId] = Clock::now();
  }
  const auto fail = [this, gameId](const QString& reason, bool storeError) {
    {
      QMutexLocker l(&mu_);
      dbBusy_.remove(gameId);
      online_ = reason != QLatin1String("offline");
      if (storeError) dbError_[gameId] = reason;
    }
    postApp(QStringLiteral("cheats.database"), QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("reason"), reason}});
  };
  if (!conn_ || conn_->state() != HubConnection::State::Connected) return fail(QStringLiteral("offline"), false);
  if (!conn_->hubHasFeature(QStringLiteral("cheat_db_v1"))) return fail(QStringLiteral("off"), true);
  QNetworkReply* reply = conn_->authorizedGet(QStringLiteral("/games/%1/cheats").arg(QString::fromLatin1(QUrl::toPercentEncoding(gameId))));
  if (reply == nullptr) return fail(QStringLiteral("offline"), false);
  const QString hubId = conn_->hubId();
  connect(reply, &QNetworkReply::finished, this, [this, reply, gameId, hubId, fail]() {
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    if (r.networkError || r.certMismatch) return fail(QStringLiteral("offline"), false);
    if (r.status == 404) return fail(QStringLiteral("not_found"), true);
    if (!r.ok()) return fail(QStringLiteral("error: ") + (r.apiErrorMessage.isEmpty() ? QString::number(r.status) : r.apiErrorMessage), true);
    const QString path = QDir(dataDir_).filePath(QStringLiteral("cheatdb/%1/%2.json").arg(atomicName(hubId), atomicName(gameId)));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly)) {
      out.write(r.body);
      out.commit();
    }
    {
      QMutexLocker l(&mu_);
      dbBusy_.remove(gameId);
      dbError_.remove(gameId);
      online_ = true;
    }
    postApp(QStringLiteral("cheats.database"), QVariantMap{{QStringLiteral("ok"), true}});
  });
}

// ------------------------------------------------------------------ fb.save.snapshot

QString PluginEmulationService::snapshot(const QString& pluginId, const QString& pluginName, const QString& reason) {
  {
    QMutexLocker l(&mu_);
    if (!active_) return QStringLiteral("no_game");
  }
  if (!snapshotFn_) return QStringLiteral("unsupported");
  QMetaObject::invokeMethod(
      this,
      [this, pluginId, pluginName, reason] {
        const QString label = QStringLiteral("%1: %2").arg(pluginName, reason).left(120);
        snapshotFn_(label, [this, pluginId, pluginName, reason](SnapshotOutcome o, const QString& msg) {
          if (o == SnapshotOutcome::Ok) raise({pluginId, pluginName, QStringLiteral("done"), tr("saved a snapshot · %1").arg(reason), QString()});
          else
            raise({pluginId, pluginName, QStringLiteral("warning"), tr("could not save a snapshot"),
                   o == SnapshotOutcome::NoSave ? tr("There is no save on the server yet, so there is nothing to snapshot.") : msg});
        });
      },
      Qt::QueuedConnection);
  return {};
}

}  // namespace uzume::ui
