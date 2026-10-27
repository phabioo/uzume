#include "profilestore.h"
#include "envvar.h"

#include "fsutil.h"
#include "installroot.h"
#include "savestore.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>

namespace uzume {

Q_LOGGING_CATEGORY(lcProfiles, "uzume.profiles")

namespace {

QJsonObject readJsonFile(const QString& path) { return fsutil::readJsonObject(path); }  // corrupt file: moved aside first

bool writeJsonFile(const QString& path, const QJsonObject& obj) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    return false;
  }
  f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
  return f.commit();
}

QJsonObject toJson(const HubAccount& a) {
  QJsonObject o;
  o.insert(QStringLiteral("hub_user_id"), a.hubUserId);
  o.insert(QStringLiteral("display_name"), a.displayName);
  o.insert(QStringLiteral("username"), a.username);
  o.insert(QStringLiteral("role"), a.role);
  o.insert(QStringLiteral("device_id"), a.deviceId);
  o.insert(QStringLiteral("credential_ref"), a.credentialRef);
  o.insert(QStringLiteral("last_used"), a.lastUsed.isValid() ? a.lastUsed.toUTC().toString(Qt::ISODate) : QString());
  o.insert(QStringLiteral("has_pin"), a.hasPin);
  o.insert(QStringLiteral("pin_failures"), a.pinFailures);
  return o;
}

HubAccount accountFromJson(const QJsonObject& o) {
  HubAccount a;
  a.hubUserId = o.value(QStringLiteral("hub_user_id")).toString();
  a.displayName = o.value(QStringLiteral("display_name")).toString();
  a.username = o.value(QStringLiteral("username")).toString();
  a.role = o.value(QStringLiteral("role")).toString();
  a.deviceId = o.value(QStringLiteral("device_id")).toString();
  a.credentialRef = o.value(QStringLiteral("credential_ref")).toString();
  a.lastUsed = QDateTime::fromString(o.value(QStringLiteral("last_used")).toString(), Qt::ISODate);
  a.hasPin = o.value(QStringLiteral("has_pin")).toBool(false);
  a.pinFailures = o.value(QStringLiteral("pin_failures")).toInt(0);
  return a;
}

// The account the flat fields of a profile describe.
HubAccount accountFromFlat(const HubProfile& p) {
  HubAccount a;
  a.hubUserId = p.hubUserId;
  a.displayName = p.userDisplayName;
  a.role = p.userRole;
  a.deviceId = p.deviceId;
  a.credentialRef = p.credentialRef;
  a.lastUsed = p.lastConnected;
  return a;
}

void copyAccountToFlat(HubProfile& p, const HubAccount& a) {
  p.hubUserId = a.hubUserId;
  p.userDisplayName = a.displayName;
  p.userRole = a.role;
  p.deviceId = a.deviceId;
  p.credentialRef = a.credentialRef;
  p.activeUserId = a.hubUserId;
}

qsizetype accountIndex(const QList<HubAccount>& list, const QString& userId) {
  for (qsizetype i = 0; i < list.size(); ++i) {
    if (list.at(i).hubUserId == userId) {
      return i;
    }
  }
  return -1;
}

QJsonObject toJson(const HubProfile& p) {
  QJsonObject o;
  o.insert(QStringLiteral("hub_id"), p.hubId);
  o.insert(QStringLiteral("name"), p.name);
  o.insert(QStringLiteral("address"), p.address);
  o.insert(QStringLiteral("hub_user_id"), p.hubUserId);
  o.insert(QStringLiteral("user_display_name"), p.userDisplayName);
  o.insert(QStringLiteral("user_role"), p.userRole);
  o.insert(QStringLiteral("device_id"), p.deviceId);
  o.insert(QStringLiteral("credential_ref"), p.credentialRef);
  o.insert(QStringLiteral("pinned_fingerprint"), p.pinnedFingerprint);
  o.insert(QStringLiteral("last_connected"),
           p.lastConnected.isValid() ? p.lastConnected.toUTC().toString(Qt::ISODate) : QString());
  o.insert(QStringLiteral("allow_http"), p.allowHttp);
  QJsonArray accounts;
  for (const HubAccount& a : p.accounts) {
    accounts.append(toJson(a));
  }
  o.insert(QStringLiteral("accounts"), accounts);
  o.insert(QStringLiteral("active_user_id"), p.activeUserId);
  return o;
}

HubProfile fromJson(const QJsonObject& o) {
  HubProfile p;
  p.hubId = o.value(QStringLiteral("hub_id")).toString();
  p.name = o.value(QStringLiteral("name")).toString();
  p.address = o.value(QStringLiteral("address")).toString();
  p.hubUserId = o.value(QStringLiteral("hub_user_id")).toString();
  p.userDisplayName = o.value(QStringLiteral("user_display_name")).toString();
  p.userRole = o.value(QStringLiteral("user_role")).toString();
  p.deviceId = o.value(QStringLiteral("device_id")).toString();
  p.credentialRef = o.value(QStringLiteral("credential_ref")).toString();
  p.pinnedFingerprint = o.value(QStringLiteral("pinned_fingerprint")).toString();
  p.lastConnected = QDateTime::fromString(o.value(QStringLiteral("last_connected")).toString(), Qt::ISODate);
  p.allowHttp = o.value(QStringLiteral("allow_http")).toBool(false);
  if (o.contains(QStringLiteral("accounts"))) {
    for (const QJsonValue& v : o.value(QStringLiteral("accounts")).toArray()) {
      const HubAccount a = accountFromJson(v.toObject());
      if (!a.hubUserId.isEmpty() && accountIndex(p.accounts, a.hubUserId) < 0) {
        p.accounts.append(a);
      }
    }
    p.activeUserId = o.value(QStringLiteral("active_user_id")).toString();
    if (const qsizetype i = accountIndex(p.accounts, p.activeUserId); i >= 0) {
      copyAccountToFlat(p, p.accounts.at(i));  // the flat fields are the active account
    } else {
      p.activeUserId.clear();
    }
  } else if (!p.hubUserId.isEmpty()) {
    // Profile of an older version: the one paired person is the one account.
    p.accounts.append(accountFromFlat(p));
    p.activeUserId = p.hubUserId;
  }
  return p;
}

}  // namespace

namespace {

bool dirIsWritable(const QString& path) {
  if (path.isEmpty() || !QDir().mkpath(path)) {
    return false;
  }
  QTemporaryFile probe(QDir(path).filePath(QStringLiteral(".uzume-write-test-XXXXXX")));
  return probe.open();  // file is deleted on destruction
}

}  // namespace

ProfileStore::BaseDirChoice ProfileStore::chooseBaseDir(const QString& appDir, const QString& appDataDir) {
  // Inside an AppImage (APPIMAGE is set by its runtime) the program directory is the image's mount, which extract-and-run
  // mode makes writable but throws away: never portable there.
  if (!appDir.isEmpty() && qEnvironmentVariableIsEmpty("APPIMAGE")) {
    const QString portable = QDir(appDir).filePath(QStringLiteral("data"));
    if (dirIsWritable(portable)) {
      return {portable, true};
    }
  }
  return {appDataDir, false};
}

// One-time AppData -> portable migration.
// Completion is tracked by a marker file in the new folder, written only after every required
// copy succeeded. The presence of profiles.json etc. says nothing (an interrupted run may have
// copied only some files); a failed or interrupted run is simply retried on the next start.
// Never deletes the source, never overwrites existing files, skips cache/.
// Exception (device.json): if the new folder already has a device.json with a different ID and
// no marker exists, the legacy ID wins only if no profile in the new folder references
// credentials (so no pairing is lost); otherwise the existing one is kept and a warning logged.
// ProfileStore::ProfileStore calls defaultBaseDir() (and thus this function) before load(), so
// no fresh device.json can be created in the new folder before the migration ran.
int ProfileStore::migrateLegacyData(const QString& legacyDir, const QString& newDir) {
  if (legacyDir.isEmpty() || newDir.isEmpty() || QFileInfo(legacyDir).absoluteFilePath() == QFileInfo(newDir).absoluteFilePath() ||
      !QFileInfo(legacyDir).isDir()) {
    return 0;
  }
  const QString marker = QDir(newDir).filePath(QStringLiteral(".migrated-from-appdata"));
  if (QFileInfo::exists(marker)) {
    return 0;
  }
  const QDir src(legacyDir);
  if (!src.exists(QStringLiteral("profiles.json")) && !src.exists(QStringLiteral("device.json")) &&
      !src.exists(QStringLiteral("hubs"))) {
    return 0;
  }
  if (!QDir().mkpath(newDir)) {
    qCWarning(lcProfiles) << "Migration: target folder not creatable";
    return 0;
  }
  int copied = 0;
  bool ok = true;
  QDirIterator it(legacyDir, QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString from = it.next();
    const QString rel = src.relativeFilePath(from);
    if (rel.startsWith(QStringLiteral("cache/"))) {
      continue;  // ROM cache is not migrated (large, re-downloaded)
    }
    const QString to = QDir(newDir).filePath(rel);
    if (QFileInfo::exists(to)) {
      if (rel != QStringLiteral("device.json")) {
        continue;  // never overwrite anything existing
      }
      const QString legacyId = readJsonFile(from).value(QStringLiteral("device_id")).toString();
      const QString curId = readJsonFile(to).value(QStringLiteral("device_id")).toString();
      if (legacyId.isEmpty() || legacyId == curId) {
        continue;
      }
      bool hasCredentials = false;
      const QJsonArray arr = readJsonFile(QDir(newDir).filePath(QStringLiteral("profiles.json")))
                                 .value(QStringLiteral("profiles")).toArray();
      for (const QJsonValue& v : arr) {
        if (!v.toObject().value(QStringLiteral("credential_ref")).toString().isEmpty()) {
          hasCredentials = true;
        }
      }
      if (hasCredentials) {
        qCWarning(lcProfiles) << "Migration: existing device.json differs from legacy one and is in use, kept";
        continue;
      }
      if (!QFile::remove(to)) {  // only the new-folder copy; the source is never touched
        qCWarning(lcProfiles) << "Migration: device.json not replaced";
        ok = false;
        continue;
      }
    }
    if (!QDir().mkpath(QFileInfo(to).absolutePath()) || !QFile::copy(from, to)) {
      qCWarning(lcProfiles) << "Migration: file not copied:" << rel;
      ok = false;
      continue;
    }
    ++copied;
  }
  if (ok) {
    QFile m(marker);
    if (!m.open(QIODevice::WriteOnly)) {
      qCWarning(lcProfiles) << "Migration: completion marker not written, will retry";
    }
  } else {
    qCWarning(lcProfiles) << "Migration incomplete, will retry on next start";
  }
  return copied;
}

QString ProfileStore::defaultBaseDir() {
  const QString dir = fsutil::envPath("UZUME_DATA_DIR");
  if (!dir.isEmpty()) {
    qCInfo(lcProfiles) << "Data directory (UZUME_DATA_DIR):" << dir;
    return dir;
  }
  const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  // bin\ layout (installer): data stays in <install root>\data, not in bin\data.
  const QString appDir = QCoreApplication::instance()
                             ? update::installRootFor(QCoreApplication::applicationDirPath())
                             : QString();
  const BaseDirChoice c = chooseBaseDir(appDir, appData);
  if (!c.portable) {
    qCInfo(lcProfiles) << "Data directory (fallback AppData, program directory not writable or unknown):"
                       << c.path;
    return c.path;
  }
  qCInfo(lcProfiles) << "Data directory (portable):" << c.path;
  const int n = migrateLegacyData(appData, c.path);
  if (n > 0) {
    qCInfo(lcProfiles) << "Data from AppData to" << c.path << "copied (" << n
                       << "files, source unchanged, ROM cache not migrated)";
  }
  return c.path;
}

bool ProfileStore::isValidHubId(const QString& hubId) {
  static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$"));
  return re.match(hubId).hasMatch() && !SaveStore::isWindowsAliasName(hubId);
}

ProfileStore::ProfileStore(const QString& baseDir) : baseDir_(baseDir.isEmpty() ? defaultBaseDir() : baseDir) {
  QDir().mkpath(baseDir_);
  load();
}

QString ProfileStore::profilesFilePath() const { return QDir(baseDir_).filePath(QStringLiteral("profiles.json")); }

QString ProfileStore::hubDir(const QString& hubId) const {
  if (!isValidHubId(hubId)) {
    return {};
  }
  const QString dir = QDir(baseDir_).filePath(QStringLiteral("hubs/") + hubId);
  QDir().mkpath(dir);
  return dir;
}

QString ProfileStore::coreCacheDir() const {
  const QString dir = QDir(baseDir_).filePath(QStringLiteral("cache/cores"));
  QDir().mkpath(dir);
  return dir;
}

QString ProfileStore::toolCacheDir() const {
  const QString dir = QDir(baseDir_).filePath(QStringLiteral("cache/tools"));
  QDir().mkpath(dir);
  return dir;
}

QString ProfileStore::variantCacheDir() const {
  const QString dir = QDir(baseDir_).filePath(QStringLiteral("cache/variants"));
  QDir().mkpath(dir);
  return dir;
}

QString ProfileStore::romCacheDir() const {
  const QString dir = QDir(baseDir_).filePath(QStringLiteral("cache/roms"));
  QDir().mkpath(dir);
  return dir;
}

void ProfileStore::load() {
  const QJsonObject dev = readJsonFile(QDir(baseDir_).filePath(QStringLiteral("device.json")));
  deviceId_ = dev.value(QStringLiteral("device_id")).toString();
  deviceName_ = dev.value(QStringLiteral("device_name")).toString();
  bool devDirty = false;
  if (QUuid::fromString(deviceId_).isNull()) {
    deviceId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    devDirty = true;
  }
  if (deviceName_.isEmpty()) {
    deviceName_ = QSysInfo::machineHostName();
    if (deviceName_.isEmpty()) {
      deviceName_ = QStringLiteral("Uzume");
    }
    devDirty = true;
  }
  if (devDirty && !saveDevice()) {
    qCWarning(lcProfiles) << "device.json could not be written";
  }

  const QJsonObject root = readJsonFile(profilesFilePath());
  const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();
  autoConnectChoice_.reset();
  const bool legacy = !settings.contains(QStringLiteral("connected_once"));  // written before the default existed
  if (settings.value(QStringLiteral("auto_connect")).isBool()) {
    const bool value = settings.value(QStringLiteral("auto_connect")).toBool();
    // A legacy "false" was only the old default, not a choice; every other stored value is explicit.
    if (!legacy || value) autoConnectChoice_ = value;
  }
  connectedOnce_ = settings.value(QStringLiteral("connected_once")).toBool(false);
  lastHubId_ = settings.value(QStringLiteral("last_hub_id")).toString();
  profiles_.clear();
  const QJsonArray arr = root.value(QStringLiteral("profiles")).toArray();
  for (const QJsonValue& v : arr) {
    const HubProfile p = fromJson(v.toObject());
    if (isValidHubId(p.hubId)) {
      profiles_.append(p);
    }
  }
  if (legacy && !profiles_.isEmpty()) {
    connectedOnce_ = true;  // such a file exists only because a connection happened
  }
}

bool ProfileStore::save() const {
  QJsonArray arr;
  for (const HubProfile& p : profiles_) {
    arr.append(toJson(p));
  }
  QJsonObject settings;
  if (autoConnectChoice_.has_value()) {
    settings.insert(QStringLiteral("auto_connect"), *autoConnectChoice_);
  }
  settings.insert(QStringLiteral("connected_once"), connectedOnce_);
  settings.insert(QStringLiteral("last_hub_id"), lastHubId_);
  QJsonObject root;
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("settings"), settings);
  root.insert(QStringLiteral("profiles"), arr);
  return writeJsonFile(profilesFilePath(), root);
}

bool ProfileStore::saveDevice() const {
  QJsonObject o;
  o.insert(QStringLiteral("device_id"), deviceId_);
  o.insert(QStringLiteral("device_name"), deviceName_);
  return writeJsonFile(QDir(baseDir_).filePath(QStringLiteral("device.json")), o);
}

bool ProfileStore::setDeviceName(const QString& name) {
  if (name.trimmed().isEmpty()) {
    return false;
  }
  deviceName_ = name.trimmed();
  return saveDevice();
}

std::optional<HubProfile> ProfileStore::profile(const QString& hubId) const {
  for (const HubProfile& p : profiles_) {
    if (p.hubId == hubId) {
      return p;
    }
  }
  return std::nullopt;
}

std::optional<HubProfile> ProfileStore::profileByAddress(const QString& address) const {
  for (const HubProfile& p : profiles_) {
    if (p.address.compare(address, Qt::CaseInsensitive) == 0) {
      return p;
    }
  }
  return std::nullopt;
}

bool ProfileStore::upsertProfile(const HubProfile& profile) {
  if (!isValidHubId(profile.hubId)) {
    return false;
  }
  for (HubProfile& p : profiles_) {
    if (p.hubId != profile.hubId) {
      continue;
    }
    HubProfile next = profile;
    next.accounts = p.accounts;  // changed through the account functions only
    next.activeUserId = p.activeUserId;
    if (!next.hubUserId.isEmpty()) {
      // The flat fields are the active account.
      HubAccount a = accountFromFlat(next);
      const qsizetype i = accountIndex(next.accounts, a.hubUserId);
      if (i >= 0) {
        const HubAccount& old = next.accounts.at(i);
        if (a.displayName.isEmpty()) a.displayName = old.displayName;  // an older server sends no user
        if (a.role.isEmpty()) a.role = old.role;
        if (a.username.isEmpty()) a.username = old.username;
        if (!a.lastUsed.isValid() || (old.lastUsed.isValid() && old.lastUsed > a.lastUsed)) a.lastUsed = old.lastUsed;
        a.hasPin = old.hasPin;
        a.pinFailures = old.pinFailures;
        next.accounts[i] = a;
      } else {
        next.accounts.append(a);
      }
      next.activeUserId = a.hubUserId;
    } else if (!next.activeUserId.isEmpty()) {
      // Credential lost (revoked, expired): the person stays in the list and needs the password once.
      if (const qsizetype i = accountIndex(next.accounts, next.activeUserId); i >= 0 && next.credentialRef.isEmpty()) {
        next.accounts[i].credentialRef.clear();
      }
      next.activeUserId.clear();
    }
    p = next;
    return save();
  }
  HubProfile added = profile;
  if (!added.hubUserId.isEmpty()) {
    if (const qsizetype i = accountIndex(added.accounts, added.hubUserId); i >= 0) {
      added.accounts[i] = accountFromFlat(added);
    } else {
      added.accounts.append(accountFromFlat(added));
    }
    added.activeUserId = added.hubUserId;
  }
  profiles_.append(added);
  return save();
}

QList<HubAccount> ProfileStore::accounts(const QString& hubId) const {
  const auto p = profile(hubId);
  return p ? p->accounts : QList<HubAccount>();
}

std::optional<HubAccount> ProfileStore::account(const QString& hubId, const QString& userId) const {
  const auto p = profile(hubId);
  if (p) {
    if (const qsizetype i = accountIndex(p->accounts, userId); i >= 0) {
      return p->accounts.at(i);
    }
  }
  return std::nullopt;
}

bool ProfileStore::upsertAccount(const QString& hubId, const HubAccount& account) {
  if (account.hubUserId.isEmpty()) {
    return false;
  }
  for (HubProfile& p : profiles_) {
    if (p.hubId != hubId) {
      continue;
    }
    if (const qsizetype i = accountIndex(p.accounts, account.hubUserId); i >= 0) {
      p.accounts[i] = account;
    } else {
      p.accounts.append(account);
    }
    if (p.activeUserId == account.hubUserId) {
      copyAccountToFlat(p, account);
    }
    return save();
  }
  return false;
}

bool ProfileStore::removeAccount(const QString& hubId, const QString& userId) {
  for (HubProfile& p : profiles_) {
    if (p.hubId != hubId) {
      continue;
    }
    const qsizetype i = accountIndex(p.accounts, userId);
    if (i < 0) {
      return true;
    }
    p.accounts.removeAt(i);
    if (p.activeUserId == userId) {
      p.activeUserId.clear();
      p.hubUserId.clear();
      p.userDisplayName.clear();
      p.userRole.clear();
      p.credentialRef.clear();
      p.deviceId = p.accounts.isEmpty() ? deviceId_ : QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    return save();
  }
  return false;
}

bool ProfileStore::setActiveAccount(const QString& hubId, const QString& userId) {
  for (HubProfile& p : profiles_) {
    if (p.hubId != hubId) {
      continue;
    }
    const qsizetype i = accountIndex(p.accounts, userId);
    if (i < 0) {
      return false;
    }
    copyAccountToFlat(p, p.accounts.at(i));
    return save();
  }
  return false;
}

QString ProfileStore::newAccountDeviceId(const QString& hubId) const {
  const auto p = profile(hubId);
  if (!p || p->accounts.isEmpty()) {
    return deviceId_;
  }
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QStringList ProfileStore::validateHubAddress(const QString& host, const QString& port) {
  QStringList messages;
  const QString h = host.trimmed();
  // Colon outside [...] (an IPv6 literal in brackets may contain colons).
  const bool bracketed = h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']')) && h.size() > 2;
  if (h.isEmpty()) {
    messages.append(QObject::tr("Enter an address"));
  } else if (h.contains(QLatin1String("://"))) {
    messages.append(QObject::tr("Leave out https:// — host name only"));
  } else if (h.contains(QLatin1Char(' ')) || h.contains(QLatin1Char('\t'))) {
    messages.append(QObject::tr("No spaces in the address"));
  } else if (!bracketed && h.contains(QLatin1Char(':'))) {
    messages.append(QObject::tr("Put the port in the port field"));
  } else {
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9.-]+$"));
    static const QRegularExpression v6(QStringLiteral("^\\[[0-9A-Fa-f:.]+\\]$"));
    if (!(bracketed ? v6.match(h).hasMatch() : plain.match(h).hasMatch())) {
      messages.append(QObject::tr("Only letters, digits, dots and hyphens"));
    }
  }
  bool ok = false;
  const int n = port.trimmed().toInt(&ok);
  if (!ok || n < 1 || n > 65535) {
    messages.append(QObject::tr("Port must be a number from 1 to 65535"));
  }
  return messages;
}

void ProfileStore::splitAddress(const QString& address, QString* host, int* port) {
  const QUrl u(address, QUrl::StrictMode);
  QString h = u.host();
  if (h.contains(QLatin1Char(':'))) {
    h = QStringLiteral("[") + h + QStringLiteral("]");
  }
  if (host != nullptr) *host = h;
  if (port != nullptr) *port = u.port() > 0 ? u.port() : 0;
}

bool ProfileStore::updateHubAddress(const QString& hubId, const QString& host, int port) {
  if (!validateHubAddress(host, QString::number(port)).isEmpty()) {
    return false;
  }
  for (HubProfile& p : profiles_) {
    if (p.hubId != hubId) {
      continue;
    }
    const QUrl old(p.address, QUrl::StrictMode);
    QUrl next;
    next.setScheme(old.scheme().isEmpty() ? QStringLiteral("https") : old.scheme());
    QString bare = host.trimmed();
    if (bare.startsWith(QLatin1Char('[')) && bare.endsWith(QLatin1Char(']'))) {
      bare = bare.mid(1, bare.size() - 2);  // QUrl re-adds the brackets for IPv6 hosts
    }
    next.setHost(bare);
    next.setPort(port);
    if (!next.isValid() || next.host().isEmpty()) {
      return false;
    }
    const QString address = next.toString(QUrl::RemoveUserInfo | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    for (const HubProfile& other : std::as_const(profiles_)) {
      if (other.hubId != hubId && other.address == address) {
        return false;  // another saved Hub already lives there
      }
    }
    const QString previous = p.address;
    p.address = address;
    if (!save()) {
      p.address = previous;  // keep the old value until the write succeeds
      return false;
    }
    return true;
  }
  return false;
}

bool ProfileStore::removeProfile(const QString& hubId) {
  for (qsizetype i = 0; i < profiles_.size(); ++i) {
    if (profiles_.at(i).hubId == hubId) {
      profiles_.removeAt(i);
      if (lastHubId_ == hubId) {
        lastHubId_.clear();
      }
      return save();
    }
  }
  return true;
}

bool ProfileStore::setAutoConnect(bool on) {
  autoConnectChoice_ = on;
  return save();
}

bool ProfileStore::markConnectedOnce() {
  if (connectedOnce_) {
    return true;
  }
  connectedOnce_ = true;
  return save();
}

bool ProfileStore::setLastHubId(const QString& hubId) {
  lastHubId_ = hubId;
  return save();
}

}  // namespace uzume
