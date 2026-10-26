#pragma once
// Small helpers shared by the tool groups: JSON schema snippets, argument parsing, PNG encoding, Lua value -> JSON.
#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <optional>

#include "plugin_value.h"

namespace uzume::mcp::tools {

inline constexpr int kMaxLogLines = 2000;
inline constexpr qint64 kMaxMemoryBytes = 1024 * 1024;
inline constexpr qint64 kMaxRomfsBytes = 4 * 1024 * 1024;
inline constexpr qint64 kMaxScanBytes = 64ll * 1024 * 1024;
inline constexpr int kMaxImageDim = 4096;

// JSON schema builders.
QJsonObject typeStr(const QString& description);
QJsonObject typeInt(const QString& description);
QJsonObject typeBool(const QString& description);
QJsonObject typeAny(const QString& description);
QJsonObject typeArray(const QJsonObject& items, const QString& description);
QJsonObject typeObject(const QJsonObject& properties, const QStringList& required = {}, const QString& description = QString());
QJsonObject schema(const QJsonObject& properties = {}, const QStringList& required = {});

// An address or offset: a JSON number or a string ("0x08000000", "1234"). nullopt when it is neither or negative.
std::optional<quint64> parseNumber(const QJsonValue& v);
QString hexAddr(quint64 v);

// PNG bytes of an image; images larger than kMaxImageDim in a dimension are scaled down first. Empty for a null image.
QByteArray pngOf(const QImage& image);
// {width, height, rgba} (RGBA8888, straight alpha) -> image; null image when the sizes do not fit.
QImage imageFromRgba(int width, int height, const std::string& rgba);

// A Lua value as JSON: tables with the keys 1..n become arrays, other tables objects; strings that are not UTF-8 become
// {"base64": "..."}; an image table {width, height, rgba} becomes {"image": <index>, "width", "height"} and its PNG is
// appended to `images` (at most 8; further ones are replaced by {"image": null}).
QJsonValue valueToJson(const plugins::Value& v, QList<QByteArray>& images, int depth = 0);

}  // namespace uzume::mcp::tools
