#include "tools_util.h"

#include <QBuffer>
#include <QStringDecoder>

namespace uzume::mcp::tools {

QJsonObject typeStr(const QString& d) { return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("description"), d}}; }
QJsonObject typeInt(const QString& d) { return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("description"), d}}; }
QJsonObject typeBool(const QString& d) { return QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}, {QStringLiteral("description"), d}}; }
QJsonObject typeAny(const QString& d) { return QJsonObject{{QStringLiteral("description"), d}}; }
QJsonObject typeArray(const QJsonObject& items, const QString& d) {
  return QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), items}, {QStringLiteral("description"), d}};
}
QJsonObject typeObject(const QJsonObject& props, const QStringList& required, const QString& d) {
  QJsonObject o{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), props}};
  if (!required.isEmpty()) {
    QJsonArray r;
    for (const QString& s : required) r.append(s);
    o.insert(QStringLiteral("required"), r);
  }
  if (!d.isEmpty()) o.insert(QStringLiteral("description"), d);
  return o;
}
QJsonObject schema(const QJsonObject& props, const QStringList& required) { return typeObject(props, required); }

std::optional<quint64> parseNumber(const QJsonValue& v) {
  if (v.isDouble()) {
    const double d = v.toDouble();
    if (d < 0 || d > 1.8e19) return std::nullopt;
    return static_cast<quint64>(d);
  }
  if (v.isString()) {
    QString s = v.toString().trimmed();
    bool ok = false;
    const quint64 n = s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? s.mid(2).toULongLong(&ok, 16) : s.toULongLong(&ok, 10);
    if (ok) return n;
  }
  return std::nullopt;
}

QString hexAddr(quint64 v) { return QStringLiteral("0x") + QString::number(v, 16).rightJustified(8, QLatin1Char('0')).toUpper(); }

QByteArray pngOf(const QImage& image) {
  if (image.isNull()) return {};
  QImage img = image;
  if (img.width() > kMaxImageDim || img.height() > kMaxImageDim) img = img.scaled(kMaxImageDim, kMaxImageDim, Qt::KeepAspectRatio, Qt::FastTransformation);
  QByteArray out;
  QBuffer buf(&out);
  buf.open(QIODevice::WriteOnly);
  img.save(&buf, "PNG");
  return out;
}

QImage imageFromRgba(int w, int h, const std::string& rgba) {
  if (w < 1 || h < 1 || w > kMaxImageDim || h > kMaxImageDim) return {};
  if (rgba.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) return {};
  QImage img(reinterpret_cast<const uchar*>(rgba.data()), w, h, w * 4, QImage::Format_RGBA8888);
  return img.copy();  // owns its pixels
}

namespace {
using plugins::Value;

const Value* field(const Value& t, const char* name) {
  for (size_t i = 0; i < t.keys.size(); ++i)
    if (t.keys[i].type == Value::Type::String && t.keys[i].s == name) return &t.vals[i];
  return nullptr;
}

bool isImageTable(const Value& t) {
  const Value* w = field(t, "width");
  const Value* h = field(t, "height");
  const Value* d = field(t, "rgba");
  return w && h && d && w->type == Value::Type::Int && h->type == Value::Type::Int && d->type == Value::Type::String;
}

QString keyText(const Value& k) {
  switch (k.type) {
    case Value::Type::String: return QString::fromUtf8(k.s.data(), static_cast<qsizetype>(k.s.size()));
    case Value::Type::Int: return QString::number(k.i);
    case Value::Type::Float: return QString::number(k.d);
    case Value::Type::Bool: return k.b ? QStringLiteral("true") : QStringLiteral("false");
    default: return QStringLiteral("?");
  }
}
}  // namespace

QJsonValue valueToJson(const Value& v, QList<QByteArray>& images, int depth) {
  if (depth > plugins::kMaxValueDepth) return QStringLiteral("...");
  switch (v.type) {
    case Value::Type::Nil: return QJsonValue(QJsonValue::Null);
    case Value::Type::Bool: return v.b;
    case Value::Type::Int: return static_cast<double>(v.i);
    case Value::Type::Float: return v.d;
    case Value::Type::String: {
      QStringDecoder dec(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
      const QString s = dec.decode(QByteArrayView(v.s.data(), static_cast<qsizetype>(v.s.size())));
      if (!dec.hasError()) return s;
      return QJsonObject{{QStringLiteral("base64"), QString::fromLatin1(QByteArray(v.s.data(), static_cast<qsizetype>(v.s.size())).toBase64())}};
    }
    case Value::Type::Table: break;
  }
  if (isImageTable(v)) {
    const int w = static_cast<int>(field(v, "width")->i), h = static_cast<int>(field(v, "height")->i);
    QJsonObject o{{QStringLiteral("width"), w}, {QStringLiteral("height"), h}};
    const QByteArray png = images.size() < 8 ? pngOf(imageFromRgba(w, h, field(v, "rgba")->s)) : QByteArray();
    if (png.isEmpty()) {
      o.insert(QStringLiteral("image"), QJsonValue(QJsonValue::Null));
    } else {
      o.insert(QStringLiteral("image"), static_cast<int>(images.size()));
      images.append(png);
    }
    return o;
  }
  bool array = !v.keys.empty();
  for (size_t i = 0; i < v.keys.size() && array; ++i) array = v.keys[i].type == Value::Type::Int && v.keys[i].i == static_cast<int64_t>(i) + 1;
  if (array) {
    QJsonArray a;
    for (const Value& e : v.vals) a.append(valueToJson(e, images, depth + 1));
    return a;
  }
  QJsonObject o;
  for (size_t i = 0; i < v.keys.size(); ++i) o.insert(keyText(v.keys[i]), valueToJson(v.vals[i], images, depth + 1));
  return o;
}

}  // namespace uzume::mcp::tools
