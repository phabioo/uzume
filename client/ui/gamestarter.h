#pragma once
// GameStarter: the start pipeline of a game: core provisioning, firmware, ROM, start sync with the Hub (incl. the
// save-conflict dialog data) and launching the GameSession. The start state (phase, launch game, errors, ...) stays
// owned by PlayerController, which the QML detail pane reads; it is referenced here.

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "core_locator.h"
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
#include "romsource.h"
#include "savesync.h"
#include "system_manifest.h"
#include "variantlaunch.h"
#include "corecatalog.h"

namespace uzume::ui {

class GameStarter : public QObject {
  Q_OBJECT

 public:
  struct Deps {
    HubConnection* conn;
    ProfileStore* profiles;
    SaveSync* saves;
    CoreProvisioner* coreProv;
    FirmwareProvisioner* provisioner;
    RomDownloader* downloader;
    HubLibrary* library;
    EmulationController* emulation;
    CoreCatalog* catalog;
    LibraryModel& model;
    GameSession& session;
    const QString& selectedId;
    PlayPhase& phase;
    GameEntry& launchGame;
    QString& launchRom;
    bool& saveReady;
    QString& saveNoteStart;
    QString& startError;
    QString& pendingSha;
    QMap<QString, QString>& fwOptions;
    bool& shareOnStart;
    QList<FirmwareProblem>& fwProblems;
    QMap<QString, QString>& coreProblems;
    QList<CoreInfo>& coreList;
    HandshakeInfo& handshake;
    QVariantMap& conflict;
    bool probeCoreVersions;
    std::function<RomSourceChoice(const GameEntry&)> romSource;  // library folder / cache / download / missing
    // Variant start (0.21): active = start exactly this variant (its ROM / mod folder); inactive = a normal start, titleId is
    // still filled for 3DS so that no stale mod stays in the slot. Empty function = no variants.
    std::function<VariantLaunch(const GameEntry&)> variantLaunch;
  };

  explicit GameStarter(const Deps& d, QObject* parent = nullptr);

  void startSelected(bool share);
  void onCoreFinished(const CoreResult& result);
  void onFirmwareFinished(const FirmwareResult& result);
  void onRomReady(const QString& sha, const QString& path);
  void onSaveReady(const QString& gameId, const QString& saveDir, const QString& note);
  void onSaveConflict(const SaveSync::ConflictView& view);
  void resolveSaveConflict(const QString& action);

 signals:
  void selectedGameChanged();
  void saveConflictChanged();
  void coreStateRefreshRequested();
  // The game is about to start on this Player (after the start sync): "last played" is recorded.
  void gameLaunched(const QString& gameId);
  // The system about to start: input profile and the short labels of its input / touch columns (touch label empty = the system has no touch screen).
  void systemInputSelected(const QString& inputProfile, const QString& inputLabel, const QString& touchLabel);

 private:
  SaveSync::CoreRef coreRefFor(const emu::SystemManifest& man) const;
  void continueStartAfterCore(const GameEntry& game, const emu::SystemManifest& man);
  void beginRomPhase(const GameEntry& game);
  void launch(const GameEntry& game, const QString& romPath);
  void refreshCoreState() { emit coreStateRefreshRequested(); }
  static QString formatWhen(const QDateTime& when);

  HubConnection* conn_;
  ProfileStore* profiles_;
  SaveSync* saves_;
  CoreProvisioner* coreProv_;
  FirmwareProvisioner* provisioner_;
  RomDownloader* downloader_;
  HubLibrary* library_;
  EmulationController* emulation_;
  CoreCatalog* catalog_;
  LibraryModel& model_;
  GameSession& session_;
  const QString& selectedId_;
  PlayPhase& phase_;
  GameEntry& launchGame_;
  QString& launchRom_;
  bool& saveReady_;
  QString& saveNoteStart_;
  QString& startError_;
  QString& pendingSha_;
  QMap<QString, QString>& fwOptions_;
  bool& shareOnStart_;
  QList<FirmwareProblem>& fwProblems_;
  QMap<QString, QString>& coreProblems_;
  QList<CoreInfo>& coreList_;
  HandshakeInfo& handshake_;
  QVariantMap& conflict_;
  bool probeCoreVersions_;
  std::function<RomSourceChoice(const GameEntry&)> romSource_;
  std::function<VariantLaunch(const GameEntry&)> variantLaunch_;
  VariantLaunch activeVariant_;  // set in beginRomPhase, read in onSaveReady
};

}  // namespace uzume::ui
