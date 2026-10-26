// Controllers page revision (3f-2, 3f-3): glyph sets (Xbox, PlayStation, Generic, Keyboard), "Button labels" with Auto
// from the detected controller type and a per-device override, drawn D-pad arrows, controller-shaped input test and the
// layout of the mapping table inside its column at 1280 and 960 px. SDL virtual joysticks, FakeHub, no core, no ROMs.
#include <SDL3/SDL.h>

#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

#include "controllerprofiles.h"
#include "controllersglyphs.h"
#include "fakehub.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
enum Btn { South = 0, East, West, North, Back, Guide, Start, LS, RS, LB, RB, DUp, DDown, DLeft, DRight };

struct VirtualPad {
  SDL_JoystickID id = 0;
  SDL_Joystick* js = nullptr;
  VirtualPad(const char* name, Uint16 vendor, Uint16 product) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 6;
    desc.nbuttons = 15;
    desc.name = name;
    desc.vendor_id = vendor;
    desc.product_id = product;
    id = SDL_AttachVirtualJoystick(&desc);
    if (id != 0) js = SDL_OpenJoystick(id);
  }
  ~VirtualPad() { detach(); }
  void detach() {
    if (js) SDL_CloseJoystick(js);
    js = nullptr;
    if (id != 0) SDL_DetachVirtualJoystick(id);
    id = 0;
  }
  void button(int b, bool down) { SDL_SetJoystickVirtualButton(js, b, down); }
};

QStringList glyphsOf(const QVariantMap& m) { return m.value(QStringLiteral("glyphs")).toStringList(); }

// Brightness above the chip and centroid (relative to the cell center) of the icon drawn in a PadGlyph.
struct Ink {
  double mass = 0;
  double dx = 0, dy = 0;
};
Ink inkOf(QQuickWindow* w, QQuickItem* cell) {
  const QImage img = w->grabWindow().convertToFormat(QImage::Format_RGB32);
  const double dpr = double(img.width()) / w->width();  // grabWindow returns device pixels
  QRectF r = cell->mapRectToScene(QRectF(0, 0, cell->width(), cell->height()));
  r = QRectF(r.topLeft() * dpr, r.size() * dpr);  // device pixels
  const QPointF c = r.center();
  const int inset = int(4 * dpr);  // stay inside the chip: the ring is not part of the icon
  Ink ink;
  double sx = 0, sy = 0;
  for (int y = int(r.top()) + inset; y < int(r.bottom()) - inset; ++y) {
    for (int x = int(r.left()) + inset; x < int(r.right()) - inset; ++x) {
      const double v = qGray(img.pixel(x, y)) - 0x27;  // chip #26272b
      if (v <= 0) continue;
      ink.mass += v;
      sx += v * (x + 0.5 - c.x());
      sy += v * (y + 0.5 - c.y());
    }
  }
  if (ink.mass > 0) {
    ink.dx = sx / ink.mass / dpr;  // item pixels
    ink.dy = sy / ink.mass / dpr;
    ink.mass /= dpr * dpr;
  }
  return ink;
}
}  // namespace

class ControllersGlyphsTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
  }
  static QString text(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it ? it->property("text").toString() : QString();
  }
  static QVariantMap deviceRow(Harness& h, const QString& kind, int nth = 0) {
    for (const QVariant& v : h.controller->controllers()->devices()) {
      if (v.toMap().value(QStringLiteral("kind")).toString() == kind && nth-- == 0) return v.toMap();
    }
    return {};
  }
  static QVariantMap inputRow(Harness& h, const QString& input) {
    for (const QVariant& v : h.controller->controllers()->rows()) {
      if (v.toMap().value(QStringLiteral("input")).toString() == input) return v.toMap();
    }
    return {};
  }
  // g of the first PadGlyph in the mapping field of an input (object name mapGlyphs_<input>).
  static QString fieldGlyph(Harness& h, const char* input) {
    QQuickItem* row = h.item((QByteArray("mapGlyphs_") + input).constData());
    if (!row) return QStringLiteral("(no chips)");
    const auto kids = row->childItems();
    for (QQuickItem* k : kids) {
      if (k->metaObject()->indexOfProperty("g") >= 0) return k->property("g").toString();
    }
    return QStringLiteral("(no chips)");
  }
  static QString tileGlyph(Harness& h, const char* id) {
    QQuickItem* t = h.item((QByteArray("testTile_") + id).constData());
    return t ? t->property("g").toString() : QStringLiteral("(no tile)");
  }
  static void pick(Harness& h, const char* selectName, const QString& value) {
    QQuickItem* sel = h.item(selectName);
    QVERIFY(sel != nullptr);
    QVERIFY(QMetaObject::invokeMethod(sel, "picked", Q_ARG(QString, value)));
  }
  bool sdlHeld_ = false;
  static QRectF sceneRect(QQuickItem* it) { return it->mapRectToScene(QRectF(0, 0, it->width(), it->height())); }

 private slots:
  void initTestCase() {
    uitest::installWarningCounter();
    // Keep SDL's reference count above zero for the whole run. Each Harness with gamepads makes the GamepadService
    // SDL_Init / SDL_QuitSubSystem; a full shutdown followed by a re-init crashed inside SDL's Windows joystick
    // drivers (second Harness in this process, MSVC CI). The Player itself initializes SDL once per process.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    sdlHeld_ = SDL_Init(SDL_INIT_GAMEPAD);
    QVERIFY2(sdlHeld_, SDL_GetError());
  }
  void cleanupTestCase() {
    if (sdlHeld_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    sdlHeld_ = false;
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // ------------------------------------------------------------------ pure glyph data

  void glyphSetsFollowTheButtonPositions() {
    const auto g = [](const char* set, const char* sdl) {
      return tokenGlyph(QLatin1String(set), padToken(QLatin1String(sdl)));
    };
    const auto glyph = [](const QVariantMap& m) { return m.value(QStringLiteral("glyph")).toString(); };
    const auto name = [](const QVariantMap& m) { return m.value(QStringLiteral("name")).toString(); };
    // SDL "b" = east button, "a" = south.
    QCOMPARE(glyph(g("xbox", "b")), QStringLiteral("B"));
    QCOMPARE(glyph(g("xbox", "a")), QStringLiteral("A"));
    QCOMPARE(glyph(g("xbox", "leftshoulder")), QStringLiteral("LB"));
    QCOMPARE(name(g("xbox", "leftshoulder")), QStringLiteral("Left bumper"));
    QCOMPARE(glyph(g("xbox", "back")), QStringLiteral("View"));
    QCOMPARE(glyph(g("xbox", "start")), QStringLiteral("Menu"));
    QCOMPARE(glyph(g("xbox", "righttrigger")), QStringLiteral("RT"));
    QCOMPARE(glyph(g("playstation", "a")), QStringLiteral("cross"));
    QCOMPARE(name(g("playstation", "a")), QStringLiteral("Cross"));
    QCOMPARE(glyph(g("playstation", "b")), QStringLiteral("circle"));
    QCOMPARE(glyph(g("playstation", "x")), QStringLiteral("square"));
    QCOMPARE(glyph(g("playstation", "y")), QStringLiteral("triangle"));
    QCOMPARE(glyph(g("playstation", "rightshoulder")), QStringLiteral("R1"));
    QCOMPARE(glyph(g("playstation", "lefttrigger")), QStringLiteral("L2"));
    QCOMPARE(glyph(g("playstation", "back")), QStringLiteral("Create"));
    QCOMPARE(glyph(g("playstation", "start")), QStringLiteral("Options"));
    // Generic = names of the emulated system from the built-in profile: east is DS A, south is DS B, north X, west Y.
    QCOMPARE(glyph(g("generic", "b")), QStringLiteral("A"));
    QCOMPARE(glyph(g("generic", "a")), QStringLiteral("B"));
    QCOMPARE(glyph(g("generic", "y")), QStringLiteral("X"));
    QCOMPARE(glyph(g("generic", "x")), QStringLiteral("Y"));
    QCOMPARE(glyph(g("generic", "leftshoulder")), QStringLiteral("L"));
    QCOMPARE(glyph(g("generic", "start")), QStringLiteral("Start"));
    QCOMPARE(glyph(g("generic", "back")), QStringLiteral("Select"));
    QCOMPARE(glyph(g("generic", "lefttrigger")), QStringLiteral("ZL"));  // trigger = 3DS ZL in the built-in profile
    // Directions are arrows in every set.
    for (const char* set : {"xbox", "playstation", "generic"}) {
      QCOMPARE(glyph(g(set, "dpup")), QStringLiteral("up"));
      QCOMPARE(glyph(g(set, "leftx+")), QStringLiteral("right"));
    }
  }

  void bindingsCollapseAndUnassigned() {
    const QStringList up{padToken(QStringLiteral("dpup")), padToken(QStringLiteral("lefty-"))};
    const QVariantMap d = describeBinding(QStringLiteral("xbox"), up);
    QCOMPARE(glyphsOf(d), QStringList{QStringLiteral("up")});
    QCOMPARE(d.value(QStringLiteral("name")).toString(), QStringLiteral("D-pad · or left stick"));
    const QVariantMap l = describeBinding(QStringLiteral("xbox"), {padToken(QStringLiteral("leftshoulder")), padToken(QStringLiteral("lefttrigger"))});
    QCOMPARE(glyphsOf(l), (QStringList{QStringLiteral("LB"), QStringLiteral("LT")}));
    QCOMPARE(describeBinding(QStringLiteral("xbox"), {}).value(QStringLiteral("name")).toString(), QStringLiteral("Unassigned"));
    QVERIFY(glyphsOf(describeBinding(QStringLiteral("xbox"), {})).isEmpty());
    // Keyboard: key names as chips, no extra name.
    const QVariantMap k = describeBinding(QStringLiteral("keyboard"), {keyToken(Qt::Key_X)});
    QCOMPARE(glyphsOf(k), QStringList{QStringLiteral("X")});
    QCOMPARE(k.value(QStringLiteral("name")).toString(), QString());
  }

  void inputTestCellsFormAController() {
    const QVariantList cells = inputTestCells(QStringLiteral("playstation"));
    QCOMPARE(cells.size(), 14);  // no stick cells (decisions.md aj)
    QSet<QString> ids;
    QList<QRectF> used;  // Select / Start have fractional columns so that the word fits the chip (P36)
    for (const QVariant& v : cells) {
      const QVariantMap c = v.toMap();
      ids.insert(c.value(QStringLiteral("id")).toString());
      const double col = c.value(QStringLiteral("col")).toDouble(), span = c.value(QStringLiteral("span")).toDouble();
      const int row = c.value(QStringLiteral("row")).toInt();
      QVERIFY(col >= 0 && col + span <= 7.001 && row >= 0 && row < 5);
      const QRectF r(col + 0.01, row, span - 0.02, 0.9);
      for (const QRectF& u : used) QVERIFY2(!u.intersects(r), "overlapping cells");
      used.append(r);
      if (c.value(QStringLiteral("id")).toString() == QLatin1String("select") || c.value(QStringLiteral("id")).toString() == QLatin1String("start")) {
        QVERIFY2(span >= 1.4, "Select / Start chips are wide enough for the word");
      }
    }
    QCOMPARE(ids.size(), 14);
    QVERIFY(ids.contains(QStringLiteral("a")) && ids.contains(QStringLiteral("up")) && ids.contains(QStringLiteral("zl")));
    const auto glyphOf = [&](const QString& set, const QString& id) {
      for (const QVariant& v : inputTestCells(set)) {
        if (v.toMap().value(QStringLiteral("id")).toString() == id) return v.toMap().value(QStringLiteral("glyph")).toString();
      }
      return QString();
    };
    QCOMPARE(glyphOf(QStringLiteral("xbox"), QStringLiteral("a")), QStringLiteral("B"));
    QCOMPARE(glyphOf(QStringLiteral("playstation"), QStringLiteral("a")), QStringLiteral("circle"));
    QCOMPARE(glyphOf(QStringLiteral("generic"), QStringLiteral("a")), QStringLiteral("A"));
    QCOMPARE(glyphOf(QStringLiteral("keyboard"), QStringLiteral("a")), QStringLiteral("A"));  // keyboard: DS names
    QCOMPARE(glyphOf(QStringLiteral("xbox"), QStringLiteral("zl")), QStringLiteral("LT"));
    QCOMPARE(glyphOf(QStringLiteral("playstation"), QStringLiteral("down")), QStringLiteral("down"));
  }

  // ------------------------------------------------------------------ persistence per physical device

  void overrideIsSavedPerDevice() {
    QTemporaryDir dir;
    const QString padA = QStringLiteral("030000004c050000e60c000000000000");
    const QString padB = QStringLiteral("03000000045e0000ea02000000000000");
    {
      ControllerProfiles p(dir.path());
      QCOMPARE(p.labelSet(padA), QString());  // Auto by default
      QVERIFY(p.setLabelSet(padA, QStringLiteral("xbox")));
      QVERIFY(p.setLabelSet(padB, QStringLiteral("generic")));
      QVERIFY(!p.setLabelSet(padA, QStringLiteral("nintendo")));  // unknown value
      QVERIFY(p.assign(padA, QString::fromLatin1(ControllerProfiles::kBuiltinGamepadId)));  // other data in the file is kept
    }
    {
      ControllerProfiles p(dir.path());
      QCOMPARE(p.labelSet(padA), QStringLiteral("xbox"));
      QCOMPARE(p.labelSet(padB), QStringLiteral("generic"));
      QCOMPARE(p.labelSet(QStringLiteral("other")), QString());
      QVERIFY(p.setLabelSet(padA, QString()));  // Auto again: the entry is removed
    }
    {
      ControllerProfiles p(dir.path());
      QCOMPARE(p.labelSet(padA), QString());
      QCOMPARE(p.labelSet(padB), QStringLiteral("generic"));
    }
    QFile f(ControllerProfiles(dir.path()).filePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    QCOMPARE(o.value(QStringLiteral("labelSets")).toObject().keys(), QStringList{padB});
    QVERIFY(!o.contains(QStringLiteral("secret")));
  }

  // ------------------------------------------------------------------ the page

  void autoFollowsDetectedTypeAndOverrideSwitchesSets() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    h.gamepads = true;
    QVERIFY(h.start());
    h.window->resize(1600, 900);   // the input test is a column from 1440 px on
    ControllersController* c = h.controller->controllers();
    QVERIFY2(c->gamepadAvailable(), qPrintable(c->gamepadNote()));
    pair(h, hub);
    VirtualPad ds("Test DualSense", 0x054c, 0x0ce6);
    VirtualPad xb("Test Xbox Pad", 0x045e, 0x02ea);
    QVERIFY(ds.js && xb.js);
    c->gamepads()->poll();
    h.controller->showControllers();
    QCOMPARE(h.controller->screen(), QStringLiteral("controllers"));
    const QVariantMap dsRow = deviceRow(h, QStringLiteral("gamepad"), 0);
    const QVariantMap xbRow = deviceRow(h, QStringLiteral("gamepad"), 1);
    QCOMPARE(dsRow.value(QStringLiteral("padType")).toString(), QStringLiteral("playstation"));
    QCOMPARE(xbRow.value(QStringLiteral("padType")).toString(), QStringLiteral("xbox"));

    // Keyboard keeps key names and has no Button labels select.
    c->selectDevice(QStringLiteral("keyboard"));
    QCOMPARE(c->labelSet(), QStringLiteral("keyboard"));
    QCOMPARE(glyphsOf(inputRow(h, QStringLiteral("a"))), QStringList{QStringLiteral("X")});
    QCOMPARE(fieldGlyph(h, "a"), QStringLiteral("X"));
    QVERIFY(!h.item("labelSetSelect")->isVisible());
    QVERIFY(!h.item("labelSetHelp")->isVisible());

    // DualSense: Auto (PlayStation).
    c->selectDevice(dsRow.value(QStringLiteral("key")).toString());
    QTRY_VERIFY(h.item("labelSetSelect")->isVisible());
    QCOMPARE(c->labelChoice(), QStringLiteral("auto"));
    QCOMPARE(c->labelSet(), QStringLiteral("playstation"));
    QCOMPARE(c->labelChoices().at(0).toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Auto (PlayStation)"));
    QCOMPARE(h.item("labelSetSelect")->property("displayText").toString(), QStringLiteral("Auto (PlayStation)"));
    QCOMPARE(text(h, "labelSetHelp"), QStringLiteral("Auto uses the detected controller type. Saved per device."));
    QCOMPARE(text(h, "testLabelsNote"), QStringLiteral("Labels: PlayStation"));
    QCOMPARE(fieldGlyph(h, "a"), QStringLiteral("circle"));   // NDS A on the east button = Circle
    QCOMPARE(fieldGlyph(h, "b"), QStringLiteral("cross"));
    QCOMPARE(fieldGlyph(h, "x"), QStringLiteral("triangle"));
    QCOMPARE(fieldGlyph(h, "y"), QStringLiteral("square"));
    QCOMPARE(fieldGlyph(h, "start"), QStringLiteral("Options"));
    QCOMPARE(fieldGlyph(h, "select"), QStringLiteral("Create"));
    QCOMPARE(text(h, "mapText_a"), QStringLiteral("Circle"));
    QCOMPARE(tileGlyph(h, "a"), QStringLiteral("circle"));
    QCOMPARE(tileGlyph(h, "b"), QStringLiteral("cross"));
    QCOMPARE(tileGlyph(h, "zl"), QStringLiteral("L2"));
    QCOMPARE(fieldGlyph(h, "up"), QStringLiteral("up"));
    QCOMPARE(text(h, "mapText_up"), QStringLiteral("D-pad"));

    // Override: Xbox.
    pick(h, "labelSetSelect", QStringLiteral("xbox"));
    QCOMPARE(c->labelChoice(), QStringLiteral("xbox"));
    QCOMPARE(c->labelSet(), QStringLiteral("xbox"));
    QCOMPARE(h.item("labelSetSelect")->property("displayText").toString(), QStringLiteral("Xbox"));
    QCOMPARE(text(h, "testLabelsNote"), QStringLiteral("Labels: Xbox"));
    QCOMPARE(fieldGlyph(h, "a"), QStringLiteral("B"));
    QCOMPARE(fieldGlyph(h, "start"), QStringLiteral("Menu"));
    QCOMPARE(text(h, "mapText_l"), QStringLiteral("Left bumper"));
    QCOMPARE(tileGlyph(h, "a"), QStringLiteral("B"));
    QCOMPARE(tileGlyph(h, "select"), QStringLiteral("View"));
    // Generic: DS names.
    pick(h, "labelSetSelect", QStringLiteral("generic"));
    QCOMPARE(fieldGlyph(h, "a"), QStringLiteral("A"));
    QCOMPARE(fieldGlyph(h, "select"), QStringLiteral("Select"));
    QCOMPARE(tileGlyph(h, "x"), QStringLiteral("X"));
    QCOMPARE(text(h, "testLabelsNote"), QStringLiteral("Labels: Generic"));

    // The Xbox pad is unaffected (own device, Auto = Xbox) and keeps its own state.
    c->selectDevice(xbRow.value(QStringLiteral("key")).toString());
    QCOMPARE(c->labelChoice(), QStringLiteral("auto"));
    QCOMPARE(h.item("labelSetSelect")->property("displayText").toString(), QStringLiteral("Auto (Xbox)"));
    QCOMPARE(fieldGlyph(h, "a"), QStringLiteral("B"));
    QCOMPARE(text(h, "mapText_l"), QStringLiteral("Left bumper"));

    // Saved per physical device (GUID), survives a restart of the store; Auto removes the override again.
    const QString dsGuid = h.controller->controllers()->gamepads()->devices().at(0).key;
    {
      ControllerProfiles reread(h.controller->profileStore()->baseDir());
      QCOMPARE(reread.labelSet(dsGuid), QStringLiteral("generic"));
      QCOMPARE(reread.labelSet(h.controller->controllers()->gamepads()->devices().at(1).key), QString());
    }
    c->selectDevice(dsRow.value(QStringLiteral("key")).toString());
    pick(h, "labelSetSelect", QStringLiteral("auto"));
    QCOMPARE(c->labelSet(), QStringLiteral("playstation"));
    QCOMPARE(fieldGlyph(h, "a"), QStringLiteral("circle"));
    QCOMPARE(ControllerProfiles(h.controller->profileStore()->baseDir()).labelSet(dsGuid), QString());

    // Input test follows the physical button: East lights NDS-A's cell, the glyph is Circle.
    ds.button(East, true);
    c->gamepads()->poll();
    QVERIFY(h.item("testTile_a")->property("active").toBool());
    QVERIFY(!h.item("testTile_b")->property("active").toBool());
    QCOMPARE(tileGlyph(h, "a"), QStringLiteral("circle"));
    ds.button(East, false);
    ds.button(LB, true);
    c->gamepads()->poll();
    QVERIFY(h.item("testTile_l")->property("active").toBool());
    ds.button(LB, false);
    c->gamepads()->poll();
    QVERIFY(!h.item("testTile_l")->property("active").toBool());
  }

  // Screenshots 3f-2 (Xbox) and 3f-3 (DualSense) at 1280 x 800, dark; only written with UZUME_SHOT_DIR.
  void screenshotsXboxAndDualSense() {
    if (qEnvironmentVariableIsEmpty("UZUME_SHOT_DIR")) QSKIP("UZUME_SHOT_DIR not set");
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    h.gamepads = true;
    QVERIFY(h.start());
    ControllersController* c = h.controller->controllers();
    pair(h, hub);
    h.controller->setAppearance(QStringLiteral("dark"));
    VirtualPad xb("Xbox Wireless Controller", 0x045e, 0x02ea);
    VirtualPad ds("DualSense Wireless Controller", 0x054c, 0x0ce6);
    QVERIFY(xb.js && ds.js);
    c->gamepads()->poll();
    h.controller->showControllers();
    h.window->resize(1280, 800);
    const auto shot = [&](VirtualPad& pad, int device, const QString& name, std::initializer_list<int> buttons) {
      c->selectDevice(deviceRow(h, QStringLiteral("gamepad"), device).value(QStringLiteral("key")).toString());
      for (int b : buttons) pad.button(b, true);
      c->gamepads()->poll();
      QTest::qWait(300);
      uitest::saveShot(h.window, name);
      for (int b : buttons) pad.button(b, false);
      c->gamepads()->poll();
    };
    shot(xb, 0, QStringLiteral("3f-2-xbox"), {South, DRight, RB});
    shot(ds, 1, QStringLiteral("3f-3-dualsense"), {South, LB});
  }

  void overrideSurvivesReplug() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    h.gamepads = true;
    QVERIFY(h.start());
    ControllersController* c = h.controller->controllers();
    pair(h, hub);
    h.controller->showControllers();
    {
      VirtualPad ds("Test DualSense", 0x054c, 0x0ce6);
      c->gamepads()->poll();
      c->selectDevice(deviceRow(h, QStringLiteral("gamepad")).value(QStringLiteral("key")).toString());
      c->setLabelChoice(QStringLiteral("xbox"));
      QCOMPARE(c->labelSet(), QStringLiteral("xbox"));
    }
    c->gamepads()->poll();
    QCOMPARE(c->selectedDevice(), QStringLiteral("keyboard"));
    VirtualPad again("Test DualSense", 0x054c, 0x0ce6);
    c->gamepads()->poll();
    c->selectDevice(deviceRow(h, QStringLiteral("gamepad")).value(QStringLiteral("key")).toString());
    QCOMPARE(c->labelChoice(), QStringLiteral("xbox"));
    // Keyboard and mouse ignore the choice.
    c->selectDevice(QStringLiteral("keyboard"));
    c->setLabelChoice(QStringLiteral("generic"));
    QCOMPARE(c->labelSet(), QStringLiteral("keyboard"));
  }

  void arrowsHaveEqualSizeWeightAndCenter() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    h.window->resize(1600, 900);   // the input test is a column from 1440 px on
    pair(h, hub);
    h.controller->showControllers();
    QTest::qWait(150);
    const char* names[] = {"testTile_up", "testTile_right", "testTile_down", "testTile_left"};
    QList<QQuickItem*> tiles, icons;
    for (const char* n : names) {
      QQuickItem* t = h.item(n);
      QVERIFY2(t != nullptr, n);
      tiles.append(t);
      QQuickItem* icon = Harness::findVisual(t, QStringLiteral("padGlyphIcon"));
      QVERIFY(icon != nullptr);
      icons.append(icon);
    }
    for (int i = 1; i < 4; ++i) {
      QCOMPARE(tiles[i]->width(), tiles[0]->width());
      QCOMPARE(tiles[i]->height(), tiles[0]->height());
      QCOMPARE(icons[i]->width(), icons[0]->width());
      QCOMPARE(icons[i]->height(), icons[0]->height());
    }
    QCOMPARE(tiles[0]->width(), 36.0);
    QCOMPARE(icons[0]->width(), double(qRound(36 * 0.62)));
    // Drawn pixels: same amount of ink and the same distance of the optical center from the cell center.
    QTest::qWait(500);
    QList<Ink> ink;
    for (QQuickItem* t : std::as_const(tiles)) ink.append(inkOf(h.window, t));
    QVERIFY2(ink[0].mass > 0, "arrow not drawn");
    for (int i = 1; i < 4; ++i) {
      QVERIFY2(qAbs(ink[i].mass - ink[0].mass) / ink[0].mass < 0.03, qPrintable(QStringLiteral("ink %1 vs %2").arg(ink[i].mass).arg(ink[0].mass)));
      const double d0 = std::hypot(ink[0].dx, ink[0].dy), di = std::hypot(ink[i].dx, ink[i].dy);
      QVERIFY2(qAbs(di - d0) < 0.5, qPrintable(QStringLiteral("center offset %1 vs %2").arg(di).arg(d0)));
    }
    // Opposite arrows are mirrored about the cell center.
    QVERIFY(qAbs(ink[0].dy + ink[2].dy) < 0.5);  // up / down
    QVERIFY(qAbs(ink[1].dx + ink[3].dx) < 0.5);  // right / left
  }

  void layoutInsideTheColumn() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    h.gamepads = true;
    QVERIFY(h.start());
    ControllersController* c = h.controller->controllers();
    pair(h, hub);
    VirtualPad ds("Test DualSense", 0x054c, 0x0ce6);
    c->gamepads()->poll();
    h.controller->showControllers();
    // Longest content: two chips (LB + LT) and the names of the PlayStation / Xbox sets.
    for (const int width : {1440, 960}) {
      for (const QString& set : {QStringLiteral("xbox"), QStringLiteral("playstation"), QStringLiteral("generic")}) {
        h.window->resize(width, 800);
        c->setLabelChoice(set);
        QTest::qWait(100);
        QQuickTest::qWaitForPolish(h.window);
        const QByteArray ctx = QStringLiteral("width %1, set %2").arg(width).arg(set).toLocal8Bit();
        QQuickItem* selects = h.item("headerSelects");
        QQuickItem* title = h.item("controllersTitle");
        QVERIFY2(selects && title, ctx.constData());
        QQuickItem* flick = selects->parentItem();
        while (flick && !flick->inherits("QQuickFlickable")) flick = flick->parentItem();
        QVERIFY2(flick != nullptr, ctx.constData());
        const QRectF column = sceneRect(flick);
        QVERIFY2(title->width() > 40, ctx.constData());
        QVERIFY2(sceneRect(selects).right() <= column.right() - 28 + 0.5, ctx.constData());
        QVERIFY2(sceneRect(selects).left() >= column.left() + 28 - 0.5, ctx.constData());
        for (const QVariant& v : c->rows()) {
          const QByteArray id = v.toMap().value(QStringLiteral("input")).toString().toLatin1();
          QQuickItem* row = h.item("mapRow_" + id);
          QQuickItem* label = h.item("mapLabel_" + id);
          QQuickItem* field = h.item("mapField_" + id);
          QQuickItem* action = h.item("mapAction_" + id);
          QVERIFY2(row && label && field && action, ctx.constData());
          const QRectF r = sceneRect(row), l = sceneRect(label), f = sceneRect(field), a = sceneRect(action);
          const QByteArray msg = ctx + " input " + id;
          QVERIFY2(r.right() <= column.right() - 28 + 0.5, msg.constData());
          QVERIFY2(l.left() >= r.left() + 14, msg.constData());
          QVERIFY2(l.width() > 20, msg.constData());
          QVERIFY2(l.right() <= f.left() + 0.5, msg.constData());
          QVERIFY2(f.right() <= a.left() + 0.5, msg.constData());
          QVERIFY2(a.right() <= r.right() + 0.5, msg.constData());
          QVERIFY2(f.width() <= 240.5 && f.width() >= 120, msg.constData());
          QCOMPARE(a.width() >= 56, true);
          // Chips and name stay inside the field.
          if (QQuickItem* chips = h.item("mapGlyphs_" + id); chips && chips->isVisible()) {
            QVERIFY2(sceneRect(chips).right() <= f.right() + 0.5, msg.constData());
            QVERIFY2(sceneRect(chips).left() >= f.left(), msg.constData());
          }
          QQuickItem* name = h.item("mapText_" + id);
          QVERIFY2(sceneRect(name).right() <= f.right() + 0.5, msg.constData());
        }
        // At 1440 px and wider the input test is a column; below that it is a disclosure (closed here, so no grid).
        QQuickItem* testGrid = h.item("inputTestGrid");
        if (width >= 1440) QVERIFY2(testGrid != nullptr && testGrid->isVisible(), ctx.constData());
        else QVERIFY2(testGrid == nullptr || !testGrid->isVisible(), ctx.constData());
      }
    }
    // Input test inside its panel at 1440.
    h.window->resize(1440, 800);
    QTest::qWait(100);
    QQuickItem* grid = h.item("inputTestGrid");
    QVERIFY(grid->isVisible());
    // Widen every text like a wide Windows font would (letter spacing), then let the layout settle.
    const auto widen = [&](qreal spacing) {
      for (QQuickItem* it : h.window->contentItem()->findChildren<QQuickItem*>()) {
        const int idx = it->metaObject()->indexOfProperty("font");
        if (idx < 0 || it->metaObject()->property(idx).userType() != QMetaType::QFont) continue;
        QFont f = it->property("font").value<QFont>();
        f.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
        it->setProperty("font", f);
      }
      QTest::qWait(100);
      QQuickTest::qWaitForPolish(h.window);
    };
    widen(qEnvironmentVariableIsSet("FB_TEST_LETTERSPACING") ? qEnvironmentVariable("FB_TEST_LETTERSPACING").toDouble() : 4.5);
    const QRectF panel = sceneRect(grid->parentItem());
    QString chain = QStringLiteral(" column:");
    for (QQuickItem* k : grid->parentItem()->childItems()) {
      chain += QStringLiteral(" [%1 x=%2 w=%3 iw=%4]").arg(QString::fromLatin1(k->metaObject()->className()))
                   .arg(k->x()).arg(k->width()).arg(k->implicitWidth());
    }
    chain += QStringLiteral(" row:");
    for (QQuickItem* p = grid->parentItem(); p && p->parentItem(); p = p->parentItem()) {
      if (qobject_cast<QQuickItem*>(p->parentItem()) && p->parentItem()->inherits("QQuickRowLayout")) {
        for (QQuickItem* sib : p->parentItem()->childItems()) {
          chain += QStringLiteral(" [%1 w=%2 iw=%3 minW=%4]").arg(QString::fromLatin1(sib->metaObject()->className()))
                       .arg(sib->width()).arg(sib->implicitWidth())
                       .arg(sib->property("Layout.minimumWidth").toString());
        }
        break;
      }
    }
    const QRectF g = sceneRect(grid);
    QCOMPARE(g.width(), 7 * 36.0 + 6 * 6.0);
    QCOMPARE(g.height(), 5 * 36.0 + 4 * 6.0);
    const QByteArray gridMsg = QStringLiteral("grid %1,%2 %3x%4 panel %5,%6 %7x%8 window %9")
                                   .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
                                   .arg(panel.x()).arg(panel.y()).arg(panel.width()).arg(panel.height())
                                   .arg(h.window->width()).toLocal8Bit() + chain.toLocal8Bit();
    QVERIFY2(g.left() >= panel.left() && g.right() <= panel.right(), gridMsg.constData());
    qInfo().noquote() << gridMsg;
    QVERIFY(qAbs((g.center().x()) - panel.center().x()) < 1.0);  // centered in the panel
    for (const QVariant& v : c->testCells()) {
      QQuickItem* tile = h.item(("testTile_" + v.toMap().value(QStringLiteral("id")).toString()).toLatin1().constData());
      QVERIFY(tile != nullptr);
      const QRectF t = sceneRect(tile);
      QVERIFY(t.left() >= g.left() - 0.5 && t.right() <= g.right() + 0.5 && t.top() >= g.top() - 0.5 && t.bottom() <= g.bottom() + 0.5);
    }
    // Wide column: the selects row sits below the title and the tabs (board 9a-1).
    h.window->resize(1600, 900);
    QTest::qWait(100);
    QQuickItem* selects = h.item("headerSelects");
    QQuickItem* title = h.item("controllersTitle");
    QVERIFY(sceneRect(selects).top() >= sceneRect(title).bottom());
    uitest::saveShot(h.window, QStringLiteral("5-controllers-glyphs"));
  }
};

UITEST_MAIN(ControllersGlyphsTest)
#include "controllers_glyphs_test.moc"
