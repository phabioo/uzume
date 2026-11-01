#pragma once
// CoreCatalog: system manifests, core lookup/probing and the core / firmware status of a system (Library "needs
// attention", detail pane, Emulation page cards). Read-mostly helper of PlayerController; the shared state
// (manifests, probe results, provisioning problems) stays owned by PlayerController and is referenced here.

#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <map>

#include "core_locator.h"
#include "corechoice.h"
#include "coreprober.h"
#include "corecache.h"
#include "coreprovisioner.h"
#include "emulationcontroller.h"
#include "firmwarecache.h"
#include "firmwareprovisioner.h"
#include "gamesession.h"
#include "hubconnection.h"
#include "hublibrary.h"
#include "hubsystems.h"
#include "librarymodel.h"
#include "playphase.h"
#include "romcache.h"
#include "romdownloader.h"
#include "savesync.h"
#include "system_manifest.h"

namespace uzume::ui {

class CoreCatalog : public QObject {
  Q_OBJECT

 public:
  struct Deps {
    HubConnection* conn;
    ProfileStore* profiles;
    HubSystems* systems;
    CoreCache* coreCache;
    EmulationController* emulation;
    emu::ManifestRegistry& manifests;
    emu::CoreLocator& locator;
    QList<CoreInfo>& coreList;
    QMap<QString, QString>& coreVersions;  // core_id -> version from the core info
    QMap<QString, QString>& coreNames;     // core_id -> name from the core info (only if probed)
    QMap<QString, QString>& coreProblems;  // core_id -> last provisioning problem reason
    const QList<FirmwareProblem>& fwProblems;
    const PlayPhase& phase;
    const GameEntry& launchGame;
  };

  explicit CoreCatalog(const Deps& d, QObject* parent = nullptr);

 signals:
  void coreProbed(const QString& coreId);  // one core's probe result was applied
  void coresProbed();                       // the queue ran empty (handshake core list is up to date)

 public:

  // `<base>/system` (cores' system directory and firmware cache); usable before a CoreCatalog exists.
  static QString systemDirIn(const QString& baseDir);

  // Asynchronous (CoreProber worker thread): requests a probe of every located core that is not in the core list yet.
  // Results are applied on the GUI thread (core list, names, versions, EmulationController probe); `coresProbed` follows
  // when the last queued probe is done. Not while a game is being started (only one core can be loaded per process).
  void probeCores();
  // Probes one core for the Emulation page options (not twice concurrently, not again after an attempt on the same
  // library); false = nothing queued. On failure the options cache is loaded instead. `coreProbed` follows each result.
  bool probeCoreOptions(const emu::SystemManifest& man);
  bool probeBusy() const { return prober_.busy(); }
  bool probing(const QString& coreId) const { return probingIds_.contains(coreId); }
  CoreProber& prober() { return prober_; }
  // Effective core of a system (and game): game > system > Hub default, among the cores the Hub serves (ADR 0020 D6).
  // Without Hub data (offline): the stored choice, else the first profiled core that is available locally.
  CoreChoice choiceFor(const QString& systemId, const QString& gameId = QString()) const;
  // Effective manifest (system + core profile; experimental without profile) of a system / game. Pointers stay valid.
  const emu::SystemManifest* manifestForSystem(const QString& systemId, const QString& gameId = QString()) const;
  // Cores of a system for the Emulation page: [{id, name, version, license, buildDate, experimental, requiredHwApi,
  // isDefault, label}]; the Hub's list, else the local profiles.
  QVariantList coresOf(const QString& systemId) const;
  QString reportedCoreVersion(const QString& coreId) const;  // version from the core info (probe); empty = unknown
  QString systemDefaultCore(const QString& systemId) const;  // Hub default core id, else the first profile
  // "A stored choice is not served any more" notice for the effective core of this system / game; empty = none.
  QString coreNoticeFor(const QString& systemId, const QString& gameId = QString()) const;
  // Core lookup with the version the Hub serves (cache source), see CoreLocator.
  emu::CoreLocation locateCore(const emu::SystemManifest& man) const;
  // The Hub serves this core for the system (cores_v1: SystemCore version, or the legacy preferred core).
  bool hubOffersCore(const emu::SystemManifest& man, QString* version = nullptr) const;
  // Located core is usable as is (a cached core must also pass the SHA-256 check).
  bool coreUsable(const emu::SystemManifest& man, emu::CoreLocation* loc = nullptr) const;
  // True when the core (id or legacy alias) is located and usable on this device for any system (Hub warning filter).
  bool coreInstalledLocally(const QString& coreIdOrAlias) const;
  // Core status text/tone/hint for the detail pane and the system cards.
  void coreStatus(const emu::SystemManifest& man, QString* text, QString* tone, QString* hint) const;
  static QString coreProblemText(const QString& reason);
  QString attentionFor(const GameEntry& game) const;
  QVariantList systemCards();
  // Core name for the UI; cores without profile carry the "Experimental" marker.
  QString coreLabel(const emu::SystemManifest& m, const emu::CoreLocation& loc) const;
  // Effective manifest of the game's system for the game's effective core.
  const emu::SystemManifest* manifestFor(const GameEntry& game) const;
  QStringList wantedFirmwareIds(const emu::SystemManifest& man) const;
  // Firmware mode of the Hub for the system of this manifest: true = native (files required).
  bool nativeFirmware(const emu::SystemManifest& man, SystemInfo* system = nullptr) const;
  QString systemDir() const;

 private:
  void applyProbe(const QString& coreId, const emu::CoreProbe& probe);

  HubConnection* conn_;
  ProfileStore* profiles_;
  HubSystems* systems_;
  CoreCache* coreCache_;
  EmulationController* emulation_;
  emu::ManifestRegistry& manifests_;
  emu::CoreLocator& locator_;
  QList<CoreInfo>& coreList_;
  QMap<QString, QString>& coreVersions_;
  QMap<QString, QString>& coreNames_;
  QMap<QString, QString>& coreProblems_;
  const QList<FirmwareProblem>& fwProblems_;
  const PlayPhase& phase_;
  const GameEntry& launchGame_;
  CoreProber prober_;
  QSet<QString> probingIds_;  // core ids with a queued / running probe
  QSet<QString> attempted_;   // library keys (path|size|mtime) probed this run (a failed probe is not repeated)
  QMap<QString, QString> listPackageVersions_;  // core_id -> cache package version, for cores located in the download cache
  QSet<QString> listIds_;     // core ids requested by probeCores(): get a core list entry when their result arrives
  mutable std::map<QString, emu::SystemManifest> effective_;
};

}  // namespace uzume::ui
