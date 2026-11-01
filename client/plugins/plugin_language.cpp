#include "plugin_language.h"

#include <QRegularExpression>
#include <QStringList>

namespace uzume::plugins {

QList<PluginLanguageChoice> pluginLanguageChoices() {
  return {{QStringLiteral("en"), QStringLiteral("English")}, {QStringLiteral("de"), QStringLiteral("Deutsch")}};
}

QString normalizeLanguageTag(const QString& tag) {
  QString t = tag.trimmed();
  const int dot = t.indexOf(QLatin1Char('.'));  // POSIX "de_DE.UTF-8"
  if (dot >= 0) t.truncate(dot);
  const int at = t.indexOf(QLatin1Char('@'));
  if (at >= 0) t.truncate(at);
  t.replace(QLatin1Char('_'), QLatin1Char('-'));
  static const QRegularExpression ok(QStringLiteral("^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*$"));
  if (!ok.match(t).hasMatch()) return QString();  // also "C" is not a language
  if (t.compare(QLatin1String("POSIX"), Qt::CaseInsensitive) == 0) return QString();
  const QStringList parts = t.split(QLatin1Char('-'));
  QString out = parts.first().toLower();
  for (int i = 1; i < parts.size(); ++i) {
    const QString& p = parts.at(i);
    if (p.size() == 2 && p.at(0).isLetter() && p.at(1).isLetter()) out += QLatin1Char('-') + p.toUpper();              // region
    else if (p.size() == 4 && p.at(0).isLetter()) out += QLatin1Char('-') + p.at(0).toUpper() + p.mid(1).toLower();  // script
    else out += QLatin1Char('-') + p;
  }
  return out;
}

QString languageFromAzaharOption(const QString& value) {
  static const QMap<QString, QString> names = {
      {QStringLiteral("japanese"), QStringLiteral("ja")},
      {QStringLiteral("english"), QStringLiteral("en")},
      {QStringLiteral("french"), QStringLiteral("fr")},
      {QStringLiteral("german"), QStringLiteral("de")},
      {QStringLiteral("italian"), QStringLiteral("it")},
      {QStringLiteral("spanish"), QStringLiteral("es")},
      {QStringLiteral("korean"), QStringLiteral("ko")},
      {QStringLiteral("dutch"), QStringLiteral("nl")},
      {QStringLiteral("portuguese"), QStringLiteral("pt")},
      {QStringLiteral("russian"), QStringLiteral("ru")},
      {QStringLiteral("simplified chinese"), QStringLiteral("zh-Hans")},
      {QStringLiteral("traditional chinese"), QStringLiteral("zh-Hant")}};
  const QString v = value.trimmed().toLower();
  if (v.isEmpty()) return QString();
  if (const auto it = names.constFind(v); it != names.constEnd()) return it.value();
  return normalizeLanguageTag(value);
}

QString appUiLanguage() {
  return QStringLiteral("en");  // the app UI is English only; return its language here once the app gets translations
}

QString resolvePluginLanguage(const QString& setting) {
  const QString s = setting.trimmed();
  if (!s.isEmpty() && s.compare(QLatin1String(kPluginLanguageAuto), Qt::CaseInsensitive) != 0) {
    const QString explicitTag = normalizeLanguageTag(s);
    if (!explicitTag.isEmpty()) return explicitTag;
  }
  return appUiLanguage();
}

}  // namespace uzume::plugins
