#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include "credentialstore.h"
#include "profilestore.h"

using namespace uzume;

class ProfileStoreTest : public QObject {
  Q_OBJECT
 private slots:
  void deviceIdIsStable() {
    QTemporaryDir dir;
    QString id;
    {
      ProfileStore s(dir.path());
      id = s.deviceId();
      QVERIFY(!QUuid::fromString(id).isNull());
      QVERIFY(!s.deviceName().isEmpty());
    }
    ProfileStore again(dir.path());
    QCOMPARE(again.deviceId(), id);
  }

  void corruptProfilesAndDeviceFilesAreKeptNotOverwritten() {
    QTemporaryDir dir;
    {
      QFile f(dir.filePath(QStringLiteral("profiles.json")));
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("{\"profiles\": [ typo");
    }
    {
      QFile f(dir.filePath(QStringLiteral("device.json")));
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("not json");
    }
    ProfileStore s(dir.path());
    QVERIFY(!QUuid::fromString(s.deviceId()).isNull());  // a new identity, but the old file is not lost
    HubProfile p;
    p.hubId = QStringLiteral("hub-1");
    p.address = QStringLiteral("https://192.0.2.10:8443");
    QVERIFY(s.upsertProfile(p));
    QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("profiles.json.corrupt-*")}, QDir::Files).size(), 1);
    QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("device.json.corrupt-*")}, QDir::Files).size(), 1);
  }

  void profileRoundTripWithoutSecrets() {
    QTemporaryDir dir;
    HubProfile p;
    p.hubId = QStringLiteral("hub-1");
    p.name = QStringLiteral("Living room");
    p.address = QStringLiteral("https://192.0.2.10:8443");
    p.hubUserId = QStringLiteral("u_1");
    p.deviceId = QStringLiteral("dev");
    p.credentialRef = credentialTarget(p.hubId, p.deviceId);
    p.pinnedFingerprint = QStringLiteral("AA:BB");
    p.lastConnected = QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5), Qt::UTC);
    {
      ProfileStore s(dir.path());
      QVERIFY(s.upsertProfile(p));
      QVERIFY(s.setAutoConnect(true));
      QVERIFY(s.setLastHubId(p.hubId));
    }
    ProfileStore s2(dir.path());
    QCOMPARE(s2.profiles().size(), 1);
    const auto q = s2.profile(p.hubId);
    QVERIFY(q.has_value());
    QCOMPARE(q->name, p.name);
    QCOMPARE(q->credentialRef, QStringLiteral("Uzume/hub-1/dev"));
    QCOMPARE(q->pinnedFingerprint, p.pinnedFingerprint);
    QCOMPARE(q->lastConnected, p.lastConnected);
    QVERIFY(s2.autoConnect());
    QCOMPARE(s2.lastHubId(), p.hubId);
    QVERIFY(s2.profileByAddress(QStringLiteral("HTTPS://192.0.2.10:8443")).has_value());
    QVERIFY(s2.removeProfile(p.hubId));
    QVERIFY(s2.profiles().isEmpty());
    QVERIFY(s2.lastHubId().isEmpty());
  }

  // Automatic connection: off until a server was connected once, then on by default; an explicit choice always wins.
  void autoConnectDefaultFollowsFirstSuccessfulConnection() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
      ProfileStore s(dir.path());
      QVERIFY(!s.autoConnect());
      QVERIFY(!s.autoConnectIsExplicit());
      QVERIFY(s.markConnectedOnce());
      QVERIFY(s.autoConnect());
      QVERIFY(!s.autoConnectIsExplicit());
    }
    ProfileStore s2(dir.path());
    QVERIFY(s2.connectedOnce());
    QVERIFY(s2.autoConnect());  // the default survives a restart
    QVERIFY(s2.setAutoConnect(false));
    QVERIFY(s2.markConnectedOnce());
    QVERIFY(!s2.autoConnect());  // explicit off is respected
    ProfileStore s3(dir.path());
    QVERIFY(s3.autoConnectIsExplicit());
    QVERIFY(!s3.autoConnect());
  }

  void legacyProfileFileTurnsAutoConnectOn() {
    for (const bool legacyValue : {false, true}) {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QFile f(QDir(dir.path()).filePath(QStringLiteral("profiles.json")));
      QVERIFY(f.open(QIODevice::WriteOnly));
      const QByteArray json = QByteArray(R"({"version":1,"settings":{"auto_connect":)") + (legacyValue ? "true" : "false") +
                              R"(,"last_hub_id":"hub-1"},"profiles":[{"hub_id":"hub-1","name":"Home","address":"https://192.0.2.10:8443"}]})";
      f.write(json);
      f.close();
      {
        ProfileStore s(dir.path());
        QVERIFY(s.profile(QStringLiteral("hub-1")).has_value());
        QVERIFY(s.autoConnect());  // a legacy false is "not set" and the saved server counts as connected once
        QCOMPARE(s.autoConnectIsExplicit(), legacyValue);  // a legacy true stays explicit
        QVERIFY(s.setLastHubId(QStringLiteral("hub-1")));  // rewrites the file in the new format
      }
      ProfileStore s2(dir.path());
      QVERIFY(s2.autoConnect());
      QCOMPARE(s2.autoConnectIsExplicit(), legacyValue);
      QVERIFY(s2.setAutoConnect(false));
      ProfileStore s3(dir.path());
      QVERIFY(!s3.autoConnect());  // an explicit off written now is respected
    }
  }

  void hubDirsAreSeparatedAndSafe() {
    QTemporaryDir dir;
    ProfileStore s(dir.path());
    QVERIFY(s.hubDir(QStringLiteral("a")) != s.hubDir(QStringLiteral("b")));
    QVERIFY(s.hubDir(QStringLiteral("../evil")).isEmpty());
    QVERIFY(s.hubDir(QStringLiteral("a/b")).isEmpty());
    QVERIFY(!s.upsertProfile([] { HubProfile p; p.hubId = QStringLiteral("../x"); return p; }()));
    QVERIFY(s.romCacheDir().endsWith(QStringLiteral("cache/roms")));
  }

  void chooseBaseDirNeverPortableInsideAppImage() {
    const QByteArray old = qgetenv("APPIMAGE");
    auto restore = qScopeGuard([&]() { old.isNull() ? qunsetenv("APPIMAGE") : qputenv("APPIMAGE", old); });
    QTemporaryDir app;  // writable, as the mount of an extract-and-run image is
    qputenv("APPIMAGE", "/home/x/Uzume.AppImage");
    const auto c = ProfileStore::chooseBaseDir(app.path(), QStringLiteral("/appdata"));
    QVERIFY(!c.portable);
    QCOMPARE(c.path, QStringLiteral("/appdata"));
    QVERIFY(!QFileInfo::exists(QDir(app.path()).filePath(QStringLiteral("data"))));
    qunsetenv("APPIMAGE");
    QVERIFY(ProfileStore::chooseBaseDir(app.path(), QStringLiteral("/appdata")).portable);
  }

  void chooseBaseDirPortableOrFallback() {
    qunsetenv("APPIMAGE");
    QTemporaryDir app;
    const auto c = ProfileStore::chooseBaseDir(app.path(), QStringLiteral("/appdata"));
    QVERIFY(c.portable);
    QCOMPARE(c.path, QDir(app.path()).filePath(QStringLiteral("data")));
    QVERIFY(QDir(c.path).entryList(QDir::Files | QDir::Hidden).isEmpty());  // write test cleans up
    // not writable: "data" is a file
    QTemporaryDir bad;
    QFile f(QDir(bad.path()).filePath(QStringLiteral("data")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();
    const auto b = ProfileStore::chooseBaseDir(bad.path(), QStringLiteral("/appdata"));
    QVERIFY(!b.portable);
    QCOMPARE(b.path, QStringLiteral("/appdata"));
    QVERIFY(!ProfileStore::chooseBaseDir(QString(), QStringLiteral("/appdata")).portable);
  }

  void migrationCopiesWithoutOverwriteOrDelete() {
    QTemporaryDir oldDir, newDir;
    auto write = [](const QString& base, const QString& rel, const QByteArray& data) {
      const QString p = QDir(base).filePath(rel);
      QDir().mkpath(QFileInfo(p).absolutePath());
      QFile f(p);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(data);
    };
    HubProfile p;
    p.hubId = QStringLiteral("hub-1");
    p.address = QStringLiteral("https://192.0.2.10:8443");
    QString devId;
    {
      ProfileStore old(oldDir.path());
      devId = old.deviceId();
      QVERIFY(old.upsertProfile(p));
    }
    write(oldDir.path(), QStringLiteral("hubs/hub-1/saves/a.sav"), "old-save");
    write(oldDir.path(), QStringLiteral("cache/roms/x.rom"), "rom");
    write(newDir.path(), QStringLiteral("hubs/hub-1/saves/a.sav"), "keep-me");

    QVERIFY(ProfileStore::migrateLegacyData(oldDir.path(), newDir.path()) > 0);
    ProfileStore ns(newDir.path());
    QCOMPARE(ns.deviceId(), devId);
    QVERIFY(ns.profile(QStringLiteral("hub-1")).has_value());
    QFile keep(QDir(newDir.path()).filePath(QStringLiteral("hubs/hub-1/saves/a.sav")));
    QVERIFY(keep.open(QIODevice::ReadOnly));
    QCOMPARE(keep.readAll(), QByteArray("keep-me"));
    QVERIFY(!QFileInfo::exists(QDir(newDir.path()).filePath(QStringLiteral("cache/roms/x.rom"))));
    QVERIFY(QFileInfo::exists(QDir(oldDir.path()).filePath(QStringLiteral("hubs/hub-1/saves/a.sav"))));
    QVERIFY(QFileInfo::exists(QDir(oldDir.path()).filePath(QStringLiteral("profiles.json"))));
    // second run: marker exists -> nothing more
    QVERIFY(QFileInfo::exists(QDir(newDir.path()).filePath(QStringLiteral(".migrated-from-appdata"))));
    QCOMPARE(ProfileStore::migrateLegacyData(oldDir.path(), newDir.path()), 0);
  }

  void interruptedMigrationCompletes() {
    QTemporaryDir oldDir, newDir;
    QString devId;
    {
      ProfileStore old(oldDir.path());
      devId = old.deviceId();
      HubProfile p;
      p.hubId = QStringLiteral("hub-1");
      QVERIFY(old.upsertProfile(p));
    }
    // simulate interruption: only profiles.json was copied, no marker, no device.json
    QVERIFY(QFile::copy(QDir(oldDir.path()).filePath(QStringLiteral("profiles.json")),
                        QDir(newDir.path()).filePath(QStringLiteral("profiles.json"))));
    QVERIFY(ProfileStore::migrateLegacyData(oldDir.path(), newDir.path()) > 0);
    ProfileStore ns(newDir.path());
    QCOMPARE(ns.deviceId(), devId);
    QVERIFY(QFileInfo::exists(QDir(newDir.path()).filePath(QStringLiteral(".migrated-from-appdata"))));
  }

  void failedCopyLeavesNoMarkerAndRetries() {
    QTemporaryDir oldDir, newDir;
    {
      ProfileStore old(oldDir.path());
      QVERIFY(!old.deviceId().isEmpty());
      QDir().mkpath(old.hubDir(QStringLiteral("hub-1")));
    }
    QFile sav(QDir(oldDir.path()).filePath(QStringLiteral("hubs/hub-1/a.sav")));
    QVERIFY(sav.open(QIODevice::WriteOnly));
    sav.write("s");
    sav.close();
    // "hubs" is a file in the target: the save cannot be copied
    QFile block(QDir(newDir.path()).filePath(QStringLiteral("hubs")));
    QVERIFY(block.open(QIODevice::WriteOnly));
    block.close();
    const QString marker = QDir(newDir.path()).filePath(QStringLiteral(".migrated-from-appdata"));
    QVERIFY(ProfileStore::migrateLegacyData(oldDir.path(), newDir.path()) >= 1);  // device.json copied
    QVERIFY(!QFileInfo::exists(marker));
    QVERIFY(QFile::remove(block.fileName()));
    QVERIFY(ProfileStore::migrateLegacyData(oldDir.path(), newDir.path()) >= 1);  // save copied now
    QVERIFY(QFileInfo::exists(QDir(newDir.path()).filePath(QStringLiteral("hubs/hub-1/a.sav"))));
    QVERIFY(QFileInfo::exists(marker));
    QCOMPARE(ProfileStore::migrateLegacyData(oldDir.path(), newDir.path()), 0);
  }

  void legacyDeviceIdWinsOnlyWithoutCredentials() {
    QTemporaryDir oldDir, newDir;
    QString devId;
    {
      ProfileStore old(oldDir.path());
      devId = old.deviceId();
    }
    { ProfileStore fresh(newDir.path()); QVERIFY(fresh.deviceId() != devId); }
    QVERIFY(ProfileStore::migrateLegacyData(oldDir.path(), newDir.path()) > 0);
    QCOMPARE(ProfileStore(newDir.path()).deviceId(), devId);

    QTemporaryDir old2, new2;
    { ProfileStore o(old2.path()); }
    QString keep;
    {
      ProfileStore n(new2.path());
      keep = n.deviceId();
      HubProfile p;
      p.hubId = QStringLiteral("hub-1");
      p.credentialRef = QStringLiteral("Uzume/hub-1/dev");
      QVERIFY(n.upsertProfile(p));
    }
    ProfileStore::migrateLegacyData(old2.path(), new2.path());
    QCOMPARE(ProfileStore(new2.path()).deviceId(), keep);
  }

  void memoryCredentialStore() {
    MemoryCredentialStore c;
    QVERIFY(!c.read(QStringLiteral("t")).has_value());
    QVERIFY(c.write(QStringLiteral("t"), QStringLiteral("dummy")));
    QCOMPARE(c.read(QStringLiteral("t")).value(), QStringLiteral("dummy"));
    QVERIFY(c.remove(QStringLiteral("t")));
    QVERIFY(!c.read(QStringLiteral("t")).has_value());
  }
  void credentialTargetUsesTheNewName() {
    QCOMPARE(credentialTarget(QStringLiteral("hub-1"), QStringLiteral("dev")), QStringLiteral("Uzume/hub-1/dev"));
  }
  void credentialReadAndRemove() {
    MemoryCredentialStore c;
    c.write(QStringLiteral("Uzume/h/d"), QStringLiteral("secret"));
    QCOMPARE(readCredential(c, QStringLiteral("Uzume/h/d")).value(), QStringLiteral("secret"));
    QVERIFY(!readCredential(c, QStringLiteral("Uzume/other/d")).has_value());
    QVERIFY(!readCredential(c, QString()).has_value());
    QVERIFY(removeCredential(c, QStringLiteral("Uzume/h/d")));
    QVERIFY(!c.read(QStringLiteral("Uzume/h/d")).has_value());
  }
  void hubAddressValidatorMessages() {
    const auto msgs = [](const char* host, const char* port) { return ProfileStore::validateHubAddress(QString::fromUtf8(host), QString::fromUtf8(port)); };
    QVERIFY(msgs("hub.example.com", "8443").isEmpty());
    QVERIFY(msgs("hub-1.local", "1").isEmpty());
    QVERIFY(msgs("[::1]", "65535").isEmpty());
    QCOMPARE(msgs("", "8443"), QStringList{QStringLiteral("Enter an address")});
    QCOMPARE(msgs("   ", "8443"), QStringList{QStringLiteral("Enter an address")});
    QCOMPARE(msgs("https://hub.example.com", "8443"), QStringList{QStringLiteral("Leave out https:// — host name only")});
    QCOMPARE(msgs("hub example.com", "8443"), QStringList{QStringLiteral("No spaces in the address")});
    QCOMPARE(msgs("hub.example.com:8443", "8443"), QStringList{QStringLiteral("Put the port in the port field")});
    QCOMPARE(msgs("hub_example.com", "8443"), QStringList{QStringLiteral("Only letters, digits, dots and hyphens")});
    QCOMPARE(msgs("hub.example.com/path", "8443"), QStringList{QStringLiteral("Only letters, digits, dots and hyphens")});
    QCOMPARE(msgs("[zz]", "8443"), QStringList{QStringLiteral("Only letters, digits, dots and hyphens")});
    const QString portMsg = QStringLiteral("Port must be a number from 1 to 65535");
    QCOMPARE(msgs("hub.example.com", ""), QStringList{portMsg});
    QCOMPARE(msgs("hub.example.com", "0"), QStringList{portMsg});
    QCOMPARE(msgs("hub.example.com", "65536"), QStringList{portMsg});
    QCOMPARE(msgs("hub.example.com", "84a3"), QStringList{portMsg});
    // Both fields invalid: the messages add up (the UI joins them with " · ").
    QCOMPARE(msgs("", "x"), (QStringList{QStringLiteral("Enter an address"), portMsg}));
  }

  void editHubKeepsIdentityFingerprintAndCredential() {
    QTemporaryDir dir;
    HubProfile p;
    p.hubId = QStringLiteral("hub-1");
    p.name = QStringLiteral("Home");
    p.address = QStringLiteral("https://hub.example.com:8443");
    p.hubUserId = QStringLiteral("u_1");
    p.deviceId = QStringLiteral("dev");
    p.credentialRef = credentialTarget(p.hubId, p.deviceId);
    p.pinnedFingerprint = QStringLiteral("AA:BB:CC");
    HubProfile other = p;
    other.hubId = QStringLiteral("hub-2");
    other.address = QStringLiteral("https://hub.local:8443");
    {
      ProfileStore s(dir.path());
      QVERIFY(s.upsertProfile(p));
      QVERIFY(s.upsertProfile(other));
      QVERIFY(!s.updateHubAddress(QStringLiteral("hub-1"), QStringLiteral("hub.local"), 8443));  // taken by hub-2
      QVERIFY(!s.updateHubAddress(QStringLiteral("hub-1"), QStringLiteral("bad host"), 8443));   // invalid
      QVERIFY(!s.updateHubAddress(QStringLiteral("nope"), QStringLiteral("hub2.example.com"), 8443));
      QCOMPARE(s.profile(QStringLiteral("hub-1"))->address, p.address);
      QVERIFY(s.updateHubAddress(QStringLiteral("hub-1"), QStringLiteral("hub2.example.com"), 9443));
    }
    ProfileStore again(dir.path());
    const auto q = again.profile(QStringLiteral("hub-1"));
    QVERIFY(q.has_value());
    QCOMPARE(q->address, QStringLiteral("https://hub2.example.com:9443"));
    QCOMPARE(q->name, p.name);
    QCOMPARE(q->hubUserId, p.hubUserId);
    QCOMPARE(q->deviceId, p.deviceId);
    QCOMPARE(q->credentialRef, p.credentialRef);
    QCOMPARE(q->pinnedFingerprint, p.pinnedFingerprint);
    QString host;
    int port = 0;
    ProfileStore::splitAddress(q->address, &host, &port);
    QCOMPARE(host, QStringLiteral("hub2.example.com"));
    QCOMPARE(port, 9443);
  }
  void editHubIpv6AndFailedSaveKeepsAddress() {
    QTemporaryDir dir;
    HubProfile p;
    p.hubId = QStringLiteral("hub-6");
    p.name = QStringLiteral("V6");
    p.address = QStringLiteral("https://hub.example.com:8443");
    ProfileStore s(dir.path());
    QVERIFY(s.upsertProfile(p));
    QVERIFY(s.updateHubAddress(QStringLiteral("hub-6"), QStringLiteral("[::1]"), 8443));
    QCOMPARE(s.profile(QStringLiteral("hub-6"))->address, QStringLiteral("https://[::1]:8443"));
    QString host;
    int port = 0;
    ProfileStore::splitAddress(s.profile(QStringLiteral("hub-6"))->address, &host, &port);
    QCOMPARE(host, QStringLiteral("[::1]"));
    QCOMPARE(port, 8443);

    // Make the write fail: a directory in place of profiles.json.
    const QString file = QDir(dir.path()).filePath(QStringLiteral("profiles.json"));
    QVERIFY(QFile::remove(file));
    QVERIFY(QDir().mkdir(file));
    QVERIFY(!s.updateHubAddress(QStringLiteral("hub-6"), QStringLiteral("hub2.example.com"), 9443));
    QCOMPARE(s.profile(QStringLiteral("hub-6"))->address, QStringLiteral("https://[::1]:8443"));
  }

  // ---- Several accounts per server (0.18 package 7) ----

  void oldProfileLoadsAsOneAccount() {
    QTemporaryDir dir;
    {
      QFile f(dir.filePath(QStringLiteral("profiles.json")));
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("{\"version\":1,\"settings\":{},\"profiles\":[{\"hub_id\":\"hub-1\",\"name\":\"Home\",\"address\":\"https://192.0.2.10:8443\","
              "\"hub_user_id\":\"u_max\",\"user_display_name\":\"Max\",\"user_role\":\"admin\",\"device_id\":\"dev-1\",\"credential_ref\":\"Uzume/hub-1/dev-1\","
              "\"pinned_fingerprint\":\"AA:BB\",\"last_connected\":\"2026-10-20T10:00:00Z\",\"allow_http\":false},"
              "{\"hub_id\":\"hub-2\",\"name\":\"Unpaired\",\"address\":\"https://192.0.2.11:8443\",\"device_id\":\"dev-1\"}]}");
    }
    ProfileStore s(dir.path());
    const auto p = s.profile(QStringLiteral("hub-1"));
    QVERIFY(p.has_value());
    QCOMPARE(p->accounts.size(), 1);
    QCOMPARE(p->activeUserId, QStringLiteral("u_max"));
    const HubAccount a = p->accounts.first();
    QCOMPARE(a.hubUserId, QStringLiteral("u_max"));
    QCOMPARE(a.displayName, QStringLiteral("Max"));
    QCOMPARE(a.role, QStringLiteral("admin"));
    QCOMPARE(a.deviceId, QStringLiteral("dev-1"));
    QCOMPARE(a.credentialRef, QStringLiteral("Uzume/hub-1/dev-1"));
    QVERIFY(!a.hasPin);
    QCOMPARE(a.pinFailures, 0);
    QCOMPARE(p->hubUserId, QStringLiteral("u_max"));  // the flat fields stay the active account
    QVERIFY(s.profile(QStringLiteral("hub-2"))->accounts.isEmpty());
    QVERIFY(s.profile(QStringLiteral("hub-2"))->activeUserId.isEmpty());
  }

  void accountsStayInSyncWithTheFlatFields() {
    QTemporaryDir dir;
    QString install;
    {
      ProfileStore s(dir.path());
      install = s.installId();
      QCOMPARE(s.deviceId(), install);
      HubProfile p;
      p.hubId = QStringLiteral("hub-1");
      p.address = QStringLiteral("https://192.0.2.10:8443");
      QVERIFY(s.upsertProfile(p));
      QVERIFY(s.accounts(p.hubId).isEmpty());
      QCOMPARE(s.newAccountDeviceId(p.hubId), install);  // the first account uses the install ID

      // Pairing as Max: the flat fields describe him, the account follows.
      p.hubUserId = QStringLiteral("u_max");
      p.userDisplayName = QStringLiteral("Max");
      p.userRole = QStringLiteral("admin");
      p.deviceId = install;
      p.credentialRef = credentialTarget(p.hubId, install);
      QVERIFY(s.upsertProfile(p));
      QCOMPARE(s.accounts(p.hubId).size(), 1);
      QCOMPARE(s.profile(p.hubId)->activeUserId, QStringLiteral("u_max"));
      QVERIFY(s.newAccountDeviceId(p.hubId) != install);  // a further account gets a fresh one
      QVERIFY(!QUuid::fromString(s.newAccountDeviceId(p.hubId)).isNull());

      // A stale copy of the profile (with the old accounts list) does not overwrite the accounts.
      HubProfile stale = *s.profile(p.hubId);
      HubAccount lena;
      lena.hubUserId = QStringLiteral("u_lena");
      lena.displayName = QStringLiteral("Lena");
      lena.role = QStringLiteral("user");
      lena.deviceId = QStringLiteral("dev-lena");
      lena.credentialRef = credentialTarget(p.hubId, lena.deviceId);
      lena.hasPin = true;
      QVERIFY(s.upsertAccount(p.hubId, lena));
      QCOMPARE(s.accounts(p.hubId).size(), 2);
      QCOMPARE(s.profile(p.hubId)->activeUserId, QStringLiteral("u_max"));  // adding does not switch
      stale.lastConnected = QDateTime::currentDateTimeUtc();
      QVERIFY(s.upsertProfile(stale));
      QCOMPARE(s.accounts(p.hubId).size(), 2);
      QVERIFY(s.account(p.hubId, QStringLiteral("u_max"))->lastUsed.isValid());  // the flat last-connected time went to Max

      // Switching copies Lena into the flat fields.
      QVERIFY(s.setActiveAccount(p.hubId, QStringLiteral("u_lena")));
      HubProfile now = *s.profile(p.hubId);
      QCOMPARE(now.hubUserId, QStringLiteral("u_lena"));
      QCOMPARE(now.userDisplayName, QStringLiteral("Lena"));
      QCOMPARE(now.userRole, QStringLiteral("user"));
      QCOMPARE(now.deviceId, QStringLiteral("dev-lena"));
      QCOMPARE(now.credentialRef, lena.credentialRef);
      QCOMPARE(now.activeUserId, QStringLiteral("u_lena"));
      QVERIFY(!s.setActiveAccount(p.hubId, QStringLiteral("u_nobody")));
      QVERIFY(!s.setActiveAccount(QStringLiteral("hub-x"), QStringLiteral("u_lena")));

      // The handshake renames the active account through the profile; the PIN facts survive an upsert of the profile.
      now.userDisplayName = QStringLiteral("Lena K.");
      QVERIFY(s.upsertProfile(now));
      QCOMPARE(s.account(p.hubId, QStringLiteral("u_lena"))->displayName, QStringLiteral("Lena K."));
      QVERIFY(s.account(p.hubId, QStringLiteral("u_lena"))->hasPin);
      // An older server sends no name: the known one stays in the account.
      now.userDisplayName.clear();
      QVERIFY(s.upsertProfile(now));
      QCOMPARE(s.account(p.hubId, QStringLiteral("u_lena"))->displayName, QStringLiteral("Lena K."));

      // upsertAccount on the active account moves the flat fields too.
      HubAccount changed = *s.account(p.hubId, QStringLiteral("u_lena"));
      changed.pinFailures = 2;
      changed.displayName = QStringLiteral("Lena");
      QVERIFY(s.upsertAccount(p.hubId, changed));
      QCOMPARE(s.profile(p.hubId)->userDisplayName, QStringLiteral("Lena"));
      QVERIFY(!s.upsertAccount(p.hubId, HubAccount()));
      QVERIFY(!s.upsertAccount(QStringLiteral("hub-x"), changed));
    }
    // Round trip through the file.
    ProfileStore again(dir.path());
    const auto p = again.profile(QStringLiteral("hub-1"));
    QCOMPARE(p->accounts.size(), 2);
    QCOMPARE(p->activeUserId, QStringLiteral("u_lena"));
    QCOMPARE(again.account(p->hubId, QStringLiteral("u_lena"))->pinFailures, 2);
    QVERIFY(again.account(p->hubId, QStringLiteral("u_lena"))->hasPin);
    QCOMPARE(again.account(p->hubId, QStringLiteral("u_max"))->role, QStringLiteral("admin"));
    QCOMPARE(again.account(p->hubId, QStringLiteral("u_max"))->deviceId, install);
  }

  void lostCredentialKeepsThePersonInTheList() {
    QTemporaryDir dir;
    ProfileStore s(dir.path());
    HubProfile p;
    p.hubId = QStringLiteral("hub-1");
    p.address = QStringLiteral("https://192.0.2.10:8443");
    p.hubUserId = QStringLiteral("u_max");
    p.userDisplayName = QStringLiteral("Max");
    p.deviceId = s.installId();
    p.credentialRef = credentialTarget(p.hubId, p.deviceId);
    QVERIFY(s.upsertProfile(p));
    // HubConnection::onCredentialInvalid: credential and user ID of the flat fields are cleared.
    HubProfile lost = *s.profile(p.hubId);
    lost.credentialRef.clear();
    lost.hubUserId.clear();
    QVERIFY(s.upsertProfile(lost));
    const auto after = s.profile(p.hubId);
    QVERIFY(after->hubUserId.isEmpty());
    QVERIFY(after->activeUserId.isEmpty());
    QCOMPARE(after->accounts.size(), 1);
    QVERIFY(after->accounts.first().credentialRef.isEmpty());
    QCOMPARE(after->accounts.first().displayName, QStringLiteral("Max"));
    QCOMPARE(after->accounts.first().deviceId, s.installId());
  }

  void removingAccounts() {
    QTemporaryDir dir;
    ProfileStore s(dir.path());
    HubProfile p;
    p.hubId = QStringLiteral("hub-1");
    p.address = QStringLiteral("https://192.0.2.10:8443");
    p.hubUserId = QStringLiteral("u_max");
    p.userDisplayName = QStringLiteral("Max");
    p.deviceId = s.installId();
    p.credentialRef = credentialTarget(p.hubId, p.deviceId);
    QVERIFY(s.upsertProfile(p));
    HubAccount lena;
    lena.hubUserId = QStringLiteral("u_lena");
    lena.deviceId = QStringLiteral("dev-lena");
    lena.credentialRef = credentialTarget(p.hubId, lena.deviceId);
    QVERIFY(s.upsertAccount(p.hubId, lena));

    // A person who is not the active one: the flat fields stay.
    QVERIFY(s.removeAccount(p.hubId, QStringLiteral("u_lena")));
    QCOMPARE(s.accounts(p.hubId).size(), 1);
    QCOMPARE(s.profile(p.hubId)->hubUserId, QStringLiteral("u_max"));
    QVERIFY(s.removeAccount(p.hubId, QStringLiteral("u_lena")));  // already gone: fine
    QVERIFY(!s.removeAccount(QStringLiteral("hub-x"), QStringLiteral("u_max")));

    // The active one: the flat user fields are cleared; the last account leaves the install ID for the next first account.
    QVERIFY(s.upsertAccount(p.hubId, lena));
    QVERIFY(s.removeAccount(p.hubId, QStringLiteral("u_max")));
    HubProfile after = *s.profile(p.hubId);
    QVERIFY(after.hubUserId.isEmpty());
    QVERIFY(after.credentialRef.isEmpty());
    QVERIFY(after.activeUserId.isEmpty());
    QVERIFY(after.userDisplayName.isEmpty());
    QCOMPARE(after.accounts.size(), 1);
    QVERIFY(after.deviceId != s.installId());  // somebody else is still there: a fresh device ID for the next person
    QVERIFY(s.setActiveAccount(p.hubId, QStringLiteral("u_lena")));
    QVERIFY(s.removeAccount(p.hubId, QStringLiteral("u_lena")));
    QVERIFY(s.profile(p.hubId)->accounts.isEmpty());
    QCOMPARE(s.profile(p.hubId)->deviceId, s.installId());
  }
};

QTEST_GUILESS_MAIN(ProfileStoreTest)
#include "profilestore_test.moc"
