// Plugin delivery from the Hub against the fake Hub: SHA-256 pin, signature -> origin, atomic install, failures leave the
// previous state, local copies never replace a Hub copy, ensureHubCopy for runs. Synthetic plugin files only.
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "fakehub.h"
#include "uzplugin_testutil.h"
#include "hubconnection.h"
#include "plugin_delivery.h"
#include "plugin_test_util.h"
#include "profilestore.h"

using namespace uzume;
using namespace uzume::plugins;
using namespace uzume::plugins::testutil;
using State = HubConnection::State;

namespace {
const char* kId = "net.test.plugin";
}

class DeliveryTest : public QObject {
  Q_OBJECT
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;
  std::unique_ptr<PluginHubClient> client_;
  std::unique_ptr<PluginManager> mgr_;
  std::unique_ptr<PluginDelivery> delivery_;
  TestKey key_ = fixedTestKey();

  QString root() const { return dir_->filePath("plugins"); }

  // Offers a file on the fake Hub; `served` = what the file endpoint returns (default: the same bytes).
  QString offer(const FbSpec& s, const QByteArray& served = {}, const QString& pinOverride = {}) {
    const QByteArray file = makeUzPlugin(s);
    const QString sha = pinOverride.isEmpty() ? sha256Hex(file) : pinOverride;
    hub_->pluginList.append(QJsonObject{{"plugin_id", s.id}, {"version", s.version}, {"name", s.name.isEmpty() ? s.id : s.name},
                                        {"description", "d"}, {"api", 1}, {"origin", s.sign ? "official" : "unverified"},
                                        {"sha256", sha}, {"size", file.size()}, {"permissions", QJsonArray::fromStringList(s.permissions)},
                                        {"created_at", "2026-10-12T10:00:00Z"}});
    hub_->pluginFiles.insert(s.id + "/" + s.version, served.isNull() ? file : served);
    return sha;
  }

  DeliveryResult ensure(const QString& id, const QString& version, const QString& sha) {
    DeliveryResult out;
    bool done = false;
    delivery_->ensureHubCopy(id, version, sha, [&](const DeliveryResult& r) {
      out = r;
      done = true;
    });
    if (!QTest::qWaitFor([&]() { return done; }, 8000)) QTest::qFail("no delivery result", __FILE__, __LINE__);
    return out;
  }

 private slots:
  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    profiles_ = std::make_unique<ProfileStore>(dir_->filePath("data"));
    creds_ = std::make_unique<MemoryCredentialStore>();
    hub_ = std::make_unique<FakeHub>(QStringLiteral("a"));
    hub_->features = {QStringLiteral("saves_v1"), QStringLiteral("plugins_v1")};
    QVERIFY(hub_->start());
    conn_ = std::make_unique<HubConnection>(profiles_.get(), creds_.get());
    conn_->setPollIntervalMs(50);
    conn_->connectToAddress(hub_->address());
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsTrustConfirmation, 8000);
    conn_->confirmTrust();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsPairing, 8000);
    hub_->decision = FakeHub::Decision::Approve;
    conn_->requestPairing();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::Connected, 8000);
    client_ = std::make_unique<PluginHubClient>(conn_.get());
    mgr_ = std::make_unique<PluginManager>(root());
    mgr_->setTrustedKeys({key_.pub});
    delivery_ = std::make_unique<PluginDelivery>(mgr_.get(), client_.get());
  }
  void cleanup() {
    delivery_.reset();
    mgr_.reset();
    client_.reset();
    conn_.reset();
    hub_.reset();
    creds_.reset();
    profiles_.reset();
    dir_.reset();
  }

  void officialDownloadIsInstalledAndCached() {
    FbSpec s;
    s.sign = true;
    s.extra["i18n/en.json"] = "{}";
    const QString sha = offer(s);
    const DeliveryResult r = ensure(kId, "1.0.0", sha);
    QVERIFY2(r.ok, qPrintable(r.problem + " " + r.error));
    QVERIFY(r.downloaded);
    QCOMPARE(r.origin, QString("official"));
    QCOMPARE(r.dir, root() + "/hub/net.test.plugin/1.0.0");
    QVERIFY(QFileInfo(r.dir + "/main.lua").isFile());
    QVERIFY(QFileInfo(r.dir + "/i18n/en.json").isFile());
    QVERIFY(QFileInfo(r.dir + "/origin.json").isFile());
    QCOMPARE(hub_->pluginFileDownloads, 1);
    QCOMPARE(mgr_->list().first().origin, QString("official"));
    QVERIFY(mgr_->list().first().hubCopy);

    // Present now: no second download, same answer.
    const DeliveryResult again = ensure(kId, "1.0.0", sha);
    QVERIFY(again.ok);
    QVERIFY(!again.downloaded);
    QCOMPARE(again.dir, r.dir);
    QCOMPARE(again.origin, QString("official"));
    QCOMPARE(hub_->pluginFileDownloads, 1);
    // Survives a restart
    PluginManager reopened(root());
    QCOMPARE(reopened.hubCopyDir(kId, "1.0.0", sha), r.dir);
  }

  void unsignedHubUploadIsUnverified() {
    FbSpec s;
    const QString sha = offer(s);
    const DeliveryResult r = ensure(kId, "1.0.0", sha);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.origin, QString("unverified"));
    // The Hub claims "official" but the file carries no (valid) signature: the Player's own check decides.
    FbSpec s2;
    s2.id = "net.test.other";
    s2.sign = true;
    s2.key = fixedTestKey(7);  // signed by a key this Player does not trust
    const QString sha2 = offer(s2);
    const DeliveryResult r2 = ensure("net.test.other", "1.0.0", sha2);
    QVERIFY2(r2.ok, qPrintable(r2.error));
    QCOMPARE(r2.origin, QString("unverified"));
    QVERIFY(!r2.signatureNote.isEmpty());
  }

  void hashMismatchInstallsNothing() {
    FbSpec s;
    FbSpec evil = s;
    evil.lua = "os.execute('evil')";
    // The Hub announces the hash of the good file but serves another one.
    const QString sha = offer(s, makeUzPlugin(evil));
    const DeliveryResult r = ensure(kId, "1.0.0", sha);
    QVERIFY(!r.ok);
    QCOMPARE(r.problem, QString("invalid_hash"));
    QVERIFY(!QFileInfo::exists(root() + "/hub"));
    QVERIFY(mgr_->list().isEmpty());
    QVERIFY(mgr_->hubCopies().isEmpty());
    QVERIFY(mgr_->hubCopyDir(kId, "1.0.0", sha).isEmpty());
  }

  void rightHashButNotAValidPlugin() {
    const QByteArray junk("not a zip at all");
    hub_->pluginFiles.insert("net.test.plugin/1.0.0", junk);
    const DeliveryResult r = ensure(kId, "1.0.0", sha256Hex(junk));
    QVERIFY(!r.ok);
    QCOMPARE(r.problem, QString("invalid_plugin"));
    QVERIFY(!QFileInfo::exists(root() + "/hub/net.test.plugin"));
    QVERIFY(!QFileInfo::exists(root() + "/hub/.tmp-net.test.plugin-1.0.0"));
  }

  void fileOfAnotherPluginOrVersionIsRejected() {
    FbSpec other;
    other.id = "net.test.other";
    const QByteArray file = makeUzPlugin(other);
    hub_->pluginFiles.insert("net.test.plugin/1.0.0", file);
    const DeliveryResult r = ensure(kId, "1.0.0", sha256Hex(file));
    QVERIFY(!r.ok);
    QCOMPARE(r.problem, QString("invalid_plugin"));
    FbSpec v2;
    v2.version = "2.0.0";
    const QByteArray f2 = makeUzPlugin(v2);
    hub_->pluginFiles.insert("net.test.plugin/1.0.0", f2);
    QVERIFY(!ensure(kId, "1.0.0", sha256Hex(f2)).ok);
    QVERIFY(mgr_->hubCopies().isEmpty());
  }

  void unknownVersionIs404() {
    const DeliveryResult r = ensure(kId, "9.9.9", QString(64, 'a'));
    QVERIFY(!r.ok);
    QCOMPARE(r.problem, QString("not_on_hub"));
  }

  void networkErrorKeepsThePreviousState() {
    FbSpec s;
    s.sign = true;
    const QString sha = offer(s);
    QVERIFY(ensure(kId, "1.0.0", sha).ok);
    mgr_->setEnabled(kId, true);
    const QString dir = mgr_->hubCopyDir(kId, "1.0.0", sha);
    QVERIFY(!dir.isEmpty());

    // An update that cannot be downloaded: the Hub goes away.
    FbSpec s2 = s;
    s2.version = "1.1.0";
    const QString sha2 = offer(s2);
    hub_.reset();
    const DeliveryResult r = ensure(kId, "1.1.0", sha2);
    QVERIFY(!r.ok);
    QVERIFY(r.problem == "download_failed" || r.problem == "not_supported");
    QCOMPARE(mgr_->hubCopies().size(), 1);
    QCOMPARE(mgr_->hubCopyDir(kId, "1.0.0", sha), dir);
    QVERIFY(mgr_->list().first().enabled);
    QCOMPARE(mgr_->list().first().manifest.version.toString(), QString("1.0.0"));
  }

  void failedReplacementKeepsTheOldCopy() {
    FbSpec s;
    const QString sha = offer(s);
    QVERIFY(ensure(kId, "1.0.0", sha).ok);
    // The same version is now offered as a different file whose download is damaged.
    FbSpec changed = s;
    changed.lua = "-- changed\n";
    const QByteArray good = makeUzPlugin(changed);
    hub_->pluginFiles.insert("net.test.plugin/1.0.0", good.left(good.size() - 3));
    const DeliveryResult r = ensure(kId, "1.0.0", sha256Hex(good));
    QVERIFY(!r.ok);
    QVERIFY(!mgr_->hubCopyDir(kId, "1.0.0", sha).isEmpty());  // the first file is still the installed one
    QVERIFY(mgr_->hubCopyDir(kId, "1.0.0", sha256Hex(good)).isEmpty());
    // Now the good bytes: replaced as a whole.
    hub_->pluginFiles.insert("net.test.plugin/1.0.0", good);
    const DeliveryResult ok = ensure(kId, "1.0.0", sha256Hex(good));
    QVERIFY2(ok.ok, qPrintable(ok.error));
    QVERIFY(mgr_->hubCopyDir(kId, "1.0.0", sha).isEmpty());
    QFile f(ok.dir + "/main.lua");
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("-- changed\n"));
    QCOMPARE(mgr_->hubCopies().size(), 1);
  }

  void aLocalCopyNeverReplacesTheHubCopy() {
    // A local folder install of the same id and version exists.
    QTemporaryDir src;
    PluginSpec ps;
    ps.id = kId;
    ps.lua = "-- local";
    const QString folder = writePlugin(src.path(), ps);
    QVERIFY(mgr_->installFromFolder(folder).ok);
    QCOMPARE(mgr_->list().first().origin, QString("local"));

    FbSpec s;
    s.sign = true;
    const QString sha = offer(s);
    // The local copy does not count: the exact Hub copy is downloaded next to it.
    QVERIFY(mgr_->hubCopyDir(kId, "1.0.0", sha).isEmpty());
    const DeliveryResult r = ensure(kId, "1.0.0", sha);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(r.downloaded);
    QCOMPARE(r.dir, root() + "/hub/net.test.plugin/1.0.0");
    QFile hubMain(r.dir + "/main.lua");
    QVERIFY(hubMain.open(QIODevice::ReadOnly));
    QCOMPARE(hubMain.readAll(), s.lua.toUtf8());
    // The local install is untouched and still the copy outside runs.
    QFile local(root() + "/installed/net.test.plugin/main.lua");
    QVERIFY(local.open(QIODevice::ReadOnly));
    QCOMPARE(local.readAll(), QByteArray("-- local"));
    QCOMPARE(mgr_->list().first().path, root() + "/installed/net.test.plugin");
    QCOMPARE(mgr_->hubCopyDir(kId, "1.0.0", sha), r.dir);
  }

  void installFromTheOfferedList() {
    FbSpec s;
    s.sign = true;
    const QString sha = offer(s);
    client_->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(client_->state() == PluginHubClient::State::Ready, 8000);
    QCOMPARE(client_->plugins().size(), 1);
    QCOMPARE(client_->plugins().first().sha256, sha);
    QSignalSpy spy(delivery_.get(), &PluginDelivery::finished);
    delivery_->install(kId, "1.0.0");
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 8000);
    const auto r = spy.first().first().value<DeliveryResult>();
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.origin, QString("official"));
    // Not in the list: nothing is requested.
    const int before = hub_->pluginFileDownloads;
    delivery_->install(kId, "7.7.7");
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, 8000);
    QVERIFY(!spy.at(1).first().value<DeliveryResult>().ok);
    QCOMPARE(hub_->pluginFileDownloads, before);
  }

  void listPinIsEnforcedOnInstall() {
    FbSpec s;
    FbSpec evil = s;
    evil.lua = "uz.log.info('HELLO')\n";  // same length: the size check passes, the hash does not
    offer(s, makeUzPlugin(evil));
    client_->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(client_->state() == PluginHubClient::State::Ready, 8000);
    QSignalSpy spy(delivery_.get(), &PluginDelivery::finished);
    delivery_->install(kId, "1.0.0");
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 8000);
    const auto r = spy.first().first().value<DeliveryResult>();
    QVERIFY(!r.ok);
    QCOMPARE(r.problem, QString("invalid_hash"));
    QVERIFY(mgr_->list().isEmpty());
  }

  void listIgnoresBrokenEntries() {
    FbSpec s;
    offer(s);
    hub_->pluginList.append(QJsonObject{{"plugin_id", "../etc"}, {"version", "1.0.0"}, {"origin", "official"}, {"sha256", QString(64, 'a')}});
    hub_->pluginList.append(QJsonObject{{"plugin_id", "net.test.bad"}, {"version", "1.0.0"}, {"origin", "official"}, {"sha256", "XYZ"}});
    hub_->pluginList.append(QJsonObject{{"plugin_id", "net.test.bad2"}, {"version", "1.0.0"}, {"origin", "root"}, {"sha256", QString(64, 'a')}});
    client_->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(client_->state() == PluginHubClient::State::Ready, 8000);
    QCOMPARE(client_->plugins().size(), 1);
  }

  void failedRefreshKeepsTheList() {
    FbSpec s;
    offer(s);
    client_->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(client_->state() == PluginHubClient::State::Ready, 8000);
    hub_->pluginListFails = true;
    client_->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(client_->state() == PluginHubClient::State::Failed, 8000);
    QVERIFY(!client_->errorMessage().isEmpty());
    QCOMPARE(client_->plugins().size(), 1);
  }

  void permissionGrowthTurnsThePluginOff() {
    FbSpec s;
    s.sign = true;
    const QString sha = offer(s);
    QVERIFY(ensure(kId, "1.0.0", sha).ok);
    QVERIFY(mgr_->setEnabled(kId, true));
    FbSpec s2 = s;
    s2.version = "1.1.0";
    s2.permissions = {"memory.read", "rom.read"};
    const QString sha2 = offer(s2);
    const DeliveryResult r = ensure(kId, "1.1.0", sha2);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.addedPermissions, QStringList{"rom.read"});
    const PluginInfo i = mgr_->list().first();
    QCOMPARE(i.manifest.version.toString(), QString("1.1.0"));  // newest Hub copy is active outside runs
    QVERIFY(!i.enabled);
    // The old version stays for runs that use it.
    QVERIFY(!mgr_->hubCopyDir(kId, "1.0.0", sha).isEmpty());
    // Same permissions: the on/off state carries over.
    QVERIFY(mgr_->setEnabled(kId, true));
    FbSpec s3 = s2;
    s3.version = "1.2.0";
    const QString sha3 = offer(s3);
    const DeliveryResult r3 = ensure(kId, "1.2.0", sha3);
    QVERIFY(r3.ok);
    QVERIFY(r3.addedPermissions.isEmpty());
    QVERIFY(mgr_->list().first().enabled);
  }

  void concurrentRequestsShareOneDownload() {
    FbSpec s;
    const QString sha = offer(s);
    int calls = 0;
    DeliveryResult a, b;
    delivery_->ensureHubCopy(kId, "1.0.0", sha, [&](const DeliveryResult& r) { a = r; ++calls; });
    delivery_->ensureHubCopy(kId, "1.0.0", sha, [&](const DeliveryResult& r) { b = r; ++calls; });
    QTRY_COMPARE_WITH_TIMEOUT(calls, 2, 8000);
    QVERIFY(a.ok && b.ok);
    QCOMPARE(hub_->pluginFileDownloads, 1);
  }

  void uninstallRemovesHubCopies() {
    FbSpec s;
    const QString sha = offer(s);
    QVERIFY(ensure(kId, "1.0.0", sha).ok);
    QVERIFY(mgr_->uninstall(kId));
    QVERIFY(mgr_->list().isEmpty());
    QVERIFY(mgr_->hubCopyDir(kId, "1.0.0", sha).isEmpty());
    QVERIFY(!QFileInfo::exists(root() + "/hub/net.test.plugin"));
  }
};

QTEST_MAIN(DeliveryTest)
#include "delivery_test.moc"
