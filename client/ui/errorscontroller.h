#pragma once
// ErrorsController (ADR 0035): the app's side of error groups.
//
//   - record()/noteStart(): errors of known kinds become events in the persisted queue (core/errorevents.h). The PlayerController
//     calls record*() once from each existing error path (core failure, plugin error, save sync, update, unclean exit).
//   - Opt-in: "Send errors to the server automatically" (Settings > Help & diagnostics), unanswered = off. The first time an event is
//     queued while unanswered, `promptVisible` turns on (a non-modal banner in Main.qml with "Send automatically" / "Not now"); any
//     answer ends it for good. Only with the setting on do events leave the PC, and only to the server the app is connected to
//     (network/errorsender.h, feature reports_v2).
//   - Server > Reports > Errors (ServerErrorsView.qml) uses the admin API through AdminController; this class adds what the QML cannot
//     do: the GitHub issue lookup, the prefilled issue address and opening it in the system browser (https, github.com only).

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <memory>

#include "errorevents.h"
#include "errorsender.h"
#include "githubissues.h"
#include "hubconnection.h"
#include "playersettings.h"

namespace uzume::ui {

class ErrorsController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Owned by PlayerController")

  Q_PROPERTY(bool sendAutomatically READ sendAutomatically WRITE setSendAutomatically NOTIFY settingChanged)
  Q_PROPERTY(bool answered READ answered NOTIFY settingChanged)
  Q_PROPERTY(bool promptVisible READ promptVisible NOTIFY promptChanged)
  Q_PROPERTY(int pending READ pending NOTIFY pendingChanged)           // events waiting in the queue
  Q_PROPERTY(bool serverSupportsErrors READ serverSupportsErrors NOTIFY supportChanged)  // connected and reports_v2

 public:
  struct Deps {
    PlayerSettings* settings = nullptr;
    HubConnection* conn = nullptr;
    QString dataDir;
    errors::EventSource source;                 // app version and OS of the events
    std::function<QString()> serverVersion;     // the connected server's version (issue form)
  };
  explicit ErrorsController(const Deps& deps, QObject* parent = nullptr);

  bool sendAutomatically() const;
  void setSendAutomatically(bool on);
  bool answered() const;
  bool promptVisible() const { return prompt_; }
  int pending() const { return queue_.size(); }
  bool serverSupportsErrors() const { return sender_.supported(); }

  // ---- recording (C++ callers) ----
  // True when the event was queued (false: an equal one is already waiting, or the input is invalid).
  bool record(const QString& kind, const QString& component, const QString& title, const QString& message);
  void recordCoreFailure(const QString& coreId, const QString& title, const QString& message);   // core_failure, core:<id> (app when the id is unusable)
  void recordPluginError(const QString& pluginId, const QString& message);                       // plugin_error, plugin:<id>
  void recordSyncFailure(const QString& message);                                                // sync_failure, app
  void recordUpdateFailure(const QString& message);                                              // update_failure, app
  // At start: `uncleanExit` = the marker of the previous run was found; `previousLog` is its log (player.1.log after the rotation).
  void noteStart(bool uncleanExit, const QString& previousLog);

  // ---- prompt ----
  Q_INVOKABLE void answerPrompt(bool allow);  // "Send automatically" / "Not now" (= off); both end the question for good

  // ---- GitHub (Server > Reports > Errors) ----
  // Looks the fingerprint up in the public issues of `issuesBase`; issueLookupFinished follows (ok=false: skip the lookup).
  Q_INVOKABLE void lookupIssue(const QString& fingerprint, const QString& issuesBase);
  Q_INVOKABLE void cancelLookup();
  // Prefilled "new issue" address from the server's public title and summary; empty for an invalid base.
  Q_INVOKABLE QString newIssueUrl(const QString& issuesBase, const QString& publicTitle, const QString& fingerprint,
                                  const QString& newestAppVersion, const QString& publicSummary) const;
  Q_INVOKABLE bool validIssueUrl(const QString& url) const { return GithubIssueLookup::validIssueUrl(url); }
  Q_INVOKABLE bool openInBrowser(const QString& url);  // https and github.com only; no embedded browser

  // Tests.
  errors::ErrorQueue* queue() { return &queue_; }
  ErrorSender* sender() { return &sender_; }
  GithubIssueLookup* github() { return &github_; }
  void setOpener(std::function<bool(const QUrl&)> opener) { opener_ = std::move(opener); }
  void setClock(std::function<QDateTime()> clock) { clock_ = std::move(clock); }

 signals:
  void settingChanged();
  void promptChanged();
  void pendingChanged();
  void supportChanged();
  void issueLookupFinished(const QString& fingerprint, bool ok, bool found, int number, const QString& url);

 private:
  Deps d_;
  errors::ErrorQueue queue_;
  ErrorSender sender_;
  GithubIssueLookup github_;
  bool prompt_ = false;
  std::function<bool(const QUrl&)> opener_;
  std::function<QDateTime()> clock_;
};

}  // namespace uzume::ui
