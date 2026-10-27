#include "updater.h"
#include "envvar.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSaveFile>
#include <QUrl>
#include <algorithm>

#include "updatesig.h"

namespace uzume::update {

Q_LOGGING_CATEGORY(lcUpdate, "uzume.update")

namespace {

constexpr qint64 kMaxIndexBytes = 2 * 1024 * 1024;
constexpr qint64 kMaxSigBytes = 4096;
constexpr qint64 kMaxInstallerBytes = Q_INT64_C(2) * 1024 * 1024 * 1024;

QNetworkRequest makeRequest(const QUrl& url) {
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  req.setTransferTimeout(30000);
  req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Uzume/") + QCoreApplication::applicationVersion());
  return req;
}

bool writeFile(const QString& path, const QByteArray& data) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  f.write(data);
  return f.commit();
}

QByteArray readFile(const QString& path, qint64 max = kMaxIndexBytes) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly) || f.size() > max) return {};
  return f.readAll();
}

// GET into memory with a size cap; calls done(ok, bytes, error) once.
void getBytes(QNetworkAccessManager* nam, const QUrl& url, qint64 maxBytes, QObject* context,
              std::function<void(bool, const QByteArray&, const QString&)> done) {
  QNetworkReply* reply = nam->get(makeRequest(url));
  QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply, maxBytes](qint64 got, qint64) {
    if (got > maxBytes) reply->abort();
  });
  QObject::connect(reply, &QNetworkReply::finished, context, [reply, maxBytes, done]() {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
      done(false, {}, reply->error() == QNetworkReply::OperationCanceledError
                          ? QStringLiteral("response too large") : reply->errorString());
      return;
    }
    const QByteArray data = reply->readAll();
    if (data.size() > maxBytes) {
      done(false, {}, QStringLiteral("response too large"));
      return;
    }
    done(true, data, {});
  });
}

}  // namespace

QString stagingRoot(const QString& baseDir) { return QDir(baseDir).filePath(QStringLiteral("cache/updates")); }
QString rollbackStagingRoot(const QString& baseDir) { return QDir(baseDir).filePath(QStringLiteral("cache/rollback")); }

bool verifyArtifactFile(const QString& path, const Artifact& artifact, QString* error) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (error != nullptr) *error = QStringLiteral("installer file is missing");
    return false;
  }
  if (f.size() != artifact.size) {
    if (error != nullptr) *error = QStringLiteral("installer size does not match the signed index");
    return false;
  }
  QCryptographicHash h(QCryptographicHash::Sha256);
  if (!h.addData(&f)) {
    if (error != nullptr) *error = QStringLiteral("installer could not be read");
    return false;
  }
  if (QString::fromLatin1(h.result().toHex()) != artifact.sha256) {
    if (error != nullptr) *error = QStringLiteral("installer checksum does not match the signed index");
    return false;
  }
  return true;
}

std::optional<StagedUpdate> loadVerifiedStaged(const QString& baseDir, const QString& currentVersion,
                                               const QList<QByteArray>& trustedKeys, bool allowFileUrls,
                                               const QString& platformIn, const QString& kindIn) {
  const QString platform = platformIn.isEmpty() ? QLatin1String(playerPlatform()) : platformIn;
  const QString kind = kindIn.isEmpty() ? QLatin1String(playerKind()) : kindIn;
  const QDir root(stagingRoot(baseDir));
  std::optional<StagedUpdate> best;
  std::optional<SemVer> bestVer;
  QString bestDir;
  const auto cur = SemVer::parse(currentVersion);
  const QStringList dirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString& name : dirs) {
    const QDir d(root.filePath(name));
    const QJsonObject staged = QJsonDocument::fromJson(readFile(d.filePath(QStringLiteral("staged.json")), 65536)).object();
    const QString version = staged.value(QLatin1String("version")).toString();
    const QString artifactName = staged.value(QLatin1String("artifact")).toString();
    const auto sv = SemVer::parse(version);
    if (!cur || !sv || SemVer::compare(*sv, *cur) <= 0 || artifactName.isEmpty()) continue;  // replay/old/invalid
    const QByteArray idx = readFile(d.filePath(QStringLiteral("index.json")));
    const QByteArray sig = readFile(d.filePath(QStringLiteral("index.json.sig")), kMaxSigBytes);
    if (idx.isEmpty() || !verifyIndexSignature(idx, sig, trustedKeys).ok) continue;
    const ParseResult pr = parseIndex(idx, allowFileUrls);
    if (!pr.ok) continue;
    const Release* rel = nullptr;
    for (const Release& r : pr.index.releases) {
      if (r.product == QLatin1String("player") && r.semver.toString() == sv->toString()) {
        rel = &r;
        break;
      }
    }
    if (rel == nullptr) continue;
    const auto art = rel->artifact(platform, kind);
    if (!art || art->name != artifactName) continue;
    const QString path = d.filePath(art->name);
    // Skip the (GUI thread) re-hash when size and mtime are what was recorded after the verified download; the
    // launcher hashes the MSI again right before msiexec anyway (PC-3).
    const QFileInfo fi(path);
    const bool unchanged = fi.isFile() && fi.size() == art->size && staged.value(QLatin1String("size")).toInteger(-1) == fi.size() &&
                           staged.value(QLatin1String("mtime_ms")).toInteger(-1) == fi.lastModified().toMSecsSinceEpoch();
    if (!unchanged && !verifyArtifactFile(path, *art)) continue;
    if (!bestVer || SemVer::compare(*sv, *bestVer) > 0) {
      bestVer = sv;
      bestDir = name;
      best = StagedUpdate{rel->version, path, rel->notesUrl, art->sha256, art->size};
    }
  }
  for (const QString& name : dirs) {
    if (name != bestDir) QDir(root.filePath(name)).removeRecursively();
  }
  return best;
}

std::optional<StagedUpdate> loadVerifiedRollback(const QString& baseDir, const QString& version, const QString& currentVersion,
                                                 const QList<QByteArray>& trustedKeys, bool allowFileUrls, int protocolVersion,
                                                 const QDateTime& now, const QString& platformIn, const QString& kindIn) {
  const QString platform = platformIn.isEmpty() ? QLatin1String(playerPlatform()) : platformIn;
  const QString kind = kindIn.isEmpty() ? QLatin1String(playerKind()) : kindIn;
  const auto cur = SemVer::parse(currentVersion);
  const auto want = SemVer::parse(version);
  if (!cur || !want || SemVer::compare(*want, *cur) >= 0) return std::nullopt;  // only ever older
  const QDir d(QDir(rollbackStagingRoot(baseDir)).filePath(want->toString()));
  const QJsonObject staged = QJsonDocument::fromJson(readFile(d.filePath(QStringLiteral("staged.json")), 65536)).object();
  const QString artifactName = staged.value(QLatin1String("artifact")).toString();
  if (staged.value(QLatin1String("version")).toString() != want->toString() || artifactName.isEmpty()) return std::nullopt;
  const QByteArray idx = readFile(d.filePath(QStringLiteral("index.json")));
  const QByteArray sig = readFile(d.filePath(QStringLiteral("index.json.sig")), kMaxSigBytes);
  if (idx.isEmpty() || !verifyIndexSignature(idx, sig, trustedKeys).ok) return std::nullopt;
  const ParseResult pr = parseIndex(idx, allowFileUrls);
  if (!pr.ok || !checkIndexFresh(pr.index, now).isEmpty()) return std::nullopt;
  for (const Release& r : pr.index.releases) {
    if (r.product != QLatin1String("player") || r.semver.toString() != want->toString() || (kind == QLatin1String(kKindMsi) && !r.msiDowngrade)) continue;
    if (protocolVersion > 0 && !(r.minProtocolVersion <= protocolVersion && protocolVersion <= r.protocolVersion)) continue;
    const auto art = r.artifact(platform, kind);
    if (!art || art->name != artifactName) continue;
    const QString path = d.filePath(art->name);
    if (!verifyArtifactFile(path, *art)) return std::nullopt;
    return StagedUpdate{r.version, path, r.notesUrl, art->sha256, art->size};
  }
  return std::nullopt;
}

// ---------------------------------------------------------------- fetch

void UpdateManager::fetchIndex(QNetworkAccessManager* nam, const QUrl& indexUrl, const QList<QByteArray>& trustedKeys,
                               QObject* context, std::function<void(const FetchedIndex&)> done) {
  const bool fileIndex = indexUrl.scheme() == QLatin1String("file");
  if (!(fileIndex || indexUrl.scheme() == QLatin1String("https"))) {
    FetchedIndex fi;
    fi.error = QStringLiteral("update index URL must be https");
    done(fi);
    return;
  }
  // Prefer the single-file bundle (<index>.signed: signature line, then the index bytes), which cannot be read as a
  // mismatched pair. While it is not published (transition, ADR 0016 amendment 2026-10-19) fall back to the legacy
  // pair <index> + <index>.sig. A bundle that was fetched is never replaced by the legacy pair.
  QUrl bundleUrl = indexUrl;
  bundleUrl.setPath(indexUrl.path() + QStringLiteral(".signed"));
  auto finish = [=](const QByteArray& idx, const QByteArray& sig) {
    FetchedIndex fi;
    const SigCheck sc = verifyIndexSignature(idx, sig, trustedKeys);
    if (!sc.ok) {
      fi.error = sc.error;  // never use unsigned content
      done(fi);
      return;
    }
    const ParseResult pr = parseIndex(idx, fileIndex);
    if (!pr.ok) {
      fi.error = pr.error;
      done(fi);
      return;
    }
    fi.error = checkIndexFresh(pr.index, QDateTime::currentDateTimeUtc());
    if (!fi.error.isEmpty()) {
      done(fi);
      return;
    }
    fi.ok = true;
    fi.indexBytes = idx;
    fi.sigBytes = sig;
    fi.index = pr.index;
    done(fi);
  };
  QUrl sigUrl = indexUrl;
  sigUrl.setPath(indexUrl.path() + QStringLiteral(".sig"));
  getBytes(nam, bundleUrl, kMaxIndexBytes + kMaxSigBytes, context, [=](bool bok, const QByteArray& bundle, const QString&) {
    if (bok) {
      const int nl = bundle.indexOf('\n');
      if (nl < 0 || nl + 1 > kMaxSigBytes) {
        FetchedIndex fi;
        fi.error = QStringLiteral("signed bundle has no signature line");
        done(fi);
        return;
      }
      finish(bundle.mid(nl + 1), bundle.left(nl + 1));
      return;
    }
    getBytes(nam, indexUrl, kMaxIndexBytes, context, [=](bool ok, const QByteArray& idx, const QString& err) {
      if (!ok) {
        FetchedIndex fi;
        fi.error = QStringLiteral("index download failed: %1").arg(err);
        done(fi);
        return;
      }
      getBytes(nam, sigUrl, kMaxSigBytes, context, [=](bool sok, const QByteArray& sig, const QString& serr) {
        if (!sok) {
          FetchedIndex fi;
          fi.error = QStringLiteral("signature download failed: %1").arg(serr);
          done(fi);
          return;
        }
        finish(idx, sig);
      });
    });
  });
}

// ---------------------------------------------------------------- manager

UpdateManager::UpdateManager(const Config& config, PlayerSettings* settings, QObject* parent)
    : QObject(parent), config_(config), settings_(settings) {
  indexUrl_ = config.indexUrl;
  if (indexUrl_.isEmpty()) indexUrl_ = uzume::env::get("UZUME_APP_UPDATE_INDEX_URL");
  if (indexUrl_.isEmpty()) indexUrl_ = QLatin1String(kDefaultIndexUrl);
  if (config_.trustedKeys.isEmpty()) config_.trustedKeys = trustedKeysFromEnvironment();
  platform_ = config.platform.isEmpty() ? QLatin1String(playerPlatform()) : config.platform;
  kind_ = config.kind.isEmpty() ? QLatin1String(playerKind()) : config.kind;
  timer_.setSingleShot(true);
  launcher_ = [](const QString& program, const QStringList& args) { return QProcess::startDetached(program, args); };
  connect(&timer_, &QTimer::timeout, this, [this]() {
    if (effectiveChannel() != Channel::Off && !busy()) checkNow();
    else scheduleNext();
  });
  recordRunning();
  settle();
  if (appImageKind()) {
    // This image was started by the update watcher: after a few healthy seconds (or a clean quit) it confirms the start,
    // otherwise the watcher restores <image>.previous.
    const QString image = appImagePath();
    if (!image.isEmpty()) {
      QTimer::singleShot(5000, this, [image]() { appImageConfirmStart(image); });
      connect(qApp, &QCoreApplication::aboutToQuit, this, [image]() { appImageConfirmStart(image); });
    }
  }
  // A rollback installer that has done its job (this is the version it installed) is not kept.
  if (const auto cur = SemVer::parse(config_.currentVersion); cur) {
    QDir(QDir(rollbackStagingRoot(config_.baseDir)).filePath(cur->toString())).removeRecursively();
  }
}

void UpdateManager::recordRunning() {
  if (config_.compiledChannel == QLatin1String("dev")) return;  // a development build is no release to go back to
  QList<HistoryEntry> h = parseHistory(settings_->updateHistory());
  if (recordVersion(h, config_.currentVersion, QDateTime::currentDateTimeUtc())) {
    if (!settings_->setUpdateHistory(historyToJson(h))) qCWarning(lcUpdate) << "could not persist the version history";
  }
}

Pause UpdateManager::pause() const { return parsePause(settings_->updatePause()); }

bool UpdateManager::updatesPaused() const { return pauseEffective(pause(), config_.currentVersion); }

QString UpdateManager::skippedVersion() const {
  const QString skip = settings_->updateSkip();
  return skipEffective(skip, config_.currentVersion) ? skip : QString();
}

bool UpdateManager::selectionSkipped() const {
  return !selection_.release.version.isEmpty() && selection_.release.version == skippedVersion();
}

void UpdateManager::endPause(const Pause& p) {
  bool ok = true;
  if (p.prevAuto == 1) ok = settings_->setUpdateAutoInstall(true);
  else if (p.prevAuto == 0) ok = settings_->setUpdateAutoInstall(false);
  else ok = settings_->clearUpdateAutoInstall();
  ok = settings_->setUpdatePause({}) && ok;
  if (!ok) qCWarning(lcUpdate) << "could not persist the end of the update pause";
}

// The pause ends by itself once the Player is no longer older than the version it went back from; a skip that the
// running version has reached is moot.
void UpdateManager::settle() {
  const Pause p = pause();
  if (p.active() && !pauseEffective(p, config_.currentVersion)) endPause(p);
  const QString skip = settings_->updateSkip();
  if (!skip.isEmpty() && !skipEffective(skip, config_.currentVersion)) settings_->setUpdateSkip(QString());
}

void UpdateManager::resumeUpdates() {
  const Pause p = pause();
  if (!p.active()) return;
  endPause(p);
  settingsChanged();
}

QList<RollbackCandidate> UpdateManager::rollbackOptions() const {
  if (!lastIndex_.ok) return {};
  RollbackQuery q;
  q.currentVersion = config_.currentVersion;
  q.channel = effectiveChannel();
  q.history = parseHistory(settings_->updateHistory());
  q.protocolVersion = config_.protocolVersion;
  q.platform = platform_;
  q.kind = kind_;
  return rollbackCandidates(lastIndex_.index, q);
}

void UpdateManager::rollbackTo(const QString& version) {
  if (state_ == State::Downloading || state_ == State::Applying) return;
  if (!applySupported()) {
    setState(state_, tr("This copy cannot go back by itself. %1").arg(applyUnsupportedReason()));
    return;
  }
  if (busy()) {
    setState(state_, tr("A game or Session is running. Finish it first, then go back."));
    return;
  }
  if (!lastIndex_.ok || !checkIndexFresh(lastIndex_.index, QDateTime::currentDateTimeUtc()).isEmpty()) {
    setState(State::Error, tr("The list of earlier versions is not up to date. Check for updates first."));
    return;
  }
  const auto want = SemVer::parse(version);
  const QList<RollbackCandidate> options = rollbackOptions();
  const auto it = std::find_if(options.cbegin(), options.cend(),
                               [&](const RollbackCandidate& c) { return want && c.release.semver.toString() == want->toString(); });
  if (it == options.cend()) {
    setState(State::Error, tr("%1 is not one of the earlier versions that can be installed.").arg(version));
    return;
  }
  QDir(rollbackStagingRoot(config_.baseDir)).removeRecursively();
  beginDownload(it->release, it->artifact, QDir(rollbackStagingRoot(config_.baseDir)).filePath(it->release.semver.toString()), true, true);
}

void UpdateManager::applyRollback(const StagedUpdate& staged) {
  // Pause first, so that the older Player (which reads the same player.json) does not update itself right back.
  const QJsonObject oldPause = settings_->updatePause();
  const QString oldSkip = settings_->updateSkip();
  const std::optional<bool> oldAuto = settings_->updateAutoInstall();
  Pause p = pause();
  if (!updatesPaused()) {
    p.prevAuto = oldAuto ? (*oldAuto ? 1 : 0) : -1;  // an already running pause keeps the setting from before it began
  }
  p.from = config_.currentVersion;  // going back from here
  p.to = staged.version;
  p.at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  if (!settings_->setUpdatePause(pauseToJson(p)) || !settings_->setUpdateSkip(config_.currentVersion) ||
      !settings_->setUpdateAutoInstall(false)) {
    lastError_ = QStringLiteral("settings could not be written");
    setState(State::Error, tr("Going back was stopped: the settings could not be saved."));
  } else {
    apply(staged);
  }
  if (state_ != State::Applying) {  // nothing was started: leave the settings as they were
    settings_->setUpdatePause(oldPause);
    settings_->setUpdateSkip(oldSkip);
    if (oldAuto) settings_->setUpdateAutoInstall(*oldAuto);
    else settings_->clearUpdateAutoInstall();
    emit changed();
  }
}

Channel UpdateManager::effectiveChannel() const {
  // The user's choice wins, "off" included (Settings: Off | Stable | Beta).
  if (const auto c = parseChannel(settings_->updateChannel()); c) return *c;
  // A beta build follows the resolved default (stable once its own version was promoted); dev/stable builds do not.
  const Channel compiled = channelFromCompiled(config_.compiledChannel);
  if (compiled == Channel::Beta) {
    if (const auto c = parseChannel(settings_->updateChannelDefault()); c && *c != Channel::Off) return *c;
  }
  return compiled;
}

void UpdateManager::resolveDefaultChannel(const Index& index) {
  if (!settings_->updateChannel().isEmpty() || !settings_->updateChannelDefault().isEmpty()) return;
  if (channelFromCompiled(config_.compiledChannel) != Channel::Beta) return;
  bool promoted = false;
  for (const Release& r : index.releases) {
    if (r.product == QLatin1String("player") && r.channel == QLatin1String("stable") && r.version == config_.currentVersion) {
      promoted = true;
      break;
    }
  }
  if (!settings_->setUpdateChannelDefault(promoted ? QStringLiteral("stable") : QStringLiteral("beta"))) {
    qCWarning(lcUpdate) << "could not persist the resolved default channel";
  }
  scheduleNext();  // the interval follows the resolved channel
  emit changed();
}

bool UpdateManager::autoInstallSetting() const {
  if (settings_->updateAutoInstall()) return *settings_->updateAutoInstall();
  return effectiveChannel() == Channel::Beta;
}

bool UpdateManager::applySupported() const {
  if (config_.forceApplySupport) return true;
#if defined(Q_OS_WIN)
  return canSelfUpdate(config_.installRoot);
#elif defined(Q_OS_LINUX)
  return appImageUnsupportedReason(appImagePath()).isEmpty();
#else
  return false;
#endif
}

QString UpdateManager::applyUnsupportedReason() const {
#if defined(Q_OS_WIN)
  if (!canSelfUpdate(config_.installRoot)) {
    return QStringLiteral("This copy of Uzume was not installed with the installer (portable or development build).");
  }
  return {};
#elif defined(Q_OS_LINUX)
  return appImageUnsupportedReason(appImagePath());
#else
  return QStringLiteral("Automatic installation is only available for the Windows installer and the Linux AppImage.");
#endif
}

bool UpdateManager::allowFileUrls() const { return indexUrl_.startsWith(QLatin1String("file:")); }

void UpdateManager::setState(State s, const QString& text) {
  state_ = s;
  status_ = text;
  emit changed();
}

void UpdateManager::start(int firstDelayMs) { timer_.start(firstDelayMs); }

void UpdateManager::scheduleNext() {
  const bool beta = effectiveChannel() == Channel::Beta;
  timer_.start(beta ? 60 * 60 * 1000 : 24 * 60 * 60 * 1000);
}

void UpdateManager::settingsChanged() {
  emit changed();
  if (state_ == State::Downloading || state_ == State::Applying) return;
  checkNow();
}

void UpdateManager::checkNow() {
  if (checkRunning_ || state_ == State::Downloading || state_ == State::Applying) return;
  if (!SemVer::parse(config_.currentVersion)) {
    setState(State::Disabled, tr("Updates are off: this build has no release version."));
    return;
  }
  if (effectiveChannel() == Channel::Off) {
    setState(State::Disabled, tr("Updates are off for this development build. Choose a channel to check for updates."));
    return;
  }
  const QUrl url(indexUrl_);
  checkRunning_ = true;
  if (state_ != State::Ready) setState(State::Checking, tr("Checking for updates…"));
  else emit changed();
  fetchIndex(&nam_, url, config_.trustedKeys, this, [this](const FetchedIndex& fi) { onIndex(fi); });
}

void UpdateManager::onIndex(const FetchedIndex& fi) {
  checkRunning_ = false;
  lastCheck_ = QDateTime::currentDateTime();
  scheduleNext();
  if (!fi.ok) {
    lastError_ = fi.error;
    qCWarning(lcUpdate) << "update check failed:" << fi.error;
    if (state_ == State::Ready) {
      emit changed();  // a staged update stays usable
      return;
    }
    setState(State::Error, tr("Update check failed: %1").arg(fi.error));
    return;
  }
  lastError_.clear();
  lastIndex_ = fi;
  recordRunning();
  settle();
  resolveDefaultChannel(fi.index);
  SelectionInput in;
  in.channel = effectiveChannel();
  in.currentVersion = config_.currentVersion;
  in.platform = platform_;
  in.kind = kind_;
  if (hubProtocol_) in.hub = hubProtocol_();
  selection_ = selectRelease(fi.index, in);
  switch (selection_.status) {
    case Selection::Status::Disabled:
      setState(State::Disabled, selection_.reason);
      return;
    case Selection::Status::UpToDate:
      loadVerifiedStaged(config_.baseDir, config_.currentVersion, config_.trustedKeys, allowFileUrls(), platform_, kind_);  // prune
      setState(State::UpToDate, tr("Uzume %1 is up to date.").arg(config_.currentVersion));
      return;
    case Selection::Status::Incompatible:
      setState(State::Incompatible,
               tr("Uzume %1 is available but needs a newer server. It is not installed automatically.")
                   .arg(selection_.release.version));
      return;
    case Selection::Status::Available:
      break;
  }
  const auto staged = loadVerifiedStaged(config_.baseDir, config_.currentVersion, config_.trustedKeys, allowFileUrls(), platform_, kind_);
  if (staged && staged->version == selection_.release.version && applySupported()) {
    setState(State::Ready, tr("Uzume %1 is ready to install.").arg(staged->version));
    return;
  }
  if (!applySupported()) {
    setState(State::Available, tr("Uzume %1 is available. %2").arg(selection_.release.version, applyUnsupportedReason()));
    return;
  }
  if (autoInstallEffective()) {
    startDownload(false);
    return;
  }
  setState(State::Available, tr("Uzume %1 is available.").arg(selection_.release.version));
}

void UpdateManager::startDownload(bool applyAfter) {
  beginDownload(selection_.release, selection_.artifact,
                QDir(stagingRoot(config_.baseDir)).filePath(selection_.release.semver.toString()), applyAfter, false);
}

void UpdateManager::beginDownload(const Release& release, const Artifact& art, const QString& dir, bool applyAfter, bool rollback) {
  if (art.size > kMaxInstallerBytes) {
    setState(State::Error, tr("The update is too large."));
    return;
  }
  QDir(dir).removeRecursively();
  if (!QDir().mkpath(dir)) {
    setState(State::Error, tr("The update folder could not be created."));
    return;
  }
  auto* file = new QFile(dir + QLatin1Char('/') + art.name + QStringLiteral(".part"));
  if (!file->open(QIODevice::WriteOnly)) {
    delete file;
    setState(State::Error, tr("The update file could not be written."));
    return;
  }
  applyAfterDownload_ = applyAfter;
  dlRelease_ = release;
  dlArtifact_ = art;
  dlRollback_ = rollback;
  progress_ = 0;
  setState(State::Downloading, tr("Downloading Uzume %1…").arg(release.version));
  QNetworkReply* reply = nam_.get(makeRequest(QUrl(art.url)));
  download_ = reply;
  file->setParent(reply);
  connect(reply, &QNetworkReply::readyRead, this, [this, reply, file, art]() {
    const QByteArray chunk = reply->readAll();
    if (file->size() + chunk.size() > art.size) {
      reply->setProperty("tooLarge", true);
      reply->abort();
      return;
    }
    file->write(chunk);
  });
  connect(reply, &QNetworkReply::downloadProgress, this, [this, art](qint64 got, qint64) {
    progress_ = art.size > 0 ? qBound(0.0, static_cast<double>(got) / static_cast<double>(art.size), 1.0) : 0.0;
    emit changed();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, file]() {
    file->flush();
    file->close();
    const QString part = file->fileName();
    const bool tooLarge = reply->property("tooLarge").toBool();
    const bool failed = reply->error() != QNetworkReply::NoError;
    const QString err = tooLarge ? QStringLiteral("download is larger than announced") : reply->errorString();
    reply->deleteLater();
    if (failed || tooLarge) {
      QFile::remove(part);
      lastError_ = err;
      setState(State::Error, tr("The update could not be downloaded: %1").arg(err));
      return;
    }
    finishDownload(part);
  });
}

void UpdateManager::finishDownload(const QString& partPath) {
  const Artifact art = dlArtifact_;
  QString why;
  if (!verifyArtifactFile(partPath, art, &why)) {
    QFile::remove(partPath);
    lastError_ = why;
    qCWarning(lcUpdate) << "downloaded installer rejected:" << why;
    setState(State::Error, tr("The downloaded update was rejected: %1").arg(why));
    return;
  }
  const QDir dir = QFileInfo(partPath).absoluteDir();
  const QString finalPath = dir.filePath(art.name);
  QFile::remove(finalPath);
  const bool renamed = QFile::rename(partPath, finalPath);
  const QFileInfo finalInfo(finalPath);  // size + mtime of the verified file: loadVerifiedStaged skips the re-hash while they match
  if (!renamed ||
      !writeFile(dir.filePath(QStringLiteral("index.json")), lastIndex_.indexBytes) ||
      !writeFile(dir.filePath(QStringLiteral("index.json.sig")), lastIndex_.sigBytes) ||
      !writeFile(dir.filePath(QStringLiteral("staged.json")),
                 QJsonDocument(QJsonObject{{"version", dlRelease_.version},
                                           {"artifact", art.name},
                                           {"size", finalInfo.size()},
                                           {"mtime_ms", finalInfo.lastModified().toMSecsSinceEpoch()}})
                     .toJson())) {
    setState(State::Error, tr("The update could not be staged."));
    return;
  }
  if (dlRollback_) {
    // Verify again from what is on disk (signature of the stored index, freshness, older, protocol, MSI flag, hash).
    dlRollback_ = false;
    applyAfterDownload_ = false;
    const auto staged = loadVerifiedRollback(config_.baseDir, dlRelease_.version, config_.currentVersion, config_.trustedKeys,
                                             allowFileUrls(), config_.protocolVersion, QDateTime::currentDateTimeUtc(), platform_, kind_);
    if (!staged) {
      lastError_ = QStringLiteral("the older installer did not pass the checks");
      setState(State::Error, tr("The older version was rejected: it did not pass the checks."));
      return;
    }
    if (busy()) {  // a game started while it downloaded
      setState(State::Error, tr("A game or Session is running. Finish it first, then go back."));
      return;
    }
    applyRollback(*staged);
    return;
  }
  setState(State::Ready, tr("Uzume %1 is ready to install.").arg(dlRelease_.version));
  if (applyAfterDownload_) {
    applyAfterDownload_ = false;
    installNow();
  }
}

void UpdateManager::installNow() {
  if (state_ == State::Downloading || state_ == State::Applying) return;
  if (!applySupported()) {
    setState(State::Available, tr("This copy cannot update itself. %1").arg(applyUnsupportedReason()));
    return;
  }
  if (busy()) {
    setState(state_, tr("A game or Session is running. Finish it first, then install the update."));
    return;
  }
  if (selectionSkipped()) settings_->setUpdateSkip(QString());  // "Update to X": the user asked for it
  const auto staged = loadVerifiedStaged(config_.baseDir, config_.currentVersion, config_.trustedKeys, allowFileUrls(), platform_, kind_);
  if (staged && (selection_.release.version.isEmpty() || staged->version == selection_.release.version)) {
    apply(*staged);
    return;
  }
  if (state_ == State::Available || state_ == State::Ready || state_ == State::Error) {
    if (selection_.status == Selection::Status::Available) {
      startDownload(true);
    }
  }
}

bool UpdateManager::applyStagedAtStart() {
  if (!applySupported() || effectiveChannel() != Channel::Beta || !autoInstallSetting() || updatesPaused() || busy()) return false;
  const auto staged = loadVerifiedStaged(config_.baseDir, config_.currentVersion, config_.trustedKeys, allowFileUrls(), platform_, kind_);
  if (!staged || staged->version == skippedVersion()) return false;
  if (hasAttemptRecord(*staged)) {
    // We are still on the old version, so the earlier attempt failed: do not loop, leave the retry to the user.
    qCWarning(lcUpdate) << "update to" << staged->version << "was attempted before and failed; not applying automatically";
    lastError_ = QStringLiteral("previous automatic install attempt failed");
    setState(State::Error, tr("The update to %1 could not be installed automatically. Install it from Settings or download the installer.")
                               .arg(staged->version));
    return false;
  }
  apply(*staged);
  return state_ == State::Applying;
}

bool UpdateManager::hasAttemptRecord(const StagedUpdate& staged) const {
  const QString dir = QFileInfo(staged.installerPath).absolutePath();
  const QJsonObject o = QJsonDocument::fromJson(readFile(QDir(dir).filePath(QStringLiteral("attempted.json")), 4096)).object();
  return o.value(QLatin1String("version")).toString() == staged.version;
}

void UpdateManager::recordAttempt(const StagedUpdate& staged) {
  const QString dir = QFileInfo(staged.installerPath).absolutePath();
  if (!writeFile(QDir(dir).filePath(QStringLiteral("attempted.json")), QJsonDocument(QJsonObject{{"version", staged.version}}).toJson())) {
    qCWarning(lcUpdate) << "could not record the install attempt";
  }
}

void UpdateManager::apply(const StagedUpdate& staged) {
  if (busy()) {
    setState(state_, tr("A game or Session is running. Finish it first, then install the update."));
    return;
  }
  if (appImageKind()) {
    applyAppImage(staged);
    return;
  }
  if (!isMsiFile(staged.installerPath)) {
    lastError_ = QStringLiteral("staged update is not an MSI");
    setState(State::Error, tr("The installer could not be started."));
    return;
  }
  // A temp copy of the launcher runs msiexec (elevated for a per-machine install) after this Player exited,
  // then starts the installed Player again.
  QString why;
  const QString launcher = copyLauncherToTemp(config_.installRoot, QDir::tempPath(), &why);
  if (launcher.isEmpty()) {
    lastError_ = why;
    setState(State::Error, tr("The installer could not be started."));
    return;
  }
  const MsiScope scope = detectMsiScope(directoryWritable(config_.installRoot), hubInstalledNear(config_.installRoot));
  const InstallerCommand cmd = msiRelaunchCommand(launcher, staged.installerPath, scope,
                                                  QDir(config_.installRoot).filePath(QLatin1String(kPlayerExeName)), {},
                                                  staged.sha256, staged.size);
  recordAttempt(staged);
  qCInfo(lcUpdate) << "starting installer for" << staged.version;
  if (!launcher_(cmd.program, cmd.args)) {
    lastError_ = QStringLiteral("installer could not be started");
    setState(State::Error, tr("The installer could not be started."));
    return;
  }
  setState(State::Applying, tr("Installing Uzume %1…").arg(staged.version));
  emit quitRequested();
}

// Linux AppImage: the verified file replaces the running image (kept as .previous), then a watcher starts it and quits
// this Player the same way the MSI path does, so the owner of quitRequested syncs saves and shuts down first.
void UpdateManager::applyAppImage(const StagedUpdate& staged) {
  const QString image = appImagePath();
  const QString reason = appImageUnsupportedReason(image);
  if (!reason.isEmpty()) {
    lastError_ = reason;
    setState(State::Error, tr("The update could not be installed. %1").arg(reason));
    return;
  }
  recordAttempt(staged);
  QString why;
  if (!replaceAppImage(staged.installerPath, image, staged.sha256, staged.size, &why)) {
    lastError_ = why;
    qCWarning(lcUpdate) << "AppImage update failed:" << why;
    setState(State::Error, tr("The update could not be installed: %1").arg(why));
    return;
  }
  const InstallerCommand cmd =
      appImageRelaunchCommand(image, QCoreApplication::applicationPid(), appImageRelaunchArguments(QCoreApplication::arguments().mid(1)));
  qCInfo(lcUpdate) << "AppImage replaced by" << staged.version << "; restarting";
  if (!launcher_(cmd.program, cmd.args)) {
    lastError_ = QStringLiteral("the new AppImage could not be started");
    setState(State::Error, tr("Uzume %1 is installed but could not be restarted. Start Uzume again.").arg(staged.version));
    return;
  }
  setState(State::Applying, tr("Installing Uzume %1…").arg(staged.version));
  emit quitRequested();
}

}  // namespace uzume::update
