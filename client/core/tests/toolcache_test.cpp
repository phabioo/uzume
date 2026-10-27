// ToolCache: install from a fake engine package, checksum refusal, layout check, versions, protection. Synthetic data only.
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "engine_pack.h"
#include "toolcache.h"

using namespace uzume;
using namespace uzume::testutil;

class ToolCacheTest : public QObject {
  Q_OBJECT
  static QString fakeExe(const QString& dir) {
    const QString p = dir + QStringLiteral("/fake-java");
    QFile f(p);
    f.open(QIODevice::WriteOnly);
    f.write(QByteArray("#!/bin/sh\nexit 0\n"));
    return p;
  }

 private slots:
  void installsBothLayouts() {
    QTemporaryDir t;
    ToolCache cache(t.filePath(QStringLiteral("tools")));
    for (const QString& top : {QString(), QStringLiteral("UPR_FVX-v1_6_2-Linux_x86")}) {
      QByteArray bytes;
      const QString z = writeEngineZip(t.path(), fakeExe(t.path()), top, top.isEmpty(), &bytes);
      QString err;
      const QString version = top.isEmpty() ? QStringLiteral("1.6.1") : QStringLiteral("1.6.2");
      QCOMPARE(cache.installZip(QStringLiteral("randomizer"), version, QStringLiteral("linux-x64"), z, sha256Hex(bytes), &err), ToolCache::InstallResult::Ok);
      QVERIFY2(err.isEmpty(), qPrintable(err));
      const auto l = cache.layout(QStringLiteral("randomizer"), version, QStringLiteral("linux-x64"));
      QVERIFY(l.has_value());
      QVERIFY(QFileInfo(l->javaPath).isFile());
      QVERIFY(QFileInfo(l->workDir + QLatin1Char('/') + l->jarName).isFile());
#ifndef Q_OS_WIN
      QVERIFY(QFileInfo(l->javaPath).isExecutable());
#endif
    }
    QCOMPARE(cache.versions(QStringLiteral("randomizer"), QStringLiteral("linux-x64")), (QStringList{QStringLiteral("1.6.2"), QStringLiteral("1.6.1")}));
    QVERIFY(cache.versions(QStringLiteral("randomizer"), QStringLiteral("windows-x64")).isEmpty());
    QVERIFY(cache.totalBytes() > 0);
  }

  void checksumMismatchIsRefused() {
    QTemporaryDir t;
    ToolCache cache(t.filePath(QStringLiteral("tools")));
    const QString z = writeEngineZip(t.path(), fakeExe(t.path()));
    QString err;
    QCOMPARE(cache.installZip(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64"), z, QString(64, QLatin1Char('0')), &err),
             ToolCache::InstallResult::HashMismatch);
    QVERIFY(!cache.isReady(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64")));
    QVERIFY(!QFileInfo(cache.packageDir(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64"))).exists());
  }

  void packageWithoutEngineIsRefused() {
    QTemporaryDir t;
    ToolCache cache(t.filePath(QStringLiteral("tools")));
    const QByteArray zip = buildPackZip({{QStringLiteral("readme.txt"), QByteArray("x"), 0100644, false}});
    QFile f(t.filePath(QStringLiteral("p.zip")));
    f.open(QIODevice::WriteOnly);
    f.write(zip);
    f.close();
    QCOMPARE(cache.installZip(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64"), f.fileName(), sha256Hex(zip)),
             ToolCache::InstallResult::LayoutInvalid);
    QVERIFY(!cache.isReady(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64")));
  }

  void invalidIdsAndPlatformNames() {
    QCOMPARE(ToolCache::normalizePlatform(QStringLiteral("linux-x86_64")), QStringLiteral("linux-x64"));
    QCOMPARE(ToolCache::normalizePlatform(QStringLiteral("windows-x86_64")), QStringLiteral("windows-x64"));
    QCOMPARE(ToolCache::normalizePlatform(QStringLiteral("linux-arm64")), QStringLiteral("linux-arm64"));
    ToolCache cache(QStringLiteral("/tmp/none"));
    QVERIFY(cache.packageDir(QStringLiteral("../x"), QStringLiteral("1"), QStringLiteral("p")).isEmpty());
    QVERIFY(cache.packageDir(QStringLiteral("randomizer"), QStringLiteral(".."), QStringLiteral("p")).isEmpty());
  }

  void protectionBlocksRemoval() {
    QTemporaryDir t;
    ToolCache cache(t.filePath(QStringLiteral("tools")));
    QByteArray bytes;
    const QString z = writeEngineZip(t.path(), fakeExe(t.path()), QString(), false, &bytes);
    QCOMPARE(cache.installZip(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64"), z, sha256Hex(bytes)), ToolCache::InstallResult::Ok);
    cache.protect(QStringLiteral("randomizer"), QStringLiteral("1.0.0"));
    QVERIFY(!cache.removeVersion(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64")));
    QVERIFY(cache.isReady(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64")));
    cache.unprotect(QStringLiteral("randomizer"), QStringLiteral("1.0.0"));
    QVERIFY(cache.removeVersion(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64")));
    QVERIFY(!cache.isReady(QStringLiteral("randomizer"), QStringLiteral("1.0.0"), QStringLiteral("linux-x64")));
  }
};

QTEST_APPLESS_MAIN(ToolCacheTest)
#include "toolcache_test.moc"
