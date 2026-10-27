// PluginStoreSync against the fake Hub: push with revisions, pull of newer server values, 409 store_conflict keeps both
// versions, feature gate and offline behaviour. Plain JSON only.
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "fakehub.h"
#include "hubconnection.h"
#include "plugin_store.h"
#include "plugin_store_sync.h"
#include "profilestore.h"

using namespace uzume;
using namespace uzume::plugins;
using State = HubConnection::State;

class PluginStoreSyncTest : public QObject {
  Q_OBJECT
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;
  std::shared_ptr<PluginStore> store_;
  std::unique_ptr<PluginStoreSync> sync_;

  void connectHub(const QStringList& features) {
    dir_ = std::make_unique<QTemporaryDir>();
    profiles_ = std::make_unique<ProfileStore>(dir_->filePath(QStringLiteral("data")));
    creds_ = std::make_unique<MemoryCredentialStore>();
    hub_ = std::make_unique<FakeHub>(QStringLiteral("a"));
    hub_->features = features;
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
    store_ = std::make_shared<PluginStore>(dir_->filePath(QStringLiteral("store")));
    sync_ = std::make_unique<PluginStoreSync>(store_, conn_.get());
    sync_->setRetryMs(100000);
  }
  void round() {
    bool done = false;
    sync_->syncNow([&done]() { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
  }

 private slots:
  void cleanup() {
    sync_.reset();
    conn_.reset();
    hub_.reset();
  }

  void pushesNewKeysAndFollowsRevisions() {
    connectHub({QStringLiteral("saves_v1"), QStringLiteral("plugin_store_v1")});
    QVERIFY(sync_->supported());
    QCOMPARE(store_->put("p.one", "lists", QJsonObject{{"a", 1}}).kind, PluginStore::PutResult::Kind::Ok);
    round();
    QCOMPARE(hub_->pluginStore.value("p.one/lists").revision, 1);
    QVERIFY(!store_->isDirty("p.one", "lists"));
    QCOMPARE(store_->serverRevision("p.one", "lists"), 1);
    // A second write is based on revision 1 and becomes revision 2.
    QCOMPARE(store_->put("p.one", "lists", QJsonObject{{"a", 2}}).kind, PluginStore::PutResult::Kind::Ok);
    round();
    QCOMPARE(hub_->pluginStore.value("p.one/lists").revision, 2);
    QCOMPARE(hub_->pluginStore.value("p.one/lists").value.toObject().value("a").toInt(), 2);
  }

  void pullsNewerServerValues() {
    connectHub({QStringLiteral("plugin_store_v1")});
    hub_->pluginStore["p.one/lists"] = {3, QJsonObject{{"from", "other pc"}}};
    QSignalSpy adopted(sync_.get(), &PluginStoreSync::keyAdopted);
    sync_->pullPlugin("p.one");
    round();
    QCOMPARE(adopted.count(), 1);
    const auto e = store_->get("p.one", "lists");
    QVERIFY(e.found);
    QCOMPARE(e.value.toObject().value("from").toString(), QStringLiteral("other pc"));
    QVERIFY(!store_->isDirty("p.one", "lists"));
  }

  void aServerConflictKeepsBothVersions() {
    connectHub({QStringLiteral("plugin_store_v1")});
    QVERIFY(store_->put("p.one", "lists", QJsonObject{{"v", "local"}}).kind == PluginStore::PutResult::Kind::Ok);
    hub_->pluginStore["p.one/lists"] = {5, QJsonObject{{"v", "server"}}};  // somebody else wrote first
    QSignalSpy conflicted(sync_.get(), &PluginStoreSync::keyConflicted);
    round();
    QCOMPARE(conflicted.count(), 1);
    // Nothing was overwritten: the server still has its value, the plugin sees both.
    QCOMPARE(hub_->pluginStore.value("p.one/lists").value.toObject().value("v").toString(), QStringLiteral("server"));
    QCOMPARE(store_->get("p.one", "lists").value.toObject().value("v").toString(), QStringLiteral("local"));
    const auto conflicts = store_->conflicts("p.one", "lists");
    QCOMPARE(conflicts.size(), 1);
    QCOMPARE(conflicts.first().source, QStringLiteral("server"));
    QCOMPARE(conflicts.first().value.toObject().value("v").toString(), QStringLiteral("server"));
  }

  void withoutTheFeatureNothingIsSentAndTheLocalCopyWorks() {
    connectHub({QStringLiteral("saves_v1")});
    QVERIFY(!sync_->supported());
    QVERIFY(store_->put("p.one", "k", 1).kind == PluginStore::PutResult::Kind::Ok);
    sync_->syncNow();
    QTest::qWait(300);
    QVERIFY(hub_->pluginStore.isEmpty());
    QVERIFY(store_->get("p.one", "k").found);
    QVERIFY(store_->isDirty("p.one", "k"));  // pushed later, when the server can
  }
};

QTEST_MAIN(PluginStoreSyncTest)
#include "pluginstore_sync_test.moc"
