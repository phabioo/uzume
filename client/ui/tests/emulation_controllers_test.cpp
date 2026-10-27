// Emulation page (3e), settings hierarchy at launch, Controllers page (3f) with SDL virtual joysticks.
// Offscreen, FakeHub, dummy bytes only. The tests with the real core QSKIP without UZUME_MELONDS_DS_CORE (ui_emulation_controllers_testcore runs them against the Uzume test core).
#include <SDL3/SDL.h>

#include <QQmlContext>
#include <QQmlProperty>
#include <QSignalSpy>
#include <QSemaphore>
#include <QThread>
#include <QtTest>
#include <atomic>

#include "controllerprofiles.h"
#include "corecache.h"
#include "core_options.h"
#include "coreprober.h"
#include "fakehub.h"
#include "profilestore.h"
#include "romcache.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;
using L = EmulationSettings::Level;

namespace {
constexpr quint32 kA = 1u << 8, kB = 1u << 0, kUp = 1u << 4;

emu::CoreOption option(const QString& key, const QString& desc, const QString& cat, const QStringList& values, const QString& def,
                       bool visible = true) {
  emu::CoreOption o;
  o.key = key;
  o.description = desc;
  o.info = desc + QStringLiteral(" (info)");
  o.categoryKey = cat;
  for (const QString& v : values) o.values.append({v, QString()});
  o.defaultValue = def;
  o.currentValue = def;
  o.visible = visible;
  return o;
}

// What a core like melonDS DS reports (V2 with categories): enough to exercise the page without the real core.
emu::CoreProbe fakeProbe() {
  emu::CoreProbe p;
  p.ok = true;
  p.info.name = QStringLiteral("melonDS DS");
  p.info.version = QStringLiteral("1.4.0");
  p.categories = {{QStringLiteral("audio"), QStringLiteral("Audio"), QString()}, {QStringLiteral("system"), QStringLiteral("System"), QString()}};
  p.options = {
      option(QStringLiteral("melonds_audio_interpolation"), QStringLiteral("Interpolation"), QStringLiteral("audio"),
             {QStringLiteral("disabled"), QStringLiteral("linear"), QStringLiteral("cosine")}, QStringLiteral("disabled")),
      option(QStringLiteral("melonds_boot_mode"), QStringLiteral("Boot Mode"), QStringLiteral("system"),
             {QStringLiteral("direct"), QStringLiteral("native")}, QStringLiteral("direct")),
      option(QStringLiteral("melonds_render_mode"), QStringLiteral("Render Mode"), QStringLiteral("system"),
             {QStringLiteral("software"), QStringLiteral("opengl")}, QStringLiteral("software")),
      option(QStringLiteral("melonds_opengl_resolution"), QStringLiteral("Internal Resolution"), QStringLiteral("system"),
             {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4")}, QStringLiteral("1"), false),
      option(QStringLiteral("melonds_screen_layout1"), QStringLiteral("Layout #1"), QStringLiteral("system"),
             {QStringLiteral("top-bottom"), QStringLiteral("left-right")}, QStringLiteral("top-bottom")),
      option(QStringLiteral("melonds_sysfile_mode"), QStringLiteral("BIOS/Firmware Mode"), QStringLiteral("system"),
             {QStringLiteral("native"), QStringLiteral("builtin")}, QStringLiteral("native")),
      option(QStringLiteral("melonds_hidden_by_core"), QStringLiteral("Hidden"), QStringLiteral("system"),
             {QStringLiteral("a"), QStringLiteral("b")}, QStringLiteral("a"), false),
  };
  return p;
}

// A core with many categories and every control type: toggle, segment, select (6 values), description with an option list.
emu::CoreProbe richProbe() {
  emu::CoreProbe p;
  p.ok = true;
  p.info.name = QStringLiteral("melonDS DS");
  p.info.version = QStringLiteral("1.4.0");
  const QStringList cats = {QStringLiteral("System"), QStringLiteral("Video"), QStringLiteral("Audio"), QStringLiteral("Screen"),
                            QStringLiteral("Firmware"), QStringLiteral("Network"), QStringLiteral("Date/Time"), QStringLiteral("Input Devices")};
  for (const QString& c : cats) p.categories.append({QString(c.toLower()).replace(QLatin1Char('/'), QLatin1Char('_')), c, QString()});
  p.options = {
      option(QStringLiteral("rich_boot"), QStringLiteral("Boot Mode"), QStringLiteral("system"), {QStringLiteral("direct"), QStringLiteral("native")}, QStringLiteral("direct")),
      option(QStringLiteral("rich_threaded"), QStringLiteral("Threaded Software Renderer"), QStringLiteral("video"),
             {QStringLiteral("disabled"), QStringLiteral("enabled")}, QStringLiteral("enabled")),
      option(QStringLiteral("rich_res"), QStringLiteral("Internal Resolution"), QStringLiteral("video"),
             {QStringLiteral("1x native (256 x 192)"), QStringLiteral("2x native (512 x 384)"), QStringLiteral("3x native (768 x 576)"),
              QStringLiteral("4x native (1024 x 768)"), QStringLiteral("5x native (1280 x 960)"), QStringLiteral("6x native (1536 x 1152)")},
             QStringLiteral("1x native (256 x 192)")),
      option(QStringLiteral("rich_interp"), QStringLiteral("Interpolation"), QStringLiteral("audio"),
             {QStringLiteral("None"), QStringLiteral("Linear"), QStringLiteral("Cosine"), QStringLiteral("Cubic")}, QStringLiteral("None")),
      option(QStringLiteral("rich_mic"), QStringLiteral("Microphone Input Mode"), QStringLiteral("audio"),
             {QStringLiteral("silence"), QStringLiteral("noise"), QStringLiteral("host")}, QStringLiteral("silence")),
      option(QStringLiteral("rich_screen"), QStringLiteral("Screen Gap"), QStringLiteral("screen"), {QStringLiteral("0"), QStringLiteral("1")}, QStringLiteral("0")),
      option(QStringLiteral("rich_fw"), QStringLiteral("Firmware Source"), QStringLiteral("firmware"), {QStringLiteral("a"), QStringLiteral("b")}, QStringLiteral("a")),
      option(QStringLiteral("rich_net"), QStringLiteral("Network Mode"), QStringLiteral("network"), {QStringLiteral("a"), QStringLiteral("b")}, QStringLiteral("a")),
      option(QStringLiteral("rich_time"), QStringLiteral("Time Mode"), QStringLiteral("date_time"), {QStringLiteral("a"), QStringLiteral("b")}, QStringLiteral("a")),
      option(QStringLiteral("rich_input"), QStringLiteral("Cursor Mode"), QStringLiteral("input devices"), {QStringLiteral("a"), QStringLiteral("b")}, QStringLiteral("a")),
  };
  p.options[4].info = QStringLiteral("Select the microphone input.\n- Silence: No input.\n- White noise: Random noise.\n- Host: Use the host microphone.");
  return p;
}

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
                                  {QStringLiteral("preferred_core_id"), QStringLiteral("melondsds")},
                                  {QStringLiteral("expected_core_version"), QStringLiteral("1.4.0")},
                                  {QStringLiteral("firmware_mode"), mode},
                                  {QStringLiteral("firmware"), files}}}}};
}

// Probe stand-in for the CoreProber tests: counts calls, notes whether one ran on the GUI thread and waits for the test.
std::atomic<int> g_probeCalls{0};
std::atomic<bool> g_probeOnGuiThread{false};
QSemaphore g_probeGate;
emu::CoreProbe gatedProbe(const QString&, const QString&, const QString&) {
  ++g_probeCalls;
  if (QThread::currentThread() == QCoreApplication::instance()->thread()) g_probeOnGuiThread = true;
  g_probeGate.acquire();
  emu::CoreProbe p = fakeProbe();
  p.info.version = QStringLiteral("9.9.9");
  return p;
}

// Releases the probe gate when a test leaves early, so a blocked worker never hangs the teardown.
struct ProbeGateRelease {
  ~ProbeGateRelease() { g_probeGate.release(16); }
};

// SDL virtual gamepad (default mapping: buttons a b x y back guide start ls rs lb rb dpup dpdown dpleft dpright).
enum Btn { South = 0, East, West, North, Back, Guide, Start, LS, RS, LB, RB, DUp, DDown, DLeft, DRight };
struct VirtualPad {
  SDL_JoystickID id = 0;
  SDL_Joystick* js = nullptr;
  explicit VirtualPad(const char* name) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 6;
    desc.nbuttons = 15;
    desc.name = name;
    desc.vendor_id = 0x1209;
    desc.product_id = 0x5100;
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
  void axis(int a, int v) { SDL_SetJoystickVirtualAxis(js, a, static_cast<Sint16>(v)); }   // axes: leftx lefty rightx righty lefttrigger righttrigger
};
}  // namespace

class EmulationControllersTest : public QObject {
  Q_OBJECT

  QByteArray savedCoreEnv_;

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
  }
  // One line per item for the CI log: class, objectName, width, implicitWidth, attached Layout.min/pref/max.
  static QString layoutDump(QQuickItem* it, int depth) {
    const auto lay = [it](const char* p) {
      QQmlContext* ctx = qmlContext(it);
      const QVariant v = ctx ? QQmlProperty(it, QString::fromLatin1(p), ctx).read() : QVariant();
      return v.isValid() ? QString::number(v.toDouble()) : QStringLiteral("?");
    };
    QString s = QStringLiteral("\n%1%2 '%3' w=%4 impl=%5 min=%6 pref=%7 max=%8")
                    .arg(QString(depth * 2, QLatin1Char(' ')), QString::fromLatin1(it->metaObject()->className()), it->objectName())
                    .arg(it->width()).arg(it->implicitWidth())
                    .arg(lay("Layout.minimumWidth"), lay("Layout.preferredWidth"), lay("Layout.maximumWidth"));
    return s;
  }
  static QString hotkeysLayoutDump(Harness& h) {
    QQuickItem* info = h.item("hotkeysInfo");
    if (!info || !info->parentItem()) return QStringLiteral("(no hotkeysInfo)");
    QQuickItem* table = info->parentItem();
    QString s = QStringLiteral("Hotkeys layout:");
    if (table->parentItem()) {
      s += layoutDump(table->parentItem(), 0);  // content
      for (QQuickItem* sibling : table->parentItem()->childItems()) {
        if (sibling != table && sibling->isVisible()) s += layoutDump(sibling, 1);
      }
    }
    s += layoutDump(table, 1);
    for (QQuickItem* child : table->childItems()) {
      s += layoutDump(child, 2);
      for (QQuickItem* rowBox : child->childItems()) {  // delegate column: row layout, conflict line, divider
        s += layoutDump(rowBox, 3);
        if (rowBox->objectName().isEmpty() && rowBox->inherits("QQuickRowLayout")) {
          for (QQuickItem* cell : rowBox->childItems()) {
            s += layoutDump(cell, 4);
            for (QQuickItem* inner : cell->childItems()) s += layoutDump(inner, 5);
          }
        }
      }
    }
    return s;
  }
  static QString text(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it ? it->property("text").toString() : QString();
  }
  static bool visible(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr && it->isVisible();
  }
  static QVariantMap deviceRow(Harness& h, const QString& kind) {
    for (const QVariant& v : h.controller->controllers()->devices()) {
      if (v.toMap().value(QStringLiteral("kind")).toString() == kind) return v.toMap();
    }
    return {};
  }
  static QVariantMap inputRow(Harness& h, const QString& input) {
    for (const QVariant& v : h.controller->controllers()->rows()) {
      if (v.toMap().value(QStringLiteral("input")).toString() == input) return v.toMap();
    }
    return {};
  }
  static void pick(Harness& h, const char* selectName, const QString& value) {
    QQuickItem* sel = h.item(selectName);
    QVERIFY(sel != nullptr);
    QVERIFY(QMetaObject::invokeMethod(sel, "picked", Q_ARG(QString, value)));
  }

  // A hub with the system nds (two cores) and one game "g1" (dummy file, no real ROM), paired and ready.
  static void ndsGameHub(FakeHub& hub) {
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("firmware_v1"), QStringLiteral("cores_v1"), QStringLiteral("cores_v2")};
    const auto core = [](const QString& id, const QString& name, const QString& date) {
      return QJsonObject{{QStringLiteral("core_id"), id},   {QStringLiteral("display_name"), name},
                         {QStringLiteral("version"), date},  {QStringLiteral("license"), QStringLiteral("GPLv3")},
                         {QStringLiteral("experimental"), false}, {QStringLiteral("origin"), QStringLiteral("libretro-buildbot")},
                         {QStringLiteral("build_date"), date}};
    };
    hub.systems = {{QStringLiteral("systems"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("nds")},
                                           {QStringLiteral("display_name"), QStringLiteral("Nintendo DS")},
                                           {QStringLiteral("preferred_core_id"), QStringLiteral("melondsds")},
                                           {QStringLiteral("default_core_id"), QStringLiteral("melondsds")},
                                           {QStringLiteral("core_package_version"), QStringLiteral("2026.10.09")},
                                           {QStringLiteral("cores"), QJsonArray{core(QStringLiteral("melondsds"), QStringLiteral("melonDS DS"), QStringLiteral("2026.10.09")),
                                                                                core(QStringLiteral("desmume"), QStringLiteral("DeSmuME"), QStringLiteral("2026.10.08"))}},
                                           {QStringLiteral("firmware_mode"), QStringLiteral("builtin")},
                                           {QStringLiteral("firmware"), QJsonArray{}}}}}};
    const QByteArray rom = QByteArray(2048, 'q') + "UZUME-DUMMY";
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    hub.games = QJsonObject{{QStringLiteral("games"), QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"),
                                                                                    uitest::sha256Hex(rom), rom.size())}}};
  }
  static QVariantMap gameRow(const QVariantMap& data, const QString& key) {
    for (const QVariant& t : data.value(QStringLiteral("tabs")).toList()) {
      for (const QVariant& r : t.toMap().value(QStringLiteral("rows")).toList()) {
        if (r.toMap().value(QStringLiteral("key")).toString() == key) return r.toMap();
      }
    }
    return {};
  }
  static QVariantMap gameTab(const QVariantMap& data, const QString& id) {
    for (const QVariant& t : data.value(QStringLiteral("tabs")).toList()) {
      if (t.toMap().value(QStringLiteral("id")).toString() == id) return t.toMap();
    }
    return {};
  }

 private slots:
  // 0.20 final live check: the rewind lengths reflect the buffer cap and say what they really cost.
  void rewindLengthsFitTheBufferCap() {
    const QList<emu::CoreOptionValue> all = {{"0", "Off"}, {"10", "10 seconds"}, {"30", "30 seconds"}, {"60", "60 seconds"}, {"120", "2 minutes"}};
    // DS: 7.5 MB per state, so the 1.5 GB cap holds about 13 seconds: longer steps are replaced by the real maximum.
    const QList<emu::CoreOptionValue> nds = EmulationController::rewindValues(all, QStringLiteral("nds"));
    QStringList values;
    for (const auto& v : nds) values << v.value;
    QCOMPARE(values, (QStringList{"0", "10", "13"}));
    QCOMPARE(nds.value(0).label, QStringLiteral("Off"));
    QCOMPARE(nds.value(1).label, QStringLiteral("10 seconds · 1.1 GB · High memory"));
    QCOMPARE(nds.value(2).label, QStringLiteral("13 seconds · 1.5 GB · High memory"));
    for (const auto& v : nds) QVERIFY2(!v.label.contains(QStringLiteral("about")), qPrintable(v.label));
    // GBA: everything fits; "High memory" only where it is true (above half a GB).
    const QList<emu::CoreOptionValue> gba = EmulationController::rewindValues(all, QStringLiteral("gba"));
    QCOMPARE(gba.size(), all.size());
    QVERIFY2(!gba.value(1).label.contains(QStringLiteral("High memory")), qPrintable(gba.value(1).label));
    QVERIFY2(!gba.value(3).label.contains(QStringLiteral("High memory")), qPrintable(gba.value(3).label));
    QVERIFY2(gba.value(4).label.contains(QStringLiteral("High memory")), qPrintable(gba.value(4).label));
    // Unknown state size (3DS, unknown systems): the list stays as it is.
    QCOMPARE(EmulationController::rewindValues(all, QStringLiteral("3ds")).size(), all.size());
  }

  void initTestCase() {
    savedCoreEnv_ = qgetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() {
    uitest::warningCount() = 0;
    // "Found" core that is never loaded (the options are injected); the real-core tests set the real path themselves.
    qputenv("UZUME_MELONDS_DS_CORE", QCoreApplication::applicationFilePath().toLocal8Bit());
  }
  void cleanup() {
    CoreProber::setProbeFunctionForTest(nullptr);
    g_probeGate.acquire(g_probeGate.available());
    QCOMPARE(uitest::warningCount().load(), 0);
    if (savedCoreEnv_.isEmpty()) qunsetenv("UZUME_MELONDS_DS_CORE"); else qputenv("UZUME_MELONDS_DS_CORE", savedCoreEnv_);
  }

  // ------------------------------------------------------------------ Core probe off the GUI thread

  // The startup probe (handshake core list) and the Emulation page share one probe per library; it runs on a worker, the
  // page shows a loading row meanwhile, and the handshake core list follows when the probe is done.
  void coreProbeRunsOffTheGuiThread() {
    g_probeCalls = 0;
    g_probeOnGuiThread = false;
    CoreProber::setProbeFunctionForTest(&gatedProbe);
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    ProbeGateRelease gateGuard;  // destroyed before the Harness
    QVERIFY(h.start(true));  // queues the startup probe; it blocks on the gate
    pair(h, hub);  // the GUI thread stays responsive while the probe is stuck
    h.controller->showEmulation();
    QTRY_VERIFY_WITH_TIMEOUT(g_probeCalls.load() >= 1, 3000);
    QVERIFY(visible(h, "coreProbingNote"));
    QVERIFY(h.controller->emulation()->coreProbing());
    QVERIFY(!h.controller->emulation()->hasCoreProbe(QStringLiteral("melondsds")));
    QTest::qWait(150);
    QCOMPARE(g_probeCalls.load(), 1);  // refreshes on the page do not start the same library twice
    g_probeGate.release();
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->emulation()->hasCoreProbe(QStringLiteral("melondsds")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!visible(h, "coreProbingNote"), 3000);
    QVERIFY(!h.controller->emulation()->coreProbing());
    QVERIFY(h.item("optionRow_melonds_audio_interpolation") != nullptr);
    QCOMPARE(g_probeCalls.load(), 1);
    QVERIFY(!g_probeOnGuiThread.load());
    // The handshake core list is sent after the probe (version from the core info).
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
          for (const QJsonValue& v : hub.lastHandshakeBody.value(QStringLiteral("cores")).toArray()) {
            if (v.toObject().value(QStringLiteral("id")).toString() == QLatin1String("melondsds")) {
              return v.toObject().value(QStringLiteral("version")).toString() == QLatin1String("9.9.9");
            }
          }
          return false;
        }(),
        5000);
  }

  // The prober serves one library once, one at a time, and tells every requester.
  void coreProberDeduplicatesAndSerializes() {
    g_probeCalls = 0;
    CoreProber::setProbeFunctionForTest(&gatedProbe);
    CoreProber prober;
    ProbeGateRelease gateGuard;  // destroyed before the prober
    QStringList got;
    connect(&prober, &CoreProber::probed, &prober, [&got](const QString& id, const QString&, const emu::CoreProbe& p) { got << id + QLatin1Char(':') + p.info.version; });
    QSignalSpy idle(&prober, &CoreProber::idle);
    QVERIFY(prober.request(QStringLiteral("a"), QStringLiteral("/x/a.so"), QString(), QString()));
    QVERIFY(!prober.request(QStringLiteral("a2"), QStringLiteral("/x/a.so"), QString(), QString()));  // same library: shared
    QVERIFY(prober.request(QStringLiteral("b"), QStringLiteral("/x/b.so"), QString(), QString()));
    QVERIFY(prober.busy());
    QTRY_COMPARE_WITH_TIMEOUT(g_probeCalls.load(), 1, 3000);
    QTest::qWait(100);
    QCOMPARE(g_probeCalls.load(), 1);  // b waits: one core at a time
    g_probeGate.release(2);
    QTRY_COMPARE_WITH_TIMEOUT(idle.count(), 1, 5000);
    QCOMPARE(g_probeCalls.load(), 2);
    QCOMPARE(got, (QStringList{"a:9.9.9", "a2:9.9.9", "b:9.9.9"}));
    QVERIFY(!prober.busy());
  }

  // ------------------------------------------------------------------ Settings sections (one page each)

  // Emulation, Controls and Hotkeys are sections of Settings; every entry point lands on its section with the other pages hidden,
  // and the sidebar no longer lists Emulation or Controllers.
  void entryPointsOpenTheirSettingsSection() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);

    // The sidebar: Library and Runs, nothing else in PLAY.
    h.controller->showLibrary();
    QTRY_VERIFY(visible(h, "navLibrary"));
    QVERIFY(visible(h, "navRuns"));
    QVERIFY(h.item("navEmulation") == nullptr);
    QVERIFY(h.item("navControllers") == nullptr);

    static const QList<QPair<QByteArray, QByteArray>> pages{
        {"account", "accountSection"},         {"appearance", "appearanceSection"}, {"audio", "audioSection"},
        {"controllers", "controllersSection"}, {"hotkeys", "hotkeysSection"},       {"emulation", "emulationSection"},
        {"storage", "storageSection"},         {"hubs", "hubsSection"},             {"plugins", "pluginsSection"},
        {"updates", "updatesSection"},         {"diagnostics", "diagnosticsSection"}};
    const auto expectOnly = [&](const QByteArray& id) {
      QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
      QTRY_VERIFY2(h.item("section_" + id) != nullptr && h.item("section_" + id)->property("selected").toBool(), id.constData());
      for (const auto& pg : pages) {
        if (pg.first == id) {
          QTRY_VERIFY2(visible(h, pg.second.constData()), pg.second.constData());
        } else {
          QVERIFY2(!visible(h, pg.second.constData()), pg.second.constData());
        }
      }
      QCOMPARE(h.item("settingsScroll")->property("contentY").toReal(), 0.0);   // a section starts at the top
    };
    h.controller->showEmulation();
    expectOnly("emulation");
    QVERIFY(visible(h, "emulationTabs"));
    QCOMPARE(text(h, "settingsTitle"), QStringLiteral("Emulation"));
    h.controller->showControllers();
    expectOnly("controllers");
    QVERIFY(visible(h, "controllerTabs"));
    QCOMPARE(text(h, "settingsTitle"), QStringLiteral("Controls"));
    h.controller->showSettingsSection(QStringLiteral("hotkeys"));
    expectOnly("hotkeys");
    QVERIFY(visible(h, "hotkeysInfo"));
    QCOMPARE(text(h, "settingsTitle"), QStringLiteral("Hotkeys"));
    h.item("settingsScroll")->setProperty("contentY", 40);
    h.controller->showSettingsSection(QStringLiteral("library"));   // the old id of the folders: now one page with the storage
    expectOnly("storage");
    QVERIFY(visible(h, "libraryFoldersSection"));
    QCOMPARE(text(h, "settingsTitle"), QStringLiteral("Folders & storage"));
    for (const char* id : {"account", "appearance", "audio", "hubs", "plugins", "updates", "diagnostics"}) {
      h.controller->showSettingsSection(QString::fromLatin1(id));
      expectOnly(id);
    }

    // Ctrl+3 / Ctrl+4 open Emulation / Controls in Settings, Ctrl+, Settings.
    h.controller->showLibrary();
    QTRY_VERIFY(visible(h, "navLibrary"));
    QTest::keyClick(h.window, Qt::Key_3, Qt::ControlModifier);
    expectOnly("emulation");
    QTest::keyClick(h.window, Qt::Key_4, Qt::ControlModifier);
    expectOnly("controllers");
    // The sidebar of Settings has no Emulation or Controllers item either.
    QVERIFY(h.item("navEmulation") == nullptr && h.item("navControllers") == nullptr);
  }

  // ------------------------------------------------------------------ Emulation (3e)

  void emulationPageListsOnlyReportedOptions() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);
    h.controller->showEmulation();   // a section of Settings
    QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
    QTRY_VERIFY(h.item("emulationTabs") != nullptr && h.item("emulationTabs")->isVisible());
    EmulationController* emu = h.controller->emulation();

    // System item: name, core + version, readiness, firmware (no firmware_v1 on this hub -> built-in BIOS).
    QCOMPARE(text(h, "systemCore_nds"), QStringLiteral("melonDS DS · 1.4.0"));
    QCOMPARE(text(h, "systemReady_nds"), QStringLiteral("Ready · included in Uzume"));
    QCOMPARE(text(h, "systemFirmware_nds"), QStringLiteral("Built-in BIOS"));
    QCOMPARE(text(h, "settingsTitle"), QStringLiteral("Emulation"));
    QCOMPARE(h.item("emulationTabs")->property("active").toString(), QStringLiteral("NDS"));
    QVERIFY(!emu->defaultsSelected());

    // The system shows the core's visible, non-locked options; nothing else, and no Uzume options (those are Defaults).
    QVERIFY(visible(h, "optionGroup_core"));
    QVERIFY(h.item("optionSelect_uzume.default_multiview") == nullptr);  // display options are global only (speed-up options are offered per system)
    QVERIFY(h.item("optionRow_melonds_audio_interpolation") != nullptr);
    QVERIFY(h.item("optionRow_melonds_boot_mode") != nullptr);
    for (const char* hidden : {"optionRow_melonds_screen_layout1", "optionRow_melonds_sysfile_mode",
                               "optionRow_melonds_hidden_by_core"}) {
      QVERIFY2(h.item(hidden) == nullptr, hidden);  // locked by Uzume / hidden by the core: not user-editable
    }
    QVERIFY(h.item("optionRow_melonds_render_mode") != nullptr);  // user choice since 0.5
    QVERIFY(h.item("optionRow_melonds_opengl_resolution") != nullptr);  // hidden by the core in software mode, the manifest shows it anyway
    QCOMPARE(emu->lockedCount(), 2);
    QVERIFY(visible(h, "lockedHint"));
    // Category chips: the categories of this scope.
    QCOMPARE(emu->categories(), (QStringList{QStringLiteral("Audio"), QStringLiteral("System"), QStringLiteral("Picture")}));  // + Uzume's picture options (the rewind rows have no chip, like speed-up)
    QTRY_VERIFY(visible(h, "category_Audio") && visible(h, "category_System"));  // the tabs fit once the new section is laid out
    // Nothing changed yet: "Default" in every row, no dot, no "Reset N changed".
    QCOMPARE(text(h, "optionOrigin_melonds_audio_interpolation"), QStringLiteral("Default"));
    QVERIFY(!visible(h, "changedDot_melonds_audio_interpolation"));
    QVERIFY(!visible(h, "resetAllChanged"));
    QVERIFY(!visible(h, "restartBadge_melonds_audio_interpolation"));
    uitest::saveShot(h.window, QStringLiteral("5-emulation"));

    // Category chip and search narrow the list.
    QVERIFY(h.click("category_Audio"));
    QVERIFY(h.item("optionRow_melonds_boot_mode") == nullptr);
    QVERIFY(h.item("optionRow_melonds_audio_interpolation") != nullptr);
    QVERIFY(h.click("categoryAll"));
    QVERIFY(h.item("optionRow_melonds_boot_mode") != nullptr);
    emu->setSearchText(QStringLiteral("boot"));
    QVERIFY(h.item("optionRow_melonds_audio_interpolation") == nullptr);
    QVERIFY(h.item("optionRow_melonds_boot_mode") != nullptr);
    emu->setSearchText(QString());

    // Set a value: stored as an explicit override with the changed dot, "N changed" and Reset; reset removes the key again.
    pick(h, "optionSelect_melonds_audio_interpolation", QStringLiteral("cosine"));
    QCOMPARE(emu->settings()->value(L::System, QStringLiteral("nds"), QStringLiteral("melonds_audio_interpolation")), QStringLiteral("cosine"));
    QVERIFY(visible(h, "changedDot_melonds_audio_interpolation"));
    QVERIFY(visible(h, "optionReset_melonds_audio_interpolation"));
    QCOMPARE(emu->changedCount(), 1);
    QCOMPARE(text(h, "resetAllChanged"), QStringLiteral("Reset all to default"));
    QCOMPARE(text(h, "tabCountText_NDS"), QStringLiteral("1"));
    QCOMPARE(emu->launchOverrides(QStringLiteral("nds"), QStringLiteral("g1")).value(QStringLiteral("melonds_audio_interpolation")),
             QStringLiteral("cosine"));
    {
      QFile f(emu->settings()->filePath());
      QVERIFY(f.open(QIODevice::ReadOnly));
      const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
      // Only the explicit override is stored (no complete copy of the configuration).
      QCOMPARE(o.value(QStringLiteral("systems")).toObject().value(QStringLiteral("nds")).toObject().value(QStringLiteral("options")).toObject().size(), 1);
      QVERIFY(!o.contains(QStringLiteral("global")));
    }
    // An invalid value and a locked key are refused.
    emu->setOption(QStringLiteral("melonds_audio_interpolation"), QStringLiteral("nonsense"));
    emu->setOption(QStringLiteral("melonds_screen_layout1"), QStringLiteral("left-right"));
    QCOMPARE(emu->settings()->values(L::System, QStringLiteral("nds")).size(), 1);
    QVERIFY(h.click("optionReset_melonds_audio_interpolation"));
    QVERIFY(!emu->settings()->hasValue(L::System, QStringLiteral("nds"), QStringLiteral("melonds_audio_interpolation")));
    QCOMPARE(text(h, "optionOrigin_melonds_audio_interpolation"), QStringLiteral("Default"));
    // Picking the default again stores nothing.
    emu->setOption(QStringLiteral("melonds_audio_interpolation"), QStringLiteral("cosine"));
    emu->setOption(QStringLiteral("melonds_audio_interpolation"), QStringLiteral("disabled"));
    QVERIFY(!emu->settings()->hasValue(L::System, QStringLiteral("nds"), QStringLiteral("melonds_audio_interpolation")));
    // "Reset N changed" resets every explicit value of the scope.
    emu->setOption(QStringLiteral("melonds_audio_interpolation"), QStringLiteral("linear"));
    emu->setOption(QStringLiteral("melonds_boot_mode"), QStringLiteral("native"));
    QCOMPARE(emu->changedCount(), 2);
    QVERIFY(h.click("resetAllChanged"));
    QCOMPARE(emu->changedCount(), 0);
    QCOMPARE(emu->settings()->values(L::System, QStringLiteral("nds")).size(), 0);

    // "applies on next start" badge on core options only while a game runs; the hint appears once such an option changed.
    emu->setGameRunning(true);
    QVERIFY(visible(h, "restartBadge_melonds_audio_interpolation"));
    QVERIFY(visible(h, "gameRunningNote"));
    QVERIFY(!visible(h, "restartHint"));
    emu->setOption(QStringLiteral("melonds_audio_interpolation"), QStringLiteral("linear"));
    QVERIFY(visible(h, "restartHint"));
    emu->setGameRunning(false);
    QVERIFY(!visible(h, "restartBadge_melonds_audio_interpolation"));
    QVERIFY(!visible(h, "restartHint"));
  }

  // ------------------------------------------------------------------ SettingsRow on the Emulation and Settings pages

  static QList<QQuickItem*> settingsRows(QQuickItem* root) {
    QList<QQuickItem*> out;
    for (QQuickItem* c : root->childItems()) {
      if (c->property("controlColumn").isValid() && c->isVisible()) out.append(c);
      out.append(settingsRows(c));
    }
    return out;
  }
  static QRectF sceneRect(QQuickItem* it) { return it->mapRectToScene(QRectF(0, 0, it->width(), it->height())); }
  static QQuickItem* rowItem(QQuickItem* row, const char* prop) { return row->property(prop).value<QQuickItem*>(); }
  // Every element of every row lies inside the visible column (Windows layout rule: nothing widens a column).
  static void verifyRowsInside(Harness& h, const char* scrollName, int windowWidth) {
    QQuickItem* flick = h.item(scrollName);
    QVERIFY(flick != nullptr);
    const QRectF col = sceneRect(flick);
    const QList<QQuickItem*> rows = settingsRows(flick->property("contentItem").value<QQuickItem*>());
    QVERIFY(!rows.isEmpty());
    for (QQuickItem* row : rows) {
      for (const char* prop : {"controlColumn", "resetColumn", "textColumn", "labelItem"}) {
        QQuickItem* it = rowItem(row, prop);
        QVERIFY(it != nullptr);
        const QRectF r = sceneRect(it);
        QVERIFY2(r.left() >= col.left() - 0.5 && r.right() <= col.right() + 0.5,
                 qPrintable(QStringLiteral("%1.%2 at %3 px: %4..%5 outside the column %6..%7")
                                .arg(row->objectName(), QString::fromLatin1(prop)).arg(windowWidth).arg(r.left()).arg(r.right()).arg(col.left()).arg(col.right())));
      }
      QQuickItem* ctl = rowItem(row, "controlLoader")->property("item").value<QQuickItem*>();
      if (ctl != nullptr) {
        const QRectF r = sceneRect(ctl);
        QVERIFY2(r.left() >= col.left() - 0.5 && r.right() <= col.right() + 0.5, qPrintable(row->objectName()));
      }
    }
  }
  static qreal controlRight(QQuickItem* row) { return sceneRect(rowItem(row, "controlColumn")).right(); }

  void emulationRowsAlignAndStayInsideTheColumn() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), richProbe(), false);
    h.controller->showEmulation();
    QTest::qWait(100);
    qreal emuRight = 0;
    for (const int w : {1280, 1440, 960}) {
      h.window->resize(w, 800);
      QTest::qWait(120);
      QQuickTest::qWaitForPolish(h.window);
      QQuickItem* boot = h.item("optionRow_rich_boot");        // segment
      QQuickItem* thr = h.item("optionRow_rich_threaded");     // toggle
      QQuickItem* res = h.item("optionRow_rich_res");          // select
      QQuickItem* interp = h.item("optionRow_rich_interp");    // segment, 4 options
      QVERIFY(boot && thr && res && interp);
      QCOMPARE(boot->property("kind").toString(), QStringLiteral("segment"));
      QCOMPARE(thr->property("kind").toString(), QStringLiteral("toggle"));
      QCOMPARE(res->property("kind").toString(), QStringLiteral("select"));
      verifyRowsInside(h, "settingsScroll", w);
      if (w >= 1280) {
        QCOMPARE(controlRight(thr), controlRight(boot));
        QCOMPARE(controlRight(res), controlRight(boot));
        QCOMPARE(controlRight(interp), controlRight(boot));
        QCOMPARE(sceneRect(h.item("optionSelect_rich_boot")).right(), controlRight(boot));
        QCOMPARE(sceneRect(h.item("optionSelect_rich_res")).left(), sceneRect(h.item("optionSelect_rich_interp")).left());
        QCOMPARE(sceneRect(h.item("optionSelect_rich_res")).width(), 280.0);
        QCOMPARE(sceneRect(h.item("optionSelect_rich_res")).height(), 32.0);
        QCOMPARE(sceneRect(h.item("optionToggle_rich_threaded")).right(), controlRight(boot));
        // The changed dot never moves the label.
        const qreal labelX = sceneRect(rowItem(boot, "labelItem")).left();
        QCOMPARE(sceneRect(rowItem(thr, "labelItem")).left(), labelX);
        const qreal thrRight = controlRight(thr);
        h.controller->emulation()->setOption(QStringLiteral("rich_boot"), QStringLiteral("native"));  // rebuilds the rows
        QTest::qWait(60);
        QQuickItem* boot2 = h.item("optionRow_rich_boot");
        QVERIFY(visible(h, "changedDot_rich_boot"));
        QCOMPARE(sceneRect(rowItem(boot2, "labelItem")).left(), labelX);
        QCOMPARE(controlRight(boot2), thrRight);
        h.controller->emulation()->resetOption(QStringLiteral("rich_boot"));
        QTest::qWait(60);
        if (w == 1280) emuRight = thrRight;
      }
    }
    h.window->resize(1280, 800);
    QTest::qWait(100);
    QCOMPARE(controlRight(h.item("optionRow_rich_threaded")), emuRight);
  }

  void emulationCategoryChipsCollapse() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), richProbe(), false);
    h.controller->showEmulation();
    h.window->resize(960, 800);
    QTest::qWait(150);
    QQuickTest::qWaitForPolish(h.window);
    QQuickItem* bar = h.item("categoryChips");
    QVERIFY(bar != nullptr);
    QVERIFY(visible(h, "categoryMore"));
    QVERIFY(text(h, "categoryMore").startsWith(QLatin1Char('+')) && text(h, "categoryMore").contains(QStringLiteral("more")));
    QQuickItem* flick = h.item("settingsScroll");
    QVERIFY(sceneRect(h.item("categoryMore")).right() <= sceneRect(bar).right() + 0.5);
    QVERIFY(sceneRect(bar).right() <= sceneRect(flick).right() + 0.5);
    QVERIFY(visible(h, "categoryAll"));
    // The hidden categories are in the menu; picking one filters and keeps the selection reachable.
    QVERIFY(h.click("categoryMore"));
    QTest::qWait(80);
    QQuickItem* last = h.item("categoryOverflow_Input Devices");
    QVERIFY(last != nullptr && last->isVisible());
    QVERIFY(h.click("categoryOverflow_Input Devices"));
    QTest::qWait(80);
    QCOMPARE(h.controller->emulation()->categoryFilter(), QStringLiteral("Input Devices"));
    QVERIFY(h.item("optionRow_rich_input") != nullptr);
    QVERIFY(h.item("optionRow_rich_boot") == nullptr);
    QVERIFY(h.click("categoryAll"));
    // Wider window: more chips fit than at 960 px. The page column is capped (840 px), so whether all eight chips fit
    // there depends on the platform font; the "everything fits" case is checked with a short category list below.
    const auto hiddenCount = [&]() { return visible(h, "categoryMore") ? text(h, "categoryMore").mid(1).split(QLatin1Char(' ')).first().toInt() : 0; };
    const int hiddenNarrow = hiddenCount();
    h.window->resize(1920, 800);
    QTest::qWait(150);
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(hiddenCount() < hiddenNarrow);
    QVERIFY(visible(h, "category_System"));
    // Few short categories always fit: all chips are shown and there is no "+N more".
    emu::CoreProbe few = richProbe();
    few.categories = {few.categories.at(0), few.categories.at(2)};
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), few, false);
    QTest::qWait(150);
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(!visible(h, "categoryMore"));
    QVERIFY(visible(h, "category_System") && visible(h, "category_Audio"));
  }

  void emulationDescriptionOptionList() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), richProbe(), false);
    h.controller->showEmulation();
    QTest::qWait(120);
    QQuickItem* row = h.item("optionRow_rich_mic");
    QVERIFY(row != nullptr);
    QCOMPARE(row->property("moreLabel").toString(), QStringLiteral("More · 3 options"));
    QVERIFY(!visible(h, "optionList_rich_mic"));
    QVERIFY(h.click("descMore_rich_mic"));
    QTest::qWait(60);
    QVERIFY(visible(h, "optionList_rich_mic"));
    QVERIFY(h.click("descMore_rich_mic"));
    QVERIFY(!visible(h, "optionList_rich_mic"));
  }

  void settingsRowsAlignWithEmulation() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), richProbe(), false);
    for (const int w : {1280, 1440, 960}) {
      h.window->resize(w, 800);
      h.controller->showEmulation();
      QTest::qWait(150);
      QQuickTest::qWaitForPolish(h.window);
      qreal emuRight = 0;
      if (w >= 1280) {
        QQuickItem* thr = h.item("optionRow_rich_threaded");
        QVERIFY(thr != nullptr);
        emuRight = controlRight(thr);
      }
      // Each section is its own page; the rows of all of them end where the Emulation controls end.
      const auto section = [&](const char* id) {
        h.controller->showSettingsSection(QString::fromLatin1(id));
        QTest::qWait(150);
        QQuickTest::qWaitForPolish(h.window);
        QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
        verifyRowsInside(h, "settingsScroll", w);
      };
      section("updates");
      const QRectF col = sceneRect(h.item("settingsScroll"));
      // Hub card and update card are indented 14 to the label edge and stay inside the column.
      QQuickItem* card = h.item("updateCard");
      QVERIFY(card != nullptr);
      QVERIFY(sceneRect(card).left() >= col.left() + 48 + 14 - 0.5);
      QVERIFY(sceneRect(card).right() <= col.right() + 0.5);
      QQuickItem* channelRow = h.item("updateChannelRow");
      QQuickItem* versionRow = h.item("versionRow");
      QQuickItem* autoInstall = h.item("updateAutoInstallRow");
      QVERIFY(channelRow && versionRow && autoInstall);
      QCOMPARE(versionRow->property("kind").toString(), QStringLiteral("value"));
      if (w >= 1280) {
        for (QQuickItem* r : {channelRow, versionRow, autoInstall}) QCOMPARE(controlRight(r), emuRight);
        QCOMPARE(sceneRect(h.item("updatesVersion")).right(), emuRight);
        QCOMPARE(sceneRect(h.item("updateChannelSegment")).right(), emuRight);
        QCOMPARE(sceneRect(h.item("updateAutoInstallToggle")).right(), emuRight);
      }
      section("appearance");
      QQuickItem* themeRow = h.item("themeRow");
      QVERIFY(themeRow != nullptr);
      QCOMPARE(themeRow->property("kind").toString(), QStringLiteral("segment"));
      // The reset column stays empty on Settings.
      QVERIFY(!rowItem(themeRow, "resetColumn")->isVisible());
      if (w >= 1280) {
        QCOMPARE(controlRight(themeRow), emuRight);
        QCOMPARE(sceneRect(h.item("appearanceSegment")).right(), emuRight);
        QCOMPARE(sceneRect(h.item("appearanceSegment")).width(), 280.0);
      }
      section("diagnostics");
      QQuickItem* logRow = h.item("logFileRow");
      QVERIFY(logRow != nullptr);
      if (w >= 1280) QCOMPARE(controlRight(logRow), emuRight);
    }
  }

  // 6c-3: after going back the Settings page shows the amber card; "Resume updates" ends the pause.
  void settingsAfterGoingBack() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(QDir().mkpath(h.dir.filePath(QStringLiteral("settings"))));
    QFile f(h.dir.filePath(QStringLiteral("settings/player.json")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(QJsonObject{{"update_auto_install", false},
                                      {"update_skip", "9.9.9"},
                                      {"update_paused", QJsonObject{{"from", "9.9.9"}, {"to", "0.0.1"}, {"at", "2026-10-22T10:00:00Z"}, {"prev_auto", true}}}})
                .toJson());
    f.close();
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->showSettingsSection(QStringLiteral("updates"));
    QTest::qWait(150);
    QQuickTest::qWaitForPolish(h.window);
    UpdatesController* up = h.controller->updates();
    QVERIFY(up->paused());
    QCOMPARE(up->skippedVersion(), QStringLiteral("9.9.9"));
    QVERIFY(visible(h, "updatesPausedCard"));
    QVERIFY(text(h, "updatesPausedTitle").contains(QStringLiteral("automatic updates paused until you resume them")));
    QVERIFY(text(h, "updatesPausedDetail").contains(QStringLiteral("You went back from 9.9.9")));
    QVERIFY(visible(h, "earlierEmpty"));  // no index yet: nothing to go back to
    QCOMPARE(up->earlierCount(), 0);

    // The dialog data: plugins that need a newer Player are listed as turned off.
    QVERIFY(up->rollbackInfo(QStringLiteral("0.0.1")).value(QStringLiteral("pluginLines")).toStringList().isEmpty());
    up->setPluginNeedsProvider([](const QString&) { return QList<QPair<QString, QString>>{{QStringLiteral("SoulLink"), QStringLiteral("0.16.4")}}; });
    const QVariantMap info = up->rollbackInfo(QStringLiteral("0.0.1"));
    QCOMPARE(info.value(QStringLiteral("pluginLines")).toStringList(),
             QStringList{QStringLiteral("SoulLink needs 0.16.4 and is turned off until you update.")});
    QVERIFY(!info.value(QStringLiteral("canGoBack")).toBool());  // a development build cannot install by itself

    QVERIFY(h.click("updatesResume"));
    QTest::qWait(60);
    QVERIFY(!up->paused());
    QVERIFY(!visible(h, "updatesPausedCard"));
    QCOMPARE(up->skippedVersion(), QStringLiteral("9.9.9"));  // the skip stays until the user updates
  }

  void settingsHierarchyOnThePage() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);
    h.controller->showEmulation();
    EmulationController* emu = h.controller->emulation();

    // Defaults: Uzume options only; core keys belong to the systems.
    QVERIFY(h.click("tab_Defaults"));
    QCOMPARE(emu->level(), QStringLiteral("global"));
    QVERIFY(emu->defaultsSelected());
    QCOMPARE(h.item("emulationTabs")->property("active").toString(), QStringLiteral("Defaults"));
    QVERIFY(h.item("optionGroup_core") == nullptr);
    QVERIFY(visible(h, "globalCoreHint"));
    QVERIFY(visible(h, "optionGroup_uzume"));
    QVERIFY(h.item("optionToggle_uzume.fullscreen_on_start") != nullptr);  // off/on = toggle
    QCOMPARE(text(h, "optionOrigin_uzume.fullscreen_on_start"), QStringLiteral("Default"));
    QVERIFY(!h.controller->fullscreenOnStart());
    QVERIFY(h.click("optionToggle_uzume.fullscreen_on_start"));
    QVERIFY(visible(h, "changedDot_uzume.fullscreen_on_start"));
    QVERIFY(h.controller->fullscreenOnStart());  // wired: the game view goes fullscreen on start
    QCOMPARE(emu->defaultsChangedCount(), 1);
    QCOMPARE(text(h, "tabCountText_Defaults"), QStringLiteral("1"));

    // The system inherits it (stored only once, at Global); resetting Defaults restores the default.
    QVERIFY(h.click("tab_NDS"));
    QCOMPARE(emu->level(), QStringLiteral("system"));
    QVERIFY(h.controller->fullscreenOnStart());
    QVERIFY(h.click("tab_Defaults"));
    QVERIFY(h.click("optionReset_uzume.fullscreen_on_start"));
    QVERIFY(!h.controller->fullscreenOnStart());

    // Default Multiview offers Picture-in-Picture, Side-by-Side and Grid 2x2; it is applied to the Session view.
    QCOMPARE(h.controller->sessions()->multiviewMode(), QStringLiteral("pip"));
    QVERIFY(h.item("optionSelect_uzume.default_multiview") != nullptr);
    QStringList values;
    for (const QVariant& g : emu->groups()) {
      for (const QVariant& o : g.toMap().value(QStringLiteral("options")).toList()) {
        if (o.toMap().value(QStringLiteral("key")).toString() == QStringLiteral("uzume.default_multiview")) {
          for (const QVariant& v : o.toMap().value(QStringLiteral("values")).toList()) values.append(v.toMap().value(QStringLiteral("value")).toString());
        }
      }
    }
    QCOMPARE(values, (QStringList{QStringLiteral("pip"), QStringLiteral("row"), QStringLiteral("grid")}));
    pick(h, "optionSelect_uzume.default_multiview", QStringLiteral("row"));
    QCOMPARE(h.controller->sessions()->multiviewMode(), QStringLiteral("row"));
    pick(h, "optionSelect_uzume.default_multiview", QStringLiteral("grid"));
    QCOMPARE(emu->uzumeValue(QStringLiteral("uzume.default_multiview"), QStringLiteral("nds")), QStringLiteral("grid"));

    // A core option set globally in the file (keys stay assigned to the core) is still inherited by the system.
    emu->settings()->setValue(L::Global, QString(), QStringLiteral("melonds_boot_mode"), QStringLiteral("native"));
    emu->setLevel(QStringLiteral("system"));
    QCOMPARE(emu->launchOverrides(QStringLiteral("nds"), QString()).value(QStringLiteral("melonds_boot_mode")), QStringLiteral("native"));
  }

  void pictureOptionsPerSystemAndGame() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);
    h.controller->showEmulation();
    EmulationController* emu = h.controller->emulation();
    const QString integer = QString::fromLatin1(EmulationController::kIntegerScaleKey);
    const QString aspect = QString::fromLatin1(EmulationController::kAspectRatioKey);

    // Defaults of both options.
    QCOMPARE(emu->uzumeValue(integer, QStringLiteral("nds"), QString()), QStringLiteral("off"));
    QCOMPARE(emu->uzumeValue(aspect, QStringLiteral("nds"), QString()), QStringLiteral("core"));

    // A system shows both rows under the category "Picture".
    QVERIFY(h.click("tab_NDS"));
    QCOMPARE(emu->level(), QStringLiteral("system"));
    QStringList picture;
    for (const QVariant& g : emu->groups()) {
      for (const QVariant& o : g.toMap().value(QStringLiteral("options")).toList()) {
        const QVariantMap m = o.toMap();
        if (m.value(QStringLiteral("category")).toString() == QStringLiteral("Picture")) {
          picture.append(m.value(QStringLiteral("key")).toString());
          QVERIFY(!m.value(QStringLiteral("restart")).toBool());
        }
      }
    }
    QCOMPARE(picture, (QStringList{integer, aspect}));

    // Stored per system, emitted live, and unknown values are refused.
    QSignalSpy changed(emu, &EmulationController::uzumeOptionsChanged);
    emu->setOption(integer, QStringLiteral("on"));
    emu->setOption(aspect, QStringLiteral("stretch"));
    emu->setOption(aspect, QStringLiteral("16:9"));
    QCOMPARE(changed.count(), 2);  // the unknown value is refused
    QCOMPARE(emu->uzumeValue(integer, QStringLiteral("nds"), QString()), QStringLiteral("on"));
    QCOMPARE(emu->uzumeValue(aspect, QStringLiteral("nds"), QString()), QStringLiteral("stretch"));
    QCOMPARE(emu->uzumeValue(integer, QStringLiteral("gba"), QString()), QStringLiteral("off"));  // other systems keep the default

    // A game overrides the system.
    emu->settings()->setValue(L::Game, QStringLiteral("g1"), aspect, QStringLiteral("4:3"));
    QCOMPARE(emu->uzumeValue(aspect, QStringLiteral("nds"), QStringLiteral("g1")), QStringLiteral("4:3"));
    QCOMPARE(emu->uzumeValue(aspect, QStringLiteral("nds"), QStringLiteral("g2")), QStringLiteral("stretch"));
  }

  // 0.19 package 6: layout, swap and the bottom-screen window resolve global > system > game like every uzume.* option.
  void screenLayoutOptionsPerSystemAndGame() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);
    h.controller->showEmulation();
    EmulationController* emu = h.controller->emulation();
    const QString layout = QString::fromLatin1(EmulationController::kScreenLayoutKey);
    const QString swap = QString::fromLatin1(EmulationController::kScreenSwapKey);
    const QString window = QString::fromLatin1(EmulationController::kBottomWindowKey);
    QCOMPARE(layout, QStringLiteral("uzume.screen_layout"));
    QCOMPARE(swap, QStringLiteral("uzume.screen_swap"));
    QCOMPARE(window, QStringLiteral("uzume.bottom_window"));

    QCOMPARE(emu->uzumeValue(layout, QStringLiteral("nds")), QStringLiteral("stacked"));
    QCOMPARE(emu->uzumeValue(swap, QStringLiteral("nds")), QStringLiteral("off"));
    QCOMPARE(emu->uzumeValue(window, QStringLiteral("nds")), QStringLiteral("off"));

    const auto rowKeys = [&]() {
      QStringList keys;
      for (const QVariant& g : emu->groups())
        for (const QVariant& o : g.toMap().value(QStringLiteral("options")).toList()) keys.append(o.toMap().value(QStringLiteral("key")).toString());
      return keys;
    };
    // A two-screen system shows the rows, a single-screen system does not.
    QVERIFY(h.click("tab_NDS"));
    QVERIFY(rowKeys().contains(layout) && rowKeys().contains(swap) && rowKeys().contains(window));
    QSignalSpy changed(emu, &EmulationController::uzumeOptionsChanged);
    emu->setOption(layout, QStringLiteral("large"));
    emu->setOption(swap, QStringLiteral("on"));
    emu->setOption(window, QStringLiteral("on"));
    emu->setOption(layout, QStringLiteral("diagonal"));  // refused
    QCOMPARE(changed.count(), 3);
    for (const char* v : {"stacked", "side", "large", "top", "bottom"}) {  // all five layouts are accepted
      emu->setOption(layout, QString::fromLatin1(v));
      QCOMPARE(emu->uzumeValue(layout, QStringLiteral("nds")), QString::fromLatin1(v));
    }
    emu->setOption(layout, QStringLiteral("large"));

    QCOMPARE(emu->uzumeValue(layout, QStringLiteral("nds")), QStringLiteral("large"));
    QCOMPARE(emu->uzumeValue(swap, QStringLiteral("nds")), QStringLiteral("on"));
    QCOMPARE(emu->uzumeValue(window, QStringLiteral("nds")), QStringLiteral("on"));
    QCOMPARE(emu->uzumeValue(layout, QStringLiteral("3ds")), QStringLiteral("stacked"));  // other systems keep the default
    // A game overrides the system; a value stored by an older version stays valid.
    emu->settings()->setValue(L::Game, QStringLiteral("g1"), layout, QStringLiteral("side"));
    emu->settings()->setValue(L::Game, QStringLiteral("g1"), swap, QStringLiteral("off"));
    QCOMPARE(emu->uzumeValue(layout, QStringLiteral("nds"), QStringLiteral("g1")), QStringLiteral("side"));
    QCOMPARE(emu->uzumeValue(swap, QStringLiteral("nds"), QStringLiteral("g1")), QStringLiteral("off"));
    QCOMPARE(emu->uzumeValue(layout, QStringLiteral("nds"), QStringLiteral("g2")), QStringLiteral("large"));
    QCOMPARE(emu->uzumeValue(window, QStringLiteral("nds"), QStringLiteral("g1")), QStringLiteral("on"));
  }

  void firmwareStateOnTheCard() {
    const QByteArray b7(16, '7'), b9(8, '9'), fw(32, 'f');
    struct Case { QString mode; QJsonArray files; QString expected; };
    const QList<Case> cases = {
        {QStringLiteral("builtin"), {}, QStringLiteral("Built-in BIOS")},
        {QStringLiteral("native"),
         QJsonArray{fwFile(QStringLiteral("bios7"), QStringLiteral("ARM7 BIOS"), true, b7), fwFile(QStringLiteral("bios9"), QStringLiteral("ARM9 BIOS"), true, {}),
                    fwFile(QStringLiteral("firmware"), QStringLiteral("DS Firmware"), true, fw)},
         QStringLiteral("Firmware required/missing")},
        {QStringLiteral("native"),
         QJsonArray{fwFile(QStringLiteral("bios7"), QStringLiteral("ARM7 BIOS"), true, b7), fwFile(QStringLiteral("bios9"), QStringLiteral("ARM9 BIOS"), true, b9),
                    fwFile(QStringLiteral("firmware"), QStringLiteral("DS Firmware"), true, fw)},
         QStringLiteral("Firmware from server · verified")},
    };
    for (const Case& c : cases) {
      FakeHub hub(QStringLiteral("a"));
      hub.features = {QStringLiteral("saves_v1"), QStringLiteral("firmware_v1")};
      hub.systems = ndsSystem(c.mode, c.files);
      QVERIFY(hub.start());
      Harness h;
      QVERIFY(h.start());
      pair(h, hub);
      QTRY_VERIFY_WITH_TIMEOUT(h.controller->hubSystems()->state() == HubSystems::State::Ready, 8000);
      h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);
      h.controller->showEmulation();
      QTRY_COMPARE(text(h, "systemFirmware_nds"), c.expected);
      if (c.expected.startsWith(QStringLiteral("Firmware required"))) {
        QCOMPARE(h.controller->emulation()->system().value(QStringLiteral("firmwareTone")).toString(), QStringLiteral("error"));
      }
    }
  }

  // ADR 0020 D6: the Core select lists the cores the Hub serves; the choice is stored and becomes the effective core.
  void coreChoiceOnTheEmulationPage() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("firmware_v1"), QStringLiteral("cores_v1"), QStringLiteral("cores_v2")};
    const auto core = [](const QString& id, const QString& name, const QString& license, const QString& date) {
      return QJsonObject{{QStringLiteral("core_id"), id},   {QStringLiteral("display_name"), name},
                         {QStringLiteral("version"), date},  {QStringLiteral("license"), license},
                         {QStringLiteral("experimental"), false}, {QStringLiteral("origin"), QStringLiteral("libretro-buildbot")},
                         {QStringLiteral("build_date"), date}};
    };
    hub.systems = {{QStringLiteral("systems"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("nds")},
                                           {QStringLiteral("display_name"), QStringLiteral("Nintendo DS")},
                                           {QStringLiteral("preferred_core_id"), QStringLiteral("melondsds")},
                                           {QStringLiteral("default_core_id"), QStringLiteral("melondsds")},
                                           {QStringLiteral("core_package_version"), QStringLiteral("2026.10.09")},
                                           {QStringLiteral("cores"),
                                            QJsonArray{core(QStringLiteral("melondsds"), QStringLiteral("melonDS DS"), QStringLiteral("GPLv3"), QStringLiteral("2026.10.09")),
                                                       core(QStringLiteral("desmume"), QStringLiteral("DeSmuME"), QStringLiteral("GPLv2"), QStringLiteral("2026.10.08")),
                                                       core(QStringLiteral("noods"), QStringLiteral("NooDS"), QStringLiteral("GPLv3"), QStringLiteral("2026.10.07"))}},
                                           {QStringLiteral("firmware_mode"), QStringLiteral("builtin")},
                                           {QStringLiteral("firmware"), QJsonArray{}}}}}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->hubSystems()->state() == HubSystems::State::Ready, 8000);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);
    h.controller->showEmulation();
    EmulationController* emu = h.controller->emulation();
    QCOMPARE(emu->system().value(QStringLiteral("coreId")).toString(), QStringLiteral("melondsds"));
    QVERIFY(h.item("optionSelect_uzume.core") != nullptr);
    const QVariantList cores = emu->system().value(QStringLiteral("cores")).toList();
    QCOMPARE(cores.size(), 3);
    const QString melon = cores.at(0).toMap().value(QStringLiteral("label")).toString();
    QVERIFY2(melon.contains(QStringLiteral("GPLv3")) && melon.contains(QStringLiteral("2026.10.09")) && !melon.contains(QStringLiteral("Experimental")), qPrintable(melon));
    QVERIFY(cores.at(2).toMap().value(QStringLiteral("label")).toString().contains(QStringLiteral("Experimental")));  // no profile for noods
    QVERIFY(!cores.at(1).toMap().value(QStringLiteral("experimental")).toBool());                                     // DeSmuME has one
    QCOMPARE(h.controller->selectedGame().value(QStringLiteral("coreExperimental")).toBool(), false);

    // Choose DeSmuME for the system: stored locally, effective at once.
    pick(h, "optionSelect_uzume.core", QStringLiteral("desmume"));
    const QString key = QString::fromLatin1(EmulationSettings::kCoreKey);
    QCOMPARE(emu->settings()->value(EmulationSettings::Level::System, QStringLiteral("nds"), key), QStringLiteral("desmume"));
    QCOMPARE(emu->system().value(QStringLiteral("coreId")).toString(), QStringLiteral("desmume"));
    QVERIFY(!emu->system().value(QStringLiteral("coreExperimental")).toBool());
    QVERIFY(visible(h, "changedDot_uzume.core"));
    // Choosing the Hub default again stores nothing.
    pick(h, "optionSelect_uzume.core", QStringLiteral("melondsds"));
    QVERIFY(!emu->settings()->hasValue(EmulationSettings::Level::System, QStringLiteral("nds"), key));
    QCOMPARE(emu->system().value(QStringLiteral("coreId")).toString(), QStringLiteral("melondsds"));

    // A core without profile is experimental: marked everywhere, no option defaults.
    pick(h, "optionSelect_uzume.core", QStringLiteral("noods"));
    QCOMPARE(emu->system().value(QStringLiteral("coreId")).toString(), QStringLiteral("noods"));
    QVERIFY(emu->system().value(QStringLiteral("coreExperimental")).toBool());
    QVERIFY(emu->system().value(QStringLiteral("coreName")).toString().contains(QStringLiteral("Experimental")));
    QString coreNote;
    for (const QVariant& g : emu->groups()) {
      if (g.toMap().value(QStringLiteral("id")).toString() == QStringLiteral("core")) coreNote = g.toMap().value(QStringLiteral("note")).toString();
    }
    QVERIFY2(coreNote.contains(QStringLiteral("Experimental core")), qPrintable(coreNote));

    // A stored choice the Hub does not serve (any more) falls back to the Hub default with a notice.
    QVERIFY(emu->settings()->setValue(EmulationSettings::Level::System, QStringLiteral("nds"), key, QStringLiteral("gone-core")));
    emit emu->coreChoiceChanged();  // what the page does after a change: the PlayerController refreshes the cards
    QCOMPARE(emu->system().value(QStringLiteral("coreId")).toString(), QStringLiteral("melondsds"));
    QVERIFY2(emu->system().value(QStringLiteral("coreNotice")).toString().contains(QStringLiteral("gone-core")),
             qPrintable(emu->system().value(QStringLiteral("coreNotice")).toString()));
    // Reset restores inheritance and the notice goes away.
    emu->resetOption(key);
    QVERIFY(emu->system().value(QStringLiteral("coreNotice")).toString().isEmpty());
  }

  void perGameOverridesOnTheEmulationPage() {
    FakeHub hub(QStringLiteral("a"));
    hub.features = {QStringLiteral("saves_v1"), QStringLiteral("firmware_v1"), QStringLiteral("cores_v1"), QStringLiteral("cores_v2")};
    const auto core = [](const QString& id, const QString& name, const QString& date) {
      return QJsonObject{{QStringLiteral("core_id"), id},   {QStringLiteral("display_name"), name},
                         {QStringLiteral("version"), date},  {QStringLiteral("license"), QStringLiteral("GPLv3")},
                         {QStringLiteral("experimental"), false}, {QStringLiteral("origin"), QStringLiteral("libretro-buildbot")},
                         {QStringLiteral("build_date"), date}};
    };
    hub.systems = {{QStringLiteral("systems"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("nds")},
                                           {QStringLiteral("display_name"), QStringLiteral("Nintendo DS")},
                                           {QStringLiteral("preferred_core_id"), QStringLiteral("melondsds")},
                                           {QStringLiteral("default_core_id"), QStringLiteral("melondsds")},
                                           {QStringLiteral("core_package_version"), QStringLiteral("2026.10.09")},
                                           {QStringLiteral("cores"), QJsonArray{core(QStringLiteral("melondsds"), QStringLiteral("melonDS DS"), QStringLiteral("2026.10.09")),
                                                                                core(QStringLiteral("desmume"), QStringLiteral("DeSmuME"), QStringLiteral("2026.10.08"))}},
                                           {QStringLiteral("firmware_mode"), QStringLiteral("builtin")},
                                           {QStringLiteral("firmware"), QJsonArray{}}}}}};
    const QByteArray rom = QByteArray(2048, 'q') + "UZUME-DUMMY";  // dummy file, no real ROM
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    hub.games = QJsonObject{{QStringLiteral("games"), QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"),
                                                                                    uitest::sha256Hex(rom), rom.size())}}};
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->hubSystems()->state() == HubSystems::State::Ready, 8000);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);
    h.controller->showEmulation();
    EmulationController* emu = h.controller->emulation();
    const QString key = QString::fromLatin1(EmulationSettings::kCoreKey);
    using L = EmulationSettings::Level;

    // The old placeholder is gone; the card and the picker exist.
    QVERIFY(h.item("tab_Game overrides") != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(emu->games().size(), 1, 4000);
    QCOMPARE(emu->games().first().toMap().value(QStringLiteral("systemId")).toString(), QStringLiteral("nds"));
    QVERIFY(!visible(h, "gamePicker"));
    emu->setLevel(QStringLiteral("game"));
    QVERIFY(visible(h, "gamePicker"));
    QVERIFY(visible(h, "gamePickerHint"));  // no game picked yet
    QVERIFY(emu->groups().isEmpty());

    // Pick the game: the picker leads to its game page on the Settings tab (the per-game rows live there, not on this page).
    pick(h, "gameSelect", QStringLiteral("g1"));
    QVERIFY(h.controller->gamePageOpen());
    QCOMPARE(h.controller->gamePageTab(), QStringLiteral("settings"));
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("g1"));
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
    QTRY_VERIFY(visible(h, "gamePage"));
    QTRY_VERIFY(visible(h, "gameSettingsContent"));
    QCOMPARE(h.item("gameSettingsContent")->property("gameId").toString(), QStringLiteral("g1"));
    if (h.item("gameOptionSelect_uzume.core") == nullptr && h.item("tab_Core") != nullptr) QVERIFY(h.click("tab_Core"));
    QTRY_VERIFY(h.item("gameOptionSelect_uzume.core") != nullptr);
    // The Emulation page keeps working on the same game through the controller: its core group, "Use system default" first.
    emu->selectGame(QStringLiteral("g1"));
    QCOMPARE(emu->selectedGame(), QStringLiteral("g1"));
    QVariantMap coreRow;
    for (const QVariant& g : emu->groups()) {
      for (const QVariant& o : g.toMap().value(QStringLiteral("options")).toList()) {
        if (o.toMap().value(QStringLiteral("key")).toString() == key) coreRow = o.toMap();
      }
    }
    QCOMPARE(coreRow.value(QStringLiteral("values")).toList().first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Use system default"));
    QCOMPARE(coreRow.value(QStringLiteral("value")).toString(), QString());
    QCOMPARE(coreRow.value(QStringLiteral("values")).toList().size(), 3);  // default + two cores

    // Choose DeSmuME for this game only (on the game page): stored at game level, not for the system.
    pick(h, "gameOptionSelect_uzume.core", QStringLiteral("desmume"));
    QCOMPARE(emu->settings()->value(L::Game, QStringLiteral("g1"), key), QStringLiteral("desmume"));
    QVERIFY(!emu->settings()->hasValue(L::System, QStringLiteral("nds"), key));
    QCOMPARE(emu->system().value(QStringLiteral("coreId")).toString(), QStringLiteral("melondsds"));  // the system is unchanged
    QCOMPARE(emu->game().value(QStringLiteral("changedCount")).toInt(), 1);
    QCOMPARE(emu->gameOverrideCount(), 1);
    QCOMPARE(emu->changedCount(), 1);
    QVERIFY(visible(h, "optionReset_uzume.core"));   // the row on the game page shows it changed

    // A per-game Uzume option (speed-up) overrides system and global for this game only.
    const QString ratio = QString::fromLatin1(EmulationController::kSpeedUpRatioKey);
    emu->setGameOption(QStringLiteral("g1"), ratio, QStringLiteral("4"));
    QCOMPARE(emu->uzumeValue(ratio, QStringLiteral("nds"), QStringLiteral("g1")), QStringLiteral("4"));
    QCOMPARE(emu->uzumeValue(ratio, QStringLiteral("nds"), QString()), QStringLiteral("2"));
    QCOMPARE(emu->changedCount(), 2);

    // Reset to default: one option, then everything of the game.
    emu->resetOption(key);
    QVERIFY(!emu->settings()->hasValue(L::Game, QStringLiteral("g1"), key));
    QCOMPARE(emu->changedCount(), 1);
    QTRY_VERIFY(visible(h, "gamePageResetAll"));   // the Settings tab of the game page offers "Reset all…"
    emu->resetAllChanged();
    QVERIFY(emu->settings()->values(L::Game, QStringLiteral("g1")).isEmpty());
    QCOMPARE(emu->gameOverrideCount(), 0);
    // Choosing "Use system default" stores nothing.
    emu->setOption(key, QStringLiteral("desmume"));
    emu->setOption(key, QString());
    QVERIFY(!emu->settings()->hasValue(L::Game, QStringLiteral("g1"), key));

    // The Emulation page lists the games that keep their own values; "Open" leads to the Settings tab of the game page.
    emu->setGameOption(QStringLiteral("g1"), ratio, QStringLiteral("4"));
    emu->selectGame(QString());
    h.controller->closeGamePage();
    h.controller->showEmulation();
    emu->setLevel(QStringLiteral("game"));
    QTRY_VERIFY(visible(h, "gameOverride_g1"));
    QVERIFY(h.click("gameOverrideOpen_g1"));
    QTRY_VERIFY(h.controller->gamePageOpen());
    QCOMPARE(h.controller->gamePageTab(), QStringLiteral("settings"));
    QCOMPARE(h.controller->selectedGameId(), QStringLiteral("g1"));
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
    QTRY_VERIFY(visible(h, "gameSettingsContent"));

    // Back on a system: the game section stays out of the way.
    h.controller->showEmulation();
    emu->setLevel(QStringLiteral("system"));
    QVERIFY(!visible(h, "gamePicker"));
  }

  // ------------------------------------------------------------------ 0.19 per-game settings API and controller profiles

  void gameSettingsApiResolvesAndResets() {
    FakeHub hub(QStringLiteral("a"));
    ndsGameHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->hubSystems()->state() == HubSystems::State::Ready, 8000);
    h.controller->showEmulation();  // refreshes the system cards
    EmulationController* emu = h.controller->emulation();
    QTRY_COMPARE_WITH_TIMEOUT(emu->games().size(), 1, 4000);
    const QString g = QStringLiteral("g1");
    const QString ratio = QString::fromLatin1(EmulationController::kSpeedUpRatioKey);
    const QString scale = QString::fromLatin1(EmulationController::kIntegerScaleKey);
    const QString layout = QString::fromLatin1(EmulationController::kScreenLayoutKey);
    const QString coreKey = QString::fromLatin1(EmulationSettings::kCoreKey);

    QVERIFY(emu->gameSettings(QStringLiteral("nope")).isEmpty());
    // Before the core options are known: the uzume rows and a note on the Core tab.
    QVariantMap d = emu->gameSettings(g);
    QCOMPARE(d.value(QStringLiteral("title")).toString(), QStringLiteral("Lumen Drift"));
    QCOMPARE(d.value(QStringLiteral("systemLabel")).toString(), emu->games().first().toMap().value(QStringLiteral("systemName")).toString());
    QCOMPARE(d.value(QStringLiteral("changedCount")).toInt(), 0);
    QVERIFY(!d.value(QStringLiteral("coreKnown")).toBool());
    QVERIFY(!gameRow(d, scale).isEmpty());
    QVERIFY(!gameRow(d, layout).isEmpty());  // two-screen system
    QVERIFY(!gameRow(d, QString::fromLatin1(EmulationController::kPadProfileKey)).isEmpty());
    QVERIFY(!gameTab(d, QStringLiteral("core")).value(QStringLiteral("note")).toString().isEmpty());
    QVERIFY(!gameRow(d, coreKey).isEmpty());
    QCOMPARE(gameRow(d, coreKey).value(QStringLiteral("defaultLabel")).toString(), QStringLiteral("melonDS DS"));

    // The page's own state is not touched by anything below.
    const QString pageLevel = emu->level();
    const QString pageGame = emu->selectedGame();
    QSignalSpy changedSpy(emu, &EmulationController::gameSettingsChanged);
    QSignalSpy gamesSpy(emu, &EmulationController::gamesChanged);
    QSignalSpy uzumeSpy(emu, &EmulationController::uzumeOptionsChanged);
    QSignalSpy coreSpy(emu, &EmulationController::coreChoiceChanged);

    // A system value is what the game inherits; a game value wins; the default value stores nothing.
    emu->settings()->setValue(L::System, QStringLiteral("nds"), ratio, QStringLiteral("4"));
    QVariantMap r = gameRow(emu->gameSettings(g), ratio);
    QCOMPARE(r.value(QStringLiteral("value")).toString(), QStringLiteral("4"));
    QVERIFY(!r.value(QStringLiteral("isSet")).toBool());
    QCOMPARE(r.value(QStringLiteral("defaultLabel")).toString(), r.value(QStringLiteral("valueLabel")).toString());
    emu->setGameOption(g, ratio, QStringLiteral("8"));
    r = gameRow(emu->gameSettings(g), ratio);
    QVERIFY(r.value(QStringLiteral("isSet")).toBool());
    QCOMPARE(r.value(QStringLiteral("value")).toString(), QStringLiteral("8"));
    QCOMPARE(r.value(QStringLiteral("defaultValue")).toString(), QStringLiteral("4"));
    QCOMPARE(emu->uzumeValue(ratio, QStringLiteral("nds"), g), QStringLiteral("8"));
    QCOMPARE(emu->uzumeValue(ratio, QStringLiteral("nds")), QStringLiteral("4"));
    QCOMPARE(emu->gameChangedCount(g), 1);
    QCOMPARE(emu->gameSettings(g).value(QStringLiteral("changedCount")).toInt(), 1);
    QCOMPARE(gameTab(emu->gameSettings(g), QStringLiteral("speedup")).value(QStringLiteral("changed")).toInt(), 1);
    QCOMPARE(emu->gameOverrideCount(), 1);
    QVERIFY(changedSpy.count() > 0 && gamesSpy.count() > 0 && uzumeSpy.count() > 0);
    emu->setGameOption(g, ratio, QStringLiteral("4"));  // the inherited value: nothing stays stored
    QVERIFY(!emu->settings()->hasValue(L::Game, g, ratio));
    emu->setGameOption(g, ratio, QStringLiteral("not-a-value"));  // refused
    emu->setGameOption(g, QStringLiteral("uzume.nope"), QStringLiteral("1"));
    QCOMPARE(emu->gameChangedCount(g), 0);

    // Core choice per game: "Use system default" stores nothing; a game core is stored at game level only.
    emu->setGameOption(g, coreKey, QStringLiteral("desmume"));
    QCOMPARE(emu->settings()->value(L::Game, g, coreKey), QStringLiteral("desmume"));
    QVERIFY(!emu->settings()->hasValue(L::System, QStringLiteral("nds"), coreKey));
    QVERIFY(gameRow(emu->gameSettings(g), coreKey).value(QStringLiteral("valueLabel")).toString().startsWith(QStringLiteral("DeSmuME")));
    QCOMPARE(emu->gameSettings(g).value(QStringLiteral("coreName")).toString(), QStringLiteral("DeSmuME"));
    QVERIFY(coreSpy.count() > 0);
    emu->setGameOption(g, coreKey, QString());
    QVERIFY(!emu->settings()->hasValue(L::Game, g, coreKey));

    // Core options of the game's core appear once known; locked/hidden ones stay out.
    emu->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);
    d = emu->gameSettings(g);
    QVERIFY(d.value(QStringLiteral("coreKnown")).toBool());
    QVERIFY(!gameRow(d, QStringLiteral("melonds_audio_interpolation")).isEmpty());
    QVERIFY(gameRow(d, QStringLiteral("melonds_hidden_by_core")).isEmpty());
    QVERIFY(gameRow(d, QStringLiteral("melonds_sysfile_mode")).isEmpty());  // firmware option: Uzume controls it
    emu->setGameOption(g, QStringLiteral("melonds_audio_interpolation"), QStringLiteral("linear"));
    QCOMPARE(emu->launchOverrides(QStringLiteral("nds"), g).value(QStringLiteral("melonds_audio_interpolation")), QStringLiteral("linear"));
    QVERIFY(emu->launchOverrides(QStringLiteral("nds"), QString()).value(QStringLiteral("melonds_audio_interpolation")).isEmpty());

    // Restart marks: only the running game, and not for the picture options that apply at once.
    emu->setGameOption(g, scale, QStringLiteral("on"));
    d = emu->gameSettings(g);
    QVERIFY(!gameRow(d, ratio).value(QStringLiteral("restart")).toBool());
    emu->setRunningGame(g);
    d = emu->gameSettings(g);
    QVERIFY(gameRow(d, ratio).value(QStringLiteral("restart")).toBool());
    QVERIFY(gameRow(d, QStringLiteral("melonds_audio_interpolation")).value(QStringLiteral("restart")).toBool());
    QVERIFY(!gameRow(d, scale).value(QStringLiteral("restart")).toBool());
    QVERIFY(d.value(QStringLiteral("restartHint")).toBool());  // a restart-only row is changed while the game runs
    emu->setRunningGame(QString());

    // Reset one row, then everything.
    emu->resetGameOption(g, scale);
    QVERIFY(!emu->settings()->hasValue(L::Game, g, scale));
    QCOMPARE(emu->gameChangedCount(g), 1);
    emu->setGameOption(g, ratio, QStringLiteral("8"));
    emu->resetGameOptions(g);
    QCOMPARE(emu->gameChangedCount(g), 0);
    QCOMPARE(emu->gameOverrideCount(), 0);

    QCOMPARE(emu->level(), pageLevel);
    QCOMPARE(emu->selectedGame(), pageGame);
  }

  void controllerProfilesPerGame() {
    FakeHub hub(QStringLiteral("a"));
    ndsGameHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->hubSystems()->state() == HubSystems::State::Ready, 8000);
    EmulationController* emu = h.controller->emulation();
    ControllersController* c = h.controller->controllers();
    ControllerProfiles* store = c->profileStore();
    h.controller->showEmulation();  // refreshes the system cards
    QTRY_COMPARE_WITH_TIMEOUT(emu->games().size(), 1, 4000);
    const QString g = QStringLiteral("g1");
    const QString kbKey = QString::fromLatin1(EmulationController::kKeyboardProfileKey);
    const QString padKey = QString::fromLatin1(EmulationController::kPadProfileKey);
    const QString kbDevice = QString::fromLatin1(ControllerProfiles::kKeyboardDevice);

    // A user keyboard profile with F9 on the A button.
    const QString mine = store->duplicate(QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId), QStringLiteral("Lefty"));
    QVERIFY(!mine.isEmpty());
    QVERIFY(store->setBinding(mine, QStringLiteral("a"), {QStringLiteral("key:%1").arg(int(Qt::Key_F9))}));
    emu->profilesChanged();
    QVariantMap row = gameRow(emu->gameSettings(g), kbKey);
    QCOMPARE(row.value(QStringLiteral("value")).toString(), QString());  // the device's own profile
    QCOMPARE(row.value(QStringLiteral("valueLabel")).toString(), QStringLiteral("Device's own profile"));
    QStringList ids;
    for (const QVariant& v : row.value(QStringLiteral("values")).toList()) ids << v.toMap().value(QStringLiteral("value")).toString();
    QVERIFY(ids.contains(mine) && ids.contains(QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId)));
    QVERIFY(!ids.contains(QString::fromLatin1(ControllerProfiles::kBuiltinGamepadId)));  // only profiles of the kind
    QVERIFY(gameRow(emu->gameSettings(g), padKey).value(QStringLiteral("values")).toList().size() >= 2);

    // Stored per game, resolved system/game; the game wins.
    emu->setGameOption(g, kbKey, mine);
    QCOMPARE(emu->uzumeValue(kbKey, QStringLiteral("nds"), g), mine);
    QCOMPARE(emu->uzumeValue(kbKey, QStringLiteral("nds")), QString());
    const QString assigned = store->assignedProfileId(kbDevice, QStringLiteral("keyboard"));
    QCOMPARE(assigned, QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId));

    // Applied at game start as a temporary override; the stored assignment stays, the end of the game clears it.
    QVERIFY(!c->keyboardMap().contains(int(Qt::Key_F9)));
    c->setGameProfiles(QString(), emu->uzumeValue(kbKey, QStringLiteral("nds"), g));
    QCOMPARE(c->gameKeyboardProfile(), mine);
    QVERIFY(c->keyboardMap().contains(int(Qt::Key_F9)));
    QCOMPARE(store->assignedProfileId(kbDevice, QStringLiteral("keyboard")), assigned);
    ControllerProfiles reread(h.controller->profileStore()->baseDir());
    QCOMPARE(reread.assignedProfileId(kbDevice, QStringLiteral("keyboard")), assigned);  // nothing written
    c->clearGameProfiles();
    QVERIFY(!c->keyboardMap().contains(int(Qt::Key_F9)));
    QCOMPARE(c->gameKeyboardProfile(), QString());

    // A deleted profile stays listed (marked missing) and stays stored; the device's own profile is used meanwhile.
    QVERIFY(store->remove(mine));
    emu->profilesChanged();
    row = gameRow(emu->gameSettings(g), kbKey);
    QCOMPARE(row.value(QStringLiteral("value")).toString(), mine);
    QCOMPARE(row.value(QStringLiteral("valueLabel")).toString(), QStringLiteral("Deleted profile (missing)"));
    QVERIFY(row.value(QStringLiteral("isSet")).toBool());
    QCOMPARE(emu->settings()->value(L::Game, g, kbKey), mine);
    const QHash<int, quint32> own = c->keyboardMap();
    c->setGameProfiles(QString(), mine);
    QCOMPARE(c->keyboardMap(), own);
    c->clearGameProfiles();
    emu->resetGameOption(g, kbKey);
    row = gameRow(emu->gameSettings(g), kbKey);
    QVERIFY(!row.value(QStringLiteral("isSet")).toBool());

    // The Emulation page offers the rows at system and game level, not in Defaults.
    emu->setLevel(QStringLiteral("system"));
    bool onSystem = false;
    for (const QVariant& grp : emu->groups()) {
      for (const QVariant& o : grp.toMap().value(QStringLiteral("options")).toList()) onSystem = onSystem || o.toMap().value(QStringLiteral("key")).toString() == padKey;
    }
    QVERIFY(onSystem);
    emu->setOption(padKey, QString::fromLatin1(ControllerProfiles::kBuiltinGamepadId));
    QCOMPARE(emu->uzumeValue(padKey, QStringLiteral("nds"), g), QString::fromLatin1(ControllerProfiles::kBuiltinGamepadId));
    QCOMPARE(gameRow(emu->gameSettings(g), padKey).value(QStringLiteral("defaultLabel")).toString(), QStringLiteral("Standard Gamepad"));
    emu->setLevel(QStringLiteral("global"));
    for (const QVariant& grp : emu->groups()) {
      for (const QVariant& o : grp.toMap().value(QStringLiteral("options")).toList()) QVERIFY(o.toMap().value(QStringLiteral("key")).toString() != padKey);
    }
  }

  void gamepadProfileOverrideAndHoldSheet() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    h.gamepads = true;
    QVERIFY(h.start());
    pair(h, hub);
    ControllersController* c = h.controller->controllers();
    QVERIFY2(c->gamepadAvailable(), qPrintable(c->gamepadNote()));
    ControllerProfiles* store = c->profileStore();
    VirtualPad pad("Uzume Profile Pad");
    QVERIFY(pad.js != nullptr);
    c->gamepads()->poll();
    const auto poll = [&]() { c->gamepads()->poll(); QTest::qWait(1); };
    const quint32 a = 1u << uzume::inputIndex(QStringLiteral("a"));

    // Override for P1's gamepad: the south button is "A" in the chosen profile, East is "A" in the standard one.
    const QString mine = store->duplicate(QString::fromLatin1(ControllerProfiles::kBuiltinGamepadId), QStringLiteral("Southpaw"));
    QVERIFY(store->setBinding(mine, QStringLiteral("a"), {QStringLiteral("pad:a")}));
    pad.button(South, true);
    poll();
    QVERIFY((c->gamepads()->inputMask() & a) == 0);
    c->setGameProfiles(mine, QString());
    poll();
    QVERIFY((c->gamepads()->inputMask() & a) != 0);
    const QString padDevice = c->gamepads()->devices().first().key;
    QCOMPARE(store->assignedProfileId(padDevice, QStringLiteral("gamepad")), QString::fromLatin1(ControllerProfiles::kBuiltinGamepadId));
    c->clearGameProfiles();
    poll();
    QVERIFY((c->gamepads()->inputMask() & a) == 0);
    pad.button(South, false);
    poll();

    // Hold-Select sheet: shows after the hold time, closes on release; a combo never shows it.
    c->setHoldTimeMs(40);
    QVERIFY(!c->holdSheetHeld());
    QSignalSpy sheetSpy(c, &ControllersController::holdSheetChanged);
    pad.button(Back, true);
    poll();
    QVERIFY(!c->holdSheetHeld());
    QTRY_VERIFY_WITH_TIMEOUT(c->holdSheetHeld(), 2000);
    pad.button(Back, false);
    poll();
    QVERIFY(!c->holdSheetHeld());
    QCOMPARE(sheetSpy.count(), 2);
    pad.button(Back, true);
    poll();
    pad.button(RB, true);  // Select + RB: the speed-up combo fires, the sheet never opens
    poll();
    QTest::qWait(150);
    QVERIFY(!c->holdSheetHeld());
    pad.button(RB, false);
    pad.button(Back, false);
    poll();
    // Pressing a button while the sheet is open closes it again.
    pad.button(Back, true);
    poll();
    QTRY_VERIFY_WITH_TIMEOUT(c->holdSheetHeld(), 2000);
    pad.button(LB, true);
    poll();
    QVERIFY(!c->holdSheetHeld());
    pad.button(LB, false);
    pad.button(Back, false);
    poll();

    // The rows follow the bindings in use: defaults, then a rebound action; solo bindings are not part of it.
    const auto rowOf = [&](const QString& action) {
      for (const QVariant& v : c->padSheetRows()) {
        if (v.toMap().value(QStringLiteral("action")).toString() == action) return v.toMap();
      }
      return QVariantMap();
    };
    QCOMPARE(c->padSheetRows().first().toMap().value(QStringLiteral("action")).toString(), QStringLiteral("pause"));
    QCOMPARE(rowOf(QStringLiteral("pause")).value(QStringLiteral("key")).toString(), QStringLiteral("Start"));
    QCOMPARE(rowOf(QStringLiteral("layout")).value(QStringLiteral("key")).toString(), QStringLiteral("→"));
    QCOMPARE(rowOf(QStringLiteral("layout")).value(QStringLiteral("label")).toString(), QStringLiteral("Screen layout"));
    QCOMPARE(rowOf(QStringLiteral("loadState")).value(QStringLiteral("key")).toString(), QStringLiteral("L"));
    QVERIFY(rowOf(QStringLiteral("snapshot")).isEmpty());  // its default button went to Load state (0.20)
    QCOMPARE(rowOf(QStringLiteral("speedup")).value(QStringLiteral("key")).toString(), QStringLiteral("R2"));
    QCOMPARE(rowOf(QStringLiteral("volume")).value(QStringLiteral("key")).toString(), QStringLiteral("UpDown"));
    QVERIFY(rowOf(QStringLiteral("volumeUp")).isEmpty() && rowOf(QStringLiteral("volumeDown")).isEmpty());
    QVERIFY(!rowOf(QStringLiteral("hud")).isEmpty());
    QVERIFY(store->setPadHotkey(QStringLiteral("layout"), QStringLiteral("pad:a")));
    QCOMPARE(rowOf(QStringLiteral("layout")).value(QStringLiteral("key")).toString(), QStringLiteral("A"));
    QVERIFY(store->setPadHotkey(QStringLiteral("snapshot"), QStringLiteral("solo:pad:dpleft")));  // a lone button is not a combo
    QVERIFY(rowOf(QStringLiteral("snapshot")).isEmpty());
  }

  void romCacheLimitAndClearOnTheSettingsPage() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    // Dummy files (no ROMs) in the cache: one old, one newer.
    RomCache cache(h.controller->profileStore()->romCacheDir());
    const auto add = [&](char fill, qint64 usedMs) {
      const QByteArray d(4096, fill);
      const QString sha = uitest::sha256Hex(d);
      QFile f(cache.partPath(sha, QStringLiteral("3ds")));
      if (!f.open(QIODevice::WriteOnly)) return QString();
      f.write(d);
      f.close();
      if (cache.verifyAndCommit(sha, QStringLiteral("3ds")) != RomCache::CommitResult::Ok) return QString();
      cache.touch(sha, QStringLiteral("3ds"), usedMs);
      return sha;
    };
    const QString oldSha = add('a', 1000);
    const QString newSha = add('b', 2000);
    QVERIFY(!oldSha.isEmpty() && !newSha.isEmpty());

    h.controller->showSettingsSection(QStringLiteral("storage"));
    QVERIFY(h.item("romCacheLimitSelect") != nullptr);
    QVERIFY(h.item("romCacheClear") != nullptr);
    QVariantMap rc = h.controller->romCache();
    QCOMPARE(rc.value(QStringLiteral("limitBytes")).toDouble(), 20.0 * 1024 * 1024 * 1024);  // default 20 GB
    QCOMPARE(rc.value(QStringLiteral("usedBytes")).toDouble(), 8192.0);
    QCOMPARE(rc.value(QStringLiteral("files")).toInt(), 2);
    QCOMPARE(rc.value(QStringLiteral("limitOptions")).toList().first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Unlimited"));

    // A tiny limit evicts the least recently used file first.
    h.controller->setRomCacheLimit(5000);
    QVERIFY(!QFile::exists(cache.finalPath(oldSha, QStringLiteral("3ds"))));
    QVERIFY(QFile::exists(cache.finalPath(newSha, QStringLiteral("3ds"))));
    // 0 = unlimited.
    h.controller->setRomCacheLimit(0);
    QCOMPARE(h.controller->romCache().value(QStringLiteral("limitBytes")).toDouble(), 0.0);
    QCOMPARE(h.controller->romCache().value(QStringLiteral("limitText")).toString(), QStringLiteral("Unlimited"));

    // Clear: the confirmation shows what is freed, nothing is removed until it is confirmed.
    QCOMPARE(h.controller->romCache().value(QStringLiteral("clearableBytes")).toDouble(), 4096.0);
    QVERIFY(h.click("romCacheClear"));
    QTRY_VERIFY(visible(h, "romCacheClearConfirm"));
    QVERIFY(text(h, "romCacheClearConfirmText").contains(h.controller->romCache().value(QStringLiteral("clearableText")).toString()));
    QVERIFY(QFile::exists(cache.finalPath(newSha, QStringLiteral("3ds"))));
    QVERIFY(h.click("romCacheClearConfirmButton"));
    QVERIFY(!QFile::exists(cache.finalPath(newSha, QStringLiteral("3ds"))));
    QTRY_VERIFY(!visible(h, "romCacheClearConfirm"));
    QVERIFY(text(h, "romCacheClearHint").contains(QStringLiteral("Freed")));
    QCOMPARE(h.controller->romCache().value(QStringLiteral("usedBytes")).toDouble(), 0.0);
  }

  // Settings > This PC > Storage overview (0.19): measured in a worker, bar and legend, clean-up suggestions that ask first.
  void storageOverviewMeasuresAndCleansUp() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    RomCache cache(h.controller->profileStore()->romCacheDir());
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const auto add = [&](char fill, qint64 usedMs) {
      const QByteArray d(4096, fill);
      const QString sha = uitest::sha256Hex(d);
      QFile f(cache.partPath(sha, QStringLiteral("3ds")));
      if (!f.open(QIODevice::WriteOnly)) return QString();
      f.write(d);
      f.close();
      if (cache.verifyAndCommit(sha, QStringLiteral("3ds")) != RomCache::CommitResult::Ok) return QString();
      cache.touch(sha, QStringLiteral("3ds"), usedMs);
      return sha;
    };
    const qint64 day = 24ll * 60 * 60 * 1000;
    const QString oldSha = add('a', nowMs - 100 * day);
    const QString newSha = add('b', nowMs - 2 * day);
    QVERIFY(!oldSha.isEmpty() && !newSha.isEmpty());
    // A save on this PC, a waiting screenshot and two cached versions of a core (dummy bytes).
    const QString base = h.controller->profileStore()->baseDir();
    QDir().mkpath(base + QStringLiteral("/hubs/h1/users/u1/saves/g1"));
    QFile save(base + QStringLiteral("/hubs/h1/users/u1/saves/g1/game.sav"));
    QVERIFY(save.open(QIODevice::WriteOnly));
    save.write(QByteArray(2048, 's'));
    save.close();
    QDir().mkpath(base + QStringLiteral("/screenshots/pending"));
    QFile shot(base + QStringLiteral("/screenshots/pending/a.png"));
    QVERIFY(shot.open(QIODevice::WriteOnly));
    shot.write(QByteArray(1024, 'p'));
    shot.close();
    CoreCache cores(h.controller->profileStore()->coreCacheDir());
    const QByteArray lib(512, 'l');
    for (const char* v : {"2026.09.01", "2026.10.09"}) {
      CorePackageInfo p;
      p.coreId = QStringLiteral("melonds_ds");
      p.version = QString::fromLatin1(v);
      p.platform = CoreCache::currentPlatform();
      p.files.append({QStringLiteral("dummy_libretro.so"), QStringLiteral("library"), lib.size(), uitest::sha256Hex(lib), true});
      QCOMPARE(cores.store(p, p.files.at(0), lib), CoreCache::StoreResult::Ok);
      QVERIFY(cores.writePackage(p));
    }

    h.window->resize(1400, 2400);
    h.controller->showSettingsSection(QStringLiteral("storage"));
    QTRY_VERIFY(visible(h, "storageOverview"));
    QTRY_VERIFY(text(h, "storageDataPath").contains(QStringLiteral("measured")));
    QCOMPARE(text(h, "localStorageKind_games"), QStringLiteral("8 KB"));
    QCOMPARE(text(h, "localStorageKind_saves"), QStringLiteral("2 KB"));
    QCOMPARE(text(h, "localStorageKind_states"), QStringLiteral("0 KB"));
    QCOMPARE(text(h, "localStorageKind_screenshots"), QStringLiteral("1 KB"));
    QVERIFY(visible(h, "localStorageBar"));
    QVERIFY(!text(h, "localStorageFree").isEmpty());
    QVERIFY(h.item("storageMeasure") != nullptr);
    // The old rows keep working below the overview.
    QVERIFY(h.item("romCacheLimitRow") != nullptr);
    QVERIFY(h.item("romCacheClear") != nullptr);

    // Suggestions: the game not played for 60 days and the older core version; saves are never among them.
    QTRY_VERIFY(visible(h, "storageCleanup_old_games"));
    QVERIFY(visible(h, "storageCleanup_unused_cores"));
    QVERIFY(h.item("storageCleanup_old_saves") == nullptr);
    QVERIFY(!visible(h, "storageNothingToClean"));
    // Asking first: nothing is removed until it is confirmed; Cancel removes nothing.
    QVERIFY(h.click("storageCleanupButton_old_games"));
    QTRY_VERIFY(visible(h, "storageCleanupConfirm_old_games"));
    QVERIFY(text(h, "storageCleanupConfirmText_old_games").contains(QStringLiteral("download again")));
    QVERIFY(QFile::exists(cache.finalPath(oldSha, QStringLiteral("3ds"))));
    QVERIFY(h.click("storageCleanupCancel_old_games"));
    QTRY_VERIFY(!visible(h, "storageCleanupConfirm_old_games"));
    QVERIFY(QFile::exists(cache.finalPath(oldSha, QStringLiteral("3ds"))));
    QVERIFY(h.click("storageCleanupButton_old_games"));
    QTRY_VERIFY(visible(h, "storageCleanupConfirm_old_games"));
    QVERIFY(h.click("storageCleanupConfirmButton_old_games"));
    QVERIFY(!QFile::exists(cache.finalPath(oldSha, QStringLiteral("3ds"))));
    QVERIFY(QFile::exists(cache.finalPath(newSha, QStringLiteral("3ds"))));  // played recently
    QTRY_VERIFY(text(h, "storageCleanupResult").startsWith(QStringLiteral("Freed")));
    QTRY_VERIFY(!visible(h, "storageCleanup_old_games"));
    QTRY_COMPARE(text(h, "localStorageKind_games"), QStringLiteral("4 KB"));
    QVERIFY(QFile::exists(save.fileName()));  // saves stay
    // Unused cores: the newest version stays.
    QVERIFY(h.click("storageCleanupButton_unused_cores"));
    QTRY_VERIFY(visible(h, "storageCleanupConfirm_unused_cores"));
    QVERIFY(h.click("storageCleanupConfirmButton_unused_cores"));
    QTRY_COMPARE(cores.versions(QStringLiteral("melonds_ds"), CoreCache::currentPlatform()), (QStringList{"2026.10.09"}));
    QTRY_VERIFY(!visible(h, "storageCleanup_unused_cores"));
    QVERIFY(QFile::exists(save.fileName()));
  }

  void coreMissingIsShown() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->showEmulation();
    QCOMPARE(text(h, "systemReady_nds"), QStringLiteral("Core missing"));
    // Without a core and without a cache the page still works and says why the core group is empty.
    QVERIFY(visible(h, "coreNoteHint"));
    QVERIFY(!h.controller->emulation()->coreNote().isEmpty());
  }

  // Options are captured with the real core (loaded without a game) and cached for the next start. The _testcore variant
  // runs it against the Uzume test core (melondsds flavor) instead.
  void realCoreOptionsAndCache() {
    const QString core = savedCoreEnv_;
    if (core.isEmpty()) UITEST_SKIP_NO_CORE("UZUME_MELONDS_DS_CORE not set");
    qputenv("UZUME_MELONDS_DS_CORE", core.toLocal8Bit());
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start(true));  // probes the core like the handshake does
    pair(h, hub);
    h.controller->showEmulation();
    QVERIFY(h.controller->emulation()->hasCoreProbe(QStringLiteral("melondsds")));
#ifdef FB_TEST_CORE
    QCOMPARE(text(h, "systemCore_nds").left(15), QStringLiteral("Uzume Test Core"));  // the core's own name (test core, melondsds flavor)
#else
    QCOMPARE(text(h, "systemCore_nds").left(10), QStringLiteral("melonDS DS"));
#endif
    QCOMPARE(text(h, "systemReady_nds"), QStringLiteral("Ready · included in Uzume"));
    QVERIFY(h.item("optionRow_melonds_audio_interpolation") != nullptr);
    QVERIFY(h.item("optionRow_melonds_render_mode") != nullptr);
    QVERIFY(h.item("optionRow_melonds_sysfile_mode") == nullptr);
    QVERIFY(h.item("optionRow_melonds_firmware_nds_path") == nullptr);
    QVERIFY(h.item("optionRow_melonds_show_lid_state") == nullptr);
    const QString cache = QDir(h.controller->profileStore()->baseDir()).filePath(QStringLiteral("cache/core-options/melondsds.json"));
    QVERIFY(QFile::exists(cache));
    pick(h, "optionSelect_melonds_audio_interpolation", QStringLiteral("cubic"));
    QVERIFY(visible(h, "changedDot_melonds_audio_interpolation"));

    // A fresh controller without core access reads the options from the cache.
    emu::ManifestRegistry reg;
    QVERIFY(reg.loadBuiltin());
    EmulationController cached(h.controller->profileStore()->baseDir(), &reg);
    cached.loadCoreCache(QStringLiteral("melondsds"));
    QVERIFY(cached.hasCoreProbe(QStringLiteral("melondsds")));
    QCOMPARE(cached.coreProbe(QStringLiteral("melondsds"))->options.size(), h.controller->emulation()->coreProbe(QStringLiteral("melondsds"))->options.size());
    QCOMPARE(cached.settings()->value(L::System, QStringLiteral("nds"), QStringLiteral("melonds_audio_interpolation")), QStringLiteral("cubic"));
  }

  // ------------------------------------------------------------------ Controllers (3f)

  void gamepadsDisabledGracefully() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;  // gamepads off (also what a failed SDL init looks like)
    QVERIFY(h.start());
    pair(h, hub);
    QVERIFY(!h.controller->controllers()->gamepadAvailable());
    QVERIFY(!hub.lastHandshakeBody.value(QStringLiteral("input")).toObject().value(QStringLiteral("gamepad")).toBool(true));
    QVERIFY(hub.lastHandshakeBody.value(QStringLiteral("input")).toObject().value(QStringLiteral("keyboard")).toBool());
    h.controller->showControllers();   // a section of Settings
    QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
    QTRY_VERIFY(h.item("controllerTabs") != nullptr && h.item("controllerTabs")->isVisible());
    QVERIFY(visible(h, "gamepadNote"));
    QVERIFY(!text(h, "gamepadNote").isEmpty());
    QCOMPARE(h.controller->controllers()->selectedDevice(), QStringLiteral("keyboard"));  // keyboard and mouse still work
    QCOMPARE(h.controller->controllers()->devices().size(), 2);
  }

  void controllersPageGamepadFlow() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    h.gamepads = true;
    QVERIFY(h.start());
    ControllersController* c = h.controller->controllers();
    QVERIFY2(c->gamepadAvailable(), qPrintable(c->gamepadNote()));
    pair(h, hub);
    QVERIFY(hub.lastHandshakeBody.value(QStringLiteral("input")).toObject().value(QStringLiteral("gamepad")).toBool());  // reflects availability

    h.controller->showControllers();   // a section of Settings
    QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
    QTRY_VERIFY(h.item("controllerTabs") != nullptr && h.item("controllerTabs")->isVisible());
    QCOMPARE(c->devices().size(), 2);  // Keyboard, Mouse
    QCOMPARE(c->selectedDevice(), QStringLiteral("keyboard"));
    QCOMPARE(text(h, "profilesLocalFooter"), QStringLiteral("Mapping is saved per controller on this PC."));
    QCOMPARE(c->rows().size(), 12);  // the rows of the default (first) system, nds: the 12 DS inputs; no ZL/ZR, no circle pad / C-stick aliases (P33)
    QVERIFY(inputRow(h, QStringLiteral("zl")).isEmpty() && inputRow(h, QStringLiteral("lup")).isEmpty() && inputRow(h, QStringLiteral("cup")).isEmpty());
    QVERIFY(!c->supportsLid());  // no LID row: the nds profiles have no lid input
    c->setSystemLabels(QStringLiteral("GB"), QString());  // a system without touch screen: no Mouse device
    QVERIFY(!c->hasTouch());
    QCOMPARE(c->devices().size(), 1);
    c->setSystemLabels(QStringLiteral("NDS"), QStringLiteral("DS touch"));
    QCOMPARE(c->devices().size(), 2);
    QCOMPARE(inputRow(h, QStringLiteral("a")).value(QStringLiteral("binding")).toString(), QStringLiteral("X"));
    QCOMPARE(deviceRow(h, QStringLiteral("keyboard")).value(QStringLiteral("slot")).toString(), QStringLiteral("P1"));  // no pad yet

    // Hotplug: the first connected gamepad is P1, the keyboard gives up the slot.
    VirtualPad pad("Uzume Test Pad");
    QVERIFY(pad.js != nullptr);
    c->gamepads()->poll();
    const QVariantMap padRow = deviceRow(h, QStringLiteral("gamepad"));
    QCOMPARE(padRow.value(QStringLiteral("name")).toString(), QStringLiteral("Uzume Test Pad"));
    QCOMPARE(padRow.value(QStringLiteral("slot")).toString(), QStringLiteral("P1"));
    QCOMPARE(padRow.value(QStringLiteral("profile")).toString(), QStringLiteral("Standard Gamepad"));
    QCOMPARE(deviceRow(h, QStringLiteral("keyboard")).value(QStringLiteral("slot")).toString(), QStringLiteral("—"));
    QCOMPARE(deviceRow(h, QStringLiteral("mouse")).value(QStringLiteral("profile")).toString(), QStringLiteral("DS touch"));
    const QString padKey = padRow.value(QStringLiteral("key")).toString();
    QVERIFY(h.click("tab_Uzume Test Pad"));
    QCOMPARE(c->selectedDevice(), padKey);
    QCOMPARE(h.item("controllerTabs")->property("active").toString(), QStringLiteral("Uzume Test Pad"));
    QVERIFY(c->profileBuiltin());
    QVERIFY(visible(h, "builtinNote"));
    QCOMPARE(c->labelSet(), QStringLiteral("generic"));  // not an Xbox/PlayStation pad: names of the emulated system
    QCOMPARE(text(h, "mapText_a"), QStringLiteral("A"));       // NDS A on the east button, generic set = DS names
    QCOMPARE(inputRow(h, QStringLiteral("a")).value(QStringLiteral("binding")).toString(), QStringLiteral("B"));  // token label
    QCOMPARE(text(h, "mapText_up"), QStringLiteral("D-pad"));
    // Built-in profiles are read-only.
    c->beginCapture(QStringLiteral("a"));
    QVERIFY(c->listening().isEmpty());
    uitest::saveShot(h.window, QStringLiteral("5-controllers"));

    // Input test: live tiles and the merged joypad mask of the running game path (a column from 1440 px on).
    h.window->resize(1600, 900);
    QTest::qWait(100);
    QVERIFY(h.item("testTile_a") != nullptr);
    QVERIFY(h.item("testTile_a")->isVisible());
    // The input test draws only the buttons the viewed system has: the DS has L / R but no ZL / ZR (R-P15).
    QVERIFY(h.item("testTile_l") != nullptr && h.item("testTile_l")->isVisible());
    QVERIFY(h.item("testTile_zl") != nullptr && !h.item("testTile_zl")->isVisible());
    QVERIFY(h.item("testTile_zr") != nullptr && !h.item("testTile_zr")->isVisible());
    QVERIFY(!h.item("testTile_a")->property("active").toBool());
    pad.button(East, true);
    c->gamepads()->poll();
    QVERIFY(h.item("testTile_a")->property("active").toBool());
    QVERIFY(!h.item("testTile_b")->property("active").toBool());
    QCOMPARE(h.controller->gameSession()->joypadMask(), kA);  // gamepad -> Uzume input -> joypad mask
    pad.button(DUp, true);
    c->gamepads()->poll();
    QVERIFY(h.item("testTile_up")->property("active").toBool());
    QCOMPARE(h.controller->gameSession()->joypadMask(), kA | kUp);
    // Keyboard and gamepad merge.
    QVERIFY(h.controller->gameSession()->keyEvent(Qt::Key_Z, true));
    QCOMPARE(h.controller->gameSession()->joypadMask(), kA | kUp | kB);
    h.controller->gameSession()->keyEvent(Qt::Key_Z, false);
    pad.button(East, false);
    pad.button(DUp, false);
    c->gamepads()->poll();
    QCOMPARE(h.controller->gameSession()->joypadMask(), 0u);
    // Tall enough that the whole page is in view (the pad hint row takes the bottom 44 px while the pad was the last input).
    h.window->resize(1280, 1200);
    QTest::qWait(150);
    QQuickTest::qWaitForPolish(h.window);

    // Duplicate -> own profile
    // The copy is assigned to this device; remap by "Press a button…".
    QVERIFY(h.click("duplicateProfileButton"));
    QVERIFY(!c->profileBuiltin());
    QCOMPARE(c->profileName(), QStringLiteral("Standard Gamepad copy"));
    QCOMPARE(deviceRow(h, QStringLiteral("gamepad")).value(QStringLiteral("profile")).toString(), QStringLiteral("Standard Gamepad copy"));
    // Readable pass 7: the bound key is shown once. "A" on the A button is the key frame alone, never "[A] A", and a row that still
    // has its default shows neither "Default: …" nor Reset.
    QVERIFY(visible(h, "mapGlyphs_a"));
    QVERIFY(!visible(h, "mapText_a"));
    QVERIFY(!visible(h, "mapReset_a") && !visible(h, "mapDefault_a"));
    QVERIFY(h.click("mapField_a"));
    QCOMPARE(c->listening(), QStringLiteral("a"));
    QCOMPARE(text(h, "mapText_a"), QStringLiteral("Press a button…"));
    pad.button(LB, true);
    c->gamepads()->poll();
    QVERIFY(c->listening().isEmpty());
    QCOMPARE(text(h, "mapText_a"), QStringLiteral("L"));   // left shoulder = DS "L" in the generic set
    // The changed row names its default and offers Reset; the untouched row b offers neither.
    QVERIFY(visible(h, "mapReset_a"));
    QVERIFY(visible(h, "mapDefault_a"));
    QCOMPARE(text(h, "mapDefault_a"), QStringLiteral("Default: A"));
    QVERIFY(!visible(h, "mapReset_b") && !visible(h, "mapDefault_b"));
    QCOMPARE(text(h, "mapText_l"), QStringLiteral("Unassigned"));  // LB, taken away from L (a token drives one input)
    pad.button(LB, false);
    c->gamepads()->poll();
    pad.button(LB, true);  // remapped profile drives the game
    c->gamepads()->poll();
    QCOMPARE(h.controller->gameSession()->joypadMask(), kA);
    pad.button(LB, false);
    pad.button(East, true);  // east was A, now it is nothing in this profile? (it stays on no input)
    c->gamepads()->poll();
    QCOMPARE(h.controller->gameSession()->joypadMask(), 0u);
    pad.button(East, false);
    c->gamepads()->poll();
    // Not mapped state, clear.
    QVERIFY(h.click("mapClear_start"));
    QCOMPARE(text(h, "mapText_start"), QStringLiteral("Unassigned"));
    QVERIFY(!inputRow(h, QStringLiteral("start")).value(QStringLiteral("mapped")).toBool());
    // Escape cancels a capture on the keyboard only; for pads a second tap on the field cancels.
    QVERIFY(h.click("mapField_b"));
    QCOMPARE(c->listening(), QStringLiteral("b"));
    QVERIFY(h.click("mapField_b"));
    QVERIFY(c->listening().isEmpty());

    // Profile persisted locally in <data>/settings/controllers.json
    {
      ControllerProfiles reread(h.controller->profileStore()->baseDir());
      QCOMPARE(reread.all().size(), 3);
      QVERIFY(reread.filePath().endsWith(QStringLiteral("settings/controllers.json")));
    }
    // Reset to default restores the built-in bindings; rename; delete (two-step) falls back to the built-in profile.
    QVERIFY(h.click("resetProfileButton"));
    QCOMPARE(text(h, "mapText_a"), QStringLiteral("A"));
    c->renameProfile(QStringLiteral("Couch pad"));
    QCOMPARE(c->profileName(), QStringLiteral("Couch pad"));
    QVERIFY(h.click("deleteProfileButton"));
    QCOMPARE(c->profileName(), QStringLiteral("Couch pad"));  // asked to confirm first
    QVERIFY(h.click("deleteProfileButton"));
    QCOMPARE(c->profileName(), QStringLiteral("Standard Gamepad"));
    QVERIFY(c->profileBuiltin());

    // Unplug: the device disappears and the selection falls back.
    pad.detach();
    c->gamepads()->poll();
    QVERIFY(deviceRow(h, QStringLiteral("gamepad")).isEmpty());
    QCOMPARE(c->selectedDevice(), QStringLiteral("keyboard"));
    QCOMPARE(deviceRow(h, QStringLiteral("keyboard")).value(QStringLiteral("slot")).toString(), QStringLiteral("P1"));
  }

  void settingsHotkeysSection() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    ControllersController* c = h.controller->controllers();
    GameSession* s = h.controller->gameSession();
    h.controller->showControllers();
    h.window->resize(940, 800);  // narrow main column, like Windows with wide fallback fonts
    QTest::qWait(100);
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(!visible(h, "hotkeysInfo"));   // Controls does not show the hotkeys
    h.controller->showSettingsSection(QStringLiteral("hotkeys"));
    QCOMPARE(h.controller->screen(), QStringLiteral("settings"));
    QTRY_VERIFY(visible(h, "hotkeysInfo"));
    QCOMPARE(text(h, "hotkeysInfo"), QStringLiteral("Uzume hotkeys work on the keyboard and on a gamepad (hold the hotkey button, then press another one) and are never sent to the game."));
    QCOMPARE(c->hotkeyRows().size(), 13);  // twelve actions + the fixed Esc row
    QCOMPARE(text(h, "hotkeyText_fullscreen"), QStringLiteral("F11"));
    QCOMPARE(text(h, "hotkeyText_diagnostics"), QStringLiteral("F3"));
    QCOMPARE(text(h, "hotkeyText_snapshot"), QStringLiteral("F5"));
    QCOMPARE(text(h, "hotkeyText_escape"), QStringLiteral("Esc"));
    QCOMPARE(text(h, "hotkeyText_screenshot"), QStringLiteral("F12"));
    QCOMPARE(text(h, "hotkeyPadText_screenshot"), QStringLiteral("Select+Y"));
    QCOMPARE(text(h, "hotkeyPadText_fullscreen"), QStringLiteral("Select+B"));  // Uzume hotkeys get gamepad defaults (P36, design 9b-1)
    QCOMPARE(text(h, "hotkeyPadText_diagnostics"), QStringLiteral("Select+R3"));
    QCOMPARE(text(h, "hotkeyPadText_loadState"), QStringLiteral("Select+LB"));
    QCOMPARE(text(h, "hotkeyPadText_saveState"), QStringLiteral("Select+RB"));
    QCOMPARE(text(h, "hotkeyPadText_speedup"), QStringLiteral("Select+RT"));
    QCOMPARE(text(h, "padModifierText"), QStringLiteral("Select"));
    QVERIFY(visible(h, "hotkeyPadField_escape"));  // fixed, read-only field (no capture)
    QVERIFY(!visible(h, "resetChanged"));
    // The fixed Esc row cannot be captured.
    QVERIFY(h.click("hotkeyField_escape"));
    QVERIFY(c->hotkeyListening().isEmpty());

    // Capture: Escape cancels, a key used by another action is rejected (binding stays, note), a free key is taken.
    QVERIFY(h.click("hotkeyField_fullscreen"));
    QCOMPARE(text(h, "hotkeyText_fullscreen"), QStringLiteral("Press a key…"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QVERIFY(c->hotkeyListening().isEmpty());
    QCOMPARE(text(h, "hotkeyText_fullscreen"), QStringLiteral("F11"));
    QVERIFY(h.click("hotkeyField_fullscreen"));
    QTest::keyClick(h.window, Qt::Key_F3);
    QCOMPARE(c->hotkeyNote(), QStringLiteral("Already used by Diagnostics overlay"));
    QVERIFY(visible(h, "hotkeyNote"));
    QCOMPARE(c->hotkeyListening(), QStringLiteral("fullscreen"));
    QCOMPARE(c->hotkeyAction(Qt::Key_F11), QStringLiteral("fullscreen"));
    QTest::keyClick(h.window, Qt::Key_F10);
    QVERIFY(c->hotkeyListening().isEmpty());
    QVERIFY(c->hotkeyNote().isEmpty());
    QCOMPARE(text(h, "hotkeyText_fullscreen"), QStringLiteral("F10"));
    QVERIFY(visible(h, "hotkeyChangedDot_fullscreen"));
    QCOMPARE(c->hotkeysChangedCount(), 1);
    QVERIFY(visible(h, "resetChanged"));
    QCOMPARE(c->hotkeyLabels().value(QStringLiteral("fullscreen")).toString(), QStringLiteral("F10"));
    // The Player resolves keys through the configured actions.
    QCOMPARE(c->hotkeyAction(Qt::Key_F10), QStringLiteral("fullscreen"));
    QCOMPARE(c->hotkeyAction(Qt::Key_F11), QString());
    QVERIFY(s->isReservedKey(Qt::Key_F10));
    QVERIFY(!s->isReservedKey(Qt::Key_F11));
    QVERIFY(s->isReservedKey(Qt::Key_Escape));

    // A hotkey wins over the keyboard profile: Z is B in the standard keyboard profile.
    QVERIFY(s->keyEvent(Qt::Key_Z, true));
    s->keyEvent(Qt::Key_Z, false);
    QVERIFY(h.click("hotkeyField_snapshot"));
    QTest::keyClick(h.window, Qt::Key_Z);
    QCOMPARE(text(h, "hotkeyText_snapshot"), QStringLiteral("Z"));
    QCOMPARE(c->hotkeyAction(Qt::Key_Z), QStringLiteral("snapshot"));
    QCOMPARE(c->hotkeyRows().at(2).toMap().value(QStringLiteral("conflictInput")).toString(), QStringLiteral("B"));
    QVERIFY(visible(h, "hotkeyConflict_snapshot"));
    QQuickTest::qWaitForPolish(h.window);
    {
      // Nothing of the Hotkeys table lies outside the main column (the flickable).
      QQuickItem* flick = h.item("hotkeysInfo");
      while (flick && !flick->inherits("QQuickFlickable")) flick = flick->parentItem();
      QVERIFY(flick != nullptr);
      const QRectF column = flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()));
      QVERIFY2(h.item("hotkeysInfo")->width() <= column.width() - 96 + 0.5,
               qPrintable(QStringLiteral("hotkeysInfo wider than the content column (%1)\n%2").arg(column.width() - 96).arg(hotkeysLayoutDump(h))));
      for (const char* name : {"hotkeyField_fullscreen", "hotkeyField_snapshot", "hotkeyField_escape", "hotkeyReset_fullscreen",
                               "hotkeyReset_snapshot", "hotkeyConflict_snapshot", "hotkeyText_snapshot"}) {
        QQuickItem* it = h.item(name);
        QVERIFY2(it != nullptr && it->isVisible(), name);
        const QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
        QVERIFY2(r.left() >= column.left() - 0.5 && r.right() <= column.right() + 0.5,
                 qPrintable(QStringLiteral("%1 outside the column: %2..%3 vs %4..%5\n%6").arg(QLatin1String(name)).arg(r.left()).arg(r.right()).arg(column.left()).arg(column.right()).arg(hotkeysLayoutDump(h))));
      }
    }
    QVERIFY(!c->keyboardMap().contains(Qt::Key_Z));
    QVERIFY(!s->keyEvent(Qt::Key_Z, true));
    QCOMPARE(s->joypadMask(), 0u);
    // Unassigned: no key, no hint.
    QVERIFY(h.click("hotkeyClear_diagnostics"));
    QCOMPARE(text(h, "hotkeyText_diagnostics"), QStringLiteral("not set"));
    QCOMPARE(c->hotkeyLabels().value(QStringLiteral("diagnostics")).toString(), QString());
    QVERIFY(!s->isReservedKey(Qt::Key_F3));

    // Persisted, then reset one / all.
    {
      ControllerProfiles reread(h.controller->profileStore()->baseDir());
      QCOMPARE(reread.hotkey(QStringLiteral("fullscreen")), int(Qt::Key_F10));
      QCOMPARE(reread.hotkey(QStringLiteral("diagnostics")), 0);
    }
    QCOMPARE(c->hotkeysChangedCount(), 3);
    QVERIFY(h.click("hotkeyReset_fullscreen"));
    QCOMPARE(text(h, "hotkeyText_fullscreen"), QStringLiteral("F11"));
    QCOMPARE(c->hotkeysChangedCount(), 2);
    QVERIFY(h.click("resetChanged"));
    QCOMPARE(c->hotkeysChangedCount(), 0);
    QCOMPARE(text(h, "hotkeyText_snapshot"), QStringLiteral("F5"));
    QVERIFY(s->isReservedKey(Qt::Key_F3));
    QVERIFY(c->keyboardMap().contains(Qt::Key_Z));  // the profile key is back
    // Back on Controls the mapping table is shown again.
    h.controller->showControllers();
    QTRY_VERIFY(visible(h, "mapField_a"));
    QVERIFY(!visible(h, "hotkeysInfo"));
  }

  void controllersGamepadHotkeys() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    h.gamepads = true;
    QVERIFY(h.start());
    pair(h, hub);
    ControllersController* c = h.controller->controllers();
    QVERIFY2(c->gamepadAvailable(), qPrintable(c->gamepadNote()));
    GameSession* s = h.controller->gameSession();
    h.controller->showSettingsSection(QStringLiteral("hotkeys"));
    h.window->resize(940, 800);
    QTest::qWait(100);
    QTRY_VERIFY(visible(h, "hotkeysInfo"));
    VirtualPad pad("Uzume Hotkey Pad");
    QVERIFY(pad.js != nullptr);
    c->gamepads()->poll();
    const auto poll = [&]() { c->gamepads()->poll(); QTest::qWait(1); };
    const auto storedPad = [&](const QString& action) {
      ControllerProfiles reread(h.controller->profileStore()->baseDir());
      return reread.padHotkey(action);
    };
    const auto rowPad = [&](const QString& action) {
      for (const QVariant& v : c->hotkeyRows()) {
        if (v.toMap().value(QStringLiteral("action")).toString() == action) return v.toMap().value(QStringLiteral("pad")).toString();
      }
      return QString();
    };

    // Capture: the hotkey button alone waits for the second button, a used button is rejected, a free one is taken as combo.
    QVERIFY(h.click("hotkeyPadField_fullscreen"));
    QCOMPARE(c->padHotkeyListening(), QStringLiteral("fullscreen"));
    QCOMPARE(text(h, "hotkeyPadText_fullscreen"), QStringLiteral("Press a button…"));
    pad.button(Back, true);
    poll();
    QCOMPARE(c->padHotkeyListening(), QStringLiteral("fullscreen"));
    pad.button(LB, true);  // used by Load state
    poll();
    QCOMPARE(c->hotkeyNote(), QStringLiteral("Already used by Load state"));
    QVERIFY(visible(h, "hotkeyNote"));
    QCOMPARE(c->padHotkeyListening(), QStringLiteral("fullscreen"));
    pad.button(LB, false);
    poll();
    pad.button(South, true);
    poll();
    QVERIFY(c->padHotkeyListening().isEmpty());
    QCOMPARE(storedPad(QStringLiteral("fullscreen")), QStringLiteral("pad:a"));
    QCOMPARE(text(h, "hotkeyPadText_fullscreen"), QStringLiteral("Select+A"));
    QVERIFY(visible(h, "hotkeyChangedDot_fullscreen"));
    QCOMPARE(c->hotkeysChangedCount(), 1);
    pad.button(South, false);
    pad.button(Back, false);
    poll();

    // A button pressed alone becomes a solo binding; Esc cancels; clear / reset.
    QVERIFY(h.click("hotkeyPadField_diagnostics"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QVERIFY(c->padHotkeyListening().isEmpty());
    QCOMPARE(rowPad(QStringLiteral("diagnostics")), QStringLiteral("Select+R3"));
    QVERIFY(h.click("hotkeyPadField_diagnostics"));
    pad.button(Start, true);
    poll();
    QCOMPARE(storedPad(QStringLiteral("diagnostics")), QStringLiteral("solo:pad:start"));
    QCOMPARE(text(h, "hotkeyPadText_diagnostics"), QStringLiteral("Start"));
    pad.button(Start, false);
    poll();
    QVERIFY(h.click("hotkeyPadClear_diagnostics"));
    QCOMPARE(text(h, "hotkeyPadText_diagnostics"), QStringLiteral("not set"));
    QVERIFY(h.click("hotkeyReset_diagnostics"));
    QCOMPARE(text(h, "hotkeyPadText_diagnostics"), QStringLiteral("Select+R3"));
    {
      ControllerProfiles reread(h.controller->profileStore()->baseDir());
      QCOMPARE(reread.padHotkey(QStringLiteral("fullscreen")), QStringLiteral("pad:a"));
    }

    // Change the hotkey button: Start; the combos follow.
    QVERIFY(h.click("padModifierField"));
    QCOMPARE(c->padHotkeyListening(), QStringLiteral("modifier"));
    pad.button(Start, true);
    poll();
    pad.button(Start, false);
    poll();
    QCOMPARE(text(h, "padModifierText"), QStringLiteral("Start"));
    QCOMPARE(text(h, "hotkeyPadText_speedup"), QStringLiteral("Start+RT"));
    QVERIFY(visible(h, "padModifierReset"));
    QVERIFY(h.click("padModifierReset"));
    QCOMPARE(text(h, "padModifierText"), QStringLiteral("Select"));

    // In play: the combo fires once, RT never reaches the core, Select still does; the hint follows the last input.
    QSignalSpy fired(c, &ControllersController::padHotkeyTriggered);
    c->noteKeyboardInput();
    QCOMPARE(c->hotkeyHints().value(QStringLiteral("speedup")).toString(), QStringLiteral("Space"));
    pad.button(Back, true);
    poll();
    pad.axis(5, 30000);   // right trigger
    poll();
    poll();
    QCOMPARE(fired.count(), 1);
    QCOMPARE(fired.first().first().toString(), QStringLiteral("speedup"));
    QCOMPARE(s->joypadMask(), 1u << 2);  // Select only
    QCOMPARE(c->hotkeyHints().value(QStringLiteral("speedup")).toString(), QStringLiteral("Select+RT"));
    pad.axis(5, 0);
    pad.button(Back, false);
    poll();
    QCOMPARE(s->joypadMask(), 0u);
    c->noteKeyboardInput();
    QCOMPARE(c->hotkeyHints().value(QStringLiteral("speedup")).toString(), QStringLiteral("Space"));

    // Reset all brings back keyboard keys and gamepad buttons.
    QVERIFY(h.click("resetChanged"));
    QCOMPARE(c->hotkeysChangedCount(), 0);
    QCOMPARE(text(h, "hotkeyPadText_fullscreen"), QStringLiteral("Select+B"));
  }

  // Leftover: picking a system chip moves the mapping table, the labels and the input test together.
  void controllersSystemChipFollowsMapping() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    ControllersController* c = h.controller->controllers();
    h.controller->showControllers();
    const QVariantList systems = c->systems();
    QVERIFY(systems.size() >= 2);
    const QString first = systems.first().toMap().value(QStringLiteral("id")).toString();
    QString other;
    QStringList firstTargets;
    for (const QVariant& r : c->rows()) firstTargets << r.toMap().value(QStringLiteral("target")).toString();
    for (const QVariant& v : systems) {  // a system whose table differs from the first one's
      const QString id = v.toMap().value(QStringLiteral("id")).toString();
      if (id == first) continue;
      QSignalSpy rows(c, &ControllersController::rowsChanged);
      QSignalSpy test(c, &ControllersController::testChanged);
      c->selectSystem(id);
      QCOMPARE(c->viewSystem(), id);
      QVERIFY(rows.count() >= 1);
      QVERIFY(test.count() >= 1);
      QStringList targets;
      for (const QVariant& r : c->rows()) targets << r.toMap().value(QStringLiteral("target")).toString();
      if (targets != firstTargets) {
        other = id;
        break;
      }
    }
    QVERIFY2(!other.isEmpty(), "no system with a different mapping table");
    c->selectSystem(first);
    QStringList back;
    for (const QVariant& r : c->rows()) back << r.toMap().value(QStringLiteral("target")).toString();
    QCOMPARE(back, firstTargets);
  }

  void controllersDuplicateToEdit() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    ControllersController* c = h.controller->controllers();
    h.controller->showControllers();
    QVERIFY(c->profileBuiltin());
    QVERIFY(visible(h, "duplicateToEditButton"));
    QVERIFY(h.click("duplicateToEditButton"));
    QVERIFY(!c->profileBuiltin());
    QCOMPARE(c->profileName(), QStringLiteral("Keyboard · Standard copy"));
    QVERIFY(!visible(h, "duplicateToEditButton"));
    QVERIFY(!visible(h, "builtinNote"));
    // Clicking a field of a built-in profile duplicates and starts the capture on the copy.
    c->selectProfile(QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId));
    QVERIFY(c->profileBuiltin());
    QVERIFY(h.click("mapField_a"));
    QVERIFY(!c->profileBuiltin());
    QCOMPARE(c->profileName(), QStringLiteral("Keyboard · Standard copy 2"));
    QCOMPARE(c->listening(), QStringLiteral("a"));
    QCOMPARE(text(h, "mapText_a"), QStringLiteral("Press a key…"));
    QTest::keyClick(h.window, Qt::Key_J);
    QCOMPARE(inputRow(h, QStringLiteral("a")).value(QStringLiteral("glyphs")).toStringList(), QStringList{QStringLiteral("J")});  // key names
    // The built-in profile stays untouched and read-only.
    const auto builtin = c->profileStore()->find(QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId));
    QVERIFY(builtin && builtin->builtin);
    QCOMPARE(builtin->bindings.value(QStringLiteral("a")), QStringList{keyToken(Qt::Key_X)});
  }

  void controllersKeyboardProfileAndMouse() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    ControllersController* c = h.controller->controllers();
    h.controller->showControllers();
    h.window->resize(1600, 900);   // the input test is a column from 1440 px on
    QTest::qWait(100);
    QCOMPARE(c->profileName(), QStringLiteral("Keyboard · Standard"));

    // Input test with the keyboard: X lights up A, the arrow lights up the D-pad.
    QTest::keyPress(h.window, Qt::Key_X);
    QVERIFY(h.item("testTile_a")->property("active").toBool());
    QTest::keyRelease(h.window, Qt::Key_X);
    QVERIFY(!h.item("testTile_a")->property("active").toBool());
    QTest::keyPress(h.window, Qt::Key_Left);
    QVERIFY(h.item("testTile_left")->property("active").toBool());
    QTest::keyRelease(h.window, Qt::Key_Left);

    // Duplicate, remap A to J (keyboard capture), Escape cancels a capture.
    QVERIFY(h.click("duplicateProfileButton"));
    QVERIFY(h.click("mapField_a"));
    QCOMPARE(text(h, "mapText_a"), QStringLiteral("Press a key…"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QVERIFY(c->listening().isEmpty());
    QVERIFY(h.click("mapField_a"));
    QTest::keyClick(h.window, Qt::Key_J);
    QCOMPARE(inputRow(h, QStringLiteral("a")).value(QStringLiteral("glyphs")).toStringList(), QStringList{QStringLiteral("J")});  // key names
    // Taking X away from nothing else; X is now unmapped, J is A in the game and in the test panel.
    GameSession* s = h.controller->gameSession();
    QVERIFY(!s->keyEvent(Qt::Key_X, true));
    QVERIFY(s->keyEvent(Qt::Key_J, true));
    QCOMPARE(s->joypadMask(), kA);
    s->keyEvent(Qt::Key_J, false);
    QCOMPARE(s->joypadMask(), 0u);
    QVERIFY(s->keyEvent(Qt::Key_Up, true));  // untouched keys keep working
    QCOMPARE(s->joypadMask(), kUp);
    s->keyEvent(Qt::Key_Up, false);
    QTest::keyPress(h.window, Qt::Key_J);
    QVERIFY(h.item("testTile_a")->property("active").toBool());
    QTest::keyRelease(h.window, Qt::Key_J);
    // Built-in keyboard profile is untouched (read-only).
    c->selectProfile(QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId));
    QCOMPARE(inputRow(h, QStringLiteral("a")).value(QStringLiteral("glyphs")).toStringList(), QStringList{QStringLiteral("X")});
    QVERIFY(visible(h, "builtinNote"));
    QTest::keyClick(h.window, Qt::Key_Q);  // not capturing: only feeds the test

    // Mouse = DS touch: fixed, no profile, no mapping table.
    QVERIFY(h.click("tab_Mouse"));
    QCOMPARE(c->rows().size(), 0);
    QVERIFY(visible(h, "mouseNote"));
    QVERIFY(!visible(h, "profileSelect"));
    QVERIFY(c->activeInputs().isEmpty());
  }

  // Both pages in the light palette (tokens only, no hardcoded colors) load without QML warnings.
  void pagesInLightAndDark() {
    FakeHub hub(QStringLiteral("a"));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    h.controller->emulation()->setCoreProbe(QStringLiteral("melondsds"), fakeProbe(), false);
    for (const QString& mode : {QStringLiteral("light"), QStringLiteral("dark")}) {
      h.controller->setAppearance(mode);
      h.controller->showEmulation();
      QTest::qWait(50);
      uitest::saveShot(h.window, QStringLiteral("5-emulation-") + mode);
      h.controller->showControllers();
      QTest::qWait(50);
      uitest::saveShot(h.window, QStringLiteral("5-controllers-") + mode);
    }
  }
};

UITEST_MAIN(EmulationControllersTest)
#include "emulation_controllers_test.moc"
