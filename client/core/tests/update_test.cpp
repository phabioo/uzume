// Updater unit tests: SemVer, index validation, selection, protocol compatibility, signature (throwaway keys),
// install root, installer command line, download/stage/apply with file:// index and installer.
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include <openssl/evp.h>

#include <unistd.h>
#include "installroot.h"
#include "playersettings.h"
#include "semver.h"
#include "updater.h"
#include "updateindex.h"
#include "updaterollback.h"
#include "updatesig.h"

using namespace uzume::update;

namespace {

struct KeyPair {
  QByteArray seed;
  QByteArray pub;
};

KeyPair makeKey() {
  KeyPair k;
  k.seed.resize(32);
  for (int i = 0; i < 32; ++i) k.seed[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
  EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                reinterpret_cast<const unsigned char*>(k.seed.constData()), 32);
  size_t len = 32;
  k.pub.resize(32);
  EVP_PKEY_get_raw_public_key(pkey, reinterpret_cast<unsigned char*>(k.pub.data()), &len);
  EVP_PKEY_free(pkey);
  return k;
}

QByteArray signLine(const KeyPair& k, const QByteArray& index, const QByteArray& context = {}) {
  const QByteArray msg = context.isEmpty() ? index : context + '\n' + index;
  EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                reinterpret_cast<const unsigned char*>(k.seed.constData()), 32);
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey);
  size_t siglen = 64;
  QByteArray sig(64, 0);
  EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &siglen,
                 reinterpret_cast<const unsigned char*>(msg.constData()), static_cast<size_t>(msg.size()));
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  if (!context.isEmpty()) {
    return (QStringLiteral("ed25519ctx ") + keyId(k.pub) + QLatin1Char(' ') + QString::fromLatin1(context) + QLatin1Char(' ') +
            QString::fromLatin1(sig.toBase64()) + QLatin1Char('\n')).toLatin1();
  }
  return (QStringLiteral("ed25519 ") + keyId(k.pub) + QLatin1Char(' ') + QString::fromLatin1(sig.toBase64()) + QLatin1Char('\n')).toLatin1();
}

QJsonObject artifact(const QString& name, const QByteArray& data, const QString& url, const QString& kind = QStringLiteral("msi")) {
  return {{"platform", "windows-x64"}, {"kind", kind}, {"name", name}, {"size", data.size()},
          {"sha256", QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())}, {"url", url}};
}

QJsonObject release(const QString& product, const QString& channel, const QString& version, const QJsonArray& arts,
                    int proto = 1, int minProto = 1) {
  return {{"product", product}, {"channel", channel}, {"version", version},
          {"commit", "0123456789abcdef0123456789abcdef01234567"}, {"published_at", "2026-10-07T10:00:00Z"},
          {"notes_url", "https://github.com/phabioo/uzume/releases/tag/v" + version},
          {"protocol_version", proto}, {"min_protocol_version", minProto}, {"artifacts", arts}, {"future_field", 42}};
}

QByteArray indexJson(const QJsonArray& releases) {
  return QJsonDocument(QJsonObject{{"schema", 1}, {"generated_at", "2026-10-07T10:00:00Z"}, {"releases", releases}})
      .toJson(QJsonDocument::Compact);
}

QJsonArray oneArt(const QString& v = QStringLiteral("x")) {
  return {artifact("setup-" + v + ".exe", QByteArray("data-") + v.toUtf8(), "https://example.invalid/setup-" + v + ".exe")};
}

Index parseOk(const QByteArray& json) {
  const ParseResult r = parseIndex(json);
  if (!r.ok) qWarning() << r.error;
  return r.index;
}

constexpr QFileDevice::Permissions kMode755 = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                              QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                              QFileDevice::ExeOther;

bool writeBytes(const QString& path, const QByteArray& b) {
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(b) == b.size();
}

}  // namespace

class UpdateTest : public QObject {
  Q_OBJECT

 private slots:
  // ---------------------------------------------------------------- SemVer
  void semverPrecedence() {
    const QStringList ordered = {"0.3.0-dev", "0.3.0-test.9", "0.3.0-test.10", "0.3.0-test.10.1", "0.3.0-test.a",
                                 "0.3.0", "0.3.1", "0.10.0", "1.0.0-alpha", "1.0.0"};
    for (int i = 0; i + 1 < ordered.size(); ++i) {
      QVERIFY2(compareVersions(ordered[i], ordered[i + 1]).value() < 0, qPrintable(ordered[i] + " < " + ordered[i + 1]));
      QVERIFY(compareVersions(ordered[i + 1], ordered[i]).value() > 0);
    }
  }
  void coreVersionVerdictMajorOnly() {
    using V = uzume::update::CoreVersionVerdict;
    const auto v = [](const char* core, const char* expected) {
      return uzume::update::coreVersionVerdict(QString::fromLatin1(core), QString::fromLatin1(expected));
    };
    QCOMPARE(v("1.2.3", "1.2.3"), V::Compatible);
    QCOMPARE(v("1.2.3", "1.4.0"), V::Warn);
    QCOMPARE(v("1.2.3", "1.2.9"), V::Warn);
    QCOMPARE(v("1.2.3", "2.0.0"), V::Block);
    QCOMPARE(v("v1.2.3", "2.0.0"), V::Block);
    QCOMPARE(v("v1.2.3", "1.9.0"), V::Warn);
    QCOMPARE(v("1.2.3", "v1.2.3"), V::Compatible);
    QCOMPARE(v("1.2.3", ""), V::Compatible);  // empty Hub version: any version
    QCOMPARE(v("", "1.2.3"), V::Warn);        // unparsable core version: warn, never block
    QCOMPARE(v("nightly", "2.0.0"), V::Warn);
    // Buildbot build ids and core-reported "SVN"/git hashes (ADR 0020 D3) never block.
    QCOMPARE(v("2026.10.09", "2027.01.02"), V::Warn);
    QCOMPARE(v("2026.10.10", "2027.11.12"), V::Warn);
    QCOMPARE(v("2026.10.10.2", "2.0.0"), V::Warn);
    QCOMPARE(v("SVN", "2026.10.09"), V::Warn);
    QCOMPARE(v("git1a2b3c4", "2026.10.09"), V::Warn);
    QCOMPARE(v("2026.10.09", "2026.10.09"), V::Compatible);
    QCOMPARE(v("1.2.3", "latest"), V::Warn);
  }

  void semverBuildMetadataIgnored() {
    QCOMPARE(compareVersions("1.2.3+abc", "1.2.3+def").value(), 0);
    QCOMPARE(SemVer::parse("1.2.3-rc.1+build")->toString(), QString("1.2.3-rc.1"));
  }
  void semverRejectsInvalid() {
    for (const char* v : {"", "dev", "1.2", "1.2.3.4", "01.2.3", "1.2.3-", "1.2.3-01", "v1.2.3", "1.2.3-a..b", " 1.2.3"}) {
      QVERIFY2(!SemVer::parse(QString::fromLatin1(v)), v);
    }
    QVERIFY(!compareVersions("dev", "1.0.0"));
  }

  // ---------------------------------------------------------------- index
  void indexValid() {
    const ParseResult r = parseIndex(indexJson({release("player", "beta", "0.3.0-test.5", oneArt()),
                                                release("hub", "stable", "0.3.0", oneArt())}));
    QVERIFY(r.ok);
    QCOMPARE(r.index.releases.size(), 2);
    QVERIFY(r.index.skipped.isEmpty());  // unknown field "future_field" is ignored
  }
  void indexNotes() {
    const QString good = QStringLiteral("### Added\n- Something new \u00fc\n\t- tab");
    const auto withNotes = [&](const QString& v, const QJsonValue& notes) {
      QJsonObject r = release("player", "stable", v, oneArt(v));
      r.insert("notes", notes);
      return r;
    };
    const ParseResult r = parseIndex(indexJson({withNotes("1.0.0", good), withNotes("1.1.0", QString(4001, 'a')),
                                                withNotes("1.2.0", QStringLiteral("a\x1b[31mb")),
                                                withNotes("1.3.0", QString(QChar(0x20AC)).repeated(1334)),
                                                withNotes("1.4.0", 42), release("player", "stable", "1.5.0", oneArt("5")),
                                                withNotes("1.6.0", QString(4000, 'a')),
                                                withNotes("1.7.0", QStringLiteral("a\r\nb"))}));
    QVERIFY(r.ok);
    QCOMPARE(r.index.releases.size(), 8);  // invalid notes drop the field, never the release
    QVERIFY(r.index.skipped.isEmpty());
    QCOMPARE(releaseNotes(r.index, "player", "1.0.0"), good);
    QCOMPARE(releaseNotes(r.index, "player", "1.0.0"), r.index.releases[0].notes);
    QVERIFY(r.index.releases[0].toJson().contains("notes"));
    QCOMPARE(releaseNotes(r.index, "player", "1.1.0"), QString());
    QCOMPARE(releaseNotes(r.index, "player", "1.2.0"), QString());
    QCOMPARE(releaseNotes(r.index, "player", "1.3.0"), QString());  // 4002 bytes
    QCOMPARE(releaseNotes(r.index, "player", "1.4.0"), QString());
    QCOMPARE(releaseNotes(r.index, "player", "1.5.0"), QString());
    QVERIFY(!r.index.releases[5].toJson().contains("notes"));
    QCOMPARE(releaseNotes(r.index, "player", "1.6.0").size(), 4000);
    QCOMPARE(releaseNotes(r.index, "player", "1.7.0"), QString());
    QCOMPARE(releaseNotes(r.index, "hub", "1.0.0"), QString());     // other product
    QCOMPARE(releaseNotes(r.index, "player", "9.9.9"), QString());  // unknown version
  }
  void indexUnknownSchema() {
    const QByteArray j = R"({"schema":2,"releases":[]})";
    const ParseResult r = parseIndex(j);
    QVERIFY(!r.ok);
    QVERIFY(!parseIndex("not json").ok);
    QVERIFY(!parseIndex(R"({"schema":1})").ok);
  }
  void indexDuplicateRejectsAll() {
    const ParseResult r = parseIndex(indexJson({release("player", "beta", "0.3.0-test.5", oneArt()),
                                                release("player", "beta", "0.3.0-test.5+x", oneArt())}));
    QVERIFY(!r.ok);
    QVERIFY(r.index.releases.isEmpty());
    // same version in different channel/product is fine
    QVERIFY(parseIndex(indexJson({release("player", "beta", "0.3.0", oneArt()), release("player", "stable", "0.3.0", oneArt()),
                                  release("hub", "stable", "0.3.0", oneArt())})).ok);
  }
  void indexInvalidEntriesSkipped() {
    QJsonObject badSha = release("player", "beta", "0.3.0-test.1", oneArt());
    QJsonArray arts{artifact("a.exe", "x", "https://e.invalid/a.exe")};
    QJsonObject a0 = arts[0].toObject();
    a0["sha256"] = "ABC";
    badSha["artifacts"] = QJsonArray{a0};
    QJsonObject httpUrl = release("player", "beta", "0.3.0-test.2", QJsonArray{artifact("a.exe", "x", "http://e.invalid/a.exe")});
    QJsonObject badVer = release("player", "beta", "latest", oneArt());
    QJsonObject badProduct = release("tool", "test", "0.3.0-test.3", oneArt());
    QJsonObject testChannel = release("player", "test", "0.3.0-test.10", oneArt());
    QJsonObject badChannel = release("player", "dev", "0.3.0-test.4", oneArt());
    QJsonObject badProto = release("player", "beta", "0.3.0-test.6", oneArt(), 1, 2);
    QJsonObject noArt = release("player", "beta", "0.3.0-test.7", {});
    QJsonObject badPath = release("player", "beta", "0.3.0-test.8", QJsonArray{artifact("../evil.exe", "x", "https://e.invalid/a.exe")});
    QJsonObject good = release("player", "beta", "0.3.0-test.9", oneArt());
    const ParseResult r = parseIndex(indexJson({badSha, httpUrl, badVer, badProduct, badChannel, badProto, noArt, badPath, testChannel, good}));
    QVERIFY(r.ok);
    QCOMPARE(r.index.releases.size(), 1);
    QCOMPARE(r.index.skipped.size(), 9);  // "test" is no valid channel any more
    QCOMPARE(r.index.releases[0].version, QString("0.3.0-test.9"));
  }
  void indexFileUrlOnlyWhenAllowed() {
    const QByteArray j = indexJson({release("player", "beta", "0.3.0-test.1", QJsonArray{artifact("a.exe", "x", "file:///tmp/a.exe")})});
    QCOMPARE(parseIndex(j, false).index.releases.size(), 0);
    QCOMPARE(parseIndex(j, true).index.releases.size(), 1);
  }

  // ---------------------------------------------------------------- selection
  void selectionChannels() {
    const Index idx = parseOk(indexJson({release("player", "beta", "0.3.0-test.7", oneArt("t7")),
                                         release("player", "stable", "0.3.0", oneArt("s")),
                                         release("player", "beta", "0.4.0-test.1", oneArt("t41"))}));
    SelectionInput in;
    in.platform = "windows-x64";
    in.kind = "msi";
    in.currentVersion = "0.3.0-dev";
    in.channel = Channel::Stable;
    Selection s = selectRelease(idx, in);
    QCOMPARE(s.status, Selection::Status::Available);
    QCOMPARE(s.release.version, QString("0.3.0"));  // stable never sees test
    in.channel = Channel::Beta;
    s = selectRelease(idx, in);
    QCOMPARE(s.release.version, QString("0.4.0-test.1"));  // highest of test + stable
    QCOMPARE(s.artifact.name, QString("setup-t41.exe"));
  }
  void selectionDevIsOff() {
    const Index idx = parseOk(indexJson({release("player", "stable", "0.3.0", oneArt())}));
    SelectionInput in;
    in.platform = "windows-x64";
    in.kind = "msi";
    in.currentVersion = "0.3.0-dev";
    in.channel = channelFromCompiled("dev");
    QCOMPARE(in.channel, Channel::Off);
    QCOMPARE(selectRelease(idx, in).status, Selection::Status::Disabled);
    in.channel = Channel::Stable;
    in.currentVersion = "dev";  // not SemVer: updater disabled
    QCOMPARE(selectRelease(idx, in).status, Selection::Status::Disabled);
  }
  void selectionNoDowngradeAndEqual() {
    const Index idx = parseOk(indexJson({release("player", "stable", "0.3.0", oneArt()), release("player", "beta", "0.3.1-test.2", oneArt("b"))}));
    SelectionInput in;
    in.platform = "windows-x64";
    in.kind = "msi";
    in.channel = Channel::Beta;
    in.currentVersion = "0.3.0";
    QCOMPARE(selectRelease(idx, in).release.version, QString("0.3.1-test.2"));
    in.currentVersion = "0.3.1-test.2";
    QCOMPARE(selectRelease(idx, in).status, Selection::Status::UpToDate);
    in.currentVersion = "0.5.0";  // running newer than everything: no automatic downgrade
    QCOMPARE(selectRelease(idx, in).status, Selection::Status::UpToDate);
    in.currentVersion = "0.3.1-test.2+local";  // build metadata ignored
    QCOMPARE(selectRelease(idx, in).status, Selection::Status::UpToDate);
  }
  void selectionProductPlatformKind() {
    QJsonArray zipOnly{artifact("p.zip", "z", "https://e.invalid/p.zip", "zip")};
    QJsonObject wrongPlatform = artifact("x.exe", "x", "https://e.invalid/x.exe");
    wrongPlatform["platform"] = "linux-amd64";
    const Index idx = parseOk(indexJson({release("hub", "stable", "9.0.0", oneArt()),
                                         release("player", "stable", "0.4.0", zipOnly),
                                         release("player", "stable", "0.5.0", QJsonArray{wrongPlatform})}));
    SelectionInput in;
    in.platform = "windows-x64";
    in.kind = "msi";
    in.channel = Channel::Stable;
    in.currentVersion = "0.3.0";
    QCOMPARE(selectRelease(idx, in).status, Selection::Status::UpToDate);  // no installer for windows-x64
  }
  void protocolCompatibility() {
    Release r;
    r.protocolVersion = 2;
    r.minProtocolVersion = 2;
    QVERIFY(protocolCompatible(r, std::nullopt));  // no Hub known
    QVERIFY(!protocolCompatible(r, HubProtocol{1, 1}));  // Hub older than candidate's min
    r.minProtocolVersion = 1;
    QVERIFY(protocolCompatible(r, HubProtocol{1, 1}));
    QVERIFY(protocolCompatible(r, HubProtocol{2, 1}));
    r.protocolVersion = 1;
    QVERIFY(!protocolCompatible(r, HubProtocol{3, 2}));  // candidate older than the Hub's min
  }
  void selectionIncompatibleIsShownNotChosen() {
    const Index idx = parseOk(indexJson({release("player", "stable", "0.4.0", oneArt("a"), 3, 3),
                                         release("player", "stable", "0.3.5", oneArt("b"), 1, 1)}));
    SelectionInput in;
    in.platform = "windows-x64";
    in.kind = "msi";
    in.channel = Channel::Stable;
    in.currentVersion = "0.3.0";
    in.hub = HubProtocol{1, 1};
    Selection s = selectRelease(idx, in);
    QCOMPARE(s.status, Selection::Status::Available);
    QCOMPARE(s.release.version, QString("0.3.5"));  // compatible one wins although lower
    const Index only = parseOk(indexJson({release("player", "stable", "0.4.0", oneArt("a"), 3, 3)}));
    s = selectRelease(only, in);
    QCOMPARE(s.status, Selection::Status::Incompatible);
    QCOMPARE(s.release.version, QString("0.4.0"));
    const QJsonObject j = selectionToJson(s, in, {});
    QCOMPARE(j["status"].toString(), QString("incompatible"));
    QCOMPARE(j["release"].toObject()["version"].toString(), QString("0.4.0"));
  }

  // ---------------------------------------------------------------- signature
  void signatureGoodWrongTampered() {
    const KeyPair k = makeKey();
    const KeyPair other = makeKey();
    const QByteArray idx = indexJson({release("player", "stable", "0.3.0", oneArt())});
    const QByteArray sig = signLine(k, idx);
    QVERIFY(verifyIndexSignature(idx, sig, {k.pub}).ok);
    QVERIFY(verifyIndexSignature(idx, sig, {other.pub, k.pub}).ok);
    // wrong key
    SigCheck c = verifyIndexSignature(idx, sig, {other.pub});
    QVERIFY(!c.ok);
    QVERIFY(c.error.contains("not trusted"));
    // same key id but different key material cannot happen; a signature made with another key but claiming k's id fails
    QByteArray forged = signLine(other, idx);
    forged.replace(keyId(other.pub).toLatin1(), keyId(k.pub).toLatin1());
    QVERIFY(!verifyIndexSignature(idx, forged, {k.pub}).ok);
    // tampered bytes
    QByteArray bad = idx;
    bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] ^ 1);
    c = verifyIndexSignature(bad, sig, {k.pub});
    QVERIFY(!c.ok);
    QVERIFY(c.error.contains("does not match"));
    // formats
    QVERIFY(!verifyIndexSignature(idx, "garbage", {k.pub}).ok);
    QVERIFY(!verifyIndexSignature(idx, sig + sig, {k.pub}).ok);
    QVERIFY(!verifyIndexSignature(idx, sig, {}).ok);
    QVERIFY(!verifyIndexSignature(idx, QByteArray("rsa ") + sig.mid(8), {k.pub}).ok);
  }
  void signatureDomainSeparation() {
    const KeyPair k = makeKey();
    const QByteArray idx = indexJson({release("player", "stable", "0.3.0", oneArt())});
    const QByteArray ctx = signLine(k, idx, kUpdatesIndexContext);
    QVERIFY(verifyIndexSignature(idx, ctx, {k.pub}).ok);
    // A signature of another document type (plugin index) must not verify as an updates index.
    SigCheck c = verifyIndexSignature(idx, signLine(k, idx, "uzume-plugin-index-v1"), {k.pub});
    QVERIFY(!c.ok);
    // Relabelling the context breaks the signature, tampering the bytes too.
    QByteArray relabelled = ctx;
    relabelled.replace("uzume-updates-index-v1", "uzume-plugin-index-v1");
    QVERIFY(!verifyIndexSignature(idx, relabelled, {k.pub}).ok);
    QByteArray bad = idx;
    bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] ^ 1);
    QVERIFY(!verifyIndexSignature(bad, ctx, {k.pub}).ok);
    // A legacy line still verifies (transition).
    QVERIFY(verifyIndexSignature(idx, signLine(k, idx), {k.pub}).ok);
  }
  void indexExpiry() {
    QJsonObject root = QJsonDocument::fromJson(indexJson({release("player", "stable", "0.3.0", oneArt())})).object();
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-19T12:00:00Z"), Qt::ISODate);
    ParseResult pr = parseIndex(QJsonDocument(root).toJson());
    QVERIFY(pr.ok);
    QVERIFY(checkIndexFresh(pr.index, now).isEmpty());  // no expires_at: accepted (transition)
    root.insert(QStringLiteral("expires_at"), QStringLiteral("2026-10-20T00:00:00Z"));
    pr = parseIndex(QJsonDocument(root).toJson());
    QVERIFY(pr.ok);
    QVERIFY(checkIndexFresh(pr.index, now).isEmpty());
    QVERIFY(checkIndexFresh(pr.index, now.addDays(2)).contains("expired"));
    root.insert(QStringLiteral("expires_at"), QStringLiteral("garbage"));
    pr = parseIndex(QJsonDocument(root).toJson());
    QVERIFY(!checkIndexFresh(pr.index, now).isEmpty());
  }
  void trustedKeys() {
    const QList<QByteArray> d = defaultTrustedKeys();
    QCOMPARE(d.size(), 1);
    QCOMPARE(d[0].size(), 32);
    QCOMPARE(keyId(d[0]).size(), 16);
    QString err;
    QCOMPARE(parsePublicKeys({"AAAA", ""}, &err).size(), 0);
    QVERIFY(!err.isEmpty());
  }

  // ---------------------------------------------------------------- install root / installer command
  void installRootBinLayout() {
    QTemporaryDir t;
    QVERIFY(QDir().mkpath(t.filePath("app/bin")));
    // bin without launcher in the parent: stays flat
    QCOMPARE(installRootFor(t.filePath("app/bin")), QDir(t.filePath("app/bin")).absolutePath());
    QVERIFY(writeBytes(t.filePath("app/Uzume.exe"), "x"));
    QCOMPARE(installRootFor(t.filePath("app/bin")), QDir(t.filePath("app")).absolutePath());
    QCOMPARE(installRootFor(t.filePath("app")), QDir(t.filePath("app")).absolutePath());
    QVERIFY(QDir().mkpath(t.filePath("app/tools")));
    QCOMPARE(installRootFor(t.filePath("app/tools")), QDir(t.filePath("app/tools")).absolutePath());
    QVERIFY(directoryWritable(t.filePath("app")));
    QVERIFY(!directoryWritable(t.filePath("nope")));
  }

  // ---------------------------------------------------------------- settings
  void channelAliasAndOldSettings() {
    QCOMPARE(parseChannel("test"), std::optional<Channel>(Channel::Beta));
    QCOMPARE(parseChannel("beta"), std::optional<Channel>(Channel::Beta));
    QCOMPARE(channelName(Channel::Beta), QString("beta"));
    QCOMPARE(channelFromCompiled("test"), Channel::Beta);
    QTemporaryDir t;
    QVERIFY(QDir().mkpath(t.filePath("settings")));
    QVERIFY(writeBytes(t.filePath("settings/player.json"), R"({"update_channel":"test","other":1})"));
    uzume::PlayerSettings s(t.path());
    QCOMPARE(s.updateChannel(), QString("beta"));
    QVERIFY(s.setUpdateAutoInstall(true));  // any save rewrites the old value
    QJsonObject o;
    {
      // Closed before the next save: on Windows QSaveFile cannot replace an open file.
      QFile f(t.filePath("settings/player.json"));
      QVERIFY(f.open(QIODevice::ReadOnly));
      o = QJsonDocument::fromJson(f.readAll()).object();
    }
    QCOMPARE(o["update_channel"].toString(), QString("beta"));
    QCOMPARE(o["other"].toInt(), 1);
    QVERIFY(s.setUpdateChannel("test"));
    QCOMPARE(s.updateChannel(), QString("beta"));
  }
  void settingsRoundTrip() {
    QTemporaryDir t;
    {
      uzume::PlayerSettings s(t.path());
      QCOMPARE(s.updateChannel(), QString());
      QVERIFY(!s.updateAutoInstall());
      QVERIFY(!s.setUpdateChannel("dev"));
      QVERIFY(s.setUpdateChannel("beta"));
      QVERIFY(s.setUpdateAutoInstall(false));
    }
    uzume::PlayerSettings s(t.path());
    QCOMPARE(s.updateChannel(), QString("beta"));
    QCOMPARE(s.updateAutoInstall(), std::optional<bool>(false));
    QVERIFY(s.setUpdateChannel(""));
    QCOMPARE(uzume::PlayerSettings(t.path()).updateChannel(), QString());
  }

  // ---------------------------------------------------------------- download / stage / apply
  void managerStagesAndApplies() {
    Env env;
    const QByteArray data = "installer-bytes-0.3.1";
    env.publish(data, "0.3.1-test.3", /*sizeOverride*/ -1, /*shaOverride*/ QString());
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    QStringList launched;
    auto mp = env.manager(settings);
    UpdateManager& m = *mp;
    m.setLauncher([&](const QString& prog, const QStringList& args) { launched << prog << args.join(' '); return true; });
    QSignalSpy quit(&m, &UpdateManager::quitRequested);
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Ready, 5000);  // test + auto: staged in the background
    QVERIFY(launched.isEmpty());  // not applied during the running session
    const QString staged = QDir(stagingRoot(env.dir.filePath("data"))).filePath("0.3.1-test.3/" + env.installerName);
    QVERIFY(QFileInfo::exists(staged));
    QCOMPARE(m.availableVersion(), QString("0.3.1-test.3"));

    // Next start: a verified staged installer is applied before the main window.
    auto m2p = env.manager(settings);
    UpdateManager& m2 = *m2p;
    m2.setLauncher([&](const QString& prog, const QStringList& args) { launched << prog << args.join(' '); return true; });
    QSignalSpy quit2(&m2, &UpdateManager::quitRequested);
    QVERIFY(m2.applyStagedAtStart());
    QCOMPARE(quit2.count(), 1);
    QCOMPARE(launched.size(), 2);
    QVERIFY(launched[0].endsWith("Uzume.exe"));  // temp copy of the launcher runs msiexec
    QVERIFY(launched[0] != env.dir.filePath("Uzume.exe"));
    QVERIFY(launched[1].startsWith("--apply-msi-update "));
    QVERIFY(launched[1].contains(QString(staged).replace(QLatin1Char('/'), QLatin1Char('\\'))));  // msiexec wants backslashes
  }
  void managerDoesNotLoopAfterFailedAttempt() {
    Env env;
    env.publish("installer-bytes-0.3.1", "0.3.1-test.3");
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    int launches = 0;
    auto mp = env.manager(settings);
    mp->setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
    mp->checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(mp->state(), UpdateManager::State::Ready, 5000);
    QVERIFY(mp->applyStagedAtStart());  // first start: applied once
    QCOMPARE(launches, 1);
    const QString rec = QDir(stagingRoot(env.dir.filePath("data"))).filePath("0.3.1-test.3/attempted.json");
    QVERIFY(QFileInfo::exists(rec));
    // The installer failed, the old Player starts again (same baseDir/currentVersion): no second automatic attempt.
    auto m2p = env.manager(settings);
    UpdateManager& m2 = *m2p;
    m2.setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
    QVERIFY(!m2.applyStagedAtStart());
    QCOMPARE(launches, 1);
    QCOMPARE(m2.state(), UpdateManager::State::Error);
    QVERIFY(m2.statusText().contains("could not be installed automatically"));
    // A user click may still retry.
    m2.installNow();
    QCOMPARE(launches, 2);
    QCOMPARE(m2.state(), UpdateManager::State::Applying);
  }
  void managerBusyBlocksApply() {
    Env env;
    env.publish("installer-bytes", "0.3.1-test.3");
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    int launches = 0;
    bool busy = true;
    auto mp = env.manager(settings);
    UpdateManager& m = *mp;
    m.setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
    m.setBusyProvider([&]() { return busy; });
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Ready, 5000);
    QVERIFY(!m.applyStagedAtStart());
    m.installNow();
    QCOMPARE(launches, 0);
    QVERIFY(m.statusText().contains("game"));
    busy = false;
    m.installNow();
    QCOMPARE(launches, 1);
  }
  void managerStableNeedsConfirmation() {
    Env env;
    env.publish("installer-bytes", "0.3.1", -1, QString(), "stable");
    uzume::PlayerSettings settings(env.dir.filePath("data"));  // compiled channel stable
    int launches = 0;
    auto mp = env.manager(settings, "stable");
    UpdateManager& m = *mp;
    m.setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
    QVERIFY(!m.autoInstallEffective());
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Available, 5000);
    QVERIFY(!QDir(stagingRoot(env.dir.filePath("data"))).exists("0.3.1"));  // nothing downloaded without confirmation
    QVERIFY(!m.applyStagedAtStart());
    QCOMPARE(launches, 0);
    m.installNow();  // user confirmed
    QTRY_COMPARE_WITH_TIMEOUT(launches, 1, 5000);
    QCOMPARE(m.state(), UpdateManager::State::Applying);
  }
  void managerStableIgnoresAutoSettingAtStart() {
    Env env;
    env.publish("installer-bytes", "0.3.1", -1, QString(), "stable");
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateAutoInstall(true);  // stable never installs without confirmation
    auto mp = env.manager(settings, "stable");
    UpdateManager& m = *mp;
    QVERIFY(!m.autoInstallEffective());
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Available, 5000);
  }
  void managerRejectsSizeAndShaMismatch() {
    for (int mode = 0; mode < 2; ++mode) {
      Env env;
      env.publish("installer-bytes", "0.3.1-test.3", mode == 0 ? 5 : -1, mode == 1 ? QString(64, 'a') : QString());
      uzume::PlayerSettings settings(env.dir.filePath("data"));
      settings.setUpdateChannel("beta");
      int launches = 0;
      auto mp = env.manager(settings);
    UpdateManager& m = *mp;
      m.setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
      m.checkNow();
      QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Error, 5000);
      QVERIFY(!QFileInfo::exists(QDir(stagingRoot(env.dir.filePath("data"))).filePath("0.3.1-test.3/" + env.installerName)));
      QVERIFY(!m.applyStagedAtStart());
      QCOMPARE(launches, 0);
    }
  }
  void managerRejectsBadIndexSignature() {
    Env env;
    env.publish("installer-bytes", "0.3.1-test.3");
    QByteArray idx = readAll(env.dir.filePath("updates-index.json"));
    idx.replace("0.3.1-test.3", "0.3.9-test.3");  // tampered after signing
    QVERIFY(writeBytes(env.dir.filePath("updates-index.json"), idx));
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    auto mp = env.manager(settings);
    UpdateManager& m = *mp;
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Error, 5000);
    QVERIFY(m.statusText().contains("signature"));
  }
  // ---------------------------------------------------------------- Linux AppImage (0.24)
  void platformAndKindPerBuild() {
    QCOMPARE(QString(playerPlatformFor(PlayerOs::Windows)), QString("windows-x64"));
    QCOMPARE(QString(playerKindFor(PlayerOs::Windows)), QString("msi"));
    QCOMPARE(QString(playerPlatformFor(PlayerOs::Linux)), QString("linux-x64"));
    QCOMPARE(QString(playerKindFor(PlayerOs::Linux)), QString("appimage"));
    QCOMPARE(QString(playerPlatformFor(PlayerOs::Other)), QString("windows-x64"));  // unchanged elsewhere
    QCOMPARE(QString(playerKindFor(PlayerOs::Other)), QString("msi"));
#if defined(Q_OS_WIN)
    QCOMPARE(QString(playerPlatform()), QString("windows-x64"));
    QCOMPARE(QString(playerKind()), QString("msi"));
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    QCOMPARE(QString(playerPlatform()), QString("linux-x64"));
    QCOMPARE(QString(playerKind()), QString("appimage"));
#endif
    // Selection follows the build's pair: an index with both artifacts yields the one of this platform.
    QJsonObject lin = artifact("Uzume-0.5.0-x86_64.AppImage", "a", "https://e.invalid/a", "appimage");
    lin["platform"] = "linux-x64";
    const Index idx = parseOk(indexJson({release("player", "stable", "0.5.0", QJsonArray{artifact("u.msi", "m", "https://e.invalid/m"), lin})}));
    SelectionInput in;
    in.platform = "windows-x64";
    in.kind = "msi";
    in.channel = Channel::Stable;
    in.currentVersion = "0.3.0";
    in.platform = "linux-x64";
    in.kind = "appimage";
    QCOMPARE(selectRelease(idx, in).artifact.name, QString("Uzume-0.5.0-x86_64.AppImage"));
    in.platform = "windows-x64";
    in.kind = "msi";
    QCOMPARE(selectRelease(idx, in).artifact.name, QString("u.msi"));
  }
  void appImageRelaunchArgumentsDropOneShotFlags() {
    QCOMPARE(appImageRelaunchArguments({"--smoke-test", "--data-dir", "/d", "--mcp", "uzume://join/x", "--dev-allow-http", "--setup-local-hub"}),
             (QStringList{"--data-dir", "/d", "--dev-allow-http"}));
  }
#ifdef Q_OS_UNIX
  void appImageApplySupportedNeedsWritableImageFolder() {
    QTemporaryDir tmp;
    uzume::PlayerSettings settings(tmp.filePath("data"));
    UpdateManager::Config c;
    c.baseDir = tmp.filePath("data");
    c.currentVersion = "0.3.0";
    c.compiledChannel = "beta";
    c.platform = "linux-x64";
    c.kind = "appimage";
    c.trustedKeys = {makeKey().pub};
    UpdateManager m(c, &settings);
    const QByteArray oldEnv = qgetenv("APPIMAGE");
    auto restore = qScopeGuard([&]() { oldEnv.isNull() ? qunsetenv("APPIMAGE") : qputenv("APPIMAGE", oldEnv); });
#ifdef Q_OS_LINUX
    const QString notAppImage = "Automatic installation is only available for the Windows installer and the Linux AppImage.";
    qunsetenv("APPIMAGE");
    QVERIFY(!m.applySupported());
    QCOMPARE(m.applyUnsupportedReason(), notAppImage);
    qputenv("APPIMAGE", tmp.filePath("missing.AppImage").toLocal8Bit());  // names no file
    QVERIFY(!m.applySupported());
    QCOMPARE(m.applyUnsupportedReason(), notAppImage);
    QVERIFY(QDir().mkpath(tmp.filePath("img")));
    const QString image = tmp.filePath("img/Uzume.AppImage");
    QVERIFY(writeBytes(image, "old"));
    qputenv("APPIMAGE", image.toLocal8Bit());
    QVERIFY(m.applySupported());
    QVERIFY(m.applyUnsupportedReason().isEmpty());
    QVERIFY(QFile::setPermissions(tmp.filePath("img"), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    auto unlock = qScopeGuard([&]() { QFile::setPermissions(tmp.filePath("img"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); });
    if (directoryWritable(tmp.filePath("img"))) QSKIP("running with rights that ignore directory permissions (root)");
    QVERIFY(!m.applySupported());
    QCOMPARE(m.applyUnsupportedReason(), QString("The folder of the AppImage is not writable."));
#endif
  }
  void replaceAppImageDirect() {
    QTemporaryDir tmp;
    const QString image = tmp.filePath("Uzume.AppImage");
    const QString staged = tmp.filePath("staged.bin");
    QVERIFY(writeBytes(image, "old"));
    QVERIFY(writeBytes(staged, "new-image"));
    const QString sha = QString::fromLatin1(QCryptographicHash::hash("new-image", QCryptographicHash::Sha256).toHex());
    QString why;
    QVERIFY(!replaceAppImage(staged, image, QString(64, 'a'), 9, &why));  // wrong hash: nothing changes
    QCOMPARE(readAll(image), QByteArray("old"));
    QVERIFY(!QFileInfo::exists(tmp.filePath(".Uzume.AppImage.part")));
    QVERIFY(!QFileInfo::exists(image + ".previous"));
    QVERIFY(replaceAppImage(staged, image, sha, 9, &why));
    QCOMPARE(readAll(image), QByteArray("new-image"));
    QCOMPARE(readAll(image + ".previous"), QByteArray("old"));
    QVERIFY(!QFileInfo::exists(tmp.filePath(".Uzume.AppImage.part")));
    // The old image was 0644 (writeBytes): its bits are kept and the owner can execute.
    QCOMPARE(QFileInfo(image).permissions() & 0x7077, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                                      QFileDevice::ReadGroup | QFileDevice::ReadOther);
  }
  void replaceAppImageKeepsPermissionBits() {
    QTemporaryDir tmp;
    const QString image = tmp.filePath("Uzume.AppImage");
    const QString staged = tmp.filePath("staged.bin");
    QVERIFY(writeBytes(image, "old"));
    QVERIFY(writeBytes(staged, "new-image"));
    const QFileDevice::Permissions mode = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                          QFileDevice::ExeGroup;  // 0750
    QVERIFY(QFile::setPermissions(image, mode));
    const QString sha = QString::fromLatin1(QCryptographicHash::hash("new-image", QCryptographicHash::Sha256).toHex());
    QString why;
    QVERIFY(replaceAppImage(staged, image, sha, 9, &why));
    QCOMPARE(QFileInfo(image).permissions() & 0x7077, mode);
  }
  // Runs the real watcher script with a fake "AppImage" shell script as the new image.
  void relaunchWatcherRestoresOnlyAfterFailure() {
    for (const int rc : {0, 3}) {
      QTemporaryDir tmp;
      const QString app = tmp.filePath("Uzume.AppImage");
      const QString runs = tmp.filePath("runs");
      const auto script = [&](const QByteArray& body) {
        QVERIFY(writeBytes(app, "#!/bin/sh\n" + body));
        QVERIFY(QFile::setPermissions(app, kMode755));
      };
      // The new image appends to `runs` and exits with rc; the restored previous one exits 0.
      script("echo new >> '" + runs.toLocal8Bit() + "'; exit " + QByteArray::number(rc) + "\n");
      const QString prev = appImagePreviousPath(app);
      QVERIFY(writeBytes(prev, "#!/bin/sh\necho prev >> '" + runs.toLocal8Bit() + "'; exit 0\n"));
      QVERIFY(QFile::setPermissions(prev, kMode755));
      const InstallerCommand cmd = appImageRelaunchCommand(app, 999999999, {});
      QProcess p;
      p.start(cmd.program, cmd.args);
      QVERIFY(p.waitForFinished(20000));
      const QByteArray log = readAll(runs);
      if (rc == 0) {  // a quick clean exit of the new image is not a failed start
        QCOMPARE(log, QByteArray("new\n"));
        QVERIFY(QFileInfo::exists(prev));
        QCOMPARE(p.exitCode(), 0);
      } else {
        QCOMPARE(log, QByteArray("new\nprev\n"));
        QVERIFY(!QFileInfo::exists(prev));
      }
      QVERIFY(!QFileInfo::exists(appImageStartMarker(app)));
    }
  }
  // #343: the detached watcher must not inherit non-close-on-exec descriptors of the parent.
  void detachedStartClosesInheritedFds() {
    QTemporaryDir tmp;
    const QString out = tmp.filePath("fds");
    QFile leak(tmp.filePath("leak"));
    QVERIFY(leak.open(QIODevice::WriteOnly));
    const int inherited = ::dup(leak.handle());  // dup() clears close-on-exec: this one would leak into the child
    QVERIFY(inherited > 2);
    auto closeIt = qScopeGuard([&]() { ::close(inherited); });
    QVERIFY(startDetachedWithoutInheritedFds("/bin/sh", {"-c", "ls -l /proc/self/fd > \"$1\"", "x", out}));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out) && QFileInfo(out).size() > 0, 5000);
    const QString listing = QString::fromLatin1(readAll(out));  // `ls -l` shows each descriptor's target
    QVERIFY2(!listing.contains("/leak"), qPrintable(listing));
    QVERIFY(listing.contains(" 0 ->") && listing.contains(" 1 ->") && listing.contains(" 2 ->"));
  }
  void managerReplacesAppImage() {
    QTemporaryDir imgDir;
    const QString image = imgDir.filePath("Uzume.AppImage");
    QVERIFY(writeBytes(image, "old-image"));
    qputenv("APPIMAGE", image.toLocal8Bit());
    auto unset = qScopeGuard([]() { qunsetenv("APPIMAGE"); });
    Env env;
    env.appImage = true;
    const QByteArray data = "new-image-bytes-0.3.1";
    env.publish(data, "0.3.1-test.3");
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    QStringList launched;
    auto mp = env.manager(settings);
    UpdateManager& m = *mp;
    m.setLauncher([&](const QString& prog, const QStringList& args) { launched << prog << args.join('\n'); return true; });
    QSignalSpy quit(&m, &UpdateManager::quitRequested);
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Ready, 5000);
    QCOMPARE(readAll(image), QByteArray("old-image"));  // staged only, the image is untouched
    QVERIFY(m.applyStagedAtStart());
    QCOMPARE(quit.count(), 1);
    QCOMPARE(readAll(image), data);
    QCOMPARE(readAll(image + ".previous"), QByteArray("old-image"));
    QVERIFY(QFileInfo(image).permissions() & QFileDevice::ExeOwner);
    QVERIFY(QDir(imgDir.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
    QCOMPARE(launched.value(0), QString("/bin/sh"));
    QVERIFY(launched.value(1).contains(image));  // watcher args: image, previous, marker, old pid, original args
    QVERIFY(launched.value(1).contains(image + ".previous"));
  }
  // #325: the user-triggered install shows "Applying" at once and does the file work on a worker thread.
  void installNowReplacesAppImageInBackground() {
    QTemporaryDir imgDir;
    const QString image = imgDir.filePath("Uzume.AppImage");
    QVERIFY(writeBytes(image, "old-image"));
    qputenv("APPIMAGE", image.toLocal8Bit());
    auto unset = qScopeGuard([]() { qunsetenv("APPIMAGE"); });
    Env env;
    env.appImage = true;
    const QByteArray data = "new-image-bytes-0.3.1";
    env.publish(data, "0.3.1-test.3");
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    int launches = 0;
    auto mp = env.manager(settings);
    UpdateManager& m = *mp;
    m.setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
    QSignalSpy quit(&m, &UpdateManager::quitRequested);
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Ready, 5000);
    m.installNow();
    QCOMPARE(m.state(), UpdateManager::State::Applying);  // state flips before the file work finished
    QCOMPARE(quit.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(quit.count(), 1, 5000);
    QCOMPARE(launches, 1);
    QCOMPARE(readAll(image), data);
    QCOMPARE(readAll(image + ".previous"), QByteArray("old-image"));
  }
  void installNowAppImageFailureShowsError() {
    QTemporaryDir imgDir;
    const QString image = imgDir.filePath("Uzume.AppImage");
    QVERIFY(writeBytes(image, "old-image"));
    qputenv("APPIMAGE", image.toLocal8Bit());
    auto unset = qScopeGuard([]() { qunsetenv("APPIMAGE"); });
    Env env;
    env.appImage = true;
    env.publish("new-image-bytes-0.3.1", "0.3.1-test.3");
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    int launches = 0;
    auto mp = env.manager(settings);
    UpdateManager& m = *mp;
    m.setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Ready, 5000);
    // Corrupt the staged file: the install must end in an error, never launch the watcher.
    bool corrupted = false;
    QDirIterator it(env.dir.filePath("data/cache/updates"), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const QString f = it.next();
      if (f.endsWith(".json")) continue;
      QVERIFY(writeBytes(f, "tampered-bytes-of-other-size"));
      corrupted = true;
    }
    QVERIFY(corrupted);
    m.installNow();
    QTRY_VERIFY_WITH_TIMEOUT(m.state() != UpdateManager::State::Applying, 5000);
    QCOMPARE(launches, 0);
    QCOMPARE(readAll(image), QByteArray("old-image"));
  }
  void managerAppImageRejectionsLeaveNothingBehind() {
    for (int mode = 0; mode < 3; ++mode) {
      QTemporaryDir imgDir;
      const QString image = imgDir.filePath("Uzume.AppImage");
      QVERIFY(writeBytes(image, "old-image"));
      qputenv("APPIMAGE", image.toLocal8Bit());
      auto unset = qScopeGuard([]() { qunsetenv("APPIMAGE"); });
      Env env;
      env.appImage = true;
      env.publish("new-image", "0.3.1-test.3", mode == 0 ? 5 : -1, mode == 1 ? QString(64, 'a') : QString());
      if (mode == 2) {  // index tampered after signing
        QByteArray idx = readAll(env.dir.filePath("updates-index.json"));
        idx.replace("0.3.1-test.3", "0.3.9-test.3");
        QVERIFY(writeBytes(env.dir.filePath("updates-index.json"), idx));
      }
      uzume::PlayerSettings settings(env.dir.filePath("data"));
      settings.setUpdateChannel("beta");
      int launches = 0;
      auto mp = env.manager(settings);
      mp->setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
      mp->checkNow();
      QTRY_COMPARE_WITH_TIMEOUT(mp->state(), UpdateManager::State::Error, 5000);
      QVERIFY(!mp->applyStagedAtStart());
      QCOMPARE(launches, 0);
      QCOMPARE(readAll(image), QByteArray("old-image"));
      QVERIFY(!QFileInfo::exists(image + ".previous"));
      QVERIFY(QDir(imgDir.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
      QVERIFY(QDir(env.dir.filePath("data/cache/updates")).entryList({"*.part", "*"}, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
  }
#endif

  // ---------------------------------------------------------------- resolved default channel
  void defaultChannelResolvesStableWhenPromoted() {
    Env env;
    env.publishPromoted("0.3.0-dev", true);
    {
      uzume::PlayerSettings settings(env.dir.filePath("data"));
      auto mp = env.manager(settings);
      QCOMPARE(mp->effectiveChannel(), Channel::Beta);  // not resolved yet
      QSignalSpy spy(mp.get(), &UpdateManager::changed);
      mp->checkNow();
      QTRY_COMPARE_WITH_TIMEOUT(settings.updateChannelDefault(), QString("stable"), 5000);
      QCOMPARE(mp->effectiveChannel(), Channel::Stable);
      QVERIFY(!mp->autoInstallSetting());
      QVERIFY(settings.updateChannel().isEmpty());
    }
    uzume::PlayerSettings reloaded(env.dir.filePath("data"));  // persisted
    QCOMPARE(reloaded.updateChannelDefault(), QString("stable"));
    QCOMPARE(env.manager(reloaded)->effectiveChannel(), Channel::Stable);
  }
  void defaultChannelResolvesBetaAndStaysAfterPromotion() {
    Env env;
    env.publishPromoted("0.3.0-dev", false);
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    auto mp = env.manager(settings);
    mp->checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(settings.updateChannelDefault(), QString("beta"), 5000);
    QCOMPARE(mp->effectiveChannel(), Channel::Beta);
    QVERIFY(mp->autoInstallSetting());
    env.publishPromoted("0.3.0-dev", true);  // promoted later: never re-resolved
    mp->checkNow();
    QTRY_VERIFY_WITH_TIMEOUT(!mp->busy() && mp->state() != UpdateManager::State::Checking, 5000);
    QCOMPARE(settings.updateChannelDefault(), QString("beta"));
    QCOMPARE(mp->effectiveChannel(), Channel::Beta);
  }
  void defaultChannelExplicitWins() {
    Env env;
    env.publishPromoted("0.3.0-dev", true);
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    auto mp = env.manager(settings);
    mp->checkNow();
    QTRY_VERIFY_WITH_TIMEOUT(mp->state() != UpdateManager::State::Checking && mp->state() != UpdateManager::State::Idle, 5000);
    QVERIFY(settings.updateChannelDefault().isEmpty());  // not resolved at all
    QCOMPARE(mp->effectiveChannel(), Channel::Beta);
    // An already stored default is overridden by an explicit choice too.
    QVERIFY(settings.setUpdateChannelDefault("stable"));
    QCOMPARE(mp->effectiveChannel(), Channel::Beta);
    settings.setUpdateChannel("stable");
    QCOMPARE(mp->effectiveChannel(), Channel::Stable);
  }
  void defaultChannelIgnoredForStableAndDevBuilds() {
    Env env;
    env.publishPromoted("0.3.0-dev", true);
    for (const QString compiled : {QStringLiteral("stable"), QStringLiteral("dev")}) {
      uzume::PlayerSettings settings(env.dir.filePath("data-" + compiled));
      auto mp = env.manager(settings, compiled);
      mp->checkNow();
      QTRY_VERIFY_WITH_TIMEOUT(mp->state() != UpdateManager::State::Checking && mp->state() != UpdateManager::State::Idle, 5000);
      QVERIFY(settings.updateChannelDefault().isEmpty());
      QCOMPARE(mp->effectiveChannel(), compiled == "stable" ? Channel::Stable : Channel::Off);
    }
    uzume::PlayerSettings s2(env.dir.filePath("data-x"));
    QVERIFY(s2.setUpdateChannelDefault("beta"));
    QCOMPARE(env.manager(s2, "stable")->effectiveChannel(), Channel::Stable);
    QCOMPARE(env.manager(s2, "dev")->effectiveChannel(), Channel::Off);
    QVERIFY(!s2.setUpdateChannelDefault("bogus"));
  }
  void managerDevChannelIsOff() {
    Env env;
    env.publish("installer-bytes", "0.3.1-test.3");
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    auto mp = env.manager(settings, "dev");
    UpdateManager& m = *mp;
    QCOMPARE(m.effectiveChannel(), Channel::Off);
    m.checkNow();
    QCOMPARE(m.state(), UpdateManager::State::Disabled);
    QVERIFY(!m.applyStagedAtStart());
  }
  void stagedReplayAndTamperRejected() {
    Env env;
    env.publish("installer-bytes", "0.3.1-test.3");
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    auto mp = env.manager(settings);
    UpdateManager& m = *mp;
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Ready, 5000);
    const QString base = env.dir.filePath("data");
    const QString inst = QDir(stagingRoot(base)).filePath("0.3.1-test.3/" + env.installerName);
    QVERIFY(QFileInfo::exists(inst));
    QVERIFY(loadVerifiedStaged(base, "0.3.0-dev", env.keys, true, "windows-x64", "msi"));
    QVERIFY(writeBytes(inst, "installer-byteX"));
    QVERIFY(!loadVerifiedStaged(base, "0.3.0-dev", env.keys, true, "windows-x64", "msi"));
  }
  void stagedReplayAndWrongKeyRejected() {
    Env env;
    env.publish("installer-bytes", "0.3.1-test.3");
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    auto mp = env.manager(settings);
    mp->checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(mp->state(), UpdateManager::State::Ready, 5000);
    const QString base = env.dir.filePath("data");
    QVERIFY(!loadVerifiedStaged(base, "0.3.0-dev", {makeKey().pub}, true, "windows-x64", "msi"));  // wrong trust keys (also prunes)
    auto m2p = env.manager(settings);  // fresh manager (state Idle): stages again
    m2p->checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m2p->state(), UpdateManager::State::Ready, 5000);
    QVERIFY(loadVerifiedStaged(base, "0.3.0-dev", env.keys, true, "windows-x64", "msi").has_value());
    // The staged version must be strictly newer than the running one (replay of an old signed release).
    QVERIFY(!loadVerifiedStaged(base, "0.3.1-test.3", env.keys, true, "windows-x64", "msi"));
    QVERIFY(!loadVerifiedStaged(base, "0.9.0", env.keys, true, "windows-x64", "msi"));
  }

  // ---------------------------------------------------------------- update rollback (ADR 0029 D4)
  void rollbackCandidateSelection() {
    auto rel = [](const QString& v, const QString& ch, bool downgrade, int proto = 2, int minProto = 1) {
      QJsonObject o = release("player", ch, v, oneArt(v), proto, minProto);
      if (downgrade) o["msi_downgrade"] = true;
      return o;
    };
    const Index idx = parseOk(indexJson({rel("0.1.0", "stable", true), rel("0.2.0", "beta", true), rel("0.3.0", "stable", true),
                                         rel("0.3.0", "beta", true), rel("0.4.0", "beta", false),      // MSI refuses a downgrade
                                         rel("0.5.0", "beta", true, 1, 1), rel("0.6.0", "beta", true, 3, 3),  // speaks protocol 1 / 3 only
                                         rel("0.7.0", "beta", true), rel("0.8.0", "stable", true), rel("0.9.0", "beta", true),
                                         rel("1.0.0", "beta", true),                                   // the running version: not older
                                         release("hub", "stable", "0.0.5", oneArt("h"))}));
    RollbackQuery q;
    q.platform = "windows-x64";
    q.kind = "msi";
    q.currentVersion = "1.0.0";
    q.channel = Channel::Beta;
    q.protocolVersion = 2;
    QList<RollbackCandidate> c = rollbackCandidates(idx, q);
    QStringList versions;
    for (const auto& x : c) versions << x.release.version;
    // newest first, at most 5, no MSI without the downgrade flag, no release that does not speak protocol 2, no Hub release
    QCOMPARE(versions, (QStringList{"0.9.0", "0.8.0", "0.7.0", "0.3.0", "0.2.0"}));
    QCOMPARE(c[3].release.channel, QString("stable"));  // a version listed in two channels counts once, stable wins
    QVERIFY(!c[0].ran);

    // Stable only sees stable releases, plus the versions this device ran.
    q.channel = Channel::Stable;
    q.history = {{"0.9.0", "2026-09-01T10:00:00Z", "2026-09-30T10:00:00Z"}, {"0.1.0", "2026-08-01T10:00:00Z", "2026-08-20T10:00:00Z"}};
    versions.clear();
    c = rollbackCandidates(idx, q);
    for (const auto& x : c) versions << x.release.version;
    QCOMPARE(versions, (QStringList{"0.9.0", "0.8.0", "0.3.0", "0.1.0"}));
    QVERIFY(c[0].ran);
    QCOMPARE(c[0].first, QString("2026-09-01T10:00:00Z"));
    QVERIFY(!c[1].ran);

    // Channel off offers only what was run; the protocol check is skipped at 0; no version is invented.
    q.channel = Channel::Off;
    q.protocolVersion = 0;
    c = rollbackCandidates(idx, q);
    QCOMPARE(c.size(), 2);
    q.currentVersion = "not-semver";
    QVERIFY(rollbackCandidates(idx, q).isEmpty());
    // A platform without an MSI has nothing to install.
    q.currentVersion = "1.0.0";
    q.platform = "linux-amd64";
    QVERIFY(rollbackCandidates(idx, q).isEmpty());
  }
  void rollbackHistoryAndPause() {
    QList<HistoryEntry> h;
    const QDateTime t0 = QDateTime::fromString("2026-10-01T10:00:00Z", Qt::ISODate);
    QVERIFY(recordVersion(h, "0.3.0", t0));
    QVERIFY(!recordVersion(h, "0.3.0", t0.addSecs(600)));  // touched at most hourly
    QVERIFY(recordVersion(h, "0.3.0", t0.addSecs(7200)));
    QCOMPARE(h.size(), 1);
    QCOMPARE(h[0].first, QString("2026-10-01T10:00:00Z"));
    QCOMPARE(h[0].last, QString("2026-10-01T12:00:00Z"));
    QVERIFY(!recordVersion(h, "garbage", t0));
    for (int i = 0; i < kMaxHistory + 5; ++i) recordVersion(h, QString("1.%1.0").arg(i), t0);
    QCOMPARE(h.size(), kMaxHistory);
    QCOMPARE(h.last().version, QString("1.24.0"));
    QCOMPARE(parseHistory(historyToJson(h)).size(), h.size());
    QCOMPARE(parseHistory(QJsonArray{QJsonObject{{"version", "x"}}, QJsonObject{{"version", "1.0.0"}}, QJsonObject{{"version", "1.0.0"}}}).size(), 1);

    Pause p;
    p.from = "0.4.0";
    p.to = "0.3.0";
    p.at = "2026-10-01T10:00:00Z";
    p.prevAuto = 1;
    QVERIFY(parsePause(pauseToJson(p)).active());
    QCOMPARE(parsePause(pauseToJson(p)).prevAuto, 1);
    QVERIFY(!parsePause(QJsonObject{{"from", "nope"}}).active());
    QVERIFY(!parsePause({}).active());
    QCOMPARE(parsePause(QJsonObject{{"from", "0.4.0"}}).prevAuto, -1);
    QVERIFY(pauseEffective(p, "0.3.0"));
    QVERIFY(pauseEffective(p, "0.3.9-beta.1"));
    QVERIFY(!pauseEffective(p, "0.4.0"));
    QVERIFY(!pauseEffective(p, "0.5.0"));
    QVERIFY(!pauseEffective(Pause(), "0.1.0"));
    QVERIFY(skipEffective("0.4.0", "0.3.0"));
    QVERIFY(!skipEffective("0.4.0", "0.4.0"));
    QVERIFY(!skipEffective("", "0.1.0"));

    // The settings keep the shape and unknown keys.
    QTemporaryDir t;
    {
      uzume::PlayerSettings s(t.path());
      QVERIFY(s.setUpdateHistory(historyToJson(h)));
      QVERIFY(s.setUpdatePause(pauseToJson(p)));
      QVERIFY(s.setUpdateSkip("0.4.0"));
      QVERIFY(s.setUpdateAutoInstall(false));
    }
    uzume::PlayerSettings back(t.path());
    QCOMPARE(parseHistory(back.updateHistory()).size(), kMaxHistory);
    QCOMPARE(parsePause(back.updatePause()).from, QString("0.4.0"));
    QCOMPARE(back.updateSkip(), QString("0.4.0"));
    QVERIFY(back.setUpdatePause({}));
    QVERIFY(back.setUpdateSkip(QString()));
    QVERIFY(back.clearUpdateAutoInstall());
    uzume::PlayerSettings cleared(t.path());
    QVERIFY(cleared.updatePause().isEmpty());
    QVERIFY(cleared.updateSkip().isEmpty());
    QVERIFY(!cleared.updateAutoInstall());
  }
  void managerRollbackFlow() {
    Env env;
    env.publishReleases({{"0.1.0", "beta", false}, {"0.2.0", "beta", true}, {"0.3.0", "beta", true}});
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    settings.setUpdateAutoInstall(true);
    QStringList launched;
    bool busy = true;
    auto mp = env.manager(settings, "beta", "0.3.0");
    UpdateManager& m = *mp;
    m.setLauncher([&](const QString& prog, const QStringList& args) { launched << prog << args.join(' '); return true; });
    m.setBusyProvider([&]() { return busy; });
    QSignalSpy quit(&m, &UpdateManager::quitRequested);
    QVERIFY(m.rollbackOptions().isEmpty());  // nothing is known before the first check
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::UpToDate, 5000);
    // The running version is in the history; 0.1.0 has no downgrade-capable MSI.
    QCOMPARE(parseHistory(settings.updateHistory()).size(), 1);
    const QList<RollbackCandidate> options = m.rollbackOptions();
    QCOMPARE(options.size(), 1);
    QCOMPARE(options[0].release.version, QString("0.2.0"));

    // Never while a game runs: nothing changes.
    m.rollbackTo("0.2.0");
    QVERIFY(m.statusText().contains("game"));
    QVERIFY(launched.isEmpty());
    QVERIFY(!m.updatesPaused());
    QVERIFY(settings.updateSkip().isEmpty());
    QCOMPARE(*settings.updateAutoInstall(), true);
    // Not one of the offered versions (no downgrade-capable MSI, unknown, newer): nothing is downloaded or started.
    busy = false;
    for (const QString& v : {QString("0.1.0"), QString("9.9.9"), QString("0.3.0"), QString("junk")}) {
      m.rollbackTo(v);
      QCOMPARE(m.state(), UpdateManager::State::Error);
      QVERIFY(launched.isEmpty());
      QVERIFY(!m.updatesPaused());
    }
    QVERIFY(!QFileInfo::exists(rollbackStagingRoot(env.dir.filePath("data")) + "/0.1.0"));

    // Going back: downloaded, verified, paused, installer started, Player quits.
    m.rollbackTo("0.2.0");
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Applying, 5000);
    QCOMPARE(quit.count(), 1);
    QCOMPARE(launched.size(), 2);
    const QString msi = QDir(rollbackStagingRoot(env.dir.filePath("data"))).filePath("0.2.0/" + QString("uzume-0.2.0-windows-x64.msi"));
    QVERIFY(QFileInfo::exists(msi));
    QVERIFY(launched[1].startsWith("--apply-msi-update "));
    QVERIFY(launched[1].contains(QString(msi).replace(QLatin1Char('/'), QLatin1Char('\\'))));
    QVERIFY(launched[1].contains("--sha256 "));
    const Pause p = parsePause(settings.updatePause());
    QCOMPARE(p.from, QString("0.3.0"));
    QCOMPARE(p.to, QString("0.2.0"));
    QCOMPARE(p.prevAuto, 1);
    QCOMPARE(settings.updateSkip(), QString("0.3.0"));
    QCOMPARE(*settings.updateAutoInstall(), false);  // the older Player reads this and stays quiet too

    // The older Player starts: paused, 0.3.0 is listed but skipped and not installed automatically.
    env.publishReleases({{"0.1.0", "beta", false}, {"0.2.0", "beta", true}, {"0.3.0", "beta", true}});
    auto m2p = env.manager(settings, "beta", "0.2.0");
    UpdateManager& m2 = *m2p;
    m2.setLauncher([&](const QString& prog, const QStringList& args) { launched << prog << args.join(' '); return true; });
    QVERIFY(m2.updatesPaused());
    QCOMPARE(m2.skippedVersion(), QString("0.3.0"));
    QVERIFY(!QFileInfo::exists(msi));  // the installer of the version now running is not kept
    const int launchesBefore = launched.size();
    QVERIFY(!m2.applyStagedAtStart());
    m2.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m2.state(), UpdateManager::State::Available, 5000);  // not downloaded
    QCOMPARE(m2.availableVersion(), QString("0.3.0"));
    QVERIFY(m2.selectionSkipped());
    QVERIFY(!m2.autoInstallEffective());
    QCOMPARE(launched.size(), launchesBefore);

    // Resume: the setting from before is back, the skipped version stays skipped.
    m2.resumeUpdates();
    QVERIFY(!m2.updatesPaused());
    QCOMPARE(*settings.updateAutoInstall(), true);
    QVERIFY(settings.updatePause().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(m2.state(), UpdateManager::State::Available, 5000);
    QVERIFY(m2.selectionSkipped());
    QVERIFY(!m2.autoInstallEffective());
    m2.resumeUpdates();  // nothing to resume: no effect

    // "Update to 0.3.0" asks for it explicitly: the skip ends, the installer runs.
    m2.installNow();
    QTRY_COMPARE_WITH_TIMEOUT(m2.state(), UpdateManager::State::Applying, 5000);
    QVERIFY(settings.updateSkip().isEmpty());
    QVERIFY(launched.size() > launchesBefore);
    // Back on 0.3.0 nothing is left to settle.
    auto m3p = env.manager(settings, "beta", "0.3.0");
    QVERIFY(!m3p->updatesPaused());
    QVERIFY(m3p->skippedVersion().isEmpty());
  }
  void managerRollbackPauseEndsByItselfWhenReached() {
    Env env;
    env.publishReleases({{"0.2.0", "beta", true}, {"0.3.0", "beta", true}});
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    Pause p;
    p.from = "0.3.0";
    p.to = "0.2.0";
    p.at = "2026-10-01T10:00:00Z";
    p.prevAuto = -1;
    settings.setUpdatePause(pauseToJson(p));
    settings.setUpdateSkip("0.3.0");
    settings.setUpdateAutoInstall(false);
    // The user installed 0.3.0 by hand (or a newer one): the pause and the skip are over, the old default is back.
    auto mp = env.manager(settings, "beta", "0.3.1");
    QVERIFY(!mp->updatesPaused());
    QVERIFY(settings.updatePause().isEmpty());
    QVERIFY(settings.updateSkip().isEmpty());
    QVERIFY(!settings.updateAutoInstall());
  }
  void managerRollbackRejectsBadInstaller() {
    Env env;
    env.publishReleases({{"0.2.0", "beta", true}, {"0.3.0", "beta", true}});
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    settings.setUpdateAutoInstall(true);
    int launches = 0;
    auto mp = env.manager(settings, "beta", "0.3.0");
    UpdateManager& m = *mp;
    m.setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::UpToDate, 5000);
    // The file on the server is not what the signed index says: rejected, nothing paused, nothing started.
    QVERIFY(writeBytes(env.dir.filePath("uzume-0.2.0-windows-x64.msi"), "installer-bytes-0.2.X"));
    m.rollbackTo("0.2.0");
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Error, 5000);
    QVERIFY(m.statusText().contains("rejected"));
    QCOMPARE(launches, 0);
    QVERIFY(!m.updatesPaused());
    QVERIFY(settings.updateSkip().isEmpty());
    QCOMPARE(*settings.updateAutoInstall(), true);

    // A launcher that cannot start leaves the settings as they were.
    env.publishReleases({{"0.2.0", "beta", true}, {"0.3.0", "beta", true}});
    m.setLauncher([&](const QString&, const QStringList&) { return false; });
    m.checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::UpToDate, 5000);
    m.rollbackTo("0.2.0");
    QTRY_COMPARE_WITH_TIMEOUT(m.state(), UpdateManager::State::Error, 5000);
    QVERIFY(!m.updatesPaused());
    QVERIFY(settings.updatePause().isEmpty());
    QVERIFY(settings.updateSkip().isEmpty());
    QCOMPARE(*settings.updateAutoInstall(), true);
  }
  void managerRollbackNeedsInstalledCopy() {
    Env env;
    env.publishReleases({{"0.2.0", "beta", true}, {"0.3.0", "beta", true}});
    uzume::PlayerSettings settings(env.dir.filePath("data"));
    settings.setUpdateChannel("beta");
    UpdateManager::Config c;
    c.baseDir = env.dir.filePath("data");
    c.indexUrl = QUrl::fromLocalFile(env.dir.filePath("updates-index.json")).toString();
    c.currentVersion = "0.3.0";
    c.compiledChannel = "beta";
    c.trustedKeys = env.keys;
    c.installRoot = env.dir.path();
    c.forceApplySupport = false;  // portable / Linux / development: the list is shown, going back is manual
    c.platform = QStringLiteral("windows-x64");
    c.kind = QStringLiteral("msi");
    qunsetenv("APPIMAGE");
    UpdateManager m(c, &settings);
    int launches = 0;
    m.setLauncher([&](const QString&, const QStringList&) { ++launches; return true; });
    m.checkNow();
    QTRY_VERIFY_WITH_TIMEOUT(m.rollbackListLoaded(), 5000);
    QCOMPARE(m.rollbackOptions().size(), 1);
    m.rollbackTo("0.2.0");
    QCOMPARE(launches, 0);
    QVERIFY(!m.updatesPaused());
    QVERIFY(settings.updateSkip().isEmpty());
  }
  void rollbackStagedChecks() {
    Env env;
    env.publishReleases({{"0.2.0", "beta", true}, {"0.3.0", "beta", true}});
    const QString base = env.dir.filePath("data");
    uzume::PlayerSettings settings(base);
    settings.setUpdateChannel("beta");
    auto mp = env.manager(settings, "beta", "0.3.0");
    mp->setLauncher([](const QString&, const QStringList&) { return true; });
    mp->checkNow();
    QTRY_COMPARE_WITH_TIMEOUT(mp->state(), UpdateManager::State::UpToDate, 5000);
    mp->rollbackTo("0.2.0");
    QTRY_COMPARE_WITH_TIMEOUT(mp->state(), UpdateManager::State::Applying, 5000);

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool file = true;
    QVERIFY(loadVerifiedRollback(base, "0.2.0", "0.3.0", env.keys, file, 1, now, "windows-x64", "msi"));
    QVERIFY(!loadVerifiedRollback(base, "0.2.0", "0.2.0", env.keys, file, 1, now, "windows-x64", "msi"));  // not older than the running version
    QVERIFY(!loadVerifiedRollback(base, "0.2.0", "0.1.0", env.keys, file, 1, now, "windows-x64", "msi"));
    QVERIFY(!loadVerifiedRollback(base, "0.3.0", "0.4.0", env.keys, file, 1, now, "windows-x64", "msi"));  // nothing staged for it
    QVERIFY(!loadVerifiedRollback(base, "0.2.0", "0.3.0", {makeKey().pub}, file, 1, now, "windows-x64", "msi"));  // wrong key
    QVERIFY(!loadVerifiedRollback(base, "0.2.0", "0.3.0", env.keys, file, 2, now, "windows-x64", "msi"));  // does not speak the protocol of this Player
    QVERIFY(!loadVerifiedRollback(base, "0.2.0", "0.3.0", env.keys, false, 1, now, "windows-x64", "msi"));  // file:// artifact URLs only in tests
    // An index that ran out (a stale signed index must not bring an old installer back).
    env.publishReleases({{"0.2.0", "beta", true}}, "2026-10-08T10:00:00Z");
    auto stale = [&] {
      QDir d(QDir(rollbackStagingRoot(base)).filePath("0.2.0"));
      QFile::remove(d.filePath("index.json"));
      QFile::remove(d.filePath("index.json.sig"));
      QVERIFY(QFile::copy(env.dir.filePath("updates-index.json"), d.filePath("index.json")));
      QVERIFY(QFile::copy(env.dir.filePath("updates-index.json.sig"), d.filePath("index.json.sig")));
    };
    stale();
    QVERIFY(!loadVerifiedRollback(base, "0.2.0", "0.3.0", env.keys, file, 1, now, "windows-x64", "msi"));
    QVERIFY(loadVerifiedRollback(base, "0.2.0", "0.3.0", env.keys, file, 1, QDateTime::fromString("2026-10-07T10:00:00Z", Qt::ISODate), "windows-x64", "msi"));
    // The installer changed on disk after the download.
    QVERIFY(writeBytes(QDir(rollbackStagingRoot(base)).filePath("0.2.0/uzume-0.2.0-windows-x64.msi"), "changed"));
    QVERIFY(!loadVerifiedRollback(base, "0.2.0", "0.3.0", env.keys, file, 1, QDateTime::fromString("2026-10-07T10:00:00Z", Qt::ISODate), "windows-x64", "msi"));
  }

 private:
  static QByteArray readAll(const QString& p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
  }

  // Throwaway key, signed file:// index and installer in a temp dir.
  struct Env {
    QTemporaryDir dir;
    KeyPair key = makeKey();
    QList<QByteArray> keys{key.pub};
    QString installerName;
    QString channel;
    bool appImage = false;  // publish the Linux AppImage artifact (linux-x64/appimage) instead of the Windows MSI

    void publish(const QByteArray& installer, const QString& version, qint64 sizeOverride = -1,
                 const QString& shaOverride = QString(), const QString& chan = QStringLiteral("beta")) {
      channel = chan;
      installerName = appImage ? "Uzume-" + version + "-x86_64.AppImage" : "uzume-" + version + "-windows-x64.msi";
      QVERIFY(writeBytes(dir.filePath("Uzume.exe"), "launcher"));  // copied to temp by apply()
      const QString path = dir.filePath(installerName);
      QVERIFY(writeBytes(path, installer));
      QJsonObject art = artifact(installerName, installer, QUrl::fromLocalFile(path).toString(), appImage ? "appimage" : "msi");
      if (appImage) art["platform"] = "linux-x64";
      if (sizeOverride >= 0) art["size"] = static_cast<double>(sizeOverride);
      if (!shaOverride.isEmpty()) art["sha256"] = shaOverride;
      const QByteArray idx = indexJson({release("player", chan, version, QJsonArray{art}),
                                        release("hub", "beta", "9.9.9-test.1", oneArt("hub"))});
      QVERIFY(writeBytes(dir.filePath("updates-index.json"), idx));
      QVERIFY(writeBytes(dir.filePath("updates-index.json.sig"), signLine(key, idx)));
    }
    // Player releases (version -> {channel, msi_downgrade}); every one has a small installer file. `expires` empty = none.
    struct Rel {
      QString version;
      QString channel;
      bool downgrade;
      int proto = 1;
      int minProto = 1;
    };
    QMap<QString, QByteArray> installers;
    void publishReleases(const QList<Rel>& rels, const QString& expires = QString()) {
      QJsonArray arr;
      for (const Rel& r : rels) {
        const QByteArray data = "installer-bytes-" + r.version.toUtf8();
        const QString name = "uzume-" + r.version + "-windows-x64.msi";
        QVERIFY(writeBytes(dir.filePath(name), data));
        installers[r.version] = data;
        QJsonObject o = release("player", r.channel, r.version, QJsonArray{artifact(name, data, QUrl::fromLocalFile(dir.filePath(name)).toString())},
                                r.proto, r.minProto);
        if (r.downgrade) o["msi_downgrade"] = true;
        arr.append(o);
      }
      QJsonObject top{{"schema", 1}, {"generated_at", "2026-10-07T10:00:00Z"}, {"releases", arr}};
      if (!expires.isEmpty()) top["expires_at"] = expires;
      const QByteArray idx = QJsonDocument(top).toJson(QJsonDocument::Compact);
      QVERIFY(writeBytes(dir.filePath("Uzume.exe"), "launcher"));
      QVERIFY(writeBytes(dir.filePath("updates-index.json"), idx));
      QVERIFY(writeBytes(dir.filePath("updates-index.json.sig"), signLine(key, idx)));
    }
    void publishPromoted(const QString& version, bool stableListed) {
      publish("installer-bytes", version);
      QJsonArray rel{release("player", "beta", version, QJsonArray{}), release("hub", "beta", "9.9.9", oneArt("hub"))};
      if (stableListed) rel.append(release("player", "stable", version, oneArt("p")));
      const QByteArray idx = indexJson(rel);
      QVERIFY(writeBytes(dir.filePath("updates-index.json"), idx));
      QVERIFY(writeBytes(dir.filePath("updates-index.json.sig"), signLine(key, idx)));
    }
    std::unique_ptr<UpdateManager> manager(uzume::PlayerSettings& s, const QString& compiled = QStringLiteral("beta"),
                                           const QString& current = QStringLiteral("0.3.0-dev")) {
      UpdateManager::Config c;
      c.baseDir = dir.filePath("data");
      c.indexUrl = QUrl::fromLocalFile(dir.filePath("updates-index.json")).toString();
      c.currentVersion = current;
      c.compiledChannel = compiled;
      c.trustedKeys = keys;
      c.installRoot = dir.path();
      c.forceApplySupport = true;
      c.platform = appImage ? QStringLiteral("linux-x64") : QStringLiteral("windows-x64");  // fixtures follow their artifact, not the host
      c.kind = appImage ? QStringLiteral("appimage") : QStringLiteral("msi");
      return std::make_unique<UpdateManager>(c, &s);
    }
  };
};

QTEST_GUILESS_MAIN(UpdateTest)
#include "update_test.moc"
