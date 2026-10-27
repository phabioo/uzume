// StateApi against the fake server: list, put with verification, get with the ETag check, preview, delete, quota, feature gate.
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "fakehub.h"
#include "hubconnection.h"
#include "profilestore.h"
#include "stateapi.h"

using namespace uzume;
using State = HubConnection::State;
using Kind = StateApiResult::Kind;

class StateApiTest : public QObject {
  Q_OBJECT
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;
  std::unique_ptr<StateApi> api_;

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
    api_ = std::make_unique<StateApi>(conn_.get());
  }
  static StateRecord meta(int n, const QString& slot = QStringLiteral("default")) {
    StateRecord r;
    r.gameId = QStringLiteral("g1");
    r.saveSlot = slot;
    r.n = n;
    r.coreId = QStringLiteral("melonds");
    r.coreVersion = QStringLiteral("1.2.3");
    return r;
  }
  template <typename F>
  StateApiResult await(F&& call) {
    StateApiResult out;
    bool done = false;
    call([&](const StateApiResult& r) { out = r; done = true; });
    [&] { QTRY_VERIFY_WITH_TIMEOUT(done, 8000); }();
    return out;
  }

 private slots:
  void cleanup() {
    api_.reset();
    conn_.reset();
    hub_.reset();
  }

  void featureGate() {
    connectHub({QStringLiteral("saves_v1")});
    QVERIFY(api_->connected());
    QVERIFY(api_->featureMissing());
    QVERIFY(!api_->available());
  }

  void putListGetPreviewDelete() {
    connectHub({QStringLiteral("saves_v1"), QStringLiteral("states_v1")});
    QVERIFY(api_->available());
    const QByteArray blob(4000, 'x');
    const auto put = await([&](auto cb) { api_->put(meta(2), blob, QByteArray("PNGDATA"), cb); });
    QVERIFY2(put.ok(), qPrintable(put.errorCode));
    QVERIFY(put.state.has_value());
    QCOMPARE(put.state->n, 2);
    QCOMPARE(put.state->sizeBytes, 4000);
    QVERIFY(put.state->onServer);
    QVERIFY(put.state->hasPreview);
    QCOMPARE(hub_->states.size(), 1);
    QCOMPARE(hub_->states.value(QStringLiteral("g1/default/2")).coreVersion, QStringLiteral("1.2.3"));

    const auto list = await([&](auto cb) { api_->list(QStringLiteral("g1"), QStringLiteral("default"), cb); });
    QVERIFY(list.ok());
    QCOMPARE(list.states.size(), 1);
    QCOMPARE(list.states[0].n, 2);
    QVERIFY(list.states[0].onServer);
    QVERIFY(list.usedBytes >= 4000);
    QVERIFY(list.quotaBytes > 0);
    QVERIFY(await([&](auto cb) { api_->list(QStringLiteral("g1"), QStringLiteral("other"), cb); }).states.isEmpty());

    const auto got = await([&](auto cb) { api_->get(QStringLiteral("g1"), QStringLiteral("default"), 2, cb); });
    QVERIFY2(got.ok(), qPrintable(got.errorCode));
    QCOMPARE(got.content, blob);
    QCOMPARE(got.coreId, QStringLiteral("melonds"));
    QCOMPARE(got.coreVersion, QStringLiteral("1.2.3"));

    const auto pv = await([&](auto cb) { api_->preview(QStringLiteral("g1"), QStringLiteral("default"), 2, cb); });
    QVERIFY(pv.ok());
    QCOMPARE(pv.content, QByteArray("PNGDATA"));

    const auto del = await([&](auto cb) { api_->remove(QStringLiteral("g1"), QStringLiteral("default"), 2, cb); });
    QVERIFY(del.ok());
    QVERIFY(hub_->states.isEmpty());
    QCOMPARE(await([&](auto cb) { api_->get(QStringLiteral("g1"), QStringLiteral("default"), 2, cb); }).kind, Kind::NotFound);
  }

  void putReplacesTheSameNumber() {
    connectHub({QStringLiteral("states_v1")});
    QVERIFY(await([&](auto cb) { api_->put(meta(1), QByteArray(10, 'a'), QByteArray(), cb); }).ok());
    QVERIFY(await([&](auto cb) { api_->put(meta(1), QByteArray(20, 'b'), QByteArray(), cb); }).ok());
    QCOMPARE(hub_->states.size(), 1);
    QCOMPARE(hub_->states.value(QStringLiteral("g1/default/1")).state.size(), 20);
    QVERIFY(!await([&](auto cb) { api_->put(meta(1), QByteArray(5, 'c'), QByteArray(), cb); }).state->hasPreview);
  }

  void quotaFullIsReported() {
    connectHub({QStringLiteral("states_v1")});
    hub_->stateQuotaBytes = 100;
    const auto r = await([&](auto cb) { api_->put(meta(1), QByteArray(500, 'x'), QByteArray(), cb); });
    QCOMPARE(r.kind, Kind::QuotaFull);
    QVERIFY(hub_->states.isEmpty());
  }

  void missingFeatureAnswersOffline() {
    connectHub({QStringLiteral("saves_v1")});
    const auto r = await([&](auto cb) { api_->list(QStringLiteral("g1"), QString(), cb); });
    QVERIFY(!r.ok());
  }

  void multipartBodyHasMetaStateAndPreview() {
    const QByteArray b = StateApi::multipartBody("BOUND", QStringLiteral("core"), QStringLiteral("9"), QByteArray("STATE"), QByteArray("PNG"));
    QVERIFY(b.contains("--BOUND"));
    QVERIFY(b.indexOf("name=\"meta\"") < b.indexOf("name=\"state\""));
    QVERIFY(b.indexOf("name=\"state\"") < b.indexOf("name=\"preview\""));
    QVERIFY(b.endsWith("--BOUND--\r\n"));
    const QByteArray noPreview = StateApi::multipartBody("BOUND", QStringLiteral("core"), QStringLiteral("9"), QByteArray("STATE"), QByteArray());
    QVERIFY(!noPreview.contains("name=\"preview\""));
  }
};

QTEST_MAIN(StateApiTest)
#include "state_api_test.moc"
