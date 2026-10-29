#include "admincontroller.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QTimer>
#include <QVariantList>
#include <algorithm>
#include <memory>

namespace uzume::ui {

namespace {
constexpr int kConfirmSeconds = 300;
constexpr int kLockFallbackSeconds = 300;
constexpr int kStatusPollMs = 60 * 1000;

QString mmss(int seconds) {
  seconds = std::max(0, seconds);
  return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
}  // namespace

AdminController::AdminController(const Deps& deps, QObject* parent) : QObject(parent), d_(deps), api_(deps.conn, this) {
  clock_.setInterval(1000);
  connect(&clock_, &QTimer::timeout, this, [this]() { tick(); });
  statusTimer_.setInterval(kStatusPollMs);
  connect(&statusTimer_, &QTimer::timeout, this, [this]() { refreshStatus(); });
  if (d_.conn) {
    connect(d_.conn, &HubConnection::stateChanged, this, [this]() { onConnectionState(); });
  }
  refreshAvailability();
}

AdminController::~AdminController() = default;

// ---------------------------------------------------------------- availability

void AdminController::refreshAvailability() {
  const bool now = d_.conn && d_.conn->state() == HubConnection::State::Connected && d_.isAdmin && d_.isAdmin() &&
                   d_.conn->hubHasFeature(QStringLiteral("admin_v1"));
  const bool changed = now != available_;
  available_ = now;
  if (changed) {
    if (!available_) {
      statusState_ = QStringLiteral("idle");
      status_.clear();
      statusProblem_.clear();
      emit statusChanged();
      if (pendingDevices_ != 0 || newReports_ != 0 || newErrors_ != 0 || pluginUpdates_ != 0 || !serverUpdateVersion_.isEmpty()) {
        pendingDevices_ = newReports_ = newErrors_ = pluginUpdates_ = 0;
        serverUpdateVersion_.clear();
        emit countsChanged();
      }
    }
    emit availableChanged();
  }
  updatePolling();
  if (changed && available_) refreshStatus();  // the badge on Status in AppNav needs the first reading
}

void AdminController::onConnectionState() {
  if (!d_.conn || d_.conn->state() != HubConnection::State::Connected) {
    failParked(QStringLiteral("not_connected"), tr("Not connected to a server."));
    confirmNeeded_ = false;
    confirmState_.clear();
    confirmMessage_.clear();
    confirmedUntil_ = QDateTime();
    lockedUntil_ = QDateTime();
    secondsLeft_ = lockedSeconds_ = 0;
    ++statusGeneration_;  // an answer of the old server is dropped
    statusInFlight_ = false;
    countsInFlight_ = false;
    updateClock();
    emit confirmChanged();
  }
  refreshAvailability();
}

void AdminController::updatePolling() {
  const bool want = available_ && !(d_.inGame && d_.inGame());
  if (want && !statusTimer_.isActive()) {
    statusTimer_.start();
  } else if (!want && statusTimer_.isActive()) {
    statusTimer_.stop();
  }
}

// ---------------------------------------------------------------- generic calls

void AdminController::launch(Pending p) {
  p.epoch = epoch_;
  auto start = p.start;
  start(p);
}

int AdminController::request(const QString& method, const QString& path, const QVariant& bodyIn) {
  // A JS object from QML may arrive wrapped in a QJSValue: turn it into plain QVariant data (maps and lists).
  const QVariant body = bodyIn.metaType() == QMetaType::fromType<QJSValue>() ? bodyIn.value<QJSValue>().toVariant() : bodyIn;
  Pending p;
  p.id = nextId_++;
  p.start = [this, method, path, body](const Pending& self) {
    api_.request(method, path, body, [this, self](const AdminApi::Result& r) { onResult(self, r); });
  };
  p.deliver = [this, id = p.id, method, path](const AdminApi::Result& r) {
    emit replied(id, r.ok, r.status, r.data, r.code, r.message);
    if (r.ok && method != QLatin1String("GET") && path.startsWith(QLatin1String("/admin/tools"))) emit toolsChanged();
  };
  const int id = p.id;
  launch(std::move(p));
  return id;
}

int AdminController::requireConfirmation() {
  Pending p;
  p.id = nextId_++;
  p.start = [this](const Pending& self) {
    AdminApi::Result r;
    if (secondsLeft_ > 0 || self.epoch < epoch_) {
      r.ok = true;
      r.status = 200;
    } else {
      r.status = 403;
      r.code = QStringLiteral("confirmation_required");
    }
    onResult(self, r);
  };
  p.deliver = [this, id = p.id](const AdminApi::Result& r) { emit confirmationAnswered(id, r.ok, r.code); };
  const int id = p.id;
  launch(std::move(p));
  return id;
}

int AdminController::download(const QString& path, const QString& fileUrlOrPath, const QString& method) {
  Pending p;
  p.id = nextId_++;
  const QString file = AdminApi::localPath(fileUrlOrPath);
  p.start = [this, path, file, method](const Pending& self) {
    api_.download(path, file, [this, self](const AdminApi::Result& r) { onResult(self, r); }, method);
  };
  p.deliver = [this, id = p.id](const AdminApi::Result& r) { emit downloadFinished(id, r.ok, r.filePath, r.message); };
  const int id = p.id;
  launch(std::move(p));
  return id;
}

int AdminController::upload(const QString& method, const QString& path, const QString& fileUrlOrPath, const QString& contentType,
                            const QVariantMap& headerMap) {
  Pending p;
  p.id = nextId_++;
  const QString file = AdminApi::localPath(fileUrlOrPath);
  HttpHeaders headers;
  for (auto it = headerMap.cbegin(); it != headerMap.cend(); ++it) headers.append({it.key().toLatin1(), it.value().toString().toLatin1()});
  p.start = [this, method, path, file, contentType, headers](const Pending& self) {
    api_.upload(method, path, file, contentType, [this, self](const AdminApi::Result& r) { onResult(self, r); }, headers);
  };
  p.deliver = [this, id = p.id](const AdminApi::Result& r) { emit replied(id, r.ok, r.status, r.data, r.code, r.message); };
  const int id = p.id;
  launch(std::move(p));
  return id;
}

QString AdminController::sha256OfFile(const QString& fileUrlOrPath) const {
  QFile f(AdminApi::localPath(fileUrlOrPath));
  if (!f.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash h(QCryptographicHash::Sha256);
  if (!h.addData(&f)) return {};
  return QString::fromLatin1(h.result().toHex());
}

qint64 AdminController::sizeOfFile(const QString& fileUrlOrPath) const {
  const QFileInfo fi(AdminApi::localPath(fileUrlOrPath));
  return fi.isFile() ? fi.size() : -1;
}

void AdminController::onResult(const Pending& p, const AdminApi::Result& r) {
  if (r.status == 403 && r.code == QLatin1String("confirmation_required")) {
    if (p.epoch < epoch_) {  // a confirmation happened while this one was on its way: send it again once
      launch(p);
      return;
    }
    parked_.append(p);
    if (!confirmNeeded_) {
      confirmNeeded_ = true;
      autoTried_ = false;
      confirmMessage_.clear();
      confirmState_ = lockedSeconds_ > 0 ? QStringLiteral("locked") : QStringLiteral("asking");
      const QString pw = (d_.autoConfirmPassword && lockedSeconds_ <= 0) ? d_.autoConfirmPassword() : QString();
      if (!pw.isEmpty()) {
        autoTried_ = true;
        confirm(pw);  // emits confirmChanged once, already in "checking"
      } else {
        emit confirmChanged();
      }
      updateClock();
    }
    return;
  }
  p.deliver(r);
}

// ---------------------------------------------------------------- Confirm it's you

QString AdminController::confirmError() const {
  if (confirmState_ == QLatin1String("wrong")) return tr("Wrong password.");
  if (confirmState_ == QLatin1String("locked")) {
    return tr("Admin steps are locked on this PC for %1. Everything else keeps working.").arg(mmss(lockedSeconds_));
  }
  if (confirmState_ == QLatin1String("unreachable")) return tr("Can't reach the server, so Uzume can't confirm it's you.");
  if (confirmState_ == QLatin1String("unset")) {
    return confirmMessage_.isEmpty() ? tr("This account has no password, so Uzume can't confirm it's you. Set a password in My account first.")
                                     : confirmMessage_;
  }
  return confirmMessage_;  // "asking" with a message from the server (e.g. the sign-in expired); empty otherwise
}

QString AdminController::confirmLeftText() const { return secondsLeft_ > 0 ? mmss(secondsLeft_) : QString(); }

void AdminController::setConfirmState(const QString& state) {
  confirmState_ = state;
  emit confirmChanged();
}

void AdminController::confirm(const QString& password) {
  if (!confirmNeeded_ || password.isEmpty()) return;
  if (confirmState_ == QLatin1String("checking") || confirmState_ == QLatin1String("locked")) return;
  confirmMessage_.clear();
  setConfirmState(QStringLiteral("checking"));
  api_.request(QStringLiteral("POST"), QStringLiteral("/account/confirm"),
               QVariantMap{{QStringLiteral("password"), password}, {QStringLiteral("duration_seconds"), kConfirmSeconds}},
               [this](const AdminApi::Result& r) {
                 if (!confirmNeeded_ || confirmState_ != QLatin1String("checking")) return;  // cancelled or disconnected meanwhile
                 if (r.ok) {
                   // The countdown starts now for the 300 s that were asked for: the server's clock may differ from this PC's.
                   confirmedUntil_ = QDateTime::currentDateTime().addSecs(kConfirmSeconds);
                   secondsLeft_ = kConfirmSeconds;
                   lockedUntil_ = QDateTime();
                   lockedSeconds_ = 0;
                   ++epoch_;
                   confirmNeeded_ = false;
                   confirmState_.clear();
                   updateClock();
                   emit confirmChanged();
                   replayParked();
                   return;
                 }
                 if (r.networkError) {
                   setConfirmState(QStringLiteral("unreachable"));
                 } else if (r.status == 429) {
                   lockedSeconds_ = r.retryAfter > 0 ? r.retryAfter : kLockFallbackSeconds;
                   lockedUntil_ = QDateTime::currentDateTime().addSecs(lockedSeconds_);
                   setConfirmState(QStringLiteral("locked"));
                   updateClock();
                 } else if (r.status == 401 && r.code == QLatin1String("invalid_credentials")) {
                   setConfirmState(QStringLiteral("wrong"));
                 } else if (r.status == 409 && r.code == QLatin1String("password_not_set")) {
                   confirmMessage_ = r.message;
                   setConfirmState(QStringLiteral("unset"));
                 } else {
                   confirmMessage_ = r.status == 401 ? tr("Your sign-in to the server expired. Try again.")
                                                     : (r.message.isEmpty() ? tr("The server did not accept the request.") : r.message);
                   setConfirmState(QStringLiteral("asking"));
                 }
               });
}

void AdminController::cancelConfirm() {
  if (!confirmNeeded_ || confirmState_ == QLatin1String("checking")) return;
  confirmNeeded_ = false;
  autoTried_ = false;
  confirmState_.clear();
  confirmMessage_.clear();
  emit confirmChanged();
  failParked(QStringLiteral("confirmation_cancelled"), tr("Cancelled."));
}

void AdminController::resumeParked() {
  if (!confirmNeeded_ && parked_.isEmpty()) return;
  // Another component confirmed (the same 5 minutes): the countdown assumes the default duration.
  if (secondsLeft_ <= 0) {
    confirmedUntil_ = QDateTime::currentDateTime().addSecs(kConfirmSeconds);
    secondsLeft_ = kConfirmSeconds;
  }
  ++epoch_;
  confirmNeeded_ = false;
  confirmState_.clear();
  confirmMessage_.clear();
  updateClock();
  emit confirmChanged();
  replayParked();
}

void AdminController::endConfirmation() {
  confirmedUntil_ = QDateTime();
  secondsLeft_ = 0;
  updateClock();
  emit confirmChanged();
  api_.request(QStringLiteral("DELETE"), QStringLiteral("/account/confirm"), QVariant(), [](const AdminApi::Result&) {});
}

void AdminController::replayParked() {
  QList<Pending> list;
  list.swap(parked_);
  for (const Pending& p : std::as_const(list)) launch(p);
}

void AdminController::failParked(const QString& code, const QString& message) {
  QList<Pending> list;
  list.swap(parked_);
  for (const Pending& p : std::as_const(list)) {
    AdminApi::Result r;
    r.code = code;
    r.message = message;
    p.deliver(r);
  }
}

void AdminController::updateClock() {
  const bool need = secondsLeft_ > 0 || lockedSeconds_ > 0;
  if (need && !clock_.isActive()) {
    clock_.start();
  } else if (!need && clock_.isActive()) {
    clock_.stop();
  }
}

void AdminController::tick() {
  const QDateTime now = QDateTime::currentDateTime();
  bool changed = false;
  if (confirmedUntil_.isValid()) {
    const int left = static_cast<int>(std::max<qint64>(0, (now.msecsTo(confirmedUntil_) + 999) / 1000));
    if (left != secondsLeft_) {
      secondsLeft_ = left;
      changed = true;
    }
    if (left == 0) confirmedUntil_ = QDateTime();
  }
  if (lockedUntil_.isValid()) {
    const int left = static_cast<int>(std::max<qint64>(0, (now.msecsTo(lockedUntil_) + 999) / 1000));
    if (left != lockedSeconds_) {
      lockedSeconds_ = left;
      changed = true;
    }
    if (left == 0) {
      lockedUntil_ = QDateTime();
      if (confirmNeeded_ && confirmState_ == QLatin1String("locked")) confirmState_ = QStringLiteral("asking");
    }
  }
  updateClock();
  if (changed) emit confirmChanged();
}

// ---------------------------------------------------------------- status

void AdminController::refreshStatus() {
  if (!available_ || statusInFlight_) return;
  statusInFlight_ = true;
  const int gen = statusGeneration_;
  if (status_.isEmpty() && statusState_ != QLatin1String("loading")) {
    statusState_ = QStringLiteral("loading");
    emit statusChanged();
  }
  api_.request(QStringLiteral("GET"), QStringLiteral("/admin/status"), QVariant(), [this, gen](const AdminApi::Result& r) {
    if (gen != statusGeneration_) return;  // another server meanwhile
    statusInFlight_ = false;
    applyStatus(r);
  });
  refreshCounts();
}

QString AdminController::thisDeviceId() const { return d_.conn ? d_.conn->deviceId() : QString(); }

void AdminController::refreshCounts() {
  if (!available_ || countsInFlight_) return;
  countsInFlight_ = true;
  const int gen = statusGeneration_;
  const bool errorsToo = d_.conn && d_.conn->hubHasFeature(QStringLiteral("reports_v2"));
  auto pending = std::make_shared<int>(errorsToo ? 5 : 4);
  auto apply = [this, gen, pending](int* target, int value) {
    if (gen != statusGeneration_) return;
    if (value >= 0 && *target != value) {
      *target = value;
      emit countsChanged();
    }
    if (--*pending == 0) countsInFlight_ = false;
  };
  api_.request(QStringLiteral("GET"), QStringLiteral("/admin/pairing-requests"), QVariant(), [this, apply](const AdminApi::Result& r) {
    int n = -1;
    if (r.ok && r.data.typeId() == QMetaType::QVariantMap) n = static_cast<int>(r.data.toMap().value(QStringLiteral("requests")).toList().size());
    apply(&pendingDevices_, n);
  });
  api_.request(QStringLiteral("GET"), QStringLiteral("/admin/reports?status=new"), QVariant(), [this, apply](const AdminApi::Result& r) {
    int n = -1;
    if (r.ok && r.data.typeId() == QMetaType::QVariantMap) {
      const QVariantMap m = r.data.toMap();
      n = m.contains(QStringLiteral("count_new")) ? m.value(QStringLiteral("count_new")).toInt() : 0;
    }
    apply(&newReports_, n);
  });
  // Unseen error groups (reports_v2) count on the Reports badge too (ADR 0035); the server adds nothing to count_new.
  if (errorsToo) {
    api_.request(QStringLiteral("GET"), QStringLiteral("/admin/reports/errors"), QVariant(), [this, apply](const AdminApi::Result& r) {
      int n = -1;
      if (r.ok && r.data.typeId() == QMetaType::QVariantMap) n = r.data.toMap().value(QStringLiteral("new")).toInt();
      apply(&newErrors_, n);
    });
  }
  // Plugins waiting for an update in the signed index (catalog entries in state "update").
  api_.request(QStringLiteral("GET"), QStringLiteral("/admin/plugins"), QVariant(), [this, apply](const AdminApi::Result& r) {
    int n = -1;
    if (r.ok && r.data.typeId() == QMetaType::QVariantMap) {
      n = 0;
      const QVariantList catalog = r.data.toMap().value(QStringLiteral("index")).toMap().value(QStringLiteral("catalog")).toList();
      for (const QVariant& e : catalog)
        if (e.toMap().value(QStringLiteral("state")).toString() == QLatin1String("update")) ++n;
    }
    apply(&pluginUpdates_, n);
  });
  // A server update on offer: settings.updates.available.version, skipped versions do not count. An answer that fails
  // (older server, no such field) leaves the last value alone.
  api_.request(QStringLiteral("GET"), QStringLiteral("/admin/settings"), QVariant(), [this, gen, pending](const AdminApi::Result& r) {
    if (gen == statusGeneration_ && r.ok && r.data.typeId() == QMetaType::QVariantMap) {
      const QVariantMap av = r.data.toMap().value(QStringLiteral("updates")).toMap().value(QStringLiteral("available")).toMap();
      const QString v = av.value(QStringLiteral("skipped")).toBool() ? QString() : av.value(QStringLiteral("version")).toString();
      if (v != serverUpdateVersion_) {
        serverUpdateVersion_ = v;
        emit countsChanged();
      }
    }
    if (gen == statusGeneration_ && --*pending == 0) countsInFlight_ = false;
  });
}

void AdminController::applyStatus(const AdminApi::Result& r) {
  QString problem;
  if (r.ok && r.data.typeId() == QMetaType::QVariantMap) {
    status_ = r.data.toMap();
    statusState_ = QStringLiteral("ready");
    const QVariantList problems = status_.value(QStringLiteral("problems")).toList();
    if (!problems.isEmpty()) problem = problems.first().toMap().value(QStringLiteral("title")).toString();
  } else if (r.status == 403) {
    status_.clear();
    statusState_ = QStringLiteral("forbidden");
  } else {
    statusState_ = QStringLiteral("unreachable");  // the last known data stays
    problem = statusProblem_;
  }
  statusProblem_ = problem;
  emit statusChanged();
}

}  // namespace uzume::ui
