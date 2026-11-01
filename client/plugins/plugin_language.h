#pragma once
// Plugin UI language (docs/architecture/12-plugins.md 16.9a): the tag behind GameContext.language / uz.game.language and the
// first lookup of uz.i18n.t. Pure functions, no UI and no settings access.
#include <QList>
#include <QLocale>
#include <QMap>
#include <QString>

namespace uzume::plugins {

struct PluginLanguageChoice {
  QString tag;    // stored in the setting and handed to plugins ("en", "de")
  QString label;  // shown in Settings, in the language itself
};

constexpr const char* kPluginLanguageAuto = "auto";
// Core option of Azahar that sets the language the emulated 3DS reports to games ("3DS System Language").
constexpr const char* kAzaharLanguageOption = "citra_language_value";

// The languages Settings offers besides Auto; English first, German next (plugins ship en.json and de.json).
QList<PluginLanguageChoice> pluginLanguageChoices();

// "de_DE" / "de-de" / "DE" -> "de-DE" / "de-DE" / "de". Empty or unusable input ("C", "POSIX", garbage) -> "".
QString normalizeLanguageTag(const QString& tag);

// Azahar "3DS System Language" value ("German", "Simplified Chinese", ...; a code such as "de" is accepted too) -> tag, "" when unknown.
QString languageFromAzaharOption(const QString& value);

// Language of the app UI (English only today). What Auto resolves to, so plugin texts match the app.
QString appUiLanguage();

// What the plugins get. `setting` = "auto" (or empty) or an explicit tag. Auto: the app UI language (appUiLanguage()); game
// language and OS locale are not used. An explicit setting wins. Names read from the game stay in the game's language.
QString resolvePluginLanguage(const QString& setting);

}  // namespace uzume::plugins
