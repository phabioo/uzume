#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace uzume {

// One person signed in on this PC for one server (0.18 package 7, ADR 0030 D5). Never contains a secret: credentialRef is
// only the key in the CredentialStore, and the PIN hash lives in the CredentialStore too (core/localpin.h); only the facts
// "has a PIN" and the count of wrong tries are kept here.
struct HubAccount {
  QString hubUserId;
  QString displayName;
  QString username;       // the sign-in name when the person signed in with it on this PC (hint for "password once"); may be empty
  QString role;           // "admin" | "user"; empty when unknown
  QString deviceId;       // this person's device row on the server (the first account on a server uses the install ID)
  QString credentialRef;  // empty: signed out on this PC (credential revoked or expired), the password is needed
  QDateTime lastUsed;
  bool hasPin = false;
  int pinFailures = 0;  // wrong PINs in a row; kLocalPinMaxTries locks the PIN until a password sign-in
};

// Hub profile per docs/architecture/10-identity-pairing-tls.md. Never contains a secret:
// credentialRef is only the key in the CredentialStore.
//
// Several people can be signed in on one PC (accounts). The flat fields hubUserId, userDisplayName, userRole, deviceId and
// credentialRef always describe the ACTIVE account (activeUserId); ProfileStore keeps both in sync: upsertProfile() writes
// the flat fields into the matching account, setActiveAccount() copies an account into the flat fields. With no active
// account the flat user fields are empty and deviceId is the device ID the next sign-in will use.
struct HubProfile {
  QString hubId;
  QString name;
  QString address;  // normalized: scheme://host[:port]
  QString hubUserId;
  QString userDisplayName;  // Hub user name from the last handshake; empty for older Hubs
  QString userRole;         // "admin" | "user" from the last handshake; empty for older Hubs
  QString deviceId;
  QString credentialRef;
  QString pinnedFingerprint;  // SHA-256 over leaf DER, uppercase hex with colons; empty for HTTP dev
  QDateTime lastConnected;
  bool allowHttp = false;  // dev flag: HTTP also outside localhost
  QList<HubAccount> accounts;  // everyone signed in on this PC for this server (an old profile loads as one account)
  QString activeUserId;        // the account the flat fields show; empty = none
};

// Local data storage: <base>/profiles.json, <base>/device.json, <base>/hubs/<hub_id>/, <base>/cache/roms/.
class ProfileStore {
 public:
  // baseDir empty: defaultBaseDir() (see below).
  explicit ProfileStore(const QString& baseDir = QString());

  // Order: env UZUME_DATA_DIR, portable <program directory>/data (only if writable;
  // one-time copy from AppData), otherwise AppDataLocation.
  static QString defaultBaseDir();
  struct BaseDirChoice {
    QString path;
    bool portable = false;
  };
  // Testable: empty appDir = unknown -> fall back to appDataDir.
  static BaseDirChoice chooseBaseDir(const QString& appDir, const QString& appDataDir);
  // Copies (without ROM cache, without overwriting, without touching the source) if newDir has no
  // profiles.json yet. Returns the number of copied files.
  static int migrateLegacyData(const QString& legacyDir, const QString& newDir);
  static bool isValidHubId(const QString& hubId);

  const QString& baseDir() const { return baseDir_; }
  QString profilesFilePath() const;
  // Hub-specific directory (created on demand); empty for an invalid hub ID.
  QString hubDir(const QString& hubId) const;
  // Cross-hub, content-addressed ROM cache (created on demand).
  QString romCacheDir() const;
  QString coreCacheDir() const;  // <data>/cache/cores (downloaded core packages, never migrated)
  QString toolCacheDir() const;     // <data>/cache/tools (tool packages of the server's catalog, e.g. the randomizer engine)
  QString variantCacheDir() const;  // <data>/cache/variants (built game variants, core/variantcache.h)

  // The ID of this install of the app (device.json). The first account on a server uses it as its device ID; sent as
  // `install_id` so the server can group the device rows of one PC.
  QString installId() const { return deviceId_; }
  QString deviceId() const { return deviceId_; }  // same as installId()
  QString deviceName() const { return deviceName_; }
  bool setDeviceName(const QString& name);

  QList<HubProfile> profiles() const { return profiles_; }
  std::optional<HubProfile> profile(const QString& hubId) const;
  std::optional<HubProfile> profileByAddress(const QString& address) const;
  // Updates the stored profile of profile.hubId (or adds it). For a stored profile the accounts list stays as stored and only
  // the account of profile.hubUserId is written from the flat fields (added if missing, made active); a profile with an
  // empty hubUserId (credential lost) keeps the account but empties its credentialRef and has no active account.
  bool upsertProfile(const HubProfile& profile);
  bool removeProfile(const QString& hubId);

  // Several accounts per server. accounts() is empty for an unknown server.
  QList<HubAccount> accounts(const QString& hubId) const;
  std::optional<HubAccount> account(const QString& hubId, const QString& userId) const;
  // Adds or replaces the account of account.hubUserId. When it is the active one the flat fields follow. False for an
  // unknown server or an empty user ID.
  bool upsertAccount(const QString& hubId, const HubAccount& account);
  // Removes the account (the caller deletes its credential and PIN). When it was the active one the flat user fields are
  // cleared and deviceId becomes newAccountDeviceId().
  bool removeAccount(const QString& hubId, const QString& userId);
  // Makes the account the active one and copies it into the flat fields. False for an unknown account.
  bool setActiveAccount(const QString& hubId, const QString& userId);
  // Device ID for an account that is created now: the install ID for the first account on the server, a fresh UUID
  // for every further one.
  QString newAccountDeviceId(const QString& hubId) const;

  // Hub edit (Settings > Hubs, docs/design/legacy/player.md 3p): only host and port of a saved Hub change. hubId, pinned
  // fingerprint, credentialRef, hubUserId and the scheme stay. Fails for an unknown Hub, an invalid input or a
  // target address that another saved Hub already uses.
  bool updateHubAddress(const QString& hubId, const QString& host, int port);
  // Validation of the edit fields (port as typed). One message per field at most; empty list = valid.
  static QStringList validateHubAddress(const QString& host, const QString& port);
  // "host" and "port" of a stored address (scheme://host[:port]); port 0 = none. Used to fill the edit row.
  static void splitAddress(const QString& address, QString* host, int* port);

  // Automatic connection at startup. An explicit choice of the user (setAutoConnect, stored as "auto_connect") always wins;
  // without one the default is on as soon as a server was connected successfully once (markConnectedOnce), so a second
  // start needs no extra click. Profile files written before that default carry an explicit value and keep it.
  bool autoConnect() const { return autoConnectChoice_.value_or(connectedOnce_); }
  bool autoConnectIsExplicit() const { return autoConnectChoice_.has_value(); }
  bool setAutoConnect(bool on);
  bool connectedOnce() const { return connectedOnce_; }
  // Called when a connection reached "connected"; stored as "connected_once". True when it is stored (or was already).
  bool markConnectedOnce();
  QString lastHubId() const { return lastHubId_; }
  bool setLastHubId(const QString& hubId);

 private:
  void load();
  bool save() const;
  bool saveDevice() const;

  QString baseDir_;
  QString deviceId_;
  QString deviceName_;
  QList<HubProfile> profiles_;
  std::optional<bool> autoConnectChoice_;
  bool connectedOnce_ = false;
  QString lastHubId_;
};

}  // namespace uzume
