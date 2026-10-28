#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>
#include <utility>

#include "pluginhub.h"
#include "semver.h"

// Plugin manifest (manifest.json, docs/architecture/12-plugins.md 16.6). Pure parsing/validation, no Lua.
namespace uzume::plugins {

inline constexpr int kPluginApiVersion = 1;

// Version range of a `requires` entry: "^1.2", "~1.2", ">=1.0", "1.2.3", "*". Partial versions are padded with zeros.
// ^ : >= v and below the next change of the left-most non-zero part (^1.2 -> <2.0.0, ^0.2 -> <0.3.0).
// ~ : >= v and below the next minor (~1.2 -> <1.3.0; ~1 -> <2.0.0).
struct VersionRange {
  enum class Kind { Any, Exact, Bounded };
  Kind kind = Kind::Any;
  update::SemVer lo;                  // inclusive (Exact: the version, Bounded: lower bound)
  std::optional<update::SemVer> hi;   // exclusive (Bounded only)
  QString text = QStringLiteral("*");

  static std::optional<VersionRange> parse(const QString& text);
  bool matches(const update::SemVer& v) const;
};

struct GamePattern {
  QString system;      // case-insensitive, e.g. "nds"
  QStringList codes;   // globs with * and ?; empty = every game of the system
  bool matches(const QString& sys, const QString& code) const;
};

// Case-insensitive glob (* and ?) over a whole string.
bool globMatch(const QString& pattern, const QString& text);

struct Dependency {
  QString id;
  VersionRange range;
};

// Manifest field `ui` (plugin UI v1): which surfaces the plugin adds. Needs the permission `ui`.
struct UiHotkey {
  QString id;     // [a-z0-9-]{1,32}, unique
  QString label;  // 1..60 chars
  QString key;    // optional portable QKeySequence text (single combo)
};
struct UiDecl {
  bool panel = false;
  QString panelTitle;
  bool hud = false;
  bool tile = false;
  bool screen = false;  // slot "screen": the plugin's run tab (run view) and its screen sheet (Settings > Plugins, in game)
  QString screenTitle;
  bool detail = false;
  bool run = false;  // slot "run": the run detail view (Runs screen, outside a game)
  QString runTitle;
  QList<UiHotkey> hotkeys;

  bool any() const { return panel || hud || tile || screen || detail || run || !hotkeys.isEmpty(); }
  // "panel", "hud", "screen", "detail", "run" or "tile:<tileId>" (needs `tile`).
  bool declaresSlot(const QString& slot) const;
  // "Adds: panel tab · HUD · tile strip · game detail · run tab · n hotkeys" (run tab = ui.screen or ui.run); empty when nothing.
  QString addsText() const;
};

bool validUiId(const QString& s);  // [A-Za-z0-9_.-]{1,64}

// Manifest field `recap` (0.22): maps a run event type of this plugin to a glyph of the run recap.
inline constexpr int kMaxRecapEntries = 32;
struct RecapEntry {
  QString type;   // run event type, ^[a-z0-9_.-]{1,64}$
  QString glyph;  // catch | lost | badge | note | screenshot
};
bool validRecapGlyph(const QString& glyph);

struct Manifest {
  QString id;
  QString name;
  update::SemVer version;
  int api = kPluginApiVersion;
  QString type = QStringLiteral("script");
  QList<Dependency> dependencies;  // manifest field "requires"
  std::optional<update::SemVer> minPlayer;  // optional field "min_player": the oldest Uzume that runs the plugin
  QStringList replaces;            // optional field "replaces": ids of older plugins this one takes over (renames)
  QStringList provides;
  QList<GamePattern> games;        // empty = all games
  QStringList permissions;
  QString entry;                   // relative path of the .lua entry file
  QStringList assets;
  QJsonObject settings;            // raw, schema not interpreted yet
  UiDecl ui;                       // optional field "ui"
  QList<RunSetting> runSettings;   // optional field "run_settings": the run rules the creator sets in "Start a run"
  QList<RecapEntry> recap;         // optional field "recap": event type -> recap glyph (invalid entries are skipped)
  QString issuesUrl;               // optional field "issues_url": https://github.com/<owner>/<repo> (ADR 0035 D4); empty = none or invalid
  QStringList warnings;            // non-fatal manifest problems (an ignored `recap` entry); the plugin still loads

  bool matchesGame(const QString& system, const QString& code) const;
  bool hasPermission(const QString& p) const { return permissions.contains(p); }
  const Dependency* dependency(const QString& pluginId) const;
};

struct ManifestResult {
  bool ok = false;
  Manifest manifest;
  QString error;  // clear English message when !ok
};

// Parses and validates `json`. When `pluginDir` is not empty, `entry` must exist inside it.
ManifestResult parseManifest(const QByteArray& json, const QString& pluginDir = QString());
// Reads <dir>/manifest.json and validates it against <dir>.
ManifestResult loadManifest(const QString& dir);

// A plugin as known to the manager and handed to the host.
struct PluginInfo {
  Manifest manifest;
  QString origin = QStringLiteral("local");  // "official" (valid Uzume signature) | "unverified" (unsigned Hub copy) | "local"
  bool hubCopy = false;                      // installed under hub/<id>/<version> (delivered by the Hub)
  QString sha256;                            // hub copy: SHA-256 of the .uzplugin file it was installed from
  QString path;                              // directory the plugin runs from
  QString cacheDir;                          // per-plugin cache directory (created on demand)
  bool enabled = false;
  bool devLinked = false;                    // runs straight from a linked developer folder
  QStringList problems;                      // why it cannot start (manifest or dependency problems)
};

}  // namespace uzume::plugins
