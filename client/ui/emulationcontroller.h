#pragma once
// EmulationController: data and actions of the Emulation page (3e). Local settings only
// (<data>/settings/emulation.json): partial overrides in the hierarchy Global -> System/Core -> Game Override.
// Core options are listed exactly as the core reports them (core_options.h); Uzume's own options come
// from a small fixed list ("uzume.*"). Options that Uzume controls (manifest-locked, firmware) are not offered.
// Changes are applied at the next launch.

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <optional>

#include "controllerprofiles.h"
#include "core_options.h"
#include "emulationsettings.h"
#include "system_manifest.h"

namespace uzume::ui {

// Rows of the per-game picker ({pickValue: game id, pickLabel: "Title · System"}). A model instead of a QVariantList so QML
// never converts thousands of maps into JS objects (that is what made big libraries crawl); delegates are created lazily.
class GamePickerModel : public QAbstractListModel {
  Q_OBJECT
 public:
  enum Role { ValueRole = Qt::UserRole + 1, LabelRole };
  struct Row {
    QString value, label;
    bool operator==(const Row& o) const { return value == o.value && label == o.label; }
  };
  using QAbstractListModel::QAbstractListModel;
  int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }
  QVariant data(const QModelIndex& index, int role) const override {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
    const Row& r = rows_.at(index.row());
    if (role == ValueRole) return r.value;
    if (role == LabelRole || role == Qt::DisplayRole) return r.label;
    return {};
  }
  QHash<int, QByteArray> roleNames() const override { return {{ValueRole, "pickValue"}, {LabelRole, "pickLabel"}}; }
  void setRows(const QList<Row>& rows) {
    if (rows == rows_) return;  // only ids/labels count: a changed-count update leaves the picker alone
    beginResetModel();
    rows_ = rows;
    endResetModel();
  }

 private:
  QList<Row> rows_;
};

class EmulationController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  // [{id, name, coreName, coreVersion, coreId, coreExperimental, cores:[{id,name,version,license,buildDate,experimental,
  //   isDefault,label}], defaultCoreId, coreNotice, readyText, readyTone, firmwareText, firmwareTone}]
  Q_PROPERTY(QVariantList systems READ systems NOTIFY systemsChanged)
  Q_PROPERTY(QString selectedSystem READ selectedSystem NOTIFY selectionChanged)
  Q_PROPERTY(QVariantMap system READ system NOTIFY selectionChanged)  // card of the selected system, empty if none
  Q_PROPERTY(QString level READ level WRITE setLevel NOTIFY levelChanged)  // "global" | "system" | "game"
  // Per-game overrides: library games [{id, title, systemId, systemName, label, changedCount}], the picked game and its card
  // (empty if none), number of games that carry overrides.
  Q_PROPERTY(QVariantList games READ games NOTIFY gamesChanged)
  Q_PROPERTY(QString selectedGame READ selectedGame NOTIFY gameSelectionChanged)
  Q_PROPERTY(QVariantMap game READ game NOTIFY gameSelectionChanged)
  Q_PROPERTY(bool gameSelected READ gameSelected NOTIFY levelChanged)
  Q_PROPERTY(int gameOverrideCount READ gameOverrideCount NOTIFY gamesChanged)
  Q_PROPERTY(QAbstractItemModel* gamePicker READ gamePicker CONSTANT)  // rows for the game picker (UzModelSelect)
  Q_PROPERTY(int gameCount READ gameCount NOTIFY gamesChanged)  // cheap stand-in for games.length in bindings
  // [{id, title, subtitle, note, options:[{key, label, description, category, values:[{value,label}], value,
  //   valueLabel, isSet, origin, restart}]}]
  Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)
  // Page state of 3e: "Defaults" is level "global" (defaultsSelected), a system is level "system".
  Q_PROPERTY(bool defaultsSelected READ defaultsSelected NOTIFY levelChanged)
  Q_PROPERTY(int defaultsChangedCount READ defaultsChangedCount NOTIFY systemsChanged)
  // Category chips (names present in the current scope), filter ("" = All) and search text; both narrow `groups`.
  Q_PROPERTY(QStringList categories READ categories NOTIFY groupsChanged)
  Q_PROPERTY(QString categoryFilter READ categoryFilter WRITE setCategoryFilter NOTIFY filterChanged)
  Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY filterChanged)
  Q_PROPERTY(int changedCount READ changedCount NOTIFY groupsChanged)    // options of the current scope that differ from their default
  Q_PROPERTY(bool restartHint READ restartHint NOTIFY groupsChanged)    // a changed option applies only after the next game start
  Q_PROPERTY(bool gameRunning READ gameRunning NOTIFY gameRunningChanged)
  Q_PROPERTY(bool coreProbing READ coreProbing NOTIFY coreProbingChanged)  // a core is being read for its options (worker thread)
  Q_PROPERTY(int lockedCount READ lockedCount NOTIFY groupsChanged)  // options of the core that Uzume controls
  Q_PROPERTY(QString coreNote READ coreNote NOTIFY groupsChanged)    // why the core group is empty, else empty

 public:
  static constexpr const char* kCoreKey = EmulationSettings::kCoreKey;  // "uzume.core": core of a system (ADR 0020 D6)
  static constexpr const char* kFullscreenKey = "uzume.fullscreen_on_start";
  static constexpr const char* kMultiviewKey = "uzume.default_multiview";
  static constexpr const char* kScreenLayoutKey = "uzume.screen_layout";  // default DS/3DS screen layout (3e-3)
  static constexpr const char* kScreenSwapKey = "uzume.screen_swap";      // off | on: bottom screen where the top one would be
  static constexpr const char* kBottomWindowKey = "uzume.bottom_window";  // off | on: bottom screen in its own window at game start
  static constexpr const char* kIntegerScaleKey = "uzume.integer_scale";  // 0.19 picture: "off" | "on"
  static constexpr const char* kAspectRatioKey = "uzume.aspect_ratio";    // 0.19 picture: "core" | "4:3" | "stretch"
  static constexpr const char* kPadProfileKey = "uzume.pad_profile";            // 0.19: gamepad profile for P1 ("" = the device's own)
  static constexpr const char* kKeyboardProfileKey = "uzume.keyboard_profile";  // 0.19: keyboard profile ("" = the device's own)
  static constexpr const char* kRendererKey = EmulationSettings::kRendererKey;  // "uzume.renderer": opengl | vulkan (ADR 0034)
  static constexpr const char* kSpeedUpRatioKey = "uzume.speedup_ratio";
  static constexpr const char* kSpeedUpOnStartKey = "uzume.speedup_on_start";
  static constexpr const char* kSpeedUpAudioKey = "uzume.speedup_audio";
  static constexpr const char* kRunAheadKey = "uzume.run_ahead";            // 0.20: extra frames emulated ahead (0 | 1 | 2)
  static constexpr const char* kRewindKey = "uzume.rewind_seconds";         // 0.20: rewind length in seconds (0 = off)

  EmulationController(const QString& dataDir, const emu::ManifestRegistry* manifests, QObject* parent = nullptr);

  EmulationSettings* settings() { return &settings_; }
  QVariantList systems() const;  // cards plus "changedCount" per system
  QString selectedSystem() const { return selected_; }
  QVariantMap system() const;
  QString level() const { return level_; }
  void setLevel(const QString& level);
  QVariantList groups() const { return groups_; }
  bool defaultsSelected() const { return level_ == QLatin1String("global"); }
  bool gameSelected() const { return level_ == QLatin1String("game"); }  // the page edits per-game overrides
  QVariantList games() const;
  QString selectedGame() const { return selectedGame_; }
  QVariantMap game() const;
  int gameOverrideCount() const;
  int gameCount() const;
  QAbstractItemModel* gamePicker() { return &gamePicker_; }
  // Filled by the PlayerController from the library: [{id, title, systemId}].
  void setGames(const QVariantList& games);
  Q_INVOKABLE void selectGame(const QString& gameId);
  int defaultsChangedCount() const;
  QStringList categories() const { return categories_; }
  QString categoryFilter() const { return categoryFilter_; }
  void setCategoryFilter(const QString& category);
  QString searchText() const { return searchText_; }
  void setSearchText(const QString& text);
  int changedCount() const { return changedCount_; }
  bool restartHint() const { return restartHint_; }
  bool gameRunning() const { return gameRunning_; }
  bool coreProbing() const { return coreProbing_; }
  void setCoreProbing(bool probing);
  int lockedCount() const { return lockedCount_; }
  QString coreNote() const { return coreNote_; }

  // Filled by the PlayerController (it knows cores, versions and the firmware state).
  void setSystems(const QVariantList& cards);
  void setGameRunning(bool running);
  // Core options of a core (probe or cache). Writes the cache file for a successful probe.
  void setCoreProbe(const QString& coreId, const emu::CoreProbe& probe, bool writeCache);
  bool hasCoreProbe(const QString& coreId) const;
  const emu::CoreProbe* coreProbe(const QString& coreId) const;
  void loadCoreCache(const QString& coreId);  // when no probe is possible

  // Effective value of an Uzume option for a system (and game): game > system > global > default.
  QString uzumeValue(const QString& key, const QString& systemId, const QString& gameId = QString()) const;
  // Explicit user values for the launch (merged game > system > global).
  QMap<QString, QString> launchOverrides(const QString& systemId, const QString& gameId) const;
  // The core the game starts with (its own choice when served, else the system's core).
  QString launchCoreId(const QString& systemId, const QString& gameId) const { return coreIdFor(systemId, gameId); }

  // Controller profiles offered by the "uzume.pad_profile" / "uzume.keyboard_profile" rows (owned by the ControllersController).
  void setControllerProfiles(const ControllerProfiles* profiles);
  void profilesChanged() { rebuild(); }  // profiles were added, renamed or deleted
  // Game that is running right now ("" = none): its restart-only rows carry the "applies on next start" mark.
  void setRunningGame(const QString& gameId);

  // Per-game settings (game page and in-game menu, board 3c-3). Independent of the Emulation page's level and selection:
  //   {gameId, title, systemId, systemLabel, coreName, coreKnown, changedCount, restartHint, note,
  //    tabs:[{id, title, changed, note, rows:[{key, label, description, category, values:[{value,label}], value, valueLabel,
  //            isSet, defaultValue, defaultLabel, restart}]}]}
  // Empty for an unknown game. A row that is not set shows the value the game inherits (system > global > default).
  Q_INVOKABLE QVariantMap gameSettings(const QString& gameId) const;
  Q_INVOKABLE int gameChangedCount(const QString& gameId) const;
  Q_INVOKABLE void setGameOption(const QString& gameId, const QString& key, const QString& value);
  Q_INVOKABLE void resetGameOption(const QString& gameId, const QString& key);
  Q_INVOKABLE void resetGameOptions(const QString& gameId);
  Q_INVOKABLE void ensureGameCore(const QString& gameId);  // asks for the core options of the game's core (probe or cache)

  Q_INVOKABLE void selectSystem(const QString& id);
  Q_INVOKABLE void setOption(const QString& key, const QString& value);
  Q_INVOKABLE void resetOption(const QString& key);
  // The rewind length options with the real memory and seconds for a system: lengths the buffer cap cannot hold are left out, the longest
  // that fits is offered instead (kRewindMaxBytes); "High memory" only above half a GB. Systems with an unknown state size keep the list.
  static QList<emu::CoreOptionValue> rewindValues(QList<emu::CoreOptionValue> vals, const QString& systemId);
  QString setUzumeOption(const QString& key, const QString& value, const QString& systemId = QString(), const QString& gameId = QString());
  Q_INVOKABLE void resetAllChanged();  // "Reset N changed": every explicit value of the current scope

 signals:
  void systemsChanged();
  void selectionChanged();
  void levelChanged();
  void groupsChanged();
  void filterChanged();
  void gameRunningChanged();
  void coreProbingChanged();
  void gamesChanged();
  void gameSelectionChanged();  // the PlayerController makes sure the game's core options are known
  void uzumeOptionsChanged();  // a "uzume.*" value changed (the PlayerController applies it)
  void gameSettingsChanged(const QString& gameId);  // per-game rows changed (empty id: any game, e.g. core options arrived)
  void gameCoreWanted(const QString& gameId);       // the PlayerController makes sure the game's core options are known
  void coreChoiceChanged();        // the core of a system was chosen or reset (the PlayerController refreshes the core state)

 private:
  struct FbOption {
    QString key;
    QString label;
    QString description;
    QList<emu::CoreOptionValue> values;
    QString defaultValue;
    bool perSystem = false;  // also offered (and stored) per system, not only globally
    QString category;        // filter chip / category of the row; empty = by perSystem ("Speed-up" / "Display")
  };
  static const QList<FbOption>& uzumeOptions();
  // Effective manifest of the selected system for its effective core (card field "coreId"); the system-only manifest
  // when the card names no core.
  std::optional<emu::SystemManifest> selectedManifest() const;
  QString cachePath(const QString& coreId) const;
  void rebuild();
  QVariantMap row(const QString& key, const QString& label, const QString& description, const QString& category,
                  const QList<emu::CoreOptionValue>& values, const QString& manifestDefault, const QString& coreDefault,
                  bool uzume) const;
  bool knownOption(const QString& key, QList<emu::CoreOptionValue>* values) const;
  EmulationSettings::Level levelEnum() const;
  QString defaultValueOf(const QString& key) const;  // default the current scope falls back to (without its own value)
  QString scope() const;
  // Core of the edited game: its own choice when the system offers it, else the system's effective core.
  QString effectiveCoreId() const;
  void ensureGames() const;
  void emitGamesChanged();
  QVariantMap cardOf(const QString& systemId) const;
  QString coreIdFor(const QString& systemId, const QString& gameId) const;  // game's own choice when served, else the system's core
  std::optional<emu::SystemManifest> manifestFor(const QString& systemId, const QString& coreId) const;
  QList<emu::CoreOptionValue> profileValues(const QString& key, const QString& stored) const;
  void afterGameChange(const QString& gameId, const QString& key);

  QString dataDir_;
  const emu::ManifestRegistry* manifests_;
  const ControllerProfiles* profiles_ = nullptr;
  QString runningGame_;
  EmulationSettings settings_;
  QVariantList systems_;
  QString selected_;
  QString selectedGame_;
  QVariantList games_;
  mutable QVariantList gamesCache_;  // decorated games_, see ensureGames()
  mutable QHash<QString, int> gameIndex_;
  mutable int gameOverrides_ = 0;
  mutable bool gamesDirty_ = true;
  GamePickerModel gamePicker_;
  QString level_ = QStringLiteral("system");
  QVariantList groups_;
  QHash<QString, emu::CoreProbe> probes_;  // core id -> options
  bool gameRunning_ = false;
  bool coreProbing_ = false;
  QStringList categories_;
  QString categoryFilter_;
  QString searchText_;
  int changedCount_ = 0;
  bool restartHint_ = false;
  bool restartTouched_ = false;  // an option that applies on the next start was changed while a game runs
  int lockedCount_ = 0;
  QString coreNote_;
};

}  // namespace uzume::ui
