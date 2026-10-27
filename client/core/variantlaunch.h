#pragma once

#include <QString>

namespace uzume {

// What the game start needs to start exactly one variant (GameStarter asks PlayerController for it).
struct VariantLaunch {
  bool active = false;      // this start is a variant start
  QString variantId;
  QString gameId;
  QString slot;
  bool isMod = false;       // 3DS: the base game starts with a mod folder; otherwise the built ROM is the game file
  QString romPath;          // Gen 1 to 5: the built ROM in the variant cache
  QString modDir;           // 3DS: the built mod folder in the variant cache
  QString titleId;          // 3DS: program id, empty = take it from the game's header
  QString marker;           // identifies the recipe of the placed mod
  QString error;            // set when the variant cannot start (not built, ...)
};

}  // namespace uzume

namespace uzume::variantlaunch {

// Gen 6/7 variants are a LayeredFS folder in the Azahar user folder of the variant's slot: <saveDir>/Azahar/load/mods/<title id>/
// (ADR 0031 D2). Save sync packs only the managed paths below sdmc/ (core/sdtree.h), so the mod never travels to the server.

// True for a program id: 16 hex digits (stored and used upper case).
bool isValidTitleId(const QString& titleId);

// Folder of the mods of one slot: <azaharDir>/load/mods
QString modsDir(const QString& azaharDir);

// Makes load/mods/<title id> exactly the content of `modSource` and removes everything else in load/mods of this slot. A marker file
// (`.uzume-variant`, written last) records `markerValue`; when it already matches, the copy is skipped.
bool placeMod(const QString& azaharDir, const QString& titleId, const QString& modSource, const QString& markerValue, QString* error = nullptr);

// Main (and every slot that is not a variant): load/mods/<title id> must not exist. Returns true when it is absent afterwards.
bool ensureNoMod(const QString& azaharDir, const QString& titleId, QString* error = nullptr);

}  // namespace uzume::variantlaunch
