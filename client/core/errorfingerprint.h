#pragma once
// Error fingerprints (ADR 0035 D1): one problem keeps one id across versions, devices and runs. Identical to the Go
// implementation on the server (shared vectors: protocol/fixtures/error-fingerprint.json).
//
//   normalize(message)  lowercase; URLs -> <url>; Windows and Unix paths -> <path>; IPv4/IPv6 -> <ip>; hex runs of 6+
//                       chars (UUIDs, 0x addresses) -> <hex>; digit runs -> <n>; whitespace collapsed to one space,
//                       trimmed, cut to 300 bytes at a UTF-8 boundary
//   fingerprint         "uz-" + first 12 hex digits of SHA-256(kind + "\n" + component + "\n" + normalized)

#include <QString>

namespace uzume::errors {

inline constexpr int kMaxNormalizedBytes = 300;

// Kinds the app records (`server_error` is the server's own).
inline constexpr const char* kKindAppCrash = "app_crash";
inline constexpr const char* kKindCoreFailure = "core_failure";
inline constexpr const char* kKindPluginError = "plugin_error";
inline constexpr const char* kKindSyncFailure = "sync_failure";
inline constexpr const char* kKindUpdateFailure = "update_failure";

QString normalizeMessage(const QString& message);
QString fingerprint(const QString& kind, const QString& component, const QString& message);

bool validKind(const QString& kind);
// ^(app|server|core:[a-z0-9][a-z0-9_-]{0,63}|plugin:[A-Za-z0-9._-]{1,80})$
bool validComponent(const QString& component);
bool validFingerprint(const QString& fp);  // ^uz-[0-9a-f]{12}$

}  // namespace uzume::errors
