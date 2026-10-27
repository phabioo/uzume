#pragma once
// Update index (release feed, spec 0.3 S2): parsing, validation, selection, protocol compatibility.
// Pure functions without I/O; the signature is checked separately (updatesig.h).

#include <QtGlobal>
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

#include "semver.h"

namespace uzume::update {

inline constexpr const char* kDefaultIndexUrl =
    "https://github.com/phabioo/uzume/releases/download/updates-index/updates-index.json";
inline constexpr const char* kPlatformWindowsX64 = "windows-x64";
inline constexpr const char* kPlatformLinuxX64 = "linux-x64";
inline constexpr const char* kKindMsi = "msi";  // the Windows Player installer (ADR 0021)
inline constexpr const char* kKindAppImage = "appimage";  // the Linux Player AppImage (self-replacing file)
inline constexpr const char* kKindZip = "zip";

// Which Player artifact a build updates to. Windows: windows-x64 + msi. Linux x86-64: linux-x64 + appimage. Any other
// host keeps the Windows pair (nothing installable there; the release is only shown).
enum class PlayerOs { Windows, Linux, Other };
constexpr PlayerOs hostPlayerOs() {
#if defined(Q_OS_WIN)
  return PlayerOs::Windows;
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
  return PlayerOs::Linux;
#else
  return PlayerOs::Other;
#endif
}
constexpr const char* playerPlatformFor(PlayerOs os) { return os == PlayerOs::Linux ? kPlatformLinuxX64 : kPlatformWindowsX64; }
constexpr const char* playerKindFor(PlayerOs os) { return os == PlayerOs::Linux ? kKindAppImage : kKindMsi; }
constexpr const char* playerPlatform() { return playerPlatformFor(hostPlayerOs()); }
constexpr const char* playerKind() { return playerKindFor(hostPlayerOs()); }

inline constexpr int kMaxNotesBytes = 4000;

struct Artifact {
  QString platform;
  QString kind;
  QString name;
  qint64 size = 0;
  QString sha256;  // 64 lowercase hex
  QString url;
};

struct Release {
  QString product;  // hub | player
  QString channel;  // beta | stable
  QString version;
  SemVer semver;
  QString commit;
  QString publishedAt;
  QString notesUrl;
  QString notes;  // optional short plain-text "What's new" (<= kMaxNotesBytes UTF-8, no control chars but \n \t); empty if absent/invalid
  int protocolVersion = 0;
  int minProtocolVersion = 0;
  // The Player MSI of this release installs over a newer version (MajorUpgrade AllowDowngrades). Only such a release
  // is offered as a rollback target: an older MSI refuses to install over a newer one. `schema_version` (Hub) is ignored.
  bool msiDowngrade = false;
  QList<Artifact> artifacts;

  std::optional<Artifact> artifact(const QString& platform, const QString& kind) const;
  QJsonObject toJson() const;
};

struct Index {
  QString generatedAt;
  QString expiresAt;  // signed end of validity (RFC 3339); empty in indexes of earlier releases (accepted, transition)
  QList<Release> releases;
  QStringList skipped;  // reasons of skipped (invalid) release entries
};

struct ParseResult {
  bool ok = false;
  QString error;  // set when !ok (unknown schema, duplicate, malformed JSON)
  Index index;
};

// Empty when the index may still be used at `now`; otherwise the reason (expires_at passed, PE-18). An index without
// expires_at passes during the transition of the ADR 0016 amendment 2026-10-19.
QString checkIndexFresh(const Index& index, const QDateTime& now);

// allowFileUrls: artifact URLs may be file:// (only when the index itself came from file://, tests).
ParseResult parseIndex(const QByteArray& json, bool allowFileUrls = false);

// Notes of the release `product`/`version` (e.g. the installed version after an update, for a "What's new" screen).
// Empty when the index has no such release or it carries no (valid) notes. If the version exists in several
// channels, the first release with notes wins.
QString releaseNotes(const Index& index, const QString& product, const QString& version);

// Update channel as the user sees it.
enum class Channel { Off, Stable, Beta };
QString channelName(Channel c);  // "off" | "stable" | "beta"
std::optional<Channel> parseChannel(const QString& name);  // accepts "off", "stable", "beta" ("test" = alias of beta)
// Compiled default: "dev" (or anything unknown) = Off.
Channel channelFromCompiled(const QString& compiled);

// Protocol compatibility (S4, Player view): the Hub values come from the last handshake.
struct HubProtocol {
  int protocolVersion = 0;
  int minProtocolVersion = 0;
};
bool protocolCompatible(const Release& candidate, const std::optional<HubProtocol>& hub);

struct SelectionInput {
  QString product = QStringLiteral("player");
  Channel channel = Channel::Off;
  QString currentVersion;
  QString platform = QLatin1String(playerPlatform());
  QString kind = QLatin1String(playerKind());
  std::optional<HubProtocol> hub;
};

struct Selection {
  enum class Status {
    Disabled,      // channel off or current version is not SemVer
    UpToDate,      // nothing newer
    Available,     // `release` is the highest compatible candidate
    Incompatible,  // newer releases exist, but none is compatible with the current Hub
  };
  Status status = Status::Disabled;
  QString reason;  // Disabled: why
  Release release;
  Artifact artifact;

  static QString statusName(Status s);  // disabled | up_to_date | available | incompatible
};

// Own product, channel per S1 (beta sees beta + stable; stable only stable), an artifact for platform/kind,
// SemVer strictly greater than the running version, protocol-compatible; highest wins. Never a downgrade.
Selection selectRelease(const Index& index, const SelectionInput& in);

// {"status","current_version","channel","release":{...},"artifact":{...},"skipped":[...]} for the CLI.
QJsonObject selectionToJson(const Selection& sel, const SelectionInput& in, const QStringList& skipped);

}  // namespace uzume::update
