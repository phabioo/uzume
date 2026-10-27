#include "variantscontroller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QTimer>
#include <algorithm>
#include <cmath>

#include "corecache.h"
#include "localstorage.h"

namespace uzume::ui {

Q_LOGGING_CATEGORY(lcVariants, "uzume.variants")

namespace {

QString formatBytesText(qint64 bytes) {
  constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
  constexpr double kMiB = 1024.0 * 1024.0;
  const QLocale c = QLocale::c();
  if (bytes >= static_cast<qint64>(kGiB)) return QStringLiteral("%1 GB").arg(c.toString(static_cast<double>(bytes) / kGiB, 'f', 1));
  if (bytes >= static_cast<qint64>(kMiB)) return QStringLiteral("%1 MB").arg(c.toString(static_cast<double>(bytes) / kMiB, 'f', 0));
  return QStringLiteral("%1 KB").arg(c.toString(static_cast<double>(bytes) / 1024.0, 'f', 0));
}

QString etaTextOf(qint64 etaMs) {
  if (etaMs <= 0) return {};
  const qint64 s = (etaMs + 999) / 1000;
  if (s < 90) return QObject::tr("about %1 s left").arg(((s + 4) / 5) * 5);
  return QObject::tr("about %1 min left").arg((s + 59) / 60);
}

bool allDigits(const QString& s) {
  if (s.isEmpty() || s.size() > 19) return false;
  for (const QChar c : s)
    if (!c.isDigit()) return false;
  bool ok = false;
  s.toLongLong(&ok);
  return ok;
}

}  // namespace

// ---------------------------------------------------------------- plugin bridge

plugins::VariantEngineInfo VariantsPluginBridge::engine() const {
  QMutexLocker l(&mutex_);
  return engine_;
}

QList<plugins::VariantRow> VariantsPluginBridge::list(const QString& gameId) const {
  QMutexLocker l(&mutex_);
  return rows_.value(gameId);
}

void VariantsPluginBridge::setEngine(const plugins::VariantEngineInfo& e) {
  QMutexLocker l(&mutex_);
  engine_ = e;
}

void VariantsPluginBridge::setRows(const QHash<QString, QList<plugins::VariantRow>>& rows) {
  QMutexLocker l(&mutex_);
  rows_ = rows;
}

quint64 VariantsPluginBridge::build(const QString& pluginId, const plugins::VariantBuildRequest& req) {
  const quint64 id = nextRequestId();
  VariantsController* owner = owner_.load();
  if (owner != nullptr) {
    QMetaObject::invokeMethod(owner, [owner, pluginId, req, id]() { owner->buildForPlugin(pluginId, req, id); }, Qt::QueuedConnection);
  }
  return id;
}

// ---------------------------------------------------------------- controller

VariantsController::VariantsController(Deps deps, std::shared_ptr<VariantsPluginBridge> bridge, QObject* parent)
    : QObject(parent),
      deps_(std::move(deps)),
      bridge_(std::move(bridge)),
      builder_(deps_.toolCache, deps_.cache, ToolCache::normalizePlatform(deps_.platform.isEmpty() ? CoreCache::currentPlatform() : deps_.platform)) {
  if (!bridge_) bridge_ = std::make_shared<VariantsPluginBridge>();
  bridge_->attach(this);
  {
    const QPointer<VariantsController> self(this);
    bridge_->registry()->setOnChange([self]() {
      if (self) QMetaObject::invokeMethod(self.data(), [self]() { if (self) { ++self->registryRevision_; emit self->registryChanged(); } }, Qt::QueuedConnection);
    });
  }
  connect(&builder_, &VariantBuilder::progress, this, &VariantsController::onProgress);
  connect(&builder_, &VariantBuilder::finished, this, &VariantsController::onFinished);
  if (deps_.tools != nullptr) {
    connect(deps_.tools, &ToolsClient::toolsChanged, this, &VariantsController::refreshEngine);
    connect(deps_.tools, &ToolsClient::toolsChanged, this, &VariantsController::onToolsRefreshed);
    connect(deps_.tools, &ToolsClient::refreshFailed, this, &VariantsController::onToolsRefreshed);
    connect(deps_.tools, &ToolsClient::progress, this, [this](const QString&, qint64 got, qint64 total) {
      engineReceived_ = static_cast<double>(got);
      engineTotal_ = static_cast<double>(total);
      refreshEngine();
    });
    connect(deps_.tools, &ToolsClient::prepared, this, &VariantsController::onToolPrepared);
  }
  if (deps_.conn != nullptr) {
    connect(deps_.conn, &HubConnection::stateChanged, this, [this](HubConnection::State s) {
      if (s == HubConnection::State::Connected) {
        if (deps_.tools != nullptr && deps_.tools->available()) deps_.tools->refresh();
        reloadList();
        reloadAll();
      }
      refreshEngine();
    });
  }
  if (deps_.downloader != nullptr) {
    connect(deps_.downloader, &RomDownloader::romReady, this, &VariantsController::onRomReady);
    connect(deps_.downloader, &RomDownloader::statusChanged, this, [this](const QString& sha, const RomStatus& st) {
      if (!active_ || !activeWaitingBase_ || activeBaseSha_ != sha) return;
      if (st.state == RomState::Failed || st.state == RomState::HashMismatch || st.state == RomState::Missing) failActive(QStringLiteral("base_missing"));
    });
  }
  loadRunChoices();
  refreshEngine();
  if (deps_.tools != nullptr && deps_.tools->available()) deps_.tools->refresh();
  QTimer::singleShot(0, this, [this]() { trimCache(); });
}

VariantsController::~VariantsController() {
  if (bridge_) {
    bridge_->attach(nullptr);
    bridge_->registry()->setOnChange({});
  }
}

QString VariantsController::platformKey() const {
  return ToolCache::normalizePlatform(deps_.platform.isEmpty() ? CoreCache::currentPlatform() : deps_.platform);
}

bool VariantsController::serverSupport() const {
  return deps_.client != nullptr && deps_.client->available() && deps_.tools != nullptr && deps_.tools->available();
}

QString VariantsController::errorText(const QString& code) const {
  if (code == QLatin1String("engine_missing")) return tr("The randomizer engine is not on this PC.");
  if (code == QLatin1String("engine_not_offered")) return tr("The server does not offer the randomizer engine.");
  if (code == QLatin1String("engine_download_failed")) return tr("The engine could not be downloaded.");
  if (code == QLatin1String("base_missing")) return tr("The game file is not on this PC.");
  if (code == QLatin1String("base_mismatch")) return tr("Your file of this game is a different dump.");
  if (code == QLatin1String("not_enough_memory")) return tr("Not enough free memory for the engine.");
  if (code == QLatin1String("disk_full")) return tr("Not enough free disk space.");
  if (code == QLatin1String("engine_failed")) return tr("The engine stopped while writing the game data. Nothing was kept; Main is not touched.");
  if (code == QLatin1String("timed_out")) return tr("The engine took too long and was stopped.");
  if (code == QLatin1String("cancelled")) return tr("Stopped. Nothing was kept.");
  if (code == QLatin1String("output_missing")) return tr("The engine finished without writing a game.");
  return tr("The variant could not be built.");
}

// ---------------------------------------------------------------- engine

void VariantsController::refreshEngine() {
  QVariantMap e;
  QString state;
  QString version;
  ToolPlatformInfo plat;
  const bool online = serverSupport();
  if (online) {
    version = deps_.tools->offeredVersion(QStringLiteral("randomizer"), &plat);
    if (version.isEmpty()) state = QStringLiteral("not_offered");
    else if (deps_.toolCache->isReady(QStringLiteral("randomizer"), version, platformKey())) state = QStringLiteral("ready");
    else if (engineBusy_) state = QStringLiteral("downloading");
    else if (!engineError_.isEmpty()) state = QStringLiteral("failed");
    else state = QStringLiteral("not_on_pc");
  } else {
    const QStringList have = deps_.toolCache ? deps_.toolCache->versions(QStringLiteral("randomizer"), platformKey()) : QStringList();
    if (!have.isEmpty()) {
      state = QStringLiteral("ready");  // offline: the engine on this PC still builds
      version = have.first();
    } else {
      state = QStringLiteral("no_server");
    }
  }
  e.insert(QStringLiteral("state"), state);
  e.insert(QStringLiteral("version"), version);
  e.insert(QStringLiteral("sizeBytes"), static_cast<double>(plat.size));
  e.insert(QStringLiteral("sizeText"), plat.size > 0 ? formatBytesText(plat.size) : QString());
  const ToolInfo* t = deps_.tools != nullptr ? deps_.tools->tool(QStringLiteral("randomizer")) : nullptr;
  e.insert(QStringLiteral("license"), t ? t->license : QString());
  e.insert(QStringLiteral("sourceUrl"), t ? t->sourceUrl : QString());
  e.insert(QStringLiteral("received"), engineReceived_);
  e.insert(QStringLiteral("total"), engineTotal_);
  e.insert(QStringLiteral("receivedText"), formatBytesText(static_cast<qint64>(engineReceived_)));
  e.insert(QStringLiteral("totalText"), formatBytesText(static_cast<qint64>(engineTotal_)));
  e.insert(QStringLiteral("percent"), engineTotal_ > 0 ? std::min(100.0, engineReceived_ * 100.0 / engineTotal_) : 0.0);
  e.insert(QStringLiteral("error"), engineError_);
  e.insert(QStringLiteral("offline"), !online);
  if (e != engine_) {
    engine_ = e;
    emit engineChanged();
  }
  plugins::VariantEngineInfo info;
  info.offered = state == QLatin1String("not_on_pc") || state == QLatin1String("downloading") || state == QLatin1String("ready") || state == QLatin1String("failed");
  info.version = version;
  info.onPc = state == QLatin1String("ready");
  bridge_->setEngine(info);
}

void VariantsController::prepareEngine() {
  if (deps_.tools != nullptr && deps_.tools->busy()) return;  // a download runs: its end reports
  if (deps_.tools == nullptr || !deps_.tools->available()) {
    refreshEngine();
    emit engineChanged();  // nothing to prepare: waiters finish with the state
    return;
  }
  const QString version = deps_.tools->offeredVersion(QStringLiteral("randomizer"));
  if (version.isEmpty()) {
    // The list may be older than the admin's change: read it again, then prepare once a version is offered.
    if (engineRefreshing_) return;
    engineRefreshing_ = true;
    deps_.tools->refresh();
    return;
  }
  engineBusy_ = true;
  engineError_.clear();
  engineReceived_ = engineTotal_ = 0;
  refreshEngine();
  deps_.tools->prepare(QStringLiteral("randomizer"), version);
}

void VariantsController::onToolsRefreshed() {
  if (!engineRefreshing_) return;
  engineRefreshing_ = false;
  if (deps_.tools != nullptr && !deps_.tools->offeredVersion(QStringLiteral("randomizer")).isEmpty()) return prepareEngine();
  refreshEngine();
  emit engineChanged();  // still nothing offered: waiters finish with the state
}

void VariantsController::onToolPrepared(const ToolPrepareResult& r) {
  engineBusy_ = false;
  engineReceived_ = engineTotal_ = 0;
  if (r.ok) {
    engineError_.clear();
  } else {
    switch (r.problem) {
      case ToolProblem::NotOffered:
      case ToolProblem::NotOnServer:
      case ToolProblem::Incompatible: engineError_ = QStringLiteral("engine_not_offered"); break;
      case ToolProblem::Cancelled: engineError_.clear(); break;
      default: engineError_ = QStringLiteral("engine_download_failed"); break;
    }
  }
  refreshEngine();
  if (active_ && activeWaitingEngine_ && active_->recipe.toolId == r.toolId && active_->recipe.toolVersion == r.version) {
    activeWaitingEngine_ = false;
    if (r.ok) continueJob();
    else failActive(engineError_.isEmpty() ? QStringLiteral("cancelled") : engineError_);
  }
}

// ---------------------------------------------------------------- the game's variants

void VariantsController::setGame(const QString& gameId) {
  gameId_ = gameId;
  rebuildLists();
  reloadList();
}

void VariantsController::refresh() { reloadList(); }

void VariantsController::reloadList() {
  if (gameId_.isEmpty()) {
    rebuildLists();
    return;
  }
  if (deps_.client == nullptr || !deps_.client->available()) {
    loadedOk_[gameId_] = false;
    rebuildLists();
    publishPluginState();
    return;
  }
  const quint64 gen = ++listGen_;
  const QString gid = gameId_;
  deps_.client->list(gid, [this, gen, gid](const QList<VariantInfo>& list, const VariantCall& c) {
    Q_UNUSED(gen);
    if (c.ok) {
      // The Randomize dialog lists and creates right after each other: an answer to a list sent before a create can arrive after
      // it. Keep the variants this PC is building that the answer does not know yet, or their rows would vanish.
      QList<VariantInfo> merged = list;
      for (const VariantInfo& old : byGame_.value(gid)) {
        if (stateOf(old.id) != QLatin1String("building")) continue;
        bool known = false;
        for (const VariantInfo& v : list) known = known || v.id == old.id;
        if (!known) merged.prepend(old);
      }
      byGame_.insert(gid, merged);
      loadedOk_[gid] = true;
    } else {
      loadedOk_[gid] = false;
    }
    rebuildLists();
    publishPluginState();
  });
}

std::optional<VariantInfo> VariantsController::info(const QString& variantId) const {
  for (auto it = byGame_.begin(); it != byGame_.end(); ++it)
    for (const VariantInfo& v : it.value())
      if (v.id == variantId) return v;
  return std::nullopt;
}

QString VariantsController::gameOf(const QString& variantId) const {
  if (const auto v = info(variantId)) return v->gameId;
  if (const auto r = deps_.cache ? deps_.cache->recipe(variantId) : std::nullopt) return r->gameId;
  return {};
}

QString VariantsController::stateOf(const QString& id) const {
  if (active_ && active_->variantId == id) return QStringLiteral("building");
  for (const Job& j : queue_)
    if (j.variantId == id) return QStringLiteral("building");
  if (states_.contains(id) && !states_.value(id).error.isEmpty()) return QStringLiteral("failed");
  if (deps_.cache && deps_.cache->isBuilt(id)) return QStringLiteral("ready");
  return QStringLiteral("needs_rebuild");
}

QVariantMap VariantsController::toMap(const VariantInfo& v) const {
  QVariantMap m;
  const QString state = stateOf(v.id);
  const VarState st = states_.value(v.id);
  const auto local = deps_.cache ? deps_.cache->recipe(v.id) : std::nullopt;
  m.insert(QStringLiteral("id"), v.id);
  m.insert(QStringLiteral("gameId"), v.gameId);
  m.insert(QStringLiteral("name"), v.name);
  m.insert(QStringLiteral("slot"), v.slot);
  m.insert(QStringLiteral("presetId"), local ? local->presetId : QString());
  m.insert(QStringLiteral("toolVersion"), v.toolVersion);
  m.insert(QStringLiteral("state"), state);
  m.insert(QStringLiteral("queued"), st.queued);
  m.insert(QStringLiteral("fraction"), st.fraction);
  m.insert(QStringLiteral("percent"), static_cast<int>(std::lround(st.fraction * 100.0)));
  m.insert(QStringLiteral("step"), st.step);
  m.insert(QStringLiteral("etaMs"), static_cast<double>(st.etaMs));
  m.insert(QStringLiteral("etaText"), etaTextOf(st.etaMs));
  m.insert(QStringLiteral("error"), st.error);
  m.insert(QStringLiteral("errorText"), st.error.isEmpty() ? QString() : errorText(st.error));
  m.insert(QStringLiteral("neededBytes"), static_cast<double>(st.needed));
  m.insert(QStringLiteral("freeBytes"), static_cast<double>(st.free));
  m.insert(QStringLiteral("neededText"), st.needed > 0 ? formatBytesText(st.needed) : QString());
  m.insert(QStringLiteral("freeText"), st.free > 0 ? formatBytesText(st.free) : QString());
  m.insert(QStringLiteral("expectedSha"), st.expectedSha);
  m.insert(QStringLiteral("actualSha"), st.actualSha);
  m.insert(QStringLiteral("isRun"), !v.runId.isEmpty() || v.id.startsWith(QLatin1String("run-")));
  m.insert(QStringLiteral("runId"), v.runId);
  m.insert(QStringLiteral("hasSpoilerLog"), deps_.cache && !deps_.cache->spoilerLogPath(v.id).isEmpty());
  m.insert(QStringLiteral("spoilerLogRequested"), v.spoilerLog);
  m.insert(QStringLiteral("createdAt"), v.createdAt);
  m.insert(QStringLiteral("deleted"), !v.deletedAt.isEmpty());
  m.insert(QStringLiteral("synced"), true);
  return m;
}

void VariantsController::rebuildLists() {
  QList<VariantInfo> shown;
  for (const VariantInfo& v : byGame_.value(gameId_))
    if (v.deletedAt.isEmpty()) shown.append(v);
  std::sort(shown.begin(), shown.end(), [](const VariantInfo& a, const VariantInfo& b) { return a.createdAt > b.createdAt; });
  QVariantList out;
  for (const VariantInfo& v : shown) out.append(toMap(v));
  if (!loadedOk_.value(gameId_, false) && deps_.cache != nullptr && !gameId_.isEmpty()) {
    // The server cannot be reached: what this PC knows (recipes of earlier builds) still rebuilds.
    const QFileInfoList dirs = QDir(deps_.cache->root()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& d : dirs) {
      const auto r = deps_.cache->recipe(d.fileName());
      if (!r || r->gameId != gameId_ || d.fileName().startsWith(QLatin1String("run-"))) continue;
      bool known = false;
      for (const VariantInfo& v : shown) known = known || v.id == r->variantId;
      if (known) continue;
      VariantInfo v;
      v.id = r->variantId;
      v.gameId = r->gameId;
      v.name = r->name;
      v.slot = r->slot;
      v.toolVersion = r->toolVersion;
      QVariantMap m = toMap(v);
      m.insert(QStringLiteral("synced"), false);
      out.append(m);
    }
  }
  variantList_ = out;
  emit variantsChanged();
  emit cacheChanged();
}

QVariantMap VariantsController::variantMap(const QString& variantId) const {
  if (const auto v = info(variantId)) return toMap(*v);
  VariantInfo v;
  v.id = variantId;
  if (const auto r = deps_.cache ? deps_.cache->recipe(variantId) : std::nullopt) {
    v.gameId = r->gameId;
    v.name = r->name;
    v.slot = r->slot;
    v.toolVersion = r->toolVersion;
    QVariantMap m = toMap(v);
    m.insert(QStringLiteral("synced"), false);
    return m;
  }
  return {};
}

QVariantMap VariantsController::status(const QString& variantId) const { return variantMap(variantId); }

QVariantList VariantsController::variantsOf(const QString& gameId) const {
  QVariantList out;
  for (const VariantInfo& v : byGame_.value(gameId))
    if (v.deletedAt.isEmpty()) out.append(toMap(v));
  return out;
}

void VariantsController::publishPluginState() {
  QHash<QString, QList<plugins::VariantRow>> rows;
  for (auto it = byGame_.begin(); it != byGame_.end(); ++it) {
    for (const VariantInfo& v : it.value()) {
      if (!v.deletedAt.isEmpty()) continue;
      plugins::VariantRow r;
      r.id = v.id;
      r.name = v.name;
      r.slot = v.slot;
      const auto local = deps_.cache ? deps_.cache->recipe(v.id) : std::nullopt;
      r.presetId = local ? local->presetId : QString();
      r.state = stateOf(v.id);
      r.toolVersion = v.toolVersion;
      rows[it.key()].append(r);
    }
  }
  bridge_->setRows(rows);
}

bool VariantsController::pluginRegistered() const {
  return !bridge_->registry()->all().isEmpty();
}

bool VariantsController::supported(const QString& gameId) const {
  const auto game = deps_.library ? deps_.library->gameById(gameId) : std::nullopt;
  if (!game) return false;
  const QString code = game->system == QLatin1String("3ds") && deps_.titleIdOf ? deps_.titleIdOf(*game) : QString();
  return bridge_->registry()->supported(game->system, code.isEmpty() ? game->gameCode : code);
}

QVariantList VariantsController::presets(const QString& gameId) const {
  QVariantList out;
  const auto game = deps_.library ? deps_.library->gameById(gameId) : std::nullopt;
  if (!game || !supported(gameId)) return out;
  const QString code = game->system == QLatin1String("3ds") && deps_.titleIdOf ? deps_.titleIdOf(*game) : QString();
  for (const plugins::VariantPreset& p : bridge_->registry()->presetsFor(game->system, code.isEmpty() ? game->gameCode : code))
    out.append(QVariantMap{{QStringLiteral("id"), p.id}, {QStringLiteral("label"), p.label}, {QStringLiteral("hint"), p.hint}, {QStringLiteral("custom"), false}});
  out.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("custom")}, {QStringLiteral("label"), tr("Custom settings")}, {QStringLiteral("hint"), QString()}, {QStringLiteral("custom"), true}});
  return out;
}


// ---------------------------------------------------------------- what the screens ask

QString VariantsController::randomizeState(const QString& gameId) const {
  const auto game = deps_.library ? deps_.library->gameById(gameId) : std::nullopt;
  if (!game) return QStringLiteral("none");
  if (supported(gameId)) return QStringLiteral("ok");
  for (const plugins::VariantRegistration& reg : bridge_->registry()->all())
    for (const plugins::VariantGameSupport& g : reg.games)
      if (g.system == game->system) return QStringLiteral("unsupported");
  return QStringLiteral("none");
}

bool VariantsController::pluginAvailable() const { return !bridge_->registry()->all().isEmpty(); }

QVariantMap VariantsController::gameFile(const QString& gameId) const {
  const auto game = deps_.library ? deps_.library->gameById(gameId) : std::nullopt;
  if (!game) return {{QStringLiteral("onPc"), false}, {QStringLiteral("sizeText"), QString()}};
  const RomSourceChoice src = deps_.romSource ? deps_.romSource(*game) : RomSourceChoice{};
  return {{QStringLiteral("onPc"), src.source == RomSource::Local || src.source == RomSource::Cache},
          {QStringLiteral("sizeText"), game->romSize > 0 ? formatBytesText(game->romSize) : QString()}};
}

QString VariantsController::defaultName(const QString& gameId) const {
  int highest = 0;
  int count = 0;
  static const QRegularExpression rx(QStringLiteral("^Randomizer #(\\d+)$"));
  for (const VariantInfo& v : byGame_.value(gameId)) {
    if (!v.deletedAt.isEmpty() || !v.runId.isEmpty()) continue;
    ++count;
    const auto m = rx.match(v.name);
    if (m.hasMatch()) highest = std::max(highest, m.captured(1).toInt());
  }
  int n = std::max(highest, count) + 1;
  // Also skip names of local recipes (a variant deleted with "Keep saves" keeps its recipe and name).
  QSet<QString> local;
  if (deps_.cache)
    for (const QFileInfo& d : QDir(deps_.cache->root()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
      if (const auto r = deps_.cache->recipe(d.fileName()); r && r->gameId == gameId) local.insert(r->name.trimmed().toLower());
  while (nameTaken(gameId, QStringLiteral("Randomizer #%1").arg(n)) || local.contains(QStringLiteral("randomizer #%1").arg(n))) ++n;
  return QStringLiteral("Randomizer #%1").arg(n);
}

bool VariantsController::nameTaken(const QString& gameId, const QString& name, const QString& exceptVariantId) const {
  const QString wanted = name.trimmed();
  if (wanted.isEmpty()) return false;
  for (const VariantInfo& v : byGame_.value(gameId))
    if (v.id != exceptVariantId && v.deletedAt.isEmpty() && v.name.compare(wanted, Qt::CaseInsensitive) == 0) return true;
  return false;
}

bool VariantsController::smallGame(const QString& gameId) const {
  const auto game = deps_.library ? deps_.library->gameById(gameId) : std::nullopt;
  if (!game) return false;
  return game->system == QLatin1String("gb") || game->system == QLatin1String("gbc") || game->system == QLatin1String("gba");
}

QVariantMap VariantsController::checkSettings(const QString& gameId, const QString& text) const {
  const QString t = text.trimmed();
  QVariantMap out{{QStringLiteral("state"), QStringLiteral("empty")}, {QStringLiteral("valid"), false}, {QStringLiteral("prefix"), QString()}, {QStringLiteral("text"), t}};
  if (t.isEmpty()) return out;
  static const QRegularExpression rx(QStringLiteral("^(\\d{3})[A-Za-z0-9+/=]+$"));
  const auto m = rx.match(t);
  if (!m.hasMatch() || t.size() > 4096) {
    out[QStringLiteral("state")] = QStringLiteral("invalid");
    return out;
  }
  const QString prefix = m.captured(1);
  out[QStringLiteral("prefix")] = prefix;
  const auto game = deps_.library ? deps_.library->gameById(gameId) : std::nullopt;
  int newest = -1;
  bool known = false;
  if (game) {
    const QString code = game->system == QLatin1String("3ds") && deps_.titleIdOf ? deps_.titleIdOf(*game) : QString();
    for (const plugins::VariantPreset& p : bridge_->registry()->presetsFor(game->system, code.isEmpty() ? game->gameCode : code))
      for (auto it = p.settings.begin(); it != p.settings.end(); ++it) {
        newest = std::max(newest, it.key().toInt());
        known = known || it.key() == prefix;
      }
  }
  // A string the engine knows (a preset has one for the same settings version) matches it; an older version is upgraded by the
  // engine itself; a newer one cannot be read by the engine this server offers.
  const bool newer = !known && newest >= 0 && prefix.toInt() > newest;
  out[QStringLiteral("state")] = newer ? QStringLiteral("other_version") : QStringLiteral("ok");
  out[QStringLiteral("valid")] = !newer;
  return out;
}

QString VariantsController::slotDisplayName(const QString& slot) const {
  if (slot.isEmpty() || slot == QLatin1String("default")) return {};
  for (auto it = byGame_.begin(); it != byGame_.end(); ++it)
    for (const VariantInfo& v : it.value())
      if (v.slot == slot) return v.name;
  if (deps_.cache) {
    const QFileInfoList dirs = QDir(deps_.cache->root()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& d : dirs) {
      const auto r = deps_.cache->recipe(d.fileName());
      if (r && r->slot == slot && !d.fileName().startsWith(QLatin1String("run-"))) return r->name;
    }
  }
  return {};
}

void VariantsController::reloadAll() {
  if (deps_.client == nullptr || !deps_.client->available()) return;
  deps_.client->list(QString(), [this](const QList<VariantInfo>& list, const VariantCall& c) {
    if (!c.ok) return;
    QHash<QString, QList<VariantInfo>> grouped;
    for (const VariantInfo& v : list) grouped[v.gameId].append(v);
    if (!gameId_.isEmpty() && !grouped.contains(gameId_)) grouped.insert(gameId_, {});
    byGame_ = grouped;
    for (auto it = grouped.begin(); it != grouped.end(); ++it) loadedOk_[it.key()] = true;
    rebuildLists();
    publishPluginState();
  });
}

QVariantMap VariantsController::summary() const {
  QVariantMap out;
  QHash<QString, qint64> lastPlayed;
  if (deps_.cache)
    for (const auto& e : deps_.cache->entries()) lastPlayed.insert(e.id, e.lastPlayedMs);
  for (auto it = byGame_.begin(); it != byGame_.end(); ++it) {
    int count = 0;
    QString lastId, lastName;
    qint64 lastMs = 0;
    for (const VariantInfo& v : it.value()) {
      if (!v.deletedAt.isEmpty() || !v.runId.isEmpty() || v.id.startsWith(QLatin1String("run-"))) continue;
      ++count;
      const qint64 ms = lastPlayed.value(v.id, 0);
      if (ms > lastMs) {
        lastMs = ms;
        lastId = v.id;
        lastName = v.name;
      }
    }
    const bool building = active_ && active_->gameId == it.key() && active_->runId.isEmpty();
    if (count == 0 && !building) continue;
    QVariantMap m{{QStringLiteral("count"), count}, {QStringLiteral("buildPercent"), building ? current_.value(QStringLiteral("percent"), 0).toInt() : -1},
                  {QStringLiteral("buildName"), building ? active_->recipe.name : QString()}, {QStringLiteral("lastId"), lastId},
                  {QStringLiteral("lastName"), lastName}, {QStringLiteral("lastPlayedMs"), static_cast<double>(lastMs)}};
    out.insert(it.key(), m);
  }
  return out;
}

QVariantList VariantsController::storage() const {
  QVariantList out;
  if (!deps_.cache) return out;
  QHash<QString, VariantCache::Entry> built;
  for (const auto& e : deps_.cache->entries()) built.insert(e.id, e);
  struct Row {
    QVariantMap m;
    qint64 played = 0;
  };
  QList<Row> rows;
  QSet<QString> activeIds;  // the server lists only variants that are not deleted
  for (const QList<VariantInfo>& list : byGame_)
    for (const VariantInfo& v : list)
      if (v.deletedAt.isEmpty()) activeIds.insert(v.id);
  const QDate today = QDate::currentDate();
  const QFileInfoList dirs = QDir(deps_.cache->root()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QFileInfo& d : dirs) {
    const auto r = deps_.cache->recipe(d.fileName());
    if (!r) continue;
    const bool run = d.fileName().startsWith(QLatin1String("run-"));
    const bool removed = !built.contains(r->variantId);
    // A variant deleted with "Keep saves" keeps its recipe only on the server: no files here, so it is not listed.
    // Known only once the server list of that game was read; before that the row stays.
    if (removed && !run && loadedOk_.value(r->gameId) && !activeIds.contains(r->variantId)) continue;
    const auto e = built.value(r->variantId);
    const auto game = deps_.library ? deps_.library->gameById(r->gameId) : std::nullopt;
    QString played;
    if (removed || !e.played || e.lastPlayedMs <= 0) {
      played = QString();
    } else {
      const QDate day = QDateTime::fromMSecsSinceEpoch(e.lastPlayedMs).date();
      if (day == today) played = tr("Played today");
      else if (day == today.addDays(-1)) played = tr("Played yesterday");
      else played = tr("Played %1 %2").arg(day.day()).arg(QLocale::c().monthName(day.month(), QLocale::ShortFormat));
    }
    if (played.isEmpty() && !removed) played = tr("Not played yet");
    const QString title = game ? game->title : QString();
    QVariantMap m{{QStringLiteral("id"), r->variantId}, {QStringLiteral("name"), r->name},
                  {QStringLiteral("subtitle"), run ? (title.isEmpty() ? tr("Run variant") : tr("%1 · run variant").arg(title)) : title},
                  {QStringLiteral("isRun"), run}, {QStringLiteral("removed"), removed},
                  {QStringLiteral("lastPlayedMs"), static_cast<double>(e.lastPlayedMs)}, {QStringLiteral("lastPlayedText"), played},
                  {QStringLiteral("bytes"), static_cast<double>(e.bytes)}, {QStringLiteral("sizeText"), removed ? QString() : formatBytesText(e.bytes)}};
    rows.append({m, removed ? -1 : e.lastPlayedMs});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.played > b.played; });
  for (const Row& r : rows) out.append(r.m);
  return out;
}

QString VariantsController::presetSettings(const QString& presetId) const {
  const plugins::VariantPreset p = bridge_->registry()->preset(presetId);
  return p.id.isEmpty() ? QString() : p.bestSettings();
}

QString VariantsController::presetLabel(const QString& presetId) const {
  const plugins::VariantPreset p = bridge_->registry()->preset(presetId);
  return p.id.isEmpty() || p.label.isEmpty() ? presetId : p.label;
}

// ---------------------------------------------------------------- building

QString VariantsController::seedOrRandom(const QString& seed, bool use) const {
  if (use && allDigits(seed)) return seed;
  return QString::number(QRandomGenerator::securelySeeded().generate64() >> 2);  // 62 bits: positive, exact in the recipe
}

QVariantMap VariantsController::build(const QString& gameId, const QString& name, const QString& presetId, const QString& settings, const QString& seed,
                                      bool useSeed, bool spoilerLog) {
  BuildArgs a;
  a.gameId = gameId;
  a.name = name;
  a.presetId = presetId;
  a.settings = settings;
  a.seed = seed;
  a.useSeed = useSeed;
  a.spoilerLog = spoilerLog;
  a.requestId = bridge_->nextRequestId();
  return startBuild(a);
}

void VariantsController::buildForPlugin(const QString& pluginId, const plugins::VariantBuildRequest& req, quint64 requestId) {
  BuildArgs a;
  a.gameId = req.gameId;
  a.name = req.name;
  a.presetId = req.presetId;
  a.settings = req.settings;
  a.seed = req.seed;
  a.useSeed = !req.seed.isEmpty();
  a.spoilerLog = req.spoilerLog;
  a.requestId = requestId;
  a.pluginId = pluginId;
  const QVariantMap r = startBuild(a);
  if (!r.value(QStringLiteral("ok")).toBool()) {
    Job j;
    j.requestId = requestId;
    j.pluginId = pluginId;
    postPlugin(j, QStringLiteral("failed"), 0, r.value(QStringLiteral("error")).toString());
  }
}

QVariantMap VariantsController::startBuild(const BuildArgs& a) {
  const auto bad = [](const QString& code) { return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("error"), code}}; };
  const auto game = deps_.library ? deps_.library->gameById(a.gameId) : std::nullopt;
  if (!game || !supported(a.gameId)) return bad(QStringLiteral("unsupported"));
  const QString name = a.name.trimmed();
  if (name.isEmpty() || name.size() > 60) return bad(QStringLiteral("invalid_name"));
  if (deps_.client == nullptr || !deps_.client->available()) return bad(QStringLiteral("server_unreachable"));
  QString settings = a.settings;
  QString presetId = a.presetId;
  if (!presetId.isEmpty() && presetId != QLatin1String("custom")) {
    const plugins::VariantPreset p = bridge_->registry()->preset(presetId);
    if (p.id.isEmpty()) return bad(QStringLiteral("unknown_preset"));
    settings = p.bestSettings();
  }
  if (settings.isEmpty() || settings.size() > 4096) return bad(QStringLiteral("invalid_settings"));
  if (a.useSeed && !allDigits(a.seed)) return bad(QStringLiteral("invalid_seed"));
  QString version = deps_.tools->offeredVersion(QStringLiteral("randomizer"));
  if (version.isEmpty()) {
    const QStringList have = deps_.toolCache->versions(QStringLiteral("randomizer"), platformKey());
    if (have.isEmpty()) return bad(QStringLiteral("engine_not_offered"));
    version = have.first();
  }
  NewVariant nv;
  nv.name = name;
  nv.toolVersion = version;
  nv.settings = settings;
  nv.seed = seedOrRandom(a.seed, a.useSeed);
  nv.spoilerLog = a.spoilerLog;
  const bool mod = game->system == QLatin1String("3ds");
  if (mod) {
    nv.titleId = deps_.titleIdOf ? deps_.titleIdOf(*game) : QString();
    if (nv.titleId.isEmpty() && game->gameCodeKind == QLatin1String("3ds-titleid")) nv.titleId = game->gameCode;
  } else {
    nv.baseSha256 = game->romSha256;
  }
  const quint64 req = a.requestId;
  const QString pluginId = a.pluginId;
  const QString gameId = a.gameId;
  deps_.client->create(gameId, nv, [this, req, pluginId, gameId, presetId, nv, game = *game, mod](const VariantInfo& v, const VariantCall& c) {
    if (!c.ok || !v.valid()) {
      emit createFailed(req, c.error);
      Job j;
      j.requestId = req;
      j.pluginId = pluginId;
      postPlugin(j, QStringLiteral("failed"), 0, c.error);
      return;
    }
    VariantInfo stored = v;
    if (stored.gameId.isEmpty()) stored.gameId = gameId;
    if (stored.seed.isEmpty()) stored.seed = nv.seed;
    if (stored.settings.isEmpty()) stored.settings = nv.settings;
    if (stored.toolVersion.isEmpty()) stored.toolVersion = nv.toolVersion;
    QList<VariantInfo> list = byGame_.value(gameId);
    list.prepend(stored);
    byGame_.insert(gameId, list);
    Job j;
    j.requestId = req;
    j.pluginId = pluginId;
    j.variantId = stored.id;
    j.gameId = gameId;
    j.recipe = recipeFor(stored, game);
    j.recipe.presetId = presetId;
    emit variantCreated(req, stored.id);
    enqueue(j);
  });
  return {{QStringLiteral("ok"), true}, {QStringLiteral("requestId"), static_cast<double>(req)}};
}

VariantRecipe VariantsController::recipeFor(const VariantInfo& v, const GameEntry& game) const {
  VariantRecipe r;
  r.variantId = v.id;
  r.gameId = v.gameId.isEmpty() ? game.id : v.gameId;
  r.name = v.name;
  r.slot = v.slot;
  r.system = game.system;
  r.toolId = v.toolId.isEmpty() ? QStringLiteral("randomizer") : v.toolId;
  r.toolVersion = v.toolVersion;
  r.settings = v.settings;
  r.seed = v.seed;
  r.spoilerLog = v.spoilerLog;
  r.updateVersion = v.updateVersion;
  if (r.isMod()) {
    r.titleId = v.titleId;
    if (r.titleId.isEmpty() && deps_.titleIdOf) r.titleId = deps_.titleIdOf(game);
    if (r.titleId.isEmpty() && game.gameCodeKind == QLatin1String("3ds-titleid")) r.titleId = game.gameCode;
  } else {
    r.baseSha256 = v.baseSha256.isEmpty() ? game.romSha256 : v.baseSha256;
    r.romFileName = QFileInfo(game.romFilename).fileName();
  }
  return r;
}

void VariantsController::enqueue(Job job) {
  VarState st;
  st.queued = true;
  states_.insert(job.variantId, st);
  queue_.append(std::move(job));
  rebuildLists();
  publishPluginState();
  emit progressChanged();
  pump();
}

void VariantsController::pump() {
  if (active_ || queue_.isEmpty()) return;
  startJob(queue_.takeFirst());
}

void VariantsController::startJob(Job job) {
  active_ = std::move(job);
  activeWaitingEngine_ = activeWaitingBase_ = false;
  activeBaseSha_.clear();
  activeClock_.start();
  states_.insert(active_->variantId, VarState());
  current_ = QVariantMap{{QStringLiteral("variantId"), active_->variantId}, {QStringLiteral("name"), active_->recipe.name},
                         {QStringLiteral("gameId"), active_->gameId}, {QStringLiteral("slot"), active_->recipe.slot},
                         {QStringLiteral("step"), QStringLiteral("reading")}, {QStringLiteral("fraction"), 0.0}, {QStringLiteral("percent"), 0},
                         {QStringLiteral("etaMs"), 0.0}, {QStringLiteral("etaText"), QString()}, {QStringLiteral("elapsedMs"), 0.0},
                         {QStringLiteral("small"), !active_->recipe.isMod() && false}};
  if (!active_->runId.isEmpty()) {
    runStates_.insert(active_->runId, QStringLiteral("building"));
    reportRunState(active_->runId, QStringLiteral("building"));
    emit runVariantChanged(active_->runId);
  }
  rebuildLists();
  emit progressChanged();
  postPlugin(*active_, QStringLiteral("building"), 0);
  qCInfo(lcVariants) << "job" << active_->variantId << "engine" << active_->recipe.toolVersion;
  continueJob();
}

void VariantsController::continueJob() {
  if (!active_) return;
  const auto game = deps_.library ? deps_.library->gameById(active_->gameId) : std::nullopt;
  if (!game) return failActive(QStringLiteral("base_missing"));
  const QString tool = active_->recipe.toolId, version = active_->recipe.toolVersion;
  if (!deps_.toolCache->isReady(tool, version, platformKey())) {
    if (deps_.tools == nullptr || !deps_.tools->available()) return failActive(QStringLiteral("engine_missing"));
    activeWaitingEngine_ = true;
    if (!deps_.tools->busy()) {
      engineBusy_ = true;
      engineError_.clear();
      refreshEngine();
      deps_.tools->prepare(tool, version);
    }
    return;
  }
  prepareBase(*game);
}

void VariantsController::prepareBase(const GameEntry& game) {
  const RomSourceChoice src = deps_.romSource ? deps_.romSource(game) : RomSourceChoice{};
  if (src.source == RomSource::Local) return startBuilder(src.localPath);
  if (src.source == RomSource::Missing || deps_.downloader == nullptr) return failActive(QStringLiteral("base_missing"));
  activeWaitingBase_ = true;
  activeBaseSha_ = game.romSha256;
  deps_.downloader->ensureRom(game);  // a cache hit answers with romReady at once, otherwise it downloads first
}

void VariantsController::onRomReady(const QString& sha, const QString& path) {
  if (!active_ || !activeWaitingBase_ || sha != activeBaseSha_) return;
  activeWaitingBase_ = false;
  startBuilder(path);
}

void VariantsController::startBuilder(const QString& basePath) {
  if (!active_) return;
  builder_.start(active_->recipe, basePath, active_->skipBaseCheck);
}

void VariantsController::onProgress(const BuildProgress& p) {
  if (!active_) return;
  VarState& st = states_[active_->variantId];
  st.fraction = p.fraction;
  st.step = p.step;
  st.elapsedMs = p.elapsedMs;
  st.etaMs = p.etaMs;
  current_.insert(QStringLiteral("step"), p.step);
  current_.insert(QStringLiteral("fraction"), p.fraction);
  current_.insert(QStringLiteral("percent"), static_cast<int>(std::lround(p.fraction * 100.0)));
  current_.insert(QStringLiteral("etaMs"), static_cast<double>(p.etaMs));
  current_.insert(QStringLiteral("etaText"), etaTextOf(p.etaMs));
  current_.insert(QStringLiteral("elapsedMs"), static_cast<double>(p.elapsedMs));
  // A build of a small game takes seconds: the UI shows no dialog for it.
  current_.insert(QStringLiteral("small"), !active_->recipe.isMod() && p.etaMs > 0 && p.etaMs + p.elapsedMs < 10000);
  const int pct = static_cast<int>(std::lround(p.fraction * 100.0));
  if (pct != lastPosted_) {
    lastPosted_ = pct;
    postPlugin(*active_, QStringLiteral("building"), p.fraction);
  }
  emit progressChanged();
  for (int i = 0; i < variantList_.size(); ++i) {
    QVariantMap m = variantList_.at(i).toMap();
    if (m.value(QStringLiteral("id")).toString() == active_->variantId) {
      m.insert(QStringLiteral("fraction"), p.fraction);
      m.insert(QStringLiteral("percent"), pct);
      m.insert(QStringLiteral("step"), p.step);
      m.insert(QStringLiteral("etaMs"), static_cast<double>(p.etaMs));
      m.insert(QStringLiteral("etaText"), etaTextOf(p.etaMs));
      variantList_[i] = m;
    }
  }
  emit variantsChanged();
}

void VariantsController::failActive(const QString& error, const QString& detail) {
  BuildOutcome o;
  o.error = error;
  if (!detail.isEmpty()) o.log << detail;
  if (builder_.running()) {
    builder_.cancel();
    return;
  }
  onFinished(o);
}

void VariantsController::onFinished(const BuildOutcome& o) {
  if (!active_) return;
  const Job job = *active_;
  active_.reset();
  activeWaitingEngine_ = activeWaitingBase_ = false;
  lastPosted_ = -1;
  current_.clear();
  if (o.ok) {
    states_.remove(job.variantId);
    qCInfo(lcVariants) << "built" << job.variantId;
    if (deps_.settings != nullptr) deps_.cache->trimToLimit(deps_.settings->variantCacheLimitBytes(), QSet<QString>{job.variantId} | (deps_.protectedVariantIds ? deps_.protectedVariantIds() : QSet<QString>()));
    emit cacheChanged();
    postPlugin(job, QStringLiteral("ready"), 1.0);
    emit buildFinished(job.variantId, true, QString());
    emit toast(tr("%1 is ready").arg(job.recipe.name), job.variantId);
    if (!job.runId.isEmpty()) {
      runStates_.insert(job.runId, QStringLiteral("ready"));
      runErrors_.remove(job.runId);
      reportRunState(job.runId, QStringLiteral("ready"));
      emit runVariantChanged(job.runId);
      emit runVariantReady(job.runId);
    }
    rebuildLists();
    publishPluginState();
    emit progressChanged();
    if (job.playWhenReady && job.runId.isEmpty()) emit launchRequested(job.variantId);
  } else if (o.error == QLatin1String("cancelled")) {
    states_.remove(job.variantId);
    postPlugin(job, QStringLiteral("cancelled"), 0);
    emit buildFinished(job.variantId, false, o.error);
    if (!job.runId.isEmpty()) {
      runStates_.insert(job.runId, QStringLiteral("not_built"));
      reportRunState(job.runId, QStringLiteral("not_built"));
      emit runVariantChanged(job.runId);
    }
    rebuildLists();
    publishPluginState();
    emit progressChanged();
  } else {
    VarState st;
    st.error = o.error.isEmpty() ? QStringLiteral("engine_failed") : o.error;
    st.log = o.log;
    st.needed = o.neededBytes;
    st.free = o.freeBytes;
    st.expectedSha = o.expectedBaseSha;
    st.actualSha = o.actualBaseSha;
    if (!o.layoutNote.isEmpty()) st.log << o.layoutNote;
    st.log.prepend(QStringLiteral("exit code %1").arg(o.exitCode));
    states_.insert(job.variantId, st);
    qCWarning(lcVariants) << "build failed" << job.variantId << st.error;
    postPlugin(job, QStringLiteral("failed"), 0, st.error);
    emit buildFinished(job.variantId, false, st.error);
    if (!job.runId.isEmpty()) {
      runStates_.insert(job.runId, QStringLiteral("failed"));
      runErrors_.insert(job.runId, st.error);
      reportRunState(job.runId, QStringLiteral("not_built"));
      emit runVariantChanged(job.runId);
      emit runVariantFailed(job.runId, st.error);
    }
    rebuildLists();
    publishPluginState();
    emit progressChanged();
  }
  QTimer::singleShot(0, this, &VariantsController::pump);
}

void VariantsController::postPlugin(const Job& j, const QString& state, double progress, const QString& error) {
  if (j.requestId == 0 || j.pluginId.isEmpty() || !deps_.postPluginEvent) return;
  QVariantMap v{{QStringLiteral("id"), static_cast<qlonglong>(j.requestId)}, {QStringLiteral("state"), state}, {QStringLiteral("progress"), progress}};
  if (!error.isEmpty()) v.insert(QStringLiteral("error"), error);
  deps_.postPluginEvent(j.pluginId, QStringLiteral("variants.state"), v);
}

void VariantsController::cancel(const QString& variantId) {
  if (active_ && active_->variantId == variantId) {
    if (builder_.running()) {
      builder_.cancel();
    } else if (activeWaitingEngine_) {
      if (deps_.tools != nullptr && deps_.tools->busy()) deps_.tools->cancel();
      else failActive(QStringLiteral("cancelled"));
    } else {
      if (activeWaitingBase_ && deps_.downloader != nullptr) deps_.downloader->cancel(activeBaseSha_);
      failActive(QStringLiteral("cancelled"));
    }
    return;
  }
  for (int i = 0; i < queue_.size(); ++i) {
    if (queue_.at(i).variantId != variantId) continue;
    const Job j = queue_.takeAt(i);
    states_.remove(variantId);
    postPlugin(j, QStringLiteral("cancelled"), 0);
    emit buildFinished(variantId, false, QStringLiteral("cancelled"));
    rebuildLists();
    publishPluginState();
    emit progressChanged();
    return;
  }
}

void VariantsController::rebuild(const QString& variantId, bool buildAnyway) {
  if (stateOf(variantId) == QLatin1String("building")) return;
  VariantRecipe r;
  const auto v = info(variantId);
  const auto local = deps_.cache ? deps_.cache->recipe(variantId) : std::nullopt;
  const QString gid = v ? v->gameId : (local ? local->gameId : QString());
  const auto game = deps_.library ? deps_.library->gameById(gid) : std::nullopt;
  if (!game) {
    emit actionFailed(QStringLiteral("rebuild"), variantId, QStringLiteral("base_missing"));
    return;
  }
  if (local) r = *local;
  if (v && !v->settings.isEmpty()) {
    const QString preset = r.presetId;
    r = recipeFor(*v, *game);
    r.presetId = preset;
  }
  if (r.settings.isEmpty() || r.seed.isEmpty()) {
    emit actionFailed(QStringLiteral("rebuild"), variantId, QStringLiteral("unsupported"));  // somebody else's variant: no recipe here
    return;
  }
  Job j;
  j.variantId = variantId;
  j.gameId = gid;
  j.recipe = r;
  j.skipBaseCheck = buildAnyway;
  states_.remove(variantId);
  enqueue(j);
}

void VariantsController::play(const QString& variantId) {
  if (deps_.cache && deps_.cache->isBuilt(variantId) && stateOf(variantId) != QLatin1String("building")) {
    emit launchRequested(variantId);
    return;
  }
  if (stateOf(variantId) == QLatin1String("building")) {
    if (active_ && active_->variantId == variantId) active_->playWhenReady = true;
    for (Job& j : queue_)
      if (j.variantId == variantId) j.playWhenReady = true;
    return;
  }
  rebuild(variantId);
  if (stateOf(variantId) == QLatin1String("building")) {
    if (active_ && active_->variantId == variantId) active_->playWhenReady = true;
    for (Job& j : queue_)
      if (j.variantId == variantId) j.playWhenReady = true;
    emit needsBuild(variantId);
  }
}

void VariantsController::rename(const QString& variantId, const QString& name) {
  const QString n = name.trimmed();
  if (n.isEmpty() || n.size() > 60 || deps_.client == nullptr) {
    emit actionFailed(QStringLiteral("rename"), variantId, QStringLiteral("invalid_name"));
    return;
  }
  deps_.client->rename(variantId, n, [this, variantId, n](const VariantInfo&, const VariantCall& c) {
    if (!c.ok) {
      emit actionFailed(QStringLiteral("rename"), variantId, c.error);
      return;
    }
    for (auto it = byGame_.begin(); it != byGame_.end(); ++it)
      for (VariantInfo& v : it.value())
        if (v.id == variantId) v.name = n;
    if (auto r = deps_.cache->recipe(variantId)) {
      r->name = n;
      deps_.cache->writeRecipe(*r);
    }
    rebuildLists();
    publishPluginState();
  });
}

void VariantsController::remove(const QString& variantId, bool keepSaves) {
  cancel(variantId);
  if (deps_.client == nullptr) return;
  deps_.client->remove(variantId, keepSaves, [this, variantId, keepSaves](const VariantCall& c) {
    if (!c.ok) {
      emit actionFailed(QStringLiteral("remove"), variantId, c.error);
      return;
    }
    // The built files always go; the recipe stays locally only with the saves (the server keeps it too: "Build it again").
    deps_.cache->remove(variantId, keepSaves);
    states_.remove(variantId);
    for (auto it = byGame_.begin(); it != byGame_.end(); ++it) {
      QList<VariantInfo>& list = it.value();
      for (int i = 0; i < list.size(); ++i) {
        if (list.at(i).id != variantId) continue;
        if (keepSaves) list[i].deletedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        else list.removeAt(i--);
      }
    }
    rebuildLists();
    publishPluginState();
    emit cacheChanged();
  });
}

QVariantMap VariantsController::spoilerLog(const QString& variantId, bool confirmed) const {
  const auto bad = [](const QString& code) { return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("error"), code}}; };
  if (!confirmed) return bad(QStringLiteral("confirm_required"));
  QString runId;
  if (const auto v = info(variantId); v && !v->runId.isEmpty()) runId = v->runId;
  if (runId.isEmpty() && variantId.startsWith(QLatin1String("run-"))) runId = variantId.mid(4);
  if (!runId.isEmpty()) {
    // A run variant's log opens only after the run ended (finished or abandoned) and is always the own one.
    const QString status = deps_.runStatus ? deps_.runStatus(runId) : QString();
    if (status != QLatin1String("finished") && status != QLatin1String("abandoned")) return bad(QStringLiteral("run_not_over"));
  }
  const QString path = deps_.cache ? deps_.cache->spoilerLogPath(variantId) : QString();
  if (path.isEmpty()) return bad(QStringLiteral("no_log"));
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return bad(QStringLiteral("no_log"));
  const QByteArray b = f.read(2 * 1024 * 1024);
  return {{QStringLiteral("ok"), true}, {QStringLiteral("text"), QString::fromUtf8(b)}};
}

QVariantMap VariantsController::ownSeed(const QString& variantId) const {
  const auto v = info(variantId);
  if (!v || !v->runId.isEmpty() || variantId.startsWith(QLatin1String("run-")) || v->seed.isEmpty())
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("not_available")}};
  return {{QStringLiteral("ok"), true}, {QStringLiteral("seed"), v->seed}, {QStringLiteral("settings"), v->settings}};
}

QString VariantsController::failureDetails(const QString& variantId) const {
  const VarState st = states_.value(variantId);
  if (st.error.isEmpty()) return {};
  QStringList lines;
  lines << QStringLiteral("variant ") + variantId << QStringLiteral("error ") + st.error;
  lines += st.log;
  lines << QStringLiteral("Details hold no spoilers: no creature, item or trainer names.");
  return lines.join(QLatin1Char('\n'));
}

void VariantsController::dismissFailure(const QString& variantId) {
  if (states_.remove(variantId) > 0) {
    rebuildLists();
    publishPluginState();
  }
}

// ---------------------------------------------------------------- storage

QVariantMap VariantsController::cache() const {
  const qint64 bytes = deps_.cache ? deps_.cache->totalBytes() : 0;
  const qint64 limit = deps_.settings ? deps_.settings->variantCacheLimitBytes() : PlayerSettings::kDefaultVariantCacheLimitBytes;
  return {{QStringLiteral("bytes"), static_cast<double>(bytes)}, {QStringLiteral("text"), formatBytesText(bytes)},
          {QStringLiteral("limitBytes"), static_cast<double>(limit)}, {QStringLiteral("limitText"), limit > 0 ? formatBytesText(limit) : tr("No limit")},
          {QStringLiteral("unplayedDays"), deps_.settings ? deps_.settings->variantUnplayedDays() : 0}};
}

void VariantsController::setCacheLimit(double bytes) {
  if (deps_.settings && deps_.settings->setVariantCacheLimitBytes(static_cast<qint64>(bytes))) {
    trimCache();
    emit cacheChanged();
  }
}

void VariantsController::setUnplayedDays(int days) {
  if (deps_.settings && deps_.settings->setVariantUnplayedDays(days)) {
    trimCache();
    emit cacheChanged();
  }
}

QVariantMap VariantsController::freeCache() {
  if (!deps_.cache) return {};
  QSet<QString> prot = deps_.protectedVariantIds ? deps_.protectedVariantIds() : QSet<QString>();
  if (active_) prot.insert(active_->variantId);
  for (const Job& j : queue_) prot.insert(j.variantId);
  qint64 freed = 0;
  int removed = 0;
  for (const auto& e : deps_.cache->entries()) {
    if (prot.contains(e.id)) continue;
    if (deps_.cache->remove(e.id, /*keepRecipe=*/true)) {
      freed += e.bytes;
      ++removed;
    }
  }
  if (removed > 0) {
    rebuildLists();
    publishPluginState();
  }
  return {{QStringLiteral("freedBytes"), static_cast<double>(freed)}, {QStringLiteral("freedText"), formatBytesText(freed)}, {QStringLiteral("removed"), removed}};
}

QVariantMap VariantsController::trimCache() {
  if (!deps_.cache || !deps_.settings) return {};
  QSet<QString> prot = deps_.protectedVariantIds ? deps_.protectedVariantIds() : QSet<QString>();
  if (active_) prot.insert(active_->variantId);
  for (const Job& j : queue_) prot.insert(j.variantId);
  const auto a = deps_.cache->trimToLimit(deps_.settings->variantCacheLimitBytes(), prot);
  const auto b = deps_.cache->removeUnplayed(deps_.settings->variantUnplayedDays(), prot);
  if (a.removed + b.removed > 0) {
    rebuildLists();
    publishPluginState();
    emit cacheChanged();
  }
  return {{QStringLiteral("freedBytes"), static_cast<double>(a.freedBytes + b.freedBytes)}, {QStringLiteral("removed"), a.removed + b.removed}};
}

// ---------------------------------------------------------------- launch

VariantLaunch VariantsController::launchInfo(const QString& variantId) const {
  VariantLaunch l;
  l.variantId = variantId;
  const auto r = deps_.cache ? deps_.cache->recipe(variantId) : std::nullopt;
  if (!r || !deps_.cache->isBuilt(variantId)) {
    l.error = QStringLiteral("not_built");
    return l;
  }
  l.active = true;
  l.gameId = r->gameId;
  l.slot = r->slot;
  l.isMod = r->isMod();
  l.titleId = r->titleId;
  if (l.isMod) l.modDir = deps_.cache->modDir(variantId);
  else l.romPath = deps_.cache->romPath(variantId);
  l.marker = variantId + QLatin1Char('@') + QString::fromLatin1(QCryptographicHash::hash((r->toolVersion + QLatin1Char('|') + r->settings + QLatin1Char('|') + r->seed).toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
  if ((l.isMod && l.modDir.isEmpty()) || (!l.isMod && l.romPath.isEmpty())) l.error = QStringLiteral("not_built");
  return l;
}

// ---------------------------------------------------------------- runs

bool VariantsController::runUsesVariant(const QString& runId) const {
  const QString mode = deps_.runRandomizerMode ? deps_.runRandomizerMode(runId) : QString();
  return !mode.isEmpty() && mode != QLatin1String("off");
}

QString VariantsController::runVariantId(const QString& runId) const {
  if (!runUsesVariant(runId)) return {};
  const QString kind = deps_.runVariantKind ? deps_.runVariantKind(runId) : QString();
  if (kind == QLatin1String("own")) return runChoices_.value(runId.toLower());
  return runVariantKey(runId);
}

bool VariantsController::runVariantBuilt(const QString& runId) const {
  const QString id = runVariantId(runId);
  return !id.isEmpty() && deps_.cache && deps_.cache->isBuilt(id);
}

void VariantsController::setRunOwnVariant(const QString& runId, const QString& variantId) {
  runChoices_.insert(runId.toLower(), variantId);
  saveRunChoices();
}

void VariantsController::loadRunChoices() {
  if (!deps_.cache) return;
  QFile f(deps_.cache->root() + QStringLiteral("/.run-choices.json"));
  if (!f.open(QIODevice::ReadOnly)) return;
  const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
  for (auto it = o.begin(); it != o.end(); ++it) runChoices_.insert(it.key(), it.value().toString());
}

void VariantsController::saveRunChoices() const {
  if (!deps_.cache) return;
  QDir().mkpath(deps_.cache->root());
  QJsonObject o;
  for (auto it = runChoices_.begin(); it != runChoices_.end(); ++it) o.insert(it.key(), it.value());
  QFile f(deps_.cache->root() + QStringLiteral("/.run-choices.json"));
  if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void VariantsController::reportRunState(const QString& runId, const QString& state) {
  if (deps_.client != nullptr && deps_.client->available()) deps_.client->setRunVariantState(runId, state, [](const VariantCall&) {});
}

QVariantMap VariantsController::runVariantState(const QString& runId) const {
  QString state = runStates_.value(runId.toLower());
  if (state.isEmpty()) state = runVariantBuilt(runId) ? QStringLiteral("ready") : QStringLiteral("not_built");
  QVariantMap m{{QStringLiteral("state"), state}, {QStringLiteral("error"), runErrors_.value(runId.toLower())}, {QStringLiteral("fraction"), 0.0}};
  if (active_ && active_->runId == runId.toLower()) m.insert(QStringLiteral("fraction"), states_.value(active_->variantId).fraction);
  return m;
}

void VariantsController::ensureRunVariant(const QString& runIdIn, const QString& gameId) {
  const QString runId = runIdIn.toLower();
  const QString kind = deps_.runVariantKind ? deps_.runVariantKind(runId) : QString();
  const auto game = deps_.library ? deps_.library->gameById(gameId) : std::nullopt;
  if (!game) {
    emit runVariantFailed(runId, QStringLiteral("base_missing"));
    return;
  }
  if (kind == QLatin1String("own")) {
    // An own variant the person joined the run with: it plays in the run slot like a run variant.
    const QString id = runChoices_.value(runId);
    if (id.isEmpty()) {
      emit runVariantFailed(runId, QStringLiteral("unsupported"));
      return;
    }
    if (deps_.cache->isBuilt(id)) {
      runStates_.insert(runId, QStringLiteral("ready"));
      emit runVariantReady(runId);
      return;
    }
    runStates_.insert(runId, QStringLiteral("building"));
    rebuild(id);
    for (Job& j : queue_)
      if (j.variantId == id) j.runId = runId;
    if (active_ && active_->variantId == id) active_->runId = runId;
    emit runVariantChanged(runId);
    return;
  }
  const QString id = runVariantKey(runId);
  if (stateOf(id) == QLatin1String("building")) return;  // already on its way
  const auto done = [this, runId](const RunVariantRecipe& rr, const VariantCall& c, const GameEntry& g) {
    const QString vid = runVariantKey(runId);
    const auto cached = deps_.cache->recipe(vid);
    if (!c.ok || !rr.valid()) {
      // Server unreachable: a variant built earlier with its recipe still plays.
      if (cached && deps_.cache->isBuilt(vid)) {
        runStates_.insert(runId, QStringLiteral("ready"));
        emit runVariantReady(runId);
        return;
      }
      runStates_.insert(runId, QStringLiteral("failed"));
      runErrors_.insert(runId, c.ok ? QStringLiteral("unsupported") : QStringLiteral("server_unreachable"));
      emit runVariantChanged(runId);
      emit runVariantFailed(runId, runErrors_.value(runId));
      return;
    }
    runRecipes_.insert(runId, rr);
    VariantRecipe r;
    r.variantId = vid;
    r.gameId = g.id;
    r.name = deps_.runSlot ? tr("Run variant") : tr("Run variant");
    r.slot = !rr.slot.isEmpty() ? rr.slot : (deps_.runSlot ? deps_.runSlot(runId) : QString());
    r.system = g.system;
    r.toolId = rr.toolId;
    r.toolVersion = rr.toolVersion;
    r.settings = rr.settings;
    r.seed = rr.seed;
    r.updateVersion = rr.updateVersion;
    if (r.isMod()) {
      r.titleId = !rr.titleId.isEmpty() ? rr.titleId : (deps_.titleIdOf ? deps_.titleIdOf(g) : QString());
      if (r.titleId.isEmpty() && g.gameCodeKind == QLatin1String("3ds-titleid")) r.titleId = g.gameCode;
    } else {
      r.baseSha256 = rr.baseSha256;
      r.romFileName = QFileInfo(g.romFilename).fileName();
    }
    if (cached && deps_.cache->isBuilt(vid) && cached->settings == r.settings && cached->seed == r.seed && cached->toolVersion == r.toolVersion) {
      runStates_.insert(runId, QStringLiteral("ready"));
      reportRunState(runId, QStringLiteral("ready"));
      emit runVariantChanged(runId);
      emit runVariantReady(runId);
      return;
    }
    Job j;
    j.variantId = vid;
    j.gameId = g.id;
    j.recipe = r;
    j.runId = runId;
    states_.remove(vid);
    runStates_.insert(runId, QStringLiteral("building"));
    emit runVariantChanged(runId);
    enqueue(j);
  };
  if (deps_.client == nullptr || !deps_.client->available()) {
    done({}, VariantCall{false, 0, QStringLiteral("network")}, *game);
    return;
  }
  deps_.client->runVariant(runId, [done, g = *game](const RunVariantRecipe& rr, const VariantCall& c) { done(rr, c, g); });
}

}  // namespace uzume::ui
