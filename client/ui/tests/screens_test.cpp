// UI tests: screens 3a/3b/3c of the Player UI against a fake hub, offscreen.
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;

class ScreensTest : public QObject {
  Q_OBJECT

  static QString screenOf(Harness& h) { return h.controller->screen(); }

  static QVariantMap pairingOf(Harness& h) { return h.controller->pairing(); }

  static HubProfile profile(const QString& id, const QString& name, const QString& address, const QString& pin = QString()) {
    HubProfile p;
    p.hubId = id;
    p.name = name;
    p.address = address;
    p.pinnedFingerprint = pin;
    p.lastConnected = QDateTime(QDate(2026, 10, 4), QTime(18, 40), QTimeZone::utc());
    return p;
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");  // core deliberately not set: detail pane shows "missing"
    uitest::installWarningCounter();
  }

  void init() { uitest::warningCount() = 0; }
  void cleanup() {
    QCOMPARE(uitest::warningCount().load(), 0);  // no QML/Qt warnings during the test
  }

  // 3a empty: input field, footer, no hub saved.
  void connectionScreenEmpty() {
    Harness h;
    QVERIFY(h.start());
    h.controller->onboarding()->setStartScreenEnabled(false);  // the older connection screens
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
    QVERIFY(h.item("addressField") != nullptr);
    QVERIFY(h.controller->deviceFooter().startsWith(QStringLiteral("This device: ")));
    QVERIFY(h.controller->deviceFooter().contains(QStringLiteral("Uzume ")));
    QCOMPARE(h.controller->hubs().size(), 0);
    // Empty address: notice instead of a connection attempt.
    h.controller->addHub(QStringLiteral("  "));
    QVERIFY(!h.controller->connectionNotice().isEmpty());
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
    uitest::saveShot(h.window, QStringLiteral("3a-connection-empty"));
  }

  // Certificate changed: warning with both fingerprints, two-step confirm, Cancel; pin only updated after the confirm.
  void certificateChangeTwoStepConfirm() {
    FakeHub hub(QStringLiteral("b"));
    hub.hubId = QStringLiteral("hub-lena");
    hub.name = QStringLiteral("Studio Lena");
    QVERIFY(hub.start());
    FakeHub former(QStringLiteral("a"));
    Harness h;
    QVERIFY(h.start());
    ProfileStore* ps = h.controller->profileStore();
    QVERIFY(ps->upsertProfile(profile(QStringLiteral("hub-lena"), QStringLiteral("Studio Lena"), hub.address(), former.fingerprint())));

    h.controller->connectProfile(QStringLiteral("hub-lena"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::CertificateChanged);
    QTRY_VERIFY(h.item("certFingerprints") != nullptr && h.item("certFingerprints")->isVisible());
    QVERIFY(h.item("certExpectedFingerprint") != nullptr);
    QVERIFY(h.item("certObservedFingerprint") != nullptr);
    QVERIFY(h.item("certHint") != nullptr);
    QVERIFY(h.item("certTrustNew") != nullptr);
    QVERIFY(h.item("certCancel") != nullptr);
    QVERIFY(!h.item("certConfirmBox")->isVisible());
    QVERIFY(!h.item("certConfirmTrust")->isVisible());
    uitest::saveShot(h.window, QStringLiteral("3a-connection-cert-changed-warning"));

    // Step 1 alone changes nothing.
    QVERIFY(h.click("certTrustNew"));
    QTRY_VERIFY(h.item("certConfirmBox")->isVisible());
    QCOMPARE(ps->profile(QStringLiteral("hub-lena"))->pinnedFingerprint, former.fingerprint());
    QCOMPARE(hub.requests.size(), 0);
    uitest::saveShot(h.window, QStringLiteral("3a-connection-cert-changed-confirm"));
    // Back returns to the first step.
    QVERIFY(h.click("certConfirmBack"));
    QTRY_VERIFY(!h.item("certConfirmBox")->isVisible());
    QCOMPARE(h.controller->connection()->state(), HubConnection::State::CertificateChanged);

    // Cancel: no change to the profile, the attempt is dropped.
    QVERIFY(h.click("certCancel"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Disconnected);
    QCOMPARE(ps->profile(QStringLiteral("hub-lena"))->pinnedFingerprint, former.fingerprint());
    QCOMPARE(hub.requests.size(), 0);

    // Second step: confirm re-pins exactly the presented fingerprint.
    h.controller->connectProfile(QStringLiteral("hub-lena"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::CertificateChanged);
    QVERIFY(h.click("certTrustNew"));
    QTRY_VERIFY(h.item("certConfirmTrust")->isVisible());
    QVERIFY(h.click("certConfirmTrust"));
    QTRY_VERIFY(h.controller->connection()->state() != HubConnection::State::CertificateChanged);
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::NeedsPairing);  // profile has no credential here
    QCOMPARE(ps->profile(QStringLiteral("hub-lena"))->pinnedFingerprint, hub.fingerprint());
  }

  // 3a with cards: saved/ready, certificate changed (blocked), hub too old.
  void connectionScreenCards() {
    FakeHub lena(QStringLiteral("a"));
    lena.hubId = QStringLiteral("hub-lena");
    lena.name = QStringLiteral("Studio Lena");
    QVERIFY(lena.start());
    FakeHub other(QStringLiteral("b"));
    FakeHub old(QStringLiteral("a"));
    old.hubId = QStringLiteral("hub-office");
    old.name = QStringLiteral("Office");
    old.protocolVersion = 2;  // hub requires protocol v2: Player (v1) is too old
    old.minProtocolVersion = 2;
    QVERIFY(old.start());

    Harness h;
    QVERIFY(h.start());
    h.controller->onboarding()->setStartScreenEnabled(false);  // the older connection screens
    ProfileStore* ps = h.controller->profileStore();
    HubProfile home = profile(QStringLiteral("hub-home"), QStringLiteral("Home"), QStringLiteral("https://hub.local:8443"));
    QVERIFY(ps->upsertProfile(home));
    QVERIFY(ps->upsertProfile(profile(QStringLiteral("hub-lena"), QStringLiteral("Studio Lena"), lena.address(), other.fingerprint())));
    QVERIFY(ps->upsertProfile(profile(QStringLiteral("hub-office"), QStringLiteral("Office"), old.address(), old.fingerprint())));
    ps->setLastHubId(QStringLiteral("hub-home"));
    h.controller->setAutoConnect(true);
    QVERIFY(ps->autoConnect());
    QCOMPARE(h.controller->hubs().size(), 3);
    const QVariantMap idle = h.controller->hubs().at(0).toMap();
    QCOMPARE(idle.value(QStringLiteral("status")).toString(), QStringLiteral("idle"));
    QVERIFY(idle.value(QStringLiteral("isLast")).toBool());
    // R-P2: a saved Hub in the idle state is a full-width card with its name and a Connect button with a label.
    {
      QTRY_COMPARE(screenOf(h), QStringLiteral("connection"));
      emit h.controller->hubsChanged();  // the profiles were written to the store directly: refresh the cards
      QTRY_VERIFY2(h.item("hubCard_hub-home") != nullptr, qPrintable(QStringLiteral("hubConnectButton items: %1, hubCardName: %2").arg(h.items("hubConnectButton").size()).arg(h.items("hubCardName").size())));
      QQuickItem* card = h.item("hubCard_hub-home");
      QQuickTest::qWaitForPolish(h.window);
      QVERIFY2(card->width() >= 400, qPrintable(QStringLiteral("card width %1").arg(card->width())));
      QVERIFY(card->height() >= 90);
      QQuickItem* name = Harness::findVisual(card, QStringLiteral("hubCardName"));
      QVERIFY(name != nullptr && name->isVisible() && name->width() > 40);
      QQuickItem* connect = Harness::findVisual(card, QStringLiteral("hubConnectButton"));
      QVERIFY(connect != nullptr && connect->isVisible());
      QVERIFY2(connect->width() >= 70, qPrintable(QStringLiteral("Connect button width %1").arg(connect->width())));
      uitest::saveShot(h.window, QStringLiteral("3a-connection-idle-card"));
    }

    // Certificate changed: blocked, no Connect action, both fingerprints present.
    h.controller->connectProfile(QStringLiteral("hub-lena"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::CertificateChanged);
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
    QVariantMap card;
    for (const QVariant& v : h.controller->hubs()) {
      if (v.toMap().value(QStringLiteral("hubId")).toString() == QLatin1String("hub-lena")) card = v.toMap();
    }
    QCOMPARE(card.value(QStringLiteral("status")).toString(), QStringLiteral("certChanged"));
    QCOMPARE(card.value(QStringLiteral("tone")).toString(), QStringLiteral("error"));
    QVERIFY(!card.value(QStringLiteral("expectedFingerprint")).toString().isEmpty());
    QVERIFY(!card.value(QStringLiteral("observedFingerprint")).toString().isEmpty());
    QVERIFY(card.value(QStringLiteral("expectedFingerprint")) != card.value(QStringLiteral("observedFingerprint")));
    QVERIFY(h.controller->connection()->state() != HubConnection::State::NeedsPairing);  // never silently accepted
    uitest::saveShot(h.window, QStringLiteral("3a-connection-cert-changed"));

    // Player too old (hub requires a newer protocol)
    h.controller->connectProfile(QStringLiteral("hub-office"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Incompatible);
    for (const QVariant& v : h.controller->hubs()) {
      if (v.toMap().value(QStringLiteral("hubId")).toString() == QLatin1String("hub-office")) card = v.toMap();
    }
    QCOMPARE(card.value(QStringLiteral("status")).toString(), QStringLiteral("incompatible"));
    QCOMPARE(card.value(QStringLiteral("statusText")).toString(), QStringLiteral("App too old"));
    uitest::saveShot(h.window, QStringLiteral("3a-connection-incompatible"));

    // Remove works on every card (including the blocked one).
    h.controller->removeHub(QStringLiteral("hub-lena"));
    QCOMPARE(h.controller->hubs().size(), 2);
    QVERIFY(!ps->profile(QStringLiteral("hub-lena")).has_value());
  }

  // 3a: not reachable -> card for the attempted address with Retry.
  void unreachableAttempt() {
    Harness h;
    QVERIFY(h.start());
    h.controller->onboarding()->setStartScreenEnabled(false);  // the older connection screens
    h.controller->addHub(QStringLiteral("http://127.0.0.1:1"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Unreachable);
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
    const QVariantList hubs = h.controller->hubs();
    QCOMPARE(hubs.size(), 1);
    const QVariantMap c = hubs.at(0).toMap();
    QCOMPARE(c.value(QStringLiteral("status")).toString(), QStringLiteral("unreachable"));
    QVERIFY(!c.value(QStringLiteral("saved")).toBool());
    QVERIFY(!c.value(QStringLiteral("message")).toString().isEmpty());
    uitest::saveShot(h.window, QStringLiteral("3a-connection-unreachable"));
    h.controller->removeHub(QString());  // discard attempt
    QCOMPARE(h.controller->hubs().size(), 0);
  }

  // Regression: every attempt (addHub, Retry) shows the pairing/status phase, not just the first.
  void unreachableTwiceShowsStatusScreen() {
    Harness h;
    QVERIFY(h.start());
    h.controller->onboarding()->setStartScreenEnabled(false);  // the older connection screens
    for (int i = 0; i < 2; ++i) {
      h.controller->addHub(QStringLiteral("http://127.0.0.1:1"));
      QCOMPARE(screenOf(h), QStringLiteral("pairing"));
      QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Unreachable);
      QCOMPARE(screenOf(h), QStringLiteral("connection"));
    }
    h.controller->retryConnection();
    QCOMPARE(screenOf(h), QStringLiteral("pairing"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Unreachable);
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
  }

  // Retry after connectProfile (saved profile) never shows the pairing status screen.
  void retryForSavedProfileStaysOnConnection() {
    Harness h;
    QVERIFY(h.start());
    QVERIFY(h.controller->profileStore()->upsertProfile(
        profile(QStringLiteral("hub-gone"), QStringLiteral("Gone"), QStringLiteral("http://127.0.0.1:1"))));
    h.controller->connectProfile(QStringLiteral("hub-gone"));
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Unreachable);
    h.controller->retryConnection();
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Unreachable);
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
  }

  // addHub on an already paired address (profile with credential): Retry stays on "connection".
  void retryAfterAddHubOfPairedProfileStaysOnConnection() {
    Harness h;
    QVERIFY(h.start());
    HubProfile p = profile(QStringLiteral("hub-paired"), QStringLiteral("Paired"), QStringLiteral("http://127.0.0.1:1"));
    p.credentialRef = QStringLiteral("uzume/test-credential");
    QVERIFY(h.controller->profileStore()->upsertProfile(p));
    h.controller->addHub(QStringLiteral("http://127.0.0.1:1"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Unreachable);
    h.controller->retryConnection();
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Unreachable);
    QCOMPARE(screenOf(h), QStringLiteral("connection"));
  }

  // 3b + 3c: TOFU confirmation, approval (waiting, denied, approved), Library.
  void pairingAndLibrary() {
    const QByteArray readyRom = "homebrew-dummy-rom-ready";
    const QByteArray badRom = "this-is-not-the-expected-content";
    const QString readySha = uitest::sha256Hex(readyRom);
    const QString badSha = uitest::sha256Hex(QByteArray("expected-but-never-served"));

    FakeHub hub(QStringLiteral("a"));
    hub.hubId = QStringLiteral("hub-home");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Pending;
    QJsonArray games;
    games.append(uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"), readySha, readyRom.size()));
    games.append(uitest::gameJson(QStringLiteral("g2"), QStringLiteral("Paper Wizards"), uitest::sha256Hex("pw"), 128LL * 1024 * 1024));
    games.append(uitest::gameJson(QStringLiteral("g3"), QStringLiteral("Orbit Gardens"), uitest::sha256Hex("og"), 16LL * 1024 * 1024));
    games.append(uitest::gameJson(QStringLiteral("g4"), QStringLiteral("Clocktower Kids"), uitest::sha256Hex("ck"), 24LL * 1024 * 1024));
    games.append(uitest::gameJson(QStringLiteral("g5"), QStringLiteral("Copper Courier"), badSha, badRom.size()));
    games.append(uitest::gameJson(QStringLiteral("g6"), QStringLiteral("Stylus Knights"), uitest::sha256Hex("sk"), 32LL * 1024 * 1024));
    hub.games = QJsonObject{{QStringLiteral("games"), games}};
    hub.roms.insert(badSha, badRom);
    QVERIFY(hub.start());

    Harness h;
    QVERIFY(h.start());
    h.controller->onboarding()->setStartScreenEnabled(false);  // the older connection screens
    // Lumen Drift is already in the cache (validated).
    {
      QFile f(QDir(h.controller->profileStore()->romCacheDir()).filePath(readySha + QStringLiteral(".nds")));
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(readyRom);
    }

    h.controller->addHub(hub.address());
    QTRY_COMPARE(screenOf(h), QStringLiteral("pairing"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation);

    // Step 2: fingerprint is shown and must be confirmed explicitly.
    QVariantMap p = pairingOf(h);
    QCOMPARE(p.value(QStringLiteral("phase")).toString(), QStringLiteral("trust"));
    QCOMPARE(p.value(QStringLiteral("hubName")).toString(), QStringLiteral("Home"));
    QCOMPARE(p.value(QStringLiteral("fingerprint")).toString().remove(QLatin1Char('\n')).remove(QLatin1Char(':')),
             PlayerController::formatFingerprint(hub.fingerprint()).remove(QLatin1Char('\n')).remove(QLatin1Char(':')));
    QCOMPARE(p.value(QStringLiteral("fingerprint")).toString().count(QLatin1Char('\n')), 1);
    QVERIFY(h.item("trustButton") != nullptr);
    QVERIFY(h.item("fingerprintText") != nullptr);
    QTest::qWait(100);
    QCOMPARE(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation);  // no silent trust
    uitest::saveShot(h.window, QStringLiteral("3b-pairing-trust"));

    QVERIFY(h.click("trustButton"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::NeedsPairing);
    QCOMPARE(pairingOf(h).value(QStringLiteral("phase")).toString(), QStringLiteral("needsPairing"));
    QVERIFY(h.controller->profileStore()->profile(QStringLiteral("hub-home")).has_value());
    QCOMPARE(h.controller->profileStore()->profile(QStringLiteral("hub-home"))->pinnedFingerprint, hub.fingerprint());

    // Step 3: request approval -> waiting.
    QVERIFY(h.click("requestButton"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::AwaitingApproval);
    p = pairingOf(h);
    QCOMPARE(p.value(QStringLiteral("phase")).toString(), QStringLiteral("awaiting"));
    QVERIFY(!p.value(QStringLiteral("deviceName")).toString().isEmpty());
    QVERIFY(!p.value(QStringLiteral("platform")).toString().isEmpty());
    QVERIFY(!p.value(QStringLiteral("playerVersion")).toString().isEmpty());
    uitest::saveShot(h.window, QStringLiteral("3b-pairing-waiting"));

    // Cancel request -> back to "Request approval"
    QVERIFY(h.click("cancelRequestButton"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::NeedsPairing);

    // Denied
    hub.decision = FakeHub::Decision::Deny;
    QVERIFY(h.click("requestButton"));
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Denied);
    QCOMPARE(pairingOf(h).value(QStringLiteral("phase")).toString(), QStringLiteral("denied"));
    QVERIFY(h.item("pairingProblem") != nullptr && h.item("pairingProblem")->isVisible());
    uitest::saveShot(h.window, QStringLiteral("3b-pairing-denied"));

    // Expired
    hub.decision = FakeHub::Decision::Expire;
    QVERIFY(h.click("requestButton"));  // "Request again"
    QTRY_COMPARE(h.controller->connection()->state(), HubConnection::State::Expired);
    QCOMPARE(pairingOf(h).value(QStringLiteral("phase")).toString(), QStringLiteral("expired"));
    uitest::saveShot(h.window, QStringLiteral("3b-pairing-expired"));

    // Approved -> Library
    hub.decision = FakeHub::Decision::Approve;
    QVERIFY(h.click("requestButton"));
    QTRY_COMPARE_WITH_TIMEOUT(screenOf(h), QStringLiteral("library"), 8000);
    QTRY_COMPARE(h.controller->libraryState(), QStringLiteral("ready"));
    LibraryModel* lib = h.controller->library();
    QCOMPARE(lib->totalCount(), 6);
    QCOMPARE(h.controller->hubName(), QStringLiteral("Home"));
    QTRY_COMPARE(lib->readyCount(), 1);  // Lumen Drift after asynchronous verification
    QCOMPARE(h.controller->selectedGameId(), h.controller->library()->data(h.controller->library()->index(0), LibraryModel::GameIdRole).toString());  // first game in the current sort preselected

    // Make hash mismatch visible (hub delivers wrong content).
    h.controller->downloader()->ensureRom(h.controller->hubLibrary()->games().at(4));
    QTRY_COMPARE(lib->data(lib->index(lib->rowOfGame(QStringLiteral("g5"))), LibraryModel::StateKindRole).toString(),
                 QStringLiteral("mismatch"));

    // Detail pane: core missing (path hint), firmware not required, Play disabled.
    h.controller->selectGame(QStringLiteral("g1"));  // the preselected game follows the sort order
    QVariantMap g = h.controller->selectedGame();
    QCOMPARE(g.value(QStringLiteral("title")).toString(), QStringLiteral("Lumen Drift"));
    QCOMPARE(g.value(QStringLiteral("systemName")).toString(), QStringLiteral("Nintendo DS"));
    QCOMPARE(g.value(QStringLiteral("romText")).toString(), QStringLiteral("Cached locally · verified"));
    QVERIFY(g.value(QStringLiteral("coreText")).toString().contains(QStringLiteral("missing")));
    QVERIFY(!g.value(QStringLiteral("coreHint")).toString().contains(QStringLiteral("UZUME_")));  // no developer env var in the UI
    QCOMPARE(g.value(QStringLiteral("firmwareText")).toString(), QStringLiteral("Not required"));
    QVERIFY(!g.value(QStringLiteral("canPlay")).toBool());
    QCOMPARE(g.value(QStringLiteral("checklist")).toList().size(), 5);
    QCOMPARE(g.value(QStringLiteral("sha")).toString(), readySha);
    QVERIFY(g.value(QStringLiteral("cachePath")).toString().endsWith(readySha + QStringLiteral(".nds")));

    // Selection, search, chips
    h.controller->selectGame(QStringLiteral("g2"));
    g = h.controller->selectedGame();
    QCOMPARE(g.value(QStringLiteral("title")).toString(), QStringLiteral("Paper Wizards"));
    QCOMPARE(g.value(QStringLiteral("romText")).toString(), QStringLiteral("Download needed · 128 MB"));
    // R-P1: the status rows of a game that is not downloaded stay compact (a 1 px wide wrapped value once made them 300 px high).
    QVERIFY(h.click("tab_Details"));  // the facts (ROM, core, firmware) are the Details tab
    QTRY_VERIFY(h.items("detailRowValue").size() >= 3);
    QTRY_VERIFY(([&h]() {
      const QList<QQuickItem*> values = h.items("detailRowValue");
      for (QQuickItem* v : values) {
        if (!v->isVisible()) continue;
        QQuickItem* row = v->parentItem()->parentItem()->parentItem();  // label -> RowLayout -> rowCol -> row
        const bool wide = qEnvironmentVariableIntValue("UZUME_TEST_WIDE_FONT") != 0;  // wide fonts wrap the value onto more lines
        if (row->height() > (wide ? 400 : 110) || v->height() > (wide ? 120 : 70)) return false;
      }
      return true;
    })());
    // R-P13: the START step names the state ("Core missing"), not "Core ready"; the same wording as the Core row.
    {
      const QVariantList chk = g.value(QStringLiteral("checklist")).toList();
      bool found = false;
      for (const QVariant& c : chk) {
        const QString label = c.toMap().value(QStringLiteral("label")).toString();
        QVERIFY2(label != QLatin1String("Core ready"), "core step claims ready while the core is missing");
        found = found || label == QLatin1String("Core missing");
      }
      QVERIFY(found);
    }
    h.controller->selectGame(QStringLiteral("g1"));
    lib->setFilterText(QStringLiteral("ts"));  // Stylus Knights
    QCOMPARE(lib->rowCount(), 1);
    lib->setFilterText(QString());
    // Chips (D14): without a core every game of the system "needs attention" (core missing); counts add up to the total.
    QCOMPARE(lib->counts().value(QStringLiteral("attention")).toInt(), 6);
    QCOMPARE(lib->counts().value(QStringLiteral("ready")).toInt(), 0);
    lib->setFilter(QStringLiteral("attention"));
    QCOMPARE(lib->rowCount(), 6);
    QCOMPARE(lib->data(lib->index(lib->rowOfGame(QStringLiteral("g1"))), LibraryModel::TileTextRole).toString(), QStringLiteral("▲ Core missing"));
    lib->setFilter(QStringLiteral("all"));
    // D15: the core appears (as after a download) and the state is re-evaluated without a restart.
    qputenv("UZUME_MELONDS_DS_CORE", QCoreApplication::applicationFilePath().toLocal8Bit());
    h.controller->refreshCoreState();
    QVERIFY(h.controller->selectedGame().value(QStringLiteral("coreText")).toString().contains(QStringLiteral("ready")));
    QCOMPARE(lib->counts().value(QStringLiteral("attention")).toInt(), 1);  // Copper Courier: hash mismatch
    QCOMPARE(lib->counts().value(QStringLiteral("ready")).toInt(), 1);
    QCOMPARE(lib->counts().value(QStringLiteral("download")).toInt(), 4);
    lib->setReadyOnly(true);
    QCOMPARE(lib->rowCount(), 1);
    lib->setFilter(QStringLiteral("download"));
    QCOMPARE(lib->rowCount(), 4);
    lib->setFilter(QStringLiteral("attention"));
    QCOMPARE(lib->rowCount(), 1);
    lib->setFilterText(QStringLiteral("zzz"));  // chips combine with the search
    QCOMPARE(lib->rowCount(), 0);
    lib->setFilterText(QString());
    lib->setReadyOnly(false);
    QCOMPARE(lib->rowCount(), 6);
    qunsetenv("UZUME_MELONDS_DS_CORE");
    QVERIFY(h.item("gameGrid") != nullptr);
    QVERIFY(h.item("searchField") != nullptr);
    uitest::saveShot(h.window, QStringLiteral("3c-library"));

    // R-P4: chips, tiles and sidebar entries are Tab stops and work with Enter / Space.
    {
      QQuickItem* status = h.item("statusChip_attention");
      QVERIFY(status != nullptr && status->activeFocusOnTab());

      QTRY_VERIFY(h.item("gameTile_g3") != nullptr);
      QQuickItem* tile = h.item("gameTile_g3");
      QVERIFY(tile->activeFocusOnTab());
      tile->forceActiveFocus();
      QTest::keyClick(h.window, Qt::Key_Return);
      QCOMPARE(h.controller->selectedGameId(), QStringLiteral("g3"));
      QVERIFY(h.item("tileFocusRing")->isVisible());

      QQuickItem* nav = h.item("navSettings");
      QVERIFY(nav != nullptr && nav->activeFocusOnTab());
      nav->forceActiveFocus();
      QTest::keyClick(h.window, Qt::Key_Return);
      QTRY_COMPARE(screenOf(h), QStringLiteral("settings"));
      QQuickItem* back = h.item("navLibrary");
      QVERIFY(back != nullptr && back->activeFocusOnTab());
      back->forceActiveFocus();
      QTest::keyClick(h.window, Qt::Key_Space);
      QTRY_COMPARE(screenOf(h), QStringLiteral("library"));
    }

    // Switch hub = disconnect and back to 3a.
    QVERIFY(h.click("serverMenuButton"));
    QTRY_VERIFY(h.item("menuItem_switchServer") != nullptr);
    QVERIFY(h.click("menuItem_switchServer"));
    QTRY_COMPARE(screenOf(h), QStringLiteral("connection"));
    QCOMPARE(lib->totalCount(), 0);
    QCOMPARE(h.controller->hubs().size(), 1);
    QVERIFY(h.controller->hubs().at(0).toMap().value(QStringLiteral("saved")).toBool());
    // R-P2: the saved Hub card, created while the Library was shown, is a full-width card with name and a labelled Connect button.
    QTRY_VERIFY(h.item("hubCard_hub-home") != nullptr);
    {
      QQuickItem* card = h.item("hubCard_hub-home");
      QTRY_VERIFY2(card->width() >= 400, qPrintable(QStringLiteral("card width %1").arg(card->width())));
      QQuickItem* name = Harness::findVisual(card, QStringLiteral("hubCardName"));
      QVERIFY(name != nullptr && name->width() > 40);
      QQuickItem* connect = Harness::findVisual(card, QStringLiteral("hubConnectButton"));
      QVERIFY(connect != nullptr && connect->width() >= 70);
    }
  }

  void autoConnectIsProfileSetting() {
    Harness h;
    QVERIFY(h.start());
    QVERIFY(!h.controller->autoConnect());
    h.controller->setAutoConnect(true);
    QVERIFY(h.controller->autoConnect());
    ProfileStore reread(h.dir.path());
    QVERIFY(reread.autoConnect());
  }

  void fingerprintFormat() {
    const QString fp = QStringLiteral("AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89");
    const QString f = PlayerController::formatFingerprint(fp);
    QCOMPARE(f.count(QLatin1Char('\n')), 1);
    QCOMPARE(f.section(QLatin1Char('\n'), 0, 0).count(QLatin1Char(':')), 15);
    QCOMPARE(f.section(QLatin1Char('\n'), 1, 1).count(QLatin1Char(':')), 15);
    QCOMPARE(PlayerController::platformLabel(QStringLiteral("windows"), QStringLiteral("x86_64")), QStringLiteral("Windows x86-64"));
  }
};

UITEST_MAIN(ScreensTest)
#include "screens_test.moc"
