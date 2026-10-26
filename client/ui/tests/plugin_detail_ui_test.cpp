// Plugin "game detail" slot in the Library detail column (0.11 package 2, design 4h-2): a real homebrew Lua plugin in the shell
// context answers the host event fb/detail.game with fb.ui.set("detail", ...). Covers the section per selected game, the update on
// another selection, an action round trip, the "Plugins" status row and the empty case. With UZUME_SCREENSHOT_DIR=<dir> the
// library is also saved as PNG (dark and light; never checked in).
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;

namespace {

const char* kDetailLua = R"LUA(
  local n = 0
  local cur = nil
    local function render()
    if not cur then fb.ui.clear('detail') return end
    fb.ui.set('detail', {type='column', gap=8, children={
      {type='text', style='title', id='gameTitle', text='Notes for ' .. cur.title},
      {type='row', gap=8, children={{type='badge', text='Tracked', tone='ok'}, {type='text', style='meta', text=cur.system}}},
      {type='button', id='btnPing', label='Ping ' .. n, variant='primary', action='ping', value=n}}})
  end
  fb.events.on('fb', 'detail.game', function(g) cur = g render() end)
  fb.ui.on_action(function(a, v, slot) if a == 'ping' then n = n + 1 render() end end)
)LUA";

bool writePlugin(const QString& parent, const QString& id, const QString& name, const QJsonObject& ui, const QString& lua) {
  const QString dir = QDir(parent).filePath(id);
  QDir().mkpath(dir);
  QJsonObject m{{"id", id}, {"name", name}, {"version", "1.0.0"}, {"api", 1}, {"type", "script"}, {"entry", "main.lua"},
                {"permissions", QJsonArray{"ui"}}, {"ui", ui}};
  QFile mf(dir + "/manifest.json");
  QFile lf(dir + "/main.lua");
  if (!mf.open(QIODevice::WriteOnly) || !lf.open(QIODevice::WriteOnly)) return false;
  mf.write(QJsonDocument(m).toJson());
  lf.write(lua.toUtf8());
  return true;
}

}  // namespace

class PluginDetailUiTest : public QObject {
  Q_OBJECT

  QString shotDir_;
  QTemporaryDir src_;
  FakeHub hub_{QStringLiteral("a")};
  Harness h_;

  PluginsController* pc() { return h_.controller->plugins(); }
  QQuickItem* visibleItem(const char* name) {
    for (QQuickItem* i : h_.items(name))
      if (i->isVisible()) return i;
    return nullptr;
  }
  static QString textOf(QQuickItem* it) {
    if (it->property("text").isValid() && !it->property("text").toString().isEmpty()) return it->property("text").toString();
    for (QQuickItem* c : it->childItems())
      if (const QString t = textOf(c); !t.isEmpty()) return t;
    return {};
  }
  QString sectionTitle() {
    QQuickItem* sec = visibleItem("pluginDetail_net.test.notes");
    QQuickItem* t = sec == nullptr ? nullptr : Harness::findVisual(sec, QStringLiteral("plugin_gameTitle"));
    return t == nullptr ? QString() : textOf(t);
  }
  void shot(const QString& name) {
    if (shotDir_.isEmpty()) return;
    QQuickTest::qWaitForPolish(h_.window);
    QTest::qWait(400);
    QDir().mkpath(shotDir_);
    const QImage img = h_.window->grabWindow();
    QVERIFY2(!img.isNull() && !uitest::isAllBlack(img), qPrintable(name));
    QVERIFY(img.save(QDir(shotDir_).filePath(name + QStringLiteral(".png"))));
  }

 private slots:
  void initTestCase() {
    shotDir_ = qEnvironmentVariable("UZUME_SCREENSHOT_DIR");
    uitest::installWarningCounter();
    hub_.hubId = QStringLiteral("hub-detail");
    hub_.name = QStringLiteral("Home");
    hub_.decision = FakeHub::Decision::Approve;
    hub_.games = QJsonObject{{QStringLiteral("games"),
                              QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"), uitest::sha256Hex("a"), 1),
                                         uitest::gameJson(QStringLiteral("g2"), QStringLiteral("Night Run"), uitest::sha256Hex("b"), 1)}}};
    QVERIFY(hub_.start());
    QVERIFY(h_.start());
    h_.controller->addHub(hub_.address());
    QTRY_COMPARE_WITH_TIMEOUT(h_.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation, 8000);
    h_.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h_.controller->connection()->state(), HubConnection::State::NeedsPairing, 8000);
    h_.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h_.controller->screen(), QStringLiteral("library"), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(h_.controller->libraryState(), QStringLiteral("ready"), 8000);
    h_.window->resize(1440, 900);
    QTest::qWait(150);
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void noPluginNoSection() {
    h_.controller->selectGame(QStringLiteral("g1"));
    QTRY_VERIFY(visibleItem("detailTitle") != nullptr);
    QCOMPARE(h_.items("pluginDetail_net.test.notes").size(), 0);
    QVERIFY(visibleItem("detailPluginsRow") == nullptr);
  }

  void sectionFollowsSelection() {
    QVERIFY(writePlugin(src_.path(), "net.test.notes", "Notes Plugin", QJsonObject{{"detail", true}}, kDetailLua));
    const auto r = pc()->manager()->installFromFolder(src_.filePath(QStringLiteral("net.test.notes")));
    QVERIFY2(r.ok, qPrintable(r.error));
    pc()->setPluginEnabled(QStringLiteral("net.test.notes"), true);
    pc()->setDeveloperMode(true);

    QTRY_COMPARE_WITH_TIMEOUT(sectionTitle(), QStringLiteral("Notes for Lumen Drift"), 10000);
    QCOMPARE(textOf(visibleItem("pluginDetailName")), QStringLiteral("Drawn by Notes Plugin"));
    QVERIFY(visibleItem("pluginDetailOrigin") != nullptr);  // installed from a folder: not official
    QVERIFY(h_.click("tab_Details"));  // the plugins row is one of the facts of the Details tab
    QTRY_VERIFY(visibleItem("detailPluginsRow") != nullptr);
    QQuickItem* row = visibleItem("detailPluginsRow");
    QVERIFY(row != nullptr);
    QQuickItem* val = Harness::findVisual(row, QStringLiteral("detailRowValue"));
    QVERIFY(val != nullptr);
    QCOMPARE(val->property("text").toString(), QStringLiteral("1 on · Notes Plugin"));
    QVERIFY(h_.click("tab_Overview"));
    shot(QStringLiteral("detail-plugin-section-dark"));
    h_.controller->setAppearance(QStringLiteral("light"));
    shot(QStringLiteral("detail-plugin-section-light"));
    h_.controller->setAppearance(QStringLiteral("dark"));

    h_.controller->selectGame(QStringLiteral("g2"));
    QTRY_COMPARE_WITH_TIMEOUT(sectionTitle(), QStringLiteral("Notes for Night Run"), 10000);
  }

  void actionRoundTrip() {
    QTRY_VERIFY(h_.item("plugin_btnPing") != nullptr && h_.item("plugin_btnPing")->isVisible());
    QVERIFY(h_.click("plugin_btnPing"));
    QTRY_COMPARE_WITH_TIMEOUT(pc()->detailSections().value(0).toMap().value("tree").toMap().value("children").toList().at(2).toMap().value("label").toString(),
                              QStringLiteral("Ping 1"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(textOf(h_.item("plugin_btnPing")), QStringLiteral("Ping 1"), 5000);
  }

  void disabledPluginHidesSection() {
    pc()->setPluginEnabled(QStringLiteral("net.test.notes"), false);
    QTRY_VERIFY_WITH_TIMEOUT(visibleItem("pluginDetail_net.test.notes") == nullptr, 10000);
    QTRY_VERIFY(visibleItem("detailPluginsRow") == nullptr);
  }
};

QTEST_MAIN(PluginDetailUiTest)
#include "plugin_detail_ui_test.moc"
