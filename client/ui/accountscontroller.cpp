#include "accountscontroller.h"

#include <QDateTime>
#include <algorithm>

namespace uzume::ui {

using State = HubConnection::State;

namespace {

QString durationText(int seconds) {
  if (seconds >= 120) return AccountsController::tr("%1 minutes").arg((seconds + 59) / 60);
  if (seconds >= 60) return AccountsController::tr("1 minute");
  return AccountsController::tr("%1 seconds").arg(std::max(seconds, 1));
}

QString isoOrEmpty(const QDateTime& t) { return t.isValid() ? t.toUTC().toString(Qt::ISODate) : QString(); }

QVariantMap accountMap(const AccountInfo& a, bool hasPin) {
  QVariantMap m;
  m[QStringLiteral("userId")] = a.userId;
  m[QStringLiteral("username")] = a.username;
  m[QStringLiteral("displayName")] = a.displayName;
  m[QStringLiteral("role")] = a.role;
  m[QStringLiteral("isAdmin")] = a.isAdmin();
  m[QStringLiteral("hasPassword")] = a.hasPassword;
  m[QStringLiteral("passwordRequired")] = a.passwordRequired;
  m[QStringLiteral("mustChangePassword")] = a.mustChangePassword;
  m[QStringLiteral("hasPin")] = hasPin;
  return m;
}

}  // namespace

AccountsController::AccountsController(const Deps& deps, QObject* parent)
    : QObject(parent), d_(deps), pin_(deps.profiles, deps.credentials) {
  connect(d_.conn, &HubConnection::stateChanged, this, &AccountsController::onState);
  connect(d_.conn, &HubConnection::errorOccurred, this, &AccountsController::onError);
  connect(d_.conn, &HubConnection::accountRemoved, this, [this] { emit changed(); });
}

AccountsController::~AccountsController() = default;

// ---------------------------------------------------------------- State

QString AccountsController::hubId() const {
  const QString id = d_.conn->hubId();
  if (!id.isEmpty()) return id;
  const auto p = d_.conn->profile();
  return p ? p->hubId : QString();
}

bool AccountsController::supported() const {
  return d_.conn->hubHasFeature(QStringLiteral("accounts_v1"));
}

QString AccountsController::phase() const {
  switch (phase_) {
    case Phase::Idle: return QStringLiteral("idle");
    case Phase::People: return QStringLiteral("people");
    case Phase::QuitGame: return QStringLiteral("quit_game");
    case Phase::Pin: return QStringLiteral("pin");
    case Phase::Password: return QStringLiteral("password");
    case Phase::Switching: return QStringLiteral("switching");
    case Phase::Add: return QStringLiteral("add");
    case Phase::ConfirmRemove: return QStringLiteral("confirm_remove");
    case Phase::RemoveBlocked: return QStringLiteral("remove_blocked");
  }
  return QStringLiteral("idle");
}

void AccountsController::setPhase(Phase p) {
  phase_ = p;
  emit changed();
}

bool AccountsController::takesScreen() const {
  const bool away = phase_ == Phase::Password || phase_ == Phase::Switching || phase_ == Phase::Add;
  return away && d_.conn->state() != State::Connected;
}

QVariantList AccountsController::people() const {
  QVariantList out;
  const QString hub = hubId();
  const auto profile = d_.profiles->profile(hub);
  if (!profile) return out;
  QList<HubAccount> list = profile->accounts;
  std::stable_sort(list.begin(), list.end(), [&](const HubAccount& a, const HubAccount& b) {
    const bool aa = a.hubUserId == profile->activeUserId;
    const bool bb = b.hubUserId == profile->activeUserId;
    if (aa != bb) return aa;
    return a.lastUsed > b.lastUsed;
  });
  for (const HubAccount& a : list) {
    QVariantMap m;
    m[QStringLiteral("userId")] = a.hubUserId;
    m[QStringLiteral("name")] = a.displayName;
    m[QStringLiteral("username")] = a.username;
    m[QStringLiteral("role")] = a.role;
    m[QStringLiteral("isAdmin")] = a.role == QLatin1String("admin");
    m[QStringLiteral("lastUsed")] = isoOrEmpty(a.lastUsed);
    m[QStringLiteral("hasPin")] = a.hasPin;
    m[QStringLiteral("pinLocked")] = a.hasPin && a.pinFailures >= kLocalPinMaxTries;
    m[QStringLiteral("isActive")] = a.hubUserId == profile->activeUserId;
    m[QStringLiteral("signedIn")] = !a.credentialRef.isEmpty();
    m[QStringLiteral("pendingSaves")] = d_.conn->pendingSaves(hub, a.hubUserId);
    out.append(m);
  }
  return out;
}

QString AccountsController::serverName() const {
  const QString n = d_.conn->hubInfo().name;
  if (!n.isEmpty()) return n;
  const auto p = d_.conn->profile();
  return p ? p->name : QString();
}

QString AccountsController::targetName() const {
  const auto a = d_.profiles->account(hubId(), target_);
  return a ? a->displayName : QString();
}

int AccountsController::pinTriesLeft() const { return pin_.triesLeft(hubId(), target_); }

int AccountsController::pendingCount() const { return target_.isEmpty() ? 0 : d_.conn->pendingSaves(hubId(), target_); }

bool AccountsController::addWaiting() const { return phase_ == Phase::Add && d_.conn->state() == State::AwaitingApproval; }

bool AccountsController::addDenied() const {
  return phase_ == Phase::Add && (d_.conn->state() == State::Denied || d_.conn->state() == State::Expired);
}

QVariantMap AccountsController::forgotPassword() const {
  const auto a = d_.profiles->account(hubId(), target_);
  const bool admin = a && a->role == QLatin1String("admin");
  QVariantMap m;
  m[QStringLiteral("isAdmin")] = admin;
  m[QStringLiteral("command")] = QStringLiteral("uzume-server reset-admin");
  m[QStringLiteral("dockerCommand")] = QStringLiteral("docker exec -it uzume-server uzume-server reset-admin");
  m[QStringLiteral("text")] =
      admin ? tr("There is no email reset. Run `uzume-server reset-admin` on the device that runs the server. It prints a one-time "
                 "password; saves, people and games stay as they are.")
            : tr("There is no email reset. Ask an admin of %1 to reset your password; they give you a temporary one.").arg(serverName());
  return m;
}

// ---------------------------------------------------------------- Errors

QString AccountsController::messageFor(const QString& code, const QString& serverMessage, int retryAfter) const {
  if (code == QLatin1String("invalid_credentials")) return tr("Wrong name or password.");
  if (code == QLatin1String("rate_limited")) {
    return retryAfter > 0 ? tr("Too many wrong tries. Try again in %1.").arg(durationText(retryAfter))
                          : tr("Too many tries. Please wait a moment and try again.");
  }
  if (code == QLatin1String("user_disabled")) return tr("This account has been turned off on %1. Your saves are kept.").arg(serverName());
  if (code == QLatin1String("conflict")) return tr("This PC could not be signed in. Please try again.");
  if (code == QLatin1String("unreachable")) return tr("No answer from the server after 10 seconds.");
  if (code == QLatin1String("not_connected")) return tr("Not connected to a server.");
  if (code == QLatin1String("not_supported")) return tr("This server is too old for accounts. Update the server first.");
  if (code == QLatin1String("hub_too_old")) return tr("%1 is too old for this version of Uzume.").arg(serverName());
  if (code == QLatin1String("display_name_taken")) {
    return tr("Someone on %1 is already called that. Add a letter or pick another name.").arg(serverName());
  }
  if (code == QLatin1String("password_required")) return tr("This account needs a password on %1.").arg(serverName());
  if (code == QLatin1String("password_not_set")) return tr("This account has no password.");
  if (code == QLatin1String("bad_request")) return tr("Check the entries. Passwords need at least 12 characters.");
  if (code == QLatin1String("invite_invalid")) return tr("This invite has expired, or all its uses are gone.");
  if (code == QLatin1String("invalid_input")) return tr("Enter your name and password.");
  if (code == QLatin1String("invalid_pin")) return tr("A PIN has four digits.");
  if (code == QLatin1String("password_mismatch")) return tr("The two passwords are not the same.");
  if (code == QLatin1String("password_too_short")) return tr("Use at least 12 characters.");
  return serverMessage.isEmpty() ? tr("Something went wrong (%1).").arg(code) : serverMessage;
}

void AccountsController::setError(const QString& code, const QString& message, int retryAfter) {
  errorCode_ = code;
  retryAfter_ = retryAfter;
  errorMessage_ = messageFor(code, message, retryAfter);
  emit changed();
}

void AccountsController::clearError() {
  if (errorCode_.isEmpty() && errorMessage_.isEmpty()) return;
  errorCode_.clear();
  errorMessage_.clear();
  retryAfter_ = 0;
  emit changed();
}

// ---------------------------------------------------------------- Connection

void AccountsController::onError(const QString& code, const QString& message) {
  if (code.isEmpty()) return;
  if (phase_ != Phase::Switching && phase_ != Phase::Password && phase_ != Phase::Add) return;
  busy_ = false;
  hasPending_ = false;
  pendingPassword_.clear();
  setError(code, message, d_.conn->errorRetryAfter());
}

void AccountsController::onState(State s) {
  if (phase_ == Phase::Idle || phase_ == Phase::People || phase_ == Phase::QuitGame || phase_ == Phase::Pin ||
      phase_ == Phase::ConfirmRemove || phase_ == Phase::RemoveBlocked) {
    emit changed();  // people / takesScreen follow the connection
    return;
  }
  switch (s) {
    case State::Connected:
      finishSwitch();
      return;
    case State::NeedsPairing:
      busy_ = false;
      passwordReady_ = true;
      if (phase_ == Phase::Switching) {
        phase_ = Phase::Password;  // signed out on this PC: the password is needed
      }
      if (hasPending_) {
        hasPending_ = false;
        const QString name = pendingName_, pw = pendingPassword_;
        pendingPassword_.clear();
        signInWithPassword(name, pw);
        return;
      }
      break;
    case State::Identifying:
    case State::Authenticating:
      busy_ = true;
      break;
    case State::AwaitingApproval:
    case State::Denied:
    case State::Expired:
      busy_ = false;
      break;
    case State::Unreachable:
    case State::UserDisabled:
    case State::Disconnected:
    case State::Incompatible:
    case State::CertificateChanged:
    case State::NeedsTrustConfirmation:
      busy_ = false;
      hasPending_ = false;
      pendingPassword_.clear();
      break;
  }
  emit changed();
}

void AccountsController::finishSwitch() {
  busy_ = false;
  passwordReady_ = false;
  hasPending_ = false;
  pendingPassword_.clear();
  const bool added = phase_ == Phase::Add;
  const bool back = reconnecting_;
  reconnecting_ = false;
  clearError();
  if (const auto p = d_.conn->profile(); p && !p->hubUserId.isEmpty()) {
    if (auto a = d_.profiles->account(p->hubId, p->hubUserId)) {
      a->lastUsed = QDateTime::currentDateTimeUtc();
      d_.profiles->upsertAccount(p->hubId, *a);
    }
  }
  target_.clear();
  pinError_.clear();
  if (back) {
    setPhase(Phase::People);
    return;
  }
  setPhase(Phase::Idle);
  emit done(added ? QStringLiteral("person_added") : QStringLiteral("switched"));
}

void AccountsController::reconnectBefore() {
  reconnecting_ = true;
  busy_ = true;
  passwordReady_ = false;
  hasPending_ = false;
  pendingPassword_.clear();
  clearError();
  phase_ = Phase::Switching;
  const QString hub = hubId();
  if (!activeBefore_.isEmpty()) {
    d_.conn->connectToProfileAs(hub, activeBefore_);
  } else {
    d_.conn->connectToProfile(hub);
  }
  emit changed();
}

// ---------------------------------------------------------------- People

void AccountsController::open() {
  clearError();
  target_.clear();
  pinError_.clear();
  setPhase(Phase::People);
}

void AccountsController::close() {
  if (phase_ == Phase::Password || phase_ == Phase::Add || phase_ == Phase::Switching) {
    if (d_.conn->state() != State::Connected) {
      reconnectBefore();
      phase_ = Phase::Idle;  // closing: nobody waits for the old person to come back
      reconnecting_ = false;
    }
  }
  clearError();
  target_.clear();
  pinError_.clear();
  setPhase(Phase::Idle);
}

void AccountsController::switchTo(const QString& userId) {
  const QString hub = hubId();
  const auto acc = d_.profiles->account(hub, userId);
  clearError();
  if (!acc) return;
  const auto profile = d_.profiles->profile(hub);
  if (profile && profile->activeUserId == userId && d_.conn->state() == State::Connected) {
    close();
    return;
  }
  target_ = userId;
  pinError_.clear();
  activeBefore_ = profile ? profile->activeUserId : QString();
  if (acc->hasPin) {
    if (pin_.isLocked(hub, userId)) {
      pinError_ = QStringLiteral("locked");
      setPhase(Phase::Pin);
    } else {
      setPhase(Phase::Pin);
    }
    return;
  }
  afterAuthCheck();
}

void AccountsController::enterPin(const QString& pin) {
  if (phase_ != Phase::Pin) return;
  const auto r = pin_.verify(hubId(), target_, pin);
  switch (r.status) {
    case LocalPin::Status::Ok:
    case LocalPin::Status::NoPin:
      pinError_.clear();
      afterAuthCheck();
      return;
    case LocalPin::Status::Wrong:
      pinError_ = QStringLiteral("wrong");
      break;
    case LocalPin::Status::Locked:
      pinError_ = QStringLiteral("locked");
      break;
    case LocalPin::Status::Unavailable:
      pinError_ = QStringLiteral("unavailable");
      break;
  }
  emit changed();
}

void AccountsController::usePasswordInstead() {
  if (phase_ != Phase::Pin) return;
  pinError_.clear();
  startPasswordSignIn();
}

void AccountsController::afterAuthCheck() {
  if (d_.gameRunning && d_.gameRunning()) {
    setPhase(Phase::QuitGame);
    return;
  }
  startSwitch();
}

void AccountsController::confirmQuitAndSwitch() {
  if (phase_ != Phase::QuitGame) return;
  if (d_.endRunningWork) d_.endRunningWork();
  startSwitch();
}

void AccountsController::keepPlaying() {
  if (phase_ == Phase::Password && d_.conn->state() != State::Connected) {
    reconnectBefore();
    return;
  }
  clearError();
  target_.clear();
  pinError_.clear();
  setPhase(Phase::People);
}

void AccountsController::startSwitch() {
  clearError();
  busy_ = true;
  passwordReady_ = false;
  phase_ = Phase::Switching;
  d_.conn->connectToProfileAs(hubId(), target_);
  emit changed();
}

void AccountsController::startPasswordSignIn() {
  clearError();
  busy_ = true;
  passwordReady_ = false;
  hasPending_ = false;
  phase_ = Phase::Password;
  d_.conn->beginSignInAs(hubId(), target_);
  emit changed();
}

void AccountsController::signInWithPassword(const QString& name, const QString& password) {
  if (phase_ != Phase::Password) return;
  if (name.trimmed().isEmpty() || password.isEmpty()) {
    setError(QStringLiteral("invalid_input"));
    return;
  }
  clearError();
  busy_ = true;
  if (passwordReady_ && d_.conn->state() == State::NeedsPairing) {
    passwordReady_ = false;
    d_.conn->signIn(name.trimmed(), password);
  } else {
    pendingName_ = name.trimmed();
    pendingPassword_ = password;
    hasPending_ = true;
  }
  emit changed();
}

// ---------------------------------------------------------------- Add a person

void AccountsController::addPerson() {
  const QString hub = hubId();
  const auto profile = d_.profiles->profile(hub);
  if (!profile) return;
  clearError();
  activeBefore_ = profile->activeUserId;
  target_.clear();
  addView_ = QStringLiteral("sign_in");
  busy_ = true;
  passwordReady_ = false;
  phase_ = Phase::Add;
  d_.conn->beginAddAccount(hub);
  emit changed();
}

void AccountsController::addSignIn(const QString& name, const QString& password) {
  if (phase_ != Phase::Add) return;
  if (name.trimmed().isEmpty() || password.isEmpty()) {
    setError(QStringLiteral("invalid_input"));
    return;
  }
  if (d_.conn->state() != State::NeedsPairing) return;
  clearError();
  busy_ = true;
  d_.conn->signIn(name.trimmed(), password);
  emit changed();
}

void AccountsController::addRedeemInvite(const QString& code, const QString& name, const QString& password) {
  if (phase_ != Phase::Add) return;
  if (code.trimmed().isEmpty() || name.trimmed().isEmpty()) {
    setError(QStringLiteral("invalid_input"));
    return;
  }
  if (d_.conn->state() != State::NeedsPairing) return;
  clearError();
  busy_ = true;
  d_.conn->redeemInvite(code.trimmed(), name.trimmed(), password);
  emit changed();
}

void AccountsController::addRequestApproval() {
  if (phase_ != Phase::Add) return;
  const State s = d_.conn->state();
  if (s != State::NeedsPairing && s != State::Denied && s != State::Expired) return;
  clearError();
  d_.conn->requestPairing();
  emit changed();
}

void AccountsController::addCancelApproval() {
  if (phase_ != Phase::Add || d_.conn->state() != State::AwaitingApproval) return;
  d_.conn->cancelPairing();
  emit changed();
}

void AccountsController::addShowInvite() {
  addView_ = QStringLiteral("invite");
  clearError();
  emit changed();
}

void AccountsController::addShowSignIn() {
  addView_ = QStringLiteral("sign_in");
  clearError();
  emit changed();
}

void AccountsController::cancelAdd() {
  if (phase_ != Phase::Add) return;
  reconnectBefore();
}

// ---------------------------------------------------------------- Remove from this PC

void AccountsController::removeFromThisPc(const QString& userId) {
  if (!d_.profiles->account(hubId(), userId)) return;
  clearError();
  target_ = userId;
  setPhase(pendingCount() > 0 ? Phase::RemoveBlocked : Phase::ConfirmRemove);
}

void AccountsController::confirmRemove() {
  if (phase_ != Phase::ConfirmRemove) return;
  const QString hub = hubId();
  const QString user = target_;
  const auto profile = d_.profiles->profile(hub);
  const bool wasActive = profile && profile->activeUserId == user;
  const auto r = d_.conn->removeAccountFromThisPc(hub, user, false);
  switch (r) {
    case HubConnection::RemoveResult::Removed:
    case HubConnection::RemoveResult::Revoking:
      target_.clear();
      setPhase(wasActive ? Phase::Idle : Phase::People);
      emit done(QStringLiteral("person_removed"));
      return;
    case HubConnection::RemoveResult::PendingSaves:
      setPhase(Phase::RemoveBlocked);
      return;
    case HubConnection::RemoveResult::Unknown:
      target_.clear();
      setPhase(Phase::People);
      return;
  }
}

void AccountsController::confirmRemoveAnyway() {
  if (phase_ != Phase::RemoveBlocked) return;
  const QString hub = hubId();
  const QString user = target_;
  const auto profile = d_.profiles->profile(hub);
  const bool wasActive = profile && profile->activeUserId == user;
  d_.conn->removeAccountFromThisPc(hub, user, true);
  target_.clear();
  setPhase(wasActive ? Phase::Idle : Phase::People);
  emit done(QStringLiteral("person_removed"));
}

// ---------------------------------------------------------------- Account (8e)

AccountApi* AccountsController::api() {
  if (!api_) api_ = std::make_unique<AccountApi>(d_.conn, this);
  return api_.get();
}

void AccountsController::apiDone(const QString& what, const AccountApiResult& r,
                                 const std::function<void(const AccountApiResult&)>& ok) {
  busy_ = false;
  if (!r.ok) {
    setError(r.errorCode, r.errorMessage, r.retryAfter);
    return;
  }
  errorCode_.clear();
  errorMessage_.clear();
  retryAfter_ = 0;
  if (ok) ok(r);
  emit changed();
  emit done(what);
}

void AccountsController::refreshAccountPin() {
  if (account_.isEmpty()) return;
  account_[QStringLiteral("hasPin")] = pin_.hasPin(hubId(), account_.value(QStringLiteral("userId")).toString());
}

void AccountsController::loadAccount() {
  if (!supported()) {
    setError(QStringLiteral("not_supported"));
    return;
  }
  busy_ = true;
  clearError();
  api()->getAccount([this](const AccountApiResult& r) {
    apiDone(QStringLiteral("account_loaded"), r, [this](const AccountApiResult& res) {
      account_ = accountMap(res.account, pin_.hasPin(hubId(), res.account.userId));
    });
  });
}

void AccountsController::rename(const QString& displayName) {
  const QString name = displayName.trimmed();
  if (name.isEmpty()) {
    setError(QStringLiteral("invalid_input"));
    return;
  }
  busy_ = true;
  clearError();
  api()->updateDisplayName(name, [this](const AccountApiResult& r) {
    apiDone(QStringLiteral("renamed"), r, [this](const AccountApiResult& res) {
      account_ = accountMap(res.account, pin_.hasPin(hubId(), res.account.userId));
      const QString hub = hubId();
      if (auto a = d_.profiles->account(hub, res.account.userId)) {
        a->displayName = res.account.displayName;
        d_.profiles->upsertAccount(hub, *a);
      }
    });
  });
}

void AccountsController::setPassword(const QString& current, const QString& password, const QString& repeat) {
  if (password != repeat) {
    setError(QStringLiteral("password_mismatch"));
    return;
  }
  if (password.size() < 12) {
    setError(QStringLiteral("password_too_short"));
    return;
  }
  busy_ = true;
  clearError();
  api()->setPassword(current, password, [this](const AccountApiResult& r) {
    apiDone(QStringLiteral("password_set"), r, [this](const AccountApiResult&) {
      account_[QStringLiteral("hasPassword")] = true;
      account_[QStringLiteral("mustChangePassword")] = false;
    });
  });
}

void AccountsController::removePassword(const QString& current) {
  busy_ = true;
  clearError();
  api()->removePassword(current, [this](const AccountApiResult& r) {
    apiDone(QStringLiteral("password_removed"), r, [this](const AccountApiResult&) {
      account_[QStringLiteral("hasPassword")] = false;
    });
  });
}

bool AccountsController::setPin(const QString& pin) {
  const auto p = d_.conn->profile();
  if (!p || p->hubUserId.isEmpty()) {
    setError(QStringLiteral("not_connected"));
    return false;
  }
  if (!LocalPin::isValidPin(pin)) {
    setError(QStringLiteral("invalid_pin"));
    return false;
  }
  if (!pin_.setPin(p->hubId, p->hubUserId, pin)) {
    setError(QStringLiteral("credential_store_failed"));
    return false;
  }
  clearError();
  refreshAccountPin();
  emit changed();
  emit done(QStringLiteral("pin_set"));
  return true;
}

void AccountsController::removePin() {
  const auto p = d_.conn->profile();
  if (!p || p->hubUserId.isEmpty()) return;
  pin_.removePin(p->hubId, p->hubUserId);
  clearError();
  refreshAccountPin();
  emit changed();
  emit done(QStringLiteral("pin_removed"));
}

void AccountsController::loadDevices() {
  if (!supported()) {
    setError(QStringLiteral("not_supported"));
    return;
  }
  busy_ = true;
  clearError();
  api()->listDevices([this](const AccountApiResult& r) {
    apiDone(QStringLiteral("devices_loaded"), r, [this](const AccountApiResult& res) {
      devices_.clear();
      for (const AccountDeviceInfo& d : res.devices) {
        QVariantMap m;
        m[QStringLiteral("deviceId")] = d.deviceId;
        m[QStringLiteral("name")] = d.name;
        m[QStringLiteral("platform")] = d.platform;
        m[QStringLiteral("appVersion")] = d.appVersion;
        m[QStringLiteral("lastSeen")] = isoOrEmpty(d.lastSeenAt);
        m[QStringLiteral("signInKind")] = d.signInKind;
        m[QStringLiteral("revoked")] = d.revoked();
        m[QStringLiteral("current")] = d.current;
        devices_.append(m);
      }
    });
  });
}

void AccountsController::signOutDevice(const QString& deviceId) {
  busy_ = true;
  clearError();
  api()->signOutDevice(deviceId, [this](const AccountApiResult& r) {
    apiDone(QStringLiteral("device_signed_out"), r);
    if (r.ok) loadDevices();
  });
}

void AccountsController::signOutEverywhere(bool includeThisDevice) {
  busy_ = true;
  clearError();
  api()->signOutAll(includeThisDevice, [this, includeThisDevice](const AccountApiResult& r) {
    apiDone(QStringLiteral("signed_out_everywhere"), r);
    if (r.ok && !includeThisDevice) loadDevices();
  });
}

}  // namespace uzume::ui
