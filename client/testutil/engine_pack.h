#pragma once
// Test helper: packs the test engine (testutil/test_engine.cpp) into a zip shaped like the randomizer engine's release package:
// UPR-FVX.jar (empty), java/bin/java[.exe] (the test engine), data/readme.txt, optionally below one top folder. Entries are stored
// (or deflated with deflate=true) and carry unix modes, so the extracted java is executable. Synthetic data only.

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QList>
#include <QString>

namespace uzume::testutil {

struct PackEntry {
  QString name;
  QByteArray data;
  quint32 mode = 0100644;  // st_mode
  bool deflate = false;
};

inline quint32 packCrc32(const QByteArray& data) {
  quint32 c = 0xFFFFFFFFu;
  for (const char ch : data) {
    c ^= static_cast<quint8>(ch);
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
  }
  return c ^ 0xFFFFFFFFu;
}

inline void packPut16(QByteArray& b, quint16 v) {
  b.append(static_cast<char>(v & 0xFF));
  b.append(static_cast<char>(v >> 8));
}
inline void packPut32(QByteArray& b, quint32 v) {
  packPut16(b, static_cast<quint16>(v & 0xFFFF));
  packPut16(b, static_cast<quint16>(v >> 16));
}

inline QByteArray buildPackZip(const QList<PackEntry>& entries) {
  QByteArray out, cd;
  for (const PackEntry& e : entries) {
    const QByteArray name = e.name.toUtf8();
    QByteArray body = e.data;
    quint16 method = 0;
    if (e.deflate && !e.data.isEmpty()) {
      const QByteArray z = qCompress(e.data, 6);  // 4 byte length, zlib header (2), deflate data, adler-32 (4)
      body = z.mid(6, z.size() - 10);
      method = 8;
    }
    const quint32 crc = packCrc32(e.data);
    const quint32 offset = static_cast<quint32>(out.size());
    packPut32(out, 0x04034b50);
    packPut16(out, 20);
    packPut16(out, 0);
    packPut16(out, method);
    packPut16(out, 0);
    packPut16(out, 0x21);
    packPut32(out, crc);
    packPut32(out, static_cast<quint32>(body.size()));
    packPut32(out, static_cast<quint32>(e.data.size()));
    packPut16(out, static_cast<quint16>(name.size()));
    packPut16(out, 0);
    out += name;
    out += body;
    packPut32(cd, 0x02014b50);
    packPut16(cd, (3 << 8) | 20);
    packPut16(cd, 20);
    packPut16(cd, 0);
    packPut16(cd, method);
    packPut16(cd, 0);
    packPut16(cd, 0x21);
    packPut32(cd, crc);
    packPut32(cd, static_cast<quint32>(body.size()));
    packPut32(cd, static_cast<quint32>(e.data.size()));
    packPut16(cd, static_cast<quint16>(name.size()));
    packPut16(cd, 0);
    packPut16(cd, 0);
    packPut16(cd, 0);
    packPut16(cd, 0);
    packPut32(cd, e.mode << 16);
    packPut32(cd, offset);
    cd += name;
  }
  const quint32 cdOff = static_cast<quint32>(out.size());
  out += cd;
  packPut32(out, 0x06054b50);
  packPut16(out, 0);
  packPut16(out, 0);
  packPut16(out, static_cast<quint16>(entries.size()));
  packPut16(out, static_cast<quint16>(entries.size()));
  packPut32(out, static_cast<quint32>(cd.size()));
  packPut32(out, cdOff);
  packPut16(out, 0);
  return out;
}

// The fake engine package: `engineExe` becomes java/bin/java (java.exe on Windows). topFolder empty = files at the zip root.
inline QByteArray makeEngineZip(const QString& engineExe, const QString& topFolder = QString(), bool deflate = false) {
  QFile f(engineExe);
  QByteArray exe;
  if (f.open(QIODevice::ReadOnly)) exe = f.readAll();
  const QString p = topFolder.isEmpty() ? QString() : topFolder + QLatin1Char('/');
#ifdef Q_OS_WIN
  const QString javaName = QStringLiteral("java/bin/java.exe");
#else
  const QString javaName = QStringLiteral("java/bin/java");
#endif
  QList<PackEntry> es;
  es.append({p + QStringLiteral("UPR-FVX.jar"), QByteArray(), 0100644, false});
  es.append({p + javaName, exe, 0100755, deflate});
  es.append({p + QStringLiteral("data/readme.txt"), QByteArray("synthetic engine package\n").repeated(40), 0100644, deflate});
  return buildPackZip(es);
}

inline QString sha256Hex(const QByteArray& d) { return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex()); }

inline QString writeEngineZip(const QString& dir, const QString& engineExe, const QString& topFolder = QString(), bool deflate = false, QByteArray* bytesOut = nullptr) {
  const QByteArray z = makeEngineZip(engineExe, topFolder, deflate);
  if (bytesOut) *bytesOut = z;
  const QString path = dir + QStringLiteral("/engine.zip");
  QFile out(path);
  if (!out.open(QIODevice::WriteOnly) || out.write(z) != z.size()) return {};
  return path;
}

}  // namespace uzume::testutil
