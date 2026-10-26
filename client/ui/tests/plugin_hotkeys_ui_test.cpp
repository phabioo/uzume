// Plugin hotkeys in Controllers > Hotkeys (0.11 package 2): one group per plugin that declares hotkeys (name, origin marker,
// key count), Uzume keys always win (a conflicting plugin key is "not active" with a note), capture / clear / reset, and
// the user's choice persisted in the player settings by plugin id + hotkey id. The in-game side (key press -> on_action,
// Alt+n, F4, show a hidden HUD widget again) is in plugin_slots_ui_test.cpp. FakeHub, preview plugin models, no core.
#include <QDir>
#include <QtTest>

#include "fakehub.h"
#include "playersettings.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;

namespace {

void pair(Harness& h, FakeHub& hub) {
  h.controller->addHub(hub.address());
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation, 8000);
  h.controller->confirmTrust();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsPairing, 8000);
  h.controller->requestPairing();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
}

QVariantMap hotkeyPlugin(const QString& id, const QString& name, const QString& origin, const QVariantList& hotkeys) {
  return {{"pluginId", id}, {"name", name}, {"origin", origin}, {"hotkeys", hotkeys}};
}

QVariantMap hotkeyDecl(const QString& id, const QString& label, const QString& key) { return {{"id", id}, {"label", label}, {"key", key}}; }

}  // namespace

class PluginHotkeysUiTest : public QObject {
  Q_OBJECT

  FakeHub hub_{QStringLiteral("a")};
  Harness h_;

  PluginsController* pc() { return h_.controller->plugins(); }
  void shot(const QString& name) {
    const QString dir = qEnvironmentVariable("UZUME_SCREENSHOT_DIR");
    if (dir.isEmpty()) return;
    QQuickTest::qWaitForPolish(h_.window);
    QTest::qWait(400);
    QDir().mkpath(dir);
    QVERIFY(h_.window->grabWindow().save(QDir(dir).filePath(name + QStringLiteral(".png"))));
  }
  static QString textOf(QQuickItem* it) {
    if (it->property("text").isValid() && !it->property("text").toString().isEmpty()) return it->property("text").toString();
    for (QQuickItem* c : it->childItems())
      if (const QString t = textOf(c); !t.isEmpty()) return t;
    return {};
  }
  PlayerSettings freshSettings() { return PlayerSettings(h_.controller->profileStore()->baseDir()); }
  QVariantMap hotkeyUi() {
    return {{"hotkeyPlugins",
             QVariantList{hotkeyPlugin("net.test.a", "SoulLink & Nuzlocke", "official",
                                       {hotkeyDecl("toggle-hud", "Show or hide HUD widget", "Ctrl+H"), hotkeyDecl("team", "Open team", "Ctrl+D"),
                                        hotkeyDecl("reload", "Reload run", "Ctrl+R")}),
                          hotkeyPlugin("net.test.b", "Item Helper", "unverified",
                                       {hotkeyDecl("peek", "Peek bag", "Ctrl+H"), hotkeyDecl("fs", "Quick save", "F11"), hotkeyDecl("bag", "Open bag", "F8")})}}};
  }

 private slots:
  void initTestCase() {
    uitest::installWarningCounter();
    hub_.hubId = QStringLiteral("hub-hotkeys");
    hub_.name = QStringLiteral("Home");
    hub_.decision = FakeHub::Decision::Approve;
    QVERIFY(hub_.start());
    QVERIFY(h_.start());
    pair(h_, hub_);
    h_.window->resize(1440, 900);
    QTest::qWait(150);
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void hotkeysGroupsConflictsAndOverrides() {
    pc()->setPreviewUi(hotkeyUi());
    // Model: Uzume keys win, then the earlier plugin; a conflicting key is not active.
    const QVariantList groups = pc()->hotkeyGroups();
    QCOMPARE(groups.size(), 2);
    QCOMPARE(groups.at(0).toMap().value("conflicts").toInt(), 0);
    QCOMPARE(groups.at(0).toMap().value("marker").toString(), QString());
    QCOMPARE(groups.at(1).toMap().value("marker").toString(), QStringLiteral("Unverified"));
    QCOMPARE(groups.at(1).toMap().value("count").toInt(), 3);
    QCOMPARE(groups.at(1).toMap().value("conflicts").toInt(), 2);
    const QVariantMap peek = groups.at(1).toMap().value("rows").toList().at(0).toMap();
    QVERIFY(!peek.value("active").toBool());
    QCOMPARE(peek.value("keyText").toString(), QStringLiteral("Ctrl+H · not active"));
    QCOMPARE(peek.value("note").toString(), QStringLiteral("Already used by SoulLink & Nuzlocke. Unassigned until you choose."));
    QCOMPARE(groups.at(1).toMap().value("rows").toList().at(1).toMap().value("note").toString(),
             QStringLiteral("Already used by Toggle fullscreen. Uzume keys always win."));
    QVERIFY(groups.at(1).toMap().value("rows").toList().at(2).toMap().value("active").toBool());  // F8
    QVERIFY(pc()->activeHotkeyCombos().contains(int(Qt::Key_H) | int(Qt::ControlModifier)));
    QVERIFY(!pc()->activeHotkeyCombos().contains(int(Qt::Key_F11)));
    QCOMPARE(pc()->activeHotkeyCombos().size(), 4);  // Ctrl+H, Ctrl+D, Ctrl+R, F8

    // Settings > Hotkeys: Uzume rows, one group per plugin, conflict row in the warning tone.
    h_.controller->showSettingsSection(QStringLiteral("hotkeys"));
    QTRY_COMPARE(h_.controller->screen(), QStringLiteral("settings"));
    QTRY_VERIFY(h_.item("hotkeysContent") != nullptr && h_.item("hotkeysContent")->isVisible());
    QQuickTest::qWaitForPolish(h_.window);
    QTRY_VERIFY(h_.item("pluginHotkeyGroup_net.test.a") != nullptr && h_.item("pluginHotkeyGroup_net.test.a")->isVisible());
    QVERIFY(h_.item("pluginHotkeyGroup_net.test.b")->isVisible());
    QCOMPARE(textOf(h_.item("uzumeHotkeyGroup")), QStringLiteral("Uzume"));
    QCOMPARE(textOf(h_.item("hotkeyText_hudAll")), QStringLiteral("F4"));
    QCOMPARE(textOf(h_.item("hotkeyText_panelTabs")), QStringLiteral("Alt+1 … Alt+9"));
    QCOMPARE(textOf(h_.item("pluginHotkeyName_net.test.a")), QStringLiteral("SoulLink & Nuzlocke"));
    QVERIFY(!h_.item("pluginHotkeyOrigin_net.test.a")->isVisible());
    QVERIFY(h_.item("pluginHotkeyOrigin_net.test.b")->isVisible());
    QCOMPARE(textOf(h_.item("pluginHotkeyOrigin_net.test.b")), QStringLiteral("Unverified"));
    QCOMPARE(textOf(h_.item("pluginHotkeyCount_net.test.a")), QStringLiteral("3 keys"));
    QCOMPARE(textOf(h_.item("pluginHotkeyText_net.test.a_toggle-hud")), QStringLiteral("Ctrl+H"));
    QCOMPARE(textOf(h_.item("pluginHotkeyText_net.test.b_peek")), QStringLiteral("Ctrl+H · not active"));
    QVERIFY(h_.item("pluginHotkeyNote_net.test.b_peek")->isVisible());
    QVERIFY(!h_.item("pluginHotkeyNote_net.test.a_toggle-hud")->isVisible());
    QVERIFY(!h_.item("pluginHotkeyReset_net.test.b_peek")->isVisible());
    shot(QStringLiteral("hotkeys-plugin-groups"));

    // Capture: an Uzume key and another plugin's key are rejected (the field keeps listening), a free key is taken.
    QVERIFY(h_.click("pluginHotkeyField_net.test.b_peek"));
    QTRY_COMPARE(pc()->hotkeyListening(), QStringLiteral("net.test.b/peek"));
    QCOMPARE(textOf(h_.item("pluginHotkeyText_net.test.b_peek")), QStringLiteral("Press a key…"));
    QTest::keyClick(h_.window, Qt::Key_F3);
    QCOMPARE(pc()->hotkeyNote(), QStringLiteral("Already used by Diagnostics overlay"));
    QTest::keyClick(h_.window, Qt::Key_F4);
    QCOMPARE(pc()->hotkeyNote(), QStringLiteral("Already used by Hide all HUD widgets"));
    QTest::keyClick(h_.window, Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(pc()->hotkeyNote(), QStringLiteral("Already used by SoulLink & Nuzlocke"));
    QCOMPARE(pc()->hotkeyListening(), QStringLiteral("net.test.b/peek"));
    QTest::keyClick(h_.window, Qt::Key_1, Qt::AltModifier);
    QCOMPARE(pc()->hotkeyNote(), QStringLiteral("Already used by Panel tabs"));
    QTest::keyClick(h_.window, Qt::Key_Escape);  // cancels
    QTRY_VERIFY(pc()->hotkeyListening().isEmpty());
    QVERIFY(freshSettings().pluginHotkeyOverrides().isEmpty());
    QVERIFY(h_.click("pluginHotkeyField_net.test.b_peek"));
    QTRY_COMPARE(pc()->hotkeyListening(), QStringLiteral("net.test.b/peek"));
    QTest::keyClick(h_.window, Qt::Key_K, Qt::ControlModifier);
    QTRY_VERIFY(pc()->hotkeyListening().isEmpty());
    QTRY_COMPARE(textOf(h_.item("pluginHotkeyText_net.test.b_peek")), QStringLiteral("Ctrl+K"));
    QTRY_VERIFY(!h_.item("pluginHotkeyNote_net.test.b_peek")->isVisible());
    QTRY_VERIFY(h_.item("pluginHotkeyReset_net.test.b_peek")->isVisible());
    QCOMPARE(textOf(h_.item("pluginHotkeyCount_net.test.b")), QStringLiteral("3 keys · 1 conflict"));
    QVERIFY(pc()->activeHotkeyCombos().contains(int(Qt::Key_K) | int(Qt::ControlModifier)));

    // The choice persists locally, keyed by plugin id + hotkey id, and a new controller reads it back.
    QCOMPARE(freshSettings().pluginHotkeyOverrides().value("net.test.b/peek"), QStringLiteral("Ctrl+K"));
    QCOMPARE(freshSettings().pluginHotkeyOverrides().size(), 1);

    // Clear (x) leaves the key unassigned, Reset goes back to the declared key (here: in conflict again).
    QVERIFY(h_.click("pluginHotkeyClear_net.test.b_peek"));
    QTRY_COMPARE(textOf(h_.item("pluginHotkeyText_net.test.b_peek")), QStringLiteral("not set"));
    QCOMPARE(freshSettings().pluginHotkeyOverrides().value("net.test.b/peek"), QStringLiteral("none"));
    QVERIFY(h_.click("pluginHotkeyReset_net.test.b_peek"));
    QTRY_COMPARE(textOf(h_.item("pluginHotkeyText_net.test.b_peek")), QStringLiteral("Ctrl+H · not active"));
    QVERIFY(freshSettings().pluginHotkeyOverrides().isEmpty());

    // Pick Ctrl+K again for the key tests below.
    pc()->beginHotkeyCapture(QStringLiteral("net.test.b"), QStringLiteral("peek"));
    QVERIFY(pc()->captureHotkey(Qt::Key_K, Qt::ControlModifier));
    QCOMPARE(freshSettings().pluginHotkeyOverrides().value("net.test.b/peek"), QStringLiteral("Ctrl+K"));
  }

};

UITEST_MAIN(PluginHotkeysUiTest)
#include "plugin_hotkeys_ui_test.moc"
