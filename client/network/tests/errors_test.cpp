// Error events (ADR 0035, feature reports_v2): the sender against the fake server, and the GitHub issue lookup and address.
// Synthetic data only.
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "errorevents.h"
#include "errorsender.h"
#include "fakehub.h"
#include "githubissues.h"
#include "hubconnection.h"
#include "profilestore.h"

using namespace uzume;
using State = HubConnection::State;

namespace {

errors::ErrorEvent ev(int i, const QDateTime& at = QDateTime::currentDateTimeUtc()) {
  errors::ErrorEvent e;
  e.kind = QStringLiteral("plugin_error");
  e.component = QStringLiteral("plugin:uzume.test");
  e.fingerprint = QStringLiteral("uz-%1").arg(i, 12, 16, QLatin1Char('0'));
  e.title = QStringLiteral("Plugin stopped after an error");
  e.message = QStringLiteral("main.lua:%1: boom").arg(i);
  e.appVersion = QStringLiteral("0.24.0");
  e.os = QStringLiteral("Linux");
  e.occurredAt = at;
  return e;
}

// A one-shot HTTP server for the GitHub search: answers every request with `status` and `body`, remembers the request line.
class OneShotHttp : public QObject {
 public:
  OneShotHttp() {
    connect(&server, &QTcpServer::newConnection, this, [this]() {
      QTcpSocket* s = server.nextPendingConnection();
      connect(s, &QTcpSocket::readyRead, this, [this, s]() {
        request += s->readAll();
        if (!request.contains("\r\n\r\n")) return;
        if (hang) return;
        s->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                 QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
      });
    });
  }
  bool listen() { return server.listen(QHostAddress::LocalHost); }
  QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
  QTcpServer server;
  int status = 200;
  QByteArray body, request;
  bool hang = false;
};

}  // namespace

class ErrorsNetworkTest : public QObject {
  Q_OBJECT
 private:
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;
  std::unique_ptr<errors::ErrorQueue> queue_;
  std::unique_ptr<ErrorSender> sender_;

  void pairAndConnect() {
    conn_->connectToAddress(hub_->address());
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsTrustConfirmation, 8000);
    conn_->confirmTrust();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsPairing, 8000);
    hub_->decision = FakeHub::Decision::Approve;
    conn_->requestPairing();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::Connected, 8000);
  }

 private slots:
  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    profiles_ = std::make_unique<ProfileStore>(dir_->filePath(QStringLiteral("data")));
    creds_ = std::make_unique<MemoryCredentialStore>();
    hub_ = std::make_unique<FakeHub>(QStringLiteral("a"));
    hub_->features = {QStringLiteral("saves_v1"), QStringLiteral("reports_v1"), QStringLiteral("reports_v2")};
    QVERIFY(hub_->start());
    conn_ = std::make_unique<HubConnection>(profiles_.get(), creds_.get());
    conn_->setPollIntervalMs(50);
    queue_ = std::make_unique<errors::ErrorQueue>(dir_->filePath(QStringLiteral("data")));
    sender_ = std::make_unique<ErrorSender>(conn_.get(), queue_.get());
    sender_->setRetryMs(50);
  }
  void cleanup() {
    sender_.reset();
    queue_.reset();
    conn_.reset();
    hub_.reset();
    creds_.reset();
    profiles_.reset();
    dir_.reset();
  }

  void sendsNothingWhileOff() {
    pairAndConnect();
    QVERIFY(sender_->supported());
    for (int i = 0; i < 3; ++i) queue_->add(ev(i));
    sender_->flush();
    QTest::qWait(300);
    QCOMPARE(hub_->errorBatches.size(), 0);
    QCOMPARE(queue_->size(), 3);
  }

  void sendsTheQueueWhenTurnedOnAndEmptiesIt() {
    pairAndConnect();
    for (int i = 0; i < 3; ++i) queue_->add(ev(i));
    QSignalSpy sent(sender_.get(), &ErrorSender::batchSent);
    sender_->setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 8000);
    QCOMPARE(sent.first().at(0).toInt(), 3);
    QCOMPARE(hub_->errorBatches.size(), 1);
    const QJsonObject first = hub_->errorBatches.first().first().toObject();
    QCOMPARE(first.value("kind").toString(), QString("plugin_error"));
    QCOMPARE(first.value("component").toString(), QString("plugin:uzume.test"));
    QVERIFY(first.value("fingerprint").toString().startsWith("uz-"));
    QCOMPARE(first.keys().size(), 8);
    QVERIFY(queue_->isEmpty());
  }

  void sendsInBatchesOfTwenty() {
    pairAndConnect();
    for (int i = 0; i < 45; ++i) queue_->add(ev(i));
    sender_->setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(hub_->errorBatches.size(), 3, 8000);
    QCOMPARE(hub_->errorBatches.at(0).size(), 20);
    QCOMPARE(hub_->errorBatches.at(1).size(), 20);
    QCOMPARE(hub_->errorBatches.at(2).size(), 5);
    QTRY_VERIFY(queue_->isEmpty());
  }

  void aServerWithoutReportsV2GetsNothing() {
    hub_->features = {QStringLiteral("saves_v1"), QStringLiteral("reports_v1")};
    pairAndConnect();
    QVERIFY(!sender_->supported());
    queue_->add(ev(1));
    sender_->setEnabled(true);
    QTest::qWait(300);
    QCOMPARE(hub_->errorBatches.size(), 0);
    QCOMPARE(queue_->size(), 1);
  }

  void eventsWaitForTheConnection() {
    queue_->add(ev(1));
    sender_->setEnabled(true);  // not connected: nothing happens
    QVERIFY(!sender_->sending());
    pairAndConnect();           // connected: goes out by itself
    QTRY_COMPARE_WITH_TIMEOUT(hub_->errorBatches.size(), 1, 8000);
    QTRY_VERIFY(queue_->isEmpty());
  }

  void aFailedBatchStaysAndIsRetried() {
    pairAndConnect();
    queue_->add(ev(1));
    hub_->errorBatchFailNext = 1;
    hub_->errorBatchFailStatus = 503;
    QSignalSpy failed(sender_.get(), &ErrorSender::batchFailed);
    sender_->setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 8000);
    QCOMPARE(failed.first().at(0).toString(), QString("server"));
    QTRY_COMPARE_WITH_TIMEOUT(hub_->errorBatches.size(), 1, 8000);  // the retry
    QTRY_VERIFY(queue_->isEmpty());
  }

  void aRefusedShapeIsDroppedNotRetried() {
    pairAndConnect();
    queue_->add(ev(1));
    hub_->errorBatchFailNext = 1;
    hub_->errorBatchFailStatus = 400;
    QSignalSpy failed(sender_.get(), &ErrorSender::batchFailed);
    sender_->setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 8000);
    QCOMPARE(failed.first().at(0).toString(), QString("rejected"));
    QVERIFY(queue_->isEmpty());
    QTest::qWait(200);
    QCOMPARE(hub_->errorBatches.size(), 0);
  }

  // ------------------------------------------------------------ GitHub

  void repoAndIssueAddressesAreChecked() {
    QVERIFY(GithubIssueLookup::validRepoBase("https://github.com/phabioo/uzume"));
    QVERIFY(GithubIssueLookup::validRepoBase("https://github.com/phabioo/uzume-plugin.soullink"));
    for (const char* bad : {"http://github.com/a/b", "https://github.com/a", "https://github.com/a/b/", "https://github.com/a/b/issues/1",
                            "https://evil.example/github.com/a/b", "https://github.com/a/b\n", "https://user@github.com/a/b"})
      QVERIFY2(!GithubIssueLookup::validRepoBase(bad), bad);
    QVERIFY(GithubIssueLookup::validIssueUrl("https://github.com/phabioo/uzume/issues/12"));
    QVERIFY(!GithubIssueLookup::validIssueUrl("https://github.com/phabioo/uzume/issues/x"));
    QVERIFY(!GithubIssueLookup::validIssueUrl("https://github.com/phabioo/uzume/pull/12"));
    QVERIFY(GithubIssueLookup::openAllowed("https://github.com/phabioo/uzume/issues/12"));
    QVERIFY(GithubIssueLookup::openAllowed("https://github.com/phabioo/uzume/issues/new?x=1"));
    for (const char* bad : {"http://github.com/a/b", "file:///etc/passwd", "https://github.com.evil.example/a", "https://evil.example/", "javascript:alert(1)",
                            "https://user:pw@github.com/a/b", "https://github.com:8443/a/b", ""})
      QVERIFY2(!GithubIssueLookup::openAllowed(bad), bad);
  }

  void searchAddressHasTheFingerprintAndRepository() {
    const QUrl u = GithubIssueLookup::searchUrl("https://api.github.com", "uz-0123456789ab", "https://github.com/phabioo/uzume");
    QCOMPARE(u.toEncoded(), QByteArray("https://api.github.com/search/issues?q=uz-0123456789ab+repo:phabioo/uzume+is:issue"));
    QVERIFY(GithubIssueLookup::searchUrl("https://api.github.com", "uz-0123456789ab", "https://example.com/a/b").isEmpty());
  }

  void searchAnswerIsParsed() {
    const QString base = "https://github.com/phabioo/uzume";
    const QByteArray hit = "{\"total_count\":2,\"items\":[\n"
"      {\"number\":9,\"state\":\"closed\",\"html_url\":\"https://github.com/phabioo/uzume/issues/9\"},\n"
"      {\"number\":12,\"state\":\"open\",\"html_url\":\"https://github.com/phabioo/uzume/issues/12\"}]}";
    auto r = GithubIssueLookup::parseSearch(hit, base);
    QVERIFY(r.ok);
    QVERIFY(r.found);
    QCOMPARE(r.number, 12);  // the open one wins
    QCOMPARE(r.url, QString("https://github.com/phabioo/uzume/issues/12"));
    r = GithubIssueLookup::parseSearch(R"({"total_count":0,"items":[]})", base);
    QVERIFY(r.ok);
    QVERIFY(!r.found);
    // Pull requests, other repositories and odd addresses are never taken.
    r = GithubIssueLookup::parseSearch("{\"items\":[\n"
"      {\"number\":1,\"state\":\"open\",\"html_url\":\"https://github.com/phabioo/uzume/issues/1\",\"pull_request\":{}},\n"
"      {\"number\":2,\"state\":\"open\",\"html_url\":\"https://github.com/other/repo/issues/2\"},\n"
"      {\"number\":3,\"state\":\"open\",\"html_url\":\"javascript:alert(3)\"}]}", base);
    QVERIFY(r.ok);
    QVERIFY(!r.found);
    QVERIFY(!GithubIssueLookup::parseSearch("not json", base).ok);
    QVERIFY(!GithubIssueLookup::parseSearch(R"({"message":"rate limit"})", base).ok);
  }

  void newIssueAddressIsPrefilledAndQueryEncoded() {
    const QString url = GithubIssueLookup::newIssueUrl("https://github.com/phabioo/uzume", "Core failed: <path> & more", "uz-0123456789ab",
                                                       "0.24.0", "0.24.1", "Seen 3 times on 2 devices, versions 0.24.0");
    QVERIFY(url.startsWith("https://github.com/phabioo/uzume/issues/new?template=bug_report.yml&title="));
    QVERIFY(url.contains("title=Core%20failed%3A%20%3Cpath%3E%20%26%20more"));
    QVERIFY(url.contains("&fingerprint=uz-0123456789ab"));
    QVERIFY(url.contains("&app-version=0.24.0"));
    QVERIFY(url.contains("&server-version=0.24.1"));
    QVERIFY(url.contains("&issue-desc=Seen%203%20times"));
    QVERIFY(url.endsWith("&labels=bug,triage"));
    QVERIFY(GithubIssueLookup::openAllowed(url));
    QVERIFY(GithubIssueLookup::newIssueUrl("https://example.com/a/b", "t", "f", "a", "s", "d").isEmpty());
  }

  void newIssueAddressCutsTheDescriptionFirst() {
    const QString longSummary(20000, QLatin1Char('x'));
    const QString url = GithubIssueLookup::newIssueUrl("https://github.com/phabioo/uzume", "Title stays", "uz-0123456789ab", "0.24.0", "0.24.1", longSummary);
    QVERIFY2(url.size() <= GithubIssueLookup::kMaxUrlChars, qPrintable(QString::number(url.size())));
    QVERIFY(url.size() > GithubIssueLookup::kMaxUrlChars - 10);
    QVERIFY(url.contains("title=Title%20stays"));
    QVERIFY(url.contains("&fingerprint=uz-0123456789ab"));
    QVERIFY(url.contains("&app-version=0.24.0"));
    QVERIFY(url.endsWith("&labels=bug,triage"));
    // Multi-byte text is encoded per byte (three characters per 9 chars of address) and still fits.
    const QString wide(5000, QChar(0x65E5));
    QVERIFY(GithubIssueLookup::newIssueUrl("https://github.com/phabioo/uzume", "t", "uz-0123456789ab", "a", "s", wide).size() <= GithubIssueLookup::kMaxUrlChars);
  }

  void lookupFindsAnIssue() {
    OneShotHttp http;
    QVERIFY(http.listen());
    http.body = "{\"items\":[{\"number\":7,\"state\":\"open\",\"html_url\":\"https://github.com/phabioo/uzume/issues/7\"}]}";
    GithubIssueLookup lookup;
    lookup.setApiBase(http.base());
    std::optional<GithubIssueLookup::Result> got;
    lookup.lookup("uz-0123456789ab", "https://github.com/phabioo/uzume", [&](const GithubIssueLookup::Result& r) { got = r; });
    QTRY_VERIFY_WITH_TIMEOUT(got.has_value(), 8000);
    QVERIFY(got->ok);
    QVERIFY(got->found);
    QCOMPARE(got->number, 7);
    QVERIFY(http.request.startsWith("GET /search/issues?q=uz-0123456789ab+repo:phabioo/uzume+is:issue HTTP/1.1"));
    QVERIFY(!http.request.toLower().contains("authorization"));  // never a credential
    QVERIFY(!http.request.toLower().contains("cookie"));
  }

  void lookupFailureMeansSkip() {
    OneShotHttp http;
    QVERIFY(http.listen());
    http.status = 403;
    http.body = R"({"message":"API rate limit exceeded"})";
    GithubIssueLookup lookup;
    lookup.setApiBase(http.base());
    std::optional<GithubIssueLookup::Result> got;
    lookup.lookup("uz-0123456789ab", "https://github.com/phabioo/uzume", [&](const GithubIssueLookup::Result& r) { got = r; });
    QTRY_VERIFY_WITH_TIMEOUT(got.has_value(), 8000);
    QVERIFY(!got->ok);
    QVERIFY(!got->found);
    // An invalid repository never reaches the network.
    got.reset();
    lookup.lookup("uz-0123456789ab", "https://example.com/a/b", [&](const GithubIssueLookup::Result& r) { got = r; });
    QTRY_VERIFY_WITH_TIMEOUT(got.has_value(), 2000);
    QVERIFY(!got->ok);
  }

  void lookupTimesOutAfterFiveSeconds() {
    OneShotHttp http;
    QVERIFY(http.listen());
    http.hang = true;
    GithubIssueLookup lookup;
    lookup.setApiBase(http.base());
    std::optional<GithubIssueLookup::Result> got;
    QElapsedTimer t;
    t.start();
    lookup.lookup("uz-0123456789ab", "https://github.com/phabioo/uzume", [&](const GithubIssueLookup::Result& r) { got = r; });
    QTRY_VERIFY_WITH_TIMEOUT(got.has_value(), 9000);
    QVERIFY(!got->ok);
    QVERIFY2(t.elapsed() >= 4500 && t.elapsed() < 8000, qPrintable(QString::number(t.elapsed())));
  }
};

QTEST_MAIN(ErrorsNetworkTest)
#include "errors_test.moc"
