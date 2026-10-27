// SecretServiceCredentialStore against a minimal fake org.freedesktop.secrets on a private dbus-daemon.
// The fake runs on its own thread with its own bus connection. Only homebrew dummy secrets are used.
#include <QtConcurrent>
#include <QtDBus>
#include <QtTest>

#include "credentialstore.h"
#include "secretservicetypes.h"

using namespace uzume;

namespace fakess {

using Attrs = QMap<QString, QString>;

using secretservice::Secret;

const QString kColl = QStringLiteral("/org/freedesktop/secrets/collection/default");
const QString kSess = QStringLiteral("/org/freedesktop/secrets/session/s1");

struct FakeItem {
  QString label;
  Attrs attrs;
  QByteArray value;
  QString contentType;
};

class Fake;

class PromptAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Prompt")
 public:
  PromptAdaptor(QObject* holder, Fake* fake, bool forCollection)
      : QDBusAbstractAdaptor(holder), fake_(fake), forCollection_(forCollection) {}
 public slots:
  void Prompt(const QString& windowId);
  void Dismiss() {}
 signals:
  void Completed(bool dismissed, const QDBusVariant& result);

 private:
  Fake* fake_;
  bool forCollection_;
};

class Fake : public QObject {
  Q_OBJECT
 public:
  enum PromptMode { Complete, Dismiss, Never };
  mutable QMutex m;
  bool collectionExists = true;
  bool locked = false;
  bool createNeedsPrompt = false;
  PromptMode promptMode = PromptMode::Complete;
  QMap<QString, FakeItem> items;  // by path
  int nextItem = 0;
  int nextPrompt = 0;
  int bogusCalls = 0;

  QMap<QString, FakeItem> snapshot() const {
    QMutexLocker l(&m);
    return items;
  }
  void set(const std::function<void(Fake&)>& f) {
    QMutexLocker l(&m);
    f(*this);
  }

  QDBusConnection conn{QStringLiteral("fake-unconnected")};

  QString newPrompt(bool forCollection) {
    const QString path = QStringLiteral("/org/freedesktop/secrets/prompt/p%1").arg(++nextPrompt);
    auto* holder = new QObject(this);
    new PromptAdaptor(holder, this, forCollection);
    conn.registerObject(path, holder, QDBusConnection::ExportAdaptors);
    return path;
  }
  void addItem(const QString& label, const Attrs& attrs, const QByteArray& value, const QString& ctype);

 public slots:
  void start();
  void stop();
};

class ServiceAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Service")
 public:
  ServiceAdaptor(QObject* holder, Fake* f) : QDBusAbstractAdaptor(holder), f_(f) {}
 public slots:
  QDBusVariant OpenSession(const QString& algorithm, const QDBusVariant& input, QDBusObjectPath& session) {
    Q_UNUSED(input);
    if (algorithm != QLatin1String("plain")) ++f_->bogusCalls;
    session = QDBusObjectPath(kSess);
    return QDBusVariant(QString());
  }
  QDBusObjectPath ReadAlias(const QString& name) {
    QMutexLocker l(&f_->m);
    return QDBusObjectPath(name == QLatin1String("default") && f_->collectionExists ? kColl : QStringLiteral("/"));
  }
  QDBusObjectPath CreateCollection(const QVariantMap& props, const QString& alias, QDBusObjectPath& prompt) {
    Q_UNUSED(props);
    QMutexLocker l(&f_->m);
    if (alias != QLatin1String("default")) ++f_->bogusCalls;
    if (f_->createNeedsPrompt) {
      prompt = QDBusObjectPath(f_->newPrompt(true));
      return QDBusObjectPath(QStringLiteral("/"));
    }
    f_->collectionExists = true;
    prompt = QDBusObjectPath(QStringLiteral("/"));
    return QDBusObjectPath(kColl);
  }
  QList<QDBusObjectPath> SearchItems(const QMap<QString, QString>& attrs, QList<QDBusObjectPath>& lockedOut) {
    QMutexLocker l(&f_->m);
    QList<QDBusObjectPath> found;
    for (auto it = f_->items.constBegin(); it != f_->items.constEnd(); ++it) {
      bool match = true;
      for (auto a = attrs.constBegin(); a != attrs.constEnd(); ++a) {
        if (it->attrs.value(a.key()) != a.value()) match = false;
      }
      if (match) found.append(QDBusObjectPath(it.key()));
    }
    if (f_->locked) {
      lockedOut = found;
      return {};
    }
    return found;
  }
  QList<QDBusObjectPath> Unlock(const QList<QDBusObjectPath>& objects, QDBusObjectPath& prompt) {
    QMutexLocker l(&f_->m);
    if (f_->locked) {
      prompt = QDBusObjectPath(f_->newPrompt(false));
      return {};
    }
    prompt = QDBusObjectPath(QStringLiteral("/"));
    return objects;
  }

 private:
  Fake* f_;
};

class CollectionAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Collection")
  Q_PROPERTY(bool Locked READ locked)
 public:
  CollectionAdaptor(QObject* holder, Fake* f) : QDBusAbstractAdaptor(holder), f_(f) {}
  bool locked() const {
    QMutexLocker l(&f_->m);
    return f_->locked;
  }
 public slots:
  QDBusObjectPath CreateItem(const QVariantMap& props, const uzume::secretservice::Secret& secret, bool replace, QDBusObjectPath& prompt) {
    QMutexLocker l(&f_->m);
    prompt = QDBusObjectPath(QStringLiteral("/"));
    const Attrs attrs = qdbus_cast<Attrs>(props.value(QStringLiteral("org.freedesktop.Secret.Item.Attributes")));
    if (replace) {
      for (auto it = f_->items.begin(); it != f_->items.end();) {
        it = (it->attrs == attrs) ? f_->items.erase(it) : std::next(it);
      }
    }
    if (secret.session.path() != kSess) ++f_->bogusCalls;
    const QString path = QStringLiteral("/org/freedesktop/secrets/collection/default/i%1").arg(++f_->nextItem);
    f_->items.insert(path, {props.value(QStringLiteral("org.freedesktop.Secret.Item.Label")).toString(), attrs,
                            secret.value, secret.contentType});
    f_->addItem(path, {}, {}, {});
    return QDBusObjectPath(path);
  }

 private:
  Fake* f_;
};

class ItemAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Item")
 public:
  ItemAdaptor(QObject* holder, Fake* f, const QString& path) : QDBusAbstractAdaptor(holder), f_(f), path_(path) {}
 public slots:
  QDBusObjectPath Delete() {
    QMutexLocker l(&f_->m);
    f_->items.remove(path_);
    f_->conn.unregisterObject(path_);
    parent()->deleteLater();
    return QDBusObjectPath(QStringLiteral("/"));
  }
  uzume::secretservice::Secret GetSecret(const QDBusObjectPath& session) {
    QMutexLocker l(&f_->m);
    const FakeItem it = f_->items.value(path_);
    return {session, QByteArray(), it.value, it.contentType};
  }

 private:
  Fake* f_;
  QString path_;
};

class SessionAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Session")
 public:
  explicit SessionAdaptor(QObject* holder) : QDBusAbstractAdaptor(holder) {}
 public slots:
  void Close() {}
};

// Note: slot parameter types of adaptors are spelled fully qualified (QtDBus resolves them by registered name).
// Registers the D-Bus object of an item already inserted into `items` (called with the mutex held).
void Fake::addItem(const QString& path, const Attrs&, const QByteArray&, const QString&) {
  auto* holder = new QObject(this);
  new ItemAdaptor(holder, this, path);
  conn.registerObject(path, holder, QDBusConnection::ExportAdaptors);
}

void PromptAdaptor::Prompt(const QString&) {
  QMutexLocker l(&fake_->m);
  const auto mode = fake_->promptMode;
  if (mode == Fake::Never) return;
  const bool forCollection = forCollection_;
  QTimer::singleShot(0, this, [this, mode, forCollection] {
    QMutexLocker l2(&fake_->m);
    if (mode == Fake::Dismiss) {
      emit Completed(true, QDBusVariant(QString()));
      return;
    }
    if (forCollection) {
      fake_->collectionExists = true;
      emit Completed(false, QDBusVariant(QVariant::fromValue(QDBusObjectPath(kColl))));
    } else {
      fake_->locked = false;
      emit Completed(false, QDBusVariant(QVariant::fromValue(QList<QDBusObjectPath>{QDBusObjectPath(kColl)})));
    }
  });
}

void Fake::start() {
  conn = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-secrets"));
  auto* root = new QObject(this);
  new ServiceAdaptor(root, this);
  conn.registerObject(QStringLiteral("/org/freedesktop/secrets"), root, QDBusConnection::ExportAdaptors);
  auto* coll = new QObject(this);
  new CollectionAdaptor(coll, this);
  conn.registerObject(kColl, coll, QDBusConnection::ExportAdaptors);
  auto* sess = new QObject(this);
  new SessionAdaptor(sess);
  conn.registerObject(kSess, sess, QDBusConnection::ExportAdaptors);
  conn.registerService(QStringLiteral("org.freedesktop.secrets"));
}

void Fake::stop() {
  conn.unregisterService(QStringLiteral("org.freedesktop.secrets"));
  qDeleteAll(children());
  conn = QDBusConnection(QStringLiteral("fake-unconnected"));
  QDBusConnection::disconnectFromBus(QStringLiteral("fake-secrets"));
}

}  // namespace fakess

using namespace fakess;

class SecretServiceStoreTest : public QObject {
  Q_OBJECT
 public:
  void startFake() {
    thread_ = new QThread;
    fake_ = new Fake;
    fake_->moveToThread(thread_);
    thread_->start();
    QMetaObject::invokeMethod(fake_, "start", Qt::BlockingQueuedConnection);
  }
  void stopFake() {
    if (!fake_) return;
    QMetaObject::invokeMethod(fake_, "stop", Qt::BlockingQueuedConnection);
    thread_->quit();
    thread_->wait();
    delete fake_;
    delete thread_;
    fake_ = nullptr;
    thread_ = nullptr;
  }

 private slots:
  void initTestCase() {
    const QString daemon = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
    if (daemon.isEmpty()) QSKIP("dbus-daemon not found");
    bus_.start(daemon, {QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address")});
    if (!bus_.waitForStarted() || !bus_.waitForReadyRead(10000)) QSKIP("private dbus-daemon did not start");
    const QByteArray address = bus_.readLine().trimmed();
    if (address.isEmpty()) QSKIP("private dbus-daemon printed no address");
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);
    qunsetenv("UZUME_CREDENTIAL_STORE");
    secretservice::registerTypes();
    // The fake dispatches on another thread and resolves slot parameter types by name; register these names up front
    // so the first incoming call does not depend on lazy QtDBus initialization.
    qRegisterMetaType<QDBusVariant>("QDBusVariant");
    qRegisterMetaType<QDBusObjectPath>("QDBusObjectPath");
  }
  void cleanup() {
    stopFake();
    qunsetenv("UZUME_CREDENTIAL_STORE");
  }
  void cleanupTestCase() {
    bus_.terminate();
    bus_.waitForFinished(5000);
  }

  void withoutServiceFallsBackToMemory() {
    QVERIFY(!SecretServiceCredentialStore::isAvailable());
    const auto store = createDefaultCredentialStore();
    QCOMPARE(store->backendName(), QStringLiteral("memory"));
    QVERIFY(!store->isPersistent());
    QVERIFY(store->write(QStringLiteral("Uzume/h/d"), QStringLiteral("x")));
    QCOMPARE(store->read(QStringLiteral("Uzume/h/d")).value_or(QString()), QStringLiteral("x"));
    // The Secret Service store itself fails cleanly without a service.
    SecretServiceCredentialStore ss(500);
    QVERIFY(!ss.write(QStringLiteral("Uzume/h/d"), QStringLiteral("x")));
    QVERIFY(!ss.read(QStringLiteral("Uzume/h/d")).has_value());
  }
  void withoutSessionBusFallsBackToMemory() {
    const QByteArray saved = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/uzume-no-bus");
    const auto store = createDefaultCredentialStore();
    qputenv("DBUS_SESSION_BUS_ADDRESS", saved);
    QCOMPARE(store->backendName(), QStringLiteral("memory"));
  }
  void defaultStoreUsesServiceUnlessOverridden() {
    startFake();
    QCOMPARE(createDefaultCredentialStore()->backendName(), QStringLiteral("secret-service"));
    qputenv("UZUME_CREDENTIAL_STORE", "memory");
    QCOMPARE(createDefaultCredentialStore()->backendName(), QStringLiteral("memory"));
  }
  void writeReadReplaceRemove() {
    startFake();
    const auto store = createDefaultCredentialStore();
    QCOMPARE(store->backendName(), QStringLiteral("secret-service"));
    QVERIFY(store->isPersistent());
    const QString target = credentialTarget(QStringLiteral("hub1"), QStringLiteral("dev1"));
    QVERIFY(!store->read(target).has_value());
    QVERIFY(store->write(target, QStringLiteral("secret-one")));
    QCOMPARE(store->read(target).value_or(QString()), QStringLiteral("secret-one"));
    const auto items = fake_->snapshot();
    QCOMPARE(items.size(), 1);
    const FakeItem item = items.first();
    QCOMPARE(item.attrs.value(QStringLiteral("application")), QStringLiteral("uzume"));
    QCOMPARE(item.attrs.value(QStringLiteral("uzume-target")), target);
    QCOMPARE(item.label, QStringLiteral("Uzume: ") + target);
    QCOMPARE(item.contentType, QStringLiteral("text/plain"));
    QVERIFY(store->write(target, QStringLiteral("secret-two (äö)")));
    QCOMPARE(store->read(target).value_or(QString()), QStringLiteral("secret-two (äö)"));
    QCOMPARE(fake_->snapshot().size(), 1);
    QVERIFY(store->remove(target));
    QVERIFY(!store->read(target).has_value());
    QCOMPARE(fake_->snapshot().size(), 0);
    QVERIFY(store->remove(target));  // missing entry: still true
    QCOMPARE(fake_->bogusCalls, 0);
  }
  void mcpTargetIsIndependent() {
    startFake();
    SecretServiceCredentialStore store;
    QVERIFY(store.write(QStringLiteral("Uzume/mcp"), QStringLiteral("token")));
    QVERIFY(store.write(QStringLiteral("Uzume/pin/h/d"), QStringLiteral("hash")));
    QCOMPARE(store.read(QStringLiteral("Uzume/mcp")).value_or(QString()), QStringLiteral("token"));
    QVERIFY(store.remove(QStringLiteral("Uzume/mcp")));
    QVERIFY(!store.read(QStringLiteral("Uzume/mcp")).has_value());
    QCOMPARE(store.read(QStringLiteral("Uzume/pin/h/d")).value_or(QString()), QStringLiteral("hash"));
  }
  void lockedCollectionWithCompletingPrompt() {
    startFake();
    fake_->set([](Fake& f) { f.locked = true; });
    SecretServiceCredentialStore store;
    QVERIFY(store.write(QStringLiteral("Uzume/a"), QStringLiteral("v")));
    fake_->set([](Fake& f) { f.locked = true; });
    QCOMPARE(store.read(QStringLiteral("Uzume/a")).value_or(QString()), QStringLiteral("v"));
    fake_->set([](Fake& f) { f.locked = true; });
    QVERIFY(store.remove(QStringLiteral("Uzume/a")));
    QCOMPARE(fake_->snapshot().size(), 0);
  }
  void dismissedPromptFailsWithoutCrash() {
    startFake();
    SecretServiceCredentialStore store;
    QVERIFY(store.write(QStringLiteral("Uzume/a"), QStringLiteral("v")));
    fake_->set([](Fake& f) {
      f.locked = true;
      f.promptMode = Fake::Dismiss;
    });
    QVERIFY(!store.write(QStringLiteral("Uzume/a"), QStringLiteral("changed")));
    QVERIFY(!store.read(QStringLiteral("Uzume/a")).has_value());
    QVERIFY(!store.remove(QStringLiteral("Uzume/a")));
    QCOMPARE(fake_->snapshot().size(), 1);
    QCOMPARE(fake_->snapshot().first().value, QByteArray("v"));
  }
  void unansweredPromptTimesOut() {
    startFake();
    fake_->set([](Fake& f) {
      f.locked = true;
      f.promptMode = Fake::Never;
    });
    SecretServiceCredentialStore store(300);
    QElapsedTimer t;
    t.start();
    QVERIFY(!store.write(QStringLiteral("Uzume/a"), QStringLiteral("v")));
    QVERIFY(t.elapsed() < 10000);
    QCOMPARE(fake_->snapshot().size(), 0);
  }
  void createsDefaultCollectionThroughPrompt() {
    startFake();
    fake_->set([](Fake& f) {
      f.collectionExists = false;
      f.createNeedsPrompt = true;
    });
    SecretServiceCredentialStore store;
    QVERIFY(store.write(QStringLiteral("Uzume/a"), QStringLiteral("v")));
    QCOMPARE(store.read(QStringLiteral("Uzume/a")).value_or(QString()), QStringLiteral("v"));
    fake_->set([](Fake& f) {
      f.items.clear();
      f.collectionExists = false;
      f.createNeedsPrompt = false;
    });
    QVERIFY(store.write(QStringLiteral("Uzume/b"), QStringLiteral("w")));
  }
  void worksFromWorkerThreads() {
    startFake();
    SecretServiceCredentialStore store;
    QList<QFuture<bool>> futures;
    for (int i = 0; i < 8; ++i) {
      futures.append(QtConcurrent::run([&store, i] {
        const QString target = QStringLiteral("Uzume/t%1").arg(i);
        const QString value = QStringLiteral("v%1").arg(i);
        return store.write(target, value) && store.read(target).value_or(QString()) == value &&
               store.remove(target);
      }));
    }
    for (auto& f : futures) QVERIFY(f.result());
    QCOMPARE(fake_->snapshot().size(), 0);
  }

 private:
  QProcess bus_;
  QThread* thread_ = nullptr;
  Fake* fake_ = nullptr;
};

QTEST_GUILESS_MAIN(SecretServiceStoreTest)
#include "secret_service_store_test.moc"
