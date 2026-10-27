// ToolsClient and VariantsClient against the fake server (custom routes): catalog, download with checksum check, variants REST,
// exact int63 seeds. Synthetic data only.
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "corecache.h"
#include "credentialstore.h"
#include "engine_pack.h"
#include "fakehub.h"
#include "hubconnection.h"
#include "profilestore.h"
#include "toolsclient.h"
#include "variantsclient.h"

using namespace uzume;
using State = HubConnection::State;

namespace {
QString x86(QString p) { return p.replace(QLatin1String("x64"), QLatin1String("x86_64")); }
}

class ToolsVariantsTest : public QObject {
  Q_OBJECT
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;
  std::unique_ptr<ToolCache> cache_;
  std::unique_ptr<ToolsClient> tools_;
  QByteArray zip_;
  QString platform_;
  bool offered_ = true;
  QByteArray served_;

  static QByteArray makeZip() {
    return testutil::buildPackZip({{QStringLiteral("UPR-FVX.jar"), QByteArray(), 0100644, false},
                                   {QStringLiteral("java/bin/java"), QByteArray("#!/bin/sh\n"), 0100755, false},
                                   {QStringLiteral("java/bin/java.exe"), QByteArray("MZ"), 0100755, false}});
  }
  QByteArray catalog(const QString& platform, const QByteArray& zip, bool offered) const {
    QJsonObject plat{{QStringLiteral("platform"), platform}, {QStringLiteral("size"), zip.size()}, {QStringLiteral("sha256"), testutil::sha256Hex(zip)}, {QStringLiteral("cached"), true}};
    QJsonObject tool{{QStringLiteral("id"), QStringLiteral("randomizer")}, {QStringLiteral("name"), QStringLiteral("Randomizer engine")},
                     {QStringLiteral("offered"), offered}, {QStringLiteral("version"), QStringLiteral("1.6.2")},
                     {QStringLiteral("license"), QStringLiteral("GPL-3.0")}, {QStringLiteral("source_url"), QStringLiteral("https://example.invalid/src")},
                     {QStringLiteral("versions"), QJsonArray{QJsonObject{{QStringLiteral("version"), QStringLiteral("1.6.2")}, {QStringLiteral("platforms"), QJsonArray{plat}},
                                                                        {QStringLiteral("used_by"), QJsonArray()}}}}};
    return QJsonDocument(QJsonArray{tool}).toJson(QJsonDocument::Compact);
  }
  ToolPrepareResult prepare(const QString& version) {
    QSignalSpy spy(tools_.get(), &ToolsClient::prepared);
    tools_->prepare(QStringLiteral("randomizer"), version);
    if (!spy.wait(8000) && spy.isEmpty()) {
      QTest::qFail("no result", __FILE__, __LINE__);
      return {};
    }
    return spy.takeFirst().at(0).value<ToolPrepareResult>();
  }

 private slots:
  void init() {
    dir_ = std::make_unique<QTemporaryDir>();
    profiles_ = std::make_unique<ProfileStore>(dir_->filePath(QStringLiteral("data")));
    creds_ = std::make_unique<MemoryCredentialStore>();
    hub_ = std::make_unique<FakeHub>(QStringLiteral("a"));
    hub_->features = {QStringLiteral("saves_v1"), QStringLiteral("tools_v1"), QStringLiteral("variants_v1")};
    QVERIFY(hub_->start());
    conn_ = std::make_unique<HubConnection>(profiles_.get(), creds_.get());
    conn_->setPollIntervalMs(50);
    conn_->connectToAddress(hub_->address());
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsTrustConfirmation, 8000);
    conn_->confirmTrust();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsPairing, 8000);
    hub_->decision = FakeHub::Decision::Approve;
    conn_->requestPairing();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::Connected, 8000);
    cache_ = std::make_unique<ToolCache>(profiles_->toolCacheDir());
    tools_ = std::make_unique<ToolsClient>(conn_.get(), cache_.get());
    platform_ = CoreCache::currentPlatform();
    zip_ = makeZip();
    served_ = zip_;
    offered_ = true;
    hub_->customRoute = [this](const FakeRequest& req, int& status, QByteArray& body, QByteArray& ct) {
      if (req.method == "GET" && req.path == QLatin1String("/api/v1/tools")) {
        body = catalog(x86(platform_), zip_, offered_);  // the catalog may use x86_64
        return true;
      }
      if (req.method == "GET" && req.path == QLatin1String("/api/v1/tools/randomizer/packages/1.6.2/") + x86(platform_)) {
        body = served_;
        ct = "application/zip";
        return true;
      }
      Q_UNUSED(status);
      return false;
    };
  }
  void cleanup() {
    tools_.reset();
    cache_.reset();
    conn_.reset();
    hub_.reset();
    creds_.reset();
    profiles_.reset();
    dir_.reset();
  }

  void downloadsVerifiesAndInstalls() {
    QVERIFY(tools_->available());
    QSignalSpy prog(tools_.get(), &ToolsClient::progress);
    const ToolPrepareResult r = prepare(QStringLiteral("1.6.2"));
    QVERIFY2(r.ok, qPrintable(r.detail));
    QVERIFY(cache_->isReady(QStringLiteral("randomizer"), QStringLiteral("1.6.2"), ToolCache::normalizePlatform(platform_)));
    QVERIFY(cache_->layout(QStringLiteral("randomizer"), QStringLiteral("1.6.2"), ToolCache::normalizePlatform(platform_)).has_value());
    QVERIFY(!QFileInfo::exists(cache_->root() + QStringLiteral("/.downloads/randomizer-1.6.2-") + x86(platform_) + QStringLiteral(".zip")));
    QCOMPARE(tools_->offeredVersion(QStringLiteral("randomizer")), QStringLiteral("1.6.2"));
    QCOMPARE(tools_->tool(QStringLiteral("randomizer"))->license, QStringLiteral("GPL-3.0"));
    // A second prepare is a cache hit.
    const int requests = hub_->count(QStringLiteral("/api/v1/tools/randomizer/packages"), "GET");
    QVERIFY(prepare(QStringLiteral("1.6.2")).ok);
    QCOMPARE(hub_->count(QStringLiteral("/api/v1/tools/randomizer/packages"), "GET"), requests);
  }

  void wrongBytesAreRefused() {
    served_ = zip_;
    served_[served_.size() / 2] = static_cast<char>(served_[served_.size() / 2] ^ 0x55);  // same size, other content
    const ToolPrepareResult r = prepare(QStringLiteral("1.6.2"));
    QVERIFY(!r.ok);
    QCOMPARE(r.problem, ToolProblem::InvalidHash);
    QVERIFY(!cache_->isReady(QStringLiteral("randomizer"), QStringLiteral("1.6.2"), ToolCache::normalizePlatform(platform_)));
    served_ = zip_ + QByteArray("extra");  // longer than announced
    QVERIFY(!prepare(QStringLiteral("1.6.2")).ok);
  }

  void notOfferedOrUnknown() {
    offered_ = false;
    QCOMPARE(prepare(QStringLiteral("1.6.2")).problem, ToolProblem::NotOffered);
    QVERIFY(tools_->offeredVersion(QStringLiteral("randomizer")).isEmpty());
    offered_ = true;
    QCOMPARE(prepare(QStringLiteral("9.9.9")).problem, ToolProblem::NotOnServer);
  }

  void variantsRestAndExactSeeds() {
    VariantsClient vc(conn_.get());
    QVERIFY(vc.available());
    QByteArray createBody;
    hub_->customRoute = [&](const FakeRequest& req, int& status, QByteArray& body, QByteArray& ct) {
      Q_UNUSED(ct);
      if (req.method == "GET" && req.path == QLatin1String("/api/v1/games/g1/variants")) {
        body = R"({"variants":[{"id":"v1","game_id":"g1","name":"Randomizer #1","slot":"var-12345678","tool_id":"randomizer","tool_version":"1.6.2",)"
               R"("settings":"428abc","seed":9223372036854775807,"base_sha256":"aa","spoiler_log":true,"run_id":null,"created_at":"2026-10-27T10:00:00Z"}]})";
        return true;
      }
      if (req.method == "POST" && req.path == QLatin1String("/api/v1/games/g1/variants")) {
        createBody = req.body;
        if (QJsonDocument::fromJson(req.body).object().value(QStringLiteral("name")).toString() == QLatin1String("taken")) {
          status = 409;
          body = R"({"error":{"code":"name_taken","message":"x"}})";
          return true;
        }
        body = R"({"id":"v2","game_id":"g1","name":"n","slot":"var-aaaaaaaa","seed":9007199254740993,"tool_version":"1.6.2","settings":"s"})";
        status = 201;
        return true;
      }
      if (req.method == "GET" && req.path == QLatin1String("/api/v1/runs/r1/variant")) {
        body = R"({"settings":"428abc","seed":4611686018427387905,"tool_id":"randomizer","tool_version":"1.6.2","base_sha256":"bb","slot":"run-r1"})";
        return true;
      }
      if (req.method == "POST" && req.path == QLatin1String("/api/v1/runs/r1/variant-state")) {
        body = "{}";
        return true;
      }
      if (req.method == "PATCH" && req.path == QLatin1String("/api/v1/variants/v1")) {
        body = R"({"id":"v1","game_id":"g1","name":"New","slot":"var-12345678"})";
        return true;
      }
      if (req.method == "DELETE" && req.path == QLatin1String("/api/v1/variants/v1?saves=keep")) {
        status = 204;
        body = {};
        return true;
      }
      return false;
    };
    const auto wait = [](const auto& fn) {
      bool done = false;
      fn(&done);
      QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    };
    wait([&](bool* done) {
      vc.list(QStringLiteral("g1"), [done](const QList<VariantInfo>& l, const VariantCall& c) {
        QVERIFY(c.ok);
        QCOMPARE(l.size(), 1);
        QCOMPARE(l.first().seed, QStringLiteral("9223372036854775807"));  // an int63 stays exact
        QCOMPARE(l.first().slot, QStringLiteral("var-12345678"));
        QVERIFY(l.first().spoilerLog);
        QVERIFY(l.first().runId.isEmpty());
        *done = true;
      });
    });
    NewVariant nv;
    nv.name = QStringLiteral("Randomizer #2");
    nv.toolVersion = QStringLiteral("1.6.2");
    nv.settings = QStringLiteral("428abc");
    nv.seed = QStringLiteral("9223372036854775806");
    nv.baseSha256 = QStringLiteral("aa");
    wait([&](bool* done) {
      vc.create(QStringLiteral("g1"), nv, [done](const VariantInfo& v, const VariantCall& c) {
        QVERIFY(c.ok);
        QCOMPARE(v.id, QStringLiteral("v2"));
        QCOMPARE(v.seed, QStringLiteral("9007199254740993"));
        *done = true;
      });
    });
    QVERIFY(createBody.contains("\"seed\":9223372036854775806"));  // sent as an exact integer
    QCOMPARE(QJsonDocument::fromJson(createBody).object().value(QStringLiteral("tool_id")).toString(), QStringLiteral("randomizer"));
    nv.name = QStringLiteral("taken");
    wait([&](bool* done) {
      vc.create(QStringLiteral("g1"), nv, [done](const VariantInfo&, const VariantCall& c) {
        QVERIFY(!c.ok);
        QCOMPARE(c.error, QStringLiteral("name_taken"));
        *done = true;
      });
    });
    wait([&](bool* done) {
      vc.runVariant(QStringLiteral("r1"), [done](const RunVariantRecipe& r, const VariantCall& c) {
        QVERIFY(c.ok);
        QCOMPARE(r.seed, QStringLiteral("4611686018427387905"));
        QCOMPARE(r.slot, QStringLiteral("run-r1"));
        QVERIFY(r.valid());
        *done = true;
      });
    });
    wait([&](bool* done) { vc.setRunVariantState(QStringLiteral("r1"), QStringLiteral("building"), [done](const VariantCall& c) { QVERIFY(c.ok); *done = true; }); });
    QCOMPARE(QJsonDocument::fromJson(hub_->requests.last().body).object().value(QStringLiteral("state")).toString(), QStringLiteral("building"));
    wait([&](bool* done) { vc.rename(QStringLiteral("v1"), QStringLiteral("New"), [done](const VariantInfo& v, const VariantCall& c) { QVERIFY(c.ok); QCOMPARE(v.name, QStringLiteral("New")); *done = true; }); });
    wait([&](bool* done) { vc.remove(QStringLiteral("v1"), true, [done](const VariantCall& c) { QVERIFY(c.ok); *done = true; }); });
  }

  void parseHelpers() {
    const QList<ToolInfo> t = parseTools(R"([{"id":"randomizer","offered":true,"versions":[{"version":"1.6.1","platforms":[]},{"version":"1.6.2","platforms":[{"platform":"linux-x86_64","size":3,"sha256":"AB"}]}]}])");
    QCOMPARE(t.size(), 1);
    QCOMPARE(t.first().version, QStringLiteral("1.6.2"));  // newest when the server names none
    QCOMPARE(t.first().versions.last().platforms.first().sha256, QStringLiteral("ab"));
  }
};

QTEST_GUILESS_MAIN(ToolsVariantsTest)
#include "tools_variants_test.moc"
