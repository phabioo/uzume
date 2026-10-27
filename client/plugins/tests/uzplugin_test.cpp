// .uzplugin reading (contract section 1): zip validation with synthetic archives built in the test, limits, manifest,
// entry and api checks, SIGNATURE.json with a test key (never the release key), extraction.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "uzplugin.h"
#include "uzplugin_testutil.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

namespace {

const QByteArray kManifest = R"({"id":"net.test.plugin","name":"T","version":"1.0.0","api":1,"entry":"main.lua","permissions":["memory.read"]})";

QList<ZipEntrySpec> base() { return {{"manifest.json", kManifest}, {"main.lua", "-- lua\n"}}; }

QList<QByteArray> keys(const TestKey& k) { return {k.pub}; }

}  // namespace

class UzPluginTest : public QObject {
  Q_OBJECT

  static UzPluginResult read(const QList<ZipEntrySpec>& e, const QList<QByteArray>& trusted = {}) { return readUzPlugin(buildZip(e), trusted); }

 private slots:
  void validUnsigned() {
    const QByteArray file = buildZip(base() + QList<ZipEntrySpec>{{"i18n/de-\xC3\xBC.json", "{}"}, {"assets/", ""}});
    const UzPluginResult r = readUzPlugin(file, {});
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.package.manifest.id, QString("net.test.plugin"));
    QCOMPARE(r.package.files.size(), 3);  // directory entry ignored
    QVERIFY(r.package.signature == SignatureState::None);
    QVERIFY(!r.package.official());
    QCOMPARE(r.package.sha256, sha256Hex(file));
    QCOMPARE(r.package.size, file.size());
  }

  void deflateAndStore() {
    ZipEntrySpec big{"data/big.bin", QByteArray(100000, 'a'), true};
    ZipEntrySpec stored{"data/small.bin", "xyz", false};
    const UzPluginResult r = read(base() + QList<ZipEntrySpec>{big, stored});
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.package.files["data/big.bin"], QByteArray(100000, 'a'));
    QCOMPARE(r.package.files["data/small.bin"], QByteArray("xyz"));
  }

  void zipCommentIsAllowed() {
    const UzPluginResult r = readUzPlugin(buildZip(base(), "built by a test"), {});
    QVERIFY2(r.ok, qPrintable(r.error));
  }

  void notAZip() {
    QVERIFY(!readUzPlugin(QByteArray("this is not a zip"), {}).ok);
    QVERIFY(!readUzPlugin(QByteArray(), {}).ok);
    const QByteArray good = buildZip(base());
    QVERIFY(!readUzPlugin(good.left(good.size() - 5), {}).ok);  // truncated: no end record
    QByteArray damaged = good;
    damaged[0] = 'X';  // local header of the first entry
    QVERIFY(!readUzPlugin(damaged, {}).ok);
  }

  void badNames_data() {
    QTest::addColumn<QString>("name");
    QTest::newRow("traversal") << "../evil.lua";
    QTest::newRow("traversal nested") << "a/../../evil.lua";
    QTest::newRow("dotdot middle") << "a/../b.lua";
    QTest::newRow("dot segment") << "a/./b.lua";
    QTest::newRow("leading dot slash") << "./b.lua";
    QTest::newRow("absolute") << "/abs.lua";
    QTest::newRow("backslash") << "a\\b.lua";
    QTest::newRow("backslash traversal") << "..\\evil.lua";
    QTest::newRow("drive letter") << "C:evil.lua";
    QTest::newRow("drive letter path") << "C:/evil.lua";
    QTest::newRow("stream") << "main.lua:stream";
    QTest::newRow("empty segment") << "a//b.lua";
    QTest::newRow("control char") << QString("a\x01").append("b.lua");
    QTest::newRow("too long") << QString(300, 'a') + ".lua";
    QTest::newRow("device name") << "nul";
    QTest::newRow("device name with extension") << "assets/CON.png";
    QTest::newRow("device name lua") << "aux.lua";
    QTest::newRow("numbered device") << "com1";
    QTest::newRow("numbered device lpt") << "Lpt9.txt";
    QTest::newRow("trailing dot") << "dir./x.lua";
    QTest::newRow("trailing dot file") << "trailing.";
  }
  void badNames() {
    QFETCH(QString, name);
    const UzPluginResult r = read(base() + QList<ZipEntrySpec>{{name, "x"}});
    QVERIFY2(!r.ok, qPrintable(name));
    QVERIFY(!r.error.isEmpty());
  }

  void maxNameLengthIsAllowed() {
    const QString name = QString(255 - 4, 'a') + ".lua";
    QCOMPARE(name.toUtf8().size(), 255);
    QVERIFY(read(base() + QList<ZipEntrySpec>{{name, "x"}}).ok);
  }

  void directoryEntriesAreIgnored() {
    // Directory entries are ignored, but their names are checked like file names (as the Hub does).
    QVERIFY(!read(base() + QList<ZipEntrySpec>{{"../", ""}}).ok);
    const UzPluginResult r = read(base() + QList<ZipEntrySpec>{{"a/b/", ""}, {"a/b/c.txt", "1"}});
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.package.files.size(), 3);
  }

  void duplicates() {
    QVERIFY(!read(base() + QList<ZipEntrySpec>{{"a.txt", "1"}, {"a.txt", "2"}}).ok);
    const UzPluginResult ci = read(base() + QList<ZipEntrySpec>{{"Main.lua", "x"}});  // differs from main.lua only in case
    QVERIFY(!ci.ok);
    QVERIFY(ci.error.contains("duplicate"));
    QVERIFY(!read(base() + QList<ZipEntrySpec>{{"data/A.bin", "1"}, {"DATA/a.bin", "2"}}).ok);
  }

  void fileAndFolderConflict() {
    QVERIFY(!read(base() + QList<ZipEntrySpec>{{"data", "1"}, {"data/x.txt", "2"}}).ok);
  }

  void symlinkIsRejected() {
    ZipEntrySpec link{"link.lua", "main.lua", false, true};
    const UzPluginResult r = read(base() + QList<ZipEntrySpec>{link});
    QVERIFY(!r.ok);
    QVERIFY(r.error.contains("symbolic link"));
  }

  void tooManyEntries() {
    QList<ZipEntrySpec> e = base();
    for (int i = 0; i < 1998; ++i) e.append(ZipEntrySpec{QString("f/%1.txt").arg(i), "x"});
    QCOMPARE(e.size(), 2000);
    QVERIFY2(read(e).ok, "2000 entries are allowed");
    e.append(ZipEntrySpec{"f/over.txt", "x"});
    const UzPluginResult r = read(e);
    QVERIFY(!r.ok);
    QVERIFY(r.error.contains("2000"));
  }

  void directoryEntriesCountTowardsTheLimit() {
    QList<ZipEntrySpec> e = base();
    for (int i = 0; i < 1999; ++i) e.append(ZipEntrySpec{QString("d%1/").arg(i), ""});
    QVERIFY(!read(e).ok);
  }

  void unpackedSizeLimit() {
    // The headers are checked before any data is touched: two entries that claim 40 MiB each.
    ZipEntrySpec a{"a.bin", "x", false, false, 40ll << 20};
    ZipEntrySpec b{"b.bin", "x", false, false, 40ll << 20};
    const UzPluginResult r = read(base() + QList<ZipEntrySpec>{a, b});
    QVERIFY(!r.ok);
    QVERIFY2(r.error.contains("64 MiB"), qPrintable(r.error));
  }

  void exactlyAtTheUnpackedLimit() {
    // 64 MiB of real data in one stored entry is fine (the manifest and entry file are tiny and count too).
    const qint64 rest = kUzPluginMaxUnpackedBytes - kManifest.size() - 7;
    ZipEntrySpec big{"big.bin", QByteArray(static_cast<qsizetype>(rest), 'z'), true};
    const UzPluginResult r = read(base() + QList<ZipEntrySpec>{big});
    QVERIFY2(r.ok, qPrintable(r.error));
  }

  void fileSizeLimit() {
    const QByteArray huge(static_cast<qsizetype>(kUzPluginMaxFileBytes) + 1, 'x');
    const UzPluginResult r = readUzPlugin(huge, {});
    QVERIFY(!r.ok);
    QVERIFY(r.error.contains("32 MiB"));
  }

  void lyingSizeIsCaught() {
    // Declares 10 bytes, inflates to 100000: the inflater stops at the declared size, the entry is rejected.
    ZipEntrySpec bomb{"bomb.bin", QByteArray(100000, 'a'), true, false, 10};
    const UzPluginResult r = read(base() + QList<ZipEntrySpec>{bomb});
    QVERIFY(!r.ok);
    // Stored entry whose data is longer than declared.
    ZipEntrySpec lie{"lie.bin", QByteArray(500, 'a'), false, false, 10};
    QVERIFY(!read(base() + QList<ZipEntrySpec>{lie}).ok);
  }

  void crcMismatch() {
    ZipEntrySpec bad{"bad.bin", "hello", false, false, -1, 0x12345678u, true};
    const UzPluginResult r = read(base() + QList<ZipEntrySpec>{bad});
    QVERIFY(!r.ok);
    QVERIFY(r.error.contains("checksum"));
  }

  void manifestAndEntryChecks() {
    QVERIFY(!read({{"main.lua", "x"}}).ok);  // no manifest
    QVERIFY(!read({{"manifest.json", "{not json"}, {"main.lua", "x"}}).ok);
    // entry file missing
    QVERIFY(!read({{"manifest.json", kManifest}}).ok);
    const UzPluginResult noEntry = read({{"manifest.json", kManifest}, {"other.lua", "x"}});
    QVERIFY(!noEntry.ok);
    QVERIFY(noEntry.error.contains("main.lua"));
    // api 2
    QByteArray api2 = kManifest;
    api2.replace("\"api\":1", "\"api\":2");
    const UzPluginResult r2 = read({{"manifest.json", api2}, {"main.lua", "x"}});
    QVERIFY(!r2.ok);
    QVERIFY(r2.error.contains("API"));
    // invalid id, version, entry
    QByteArray badId = kManifest;
    badId.replace("net.test.plugin", "Not A Plugin");
    QVERIFY(!read({{"manifest.json", badId}, {"main.lua", "x"}}).ok);
    QByteArray badVer = kManifest;
    badVer.replace("1.0.0", "one");
    QVERIFY(!read({{"manifest.json", badVer}, {"main.lua", "x"}}).ok);
    QByteArray badEntry = kManifest;
    badEntry.replace("main.lua", "../main.lua");
    QVERIFY(!read({{"manifest.json", badEntry}, {"main.lua", "x"}}).ok);
    // manifest in a subfolder is not the manifest
    QVERIFY(!read({{"sub/manifest.json", kManifest}, {"sub/main.lua", "x"}}).ok);
  }

  // ---- signature

  void signatureMessageFormat() {
    QMap<QString, QString> h;
    h.insert("main.lua", QString(64, 'b'));
    h.insert("manifest.json", QString(64, 'a'));
    h.insert("B/x", QString(64, 'c'));
    h.insert(QString::fromUtf8("\xC3\xA9.txt"), QString(64, 'd'));  // e-acute sorts after ASCII by bytes
    h.insert("a/b", QString(64, 'e'));
    QByteArray expect = "uzume-plugin-v1\n";
    expect += QByteArray(64, 'c') + "  B/x\n";
    expect += QByteArray(64, 'e') + "  a/b\n";
    expect += QByteArray(64, 'b') + "  main.lua\n";
    expect += QByteArray(64, 'a') + "  manifest.json\n";
    expect += QByteArray(64, 'd') + "  \xC3\xA9.txt\n";
    QCOMPARE(pluginSignatureMessage(h), expect);
  }

  void validSignatureIsOfficial() {
    const TestKey key = fixedTestKey();
    FbSpec s;
    s.sign = true;
    s.extra["i18n/en.json"] = "{}";
    const UzPluginResult r = readUzPlugin(makeUzPlugin(s), keys(key));
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY2(r.package.signature == SignatureState::Valid, qPrintable(r.package.signatureError));
    QVERIFY(r.package.official());
  }

  void unsignedIsNotOfficial() {
    const UzPluginResult r = readUzPlugin(makeUzPlugin(FbSpec{}), keys(fixedTestKey()));
    QVERIFY(r.ok);
    QVERIFY(r.package.signature == SignatureState::None);
    QVERIFY(!r.package.official());
  }

  void untrustedKeyIsInvalid() {
    FbSpec s;
    s.sign = true;
    s.key = fixedTestKey(1);  // signed with another key
    const UzPluginResult r = readUzPlugin(makeUzPlugin(s), keys(fixedTestKey()));
    QVERIFY(r.ok);  // an invalid signature is reported, the file itself is still a valid plugin
    QVERIFY(r.package.signature == SignatureState::Invalid);
    QVERIFY(!r.package.official());
    QVERIFY(!r.package.signatureError.isEmpty());
    // No trusted keys at all
    FbSpec s2;
    s2.sign = true;
    QVERIFY(readUzPlugin(makeUzPlugin(s2), {}).package.signature == SignatureState::Invalid);
  }

  void tamperedFileIsInvalid() {
    const TestKey key = fixedTestKey();
    FbSpec s;
    QMap<QString, QByteArray> files = fbFiles(s);
    const QByteArray sig = signatureJson(files, key);
    files["main.lua"] = "os.execute('evil')";  // changed after signing
    files["SIGNATURE.json"] = sig;
    QList<ZipEntrySpec> e;
    for (auto it = files.begin(); it != files.end(); ++it) e.append(ZipEntrySpec{it.key(), it.value()});
    const UzPluginResult r = read(e, keys(key));
    QVERIFY(r.ok);
    QVERIFY(r.package.signature == SignatureState::Invalid);
    QVERIFY(r.package.signatureError.contains("main.lua"));
  }

  void extraOrMissingFilesInvalidate() {
    const TestKey key = fixedTestKey();
    FbSpec s;
    const QMap<QString, QByteArray> files = fbFiles(s);
    // extra file not covered by the signature
    {
      QMap<QString, QByteArray> f = files;
      const QByteArray sig = signatureJson(f, key);
      f["extra.txt"] = "not signed";
      f["SIGNATURE.json"] = sig;
      QList<ZipEntrySpec> e;
      for (auto it = f.begin(); it != f.end(); ++it) e.append(ZipEntrySpec{it.key(), it.value()});
      const UzPluginResult r = read(e, keys(key));
      QVERIFY(r.ok);
      QVERIFY(r.package.signature == SignatureState::Invalid);
    }
    // signature lists a file that is not in the archive
    {
      QMap<QString, QByteArray> f = files;
      f["gone.txt"] = "was signed";
      const QByteArray sig = signatureJson(f, key);
      f.remove("gone.txt");
      f["SIGNATURE.json"] = sig;
      QList<ZipEntrySpec> e;
      for (auto it = f.begin(); it != f.end(); ++it) e.append(ZipEntrySpec{it.key(), it.value()});
      const UzPluginResult r = read(e, keys(key));
      QVERIFY(r.ok);
      QVERIFY(r.package.signature == SignatureState::Invalid);
      QVERIFY(r.package.signatureError.contains("gone.txt"));
    }
  }

  void brokenSignatureFilesAreInvalid_data() {
    QTest::addColumn<QByteArray>("json");
    QTest::newRow("not json") << QByteArray("{nope");
    QTest::newRow("array") << QByteArray("[]");
    QTest::newRow("format 2") << QByteArray(R"({"format":2,"files":{},"signature":"AAAA"})");
    QTest::newRow("no files") << QByteArray(R"({"format":1,"signature":"AAAA"})");
    QTest::newRow("no signature") << QByteArray(R"({"format":1,"files":{}})");
    QTest::newRow("short signature") << QByteArray(R"({"format":1,"files":{},"signature":"AAAA"})");
    QTest::newRow("not base64") << QByteArray(R"({"format":1,"files":{},"signature":"!!!!"})");
  }
  void brokenSignatureFilesAreInvalid() {
    QFETCH(QByteArray, json);
    const UzPluginResult r = read(base() + QList<ZipEntrySpec>{{"SIGNATURE.json", json}}, keys(fixedTestKey()));
    QVERIFY(r.ok);
    QVERIFY(r.package.signature == SignatureState::Invalid);
    QVERIFY(!r.package.official());
  }

  void uppercaseHashesAreInvalid() {
    const TestKey key = fixedTestKey();
    FbSpec s;
    QMap<QString, QByteArray> f = fbFiles(s);
    QJsonObject listed;
    for (auto it = f.begin(); it != f.end(); ++it) listed.insert(it.key(), sha256Hex(it.value()).toUpper());
    const QByteArray sig = QJsonDocument(QJsonObject{{"format", 1}, {"files", listed}, {"signature", QString(88, 'A')}}).toJson();
    f["SIGNATURE.json"] = sig;
    QList<ZipEntrySpec> e;
    for (auto it = f.begin(); it != f.end(); ++it) e.append(ZipEntrySpec{it.key(), it.value()});
    QVERIFY(read(e, keys(key)).package.signature == SignatureState::Invalid);
  }

  // Known answer: Ed25519 is deterministic, so the Hub test can verify exactly this vector (seed 0x00..0x1f).
  void knownAnswerVector() {
    const TestKey key = fixedTestKey();
    QCOMPARE(key.pub.toBase64(), QByteArray("A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg="));
    QMap<QString, QByteArray> files;
    files["manifest.json"] = kManifest;
    files["main.lua"] = "-- lua\n";
    QMap<QString, QString> h;
    for (auto it = files.begin(); it != files.end(); ++it) h.insert(it.key(), sha256Hex(it.value()));
    const QByteArray msg = pluginSignatureMessage(h);
    QCOMPARE(msg.left(16), QByteArray("uzume-plugin-v1\n"));
    QCOMPARE(ed25519Sign(key, msg).toBase64(), QByteArray("oJfjgZSOzLHD5cDbKOkqU06+fME0niaRpl3CqqAC/EHtKqh683SRiwzSIA43bOgvu7caoP0yhS6LnvBgo4iZCA=="));
  }

  // Vector written by the Hub (server/internal/pluginpkg/testdata): same message, same signature, same file.
  void hubVector() {
    QFile vf(QString(UZ_PLUGIN_TESTDATA) + "/signature-vector.json");
    QVERIFY(vf.open(QIODevice::ReadOnly));
    const QJsonObject v = QJsonDocument::fromJson(vf.readAll()).object();
    const QByteArray pub = QByteArray::fromBase64(v["public_key_base64"].toString().toLatin1());
    QCOMPARE(fixedTestKey().pub, pub);
    QMap<QString, QString> h;
    for (const QJsonValue& f : v["files"].toArray()) {
      QCOMPARE(sha256Hex(f["body"].toString().toUtf8()), f["sha256"].toString());
      h.insert(f["path"].toString(), f["sha256"].toString());
    }
    const QByteArray msg = pluginSignatureMessage(h);
    QCOMPARE(msg, v["signed_message"].toString().toUtf8());
    QCOMPARE(ed25519Sign(fixedTestKey(), msg).toBase64(), v["signature_base64"].toString().toLatin1());
    QFile pf(QString(UZ_PLUGIN_TESTDATA) + "/vector.uzplugin");
    QVERIFY(pf.open(QIODevice::ReadOnly));
    const QByteArray vfile = pf.readAll();
    const UzPluginResult r = readUzPlugin(vfile, {pub});
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY2(r.package.signature == SignatureState::Valid, qPrintable(r.package.signatureError));
    QVERIFY(readUzPlugin(vfile, {fixedTestKey(1).pub}).package.signature == SignatureState::Invalid);
  }

  // ---- extraction

  void extractWritesTheTree() {
    FbSpec s;
    s.extra["i18n/en.json"] = "{\"a\":1}";
    s.extra["assets/img/x.png"] = "png";
    const UzPluginResult r = readUzPlugin(makeUzPlugin(s), {});
    QVERIFY(r.ok);
    QTemporaryDir dir;
    const QString dest = dir.filePath("out");
    QString err;
    QVERIFY2(extractUzPlugin(r.package, dest, &err), qPrintable(err));
    QVERIFY(QFileInfo(dest + "/manifest.json").isFile());
    QVERIFY(QFileInfo(dest + "/main.lua").isFile());
    QFile f(dest + "/assets/img/x.png");
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("png"));
    QVERIFY(!extractUzPlugin(r.package, dest, &err));  // refuses to write over existing files
  }
};

QTEST_APPLESS_MAIN(UzPluginTest)
#include "uzplugin_test.moc"
