// UI tests (offscreen, FakeHub): invite redemption, user_disabled, upload, firmware state, core warnings,
// Settings/appearance. Dummy bytes only (no real ROM/BIOS/firmware); no real core needed.
#include <QSignalSpy>
#include <QtTest>

#include "corecache.h"
#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
QJsonObject fwFile(const QString& id, const QString& name, bool required, const QByteArray& content) {
  return {{QStringLiteral("id"), id},
          {QStringLiteral("display_name"), name},
          {QStringLiteral("required"), required},
          {QStringLiteral("present"), !content.isEmpty()},
          {QStringLiteral("size"), content.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(content.size())},
          {QStringLiteral("sha256"), content.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(uitest::sha256Hex(content))}};
}
QJsonObject ndsSystem(const QString& mode, const QJsonArray& files) {
  return {{QStringLiteral("systems"),
           QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("nds")},
                                  {QStringLiteral("display_name"), QStringLiteral("Nintendo DS")},
                                  {QStringLiteral("preferred_core_id"), QStringLiteral("melonds_ds")},
                                  {QStringLiteral("expected_core_version"), QStringLiteral("1.4.0")},
                                  {QStringLiteral("firmware_mode"), mode},
                                  {QStringLiteral("firmware"), files}}}}};
}
}  // namespace

class HubFeaturesUiTest : public QObject {
  Q_OBJECT

  static QVariantMap game(Harness& h) { return h.controller->selectedGame(); }

  // Pairs directly (approve) and waits for the library.
  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
  }

  static void setGames(FakeHub& hub, const QByteArray& rom) {
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    hub.games = QJsonObject{{QStringLiteral("games"), QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"),
                                                                                    uitest::sha256Hex(rom), rom.size())}}};
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // 3b: "I have an invite code" -> code + display name -> paired immediately (200), errors are clear.
  void inviteRedemption() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("users_v1")};
    hub.takenNames = {QStringLiteral("Lena")};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.controller->onboarding()->setStartScreenEnabled(false);  // the older connection screens
    h.controller->addHub(hub.address());
    QTRY_COMPARE(h.controller->connection()->state(), S::NeedsTrustConfirmation);
    QVERIFY(h.click("trustButton"));
    QTRY_COMPARE(h.controller->connection()->state(), S::NeedsPairing);

    QVERIFY(h.click("inviteSegment"));
    QVERIFY(h.item("inviteCodeField") != nullptr);
    QVERIFY(h.item("inviteCodeField")->isVisible());
    QVERIFY(h.item("redeemButton")->isVisible());
    QVERIFY(!h.item("requestButton")->isVisible());
    uitest::saveShot(h.window, QStringLiteral("5-invite-form"));

    // Wrong code
    h.item("inviteCodeField")->setProperty("text", QStringLiteral("FB-WRONG-CODE"));
    h.item("inviteNameField")->setProperty("text", QStringLiteral("Anna"));
    QVERIFY(h.click("redeemButton"));
    QTRY_VERIFY(pairingError(h).contains(QStringLiteral("not valid")));
    QCOMPARE(h.controller->screen(), QStringLiteral("pairing"));
    QVERIFY(!h.controller->pairing().value(QStringLiteral("inviteBusy")).toBool());
    uitest::saveShot(h.window, QStringLiteral("5-invite-invalid"));

    // Taken display name
    h.item("inviteCodeField")->setProperty("text", QStringLiteral("fb-test-code"));
    h.item("inviteNameField")->setProperty("text", QStringLiteral("lena"));
    QVERIFY(h.click("redeemButton"));
    QTRY_VERIFY(pairingError(h).contains(QStringLiteral("already taken")));

    // Rate limit
    hub.rateLimitInvites = true;
    QVERIFY(h.click("redeemButton"));
    QTRY_VERIFY(pairingError(h).contains(QStringLiteral("Too many attempts")));
    hub.rateLimitInvites = false;

    // Missing input is explained locally.
    h.item("inviteNameField")->setProperty("text", QString());
    QVERIFY(h.click("redeemButton"));
    QTRY_VERIFY(pairingError(h).contains(QStringLiteral("display name")));

    // Success: paired immediately, Library shown.
    h.item("inviteNameField")->setProperty("text", QStringLiteral("Anna"));
    QVERIFY(h.click("redeemButton"));
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
    QCOMPARE(h.controller->profileStore()->profile(hub.hubId)->hubUserId, QStringLiteral("u_invited_1"));
    QCOMPARE(hub.lastInviteBody.value(QStringLiteral("display_name")).toString(), QStringLiteral("Anna"));
  }

  // 202: the invite leads into the existing waiting/poll flow.
  void invitePendingWaitsForApproval() {
    FakeHub hub(QStringLiteral("a"));
    hub.inviteDirect = false;
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.controller->addHub(hub.address());
    QTRY_COMPARE(h.controller->connection()->state(), S::NeedsTrustConfirmation);
    h.controller->confirmTrust();
    QTRY_COMPARE(h.controller->connection()->state(), S::NeedsPairing);
    h.controller->redeemInvite(QStringLiteral("FB-TEST-CODE"), QStringLiteral("Anna"));
    QTRY_COMPARE(h.controller->connection()->state(), S::AwaitingApproval);
    QCOMPARE(h.controller->pairing().value(QStringLiteral("phase")).toString(), QStringLiteral("awaiting"));
    hub.decision = FakeHub::Decision::Approve;
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
  }

  // user_disabled: clear state on the connection screen, no retry loop.
  void userDisabledState() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->switchHub();
    hub.userDisabled = true;
    h.controller->connectProfile(hub.hubId);
    QTRY_COMPARE(h.controller->connection()->state(), S::UserDisabled);
    QCOMPARE(h.controller->screen(), QStringLiteral("connection"));
    QVariantMap card;
    for (const QVariant& v : h.controller->hubs()) {
      if (v.toMap().value(QStringLiteral("hubId")).toString() == hub.hubId) card = v.toMap();
    }
    QCOMPARE(card.value(QStringLiteral("status")).toString(), QStringLiteral("userDisabled"));
    QVERIFY(card.value(QStringLiteral("message")).toString().contains(QStringLiteral("This user is disabled on the server")));
    QCOMPARE(card.value(QStringLiteral("tone")).toString(), QStringLiteral("error"));
    const int tokens = hub.tokenRequests();
    QTest::qWait(500);
    QCOMPARE(hub.tokenRequests(), tokens);
    uitest::saveShot(h.window, QStringLiteral("5-user-disabled"));
    hub.userDisabled = false;
    h.controller->retryConnection();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
  }

  // Upload: button only with uploads_v1; 201 selects the new game, 409 says "Already in the library" and selects it.
  void uploadFlow() {
    const QByteArray rom = "homebrew-dummy-rom-one";
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("uploads_v1")};
    setGames(hub, rom);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QVERIFY(h.controller->canUpload());
    QVERIFY(h.item("uploadButton") != nullptr && h.item("uploadButton")->isVisible());

    QTemporaryDir files;
    QFile f(files.filePath(QStringLiteral("New Game.nds")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(200000, 'n') + "DUMMY");
    f.close();
    h.controller->uploadRom(QUrl::fromLocalFile(f.fileName()).toString());
    QVERIFY(h.controller->upload().value(QStringLiteral("active")).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!h.controller->upload().value(QStringLiteral("active")).toBool(), 8000);
    QVERIFY(!h.controller->upload().value(QStringLiteral("isError")).toBool());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->selectedGameId(), QStringLiteral("up-1"), 8000);
    QCOMPARE(h.controller->library()->totalCount(), 2);
    QCOMPARE(hub.uploads.first().filename, QStringLiteral("New Game.nds"));
    uitest::saveShot(h.window, QStringLiteral("5-upload-done"));

    // Same file again: duplicate, the existing game is selected.
    h.controller->selectGame(QStringLiteral("g1"));
    h.controller->uploadRom(f.fileName());
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->upload().value(QStringLiteral("message")).toString() == QLatin1String("Already in the library."), 8000);
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("up-1"));
    QVERIFY(!h.controller->upload().value(QStringLiteral("isError")).toBool());

    // Forbidden
    hub.uploadsAllowed = false;
    QFile g(files.filePath(QStringLiteral("Other.nds")));
    QVERIFY(g.open(QIODevice::WriteOnly));
    g.write("other-dummy");
    g.close();
    h.controller->uploadRom(g.fileName());
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->upload().value(QStringLiteral("isError")).toBool(), 8000);
    QVERIFY(h.controller->upload().value(QStringLiteral("message")).toString().contains(QStringLiteral("not allowed")));
  }

  void noUploadWithoutFeature() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1")};
    setGames(hub, "homebrew-dummy-rom-one");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QVERIFY(!h.controller->canUpload());
    QVERIFY(h.item("uploadButton") == nullptr || !h.item("uploadButton")->isVisible());
    h.controller->uploadRom(QStringLiteral("/nonexistent.nds"));  // ignored
    QVERIFY(!h.controller->upload().value(QStringLiteral("active")).toBool());
  }

  // Firmware mode native with a required file missing on the Hub: "Firmware required/missing", launch blocked.
  void firmwareMissingBlocksLaunch() {
    const QByteArray rom = "homebrew-dummy-rom-one";
    const QByteArray b7(16, '7');
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("firmware_v1")};
    setGames(hub, rom);
    hub.systems = ndsSystem(QStringLiteral("native"), QJsonArray{fwFile(QStringLiteral("bios7"), QStringLiteral("ARM7 BIOS"), true, b7),
                                                                 fwFile(QStringLiteral("bios9"), QStringLiteral("ARM9 BIOS"), true, {}),
                                                                 fwFile(QStringLiteral("firmware"), QStringLiteral("DS Firmware"), true, {})});
    hub.firmwareFiles = {{QStringLiteral("nds/bios7"), b7}};
    QVERIFY(hub.start());
    qputenv("UZUME_MELONDS_DS_CORE", QCoreApplication::applicationFilePath().toLocal8Bit());  // "found" core, never loaded
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->hubSystems()->state() == HubSystems::State::Ready, 8000);
    QTRY_COMPARE(game(h).value(QStringLiteral("firmwareText")).toString(), QStringLiteral("Firmware required/missing"));
    const QString hint = game(h).value(QStringLiteral("firmwareHint")).toString();
    QVERIFY2(hint.contains(QStringLiteral("ARM9 BIOS")) && hint.contains(QStringLiteral("DS Firmware")) && !hint.contains(QStringLiteral("ARM7")), qPrintable(hint));
    QCOMPARE(game(h).value(QStringLiteral("firmwareTone")).toString(), QStringLiteral("error"));
    QVERIFY(!game(h).value(QStringLiteral("canPlay")).toBool());
    QVERIFY(h.item("firmwareRecheck") != nullptr && h.item("firmwareRecheck")->isVisible());
    uitest::saveShot(h.window, QStringLiteral("5-firmware-missing"));

    h.controller->playSelected();
    QTest::qWait(300);
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
    QCOMPARE(hub.firmwareDownloads, 0);
    QCOMPARE(hub.count(QStringLiteral("/api/v1/roms/")), 0);  // blocked before anything is downloaded

    // The admin provides the files; "Check again" releases the launch.
    const QByteArray b9(8, '9'), fw(32, 'f');
    hub.systems = ndsSystem(QStringLiteral("native"), QJsonArray{fwFile(QStringLiteral("bios7"), QStringLiteral("ARM7 BIOS"), true, b7),
                                                                 fwFile(QStringLiteral("bios9"), QStringLiteral("ARM9 BIOS"), true, b9),
                                                                 fwFile(QStringLiteral("firmware"), QStringLiteral("DS Firmware"), true, fw)});
    h.controller->recheckFirmware();
    QTRY_VERIFY_WITH_TIMEOUT(game(h).value(QStringLiteral("canPlay")).toBool(), 8000);
    QVERIFY(game(h).value(QStringLiteral("firmwareText")).toString().startsWith(QStringLiteral("From server")));
    qunsetenv("UZUME_MELONDS_DS_CORE");
  }

  // Native: files are downloaded, validated and materialized under the names the core reads; builtin: nothing.
  void firmwareNativeMaterializesBuiltinDoesNot() {
    const QByteArray rom = "homebrew-dummy-rom-one";
    const QByteArray b7(16, '7'), b9(8, '9'), fw(32, 'f');
    for (const bool native : {false, true}) {
      FakeHub hub(QStringLiteral("a"));
      hub.features = {QStringLiteral("saves_v1"), QStringLiteral("firmware_v1")};
      setGames(hub, rom);
      hub.systems = ndsSystem(native ? QStringLiteral("native") : QStringLiteral("builtin"),
                              QJsonArray{fwFile(QStringLiteral("bios7"), QStringLiteral("ARM7 BIOS"), native, b7),
                                         fwFile(QStringLiteral("bios9"), QStringLiteral("ARM9 BIOS"), native, b9),
                                         fwFile(QStringLiteral("firmware"), QStringLiteral("DS Firmware"), native, fw)});
      hub.firmwareFiles = {{QStringLiteral("nds/bios7"), b7}, {QStringLiteral("nds/bios9"), b9}, {QStringLiteral("nds/firmware"), fw}};
      QVERIFY(hub.start());
      qputenv("UZUME_MELONDS_DS_CORE", QCoreApplication::applicationFilePath().toLocal8Bit());  // not a core: the start fails later
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      QTRY_VERIFY_WITH_TIMEOUT(h.controller->hubSystems()->state() == HubSystems::State::Ready, 8000);
      QTRY_VERIFY_WITH_TIMEOUT(game(h).value(QStringLiteral("canPlay")).toBool(), 8000);
      h.controller->playSelected();
      const QString sys = QDir(h.controller->profileStore()->baseDir()).filePath(QStringLiteral("system"));
      if (native) {
        QTRY_COMPARE_WITH_TIMEOUT(hub.firmwareDownloads, 3, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(sys + QStringLiteral("/firmware.bin")), 8000);
        QFile f(sys + QStringLiteral("/bios7.bin"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), b7);
        QVERIFY(QFile::exists(sys + QStringLiteral("/bios9.bin")));
        QVERIFY(QFile::exists(sys + QStringLiteral("/firmware/nds/") + uitest::sha256Hex(fw)));  // cache, separate from ROMs
      } else {
        QTest::qWait(500);
        QCOMPARE(hub.firmwareDownloads, 0);
        QVERIFY(!QFile::exists(sys + QStringLiteral("/bios7.bin")));
      }
      QTRY_VERIFY_WITH_TIMEOUT(hub.count(QStringLiteral("/api/v1/roms/")) >= 1, 8000);  // ROM only after firmware
      QTest::qWait(300);
      qunsetenv("UZUME_MELONDS_DS_CORE");
    }
  }

  // Old Hub without firmware_v1 behaves as builtin: no /systems request, no firmware row problem.
  void noFirmwareFeatureIsBuiltin() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1")};
    setGames(hub, "homebrew-dummy-rom-one");
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTest::qWait(150);
    QCOMPARE(hub.count(QStringLiteral("/api/v1/systems")), 0);
    QCOMPARE(game(h).value(QStringLiteral("firmwareText")).toString(), QStringLiteral("Not required"));
    QVERIFY(!game(h).value(QStringLiteral("firmwareBlocked")).toBool());
  }

  // Handshake problems about the core: non-blocking warning in the library.
  void coreWarningsAreShown() {
    FakeHub hub(QStringLiteral("a"));
    hub.handshakeExtra = QJsonObject{{QStringLiteral("problems"),
                                      QJsonArray{QJsonObject{{QStringLiteral("code"), QStringLiteral("core_version_mismatch")},
                                                             {QStringLiteral("detail"), QStringLiteral("Core melonds_ds 1.3.0, expected 1.4.0")},
                                                             {QStringLiteral("core_id"), QStringLiteral("melonds_ds")}}}}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));  // not blocked
    const QVariantList warnings = h.controller->coreWarnings();
    QCOMPARE(warnings.size(), 1);
    QVERIFY(warnings.first().toMap().value(QStringLiteral("text")).toString().contains(QStringLiteral("melonDS DS")));
    QTRY_VERIFY(h.item("coreWarning") != nullptr && h.item("coreWarning")->isVisible());
    uitest::saveShot(h.window, QStringLiteral("5-core-warning"));
  }

  // core_missing for a core that exists on this device (stale snapshot or legacy id "melonds_ds") is not shown;
  // for a core that really is missing it still is.
  void coreMissingWarningIgnoredWhenCoreIsLocal() {
    FakeHub hub(QStringLiteral("a"));
    hub.handshakeExtra = QJsonObject{{QStringLiteral("problems"),
                                      QJsonArray{QJsonObject{{QStringLiteral("code"), QStringLiteral("core_missing")},
                                                             {QStringLiteral("detail"), QStringLiteral("Core melonds_ds is not installed")},
                                                             {QStringLiteral("core_id"), QStringLiteral("melonds_ds")}}}}};
    QVERIFY(hub.start());
    qputenv("UZUME_MELONDS_DS_CORE", QCoreApplication::applicationFilePath().toLocal8Bit());  // "found" core, never loaded
    {
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      QVERIFY(h.controller->coreWarnings().isEmpty());
    }
    qunsetenv("UZUME_MELONDS_DS_CORE");
    Harness h2;
    QVERIFY(h2.start());
    pair(h2, hub);
    QCOMPARE(h2.controller->coreWarnings().size(), 1);
  }

  // Sidebar user block: Hub user name above the device name; without `user` (older Hub) device name + "This device".
  void userBlockShowsHubUserName() {
    {
      FakeHub hub(QStringLiteral("a"));
      hub.handshakeExtra = QJsonObject{{QStringLiteral("user"),
                                        QJsonObject{{QStringLiteral("id"), QStringLiteral("u_1")},
                                                    {QStringLiteral("display_name"), QStringLiteral("lena")},
                                                    {QStringLiteral("role"), QStringLiteral("admin")}}}};
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      QCOMPARE(h.controller->userName(), QStringLiteral("lena"));
      QTRY_VERIFY(h.item("userName") != nullptr);
      QCOMPARE(h.item("userName")->property("text").toString(), QStringLiteral("lena"));
      QCOMPARE(h.item("userDevice")->property("text").toString(), h.controller->deviceName());
      QCOMPARE(h.item("userAvatarInitial")->property("text").toString(), QStringLiteral("L"));
      // Settings is This PC only, also for admins: no "Settings for" switch. The server's settings are a tab of the Server screen (here
      // the server has no admin_v1, so it shows the member block).
      QVERIFY(h.controller->isAdmin());
      h.controller->showSettings();
      QTest::qWait(100);
      QVERIFY(h.item("settingsScopeRow") == nullptr);
      QVERIFY(h.item("settingsScope") == nullptr);
      QVERIFY(h.item("scopeServer") == nullptr);
      h.controller->showServerPage(QStringLiteral("settings"));
      QTRY_VERIFY(h.item("serverMemberState") != nullptr && h.item("serverMemberState")->isVisible());
    }
    {
      FakeHub hub(QStringLiteral("a"));
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      QVERIFY(h.controller->userName().isEmpty());
      QTRY_VERIFY(h.item("userDevice") != nullptr);
      // Both lines stay present (P9): the device name takes the top line, "This device" the second.
      QCOMPARE(h.item("userName")->property("text").toString(), h.controller->deviceName());
      QCOMPARE(h.item("userDevice")->property("text").toString(), QStringLiteral("This device"));
      // No role from an older server: no admin switch in Settings.
      QVERIFY(!h.controller->isAdmin());
      h.controller->showSettings();
      QTest::qWait(100);
      QVERIFY(h.item("settingsScopeRow") == nullptr);
    }
  }

  // The handshake reports the core id of the registry ("melondsds" (offline default; ADR 0020)), never "melonds".
  void handshakeCoreId() {
    qputenv("UZUME_MELONDS_DS_CORE", QCoreApplication::applicationFilePath().toLocal8Bit());
    Harness h;
    QVERIFY(h.start(/*probeCores=*/true));
    qunsetenv("UZUME_MELONDS_DS_CORE");
    // The probe runs on a worker thread (CoreCatalog::probeCores): wait for it, it can finish after the window is exposed.
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->handshakeCores().size(), 1, 15000);
    QCOMPARE(h.controller->handshakeCores().first().id, QStringLiteral("melondsds"));
  }

  // Settings page: Appearance Dark (default) | Light | System, persisted; the game view stays dark.
  void settingsAppearance() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    {
      Harness h;
      QVERIFY(h.start());
      const QString dataDir = h.dir.path();
      QCOMPARE(h.controller->appearance(), QStringLiteral("dark"));
      QVERIFY(h.controller->darkMode());
      pair(h, hub);
      QTest::qWait(150);  // the library was just shown: let the layout settle
      QVERIFY(h.click("navSettings"));
      QTest::qWait(100);
      h.controller->showSettingsSection(QStringLiteral("appearance"));
      QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
      QVERIFY(h.item("appearanceSegment") != nullptr);
      QVERIFY(h.click("appearanceLight"));
      QCOMPARE(h.controller->appearance(), QStringLiteral("light"));
      QVERIFY(!h.controller->darkMode());
      // Text size and Reduce motion rows: apply at once and persist.
      QVERIFY(h.item("textSizeSegment") != nullptr);
      QVERIFY(h.item("reduceMotionToggle") != nullptr);
      QVERIFY(h.click("textSize125"));
      QTest::qWait(300);
      QVERIFY(h.click("textSizeTv"));
      QCOMPARE(h.controller->textSize(), QStringLiteral("tv"));
      QVERIFY(h.click("reduceMotionToggle"));
      QVERIFY(h.controller->reduceMotion());
      QTest::qWait(100);
      uitest::saveShot(h.window, QStringLiteral("5-settings-appearance-tv"));
      QVERIFY(h.click("textSize100"));
      QCOMPARE(h.controller->textSize(), QStringLiteral("100"));
      QTest::qWait(100);
      uitest::saveShot(h.window, QStringLiteral("5-settings-light"));
      QVERIFY(h.click("appearanceSystem"));
      QCOMPARE(h.controller->appearance(), QStringLiteral("system"));
      QVERIFY(h.click("appearanceLight"));
      QVERIFY(h.click("navLibrary"));
      QCOMPARE(h.controller->screen(), QStringLiteral("library"));
      QTest::qWait(100);
      uitest::saveShot(h.window, QStringLiteral("5-library-light"));
      h.controller->setAppearance(QStringLiteral("nonsense"));  // ignored
      QCOMPARE(h.controller->appearance(), QStringLiteral("light"));
      QVERIFY(QFile::exists(h.controller->playerSettings()->filePath()));
      PlayerSettings reread(dataDir);  // a new start reads the persisted value
      QCOMPARE(reread.appearance(), PlayerSettings::Appearance::Light);
      QCOMPARE(reread.textSize(), QStringLiteral("100"));
      QVERIFY(reread.reduceMotion().has_value() && *reread.reduceMotion());
    }
  }

  // Settings -> Hubs: list, switch (same path as the connection screen), auto-connect, remove with confirmation.
  void settingsHubs() {
    FakeHub hubA(QStringLiteral("a"));
    FakeHub hubB(QStringLiteral("b"));
    hubB.hubId = QStringLiteral("hub-test-2");
    auto nm = [](const char* prefix, const QString& id) { return QByteArray(prefix) + id.toUtf8(); };
    QVERIFY(hubA.start());
    QVERIFY(hubB.start());
    Harness h;
    QVERIFY(h.start());
    h.controller->onboarding()->setStartScreenEnabled(false);  // the older connection screens
    pair(h, hubA);
    h.controller->switchHub();
    pair(h, hubB);
    QTest::qWait(150);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
    QVERIFY(h.click("navSettings"));
    QTest::qWait(100);
    h.controller->showSettingsSection(QStringLiteral("hubs"));
    QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
    QVERIFY(h.item("hubsSection") != nullptr);
    const QString idA = hubA.hubId;
    const QString idB = hubB.hubId;
    // Both Hubs listed, B is current and connected; Switch only on A.
    QVERIFY(h.item(nm("hubName_", idA).constData()) != nullptr && h.item(nm("hubName_", idB).constData()) != nullptr);
    QVERIFY(h.item(nm("hubState_", idB).constData())->property("text").toString().contains(QStringLiteral("Connected")));
    QVERIFY(!h.item(nm("hubState_", idA).constData())->property("text").toString().contains(QStringLiteral("Connected")));
    QVERIFY(!h.item(nm("hubSwitch_", idB).constData())->isVisible());
    QVERIFY(h.item(nm("hubSwitch_", idA).constData())->isVisible());

    // Auto-connect toggle uses the same setting as the connection screen.
    QVERIFY(!h.controller->autoConnect());
    QVERIFY(h.click("settingsAutoConnectToggle"));
    QVERIFY(h.controller->autoConnect());
    QVERIFY(h.click("settingsAutoConnectToggle"));
    QVERIFY(!h.controller->autoConnect());

    // Switch to A: B disconnects, A connects, back on the Settings page after reconnect is not required.
    QVERIFY(h.click(nm("hubSwitch_", idA).constData()));
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    QCOMPARE(h.controller->connection()->profile()->hubId, idA);

    // Remove asks first; Cancel keeps the Hub.
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
    h.controller->showSettingsSection(QStringLiteral("hubs"));
    QTest::qWait(100);
    QVERIFY(h.click(nm("hubMenu_", idB).constData()));
    QTest::qWait(150);
    QVERIFY(h.click(nm("hubMenu_", idB).append("_remove").constData()));
    QVERIFY(h.item(nm("hubRemoveConfirm_", idB).constData())->isVisible());
    QVERIFY(h.click(nm("hubRemoveCancel_", idB).constData()));
    QCOMPARE(h.controller->hubs().size(), 2);
    // Confirmed removal of a non-current Hub.
    QVERIFY(h.click(nm("hubMenu_", idB).constData()));
    QTest::qWait(150);
    QVERIFY(h.click(nm("hubMenu_", idB).append("_remove").constData()));
    QVERIFY(h.click(nm("hubRemoveConfirmButton_", idB).constData()));
    QCOMPARE(h.controller->hubs().size(), 1);
    QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
    // Removing the current Hub returns to the connection screen.
    QVERIFY(h.click(nm("hubMenu_", idA).constData()));
    QTest::qWait(150);
    QVERIFY(h.click(nm("hubMenu_", idA).append("_remove").constData()));
    QVERIFY(h.click(nm("hubRemoveConfirmButton_", idA).constData()));
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("connection"));
    QCOMPARE(h.controller->hubs().size(), 0);
  }

  // Settings -> Hubs -> Edit (D3): validator messages, edit row with an invalid field, edit keeps fingerprint + credential,
  // editing the active Hub reconnects.
  void settingsHubEdit() {
    FakeHub hubA(QStringLiteral("a"));
    FakeHub hubB(QStringLiteral("b"));
    hubB.hubId = QStringLiteral("hub-test-2");
    QVERIFY(hubA.start());
    QVERIFY(hubB.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hubA);
    h.controller->switchHub();
    pair(h, hubB);
    PlayerController* pc = h.controller.get();
    QTest::qWait(150);
    QTRY_COMPARE_WITH_TIMEOUT(pc->screen(), QStringLiteral("library"), 8000);

    // Every validator message.
    QCOMPARE(pc->validateHubAddress(QString(), QStringLiteral("8443")), QStringList{QStringLiteral("Enter an address")});
    QCOMPARE(pc->validateHubAddress(QStringLiteral("https://x"), QStringLiteral("8443")),
             QStringList{QStringLiteral("Leave out https:// — host name only")});
    QCOMPARE(pc->validateHubAddress(QStringLiteral("a b"), QStringLiteral("8443")), QStringList{QStringLiteral("No spaces in the address")});
    QCOMPARE(pc->validateHubAddress(QStringLiteral("a:1"), QStringLiteral("8443")),
             QStringList{QStringLiteral("Put the port in the port field")});
    QCOMPARE(pc->validateHubAddress(QStringLiteral("a_b!"), QStringLiteral("8443")),
             QStringList{QStringLiteral("Only letters, digits, dots and hyphens")});
    QCOMPARE(pc->validateHubAddress(QStringLiteral("hub.example.com"), QStringLiteral("0")),
             QStringList{QStringLiteral("Port must be a number from 1 to 65535")});
    QVERIFY(pc->validateHubAddress(QStringLiteral("hub.example.com"), QStringLiteral("8443")).isEmpty());

    QVERIFY(h.click("navSettings"));
    QTest::qWait(100);
    h.controller->showSettingsSection(QStringLiteral("hubs"));
    const QString idA = hubA.hubId;
    const QString idB = hubB.hubId;
    auto nm = [](const char* prefix, const QString& id) { return QByteArray(prefix) + id.toUtf8(); };

    // Open the edit row of the non-active Hub A, enter an invalid host: error shown, Save disabled.
    QVERIFY(h.click(nm("hubEdit_", idA).constData()));
    QQuickItem* host = h.item(nm("hubEditAddress_", idA).constData());
    QVERIFY(host != nullptr && host->isVisible());
    host->setProperty("text", QStringLiteral("bad host"));
    QTRY_VERIFY(h.item(nm("hubEditError_", idA).constData())->isVisible());
    QVERIFY(!h.item(nm("hubEditSave_", idA).constData())->isEnabled());
    QVERIFY(h.click(nm("hubEditCancel_", idA).constData()));
    QCOMPARE(pc->profileStore()->profile(idA)->address, hubA.address());

    // Edit through the controller: the fingerprint, user and credential reference stay.
    const auto before = *pc->profileStore()->profile(idA);
    QVERIFY(!pc->editHub(idA, QStringLiteral("bad host"), QStringLiteral("8443")));
    QVERIFY(pc->editHub(idA, QStringLiteral("hub-a.invalid"), QStringLiteral("9443")));
    const auto after = *pc->profileStore()->profile(idA);
    QCOMPARE(after.address, QStringLiteral("https://hub-a.invalid:9443"));
    QCOMPARE(after.pinnedFingerprint, before.pinnedFingerprint);
    QCOMPARE(after.credentialRef, before.credentialRef);
    QCOMPARE(after.hubUserId, before.hubUserId);
    // An address that another Hub already uses is refused.
    QVERIFY(!pc->editHub(idA, QUrl(hubB.address()).host(), QString::number(QUrl(hubB.address()).port())));

    // Active Hub B: the edit is stored and the link is rebuilt (the page stays Settings).
    const auto beforeB = *pc->profileStore()->profile(idB);
    QVERIFY(pc->editHub(idB, QStringLiteral("localhost"), QString::number(QUrl(hubB.address()).port())));
    QCOMPARE(pc->profileStore()->profile(idB)->pinnedFingerprint, beforeB.pinnedFingerprint);
    QCOMPARE(pc->profileStore()->profile(idB)->credentialRef, beforeB.credentialRef);
    QVERIFY(pc->profileStore()->profile(idB)->address.contains(QStringLiteral("localhost")));
  }

  // D15: the core arrives through the Hub (cores_v1 package) while the Player runs; tile, counts and detail pane
  // change from "core missing" to ready without a restart.
  void coreDownloadRefreshesLibraryState() {
    const QByteArray rom = "dummy-rom-for-d15";
    const QByteArray lib(2048, 'l');
    const QByteArray license(128, 'x');
    const QString platform = CoreCache::currentPlatform();
    if (platform.isEmpty()) QSKIP("no core platform on this build");
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("firmware_v1"), QStringLiteral("cores_v1")};
    setGames(hub, rom);
    QJsonObject sys = ndsSystem(QStringLiteral("builtin"), {});
    QJsonObject nds = sys.value(QStringLiteral("systems")).toArray().first().toObject();
    nds.insert(QStringLiteral("core_package_version"), QStringLiteral("1.4.0"));
    hub.systems = QJsonObject{{QStringLiteral("systems"), QJsonArray{nds}}};
    const auto sha = [](const QByteArray& d) { return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex()); };
    const auto file = [&](const QString& name, const QString& role, const QByteArray& c) {
      return QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("role"), role}, {QStringLiteral("size"), c.size()},
                         {QStringLiteral("sha256"), sha(c)}, {QStringLiteral("available"), true}};
    };
    const QString key = QStringLiteral("melonds_ds/1.4.0/") + platform;
    hub.corePackages.insert(key, QJsonObject{{QStringLiteral("core_id"), QStringLiteral("melonds_ds")},
                                             {QStringLiteral("version"), QStringLiteral("1.4.0")},
                                             {QStringLiteral("platform"), platform},
                                             {QStringLiteral("license"), QStringLiteral("GPL-3.0")},
                                             {QStringLiteral("source_url"), QStringLiteral("https://example.invalid/src")},
                                             {QStringLiteral("source_ref"), QStringLiteral("v1.4.0")},
                                             {QStringLiteral("files"), QJsonArray{file(QStringLiteral("dummy_libretro.so"), QStringLiteral("library"), lib),
                                                                                  file(QStringLiteral("LICENSE.txt"), QStringLiteral("license"), license)}}});
    hub.coreFiles.insert(key + QStringLiteral("/dummy_libretro.so"), lib);
    hub.coreFiles.insert(key + QStringLiteral("/LICENSE.txt"), license);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    PlayerController* pc = h.controller.get();
    pc->selectGame(QStringLiteral("g1"));
    const auto tileText = [pc]() {
      const QHash<int, QByteArray> roles = pc->library()->roleNames();
      const int role = roles.key("tileText");
      return pc->library()->index(0, 0).data(role).toString();
    };
    const QString tileBefore = tileText();
    QVERIFY2(!tileBefore.startsWith(QStringLiteral("● Ready")), qPrintable(tileBefore));
    const QString before = pc->selectedGame().value(QStringLiteral("coreTone")).toString();
    QVERIFY(before != QLatin1String("ok"));

    // Fetch the package the way a start does; afterwards the state is refreshed without a restart or a manual reload.
    // The start only provisions once the Hub's systems list (core_package_version) has arrived; playSelected() is
    // a silent no-op before that, so wait for the state that signals "provisionable".
    QTRY_VERIFY_WITH_TIMEOUT(pc->selectedGame().value(QStringLiteral("canPlay")).toBool(), 8000);
    pc->playSelected();
    QTRY_VERIFY_WITH_TIMEOUT(hub.coreFileDownloads >= 2, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(pc->selectedGame().value(QStringLiteral("coreTone")).toString(), QStringLiteral("ok"), 8000);
    QCOMPARE(pc->library()->counts().value(QStringLiteral("attention")).toInt(), 0);
    QVERIFY(!tileText().contains(QStringLiteral("Core"), Qt::CaseSensitive));
    QVERIFY(!pc->selectedGame().value(QStringLiteral("coreText")).toString().contains(QStringLiteral("missing"), Qt::CaseInsensitive));
  }

  // ------------------------------------------------------------ "Set up a server on this PC" (0.9)

  static QString hubPort(const FakeHub& hub) { return QString::number(QUrl(hub.address()).port()); }

  void localHubCreatesAdminAndPairs() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    qputenv("UZUME_LOCAL_SERVER_PORT", hubPort(hub).toUtf8());
    Harness h;
    QVERIFY(h.start());
    auto* local = h.controller->localHub();
    QVERIFY(local->available());
    QVERIFY(h.item("setupLocalHubButton") != nullptr);
    local->start();
    // The self-signed certificate of the loopback Hub is trusted without the trust screen; the admin form follows.
    QTRY_COMPARE_WITH_TIMEOUT(local->phase(), QStringLiteral("needsSetup"), 8000);
    QVERIFY(h.controller->profileStore()->profile(hub.hubId).has_value());
    QVERIFY(!h.controller->profileStore()->profile(hub.hubId)->pinnedFingerprint.isEmpty());
    QVERIFY(h.item("localUserField") != nullptr);
    local->submit(QStringLiteral("fabio"), QStringLiteral("pw-one"), QStringLiteral("pw-two"));
    QVERIFY(!local->error().isEmpty());  // passwords differ
    QCOMPARE(local->phase(), QStringLiteral("needsSetup"));
    local->submit(QStringLiteral("fabio"), QStringLiteral("pw-one"), QStringLiteral("pw-one"));
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::Connected, 8000);
    QCOMPARE(hub.lastLocalBody.value(QStringLiteral("username")).toString(), QStringLiteral("fabio"));
    QCOMPARE(hub.lastLocalBody.value(QStringLiteral("device_id")).toString(), h.controller->profileStore()->deviceId());
    QVERIFY(!hub.lastLocalBody.value(QStringLiteral("player_version")).toString().isEmpty());
    const auto p = h.controller->profileStore()->profile(hub.hubId);
    QCOMPARE(p->hubUserId, QStringLiteral("u_admin_1"));
    QVERIFY(!p->credentialRef.isEmpty());
    QVERIFY(local->onThisPc());
    QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
    qunsetenv("UZUME_LOCAL_SERVER_PORT");
  }

  void localHubSignInAndSettings() {
    FakeHub hub(QStringLiteral("a"));
    hub.localAdminExists = true;
    hub.localImportDir = QStringLiteral("C:\\Games");
    QVERIFY(hub.start());
    qputenv("UZUME_LOCAL_SERVER_PORT", hubPort(hub).toUtf8());
    Harness h;
    QVERIFY(h.start());
    auto* local = h.controller->localHub();
    local->start();
    QTRY_COMPARE_WITH_TIMEOUT(local->phase(), QStringLiteral("needsSignIn"), 8000);
    local->submit(QStringLiteral("fabio"), QStringLiteral("wrong"), QString());
    QTRY_VERIFY_WITH_TIMEOUT(!local->error().isEmpty(), 8000);
    QCOMPARE(local->phase(), QStringLiteral("needsSignIn"));  // form stays open
    local->submit(QStringLiteral("fabio"), QStringLiteral("correct horse"), QString());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::Connected, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(local->importDir(), QStringLiteral("C:\\Games"), 8000);

    local->setNetworkSharing(true);
    QTRY_VERIFY_WITH_TIMEOUT(local->networkSharing() && !local->settingsBusy(), 8000);
    QCOMPARE(hub.lastLocalSettings.value(QStringLiteral("network_sharing")).toBool(), true);

    hub.localImportDirError = QStringLiteral("The server cannot read this folder.");
    local->chooseFolder(QStringLiteral("/nonexistent/roms"));
    QTRY_VERIFY_WITH_TIMEOUT(!local->settingsBusy(), 8000);
    QCOMPARE(local->settingsError(), QStringLiteral("The server cannot read this folder."));
    hub.localImportDirError.clear();
    local->chooseFolder(QStringLiteral("/tmp"));
    QTRY_VERIFY_WITH_TIMEOUT(!local->settingsBusy() && local->settingsError().isEmpty(), 8000);
    // The Player sends and shows the native absolute path (backslashes on Windows).
    QCOMPARE(local->importDir(), QDir::toNativeSeparators(QStringLiteral("/tmp")));
    QVERIFY(!local->settingsNotice().isEmpty());
    qunsetenv("UZUME_LOCAL_SERVER_PORT");
  }

  void localHubAddressMismatchIsNotTrusted() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    // A normally added Hub on the same loopback address is never trusted automatically.
    qputenv("UZUME_LOCAL_SERVER_PORT", "1");
    Harness h;
    QVERIFY(h.start());
    h.controller->addHub(hub.address());
    QTRY_COMPARE(h.controller->connection()->state(), S::NeedsTrustConfirmation);
    QTest::qWait(300);
    QCOMPARE(h.controller->connection()->state(), S::NeedsTrustConfirmation);
    qunsetenv("UZUME_LOCAL_SERVER_PORT");
  }

 private:
  static QString pairingError(Harness& h) { return h.controller->pairing().value(QStringLiteral("error")).toString(); }
};

UITEST_MAIN(HubFeaturesUiTest)
#include "hub_features_ui_test.moc"
