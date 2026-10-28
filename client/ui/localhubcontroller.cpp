#include "localhubcontroller.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTimer>
#include <QUuid>
#include "hubhttp.h"
#include <QDesktopServices>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QThread>
#include <QUrl>
#include <QtConcurrent>

namespace uzume::ui {

using S = HubConnection::State;

LocalHubController::LocalHubController(const Deps& deps, QObject* parent)
    : QObject(parent), deps_(deps), installer_(deps.installer), adminApi_(deps.conn), accountApi_(deps.conn), info_(localhub::detect()) {
  grant_ = [](const QString& program, const QStringList& args, QString* err) {
    return localhub::runElevatedAndWait(program, args, err);
  };
  sharing_ = grant_;
  connect(&server_, &LocalServerProcess::stateChanged, this, &LocalHubController::onServerStateChanged);
  connect(&server_, &LocalServerProcess::ready, this, &LocalHubController::onChildReady);
  connect(deps_.conn, &HubConnection::stateChanged, this, &LocalHubController::onState);
  connect(deps_.conn, &HubConnection::errorOccurred, this, &LocalHubController::onError);
  connect(&installer_, &update::HubSetupInstaller::changed, this, [this]() {
    if (installer_.state() == update::HubSetupInstaller::State::Failed) {
      installer_.setShareProperties({});
      if (sharePhase_ == QLatin1String("installing")) {
        rollbackShare(QStringLiteral("install_failed"), installer_.error());
      } else {
        setPhase(QStringLiteral("error"), installer_.error());
      }
    } else {
      emit changed();
    }
  });
  connect(&installer_, &update::HubSetupInstaller::quitRequested, this, &LocalHubController::quitRequested);
}

bool LocalHubController::available() const { return info_.present() || localhub::installSupported(); }

QString LocalHubController::installText() const {
  using St = update::HubSetupInstaller::State;
  switch (installer_.state()) {
    case St::Fetching: return tr("Looking up the installer…");
    case St::Downloading: return tr("Downloading the installer…");
    case St::Launching: return tr("Waiting for Windows to ask for administrator permission…");
    default: return {};
  }
}

bool LocalHubController::onThisPc() const {
  return info_.present() && deps_.conn->state() == S::Connected && localhub::isLocalHubAddress(deps_.conn->address(), info_.port);
}

QString LocalHubController::hubWebUrl() const { return info_.present() ? localhub::hubUrl(info_.port) + QStringLiteral("/") : QString(); }

void LocalHubController::setPhase(const QString& p, const QString& err) {
  phase_ = p;
  error_ = err;
  emit changed();
}

QString LocalHubController::friendly(const QString& code, const QString& message) {
  if (code == QLatin1String("rate_limited")) return tr("Too many attempts. Please wait a moment and try again.");
  if (code == QLatin1String("invalid_credentials") || code == QLatin1String("unauthorized")) {
    return tr("User name or password is wrong, or this user is not a server admin.");
  }
  if (code == QLatin1String("admin_exists")) return tr("This server already has an admin. Sign in instead.");
  if (code == QLatin1String("not_local")) return tr("This is only possible with the server on this PC.");
  if (code == QLatin1String("invalid_input")) return tr("Please enter a user name and a password.");
  if (code == QLatin1String("unreachable")) return tr("The Server on this PC does not respond. Check that the Windows service \"Uzume server\" is running.");
  return message.isEmpty() ? tr("The server refused the request (%1).").arg(code) : message;
}

void LocalHubController::start() {
  if (phase_ == QLatin1String("installing") || phase_ == QLatin1String("connecting") || phase_ == QLatin1String("working")) return;
  info_ = localhub::detect();
  error_.clear();
  if (!info_.present()) {
    if (!localhub::installSupported()) {
      setPhase(QStringLiteral("error"), tr("Setting up a server from Uzume is only available on Windows."));
      return;
    }
    setPhase(QStringLiteral("installing"));
    installer_.setShareProperties({});
    installer_.start();
    return;
  }
  active_ = true;
  setPhase(QStringLiteral("connecting"));
  deps_.addHub(localhub::hubUrl(info_.port));
}

void LocalHubController::startAfterInstall() {
  // "Share with others" was finished (or rolled back) by startup(); a server mode is already chosen: nothing to set up.
  if (shareHandled_ || mode() == QLatin1String("child") || mode() == QLatin1String("service")) return;
  retries_ = 20;
  start();
}

void LocalHubController::cancel() {
  if (phase_ == QLatin1String("installing")) installer_.cancel();
  active_ = false;
  childFlow_ = false;
  if (phase_ == QLatin1String("connecting") || phase_ == QLatin1String("needsSetup") || phase_ == QLatin1String("needsSignIn") ||
      phase_ == QLatin1String("working")) {
    deps_.conn->disconnectFromHub();
  }
  setPhase(QStringLiteral("idle"));
}

void LocalHubController::onState(S s) {
  if (childFlow_) {
    onChildState(s);
    emit changed();
    return;
  }
  if (s == S::Connected && onThisPc()) {
    info_ = localhub::detect();
    const bool fresh = active_;
    if (active_) setPhase(QStringLiteral("idle"));
    active_ = false;
    refreshStatus();
    emit changed();
    if (fresh) emit setupCompleted();
    return;
  }
  if (!active_) {
    emit changed();
    return;
  }
  switch (s) {
    case S::NeedsTrustConfirmation:
      // Only the loopback address of the registered Hub: pinned as if the user had accepted it (self-signed cert).
      if (localhub::isLocalHubAddress(deps_.conn->address(), info_.port) && !deps_.conn->observedFingerprint().isEmpty()) {
        deps_.conn->confirmTrust();
      } else {
        active_ = false;
        setPhase(QStringLiteral("error"), tr("The address does not match the server on this PC."));
      }
      break;
    case S::NeedsPairing:
    case S::Denied:
    case S::Expired:
      if (phase_ == QLatin1String("connecting") || phase_ == QLatin1String("working")) loadStatus(true);
      break;
    case S::Unreachable:
      if (deps_.conn->credentialStoreLocked()) {  // a retry would only open the unlock prompt again
        active_ = false;
        setPhase(QStringLiteral("error"), tr("Your password store is locked. Unlock it to sign in."));
        break;
      }
      if (retries_ > 0) {  // the Hub service was just installed and may still be starting
        --retries_;
        QTimer::singleShot(1500, this, [this]() {
          if (active_ && deps_.conn->state() == S::Unreachable) deps_.addHub(localhub::hubUrl(info_.port));
        });
        break;
      }
      active_ = false;
      setPhase(QStringLiteral("error"), friendly(QStringLiteral("unreachable"), {}));
      break;
    case S::CertificateChanged:
    case S::Incompatible:
    case S::UserDisabled:
      active_ = false;
      setPhase(QStringLiteral("error"), tr("Uzume could not connect to the server on this PC. Use Settings › Servers to check it."));
      break;
    case S::Disconnected:
      if (phase_ != QLatin1String("idle") && phase_ != QLatin1String("error")) {
        active_ = false;
        setPhase(QStringLiteral("idle"));
      }
      break;
    default:
      emit changed();
      break;
  }
}

void LocalHubController::onError(const QString& code, const QString& message) {
  if ((!active_ && !childFlow_) || code == QLatin1String("unreachable")) return;  // unreachable is handled by the state change
  if (childFlow_ && !manualForm_ && phase_ == QLatin1String("working")) {
    if (setupForm_ && code == QLatin1String("admin_exists")) {  // an admin appeared meanwhile: pair with the stored password
      pairChild();
    } else if (code == QLatin1String("invalid_credentials")) {
      failChild(tr("The saved password for the server on this PC was not accepted."));
    } else {
      failChild(friendly(code, message));
    }
    return;
  }
  if (phase_ == QLatin1String("working")) {
    setPhase(setupForm_ ? QStringLiteral("needsSetup") : QStringLiteral("needsSignIn"), friendly(code, message));  // form stays open
  } else if (phase_ == QLatin1String("needsSetup") || phase_ == QLatin1String("needsSignIn")) {
    error_ = friendly(code, message);
    emit changed();
  }
}

void LocalHubController::loadStatus(bool forPairingForm) {
  deps_.conn->fetchLocalStatus([this, forPairingForm](bool ok, const QJsonObject& st, const QString& why) {
    if (!ok) {
      if (!forPairingForm) return;
      active_ = false;
      setPhase(QStringLiteral("error"),
               why == QLatin1String("not_local") ? friendly(why, {})
                                                 : tr("This server cannot be set up from Uzume. Update Uzume server or add it as a normal server."));
      return;
    }
    applyStatus(st);
    if (forPairingForm) {
      setupForm_ = !st.value(QStringLiteral("admin_exists")).toBool();
      setPhase(setupForm_ ? QStringLiteral("needsSetup") : QStringLiteral("needsSignIn"));
    }
  });
}

void LocalHubController::applyStatus(const QJsonObject& st) {
  networkSharing_ = st.value(QStringLiteral("network_sharing")).toBool();
  importDir_ = st.value(QStringLiteral("import_dir")).toString();
  emit changed();
}

void LocalHubController::refreshStatus() {
  if (!onThisPc()) return;
  loadStatus(false);
}

void LocalHubController::submit(const QString& username, const QString& password, const QString& passwordAgain) {
  if (phase_ != QLatin1String("needsSetup") && phase_ != QLatin1String("needsSignIn")) return;
  if (username.trimmed().isEmpty() || password.isEmpty()) {
    error_ = friendly(QStringLiteral("invalid_input"), {});
    emit changed();
    return;
  }
  if (phase_ == QLatin1String("needsSetup") && password != passwordAgain) {
    error_ = tr("The two passwords are not the same.");
    emit changed();
    return;
  }
  const bool setup = phase_ == QLatin1String("needsSetup");
  setupForm_ = setup;
  manualForm_ = childFlow_;
  setPhase(QStringLiteral("working"));
  if (setup) {
    deps_.conn->localSetup(username, password);
  } else {
    deps_.conn->localPair(username, password);
  }
}

void LocalHubController::putSettings(const QJsonObject& body, const std::function<void(bool, const QString&)>& done) {
  QNetworkReply* reply = deps_.conn->authorizedSend("PUT", QStringLiteral("/local/settings"),
                                                    QJsonDocument(body).toJson(QJsonDocument::Compact), {}, "application/json");
  if (reply == nullptr) {
    done(false, tr("The Server on this PC is not connected."));
    return;
  }
  connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
    const HttpResult r = HubHttp::resultOf(reply);
    reply->deleteLater();
    if (r.ok()) {
      applyStatus(r.json());
      done(true, {});
    } else if (r.networkError) {
      // A change of network sharing restarts the Hub's server; the answer may be lost. The status tells the result.
      done(false, QString());
    } else {
      done(false, r.apiErrorMessage.isEmpty() ? friendly(r.apiErrorCode, {}) : r.apiErrorMessage);
    }
  });
}

void LocalHubController::setNetworkSharing(bool on) {
  if (!onThisPc() || settingsBusy_) return;
  settingsBusy_ = true;
  settingsError_.clear();
  settingsNotice_.clear();
  emit changed();
  putSettings({{QStringLiteral("network_sharing"), on}}, [this, on](bool ok, const QString& why) {
    settingsBusy_ = false;
    if (!ok && !why.isEmpty()) {
      settingsError_ = why;
    } else if (!ok) {
      networkSharing_ = on;  // answer lost while the server restarted; refreshStatus() corrects it if needed
      QTimer::singleShot(2000, this, &LocalHubController::refreshStatus);
    }
    emit changed();
  });
}

void LocalHubController::chooseFolder(const QString& pathOrUrl) {
  if (!onThisPc() || settingsBusy_) return;
  QString path = pathOrUrl.trimmed();
  if (path.startsWith(QLatin1String("file:"))) path = QUrl(path).toLocalFile();
  path = QDir::toNativeSeparators(path);
  if (path.isEmpty()) return;
  settingsBusy_ = true;
  settingsError_.clear();
  settingsNotice_.clear();
  emit changed();
  const auto save = [this, path]() {
    putSettings({{QStringLiteral("import_dir"), path}}, [this](bool ok, const QString& why) {
      settingsBusy_ = false;
      if (ok) {
        settingsNotice_ = tr("Folder saved. Open the server web page and choose \"Rescan folder\" to import the games.");
      } else {
        settingsError_ = why.isEmpty() ? tr("The server did not answer.") : why;
      }
      emit changed();
    });
  };
  if (!localhub::installSupported() || info_.installDir.isEmpty()) {
    save();  // no Windows service to grant access to (development): the Hub checks the folder itself
    return;
  }
  const localhub::Command cmd = localhub::grantFolderCommand(info_.installDir, path);
  const auto runner = grant_;
  auto* watcher = new QFutureWatcher<QPair<int, QString>>(this);
  connect(watcher, &QFutureWatcher<QPair<int, QString>>::finished, this, [this, watcher, save]() {
    const QPair<int, QString> res = watcher->result();
    watcher->deleteLater();
    if (res.first != 0) {
      settingsBusy_ = false;
      settingsError_ = res.first < 0 ? res.second
                                     : tr("Windows could not give the server access to this folder (code %1).").arg(res.first);
      emit changed();
      return;
    }
    save();
  });
  watcher->setFuture(QtConcurrent::run([runner, cmd]() {
    QString err;
    const int code = runner(cmd.program, cmd.args, &err);
    return QPair<int, QString>(code, err);
  }));
}

void LocalHubController::openHubWebUi() {
  if (info_.present()) QDesktopServices::openUrl(QUrl(hubWebUrl()));
}


// ================================================================ 0.18 package 10: Play on this PC / Share with others

namespace {
bool portIsFree(int port) {
  QTcpServer probe;
  return probe.listen(QHostAddress::Any, static_cast<quint16>(port));
}
QString userSpec() {
  QString user = qEnvironmentVariable("USERNAME");
  if (user.isEmpty()) user = qEnvironmentVariable("USER");
  if (user.isEmpty()) user = QDir::home().dirName();
  const QString domain = qEnvironmentVariable("USERDOMAIN");
  return domain.isEmpty() ? user : domain + QLatin1Char('\\') + user;
}
}  // namespace

QString LocalHubController::mode() const { return deps_.settings != nullptr ? deps_.settings->localServerMode() : QString(); }

bool LocalHubController::onThisPcOnly() const {
  if (deps_.settings == nullptr || mode() != QLatin1String("child")) return false;
  const QString id = deps_.settings->localServerHubId();
  return !id.isEmpty() && deps_.conn->state() == S::Connected && deps_.conn->hubId() == id;
}

QString LocalHubController::childState() const {
  switch (server_.state()) {
    case LocalServerProcess::State::Starting: return QStringLiteral("starting");
    case LocalServerProcess::State::Running: return QStringLiteral("running");
    case LocalServerProcess::State::Failed: return QStringLiteral("failed");
    case LocalServerProcess::State::Stopped: break;
  }
  return QStringLiteral("stopped");
}

bool LocalHubController::shareSupported() const { return localhub::installSupported() || shareSupportedTest_; }

bool LocalHubController::shareAgainAvailable() const {
  if (deps_.settings == nullptr || mode() != QLatin1String("child")) return false;
  const QString svc = localhub::serviceDataDir();
  if (svc.isEmpty() || !localhub::detect().present()) return false;
  const auto norm = [](const QString& p) { return QDir::cleanPath(QDir::fromNativeSeparators(p)).toLower(); };
  return norm(effectiveDataDir()) == norm(svc);
}

QString LocalHubController::effectiveDataDir() const {
  const QString d = deps_.settings != nullptr ? deps_.settings->localServerDataDir() : QString();
  if (!d.isEmpty()) return d;
  return LocalServerProcess::defaultDataDir(deps_.profiles != nullptr ? deps_.profiles->baseDir() : QString());
}

// ---- The hidden admin: random password, only in the CredentialStore, keyed by the data folder of the server.

QString LocalHubController::hiddenTarget() const {
  const QString dir = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(effectiveDataDir()).absoluteFilePath())).toLower();
  const QByteArray h = QCryptographicHash::hash(dir.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
  return QStringLiteral("uzume:local-server-admin:") + QString::fromLatin1(h);
}

std::optional<LocalHubController::HiddenAdmin> LocalHubController::hiddenAdmin(bool* readFailed) const {
  if (readFailed != nullptr) *readFailed = false;
  if (deps_.credentials == nullptr) return std::nullopt;
  const CredentialRead raw = deps_.credentials->readChecked(hiddenTarget());
  if (raw.status == CredentialRead::Status::Error && readFailed != nullptr) *readFailed = true;
  if (raw.status != CredentialRead::Status::Ok) return std::nullopt;
  const QJsonObject o = QJsonDocument::fromJson(raw.secret.toUtf8()).object();
  HiddenAdmin h{o.value(QStringLiteral("u")).toString(), o.value(QStringLiteral("p")).toString()};
  if (h.username.isEmpty() || h.password.isEmpty()) return std::nullopt;
  return h;
}

bool LocalHubController::storeHiddenAdmin(const HiddenAdmin& a) {
  if (deps_.credentials == nullptr) return false;
  return deps_.credentials->write(hiddenTarget(), QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("u"), a.username},
                                                                                       {QStringLiteral("p"), a.password}})
                                                                 .toJson(QJsonDocument::Compact)));
}

void LocalHubController::clearHiddenAdmin() {
  if (deps_.credentials != nullptr) deps_.credentials->remove(hiddenTarget());
}

QString LocalHubController::sanitizeUsername(const QString& raw) {
  // Server rule: ^[A-Za-z0-9._-]{1,64}$ (ASCII only).
  QString name;
  for (const QChar c : raw) {
    const ushort u = c.unicode();
    const bool ok = (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '.' || u == '_' || u == '-';
    if (ok) name += c;
    else if (c.isSpace()) name += QLatin1Char('_');
  }
  name.truncate(64);
  return name.isEmpty() ? QStringLiteral("player") : name;
}

QString LocalHubController::osUserName() {
  QString raw = qEnvironmentVariable("USERNAME");
  if (raw.isEmpty()) raw = qEnvironmentVariable("USER");
  return sanitizeUsername(raw);
}

QString LocalHubController::randomPassword() {
  static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  QString pw;
  for (int i = 0; i < 32; ++i) {
    pw += QLatin1Char(kAlphabet[QRandomGenerator::system()->bounded(static_cast<quint32>(sizeof(kAlphabet) - 1))]);
  }
  return pw;
}

QString LocalHubController::autoConfirmPassword() const {
  if (!onThisPcOnly()) return {};
  const auto h = hiddenAdmin();
  return h ? h->password : QString();
}

// ---- Play on this PC

bool LocalHubController::startServerProcess() {
  LocalServerProcess::Config c;
  c.dataDir = effectiveDataDir();
  c.port = deps_.settings != nullptr ? deps_.settings->localServerPort() : 0;
  if (serverConfigPatch_) serverConfigPatch_(c);
  server_.setConfig(c);
  if (server_.state() == LocalServerProcess::State::Running) {  // already up: go on as if it had just reported ready
    QTimer::singleShot(0, this, [this]() {
      if (server_.state() == LocalServerProcess::State::Running) onChildReady(server_.port());
    });
    return true;
  }
  return server_.start();
}

void LocalHubController::startChild() {
  const bool busyShare = sharePhase_ == QLatin1String("preparing") || sharePhase_ == QLatin1String("installing") ||
                         sharePhase_ == QLatin1String("stopping") || sharePhase_ == QLatin1String("resuming");
  if (busyShare || phase_ == QLatin1String("installing") || phase_ == QLatin1String("starting") || phase_ == QLatin1String("connecting") ||
      phase_ == QLatin1String("working")) {
    return;
  }
  if (deps_.settings == nullptr || deps_.profiles == nullptr || deps_.credentials == nullptr || !deps_.connectProfile) {
    setPhase(QStringLiteral("error"), tr("The server on this PC cannot be started here."));
    return;
  }
  active_ = false;
  childFlow_ = true;
  manualForm_ = false;
  childRetries_ = 5;
  setPhase(QStringLiteral("starting"));
  if (!startServerProcess()) failChild(server_.error());
}

bool LocalHubController::resumeAtStartup(bool connectToServer) {
  if (deps_.settings == nullptr) return false;
  if (resolvePendingShare()) return true;
  if (mode() != QLatin1String("child")) return false;
  active_ = false;
  childFlow_ = connectToServer;
  manualForm_ = false;
  childRetries_ = 5;
  if (connectToServer) setPhase(QStringLiteral("starting"));
  if (!startServerProcess() && connectToServer) failChild(server_.error());
  emit changed();
  return true;
}

void LocalHubController::restartChild() {
  if (mode() != QLatin1String("child")) return;
  server_.stop();
  active_ = false;
  childFlow_ = true;
  manualForm_ = false;
  childRetries_ = 5;
  setPhase(QStringLiteral("starting"));
  if (!startServerProcess()) failChild(server_.error());
}

void LocalHubController::stopChild() {
  childFlow_ = false;
  server_.stop();
}

void LocalHubController::onServerStateChanged() {
  if (server_.state() == LocalServerProcess::State::Failed && childFlow_ && (phase_ == QLatin1String("starting"))) {
    failChild(server_.error());
    return;
  }
  emit changed();
}

void LocalHubController::onChildReady(int port) {
  childPort_ = port;
  if (deps_.settings == nullptr || deps_.profiles == nullptr) return;
  deps_.settings->setLocalServerPort(port);
  // The profile's address follows the port of the server (a taken port is replaced at start).
  const QString hubId = deps_.settings->localServerHubId();
  if (!hubId.isEmpty()) {
    if (const auto p = deps_.profiles->profile(hubId)) {
      QString host;
      int pp = 0;
      ProfileStore::splitAddress(p->address, &host, &pp);
      if (pp != port) deps_.profiles->updateHubAddress(hubId, host.isEmpty() ? QStringLiteral("127.0.0.1") : host, port);
    }
  }
  if (!childFlow_) {
    // The server came back by itself (restart after a crash) or at app start without connecting: reconnect when the
    // app had lost it.
    const auto prof = deps_.conn->profile();
    if (mode() == QLatin1String("child") && !hubId.isEmpty() && prof && prof->hubId == hubId && deps_.conn->state() != S::Connected &&
        deps_.conn->state() != S::Disconnected && !deps_.conn->credentialStoreLocked() && deps_.connectProfile) {
      deps_.connectProfile(hubId);
    }
    emit changed();
    return;
  }
  connectToChild();
}

void LocalHubController::connectToChild() {
  setPhase(QStringLiteral("connecting"));
  std::optional<HubProfile> prof;
  const QString hubId = deps_.settings->localServerHubId();
  if (!hubId.isEmpty()) prof = deps_.profiles->profile(hubId);
  if (!prof) prof = deps_.profiles->profileByAddress(localhub::hubUrl(childPort_));
  if (prof) {
    QString host;
    int pp = 0;
    ProfileStore::splitAddress(prof->address, &host, &pp);
    if (pp != childPort_ && !deps_.profiles->updateHubAddress(prof->hubId, host.isEmpty() ? QStringLiteral("127.0.0.1") : host, childPort_)) {
      prof.reset();
    }
  }
  if (prof) {
    deps_.connectProfile(prof->hubId);
  } else {
    deps_.addHub(localhub::hubUrl(childPort_));
  }
}

void LocalHubController::onChildState(S s) {
  switch (s) {
    case S::Connected:
      finishChild();
      break;
    case S::NeedsTrustConfirmation:
      // Only the loopback address and port of our own child server: pinned as if the person had accepted it.
      if (localhub::isLocalHubAddress(deps_.conn->address(), childPort_) && !deps_.conn->observedFingerprint().isEmpty()) {
        deps_.conn->confirmTrust();
      } else {
        failChild(tr("The address does not match the server on this PC."));
      }
      break;
    case S::NeedsPairing:
    case S::Denied:
    case S::Expired:
      if (phase_ == QLatin1String("connecting")) pairChild();
      break;
    case S::Unreachable:
      if (deps_.conn->credentialStoreLocked()) {  // a retry would only open the unlock prompt again
        failChild(tr("Your password store is locked. Unlock it to sign in."));
        break;
      }
      if (childRetries_ > 0) {
        --childRetries_;
        QTimer::singleShot(1000, this, [this]() {
          if (childFlow_ && deps_.conn->state() == S::Unreachable) connectToChild();
        });
      } else {
        failChild(tr("The server on this PC does not respond."));
      }
      break;
    case S::CertificateChanged:
      failChild(tr("The certificate of the server on this PC changed. Use Settings › Servers to check it."));
      break;
    case S::Incompatible:
    case S::UserDisabled:
      failChild(tr("Uzume could not connect to the server on this PC."));
      break;
    default:
      break;
  }
}

void LocalHubController::pairChild() {
  setPhase(QStringLiteral("working"));
  manualForm_ = false;
  deps_.conn->fetchLocalStatus([this](bool ok, const QJsonObject& st, const QString& why) {
    if (!childFlow_) return;
    if (!ok) {
      failChild(why == QLatin1String("not_local") ? friendly(why, {})
                                                   : tr("This server cannot be set up from Uzume. Update Uzume or reinstall it."));
      return;
    }
    bool readFailed = false;
    auto hidden = hiddenAdmin(&readFailed);
    if (readFailed) {
      failChild(tr("The credential store did not answer. Unlock it and try again."));
      return;
    }
    if (!st.value(QStringLiteral("admin_exists")).toBool()) {
      if (!hidden) {
        QString name = osUserName();
        if (deps_.settings != nullptr && !deps_.settings->displayName().isEmpty()) name = sanitizeUsername(deps_.settings->displayName());
        hidden = HiddenAdmin{name, randomPassword()};
        // before the call: a lost answer must not lose the password; a password that was not stored is never used
        if (!storeHiddenAdmin(*hidden)) {
          failChild(tr("The admin password could not be stored in the credential store."));
          return;
        }
      }
      setupForm_ = true;
      deps_.conn->localSetup(hidden->username, hidden->password);
    } else if (hidden) {
      setupForm_ = false;
      deps_.conn->localPair(hidden->username, hidden->password);
    } else {
      // The person has their own password (after "Stop sharing" with the credential lost): the sign-in form of the
      // service flow asks for it.
      setupForm_ = false;
      setPhase(QStringLiteral("needsSignIn"));
    }
  });
}

void LocalHubController::finishChild() {
  childFlow_ = false;
  manualForm_ = false;
  if (deps_.settings != nullptr) {
    deps_.settings->setLocalServerMode(QStringLiteral("child"));
    deps_.settings->setLocalServerHubId(deps_.conn->hubId());
    deps_.settings->setLocalServerPort(childPort_);
  }
  if (sharePhase_ == QLatin1String("stopping")) sharingStopped_ = true;
  if (sharePhase_ == QLatin1String("resuming") || sharePhase_ == QLatin1String("stopping")) {
    sharePhase_ = QStringLiteral("idle");
  }
  setPhase(QStringLiteral("idle"));
  emit childConnected();
}

void LocalHubController::failChild(const QString& text) {
  childFlow_ = false;
  manualForm_ = false;
  if (sharePhase_ == QLatin1String("resuming")) {
    failShare(QStringLiteral("start_failed"), text);
  }
  setPhase(QStringLiteral("error"), text);
}

// ---- Share with others

void LocalHubController::beforeStop() {
  if (deps_.beforeServerStop) deps_.beforeServerStop();
}

QString LocalHubController::shareCopy(const QString& code, const QString& detail, int port) {
  if (code == QLatin1String("admin_rights_refused")) return tr("Windows did not give Uzume administrator rights. Nothing was changed.");
  if (code == QLatin1String("port_in_use")) return tr("Port %1 is already used by another program.").arg(port);
  if (code == QLatin1String("install_failed")) {
    return tr("The server could not be installed and everything was rolled back. Your library and saves are untouched.");
  }
  if (code == QLatin1String("password_too_short")) return tr("The password needs at least 12 characters.");
  if (code == QLatin1String("name_taken")) return tr("This name is already taken on the server.");
  if (code == QLatin1String("invalid_input")) return tr("Please enter a server name and your name.");
  if (code == QLatin1String("invalid_port")) return tr("Please enter a port between 1 and 65535.");
  if (code == QLatin1String("not_connected")) return tr("Uzume is not connected to the server on this PC.");
  if (code == QLatin1String("not_supported")) return tr("Sharing is only available on Windows.");
  if (code == QLatin1String("no_local_admin")) return tr("Uzume cannot change this server by itself because the admin password is not saved on this PC.");
  if (code == QLatin1String("stop_failed")) {
    return detail.isEmpty() ? tr("The server could not be stopped.") : tr("The server could not be stopped: %1").arg(detail);
  }
  if (code == QLatin1String("start_failed")) {
    return detail.isEmpty() ? tr("The server could not be started.") : tr("The server could not be started: %1").arg(detail);
  }
  if (code == QLatin1String("busy")) return tr("Another step is still running.");
  return detail.isEmpty() ? tr("The server on this PC did not accept the change.") : detail;
}

void LocalHubController::failShare(const QString& code, const QString& text, int port) {
  sharePhase_ = QStringLiteral("failed");
  shareErrorCode_ = code;
  if (port > 0) sharePort_ = port;
  shareError_ = shareCopy(code, text, port > 0 ? port : sharePort_);
  emit changed();
}

void LocalHubController::setShareProgress(const QString& phase) {
  sharePhase_ = phase;
  shareErrorCode_.clear();
  shareError_.clear();
  emit changed();
}

void LocalHubController::openShareDialog(const QString& which) {
  if (which != QLatin1String("intro") && which != QLatin1String("setup") && which != QLatin1String("stop")) return;
  if (sharePhase_ == QLatin1String("failed")) dismissShareError();
  shareDialog_ = which;
  emit changed();
}

void LocalHubController::closeShareDialog() {
  shareDialog_.clear();
  emit changed();
}

void LocalHubController::retryShare(int port) {
  if (sharePhase_ == QLatin1String("preparing") || sharePhase_ == QLatin1String("installing") || sharePhase_ == QLatin1String("stopping") ||
      sharePhase_ == QLatin1String("resuming")) {
    return;
  }
  shareErrorCode_.clear();
  shareError_.clear();
  if (!shareSupported()) return failShare(QStringLiteral("not_supported"));
  if (!canRetryShare() || deps_.settings == nullptr) return failShare(QStringLiteral("not_connected"));
  const int p = port > 0 ? port : retryPort_;
  if (p > 65535) return failShare(QStringLiteral("invalid_port"));
  if (p != childPort_ && !portIsFree(p)) {
    shareSuggestedPort_ = p < 65535 ? p + 1 : p - 1;
    return failShare(QStringLiteral("port_in_use"), {}, p);
  }
  shareSuggestedPort_ = 0;
  shareDialog_.clear();
  setShareProgress(QStringLiteral("installing"));
  retryPort_ = p;
  deps_.settings->setPendingShare({p, retryJoin_, effectiveDataDir()});
  launchShareInstaller(p);
}

void LocalHubController::acknowledgeStopped() {
  sharingStopped_ = false;
  emit changed();
}

void LocalHubController::acknowledgeShareDone() {
  shareDone_ = false;
  if (sharePhase_ == QLatin1String("done")) sharePhase_ = QStringLiteral("idle");
  emit changed();
}

void LocalHubController::dismissShareError() {
  if (sharePhase_ != QLatin1String("failed")) return;
  sharePhase_ = QStringLiteral("idle");
  shareErrorCode_.clear();
  shareError_.clear();
  emit changed();
}

void LocalHubController::adminRequest(const QString& method, const QString& path, const QVariantMap& body,
                                      const std::function<void(const AdminApi::Result&)>& done, bool confirmed) {
  QPointer<LocalHubController> self(this);
  adminApi_.request(method, path, body, [self, method, path, body, done, confirmed](const AdminApi::Result& r) {
    if (!self) return;
    if (!confirmed && r.status == 403 && r.code == QLatin1String("confirmation_required")) {
      const auto h = self->hiddenAdmin();
      if (!h) {
        done(r);
        return;
      }
      // The person has no password to type: the hidden one confirms (same 5 minutes as the dialog).
      self->adminApi_.request(QStringLiteral("POST"), QStringLiteral("/account/confirm"),
                              QVariantMap{{QStringLiteral("password"), h->password}, {QStringLiteral("duration_seconds"), 300}},
                              [self, method, path, body, done](const AdminApi::Result& c) {
                                if (!self) return;
                                if (!c.ok) {
                                  done(c);
                                  return;
                                }
                                self->adminRequest(method, path, body, done, true);
                              });
      return;
    }
    done(r);
  });
}

void LocalHubController::beginShare(const QString& serverName, const QString& yourName, const QString& password, const QString& join,
                                    int port) {
  if (sharePhase_ == QLatin1String("preparing") || sharePhase_ == QLatin1String("installing") || sharePhase_ == QLatin1String("stopping") ||
      sharePhase_ == QLatin1String("resuming")) {
    return;
  }
  shareErrorCode_.clear();
  shareError_.clear();
  shareDone_ = false;
  shareSuggestedPort_ = 0;
  retryPort_ = 0;
  sharePort_ = port;
  if (!shareSupported()) return failShare(QStringLiteral("not_supported"));
  if (!onThisPcOnly()) return failShare(QStringLiteral("not_connected"));
  if (serverName.trimmed().isEmpty() || yourName.trimmed().isEmpty()) return failShare(QStringLiteral("invalid_input"));
  if (password.size() < 12) return failShare(QStringLiteral("password_too_short"));
  if (port < 1 || port > 65535) return failShare(QStringLiteral("invalid_port"));
  if (!hiddenAdmin()) return failShare(QStringLiteral("no_local_admin"));
  if (port != childPort_ && !portIsFree(port)) {
    shareSuggestedPort_ = port < 65535 ? port + 1 : port - 1;
    sharePhase_ = QStringLiteral("failed");
    shareErrorCode_ = QStringLiteral("port_in_use");
    shareError_ = shareCopy(shareErrorCode_, {}, port);
    emit changed();
    return;
  }
  shareDialog_.clear();
  setShareProgress(QStringLiteral("preparing"));
  shareSteps(serverName.trimmed(), yourName.trimmed(), password, join, port);
}

void LocalHubController::shareSteps(const QString& serverName, const QString& yourName, const QString& password, const QString& join, int port) {
  QPointer<LocalHubController> self(this);
  const HiddenAdmin hidden = *hiddenAdmin();
  const auto prepareFailed = [self](const QString& code, const QString& message) {
    if (!self) return;
    if (code == QLatin1String("not_connected") || code == QLatin1String("network")) {
      self->failShare(QStringLiteral("not_connected"));
    } else {
      self->failShare(QStringLiteral("prepare_failed"), message);
    }
  };
  // 1. server name
  adminRequest(QStringLiteral("PATCH"), QStringLiteral("/admin/settings"), QVariantMap{{QStringLiteral("name"), serverName}},
               [=](const AdminApi::Result& r) {
                 if (!self) return;
                 if (!r.ok) {
                   prepareFailed(r.code, r.message);
                   return;
                 }
                 // 2. the person's name
                 self->accountApi_.updateDisplayName(yourName, [=](const AccountApiResult& n) {
                   if (!self) return;
                   if (!n.ok) {
                     if (n.errorCode == QLatin1String("display_name_taken")) {
                       self->failShare(QStringLiteral("name_taken"));
                     } else {
                       prepareFailed(n.errorCode, n.errorMessage);
                     }
                     return;
                   }
                   // 3. the password of the person replaces the hidden one (last: until here the hidden one still works)
                   self->accountApi_.setPassword(hidden.password, password, [=](const AccountApiResult& pw) {
                     if (!self) return;
                     if (!pw.ok) {
                       prepareFailed(pw.errorCode, pw.errorMessage);
                       return;
                     }
                     self->clearHiddenAdmin();
                     if (self->deps_.settings != nullptr) self->deps_.settings->setDisplayName(yourName);
                     if (self->deps_.profiles != nullptr && n.account.valid()) {
                       const QString hub = self->deps_.settings != nullptr ? self->deps_.settings->localServerHubId() : QString();
                       if (auto a = self->deps_.profiles->account(hub, n.account.userId)) {
                         a->displayName = n.account.displayName;
                         self->deps_.profiles->upsertAccount(hub, *a);
                       }
                     }
                     if (self->deps_.settings != nullptr) {
                       self->deps_.settings->setPendingShare({port, join, self->effectiveDataDir()});
                     }
                     self->launchShareInstaller(port);
                   });
                 });
               });
}

void LocalHubController::launchShareInstaller(int port) {
  beforeStop();  // uploads the saves while the server still runs
  const QString dataDir = effectiveDataDir();
  if (deps_.settings != nullptr) {
    const PlayerSettings::PendingShare ps = deps_.settings->pendingShare();
    retryPort_ = ps.port;
    retryJoin_ = ps.join;
  }
  sharePort_ = port;
  childFlow_ = false;
  server_.stop();
  deps_.conn->disconnectFromHub();
  const QStringList props{QStringLiteral("HUB_PORT=%1").arg(port), QStringLiteral("NETWORK_SHARING=1"),
                          QStringLiteral("IMPORT_SERVER_DATA=") + QDir::toNativeSeparators(QDir::cleanPath(dataDir))};
  setShareProgress(QStringLiteral("installing"));
  if (shareStarter_) {
    if (!shareStarter_(props)) rollbackShare(QStringLiteral("install_failed"), {});
    return;
  }
  installer_.setShareProperties(props);
  installer_.start();  // quitRequested follows when the launcher runs: the app exits, the next start finishes it
}

void LocalHubController::rollbackShare(const QString& code, const QString& text) {
  if (deps_.settings != nullptr) deps_.settings->clearPendingShare();
  failShare(code, text);
  // Back to "Play on this PC" on the unchanged data.
  active_ = false;
  childFlow_ = true;
  childRetries_ = 5;
  setPhase(QStringLiteral("starting"));
  if (!startServerProcess()) failChild(server_.error());
}

bool LocalHubController::resolvePendingShare() {
  if (deps_.settings == nullptr) return false;
  const PlayerSettings::PendingShare ps = deps_.settings->pendingShare();
  if (!ps.valid()) return false;
  shareHandled_ = true;
  deps_.settings->clearPendingShare();
  info_ = localhub::detect();
  const bool moved = QFile::exists(QDir(ps.dataDir).filePath(QStringLiteral("moved.json")));
  if (info_.present() && moved) {
    goToService(info_.port);
    shareJoin_ = ps.join;
    shareDone_ = true;
    sharePhase_ = QStringLiteral("done");
    emit changed();
    return true;
  }
  // The installer did not finish (UAC declined, port taken, rolled back): the data is untouched, back to the child.
  QString code = QStringLiteral("install_failed");
  if (!info_.present()) code = portIsFree(ps.port) ? QStringLiteral("admin_rights_refused") : QStringLiteral("port_in_use");
  if (deps_.settings->localServerMode().isEmpty()) deps_.settings->setLocalServerMode(QStringLiteral("child"));
  retryPort_ = ps.port;
  retryJoin_ = ps.join;
  failShare(code, {}, ps.port);
  if (code == QLatin1String("port_in_use")) shareSuggestedPort_ = ps.port < 65535 ? ps.port + 1 : ps.port - 1;
  active_ = false;
  childFlow_ = true;
  childRetries_ = 5;
  setPhase(QStringLiteral("starting"));
  if (!startServerProcess()) failChild(server_.error());
  return true;
}

void LocalHubController::goToService(int port) {
  server_.stop();
  childFlow_ = false;
  deps_.settings->setLocalServerMode(QStringLiteral("service"));
  deps_.settings->setLocalServerPort(port);
  deps_.settings->setLocalServerDataDir(QString());
  const QString hubId = deps_.settings->localServerHubId();
  if (!hubId.isEmpty() && deps_.profiles != nullptr && deps_.profiles->profile(hubId) && deps_.connectProfile) {
    // Same server ID and certificate, new port at most: the pairing stays valid.
    const auto p = deps_.profiles->profile(hubId);
    QString host;
    int pp = 0;
    ProfileStore::splitAddress(p->address, &host, &pp);
    if (pp != port) deps_.profiles->updateHubAddress(hubId, host.isEmpty() ? QStringLiteral("127.0.0.1") : host, port);
    deps_.connectProfile(hubId);
  } else {
    start();  // no saved profile: the sign-in flow of the service
  }
  emit changed();
}

void LocalHubController::goToChild(const QString& dataDir, int port) {
  deps_.settings->setLocalServerMode(QStringLiteral("child"));
  deps_.settings->setLocalServerDataDir(dataDir);
  if (port > 0) deps_.settings->setLocalServerPort(port);
  active_ = false;
  childFlow_ = true;
  childRetries_ = 5;
  setPhase(QStringLiteral("starting"));
  if (!startServerProcess()) failChild(server_.error());
}

// ---- Stop sharing / Share again

void LocalHubController::runSharing(const QStringList& args, const QString& what, const std::function<void(const QJsonObject&)>& done) {
  info_ = localhub::detect();
  if (info_.installDir.isEmpty()) {
    failShare(what == QLatin1String("stop") ? QStringLiteral("stop_failed") : QStringLiteral("start_failed"),
              tr("The server program was not found."));
    return;
  }
  const QString exe = localhub::hubExe(info_.installDir);
  const QString resultFile = QDir::temp().filePath(QStringLiteral("uzume-sharing-%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  QStringList full = args;
  full << QStringLiteral("--result") << resultFile;
  const auto runner = sharing_;
  auto* watcher = new QFutureWatcher<QPair<int, QString>>(this);
  connect(watcher, &QFutureWatcher<QPair<int, QString>>::finished, this, [this, watcher, resultFile, what, done]() {
    const QPair<int, QString> res = watcher->result();
    watcher->deleteLater();
    QJsonObject r;
    if (QFile f(resultFile); f.open(QIODevice::ReadOnly)) {
      r = QJsonDocument::fromJson(f.readAll()).object();
      f.close();
    }
    QFile::remove(resultFile);
    const QString failCode = what == QLatin1String("stop") ? QStringLiteral("stop_failed") : QStringLiteral("start_failed");
    if (res.first < 0) {
      failShare(QStringLiteral("admin_rights_refused"));
    } else if (!r.value(QStringLiteral("ok")).toBool()) {
      failShare(failCode, r.value(QStringLiteral("error")).toString());
    } else {
      done(r);
      return;
    }
    if (what == QLatin1String("start")) {
      // Share again failed: the child was stopped for it, bring it back.
      active_ = false;
      childFlow_ = true;
      childRetries_ = 5;
      setPhase(QStringLiteral("starting"));
      if (!startServerProcess()) failChild(server_.error());
    }
  });
  watcher->setFuture(QtConcurrent::run([runner, exe, full]() {
    QString err;
    const int code = runner(exe, full, &err);
    return QPair<int, QString>(code, err);
  }));
}

void LocalHubController::stopSharing() {
  if (sharePhase_ == QLatin1String("preparing") || sharePhase_ == QLatin1String("installing") || sharePhase_ == QLatin1String("stopping") ||
      sharePhase_ == QLatin1String("resuming")) {
    return;
  }
  shareErrorCode_.clear();
  shareError_.clear();
  if (mode() != QLatin1String("service")) return failShare(QStringLiteral("not_connected"));
  shareDialog_.clear();
  setShareProgress(QStringLiteral("stopping"));
  beforeStop();
  runSharing({QStringLiteral("sharing"), QStringLiteral("stop"), QStringLiteral("--grant-user"), userSpec()}, QStringLiteral("stop"),
             [this](const QJsonObject& r) {
               goToChild(r.value(QStringLiteral("data_dir")).toString(), r.value(QStringLiteral("port")).toInt(0));
             });
}

void LocalHubController::shareAgain() {
  if (sharePhase_ == QLatin1String("preparing") || sharePhase_ == QLatin1String("installing") || sharePhase_ == QLatin1String("stopping") ||
      sharePhase_ == QLatin1String("resuming")) {
    return;
  }
  shareErrorCode_.clear();
  shareError_.clear();
  if (!shareAgainAvailable()) return failShare(QStringLiteral("not_connected"));
  sharingStopped_ = false;
  shareDialog_.clear();
  setShareProgress(QStringLiteral("resuming"));
  beforeStop();
  childFlow_ = false;
  server_.stop();
  deps_.conn->disconnectFromHub();
  runSharing({QStringLiteral("sharing"), QStringLiteral("start")}, QStringLiteral("start"), [this](const QJsonObject& r) {
    const int port = r.value(QStringLiteral("port")).toInt(0);
    goToService(port > 0 ? port : (deps_.settings != nullptr ? deps_.settings->localServerPort() : 0));
    if (sharePhase_ == QLatin1String("resuming")) sharePhase_ = QStringLiteral("idle");
    emit changed();
  });
}

}  // namespace uzume::ui
