// Plugin UI slots in the Player (0.11 package 2): sidebar entry + plugin screen, Settings > Plugins "Adds" lines, in-game
// panel tabs (fitting, overflow, news / paused dots, actions, rail), HUD widgets (corners, max 4, menu, hide, persistence),
// notices (stack of 3 + pill, auto-dismiss, mute, host notice) and the tile strips on remote Multiview tiles. The game is a
// preview (no core); in-game plugin UI comes from PluginsController::setPreviewUi, the sidebar screen from a real homebrew
// Lua plugin in the shell context. With UZUME_SCREENSHOT_DIR=<dir> the views are also saved as PNGs (never checked in).
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickWindow>
#include <QSignalSpy>
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
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->sessions()->hubLink(), QStringLiteral("online"), 8000);
}

QJsonObject sessionObj(const QString& id, const QString& who, const QString& game) {
  return {{QStringLiteral("session_id"), id},
          {QStringLiteral("game_id"), QStringLiteral("t1")},
          {QStringLiteral("game_title"), game},
          {QStringLiteral("owner"), QJsonObject{{QStringLiteral("user_id"), QStringLiteral("u_") + who.toLower()},
                                                {QStringLiteral("display_name"), who},
                                                {QStringLiteral("device_name"), QStringLiteral("PC")}}},
          {QStringLiteral("visibility"), QStringLiteral("hub_users")},
          {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().addSecs(-600).toString(Qt::ISODate)},
          {QStringLiteral("viewer_count"), 0},
          {QStringLiteral("is_owner"), false},
          {QStringLiteral("invited"), false}};
}

QImage fakeFrame(int scale, const QColor& tint) {
  QImage img(256 * scale, 384 * scale, QImage::Format_RGB32);
  QPainter p(&img);
  p.scale(scale, scale);
  QLinearGradient sky(0, 0, 0, 192);
  sky.setColorAt(0, tint.lighter(150));
  sky.setColorAt(1, tint.darker(160));
  p.fillRect(QRect(0, 0, 256, 192), sky);
  p.setPen(Qt::NoPen);
  p.setBrush(tint.darker(220));
  p.drawPolygon(QPolygon({QPoint(0, 192), QPoint(60, 110), QPoint(110, 150), QPoint(170, 90), QPoint(256, 192)}));
  p.fillRect(QRect(0, 192, 256, 192), QColor(28, 34, 44));
  p.setBrush(tint);
  for (int i = 0; i < 3; ++i) p.drawRoundedRect(QRect(24, 212 + i * 52, 208, 40), 8, 8);
  return img;
}

emu::DisplayProfile ndsProfile() {
  emu::DisplayProfile p;
  p.layout = QStringLiteral("vertical");
  p.gap = 0;
  p.screens = {{QStringLiteral("top"), 256, 192, false}, {QStringLiteral("bottom"), 256, 192, true}};
  return p;
}

QVariantMap json(const char* text) {
  QJsonParseError err;
  const QJsonDocument d = QJsonDocument::fromJson(QByteArray(text), &err);
  if (err.error != QJsonParseError::NoError) qFatal("bad test JSON: %s", qPrintable(err.errorString()));
  return d.toVariant().toMap();
}

QVariantMap tabEntry(const QString& id, const QString& name, const QString& dot = {}, bool paused = false, const QString& origin = QStringLiteral("local")) {
  return {{"pluginId", id}, {"name", name}, {"title", name}, {"icon", QString()}, {"origin", origin}, {"dot", dot}, {"paused", paused}};
}

QVariantMap hudEntry(const QString& id, const QString& name, const QString& corner, const QString& text, bool hidden = false) {
  return {{"pluginId", id}, {"name", name}, {"icon", QString()}, {"corner", corner}, {"hidden", hidden}, {"origin", QStringLiteral("local")},
          {"tree", QVariantMap{{"type", "text"}, {"text", text}, {"style", "meta"}}}};
}

QVariantMap toastEntry(int id, const QString& pluginId, const QString& name, const QString& tone, const QString& text,
                       const QVariantList& actions = {}, bool host = false) {
  return {{"toastId", id}, {"pluginId", pluginId}, {"pluginName", name}, {"tone", tone}, {"title", host ? QStringLiteral("plugin paused") : QString()},
          {"text", text}, {"actions", actions}, {"origin", QStringLiteral("local")}, {"host", host}};
}

QVariantMap action(const QString& label, const QString& act, const QVariant& value = {}) {
  return {{"label", label}, {"action", act}, {"value", value}};
}

QVariantMap hotkeyPlugin(const QString& id, const QString& name, const QString& origin, const QVariantList& hotkeys) {
  return {{"pluginId", id}, {"name", name}, {"origin", origin}, {"hotkeys", hotkeys}};
}

QVariantMap hotkeyDecl(const QString& id, const QString& label, const QString& key) { return {{"id", id}, {"label", label}, {"key", key}}; }

QString writePlugin(const QString& parent, const QString& id, const QString& name, const QJsonObject& ui, const QString& lua) {
  const QString dir = QDir(parent).filePath(id);
  QDir().mkpath(dir);
  QJsonObject m{{"id", id}, {"name", name}, {"version", "1.0.0"}, {"api", 1}, {"type", "script"}, {"entry", "main.lua"},
                {"permissions", QJsonArray{"ui"}}, {"ui", ui}};
  QFile mf(dir + "/manifest.json");
  if (!mf.open(QIODevice::WriteOnly)) return {};
  mf.write(QJsonDocument(m).toJson());
  QFile lf(dir + "/main.lua");
  if (!lf.open(QIODevice::WriteOnly)) return {};
  lf.write(lua.toUtf8());
  return dir;
}

const char* kRunsLua = R"LUA(
  local n = 0
  local function render()
    fb.ui.set('screen', {type='column', gap=12, children={
      {type='text', style='title', text='Ember & Tide'},
      {type='card', title='Active run', eyebrow='Run', children={
        {type='row', gap=8, children={{type='badge', text='Running', tone='ok'}, {type='text', style='meta', text='3 players'}}},
        {type='bar', id='progress', value=0.6, label='Progress'}}},
      {type='row', gap=8, children={
        {type='button', id='btnInc', label='Count ' .. n, variant='primary', action='inc', value=n},
        {type='button', label='Later', variant='quiet', action='later'}}},
      {type='list', rows={
        {id='r1', title='Route 1', sub='Rattata', meta='open', tone='info'},
        {id='r2', title='Route 2', sub='Caught', meta='caught', tone='ok'},
        {id='r3', title='Route 3', sub='Fainted', meta='dead', tone='neutral'}}}
    }})
  end
  fb.ui.on_action(function(a, v, slot) if a == 'inc' then n = n + 1 render() end end)
  fb.ui.badge(2)
  render()
)LUA";

const char* kBoomLua = R"LUA(
  fb.ui.set('screen', {type='text', text='about to fail'})
  error('boom')
)LUA";

}  // namespace

class PluginSlotsUiTest : public QObject {
  Q_OBJECT

  QString shotDir_;
  QTemporaryDir src_;
  FakeHub hub_{QStringLiteral("a")};
  Harness h_;

  PluginsController* pc() { return h_.controller->plugins(); }
  void shot(const QString& name) {
    if (shotDir_.isEmpty()) return;
    QQuickTest::qWaitForPolish(h_.window);
    QTest::qWait(400);
    QDir().mkpath(shotDir_);
    const QImage img = h_.window->grabWindow();
    QVERIFY2(!img.isNull() && !uitest::isAllBlack(img), qPrintable(name));
    QVERIFY(img.save(QDir(shotDir_).filePath(name + QStringLiteral(".png"))));
  }
  // Whether the panel tab bar can hold all these tabs with their labels (same widths as GamePanel.qml: gap 18, label at most 96, icon 16 + 6, unread dot 10). A very wide font (Windows fallback) cannot: the last tabs then move into "More", by design.
  bool tabsFit(const QStringList& names, const QStringList& withDot = {}) {
    QFont f = QGuiApplication::font();
    f.setPixelSize(13);
    const QFontMetricsF fm(f);
    qreal w = 18.0 * (names.size() - 1);
    for (const QString& n : names)
      w += std::min<qreal>(96.0, std::ceil(fm.horizontalAdvance(n))) + 22 + (withDot.contains(n) ? 10 : 0);
    return w + 4 <= h_.item("panelTabBar")->width() - 48;   // 4 px slack for rounding, 24 px padding on both sides
  }
  static qreal sceneH(QQuickItem* it) { return it->mapRectToScene(QRectF(0, 0, it->width(), it->height())).height(); }
  // The attached ToolTip.text of an item.
  QString tooltipOf(QQuickItem* it) {
    const QQmlProperty p(it, QStringLiteral("ToolTip.text"), qmlContext(it));
    return p.isValid() ? p.read().toString() : QString();
  }
  QString accessibleName(QQuickItem* it) {
    const QQmlProperty p(it, QStringLiteral("Accessible.name"), qmlContext(it));
    return p.isValid() ? p.read().toString() : QString();
  }
  int visibleCount(const char* name) {
    int n = 0;
    for (QQuickItem* i : h_.items(name))
      if (i->isVisible()) ++n;
    return n;
  }
  QQuickItem* visibleItem(const char* name) {
    for (QQuickItem* i : h_.items(name))
      if (i->isVisible()) return i;
    return nullptr;
  }
  QQuickItem* gameScreen() {
    for (QQuickItem* i = h_.item("gameHeader"); i != nullptr; i = i->parentItem())
      if (QString::fromLatin1(i->metaObject()->className()).startsWith(QLatin1String("GameScreen"))) return i;
    return nullptr;
  }
  QQuickItem* layer() { return h_.item("pluginHudLayer"); }
  QPointF center(QQuickItem* it) { return it->mapToScene(QPointF(it->width() / 2, it->height() / 2)); }
  void rightClick(QQuickItem* it) {
    QQuickTest::qWaitForPolish(h_.window);
    QTest::mouseClick(h_.window, Qt::RightButton, Qt::NoModifier, center(it).toPoint());
  }
  void hover(QQuickItem* it) {
    QTest::mouseMove(h_.window, center(it).toPoint());
    QTest::qWait(60);
    QTest::mouseMove(h_.window, center(it).toPoint() + QPoint(1, 1));
    QTest::qWait(60);
  }
  // Click on the center of a named descendant of a named item (several items share names like toastDismiss).
  bool clickIn(const char* parentName, const char* childName) {
    QQuickItem* p = h_.item(parentName);
    if (p == nullptr) return false;
    QQuickItem* c = Harness::findVisual(p, QString::fromLatin1(childName));
    if (c == nullptr || !c->isVisible()) return false;
    QQuickTest::qWaitForPolish(h_.window);
    QTest::mouseClick(h_.window, Qt::LeftButton, Qt::NoModifier, center(c).toPoint());
    return true;
  }
  // Scene position of a named item (-1 while it does not exist, e.g. between delegate rebuilds).
  QPointF posOf(const char* name) {
    QQuickItem* it = h_.item(name);
    return it != nullptr && it->isVisible() ? it->mapToScene(QPointF(0, 0)) : QPointF(-1, -1);
  }
  static QString textOf(QQuickItem* it) {
    if (it->property("text").isValid() && !it->property("text").toString().isEmpty()) return it->property("text").toString();
    for (QQuickItem* c : it->childItems())
      if (const QString t = textOf(c); !t.isEmpty()) return t;
    return {};
  }
  PlayerSettings freshSettings() { return PlayerSettings(h_.controller->profileStore()->baseDir()); }

 private slots:
  void initTestCase() {
    shotDir_ = qEnvironmentVariable("UZUME_SCREENSHOT_DIR");
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
    hub_.hubId = QStringLiteral("hub-slots");
    hub_.name = QStringLiteral("Home");
    hub_.decision = FakeHub::Decision::Approve;
    hub_.features = {QStringLiteral("saves_v1"), QStringLiteral("sessions_v1")};
    hub_.sessions.insert(QStringLiteral("s1"), sessionObj(QStringLiteral("s1"), QStringLiteral("Lena"), QStringLiteral("Harbor Rally")));
    hub_.sessions.insert(QStringLiteral("s2"), sessionObj(QStringLiteral("s2"), QStringLiteral("Jonas"), QStringLiteral("Lumen Drift")));
    hub_.sessions.insert(QStringLiteral("s3"), sessionObj(QStringLiteral("s3"), QStringLiteral("Mo"), QStringLiteral("Pixel Quest")));
    QVERIFY(hub_.start());
    QVERIFY(h_.start());
    pair(h_, hub_);
    h_.window->resize(1440, 900);
    QTest::qWait(150);
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // ---------------------------------------------------------------- shell context: Settings > Plugins, sidebar

  void settingsAddsLines() {
    QVERIFY(!writePlugin(src_.path(), "net.test.runs", "Runs Plugin", QJsonObject{{"screen", QJsonObject{{"title", "Runs"}}}}, kRunsLua).isEmpty());
    QVERIFY(!writePlugin(src_.path(), "net.test.tools", "Tools Plugin",
                         QJsonObject{{"panel", QJsonObject{{"title", "Tools"}}}, {"hud", true}, {"tile", true}, {"detail", true},
                                     {"hotkeys", QJsonArray{QJsonObject{{"id", "toggle-hud"}, {"label", "Toggle"}}}}},
                         "fb.ui.set('panel', {type='text', text='x'})").isEmpty());
    for (const char* id : {"net.test.runs", "net.test.tools"}) {
      const auto r = pc()->manager()->installFromFolder(src_.filePath(QString::fromLatin1(id)));
      QVERIFY2(r.ok, qPrintable(r.error));
    }
    QTRY_COMPARE(pc()->plugins().size(), 2);
    h_.controller->showSettings();
    QTest::qWait(150);
    QVERIFY(h_.click("section_plugins"));
    QTRY_VERIFY(h_.item("pluginRow_net.test.tools") != nullptr && h_.item("pluginRow_net.test.tools")->isVisible());
    // The "Adds:" line lives in the detail sheet of each plugin (4a-4).
    QStringList adds;
    for (const char* id : {"net.test.runs", "net.test.tools"}) {
      QQuickItem* row = h_.item(QByteArray("pluginRow_") + id);
      QVERIFY(row != nullptr);
      QVERIFY(h_.click((QByteArray("pluginRow_") + id).constData()));
      QTRY_VERIFY(h_.item("pluginAdds") != nullptr && h_.item("pluginAdds")->isVisible());
      adds << h_.item("pluginAdds")->property("text").toString();
      QVERIFY(h_.click("pluginDetailClose"));
      QTest::qWait(150);
    }
    adds.sort();
    QCOMPARE(adds.size(), 2);
    QCOMPARE(adds.at(0), QStringLiteral("Adds: panel tab · HUD · tile strip · game detail · 1 hotkey"));
    QCOMPARE(adds.at(1), QStringLiteral("Adds: run tab"));
    shot(QStringLiteral("slots-settings-plugins-adds"));
  }

  // Plugins have no sidebar row any more: a plugin with ui.screen shows its badge in Settings > Plugins and opens its screen as a
  // sheet from the plugin's detail ("Open").
  void noSidebarEntryScreenOpensFromSettings() {
    pc()->setPluginEnabled(QStringLiteral("net.test.runs"), true);
    pc()->setDeveloperMode(true);
    QTRY_COMPARE_WITH_TIMEOUT(pc()->sidebarEntries().size(), 1, 10000);  // the plugin runs in the shell; its tree exists
    h_.controller->showLibrary();
    QTest::qWait(200);
    QVERIFY(h_.item("navPlugin_net.test.runs") == nullptr);
    QVERIFY(visibleItem("navPluginBadge") == nullptr);
    QVERIFY(visibleItem("navLibrary") != nullptr && visibleItem("navRuns") != nullptr);

    h_.controller->showSettings();
    QTest::qWait(150);
    QVERIFY(h_.click("section_plugins"));
    QTRY_VERIFY(h_.item("pluginRow_net.test.runs") != nullptr && h_.item("pluginRow_net.test.runs")->isVisible());
    // fb.ui.badge(2) shows in the plugin's row.
    QTRY_VERIFY(visibleItem("pluginBadge") != nullptr);
    QCOMPARE(textOf(visibleItem("pluginBadge")), QStringLiteral("2"));
    QVERIFY(h_.click("pluginRow_net.test.runs"));
    QTRY_VERIFY(h_.item("pluginDetailOpen") != nullptr && h_.item("pluginDetailOpen")->isVisible());
    QVERIFY(h_.item("pluginDetailOpen")->isEnabled());
    QVERIFY(h_.click("pluginDetailOpen"));
    QTRY_VERIFY(h_.item("appPluginScreenSheet")->isVisible());
    QTRY_VERIFY(h_.item("plugin_btnInc") != nullptr && h_.item("plugin_btnInc")->isVisible());
    QCOMPARE(h_.controller->screen(), QStringLiteral("settings"));
    shot(QStringLiteral("4f-3-plugin-screen-sheet-dark"));

    // Action round trip: button -> plugin thread -> new tree; the focused button survives the re-set (stable delegates).
    QQuickItem* before = h_.item("plugin_btnInc");
    QVERIFY(h_.click("plugin_btnInc"));
    QTRY_COMPARE_WITH_TIMEOUT(pc()->slotTree("net.test.runs", "screen").value("children").toList().at(2).toMap().value("children").toList().at(0).toMap().value("label").toString(),
                              QStringLiteral("Count 1"), 5000);
    QTRY_COMPARE(h_.item("plugin_btnInc")->property("visible").toBool(), true);
    QCOMPARE(h_.item("plugin_btnInc"), before);

    // Back closes the sheet (Esc too).
    QVERIFY(h_.click("pluginScreenBack"));
    QTRY_VERIFY(!h_.item("appPluginScreenSheet")->isVisible());
    h_.controller->openPluginScreen(QStringLiteral("net.test.runs"));
    QTRY_VERIFY(h_.item("appPluginScreenSheet")->isVisible());
    QTest::keyClick(h_.window, Qt::Key_Escape);
    QTRY_VERIFY(!h_.item("appPluginScreenSheet")->isVisible());

    // Turning the plugin off removes its entry.
    pc()->setPluginEnabled(QStringLiteral("net.test.runs"), false);
    QTRY_COMPARE_WITH_TIMEOUT(pc()->sidebarEntries().size(), 0, 10000);
  }

  void openRequestOutOfGame() {
    // fb.ui.open("screen") from a shell plugin outside a game: the plugin screen sheet; "panel" is ignored.
    QVERIFY(!writePlugin(src_.path(), "net.test.opener", "Opener Plugin", QJsonObject{{"screen", QJsonObject{{"title", "Opener"}}}},
                         "fb.ui.set('screen', {type='text', text='hello'})\n"
                         "fb.ui.on_action(function(a) if a == 'screen' then fb.ui.open('screen') else pcall(fb.ui.open, 'panel') end end)").isEmpty());
    const auto r = pc()->manager()->installFromFolder(src_.filePath(QStringLiteral("net.test.opener")));
    QVERIFY2(r.ok, qPrintable(r.error));
    pc()->setPluginEnabled(QStringLiteral("net.test.opener"), true);
    QTRY_COMPARE_WITH_TIMEOUT(pc()->sidebarEntries().size(), 1, 10000);
    h_.controller->showLibrary();
    QTRY_COMPARE(h_.controller->screen(), QStringLiteral("library"));
    QSignalSpy spy(pc(), &PluginsController::openRequested);
    QSignalSpy sheet(h_.controller.get(), &PlayerController::pluginScreenRequested);
    pc()->sendAction(QStringLiteral("net.test.opener"), QStringLiteral("hotkey"), QStringLiteral("other"));
    pc()->sendAction(QStringLiteral("net.test.opener"), QStringLiteral("hotkey"), QStringLiteral("screen"));
    QTRY_VERIFY_WITH_TIMEOUT(spy.size() >= 1, 5000);
    QCOMPARE(spy.last().at(0).toString(), QStringLiteral("net.test.opener"));
    QCOMPARE(spy.last().at(1).toString(), QStringLiteral("screen"));
    QTRY_COMPARE(sheet.size(), 1);
    QCOMPARE(sheet.last().at(0).toString(), QStringLiteral("net.test.opener"));
    QCOMPARE(h_.controller->screen(), QStringLiteral("library"));  // no page of its own
    QTRY_VERIFY(h_.item("appPluginScreenSheet")->isVisible());
    QTRY_VERIFY(h_.item("pluginScreenSheetBlock") != nullptr);
    QTest::keyClick(h_.window, Qt::Key_Escape);
    QTRY_VERIFY(!h_.item("appPluginScreenSheet")->isVisible());
    pc()->setPluginEnabled(QStringLiteral("net.test.opener"), false);
    QTRY_VERIFY_WITH_TIMEOUT(pc()->sidebarEntries().isEmpty() && !pc()->shellRunning(), 10000);
  }

  void hostNoticeWhenPluginPauses() {
    // Isolation: nothing of the previous test may still run (the shell set restarts asynchronously).
    for (const QVariant& p : pc()->plugins()) pc()->setPluginEnabled(p.toMap().value("id").toString(), false);
    QTRY_VERIFY_WITH_TIMEOUT(pc()->sidebarEntries().isEmpty() && !pc()->shellRunning(), 10000);
    pc()->setPreviewUi({});
    QVERIFY(!writePlugin(src_.path(), "net.test.boom", "Boom Plugin", QJsonObject{{"screen", QJsonObject{{"title", "Boom"}}}}, kBoomLua).isEmpty());
    const auto r = pc()->manager()->installFromFolder(src_.filePath(QStringLiteral("net.test.boom")));
    QVERIFY2(r.ok, qPrintable(r.error));
    pc()->setPluginEnabled(QStringLiteral("net.test.boom"), true);
    QTRY_VERIFY_WITH_TIMEOUT(!pc()->toasts().isEmpty(), 10000);
    const QVariantMap t = pc()->toasts().last().toMap();
    QVERIFY(t.value("host").toBool());
    QVERIFY(t.value("toastId").toInt() < 0);
    QCOMPARE(t.value("pluginName").toString(), QStringLiteral("Uzume"));
    QCOMPARE(t.value("title").toString(), QStringLiteral("Boom Plugin paused after an error"));
    QVERIFY(t.value("text").toString().contains(QStringLiteral("Your game keeps running")));
    const QVariantList acts = t.value("actions").toList();
    QCOMPARE(acts.at(0).toMap().value("action").toString(), QStringLiteral("restart"));
    QCOMPARE(acts.size(), 2);  // developer mode is on: "Open console" too
    QCOMPARE(acts.at(1).toMap().value("label").toString(), QStringLiteral("Open console"));
    pc()->dismissToast(t.value("toastId").toInt());
    pc()->setPluginEnabled(QStringLiteral("net.test.boom"), false);
    QTRY_VERIFY_WITH_TIMEOUT(pc()->sidebarEntries().isEmpty() && !pc()->shellRunning(), 10000);
    pc()->setPreviewUi({});  // leaves nothing behind
    QCOMPARE(pc()->toasts().size(), 0);
  }

  // ---------------------------------------------------------------- in game

  void startGame() {
    GameSession* gs = h_.controller->gameSession();
    gs->setPreview(QStringLiteral("Uzume Test"), fakeFrame(1, QColor(60, 140, 200)), ndsProfile(), EmulationDiagnostics());
    emit gs->started();
    QTRY_COMPARE_WITH_TIMEOUT(h_.controller->screen(), QStringLiteral("game"), 8000);
    h_.controller->sessions()->gameStarted(QStringLiteral("t1"), QStringLiteral("Uzume Test"));
    h_.controller->sessions()->setTab(QStringLiteral("session"));
    QQuickItem* screen = gameScreen();
    QVERIFY(screen != nullptr);
    screen->setProperty("applyToWindow", false);
    QQuickTest::qWaitForPolish(h_.window);
    QVERIFY(layer() != nullptr);
  }

  void panelTabsFitAndOverflow() {
    // No plugin tabs: the Session tab alone, no overflow.
    QVERIFY(pc()->panelTabs().isEmpty() && !pc()->consoleAvailable());
    QTRY_VERIFY(visibleCount("panelTabLabel") <= 1 && !h_.item("panelTabOverflow")->isVisible());  // the Game tab alone

    // One plugin tab: labels for both; the panel content is the plugin's tree.
    QVariantMap trees{{"net.test.a|panel", json(R"({"type":"column","gap":8,"children":[
        {"type":"text","style":"title","text":"SoulLink"},
        {"type":"card","title":"Team","eyebrow":"Run","children":[{"type":"text","text":"Max, Lena, Jonas"}]},
        {"type":"button","id":"btnGo","label":"Go","variant":"primary","action":"go","value":7}]})")}};
    pc()->setPreviewUi({{"panelTabs", QVariantList{tabEntry("net.test.a", "SoulLink", "news")}}, {"trees", trees}});
    QTRY_VERIFY(h_.item("panelTabBar")->isVisible() && h_.item("panelTabBar")->width() > 100);
    QTRY_VERIFY(h_.item("panelTab_plugin:net.test.a") != nullptr);
    QVERIFY(h_.item("panelTab_game")->isVisible());
    QVERIFY(h_.item("panelTab_plugin:net.test.a")->isVisible());
    if (tabsFit({"Game", "SoulLink"}, {"SoulLink"})) QCOMPARE(visibleCount("panelTabLabel"), 2);
    QCOMPARE(visibleCount("tabDot"), 1);  // news dot
    {   // The plugin tab reads "SoulLink" (icon + short label, never a single letter), with a tooltip and an accessible name.
      QQuickItem* tab = h_.item("panelTab_plugin:net.test.a");
      QQuickItem* label = nullptr;
      for (QQuickItem* c : tab->findChildren<QQuickItem*>(QStringLiteral("panelTabLabel"))) label = c;
      QVERIFY(label != nullptr);
      QCOMPARE(label->property("text").toString(), QStringLiteral("SoulLink"));
      QVERIFY(tab->findChild<QQuickItem*>(QStringLiteral("panelTabIcon")) != nullptr);
      QVERIFY2(tooltipOf(tab).startsWith(QStringLiteral("SoulLink")), qPrintable(tooltipOf(tab)));   // + the origin marker
      QCOMPARE(accessibleName(tab), QStringLiteral("SoulLink"));
    }
    QVERIFY(h_.click("panelTab_plugin:net.test.a"));
    QTRY_VERIFY(h_.item("gamePanelPlugin")->isVisible());
    shot(QStringLiteral("slots-panel-2-tabs"));

    // Action round trip: panel button -> sendAction(plugin, "panel", action, value).
    QSignalSpy sent(pc(), &PluginsController::actionSent);
    QTRY_VERIFY(h_.item("plugin_btnGo") != nullptr && h_.item("plugin_btnGo")->isVisible());
    QVERIFY(h_.click("plugin_btnGo"));
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(sent.at(0).at(0).toString(), QStringLiteral("net.test.a"));
    QCOMPARE(sent.at(0).at(1).toString(), QStringLiteral("panel"));
    QCOMPARE(sent.at(0).at(2).toString(), QStringLiteral("go"));
    QCOMPARE(sent.at(0).at(3).toInt(), 7);

    // Four tabs (Session + 3): every tab keeps its icon and label while they fit; else the last ones go to "More".
    pc()->setPreviewUi({{"panelTabs", QVariantList{tabEntry("net.test.a", "SoulLink", "news"), tabEntry("net.test.b", "Item Helper", {}, false, "unverified"),
                                                    tabEntry("net.test.c", "Party Editor")}},
                        {"trees", trees}});
    if (tabsFit({"Game", "SoulLink", "Item Helper", "Party Editor"}, {"SoulLink"})) {
      QTRY_COMPARE(visibleCount("panelTabLabel"), 4);
      QVERIFY(!h_.item("panelTabOverflow")->isVisible());
    } else {
      QTRY_VERIFY(h_.item("panelTabOverflow")->isVisible());
    }
    if (h_.item("panelTab_plugin:net.test.c") != nullptr && h_.item("panelTab_plugin:net.test.c")->isVisible()) {
      QVERIFY(h_.click("panelTab_plugin:net.test.c"));
    } else {   // a very wide font moved it into the "More" menu
      QVERIFY(h_.click("panelTabOverflow"));
      QTRY_VERIFY(h_.item("menuItem_tabOverflow_plugin:net.test.c") != nullptr && h_.item("menuItem_tabOverflow_plugin:net.test.c")->isVisible());
      QVERIFY(h_.click("menuItem_tabOverflow_plugin:net.test.c"));
    }
    QTRY_VERIFY(h_.item("panelTab_plugin:net.test.c")->property("open").toBool());
    shot(QStringLiteral("slots-panel-4-tabs"));

    // Narrow window (320 px panel) with many tabs: the rest moves into the "More ▾" tab menu.
    h_.window->resize(1280, 800);
    QVariantList many;
    for (int i = 0; i < 9; ++i) many << tabEntry(QStringLiteral("net.test.p%1").arg(i), QStringLiteral("Plugin %1").arg(i), i == 8 ? QStringLiteral("news") : QString());
    pc()->setPreviewUi({{"panelTabs", many}, {"trees", trees}});
    QTRY_VERIFY(h_.item("panelTabOverflow")->isVisible());
    int shownPlugins = 0;
    for (int i = 0; i < 9; ++i) {
      QQuickItem* it = h_.item(qPrintable(QStringLiteral("panelTab_plugin:net.test.p%1").arg(i)));
      if (it != nullptr && it->isVisible()) ++shownPlugins;
    }
    QVERIFY(shownPlugins < 9);   // some go to "More" (all of them at a very wide font)
    const QString overflowText = [&]() {
      for (QQuickItem* c : h_.item("panelTabOverflow")->childItems())
        if (c->property("text").isValid()) return c->property("text").toString();
      return QString();
    }();
    QCOMPARE(overflowText, QStringLiteral("More ▾"));
    QCOMPARE(accessibleName(h_.item("panelTabOverflow")), QStringLiteral("More tabs"));
    QVERIFY(!tooltipOf(h_.item("panelTabOverflow")).isEmpty());
    for (int i = 0; i < 9; ++i) {   // every tab that stays shown has a label of more than one letter, a tooltip and an accessible name
      QQuickItem* it = h_.item(qPrintable(QStringLiteral("panelTab_plugin:net.test.p%1").arg(i)));
      if (it == nullptr || !it->isVisible()) continue;
      const QString want = QStringLiteral("Plugin %1").arg(i);
      QVERIFY2(tooltipOf(it).startsWith(want), qPrintable(tooltipOf(it)));
      QCOMPARE(accessibleName(it), want);
    }
    QVERIFY(h_.click("panelTabOverflow"));
    const QString lastId = QStringLiteral("menuItem_tabOverflow_plugin:net.test.p8");
    QTRY_VERIFY(h_.item(qPrintable(lastId)) != nullptr && h_.item(qPrintable(lastId))->isVisible());
    shot(QStringLiteral("slots-panel-overflow-1280"));
    QVERIFY(h_.click(qPrintable(lastId)));
    QTRY_VERIFY(h_.item("panelTab_plugin:net.test.p8")->isVisible());  // the picked tab is open and shown
    QTRY_VERIFY(h_.item("gamePanelPlugin")->isVisible());
    h_.window->resize(1440, 900);

    // A paused plugin keeps its tree, dimmed, under the "Paused" banner; "Restart plugin" reloads it.
    pc()->setPreviewUi({{"panelTabs", QVariantList{tabEntry("net.test.a", "SoulLink", "paused", true)}}, {"trees", trees}});
    QVERIFY(h_.click("panelTab_plugin:net.test.a"));
    QTRY_VERIFY(h_.item("pluginPausedBanner")->isVisible());
    QCOMPARE(h_.item("pluginPanelBlock")->property("opacity").toDouble() < 1.0, true);
    QCOMPARE(visibleCount("tabDot"), 1);
    QSignalSpy reload(pc(), &PluginsController::reloadRequested);
    QVERIFY(h_.click("pluginRestartButton"));
    QCOMPARE(reload.size(), 1);
    QCOMPARE(reload.at(0).at(0).toString(), QStringLiteral("net.test.a"));

    // Collapsed rail: one icon per plugin tab; a click opens the panel on that tab.
    pc()->setPreviewUi({{"panelTabs", QVariantList{tabEntry("net.test.a", "SoulLink"), tabEntry("net.test.b", "Item Helper", "news")}}, {"trees", trees}});
    gameScreen()->setProperty("panelCollapsed", true);
    QTRY_VERIFY(h_.item("panelRail")->isVisible());
    QCOMPARE(visibleCount("panelRailTab_net.test.a"), 1);
    QCOMPARE(visibleCount("panelRailTab_net.test.b"), 1);
    QVERIFY(h_.click("panelRailTab_net.test.b"));
    QTRY_COMPARE(gameScreen()->property("panelCollapsed").toBool(), false);
    QTRY_VERIFY(h_.item("gamePanelPlugin")->isVisible());
    // A plugin tab is a 40 px strip with the plugin name and its origin over the content (board 7-5 section 02).
    QCOMPARE(sceneH(h_.item("pluginStrip")), 40.0);
    QCOMPARE(h_.item("pluginStripName")->property("text").toString(), QStringLiteral("Item Helper"));
    QCOMPARE(h_.item("pluginStripMarker")->property("text").toString(), QStringLiteral("Local"));
    // The rail names the Game tab as well (vertical text); a click opens the panel on the Game tab.
    gameScreen()->setProperty("panelCollapsed", true);
    QTRY_VERIFY(h_.item("panelRail")->isVisible());
    QCOMPARE(h_.item("panelRailTab_game")->findChild<QQuickItem*>("panelRailLabel")->property("text").toString(), QStringLiteral("Game"));
    QCOMPARE(h_.item("panelRailTab_game")->findChild<QQuickItem*>("panelRailLabel")->rotation(), 270.0);
    QTRY_COMPARE(h_.item("panelRail")->parentItem()->width(), 56.0);
    QVERIFY(h_.click("panelRailTab_game"));
    QTRY_COMPARE(gameScreen()->property("panelCollapsed").toBool(), false);
    QTRY_VERIFY(!h_.item("gamePanelPlugin")->isVisible());
    QVERIFY(h_.item("panelTab_game")->property("open").toBool());

    // The tab disappears with its plugin: back to the Game tab.
    pc()->setPreviewUi({{"panelTabs", QVariantList{tabEntry("net.test.a", "SoulLink")}}, {"trees", trees}});
    QTRY_VERIFY(!h_.item("gamePanelPlugin")->isVisible());
    pc()->setPreviewUi({});
    QTRY_VERIFY(visibleCount("panelTabLabel") <= 1 && !h_.item("panelTabOverflow")->isVisible());  // the Game tab alone
  }

  void hudCornersMaxAndMenu() {
    QVariantList hud;
    for (int i = 0; i < 5; ++i) hud << hudEntry(QStringLiteral("net.test.bl%1").arg(i), QStringLiteral("BL %1").arg(i), "bottom-left", QStringLiteral("Bottom left %1").arg(i));
    hud << hudEntry("net.test.br", "BR", "bottom-right", "Bottom right widget");
    hud << hudEntry("net.test.tl", "TL", "top-left", "Top left widget");
    pc()->setPreviewUi({{"hudWidgets", hud}});
    QTRY_VERIFY(h_.item("hudWidget_net.test.tl") != nullptr && h_.item("hudWidget_net.test.tl")->isVisible());
    QQuickItem* lay = layer();
    // At most 4 per corner (the 5th is not shown), max 320 wide, 16 from the edge, stack grows away from the corner.
    QQuickItem* bl = h_.item("hudStack_bottom-left");
    int shown = 0;
    for (int i = 0; i < 5; ++i)
      if (QQuickItem* w = h_.item(qPrintable(QStringLiteral("hudWidget_net.test.bl%1").arg(i))); w != nullptr && w->isVisible()) ++shown;
    QCOMPARE(shown, 4);
    QVERIFY(bl->isVisible());
    QQuickItem* first = h_.item("hudWidget_net.test.bl0");
    QQuickItem* second = h_.item("hudWidget_net.test.bl1");
    QVERIFY(first->isVisible() && second->isVisible());
    QTRY_VERIFY(second->mapToItem(lay, QPointF(0, 0)).y() < first->mapToItem(lay, QPointF(0, 0)).y());  // grows upwards
    QCOMPARE(qRound(first->mapToItem(lay, QPointF(0, 0)).x()), 16);
    QCOMPARE(qRound(lay->height() - (first->mapToItem(lay, QPointF(0, 0)).y() + first->height())), 16);
    QVERIFY(first->width() <= 320);
    QQuickItem* br = h_.item("hudWidget_net.test.br");
    QCOMPARE(qRound(lay->width() - (br->mapToItem(lay, QPointF(0, 0)).x() + br->width())), 16);
    QQuickItem* tl = h_.item("hudWidget_net.test.tl");
    QCOMPARE(qRound(tl->mapToItem(lay, QPointF(0, 0)).y()), 16);
    QCOMPARE(qRound(tl->mapToItem(lay, QPointF(0, 0)).x()), 16);

    // Corner rules in the controller: top right is refused; the others persist.
    pc()->setHudCorner(QStringLiteral("net.test.tl"), QStringLiteral("top-right"));
    QCOMPARE(pc()->hudWidgets().last().toMap().value("corner").toString(), QStringLiteral("top-left"));
    QVERIFY(!freshSettings().pluginHudCorners().contains(QStringLiteral("net.test.tl")));

    // "⋯" menu on hover: move to a corner, hide, hide all.
    hover(tl);
    QTRY_VERIFY(visibleItem("hudMenuButton") != nullptr);
    QVERIFY(h_.click("hudMenuButton"));
    QTRY_VERIFY(h_.item("pluginMenu") != nullptr && h_.item("pluginMenu")->isVisible());
    shot(QStringLiteral("slots-hud-menu"));
    QVERIFY(h_.click("pluginMenuChoice_corner-bottom-right"));
    QTRY_COMPARE(pc()->hudWidgets().last().toMap().value("corner").toString(), QStringLiteral("bottom-right"));
    QCOMPARE(freshSettings().pluginHudCorners().value(QStringLiteral("net.test.tl")), QStringLiteral("bottom-right"));
    QVERIFY(!h_.item("pluginMenu")->isVisible());
    // Now two in the bottom-right stack: the moved one is the second, above the first.
    QTRY_VERIFY(posOf("hudWidget_net.test.tl").x() > lay->width() / 2);

    hover(h_.item("hudWidget_net.test.br"));
    QTRY_VERIFY(visibleItem("hudMenuButton") != nullptr);
    QVERIFY(h_.click("hudMenuButton"));
    QTRY_VERIFY(h_.item("pluginMenu")->isVisible());
    QVERIFY(h_.click("pluginMenuItem_hide"));
    QTRY_VERIFY(posOf("hudWidget_net.test.br").x() < 0);
    QVERIFY(freshSettings().pluginHudHidden().contains(QStringLiteral("net.test.br")));

    shot(QStringLiteral("slots-hud-corners"));

    // The "⋯" button stays while its menu is open, even when the mouse is elsewhere, and Esc closes the menu (not the game).
    hover(h_.item("hudWidget_net.test.bl0"));
    QTRY_VERIFY(visibleItem("hudMenuButton") != nullptr);
    QVERIFY(h_.click("hudMenuButton"));
    QTRY_VERIFY(h_.item("pluginMenu")->isVisible());
    QTest::mouseMove(h_.window, QPoint(h_.window->width() / 2, h_.window->height() / 2));
    QTest::qWait(60);
    QVERIFY(visibleItem("hudMenuButton") != nullptr);
    QTest::keyClick(h_.window, Qt::Key_Escape);
    QTRY_VERIFY(!h_.item("pluginMenu")->isVisible());
    QVERIFY(h_.controller->gameSession()->state() != GameSession::Paused);

    // Hide all: widgets go, the 2 s hint shows and then disappears; the state persists.
    lay->setProperty("hintMs", 400);
    hover(h_.item("hudWidget_net.test.bl0"));
    QTRY_VERIFY(visibleItem("hudMenuButton") != nullptr);
    QVERIFY(h_.click("hudMenuButton"));
    QTRY_VERIFY(h_.item("pluginMenu")->isVisible());
    QVERIFY(h_.click("pluginMenuItem_hideAll"));
    QTRY_VERIFY(pc()->hudAllHidden());
    QVERIFY(freshSettings().pluginHudAllHidden());
    QTRY_VERIFY(h_.item("hudHiddenHint")->isVisible());
    QCOMPARE(textOf(h_.item("hudHiddenHint")), QStringLiteral("HUD hidden · F4 to show"));
    QTRY_VERIFY(!h_.item("hudStack_bottom-left")->isVisible());
    QTRY_VERIFY(!h_.item("hudHiddenHint")->isVisible());
    pc()->setHudAllHidden(false);
    QVERIFY(!freshSettings().pluginHudAllHidden());
    QTRY_VERIFY(h_.item("hudStack_bottom-left")->isVisible());
    pc()->showHud(QStringLiteral("net.test.br"));
    QVERIFY(!freshSettings().pluginHudHidden().contains(QStringLiteral("net.test.br")));

    // Fullscreen: widgets stay, the panel goes.
    gameScreen()->setProperty("fullscreen", true);
    QTRY_VERIFY(!h_.item("gamePanel")->isVisible());
    QVERIFY(h_.item("hudWidget_net.test.bl0")->isVisible());
    gameScreen()->setProperty("fullscreen", false);
    pc()->setPreviewUi({});
  }

  void toastStackMuteAndHostNotice() {
    QVariantList toasts;
    toasts << toastEntry(1, "net.test.a", "SoulLink", "info", "Max caught a creature on Route 1.");
    toasts << toastEntry(2, "net.test.a", "SoulLink", "important", "Lena's creature fainted. The link partner is marked as dead.",
                         QVariantList{action("Open run", "open", "r1"), action("Undo death…", "undo", 3)});
    toasts << toastEntry(3, "net.test.b", "Item Helper", "warning", "Bag data not found. Open the bag once in game.");
    toasts << toastEntry(4, "net.test.b", "Item Helper", "warning", "Second warning");
    toasts << toastEntry(5, "net.test.a", "SoulLink", "important", "Route 3 is now open.");
    pc()->setPreviewUi({{"toasts", toasts}, {"hudWidgets", QVariantList{hudEntry("net.test.br", "BR", "bottom-right", "Bottom right widget")}}});
    // Max 3 visible + the pill for the 2 earlier ones; newest at the bottom, above the bottom-right HUD stack.
    QTRY_VERIFY(h_.item("toast_5") != nullptr && h_.item("toast_5")->isVisible());
    QVERIFY(h_.item("toast_4")->isVisible() && h_.item("toast_3")->isVisible());
    QVERIFY(!h_.item("toast_2")->isVisible() && !h_.item("toast_1")->isVisible());
    QVERIFY(h_.item("toastPill")->isVisible());
    QCOMPARE(textOf(h_.item("toastPill")), QStringLiteral("+2 earlier notices"));
    QQuickItem* lay = layer();
    QTRY_VERIFY(h_.item("toast_5")->mapToItem(lay, QPointF(0, 0)).y() > h_.item("toast_4")->mapToItem(lay, QPointF(0, 0)).y());
    QCOMPARE(qRound(h_.item("toast_5")->width()), 390);
    QTRY_VERIFY(h_.item("toast_5")->mapToItem(lay, QPointF(0, 0)).y() + h_.item("toast_5")->height() <=
                h_.item("hudWidget_net.test.br")->mapToItem(lay, QPointF(0, 0)).y() - 8);
    shot(QStringLiteral("slots-hud-toasts"));

    // "×" dismisses the newest one; the pill shrinks and the earlier notice 2 comes back.
    QVERIFY(clickIn("toast_5", "toastDismiss"));
    QTRY_COMPARE(pc()->toasts().size(), 4);
    QTRY_VERIFY(h_.item("toast_2")->isVisible());
    QVERIFY(!h_.item("toast_5") || !h_.item("toast_5")->isVisible() || h_.items("toast_5").isEmpty());

    // Actions go to sendAction(plugin, "toast", ...) and dismiss.
    QSignalSpy sent(pc(), &PluginsController::actionSent);
    QVERIFY(clickIn("toast_2", "toastAction_undo"));
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(sent.at(0).at(1).toString(), QStringLiteral("toast"));
    QCOMPARE(sent.at(0).at(2).toString(), QStringLiteral("undo"));
    QCOMPARE(sent.at(0).at(3).toInt(), 3);
    QTRY_COMPARE(pc()->toasts().size(), 3);

    // Mute menu: "Mute <plugin>'s notices" drops that plugin's notices and persists; "Only important ones" keeps important ones.
    QVERIFY(pc()->toastMuteMode(QStringLiteral("net.test.b")).isEmpty());
    pc()->setPreviewUi({{"toasts", QVariantList{toastEntry(21, "net.test.b", "Item Helper", "warning", "Bag data not found. Open the bag once in game."),
                                                 toastEntry(22, "net.test.c", "Party Editor", "important", "Edits are on hold.")}}});
    QTRY_VERIFY(h_.item("toast_21") != nullptr && h_.item("toast_21")->isVisible());
    QVERIFY(clickIn("toast_21", "toastMore"));
    QTRY_VERIFY(h_.item("pluginMenu")->isVisible());
    QCOMPARE(textOf(h_.item("pluginMenuItem_mute")), QStringLiteral("Mute Item Helper's notices"));
    shot(QStringLiteral("slots-toast-menu"));
    QVERIFY(h_.click("pluginMenuItem_mute"));
    QTRY_COMPARE(pc()->toastMuteMode(QStringLiteral("net.test.b")), QStringLiteral("always"));
    QCOMPARE(freshSettings().pluginToastMutes().value(QStringLiteral("net.test.b")), QStringLiteral("always"));
    QTRY_COMPARE(pc()->toasts().size(), 1);
    QCOMPARE(pc()->toasts().at(0).toMap().value("toastId").toInt(), 22);
    QVERIFY(clickIn("toast_22", "toastMore"));
    QTRY_VERIFY(h_.item("pluginMenu")->isVisible());
    QVERIFY(h_.click("pluginMenuItem_important"));
    QTRY_COMPARE(pc()->toastMuteMode(QStringLiteral("net.test.c")), QStringLiteral("important"));
    QCOMPARE(freshSettings().pluginToastMutes().value(QStringLiteral("net.test.c")), QStringLiteral("important"));
    QCOMPARE(pc()->toasts().size(), 1);  // the important notice stays
    pc()->muteToasts(QStringLiteral("net.test.b"), QStringLiteral("off"));
    pc()->muteToasts(QStringLiteral("net.test.c"), QStringLiteral("off"));
    QVERIFY(freshSettings().pluginToastMutes().isEmpty());

    // Info notices dismiss themselves; warning and important ones stay.
    layer()->setProperty("infoTimeoutMs", 300);
    pc()->setPreviewUi({{"toasts", QVariantList{toastEntry(11, "net.test.a", "SoulLink", "info", "Auto"), toastEntry(12, "net.test.a", "SoulLink", "warning", "Stays"),
                                                 toastEntry(13, "net.test.a", "SoulLink", "important", "Stays too")}}});
    QTRY_COMPARE_WITH_TIMEOUT(pc()->toasts().size(), 2, 4000);
    QCOMPARE(pc()->toasts().at(0).toMap().value("toastId").toInt(), 12);
    layer()->setProperty("infoTimeoutMs", 8000);

    // Host notice "Uzume · plugin paused": Restart plugin reloads, Details opens the console tab.
    pc()->setPreviewConsole(json(R"({"plugins":[{"id":"net.test.a","name":"A","status":"error","statusText":"Paused after an error"}],"selected":"net.test.a",
                                     "status":"error","statusText":"Paused after an error","error":"boom","watched":[],"events":[],"log":[],"regions":[],"regionsText":""})"));
    const QVariantList hostActs{action("Restart plugin", "restart", "net.test.a"), action("Details", "details", "net.test.a")};
    pc()->setPreviewUi({{"toasts", QVariantList{toastEntry(-1, "net.test.a", "Uzume", "warning", "SoulLink was paused after an error. Your game keeps running.", hostActs, true)}}});
    QTRY_VERIFY(h_.item("toast_-1") != nullptr && h_.item("toast_-1")->isVisible());
    QVERIFY(!h_.item("toastMore")->isVisible());  // host notices are not mutable
    QSignalSpy reload(pc(), &PluginsController::reloadRequested);
    QVERIFY(h_.click("toastAction_details"));
    QTRY_VERIFY(h_.item("gamePanelConsole")->isVisible());
    QCOMPARE(pc()->toasts().size(), 0);
    pc()->setPreviewUi({{"toasts", QVariantList{toastEntry(-2, "net.test.a", "Uzume", "warning", "SoulLink was paused after an error.", hostActs, true)}}});
    QTRY_VERIFY(h_.item("toast_-2") != nullptr && h_.item("toast_-2")->isVisible());
    QVERIFY(h_.click("toastAction_restart"));
    QCOMPARE(reload.size(), 1);
    pc()->setPreviewConsole({});
    pc()->setPreviewUi({});
  }

  void tileStripsOnRemoteTiles() {
    SessionController* ctl = h_.controller->sessions();
    QSignalSpy tilesSet(pc(), &PluginsController::tilesSet);
    ctl->watch(QStringLiteral("s1"));
    QTRY_COMPARE_WITH_TIMEOUT(ctl->surfaceCount(), 2, 8000);
    ctl->watch(QStringLiteral("s2"));
    QTRY_COMPARE_WITH_TIMEOUT(ctl->surfaceCount(), 3, 8000);
    ctl->watch(QStringLiteral("s3"));
    QTRY_COMPARE_WITH_TIMEOUT(ctl->surfaceCount(), 4, 8000);
    const QList<QPair<QString, QColor>> feeds{{"s1", QColor(200, 110, 60)}, {"s2", QColor(120, 90, 190)}, {"s3", QColor(70, 160, 110)}};
    for (const auto& f : feeds) QMetaObject::invokeMethod(ctl->viewer(f.first), "frameReady", Qt::DirectConnection, Q_ARG(QImage, fakeFrame(1, f.second)));
    ctl->setTab(QStringLiteral("multiview"));
    ctl->setMultiviewMode(QStringLiteral("grid"));
    QQuickTest::qWaitForPolish(h_.window);

    // The visible remote tiles go to the plugins (never the own tile).
    QTRY_VERIFY(!tilesSet.isEmpty());
    const QVariantList tiles = tilesSet.last().at(0).toList();
    QCOMPARE(tiles.size(), 3);
    QStringList ids;
    for (const QVariant& t : tiles) ids << t.toMap().value("id").toString();
    ids.sort();
    QCOMPARE(ids, (QStringList{"s1", "s2", "s3"}));
    QCOMPARE(tiles.at(0).toMap().value("player").toString().isEmpty(), false);

    QVariantMap trees;
    QVariantMap strips;
    auto strip = [&](const QString& who, const QString& place, const QString& status, int color) {
      return json(QStringLiteral(R"({"type":"column","gap":8,"children":[
        {"type":"row","gap":8,"children":[{"type":"avatar","initial":"%1","color":%4,"size":"s"},{"type":"text","style":"heading","text":"%2"},{"type":"text","style":"meta","text":"%3"}]},
        {"type":"bar","value":0.7,"label":"Team health"},
        {"type":"button","id":"btnJump","label":"Open run","variant":"secondary","action":"open","value":"%1"}]})").arg(who.left(1), place, status).arg(color).toUtf8().constData());
    };
    auto stripEntry = [&](const QString& pid, const QString& name, const QVariantMap& tree) {
      return QVariantMap{{"pluginId", pid}, {"name", name}, {"icon", QString()}, {"origin", QStringLiteral("local")}, {"tree", tree}};
    };
    // s1 and s2 have a strip (s2 from two plugins), s3 none, the own tile never.
    strips["s1"] = QVariantList{stripEntry("net.test.a", "SoulLink", strip("Lena", "Route 3", "· encounter", 2))};
    strips["s2"] = QVariantList{stripEntry("net.test.a", "SoulLink", strip("Jonas", "Cave of Echoes", "· caught", 3)), stripEntry("net.test.b", "Item Helper", strip("Jonas", "Bag: 12 items", "", 3))};
    strips["local"] = QVariantList{stripEntry("net.test.a", "SoulLink", strip("Max", "Route 1", "", 1))};
    pc()->setPreviewUi({{"tileStrips", strips}});
    QTRY_VERIFY(h_.item("tileStrip_s1") != nullptr && h_.item("tileStrip_s1")->isVisible());
    QVERIFY(h_.item("tileStrip_s2")->isVisible());
    QVERIFY(h_.items("tileStrip_s3").isEmpty() || !h_.item("tileStrip_s3")->isVisible());
    QVERIFY(h_.items("tileStrip_local").isEmpty());  // never on the own tile
    QCOMPARE(h_.item("tileStrip_s1")->height(), 20.0);
    // Top left inside the tile (17 px from its edge: frame + 16), above the info row with the key badge: the two never overlap.
    QQuickItem* badge = h_.item("keyBadge_s1");
    QQuickItem* st = h_.item("tileStrip_s1");
    QQuickItem* tl = h_.item("surfaceTile_s1");
    QVERIFY(tl != nullptr);
    QCOMPARE(qRound(st->mapToScene(QPointF(0, 0)).x() - tl->mapToScene(QPointF(0, 0)).x()), 17);
    QVERIFY(st->mapToScene(QPointF(0, 0)).y() + st->height() <= badge->mapToScene(QPointF(0, 0)).y());
    // Collapsed: the first text row of the tree.
    QCOMPARE(visibleItem("tileStripText") != nullptr, true);
    QStringList texts;
    for (QQuickItem* i : h_.items("tileStripText"))
      if (i->isVisible()) texts << i->property("text").toString();
    texts.sort();
    QCOMPARE(texts, (QStringList{"Cave of Echoes", "Route 3"}));
    // Two plugins on s2: the tile "⋯" menu picks the plugin.
    QVERIFY(h_.items("tileStripMore_s1").isEmpty() || !h_.item("tileStripMore_s1")->isVisible());
    QTest::mouseMove(h_.window, center(h_.item("tileStrip_s2")).toPoint());
    QTRY_VERIFY(h_.item("tileStripMore_s2")->isVisible());
    QVERIFY(h_.click("tileStripMore_s2"));
    QTRY_VERIFY(h_.item("pluginMenu")->isVisible());
    QVERIFY(h_.click("pluginMenuItem_net.test.b"));
    QTRY_COMPARE(visibleCount("tileStripText"), 2);
    texts.clear();
    for (QQuickItem* i : h_.items("tileStripText"))
      if (i->isVisible()) texts << i->property("text").toString();
    texts.sort();
    QCOMPARE(texts, (QStringList{"Bag: 12 items", "Route 3"}));
    QTest::mouseMove(h_.window, QPoint(5, 5));

    // Click expands (max 300 x 120); the action goes to the plugin with the tile slot; one expanded, the others collapsed.
    QVERIFY(clickIn("tileStrip_s1", "tileStripHeader"));
    QTRY_VERIFY(h_.item("tileStrip_s1")->property("expanded").toBool());
    QTRY_VERIFY(Harness::findVisual(h_.item("tileStrip_s1"), QStringLiteral("tileStripBlock"))->isVisible());
    QVERIFY(!h_.item("tileStrip_s2")->property("expanded").toBool());
    QVERIFY(h_.item("tileStrip_s1")->width() <= 300 && h_.item("tileStrip_s1")->height() <= 120 && h_.item("tileStrip_s1")->height() > 20);
    QCOMPARE(h_.item("tileStrip_s2")->height(), 20.0);
    shot(QStringLiteral("slots-multiview-strips"));
    QSignalSpy sent(pc(), &PluginsController::actionSent);
    QVERIFY(h_.click("plugin_btnJump"));
    QTRY_COMPARE(sent.size(), 1);
    QVERIFY(sent.at(0).at(1).toString().startsWith(QStringLiteral("tile:s")));
    QCOMPARE(sent.at(0).at(2).toString(), QStringLiteral("open"));
    pc()->setPreviewUi({});
    QTRY_VERIFY(h_.items("tileStrip_s1").isEmpty() || !h_.item("tileStrip_s1")->isVisible());
  }

  // ---------------------------------------------------------------- fb.ui.open: in-game screen sheet and panel tab

  void screenSheetOpensAndCloses() {
    h_.controller->sessions()->leaveWatch();
    h_.controller->sessions()->setTab(QStringLiteral("session"));
    QQuickItem* screen = gameScreen();
    QVERIFY(screen != nullptr);
    QTRY_VERIFY(screen->isVisible());
    QVariantMap trees{{"net.test.a|screen", json(R"({"type":"column","gap":8,"children":[
        {"type":"text","style":"title","text":"Run screen"},
        {"type":"button","id":"btnRun","label":"Go","variant":"primary","action":"go","value":3}]})")},
                      {"net.test.a|panel", json(R"({"type":"text","text":"panel"})")}};
    const QVariantMap entry{{"pluginId", "net.test.a"}, {"name", "SoulLink"}, {"title", "Run"}, {"icon", QString()}, {"badge", 0}, {"origin", "local"}};
    pc()->setPreviewUi({{"panelTabs", QVariantList{tabEntry("net.test.a", "SoulLink")}}, {"sidebarEntries", QVariantList{entry}}, {"trees", trees}});
    QTRY_VERIFY(h_.item("panelTab_plugin:net.test.a") != nullptr);
    QVERIFY(h_.item("pluginScreenSheet") != nullptr);
    QVERIFY(!h_.item("pluginScreenSheet")->isVisible());

    // fb.ui.open("panel") selects the plugin's tab.
    emit pc()->openRequested(QStringLiteral("net.test.a"), QStringLiteral("panel"));
    QTRY_VERIFY(h_.item("panelTab_plugin:net.test.a")->property("open").toBool());
    QVERIFY(!h_.item("pluginScreenSheet")->isVisible());

    // fb.ui.open("screen") shows the sheet with the screen tree, covering the play area and the panel.
    GameSession* gs = h_.controller->gameSession();
    const bool pausedBefore = gs->isPaused();
    emit pc()->openRequested(QStringLiteral("net.test.a"), QStringLiteral("screen"));
    QTRY_VERIFY(h_.item("pluginScreenSheet")->isVisible());
    QTRY_VERIFY(h_.item("pluginScreenSheetBlock") != nullptr && h_.item("plugin_btnRun") != nullptr && h_.item("plugin_btnRun")->isVisible());
    QVERIFY(h_.item("pluginScreenSheet")->width() >= screen->width() - 1);
    QCOMPARE(h_.item("pluginScreenSheetTitle")->property("text").toString(), QStringLiteral("Run  ·  SoulLink"));
    shot(QStringLiteral("slots-in-game-screen-sheet"));

    // Actions in the sheet reach the plugin with slot "screen".
    QSignalSpy sent(pc(), &PluginsController::actionSent);
    QVERIFY(h_.click("plugin_btnRun"));
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(sent.at(0).at(1).toString(), QStringLiteral("screen"));
    QCOMPARE(sent.at(0).at(2).toString(), QStringLiteral("go"));

    // Esc closes the sheet and does not pause the game.
    QTest::keyClick(h_.window, Qt::Key_Escape);
    QTRY_VERIFY(!h_.item("pluginScreenSheet")->isVisible());
    QCOMPARE(gs->isPaused(), pausedBefore);

    // "← Back to game" closes it too.
    emit pc()->openRequested(QStringLiteral("net.test.a"), QStringLiteral("screen"));
    QTRY_VERIFY(h_.item("pluginScreenSheet")->isVisible());
    QVERIFY(h_.click("pluginScreenBack"));
    QTRY_VERIFY(!h_.item("pluginScreenSheet")->isVisible());
    pc()->setPreviewUi({});
    QTRY_VERIFY(!h_.item("pluginScreenSheet")->isVisible());
  }

  // ---------------------------------------------------------------- plugin hotkeys (Controllers > Hotkeys, keys in game)

  QVariantMap hotkeyUi() {
    return {{"panelTabs", QVariantList{tabEntry("net.test.a", "SoulLink", "", false, "official"), tabEntry("net.test.b", "Item Helper", "", false, "unverified")}},
            {"hudWidgets", QVariantList{hudEntry("net.test.a", "SoulLink", "bottom-left", "Team")}},
            {"hotkeyPlugins",
             QVariantList{hotkeyPlugin("net.test.a", "SoulLink & Nuzlocke", "official",
                                       {hotkeyDecl("toggle-hud", "Show or hide HUD widget", "Ctrl+H"), hotkeyDecl("team", "Open team", "Ctrl+D"),
                                        hotkeyDecl("reload", "Reload run", "Ctrl+R")}),
                          hotkeyPlugin("net.test.b", "Item Helper", "unverified",
                                       {hotkeyDecl("peek", "Peek bag", "Ctrl+H"), hotkeyDecl("fs", "Quick save", "F11"), hotkeyDecl("bag", "Open bag", "F8")})}}};
  }

  void hotkeysInGame() {
    h_.controller->sessions()->leaveWatch();  // back to the own tile (the Multiview test left remote tiles open)
    h_.controller->sessions()->setTab(QStringLiteral("session"));
    QTRY_VERIFY(!h_.controller->sessions()->watching());
    QQuickItem* screen = gameScreen();
    QVERIFY(screen != nullptr);
    QTRY_VERIFY(screen->isVisible());
    QTRY_VERIFY(layer() != nullptr && layer()->isVisible());
    screen->forceActiveFocus();
    pc()->setHudAllHidden(false);
    pc()->setPreviewUi(hotkeyUi());
    // Item Helper's Ctrl+H is in conflict (not active); the user picks Ctrl+K (persisted, see the Controllers test).
    QVERIFY(!pc()->activeHotkeyCombos().contains(int(Qt::Key_K) | int(Qt::ControlModifier)));
    pc()->beginHotkeyCapture(QStringLiteral("net.test.b"), QStringLiteral("peek"));
    QVERIFY(pc()->captureHotkey(Qt::Key_K, Qt::ControlModifier));
    QCOMPARE(freshSettings().pluginHotkeyOverrides().value("net.test.b/peek"), QStringLiteral("Ctrl+K"));
    QSignalSpy sent(pc(), &PluginsController::actionSent);

    // Active plugin key -> on_action(id, nil, "hotkey").
    QTest::keyClick(h_.window, Qt::Key_H, Qt::ControlModifier);
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(sent.at(0).at(0).toString(), QStringLiteral("net.test.a"));
    QCOMPARE(sent.at(0).at(1).toString(), QStringLiteral("hotkey"));
    QCOMPARE(sent.at(0).at(2).toString(), QStringLiteral("toggle-hud"));
    QVERIFY(!sent.at(0).at(3).isValid());
    QTest::keyClick(h_.window, Qt::Key_K, Qt::ControlModifier);  // the user's own key for Item Helper
    QTRY_COMPARE(sent.size(), 2);
    QCOMPARE(sent.at(1).at(0).toString(), QStringLiteral("net.test.b"));
    QCOMPARE(sent.at(1).at(2).toString(), QStringLiteral("peek"));
    QTest::keyClick(h_.window, Qt::Key_F8);
    QTRY_COMPARE(sent.size(), 3);
    QCOMPARE(sent.at(2).at(2).toString(), QStringLiteral("bag"));
    QTest::keyClick(h_.window, Qt::Key_H);  // without Ctrl: not a hotkey
    QTest::keyClick(h_.window, Qt::Key_F, Qt::ControlModifier);
    QTest::qWait(50);
    QCOMPARE(sent.size(), 3);

    // The core never sees an active plugin key (GameSession keeps the combination), a plain H still goes through.
    GameSession* gs = h_.controller->gameSession();
    gs->setKeyboardMap({{Qt::Key_H, 1u << 8}});
    QVERIFY(!gs->keyEvent(Qt::Key_H, true, Qt::ControlModifier));
    QVERIFY(gs->keyEvent(Qt::Key_H, true, Qt::NoModifier));
    gs->keyEvent(Qt::Key_H, false, Qt::NoModifier);
    gs->setKeyboardMap(h_.controller->controllers()->keyboardMap());

    // Alt+n selects panel tab n (Game, SoulLink, Item Helper); a missing tab changes nothing.
    QTest::keyClick(h_.window, Qt::Key_2, Qt::AltModifier);
    QTRY_VERIFY(h_.item("gamePanelPlugin")->isVisible());
    QTRY_VERIFY(h_.item("panelTab_plugin:net.test.a")->property("open").toBool());
    QTest::keyClick(h_.window, Qt::Key_3, Qt::AltModifier);
    QTRY_VERIFY(h_.item("panelTab_plugin:net.test.b")->property("open").toBool());
    QTest::keyClick(h_.window, Qt::Key_9, Qt::AltModifier);
    QTest::qWait(50);
    QVERIFY(h_.item("panelTab_plugin:net.test.b")->property("open").toBool());
    QTest::keyClick(h_.window, Qt::Key_1, Qt::AltModifier);
    QTRY_VERIFY(!h_.item("gamePanelPlugin")->isVisible());
    QCOMPARE(sent.size(), 3);  // none of these reached a plugin

    // F4 toggles "Hide all HUD widgets".
    QVERIFY(!pc()->hudAllHidden());
    QTest::keyClick(h_.window, Qt::Key_F4);
    QTRY_VERIFY(pc()->hudAllHidden());
    QVERIFY(freshSettings().pluginHudAllHidden());
    QTest::keyClick(h_.window, Qt::Key_F4);
    QTRY_VERIFY(!pc()->hudAllHidden());
    pc()->resetHotkey(QStringLiteral("net.test.b"), QStringLiteral("peek"));
    QVERIFY(freshSettings().pluginHotkeyOverrides().isEmpty());
    pc()->setPreviewUi({});
  }

  void hiddenHudWidgetsCanBeShownAgain() {
    pc()->setHudAllHidden(false);
    pc()->setPreviewUi(hotkeyUi());
    QTRY_VERIFY(h_.item("hudWidget_net.test.a") != nullptr && h_.item("hudWidget_net.test.a")->isVisible());
    QVERIFY(!h_.item("hudRestoreButton")->isVisible());

    // One widget hidden: the pill offers "Show <plugin> widget".
    pc()->hideHud(QStringLiteral("net.test.a"));
    QTRY_VERIFY(h_.item("hudRestoreButton")->isVisible());
    QCOMPARE(textOf(h_.item("hudRestoreButton")), QStringLiteral("1 widget hidden · Show"));
    rightClick(h_.item("hudRestoreButton"));   // the menu with the per-plugin entries is on the right click
    QTRY_VERIFY(h_.item("pluginMenu")->isVisible());
    QCOMPARE(textOf(h_.item("pluginMenuItem_show:net.test.a")), QStringLiteral("Show SoulLink widget"));
    QVERIFY(h_.item("pluginMenuItem_hideAll") != nullptr);
    QCOMPARE(pc()->activeHotkeyKey(QStringLiteral("net.test.a"), QStringLiteral("toggle-hud")), QStringLiteral("Ctrl+H"));
    QCOMPARE(pc()->activeHotkeyKey(QStringLiteral("net.test.b"), QStringLiteral("peek")), QString());
    QVERIFY(h_.click("pluginMenuItem_show:net.test.a"));
    QTRY_VERIFY(!pc()->hudWidgets().first().toMap().value("hidden").toBool());
    QTRY_VERIFY(!h_.item("hudRestoreButton")->isVisible());
    QTRY_VERIFY(h_.item("hudWidget_net.test.a")->isVisible());

    // Everything hidden (F4 or "Hide all"): the pill stays and offers "Show all HUD widgets".
    pc()->setHudAllHidden(true);
    QTRY_VERIFY(h_.item("hudRestoreButton")->isVisible());
    QCOMPARE(textOf(h_.item("hudRestoreButton")), QStringLiteral("HUD hidden · Show"));
    rightClick(h_.item("hudRestoreButton"));
    QTRY_VERIFY(h_.item("pluginMenu")->isVisible());
    QVERIFY(h_.click("pluginMenuItem_showAll"));
    QTRY_VERIFY(!pc()->hudAllHidden());
    QTRY_VERIFY(!h_.item("hudRestoreButton")->isVisible());

    // Left click on the pill shows the hidden widgets and the HUD again directly (no menu).
    pc()->hideHud(QStringLiteral("net.test.a"));
    pc()->setHudAllHidden(true);
    QTRY_VERIFY(h_.item("hudRestoreButton")->isVisible());
    QVERIFY(h_.click("hudRestoreButton"));
    QTRY_VERIFY(!pc()->hudAllHidden());
    QTRY_VERIFY(!pc()->hudWidgets().first().toMap().value("hidden").toBool());
    QTRY_VERIFY(!h_.item("hudRestoreButton")->isVisible());
    QVERIFY(!h_.item("pluginMenu")->isVisible());

    pc()->setPreviewUi({});
  }

  // "Notice settings…" opens Settings > Plugins; the game goes to the background (as for "Hotkeys…").
  void noticeSettingsOpensPlugins() {
    h_.controller->sessions()->setTab(QStringLiteral("session"));
    pc()->setPreviewUi({{"toasts", QVariantList{toastEntry(7, "net.test.a", "SoulLink", "warning", "Something to look at.")}}});
    QTRY_VERIFY(h_.item("toast_7") != nullptr && h_.item("toast_7")->isVisible());
    QVERIFY(clickIn("toast_7", "toastMore"));
    QTRY_VERIFY(h_.item("pluginMenu")->isVisible());
    QSignalSpy requested(h_.controller.get(), &PlayerController::pluginSettingsRequested);
    QVERIFY(h_.click("pluginMenuItem_settings"));
    QTRY_COMPARE(requested.size(), 1);
    QCOMPARE(requested.at(0).at(0).toString(), QString());
    pc()->setPreviewUi({});
  }
};

UITEST_MAIN(PluginSlotsUiTest)
#include "plugin_slots_ui_test.moc"
