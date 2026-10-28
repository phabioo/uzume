#include "errorfingerprint.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QStringList>

namespace uzume::errors {

namespace {

struct Rule {
  QRegularExpression re;
  QString with;
};

QRegularExpression compile(const char* pattern) {
  QRegularExpression re(QString::fromLatin1(pattern));
  Q_ASSERT(re.isValid());
  return re;
}

// Order matters (ADR 0035 contract): URLs first so their paths and hosts do not match again, digits last.
const QList<Rule>& rules() {
  static const QList<Rule> r = {
      {compile(R"([a-z][a-z0-9+.-]*://\S+)"), QStringLiteral("<url>")},
      {compile(R"([a-z]:[\\/][^\s"']*)"), QStringLiteral("<path>")},
      {compile(R"((?:~|/)[^\s"':]*/[^\s"':]*)"), QStringLiteral("<path>")},
      {compile(R"(\b\d{1,3}(?:\.\d{1,3}){3}(?::\d+)?\b)"), QStringLiteral("<ip>")},
      {compile(R"(\b(?:[0-9a-f]{0,4}:){2,7}[0-9a-f]{0,4}\b)"), QStringLiteral("<ip>")},
      {compile(R"(\b(?:0x)?[0-9a-f]{6,}\b)"), QStringLiteral("<hex>")},
      {compile(R"(\d+)"), QStringLiteral("<n>")},
  };
  return r;
}

bool isSpace(QChar c) {
  // Same set as Go's unicode.IsSpace.
  const char16_t u = c.unicode();
  return u == 0x20 || (u >= 0x09 && u <= 0x0D) || u == 0x85 || u == 0xA0 || u == 0x1680 || (u >= 0x2000 && u <= 0x200A) ||
         u == 0x2028 || u == 0x2029 || u == 0x202F || u == 0x205F || u == 0x3000;
}

QString collapseSpace(const QString& s) {
  QString out;
  out.reserve(s.size());
  bool pending = false;
  for (const QChar c : s) {
    if (isSpace(c)) {
      pending = !out.isEmpty();
      continue;
    }
    if (pending) out += QLatin1Char(' ');
    pending = false;
    out += c;
  }
  return out;
}

}  // namespace

QString normalizeMessage(const QString& message) {
  QString s = message;
  for (QChar& c : s) {
    if (c.unicode() >= 'A' && c.unicode() <= 'Z') c = QChar(c.unicode() + 32);  // ASCII only, like the server
  }
  for (const Rule& r : rules()) s.replace(r.re, r.with);
  s = collapseSpace(s);
  QByteArray utf8 = s.toUtf8();
  if (utf8.size() > kMaxNormalizedBytes) {
    int cut = kMaxNormalizedBytes;
    while (cut > 0 && (static_cast<unsigned char>(utf8.at(cut)) & 0xC0) == 0x80) --cut;  // never inside a character
    utf8.truncate(cut);
    return QString::fromUtf8(utf8);
  }
  return s;
}

QString fingerprint(const QString& kind, const QString& component, const QString& message) {
  const QByteArray input = kind.toUtf8() + '\n' + component.toUtf8() + '\n' + normalizeMessage(message).toUtf8();
  const QByteArray hex = QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex();
  return QStringLiteral("uz-") + QString::fromLatin1(hex.left(12));
}

bool validKind(const QString& kind) {
  static const QStringList kinds = {QStringLiteral("app_crash"), QStringLiteral("core_failure"), QStringLiteral("plugin_error"),
                                    QStringLiteral("sync_failure"), QStringLiteral("update_failure"), QStringLiteral("server_error")};
  return kinds.contains(kind);
}

bool validComponent(const QString& component) {
  static const QRegularExpression re(QStringLiteral(R"(\A(?:app|server|core:[a-z0-9][a-z0-9_-]{0,63}|plugin:[A-Za-z0-9._-]{1,80})\z)"));
  return component.size() <= 96 && re.match(component).hasMatch();
}

bool validFingerprint(const QString& fp) {
  static const QRegularExpression re(QStringLiteral(R"(\Auz-[0-9a-f]{12}\z)"));
  return re.match(fp).hasMatch();
}

}  // namespace uzume::errors
