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
  void autoFollowsAppLanguage() {
    QCOMPARE(resolvePluginLanguage("auto"), appUiLanguage());
    QCOMPARE(resolvePluginLanguage(""), QStringLiteral("en"));
    QCOMPARE(resolvePluginLanguage("  AUTO "), QStringLiteral("en"));
  }
  void explicitChoiceWins() {
    QCOMPARE(resolvePluginLanguage("en"), QStringLiteral("en"));
    QCOMPARE(resolvePluginLanguage("de"), QStringLiteral("de"));
    QCOMPARE(resolvePluginLanguage("de_DE"), QStringLiteral("de-DE"));
    // An unusable explicit value behaves like Auto.
    QCOMPARE(resolvePluginLanguage("!!"), QStringLiteral("en"));
  }
};

QTEST_APPLESS_MAIN(LanguageTest)
#include "language_test.moc"
