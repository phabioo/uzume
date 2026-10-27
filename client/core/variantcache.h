#pragma once

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <optional>

namespace uzume {

// What it takes to build a variant again (ADR 0031 D1): the engine and its version, the settings string, the seed and the pin of
// the base. Stored next to the built files so a variant can be rebuilt while the server is offline.
struct VariantRecipe {
  QString variantId;  // the server's variant id, or "run-<run id>" for a run variant
  QString gameId;
  QString name;
  QString slot;
  QString presetId;     // the preset it was made from ("custom" or empty = own settings); the server does not store it
  QString system;       // "nds", "3ds", "gb", ...
  QString toolId = QStringLiteral("randomizer");
  QString toolVersion;
  QString settings;     // settings string: 3-digit settings version + base64
  QString seed;         // decimal integer text (an int63 does not fit a JSON double)
  QString baseSha256;   // GB to NDS: the SHA-256 of the base dump the world depends on; empty = not pinned
  QString titleId;      // 3DS: program id, 16 upper case hex digits (folder of load/mods)
  QString updateVersion;
  QString romFileName;  // GB to NDS: file name of the built ROM ("<base name>.nds")
  bool spoilerLog = false;

  bool isMod() const { return system == QLatin1String("3ds"); }  // the output is a LayeredFS folder, not a ROM
  QJsonObject toJson() const;
  static VariantRecipe fromJson(const QJsonObject& o);
};

// Local variant cache: <root>/<variant id>/ with recipe.json (kept when files are dropped), built.json (the marker of a finished
// build: size, built and last played time), out/ (Gen 1 to 5: the ROM file; 3DS: the content of the mod folder) and the optional
// spoiler.log. root = <data>/cache/variants. Builds happen in <root>/.tmp-<id> and are moved in atomically. Files may be dropped at
// any time (size limit, age, storage clean-up): the recipe rebuilds them. Nothing here ever logs file content.
class VariantCache {
 public:
  explicit VariantCache(const QString& root);

  static bool isValidId(const QString& id);  // [A-Za-z0-9_-]{1,64}

  const QString& root() const { return root_; }
  QString dir(const QString& id) const;       // empty for an invalid id
  QString outDir(const QString& id) const;    // <dir>/out
  QString tempDir(const QString& id) const;   // <root>/.tmp-<id>
  QString romPath(const QString& id) const;   // Gen 1 to 5: the built ROM; empty if not built or a mod variant
  QString modDir(const QString& id) const;    // 3DS: the mod folder content; empty if not built or not a mod variant
  QString spoilerLogPath(const QString& id) const;  // empty if there is none

  bool isBuilt(const QString& id) const;
  std::optional<VariantRecipe> recipe(const QString& id) const;
  bool writeRecipe(const VariantRecipe& r) const;

  // Moves a finished build (<tempDir>/out, optional <tempDir>/spoiler.log) into place, replacing an earlier one, and writes the
  // marker. The recipe is written too. false on an I/O error (the temp folder is removed either way).
  bool commit(const VariantRecipe& r, qint64 nowMs = 0) const;
  void discardTemp(const QString& id) const;
  // Removes everything of a variant (files and recipe); with keepRecipe only the built files.
  bool remove(const QString& id, bool keepRecipe = false) const;
  // "Last played": a start of the variant, evicted last by the LRU.
  void touch(const QString& id, qint64 nowMs = 0) const;

  struct Entry {
    QString id;
    qint64 bytes = 0;
    qint64 lastPlayedMs = 0;
    qint64 builtMs = 0;
    bool played = false;  // started at least once (lastPlayedMs alone starts at the build time, for the LRU)
  };
  QList<Entry> entries() const;  // built variants, least recently played first
  qint64 totalBytes() const;     // built variants and unfinished temp folders

  struct TrimResult {
    qint64 freedBytes = 0;
    int removed = 0;
    qint64 remainingBytes = 0;
  };
  static constexpr qint64 kDefaultLimitBytes = 8ll * 1024 * 1024 * 1024;  // 8 GB; 0 = unlimited
  // Drops built files (the recipe stays) least recently played first until the rest fits; protected ids stay. Stale temp folders
  // (no protected id, older than a day) are removed.
  TrimResult trimToLimit(qint64 limitBytes, const QSet<QString>& protectedIds = {}, qint64 nowMs = 0) const;
  // "Remove variants not played for N days" (0 = never): built files of variants whose last played time is older.
  TrimResult removeUnplayed(int days, const QSet<QString>& protectedIds = {}, qint64 nowMs = 0) const;

 private:
  static qint64 folderBytes(const QString& dir);
  QString root_;
};

}  // namespace uzume
