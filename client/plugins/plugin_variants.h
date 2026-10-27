#pragma once

// Seam between the plugin runtime and the app for fb.variants (permission variants.build, ADR 0031 D5). The plugin Randomizer only
// declares what it offers (engine, presets, supported games) and asks Uzume to build a variant from a recipe; Uzume runs the engine.
// Plugin threads call the provider: every method is thread-safe and non-blocking.
#include <QList>
#include <QMap>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

namespace uzume::plugins {

struct VariantPreset {
  QString id;
  QString label;
  QString hint;
  QMap<QString, QString> settings;  // settings version ("428") -> settings string
  // The string of the highest settings version: the engine upgrades older strings itself, never the other way round.
  QString bestSettings() const {
    QString best;
    int bestV = -1;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
      const int v = it.key().toInt();
      if (v > bestV) {
        bestV = v;
        best = it.value();
      }
    }
    return best;
  }
};

struct VariantGameSupport {
  QString system;      // "nds", "3ds", ...
  QStringList codes;   // game codes as fb.game.code carries them (NDS/GBA 4 characters, 3DS 16 hex digits, GB title)
};

struct VariantRegistration {
  QString pluginId;
  QString engine;  // the tool id, "randomizer"
  QList<VariantPreset> presets;
  QList<VariantGameSupport> games;
};

// What the enabled plugins registered through fb.variants.register. Read from the GUI thread, written from the plugin thread.
class VariantRegistry {
 public:
  void set(const VariantRegistration& reg);
  void remove(const QString& pluginId);  // plugin disabled, unloaded or reloaded
  void clear();
  void keepOnly(const QSet<QString>& pluginIds);  // drops the registrations of every other plugin (disabled, removed, with problems)
  QList<VariantRegistration> all() const;
  bool hasEngine(const QString& engine) const;
  // A plugin registered this game (system + code, code compared ignoring case; an empty code list matches the whole system).
  bool supported(const QString& system, const QString& code) const;
  // Presets of every plugin that registered a game match for `system` + `code` ("custom" is added by Uzume, not here).
  QList<VariantPreset> presetsFor(const QString& system, const QString& code) const;
  // The preset with this id of any registration (any plugin) and the settings string for a settings version ("" = first one).
  VariantPreset preset(const QString& id) const;
  // Called (on the thread that changed the registry, without the lock) after every change; the UI turns it into a signal.
  void setOnChange(std::function<void()> cb);

 private:
  void changed();
  mutable QMutex mutex_;
  QMap<QString, VariantRegistration> byPlugin_;
  std::function<void()> onChange_;
};

struct VariantEngineInfo {
  bool offered = false;   // the server offers the engine
  QString version;        // the version it offers
  bool onPc = false;      // already downloaded to this PC
};

struct VariantRow {
  QString id;
  QString name;
  QString slot;
  QString presetId;
  QString state;       // not_built | building | ready | needs_rebuild | failed
  QString toolVersion;
};

struct VariantBuildRequest {
  QString gameId;      // empty: the game of the plugin's context
  QString name;
  QString presetId;    // a registered preset, or empty with `settings`
  QString settings;
  QString seed;        // decimal integer text, empty = random
  bool spoilerLog = false;
};

class VariantsProvider {
 public:
  virtual ~VariantsProvider() = default;
  virtual std::shared_ptr<VariantRegistry> registry() = 0;
  virtual VariantEngineInfo engine() const = 0;
  virtual QList<VariantRow> list(const QString& gameId) const = 0;
  // Starts a build; returns a request id at once. Progress and the result arrive as the host event "variants.state"
  // {id, state, progress, error} to `pluginId`. Any thread.
  virtual quint64 build(const QString& pluginId, const VariantBuildRequest& req) = 0;
};

}  // namespace uzume::plugins
