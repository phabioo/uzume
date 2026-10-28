// Plugin UI language resolution (plugin_language.h). The uz.i18n fallback chain de-DE -> de -> en is covered in host_test.
#include <QtTest>

#include "plugin_language.h"

using namespace uzume::plugins;

class LanguageTest : public QObject {
  Q_OBJECT
 private slots:
  void choicesEnglishFirst() {
    const auto c = pluginLanguageChoices();
    QVERIFY(c.size() >= 2);
    QCOMPARE(c.at(0).tag, QStringLiteral("en"));
    QCOMPARE(c.at(1).tag, QStringLiteral("de"));
    QCOMPARE(c.at(1).label, QStringLiteral("Deutsch"));
  }
  void normalize() {
    QCOMPARE(normalizeLanguageTag("de_DE"), QStringLiteral("de-DE"));
    QCOMPARE(normalizeLanguageTag("de-de"), QStringLiteral("de-DE"));
    QCOMPARE(normalizeLanguageTag("DE"), QStringLiteral("de"));
    QCOMPARE(normalizeLanguageTag("de_DE.UTF-8"), QStringLiteral("de-DE"));
    QCOMPARE(normalizeLanguageTag("zh-hans-cn"), QStringLiteral("zh-Hans-CN"));
    QCOMPARE(normalizeLanguageTag("C"), QString());
    QCOMPARE(normalizeLanguageTag("POSIX"), QString());
    QCOMPARE(normalizeLanguageTag("../x"), QString());
    QCOMPARE(normalizeLanguageTag(""), QString());
  }
  void azaharValues() {
    QCOMPARE(languageFromAzaharOption("German"), QStringLiteral("de"));
    QCOMPARE(languageFromAzaharOption("english"), QStringLiteral("en"));  // the core's default value
    QCOMPARE(languageFromAzaharOption("English"), QStringLiteral("en"));
    QCOMPARE(languageFromAzaharOption("Simplified Chinese"), QStringLiteral("zh-Hans"));
    QCOMPARE(languageFromAzaharOption("fr"), QStringLiteral("fr"));
    QCOMPARE(languageFromAzaharOption(""), QString());
    QCOMPARE(languageFromAzaharOption("???"), QString());
  }
  void autoWith3dsOption() {
    const QMap<QString, QString> o{{kAzaharLanguageOption, "German"}};
    QCOMPARE(resolvePluginLanguage("auto", "3ds", o, QLocale("en_US")), QStringLiteral("de"));
    QCOMPARE(resolvePluginLanguage("", "3ds", o, QLocale("fr_FR")), QStringLiteral("de"));
    // Other systems ignore the 3DS option.
    QCOMPARE(resolvePluginLanguage("auto", "nds", o, QLocale("fr_FR")), QStringLiteral("fr-FR"));
    // An unknown option value falls through to the OS language.
    const QMap<QString, QString> bad{{kAzaharLanguageOption, "Klingon!"}};
    QCOMPARE(resolvePluginLanguage("auto", "3ds", bad, QLocale("de_DE")), QStringLiteral("de-DE"));
  }
  void autoWithSystemLocale() {
    QCOMPARE(resolvePluginLanguage("auto", "3ds", {}, QLocale("de_DE")), QStringLiteral("de-DE"));
    QCOMPARE(resolvePluginLanguage("auto", "", {}, QLocale("en_GB")), QStringLiteral("en-GB"));
    QCOMPARE(resolvePluginLanguage("auto", "nds", {}, QLocale::c()), QStringLiteral("en"));
  }
  void explicitChoiceWins() {
    const QMap<QString, QString> o{{kAzaharLanguageOption, "German"}};
    QCOMPARE(resolvePluginLanguage("en", "3ds", o, QLocale("de_DE")), QStringLiteral("en"));
    QCOMPARE(resolvePluginLanguage("de", "nds", {}, QLocale("en_US")), QStringLiteral("de"));
    QCOMPARE(resolvePluginLanguage("de_DE", "nds", {}, QLocale("en_US")), QStringLiteral("de-DE"));
    // An unusable explicit value behaves like Auto.
    QCOMPARE(resolvePluginLanguage("!!", "nds", {}, QLocale("de_DE")), QStringLiteral("de-DE"));
  }
};

QTEST_APPLESS_MAIN(LanguageTest)
#include "language_test.moc"
