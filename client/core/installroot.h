#pragma once
// Install layout of the Windows Player (spec 0.3 S6):
//   {app}\Uzume.exe                  tiny launcher (launcher/), starts bin\Uzume.exe
//   {app}\bin\Uzume.exe              the real app (+ DLLs, Qt plugins, qml)
//   {app}\data\                       portable data (ADR 0004), {app}\uzume-app.msi-installed = installed via the MSI

#include <QString>
#include <QStringList>

namespace uzume::update {

inline constexpr const char* kPlayerExeName = "Uzume.exe";

// File name of the launcher in `installRoot` (kPlayerExeName); empty if it does not exist.
QString launcherFileName(const QString& installRoot);

// When the executable's directory is named "bin" and its parent contains the launcher, the install
// root is the parent; otherwise the executable's directory itself (flat/portable layout, Linux, dev builds).
QString installRootFor(const QString& exeDir);

inline constexpr const char* kPlayerMsiMarker = "uzume-app.msi-installed";  // next to the launcher (MSI install)
inline constexpr const char* kHubMsiMarker = "uzume-server.msi-installed";      // next to uzume-server.exe

// The root contains the MSI marker of the Player feature.
bool hasMsiMarker(const QString& installRoot);
// Only an installed copy (MSI marker) may update itself.
bool canSelfUpdate(const QString& installRoot);
// The per-machine MSI installs the Hub as a sibling of the Player directory (<ProgramFiles>\Uzume\{App,Server}).
bool hubInstalledNear(const QString& installRoot);
bool isMsiFile(const QString& path);
// A file can really be created and removed in the directory (a permission check is not enough).
bool directoryWritable(const QString& dir);

struct InstallerCommand {
  QString program;
  QStringList args;
};

// ---- MSI updates (0.9). msiexec arguments are built here (and mirrored in launcher/msiupdate.h, which has no Qt).
enum class MsiScope {
  User,           // per-user Player, install root writable: no elevation
  Machine,        // per-machine Player (UAC)
  MachineWithHub, // per-machine Player + Hub feature installed (UAC), INSTALL_HUB=1
  SetupHub,       // "Set up a server on this PC": ALLUSERS=1 INSTALL_HUB=1 NETWORK_SHARING=0 (UAC)
  ShareHub,       // "Share with others" (0.18 p10): ALLUSERS=1 INSTALL_HUB=1 + the given properties (HUB_PORT, NETWORK_SHARING, IMPORT_SERVER_DATA) (UAC)
};
MsiScope detectMsiScope(bool rootWritable, bool hubInstalled);
QString msiScopeName(MsiScope s);  // user | machine | machine-hub | setup-hub | share-hub
bool msiScopeNeedsElevation(MsiScope s);
// /i <msi> /qn /norestart + properties of the scope.
// `properties` (NAME=VALUE) are appended for ShareHub only.
QStringList msiexecArguments(const QString& msiPath, MsiScope scope, const QStringList& properties = {});
// Copies <root>\Uzume.exe to a fresh folder below tempBase; returns the copy ("" + error on failure).
QString copyLauncherToTemp(const QString& installRoot, const QString& tempBase, QString* error = nullptr);
// <launcherCopy> --apply-msi-update [--sha256 <hex> --size <n>] <msi> <scope> <relaunchExe> [relaunchArgs...]: the copy
// waits until no Player runs, re-verifies size and SHA-256 of the MSI (refuses on mismatch, mandatory for elevated
// scopes), runs msiexec (elevated for non-user scopes), starts relaunchExe with relaunchArgs and removes itself.
// `sha256`/`size` are the values the Player verified against the signed update index (empty / < 0 = not passed).
InstallerCommand msiRelaunchCommand(const QString& launcherCopy, const QString& msiPath, MsiScope scope,
                                    const QString& relaunchExe, const QStringList& relaunchArgs = {},
                                    const QString& sha256 = {}, qint64 size = -1, const QStringList& msiProperties = {});
// Where the per-machine Player lives after "Set up a server on this PC" (<programFiles>\Uzume\App).
QString perMachinePlayerExe(const QString& programFilesDir);

// ---- Linux AppImage updates (0.24). The running image replaces itself: the new file is written next to it, the old one
// stays as <image>.previous, then a short shell watcher starts the new image and restores .previous when it never
// confirms its start.
// The AppImage runtime sets APPIMAGE to the path of the running image (an AppImage runtime variable, not a UZUME_ one,
// hence read directly). Empty when it does not name an existing regular file.
QString appImagePath();
// Empty when `appImage` can update itself; otherwise the reason (not an AppImage / folder not writable).
QString appImageUnsupportedReason(const QString& appImage);
// Arguments for the restarted image: `args` (without the program name) minus the one-shot flags (--smoke-test,
// --setup-local-hub, --mcp) and uzume:// links, which belong to the start that is being replaced.
QStringList appImageRelaunchArguments(const QStringList& args);
inline QString appImagePreviousPath(const QString& appImage) { return appImage + QStringLiteral(".previous"); }
// Exists from the restart until the new image confirms a healthy start (or quits cleanly).
inline QString appImageStartMarker(const QString& appImage) { return appImage + QStringLiteral(".starting"); }
// The new image calls this a few seconds after it started (and on a clean quit): removes the start marker.
void appImageConfirmStart(const QString& appImage);
// Writes `staged` to <dir>/.<name>.part (size and SHA-256 checked while copying when given: sha256 non-empty, size >= 0),
// fsyncs it, makes it 0755, keeps the current image as <image>.previous (hard link, copy as fallback) and renames the
// part file over the image atomically. The running image stays mapped, which is fine on Linux. On failure nothing but
// possibly a stale .previous changed and the part file is removed.
bool replaceAppImage(const QString& staged, const QString& appImage, const QString& sha256, qint64 size, QString* error = nullptr);
// /bin/sh -c <watcher> uzume-update <image> <previous> <marker> <oldPid> [args...]: waits until `oldPid` exited, creates
// the start marker, runs the image and, when the marker is still there after it exited (it never confirmed its start),
// moves .previous back and starts that instead.
InstallerCommand appImageRelaunchCommand(const QString& appImage, qint64 oldPid, const QStringList& args);

}  // namespace uzume::update
