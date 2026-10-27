#include "zipextract.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <cstdint>
#include <vector>

// The inflate part follows zlib's puff.c (Copyright (C) 2002-2013 Mark Adler, zlib license; full notice in
// plugins/uzplugin.cpp), changed to write into a buffer of known size.

namespace uzume {

namespace {

quint16 le16(const QByteArray& b, qsizetype p) {
  return static_cast<quint16>(static_cast<quint8>(b[p]) | (static_cast<quint8>(b[p + 1]) << 8));
}
quint32 le32(const QByteArray& b, qsizetype p) { return le16(b, p) | (static_cast<quint32>(le16(b, p + 2)) << 16); }

quint32 crc32of(const uint8_t* d, size_t n) {
  static const std::vector<quint32> table = [] {
    std::vector<quint32> t(256);
    for (quint32 i = 0; i < 256; ++i) {
      quint32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  quint32 c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

struct Bits {
  const uint8_t* in;
  size_t size;
  size_t pos = 0;
  uint32_t buf = 0;
  int cnt = 0;
  bool bad = false;
  int get(int need) {
    uint32_t v = buf;
    while (cnt < need) {
      if (pos >= size) {
        bad = true;
        return 0;
      }
      v |= static_cast<uint32_t>(in[pos++]) << cnt;
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

bool buildHuff(Huff* h, const short* length, int n) {
  for (short& c : h->count) c = 0;
  for (int i = 0; i < n; ++i) ++h->count[length[i]];
  short offs[16];
  offs[1] = 0;
  for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<short>(offs[l] + h->count[l]);
  for (int i = 0; i < n; ++i)
    if (length[i] != 0) h->symbol[offs[length[i]]++] = static_cast<short>(i);
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

struct Out {
  uint8_t* p;
  size_t cap;
  size_t n = 0;
};

bool inflateCodes(Bits* s, Out* o, const Huff& lencode, const Huff& distcode) {
  static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int sym = decodeSym(s, lencode);
    if (sym < 0) return false;
    if (sym < 256) {
      if (o->n >= o->cap) return false;
      o->p[o->n++] = static_cast<uint8_t>(sym);
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const int len = lens[sym] + s->get(lext[sym]);
      const int ds = decodeSym(s, distcode);
      if (ds < 0 || ds >= 30) return false;
      const int dist = dists[ds] + s->get(dext[ds]);
      if (s->bad || static_cast<size_t>(dist) > o->n || o->n + static_cast<size_t>(len) > o->cap) return false;
      for (int i = 0; i < len; ++i, ++o->n) o->p[o->n] = o->p[o->n - static_cast<size_t>(dist)];
    }
  }
}

// Raw deflate `in` into exactly `cap` bytes at `out`.
bool inflateRaw(const uint8_t* in, size_t inSize, uint8_t* out, size_t cap) {
  Bits s{in, inSize};
  Out o{out, cap};
  int last = 0;
  do {
    last = s.get(1);
    const int type = s.get(2);
    if (s.bad) return false;
    if (type == 0) {
      s.buf = 0;
      s.cnt = 0;
      if (s.pos + 4 > s.size) return false;
      const size_t len = static_cast<size_t>(in[s.pos] | (in[s.pos + 1] << 8));
      s.pos += 4;
      if (s.pos + len > s.size || o.n + len > o.cap) return false;
      std::copy(in + s.pos, in + s.pos + len, out + o.n);
      o.n += len;
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
      if (!inflateCodes(&s, &o, lc, dc)) return false;
    } else if (type == 2) {
      static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      const int nlen = s.get(5) + 257;
      const int ndist = s.get(5) + 1;
      const int ncode = s.get(4) + 4;
      if (s.bad || nlen > 286 || ndist > 30) return false;
      short lengths[320];
      int idx = 0;
      for (; idx < ncode; ++idx) lengths[order[idx]] = static_cast<short>(s.get(3));
      for (; idx < 19; ++idx) lengths[order[idx]] = 0;
      Huff lc, dc;
      buildHuff(&lc, lengths, 19);
      idx = 0;
      while (idx < nlen + ndist) {
        int sym = decodeSym(&s, lc);
        if (sym < 0) return false;
        if (sym < 16) {
          lengths[idx++] = static_cast<short>(sym);
        } else {
          int len = 0;
          int rep;
          if (sym == 16) {
            if (idx == 0) return false;
            len = lengths[idx - 1];
            rep = 3 + s.get(2);
          } else if (sym == 17) {
            rep = 3 + s.get(3);
          } else {
            rep = 11 + s.get(7);
          }
          if (s.bad || idx + rep > nlen + ndist) return false;
          while (rep--) lengths[idx++] = static_cast<short>(len);
        }
      }
      if (lengths[256] == 0) return false;
      buildHuff(&lc, lengths, nlen);
      buildHuff(&dc, lengths + nlen, ndist);
      if (!inflateCodes(&s, &o, lc, dc)) return false;
    } else {
      return false;
    }
    if (s.bad) return false;
  } while (!last);
  return o.n == cap;
}

bool safeName(const QString& n) {
  if (n.isEmpty() || n.size() > 1024 || n.startsWith(QLatin1Char('/')) || n.contains(QLatin1Char('\\')) || n.contains(QLatin1Char(':')) ||
      n.contains(QChar(0)))
    return false;
  const QStringList segs = n.split(QLatin1Char('/'));
  for (int i = 0; i < segs.size(); ++i) {
    const QString& s = segs.at(i);
    if (s == QLatin1String("..") || s == QLatin1String(".")) return false;
    if (s.isEmpty() && i != segs.size() - 1) return false;  // empty segment (only the trailing one of a folder entry)
    for (const QChar c : s)
      if (c.unicode() < 32) return false;
  }
  return true;
}

// Resolved target of a relative symlink at `linkRel` stays inside the tree?
bool linkStaysInside(const QString& linkRel, const QString& target) {
  if (target.isEmpty() || target.startsWith(QLatin1Char('/')) || target.contains(QLatin1Char('\\')) || target.contains(QLatin1Char(':'))) return false;
  QStringList base = linkRel.split(QLatin1Char('/'));
  base.removeLast();
  for (const QString& seg : target.split(QLatin1Char('/'))) {
    if (seg.isEmpty() || seg == QLatin1String(".")) continue;
    if (seg == QLatin1String("..")) {
      if (base.isEmpty()) return false;
      base.removeLast();
    } else {
      base.append(seg);
    }
  }
  return true;
}

}  // namespace

bool extractZip(const QString& zipPath, const QString& destDir, QString* error, const ZipExtractLimits& limits) {
  const auto fail = [&](const QString& why) {
    if (error) *error = why;
    return false;
  };
  QFile f(zipPath);
  if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("cannot open the archive"));
  const qint64 fsize = f.size();
  if (fsize < 22) return fail(QStringLiteral("not a zip archive"));
  // End of central directory.
  const qint64 tailLen = qMin<qint64>(fsize, 22 + 65535);
  f.seek(fsize - tailLen);
  const QByteArray tail = f.read(tailLen);
  qsizetype eocd = -1;
  for (qsizetype i = tail.size() - 22; i >= 0; --i) {
    if (le32(tail, i) == 0x06054b50u) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) return fail(QStringLiteral("not a zip archive"));
  const int total = le16(tail, eocd + 10);
  const quint32 cdSize = le32(tail, eocd + 12);
  const quint32 cdOff = le32(tail, eocd + 16);
  if (total == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) return fail(QStringLiteral("zip64 archives are not supported"));
  if (total > limits.maxFiles) return fail(QStringLiteral("too many entries"));
  if (static_cast<qint64>(cdOff) + cdSize > fsize) return fail(QStringLiteral("damaged archive"));
  f.seek(cdOff);
  const QByteArray cd = f.read(cdSize);
  if (cd.size() != static_cast<qsizetype>(cdSize)) return fail(QStringLiteral("damaged archive"));

  if (!QDir().mkpath(destDir)) return fail(QStringLiteral("cannot create the folder"));
  const QDir root(destDir);
  const QString rootAbs = QDir::cleanPath(root.absolutePath());

  QSet<QString> seen;
  qint64 totalBytes = 0;
  qsizetype p = 0;
  struct Link {
    QString rel, target;
  };
  QList<Link> links;
  for (int e = 0; e < total; ++e) {
    if (p + 46 > cd.size() || le32(cd, p) != 0x02014b50u) return fail(QStringLiteral("damaged archive"));
    const int madeBy = le16(cd, p + 4);
    const int flags = le16(cd, p + 8);
    const int method = le16(cd, p + 10);
    const quint32 crc = le32(cd, p + 16);
    const quint32 csize = le32(cd, p + 20);
    const quint32 size = le32(cd, p + 24);
    const int nlen = le16(cd, p + 28), elen = le16(cd, p + 30), clen = le16(cd, p + 32);
    const quint32 eattr = le32(cd, p + 38);
    const quint32 lho = le32(cd, p + 42);
    if (p + 46 + nlen + elen + clen > cd.size()) return fail(QStringLiteral("damaged archive"));
    const QString name = QString::fromUtf8(cd.mid(p + 46, nlen));
    p += 46 + nlen + elen + clen;
    if (flags & 1) return fail(QStringLiteral("encrypted entries are not supported"));
    if (csize == 0xFFFFFFFFu || size == 0xFFFFFFFFu) return fail(QStringLiteral("zip64 archives are not supported"));
    if (!safeName(name)) return fail(QStringLiteral("unsafe entry name"));
    const bool isDir = name.endsWith(QLatin1Char('/'));
    const QString rel = isDir ? name.left(name.size() - 1) : name;
    if (rel.isEmpty()) continue;
    if (seen.contains(rel.toLower())) return fail(QStringLiteral("duplicate entry"));
    seen.insert(rel.toLower());
    const QString abs = rootAbs + QLatin1Char('/') + rel;
    if (isDir) {
      if (!QDir().mkpath(abs)) return fail(QStringLiteral("cannot create a folder"));
      continue;
    }
    if (size > limits.maxFileBytes) return fail(QStringLiteral("an entry is too large"));
    totalBytes += size;
    if (totalBytes > limits.maxTotalBytes) return fail(QStringLiteral("the archive is too large"));

    const quint32 unixMode = (madeBy >> 8) == 3 ? (eattr >> 16) : 0;
    const bool isLink = (unixMode & 0170000u) == 0120000u;
    if ((unixMode & 0170000u) != 0 && (unixMode & 0170000u) != 0100000u && !isLink) return fail(QStringLiteral("special files are not supported"));

    // Local header -> data offset.
    f.seek(lho);
    const QByteArray lh = f.read(30);
    if (lh.size() != 30 || le32(lh, 0) != 0x04034b50u) return fail(QStringLiteral("damaged archive"));
    const qint64 dataOff = static_cast<qint64>(lho) + 30 + le16(lh, 26) + le16(lh, 28);
    if (dataOff + csize > fsize) return fail(QStringLiteral("damaged archive"));
    f.seek(dataOff);
    const QByteArray raw = f.read(csize);
    if (raw.size() != static_cast<qsizetype>(csize)) return fail(QStringLiteral("damaged archive"));

    QByteArray content;
    if (method == 0) {
      if (csize != size) return fail(QStringLiteral("damaged archive"));
      content = raw;
    } else if (method == 8) {
      content.resize(static_cast<qsizetype>(size));
      if (!inflateRaw(reinterpret_cast<const uint8_t*>(raw.constData()), static_cast<size_t>(raw.size()),
                      reinterpret_cast<uint8_t*>(content.data()), static_cast<size_t>(size)))
        return fail(QStringLiteral("an entry is damaged"));
    } else {
      return fail(QStringLiteral("unsupported compression"));
    }
    if (crc32of(reinterpret_cast<const uint8_t*>(content.constData()), static_cast<size_t>(content.size())) != crc)
      return fail(QStringLiteral("an entry is damaged (checksum differs)"));

    if (isLink) {
      const QString target = QString::fromUtf8(content);
      if (!linkStaysInside(rel, target)) return fail(QStringLiteral("a link leaves the folder"));
      links.append({rel, target});
      continue;
    }
    if (!QDir().mkpath(QFileInfo(abs).absolutePath())) return fail(QStringLiteral("cannot create a folder"));
    QSaveFile out(abs);
    if (!out.open(QIODevice::WriteOnly) || out.write(content) != content.size() || !out.commit()) return fail(QStringLiteral("cannot write a file"));
    QFileDevice::Permissions perm = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
    const bool exec = (unixMode & 0100u) != 0 || (unixMode == 0 && (rel.contains(QLatin1String("/bin/")) || rel.startsWith(QLatin1String("bin/"))));
    if (exec) perm |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
    QFile::setPermissions(abs, perm);
  }
  for (const Link& l : links) {
    const QString abs = rootAbs + QLatin1Char('/') + l.rel;
    if (!QDir().mkpath(QFileInfo(abs).absolutePath())) return fail(QStringLiteral("cannot create a folder"));
    if (!QFile::link(l.target, abs)) return fail(QStringLiteral("cannot create a link"));
  }
  return true;
}

}  // namespace uzume
