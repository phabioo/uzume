#include "corecatalog.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <algorithm>

#include "core_options.h"
#include "firmware_materializer.h"
#include "libretro_backend.h"
#include "semver.h"
#include "version.h"

namespace uzume::ui {

CoreCatalog::CoreCatalog(const Deps& d, QObject* parent)
    : QObject(parent),
      conn_(d.conn),
      profiles_(d.profiles),
      systems_(d.systems),
      coreCache_(d.coreCache),
      emulation_(d.emulation),
      manifests_(d.manifests),
      locator_(d.locator),
      coreList_(d.coreList),
      coreVersions_(d.coreVersions),
      coreNames_(d.coreNames),
      coreProblems_(d.coreProblems),
      fwProblems_(d.fwProblems),
      phase_(d.phase),
      launchGame_(d.launchGame) {
  connect(&prober_, &CoreProber::probed, this, [this](const QString& coreId, const QString&, const emu::CoreProbe& probe) { applyProbe(coreId, probe); });
  connect(&prober_, &CoreProber::idle, this, &CoreCatalog::coresProbed);
}

QString CoreCatalog::systemDirIn(const QString& baseDir) { return QDir(baseDir).filePath(QStringLiteral("system")); }

namespace {
QString libraryKeyOf(const QString& path) {
  const QFileInfo fi(path);
  return QStringLiteral("%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
}
}  // namespace

void CoreCatalog::probeCores() {
  // The core version is only available in the core info after loadCore(). A worker thread loads each core once
  // without a game (that also yields its options for the Emulation page), reads name/version and unloads it again.
  // If that fails, we report only the core_id (empty version) instead of guessing.
  if (phase_ == PlayPhase::Launching) {
    return;  // a game is about to load its core; the next call after the game picks the list up
  }
  for (const emu::SystemManifest& sys : manifests_.all()) {
    // Every core of the system, not only the one in use: a second installed core (DeSmuME next to melonDS) must be listed too (#411).
    QList<emu::SystemManifest> candidates;
    if (const emu::SystemManifest* mp = manifestForSystem(sys.systemId)) candidates.append(*mp);
    QStringList coreIds;
    for (const QVariant& c : coresOf(sys.systemId)) coreIds.append(c.toMap().value(QStringLiteral("id")).toString());
    for (const emu::CoreProfile& p : manifests_.profilesForSystem(sys.systemId)) coreIds.append(p.coreId);
    for (const QString& id : std::as_const(coreIds)) {
      const bool have = std::any_of(candidates.cbegin(), candidates.cend(), [&](const emu::SystemManifest& c) { return c.coreId == id; });
      if (have || id.isEmpty()) continue;
      if (const auto resolved = manifests_.resolve(sys.systemId, id)) candidates.append(*resolved);
    }
    for (const emu::SystemManifest& m : std::as_const(candidates)) {
      if (m.coreId.isEmpty()) {
        continue;
      }
      const emu::CoreLocation loc = locateCore(m);
      if (!loc.found()) {
        continue;
      }
      const bool known = std::any_of(coreList_.cbegin(), coreList_.cend(), [&](const CoreInfo& c) { return c.id == m.coreId; });
      if (known || listIds_.contains(m.coreId)) {
        continue;
      }
      listIds_.insert(m.coreId);
      probingIds_.insert(m.coreId);
      prober_.request(m.coreId, loc.path, systemDir(), QDir(profiles_->baseDir()).filePath(QStringLiteral("probe")));
    }
  }
}

bool CoreCatalog::probeCoreOptions(const emu::SystemManifest& man) {
  if (man.coreId.isEmpty() || phase_ == PlayPhase::Launching || probingIds_.contains(man.coreId)) {
    return false;
  }
  const emu::CoreLocation loc = locateCore(man);
  if (!loc.found()) {
    return false;
  }
  const QString key = libraryKeyOf(loc.path);
  if (attempted_.contains(key)) {
    return false;
  }
  attempted_.insert(key);
  probingIds_.insert(man.coreId);
  prober_.request(man.coreId, loc.path, systemDir(), QDir(profiles_->baseDir()).filePath(QStringLiteral("probe")));
  return true;
}

void CoreCatalog::applyProbe(const QString& coreId, const emu::CoreProbe& probe) {
  probingIds_.remove(coreId);
  if (probe.ok) {
    coreNames_.insert(coreId, probe.info.name);
    coreVersions_.insert(coreId, probe.info.version);
    emulation_->setCoreProbe(coreId, probe, true);
  } else {
    emulation_->loadCoreCache(coreId);
  }
  if (listIds_.remove(coreId)) {
    const bool known = std::any_of(coreList_.cbegin(), coreList_.cend(), [&](const CoreInfo& c) { return c.id == coreId; });
    if (!known) {
      CoreInfo ci;
      ci.id = coreId;
      if (probe.ok) {
        ci.version = probe.info.version;
      }
      coreList_.append(ci);
    }
  }
  emit coreProbed(coreId);
}

emu::CoreLocation CoreCatalog::locateCore(const emu::SystemManifest& man) const {
  QString version;
  hubOffersCore(man, &version);
  return locator_.locate(man, version);
}

bool CoreCatalog::hubOffersCore(const emu::SystemManifest& man, QString* version) const {
  if (conn_->state() != HubConnection::State::Connected || !conn_->hubHasFeature(QStringLiteral("cores_v1")) || !systems_->supported() ||
      CoreCache::currentPlatform().isEmpty()) {
    return false;
  }
  const auto sys = systems_->system(man.systemId);
  if (!sys) {
    return false;
  }
  QString v;
  if (const SystemCore* c = sys->core(man.coreId)) {
    v = c->version;
  } else if (sys->cores.isEmpty() && sys->preferredCoreId == man.coreId) {
    v = sys->corePackageVersion;  // Hub without cores_v2
  }
  if (v.isEmpty()) {
    return false;
  }
  if (version != nullptr) {
    *version = v;
  }
  return true;
}

bool CoreCatalog::coreUsable(const emu::SystemManifest& man, emu::CoreLocation* out) const {
  const emu::CoreLocation loc = locateCore(man);
  if (out != nullptr) {
    *out = loc;
  }
  if (!loc.found()) {
    return false;
  }
  // A cached core must match its package.json (size + SHA-256, memoized); the locator only checks the size.
  return loc.source != QLatin1String("cache") || !coreCache_->libraryPath(loc.cacheCoreId.isEmpty() ? man.coreId : loc.cacheCoreId, loc.version, CoreCache::currentPlatform()).isEmpty();
}

bool CoreCatalog::coreInstalledLocally(const QString& coreIdOrAlias) const {
  for (const emu::SystemManifest& sys : manifests_.all()) {
    if (const auto m = manifests_.resolve(sys.systemId, coreIdOrAlias); m && !m->coreId.isEmpty() && coreUsable(*m)) {
      return true;
    }
  }
  return false;
}

QString CoreCatalog::coreProblemText(const QString& reason) {
  if (reason == QLatin1String("incompatible")) return tr("Core incompatible");
  if (reason == QLatin1String("not_on_hub") || reason == QLatin1String("not_cached_on_hub")) return tr("Core missing");
  return tr("Core download failed");
}

void CoreCatalog::coreStatus(const emu::SystemManifest& man, QString* text, QString* tone, QString* hint) const {
  emu::CoreLocation loc;
  const bool usable = coreUsable(man, &loc);
  hint->clear();
  if (usable) {
    *text = QString();
    *tone = QStringLiteral("ok");
    return;
  }
  if (phase_ == PlayPhase::Core && launchGame_.system == man.systemId) {
    *text = tr("Loading core…");
    *tone = QStringLiteral("neutral");
    return;
  }
  const QString problem = coreProblems_.value(man.coreId);
  if (!problem.isEmpty()) {
    *text = coreProblemText(problem);
    // "Core missing" needs your judgment (amber); a failed download or an incompatible core stays an error.
    *tone = problem == QLatin1String("not_on_hub") || problem == QLatin1String("not_cached_on_hub") ? QStringLiteral("amber") : QStringLiteral("error");
    if (problem == QLatin1String("not_on_hub")) {
      *hint = tr("The server has no package of this core. Ask the server admin to select a core version on the Systems & Cores page.");
    } else if (problem == QLatin1String("not_cached_on_hub")) {
      *hint = tr("The server has not downloaded the core files yet. Ask the server admin to check the core source.");
    } else if (problem == QLatin1String("incompatible")) {
      *hint = tr("The server has this core only for other platforms than %1.").arg(CoreCache::currentPlatform());
    } else {
      *hint = tr("The core could not be downloaded or verified. Play to try again.");
    }
    return;
  }
  if (hubOffersCore(man)) {
    *text = tr("Core from server · download at start");
    *tone = QStringLiteral("neutral");
    return;
  }
  *text = tr("Core missing");
  *tone = QStringLiteral("amber");
  // The search paths are not shown here (a developer path wrapped mid-word in mono, P13).
  *hint = tr("This core is not installed on this device. Connect to a server that provides cores.");
}

// "Needs attention" (D14) for the Library: core missing/incompatible or firmware missing, per system once.
QString CoreCatalog::attentionFor(const GameEntry& game) const {
  const emu::SystemManifest* man = manifestFor(game);
  if (man == nullptr) {
    return {};
  }
  if (!coreUsable(*man)) {
    QString text, tone, hint;
    coreStatus(*man, &text, &tone, &hint);
    if (tone == QLatin1String("error") || tone == QLatin1String("amber")) {
      return text;
    }
  }
  SystemInfo sys;
  if (nativeFirmware(*man, &sys)) {
    QList<FirmwareProblem> problems = FirmwareProvisioner::missingOnHub(sys, wantedFirmwareIds(*man));
    for (const FirmwareProblem& p : fwProblems_) {
      if (p.reason == QLatin1String("invalid_hash") || p.reason == QLatin1String("invalid_size")) {
        problems.append(p);
      }
    }
    if (!problems.isEmpty()) {
      return tr("Firmware missing");
    }
  }
  return {};
}

QVariantList CoreCatalog::systemCards() {
  QVariantList cards;
  for (const emu::SystemManifest& sysManifest : manifests_.all()) {
    const emu::SystemManifest* mp = manifestForSystem(sysManifest.systemId);
    if (mp == nullptr) {
      continue;
    }
    const emu::SystemManifest& m = *mp;
    const emu::CoreLocation loc = locateCore(m);
    QString version = coreVersions_.value(m.coreId);
    if (version.isEmpty()) {
      if (const emu::CoreProbe* p = emulation_->coreProbe(m.coreId)) version = p->info.version;
    }
    QString readyText;
    QString readyTone;
    if (coreUsable(m)) {
      readyText = loc.source == QLatin1String("cache") ? tr("Ready · core from the server") : tr("Ready · included in Uzume");
      readyTone = QStringLiteral("ok");
    } else {
      QString hint;
      coreStatus(m, &readyText, &readyTone, &hint);
    }
    // Firmware (firmware path from the Hub registry, architecture 05): built-in BIOS, from the Hub, or required/missing.
    // Only systems whose core can use firmware files get a firmware line (3e-3, P30): GB, GBA and 3DS have none.
    QString fwText = m.firmware.sysfileOption.isEmpty() ? QString() : tr("Built-in BIOS");
    QString fwTone = QStringLiteral("neutral");
    SystemInfo sys;
    if (nativeFirmware(m, &sys)) {
      const QStringList wanted = wantedFirmwareIds(m);
      QList<FirmwareProblem> problems = FirmwareProvisioner::missingOnHub(sys, wanted);
      for (const FirmwareProblem& p : fwProblems_) {
        if (p.reason == QLatin1String("invalid_hash") || p.reason == QLatin1String("invalid_size")) problems.append(p);
      }
      if (!problems.isEmpty()) {
        fwText = tr("Firmware required/missing");
        fwTone = QStringLiteral("error");
      } else {
        fwText = tr("Firmware from server · verified");
        fwTone = QStringLiteral("ok");
      }
    }
    // The Hub preselects no core and the user has not chosen one (P31): say so instead of showing the fallback core as chosen.
    bool noCore = false;
    if (systems_->supported()) {
      if (const auto sysInfo = systems_->system(m.systemId)) {
        noCore = !sysInfo->defaultCore().valid() &&
                 !emulation_->settings()->hasValue(EmulationSettings::Level::System, m.systemId, QString::fromLatin1(EmulationSettings::kCoreKey));
      }
    }
    cards.append(QVariantMap{{QStringLiteral("id"), m.systemId},
                             {QStringLiteral("name"), m.displayName},
                             {QStringLiteral("coreName"), noCore ? tr("No core chosen yet") : coreLabel(m, loc)},
                             {QStringLiteral("fallbackCoreName"), coreLabel(m, loc)},
                             {QStringLiteral("noCoreChosen"), noCore},
                             {QStringLiteral("coreVersion"), noCore ? QString() : version},
                             {QStringLiteral("coreId"), m.coreId},
                             {QStringLiteral("coreExperimental"), m.experimental},
                             {QStringLiteral("cores"), coresOf(m.systemId)},
                             {QStringLiteral("defaultCoreId"), systemDefaultCore(m.systemId)},
                             {QStringLiteral("coreNotice"), coreNoticeFor(m.systemId)},
                             {QStringLiteral("readyText"), readyText},
                             {QStringLiteral("readyTone"), readyTone},
                             {QStringLiteral("firmwareText"), fwText},
                             {QStringLiteral("firmwareTone"), fwTone}});
  }
  return cards;
}

QString CoreCatalog::coreLabel(const emu::SystemManifest& m, const emu::CoreLocation&) const {
  QString label = coreNames_.value(m.coreId);
  if (label.isEmpty()) {
    label = m.coreDisplayName.isEmpty() ? m.coreId : m.coreDisplayName;
  }
  return m.experimental ? tr("%1 (Experimental)").arg(label) : label;
}

const emu::SystemManifest* CoreCatalog::manifestFor(const GameEntry& game) const {
  const QString ext = QStringLiteral(".") + RomCache::extensionFromFilename(game.romFilename);
  const emu::SystemManifest* base = manifests_.forExtension(ext);
  if (base == nullptr) {
    base = manifests_.find(game.system);
  }
  return base != nullptr ? manifestForSystem(base->systemId, game.id) : nullptr;
}

QString CoreCatalog::reportedCoreVersion(const QString& coreId) const {
  QString v = coreVersions_.value(coreId);
  if (v.isEmpty()) {
    if (const emu::CoreProbe* p = emulation_->coreProbe(coreId)) v = p->info.version;
  }
  return v;
}

QString CoreCatalog::systemDefaultCore(const QString& systemId) const {
  if (systems_->supported()) {
    if (const auto sys = systems_->system(systemId)) {
      const SystemCore d = sys->defaultCore();
      if (d.valid()) {
        return d.coreId;
      }
    }
  }
  const QList<emu::CoreProfile> profiles = manifests_.profilesForSystem(systemId);
  return profiles.isEmpty() ? QString() : profiles.first().coreId;
}

CoreChoice CoreCatalog::choiceFor(const QString& systemId, const QString& gameId) const {
  const auto canon = [this](const QString& id) { return manifests_.canonicalCoreId(id); };
  CoreChoice hubStale;
  if (systems_->supported()) {
    if (const auto sys = systems_->system(systemId)) {
      CoreChoice c = chooseCore(*sys, *emulation_->settings(), gameId, canon);
      if (c.valid()) {
        return c;
      }
      hubStale = c;  // the Hub serves no core for the system
    }
  }
  // No Hub data: the stored choice, else the first profiled core that is available on this device.
  CoreChoice out = hubStale;
  const QString key = QString::fromLatin1(EmulationSettings::kCoreKey);
  using L = EmulationSettings::Level;
  const EmulationSettings& st = *emulation_->settings();
  const auto fill = [&](const QString& id, const QString& source) {
    out.core = SystemCore{};
    out.core.coreId = id;
    out.core.displayName = manifests_.profile(id) != nullptr ? manifests_.profile(id)->displayName : id;
    out.source = source;
  };
  if (!gameId.isEmpty() && st.hasValue(L::Game, gameId, key)) {
    fill(st.value(L::Game, gameId, key), QStringLiteral("game"));
    return out;
  }
  if (st.hasValue(L::System, systemId, key)) {
    fill(st.value(L::System, systemId, key), QStringLiteral("system"));
    return out;
  }
  const QList<emu::CoreProfile> profiles = manifests_.profilesForSystem(systemId);
  for (const emu::CoreProfile& p : profiles) {
    if (const auto m = manifests_.resolve(systemId, p.coreId); m && locator_.locate(*m).found()) {
      fill(p.coreId, QStringLiteral("local"));
      return out;
    }
  }
  if (!profiles.isEmpty()) {
    fill(profiles.first().coreId, QStringLiteral("local"));
  } else {
    out.source = QStringLiteral("none");
  }
  return out;
}

const emu::SystemManifest* CoreCatalog::manifestForSystem(const QString& systemId, const QString& gameId) const {
  const CoreChoice choice = choiceFor(systemId, gameId);
  if (!choice.valid()) {
    return manifests_.find(systemId);
  }
  const QString key = systemId + QLatin1Char('\n') + choice.core.coreId;
  auto it = effective_.find(key);
  if (it == effective_.end()) {
    const auto m = manifests_.resolve(systemId, choice.core.coreId);
    if (!m) {
      return manifests_.find(systemId);
    }
    it = effective_.emplace(key, *m).first;
  }
  return &it->second;
}

QVariantList CoreCatalog::coresOf(const QString& systemId) const {
  QVariantList out;
  QList<SystemCore> cores;
  QString def;
  if (systems_->supported()) {
    if (const auto sys = systems_->system(systemId)) {
      cores = sys->cores;
      def = sys->defaultCore().coreId;
      if (cores.isEmpty() && sys->defaultCore().valid()) {
        cores.append(sys->defaultCore());
      }
    }
  }
  if (cores.isEmpty()) {
    for (const emu::CoreProfile& p : manifests_.profilesForSystem(systemId)) {
      SystemCore c;
      c.coreId = p.coreId;
      c.displayName = p.displayName;
      cores.append(c);
    }
    def = cores.isEmpty() ? QString() : cores.first().coreId;
  }
  for (const SystemCore& c : std::as_const(cores)) {
    const bool experimental = manifests_.profile(c.coreId) == nullptr;
    QStringList parts{c.displayName.isEmpty() ? c.coreId : c.displayName};
    if (!c.license.isEmpty()) {
      parts.append(c.license);
    }
    if (!c.buildDate.isEmpty()) {
      parts.append(tr("build %1").arg(c.buildDate));
    }
    if (experimental) {
      parts.append(tr("Experimental"));
    }
    out.append(QVariantMap{{QStringLiteral("id"), c.coreId},
                           {QStringLiteral("name"), c.displayName.isEmpty() ? c.coreId : c.displayName},
                           {QStringLiteral("version"), c.version},
                           {QStringLiteral("license"), c.license},
                           {QStringLiteral("buildDate"), c.buildDate},
                           {QStringLiteral("experimental"), experimental},
                           {QStringLiteral("requiredHwApi"), c.requiredHwApi},
                           {QStringLiteral("isDefault"), c.coreId == def},
                           {QStringLiteral("label"), parts.join(QStringLiteral(" · "))}});
  }
  return out;
}

QString CoreCatalog::coreNoticeFor(const QString& systemId, const QString& gameId) const {
  const CoreChoice c = choiceFor(systemId, gameId);
  if (c.staleChoice.isEmpty() || !c.valid()) {
    return {};
  }
  return tr("The core \"%1\" chosen for this %2 is not served by the server any more. %3 is used instead.")
      .arg(c.staleChoice, c.staleLevel == QLatin1String("game") ? tr("game") : tr("system"),
           c.core.displayName.isEmpty() ? c.core.coreId : c.core.displayName);
}

QStringList CoreCatalog::wantedFirmwareIds(const emu::SystemManifest& man) const {
  QStringList ids;
  for (const emu::FirmwareFile& f : man.firmware.files) {
    ids.append(f.id);
  }
  return ids;
}

bool CoreCatalog::nativeFirmware(const emu::SystemManifest& man, SystemInfo* system) const {
  // Without firmware_v1 (older hub) or without registry data the core runs on its built-in firmware.
  if (!systems_->supported() || man.firmware.sysfileOption.isEmpty()) {
    return false;
  }
  const auto info = systems_->system(man.systemId);
  if (!info || !info->nativeFirmware()) {
    return false;
  }
  if (system != nullptr) {
    *system = *info;
  }
  return true;
}

QString CoreCatalog::systemDir() const {
  return systemDirIn(profiles_->baseDir());
}

}  // namespace uzume::ui
