#include "localpin.h"

#include <QCryptographicHash>
#include <algorithm>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QRegularExpression>

#include "credentialstore.h"
#include "profilestore.h"

namespace uzume {

namespace {

constexpr int kIterations = 150000;
constexpr int kSaltBytes = 16;
constexpr int kHashBytes = 32;

QByteArray randomBytes(int n) {
  QByteArray out(n, Qt::Uninitialized);
  QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(out.data()), n / 4);
  return out;
}

// The device ID is mixed into the salt so a hash copied to another account or PC does not verify there.
QByteArray derive(const QString& pin, const QByteArray& salt, const QString& deviceId, int iterations) {
  return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, pin.toUtf8(), salt + deviceId.toUtf8(), iterations,
                                            kHashBytes);
}

// Constant-time comparison of two hashes of the same length.
bool sameBytes(const QByteArray& a, const QByteArray& b) {
  if (a.size() != b.size()) {
    return false;
  }
  quint8 diff = 0;
  for (qsizetype i = 0; i < a.size(); ++i) {
    diff |= static_cast<quint8>(a.at(i)) ^ static_cast<quint8>(b.at(i));
  }
  return diff == 0;
}

QString encode(const QByteArray& salt, const QByteArray& hash, int iterations) {
  return QStringLiteral("v1:%1:%2:%3").arg(iterations).arg(QString::fromLatin1(salt.toHex()), QString::fromLatin1(hash.toHex()));
}

struct Stored {
  int iterations = 0;
  QByteArray salt;
  QByteArray hash;
  bool valid = false;
};

Stored decode(const QString& text) {
  Stored s;
  const QStringList parts = text.split(QLatin1Char(':'));
  if (parts.size() != 4 || parts.at(0) != QLatin1String("v1")) {
    return s;
  }
  bool ok = false;
  s.iterations = parts.at(1).toInt(&ok);
  s.salt = QByteArray::fromHex(parts.at(2).toLatin1());
  s.hash = QByteArray::fromHex(parts.at(3).toLatin1());
  s.valid = ok && s.iterations >= 100000 && s.iterations <= 10000000 && s.salt.size() >= 8 && s.hash.size() == kHashBytes;
  return s;
}

}  // namespace

QString pinTarget(const QString& hubId, const QString& deviceId) {
  return QStringLiteral("Uzume/pin/%1/%2").arg(hubId, deviceId);
}

LocalPin::LocalPin(ProfileStore* profiles, CredentialStore* credentials) : profiles_(profiles), credentials_(credentials) {}

bool LocalPin::isValidPin(const QString& pin) {
  static const QRegularExpression re(QStringLiteral("^[0-9]{%1}$").arg(kLocalPinDigits));
  return re.match(pin).hasMatch();
}

bool LocalPin::hasPin(const QString& hubId, const QString& userId) const {
  const auto a = profiles_->account(hubId, userId);
  return a && a->hasPin;
}

bool LocalPin::isLocked(const QString& hubId, const QString& userId) const {
  const auto a = profiles_->account(hubId, userId);
  return a && a->hasPin && a->pinFailures >= kLocalPinMaxTries;
}

int LocalPin::triesLeft(const QString& hubId, const QString& userId) const {
  const auto a = profiles_->account(hubId, userId);
  return a && a->hasPin ? std::max(0, kLocalPinMaxTries - a->pinFailures) : 0;
}

bool LocalPin::setPin(const QString& hubId, const QString& userId, const QString& pin) {
  auto a = profiles_->account(hubId, userId);
  if (!a || !isValidPin(pin) || a->deviceId.isEmpty()) {
    return false;
  }
  const QByteArray salt = randomBytes(kSaltBytes);
  const QByteArray hash = derive(pin, salt, a->deviceId, kIterations);
  if (hash.isEmpty() || !credentials_->write(pinTarget(hubId, a->deviceId), encode(salt, hash, kIterations))) {
    return false;
  }
  a->hasPin = true;
  a->pinFailures = 0;
  return profiles_->upsertAccount(hubId, *a);
}

bool LocalPin::removePin(const QString& hubId, const QString& userId) {
  auto a = profiles_->account(hubId, userId);
  if (!a) {
    return false;
  }
  credentials_->remove(pinTarget(hubId, a->deviceId));
  a->hasPin = false;
  a->pinFailures = 0;
  return profiles_->upsertAccount(hubId, *a);
}

LocalPin::Result LocalPin::verify(const QString& hubId, const QString& userId, const QString& pin) {
  Result r;
  auto a = profiles_->account(hubId, userId);
  if (!a || !a->hasPin) {
    return r;
  }
  if (a->pinFailures >= kLocalPinMaxTries) {
    r.status = Status::Locked;
    return r;
  }
  const CredentialRead stored = credentials_->readChecked(pinTarget(hubId, a->deviceId));
  if (stored.status == CredentialRead::Status::Error) {
    // A locked or dismissed keyring is not "no hash": keep the tries and the lock as they are.
    r.status = Status::Unavailable;
    r.triesLeft = std::max(0, kLocalPinMaxTries - a->pinFailures);
    return r;
  }
  const Stored s = stored.status == CredentialRead::Status::Ok ? decode(stored.secret) : Stored();
  if (!s.valid) {
    // The hash is gone (credential store reset): never let anyone in with a PIN that cannot be checked.
    r.status = Status::Locked;
    return r;
  }
  if (isValidPin(pin) && sameBytes(derive(pin, s.salt, a->deviceId, s.iterations), s.hash)) {
    if (a->pinFailures != 0) {
      a->pinFailures = 0;
      profiles_->upsertAccount(hubId, *a);
    }
    r.status = Status::Ok;
    r.triesLeft = kLocalPinMaxTries;
    return r;
  }
  a->pinFailures += 1;
  profiles_->upsertAccount(hubId, *a);
  r.triesLeft = std::max(0, kLocalPinMaxTries - a->pinFailures);
  r.status = r.triesLeft == 0 ? Status::Locked : Status::Wrong;
  return r;
}

bool LocalPin::clearLock(const QString& hubId, const QString& userId) {
  auto a = profiles_->account(hubId, userId);
  if (!a) {
    return false;
  }
  if (a->pinFailures == 0) {
    return true;
  }
  a->pinFailures = 0;
  return profiles_->upsertAccount(hubId, *a);
}

void LocalPin::removeStored(CredentialStore& credentials, const QString& hubId, const QString& deviceId) {
  if (!deviceId.isEmpty()) {
    credentials.remove(pinTarget(hubId, deviceId));
  }
}

}  // namespace uzume
