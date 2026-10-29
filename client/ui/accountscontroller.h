#pragma once
// AccountsController (player.accounts): the people on this PC for the open server and the signed-in person's own account
// (0.18 package 7, docs/design/uzume/accounts-and-server.md 8d, 8e, 8g). Switching people (with the local PIN), adding a
// person, removing one from this PC, the account settings (name, password, PIN) and the person's devices. There is no QML in here.
//
// QML contract
//   * `people` is the list of 8d-1. Open the sheet with open(), close it with close(); `phase` says what the sheet shows.
//   * `player.screen == "start"` while takesScreen() is true: adding a person or signing one in with a password replaces the
//     screen the same way the first start does (the open server is not connected in between).
//   * `busy`, `errorCode`, `errorMessage`, `retryAfter` describe the last request; `done(what)` fires when one succeeded.
//
// Phases (`phase`)
//   idle            nothing open
//   people          8d-1: the list of people (`people`), switchTo(), addPerson(), removeFromThisPc()
//   quit_game       8d-5: a game runs -> confirmQuitAndSwitch() saves, uploads and quits it, then switches; keepPlaying()
//   pin             8d-2/8d-3: enterPin(pin); `pinTriesLeft`, `pinError` ("" | "wrong" | "locked" | "unavailable"); usePasswordInstead()
//   password        8d-7c/8d-7e: signInWithPassword(name, password) for `targetUserId` (session expired or PIN locked);
//                   `forgotPassword` has the 8g texts; keepPlaying()/close() leaves
//   switching       connecting as the chosen person
//   add             8d-6: sign in / redeem an invite / ask for approval as another person: `addView` ("sign_in" | "invite"),
//                   `addWaiting` (asked an admin), `addDenied`; addSignIn(), addRedeemInvite(), addRequestApproval(), cancelAdd()
//   confirm_remove  8d-7a: removeFromThisPc() asks; confirmRemove() does it
//   remove_blocked  8d-7b: the person has saves not uploaded yet (`pendingCount`); confirmRemoveAnyway() or close()
//
// Error codes (`errorCode`; "" = none)
//   invalid_credentials, rate_limited (+retryAfter), user_disabled, conflict, unreachable, not_connected, hub_too_old,
//   display_name_taken, password_required, password_not_set, bad_request, invite_invalid, invalid_input, invalid_pin,
//   password_mismatch, password_too_short, not_supported, and the server's own code for anything else.
//
// done(what): switched, person_added, person_removed, renamed, password_set, password_removed, pin_set, pin_removed,
//   device_signed_out, signed_out_everywhere

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <memory>

#include "accountapi.h"
#include "credentialstore.h"
#include "hubconnection.h"
#include "localpin.h"
#include "profilestore.h"

namespace uzume::ui {

class AccountsController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  // The server supports accounts (feature accounts_v1, known while connected); account and device calls report not_supported otherwise.
  Q_PROPERTY(bool supported READ supported NOTIFY changed)
  Q_PROPERTY(QString phase READ phase NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString errorCode READ errorCode NOTIFY changed)
  Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY changed)
  Q_PROPERTY(int retryAfter READ retryAfter NOTIFY changed)
  // The people on this PC for the open server, active first:
  // [{userId, name, username, role, isAdmin, lastUsed (ISO), hasPin, pinLocked, isActive, signedIn, pendingSaves}]
  Q_PROPERTY(QVariantList people READ people NOTIFY changed)
  Q_PROPERTY(QString serverName READ serverName NOTIFY changed)
  Q_PROPERTY(QString targetUserId READ targetUserId NOTIFY changed)  // the person being switched to / signed in / removed
  Q_PROPERTY(QString targetName READ targetName NOTIFY changed)
  Q_PROPERTY(int pinTriesLeft READ pinTriesLeft NOTIFY changed)
  Q_PROPERTY(QString pinError READ pinError NOTIFY changed)           // "" | "wrong" | "locked" | "unavailable"
  Q_PROPERTY(int pendingCount READ pendingCount NOTIFY changed)       // saves of targetUserId not uploaded yet
  // Add person (phase add)
  Q_PROPERTY(QString addView READ addView NOTIFY changed)             // "sign_in" | "invite"
  Q_PROPERTY(bool addWaiting READ addWaiting NOTIFY changed)          // an admin was asked to approve
  Q_PROPERTY(bool addDenied READ addDenied NOTIFY changed)            // denied or expired
  // 8g: {isAdmin (of targetUserId), command: "uzume-server reset-admin", dockerCommand, text} for the forgot-password sheet.
  Q_PROPERTY(QVariantMap forgotPassword READ forgotPassword NOTIFY changed)
  // The signed-in person's own account (loadAccount()): {userId, username, displayName, role, isAdmin, hasPassword,
  // passwordRequired, mustChangePassword, hasPin}; empty until loaded.
  Q_PROPERTY(QVariantMap account READ account NOTIFY changed)
  // The signed-in person's devices (loadDevices()):
  // [{deviceId, name, platform, appVersion, lastSeen (ISO or ""), signInKind, revoked, current}]
  Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)

 public:
  struct Deps {
    HubConnection* conn = nullptr;
    ProfileStore* profiles = nullptr;
    CredentialStore* credentials = nullptr;
    std::function<bool()> gameRunning;     // a game or a watched Session is running
    std::function<void()> endRunningWork;  // saves and uploads the running game (PlayerController::endRunningWork)
  };
  explicit AccountsController(const Deps& deps, QObject* parent = nullptr);
  ~AccountsController() override;

  bool supported() const;
  QString phase() const;
  bool busy() const { return busy_; }
  QString errorCode() const { return errorCode_; }
  QString errorMessage() const { return errorMessage_; }
  int retryAfter() const { return retryAfter_; }
  QVariantList people() const;
  QString serverName() const;
  QString targetUserId() const { return target_; }
  QString targetName() const;
  int pinTriesLeft() const;
  QString pinError() const { return pinError_; }
  int pendingCount() const;
  QString addView() const { return addView_; }
  bool addWaiting() const;
  bool addDenied() const;
  QVariantMap forgotPassword() const;
  QVariantMap account() const { return account_; }
  QVariantList devices() const { return devices_; }

  // This controller owns the screen while a person is added or signed in with a password (PlayerController::updateScreen).
  bool takesScreen() const;

  // ---- people ----
  Q_INVOKABLE void open();
  Q_INVOKABLE void close();  // leaves every phase; abandons an add / password sign-in and reconnects the open person
  // Switch to a person. Own account: nothing. PIN first when the person has one (phase pin; locked -> password), then
  // quit_game when a game runs, then the switch.
  Q_INVOKABLE void switchTo(const QString& userId);
  Q_INVOKABLE void enterPin(const QString& pin);
  Q_INVOKABLE void usePasswordInstead();  // pin -> password
  Q_INVOKABLE void signInWithPassword(const QString& name, const QString& password);
  Q_INVOKABLE void confirmQuitAndSwitch();
  Q_INVOKABLE void keepPlaying();  // quit_game / password -> back to the list, nothing changed

  // ---- add a person ----
  Q_INVOKABLE void addPerson();
  Q_INVOKABLE void addSignIn(const QString& name, const QString& password);
  Q_INVOKABLE void addRedeemInvite(const QString& code, const QString& name, const QString& password);
  Q_INVOKABLE void addRequestApproval();
  Q_INVOKABLE void addCancelApproval();  // stop waiting for the admin
  Q_INVOKABLE void addShowInvite();   // sign_in -> invite
  Q_INVOKABLE void addShowSignIn();   // invite -> sign_in
  Q_INVOKABLE void cancelAdd();       // phase add -> people, the open person is connected again

  // ---- remove from this PC ----
  Q_INVOKABLE void removeFromThisPc(const QString& userId);
  Q_INVOKABLE void confirmRemove();
  Q_INVOKABLE void confirmRemoveAnyway();

  // ---- the signed-in person's account (8e) ----
  Q_INVOKABLE void loadAccount();
  Q_INVOKABLE void rename(const QString& displayName);
  // The new password is checked here (12 characters or more, both fields equal) before anything is sent.
  Q_INVOKABLE void setPassword(const QString& current, const QString& password, const QString& repeat);
  Q_INVOKABLE void removePassword(const QString& current);
  Q_INVOKABLE bool setPin(const QString& pin);  // four digits; this PC only
  Q_INVOKABLE void removePin();
  Q_INVOKABLE void loadDevices();
  Q_INVOKABLE void signOutDevice(const QString& deviceId);
  Q_INVOKABLE void signOutEverywhere(bool includeThisDevice);
  Q_INVOKABLE void clearError();

 signals:
  void changed();
  void done(const QString& what);

 private:
  enum class Phase { Idle, People, QuitGame, Pin, Password, Switching, Add, ConfirmRemove, RemoveBlocked };

  QString hubId() const;
  void setPhase(Phase p);
  void setError(const QString& code, const QString& message = QString(), int retryAfter = 0);
  QString messageFor(const QString& code, const QString& serverMessage, int retryAfter) const;
  void onState(HubConnection::State s);
  void onError(const QString& code, const QString& message);
  void afterAuthCheck();
  void startSwitch();
  void reconnectBefore();
  void startPasswordSignIn();
  void finishSwitch();
  void apiDone(const QString& what, const AccountApiResult& r, const std::function<void(const AccountApiResult&)>& ok = {});
  AccountApi* api();
  void refreshAccountPin();

  Deps d_;
  LocalPin pin_;
  std::unique_ptr<AccountApi> api_;
  Phase phase_ = Phase::Idle;
  bool busy_ = false;
  QString errorCode_;
  QString errorMessage_;
  int retryAfter_ = 0;
  QString target_;
  QString pinError_;
  QString addView_ = QStringLiteral("sign_in");
  QString activeBefore_;         // the person connected before an add / password sign-in, to return to
  bool passwordReady_ = false;   // the connection waits for signIn() (NeedsPairing as the target person)
  QString pendingName_, pendingPassword_;  // typed before the connection was ready; memory only
  bool hasPending_ = false;
  bool reconnecting_ = false;    // returning to the open person after cancelAdd() / close()
  QVariantMap account_;
  QVariantList devices_;
};

}  // namespace uzume::ui
