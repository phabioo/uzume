#include "errorscontroller.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>

#include "errorfingerprint.h"

namespace uzume::ui {

ErrorsController::ErrorsController(const Deps& deps, QObject* parent)
    : QObject(parent), d_(deps), queue_(deps.dataDir), sender_(deps.conn, &queue_), github_() {
  sender_.setEnabled(sendAutomatically());
  connect(&sender_, &ErrorSender::batchSent, this, [this]() { emit pendingChanged(); });
  connect(&sender_, &ErrorSender::batchFailed, this, [this](const QString&) { emit pendingChanged(); });
  if (d_.conn) connect(d_.conn, &HubConnection::stateChanged, this, [this]() { emit supportChanged(); });
  // Events left from an earlier run and nobody was asked yet: ask now.
  if (!queue_.isEmpty() && !answered()) {
    prompt_ = true;
  }
}

bool ErrorsController::sendAutomatically() const { return d_.settings && d_.settings->sendErrors().value_or(false); }
bool ErrorsController::answered() const { return d_.settings && d_.settings->sendErrors().has_value(); }

void ErrorsController::setSendAutomatically(bool on) {
  if (!d_.settings) return;
  const bool was = sendAutomatically(), wasAnswered = answered();
  d_.settings->setSendErrors(on);
  sender_.setEnabled(on);
  if (prompt_) {
    prompt_ = false;
    emit promptChanged();
  }
  if (was != on || !wasAnswered) emit settingChanged();
}

void ErrorsController::answerPrompt(bool allow) { setSendAutomatically(allow); }

bool ErrorsController::record(const QString& kind, const QString& component, const QString& title, const QString& message) {
  const QDateTime now = clock_ ? clock_() : QDateTime::currentDateTimeUtc();
  const errors::ErrorEvent e = errors::makeEvent(kind, component, title, message, d_.source, now);
  if (!queue_.add(e)) return false;
  emit pendingChanged();
  if (!answered() && !prompt_) {
    prompt_ = true;
    emit promptChanged();
  }
  sender_.flush();
  return true;
}

void ErrorsController::recordCoreFailure(const QString& coreId, const QString& title, const QString& message) {
  const QString component = QStringLiteral("core:") + coreId;
  record(QString::fromLatin1(errors::kKindCoreFailure), errors::validComponent(component) ? component : QStringLiteral("app"), title, message);
}

void ErrorsController::recordPluginError(const QString& pluginId, const QString& message) {
  const QString component = QStringLiteral("plugin:") + pluginId;
  record(QString::fromLatin1(errors::kKindPluginError), errors::validComponent(component) ? component : QStringLiteral("app"),
         QStringLiteral("Plugin stopped after an error"), message);
}

void ErrorsController::recordSyncFailure(const QString& message) {
  record(QString::fromLatin1(errors::kKindSyncFailure), QStringLiteral("app"), QStringLiteral("Save sync failed"), message);
}

void ErrorsController::recordUpdateFailure(const QString& message) {
  record(QString::fromLatin1(errors::kKindUpdateFailure), QStringLiteral("app"), QStringLiteral("Update failed"), message);
}

void ErrorsController::noteStart(bool uncleanExit, const QString& previousLog) {
  if (!uncleanExit) return;
  record(QString::fromLatin1(errors::kKindAppCrash), QStringLiteral("app"), QStringLiteral("App closed unexpectedly"),
         errors::lastErrorLine(previousLog));
}

void ErrorsController::lookupIssue(const QString& fingerprint, const QString& issuesBase) {
  github_.lookup(fingerprint, issuesBase, [this, fingerprint](const GithubIssueLookup::Result& r) {
    emit issueLookupFinished(fingerprint, r.ok, r.found, r.number, r.url);
  });
}

void ErrorsController::cancelLookup() { github_.cancel(); }

QString ErrorsController::newIssueUrl(const QString& issuesBase, const QString& publicTitle, const QString& fingerprint,
                                      const QString& newestAppVersion, const QString& publicSummary) const {
  const QString server = d_.serverVersion ? d_.serverVersion() : QString();
  return GithubIssueLookup::newIssueUrl(issuesBase, publicTitle, fingerprint, newestAppVersion, server, publicSummary);
}

bool ErrorsController::openInBrowser(const QString& url) {
  if (!GithubIssueLookup::openAllowed(url)) return false;
  const QUrl u(url);
  return opener_ ? opener_(u) : QDesktopServices::openUrl(u);
}

}  // namespace uzume::ui
