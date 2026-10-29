#pragma once
// Player updater (spec 0.3 S6): check the signed update index, download + verify + stage the installer,
// apply it (Windows installer install only). Never applies during a running game.

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

#include "installroot.h"
#include "playersettings.h"
#include "updateindex.h"
#include "updaterollback.h"

class QNetworkReply;

namespace uzume::update {

// Index fetch result (also used by the CLI): raw bytes verified against the trusted keys and parsed.
struct FetchedIndex {
  bool ok = false;
  QString error;
  QByteArray indexBytes;
  QByteArray sigBytes;
  Index index;
};

// A staged installer that passed the full verification (signature of the stored index, release entry, size,
// SHA-256, version strictly newer than the running one).
struct StagedUpdate {
  QString version;
  QString installerPath;
  QString notesUrl;
  QString sha256;   // of the installer, from the signed index (the launcher re-checks it before msiexec)
  qint64 size = -1;
};
// Looks into <baseDir>/cache/updates/<version>/ (index.json, index.json.sig, staged.json, installer) and returns
// the highest verified staged update; stale or invalid staging directories are removed.
// `platform`/`kind` empty: the artifact of this build (playerPlatform()/playerKind()).
std::optional<StagedUpdate> loadVerifiedStaged(const QString& baseDir, const QString& currentVersion,
                                               const QList<QByteArray>& trustedKeys, bool allowFileUrls,
                                               const QString& platform = {}, const QString& kind = {});
QString stagingRoot(const QString& baseDir);  // <baseDir>/cache/updates
// Rollback installers are staged apart (<baseDir>/cache/rollback/<version>/): the normal staging only keeps newer versions.
QString rollbackStagingRoot(const QString& baseDir);
// The staged installer of the rollback to `version`, only when everything still holds: signature of the stored index,
// the index is fresh (an old signed installer must not come back through a stale index), the release is a strictly
// older player release that speaks `protocolVersion` (0 = not checked) and whose MSI allows downgrades, size and SHA-256.
std::optional<StagedUpdate> loadVerifiedRollback(const QString& baseDir, const QString& version, const QString& currentVersion,
                                                 const QList<QByteArray>& trustedKeys, bool allowFileUrls, int protocolVersion,
                                                 const QDateTime& now, const QString& platform = {}, const QString& kind = {});

// Verifies size and SHA-256 of a file against an artifact.
bool verifyArtifactFile(const QString& path, const Artifact& artifact, QString* error = nullptr);

class UpdateManager : public QObject {
  Q_OBJECT
 public:
  enum class State {
    Idle,         // nothing checked yet
    Disabled,     // channel off (dev build without explicit channel)
    Checking,
    UpToDate,
    Available,    // newer release found; not downloaded
    Incompatible, // newer release needs a newer Hub
    Downloading,
    Ready,        // verified installer staged
    Applying,
    Error,
  };
  Q_ENUM(State)

  struct Config {
    QString baseDir;          // data directory
    QString indexUrl;         // empty: env UZUME_APP_UPDATE_INDEX_URL, else the default URL
    QString currentVersion;
    QString compiledChannel;  // stable | beta | dev
    QList<QByteArray> trustedKeys;
    QString installRoot;      // install root (installRootFor), empty = unknown
    bool forceApplySupport = false;  // tests: behave like an installed Player (Windows MSI or Linux AppImage)
    QString platform;                // tests: artifact platform/kind to follow; empty = this build's (playerPlatform()/playerKind())
    QString kind;
    int protocolVersion = 0;         // this Player's Hub protocol (rollback targets must speak it); 0 = not checked
  };

  UpdateManager(const Config& config, PlayerSettings* settings, QObject* parent = nullptr);

  // Providers (optional): Hub protocol of the current Hub (S4), "a game/Session is running" (blocks apply).
  void setHubProtocolProvider(std::function<std::optional<HubProtocol>()> f) { hubProtocol_ = std::move(f); }
  void setBusyProvider(std::function<bool()> f) { busy_ = std::move(f); }
  // Starts the installer (default: QProcess::startDetached). Tests inject a fake.
  void setLauncher(std::function<bool(const QString& program, const QStringList& args)> f) { launcher_ = std::move(f); }

  Channel effectiveChannel() const;
  // Effective channel: explicit setting > resolved default (beta builds only) > compiled channel.
  bool channelIsOverridden() const { return !settings_->updateChannel().isEmpty(); }
  // Automatic install is only effective on the beta channel (stable always asks first).
  bool autoInstallSetting() const;
  // Not while paused after going back, and never for the version the user went back from.
  bool autoInstallEffective() const {
    return effectiveChannel() == Channel::Beta && autoInstallSetting() && !updatesPaused() && !selectionSkipped();
  }
  // Only an MSI install (Windows) or a Linux AppImage in a writable folder updates itself; zip/dev builds just show the release.
  bool applySupported() const;
  QString applyUnsupportedReason() const;

  State state() const { return state_; }
  QString statusText() const { return status_; }
  QString availableVersion() const { return selection_.release.version; }
  QString notesUrl() const { return selection_.release.notesUrl; }
  // Notes of the running version from the last verified index ("What's new"); empty when the index has none or was not read yet.
  QString installedNotes() const { return lastIndex_.ok ? releaseNotes(lastIndex_.index, QStringLiteral("player"), config_.currentVersion) : QString(); }
  double progress() const { return progress_; }
  QDateTime lastCheck() const { return lastCheck_; }
  const QString& currentVersion() const { return config_.currentVersion; }
  QString indexUrl() const { return indexUrl_; }
  bool busy() const { return busy_ && busy_(); }

  // Timers: first check 10 s after start, then every 1 h (beta) / 24 h (stable).
  void start(int firstDelayMs = 10000);
  void checkNow();
  // "Install and restart" / "Restart to update": downloads when needed, then applies. Refused during a game.
  void installNow();
  // Channel or automatic-install setting changed: re-evaluate and check right away.
  void settingsChanged();
  QString lastError() const { return lastError_; }
  // Beta channel + automatic: a verified staged installer from the last run is applied right now. Returns true
  // when the installer was started and the Player has to quit (never during a game). Used before the main window.
  bool applyStagedAtStart();

  // ---- Update rollback (ADR 0029 D4)
  // Automatic updates are paused after going back, until resumeUpdates() (or until the Player has reached the version it
  // went back from, by "Update to").
  bool updatesPaused() const;
  Pause pause() const;
  // The version the user went back from (empty when none or when the Player has reached it); listed as "skipped".
  QString skippedVersion() const;
  // The newest release found is the skipped one: it is listed as available, but not installed automatically.
  bool selectionSkipped() const;
  // Older releases of the last verified index that may be offered (empty until a check succeeded).
  QList<RollbackCandidate> rollbackOptions() const;
  bool rollbackListLoaded() const { return lastIndex_.ok; }
  // Downloads the older installer, verifies it, pauses automatic updates and starts the installer. Refused while a game or
  // Session runs and when this copy cannot update itself. Nothing changes when it is refused.
  void rollbackTo(const QString& version);
  // Ends the pause (automatic updates as before the rollback) and checks right away. The skipped version stays skipped.
  void resumeUpdates();

  // Fetches and verifies an index (used by check and by the CLI). indexUrl may be https:// or file://.
  static void fetchIndex(QNetworkAccessManager* nam, const QUrl& indexUrl, const QList<QByteArray>& trustedKeys,
                         QObject* context, std::function<void(const FetchedIndex&)> done);

 signals:
  void changed();
  void quitRequested();  // installer started: the Player has to exit now
  // An update that was downloaded, checked or installed failed (not a failed check for updates: that is mostly "offline"); `reason`
  // is the technical text (ADR 0035 error events).
  void failed(const QString& reason);

 private:
  void resolveDefaultChannel(const Index& index);
  void setState(State s, const QString& text);
  void onIndex(const FetchedIndex& fi);
  void startDownload(bool applyAfter);
  void beginDownload(const Release& release, const Artifact& artifact, const QString& dir, bool applyAfter, bool rollback);
  void finishDownload(const QString& partPath);
  void apply(const StagedUpdate& staged, bool background = true);  // background: AppImage file work off the GUI thread (#325)
  void applyAppImage(const StagedUpdate& staged, bool background);
  void finishAppImageApply(const StagedUpdate& staged, const QString& image, bool replaced, const QString& why);
  bool appImageKind() const { return kind_ == QLatin1String(kKindAppImage); }
  void applyRollback(const StagedUpdate& staged);
  void restoreRollbackSettings();  // puts back what applyRollback changed (once); no-op when nothing is pending
  void recordRunning();
  void settle();
  void endPause(const Pause& p);
  bool hasAttemptRecord(const StagedUpdate& staged) const;
  void recordAttempt(const StagedUpdate& staged);
  bool allowFileUrls() const;
  void scheduleNext();

  Config config_;
  PlayerSettings* settings_;
  QString indexUrl_;
  QString platform_;  // artifact followed: config or this build's
  QString kind_;
  QNetworkAccessManager nam_;
  QTimer timer_;
  State state_ = State::Idle;
  QString status_;
  Selection selection_;
  FetchedIndex lastIndex_;
  double progress_ = 0;
  QDateTime lastCheck_;
  QString lastError_;
  bool quietError_ = false;  // the Error state of a failed check is not an update failure
  bool checkRunning_ = false;
  bool applyAfterDownload_ = false;
  Release dlRelease_;        // what the running download fetches (the selected release, or a rollback target)
  Artifact dlArtifact_;
  bool dlRollback_ = false;
  struct RollbackRestore {  // settings from before applyRollback; restored when the install fails, also after an async AppImage failure
    QJsonObject pause;
    QString skip;
    std::optional<bool> autoInstall;
  };
  std::optional<RollbackRestore> rollbackRestore_;
  QPointer<QNetworkReply> download_;
  std::function<std::optional<HubProtocol>()> hubProtocol_;
  std::function<bool()> busy_;
  std::function<bool(const QString&, const QStringList&)> launcher_;
};

}  // namespace uzume::update
