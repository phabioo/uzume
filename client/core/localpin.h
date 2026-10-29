#pragma once

#include <QString>

namespace uzume {

class CredentialStore;
class ProfileStore;

// Wrong PINs in a row after which the PIN is locked for that person on this PC.
inline constexpr int kLocalPinMaxTries = 3;
inline constexpr int kLocalPinDigits = 4;

// Credential store target of the PIN hash of one account: "Uzume/pin/<hub_id>/<device_id>".
QString pinTarget(const QString& hubId, const QString& deviceId);

// PIN for switching people on this PC (docs/design/uzume/accounts-and-server.md 8d-2, 8d-3). The PIN is four digits, never
// sent to a server and not a security boundary against someone with access to the PC's user session: it keeps the next
// person at the keyboard out of another person's account. The hash (PBKDF2-HMAC-SHA256 with a random salt that the
// account's device ID is mixed into) is stored in the CredentialStore; profiles.json only knows `hasPin` and `pinFailures`
// of the account (ProfileStore::upsertAccount).
class LocalPin {
 public:
  LocalPin(ProfileStore* profiles, CredentialStore* credentials);

  enum class Status {
    Ok,      // right PIN; the count of wrong tries is reset
    Wrong,   // wrong PIN, triesLeft tries remain
    Locked,  // locked: three wrong PINs in a row (or the stored hash is gone); a password sign-in unlocks
    NoPin,   // the account has no PIN (or does not exist)
    Unavailable,  // the credential store could not be read (locked keyring, dismissed prompt); nothing was counted or changed
  };
  struct Result {
    Status status = Status::NoPin;
    int triesLeft = 0;
    bool ok() const { return status == Status::Ok; }
  };

  static bool isValidPin(const QString& pin);  // exactly four digits 0-9

  bool hasPin(const QString& hubId, const QString& userId) const;
  bool isLocked(const QString& hubId, const QString& userId) const;
  int triesLeft(const QString& hubId, const QString& userId) const;

  // Sets or replaces the PIN of an existing account (clears a lock). False for an invalid PIN, an unknown account or when
  // the hash cannot be stored.
  bool setPin(const QString& hubId, const QString& userId, const QString& pin);
  bool removePin(const QString& hubId, const QString& userId);
  Result verify(const QString& hubId, const QString& userId, const QString& pin);
  // After a successful password sign-in or invite: wrong tries back to zero (the PIN stays).
  bool clearLock(const QString& hubId, const QString& userId);

  // Deletes the stored hash of a device (account removed from this PC). Does not touch profiles.json.
  static void removeStored(CredentialStore& credentials, const QString& hubId, const QString& deviceId);

 private:
  ProfileStore* profiles_;
  CredentialStore* credentials_;
};

}  // namespace uzume
