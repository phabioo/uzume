#pragma once
// Test helpers for .uzplugin files: a zip writer that can also build hostile archives, an Ed25519 test key (fixed seed,
// never the release key) and a builder for signed and unsigned plugin files. Synthetic data only.

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <array>

#include <openssl/evp.h>

#include "uzplugin.h"

namespace uzume::plugins::testutil {

struct ZipEntrySpec {
  QString name;
  QByteArray data;
  bool deflate = false;
  bool symlink = false;
  qint64 declaredSize = -1;  // -1 = real size; else written to the headers (lying archives)
  quint32 crcOverride = 0;
  bool overrideCrc = false;
};

inline quint32 zipCrc32(const QByteArray& data) {
  static const std::array<quint32, 256> table = [] {
    std::array<quint32, 256> t{};
    for (quint32 i = 0; i < 256; ++i) {
      quint32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  quint32 c = 0xFFFFFFFFu;
  for (const char ch : data) c = table[(c ^ static_cast<quint8>(ch)) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

inline void put16(QByteArray& b, quint16 v) {
  b.append(static_cast<char>(v & 0xFF));
  b.append(static_cast<char>(v >> 8));
}
inline void put32(QByteArray& b, quint32 v) {
  put16(b, static_cast<quint16>(v & 0xFFFF));
  put16(b, static_cast<quint16>(v >> 16));
}

// Names are written as given (UTF-8), so tests can use "../x", "a\\b", duplicates and so on.
inline QByteArray buildZip(const QList<ZipEntrySpec>& entries, const QByteArray& comment = {}) {
  QByteArray out, cd;
  for (const ZipEntrySpec& e : entries) {
    const QByteArray name = e.name.toUtf8();
    QByteArray body = e.data;
    quint16 method = 0;
    if (e.deflate) {
      const QByteArray z = qCompress(e.data, 6);  // 4 byte length, zlib header (2), raw deflate, Adler-32 (4)
      body = z.mid(6, z.size() - 10);
      method = 8;
    }
    const quint32 crc = e.overrideCrc ? e.crcOverride : zipCrc32(e.data);
    const quint32 size = e.declaredSize >= 0 ? static_cast<quint32>(e.declaredSize) : static_cast<quint32>(e.data.size());
    const quint32 offset = static_cast<quint32>(out.size());
    QByteArray h;
    put32(h, 0x04034b50);
    put16(h, 20);
    put16(h, 0x0800);
    put16(h, method);
    put16(h, 0);
    put16(h, 0x21);
    put32(h, crc);
    put32(h, static_cast<quint32>(body.size()));
    put32(h, size);
    put16(h, static_cast<quint16>(name.size()));
    put16(h, 0);
    out += h + name + body;
    put32(cd, 0x02014b50);
    put16(cd, static_cast<quint16>((e.symlink ? 3 : 0) << 8 | 20));
    put16(cd, 20);
    put16(cd, 0x0800);
    put16(cd, method);
    put16(cd, 0);
    put16(cd, 0x21);
    put32(cd, crc);
    put32(cd, static_cast<quint32>(body.size()));
    put32(cd, size);
    put16(cd, static_cast<quint16>(name.size()));
    put16(cd, 0);
    put16(cd, 0);
    put16(cd, 0);
    put16(cd, 0);
    put32(cd, e.symlink ? (0xA1FFu << 16) : (name.endsWith('/') ? 0x10u : 0u));
    put32(cd, offset);
    cd += name;
  }
  const quint32 cdOffset = static_cast<quint32>(out.size());
  out += cd;
  put32(out, 0x06054b50);
  put16(out, 0);
  put16(out, 0);
  put16(out, static_cast<quint16>(entries.size()));
  put16(out, static_cast<quint16>(entries.size()));
  put32(out, static_cast<quint32>(cd.size()));
  put32(out, cdOffset);
  put16(out, static_cast<quint16>(comment.size()));
  out += comment;
  return out;
}

struct TestKey {
  QByteArray seed;
  QByteArray pub;
};

// Deterministic test key: seed bytes 0x00 .. 0x1f (documented so the Hub test can use the same vector).
inline TestKey fixedTestKey(int seedOffset = 0) {
  TestKey k;
  k.seed.resize(32);
  for (int i = 0; i < 32; ++i) k.seed[i] = static_cast<char>(i + seedOffset);
  EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(k.seed.constData()), 32);
  size_t len = 32;
  k.pub.resize(32);
  EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<unsigned char*>(k.pub.data()), &len);
  EVP_PKEY_free(pkey);
  return k;
}

inline QByteArray ed25519Sign(const TestKey& k, const QByteArray& msg) {
  EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(k.seed.constData()), 32);
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey);
  size_t siglen = 64;
  QByteArray sig(64, 0);
  EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &siglen, reinterpret_cast<const unsigned char*>(msg.constData()),
                 static_cast<size_t>(msg.size()));
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return sig;
}

inline QString sha256Hex(const QByteArray& d) { return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex()); }

// SIGNATURE.json for `files` (path -> content, without SIGNATURE.json).
inline QByteArray signatureJson(const QMap<QString, QByteArray>& files, const TestKey& key) {
  QMap<QString, QString> hashes;
  QJsonObject listed;
  for (auto it = files.begin(); it != files.end(); ++it) {
    hashes.insert(it.key(), sha256Hex(it.value()));
    listed.insert(it.key(), sha256Hex(it.value()));
  }
  const QByteArray sig = ed25519Sign(key, pluginSignatureMessage(hashes));
  return QJsonDocument(QJsonObject{{"format", 1}, {"files", listed}, {"signature", QString::fromLatin1(sig.toBase64())}}).toJson(QJsonDocument::Compact);
}

struct FbSpec {
  QString id = QStringLiteral("net.test.plugin");
  QString name;
  QString version = QStringLiteral("1.0.0");
  QStringList permissions{QStringLiteral("memory.read")};
  QString lua = QStringLiteral("fb.log.info('hello')\n");
  QMap<QString, QByteArray> extra;  // extra files
  bool sign = false;
  TestKey key = fixedTestKey();
  QString entry = QStringLiteral("main.lua");
  QJsonObject ui;  // manifest field "ui" (needs the permission "ui")
  QJsonObject requires_;  // manifest field "requires" (plugin id -> version range)
  QJsonArray recap;  // manifest field "recap" (event type -> recap glyph, 0.22)
};

inline QMap<QString, QByteArray> fbFiles(const FbSpec& s) {
  QJsonObject m{{"id", s.id}, {"name", s.name.isEmpty() ? s.id : s.name}, {"version", s.version}, {"api", 1}, {"type", "script"}, {"entry", s.entry},
                {"permissions", QJsonArray::fromStringList(s.permissions)}};
  if (!s.ui.isEmpty()) m["ui"] = s.ui;
  if (!s.requires_.isEmpty()) m["requires"] = s.requires_;
  if (!s.recap.isEmpty()) m["recap"] = s.recap;
  QMap<QString, QByteArray> files = s.extra;
  files.insert(QStringLiteral("manifest.json"), QJsonDocument(m).toJson());
  files.insert(s.entry, s.lua.toUtf8());
  return files;
}

inline QByteArray makeUzPlugin(const FbSpec& s) {
  QMap<QString, QByteArray> files = fbFiles(s);
  if (s.sign) files.insert(QStringLiteral("SIGNATURE.json"), signatureJson(files, s.key));
  QList<ZipEntrySpec> entries;
  for (auto it = files.begin(); it != files.end(); ++it) entries.append(ZipEntrySpec{it.key(), it.value(), it.value().size() > 64});
  return buildZip(entries);
}

}  // namespace uzume::plugins::testutil
