#pragma once
// GithubIssueLookup (ADR 0035 D2): looks a fingerprint up in the public issues of a repository (GitHub search API, no credentials,
// no token, never anything but the fingerprint and the repository name), and builds the prefilled "new issue" address. A failed
// lookup (offline, rate limit, 5 s timeout) only means "skip the lookup": the caller goes on to the prefilled address.

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <functional>

namespace uzume {

class GithubIssueLookup : public QObject {
  Q_OBJECT
 public:
  struct Result {
    bool ok = false;      // the search answered; false = skip the lookup
    bool found = false;
    int number = 0;
    QString url;          // https://github.com/<owner>/<repo>/issues/<n>
    QString state;        // open | closed
  };
  using Done = std::function<void(const Result&)>;

  static constexpr int kTimeoutMs = 5000;
  static constexpr int kMaxUrlChars = 7500;

  explicit GithubIssueLookup(QObject* parent = nullptr);

  void setApiBase(const QString& base) { apiBase_ = base; }  // tests; default https://api.github.com

  // `issuesBase`: https://github.com/<owner>/<repo>. `done` is called once (also for an invalid base: ok=false), from the event loop.
  void lookup(const QString& fingerprint, const QString& issuesBase, Done done);
  void cancel();

  // Pure helpers.
  static bool validRepoBase(const QString& base);   // ^https://github\.com/[A-Za-z0-9-]+/[A-Za-z0-9._-]+$
  static bool validIssueUrl(const QString& url);    // ^https://github\.com/<o>/<r>/issues/\d+$
  static QUrl searchUrl(const QString& apiBase, const QString& fingerprint, const QString& issuesBase);
  static Result parseSearch(const QByteArray& body, const QString& issuesBase);
  // <issues_base>/issues/new?template=bug_report.yml&title=..&fingerprint=..&app-version=..&server-version=..&issue-desc=..&labels=bug,triage
  // Query-encoded; the whole address is at most kMaxUrlChars characters, issue-desc is cut first. Empty for an invalid base.
  static QString newIssueUrl(const QString& issuesBase, const QString& title, const QString& fingerprint, const QString& appVersion,
                             const QString& serverVersion, const QString& summary);
  // The only way the app opens an address from here: https, host github.com.
  static bool openAllowed(const QString& url);

 private:
  QNetworkAccessManager nam_;
  QPointer<QNetworkReply> reply_;
  QString apiBase_ = QStringLiteral("https://api.github.com");
};

}  // namespace uzume
