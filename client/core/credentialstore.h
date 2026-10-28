#pragma once

#include <QString>
#include <QtGlobal>
#include <memory>
#include <optional>

#include <QHash>
#include <QMutex>

namespace uzume {

// Result of CredentialStore::readChecked: `Error` means the backend failed (locked, prompt dismissed or timed out,
// service gone), which is not the same as `NotFound`; callers must not act as if nothing is stored.
struct CredentialRead {
  enum class Status { Ok, NotFound, Error };
  Status status = Status::NotFound;
  QString secret;  // only with Status::Ok
};

// Storage for the long-lived device credential. Never put secrets in files or logs.
class CredentialStore {
 public:
  virtual ~CredentialStore() = default;
  virtual bool write(const QString& target, const QString& secret) = 0;
  virtual std::optional<QString> read(const QString& target) const = 0;
  // Like read(), but tells a backend error from an absent entry. Stores that cannot fail keep the default.
  virtual CredentialRead readChecked(const QString& target) const;
  // true even if the entry did not exist.
  virtual bool remove(const QString& target) = 0;
  virtual QString backendName() const = 0;
  virtual bool isPersistent() const = 0;
};

// Target/reference in the store: "Uzume/<hub_id>/<device_id>".
QString credentialTarget(const QString& hubId, const QString& deviceId);

// Memory only (macOS, and Linux without a Secret Service). Warns once in the log.
class MemoryCredentialStore : public CredentialStore {
 public:
  MemoryCredentialStore();
  bool write(const QString& target, const QString& secret) override;
  std::optional<QString> read(const QString& target) const override;
  bool remove(const QString& target) override;
  QString backendName() const override { return QStringLiteral("memory"); }
  bool isPersistent() const override { return false; }

 private:
  mutable QMutex mutex_;
  QHash<QString, QString> items_;
};

#ifdef Q_OS_WIN
// Windows Credential Manager (CredWriteW/CredReadW/CredDeleteW).
class WindowsCredentialStore final : public CredentialStore {
 public:
  bool write(const QString& target, const QString& secret) override;
  std::optional<QString> read(const QString& target) const override;
  bool remove(const QString& target) override;
  QString backendName() const override { return QStringLiteral("windows-credential-manager"); }
  bool isPersistent() const override { return true; }
};
#endif

#ifdef Q_OS_LINUX
// freedesktop Secret Service over D-Bus (GNOME Keyring, KWallet, KeePassXC); no libsecret. Items live in the
// "default" collection with the attributes {application=uzume, uzume-target=<target>}. Thread-safe: every operation
// uses its own private session-bus connection and Secret Service session, so there is no shared mutable state.
// Unlock and collection-creation prompts are awaited for at most `promptTimeoutMs` (never forever); a dismissed or
// timed-out prompt fails the operation.
class SecretServiceCredentialStore final : public CredentialStore {
 public:
  explicit SecretServiceCredentialStore(int promptTimeoutMs = 60000) : promptTimeoutMs_(promptTimeoutMs) {}
  bool write(const QString& target, const QString& secret) override;
  std::optional<QString> read(const QString& target) const override;
  CredentialRead readChecked(const QString& target) const override;
  bool remove(const QString& target) override;
  QString backendName() const override { return QStringLiteral("secret-service"); }
  bool isPersistent() const override { return true; }
  // The org.freedesktop.secrets service is owned or activatable on the session bus and answers a cheap call
  // (activation included); a merely registered name that cannot start does not count.
  static bool isAvailable();

 private:
  int promptTimeoutMs_;
};
#endif

// `ref` is a stored credential reference (see credentialTarget). An empty reference reads nothing.
std::optional<QString> readCredential(CredentialStore& store, const QString& ref);
// Same with the error kept apart; an empty reference is NotFound.
CredentialRead readCredentialChecked(CredentialStore& store, const QString& ref);
bool removeCredential(CredentialStore& store, const QString& ref);

std::unique_ptr<CredentialStore> createDefaultCredentialStore();

}  // namespace uzume
