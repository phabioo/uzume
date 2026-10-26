// Screenshots of the out-of-game screens (design v4): Connection, Pairing, Library, Conflict dialog, Emulation,
// Controllers, Settings with the hub edit row. Skipped unless UZUME_SCREENSHOT_DIR is set. FakeHub, dummy
// bytes only (no ROM/BIOS/firmware). Files: <dir>/<name>.png (not checked into git).
#include <QSignalSpy>
#include <QtTest>

#include "core_options.h"
#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
emu::CoreOption option(const QString& key, const QString& desc, const QString& cat, const QStringList& values, const QString& def) {
  emu::CoreOption o;
  o.key = key;
  o.description = desc;
  o.info = desc;
  o.categoryKey = cat;
  for (const QString& v : values) o.values.append({v, QString()});
  o.defaultValue = def;
  return o;
}

emu::CoreProbe probe() {
  emu::CoreProbe p;
  p.ok = true;
  p.info.name = QStringLiteral("melonDS DS");
  p.info.version = QStringLiteral("1.4.0");
  p.categories = {{QStringLiteral("audio"), QStringLiteral("Audio"), QString()}, {QStringLiteral("system"), QStringLiteral("System"), QString()}};
  p.options = {option(QStringLiteral("melonds_audio_interpolation"), QStringLiteral("Interpolation"), QStringLiteral("audio"),
                      {QStringLiteral("disabled"), QStringLiteral("linear"), QStringLiteral("cosine")}, QStringLiteral("disabled")),
               option(QStringLiteral("melonds_boot_mode"), QStringLiteral("Boot Mode"), QStringLiteral("system"),
                      {QStringLiteral("direct"), QStringLiteral("native")}, QStringLiteral("direct"))};
  return p;
}
}  // namespace

class ShellScreenshotsTest : public QObject {
  Q_OBJECT

  QString dir_;

  void shot(Harness& h, const QString& name) {
    QTest::qWait(350);  // transitions (<= 180 ms) settle
    QDir().mkpath(dir_);
    const QImage img = h.window->grabWindow();
    QVERIFY2(!img.isNull() && !uitest::isAllBlack(img), qPrintable(name));
    QVERIFY(img.save(QDir(dir_).filePath(name + QStringLiteral(".png"))));
  }

  static void size(Harness& h) {
    h.window->resize(qEnvironmentVariableIntValue("UZUME_SHOT_WIDTH") > 0 ? qEnvironmentVariableIntValue("UZUME_SHOT_WIDTH") : 1440, 900);
    QTest::qWait(100);
  }

 private slots:
  void initTestCase() {
    dir_ = qEnvironmentVariable("UZUME_SCREENSHOT_DIR");
    if (dir_.isEmpty()) QSKIP("UZUME_SCREENSHOT_DIR not set");
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void connectionAndPairing() {
    Harness h;
    QVERIFY(h.start());
    size(h);
    shot(h, QStringLiteral("3a-connection"));

    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    hub.decision = FakeHub::Decision::Pending;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    shot(h, QStringLiteral("3a-trust"));
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    shot(h, QStringLiteral("3b-pairing"));
  }

  void libraryConflictAndSettings() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    hub.decision = FakeHub::Decision::Approve;
    const QByteArray rom = "dummy-rom-bytes";
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    QJsonArray games;
    games.append(uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"), uitest::sha256Hex(rom), rom.size()));
    games.append(uitest::gameJson(QStringLiteral("g2"), QStringLiteral("Paper Wizards"), uitest::sha256Hex("pw"), 128LL * 1024 * 1024));
    games.append(uitest::gameJson(QStringLiteral("g3"), QStringLiteral("Orbit Gardens"), uitest::sha256Hex("og"), 16LL * 1024 * 1024));
    games.append(uitest::gameJson(QStringLiteral("g4"), QStringLiteral("Clocktower Kids"), uitest::sha256Hex("ck"), 24LL * 1024 * 1024));
    hub.games = QJsonObject{{QStringLiteral("games"), games}};

    Harness h;
    QVERIFY(h.start());
    size(h);
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melonds_ds"), probe(), false);

    // Library with a selected game and the attention filter.
    h.controller->selectGame(QStringLiteral("g2"));
    shot(h, QStringLiteral("3c-library"));
    h.controller->library()->setFilter(QStringLiteral("attention"));
    shot(h, QStringLiteral("3c-library-attention"));
    h.controller->library()->setFilter(QStringLiteral("all"));

    // Emulation: a system, then Defaults with a changed value.
    h.controller->showEmulation();
    QTest::qWait(150);
    shot(h, QStringLiteral("3e-emulation-system"));
    QVERIFY(h.click("tab_Defaults"));
    QTRY_VERIFY(h.item("optionToggle_uzume.fullscreen_on_start") != nullptr);
    QVERIFY(h.click("optionToggle_uzume.fullscreen_on_start"));
    shot(h, QStringLiteral("3e-emulation-defaults"));
    // A system's core options (Nintendo DS): the list uses the full window width.
    h.controller->showEmulation();
    QTest::qWait(150);
    if (h.click("tab_NDS")) {
      QTest::qWait(150);
      shot(h, QStringLiteral("3e-emulation-nds"));
    }

    h.controller->showControllers();
    QTest::qWait(150);
    shot(h, QStringLiteral("3f-controllers"));
    h.controller->showSettingsSection(QStringLiteral("hotkeys"));
    QTRY_VERIFY(h.item("hotkeyField_diagnostics") != nullptr);
    QVERIFY(h.click("hotkeyField_diagnostics"));
    QTest::keyClick(h.window, Qt::Key_F4);
    QVERIFY(h.click("hotkeyField_snapshot"));
    QTest::qWait(100);
    shot(h, QStringLiteral("3f-controllers-hotkeys"));

    // Settings with the hub edit row open and one invalid field.
    h.controller->showSettings();
    QTest::qWait(150);
    shot(h, QStringLiteral("3p-settings"));
    // The This PC sections: the section column shows the page of each of them.
    for (const char* sec : {"appearance", "storage", "diagnostics"}) {
      QVERIFY(h.click((QByteArray("section_") + sec).constData()));
      shot(h, QStringLiteral("3p-settings-") + QString::fromLatin1(sec));
    }
    const QString id = hub.hubId;
    QVERIFY(h.click((QByteArray("hubEdit_") + id.toUtf8()).constData()));
    QQuickItem* host = h.item((QByteArray("hubEditAddress_") + id.toUtf8()).constData());
    QVERIFY(host != nullptr);
    host->setProperty("text", QStringLiteral("https://hub"));
    shot(h, QStringLiteral("3p-settings-hub-edit"));

    // Light palette.
    h.controller->setAppearance(QStringLiteral("light"));
    shot(h, QStringLiteral("3p-settings-light"));
    h.controller->showLibrary();
    shot(h, QStringLiteral("3c-library-light"));

    // Conflict dialog (3d).
    SaveSync::ConflictView v;
    v.conflict.id = QStringLiteral("c1");
    v.conflict.hubRevision = 7;
    v.conflict.hubSha256 = QStringLiteral("0123456789abcdef");
    v.conflict.hubDeviceName = QStringLiteral("Living room PC");
    v.conflict.hubCreatedAt = QStringLiteral("2026-10-01T18:20:00Z");
    v.gameId = QStringLiteral("g1");
    v.localDeviceName = QStringLiteral("This device");
    v.localModified = QDateTime::fromString(QStringLiteral("2026-10-02T09:05:00Z"), Qt::ISODate);
    v.localSha256 = QStringLiteral("fedcba9876543210");
    v.localBaseRevision = 6;
    emit h.controller->saveSync()->startConflict(v);
    QTRY_VERIFY(h.item("conflictDialog") != nullptr && h.item("conflictDialog")->isVisible());
    shot(h, QStringLiteral("3d-conflict"));
  }
};

UITEST_MAIN(ShellScreenshotsTest)
#include "screenshots_shell_test.moc"
