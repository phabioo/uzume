#include "credentialstore.h"

#include <QLoggingCategory>
#include <QMutexLocker>
#include <mutex>

#include "envvar.h"

#ifdef Q_OS_LINUX
#include "secretservicetypes.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QEventLoop>
#include <QObject>
#include <QTimer>
#include <atomic>
#endif

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>

#include <string>
#endif

namespace uzume {

Q_LOGGING_CATEGORY(lcCred, "uzume.credentials")

QString credentialTarget(const QString& hubId, const QString& deviceId) {
  return QStringLiteral("Uzume/%1/%2").arg(hubId, deviceId);
}

CredentialRead CredentialStore::readChecked(const QString& target) const {
  const auto v = read(target);
  if (!v) return {};
  return {CredentialRead::Status::Ok, *v};
}

MemoryCredentialStore::MemoryCredentialStore() {
  static std::once_flag warned;
  std::call_once(warned, [] {
    qCWarning(lcCred) << "No OS credential store available: credentials are kept in memory only, not persisted.";
  });
}

bool MemoryCredentialStore::write(const QString& target, const QString& secret) {
  QMutexLocker lock(&mutex_);
  items_.insert(target, secret);
  return true;
}

std::optional<QString> MemoryCredentialStore::read(const QString& target) const {
  QMutexLocker lock(&mutex_);
  const auto it = items_.constFind(target);
  if (it == items_.constEnd()) {
    return std::nullopt;
  }
  return it.value();
}

bool MemoryCredentialStore::remove(const QString& target) {
  QMutexLocker lock(&mutex_);
  items_.remove(target);
  return true;
}

#ifdef Q_OS_WIN
bool WindowsCredentialStore::write(const QString& target, const QString& secret) {
  const std::wstring wtarget = target.toStdWString();
  const std::wstring wuser = L"Uzume";
  const QByteArray blob = secret.toUtf8();
  CREDENTIALW cred = {};
  cred.Type = CRED_TYPE_GENERIC;
  cred.TargetName = const_cast<LPWSTR>(wtarget.c_str());
  cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
  cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(blob.constData()));
  cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
  cred.UserName = const_cast<LPWSTR>(wuser.c_str());
  return CredWriteW(&cred, 0) == TRUE;
}

std::optional<QString> WindowsCredentialStore::read(const QString& target) const {
  const std::wstring wtarget = target.toStdWString();
  PCREDENTIALW cred = nullptr;
  if (CredReadW(wtarget.c_str(), CRED_TYPE_GENERIC, 0, &cred) != TRUE || cred == nullptr) {
    return std::nullopt;
  }
  const QString secret = QString::fromUtf8(reinterpret_cast<const char*>(cred->CredentialBlob),
                                           static_cast<qsizetype>(cred->CredentialBlobSize));
  CredFree(cred);
  return secret;
}

bool WindowsCredentialStore::remove(const QString& target) {
  const std::wstring wtarget = target.toStdWString();
  if (CredDeleteW(wtarget.c_str(), CRED_TYPE_GENERIC, 0) == TRUE) {
    return true;
  }
  return GetLastError() == ERROR_NOT_FOUND;
}
#endif

#ifdef Q_OS_LINUX
namespace {

using secretservice::Secret;
using secretservice::StringMap;
const QString kBusName = QStringLiteral("org.freedesktop.secrets");
const QString kServicePath = QStringLiteral("/org/freedesktop/secrets");
const QString kServiceIface = QStringLiteral("org.freedesktop.Secret.Service");
const QString kCollectionIface = QStringLiteral("org.freedesktop.Secret.Collection");
const QString kItemIface = QStringLiteral("org.freedesktop.Secret.Item");
const QString kSessionIface = QStringLiteral("org.freedesktop.Secret.Session");
const QString kPromptIface = QStringLiteral("org.freedesktop.Secret.Prompt");
const QString kNoObject = QStringLiteral("/");
constexpr int kCallTimeoutMs = 15000;

StringMap attributesFor(const QString& target) {
  return {{QStringLiteral("application"), QStringLiteral("uzume")}, {QStringLiteral("uzume-target"), target}};
}

// Receives Prompt.Completed.
class PromptWaiter : public QObject {
  Q_OBJECT
 public:
  bool finished = false;
  bool dismissed = true;
  QVariant result;
 public slots:
  void onCompleted(bool dismissed_, const QDBusVariant& result_) {
    finished = true;
    dismissed = dismissed_;
    result = result_.variant();
    emit done();
  }
 signals:
  void done();
};

// One private session-bus connection (own name, created and closed on the calling thread) plus one Secret Service
// session. Nothing is shared between operations, hence safe from any worker thread.
class Op {
 public:
  explicit Op(int promptTimeoutMs, int callTimeoutMs = kCallTimeoutMs)
      : promptTimeoutMs_(promptTimeoutMs), callTimeoutMs_(callTimeoutMs) {
    secretservice::registerTypes();
    static std::atomic<quint64> counter{0};
    name_ = QStringLiteral("uzume-secret-%1").arg(counter.fetch_add(1));
    conn_.emplace(QDBusConnection::connectToBus(QDBusConnection::SessionBus, name_));
  }
  ~Op() {
    if (conn_ && conn_->isConnected() && !session_.isEmpty()) {
      call(session_, kSessionIface, QStringLiteral("Close"));
    }
    conn_.reset();
    QDBusConnection::disconnectFromBus(name_);
  }
  Op(const Op&) = delete;
  Op& operator=(const Op&) = delete;

  bool connected() const { return conn_ && conn_->isConnected(); }

  // Returns the reply arguments, or nullopt on a D-Bus error.
  std::optional<QVariantList> call(const QString& path, const QString& iface, const QString& method,
                                   const QVariantList& args = {}, const QString& bus = kBusName) const {
    QDBusMessage msg = QDBusMessage::createMethodCall(bus, path, iface, method);
    msg.setArguments(args);
    const QDBusMessage reply = conn_->call(msg, QDBus::Block, callTimeoutMs_);
    if (reply.type() != QDBusMessage::ReplyMessage) {
      qCDebug(lcCred) << "Secret Service call failed:" << method << reply.errorName() << reply.errorMessage();
      return std::nullopt;
    }
    return reply.arguments();
  }

  bool openSession() {
    const auto r = call(kServicePath, kServiceIface, QStringLiteral("OpenSession"),
                        {QStringLiteral("plain"), QVariant::fromValue(QDBusVariant(QString()))});
    if (!r || r->size() < 2) return false;
    session_ = qdbus_cast<QDBusObjectPath>(r->at(1)).path();
    return !session_.isEmpty() && session_ != kNoObject;
  }

  const QString& session() const { return session_; }

  // Waits for the prompt to complete. nullopt: dismissed, failed or timed out.
  std::optional<QVariant> runPrompt(const QString& promptPath) {
    PromptWaiter waiter;
    QEventLoop loop;
    QObject::connect(&waiter, &PromptWaiter::done, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (!conn_->connect(kBusName, promptPath, kPromptIface, QStringLiteral("Completed"), &waiter,
                        SLOT(onCompleted(bool, QDBusVariant)))) {
      return std::nullopt;
    }
    std::optional<QVariant> out;
    if (call(promptPath, kPromptIface, QStringLiteral("Prompt"), {QString()})) {
      timer.start(promptTimeoutMs_);
      if (!waiter.finished) loop.exec();
      if (waiter.finished && !waiter.dismissed) {
        out = waiter.result;
      } else if (!waiter.finished) {
        qCWarning(lcCred) << "Secret Service prompt timed out";
        call(promptPath, kPromptIface, QStringLiteral("Dismiss"));
      }
    }
    conn_->disconnect(kBusName, promptPath, kPromptIface, QStringLiteral("Completed"), &waiter,
                      SLOT(onCompleted(bool, QDBusVariant)));
    return out;
  }

  // Unlocks the objects; prompts are awaited. True when the call (and its prompt, if any) succeeded.
  bool unlock(const QList<QDBusObjectPath>& objects) {
    if (objects.isEmpty()) return true;
    const auto r = call(kServicePath, kServiceIface, QStringLiteral("Unlock"), {QVariant::fromValue(objects)});
    if (!r || r->size() < 2) return false;
    const QString prompt = qdbus_cast<QDBusObjectPath>(r->at(1)).path();
    return prompt == kNoObject || runPrompt(prompt).has_value();
  }

  // Path of the unlocked "default" collection; creates it when missing.
  std::optional<QString> defaultCollection() {
    const auto r = call(kServicePath, kServiceIface, QStringLiteral("ReadAlias"), {QStringLiteral("default")});
    if (!r || r->isEmpty()) return std::nullopt;
    QString path = qdbus_cast<QDBusObjectPath>(r->at(0)).path();
    if (path == kNoObject) {
      QVariantMap props;
      props.insert(kCollectionIface + QStringLiteral(".Label"), QStringLiteral("Uzume"));
      const auto c = call(kServicePath, kServiceIface, QStringLiteral("CreateCollection"),
                          {props, QStringLiteral("default")});
      if (!c || c->size() < 2) return std::nullopt;
      path = qdbus_cast<QDBusObjectPath>(c->at(0)).path();
      const QString prompt = qdbus_cast<QDBusObjectPath>(c->at(1)).path();
      if (prompt != kNoObject) {
        const auto res = runPrompt(prompt);
        if (!res) return std::nullopt;
        path = qdbus_cast<QDBusObjectPath>(*res).path();
      }
      if (path.isEmpty() || path == kNoObject) return std::nullopt;
    }
    const auto locked = call(path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"),
                             {kCollectionIface, QStringLiteral("Locked")});
    if (!locked || locked->isEmpty()) return std::nullopt;
    if (qdbus_cast<QDBusVariant>(locked->at(0)).variant().toBool()) {
      if (!unlock({QDBusObjectPath(path)})) return std::nullopt;
    }
    return path;
  }

  struct Found {
    bool ok = false;
    QList<QDBusObjectPath> items;  // unlocked afterwards
  };

  // Finds the items of `target` and unlocks them when needed.
  Found find(const QString& target) {
    Found out;
    const auto r = call(kServicePath, kServiceIface, QStringLiteral("SearchItems"),
                        {QVariant::fromValue(attributesFor(target))});
    if (!r || r->size() < 2) return out;
    out.items = qdbus_cast<QList<QDBusObjectPath>>(r->at(0));
    const auto locked = qdbus_cast<QList<QDBusObjectPath>>(r->at(1));
    if (!locked.isEmpty()) {
      if (!unlock(locked)) return out;
      out.items += locked;
    }
    out.ok = true;
    return out;
  }

 private:
  int promptTimeoutMs_;
  int callTimeoutMs_;
  QString name_;
  std::optional<QDBusConnection> conn_;
  QString session_;
};

}  // namespace

bool SecretServiceCredentialStore::write(const QString& target, const QString& secret) {
  Op op(promptTimeoutMs_);
  if (!op.connected() || !op.openSession()) return false;
  const auto collection = op.defaultCollection();
  if (!collection) return false;
  QVariantMap props;
  props.insert(kItemIface + QStringLiteral(".Label"), QStringLiteral("Uzume: %1").arg(target));
  props.insert(kItemIface + QStringLiteral(".Attributes"), QVariant::fromValue(attributesFor(target)));
  const Secret s{QDBusObjectPath(op.session()), QByteArray(), secret.toUtf8(), QStringLiteral("text/plain")};
  const auto r = op.call(*collection, kCollectionIface, QStringLiteral("CreateItem"),
                         {props, QVariant::fromValue(s), true});
  if (!r || r->size() < 2) return false;
  const QString prompt = qdbus_cast<QDBusObjectPath>(r->at(1)).path();
  return prompt == kNoObject || op.runPrompt(prompt).has_value();
}

CredentialRead SecretServiceCredentialStore::readChecked(const QString& target) const {
  using Status = CredentialRead::Status;
  Op op(promptTimeoutMs_);
  if (!op.connected() || !op.openSession()) return {Status::Error, {}};
  const auto found = op.find(target);
  if (!found.ok) return {Status::Error, {}};
  if (found.items.isEmpty()) return {Status::NotFound, {}};
  const auto r = op.call(found.items.first().path(), kItemIface, QStringLiteral("GetSecret"),
                         {QVariant::fromValue(QDBusObjectPath(op.session()))});
  if (!r || r->isEmpty()) return {Status::Error, {}};
  const Secret s = qdbus_cast<Secret>(r->at(0));
  return {Status::Ok, QString::fromUtf8(s.value)};
}

std::optional<QString> SecretServiceCredentialStore::read(const QString& target) const {
  const CredentialRead r = readChecked(target);
  if (r.status != CredentialRead::Status::Ok) return std::nullopt;
  return r.secret;
}

bool SecretServiceCredentialStore::remove(const QString& target) {
  Op op(promptTimeoutMs_);
  if (!op.connected()) return false;
  const auto found = op.find(target);
  if (!found.ok) return false;
  bool ok = true;
  for (const auto& item : found.items) {
    const auto r = op.call(item.path(), kItemIface, QStringLiteral("Delete"));
    if (!r || r->isEmpty()) {
      ok = false;
      continue;
    }
    const QString prompt = qdbus_cast<QDBusObjectPath>(r->at(0)).path();
    if (prompt != kNoObject && !op.runPrompt(prompt)) ok = false;
  }
  return ok;
}

bool SecretServiceCredentialStore::isAvailable() {
  Op op(1000, 5000);  // activation of a stopped service may take a moment, but never forever
  if (!op.connected()) return false;
  const auto owned = op.call(QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
                             QStringLiteral("NameHasOwner"), {kBusName}, QStringLiteral("org.freedesktop.DBus"));
  bool present = owned && !owned->isEmpty() && owned->at(0).toBool();
  if (!present) {
    const auto activatable = op.call(QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
                                     QStringLiteral("ListActivatableNames"), {}, QStringLiteral("org.freedesktop.DBus"));
    present = activatable && !activatable->isEmpty() && activatable->at(0).toStringList().contains(kBusName);
  }
  if (!present) return false;
  // Registered is not working: this call activates a merely activatable service and must be answered. No prompt is
  // needed for OpenSession, so a service that cannot show one still passes only when it really runs.
  if (!op.openSession()) {
    qCWarning(lcCred) << "Secret Service is registered but does not answer";
    return false;
  }
  return true;
}
#endif

std::optional<QString> readCredential(CredentialStore& store, const QString& ref) {
  if (ref.isEmpty()) return std::nullopt;
  return store.read(ref);
}

CredentialRead readCredentialChecked(CredentialStore& store, const QString& ref) {
  if (ref.isEmpty()) return {};
  return store.readChecked(ref);
}

bool removeCredential(CredentialStore& store, const QString& ref) { return store.remove(ref); }

std::unique_ptr<CredentialStore> createDefaultCredentialStore() {
#ifdef Q_OS_WIN
  return std::make_unique<WindowsCredentialStore>();
#else
#ifdef Q_OS_LINUX
  if (env::get("UZUME_CREDENTIAL_STORE") != QLatin1String("memory") && SecretServiceCredentialStore::isAvailable()) {
    static std::once_flag logged;
    std::call_once(logged, [] { qCInfo(lcCred) << "Credential backend: Secret Service"; });
    return std::make_unique<SecretServiceCredentialStore>();
  }
#endif
  return std::make_unique<MemoryCredentialStore>();
#endif
}

}  // namespace uzume

#ifdef Q_OS_LINUX
#include "credentialstore.moc"
#endif
