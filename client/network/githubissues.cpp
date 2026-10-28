#include "githubissues.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

namespace uzume {

namespace {
QString ownerRepo(const QString& issuesBase) {
  static const QString prefix = QStringLiteral("https://github.com/");
  return issuesBase.mid(prefix.size());
}
}  // namespace

GithubIssueLookup::GithubIssueLookup(QObject* parent) : QObject(parent) {}

bool GithubIssueLookup::validRepoBase(const QString& base) {
  static const QRegularExpression re(QStringLiteral(R"(\Ahttps://github\.com/[A-Za-z0-9-]+/[A-Za-z0-9._-]+\z)"));
  return re.match(base).hasMatch();
}

bool GithubIssueLookup::validIssueUrl(const QString& url) {
  static const QRegularExpression re(QStringLiteral(R"(\Ahttps://github\.com/[A-Za-z0-9-]+/[A-Za-z0-9._-]+/issues/\d+\z)"));
  return re.match(url).hasMatch();
}

bool GithubIssueLookup::openAllowed(const QString& url) {
  const QUrl u(url);
  return u.isValid() && u.scheme() == QLatin1String("https") && u.host() == QLatin1String("github.com") && u.userInfo().isEmpty() && u.port() == -1;
}

QUrl GithubIssueLookup::searchUrl(const QString& apiBase, const QString& fingerprint, const QString& issuesBase) {
  if (!validRepoBase(issuesBase)) return {};
  // q=<fp>+repo:<owner>/<repo>+is:issue ("+" is a space in a query)
  return QUrl::fromEncoded(QStringLiteral("%1/search/issues?q=%2+repo:%3+is:issue").arg(apiBase, fingerprint, ownerRepo(issuesBase)).toUtf8());
}

GithubIssueLookup::Result GithubIssueLookup::parseSearch(const QByteArray& body, const QString& issuesBase) {
  Result r;
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject() || !doc.object().value(QStringLiteral("items")).isArray()) return r;
  r.ok = true;
  const QString prefix = issuesBase + QStringLiteral("/issues/");
  for (const QJsonValue& v : doc.object().value(QStringLiteral("items")).toArray()) {
    const QJsonObject o = v.toObject();
    const QString url = o.value(QStringLiteral("html_url")).toString();
    if (!validIssueUrl(url) || !url.startsWith(prefix) || o.contains(QStringLiteral("pull_request"))) continue;
    r.found = true;
    r.url = url;
    r.number = o.value(QStringLiteral("number")).toInt();
    r.state = o.value(QStringLiteral("state")).toString();
    if (r.state == QLatin1String("open")) break;  // an open issue beats a closed one
  }
  return r;
}

void GithubIssueLookup::lookup(const QString& fingerprint, const QString& issuesBase, Done done) {
  cancel();
  const QUrl url = searchUrl(apiBase_, fingerprint, issuesBase);
  if (!url.isValid()) {
    QTimer::singleShot(0, this, [done = std::move(done)]() { if (done) done(Result{}); });
    return;
  }
  QNetworkRequest req(url);
  req.setRawHeader("Accept", "application/vnd.github+json");
  req.setRawHeader("User-Agent", QByteArray("Uzume/") + QCoreApplication::applicationVersion().toUtf8());
  req.setTransferTimeout(kTimeoutMs);
  QNetworkReply* reply = nam_.get(req);
  reply_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply, issuesBase, done = std::move(done)]() {
    const bool mine = reply_ == reply;
    reply_.clear();
    Result r;
    if (reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
      r = parseSearch(reply->readAll(), issuesBase);
    }
    reply->deleteLater();
    if (mine && done) done(r);
  });
}

void GithubIssueLookup::cancel() {
  if (!reply_) return;
  QNetworkReply* reply = reply_;
  reply_.clear();
  disconnect(reply, nullptr, this, nullptr);
  reply->abort();
  reply->deleteLater();
}

QString GithubIssueLookup::newIssueUrl(const QString& issuesBase, const QString& title, const QString& fingerprint, const QString& appVersion,
                                       const QString& serverVersion, const QString& summary) {
  if (!validRepoBase(issuesBase)) return {};
  const auto enc = [](const QString& s) { return QString::fromLatin1(QUrl::toPercentEncoding(s)); };
  const auto build = [&](const QString& desc) {
    return QStringLiteral("%1/issues/new?template=bug_report.yml&title=%2&fingerprint=%3&app-version=%4&server-version=%5&issue-desc=%6&labels=bug,triage")
        .arg(issuesBase, enc(title), enc(fingerprint), enc(appVersion), enc(serverVersion), enc(desc));
  };
  QString url = build(summary);
  if (url.size() <= kMaxUrlChars) return url;
  // Cut issue-desc first: binary search the longest prefix of the summary that fits.
  int lo = 0, hi = static_cast<int>(summary.size());
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if (build(summary.left(mid)).size() <= kMaxUrlChars) lo = mid; else hi = mid - 1;
  }
  QString cut = summary.left(lo);
  if (cut.size() > 0 && cut.back().isHighSurrogate()) cut.chop(1);
  url = build(cut);
  return url.size() <= kMaxUrlChars ? url : build(QString());
}

}  // namespace uzume
