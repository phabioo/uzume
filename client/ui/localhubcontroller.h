#pragma once
// LocalHubController: "Set up a server on this PC" (0.9 "One installer"). Installs the Hub when none is registered
// (Windows MSI), connects to the Hub on this PC over loopback (its self-signed certificate is trusted automatically for
// exactly this address), creates the first Hub admin or signs in as an existing admin, and edits the Hub's local
// settings (network sharing, ROM folder) afterwards.
//
// 0.18 package 10 "On this PC and sharing" (ADR 0030 D6) adds the child mode next to that service flow:
//   mode ""        nothing chosen yet
//   mode "child"   "Play on this PC": uzume-server runs as a child process of the app (LocalServerProcess), loopback only,
//                  per user. The first admin is hidden (random password in the CredentialStore only).
//   mode "service" "Share with others": the same server as the Windows service (one UAC; data moved, never overwritten).
// startChild() / beginShare() / stopSharing() / shareAgain() move between them; accounts and saves are kept.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <memory>
#include <optional>

#include "accountapi.h"
#include "adminapi.h"
#include "credentialstore.h"
#include "hubconnection.h"
#include "hubsetup.h"
#include "localhub.h"
#include "localserverprocess.h"
#include "playersettings.h"
#include "profilestore.h"

namespace uzume::ui {

class LocalHubController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Owned by PlayerController")

  // The button is shown: a Hub is registered on this PC, or the Player can install one (Windows).
  Q_PROPERTY(bool available READ available NOTIFY changed)
  Q_PROPERTY(bool present READ present NOTIFY changed)
  // idle | installing | connecting | needsSetup | needsSignIn | working | error
  Q_PROPERTY(QString phase READ phase NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString installText READ installText NOTIFY changed)
  Q_PROPERTY(double installProgress READ installProgress NOTIFY changed)
  // The active Hub is the Hub on this PC: "This PC's Hub" card.
  Q_PROPERTY(bool onThisPc READ onThisPc NOTIFY changed)
  Q_PROPERTY(bool networkSharing READ networkSharing NOTIFY changed)
  Q_PROPERTY(QString importDir READ importDir NOTIFY changed)
  Q_PROPERTY(QString settingsError READ settingsError NOTIFY changed)
  Q_PROPERTY(QString settingsNotice READ settingsNotice NOTIFY changed)
  Q_PROPERTY(bool settingsBusy READ settingsBusy NOTIFY changed)
  Q_PROPERTY(QString hubWebUrl READ hubWebUrl NOTIFY changed)

  // ---- 0.18 package 10. Phase additionally: "starting" (the server process of "Play on this PC" is starting).
  // "" | "child" | "service"
  Q_PROPERTY(QString mode READ mode NOTIFY changed)
  // Connected to the server that runs as the child process of this app (nobody else can reach it): the server line says
  // "on this PC", Users / invites / Online are hidden, "Share with others" is offered.
  Q_PROPERTY(bool onThisPcOnly READ onThisPcOnly NOTIFY changed)
  // stopped | starting | running | failed
  Q_PROPERTY(QString childState READ childState NOTIFY changed)
  Q_PROPERTY(QString childError READ childError NOTIFY changed)
  // "Share with others" can be offered (Windows with the installer source).
  Q_PROPERTY(bool shareSupported READ shareSupported NOTIFY changed)
  // idle | preparing | installing | stopping | resuming | done | failed
  Q_PROPERTY(QString sharePhase READ sharePhase NOTIFY changed)
  // admin_rights_refused | port_in_use | install_failed | password_too_short | name_taken | invalid_input | invalid_port |
  // not_connected | not_supported | no_local_admin | prepare_failed | stop_failed | start_failed | busy ; empty = none
  Q_PROPERTY(QString shareErrorCode READ shareErrorCode NOTIFY changed)
  Q_PROPERTY(QString shareError READ shareError NOTIFY changed)  // English copy (design 8i-6)
  Q_PROPERTY(int shareSuggestedPort READ shareSuggestedPort NOTIFY changed)  // "Use 8444" after port_in_use
  // The installer finished and the server now runs as the service: the "You're sharing" dialog (8i-4) is due.
  Q_PROPERTY(bool shareDone READ shareDone NOTIFY changed)
  // mode "service": "Stop sharing..." is offered.
  Q_PROPERTY(bool canStopSharing READ canStopSharing NOTIFY changed)
  // Which "Share with others" dialog is open: "" | "intro" (8i-1) | "setup" (8i-2) | "stop" (8i-5). The progress (8i-3), done (8i-4), error
  // (8i-6) and stopped dialogs follow sharePhase / shareDone / sharingStopped and need no state here.
  Q_PROPERTY(QString shareDialog READ shareDialog NOTIFY changed)
  // The port of the last share attempt (port_in_use names it) and the "Who can join" choice of the finished share ("home" | "internet").
  Q_PROPERTY(int sharePort READ sharePort NOTIFY changed)
  Q_PROPERTY(QString shareJoin READ shareJoin NOTIFY changed)
  // After the installer step failed (declined, port taken, rolled back) the server already has the new name and password:
  // retryShare() only repeats the installer step. False when the form must be filled again.
  Q_PROPERTY(bool canRetryShare READ canRetryShare NOTIFY changed)
  // "Stop sharing" finished: the server runs as the child on the data of the former service (8i-6e).
  Q_PROPERTY(bool sharingStopped READ sharingStopped NOTIFY changed)
  // mode "child" on the data of a former service: "Share again..." is offered.
  Q_PROPERTY(bool shareAgainAvailable READ shareAgainAvailable NOTIFY changed)

 public:
  struct Deps {
    HubConnection* conn = nullptr;
    // Starts the normal "add hub" attempt for the address (PlayerController::addHub).
    std::function<void(const QString&)> addHub;
    update::HubSetupInstaller::Config installer;
    // 0.18 package 10 (child mode, sharing); without them only the service flow works.
    ProfileStore* profiles = nullptr;
    CredentialStore* credentials = nullptr;
    PlayerSettings* settings = nullptr;
    std::function<void(const QString&)> connectProfile;  // PlayerController::connectProfile
    // Called before the server of this PC is stopped for sharing / stopping: ends a running game and uploads the saves.
    std::function<void()> beforeServerStop;
  };
  explicit LocalHubController(const Deps& deps, QObject* parent = nullptr);

  bool available() const;
  bool present() const { return info_.present(); }
  QString phase() const { return phase_; }
  QString error() const { return error_; }
  QString installText() const;
  double installProgress() const { return installer_.progress(); }
  bool onThisPc() const;
  bool networkSharing() const { return networkSharing_; }
  QString importDir() const { return importDir_; }
  QString settingsError() const { return settingsError_; }
  QString settingsNotice() const { return settingsNotice_; }
  bool settingsBusy() const { return settingsBusy_; }
  QString hubWebUrl() const;
  QString mode() const;
  bool onThisPcOnly() const;
  QString childState() const;
  QString childError() const { return server_.error(); }
  bool shareSupported() const;
  QString sharePhase() const { return sharePhase_; }
  QString shareErrorCode() const { return shareErrorCode_; }
  QString shareError() const { return shareError_; }
  int shareSuggestedPort() const { return shareSuggestedPort_; }
  bool shareDone() const { return shareDone_; }
  QString shareDialog() const { return shareDialog_; }
  int sharePort() const { return sharePort_; }
  QString shareJoin() const { return shareJoin_; }
  bool canRetryShare() const { return retryPort_ > 0 && mode() == QLatin1String("child"); }
  bool sharingStopped() const { return sharingStopped_; }
  bool canStopSharing() const { return mode() == QLatin1String("service"); }
  bool shareAgainAvailable() const;

  update::HubSetupInstaller* installer() { return &installer_; }
  // Characters the server accepts for a user name (^[A-Za-z0-9._-]{1,64}$); anything else is dropped, spaces become '_'; fallback "player".
  static QString sanitizeUsername(const QString& raw);
  LocalServerProcess* serverProcess() { return &server_; }
  // Tests: replaces the elevated `grant-folder` call (exit code, error text).
  void setGrantRunner(std::function<int(const QString& program, const QStringList& args, QString* error)> f) { grant_ = std::move(f); }

 // Tests: replaces the elevated `sharing stop|start` calls (exit code, error text).
  void setSharingRunner(std::function<int(const QString& program, const QStringList& args, QString* error)> f) { sharing_ = std::move(f); }
  // Tests: replaces the MSI launch of "Share with others" (gets the MSI properties; false = could not start).
  void setShareStarter(std::function<bool(const QStringList& msiProperties)> f) { shareStarter_ = std::move(f); }
  // Tests: "Share with others" is offered although this is not Windows.
  void setShareSupportedForTests(bool on) { shareSupportedTest_ = on; }
  void failShareForTests(const QString& code, int port = 0) {
    failShare(code, {}, port);
    if (code == QLatin1String("port_in_use") && port > 0) { shareSuggestedPort_ = port + 1; emit changed(); }
  }
  // Tests: changes the process settings (backoff, timeouts) before the child starts.
  void setServerConfigPatch(std::function<void(LocalServerProcess::Config&)> f) { serverConfigPatch_ = std::move(f); }

  // ---- "Play on this PC" (child mode)
  // Starts the server as a child process, connects to it over loopback (certificate trusted for exactly that address and
  // port), creates the hidden admin or pairs with the stored password, and saves mode "child".
  Q_INVOKABLE void startChild();
  // App start with mode "child" (PlayerController::startup): starts the server again; connects to it when `connect`.
  // Also finishes a pending "Share with others". Returns false when there is nothing to do (mode is not "child"/"service"
  // and nothing is pending).
  bool resumeAtStartup(bool connectToServer);
  Q_INVOKABLE void restartChild();
  // App quit, after the save sync: closes the server's stdin and waits for it (kills after 10 s).
  void stopChild();
  // "Confirm it's you" on the server of this PC: the hidden admin password while onThisPcOnly and it is stored, else empty.
  QString autoConfirmPassword() const;

  // ---- "Share with others" / "Stop sharing" (Windows service)
  // Server name, your name, password (>= 12), join "home" | "internet" (recorded for the settings after sharing), port.
  // Runs on the child server (rename, password), stores the pending record, stops the child and launches the MSI with
  // INSTALL_HUB=1 HUB_PORT NETWORK_SHARING=1 IMPORT_SERVER_DATA; the app quits (quitRequested). The next start finishes it.
  Q_INVOKABLE void beginShare(const QString& serverName, const QString& yourName, const QString& password, const QString& join, int port);
  Q_INVOKABLE void openShareDialog(const QString& which);  // intro | setup | stop
  Q_INVOKABLE void closeShareDialog();
  // Repeats the installer step with the same names and password (port 0 = the same port, else e.g. shareSuggestedPort).
  Q_INVOKABLE void retryShare(int port = 0);
  Q_INVOKABLE void acknowledgeStopped();
  Q_INVOKABLE void acknowledgeShareDone();
  Q_INVOKABLE void dismissShareError();
  // After "Confirm it's you": elevated `uzume-server sharing stop`, then back to the child process on the same data.
  Q_INVOKABLE void stopSharing();
  // Child on the data of the former service: elevated `sharing start`, mode "service".
  Q_INVOKABLE void shareAgain();

  // Button "Set up a server on this PC" (also `--setup-local-hub` after the MSI install).
  Q_INVOKABLE void start();
  // Like start(), but waits up to ~30 s for the freshly installed Hub service (`--setup-local-hub`).
  Q_INVOKABLE void startAfterInstall();
  Q_INVOKABLE void cancel();
  // Form of the pairing screen: first admin (needsSetup, password twice) or an existing admin (needsSignIn).
  Q_INVOKABLE void submit(const QString& username, const QString& password, const QString& passwordAgain);
  Q_INVOKABLE void setNetworkSharing(bool on);
  // Folder from the picker (path or file:// URL): grants the Hub service read access (Windows, UAC) and saves it.
  Q_INVOKABLE void chooseFolder(const QString& pathOrUrl);
  Q_INVOKABLE void openHubWebUi();
  Q_INVOKABLE void refreshStatus();

 signals:
  void changed();
  void setupCompleted();  // paired and connected to the Hub on this PC
  void quitRequested();  // installer started: the Player exits
  void childConnected();  // the app is connected to its own child server (after startChild / restartChild / app start)

 private:
  void onState(HubConnection::State s);
  void onError(const QString& code, const QString& message);
  void loadStatus(bool forPairingForm);
  void applyStatus(const QJsonObject& st);
  void putSettings(const QJsonObject& body, const std::function<void(bool, const QString&)>& done);
  void setPhase(const QString& p, const QString& err = {});
  static QString friendly(const QString& code, const QString& message);

  struct HiddenAdmin {
    QString username;
    QString password;
  };
  void onChildState(HubConnection::State s);
  void onServerStateChanged();
  void onChildReady(int port);
  void connectToChild();
  void pairChild();
  void finishChild();
  void failChild(const QString& text);
  bool startServerProcess();
  QString effectiveDataDir() const;
  QString hiddenTarget() const;
  // `readFailed` (optional) is set when the credential store itself failed (locked, dismissed, timed out), which
  // is not the same as nothing stored.
  std::optional<HiddenAdmin> hiddenAdmin(bool* readFailed = nullptr) const;
  bool storeHiddenAdmin(const HiddenAdmin& a);
  void clearHiddenAdmin();
  static QString osUserName();
  static QString randomPassword();
  void failShare(const QString& code, const QString& text = {}, int port = 0);
  void setShareProgress(const QString& phase);
  static QString shareCopy(const QString& code, const QString& detail, int port);
  void adminRequest(const QString& method, const QString& path, const QVariantMap& body, const std::function<void(const AdminApi::Result&)>& done,
                    bool confirmed = false);
  void shareSteps(const QString& serverName, const QString& yourName, const QString& password, const QString& join, int port);
  void launchShareInstaller(int port);
  void rollbackShare(const QString& code, const QString& text);
  bool resolvePendingShare();
  // Runs `uzume-server <args> --result FILE` elevated (sharing_), reads the result file; `what` is "stop" | "start".
  void runSharing(const QStringList& args, const QString& what, const std::function<void(const QJsonObject&)>& done);
  void goToService(int port);
  void goToChild(const QString& dataDir, int port);
  void beforeStop();

  Deps deps_;
  update::HubSetupInstaller installer_;
  LocalServerProcess server_;
  AdminApi adminApi_;
  AccountApi accountApi_;
  bool manualForm_ = false;   // the sign-in form of a child attempt was submitted by the person (not the hidden admin)
  bool childFlow_ = false;     // a child attempt is running: its loopback address is trusted automatically
  int childPort_ = 0;
  int childRetries_ = 0;
  bool shareHandled_ = false;
  QString sharePhase_ = QStringLiteral("idle");
  QString shareErrorCode_;
  QString shareError_;
  int shareSuggestedPort_ = 0;
  bool shareDone_ = false;
  bool sharingStopped_ = false;
  int sharePort_ = 0;
  QString shareJoin_;
  int retryPort_ = 0;
  QString retryJoin_;
  QString shareDialog_;
  bool shareSupportedTest_ = false;
  std::function<int(const QString&, const QStringList&, QString*)> sharing_;
  std::function<bool(const QStringList&)> shareStarter_;
  std::function<void(LocalServerProcess::Config&)> serverConfigPatch_;
  localhub::Info info_;
  int retries_ = 0;
  bool setupForm_ = false;  // the last form was "Create the Hub admin"
  bool active_ = false;  // a "Set up a server on this PC" attempt is running: its loopback address is trusted automatically
  QString phase_ = QStringLiteral("idle");
  QString error_;
  bool networkSharing_ = false;
  QString importDir_;
  QString settingsError_;
  QString settingsNotice_;
  bool settingsBusy_ = false;
  std::function<int(const QString&, const QStringList&, QString*)> grant_;
};

}  // namespace uzume::ui
