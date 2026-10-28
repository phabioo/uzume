#pragma once
// AdminController: QML face of the admin area (0.18 package 8). PlayerController exposes it as `admin`.
//
// - `available`: connected as an admin to a server that advertises `admin_v1`. The SERVER group of AppNav and the server
//   screen depend on it.
// - Generic requests for every admin page: request() (JSON), download() (GET into a file), upload() (a file as body).
//   All of them answer asynchronously through replied() / downloadFinished() with the id the call returned. Paths are
//   relative to api_base and must start with /admin/ or /account/.
// - "Confirm it's you": a request answered 403 `confirmation_required` is parked (with every later one), `confirmNeeded`
//   turns true and the global UzConfirmDialog asks for the password. confirm() posts /account/confirm (300 s); on success
//   the parked requests run again with the same ids. cancelConfirm() fails them with `confirmation_cancelled`.
//   resumeParked() replays them when another component already confirmed. confirmSecondsLeft counts the time left.
// - Status: /admin/status is read when the server screen opens and every 60 s while `available` and no game is shown.
//   statusProblem is the title of the most urgent problem ("" = none) and feeds the badge on Status in AppNav.
// - serverUpdateVersion: the version in GET /admin/settings updates.available (not skipped); feeds the Settings badge and the Status list.
// - Counts for the badges of AppNav: pendingDevices (waiting pairing requests, Users) and newReports (Reports). They are read
//   with the status poll; pages call refreshCounts() after they changed something. thisDeviceId() names this PC's device.
//
// confirmState: "" | "asking" | "checking" | "wrong" | "locked" | "unreachable" | "unset"
// statusState:  "idle" | "loading" | "ready" | "unreachable" | "forbidden"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>

#include "adminapi.h"
#include "hubconnection.h"

namespace uzume::ui {

class AdminController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Owned by PlayerController")

  Q_PROPERTY(bool available READ available NOTIFY availableChanged)
  Q_PROPERTY(bool confirmNeeded READ confirmNeeded NOTIFY confirmChanged)
  Q_PROPERTY(QString confirmState READ confirmState NOTIFY confirmChanged)
  Q_PROPERTY(bool confirmAutomatic READ confirmAutomatic NOTIFY confirmChanged)  // answered by the app itself: no dialog
  Q_PROPERTY(QString confirmError READ confirmError NOTIFY confirmChanged)      // text for the dialog; empty = none
  Q_PROPERTY(int confirmLockedSeconds READ confirmLockedSeconds NOTIFY confirmChanged)
  Q_PROPERTY(QDateTime confirmedUntil READ confirmedUntil NOTIFY confirmChanged)
  Q_PROPERTY(int confirmSecondsLeft READ confirmSecondsLeft NOTIFY confirmChanged)  // 0 = not confirmed
  Q_PROPERTY(QString confirmLeftText READ confirmLeftText NOTIFY confirmChanged)    // "4:12"
  Q_PROPERTY(QString statusState READ statusState NOTIFY statusChanged)
  Q_PROPERTY(QVariantMap status READ status NOTIFY statusChanged)  // last AdminStatus (empty until read)
  Q_PROPERTY(QString statusProblem READ statusProblem NOTIFY statusChanged)
  Q_PROPERTY(int pendingDevices READ pendingDevices NOTIFY countsChanged)
  Q_PROPERTY(int newReports READ newReports NOTIFY countsChanged)
  Q_PROPERTY(QString serverUpdateVersion READ serverUpdateVersion NOTIFY countsChanged)  // newer server version on offer ("" = none)

 public:
  struct Deps {
    HubConnection* conn = nullptr;
    std::function<bool()> isAdmin;  // handshake user.role == "admin"
    std::function<bool()> inGame;   // a game is shown: no background polling
    // "Play on this PC": the person has no password. A non-empty answer is the password the server of this PC knows; it
    // answers "Confirm it's you" without asking (confirmAutomatic is true meanwhile). Empty or unset = ask.
    std::function<QString()> autoConfirmPassword;
  };
  explicit AdminController(const Deps& deps, QObject* parent = nullptr);
  ~AdminController() override;

  bool available() const { return available_; }
  bool confirmNeeded() const { return confirmNeeded_; }
  QString confirmState() const { return confirmState_; }
  bool confirmAutomatic() const { return confirmNeeded_ && autoTried_ && confirmState_ == QLatin1String("checking"); }
  QString confirmError() const;
  int confirmLockedSeconds() const { return lockedSeconds_; }
  QDateTime confirmedUntil() const { return confirmedUntil_; }
  int confirmSecondsLeft() const { return secondsLeft_; }
  QString confirmLeftText() const;
  QString statusState() const { return statusState_; }
  QVariantMap status() const { return status_; }
  QString statusProblem() const { return statusProblem_; }
  QString serverUpdateVersion() const { return serverUpdateVersion_; }
  int pendingDevices() const { return pendingDevices_; }
  int newReports() const { return newReports_ + newErrors_; }  // new problem reports plus unseen error groups

  AdminApi* api() { return &api_; }

  // Re-reads availability, the polling timer and the refresh rules; PlayerController calls it when the connection, the
  // role or the screen changes.
  void refreshAvailability();

  // Generic calls (see the header comment). The returned id is the one of the signal.
  Q_INVOKABLE int request(const QString& method, const QString& path, const QVariant& body = QVariant());
  Q_INVOKABLE int download(const QString& path, const QString& fileUrlOrPath, const QString& method = QStringLiteral("GET"));
  // `headers`: extra request headers as {name: value} (save upload of a person: expected revision and SHA-256 of the file).
  Q_INVOKABLE int upload(const QString& method, const QString& path, const QString& fileUrlOrPath, const QString& contentType,
                         const QVariantMap& headers = QVariantMap());
  Q_INVOKABLE QString sha256OfFile(const QString& fileUrlOrPath) const;  // lower-case hex, empty when unreadable
  Q_INVOKABLE qint64 sizeOfFile(const QString& fileUrlOrPath) const;     // -1 when unreadable

  // "Confirm it's you" without an admin call of its own (Stop sharing runs a program, not a request): answers
  // confirmationAnswered(id, true, "") at once while confirmed, otherwise opens the dialog and answers after it
  // (false, "confirmation_cancelled" when it is cancelled).
  Q_INVOKABLE int requireConfirmation();
  Q_INVOKABLE void confirm(const QString& password);
  Q_INVOKABLE void cancelConfirm();
  Q_INVOKABLE void resumeParked();
  Q_INVOKABLE void endConfirmation();  // DELETE /account/confirm

  Q_INVOKABLE void refreshStatus();
  Q_INVOKABLE void refreshCounts();
  Q_INVOKABLE QString thisDeviceId() const;

 signals:
  void availableChanged();
  void confirmChanged();
  void statusChanged();
  void countsChanged();
  void confirmationAnswered(int id, bool ok, const QString& code);
  void toolsChanged();  // an admin call changed the tools catalog (offer, install, remove, sync): apps read GET /tools again
  // data: parsed JSON object / array (QVariantMap / QVariantList), invalid when the answer has no JSON body.
  void replied(int id, bool ok, int status, const QVariant& data, const QString& code, const QString& message);
  void downloadFinished(int id, bool ok, const QString& filePath, const QString& message);

 private:
  // One call of an admin page. `start` sends it (again): the answer goes to onResult(); `deliver` emits the signal.
  struct Pending {
    int id = 0;
    int epoch = 0;  // value of epoch_ when it was sent
    std::function<void(const Pending&)> start;
    std::function<void(const AdminApi::Result&)> deliver;
  };
  void launch(Pending p);
  // A 403 confirmation_required parks the call (or sends it again when a confirmation happened meanwhile), everything
  // else is handed to the page.
  void onResult(const Pending& p, const AdminApi::Result& r);
  void onConnectionState();
  void updateClock();
  void setConfirmState(const QString& state);
  void tick();
  void replayParked();
  void failParked(const QString& code, const QString& message);
  void applyStatus(const AdminApi::Result& r);
  void updatePolling();

  Deps d_;
  AdminApi api_;
  bool available_ = false;
  int nextId_ = 1;
  int epoch_ = 0;  // counts successful confirmations: a request that started before one is simply sent again
  QList<Pending> parked_;
  bool confirmNeeded_ = false;
  bool autoTried_ = false;  // the stored password was already tried for this round of parked requests
  QString confirmState_;
  QString confirmMessage_;  // server message for "unset"
  QDateTime confirmedUntil_;
  int secondsLeft_ = 0;
  QDateTime lockedUntil_;
  int lockedSeconds_ = 0;
  QTimer clock_;  // 1 s: confirmation countdown and lock countdown
  QTimer statusTimer_;
  QString statusState_ = QStringLiteral("idle");
  QVariantMap status_;
  QString statusProblem_;
  bool statusInFlight_ = false;
  int statusGeneration_ = 0;
  int pendingDevices_ = 0;
  int newReports_ = 0;
  int newErrors_ = 0;
  QString serverUpdateVersion_;
  bool countsInFlight_ = false;
};

}  // namespace uzume::ui
