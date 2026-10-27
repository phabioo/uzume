// .uzplugin reader. The inflate part (struct Bits ... inflateRaw) is derived from puff.c of zlib and altered: output cap,
// complete-code checks reduced to over-subscription, C++ with Qt types. Original notice of puff.c:
//
//   puff.c  Copyright (C) 2002-2013 Mark Adler, all rights reserved  version 2.3, 21 Jan 2013
//
//   This software is provided 'as-is', without any express or implied warranty.  In no event will the author be held
//   liable for any damages arising from the use of this software.
//
//   Permission is granted to anyone to use this software for any purpose, including commercial applications, and to
//   alter it and redistribute it freely, subject to the following restrictions:
//
//   1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software.
//      If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is
//      not required.
//   2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original
//      software.
//   3. This notice may not be removed or altered from any source distribution.
//
//   Mark Adler    madler@alumni.caltech.edu
#include "uzplugin.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <array>

#include "updatesig.h"

namespace uzume::plugins {

namespace {

// ---- little-endian readers ----

quint16 get16(const QByteArray& b, qsizetype o) {
  return static_cast<quint16>(static_cast<quint8>(b[o]) | (static_cast<quint8>(b[o + 1]) << 8));
}
quint32 get32(const QByteArray& b, qsizetype o) { return get16(b, o) | (static_cast<quint32>(get16(b, o + 2)) << 16); }

quint32 crc32(const QByteArray& data) {
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

QString sha256Hex(const QByteArray& d) { return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex()); }

// ---- inflate (after zlib's puff.c) with an output cap: a lying or hostile archive cannot grow beyond `limit` ----

struct Bits {
  const QByteArray& in;
  qsizetype pos = 0;
  quint32 buf = 0;
  int cnt = 0;
  bool bad = false;
  int get(int need) {
    quint32 v = buf;
    while (cnt < need) {
      if (pos >= in.size()) {
        bad = true;
        return 0;
      }
      v |= static_cast<quint32>(static_cast<quint8>(in[pos++])) << cnt;
      cnt += 8;
    }
    buf = v >> need;
    cnt -= need;
    return static_cast<int>(v & ((1u << need) - 1));
  }
};

struct Huff {
  short count[16]{};
  short symbol[288]{};
};

// false = over-subscribed set of lengths (invalid).
bool buildHuff(Huff* h, const short* length, int n) {
  for (short& c : h->count) c = 0;
  for (int i = 0; i < n; ++i) ++h->count[length[i]];
  int left = 1;
  for (int l = 1; l <= 15; ++l) {
    left <<= 1;
    left -= h->count[l];
    if (left < 0) return false;
  }
  short offs[16];
  offs[1] = 0;
  for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<short>(offs[l] + h->count[l]);
  for (int i = 0; i < n; ++i) {
    if (length[i] != 0) h->symbol[offs[length[i]]++] = static_cast<short>(i);
  }
  return true;
}

int decodeSym(Bits* s, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= 15; ++len) {
    code |= s->get(1);
    if (s->bad) return -1;
    const int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

bool inflateCodes(Bits* s, QByteArray* out, qsizetype limit, const Huff& lencode, const Huff& distcode) {
  static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int sym = decodeSym(s, lencode);
    if (sym < 0) return false;
    if (sym < 256) {
      if (out->size() >= limit) return false;
      out->append(static_cast<char>(sym));
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const int len = lens[sym] + s->get(lext[sym]);
      const int ds = decodeSym(s, distcode);
      if (ds < 0 || ds >= 30) return false;
      const int dist = dists[ds] + s->get(dext[ds]);
      if (s->bad || dist > out->size() || out->size() + len > limit) return false;
      for (int i = 0; i < len; ++i) out->append(out->at(out->size() - dist));
    }
  }
}

// Raw deflate stream -> out. Fails when the output would exceed `limit` bytes.
bool inflateRaw(const QByteArray& in, qsizetype limit, QByteArray* out) {
  Bits s{in};
  out->clear();
  out->reserve(limit);
  int last = 0;
  do {
    last = s.get(1);
    const int type = s.get(2);
    if (s.bad) return false;
    if (type == 0) {
      s.buf = 0;
      s.cnt = 0;
      if (s.pos + 4 > in.size()) return false;
      const int len = get16(in, s.pos);
      if ((get16(in, s.pos) ^ get16(in, s.pos + 2)) != 0xFFFF) return false;
      s.pos += 4;
      if (s.pos + len > in.size() || out->size() + len > limit) return false;
      out->append(in.mid(s.pos, len));
      s.pos += len;
    } else if (type == 1) {
      short lengths[288];
      int i = 0;
      for (; i < 144; ++i) lengths[i] = 8;
      for (; i < 256; ++i) lengths[i] = 9;
      for (; i < 280; ++i) lengths[i] = 7;
      for (; i < 288; ++i) lengths[i] = 8;
      Huff lc, dc;
      buildHuff(&lc, lengths, 288);
      for (i = 0; i < 30; ++i) lengths[i] = 5;
      buildHuff(&dc, lengths, 30);
      if (!inflateCodes(&s, out, limit, lc, dc)) return false;
    } else if (type == 2) {
      static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      const int nlen = s.get(5) + 257, ndist = s.get(5) + 1, ncode = s.get(4) + 4;
      if (s.bad || nlen > 286 || ndist > 30) return false;
      short lengths[320]{};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<short>(s.get(3));
      Huff lc, dc;
      if (!buildHuff(&lc, lengths, 19)) return false;
      short all[320]{};
      int idx = 0;
      while (idx < nlen + ndist) {
        int sym = decodeSym(&s, lc);
        if (sym < 0) return false;
        if (sym < 16) {
          all[idx++] = static_cast<short>(sym);
        } else {
          int len = 0, rep;
          if (sym == 16) {
            if (idx == 0) return false;
            len = all[idx - 1];
            rep = 3 + s.get(2);
          } else if (sym == 17) {
            rep = 3 + s.get(3);
          } else {
            rep = 11 + s.get(7);
          }
          if (idx + rep > nlen + ndist) return false;
          while (rep--) all[idx++] = static_cast<short>(len);
        }
      }
      if (!buildHuff(&lc, all, nlen) || !buildHuff(&dc, all + nlen, ndist)) return false;
      if (!inflateCodes(&s, out, limit, lc, dc)) return false;
    } else {
      return false;
    }
    if (s.bad) return false;
  } while (last == 0);
  return true;
}

// ---- entry names ----

// Empty = fine, otherwise the reason. `name` is a file entry (no trailing '/').
QString nameProblem(const QByteArray& raw, QString* nameOut) {
  if (raw.isEmpty()) return QStringLiteral("empty entry name");
  if (raw.size() > kUzPluginMaxNameBytes) return QStringLiteral("entry name longer than %1 bytes").arg(kUzPluginMaxNameBytes);
  QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
  const QString name = dec.decode(raw);
  if (dec.hasError()) return QStringLiteral("entry name is not valid UTF-8");
  *nameOut = name;
  if (name.startsWith(QLatin1Char('/'))) return QStringLiteral("absolute entry name '%1'").arg(name);
  if (name.contains(QLatin1Char('\\'))) return QStringLiteral("entry name '%1' contains a backslash").arg(name);
  if (name.contains(QLatin1Char(':'))) return QStringLiteral("entry name '%1' contains ':' (drive letter or stream)").arg(name);
  for (const QChar c : name)
    if (c.unicode() < 0x20 || c.unicode() == 0x7f) return QStringLiteral("entry name contains a control character");
  const QStringList parts = name.split(QLatin1Char('/'));
  for (const QString& p : parts) {
    if (p.isEmpty()) return QStringLiteral("entry name '%1' has an empty path segment").arg(name);
    if (p == QLatin1String(".") || p == QLatin1String("..")) return QStringLiteral("entry name '%1' has a '.' or '..' segment").arg(name);
    // Windows: trailing dots/spaces are dropped and device names (CON, NUL, AUX.lua ...) open the device (PI-14).
    if (p.endsWith(QLatin1Char('.')) || p.endsWith(QLatin1Char(' '))) return QStringLiteral("entry name '%1' has a segment ending in '.' or a space").arg(name);
    const QString stem = p.left(p.indexOf(QLatin1Char('.')) < 0 ? p.size() : p.indexOf(QLatin1Char('.'))).trimmed().toUpper();
    static const QStringList reserved = {QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL")};
    const bool numbered = stem.size() == 4 && (stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT"))) && stem[3] >= QLatin1Char('1') && stem[3] <= QLatin1Char('9');
    if (reserved.contains(stem) || numbered) return QStringLiteral("entry name '%1' uses a reserved Windows device name").arg(name);
  }
  return {};
}

UzPluginResult fail(const QString& msg) {
  UzPluginResult r;
  r.error = msg;
  return r;
}

}  // namespace

QByteArray pluginSignatureMessage(const QMap<QString, QString>& sha256ByPath) {
  QList<QPair<QByteArray, QString>> items;
  for (auto it = sha256ByPath.begin(); it != sha256ByPath.end(); ++it) items.append({it.key().toUtf8(), it.value()});
  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });  // QByteArray: byte order
  QByteArray msg(kUzPluginSignaturePrefix);
  for (const auto& i : items) msg += i.second.toLatin1() + "  " + i.first + "\n";
  return msg;
}

SignatureState verifyPluginSignature(const QMap<QString, QByteArray>& files, const QList<QByteArray>& trustedKeys, QString* error) {
  const auto bad = [error](const QString& m) {
    if (error != nullptr) *error = m;
    return SignatureState::Invalid;
  };
  const QString sigName = QString::fromLatin1(kUzPluginSignatureFile);
  auto sigIt = files.constFind(sigName);
  if (sigIt == files.constEnd()) return SignatureState::None;
  if (sigIt->size() > 4 * 1024 * 1024) return bad(QStringLiteral("SIGNATURE.json is too large"));
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(*sigIt, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) return bad(QStringLiteral("SIGNATURE.json is not a JSON object"));
  const QJsonObject o = doc.object();
  if (o.value(QLatin1String("format")).toInt(-1) != 1) return bad(QStringLiteral("SIGNATURE.json: unsupported format"));
  if (!o.value(QLatin1String("files")).isObject() || !o.value(QLatin1String("signature")).isString())
    return bad(QStringLiteral("SIGNATURE.json: 'files' and 'signature' are required"));
  const QJsonObject listed = o.value(QLatin1String("files")).toObject();

  // The list must name exactly every file except SIGNATURE.json, with the right hashes.
  QMap<QString, QString> hashes;
  for (auto it = listed.begin(); it != listed.end(); ++it) {
    if (!it.value().isString()) return bad(QStringLiteral("SIGNATURE.json: hash of %1 is not a string").arg(it.key()));
    hashes.insert(it.key(), it.value().toString());
  }
  for (auto it = files.begin(); it != files.end(); ++it) {
    if (it.key() == sigName) continue;
    auto h = hashes.constFind(it.key());
    if (h == hashes.constEnd()) return bad(QStringLiteral("SIGNATURE.json does not list %1").arg(it.key()));
    if (h.value() != sha256Hex(it.value())) return bad(QStringLiteral("%1 does not match its signed hash").arg(it.key()));
  }
  if (hashes.size() != files.size() - 1) {
    for (auto it = hashes.begin(); it != hashes.end(); ++it)
      if (!files.contains(it.key()) || it.key() == sigName) return bad(QStringLiteral("SIGNATURE.json lists %1, which is not in the file").arg(it.key()));
  }
  static const QRegularExpression hex64(QStringLiteral("^[0-9a-f]{64}$"));
  for (const QString& h : std::as_const(hashes))
    if (!hex64.match(h).hasMatch()) return bad(QStringLiteral("SIGNATURE.json: hashes must be lowercase hex SHA-256"));

  const QByteArray sig = QByteArray::fromBase64(o.value(QLatin1String("signature")).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  if (sig.size() != 64) return bad(QStringLiteral("SIGNATURE.json: signature is not base64 of 64 bytes"));
  if (trustedKeys.isEmpty()) return bad(QStringLiteral("no trusted signing key configured"));
  const QByteArray msg = pluginSignatureMessage(hashes);
  for (const QByteArray& k : trustedKeys)
    if (update::ed25519Verify(k, msg, sig)) return SignatureState::Valid;
  return bad(QStringLiteral("signature does not match any trusted Uzume key"));
}

UzPluginResult readUzPlugin(const QByteArray& b, const QList<QByteArray>& trustedKeys) {
  if (b.size() > kUzPluginMaxFileBytes) return fail(QStringLiteral("the plugin file is larger than %1 MiB").arg(kUzPluginMaxFileBytes >> 20));
  if (b.size() < 22) return fail(QStringLiteral("not a plugin file (not a zip archive)"));

  // End of central directory: the last signature whose comment length reaches exactly to the end of the file.
  qsizetype eocd = -1;
  for (qsizetype p = b.size() - 22; p >= 0 && p >= b.size() - 22 - 65535; --p) {
    if (get32(b, p) == 0x06054b50 && p + 22 + get16(b, p + 20) == b.size()) {
      eocd = p;
      break;
    }
  }
  if (eocd < 0) return fail(QStringLiteral("not a plugin file (not a zip archive)"));
  if (get16(b, eocd + 4) != 0 || get16(b, eocd + 6) != 0) return fail(QStringLiteral("multi-part zip archives are not supported"));
  const int count = get16(b, eocd + 10);
  if (get16(b, eocd + 8) != count) return fail(QStringLiteral("damaged zip archive (entry counts differ)"));
  if (count == 0xFFFF || get32(b, eocd + 12) == 0xFFFFFFFFu || get32(b, eocd + 16) == 0xFFFFFFFFu)
    return fail(QStringLiteral("zip64 archives are not supported"));
  if (count > kUzPluginMaxEntries) return fail(QStringLiteral("the plugin file has more than %1 entries").arg(kUzPluginMaxEntries));
  const qsizetype cdSize = get32(b, eocd + 12);
  qsizetype p = get32(b, eocd + 16);
  if (p + cdSize > eocd) return fail(QStringLiteral("damaged zip archive (central directory)"));

  {  // The declared sizes are summed before anything is unpacked.
    qint64 declared = 0;
    qsizetype q = p;
    for (int i = 0; i < count; ++i) {
      if (q + 46 > eocd || get32(b, q) != 0x02014b50) return fail(QStringLiteral("damaged zip archive (central directory entry)"));
      const int nlen = get16(b, q + 28), xlen = get16(b, q + 30), clen = get16(b, q + 32);
      if (q + 46 + nlen + xlen + clen > eocd) return fail(QStringLiteral("damaged zip archive (entry name)"));
      if (!(nlen > 0 && b[q + 46 + nlen - 1] == '/')) declared += get32(b, q + 24);
      q += 46 + nlen + xlen + clen;
    }
    if (declared > kUzPluginMaxUnpackedBytes) return fail(QStringLiteral("the plugin is larger than %1 MiB unpacked").arg(kUzPluginMaxUnpackedBytes >> 20));
  }

  UzPluginPackage pkg;
  pkg.size = b.size();
  pkg.sha256 = sha256Hex(b);
  QSet<QString> seenLower;
  QSet<QString> dirsNeeded;
  qint64 unpacked = 0;

  for (int i = 0; i < count; ++i) {
    if (p + 46 > eocd || get32(b, p) != 0x02014b50) return fail(QStringLiteral("damaged zip archive (central directory entry)"));
    const quint8 host = static_cast<quint8>(b[p + 5]);
    const quint16 flags = get16(b, p + 8);
    const quint16 method = get16(b, p + 10);
    const quint32 crc = get32(b, p + 16), csize = get32(b, p + 20), size = get32(b, p + 24);
    const int nlen = get16(b, p + 28), xlen = get16(b, p + 30), clen = get16(b, p + 32);
    const quint32 ext = get32(b, p + 38);
    const quint32 lho = get32(b, p + 42);
    if (p + 46 + nlen + xlen + clen > eocd) return fail(QStringLiteral("damaged zip archive (entry name)"));
    const QByteArray rawName = b.mid(p + 46, nlen);
    p += 46 + nlen + xlen + clen;

    if (rawName.endsWith('/')) {  // directory entries: the name is checked like any other (as the Hub does), then ignored
      QString dirName;
      if (const QString why = nameProblem(rawName.left(rawName.size() - 1), &dirName); !why.isEmpty()) return fail(why);
      continue;
    }

    QString name;
    if (const QString why = nameProblem(rawName, &name); !why.isEmpty()) return fail(why);
    if (host == 3 || host == 19) {  // Unix / macOS: the mode is in the high 16 bits of the external attributes
      if (((ext >> 16) & 0xF000u) == 0xA000u) return fail(QStringLiteral("'%1' is a symbolic link; links are not allowed").arg(name));
    }
    if (seenLower.contains(name.toLower())) return fail(QStringLiteral("duplicate entry '%1'").arg(name));
    seenLower.insert(name.toLower());
    if ((flags & 0x0001) != 0 || (flags & 0x0040) != 0) return fail(QStringLiteral("'%1' is encrypted").arg(name));
    if (csize == 0xFFFFFFFFu || size == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) return fail(QStringLiteral("zip64 archives are not supported"));
    if (method != 0 && method != 8) return fail(QStringLiteral("'%1' uses an unsupported compression method").arg(name));
    unpacked += size;
    if (unpacked > kUzPluginMaxUnpackedBytes) return fail(QStringLiteral("the plugin is larger than %1 MiB unpacked").arg(kUzPluginMaxUnpackedBytes >> 20));

    // Local header -> data
    if (static_cast<qsizetype>(lho) + 30 > b.size() || get32(b, lho) != 0x04034b50) return fail(QStringLiteral("damaged zip archive (local header of '%1')").arg(name));
    const qsizetype data = static_cast<qsizetype>(lho) + 30 + get16(b, lho + 26) + get16(b, lho + 28);
    if (data + static_cast<qsizetype>(csize) > b.size()) return fail(QStringLiteral("'%1' is truncated").arg(name));
    const QByteArray raw = b.mid(data, csize);
    QByteArray content;
    if (method == 0) {
      if (csize != size) return fail(QStringLiteral("damaged zip archive ('%1' sizes differ)").arg(name));
      content = raw;
    } else if (!inflateRaw(raw, static_cast<qsizetype>(size), &content)) {
      return fail(QStringLiteral("cannot unpack '%1' (damaged or larger than its declared size)").arg(name));
    }
    if (static_cast<quint32>(content.size()) != size || crc32(content) != crc) return fail(QStringLiteral("'%1' is damaged (checksum differs)").arg(name));
    pkg.files.insert(name, content);
    // Parent folders of this file must not be files themselves.
    for (qsizetype s = name.indexOf(QLatin1Char('/')); s >= 0; s = name.indexOf(QLatin1Char('/'), s + 1)) dirsNeeded.insert(name.left(s).toLower());
  }
  for (const QString& d : std::as_const(dirsNeeded))
    if (seenLower.contains(d)) return fail(QStringLiteral("entry '%1' is both a file and a folder").arg(d));

  // manifest, entry file, api
  auto mi = pkg.files.constFind(QStringLiteral("manifest.json"));
  if (mi == pkg.files.constEnd()) return fail(QStringLiteral("manifest.json not found in the plugin file"));
  const ManifestResult m = parseManifest(*mi);
  if (!m.ok) return fail(m.error);
  if (!pkg.files.contains(m.manifest.entry)) return fail(QStringLiteral("entry file '%1' does not exist").arg(m.manifest.entry));
  pkg.manifest = m.manifest;

  pkg.signature = verifyPluginSignature(pkg.files, trustedKeys, &pkg.signatureError);

  UzPluginResult r;
  r.ok = true;
  r.package = std::move(pkg);
  return r;
}

bool extractUzPlugin(const UzPluginPackage& pkg, const QString& destDir, QString* error) {
  const auto bad = [error](const QString& m) {
    if (error != nullptr) *error = m;
    return false;
  };
  if (!QDir().mkpath(destDir)) return bad(QStringLiteral("cannot create %1").arg(destDir));
  const QString base = QDir(destDir).canonicalPath() + QLatin1Char('/');
  for (auto it = pkg.files.begin(); it != pkg.files.end(); ++it) {
    const QString target = QDir::cleanPath(base + it.key());
    if (!target.startsWith(base)) return bad(QStringLiteral("entry '%1' leaves the plugin folder").arg(it.key()));
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) return bad(QStringLiteral("cannot create the folder for %1").arg(it.key()));
    QFile f(target);
    if (QFileInfo(target).isSymLink() || QFileInfo::exists(target)) return bad(QStringLiteral("'%1' exists already").arg(it.key()));
    if (!f.open(QIODevice::WriteOnly) || f.write(it.value()) != it.value().size()) return bad(QStringLiteral("cannot write %1").arg(it.key()));
    f.close();
  }
  return true;
}

}  // namespace uzume::plugins
