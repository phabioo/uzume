// Plugin UI v1 runtime: manifest field `ui`, tree validation/conversion, fb.ui.* through the plugin host, action round
// trip, host events, shell context. Homebrew plugins written to a temporary directory only.
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "lauxlib.h"
#include "lua.h"
#include "lua_sandbox.h"
#include "plugin_host.h"
#include "plugin_test_util.h"
#include "plugin_ui.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

namespace {

// Evaluates a Lua expression into a plain Value (same reader limits as fb.ui.set).
Value luaValue(const QString& expr) {
  LuaSandbox sb;
  Value v;
  std::string err;
  const std::string code = "return " + expr.toStdString();
  const bool ok = sb.run(
      [&](lua_State* L) {
        if (luaL_loadstring(L, code.c_str()) != LUA_OK) luaL_error(L, "%s", lua_tostring(L, -1));
        lua_call(L, 0, 1);
        std::string e;
        if (!readValue(L, -1, v, e, kUiReadDepth, kUiMaxBytes)) luaL_error(L, "%s", e.c_str());
      },
      err);
  if (!ok) qWarning("luaValue failed: %s", err.c_str());
  return v;
}

struct Conv {
  bool ok = false;
  QVariantMap tree;
  QString error;
};
Conv conv(const QString& expr) {
  Conv c;
  c.ok = convertUiTree(luaValue(expr), c.tree, c.error);
  return c;
}

QByteArray manifestJson(const QJsonValue& ui, const QStringList& perms = {"ui"}) {
  QJsonObject m{{"id", "net.test.ui"}, {"name", "UI test"}, {"version", "1.0.0"}, {"api", 1}, {"entry", "main.lua"},
                {"permissions", QJsonArray::fromStringList(perms)}};
  if (!ui.isUndefined()) m["ui"] = ui;
  return QJsonDocument(m).toJson();
}

QJsonObject uiAll() {
  return QJsonObject{{"panel", QJsonObject{{"title", "SoulLink"}}}, {"hud", true}, {"tile", true},
                     {"screen", QJsonObject{{"title", "Runs"}}}, {"detail", true}, {"hotkeys", QJsonArray{QJsonObject{{"id", "toggle-hud"}, {"label", "Show or hide HUD widget"}, {"key", "Ctrl+H"}}}}};
}

bool hasLog(const ConsoleSnapshot& c, const QString& needle) {
  for (const LogLine& l : c.log)
    if (l.text.contains(needle)) return true;
  return false;
}
QString logJoined(const ConsoleSnapshot& c) {
  QStringList out;
  for (const LogLine& l : c.log) out << l.text;
  return out.join(QLatin1Char('\n'));
}

QString nestedColumns(int n) {
  QString s = QStringLiteral("{type='text', text='leaf'}");
  for (int i = 0; i < n; ++i) s = QStringLiteral("{type='column', children={%1}}").arg(s);
  return s;
}

const QString kId = QStringLiteral("net.test.plugin");

}  // namespace

class UiTest : public QObject {
  Q_OBJECT

  QTemporaryDir m_dir;
  PluginHost* m_host = nullptr;

  QString cache() const { return m_dir.path() + "/cache"; }
  void startSpec(const PluginSpec& s, const GameContext& ctx = GameContext{"nds", "ABCD", "abc", "Game", "", "en", false}) {
    const PluginInfo info = infoFor(writePlugin(m_dir.path() + "/src", s), cache());
    m_host->start(ctx, std::make_shared<FakeMemoryProvider>(), {info});
    QTRY_COMPARE_WITH_TIMEOUT(m_host->consoleSnapshot(info.manifest.id).status, PluginStatus::Running, 10000);
  }
  static PluginSpec spec(const QString& lua, const QStringList& perms = {"ui"}) {
    PluginSpec s;
    s.permissions = perms;
    s.ui = uiAll();
    s.lua = lua;
    return s;
  }
  ConsoleSnapshot con() const { return m_host->consoleSnapshot(kId); }

 private slots:
  void init() { m_host = new PluginHost; }
  void cleanup() {
    delete m_host;
    m_host = nullptr;
    QDir(m_dir.path() + "/src").removeRecursively();
  }

  // ---------------------------------------------------------------- manifest

  void manifestValid() {
    const ManifestResult r = parseManifest(manifestJson(uiAll()));
    QVERIFY2(r.ok, qPrintable(r.error));
    const UiDecl& u = r.manifest.ui;
    QVERIFY(u.panel && u.hud && u.tile && u.screen && u.detail);
    QCOMPARE(u.panelTitle, QStringLiteral("SoulLink"));
    QCOMPARE(u.screenTitle, QStringLiteral("Runs"));
    QCOMPARE(u.hotkeys.size(), 1);
    QCOMPARE(u.hotkeys[0].id, QStringLiteral("toggle-hud"));
    QCOMPARE(u.hotkeys[0].key, QStringLiteral("Ctrl+H"));
    QCOMPARE(u.addsText(), QStringLiteral("Adds: panel tab · HUD · tile strip · game detail · run tab · 1 hotkey"));
    QVERIFY(u.declaresSlot("panel") && u.declaresSlot("tile:t1") && !u.declaresSlot("tile:bad id") && !u.declaresSlot("other"));
    // ui.run: the plugin's content in the run detail view
    const ManifestResult rv = parseManifest(manifestJson(QJsonObject{{"run", QJsonObject{{"title", "Run board"}}}, {"detail", true}}));
    QVERIFY2(rv.ok, qPrintable(rv.error));
    QVERIFY(rv.manifest.ui.run && rv.manifest.ui.any() && rv.manifest.ui.declaresSlot("run"));
    QCOMPARE(rv.manifest.ui.runTitle, QStringLiteral("Run board"));
    QCOMPARE(rv.manifest.ui.addsText(), QStringLiteral("Adds: game detail · run tab"));
    QVERIFY(!u.declaresSlot("run"));
    const ManifestResult none = parseManifest(manifestJson(QJsonValue(QJsonValue::Undefined)));
    QVERIFY(none.ok);
    QVERIFY(!none.manifest.ui.any());
    const ManifestResult hk = parseManifest(manifestJson(QJsonObject{{"hotkeys", QJsonArray{QJsonObject{{"id", "a"}, {"label", "A"}}}}}));
    QVERIFY(hk.ok);
    QVERIFY(hk.manifest.ui.hotkeys[0].key.isEmpty());
    QVERIFY(!hk.manifest.ui.declaresSlot("panel"));
  }

  void manifestInvalid() {
    auto err = [](const QJsonValue& ui, const QStringList& perms = {"ui"}) { return parseManifest(manifestJson(ui, perms)).error; };
    QCOMPARE(err(uiAll(), {}), QStringLiteral("field `ui` needs permission `ui`"));
    QVERIFY(err(QJsonObject{{"bogus", true}}).contains("unknown key 'bogus'"));
    QVERIFY(err(QJsonObject{{"panel", QJsonObject{{"title", "x"}, {"icon", "y"}}}}).contains("ui.panel: unknown key 'icon'"));
    QVERIFY(err(QJsonObject{{"panel", true}}).contains("ui.panel must be an object"));
    QVERIFY(err(QJsonObject{{"panel", QJsonObject{}}}).contains("ui.panel.title is required"));
    QVERIFY(err(QJsonObject{{"screen", QJsonObject{{"title", QString(41, 'x')}}}}).contains("ui.screen.title must be 1 to 40 characters"));
    QVERIFY(err(QJsonObject{{"run", true}}).contains("ui.run must be an object"));
    QVERIFY(err(QJsonObject{{"run", QJsonObject{}}}).contains("ui.run.title is required"));
    QVERIFY(err(QJsonObject{{"run", QJsonObject{{"title", QString(41, 'x')}}}}).contains("ui.run.title must be 1 to 40 characters"));
    QVERIFY(err(QJsonObject{{"run", QJsonObject{{"title", "x"}, {"icon", "y"}}}}).contains("ui.run: unknown key 'icon'"));
    QVERIFY(err(QJsonObject{{"hud", "yes"}}).contains("ui.hud must be true or false"));
    QVERIFY(err(QJsonValue(5)).contains("'ui' must be an object"));
    auto hot = [](const QJsonObject& h) { return QJsonObject{{"hotkeys", QJsonArray{h}}}; };
    QVERIFY(err(hot({{"id", "Bad_Id"}, {"label", "x"}})).contains("ui.hotkeys[0].id"));
    QVERIFY(err(hot({{"id", "a"}, {"label", ""}})).contains("ui.hotkeys[0].label"));
    QVERIFY(err(hot({{"id", "a"}, {"label", "x"}, {"key", "Ctrl+A, Ctrl+B"}})).contains("ui.hotkeys[0].key"));
    QVERIFY(err(hot({{"id", "a"}, {"label", "x"}, {"extra", 1}})).contains("unknown key 'extra'"));
    QJsonArray dup{QJsonObject{{"id", "a"}, {"label", "x"}}, QJsonObject{{"id", "a"}, {"label", "y"}}};
    QVERIFY(err(QJsonObject{{"hotkeys", dup}}).contains("not unique"));
    QJsonArray many;
    for (int i = 0; i < 17; ++i) many.append(QJsonObject{{"id", QStringLiteral("h%1").arg(i)}, {"label", "x"}});
    QVERIFY(err(QJsonObject{{"hotkeys", many}}).contains("at most 16"));
  }

  // ---------------------------------------------------------------- tree validation

  void everyBlockType() {
    const Conv c = conv(R"LUA({type='column', id='root', gap=13, align='stretch', children={
      {type='row', children={ {type='text', text='Hi', style='title', tone='info', wrap=false},
                              {type='chip', text='All', tone='ok', action='f', selected=true},
                              {type='badge', text='2', tone='danger'},
                              {type='avatar', initial='A', color=3, size='l'} }},
      {type='grid', columns=3, gap=8, children={ {type='stat', label='Deaths', value=3, action='open'} }},
      {type='tabs', tabs={'One','Two'}, selected=2, action='tab', children={ {type='text', text='p1'}, {type='text', text='p2'} }},
      {type='scroll', max_height='m', children={ {type='card', title='T', eyebrow='E', link_label='More', action='go', value={n=1}, children={
          {type='section', title='S', link_label='all', action='all', children={
            {type='image', src='cache:icons/25.png', size='m', state='dead', owner=2, label='Pikachu'},
            {type='bar', value=0.4, tone='warning', label='HP'} }} }} }},
      {type='list', selected='r1', rows={ {id='r1', title='Route 1', sub='x', meta='y', image='asset:icons/a.png', tone='ok', action='row', value=1}, {title='Route 2'} }},
      {type='table', columns={'A','B'}, rows={ {cells={'a', 2}, action='r', value='x'} }},
      {type='button', label='Reset', variant='danger', action='reset', value=true, confirm={title='Sure?', text='All gone', confirm_label='Reset', danger=true}},
      {type='toggle', label='HUD', checked=true, action='hud'},
      {type='select', options={{value='a', label='A'},{value='b', label='B'}}, selected='a', action='sel'},
      {type='segmented', options={{value=1, label='One'},{value=2, label='Two'}}, selected=1, action='seg'},
      {type='textfield', text='abc', placeholder='Name', error='Too short', action='name'},
      {type='empty', title='Nothing', text='x', action_label='Retry', action='retry', tone='danger'},
      {type='location', title='Route 3', sub='Area', status='encounter', children={ {type='text', text='in'} }},
      {type='map', image='cache:map.png', image_w=1024, image_h=768, filter='me', me='p1', selected='r1', legend=true, action='map',
       players={ {id='p1', initial='M', color=1, x=10, y=20, label='Me'} },
       routes={ {id='r1', x=1.5, y=2, area='North', number=1, done=false, icons={ {src='cache:icons/1.png', owner=1} }} },
       popover={ {owner=1, name='Bulbasaur', src='cache:icons/1.png', status='alive'} }}
    }})LUA");
    QVERIFY2(c.ok, qPrintable(c.error));
    QCOMPARE(c.tree.value("type").toString(), QStringLiteral("column"));
    QCOMPARE(c.tree.value("id").toString(), QStringLiteral("root"));
    QCOMPARE(c.tree.value("gap").toInt(), 12);  // 13 is rounded down to an allowed gap
    QSet<QString> types;
    std::function<void(const QVariantMap&)> walk = [&](const QVariantMap& n) {
      types.insert(n.value("type").toString());
      for (const QVariant& k : n.value("children").toList()) walk(k.toMap());
    };
    walk(c.tree);
    const QStringList all = {"column", "row", "grid", "tabs", "scroll", "card", "section", "text", "chip", "badge", "avatar", "image", "bar",
                             "stat", "list", "table", "button", "toggle", "select", "segmented", "textfield", "empty", "location", "map"};
    for (const QString& t : all) QVERIFY2(types.contains(t), qPrintable(t));
    QVERIFY(c.tree.value("children").toList().size() == 14);
    const QVariantMap list = c.tree.value("children").toList().at(4).toMap();
    QCOMPARE(list.value("rows").toList().size(), 2);
    QCOMPARE(list.value("rows").toList().at(0).toMap().value("title").toString(), QStringLiteral("Route 1"));
  }

  void gapRounding() {
    QCOMPARE(conv("{type='row', gap=3}").tree.value("gap").toInt(), 4);
    QCOMPARE(conv("{type='row', gap=20}").tree.value("gap").toInt(), 20);
    QCOMPARE(conv("{type='row', gap=19}").tree.value("gap").toInt(), 16);
    QCOMPARE(conv("{type='row', gap=99}").tree.value("gap").toInt(), 20);
  }

  void invalidTrees() {
    auto bad = [](const QString& expr, const QString& needle) {
      const Conv c = conv(expr);
      QVERIFY2(!c.ok, qPrintable(expr));
      QVERIFY2(c.error.contains(needle), qPrintable(expr + " -> " + c.error));
    };
    bad("{type='column', children={ {type='text', text='a'}, {type='foo'} }}", "children[2].type: unknown block \"foo\"");
    bad("{type='column', children={ {type='row', children={ {type='text'} }} }}", "children[1].children[1]: missing required property \"text\"");
    bad("{type='text', text='a', color='red'}", "color: unknown property");
    bad("{type='text', text='a', tone='purple'}", "tone: invalid value \"purple\"");
    bad("{type='text', text=5}", "text: expected a string");
    bad("{type='text', text='a', children={}}", "children: block \"text\" takes no children");
    bad("{text='a'}", "missing \"type\"");
    bad("{'a','b'}", "got a list");
    bad("5", "must be a table");
    bad("{type='column', id='a', children={ {type='text', text='x', id='a'} }}", "children[1].id: id \"a\" is used twice");
    bad("{type='text', text='x', id='bad id'}", "id: must be 1 to 64 characters");
    bad("{type='avatar', initial='A', color=9}", "color: must be between 1 and 8");
    bad("{type='bar', value=1.5}", "value: must be a number between 0 and 1");
    bad("{type='image', src='/etc/passwd'}", "src: must start with");
    bad("{type='image', src='cache:../x.png'}", "src: invalid image path");
    bad("{type='image', src='file:///x.png'}", "src: must start with");
    bad("{type='segmented', options={{value=1,label='A'}}, selected=1, action='a'}", "options: needs 2 to 3 entries");
    bad("{type='list', rows={ {title='a', bogus=1} }}", "rows[1].bogus: unknown property");
    bad("{type='list', rows={ {sub='a'} }}", "rows[1]: missing required property \"title\"");
    bad("{type='button', label='x'}", "missing required property \"action\"");
    bad("{type='button', label='x', action='a', confirm={title='t'}}", "confirm: missing required property \"text\"");
    bad("{type='map', routes={ {id='r', x=1} }}", "routes[1]: missing required property \"y\"");
    bad("{type='map', players={ {id='p', initial='A', color=0, x=1, y=1} }}", "players[1].color");
    bad("{type='text', text='x', wrap='yes'}", "wrap: expected true or false");
    bad("{type='tabs', tabs={}, children={}}", "tabs: needs 1 to 12 entries");
  }

  void treeLimits() {
    QVERIFY(conv(nestedColumns(23)).ok);  // 23 columns + the leaf text = 24 levels
    const Conv deep = conv(nestedColumns(24));
    QVERIFY(!deep.ok);
    QVERIFY2(deep.error.contains("nested deeper than 24 levels"), qPrintable(deep.error));
    QVERIFY(deep.error.startsWith("children[1]."));  // path shows where

    const QString many = QStringLiteral("(function() local t = {} for i = 1, %1 do t[i] = {type='text', text='x'} end return {type='column', children=t} end)()");
    QVERIFY(conv(many.arg(1999)).ok);
    const Conv tooMany = conv(many.arg(2000));
    QVERIFY(!tooMany.ok);
    QVERIFY2(tooMany.error.contains("more than 2000 blocks"), qPrintable(tooMany.error));

    QVERIFY(conv(QStringLiteral("{type='text', text=string.rep('x', 2000)}")).ok);
    const Conv longText = conv(QStringLiteral("{type='text', text=string.rep('x', 2001)}"));
    QVERIFY(!longText.ok);
    QVERIFY2(longText.error.contains("text: longer than 2000 characters"), qPrintable(longText.error));

    // The Lua reader enforces the 512 KiB budget before conversion: luaValue() would warn and return nil -> not a table.
    const QString big = QStringLiteral("(function() local t = {} for i = 1, 300 do t[i] = {type='text', text=string.rep('x', 2000)} end return {type='column', children=t} end)()");
    const Value v = luaValue(big);
    QCOMPARE(int(v.type), int(Value::Type::Nil));
  }

  void toastConversion() {
    QVariantMap t;
    QString e;
    QVERIFY(convertToast(luaValue("{text='Hello', actions={ {label='Open', action='open', value={id=1}} }}"), t, e));
    QCOMPARE(t.value("tone").toString(), QStringLiteral("info"));
    QCOMPARE(t.value("actions").toList().size(), 1);
    QVERIFY(!convertToast(luaValue("{tone='info'}"), t, e));
    QVERIFY(!convertToast(luaValue("{text='x', tone='loud'}"), t, e));
    QVERIFY(!convertToast(luaValue("{text=string.rep('x', 301)}"), t, e));
    QVERIFY(!convertToast(luaValue("{text='x', actions={ {label='a',action='a'}, {label='b',action='b'}, {label='c',action='c'} }}"), t, e));
    QVERIFY(e.contains("actions: needs 0 to 2 entries"));
  }

  void patchByIdUnit() {
    Conv c = conv("{type='column', children={ {type='text', id='t', text='a'}, {type='row', children={ {type='bar', id='b', value=0.1} }} }}");
    QVERIFY(c.ok);
    QString e;
    QVERIFY2(patchUiTree(c.tree, "b", luaValue("{value=0.9, tone='ok'}"), e), qPrintable(e));
    const QVariantMap bar = c.tree.value("children").toList().at(1).toMap().value("children").toList().at(0).toMap();
    QCOMPARE(bar.value("value").toDouble(), 0.9);
    QCOMPARE(bar.value("tone").toString(), QStringLiteral("ok"));
    const QVariantMap before = c.tree;
    QVERIFY(!patchUiTree(c.tree, "nope", luaValue("{text='x'}"), e));
    QVERIFY(e.contains("no block with id \"nope\""));
    QVERIFY(!patchUiTree(c.tree, "t", luaValue("{type='bar'}"), e));
    QVERIFY(e.contains("cannot be changed with patch"));
    QVERIFY(!patchUiTree(c.tree, "t", luaValue("{tone='bogus'}"), e));
    QVERIFY(!patchUiTree(c.tree, "t", luaValue("{value=1}"), e));  // `value` is not a prop of text
    QCOMPARE(c.tree, before);
  }

  // ---------------------------------------------------------------- fb.ui through the host

  void permissionAndSlotChecks() {
    PluginSpec s = spec(R"LUA(
      local ok, err = pcall(fb.ui.set, 'panel', {type='text', text='x'})
      fb.log.info('R1 ' .. tostring(ok) .. ' ' .. tostring(err))
      ok, err = pcall(fb.ui.badge, 3)
      fb.log.info('R2 ' .. tostring(ok) .. ' ' .. tostring(err))
    )LUA", {});
    s.ui = QJsonObject();  // no ui declaration and no permission
    startSpec(s);
    QTRY_VERIFY(hasLog(con(), "R2 "));
    QVERIFY2(hasLog(con(), "R1 false"), qPrintable(logJoined(con())));
    QVERIFY(hasLog(con(), "permission ui not granted"));
    QVERIFY(m_host->uiTree(kId, "panel").isEmpty());

    PluginSpec d = spec(R"LUA(
      local ok, err = pcall(fb.ui.set, 'hud', {type='text', text='x'})
      fb.log.info('S1 ' .. tostring(ok) .. ' ' .. tostring(err))
      ok, err = pcall(fb.ui.set, 'tile:abc', {type='text', text='x'})
      fb.log.info('S2 ' .. tostring(ok))
      ok, err = pcall(fb.ui.set, 'nope', {type='text', text='x'})
      fb.log.info('S3 ' .. tostring(err))
      ok, err = pcall(fb.ui.set, 'panel', {type='column', children={ {type='text', text='a'}, {type='foo'} }})
      fb.log.info('S4 ' .. tostring(err))
    )LUA");
    d.ui = QJsonObject{{"panel", QJsonObject{{"title", "P"}}}, {"tile", true}};
    startSpec(d);
    QTRY_VERIFY(hasLog(con(), "S4 "));
    QVERIFY2(hasLog(con(), "slot hud not declared in manifest"), qPrintable(logJoined(con())));
    QVERIFY(hasLog(con(), "S2 true"));
    QVERIFY(hasLog(con(), "slot nope not declared in manifest"));
    QVERIFY2(hasLog(con(), "ui tree: children[2].type: unknown block \"foo\""), qPrintable(logJoined(con())));
    QVERIFY(m_host->uiTree(kId, "panel").isEmpty());
    QVERIFY(!m_host->uiTree(kId, "tile:abc").isEmpty());
  }

  void setPatchClearAndMeta() {
    startSpec(spec(R"LUA(
      fb.ui.set('panel', {type='column', children={ {type='text', id='t', text='a'}, {type='bar', id='b', value=0.5} }})
      fb.ui.patch('panel', 'b', {value=0.25})
      fb.ui.set('hud', {type='text', text='h'})
      fb.ui.clear('hud')
      fb.ui.title('panel', 'My panel')
      fb.ui.badge(5000)
      fb.ui.dot('news')
      local ok, err = pcall(fb.ui.patch, 'panel', 'zzz', {text='x'})
      fb.log.info('P1 ' .. tostring(err))
      ok, err = pcall(fb.ui.dot, 'red')
      fb.log.info('P2 ' .. tostring(ok))
      fb.log.info('DONE')
    )LUA"));
    QTRY_VERIFY(hasLog(con(), "DONE"));
    const QVariantMap panel = m_host->uiTree(kId, "panel");
    QCOMPARE(panel.value("children").toList().at(1).toMap().value("value").toDouble(), 0.25);
    QVERIFY(m_host->uiTree(kId, "hud").isEmpty());
    const UiPluginState st = m_host->uiState(kId);
    QCOMPARE(st.titles.value("panel"), QStringLiteral("My panel"));
    QCOMPARE(st.badge, 999);
    QCOMPARE(st.dot, QStringLiteral("news"));
    QVERIFY(hasLog(con(), "no block with id \"zzz\""));
    QVERIFY(hasLog(con(), "P2 false"));
  }

  void notifyRateLimit() {
    startSpec(spec(R"LUA(
      local ids = {}
      for i = 1, 10 do ids[#ids + 1] = tostring(fb.ui.notify{ tone='important', title='T', text='n' .. i, actions={ {label='Go', action='go', value=i} } }) end
      fb.log.info('IDS ' .. table.concat(ids, ','))
      local ok, err = pcall(fb.ui.notify, { text='x', tone='loud' })
      fb.log.info('BAD ' .. tostring(ok))
    )LUA"));
    QTRY_VERIFY(hasLog(con(), "BAD false"));
    QTRY_VERIFY(m_host->uiState(kId).trees.isEmpty());  // sanity: no tree
    const QList<QVariantMap> toasts = m_host->takeToasts();
    QVERIFY2(toasts.size() >= 3 && toasts.size() <= 4, qPrintable(QString::number(toasts.size())));  // burst of 3 (+1 if a second elapsed)
    QCOMPARE(toasts[0].value("pluginId").toString(), kId);
    QCOMPARE(toasts[0].value("tone").toString(), QStringLiteral("important"));
    QCOMPARE(toasts[0].value("actions").toList().at(0).toMap().value("action").toString(), QStringLiteral("go"));
    QVERIFY(toasts[0].value("toastId").toInt() >= 1);
    QVERIFY(hasLog(con(), "IDS 1,2,3,nil"));
    QVERIFY(hasLog(con(), "at most one notification per second"));
    QVERIFY(m_host->takeToasts().isEmpty());
  }

  void openRequests() {
    PluginSpec s = spec(R"LUA(
      fb.ui.open('panel')
      local ok, err = pcall(fb.ui.open, 'hud')
      fb.log.info('O1 ' .. tostring(err))
      ok, err = pcall(fb.ui.open, 'detail')
      fb.log.info('O2 ' .. tostring(err))
      ok, err = pcall(fb.ui.open, 'screen')
      fb.log.info('O3 ' .. tostring(ok))
      fb.ui.open('screen')
    )LUA");
    s.ui = QJsonObject{{"panel", QJsonObject{{"title", "P"}}}, {"screen", QJsonObject{{"title", "S"}}}, {"hud", true}, {"detail", true}};
    startSpec(s);
    QTRY_VERIFY(hasLog(con(), "O3 "));
    QVERIFY2(hasLog(con(), "O1 ") && hasLog(con(), "fb.ui.open only supports the slots screen and panel"), qPrintable(logJoined(con())));
    QVERIFY(hasLog(con(), "O3 true"));
    const auto reqs = m_host->takeOpenRequests();
    QVERIFY2(reqs.size() >= 2, qPrintable(QString::number(reqs.size())));
    QCOMPARE(reqs.at(0), qMakePair(QString(kId), QStringLiteral("panel")));
    QCOMPARE(reqs.last().second, QStringLiteral("screen"));
    QVERIFY(m_host->takeOpenRequests().isEmpty());

    PluginSpec u = spec(R"LUA(
      local ok, err = pcall(fb.ui.open, 'screen')
      fb.log.info('U1 ' .. tostring(err))
    )LUA");
    u.ui = QJsonObject{{"panel", QJsonObject{{"title", "P"}}}};
    startSpec(u);
    QTRY_VERIFY(hasLog(con(), "U1 "));
    QVERIFY(hasLog(con(), "slot screen not declared in manifest"));
    QVERIFY(m_host->takeOpenRequests().isEmpty());
  }

  void actionRoundTrip() {
    startSpec(spec(R"LUA(
      fb.ui.on_action(function(action, value, slot)
        local v = value
        if type(value) == 'table' then v = 'table:' .. tostring(value.n) .. ':' .. tostring(value.name) end
        fb.log.info('ACTION ' .. action .. ' ' .. tostring(v) .. ' ' .. slot)
      end)
    )LUA"));
    m_host->sendAction(kId, "panel", "go", QVariantMap{{"n", 7}, {"name", "x"}});
    m_host->sendAction(kId, "toast", "open", 42);
    m_host->sendAction(kId, "hotkey", "toggle-hud");
    m_host->sendAction(kId, "panel", "flag", true);
    QTRY_VERIFY(hasLog(con(), "ACTION toggle-hud nil hotkey"));
    QVERIFY2(hasLog(con(), "ACTION go table:7:x panel"), qPrintable(logJoined(con())));
    QVERIFY(hasLog(con(), "ACTION open 42 toast"));
    QVERIFY(hasLog(con(), "ACTION flag true panel"));
  }

  void actionWithoutHandlerAndRemoval() {
    startSpec(spec(R"LUA(
      local n = 0
      fb.ui.on_action(function(a) n = n + 1 fb.log.info('A' .. n) end)
      fb.events.on('fb', 'ping', function() fb.ui.on_action(nil) end)
    )LUA"));
    m_host->sendAction(kId, "panel", "x");
    QTRY_VERIFY(hasLog(con(), "A1"));
    m_host->postHostEvent("ping", QVariant());
    QTest::qWait(150);
    m_host->sendAction(kId, "panel", "x");
    QTest::qWait(150);
    QVERIFY(!hasLog(con(), "A2"));
    QCOMPARE(con().status, PluginStatus::Running);
  }

  void hostEvents() {
    startSpec(spec(R"LUA(
      fb.events.on('fb', 'detail.game', function(v, from, name)
        fb.log.info('EV ' .. name .. ' ' .. from .. ' ' .. (v and (v.system .. '/' .. v.code .. '/' .. v.sha256 .. '/' .. v.title) or 'nil'))
      end)
      fb.events.on('fb', 'tiles', function(v) fb.log.info('EV tiles ' .. #v .. ' ' .. v[1].id .. ' ' .. v[1].player .. ' ' .. v[2].game .. ' ' .. v[1].user_id .. '|' .. v[2].user_id) end)
      fb.events.on('fb', 'hud.visible', function(v) fb.log.info('EV hud ' .. tostring(v)) end)
    )LUA"));
    m_host->postHostEvent("detail.game", QVariantMap{{"system", "nds"}, {"code", "ABCD"}, {"sha256", "ff"}, {"title", "Game"}});
    m_host->postHostEvent("detail.game", QVariant());
    m_host->postHostEvent("tiles", QVariantList{QVariantMap{{"id", "t1"}, {"player", "Ann"}, {"game", "G1"}, {"user_id", "u_ann"}}, QVariantMap{{"id", "t2"}, {"player", "Bob"}, {"game", "G2"}, {"user_id", ""}}});
    m_host->postHostEvent("hud.visible", false);
    m_host->postHostEvent("hud.visible", true, "net.other.plugin");  // addressed to another plugin: not delivered
    QTRY_VERIFY(hasLog(con(), "EV hud false"));
    QVERIFY2(hasLog(con(), "EV detail.game fb nds/ABCD/ff/Game"), qPrintable(logJoined(con())));
    QVERIFY(hasLog(con(), "EV detail.game fb nil"));
    QVERIFY(hasLog(con(), "EV tiles 2 t1 Ann G2 u_ann|"));
    QVERIFY(!hasLog(con(), "EV hud true"));
  }

  void coalescedDelivery() {
    startSpec(spec(R"LUA(
      local n = 0
      fb.ui.on_action(function(action)
        if action == 'burst' then
          for i = 1, 500 do fb.ui.set('hud', {type='text', text='v' .. i}) end
        else
          n = n + 1
          fb.ui.set('panel', {type='text', text='n' .. n})
        end
      end)
    )LUA"));
    QSignalSpy spy(m_host, &PluginHost::uiChanged);
    m_host->sendAction(kId, "panel", "burst");
    QTRY_COMPARE(m_host->uiTree(kId, "hud").value("text").toString(), QStringLiteral("v500"));
    QTest::qWait(120);
    int hudSignals = 0;
    for (const auto& a : spy)
      if (a.at(0).toString() == kId && a.at(1).toString() == QLatin1String("hud")) ++hudSignals;
    QCOMPARE(hudSignals, 1);  // 500 updates in one call -> a single notification

    spy.clear();
    for (int i = 0; i < 200; ++i) m_host->sendAction(kId, "panel", "tick");
    QTRY_COMPARE(m_host->uiTree(kId, "panel").value("text").toString(), QStringLiteral("n200"));
    QTest::qWait(120);
    int panelSignals = 0;
    for (const auto& a : spy)
      if (a.at(1).toString() == QLatin1String("panel")) ++panelSignals;
    QVERIFY(panelSignals >= 1);
    QVERIFY2(panelSignals < 200, qPrintable(QString::number(panelSignals)));
  }

  void pausedAndReloaded() {
    startSpec(spec(R"LUA(
      fb.ui.set('panel', {type='text', text='alive'})
      fb.ui.badge(4)
      fb.ui.on_action(function() error('boom') end)
    )LUA"));
    QCOMPARE(m_host->uiTree(kId, "panel").value("text").toString(), QStringLiteral("alive"));
    QCOMPARE(m_host->uiState(kId).badge, 4);
    QSignalSpy spy(m_host, &PluginHost::uiChanged);
    // One error is only logged; the third within ten seconds pauses (0.22, board 4d-2D).
    m_host->sendAction(kId, "panel", "x");
    QTRY_VERIFY_WITH_TIMEOUT(con().errorCount >= 1, 5000);
    QCOMPARE(con().status, PluginStatus::Running);
    m_host->sendAction(kId, "panel", "x");
    m_host->sendAction(kId, "panel", "x");
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Paused, 5000);
    QTRY_VERIFY(m_host->uiState(kId).paused);
    QVERIFY(!m_host->uiTree(kId, "panel").isEmpty());  // kept so the UI can show it dimmed
    QCOMPARE(m_host->uiState(kId).badge, 0);
    QTRY_VERIFY(!spy.isEmpty());

    m_host->reload(kId);  // the plugin runs again, its old state was cleared first and then rebuilt
    QTRY_COMPARE_WITH_TIMEOUT(con().status, PluginStatus::Running, 5000);
    QVERIFY(!m_host->uiState(kId).paused);
    QCOMPARE(m_host->uiTree(kId, "panel").value("text").toString(), QStringLiteral("alive"));

    m_host->stop();
    QVERIFY(m_host->uiTree(kId, "panel").isEmpty());
  }

  void clearSlotFromHost() {
    startSpec(spec("fb.ui.set('detail', {type='text', text='d'}) fb.ui.set('panel', {type='text', text='p'}) fb.ui.on_action(function() fb.ui.patch('detail', 'x', {text='y'}) end)"));
    QVERIFY(!m_host->uiTree(kId, "detail").isEmpty());
    m_host->clearSlot("detail");
    QTRY_VERIFY(m_host->uiTree(kId, "detail").isEmpty());
    QVERIFY(!m_host->uiTree(kId, "panel").isEmpty());
  }

  void shellContext() {
    GameContext shell;
    shell.shell = true;
    shell.language = "en";
    PluginSpec s = spec(R"LUA(
      assert(next(fb.game) == nil, 'fb.game must be empty')
      assert(fb.memory.u8(0) == nil)
      assert(fb.memory.frame() == nil)
      assert(fb.rom.size() == nil)
      assert(fb.rom.read(0, 4) == nil)
      fb.ui.set('screen', {type='text', text='shell'})
      fb.log.info('SHELL OK')
    )LUA", {"ui", "memory.read", "rom.read"});
    startSpec(s, shell);
    QTRY_VERIFY(hasLog(con(), "SHELL OK"));
    QCOMPARE(m_host->uiTree(kId, "screen").value("text").toString(), QStringLiteral("shell"));
  }
};

QTEST_GUILESS_MAIN(UiTest)
#include "ui_test.moc"
