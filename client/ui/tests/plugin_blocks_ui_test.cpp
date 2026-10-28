// Plugin UI building blocks (PluginBlock.qml / PluginMapView.qml) in isolation: a tree with every block type rendered dark
// and light, button actions with and without confirmation, toggle, map marker selection, filter, clusters, keyboard
// navigation. No hub, no core. With UZUME_SCREENSHOT_DIR=<dir> the sheet and the map are also saved as PNGs (never
// checked in). Sprites come from a test image provider that paints generated pixmaps (no game assets).
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QSignalSpy>
#include <QtTest>
#include <QPainter>

#include "testsupport.h"

using uitest::Harness;

namespace {

class TestProvider : public QQuickImageProvider {
 public:
  TestProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
  QImage requestImage(const QString& id, QSize* size, const QSize&) override {
    const bool map = id.contains(QLatin1String("map"));
    const QSize sz = map ? QSize(400, 250) : QSize(24, 18);
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    const QColor c = QColor::fromHsv(static_cast<int>(qHash(id) % 360), 140, 200);
    if (map) {
      p.fillRect(img.rect(), QColor(40, 70, 55));
      p.setPen(QPen(QColor(70, 110, 85), 3));
      for (int i = 0; i < 400; i += 40) p.drawLine(i, 0, i + 60, 250);
    } else {
      p.setBrush(c);
      p.setPen(Qt::NoPen);
      p.drawEllipse(QRect(3, 2, 18, 14));
      p.setBrush(Qt::white);
      p.drawEllipse(QRect(8, 6, 3, 3));
    }
    p.end();
    if (size != nullptr) *size = sz;
    return img;
  }
};

const char* kPageQml = R"(
import QtQuick
import Uzume.App

Rectangle {
    id: page
    property var tree: ({})
    property bool darkMode: true
    property alias block: block
    color: Theme.ground
    // The wrapped design-system blocks read Theme.dark: the page follows the theme like the app does.
    Binding { target: Theme; property: "dark"; value: page.darkMode }
    width: 760
    height: block.implicitHeight + 32
    PluginBlock {
        id: block
        objectName: "sheet"
        x: 16
        y: 16
        width: page.width - 32
        height: implicitHeight
        node: page.tree
        dark: page.darkMode
        pluginId: "testplug"
    }
}
)";

QVariant json(const char* text) {
  QJsonParseError err;
  const QJsonDocument d = QJsonDocument::fromJson(QByteArray(text), &err);
  if (err.error != QJsonParseError::NoError) qFatal("bad test JSON: %s", qPrintable(err.errorString()));
  return d.toVariant();
}

const char* kSheet = R"({
 "type":"column","gap":12,"children":[
  {"type":"text","style":"title","text":"Block sheet"},
  {"type":"text","style":"heading","text":"Heading"},
  {"type":"text","style":"body","text":"Body text that is long enough to wrap onto a second line inside the sheet column when the width is small."},
  {"type":"text","style":"meta","text":"Meta text","tone":"warning"},
  {"type":"text","style":"eyebrow","text":"Eyebrow"},
  {"type":"text","id":"valueText","style":"value","text":"0x08001A10"},
  {"type":"row","gap":8,"children":[
    {"type":"chip","id":"chipOnly","text":"Only me","tone":"neutral","action":"chip","selected":true},
    {"type":"chip","text":"Open routes","tone":"info","action":"chip2"},
    {"type":"badge","text":"Caught","tone":"ok"},
    {"type":"badge","text":"Paused","tone":"warning"},
    {"type":"badge","text":"Error","tone":"danger"},
    {"type":"avatar","initial":"M","color":1,"size":"s"},
    {"type":"avatar","initial":"L","color":2,"size":"m"},
    {"type":"avatar","initial":"J","color":3,"size":"l"},
    {"type":"avatar","initial":"S","color":4}
  ]},
  {"type":"tabs","id":"tabs","tabs":["Team","Boxes"],"selected":1,"action":"tab","children":[
    {"type":"column","gap":8,"children":[
      {"type":"row","gap":12,"align":"center","children":[
        {"type":"image","id":"imgAlive","src":"cache:icons/25.png","size":"l","state":"alive","owner":1,"label":"Alive"},
        {"type":"image","src":"cache:icons/26.png","size":"m","state":"dead","owner":2},
        {"type":"image","src":"asset:icons/x.png","size":"s","state":"missed"},
        {"type":"image","size":"m","state":"empty"}]},
      {"type":"bar","id":"barHigh","value":0.9,"label":"HP high"},
      {"type":"bar","id":"barMid","value":0.35},
      {"type":"bar","id":"barLow","value":0.1}]},
    {"type":"text","text":"Boxes page"}]},
  {"type":"card","id":"card","eyebrow":"Run","title":"Card title","link_label":"Open","action":"card","value":"c1","children":[
    {"type":"stat","label":"Deaths","value":3,"action":"stat","value":"s1"},
    {"type":"stat","label":"Badges","value":"4 / 8"}]},
  {"type":"section","title":"Routes","link_label":"All","action":"sec","children":[
    {"type":"list","selected":"r2","rows":[
      {"id":"r1","title":"Route 1","sub":"Rattata","meta":"open","image":"cache:icons/19.png","tone":"info","action":"row","value":"r1"},
      {"id":"r2","title":"Route 2","sub":"Caught","meta":"caught","tone":"ok"}]},
    {"type":"table","columns":["Player","Deaths"],"rows":[{"cells":["Max","1"],"action":"trow","value":"max"},{"cells":["Lena","2"]}]}]},
  {"type":"row","gap":8,"children":[
    {"type":"button","id":"btnPlain","label":"Go","variant":"primary","action":"go","value":7},
    {"type":"button","id":"btnConfirm","label":"Reset","variant":"secondary","action":"reset","value":"all",
     "confirm":{"title":"Reset the run?","text":"This clears all routes.","confirm_label":"Reset run"}},
    {"type":"button","id":"btnDanger","label":"Delete","variant":"danger","action":"delete",
     "confirm":{"title":"Delete the run?","text":"This cannot be undone.","danger":true}},
    {"type":"button","label":"Later","variant":"quiet","action":"later"},
    {"type":"button","id":"btnOff","label":"Off","action":"off","disabled":true},
    {"type":"button","id":"btnOffConfirm","label":"OffC","action":"offc","disabled":true,
     "confirm":{"title":"Never","text":"Never shown."}}]},
  {"type":"toggle","id":"tg","label":"Show HUD","checked":false,"action":"tg"},
  {"type":"select","id":"sel","options":[{"value":"a","label":"Alpha"},{"value":"b","label":"Beta"}],"selected":"b","action":"sel"},
  {"type":"segmented","id":"seg","options":[{"value":"x","label":"One"},{"value":"y","label":"Two"}],"selected":"x","action":"seg"},
  {"type":"textfield","id":"tf","text":"abc","placeholder":"Name","action":"tf"},
  {"type":"textfield","id":"tfErr","text":"","placeholder":"Name","error":"Name is required","action":"tf2"},
  {"type":"empty","title":"Nothing here","text":"Start a run first.","action_label":"Start","action":"start"},
  {"type":"location","title":"Route 3","sub":"Area A","status":"encounter","children":[{"type":"text","style":"meta","text":"inside location"}]},
  {"type":"grid","gap":8,"columns":2,"children":[
    {"type":"badge","text":"g1","tone":"info"},{"type":"badge","text":"g2","tone":"neutral"},{"type":"badge","text":"g3","tone":"ok"}]},
  {"type":"scroll","max_height":"s","children":[
    {"type":"text","text":"Scroll line 1"},{"type":"text","text":"Scroll line 2"},{"type":"text","text":"Scroll line 3"}]},
  {"type":"does-not-exist","id":"unknown"}
 ]})";

const char* kMap = R"({
 "type":"map","id":"map","action":"map","image":"cache:map/overworld.png","image_w":400,"image_h":250,"me":"p1","filter":"all",
 "players":[{"id":"p1","initial":"M","color":1,"x":90,"y":110},{"id":"p2","initial":"L","color":2,"x":90,"y":110},{"id":"p3","initial":"J","color":3,"x":300,"y":60}],
 "routes":[
  {"id":"r1","x":60,"y":160,"area":"North","number":1,"icons":[{"src":"cache:i/1.png","owner":1}]},
  {"id":"r2","x":120,"y":170,"area":"North","number":2,"icons":[{"src":"cache:i/2.png","owner":2},{"src":"cache:i/3.png","owner":1}]},
  {"id":"r3","x":200,"y":120,"area":"South","number":3,"icons":[]},
  {"id":"r4","x":260,"y":200,"area":"South","number":4,"done":true,"icons":[{"src":"cache:i/4.png","owner":3}]},
  {"id":"r5","x":330,"y":150,"number":5,"icons":[]}],
 "popover":[{"owner":1,"name":"Max","src":"cache:i/1.png","status":"Caught"},{"owner":2,"name":"Lena","status":"Not yet visited, very long status text that ellipsizes"}]
})";

// Narrow side panel (300 px page, 268 px block): nothing may extend past the block.
const char* kNarrow = R"({
 "type":"column","gap":8,"children":[
  {"type":"row","gap":8,"children":[
    {"type":"avatar","initial":"M","color":1},
    {"type":"text","id":"longText","text":"A very long line of text that must wrap inside the row instead of running out of the narrow side panel of the game"}]},
  {"type":"row","gap":8,"children":[
    {"type":"text","id":"noWrap","wrap":false,"text":"Another very long line that is elided because wrapping is switched off for it"},
    {"type":"badge","text":"3"}]},
  {"type":"row","gap":8,"children":[
    {"type":"stat","id":"longStat","label":"A rather long stat label that needs eliding","value":"12 / 40"},
    {"type":"chip","text":"Chip"}]},
  {"type":"tabs","id":"tabs","tabs":["Overview","Party","Boxes","Routes","Deaths","Graveyard","Rules","Log"],"selected":8,"action":"tab","children":[
    {"type":"text","text":"1"},{"type":"text","text":"2"},{"type":"text","text":"3"},{"type":"text","text":"4"},
    {"type":"text","text":"5"},{"type":"text","text":"6"},{"type":"text","text":"7"},{"type":"text","id":"lastTab","text":"Log page"}]},
  {"type":"row","gap":8,"children":[
    {"type":"image","id":"labelImg","src":"cache:icons/25.png","size":"m","label":"An image label that is much too long for this narrow width"}]},
  {"type":"list","id":"theList","rows":[{"id":"r1","title":"A very long route title that has to be elided in the list","sub":"And a long subtitle just as well to be sure","meta":"a long meta text"}]},
  {"type":"table","id":"theTable","columns":["Player name here","Deaths and more"],"rows":[{"cells":["Maximilian the long","12345678901234"]}]}
 ]})";

}  // namespace

class PluginBlocksUiTest : public QObject {
  Q_OBJECT
  QQmlEngine* engine_ = nullptr;
  QQuickWindow* win_ = nullptr;
  QQuickItem* page_ = nullptr;
  QQuickItem* sheet_ = nullptr;
  QString shotDir_;

  QQuickItem* find(const char* name) const { return Harness::findVisual(win_->contentItem(), QString::fromLatin1(name)); }
  QQuickItem* mustFind(const char* name) const {
    QQuickItem* it = find(name);
    if (it == nullptr) qWarning("[test] missing item %s", name);
    return it;
  }
  void settle() {
    QQuickTest::qWaitForPolish(win_);
    QTest::qWait(60);
    QQuickTest::qWaitForPolish(win_);
  }
  void show(const QVariant& tree, bool dark, int width = 760) {
    delete page_;
    page_ = nullptr;
    QQmlComponent c(engine_);
    c.setData(kPageQml, QUrl(QStringLiteral("qrc:/test/blocks_page.qml")));
    QObject* o = c.create();
    QVERIFY2(o != nullptr, qPrintable(c.errorString()));
    page_ = qobject_cast<QQuickItem*>(o);
    page_->setProperty("darkMode", dark);
    page_->setWidth(width);
    page_->setProperty("tree", tree);
    page_->setParentItem(win_->contentItem());
    sheet_ = page_->property("block").value<QQuickItem*>();
    QVERIFY(sheet_ != nullptr);
    win_->resize(width, (std::max)(300, static_cast<int>(page_->height())));
    settle();
    win_->resize(width, (std::max)(300, static_cast<int>(page_->height())));
    settle();
  }
  void clickItem(QQuickItem* it) {
    QVERIFY(it != nullptr);
    const QPointF p = it->mapToScene(QPointF(it->width() / 2, it->height() / 2));
    QTest::mouseClick(win_, Qt::LeftButton, Qt::NoModifier, p.toPoint());
    QTest::qWait(40);
    settle();
  }
  void shot(const QString& name) {
    if (shotDir_.isEmpty()) return;
    settle();
    QTest::qWait(300);
    QDir().mkpath(shotDir_);
    const QImage img = win_->grabWindow();
    QVERIFY2(!img.isNull() && !uitest::isAllBlack(img), qPrintable(name));
    QVERIFY(img.save(QDir(shotDir_).filePath(name + QStringLiteral(".png"))));
  }
  static QString textOf(QQuickItem* it) { return it != nullptr ? it->property("text").toString() : QString(); }

 private slots:
  void initTestCase() {
    shotDir_ = qEnvironmentVariable("UZUME_SCREENSHOT_DIR");
    uitest::installWarningCounter();
    uitest::warningCount() = 0;
    engine_ = new QQmlEngine(this);
    engine_->addImportPath(QStringLiteral("qrc:/qt/qml"));
    engine_->addImageProvider(QStringLiteral("uzplugin"), new TestProvider);
    win_ = new QQuickWindow;
    win_->resize(760, 900);
    win_->show();
    QVERIFY(QTest::qWaitForWindowExposed(win_));
  }
  void cleanupTestCase() {
    delete page_;
    delete win_;
    QCOMPARE(uitest::warningCount().load(), 0);
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void sheetRendersEveryBlock_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
  }
  void sheetRendersEveryBlock() {
    QFETCH(bool, dark);
    show(json(kSheet), dark);
    for (const char* n : {"plugin_valueText", "plugin_chipOnly", "plugin_tabs", "plugin_imgAlive", "plugin_barHigh", "plugin_barMid", "plugin_barLow",
                          "plugin_card", "plugin_btnPlain", "plugin_btnConfirm", "plugin_btnDanger", "plugin_tg", "plugin_sel", "plugin_seg", "plugin_tf",
                          "plugin_tfErr", "pluginListRow_0", "pluginTableRow_0", "pluginLocationStatus", "pluginTab_2", "pluginTextFieldError"})
      QVERIFY2(mustFind(n) != nullptr, n);
    QCOMPARE(textOf(mustFind("fbButtonLabel")), QStringLiteral("Go"));
    QCOMPARE(textOf(mustFind("pluginLocationStatus")->childItems().value(0)), QStringLiteral("Encounter now"));
    QList<QQuickItem*> errors;
    Harness::collectVisual(win_->contentItem(), QStringLiteral("pluginTextFieldError"), errors);
    QCOMPARE(errors.size(), 2);
    QCOMPARE(textOf(errors.last()), QStringLiteral("Name is required"));
    QVERIFY(!errors.first()->isVisible() && errors.last()->isVisible());
    QCOMPARE(textOf(mustFind("plugin_valueText")->childItems().value(0)->childItems().value(0)), QStringLiteral("0x08001A10"));
    // Bars: fill proportional to the value; the unknown block and the Boxes page render nothing.
    auto fillRatio = [&](const char* id) {
      QQuickItem* fill = Harness::findVisual(mustFind(id), QStringLiteral("pluginBarFill"));
      return fill->width() / fill->parentItem()->width();
    };
    QVERIFY(qAbs(fillRatio("plugin_barHigh") - 0.9) < 0.01);
    QVERIFY(qAbs(fillRatio("plugin_barLow") - 0.1) < 0.01);
    QVERIFY(find("plugin_unknown") != nullptr);
    QCOMPARE(find("plugin_unknown")->implicitHeight(), 0.0);
    QVERIFY(sheet_->height() > 600);
    shot(dark ? QStringLiteral("plugin-blocks-dark") : QStringLiteral("plugin-blocks-light"));
  }

  // 0.20 final live check: a chip is as wide as its text ("Edit list", "Turn all off" were cut to "E…" and "All …"), also in a full row.
  void chipsFollowTheirText() {
    show(json(R"({"type":"column","gap":8,"children":[
      {"type":"textfield","id":"q","text":"","placeholder":"Search cheats","action":"q"},
      {"type":"row","gap":8,"align":"center","children":[
        {"type":"chip","id":"chipEdit","text":"Edit list","action":"edit"},
        {"type":"chip","id":"chipAllOff","text":"Turn all off","action":"off"}]},
      {"type":"row","gap":8,"children":[
        {"type":"chip","id":"chipFull","text":"Open routes","action":"x"},
        {"type":"text","text":"A long line of text that has to give way before the chip does, in a narrow panel like the game side panel"},
        {"type":"chip","id":"chipFull2","text":"Only me","action":"y"}]}]})"),
         true, 300);
    for (const char* id : {"plugin_chipEdit", "plugin_chipAllOff", "plugin_chipFull", "plugin_chipFull2"}) {
      QQuickItem* chip = mustFind(id);
      QVERIFY2(chip != nullptr, id);
      QQuickItem* label = Harness::findVisual(chip, QStringLiteral("pluginChipLabel"));
      QVERIFY2(label != nullptr, id);
      QVERIFY2(!label->property("truncated").toBool(), id);
      QVERIFY2(chip->width() >= label->implicitWidth() + 2, id);
    }
    QVERIFY(sheet_->width() <= 300);
  }

  // PI-5: strings from plugins and peers are text, never HTML: no <img> fetch, no <b>/<font> restyling.
  void hostileStringsRenderLiterally() {
    const QString evil = QStringLiteral("<img src=\"https://hostile.example/p.png?ip\"><b>Official</b><font size=40>x</font>");
    QJsonObject tree = QJsonDocument::fromJson(R"({"type":"column","gap":8,"children":[]})").object();
    QJsonArray kids;
    const auto add = [&](const char* json) {
      QJsonObject o = QJsonDocument::fromJson(QByteArray(json).replace("EVIL", evil.toUtf8().replace("\"", "\\\""))).object();
      QVERIFY2(!o.isEmpty(), json);
      kids.append(o);
    };
    add(R"({"type":"text","id":"evText","text":"EVIL"})");
    add(R"({"type":"text","style":"title","text":"EVIL"})");
    add(R"({"type":"chip","text":"EVIL","action":"c"})");
    add(R"({"type":"badge","text":"EVIL"})");
    add(R"({"type":"button","label":"EVIL","action":"b"})");
    add(R"({"type":"stat","label":"EVIL","value":"EVIL"})");
    add(R"({"type":"section","title":"EVIL","children":[{"type":"list","rows":[{"id":"r1","title":"EVIL","sub":"EVIL","meta":"EVIL"}]},
        {"type":"table","columns":["EVIL"],"rows":[{"cells":["EVIL"]}]}]})");
    add(R"({"type":"card","eyebrow":"EVIL","title":"EVIL","children":[]})");
    add(R"({"type":"empty","title":"EVIL","text":"EVIL"})");
    add(R"({"type":"location","title":"EVIL","sub":"EVIL"})");
    add(R"({"type":"tabs","tabs":["EVIL","B"],"selected":1,"children":[{"type":"text","text":"EVIL"},{"type":"text","text":"EVIL"}]})");
    add(R"({"type":"select","options":[{"value":"a","label":"EVIL"}],"selected":"a"})");
    add(R"({"type":"segmented","options":[{"value":"x","label":"EVIL"}],"selected":"x"})");
    tree.insert("children", kids);
    show(tree.toVariantMap(), true);
    int checked = 0;
    std::function<void(QQuickItem*)> walk = [&](QQuickItem* it) {
      const QVariant text = it->property("text");
      const QVariant fmt = it->property("textFormat");
      if (text.isValid() && text.metaType().id() == QMetaType::QString && text.toString().contains(QLatin1Char('<')) && fmt.isValid()) {
        ++checked;
        QVERIFY2(fmt.toInt() == Qt::PlainText,
                 qPrintable(QString::fromLatin1(it->metaObject()->className()) + " renders plugin text as format " + QString::number(fmt.toInt())));
      }
      for (QQuickItem* c : it->childItems()) walk(c);
    };
    walk(sheet_);
    QVERIFY2(checked >= 15, qPrintable(QString::number(checked)));
    QQuickItem* ev = mustFind("plugin_evText");
    QVERIFY(ev != nullptr);
    std::function<QQuickItem*(QQuickItem*)> firstText = [&](QQuickItem* it) -> QQuickItem* {
      if (it->property("text").toString() == evil) return it;
      for (QQuickItem* c : it->childItems())
        if (QQuickItem* r = firstText(c)) return r;
      return nullptr;
    };
    QQuickItem* t = firstText(ev);
    QVERIFY(t != nullptr);
    QCOMPARE(t->property("textFormat").toInt(), int(Qt::PlainText));
  }

  void buttonActions() {
    show(json(kSheet), true);
    QSignalSpy spy(sheet_, SIGNAL(action(QString, QVariant)));
    QVERIFY(spy.isValid());
    clickItem(mustFind("plugin_btnPlain"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("go"));
    QCOMPARE(spy.at(0).at(1).toInt(), 7);

    // With confirm: nothing until the dialog is confirmed; Cancel emits nothing.
    clickItem(mustFind("plugin_btnConfirm"));
    QCOMPARE(spy.count(), 1);
    QTRY_VERIFY(find("pluginConfirmOk") != nullptr);
    QCOMPARE(textOf(mustFind("pluginConfirmTitle")), QStringLiteral("Reset the run?"));
    QTRY_VERIFY(mustFind("pluginConfirmOk")->hasActiveFocus());   // initial focus: primary
    clickItem(mustFind("pluginConfirmCancel"));
    QCOMPARE(spy.count(), 1);
    QTRY_VERIFY(find("pluginConfirmOk") == nullptr || !find("pluginConfirmOk")->isVisible());
    clickItem(mustFind("plugin_btnConfirm"));
    QTRY_VERIFY(find("pluginConfirmOk") != nullptr && find("pluginConfirmOk")->isVisible());
    clickItem(mustFind("pluginConfirmOk"));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("reset"));
    QCOMPARE(spy.at(1).at(1).toString(), QStringLiteral("all"));

    // Danger: initial focus on Cancel; Escape cancels.
    clickItem(mustFind("plugin_btnDanger"));
    QTRY_VERIFY(find("pluginConfirmCancel") != nullptr && find("pluginConfirmCancel")->isVisible());
    QTRY_VERIFY(mustFind("pluginConfirmCancel")->hasActiveFocus());
    QTest::keyClick(win_, Qt::Key_Escape);
    QTest::qWait(80);
    QCOMPARE(spy.count(), 2);
  }

  void disabledButtonSendsNothing() {
    show(json(kSheet), true);
    QSignalSpy spy(sheet_, SIGNAL(action(QString, QVariant)));
    QVERIFY(spy.isValid());
    clickItem(mustFind("plugin_btnOff"));
    clickItem(mustFind("plugin_btnOffConfirm"));
    QTest::qWait(80);
    QCOMPARE(spy.count(), 0);
    QVERIFY(find("pluginConfirmOk") == nullptr || !find("pluginConfirmOk")->isVisible());
    clickItem(mustFind("plugin_btnPlain"));  // an enabled one still works
    QCOMPARE(spy.count(), 1);
  }

  void togglesAndChoices() {
    show(json(kSheet), false);
    QSignalSpy spy(sheet_, SIGNAL(action(QString, QVariant)));
    clickItem(mustFind("plugin_tg"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("tg"));
    QCOMPARE(spy.at(0).at(1).toBool(), true);
    clickItem(mustFind("pluginSegment_y"));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("seg"));
    QCOMPARE(spy.at(1).at(1).toString(), QStringLiteral("y"));
    clickItem(mustFind("pluginTab_2"));
    QCOMPARE(spy.count(), 3);
    QCOMPARE(spy.at(2).at(0).toString(), QStringLiteral("tab"));
    QCOMPARE(spy.at(2).at(1).toInt(), 2);
    QTRY_VERIFY(find("pluginBarFill") == nullptr);   // the Boxes page replaced the Team page
    clickItem(mustFind("pluginListRow_0"));
    QCOMPARE(spy.at(3).at(0).toString(), QStringLiteral("row"));
    QCOMPARE(spy.at(3).at(1).toString(), QStringLiteral("r1"));
    // Text field: Enter submits the text.
    QQuickItem* input = mustFind("pluginTextFieldInput");
    input->forceActiveFocus();
    QTest::keyClick(win_, Qt::Key_Return);
    QCOMPARE(spy.at(4).at(0).toString(), QStringLiteral("tf"));
    QCOMPARE(spy.at(4).at(1).toString(), QStringLiteral("abc"));
  }

  // Right edge of `it` in the coordinates of the block under test.
  qreal rightIn(QQuickItem* it) const { return it->mapToItem(sheet_, QPointF(it->width(), 0)).x(); }

  void narrowWidthStaysInside() {
    show(json(kNarrow), true, 300);
    const qreal w = sheet_->width();
    QVERIFY2(qAbs(w - 268) < 1, qPrintable(QString::number(w)));
    for (const char* n : {"plugin_longText", "plugin_noWrap", "plugin_longStat", "plugin_tabs", "pluginTabStrip", "plugin_labelImg", "plugin_theList", "plugin_theTable",
                          "pluginListRow_0", "pluginTableRow_0"}) {
      QQuickItem* it = mustFind(n);
      QVERIFY2(it != nullptr, n);
      QVERIFY2(rightIn(it) <= w + 0.5, qPrintable(QStringLiteral("%1 right=%2 of %3").arg(QLatin1String(n)).arg(rightIn(it)).arg(w)));
    }
    // The long text wraps (taller than one line) instead of running out of the row.
    QVERIFY(mustFind("plugin_longText")->height() > 30);
    // Tabs: the strip is narrower than its content, the selected last tab was scrolled into view.
    QQuickItem* strip = mustFind("pluginTabStrip");
    QVERIFY(strip->property("contentWidth").toReal() > strip->width());
    QTRY_VERIFY(strip->property("contentX").toReal() > 0);
    QQuickItem* last = mustFind("pluginTab_8");
    const qreal lx = last->mapToItem(strip, QPointF(last->width(), 0)).x();
    QVERIFY2(lx <= strip->width() + 1, qPrintable(QString::number(lx)));
    // The label of the image block is elided.
    QVERIFY(find("plugin_lastTab") != nullptr);
    shot(QStringLiteral("plugin-blocks-narrow"));
  }

  // A row with a text and an input (Splits editor: number + name + buttons): the text keeps its natural width and the inputs of
  // different rows start at the same x, whatever they contain.
  void rowInputsAlign() {
    const char* tree = R"({"type":"column","gap":8,"children":[
      {"type":"row","gap":8,"align":"center","children":[{"type":"text","id":"n1","text":"1"},{"type":"textfield","id":"f1","text":"Route 1"},{"type":"button","label":"x","action":"d1"}]},
      {"type":"row","gap":8,"align":"center","children":[{"type":"text","id":"n2","text":"2"},{"type":"textfield","id":"f2","text":"A much longer split name than the first one has"},{"type":"button","label":"x","action":"d2"}]}]})";
    show(json(tree), false, 700);
    QQuickItem* n1 = mustFind("plugin_n1");
    QQuickItem* f1 = mustFind("plugin_f1");
    QQuickItem* f2 = mustFind("plugin_f2");
    QVERIFY(n1 != nullptr && f1 != nullptr && f2 != nullptr);
    QVERIFY2(n1->width() <= n1->implicitWidth() + 0.5, qPrintable(QString::number(n1->width())));
    const qreal x1 = f1->mapToItem(sheet_, QPointF(0, 0)).x();
    const qreal x2 = f2->mapToItem(sheet_, QPointF(0, 0)).x();
    QVERIFY2(qAbs(x1 - x2) < 0.5, qPrintable(QStringLiteral("field x %1 vs %2").arg(x1).arg(x2)));
    QVERIFY2(qAbs(f1->width() - f2->width()) < 0.5, qPrintable(QStringLiteral("field width %1 vs %2").arg(f1->width()).arg(f2->width())));
  }

  // SoulLink's HUD tree (card > row(gap 8) > text + badge) at the block width of a 240 and a 320 px HUD widget: the text never gets
  // narrower than min(its width, 96 px); the badge gives way by eliding.
  void hudRowKeepsTheTextReadable() {
    const char* tree = R"({"type":"card","children":[{"type":"row","gap":8,"align":"center","children":[
      {"type":"text","id":"slText","style":"body","text":"SL · Route 103"},
      {"type":"badge","id":"slBadge","text":"Noch keine Begegnung"}]}]})";
    for (int blockWidth : {240 - 24 - 16 - 6, 320 - 24 - 16 - 6}) {
      show(json(tree), true, blockWidth + 32);
      QQuickItem* text = mustFind("plugin_slText");
      QQuickItem* badge = mustFind("plugin_slBadge");
      QVERIFY(text != nullptr && badge != nullptr);
      const qreal want = (std::min)(text->implicitWidth(), 96.0);
      QVERIFY2(text->width() >= want - 0.5, qPrintable(QStringLiteral("text %1 px of %2 (block %3)").arg(text->width()).arg(text->implicitWidth()).arg(blockWidth)));
      QVERIFY2(rightIn(badge) <= sheet_->width() + 0.5, qPrintable(QStringLiteral("badge right %1 of %2").arg(rightIn(badge)).arg(sheet_->width())));
      QVERIFY(badge->width() > 0);
      shot(QStringLiteral("plugin-blocks-hud-row-%1").arg(blockWidth));
    }
  }

  void mapToolbarFitsNarrow() {
    for (int width : {300, 280}) {
      show(json(kMap), true, width);
      sheet_->setHeight(440);
      settle();
      QQuickItem* filter = mustFind("pluginMapFilter");
      QQuickItem* zoom = mustFind("pluginMapZoomBar");
      QQuickItem* map = mustFind("pluginMapView")->parentItem();
      QVERIFY(rightIn(filter) <= map->width() + 0.5);
      QVERIFY(zoom->mapToItem(map, QPointF(zoom->width(), 0)).x() <= map->width() + 0.5);
      QVERIFY(zoom->mapToItem(map, QPointF(0, 0)).x() >= -0.5);
      // Filter and zoom controls never overlap: side by side or the zoom row below the filter.
      const QRectF fr(filter->mapToItem(map, QPointF(0, 0)), QSizeF(filter->width(), filter->height()));
      const QRectF zr(zoom->mapToItem(map, QPointF(0, 0)), QSizeF(zoom->width(), zoom->height()));
      QVERIFY2(!fr.intersects(zr), qPrintable(QStringLiteral("overlap at width %1").arg(width)));
      QVERIFY(mustFind("pluginMapView")->parentItem()->property("clip").toBool());
      shot(QStringLiteral("plugin-map-narrow-%1").arg(width));
    }
  }

  void mapFitShowsWholeImage() {
    // 4:3 image (3DS bottom screen) in a wide, short view: height-limited, centered, whole image visible.
    QString tree = QString::fromUtf8(kMap);
    tree.replace(QStringLiteral("\"image_w\":400,\"image_h\":250"), QStringLiteral("\"image_w\":320,\"image_h\":240"));
    show(json(tree.toUtf8().constData()), true, 720);
    sheet_->setHeight(300);
    settle();
    QQuickItem* view = mustFind("pluginMapView");
    QQuickItem* map = view->parentItem();
    const qreal expect = (std::min)(view->width() / 320.0, view->height() / 240.0);
    QVERIFY(qAbs(map->property("fitScale").toReal() - expect) < 1e-6);
    QVERIFY(expect < view->width() / 320.0);   // height-limited
    map->metaObject()->invokeMethod(map, "fit");
    const qreal cw = 320 * expect, ch = 240 * expect;
    QVERIFY(ch <= view->height() + 0.5 && cw <= view->width() + 0.5);
    QVERIFY(qAbs(map->property("ox").toReal() - (view->width() - cw) / 2) < 0.6);
    QVERIFY(qAbs(map->property("oy").toReal() - (view->height() - ch) / 2) < 0.6);
  }
  void mapSelectionFilterAndClusters() {
    show(json(kMap), true, 720);
    sheet_->setHeight(440);
    settle();
    QSignalSpy spy(sheet_, SIGNAL(action(QString, QVariant)));
    QVERIFY(mustFind("pluginMapRoute_r5") != nullptr);
    QVERIFY(find("pluginMapCluster_North") == nullptr);   // 100 % zoom: single markers
    QVERIFY(mustFind("pluginMapPlayer_p1") != nullptr);
    // Two players on one spot fan out by 14 px.
    QVERIFY(qAbs(qAbs(mustFind("pluginMapPlayer_p1")->x() - mustFind("pluginMapPlayer_p2")->x()) - 14) < 0.6);
    QCOMPARE(mustFind("pluginMapPopover")->isVisible(), false);
    shot(QStringLiteral("plugin-map-select-prep"));

    clickItem(mustFind("pluginMapRoute_r1"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("map"));
    const QVariantMap v = spy.at(0).at(1).toMap();
    QCOMPARE(v.value("kind").toString(), QStringLiteral("route"));
    QCOMPARE(v.value("id").toString(), QStringLiteral("r1"));
    QVERIFY(mustFind("pluginMapPopover")->isVisible());
    QVERIFY(mustFind("pluginMapPopover")->y() >= 0);
    shot(QStringLiteral("plugin-map-default-dark"));
    // Esc closes the popover.
    mustFind("pluginMapRoute_r1")->forceActiveFocus();
    QTest::keyClick(win_, Qt::Key_Escape);
    QTRY_VERIFY(!mustFind("pluginMapPopover")->isVisible());

    // Filter "Open routes" emits; done routes fade to 35 %.
    clickItem(mustFind("pluginSegment_open"));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(1).toMap().value("kind").toString(), QStringLiteral("filter"));
    QCOMPARE(spy.at(1).at(1).toMap().value("value").toString(), QStringLiteral("open"));
    QVERIFY(qAbs(mustFind("pluginMapRoute_r4")->opacity() - 0.35) < 0.01);
    QCOMPARE(mustFind("pluginMapRoute_r3")->opacity(), 1.0);
    clickItem(mustFind("pluginSegment_me"));
    clickItem(mustFind("pluginSegment_all"));

    // Player marker selection.
    clickItem(mustFind("pluginMapPlayer_p3"));
    QCOMPARE(spy.last().at(1).toMap().value("kind").toString(), QStringLiteral("player"));
    QCOMPARE(spy.last().at(1).toMap().value("id").toString(), QStringLiteral("p3"));

    // Zoom out below 60 %: routes with an area merge into clusters; the route without an area stays.
    QQuickItem* map = mustFind("pluginMapView")->parentItem();
    for (int i = 0; i < 3; ++i) clickItem(mustFind("pluginMapZoomOut"));
    QVERIFY(map->property("zoomPct").toReal() < 0.6);
    QTRY_VERIFY(find("pluginMapCluster_North") != nullptr);
    QVERIFY(find("pluginMapCluster_South") != nullptr);
    QVERIFY(find("pluginMapRoute_r1") == nullptr);
    QVERIFY(mustFind("pluginMapRoute_r5") != nullptr);
    shot(QStringLiteral("plugin-map-cluster-dark"));
    // A click on a cluster zooms into it; fit restores 100 %.
    clickItem(mustFind("pluginMapCluster_North"));
    QVERIFY(map->property("zoomPct").toReal() >= 0.6);
    clickItem(mustFind("pluginMapFit"));
    QCOMPARE(map->property("zoomPct").toReal(), 1.0);
  }

  void mapKeyboardAndStates() {
    show(json(kMap), true, 720);
    sheet_->setHeight(440);
    settle();
    QSignalSpy spy(sheet_, SIGNAL(action(QString, QVariant)));
    QQuickItem* view = mustFind("pluginMapView");
    view->forceActiveFocus();
    QTest::keyClick(win_, Qt::Key_Right);
    QTest::qWait(30);
    QQuickItem* focused = win_->activeFocusItem();
    QVERIFY(focused != nullptr && focused->objectName().startsWith(QLatin1String("pluginMap")) && focused != view);
    QTest::keyClick(win_, Qt::Key_Right);
    QVERIFY(win_->activeFocusItem()->objectName().startsWith(QLatin1String("pluginMap")));
    QTest::keyClick(win_, Qt::Key_Return);
    QTRY_VERIFY(spy.count() >= 1);
    QCOMPARE(spy.last().at(0).toString(), QStringLiteral("map"));
    QVERIFY(mustFind("pluginMapPopover")->isVisible());
    QTest::keyClick(win_, Qt::Key_Escape);
    QTRY_VERIFY(!mustFind("pluginMapPopover")->isVisible());
    // Hooks for the gamepad wiring exist and work.
    QVERIFY(QMetaObject::invokeMethod(view->parentItem(), "zoomIn"));
    QVERIFY(view->parentItem()->property("zoomPct").toReal() > 1.0);
    QVERIFY(QMetaObject::invokeMethod(view->parentItem(), "panBy", Q_ARG(QVariant, 10), Q_ARG(QVariant, 10)));
    QVERIFY(QMetaObject::invokeMethod(view->parentItem(), "fit"));

    // Light theme and the loading state.
    show(json(kMap), false, 720);
    sheet_->setHeight(440);
    shot(QStringLiteral("plugin-map-default-light"));
    QVariantMap loading = json(kMap).toMap();
    loading["image"] = QString();
    show(loading, true, 720);
    sheet_->setHeight(440);
    settle();
    QCOMPARE(mustFind("pluginMapLoading")->isVisible(), true);
    QCOMPARE(textOf(mustFind("pluginMapLoading")->childItems().value(0)), QStringLiteral("Map not read yet"));
    shot(QStringLiteral("plugin-map-loading-dark"));
  }
  // Regression (live check 0.22): Start in the Splits screen replaced the whole tree (button flags, stat values, table rows
  // or table -> empty) and the app died in QQuickGridLayoutBase::rearrange. The next state's tree is applied as uz.ui.set
  // does, without waiting for the polish in between.
  // Regression (live check 0.22, SIGSEGV when pressing Start in the Splits screen). Qt 6.4's QQuickGridLayoutBase::rearrange does
  // not refresh its item list before it runs from QQuickLayout::geometryChange. A layout child that a Repeater destroyed after the
  // layout was invalidated but before its polish is still in that list, and the next size change reads the freed item in
  // QGridLayoutItem::stretchFactor. The plugin blocks are nested layouts sized by `anchors.fill` of a Loader, so their parent
  // layout's rearrange resizes them synchronously, before they are polished. The test applies a new tree (new table rows,
  // fewer or more children), lets the deferred deletes run and changes the width, with no polish in between.
  void treeUpdatesNeverLeaveFreedItemsInALayout() {
    auto tbl = [](int rows, int cols) {
      QVariantList cells;
      for (int i = 0; i < rows; ++i) {
        QVariantList row;
        for (int c = 0; c < cols; ++c) row << QStringLiteral("r%1c%2").arg(i).arg(c);
        cells << QVariantMap{{"cells", row}};
      }
      QVariantList heads;
      for (int c = 0; c < cols; ++c) heads << QStringLiteral("H%1").arg(c);
      return QVariant(QVariantMap{{"type", "table"}, {"id", "t"}, {"columns", heads}, {"rows", cells}});
    };
    auto lst = [](int rows) {
      QVariantList r;
      for (int i = 0; i < rows; ++i) r << QVariantMap{{"id", QString::number(i)}, {"title", QStringLiteral("Row %1").arg(i)}, {"meta", "m"}};
      return QVariant(QVariantMap{{"type", "list"}, {"id", "l"}, {"rows", r}});
    };
    auto btn = [](int i, bool off) {
      QVariantMap b{{"type", "button"}, {"label", QStringLiteral("B%1").arg(i)}, {"action", "a"}};
      if (off) b["disabled"] = true;
      return QVariant(b);
    };
    auto grid = [&](int n, int flip) {
      QVariantList kids;
      for (int i = 0; i < n; ++i) kids << btn(i, (i + flip) % 2 == 0);
      return QVariant(QVariantMap{{"type", "grid"}, {"gap", 8}, {"columns", 3}, {"children", kids}});
    };
    auto tree = [&](int step) {
      const int rows = 1 + (step * 3) % 6;
      QVariantList kids{grid(2 + step % 4, step), tbl(rows, 3 + step % 3), lst(step % 5), tbl(step % 2 ? 0 : 2, 6)};
      return QVariant(QVariantMap{{"type", "column"}, {"gap", 16}, {"children", kids}});
    };
    show(tree(0), true);
    for (int step = 1; step <= 60; ++step) {
      page_->setProperty("tree", tree(step));
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);   // the freed delegates, before any polish
      page_->setWidth(page_->width() + ((step % 2) ? 9 : -9));
      if (step % 10 == 0) settle();
    }
    settle();
    QVERIFY(find("plugin_t") != nullptr);
    // The pool keeps surplus rows hidden, not alive and visible: only the current rows show.
    page_->setProperty("tree", QVariantMap{{"type", "column"}, {"children", QVariantList{tbl(2, 3)}}});
    settle();
    int shown = 0;
    for (int i = 0; i < 10; ++i) {
      QQuickItem* r = find(QStringLiteral("pluginTableRow_%1").arg(i).toLatin1().constData());
      if (r != nullptr && r->isVisible()) ++shown;
    }
    QCOMPARE(shown, 2);
  }
};

UITEST_MAIN(PluginBlocksUiTest)
#include "plugin_blocks_ui_test.moc"
