// extractZip: stored and deflated entries, unix modes, hostile names. Synthetic data only.
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "engine_pack.h"
#include "zipextract.h"

using namespace uzume;
using namespace uzume::testutil;

class ZipExtractTest : public QObject {
  Q_OBJECT
  static QString writeZip(const QString& dir, const QList<PackEntry>& es) {
    const QString p = dir + QStringLiteral("/t.zip");
    QFile f(p);
    f.open(QIODevice::WriteOnly);
    f.write(buildPackZip(es));
    return p;
  }

 private slots:
  void storedAndDeflated() {
    QTemporaryDir t;
    QByteArray big;
    for (int i = 0; i < 20000; ++i) big.append(static_cast<char>((i * 7) % 251));
    const QByteArray text = QByteArray("synthetic line\n").repeated(500);
    const QString z = writeZip(t.path(), {{QStringLiteral("a/stored.bin"), big, 0100644, false},
                                          {QStringLiteral("a/deflated.bin"), big, 0100644, true},
                                          {QStringLiteral("b/text.txt"), text, 0100644, true},
                                          {QStringLiteral("bin/run"), QByteArray("x"), 0100755, false},
                                          {QStringLiteral("empty.txt"), QByteArray(), 0100644, false}});
    QString err;
    QVERIFY2(extractZip(z, t.filePath(QStringLiteral("out")), &err), qPrintable(err));
    const auto read = [&](const QString& rel) {
      QFile f(t.filePath(QStringLiteral("out/") + rel));
      f.open(QIODevice::ReadOnly);
      return f.readAll();
    };
    QCOMPARE(read(QStringLiteral("a/stored.bin")), big);
    QCOMPARE(read(QStringLiteral("a/deflated.bin")), big);
    QCOMPARE(read(QStringLiteral("b/text.txt")), text);
    QVERIFY(QFileInfo(t.filePath(QStringLiteral("out/empty.txt"))).isFile());
#ifndef Q_OS_WIN
    QVERIFY(QFileInfo(t.filePath(QStringLiteral("out/bin/run"))).isExecutable());
    QVERIFY(!QFileInfo(t.filePath(QStringLiteral("out/a/stored.bin"))).isExecutable());
#endif
  }

  void hostileNamesAreRefused() {
    for (const QString& name : {QStringLiteral("../evil"), QStringLiteral("/abs"), QStringLiteral("a/../../evil"), QStringLiteral("a\\b"), QStringLiteral("c:x")}) {
      QTemporaryDir t;
      const QString z = writeZip(t.path(), {{name, QByteArray("x"), 0100644, false}});
      QString err;
      QVERIFY2(!extractZip(z, t.filePath(QStringLiteral("out")), &err), qPrintable(name));
      QVERIFY(!QFileInfo(t.filePath(QStringLiteral("evil"))).exists());
    }
  }

  void duplicateAndDamaged() {
    QTemporaryDir t;
    QString err;
    QVERIFY(!extractZip(writeZip(t.path(), {{QStringLiteral("a"), QByteArray("1"), 0100644, false}, {QStringLiteral("A"), QByteArray("2"), 0100644, false}}),
                        t.filePath(QStringLiteral("o1")), &err));
    // Flip a byte of the stored content: the checksum catches it.
    QByteArray z = buildPackZip({{QStringLiteral("a"), QByteArray("hello"), 0100644, false}});
    const qsizetype at = z.indexOf("hello");
    z[at] = 'j';
    QFile f(t.filePath(QStringLiteral("bad.zip")));
    f.open(QIODevice::WriteOnly);
    f.write(z);
    f.close();
    QVERIFY(!extractZip(f.fileName(), t.filePath(QStringLiteral("o2")), &err));
    QVERIFY(err.contains(QStringLiteral("checksum")));
    QVERIFY(!extractZip(t.filePath(QStringLiteral("missing.zip")), t.filePath(QStringLiteral("o3")), &err));
  }

  void limits() {
    QTemporaryDir t;
    const QString z = writeZip(t.path(), {{QStringLiteral("a"), QByteArray(5000, 'x'), 0100644, true}});
    ZipExtractLimits lim;
    lim.maxFileBytes = 1000;
    QString err;
    QVERIFY(!extractZip(z, t.filePath(QStringLiteral("out")), &err, lim));
  }
};

QTEST_APPLESS_MAIN(ZipExtractTest)
#include "zipextract_test.moc"
