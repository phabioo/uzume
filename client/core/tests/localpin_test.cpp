#include <QTemporaryDir>
#include <QtTest>

#include "credentialstore.h"
#include "localpin.h"
#include "profilestore.h"

using namespace uzume;
using Status = LocalPin::Status;

class LocalPinTest : public QObject {
  Q_OBJECT
  const QString hub = QStringLiteral("hub-1");

  static void addAccount(ProfileStore& s, const QString& hubId, const QString& user, const QString& device) {
    HubAccount a;
    a.hubUserId = user;
    a.displayName = user;
    a.deviceId = device;
    a.credentialRef = credentialTarget(hubId, device);
    QVERIFY(s.upsertAccount(hubId, a));
  }

 private slots:
  void pinLifecycle() {
    QTemporaryDir dir;
    ProfileStore s(dir.path());
    MemoryCredentialStore creds;
    HubProfile p;
    p.hubId = hub;
    p.address = QStringLiteral("https://192.0.2.10:8443");
    QVERIFY(s.upsertProfile(p));
    addAccount(s, hub, QStringLiteral("u_lena"), QStringLiteral("dev-lena"));
    LocalPin pin(&s, &creds);

    QVERIFY(!pin.hasPin(hub, QStringLiteral("u_lena")));
    QCOMPARE(pin.verify(hub, QStringLiteral("u_lena"), QStringLiteral("1234")).status, Status::NoPin);
    QVERIFY(!pin.setPin(hub, QStringLiteral("u_lena"), QStringLiteral("123")));      // not four digits
    QVERIFY(!pin.setPin(hub, QStringLiteral("u_lena"), QStringLiteral("12a4")));
    QVERIFY(!pin.setPin(hub, QStringLiteral("u_nobody"), QStringLiteral("1234")));    // unknown account
    QVERIFY(pin.setPin(hub, QStringLiteral("u_lena"), QStringLiteral("1234")));
    QVERIFY(pin.hasPin(hub, QStringLiteral("u_lena")));
    QVERIFY(s.account(hub, QStringLiteral("u_lena"))->hasPin);
    QCOMPARE(pin.triesLeft(hub, QStringLiteral("u_lena")), 3);

    // The hash is in the credential store under its own target, never in profiles.json.
    const auto stored = creds.read(pinTarget(hub, QStringLiteral("dev-lena")));
    QVERIFY(stored.has_value());
    QVERIFY(!stored->contains(QStringLiteral("1234")));
    QByteArray json;
    {
      // Closed right away: on Windows an open handle makes the next atomic save of profiles.json fail.
      QFile f(s.profilesFilePath());
      QVERIFY(f.open(QIODevice::ReadOnly));
      json = f.readAll();
    }
    QVERIFY(!json.contains(stored->toUtf8()));
    QVERIFY(json.contains("has_pin"));

    auto r = pin.verify(hub, QStringLiteral("u_lena"), QStringLiteral("1234"));
    QVERIFY(r.ok());
    r = pin.verify(hub, QStringLiteral("u_lena"), QStringLiteral("0000"));
    QCOMPARE(r.status, Status::Wrong);
    QCOMPARE(r.triesLeft, 2);
    QCOMPARE(s.account(hub, QStringLiteral("u_lena"))->pinFailures, 1);
    r = pin.verify(hub, QStringLiteral("u_lena"), QStringLiteral("1234"));  // a right PIN resets the count
    QVERIFY(r.ok());
    QCOMPARE(s.account(hub, QStringLiteral("u_lena"))->pinFailures, 0);

    // Three wrong PINs lock it; even the right one does not open it then.
    QCOMPARE(pin.verify(hub, QStringLiteral("u_lena"), QStringLiteral("1111")).triesLeft, 2);
    QCOMPARE(pin.verify(hub, QStringLiteral("u_lena"), QStringLiteral("2222")).triesLeft, 1);
    r = pin.verify(hub, QStringLiteral("u_lena"), QStringLiteral("3333"));
    QCOMPARE(r.status, Status::Locked);
    QVERIFY(pin.isLocked(hub, QStringLiteral("u_lena")));
    QCOMPARE(pin.verify(hub, QStringLiteral("u_lena"), QStringLiteral("1234")).status, Status::Locked);
    // The lock survives a restart (it is in profiles.json).
    ProfileStore again(dir.path());
    LocalPin pinAgain(&again, &creds);
    QVERIFY(pinAgain.isLocked(hub, QStringLiteral("u_lena")));
    // A password sign-in clears it; the PIN stays.
    QVERIFY(pinAgain.clearLock(hub, QStringLiteral("u_lena")));
    QVERIFY(!pinAgain.isLocked(hub, QStringLiteral("u_lena")));
    QVERIFY(pinAgain.verify(hub, QStringLiteral("u_lena"), QStringLiteral("1234")).ok());

    // Changing the PIN replaces the hash (and clears a lock); removing deletes it.
    QVERIFY(pinAgain.setPin(hub, QStringLiteral("u_lena"), QStringLiteral("9876")));
    QCOMPARE(pinAgain.verify(hub, QStringLiteral("u_lena"), QStringLiteral("1234")).status, Status::Wrong);
    QVERIFY(pinAgain.verify(hub, QStringLiteral("u_lena"), QStringLiteral("9876")).ok());
    QVERIFY(pinAgain.removePin(hub, QStringLiteral("u_lena")));
    QVERIFY(!pinAgain.hasPin(hub, QStringLiteral("u_lena")));
    QVERIFY(!creds.read(pinTarget(hub, QStringLiteral("dev-lena"))).has_value());
    QCOMPARE(pinAgain.verify(hub, QStringLiteral("u_lena"), QStringLiteral("9876")).status, Status::NoPin);
  }

  void saltAndPerAccountIsolation() {
    QTemporaryDir dir;
    ProfileStore s(dir.path());
    MemoryCredentialStore creds;
    HubProfile p;
    p.hubId = hub;
    p.address = QStringLiteral("https://192.0.2.10:8443");
    QVERIFY(s.upsertProfile(p));
    addAccount(s, hub, QStringLiteral("u_a"), QStringLiteral("dev-a"));
    addAccount(s, hub, QStringLiteral("u_b"), QStringLiteral("dev-b"));
    LocalPin pin(&s, &creds);
    QVERIFY(pin.setPin(hub, QStringLiteral("u_a"), QStringLiteral("4242")));
    QVERIFY(pin.setPin(hub, QStringLiteral("u_b"), QStringLiteral("4242")));
    // Same PIN, different salt: the stored values differ.
    QVERIFY(*creds.read(pinTarget(hub, QStringLiteral("dev-a"))) != *creds.read(pinTarget(hub, QStringLiteral("dev-b"))));
    // Wrong tries of one person do not count for the other.
    pin.verify(hub, QStringLiteral("u_a"), QStringLiteral("0000"));
    QCOMPARE(pin.triesLeft(hub, QStringLiteral("u_a")), 2);
    QCOMPARE(pin.triesLeft(hub, QStringLiteral("u_b")), 3);
    // A hash copied from one account does not verify as the other (the device ID is part of the salt).
    QVERIFY(creds.write(pinTarget(hub, QStringLiteral("dev-b")), *creds.read(pinTarget(hub, QStringLiteral("dev-a")))));
    QCOMPARE(pin.verify(hub, QStringLiteral("u_b"), QStringLiteral("4242")).status, Status::Wrong);
  }

  void missingHashLocks() {
    QTemporaryDir dir;
    ProfileStore s(dir.path());
    MemoryCredentialStore creds;
    HubProfile p;
    p.hubId = hub;
    p.address = QStringLiteral("https://192.0.2.10:8443");
    QVERIFY(s.upsertProfile(p));
    addAccount(s, hub, QStringLiteral("u_a"), QStringLiteral("dev-a"));
    LocalPin pin(&s, &creds);
    QVERIFY(pin.setPin(hub, QStringLiteral("u_a"), QStringLiteral("4242")));
    creds.remove(pinTarget(hub, QStringLiteral("dev-a")));  // the credential store lost it
    QCOMPARE(pin.verify(hub, QStringLiteral("u_a"), QStringLiteral("4242")).status, Status::Locked);
    LocalPin::removeStored(creds, hub, QStringLiteral("dev-a"));  // harmless when absent
  }

  // A keyring that cannot be read is not a missing hash: no lock, no counted try.
  void keyringErrorKeepsState() {
    struct FailingStore : MemoryCredentialStore {
      bool fail = false;
      CredentialRead readChecked(const QString& t) const override {
        if (fail) {
          CredentialRead r;
          r.status = CredentialRead::Status::Error;
          return r;
        }
        return MemoryCredentialStore::readChecked(t);
      }
    };
    QTemporaryDir dir;
    ProfileStore s(dir.path());
    FailingStore creds;
    HubProfile p;
    p.hubId = hub;
    p.address = QStringLiteral("https://192.0.2.10:8443");
    QVERIFY(s.upsertProfile(p));
    addAccount(s, hub, QStringLiteral("u_a"), QStringLiteral("dev-a"));
    LocalPin pin(&s, &creds);
    QVERIFY(pin.setPin(hub, QStringLiteral("u_a"), QStringLiteral("4242")));
    creds.fail = true;
    for (int i = 0; i < 5; ++i) QCOMPARE(pin.verify(hub, QStringLiteral("u_a"), QStringLiteral("4242")).status, Status::Unavailable);
    QVERIFY(pin.hasPin(hub, QStringLiteral("u_a")));
    QVERIFY(!pin.isLocked(hub, QStringLiteral("u_a")));
    QCOMPARE(pin.triesLeft(hub, QStringLiteral("u_a")), 3);
    creds.fail = false;
    QCOMPARE(pin.verify(hub, QStringLiteral("u_a"), QStringLiteral("4242")).status, Status::Ok);
  }
};

QTEST_GUILESS_MAIN(LocalPinTest)
#include "localpin_test.moc"
