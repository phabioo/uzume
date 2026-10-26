// Connection check dialog (design 6e) in the real window: opened from Settings, five rows, the four outcomes, keys R / Ctrl+C / Esc,
// Copy result, Add to problem report (lands in the next diagnostics bundle), Edit Hub address…. Scripted probes (no network);
// FakeHub only for the paired Hub profile. Screenshots with UZUME_SHOT_DIR.
#include <QClipboard>
#include <QSignalSpy>
#include <QtTest>
#include <memory>

#include "connectioncheck.h"
#include "reportcontroller.h"
#include "diagbundle.h"
#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {

struct Script {
  HubProbeResult hub{true, 22, {}, {}};
  StunProbeResult stun;
  TurnProbeResult turn{true, 31, QStringLiteral("hub.example.com:3478"), {}, {}, {}};
  TestProbeResult test;
  bool slowStun = false;
};

class FakeProbes : public ConnectionProbes {
 public:
  explicit FakeProbes(std::shared_ptr<Script> s) : s_(std::move(s)) {}
  void probeHub(const ConnectionTarget&, std::function<void(const HubProbeResult&)> done) override { done(s_->hub); }
  void probeStun(const ConnectionTarget&, std::function<void(const StunProbeResult&)> done) override {
    if (s_->slowStun) return;  // never answers: the check waits (cancel / close tests)
    done(s_->stun);
  }
  void probeTurn(const ConnectionTarget&, std::function<void(const TurnProbeResult&)> done) override { done(s_->turn); }
  void probeConnection(const ConnectionTarget&, const TestPlan&, std::function<void(const TestProbeResult&)> done) override { done(s_->test); }
  void cancel() override {}

 private:
  std::shared_ptr<Script> s_;
};

StunMapping map(const QString& server, const QString& mapped) {
  StunMapping m;
  m.server = server;
  m.mapped = mapped;
  m.rttMs = 10;
  return m;
}

// The FakeHub hands out no STUN/TURN servers: the check gets these.
void addIceServers(ConnectionTarget& t) {
  TurnServer ts;
  ts.urls = {QStringLiteral("turn:hub.example.com:3478?transport=udp")};
  ts.username = QStringLiteral("u");
  ts.credential = QStringLiteral("p");
  t.stunUrls = {QStringLiteral("stun:stun.example.org:3478")};
  t.turnServers = {ts};
}

std::shared_ptr<Script> directScript() {
  auto s = std::make_shared<Script>();
  s->stun.mappings = {map(QStringLiteral("stun.example.org:3478"), QStringLiteral("198.51.100.4:40000")),
                      map(QStringLiteral("hub.example.com:3478"), QStringLiteral("198.51.100.4:40000"))};
  s->test.ok = true;
  s->test.rttMs = 2;
  s->test.srflxCandidates = 1;
  return s;
}

}  // namespace

class ConnectionCheckUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->sessions()->socket()->isOpen(), 8000);  // the Hub's ICE servers are known
  }
  static QString text(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr ? it->property("text").toString() : QString();
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void directConnectionsPossible() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("sessions_v1")};
    QVERIFY(hub.start());
    auto script = directScript();
    Harness h;
    h.connectionProbes = [script]() { return std::make_unique<FakeProbes>(script); };
    h.connectionTargetPatch = addIceServers;
    QVERIFY(h.start());
    h.window->resize(1280, 900);
    pair(h, hub);
    ConnectionCheckController* c = h.controller->connectionCheck();
    QVERIFY(c->available());
    QVERIFY(!c->isOpen());
    QVERIFY(h.item("connectionCheckDialog") == nullptr || !h.item("connectionCheckDialog")->isVisible());

    c->show();
    QTRY_COMPARE_WITH_TIMEOUT(c->outcome(), QStringLiteral("direct"), 8000);
    QTRY_VERIFY(h.item("connectionCheckDialog") != nullptr && h.item("connectionCheckDialog")->isVisible());
    QCOMPARE(text(h, "connectionCheckTitle"), QStringLiteral("Connection check"));
    QVERIFY(text(h, "connectionCheckSubtitle").startsWith(QStringLiteral("This device · ")));
    for (const char* id : {"hub", "stun", "turn", "nat", "test"}) {
      const QByteArray name = QByteArray("connectionCheckStep_") + id;
      QVERIFY2(h.item(name.constData()) != nullptr, name.constData());
    }
    QCOMPARE(text(h, "connectionCheckResult_turn"), QStringLiteral("Works · 31 ms"));
    QCOMPARE(text(h, "connectionCheckSummaryTitle"), QStringLiteral("Your network allows direct connections"));
    QVERIFY(h.item("connectionCheckEditHub") == nullptr || !h.item("connectionCheckEditHub")->isVisible());
    uitest::saveShot(h.window, QStringLiteral("6e-direct"));

    // Copy result (button and Ctrl+C) puts the plain text on the clipboard.
    QGuiApplication::clipboard()->clear();
    QVERIFY(h.click("connectionCheckCopy"));
    QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("1. Server reachable")));
    QGuiApplication::clipboard()->clear();
    QTest::keyClick(h.window, Qt::Key_C, Qt::ControlModifier);
    QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("Your network allows direct connections")));

    // Add to problem report: kept for the next bundle, once.
    QVERIFY(!c->hasAttachment());
    QVERIFY(h.click("connectionCheckAttach"));
    QVERIFY(c->attached());
    QVERIFY(c->hasAttachment());
    QVERIFY(!h.item("connectionCheckAttach")->isEnabled());
    QVERIFY(!c->lastResultAt().isEmpty());
    QVERIFY(c->lastResultJson().contains(QStringLiteral("\"outcome\": \"direct\"")));

    // The "Report a problem" dialog lists the same result as "Connection check from <time>", checked because it was marked.
    {
      ReportController* rc = h.controller->report();
      rc->show();
      QTRY_COMPARE(rc->state(), QStringLiteral("editing"));
      QVariantMap conn;
      for (const QVariant& v : rc->attachments()) {
        if (v.toMap().value("id").toString() == QLatin1String("connection")) conn = v.toMap();
      }
      QVERIFY(!conn.isEmpty());
      QVERIFY(conn.value("checked").toBool());
      QVERIFY(conn.value("label").toString().startsWith(QStringLiteral("Connection check from ")));
      QVERIFY(rc->includedFiles().contains(QStringLiteral("connection-check.json")));
      rc->close();
    }

    QTemporaryDir out;
    const QString zip = out.filePath(QStringLiteral("bundle.zip"));
    QSignalSpy collected(h.controller.get(), &PlayerController::diagnosticsCollected);
    h.controller->collectDiagnostics(zip);
    QTRY_COMPARE_WITH_TIMEOUT(collected.count(), 1, 10000);
    QVERIFY(collected.first().at(0).toBool());
    QMap<QString, QByteArray> entries;
    QString err;
    QVERIFY2(diag::readZip(zip, &entries, &err), qPrintable(err));
    QVERIFY(entries.contains(QStringLiteral("connection-check.json")));
    QVERIFY(entries.value(QStringLiteral("connection-check.json")).contains("\"outcome\": \"direct\""));
    QVERIFY(!c->hasAttachment());  // taken by that bundle
    collected.clear();
    h.controller->collectDiagnostics(out.filePath(QStringLiteral("second.zip")));
    QTRY_COMPARE_WITH_TIMEOUT(collected.count(), 1, 10000);
    entries.clear();
    QVERIFY(diag::readZip(out.filePath(QStringLiteral("second.zip")), &entries, &err));
    QVERIFY(!entries.contains(QStringLiteral("connection-check.json")));

    // Esc closes the dialog.
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!c->isOpen());
  }

  void onlyRelayedWithKeyRunAgain() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("sessions_v1")};
    QVERIFY(hub.start());
    auto script = directScript();
    script->stun.mappings[1].mapped = QStringLiteral("198.51.100.4:40001");  // symmetric
    script->test.srflxCandidates = 0;
    script->test.rttMs = 48;
    script->test.server = QStringLiteral("hub.example.com:3478");
    Harness h;
    h.connectionProbes = [script]() { return std::make_unique<FakeProbes>(script); };
    h.connectionTargetPatch = addIceServers;
    QVERIFY(h.start());
    h.window->resize(1280, 900);
    pair(h, hub);
    ConnectionCheckController* c = h.controller->connectionCheck();
    c->show();
    QTRY_COMPARE_WITH_TIMEOUT(c->outcome(), QStringLiteral("relayed"), 8000);
    QTRY_VERIFY(h.item("connectionCheckSummaryTitle") != nullptr);
    QCOMPARE(text(h, "connectionCheckSummaryTitle"), QStringLiteral("Only relayed connections are possible here"));
    QCOMPARE(text(h, "connectionCheckResult_nat"), QStringLiteral("Symmetric NAT"));
    uitest::saveShot(h.window, QStringLiteral("6e-relayed"));

    // The network changes; R runs the check again.
    *script = *directScript();
    QTest::keyClick(h.window, Qt::Key_R);
    QTRY_COMPARE_WITH_TIMEOUT(c->outcome(), QStringLiteral("direct"), 8000);
  }

  void hubUnreachableOffersTheHubEdit() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("sessions_v1")};
    QVERIFY(hub.start());
    auto script = directScript();
    script->hub = {false, 10000, QStringLiteral("no answer"), QStringLiteral("timeout")};
    Harness h;
    h.connectionProbes = [script]() { return std::make_unique<FakeProbes>(script); };
    h.connectionTargetPatch = addIceServers;
    QVERIFY(h.start());
    h.window->resize(1280, 900);
    pair(h, hub);
    ConnectionCheckController* c = h.controller->connectionCheck();
    c->show();
    QTRY_COMPARE_WITH_TIMEOUT(c->outcome(), QStringLiteral("hub_unreachable"), 8000);
    QTRY_VERIFY(h.item("connectionCheckEditHub") != nullptr && h.item("connectionCheckEditHub")->isVisible());
    QVERIFY(text(h, "connectionCheckResult_hub").startsWith(QStringLiteral("No answer from ")));
    QCOMPARE(text(h, "connectionCheckResult_stun"), QStringLiteral("Skipped"));
    QVERIFY(text(h, "connectionCheckSummaryTitle").endsWith(QStringLiteral("cannot be reached")));
    uitest::saveShot(h.window, QStringLiteral("6e-hub-unreachable"));

    // Every action stays inside the 600 px card (with wide fonts the row wraps).
    QQuickItem* card = h.item("connectionCheckDialog");
    QVERIFY(card != nullptr);
    const QRectF cardRect = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    for (const char* name : {"connectionCheckCopy", "connectionCheckAttach", "connectionCheckEditHub", "connectionCheckRun"}) {
      QQuickItem* b = h.item(name);
      QVERIFY2(b != nullptr && b->isVisible(), name);
      const QRectF r = b->mapRectToScene(QRectF(0, 0, b->width(), b->height()));
      QVERIFY2(cardRect.contains(r), name);
    }

    QVERIFY(h.click("connectionCheckEditHub"));
    QTRY_VERIFY(!c->isOpen());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("settings"), 4000);
    const QString hubId = h.controller->connection()->hubId();
    const QByteArray editName = ("hubEditAddress_" + hubId).toUtf8();
    QTRY_VERIFY_WITH_TIMEOUT(h.item(editName.constData()) != nullptr && h.item(editName.constData())->isVisible(), 4000);
  }

  void settingsEntryPointsOpenTheDialogAndEscCancels() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("sessions_v1")};
    QVERIFY(hub.start());
    auto script = directScript();
    script->slowStun = true;  // stays on step 2
    Harness h;
    h.connectionProbes = [script]() { return std::make_unique<FakeProbes>(script); };
    h.connectionTargetPatch = addIceServers;
    QVERIFY(h.start());
    h.window->resize(1280, 900);
    pair(h, hub);
    h.controller->showSettingsSection(QStringLiteral("hubs"));
    QTRY_VERIFY_WITH_TIMEOUT(h.item("settingsConnectionCheckButton") != nullptr && h.item("settingsConnectionCheckButton")->isVisible(), 4000);
    QVERIFY(h.item("connectionCheckRow") != nullptr);
    ConnectionCheckController* c = h.controller->connectionCheck();

    QVERIFY(h.click("settingsConnectionCheckButton"));
    QTRY_VERIFY(c->isOpen());
    QTRY_COMPARE_WITH_TIMEOUT(c->progressText(), QStringLiteral("step 2 of 5"), 8000);
    QTRY_VERIFY(h.item("connectionCheckCancel") != nullptr && h.item("connectionCheckCancel")->isVisible());
    QCOMPARE(text(h, "connectionCheckResult_hub").contains(QLatin1Char('·')), true);
    QVERIFY(h.item("connectionCheckCopy") == nullptr || !h.item("connectionCheckCopy")->isVisible());
    uitest::saveShot(h.window, QStringLiteral("6e-running"));

    QTest::keyClick(h.window, Qt::Key_Escape);  // closing cancels
    QTRY_VERIFY(!c->isOpen());
    QTRY_VERIFY(!c->running());
    QCOMPARE(c->outcome(), QStringLiteral("canceled"));
    QVERIFY(!c->canAttach());  // a canceled check is not something to report

    // Diagnostics section row.
    QVERIFY(h.item("connectionCheckRow") != nullptr);
  }
};

UITEST_MAIN(ConnectionCheckUiTest)
#include "connection_check_ui_test.moc"
