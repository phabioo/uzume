#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "fakehub.h"
#include "hubconnection.h"
#include "hubhttp.h"
#include "profilestore.h"

using namespace uzume;
using State = HubConnection::State;

#define WAIT_STATE(conn, st) QTRY_VERIFY_WITH_TIMEOUT((conn).state() == (st), 8000)

// Memory store that behaves like a Secret Service whose unlock prompt was cancelled; counts the reads (each one is a prompt).
class CancelledStore final : public MemoryCredentialStore {
 public:
  bool cancelled = false;
  mutable int reads = 0;
  CredentialRead readChecked(const QString& t) const override {
    ++reads;
    if (cancelled) return {CredentialRead::Status::Error, {}};
    return MemoryCredentialStore::readChecked(t);
  }
};

class HubFlowTest : public QObject {
  Q_OBJECT
 private:
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<HubConnection> conn_;

  void pairViaApproval(FakeHub& hub) {
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::NeedsTrustConfirmation);
    conn_->confirmTrust();
    WAIT_STATE(*conn_, State::NeedsPairing);
    hub.decision = FakeHub::Decision::Approve;
    conn_->requestPairing();
    WAIT_STATE(*conn_, State::Connected);
  }

  static QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
  }

 private slots:
  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    profiles_ = std::make_unique<ProfileStore>(dir_->path());
    creds_ = std::make_unique<MemoryCredentialStore>();
    conn_ = std::make_unique<HubConnection>(profiles_.get(), creds_.get());
    conn_->setPollIntervalMs(50);
  }
  void cleanup() {
    conn_.reset();
    profiles_.reset();
    creds_.reset();
    dir_.reset();
  }

  void fingerprintFormatMatchesHub() {
    QFile f(QStringLiteral(FB_TEST_DATA_DIR "/test-cert-a.pem"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QSslCertificate cert(&f, QSsl::Pem);
    QCOMPARE(HubHttp::fingerprint(cert),
             QStringLiteral("2F:E5:AD:C0:A7:7D:0E:CE:5E:05:AC:C5:CA:A2:C7:6C:E2:8E:63:A9:CB:22:A5:D8:3D:F2:09:85:79:E8:B2:D2"));
  }

  void fingerprintsEqualIgnoresColonsCaseAndSpace() {
    const QString colon = QStringLiteral("2F:E5:AD:C0:A7:7D:0E:CE:5E:05:AC:C5:CA:A2:C7:6C:E2:8E:63:A9:CB:22:A5:D8:3D:F2:09:85:79:E8:B2:D2");
    const QString plain = QStringLiteral("2fe5adc0a77d0ece5e05acc5caa2c76ce28e63a9cb22a5d83df20985"
                                         "79e8b2d2");
    QVERIFY(HubHttp::fingerprintsEqual(plain, colon));
    QVERIFY(HubHttp::fingerprintsEqual(colon, plain));
    QVERIFY(HubHttp::fingerprintsEqual(QStringLiteral(" ") + colon.toLower() + QStringLiteral("\n"), colon));
    QVERIFY(!HubHttp::fingerprintsEqual(plain.left(63) + QLatin1Char('0'), colon));
    QVERIFY(!HubHttp::fingerprintsEqual(QString(), QString()));
    QVERIFY(!HubHttp::fingerprintsEqual(QStringLiteral(" : "), QStringLiteral(" : ")));
  }

  // PN-9: the pin decision applied on every TLS connection (also when the platform trusts the certificate itself).
  void peerAcceptedDecision() {
    const QString pin = QStringLiteral("AA:BB:CC");
    QVERIFY(HubHttp::peerAccepted(QStringLiteral("aabbcc"), pin, false));
    QVERIFY(HubHttp::peerAccepted(QStringLiteral("AA:BB:CC"), pin, true));
    QVERIFY(!HubHttp::peerAccepted(QStringLiteral("AA:BB:DD"), pin, true));  // a different, even CA-valid, certificate
    QVERIFY(!HubHttp::peerAccepted(QString(), pin, true));                   // nothing observed: not accepted
    QVERIFY(HubHttp::peerAccepted(QStringLiteral("AA:BB:CC"), QString(), true));    // first contact
    QVERIFY(!HubHttp::peerAccepted(QStringLiteral("AA:BB:CC"), QString(), false));  // no pin and no first contact
    QVERIFY(!HubHttp::peerAccepted(QString(), QString(), true));
  }

  void firstContactReportsFingerprintThenPins() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    QSignalSpy states(conn_.get(), &HubConnection::stateChanged);
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::NeedsTrustConfirmation);
    QCOMPARE(conn_->observedFingerprint(), hub.fingerprint());
    QCOMPARE(conn_->hubInfo().hubId, hub.hubId);
    QVERIFY(profiles_->profiles().isEmpty());  // nothing adopted yet
    QCOMPARE(hub.requests.size(), 1);          // only the info endpoint
    conn_->confirmTrust();
    WAIT_STATE(*conn_, State::NeedsPairing);
    QCOMPARE(profiles_->profile(hub.hubId)->pinnedFingerprint, hub.fingerprint());
  }

  void rejectTrustDisconnects() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::NeedsTrustConfirmation);
    conn_->rejectTrust();
    QVERIFY(conn_->state() == State::Disconnected);
    QVERIFY(profiles_->profiles().isEmpty());
  }

  void pinnedMatchingCertificateConnects() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    pairViaApproval(hub);
    conn_->setHandshakeInfo([] {
      HandshakeInfo h = HandshakeInfo::detect();
      h.cores.append({QStringLiteral("melonds_ds"), QStringLiteral("1.4.0")});
      return h;
    }());
    conn_->disconnectFromHub();
    // second run via the stored profile: pin + credential present -> connected directly
    conn_->connectToProfile(hub.hubId);
    WAIT_STATE(*conn_, State::Connected);
    QVERIFY(profiles_->profile(hub.hubId)->lastConnected.isValid());
    QCOMPARE(profiles_->lastHubId(), hub.hubId);
    FakeRequest hs;
    for (const FakeRequest& r : hub.requests) {
      if (r.path == QLatin1String("/api/v1/handshake")) {
        hs = r;
      }
    }
    const QJsonObject body = QJsonDocument::fromJson(hs.body).object();
    QCOMPARE(body.value(QStringLiteral("protocol_version")).toInt(), 1);
    QCOMPARE(body.value(QStringLiteral("min_protocol_version")).toInt(), 1);
    QCOMPARE(body.value(QStringLiteral("cores")).toArray().first().toObject().value(QStringLiteral("id")).toString(),
             QStringLiteral("melonds_ds"));
    QVERIFY(!body.value(QStringLiteral("player_version")).toString().isEmpty());
    QVERIFY(body.contains(QStringLiteral("video")) && body.contains(QStringLiteral("audio")) && body.contains(QStringLiteral("input")));
    QVERIFY(hs.headers.value(QStringLiteral("authorization")).startsWith("Bearer fba_"));
  }

  void changedCertificateBlocksWithoutAnyRequest() {
    FakeHub hubA(QStringLiteral("a"));
    QVERIFY(hubA.start());
    HubProfile p;
    p.hubId = hubA.hubId;
    p.name = QStringLiteral("x");
    p.address = hubA.address();
    p.deviceId = profiles_->deviceId();
    p.pinnedFingerprint = hubA.fingerprint();
    p.credentialRef = credentialTarget(p.hubId, p.deviceId);
    QVERIFY(profiles_->upsertProfile(p));
    creds_->write(p.credentialRef, QString::fromLatin1(FakeHub::kDeviceCredential));
    hubA.close();

    // Same address, different certificate
    FakeHub hubB(QStringLiteral("b"));
    QVERIFY(hubB.start());
    p.address = hubB.address();
    QVERIFY(profiles_->upsertProfile(p));
    conn_->connectToProfile(p.hubId);
    WAIT_STATE(*conn_, State::CertificateChanged);
    QCOMPARE(hubB.requests.size(), 0);
    QCOMPARE(conn_->observedFingerprint(), hubB.fingerprint());
    QCOMPARE(conn_->expectedFingerprint(), hubA.fingerprint());
    QCOMPARE(profiles_->profile(p.hubId)->pinnedFingerprint, hubA.fingerprint());  // not adopted
    QCOMPARE(hubB.tokenRequests(), 0);
  }

  void confirmCertificateChangeRepinsKeepsCredential() {
    FakeHub hub(QStringLiteral("b"));
    QVERIFY(hub.start());
    const QString oldFp = QStringLiteral("AA:BB:CC");  // pin of the former certificate
    HubProfile p;
    p.hubId = hub.hubId;
    p.name = QStringLiteral("x");
    p.address = hub.address();
    p.deviceId = profiles_->deviceId();
    p.pinnedFingerprint = oldFp;
    p.credentialRef = credentialTarget(p.hubId, p.deviceId);
    QVERIFY(profiles_->upsertProfile(p));
    creds_->write(p.credentialRef, QString::fromLatin1(FakeHub::kDeviceCredential));

    conn_->connectToProfile(p.hubId);
    WAIT_STATE(*conn_, State::CertificateChanged);
    QCOMPARE(conn_->expectedFingerprint(), oldFp);
    QCOMPARE(conn_->observedFingerprint(), hub.fingerprint());
    QCOMPARE(hub.tokenRequests(), 0);
    // A wrong or empty fingerprint changes nothing.
    QVERIFY(!conn_->confirmCertificateChange(QString()));
    QVERIFY(!conn_->confirmCertificateChange(QStringLiteral("00:11")));
    QVERIFY(conn_->state() == State::CertificateChanged);
    QCOMPARE(profiles_->profile(p.hubId)->pinnedFingerprint, oldFp);

    QVERIFY(conn_->confirmCertificateChange(hub.fingerprint()));
    WAIT_STATE(*conn_, State::Connected);
    QCOMPARE(hub.tokenRequests(), 1);
    QCOMPARE(profiles_->profile(p.hubId)->pinnedFingerprint, hub.fingerprint());
    QCOMPARE(profiles_->profile(p.hubId)->credentialRef, p.credentialRef);
    QVERIFY(creds_->read(p.credentialRef).has_value());

    // The pin survives a reload of the store.
    ProfileStore reloaded(dir_->path());
    QCOMPARE(reloaded.profile(p.hubId)->pinnedFingerprint, hub.fingerprint());
  }

  void thirdCertificateAfterConfirmIsMismatchAgain() {
    FakeHub hub(QStringLiteral("b"));
    QVERIFY(hub.start());
    HubProfile p;
    p.hubId = hub.hubId;
    p.name = QStringLiteral("x");
    p.address = hub.address();
    p.deviceId = profiles_->deviceId();
    p.pinnedFingerprint = QStringLiteral("AA:BB:CC");
    p.credentialRef = credentialTarget(p.hubId, p.deviceId);
    QVERIFY(profiles_->upsertProfile(p));
    creds_->write(p.credentialRef, QString::fromLatin1(FakeHub::kDeviceCredential));
    conn_->connectToProfile(p.hubId);
    WAIT_STATE(*conn_, State::CertificateChanged);
    const QString confirmed = conn_->observedFingerprint();

    QVERIFY(hub.setCertificate(QStringLiteral("c")));  // yet another certificate on the reconnect
    QVERIFY(conn_->confirmCertificateChange(confirmed));
    WAIT_STATE(*conn_, State::CertificateChanged);
    QCOMPARE(conn_->observedFingerprint(), hub.fingerprint());
    QVERIFY(conn_->observedFingerprint() != confirmed);
    QCOMPARE(conn_->expectedFingerprint(), confirmed);
    QCOMPARE(profiles_->profile(p.hubId)->pinnedFingerprint, confirmed);
    QCOMPARE(hub.tokenRequests(), 0);
  }

  void protocolIncompatibility() {
    FakeHub hub(QStringLiteral("a"));
    hub.minProtocolVersion = 2;
    hub.protocolVersion = 2;
    QVERIFY(hub.start());
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::Incompatible);
    QVERIFY(conn_->incompatibleReason() == HubConnection::IncompatibleReason::PlayerTooOld);
    QCOMPARE(conn_->errorCode(), QStringLiteral("player_too_old"));

    HandshakeInfo h = HandshakeInfo::detect();
    h.minProtocolVersion = 3;  // player requires more than the hub supports
    conn_->setHandshakeInfo(h);
    hub.minProtocolVersion = 1;
    hub.protocolVersion = 2;
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::Incompatible);
    QVERIFY(conn_->incompatibleReason() == HubConnection::IncompatibleReason::HubTooOld);
  }

  void parseHandshakeUser() {
    QJsonObject o{{QStringLiteral("compatible"), true}};
    auto r = parseHandshakeResult(o);
    QVERIFY(r.has_value());
    QVERIFY(r->userDisplayName.isEmpty());
    QVERIFY(r->userId.isEmpty());
    o.insert(QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), QStringLiteral("u_1")},
                                                 {QStringLiteral("display_name"), QStringLiteral(" Lena ")},
                                                 {QStringLiteral("role"), QStringLiteral("user")}});
    r = parseHandshakeResult(o);
    QVERIFY(r.has_value());
    QCOMPARE(r->userId, QStringLiteral("u_1"));
    QCOMPARE(r->userDisplayName, QStringLiteral("Lena"));
    QCOMPARE(r->userRole, QStringLiteral("user"));
  }

  // The user name is stored per Hub profile on every handshake and cleared when an older Hub omits `user`.
  void userNameStoredPerProfile() {
    FakeHub hub(QStringLiteral("a"));
    hub.handshakeExtra = QJsonObject{{QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), QStringLiteral("u_1")},
                                                                          {QStringLiteral("display_name"), QStringLiteral("Lena")},
                                                                          {QStringLiteral("role"), QStringLiteral("admin")}}}};
    QVERIFY(hub.start());
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::NeedsTrustConfirmation);
    conn_->confirmTrust();
    WAIT_STATE(*conn_, State::NeedsPairing);
    conn_->requestPairing();
    WAIT_STATE(*conn_, State::AwaitingApproval);
    hub.decision = FakeHub::Decision::Approve;
    WAIT_STATE(*conn_, State::Connected);
    QCOMPARE(conn_->userDisplayName(), QStringLiteral("Lena"));
    QCOMPARE(profiles_->profile(hub.hubId)->userDisplayName, QStringLiteral("Lena"));

    hub.handshakeExtra = QJsonObject();  // Hub downgraded / older Hub
    conn_->connectToProfile(hub.hubId);
    WAIT_STATE(*conn_, State::Connected);
    QVERIFY(profiles_->profile(hub.hubId)->userDisplayName.isEmpty());
  }

  void pairingAllowStoresCredentialOutsideProfile() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::NeedsTrustConfirmation);
    conn_->confirmTrust();
    WAIT_STATE(*conn_, State::NeedsPairing);
    conn_->requestPairing();
    WAIT_STATE(*conn_, State::AwaitingApproval);
    QTest::qWait(150);  // several polls with pending
    QVERIFY(conn_->state() == State::AwaitingApproval);
    hub.decision = FakeHub::Decision::Approve;
    WAIT_STATE(*conn_, State::Connected);

    const auto p = profiles_->profile(hub.hubId);
    QVERIFY(p.has_value());
    QCOMPARE(p->hubUserId, QStringLiteral("u_test_1"));
    QCOMPARE(p->deviceId, profiles_->deviceId());
    QCOMPARE(p->credentialRef, credentialTarget(hub.hubId, profiles_->deviceId()));
    QCOMPARE(creds_->read(p->credentialRef).value(), QString::fromLatin1(FakeHub::kDeviceCredential));

    // Pairing request contains device details
    QJsonObject req;
    for (const FakeRequest& r : hub.requests) {
      if (r.path == QLatin1String("/api/v1/pairing/requests")) {
        req = QJsonDocument::fromJson(r.body).object();
      }
    }
    QCOMPARE(req.value(QStringLiteral("device_id")).toString(), profiles_->deviceId());
    QCOMPARE(req.value(QStringLiteral("device_name")).toString(), profiles_->deviceName());
    QVERIFY(!req.value(QStringLiteral("player_version")).toString().isEmpty());

    // No secrets in profile/device file
    const QByteArray files = readAll(profiles_->profilesFilePath()) + readAll(dir_->filePath(QStringLiteral("device.json")));
    QVERIFY(!files.isEmpty());
    for (const QByteArray& secret : QList<QByteArray>{FakeHub::kDeviceCredential, FakeHub::kPollToken, "fba_", "fbd_", "fbp_"}) {
      QVERIFY2(!files.contains(secret), secret.constData());
    }
  }

  void pairingDenyExpireCancel() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::NeedsTrustConfirmation);
    conn_->confirmTrust();
    WAIT_STATE(*conn_, State::NeedsPairing);

    hub.decision = FakeHub::Decision::Deny;
    conn_->requestPairing();
    WAIT_STATE(*conn_, State::Denied);
    QVERIFY(profiles_->profile(hub.hubId)->credentialRef.isEmpty());

    hub.decision = FakeHub::Decision::Expire;
    conn_->requestPairing();  // possible again from Denied
    WAIT_STATE(*conn_, State::Expired);

    hub.decision = FakeHub::Decision::Pending;
    conn_->requestPairing();
    WAIT_STATE(*conn_, State::AwaitingApproval);
    conn_->cancelPairing();
    QVERIFY(conn_->state() == State::NeedsPairing);
    const int polls = hub.count(QStringLiteral("/api/v1/pairing/requests/"));
    QTest::qWait(200);
    QCOMPARE(hub.count(QStringLiteral("/api/v1/pairing/requests/")), polls);  // poll stopped
  }

  void pairingRateLimitKeepsState() {
    FakeHub hub(QStringLiteral("a"));
    hub.rateLimitPairing = true;
    QVERIFY(hub.start());
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::NeedsTrustConfirmation);
    conn_->confirmTrust();
    WAIT_STATE(*conn_, State::NeedsPairing);
    QSignalSpy errors(conn_.get(), &HubConnection::errorOccurred);
    conn_->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 5000);
    QCOMPARE(errors.first().first().toString(), QStringLiteral("rate_limited"));
    QVERIFY(conn_->state() == State::NeedsPairing);
  }

  void accessTokenIsRenewedBeforeExpiry() {
    FakeHub hub(QStringLiteral("a"));
    hub.tokenLifetime = 1;  // renewal after ~0.8 s
    QVERIFY(hub.start());
    pairViaApproval(hub);
    QTRY_VERIFY_WITH_TIMEOUT(hub.tokenRequests() >= 3, 6000);
    QVERIFY(conn_->state() == State::Connected);
    QVERIFY(hub.issuedAccessTokens.at(0) != hub.issuedAccessTokens.at(1));
    // renewal without another handshake
    QCOMPARE(hub.count(QStringLiteral("/api/v1/handshake")), 1);
  }

  void revokeBringsBackToPairing() {
    FakeHub hub(QStringLiteral("a"));
    hub.tokenLifetime = 1;
    QVERIFY(hub.start());
    pairViaApproval(hub);
    hub.revoked = true;  // admin revokes in the hub
    WAIT_STATE(*conn_, State::NeedsPairing);
    QCOMPARE(conn_->errorCode(), QStringLiteral("device_revoked"));
    QVERIFY(!creds_->read(credentialTarget(hub.hubId, profiles_->deviceId())).has_value());
    QVERIFY(profiles_->profile(hub.hubId)->credentialRef.isEmpty());
  }

  void revokeSelf() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    pairViaApproval(hub);
    conn_->revokeSelf();
    WAIT_STATE(*conn_, State::NeedsPairing);
    QVERIFY(hub.revoked);
    QVERIFY(!creds_->read(credentialTarget(hub.hubId, profiles_->deviceId())).has_value());
  }

  void invalidStoredCredentialGoesToPairing() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    pairViaApproval(hub);
    conn_->disconnectFromHub();
    creds_->write(credentialTarget(hub.hubId, profiles_->deviceId()), QStringLiteral("fbd_WRONG"));
    conn_->connectToProfile(hub.hubId);
    WAIT_STATE(*conn_, State::NeedsPairing);
    QCOMPARE(conn_->errorCode(), QStringLiteral("invalid_credentials"));
  }

  void cancelledCredentialStoreStopsWithoutRetryLoop() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    auto store = std::make_unique<CancelledStore>();
    conn_ = std::make_unique<HubConnection>(profiles_.get(), store.get());
    conn_->setPollIntervalMs(50);
    pairViaApproval(hub);
    conn_->disconnectFromHub();

    store->cancelled = true;
    store->reads = 0;
    QSignalSpy states(conn_.get(), &HubConnection::stateChanged);
    conn_->connectToProfile(hub.hubId);
    WAIT_STATE(*conn_, State::Unreachable);
    QCOMPARE(conn_->errorCode(), QStringLiteral("credential_store_unavailable"));
    QVERIFY(conn_->credentialStoreLocked());
    QVERIFY(!conn_->errorMessage().isEmpty());
    const int readsAfterCancel = store->reads;
    QCOMPARE(readsAfterCancel, 1);
    // Nothing asks again on its own: the state stays and the store is not read again.
    const int statesAfterCancel = states.count();
    QTest::qWait(600);
    QCOMPARE(store->reads, readsAfterCancel);
    QCOMPARE(states.count(), statesAfterCancel);
    QVERIFY(conn_->state() == State::Unreachable);

    // Retry asks exactly once more (and fails again while the store stays cancelled).
    conn_->retry();
    WAIT_STATE(*conn_, State::Unreachable);
    QTRY_COMPARE_WITH_TIMEOUT(store->reads, readsAfterCancel + 1, 8000);
    QTest::qWait(300);
    QCOMPARE(store->reads, readsAfterCancel + 1);

    // Unlocked: Retry signs in.
    store->cancelled = false;
    conn_->retry();
    WAIT_STATE(*conn_, State::Connected);
    conn_.reset();
  }

  void insecureHttpOnlyOnLoopbackOrDevFlag() {
    conn_->connectToAddress(QStringLiteral("http://192.0.2.1:9"));
    QVERIFY(conn_->state() == State::Unreachable);
    QCOMPARE(conn_->errorCode(), QStringLiteral("insecure_http"));
    QVERIFY(HubHttp::isSchemeAllowed(QUrl(QStringLiteral("http://localhost:1")), false));
    QVERIFY(HubHttp::isSchemeAllowed(QUrl(QStringLiteral("http://[::1]:1")), false));
    QVERIFY(!HubHttp::isSchemeAllowed(QUrl(QStringLiteral("http://192.0.2.1")), false));
    QVERIFY(HubHttp::isSchemeAllowed(QUrl(QStringLiteral("http://192.0.2.1")), true));

    FakeHub hub{QString()};  // HTTP on 127.0.0.1
    QVERIFY(hub.start());
    conn_->connectToAddress(hub.address());
    WAIT_STATE(*conn_, State::NeedsPairing);  // no TLS -> no trust confirmation
  }

  void defaultPortIs8443() {
    conn_->connectToAddress(QStringLiteral("192.0.2.1"));
    QCOMPARE(conn_->address(), QStringLiteral("https://192.0.2.1:8443"));
    conn_->connectToAddress(QStringLiteral("https://192.0.2.1:9443"));
    QCOMPARE(conn_->address(), QStringLiteral("https://192.0.2.1:9443"));
    conn_->connectToAddress(QStringLiteral("192.0.2.1:443"));
    QCOMPARE(conn_->address(), QStringLiteral("https://192.0.2.1:443"));
    conn_->connectToAddress(QStringLiteral("http://localhost"), true);
    QCOMPARE(conn_->address(), QStringLiteral("http://localhost:8443"));
    conn_->disconnectFromHub();
  }

  void unreachableHub() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    const QString addr = hub.address();
    hub.close();
    conn_->connectToAddress(addr);
    WAIT_STATE(*conn_, State::Unreachable);
    QCOMPARE(conn_->errorCode(), QStringLiteral("unreachable"));
  }

  void switchingHubDisconnectsFirst() {
    FakeHub hub1(QStringLiteral("a"));
    FakeHub hub2(QStringLiteral("b"));
    hub2.hubId = QStringLiteral("hub-test-2");
    QVERIFY(hub1.start() && hub2.start());
    pairViaApproval(hub1);
    QList<State> seen;
    connect(conn_.get(), &HubConnection::stateChanged, this, [&seen](State s) { seen.append(s); });
    conn_->connectToAddress(hub2.address());
    QVERIFY(seen.size() >= 2);
    QVERIFY(seen.at(0) == State::Disconnected);
    QVERIFY(seen.at(1) == State::Identifying);
    WAIT_STATE(*conn_, State::NeedsTrustConfirmation);
    QCOMPARE(conn_->hubInfo().hubId, QStringLiteral("hub-test-2"));
  }
};

QTEST_GUILESS_MAIN(HubFlowTest)
#include "hub_flow_test.moc"
