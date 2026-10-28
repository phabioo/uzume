#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>

#include "credentialstore.h"
#include "hubhttp.h"
#include "hubprotocol.h"
#include "profilestore.h"

namespace uzume {

// Connection state machine for exactly one hub (identification, TOFU/pinning, approval pairing,
// token, handshake). The UI binds to stateChanged()/errorOccurred(); actions are slots.
class HubConnection : public QObject {
  Q_OBJECT
 public:
  enum class State {
    Disconnected,
    Identifying,
    NeedsTrustConfirmation,  // first contact: show observedFingerprint(), confirmTrust()/rejectTrust()
    CertificateChanged,      // certificate differs from the pin: blocked, never silently accepted
    Incompatible,            // incompatibleReason()
    Unreachable,             // errorCode()/errorMessage()
    NeedsPairing,            // requestPairing()
    AwaitingApproval,        // polling in progress, cancelPairing()
    Denied,
    Expired,
    Authenticating,
    UserDisabled,            // the Hub reports user_disabled (401): credential kept, no retry loop; retry() tries once more
    Connected
  };
  Q_ENUM(State)

  enum class IncompatibleReason { None, PlayerTooOld, HubTooOld };
  Q_ENUM(IncompatibleReason)

  HubConnection(ProfileStore* profiles, CredentialStore* credentials, QObject* parent = nullptr);
  ~HubConnection() override;

  static QString stateName(State s);

  // Handshake data (including cores); set before connecting.
  void setHandshakeInfo(const HandshakeInfo& info) { handshake_ = info; }
  // Sends the current handshake data again on a connected session (e.g. after a core download changed the reported
  // core versions) and replaces handshakeProblems(); handshakeProblemsChanged() follows. No-op when not connected.
  void resendHandshake();
  void setPollIntervalMs(int ms) { pollIntervalMs_ = ms; }

  State state() const { return state_; }
  IncompatibleReason incompatibleReason() const { return incompatible_; }
  QString errorCode() const { return errorCode_; }
  // Unreachable because the OS credential store was locked or dismissed: nothing may retry on its own (every try opens the
  // unlock prompt again); only the person's Retry or Connect asks again.
  bool credentialStoreLocked() const { return state_ == State::Unreachable && errorCode_ == QLatin1String("credential_store_unavailable"); }
  QString errorMessage() const { return errorMessage_; }
  // Seconds until the next try is possible when errorCode() is "rate_limited" and the server said so; 0 = unknown.
  int errorRetryAfter() const { return errorRetryAfter_; }
  QString address() const { return address_; }
  const HubInfo& hubInfo() const { return hubInfo_; }
  std::optional<HubProfile> profile() const { return profile_; }
  QString observedFingerprint() const { return observedFp_; }  // for NeedsTrustConfirmation/CertificateChanged
  // Security code (K7Q4-9WMP-2HXD) of observedFingerprint(): what the person compares with the server's own screen or
  // `uzume-server code` in NeedsTrustConfirmation (join step 2, set-up step 3). Empty without a certificate.
  QString securityCode() const;
  // The server has no admin yet and waits for setupWithCode() (`setup_required` of the info endpoint; false for older servers).
  bool setupRequired() const { return hubInfo_.setupRequired; }
  // The install ID sent as `install_id` (one install of the app on one PC).
  QString installId() const { return profiles_->installId(); }
  // Time after which sign-in and set-up requests count as "no answer" (default 10 s).
  void setRequestTimeoutMs(int ms) { requestTimeoutMs_ = ms; }
  QString expectedFingerprint() const { return pin_; }         // pinned value (CertificateChanged)
  QList<HandshakeProblem> handshakeProblems() const { return problems_; }
  // Feature flags advertised by the hub in the handshake (e.g. "saves_v1"); empty until Connected.
  bool hubHasFeature(const QString& feature) const { return features_.contains(feature); }
  QStringList hubFeatures() const { return features_; }
  QString hubId() const { return hubInfo_.hubId; }
  QString hubUserId() const { return profile_ ? profile_->hubUserId : QString(); }
  QString userDisplayName() const { return profile_ ? profile_->userDisplayName : QString(); }
  QString userRole() const { return profile_ ? profile_->userRole : QString(); }
  QString deviceId() const { return profile_ ? profile_->deviceId : QString(); }

  // Exactly one active hub: calls first disconnect the old connection.
  void connectToAddress(const QString& address, bool allowHttp = false);
  // Connects as the active account of the profile (the flat user fields).
  void connectToProfile(const QString& hubId);
  // Connects as one of the people stored on this PC (makes it the active account first). Without a credential (signed out
  // on this PC) it ends in NeedsPairing with that person's device ID: signIn() with the password.
  void connectToProfileAs(const QString& hubId, const QString& userId);
  // Adds a person to a known server (8d-6): identifies with the pin of the profile and ends in NeedsPairing with a fresh
  // device ID and no credential, so signIn() / redeemInvite() / requestPairing() create a second account. The stored
  // accounts stay untouched until it succeeds; then the new account is added and becomes the active one. Disconnect
  // (or any other connect call) abandons it.
  void beginAddAccount(const QString& hubId);
  bool addingAccount() const { return mode_ == IdentifyMode::AddAccount; }
  // Proves a person on this PC with their password (8d-7c/7e: session expired, PIN locked): identifies and ends in NeedsPairing
  // with that person's own device ID, without using their stored credential; signIn() then replaces the credential and
  // clears the PIN lock, the PIN itself stays. Nothing stored changes until the sign-in succeeds, which also makes
  // the person the active one.
  void beginSignInAs(const QString& hubId, const QString& userId);
  // Security code from an invite link: call before connectToAddress() / connectToProfile(). When the certificate the server
  // shows has this fingerprint it is trusted without NeedsTrustConfirmation (trustedByLink() is true); when it differs the
  // connection ends in Disconnected with errorCode "security_code_mismatch", nothing is sent and no profile is saved.
  // The value is used for the next connect only and is dropped by disconnectFromHub().
  void setExpectedFingerprint(const QString& fingerprint);
  bool trustedByLink() const { return linkTrusted_; }
  void disconnectFromHub();

  void confirmTrust();  // NeedsTrustConfirmation: pin the fingerprint and continue
  void rejectTrust();   // -> Disconnected
  // CertificateChanged: replaces the stored pin by exactly `confirmedFingerprint` (must equal observedFingerprint(),
  // otherwise nothing happens and false is returned), keeps the credential and identifies again. No token has been
  // sent to the new certificate; if the hub presents yet another certificate on the reconnect it is a mismatch again.
  bool confirmCertificateChange(const QString& confirmedFingerprint);
  void requestPairing();  // NeedsPairing/Denied/Expired -> AwaitingApproval
  // Onboarding invite (POST /invites/redeem) from NeedsPairing/Denied/Expired: 200 -> credential stored, signs in
  // directly; 202 -> AwaitingApproval (same polling as requestPairing). Errors (invite_invalid, display_name_taken,
  // rate_limited, ...) keep the state and are reported via errorOccurred(code, message).
  // `password` is optional (empty = not sent; at least 12 characters, the server checks): the new person can then sign in
  // on another PC without waiting for approval.
  void redeemInvite(const QString& code, const QString& displayName, const QString& password = QString());
  // Sign in a new device with user name and password (POST /auth/sign-in, accounts_v1) from NeedsPairing/Denied/Expired: 200
  // -> credential stored, signs in directly (like an approved pairing). Errors keep the state and are reported via
  // errorOccurred(code, message): invalid_credentials, rate_limited (errorRetryAfter()), user_disabled, conflict (a device ID
  // of another person: retried once with a fresh one first), hub_too_old (no such endpoint), unreachable (no answer within
  // setRequestTimeoutMs()), invalid_input, bad_request, certificate problems go to CertificateChanged as everywhere.
  // A successful sign-in clears the PIN lock of that person on this PC.
  void signIn(const QString& username, const QString& password);
  // Sets up a server that has no admin yet (POST /setup, setup_code_v1) from NeedsPairing/Denied/Expired: creates the first
  // admin, pairs this device and signs in. Only when setupRequired() is true; otherwise errorOccurred("already_set_up")
  // without sending anything. Errors: invalid_setup_code, setup_code_expired, already_set_up, rate_limited, bad_request,
  // unreachable, invalid_input. Call it only after the security code was compared.
  void setupWithCode(const QString& setupCode, const QString& username, const QString& password);
  // Hub on this PC (0.9, feature local_setup_v1): only against a loopback address, otherwise errorOccurred("not_local").
  // GET /local/status (no auth) from NeedsPairing/Denied/Expired/Connected; the handler gets (ok, status object,
  // error message).
  void fetchLocalStatus(std::function<void(bool, const QJsonObject&, const QString&)> done);
  // POST /local/setup (creates the first admin) or /local/pair (existing admin) from NeedsPairing/Denied/Expired: the
  // Hub answers with the normal approved-pairing payload, which is stored like a normal pairing and signs in.
  // Errors (invalid credentials, admin_exists, rate_limited, ...) keep the state and are reported via errorOccurred().
  void localSetup(const QString& username, const QString& password);
  void localPair(const QString& username, const QString& password);
  void cancelPairing();   // AwaitingApproval -> NeedsPairing (local; the hub request expires on its own)
  void retry();           // Unreachable/UserDisabled: identify again
  void revokeSelf();      // Connected: the device revokes itself, credential is deleted -> NeedsPairing
  // Removes the profile and the credentials and PINs of all its accounts (not the server-side revocation).
  void removeProfile(const QString& hubId);

  // Save slots of one person on one server that are not uploaded yet (shown before removing the person from this PC).
  int pendingSaves(const QString& hubId, const QString& userId) const;
  enum class RemoveResult {
    Removed,       // done
    Revoking,      // the person is the connected one: the device is revoked on the server (best effort) and forgotten here
    PendingSaves,  // refused: pendingSaves() > 0 and not forced
    Unknown        // no such person on this PC
  };
  // Removes a person from this PC (8d-7a): deletes their device credential and PIN here, keeps the server account and all
  // saves. For the connected person the device credential is revoked on the server first and the connection goes to
  // NeedsPairing. Refused while they have saves not uploaded yet unless `force`. accountRemoved() follows.
  RemoveResult removeAccountFromThisPc(const QString& hubId, const QString& userId, bool force = false);

  // Authenticated requests relative to api_base (e.g. "/games"); nullptr if not Connected.
  // Replies are aborted on disconnect. The token never leaves this class.
  QNetworkReply* authorizedGet(const QString& apiPath, const HttpHeaders& headers = {});
  // Same for other methods with a raw body (PUT/POST); nullptr if not Connected.
  QNetworkReply* authorizedSend(const QByteArray& method, const QString& apiPath, const QByteArray& body,
                                const HttpHeaders& headers = {}, const QByteArray& contentType = "application/octet-stream");
  // Same with a streamed body (device stays open until the reply finishes; the caller parents it to the reply).
  QNetworkReply* authorizedSendStream(const QByteArray& method, const QString& apiPath, QIODevice* body,
                                      const HttpHeaders& headers = {}, const QByteArray& contentType = "application/octet-stream");
  // Caller reports 401: the token is renewed immediately, or -> NeedsPairing if the credential is invalid.
  void noteUnauthorized();

  // Upgrade request for the WSS endpoint (apiPath relative to api_base, e.g. "/ws"): wss/ws URL of the hub, Bearer
  // header and the pinned TLS configuration (no system CAs). Default-constructed (empty URL) if not Connected.
  // The token stays inside the request. HubSocket additionally verifies the leaf against pinnedFingerprint().
  QNetworkRequest webSocketRequest(const QString& apiRelPath) const;
  QString pinnedFingerprint() const { return pin_; }

 signals:
  void stateChanged(uzume::HubConnection::State state);
  void errorOccurred(const QString& code, const QString& message);
  void handshakeProblemsChanged();
  void accountRemoved(const QString& hubId, const QString& userId);

 private:
  using Handler = std::function<void(const HttpResult&)>;

  void reset();
  void setState(State s);
  void fail(State s, const QString& code, const QString& message);
  enum class IdentifyMode { Normal, AddAccount, SignInAs };
  void startIdentify(const QString& addressInput, bool allowHttp, std::optional<HubProfile> existing,
                     IdentifyMode mode = IdentifyMode::Normal, const QString& userId = QString());
  void track(QNetworkReply* reply, Handler handler);
  QString apiPath(const QString& rel) const;

  void onIdentified(const HttpResult& r);
  void trusted();
  void authenticate();
  void onTokenResult(const HttpResult& r, bool initial);
  void doHandshake();
  void onCredentialInvalid(const QString& code, const QString& message);
  void scheduleRefresh(int expiresInSec);
  void refreshToken();
  void pollPairing();
  void onPairingApproved(const QJsonObject& obj, bool passwordProof = false);
  void reportError(const QString& code, const QString& message, int retryAfterSec = 0);
  QString pairingDeviceId() const;
  QJsonObject deviceFields(const QString& versionKey) const;
  void sendDeviceRequest(const QString& path, const QJsonObject& fields, const QString& versionKey, const QString& fallbackCode,
                         bool retryOnConflict);
  void forgetAccountLocally(const QString& hubId, const HubAccount& account);
  bool handleCommonFailure(const HttpResult& r);
  bool handleUserDisabled(const HttpResult& r);
  void onPairingAccepted(const QJsonObject& o);
  void localPairingRequest(const QString& path, const QString& username, const QString& password);
  bool isLoopbackHub() const;

  ProfileStore* profiles_;
  CredentialStore* credentials_;
  HandshakeInfo handshake_;
  int pollIntervalMs_ = 2000;

  State state_ = State::Disconnected;
  IncompatibleReason incompatible_ = IncompatibleReason::None;
  QString errorCode_;
  QString errorMessage_;
  int errorRetryAfter_ = 0;
  QString address_;
  bool allowHttp_ = false;
  QString pin_;
  QString observedFp_;
  HubInfo hubInfo_;
  std::optional<HubProfile> profile_;
  QList<HandshakeProblem> problems_;
  QStringList features_;

  quint64 gen_ = 0;
  HubHttp* http_ = nullptr;
  QSet<QNetworkReply*> inflight_;
  QByteArray accessToken_;
  QTimer refreshTimer_;
  QTimer pollTimer_;
  QString pairingRequestId_;
  QByteArray pollToken_;
  bool pollInFlight_ = false;
  bool refreshInFlight_ = false;
  int requestTimeoutMs_ = 10000;
  QString nextExpectedFp_;   // from setExpectedFingerprint(), taken over by the next connect
  QString expectedFp_;       // the security code of the invite link for this connection
  bool linkTrusted_ = false;
  IdentifyMode mode_ = IdentifyMode::Normal;
  QString modeUserId_;       // SignInAs: the person
  QString signInName_;       // sign-in name of the request in flight (remembered with the account on success)
};

}  // namespace uzume
