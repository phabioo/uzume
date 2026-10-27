#pragma once
// .uzplugin files: a zip with manifest.json, the entry file, assets and an optional SIGNATURE.json (0.11 package 3
// contract, section 1; docs/architecture/12-plugins.md 16.10). Reading validates the archive completely and in memory
// before anything is written to disk; the Hub validates the same rules.
//
// Limits: file <= 32 MiB, sum of uncompressed sizes <= 64 MiB, <= 2000 entries, names are relative paths with '/'
// (no leading '/', no '\', no drive letter, no '.' or '..' segment, no empty segment, <= 255 bytes, no duplicates
// ignoring case, no symlinks). Directory entries (trailing '/') are name-checked like files, then ignored. Same rules as
// server/internal/pluginpkg (also ':' anywhere, control characters, invalid UTF-8, a file that is also the folder of
// another file).
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

#include "manifest.h"

namespace uzume::plugins {

inline constexpr qint64 kUzPluginMaxFileBytes = 32ll * 1024 * 1024;
inline constexpr qint64 kUzPluginMaxUnpackedBytes = 64ll * 1024 * 1024;
inline constexpr int kUzPluginMaxEntries = 2000;
inline constexpr int kUzPluginMaxNameBytes = 255;
inline constexpr const char* kUzPluginSignatureFile = "SIGNATURE.json";
inline constexpr const char* kUzPluginSignaturePrefix = "uzume-plugin-v1\n";

enum class SignatureState {
  None,     // no SIGNATURE.json
  Valid,    // signed by a trusted key, every file hash matches: the plugin is "official"
  Invalid,  // SIGNATURE.json present but broken, wrong key or the files do not match: never official
};

struct UzPluginPackage {
  Manifest manifest;
  QMap<QString, QByteArray> files;  // every non-directory entry (including manifest.json and SIGNATURE.json)
  SignatureState signature = SignatureState::None;
  QString signatureError;           // why the signature is Invalid
  QString sha256;                   // lowercase hex of the whole .uzplugin file
  qint64 size = 0;                  // size of the .uzplugin file
  bool official() const { return signature == SignatureState::Valid; }
};

struct UzPluginResult {
  bool ok = false;
  UzPluginPackage package;
  QString error;  // clear English message when !ok
};

// Parses and validates `file` (the bytes of a .uzplugin). A broken signature does not fail the read: it is reported in
// package.signature (Invalid), never as Valid. `trustedKeys` = raw 32-byte Ed25519 public keys.
UzPluginResult readUzPlugin(const QByteArray& file, const QList<QByteArray>& trustedKeys);

// Signature part on its own. `files` = all non-directory entries. Returns Valid / Invalid / None, *error says why Invalid.
SignatureState verifyPluginSignature(const QMap<QString, QByteArray>& files, const QList<QByteArray>& trustedKeys, QString* error = nullptr);
// The signed message: "uzume-plugin-v1\n" then per file sorted by path (UTF-8 byte order) "<sha256>  <path>\n".
QByteArray pluginSignatureMessage(const QMap<QString, QString>& sha256ByPath);

// Writes every file of `pkg` below `destDir` (created; must not contain anything yet). false + *error on failure.
bool extractUzPlugin(const UzPluginPackage& pkg, const QString& destDir, QString* error);

}  // namespace uzume::plugins
