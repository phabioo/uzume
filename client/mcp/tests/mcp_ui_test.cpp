// UI-control tools of the debug MCP server (ADR 0032 D3/D4) against the real Main.qml offscreen, paired with a fake hub:
// ui_tree, ui_click, ui_type, ui_key, navigate, app_screenshot, the destructive brake and the password rule.
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "mcp_registry.h"
#include "mcp_tools.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::mcp;
using uitest::Harness;

namespace {

struct Answer {
  ToolResult result;
  QJsonObject json() const {
    for (const ToolContent& c : result.content)
      if (c.kind == ToolContent::Kind::Text) {
        const QJsonDocument d = QJsonDocument::fromJson(c.text.toUtf8());
        if (d.isObject()) return d.object();
      }
    return {};
  }
  QString text() const {
    QString s;
    for (const ToolContent& c : result.content)
      if (c.kind == ToolContent::Kind::Text) s += c.text + QLatin1Char('\n');
    return s;
  }
};

// The first node with this objectName in a ui_tree answer.
QJsonObject findNode(const QJsonValue& v, const QString& name) {
  if (v.isArray()) {
    for (const QJsonValue& e : v.toArray()) {
      const QJsonObject r = findNode(e, name);
      if (!r.isEmpty()) return r;
    }
  } else if (v.isObject()) {
    const QJsonObject o = v.toObject();
    if (o.value(QStringLiteral("objectName")).toString() == name) return o;
    for (const QString& k : {QStringLiteral("windows"), QStringLiteral("tree"), QStringLiteral("children")}) {
      const QJsonObject r = findNode(o.value(k), name);
      if (!r.isEmpty()) return r;
    }
  }
  return {};
}

}  // namespace

class McpUiTest : public QObject {
  Q_OBJECT

  McpToolRegistry registry;
  FakeHub hub{QStringLiteral("a")};
  std::unique_ptr<Harness> h;

  Answer call(const QString& tool, const QJsonObject& args = {}) {
    Answer a;
    bool done = false;
    registry.call(tool, args, [&](ToolResult r) {
      a.result = std::move(r);
      done = true;
    });
    (void)QTest::qWaitFor([&]() { return done; }, 20000);
    if (!done) a.result = ToolResult::error(QStringLiteral("test_timeout"));
    return a;
  }

  // An item tree for the tests, created from the app's own QML module (UzButton, UzField) inside the window.
  QQuickItem* inject(const QByteArray& qml) {
    auto* comp = new QQmlComponent(h->engine.get());
    comp->setData("import QtQuick\nimport QtQuick.Controls.Basic\nimport Uzume.App\n" + qml, QUrl());
    QObject* o = comp->create();
    if (o == nullptr) qWarning("[mcp_ui_test] inject failed: %s", qPrintable(comp->errorString()));
    auto* item = qobject_cast<QQuickItem*>(o);
    if (item != nullptr) {
      item->setParentItem(h->window->contentItem());
      item->setZ(1000);
    }
    delete comp;
    return item;
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
    hub.hubId = QStringLiteral("hub-mcpui");
    hub.name = QStringLiteral("Home");
    hub.decision = FakeHub::Decision::Approve;
    hub.games = QJsonObject{{QStringLiteral("games"),
                             QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"), uitest::sha256Hex("a"), 1)}}};
    QVERIFY(hub.start());
    h = std::make_unique<Harness>();
    QVERIFY(h->start());
    h->controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h->controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation, 8000);
    h->controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h->controller->connection()->state(), HubConnection::State::NeedsPairing, 8000);
    h->controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h->controller->screen(), QStringLiteral("library"), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(h->controller->libraryState(), QStringLiteral("ready"), 8000);
    installTools(registry, *h->controller);
  }

  void cleanupTestCase() {
    h.reset();
  }

  void init() { uitest::warningCount() = 0; }

  void toolsAreListed() {
    QStringList names;
    for (const ToolDef& t : registry.tools()) names << t.name;
    for (const char* n : {"ui_tree", "ui_click", "ui_type", "ui_key", "ui_scroll", "navigate", "game_start", "game_stop", "app_screenshot"})
      QVERIFY2(names.contains(QLatin1String(n)), n);
  }

  // ui_tree finds known objectNames on the library screen; every node carries a path id and a rect.
  void treeFindsLibraryNames() {
    const Answer a = call(QStringLiteral("ui_tree"));
    QVERIFY2(!a.result.isError, qPrintable(a.text()));
    const QJsonObject j = a.json();
    QVERIFY(!j.value(QStringLiteral("truncated")).toBool());
    for (const QString& n : {QStringLiteral("libraryTitle"), QStringLiteral("searchField"), QStringLiteral("gameGrid")}) {
      const QJsonObject node = findNode(j, n);
      QVERIFY2(!node.isEmpty(), qPrintable(n));
      QVERIFY(!node.value(QStringLiteral("path")).toString().isEmpty());
      QCOMPARE(node.value(QStringLiteral("rect")).toArray().size(), 4);
    }
    // The path id resolves to the same item.
    const QString path = findNode(j, QStringLiteral("searchField")).value(QStringLiteral("path")).toString();
    const Answer b = call(QStringLiteral("ui_tree"), {{"root", path}, {"depth", 0}});
    QCOMPARE(b.json().value(QStringLiteral("root")).toObject().value(QStringLiteral("objectName")).toString(), QStringLiteral("searchField"));
  }

  void treeDepthAndRootAreHonoured() {
    const Answer shallow = call(QStringLiteral("ui_tree"), {{"depth", 1}});
    QVERIFY(shallow.json().value(QStringLiteral("depthLimit")).isString());
    const Answer part = call(QStringLiteral("ui_tree"), {{"root", "libraryToolbar"}});
    QVERIFY2(!part.result.isError, qPrintable(part.text()));
    QVERIFY(!findNode(part.json().value(QStringLiteral("tree")), QStringLiteral("searchField")).isEmpty());
    QVERIFY(call(QStringLiteral("ui_tree"), {{"root", "noSuchObject"}}).result.isError);
  }

  // ui_type: real key events into a text field, found by objectName.
  void typeWritesIntoTextField() {
    const Answer a = call(QStringLiteral("ui_type"), {{"target", "searchField"}, {"text", "lumen"}});
    QVERIFY2(!a.result.isError, qPrintable(a.text()));
    QCOMPARE(a.json().value(QStringLiteral("value")).toString(), QStringLiteral("lumen"));
    QQuickItem* field = h->item("searchField");
    QVERIFY(field != nullptr);
    QCOMPARE(field->property("text").toString(), QStringLiteral("lumen"));
    // clear replaces the content
    const Answer b = call(QStringLiteral("ui_type"), {{"target", "searchField"}, {"text", "x"}, {"clear", true}});
    QCOMPARE(b.json().value(QStringLiteral("value")).toString(), QStringLiteral("x"));
    call(QStringLiteral("ui_type"), {{"target", "searchField"}, {"text", ""}, {"clear", true}});
  }

  // ui_click toggles a switch (a real mouse press and release on it).
  void clickTogglesSwitch() {
    h->window->resize(1000, 380);  // short: the toggle is below the fold, so the tool has to scroll the page to it
    QTest::qWait(100);
    const Answer nav = call(QStringLiteral("navigate"), {{"to", "settings"}, {"page", "audio"}});
    QVERIFY2(!nav.result.isError, qPrintable(nav.text()));
    QCOMPARE(nav.json().value(QStringLiteral("screen")).toString(), QStringLiteral("settings"));
    QTRY_VERIFY_WITH_TIMEOUT(h->item("audioShowVolumeToggle") != nullptr && h->item("audioShowVolumeToggle")->isVisible(), 8000);
    QQuickItem* sw = h->item("audioShowVolumeToggle");
    QVERIFY(sw != nullptr);
    const bool before = sw->property("checked").toBool();
    const Answer a = call(QStringLiteral("ui_click"), {{"target", "audioShowVolumeToggle"}});
    QVERIFY2(!a.result.isError, qPrintable(a.text()));
    QTRY_COMPARE(sw->property("checked").toBool(), !before);
    QCOMPARE(a.json().value(QStringLiteral("target")).toObject().value(QStringLiteral("checked")).toBool(), !before);
    call(QStringLiteral("ui_click"), {{"target", "audioShowVolumeToggle"}});  // back
    QTRY_COMPARE(sw->property("checked").toBool(), before);
    h->window->resize(1280, 800);
  }

  // navigate through the controller, and the tree of the opened settings page.
  void navigateToSettingsPage() {
    QVERIFY(!call(QStringLiteral("navigate"), {{"to", "library"}}).result.isError);
    QCOMPARE(h->controller->screen(), QStringLiteral("library"));
    const Answer a = call(QStringLiteral("navigate"), {{"to", "settings"}, {"page", "diagnostics"}});
    QVERIFY2(!a.result.isError, qPrintable(a.text()));
    QCOMPARE(h->controller->screen(), QStringLiteral("settings"));
    QTRY_VERIFY_WITH_TIMEOUT(!findNode(call(QStringLiteral("ui_tree")).json(), QStringLiteral("mcpEnableRow")).isEmpty(), 8000);
    QVERIFY(call(QStringLiteral("navigate"), {{"to", "settings"}, {"page", "nonsense"}}).result.isError);
    QVERIFY(call(QStringLiteral("navigate"), {{"to", "nowhere"}}).result.isError);
    // Server pages are the tabs of the Server screen (the old ids status and users still pass the check).
    const Answer badServer = call(QStringLiteral("navigate"), {{"to", "server"}, {"page", "nonsense"}});
    QVERIFY(badServer.result.isError);
    for (const char* id : {"overview", "people", "games", "systems", "tools", "plugins", "reports", "settings"}) {
      QVERIFY2(badServer.text().contains(QLatin1String(id)), id);
    }
    const Answer back = call(QStringLiteral("navigate"), {{"to", "game"}, {"gameId", "g1"}});
    QVERIFY2(!back.result.isError, qPrintable(back.text()));
    QCOMPARE(back.json().value(QStringLiteral("selectedGameId")).toString(), QStringLiteral("g1"));
    QCOMPARE(back.json().value(QStringLiteral("screen")).toString(), QStringLiteral("library"));   // the game page lives in the Library screen
    QTRY_VERIFY_WITH_TIMEOUT(!findNode(call(QStringLiteral("ui_tree")).json(), QStringLiteral("gamePage")).isEmpty(), 8000);
    QVERIFY(!call(QStringLiteral("navigate"), {{"to", "library"}}).result.isError);
    QTRY_VERIFY_WITH_TIMEOUT(findNode(call(QStringLiteral("ui_tree")).json(), QStringLiteral("gamePage")).isEmpty(), 8000);
    QVERIFY(call(QStringLiteral("navigate"), {{"to", "game"}, {"gameId", "unknown"}}).result.isError);
  }

  // ui_key sends real key events: Escape is delivered to the focused item.
  void keyReachesFocusedItem() {
    QQuickItem* box = inject(R"QML(
      TextField { objectName: "mcpKeyProbe"; x: 20; y: 20; width: 200; height: 36
        property int escapes: 0
        Keys.onEscapePressed: escapes++ }
    )QML");
    QVERIFY(box != nullptr);
    const Answer a = call(QStringLiteral("ui_key"), {{"key", "Escape"}, {"target", "mcpKeyProbe"}});
    QVERIFY2(!a.result.isError, qPrintable(a.text()));
    QCOMPARE(box->property("escapes").toInt(), 1);
    QVERIFY(call(QStringLiteral("ui_key"), {{"key", "Nonsense+Key"}}).result.isError);
    delete box;
  }

  // App shortcuts fire from ui_key like from the keyboard (Ctrl+4 controllers, Ctrl+, settings, Ctrl+1 library).
  void shortcutsFire() {
    call(QStringLiteral("navigate"), {{"to", "library"}});
    QVERIFY(!call(QStringLiteral("ui_key"), {{"key", "Ctrl+4"}}).result.isError);
    QTRY_COMPARE(h->controller->screen(), QStringLiteral("settings"));   // Controls is a section of Settings
    QVERIFY(!call(QStringLiteral("ui_key"), {{"key", "Ctrl+,"}}).result.isError);
    QTRY_COMPARE(h->controller->screen(), QStringLiteral("settings"));
    QVERIFY(!call(QStringLiteral("ui_key"), {{"key", "Ctrl+1"}}).result.isError);
    QTRY_COMPARE(h->controller->screen(), QStringLiteral("library"));
  }

  // The brake: danger buttons, destructive-labelled buttons and Return on them are refused, and nothing happens.
  void destructiveControlsAreRefused() {
    QQuickItem* root = inject(R"QML(
      Item { x: 10; y: 80; width: 400; height: 200
        property int dangerClicks: 0
        property int shareClicks: 0
        property int okClicks: 0
        property int markedClicks: 0
        UzButton { objectName: "probeDanger"; x: 0; y: 0; kind: "danger"; text: "Delete slot"; onClicked: parent.dangerClicks++ }
        UzButton { objectName: "probeShare"; x: 140; y: 0; text: "Share with others"; onClicked: parent.shareClicks++ }
        UzButton { objectName: "probeMarked"; x: 0; y: 50; text: "Go on"; destructive: true; onClicked: parent.markedClicks++ }
        UzButton { objectName: "probeOk"; x: 140; y: 50; text: "Refresh"; onClicked: parent.okClicks++ }
      }
    )QML");
    QVERIFY(root != nullptr);
    QTRY_VERIFY(h->item("probeOk")->isVisible());
    QVERIFY(h->item("probeDanger")->property("destructive").toBool());

    for (const QString& t : {QStringLiteral("probeDanger"), QStringLiteral("probeShare"), QStringLiteral("probeMarked"), QStringLiteral("Delete slot"), QStringLiteral("Share with others")}) {
      const Answer a = call(QStringLiteral("ui_click"), {{"target", t}});
      QVERIFY2(a.result.isError, qPrintable(t));
      QVERIFY2(a.text().startsWith(QStringLiteral("needs_confirmation_in_app")), qPrintable(a.text()));
    }
    // by coordinates, too
    const QPointF p = h->item("probeDanger")->mapToScene(QPointF(10, 10));
    QVERIFY(call(QStringLiteral("ui_click"), {{"x", p.x()}, {"y", p.y()}}).result.isError);
    // Return on the focused danger button
    h->item("probeDanger")->forceActiveFocus();
    const Answer k = call(QStringLiteral("ui_key"), {{"key", "Return"}});
    QVERIFY(k.result.isError);
    QVERIFY(k.text().startsWith(QStringLiteral("needs_confirmation_in_app")));
    QVERIFY(call(QStringLiteral("ui_key"), {{"key", "Space"}, {"target", "probeShare"}}).result.isError);
    QTest::qWait(200);
    QCOMPARE(root->property("dangerClicks").toInt(), 0);
    QCOMPARE(root->property("shareClicks").toInt(), 0);
    QCOMPARE(root->property("markedClicks").toInt(), 0);

    // an ordinary button works
    const Answer ok = call(QStringLiteral("ui_click"), {{"target", "probeOk"}});
    QVERIFY2(!ok.result.isError, qPrintable(ok.text()));
    QTRY_COMPARE(root->property("okClicks").toInt(), 1);
    // the tree marks the controls the person must operate
    const QJsonObject j = call(QStringLiteral("ui_tree")).json();
    QVERIFY(findNode(j, QStringLiteral("probeDanger")).value(QStringLiteral("destructive")).toBool());
    QVERIFY(!findNode(j, QStringLiteral("probeOk")).contains(QStringLiteral("destructive")));
    delete root;
  }

  // Password fields: typing works, the content never comes back (tree, type answer, text search), the screenshot shows dots only.
  void passwordTextIsNeverReturned() {
    QQuickItem* root = inject(R"QML(
      Item { x: 10; y: 300; width: 400; height: 100
        UzField { objectName: "probePassword"; x: 0; y: 0; width: 240; echoMode: TextInput.Password; text: "hunter2-preset"
                  placeholderText: "Password" }
      }
    )QML");
    QVERIFY(root != nullptr);
    QTRY_VERIFY(h->item("probePassword")->isVisible());
    const Answer tree = call(QStringLiteral("ui_tree"));
    const QString all = QString::fromUtf8(QJsonDocument(tree.json()).toJson());
    QVERIFY(!all.contains(QStringLiteral("hunter2")));
    const QJsonObject node = findNode(tree.json(), QStringLiteral("probePassword"));
    QVERIFY(!node.isEmpty());
    QCOMPARE(node.value(QStringLiteral("text")).toString(), QStringLiteral("***"));
    QVERIFY(node.value(QStringLiteral("secret")).toBool());

    const Answer typed = call(QStringLiteral("ui_type"), {{"target", "probePassword"}, {"text", "s3cret-typed"}, {"clear", true}});
    QVERIFY2(!typed.result.isError, qPrintable(typed.text()));
    QVERIFY(!typed.text().contains(QStringLiteral("s3cret")));
    QCOMPARE(h->item("probePassword")->property("text").toString(), QStringLiteral("s3cret-typed"));  // it was typed for real
    QCOMPARE(typed.json().value(QStringLiteral("typed")).toInt(), 12);
    QVERIFY(typed.json().value(QStringLiteral("secret")).toBool());
    QVERIFY(!typed.json().contains(QStringLiteral("value")));

    // A text search never matches a secret field's content.
    QVERIFY(call(QStringLiteral("ui_click"), {{"target", "s3cret-typed"}}).result.isError);
    delete root;
  }

  void screenshotIsPng() {
    const Answer a = call(QStringLiteral("app_screenshot"));
    QVERIFY2(!a.result.isError, qPrintable(a.text()));
    QByteArray png;
    for (const ToolContent& c : a.result.content)
      if (c.kind == ToolContent::Kind::Image) png = c.png;
    QVERIFY(png.size() > 100);
    QVERIFY(png.startsWith(QByteArray("\x89PNG")));
    QImage img;
    QVERIFY(img.loadFromData(png, "PNG"));
    QVERIFY(img.width() > 100 && img.height() > 100);
    QVERIFY(!uitest::isAllBlack(img));
    QVERIFY(call(QStringLiteral("app_screenshot"), {{"window", "42"}}).result.isError);
  }

  void scrollAndGameToolsAnswer() {
    QVERIFY(!call(QStringLiteral("ui_scroll"), {{"dy", 3}}).result.isError);
    QVERIFY(call(QStringLiteral("ui_scroll"), {}).result.isError);
    QCOMPARE(call(QStringLiteral("game_stop")).text().trimmed(), QStringLiteral("no_game"));
    const Answer missing = call(QStringLiteral("game_start"), {{"gameId", "missing"}});
    QVERIFY(missing.result.isError);
    QVERIFY(missing.text().startsWith(QStringLiteral("unknown_game")));
    QCOMPARE(h->controller->selectedGameId(), QStringLiteral("g1"));  // the selection is untouched
  }

  // 0.22: game_start identity {system, code} overrides the game identity for plugin matching; only with Developer mode on.
  void gameStartIdentityNeedsDeveloperMode() {
    const ToolDef* def = registry.find(QStringLiteral("game_start"));
    QVERIFY(def != nullptr);
    QVERIFY(def->inputSchema.value("properties").toObject().contains("identity"));
    const QJsonObject identity{{"system", "3ds"}, {"code", "0004000000174600"}};
    ui::PluginsController* plugins = h->controller->plugins();
    plugins->setDeveloperMode(false);
    const Answer refused = call(QStringLiteral("game_start"), {{"gameId", "g1"}, {"identity", identity}});
    QVERIFY(refused.result.isError);
    QVERIFY(refused.text().startsWith(QStringLiteral("developer_mode_required")));
    QVERIFY(plugins->identityOverride().isEmpty());
    plugins->setDeveloperMode(true);
    const Answer incomplete = call(QStringLiteral("game_start"), {{"gameId", "g1"}, {"identity", QJsonObject{{"system", "3ds"}}}});
    QVERIFY(incomplete.result.isError);
    QVERIFY(incomplete.text().startsWith(QStringLiteral("identity needs")));
    const Answer unknown = call(QStringLiteral("game_start"), {{"gameId", "missing"}, {"identity", identity}});
    QVERIFY(unknown.text().startsWith(QStringLiteral("unknown_game")));
    QVERIFY(plugins->identityOverride().isEmpty());  // nothing is armed for a start that did not happen
    const Answer bad = call(QStringLiteral("game_start"), {{"gameId", "g1"}, {"identity", QJsonObject{{"system", "3DS!"}, {"code", "x"}}}});
    QVERIFY(bad.result.isError);
    QVERIFY(plugins->identityOverride().isEmpty());
    plugins->setDeveloperMode(false);
  }

  void noQmlWarnings() { QCOMPARE(uitest::warningCount().load(), 0); }
};

QTEST_MAIN(McpUiTest)
#include "mcp_ui_test.moc"
