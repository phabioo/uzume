#include "emulationcontroller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>

namespace uzume::ui {

using Level = EmulationSettings::Level;
using Source = EmulationSettings::Source;

namespace {
bool isProfileKey(const QString& key) {
  return key == QLatin1String(EmulationController::kPadProfileKey) || key == QLatin1String(EmulationController::kKeyboardProfileKey);
}
// Picture options apply to the running game at once; every other row of the per-game dialog waits for the next start.
bool appliesLive(const QString& key) {
  return key == QLatin1String(EmulationController::kIntegerScaleKey) || key == QLatin1String(EmulationController::kAspectRatioKey);
}
bool isTwoScreenKey(const QString& key) {
  return key == QLatin1String(EmulationController::kScreenLayoutKey) || key == QLatin1String(EmulationController::kScreenSwapKey) ||
         key == QLatin1String(EmulationController::kBottomWindowKey);
}
QString labelOf(const QList<emu::CoreOptionValue>& values, const QString& value) {
  for (const emu::CoreOptionValue& v : values) {
    if (v.value == value) return v.label.isEmpty() ? v.value : v.label;
  }
  return value;
}
QVariantList valuesToList(const QList<emu::CoreOptionValue>& values) {
  QVariantList out;
  for (const emu::CoreOptionValue& v : values) {
    out.append(QVariantMap{{QStringLiteral("value"), v.value}, {QStringLiteral("label"), v.label.isEmpty() ? v.value : v.label}});
  }
  return out;
}
}  // namespace

const QList<EmulationController::FbOption>& EmulationController::uzumeOptions() {
  static const QList<FbOption> opts = {
      {QString::fromLatin1(kFullscreenKey),
       QObject::tr("Fullscreen on start"),
       QObject::tr("Opens the game view in fullscreen when a game starts."),
       {{QStringLiteral("off"), QObject::tr("Off")}, {QStringLiteral("on"), QObject::tr("On")}},
       QStringLiteral("off")},
      {QString::fromLatin1(kMultiviewKey),
       QObject::tr("Default Multiview"),
       QObject::tr("Layout the multiview opens with."),
       {{QStringLiteral("pip"), QObject::tr("Picture-in-Picture")},
        {QStringLiteral("row"), QObject::tr("Row")},
        {QStringLiteral("grid"), QObject::tr("Grid 2×2")}},
       QStringLiteral("pip")},
      {QString::fromLatin1(kScreenLayoutKey),
       QObject::tr("DS and 3DS screen layout"),
       QObject::tr("How the two screens are drawn. Systems and games can override. In game, F7 opens the layout menu."),
       {{QStringLiteral("stacked"), QObject::tr("Stacked")},
        {QStringLiteral("side"), QObject::tr("Side by side")},
        {QStringLiteral("large"), QObject::tr("Large top / small bottom")},
        {QStringLiteral("top"), QObject::tr("Top screen only")},
        {QStringLiteral("bottom"), QObject::tr("Bottom screen only")}},
       QStringLiteral("stacked"),
       true},
      {QString::fromLatin1(kScreenSwapKey),
       QObject::tr("Swap screens"),
       QObject::tr("Draws the bottom screen where the top one would be. Not used by the single-screen layouts."),
       {{QStringLiteral("off"), QObject::tr("Off")}, {QStringLiteral("on"), QObject::tr("On")}},
       QStringLiteral("off"),
       true},
      {QString::fromLatin1(kBottomWindowKey),
       QObject::tr("Bottom screen in its own window"),
       QObject::tr("Opens a second window with the bottom screen (touch works there); the game view shows the top screen only."),
       {{QStringLiteral("off"), QObject::tr("Off")}, {QStringLiteral("on"), QObject::tr("On")}},
       QStringLiteral("off"),
       true},
      {QString::fromLatin1(kSpeedUpRatioKey),
       QObject::tr("Speed-up speed"),
       QObject::tr("How fast the game runs while speed-up is on."),
       {{QStringLiteral("1.5"), QStringLiteral("1.5×")}, {QStringLiteral("2"), QStringLiteral("2×")}, {QStringLiteral("3"), QStringLiteral("3×")},
        {QStringLiteral("4"), QStringLiteral("4×")}, {QStringLiteral("6"), QStringLiteral("6×")}, {QStringLiteral("8"), QStringLiteral("8×")}},
       QStringLiteral("2"),
       true},
      {QString::fromLatin1(kSpeedUpOnStartKey),
       QObject::tr("Speed-up on start"),
       QObject::tr("The game starts sped up and stays so until you switch speed-up off."),
       {{QStringLiteral("false"), QObject::tr("Off")}, {QStringLiteral("true"), QObject::tr("On")}},
       QStringLiteral("false"),
       true},
      {QString::fromLatin1(kSpeedUpAudioKey),
       QObject::tr("Audio during speed-up"),
       QObject::tr("Plays the sound at a higher pitch while sped up; off mutes it."),
       {{QStringLiteral("false"), QObject::tr("Off")}, {QStringLiteral("true"), QObject::tr("On")}},
       QStringLiteral("true"),
       true},
      {QString::fromLatin1(kIntegerScaleKey),
       QObject::tr("Integer scaling"),
       QObject::tr("Whole-number scaling. Sharp pixels, may leave borders."),
       {{QStringLiteral("off"), QObject::tr("Off")}, {QStringLiteral("on"), QObject::tr("On")}},
       QStringLiteral("off"),
       true,
       QObject::tr("Picture")},
      {QString::fromLatin1(kAspectRatioKey),
       QObject::tr("Aspect ratio"),
       QObject::tr("Core uses the system's shape (NDS: two 4:3 screens)."),
       {{QStringLiteral("core"), QObject::tr("Core")}, {QStringLiteral("4:3"), QStringLiteral("4:3")}, {QStringLiteral("stretch"), QObject::tr("Stretch")}},
       QStringLiteral("core"),
       true,
       QObject::tr("Picture")},
      {QString::fromLatin1(kPadProfileKey),
       QObject::tr("Gamepad profile"),
       QObject::tr("Button mapping of player 1's gamepad for these games. \"Device's own profile\" keeps the profile assigned to the "
                   "gamepad on the Controllers page; the assignment itself never changes."),
       {},
       QString(),
       true},
      {QString::fromLatin1(kKeyboardProfileKey),
       QObject::tr("Keyboard profile"),
       QObject::tr("Key mapping of the keyboard for these games. \"Device's own profile\" keeps the profile assigned to the keyboard "
                   "on the Controllers page; the assignment itself never changes."),
       {},
       QString(),
       true},
  };
  return opts;
}

EmulationController::EmulationController(const QString& dataDir, const emu::ManifestRegistry* manifests, QObject* parent)
    : QObject(parent), dataDir_(dataDir), manifests_(manifests), settings_(dataDir) {}

QVariantList EmulationController::systems() const {
  QVariantList out;
  for (const QVariant& v : systems_) {
    QVariantMap m = v.toMap();
    m.insert(QStringLiteral("changedCount"), static_cast<int>(settings_.values(Level::System, m.value(QStringLiteral("id")).toString()).size()));
    out.append(m);
  }
  return out;
}

// The decorated game list is built once and reused until games, systems or settings change (every mutation goes through
// emitGamesChanged): QML reads games / game / gameOverrideCount several times per binding evaluation.
void EmulationController::ensureGames() const {
  if (!gamesDirty_) return;
  QHash<QString, QString> sysNames;
  for (const QVariant& c : systems_) {
    const QVariantMap cm = c.toMap();
    sysNames.insert(cm.value(QStringLiteral("id")).toString(), cm.value(QStringLiteral("name")).toString());
  }
  gamesCache_.clear();
  gamesCache_.reserve(games_.size());
  gameIndex_.clear();
  gameOverrides_ = 0;
  for (const QVariant& v : games_) {
    QVariantMap m = v.toMap();
    const QString id = m.value(QStringLiteral("id")).toString();
    const QString systemId = m.value(QStringLiteral("systemId")).toString();
    const QString sysName = sysNames.value(systemId, systemId);
    const int changed = static_cast<int>(settings_.values(Level::Game, id).size());
    m.insert(QStringLiteral("systemName"), sysName);
    m.insert(QStringLiteral("label"), QStringLiteral("%1 · %2").arg(m.value(QStringLiteral("title")).toString(), sysName));
    m.insert(QStringLiteral("changedCount"), changed);
    gameOverrides_ += changed > 0 ? 1 : 0;
    if (!gameIndex_.contains(id)) gameIndex_.insert(id, static_cast<int>(gamesCache_.size()));
    gamesCache_.append(m);
  }
  gamesDirty_ = false;
}

void EmulationController::emitGamesChanged() {
  gamesDirty_ = true;
  ensureGames();
  QList<GamePickerModel::Row> rows;
  rows.reserve(gamesCache_.size());
  for (const QVariant& v : std::as_const(gamesCache_)) {
    const QVariantMap m = v.toMap();
    rows.append({m.value(QStringLiteral("id")).toString(), m.value(QStringLiteral("label")).toString()});
  }
  gamePicker_.setRows(rows);
  emit gamesChanged();
}

QVariantList EmulationController::games() const {
  ensureGames();
  return gamesCache_;
}

int EmulationController::gameCount() const { return static_cast<int>(games_.size()); }

QVariantMap EmulationController::game() const {
  ensureGames();
  const auto it = gameIndex_.constFind(selectedGame_);
  return it == gameIndex_.cend() ? QVariantMap() : gamesCache_.at(it.value()).toMap();
}

int EmulationController::gameOverrideCount() const {
  ensureGames();
  return gameOverrides_;
}

void EmulationController::setGames(const QVariantList& games) {
  if (games == games_) return;
  games_ = games;
  emitGamesChanged();
  if (!selectedGame_.isEmpty() && game().isEmpty()) {  // the game left the library
    selectedGame_.clear();
    emit gameSelectionChanged();
    rebuild();
  }
}

void EmulationController::selectGame(const QString& gameId) {
  if (gameId == selectedGame_) return;
  const auto it = std::find_if(games_.cbegin(), games_.cend(), [&](const QVariant& v) { return v.toMap().value(QStringLiteral("id")).toString() == gameId; });
  if (it == games_.cend()) return;
  selectedGame_ = gameId;
  const QString sys = it->toMap().value(QStringLiteral("systemId")).toString();
  if (sys != selected_) {
    selected_ = sys;
    emit selectionChanged();
  }
  emit gameSelectionChanged();
  const QString core = effectiveCoreId();
  if (!core.isEmpty()) loadCoreCache(core);
  rebuild();
}

QString EmulationController::scope() const {
  if (level_ == QLatin1String("global")) return {};
  return level_ == QLatin1String("game") ? selectedGame_ : selected_;
}

QString EmulationController::effectiveCoreId() const {
  return coreIdFor(selected_, level_ == QLatin1String("game") ? selectedGame_ : QString());
}

QVariantMap EmulationController::cardOf(const QString& systemId) const {
  for (const QVariant& v : systems_) {
    if (v.toMap().value(QStringLiteral("id")).toString() == systemId) return v.toMap();
  }
  return {};
}

QString EmulationController::coreIdFor(const QString& systemId, const QString& gameId) const {
  const QVariantMap card = cardOf(systemId);
  const QString systemCore = card.value(QStringLiteral("coreId")).toString();
  if (gameId.isEmpty()) return systemCore;
  const QString chosen = settings_.value(Level::Game, gameId, QString::fromLatin1(kCoreKey));
  if (chosen.isEmpty()) return systemCore;
  for (const QVariant& c : card.value(QStringLiteral("cores")).toList()) {
    const QString id = c.toMap().value(QStringLiteral("id")).toString();
    if (id == chosen || (manifests_ != nullptr && manifests_->canonicalCoreId(id) == manifests_->canonicalCoreId(chosen))) return id;
  }
  return systemCore;  // a core the Hub no longer serves is ignored (the notice names it)
}

std::optional<emu::SystemManifest> EmulationController::manifestFor(const QString& systemId, const QString& coreId) const {
  if (manifests_ == nullptr || systemId.isEmpty()) return std::nullopt;
  if (!coreId.isEmpty()) {
    if (auto m = manifests_->resolve(systemId, coreId)) return m;
  }
  const emu::SystemManifest* base = manifests_->find(systemId);
  return base != nullptr ? std::optional<emu::SystemManifest>(*base) : std::nullopt;
}

int EmulationController::defaultsChangedCount() const { return static_cast<int>(settings_.values(Level::Global, QString()).size()); }

QVariantMap EmulationController::system() const {
  for (const QVariant& v : systems_) {
    if (v.toMap().value(QStringLiteral("id")).toString() == selected_) return v.toMap();
  }
  return {};
}

std::optional<emu::SystemManifest> EmulationController::selectedManifest() const { return manifestFor(selected_, effectiveCoreId()); }

QString EmulationController::cachePath(const QString& coreId) const {
  return QDir(dataDir_).filePath(QStringLiteral("cache/core-options/") + coreId + QStringLiteral(".json"));
}

void EmulationController::setSystems(const QVariantList& cards) {
  if (cards == systems_ && !selected_.isEmpty()) {
    return;  // periodic refresh without a change: keep the page as it is
  }
  systems_ = cards;
  if (selected_.isEmpty() || system().isEmpty()) {
    selected_ = cards.isEmpty() ? QString() : cards.first().toMap().value(QStringLiteral("id")).toString();
    emit selectionChanged();
  }
  emit systemsChanged();
  emitGamesChanged();
  rebuild();
}

void EmulationController::setCoreProbing(bool probing) {
  if (coreProbing_ == probing) return;
  coreProbing_ = probing;
  emit coreProbingChanged();
  rebuild();
}

void EmulationController::setGameRunning(bool running) {
  if (gameRunning_ == running) return;
  gameRunning_ = running;
  restartTouched_ = false;
  emit gameRunningChanged();
  rebuild();
}

void EmulationController::setCoreProbe(const QString& coreId, const emu::CoreProbe& probe, bool writeCache) {
  if (!probe.ok) return;
  probes_.insert(coreId, probe);
  if (writeCache) {
    const QString path = cachePath(coreId);
    if (QDir().mkpath(QFileInfo(path).absolutePath())) {
      QSaveFile f(path);
      if (f.open(QIODevice::WriteOnly)) {
        f.write(emu::coreProbeToJson(probe));
        f.commit();
      }
    }
  }
  rebuild();
}

// A successful probe counts as done even when the core reports no options (no re-probe, no cache rewrite).
bool EmulationController::hasCoreProbe(const QString& coreId) const { return probes_.contains(coreId); }

const emu::CoreProbe* EmulationController::coreProbe(const QString& coreId) const {
  const auto it = probes_.constFind(coreId);
  return it == probes_.cend() ? nullptr : &it.value();
}

void EmulationController::loadCoreCache(const QString& coreId) {
  if (probes_.contains(coreId)) return;
  QFile f(cachePath(coreId));
  if (!f.open(QIODevice::ReadOnly)) return;
  const emu::CoreProbe p = emu::coreProbeFromJson(f.readAll());
  if (p.ok) {
    probes_.insert(coreId, p);
    rebuild();
  }
}

void EmulationController::setLevel(const QString& level) {
  if ((level != QLatin1String("global") && level != QLatin1String("system") && level != QLatin1String("game")) || level == level_) return;
  level_ = level;
  emit levelChanged();
  rebuild();
  emit systemsChanged();
}

void EmulationController::selectSystem(const QString& id) {
  if (id == selected_) return;
  for (const QVariant& v : systems_) {
    if (v.toMap().value(QStringLiteral("id")).toString() == id) {
      selected_ = id;
      emit selectionChanged();
      rebuild();
      return;
    }
  }
}

Level EmulationController::levelEnum() const {
  return level_ == QLatin1String("global") ? Level::Global : (level_ == QLatin1String("game") ? Level::Game : Level::System);
}

QString EmulationController::uzumeValue(const QString& key, const QString& systemId, const QString& gameId) const {
  QString def;
  for (const FbOption& o : uzumeOptions()) {
    if (o.key == key) def = o.defaultValue;
  }
  const QString value = settings_.resolve(key, systemId, gameId, QString(), def).value;
  // 0.14: the Multiview layout "Side by side" is called "Row" (the DS screen layout keeps the old name); old stored value.
  if (key == QLatin1String(kMultiviewKey) && value == QLatin1String("side")) return QStringLiteral("row");
  return value;
}

QMap<QString, QString> EmulationController::launchOverrides(const QString& systemId, const QString& gameId) const {
  return settings_.mergedOverrides(systemId, gameId);
}

bool EmulationController::knownOption(const QString& key, QList<emu::CoreOptionValue>* values) const {
  for (const FbOption& o : uzumeOptions()) {
    if (o.key == key) {
      if (isProfileKey(key)) {
        if (level_ == QLatin1String("global")) return false;
        *values = profileValues(key, QString());
      } else {
        *values = o.values;
      }
      return true;
    }
  }
  if (level_ == QLatin1String("global")) return false;  // core keys are set per system/core
  if (key == QLatin1String(kCoreKey)) {
    if (level_ == QLatin1String("game")) values->append({QString(), tr("Use system default")});
    for (const QVariant& c : system().value(QStringLiteral("cores")).toList()) {
      const QVariantMap cm = c.toMap();
      values->append({cm.value(QStringLiteral("id")).toString(), cm.value(QStringLiteral("label")).toString()});
    }
    return values->size() > (level_ == QLatin1String("game") ? 1 : 0);
  }
  const std::optional<emu::SystemManifest> manOpt = selectedManifest();
  const emu::SystemManifest* man = manOpt ? &*manOpt : nullptr;
  const emu::CoreProbe* probe = man != nullptr ? coreProbe(man->coreId) : nullptr;
  if (man == nullptr || probe == nullptr || emu::isLockedCoreOption(*man, key)) return false;
  for (const emu::CoreOption& o : probe->options) {
    if (o.key == key && (o.visible || man->alwaysShownCoreOptions.contains(key))) {
      *values = o.values;
      return true;
    }
  }
  return false;
}

QString EmulationController::defaultValueOf(const QString& key) const {
  const bool global = level_ == QLatin1String("global");
  for (const FbOption& o : uzumeOptions()) {
    if (o.key == key) return o.defaultValue;
  }
  if (key == QLatin1String(kCoreKey)) return level_ == QLatin1String("game") ? QString() : system().value(QStringLiteral("defaultCoreId")).toString();
  const std::optional<emu::SystemManifest> manOpt = selectedManifest();
  const emu::SystemManifest* man = manOpt ? &*manOpt : nullptr;
  if (man == nullptr) return {};
  QString coreDefault;
  if (const emu::CoreProbe* probe = coreProbe(man->coreId)) {
    for (const emu::CoreOption& o : probe->options) {
      if (o.key == key) coreDefault = o.defaultValue;
    }
  }
  // System level inherits a Global value if the file has one; otherwise manifest, then core default.
  if (level_ == QLatin1String("game") && settings_.hasValue(Level::System, selected_, key)) return settings_.value(Level::System, selected_, key);
  if (!global && settings_.hasValue(Level::Global, QString(), key)) return settings_.value(Level::Global, QString(), key);
  const QString manifestDefault = man->coreOptions.value(key, QString());
  return manifestDefault.isNull() ? coreDefault : manifestDefault;
}

void EmulationController::setOption(const QString& key, const QString& value) {
  QList<emu::CoreOptionValue> values;
  if (selected_.isEmpty() || (level_ == QLatin1String("game") && selectedGame_.isEmpty()) || !knownOption(key, &values)) return;
  const bool valid = std::any_of(values.cbegin(), values.cend(), [&](const emu::CoreOptionValue& v) { return v.value == value; });
  if (!valid) return;
  if (value == defaultValueOf(key)) {
    settings_.removeValue(levelEnum(), scope(), key);  // back to the default: nothing stays stored, no "changed" mark
  } else {
    settings_.setValue(levelEnum(), scope(), key, value);
    if (gameRunning_ && !key.startsWith(QLatin1String("uzume."))) restartTouched_ = true;
  }
  rebuild();
  emit systemsChanged();
  emitGamesChanged();
  if (key.startsWith(QLatin1String("uzume."))) emit uzumeOptionsChanged();
  if (key == QLatin1String(kCoreKey)) emit coreChoiceChanged();
}

void EmulationController::resetOption(const QString& key) {
  if (selected_.isEmpty() || (level_ == QLatin1String("game") && selectedGame_.isEmpty())) return;
  settings_.removeValue(levelEnum(), scope(), key);
  rebuild();
  emit systemsChanged();
  emitGamesChanged();
  if (key.startsWith(QLatin1String("uzume."))) emit uzumeOptionsChanged();
  if (key == QLatin1String(kCoreKey)) emit coreChoiceChanged();
}

void EmulationController::resetAllChanged() {
  if (selected_.isEmpty() || (level_ == QLatin1String("game") && selectedGame_.isEmpty())) return;
  bool fb = false;
  bool coreChanged = false;
  const QMap<QString, QString> vals = settings_.values(levelEnum(), scope());
  for (auto it = vals.cbegin(); it != vals.cend(); ++it) {
    settings_.removeValue(levelEnum(), scope(), it.key());
    fb = fb || it.key().startsWith(QLatin1String("uzume."));
    coreChanged = coreChanged || it.key() == QLatin1String(kCoreKey);
  }
  restartTouched_ = false;
  rebuild();
  emit systemsChanged();
  emitGamesChanged();
  if (fb) emit uzumeOptionsChanged();
  if (coreChanged) emit coreChoiceChanged();
}

void EmulationController::setCategoryFilter(const QString& category) {
  if (categoryFilter_ == category) return;
  categoryFilter_ = category;
  rebuild();
  emit filterChanged();
}

void EmulationController::setSearchText(const QString& text) {
  if (searchText_ == text) return;
  searchText_ = text;
  rebuild();
  emit filterChanged();
}

QVariantMap EmulationController::row(const QString& key, const QString& label, const QString& description, const QString& category,
                                     const QList<emu::CoreOptionValue>& values, const QString& manifestDefault,
                                     const QString& coreDefault, bool uzume) const {
  const bool global = level_ == QLatin1String("global");
  const Level lvl = levelEnum();
  const bool isSet = settings_.hasValue(lvl, scope(), key);
  // Value shown: what the level being edited yields (global: global > default; system: system > global > ... ).
  const bool gameLvl = level_ == QLatin1String("game");
  const EmulationSettings::Resolved r =
      settings_.resolve(key, global ? QString() : selected_, gameLvl ? selectedGame_ : QString(), manifestDefault, coreDefault);
  QString origin;
  if (isSet) {
    origin = tr("set here");
  } else {
    switch (r.source) {
      case Source::System: origin = tr("inherited · System"); break;
      case Source::Global: origin = tr("inherited · Global"); break;
      case Source::Manifest: origin = tr("inherited · Uzume default"); break;
      case Source::Core: origin = uzume ? (global ? tr("default") : tr("inherited · default")) : tr("inherited · core default"); break;
      default: origin = tr("inherited"); break;
    }
  }
  QVariantList vals;
  QString valueLabel = r.value;
  bool known = false;
  for (const emu::CoreOptionValue& v : values) {
    vals.append(QVariantMap{{QStringLiteral("value"), v.value}, {QStringLiteral("label"), v.label.isEmpty() ? v.value : v.label}});
    if (v.value == r.value) {
      valueLabel = v.label.isEmpty() ? v.value : v.label;
      known = true;
    }
  }
  if (!known && !r.value.isEmpty()) {
    // A stored value the core does not offer (any more) is shown as it is, never silently replaced.
    vals.prepend(QVariantMap{{QStringLiteral("value"), r.value}, {QStringLiteral("label"), r.value}});
  }
  return {{QStringLiteral("key"), key},
          {QStringLiteral("label"), label},
          {QStringLiteral("description"), description},
          {QStringLiteral("category"), category},
          {QStringLiteral("values"), vals},
          {QStringLiteral("value"), r.value},
          {QStringLiteral("valueLabel"), valueLabel},
          {QStringLiteral("isSet"), isSet},
          {QStringLiteral("origin"), origin},
          {QStringLiteral("restart"), !uzume && gameRunning_}};
}

void EmulationController::rebuild() {
  QVariantList groups;
  QStringList categories;
  lockedCount_ = 0;
  coreNote_.clear();
  changedCount_ = 0;
  const auto keep = [this](const QVariantMap& row) {
    if (!categoryFilter_.isEmpty() && row.value(QStringLiteral("category")).toString() != categoryFilter_) return false;
    const QString needle = searchText_.trimmed();
    return needle.isEmpty() || row.value(QStringLiteral("label")).toString().contains(needle, Qt::CaseInsensitive) ||
           row.value(QStringLiteral("description")).toString().contains(needle, Qt::CaseInsensitive);
  };
  // Collects chips and the changed count over all rows of the scope, returns the rows that pass the filters.
  const auto collect = [&](const QVariantList& rows) {
    QVariantList out;
    for (const QVariant& v : rows) {
      const QVariantMap r = v.toMap();
      const QString cat = r.value(QStringLiteral("category")).toString();
      if (!cat.isEmpty() && !categories.contains(cat)) categories.append(cat);
      if (r.value(QStringLiteral("isSet")).toBool()) ++changedCount_;
    }
    if (!categoryFilter_.isEmpty() && !categories.contains(categoryFilter_)) categoryFilter_.clear();
    for (const QVariant& v : rows) {
      if (keep(v.toMap())) out.append(v);
    }
    return out;
  };
  if (!selected_.isEmpty() && !(level_ == QLatin1String("game") && selectedGame_.isEmpty())) {
    const bool global = level_ == QLatin1String("global");
    const bool gameLvl = level_ == QLatin1String("game");
    if (global) {
      // Defaults: options that do not depend on a system.
      QVariantList fb;
      for (const FbOption& o : uzumeOptions()) {
        if (isProfileKey(o.key)) continue;  // controller profiles are chosen per system or game
        fb.append(row(o.key, o.label, o.description, !o.category.isEmpty() ? o.category : (o.perSystem && !isTwoScreenKey(o.key)) ? tr("Speed-up") : tr("Display"), o.values, QString(), o.defaultValue, true));
      }
      groups.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("uzume")},
                                {QStringLiteral("title"), tr("Uzume")},
                                {QStringLiteral("subtitle"), tr("Presentation in Uzume")},
                                {QStringLiteral("note"), QString()},
                                {QStringLiteral("options"), collect(fb)}});
    }
    // Core group (system scope only: the keys belong to the core)
    const std::optional<emu::SystemManifest> manOpt = selectedManifest();
    const emu::SystemManifest* man = manOpt ? &*manOpt : nullptr;
    if (man != nullptr && !global) {
      const emu::CoreProbe* probe = coreProbe(man->coreId);
      QString coreName = probe != nullptr && !probe->info.name.isEmpty() ? probe->info.name : QString();
      if (gameLvl && coreName.isEmpty()) coreName = man->coreDisplayName;
      for (const QVariant& v : systems_) {
        const QVariantMap m = v.toMap();
        if (!gameLvl && m.value(QStringLiteral("id")).toString() == selected_ && coreName.isEmpty()) coreName = m.value(QStringLiteral("coreName")).toString();
      }
      QVariantList core;
      if (probe != nullptr) {
        // Categories in the order the core reports them (options without a category last).
        QStringList order;
        for (const emu::CoreOptionCategory& c : probe->categories) order.append(c.key);
        order.append(QString());
        for (const QString& catKey : std::as_const(order)) {
          QString catName;
          for (const emu::CoreOptionCategory& c : probe->categories) {
            if (c.key == catKey) catName = c.description;
          }
          if (catName.isEmpty()) catName = tr("Other");
          for (const emu::CoreOption& o : probe->options) {
            if (o.categoryKey != catKey && !(catKey.isEmpty() && !order.contains(o.categoryKey))) continue;
            if ((!o.visible && !man->alwaysShownCoreOptions.contains(o.key)) || o.values.isEmpty()) continue;
            if (emu::isLockedCoreOption(*man, o.key)) {
              ++lockedCount_;
              continue;
            }
            core.append(row(o.key, o.description, o.info, catName, o.values, man->coreOptions.value(o.key, QString()), o.defaultValue, false));
          }
        }
      }
      if ((probe == nullptr || probe->options.isEmpty()) && coreProbing_) {
        coreNote_.clear();  // the page shows the loading row instead
      } else if (probe == nullptr || probe->options.isEmpty()) {
        coreNote_ = tr("The core has not reported its options yet. They appear after the core was loaded once; "
                       "Uzume does that when it can (not while a game is running).");
      }
      QString groupNote = coreNote_;
      if (man->experimental) {
        const QString exp = tr("Experimental core: Uzume has no profile for it. It runs without option defaults or locked options, "
                               "its picture is shown as the core renders it, and touch input is off.");
        groupNote = groupNote.isEmpty() ? exp : groupNote + QLatin1Char(' ') + exp;
      }
      groups.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("core")},
                                {QStringLiteral("title"), coreName.isEmpty() ? man->coreId : coreName},
                                {QStringLiteral("subtitle"), tr("from Libretro core options · only options reported by the core")},
                                {QStringLiteral("note"), groupNote},
                                {QStringLiteral("options"), collect(core)}});
    }
    // Speed-up options of the system: collected after the core group so its categories exist for the filter.
    if (!global) {
      QVariantList fb;
      const QVariantMap card = system();
      QList<emu::CoreOptionValue> coreValues;
      if (knownOption(QString::fromLatin1(kCoreKey), &coreValues)) {
        QVariantMap r = row(QString::fromLatin1(kCoreKey), tr("Core"),
                            tr("The core that runs games of this system. The server offers the cores listed here; the change applies the next time a game starts."),
                            QString(), coreValues, QString(), card.value(QStringLiteral("defaultCoreId")).toString(), true);
        const QString effective = card.value(QStringLiteral("coreId")).toString();
        if (!card.value(QStringLiteral("coreNotice")).toString().isEmpty() && !effective.isEmpty()) {
          // The stored choice is not served any more: show what is really used.
          for (const emu::CoreOptionValue& v : std::as_const(coreValues)) {
            if (v.value == effective) {
              r.insert(QStringLiteral("value"), v.value);
              r.insert(QStringLiteral("valueLabel"), v.label);
            }
          }
          r.insert(QStringLiteral("values"), [&]() {
            QVariantList vals;
            for (const emu::CoreOptionValue& v : std::as_const(coreValues)) vals.append(QVariantMap{{QStringLiteral("value"), v.value}, {QStringLiteral("label"), v.label}});
            return vals;
          }());
        } else if (!r.value(QStringLiteral("isSet")).toBool() && card.value(QStringLiteral("noCoreChosen")).toBool() && !gameLvl) {
          // The Hub preselects no core (since 0.13) and none was chosen: nothing is shown as selected.
          r.insert(QStringLiteral("value"), QString());
          r.insert(QStringLiteral("valueLabel"), tr("Choose a core"));
          r.insert(QStringLiteral("origin"), tr("No core chosen yet"));
          r.insert(QStringLiteral("description"),
                   tr("No core chosen yet. The server offers the cores listed here; until you choose, Uzume uses %1.")
                       .arg(card.value(QStringLiteral("fallbackCoreName")).toString()));
        } else if (!r.value(QStringLiteral("isSet")).toBool()) {
          r.insert(QStringLiteral("origin"), tr("Server default"));
        }
        if (gameLvl) {
          // Per game: "Use system default" when the game has no choice of its own; the stored id is shown even when the Hub
          // does not serve that core any more (listed as it is, never replaced silently).
          const QString own = settings_.value(Level::Game, selectedGame_, QString::fromLatin1(kCoreKey));
          if (own.isEmpty()) {
            r.insert(QStringLiteral("value"), QString());
            r.insert(QStringLiteral("valueLabel"), tr("Use system default"));
            r.insert(QStringLiteral("origin"), tr("inherited · %1").arg(card.value(QStringLiteral("coreName")).toString()));
          }
          r.insert(QStringLiteral("description"),
                   tr("The core that runs this game. \"Use system default\" follows the system's choice; the change applies the next time the game starts."));
        }
        r.insert(QStringLiteral("restart"), false);
        fb.append(r);
      }
      for (const FbOption& o : uzumeOptions()) {
        if (!o.perSystem) continue;
        // The screen layout only exists for systems with two screens (DS, 3DS); "Overrides Defaults for DS games."
        const bool twoScreen = o.key == QLatin1String(kScreenLayoutKey) || o.key == QLatin1String(kScreenSwapKey) || o.key == QLatin1String(kBottomWindowKey);
        if (twoScreen && (man == nullptr || man->display.screens.size() < 2)) continue;
        QList<emu::CoreOptionValue> vals = o.values;
        if (isProfileKey(o.key)) vals = profileValues(o.key, settings_.resolve(o.key, selected_, gameLvl ? selectedGame_ : QString(), QString(), o.defaultValue).value);
        fb.append(row(o.key, o.label, o.description, o.category, vals, QString(), o.defaultValue, true));
      }
      groups.prepend(QVariantMap{{QStringLiteral("id"), QStringLiteral("uzume")},
                                {QStringLiteral("title"), tr("Uzume")},
                                {QStringLiteral("subtitle"), gameLvl ? tr("Core and speed-up for this game") : tr("Core and speed-up in Uzume")},
                                {QStringLiteral("note"), gameLvl ? QString() : card.value(QStringLiteral("coreNotice")).toString()},
                                {QStringLiteral("options"), collect(fb)}});
    }
  }
  categories_ = categories;
  restartHint_ = restartTouched_ && gameRunning_;
  groups_ = groups;
  emit groupsChanged();
  emit gameSettingsChanged(QString());
}

// ---------------------------------------------------------------- Controller profiles and per-game settings

void EmulationController::setControllerProfiles(const ControllerProfiles* profiles) {
  profiles_ = profiles;
  rebuild();
}

void EmulationController::setRunningGame(const QString& gameId) {
  if (runningGame_ == gameId) return;
  runningGame_ = gameId;
  emit gameSettingsChanged(QString());
}

// Profiles of the kind plus "Device's own profile". A stored id whose profile is gone stays listed (marked missing) so it is
// never replaced silently; at game start the device's own profile is used for it.
QList<emu::CoreOptionValue> EmulationController::profileValues(const QString& key, const QString& stored) const {
  QList<emu::CoreOptionValue> out;
  out.append({QString(), tr("Device's own profile")});
  const QString kind = key == QLatin1String(kKeyboardProfileKey) ? QStringLiteral("keyboard") : QStringLiteral("gamepad");
  bool found = stored.isEmpty();
  if (profiles_ != nullptr) {
    for (const ControllerProfile& p : profiles_->ofKind(kind)) {
      out.append({p.id, p.name});
      found = found || p.id == stored;
    }
  }
  if (!found) out.append({stored, tr("Deleted profile (missing)")});
  return out;
}

int EmulationController::gameChangedCount(const QString& gameId) const {
  return gameId.isEmpty() ? 0 : static_cast<int>(settings_.values(Level::Game, gameId).size());
}

void EmulationController::ensureGameCore(const QString& gameId) {
  ensureGames();
  const auto it = gameIndex_.constFind(gameId);
  if (it == gameIndex_.cend()) return;
  const QString coreId = coreIdFor(gamesCache_.at(it.value()).toMap().value(QStringLiteral("systemId")).toString(), gameId);
  if (!coreId.isEmpty()) loadCoreCache(coreId);
  emit gameCoreWanted(gameId);
}

QVariantMap EmulationController::gameSettings(const QString& gameId) const {
  ensureGames();
  const auto git = gameIndex_.constFind(gameId);
  if (git == gameIndex_.cend()) return {};
  const QVariantMap game = gamesCache_.at(git.value()).toMap();
  const QString sys = game.value(QStringLiteral("systemId")).toString();
  const QVariantMap card = cardOf(sys);
  const QString coreId = coreIdFor(sys, gameId);
  const std::optional<emu::SystemManifest> manOpt = manifestFor(sys, coreId);
  const emu::SystemManifest* man = manOpt ? &*manOpt : nullptr;
  const emu::CoreProbe* probe = coreProbe(man != nullptr && !man->coreId.isEmpty() ? man->coreId : coreId);
  const bool runningHere = !runningGame_.isEmpty() && runningGame_ == gameId;

  // One row: the value shown is what the game uses; "defaultValue" is what it inherits (system > global > default).
  const auto makeRow = [&](const QString& key, const QString& label, const QString& description, const QString& category,
                           QList<emu::CoreOptionValue> values, const QString& manifestDefault, const QString& coreDefault, bool restart) {
    const bool isSet = settings_.hasValue(Level::Game, gameId, key);
    const QString inherited = settings_.resolve(key, sys, QString(), manifestDefault, coreDefault).value;
    const QString shown = isSet ? settings_.value(Level::Game, gameId, key) : inherited;
    bool known = false;
    for (const emu::CoreOptionValue& v : std::as_const(values)) known = known || v.value == shown;
    if (!known && !shown.isEmpty()) values.prepend({shown, shown});  // a stored value the core no longer offers stays visible
    return QVariantMap{{QStringLiteral("key"), key},
                       {QStringLiteral("label"), label},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("category"), category},
                       {QStringLiteral("values"), valuesToList(values)},
                       {QStringLiteral("value"), shown},
                       {QStringLiteral("valueLabel"), labelOf(values, shown)},
                       {QStringLiteral("isSet"), isSet},
                       {QStringLiteral("defaultValue"), inherited},
                       {QStringLiteral("defaultLabel"), labelOf(values, inherited)},
                       {QStringLiteral("restart"), restart && runningHere}};
  };
  const auto uzumeRow = [&](const FbOption& o) {
    QList<emu::CoreOptionValue> vals = o.values;
    if (isProfileKey(o.key)) vals = profileValues(o.key, settings_.resolve(o.key, sys, gameId, QString(), o.defaultValue).value);
    return makeRow(o.key, o.label, o.description, o.category, vals, QString(), o.defaultValue, !appliesLive(o.key));
  };

  QVariantList picture, controls, speed, core;
  for (const FbOption& o : uzumeOptions()) {
    if (!o.perSystem) continue;
    if (isTwoScreenKey(o.key) && (man == nullptr || man->display.screens.size() < 2)) continue;  // DS / 3DS only
    if (isProfileKey(o.key)) controls.append(uzumeRow(o));
    else if (appliesLive(o.key) || isTwoScreenKey(o.key)) picture.append(uzumeRow(o));
    else speed.append(uzumeRow(o));
  }

  // Core choice: "Use system default" or one of the cores the server serves; a stored choice that is no longer served stays visible.
  QString coreName = probe != nullptr && !probe->info.name.isEmpty() ? probe->info.name : QString();
  QList<emu::CoreOptionValue> coreValues;
  const QString ownCore = settings_.value(Level::Game, gameId, QString::fromLatin1(kCoreKey));
  QString systemCoreName = card.value(QStringLiteral("coreName")).toString();
  for (const QVariant& c : card.value(QStringLiteral("cores")).toList()) {
    const QVariantMap cm = c.toMap();
    coreValues.append({cm.value(QStringLiteral("id")).toString(), cm.value(QStringLiteral("label")).toString()});
    if (coreName.isEmpty() && cm.value(QStringLiteral("id")).toString() == coreId) coreName = cm.value(QStringLiteral("name")).toString();
  }
  if (coreName.isEmpty() && man != nullptr) coreName = man->coreDisplayName;
  if (coreName.isEmpty()) coreName = systemCoreName;
  if (coreValues.size() > 1 || !ownCore.isNull()) {
    QList<emu::CoreOptionValue> vals{{QString(), tr("Use system default")}};
    vals.append(coreValues);
    QVariantMap r = makeRow(QString::fromLatin1(kCoreKey), tr("Core"),
                            tr("The core that runs this game. \"Use system default\" follows the system's choice; the change applies the next time the game starts."),
                            QString(), vals, QString(), QString(), true);
    // The game's own choice or none (the system's choice is not a value of this row).
    const QString shown = ownCore.isNull() ? QString() : ownCore;
    bool listed = shown.isEmpty();
    for (const emu::CoreOptionValue& v : std::as_const(vals)) listed = listed || v.value == shown;
    if (!listed) vals.append({shown, shown});
    r.insert(QStringLiteral("values"), valuesToList(vals));
    r.insert(QStringLiteral("value"), shown);
    r.insert(QStringLiteral("valueLabel"), labelOf(vals, shown));
    r.insert(QStringLiteral("defaultValue"), QString());
    r.insert(QStringLiteral("defaultLabel"), systemCoreName.isEmpty() ? tr("Use system default") : systemCoreName);
    core.append(r);
  }

  // Core options of the game's core, exactly as the core reported them (nothing invented, locked options left out).
  QString coreNote;
  if (man != nullptr && probe != nullptr) {
    QStringList order;
    for (const emu::CoreOptionCategory& c : probe->categories) order.append(c.key);
    order.append(QString());
    for (const QString& catKey : std::as_const(order)) {
      QString catName;
      for (const emu::CoreOptionCategory& c : probe->categories) {
        if (c.key == catKey) catName = c.description;
      }
      if (catName.isEmpty()) catName = tr("Other");
      for (const emu::CoreOption& o : probe->options) {
        if (o.categoryKey != catKey && !(catKey.isEmpty() && !order.contains(o.categoryKey))) continue;
        if ((!o.visible && !man->alwaysShownCoreOptions.contains(o.key)) || o.values.isEmpty()) continue;
        if (emu::isLockedCoreOption(*man, o.key)) continue;
        core.append(makeRow(o.key, o.description, o.info, catName, o.values, man->coreOptions.value(o.key, QString()), o.defaultValue, true));
      }
    }
    if (probe->options.isEmpty()) coreNote = tr("The core has not reported any options.");
  } else {
    coreNote = tr("The core's options appear after it was loaded once. Uzume does that when it can (not while a game is running).");
  }

  QVariantList tabs;
  int changedAll = 0;
  const auto addTab = [&](const QString& id, const QString& title, const QVariantList& rows, const QString& note) {
    if (rows.isEmpty() && note.isEmpty()) return;
    int changed = 0;
    for (const QVariant& v : rows) changed += v.toMap().value(QStringLiteral("isSet")).toBool() ? 1 : 0;
    changedAll += changed;
    tabs.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("title"), title}, {QStringLiteral("changed"), changed},
                            {QStringLiteral("note"), note}, {QStringLiteral("rows"), rows}});
  };
  addTab(QStringLiteral("picture"), tr("Picture"), picture, QString());
  addTab(QStringLiteral("controls"), tr("Controls"), controls, QString());
  addTab(QStringLiteral("speedup"), tr("Speed-up"), speed, QString());
  addTab(QStringLiteral("core"), tr("Core"), core, coreNote);

  bool restartHint = false;
  for (const QVariant& t : std::as_const(tabs)) {
    for (const QVariant& r : t.toMap().value(QStringLiteral("rows")).toList()) {
      restartHint = restartHint || (r.toMap().value(QStringLiteral("restart")).toBool() && r.toMap().value(QStringLiteral("isSet")).toBool());
    }
  }
  return {{QStringLiteral("gameId"), gameId},
          {QStringLiteral("title"), game.value(QStringLiteral("title")).toString()},
          {QStringLiteral("systemId"), sys},
          {QStringLiteral("systemLabel"), game.value(QStringLiteral("systemName")).toString()},
          {QStringLiteral("coreName"), coreName},
          {QStringLiteral("coreKnown"), probe != nullptr},
          {QStringLiteral("changedCount"), gameChangedCount(gameId)},
          {QStringLiteral("restartHint"), restartHint},
          {QStringLiteral("note"), coreNote},
          {QStringLiteral("tabs"), tabs}};
}

void EmulationController::afterGameChange(const QString& gameId, const QString& key) {
  if (level_ == QLatin1String("game") && selectedGame_ == gameId) rebuild();  // the Emulation page shows the same game
  emitGamesChanged();
  emit gameSettingsChanged(gameId);
  if (key.startsWith(QLatin1String("uzume."))) emit uzumeOptionsChanged();
  if (key == QLatin1String(kCoreKey)) emit coreChoiceChanged();
}

void EmulationController::setGameOption(const QString& gameId, const QString& key, const QString& value) {
  const QVariantMap data = gameSettings(gameId);
  for (const QVariant& t : data.value(QStringLiteral("tabs")).toList()) {
    for (const QVariant& r : t.toMap().value(QStringLiteral("rows")).toList()) {
      const QVariantMap row = r.toMap();
      if (row.value(QStringLiteral("key")).toString() != key) continue;
      bool valid = false;
      for (const QVariant& v : row.value(QStringLiteral("values")).toList()) valid = valid || v.toMap().value(QStringLiteral("value")).toString() == value;
      if (!valid) return;
      if (value == row.value(QStringLiteral("defaultValue")).toString()) {
        settings_.removeValue(Level::Game, gameId, key);  // back to what the game inherits: nothing stays stored
      } else {
        settings_.setValue(Level::Game, gameId, key, value);
        if (gameRunning_ && !key.startsWith(QLatin1String("uzume."))) restartTouched_ = true;
      }
      afterGameChange(gameId, key);
      return;
    }
  }
}

void EmulationController::resetGameOption(const QString& gameId, const QString& key) {
  if (!settings_.hasValue(Level::Game, gameId, key)) return;
  settings_.removeValue(Level::Game, gameId, key);
  afterGameChange(gameId, key);
}

void EmulationController::resetGameOptions(const QString& gameId) {
  const QMap<QString, QString> vals = settings_.values(Level::Game, gameId);
  if (vals.isEmpty()) return;
  bool fb = false;
  bool core = false;
  for (auto it = vals.cbegin(); it != vals.cend(); ++it) {
    settings_.removeValue(Level::Game, gameId, it.key());
    fb = fb || it.key().startsWith(QLatin1String("uzume."));
    core = core || it.key() == QLatin1String(kCoreKey);
  }
  restartTouched_ = false;
  if (level_ == QLatin1String("game") && selectedGame_ == gameId) rebuild();
  emitGamesChanged();
  emit gameSettingsChanged(gameId);
  if (fb) emit uzumeOptionsChanged();
  if (core) emit coreChoiceChanged();
}

}  // namespace uzume::ui
