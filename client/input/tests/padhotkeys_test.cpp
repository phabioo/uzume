// Gamepad hotkeys: pure combo detection (PadHotkeyDetector) and the GamepadService integration with SDL virtual joysticks
// (fires once per press, combo buttons never reach the core, the hotkey button still does). No hardware.
#include <SDL3/SDL.h>

#include <QSignalSpy>
#include <QtTest>

#include "gamepadservice.h"
#include "inputmap.h"
#include "padhotkeys.h"
#include "system_manifest.h"

using namespace uzume;
using namespace uzume::input;

namespace {
const QString kBack = QStringLiteral("pad:back");
const QString kRb = QStringLiteral("pad:rightshoulder");
const QString kLb = QStringLiteral("pad:leftshoulder");
const QString kPaddle = QStringLiteral("pad:x");

QList<PadHotkeyBinding> defaults() {
  return {{QStringLiteral("speedup"), kRb, false}, {QStringLiteral("snapshot"), kLb, false}, {QStringLiteral("fullscreen"), kPaddle, true}};
}

enum Btn { South = 0, East, West, North, Back, Guide, Start, LS, RS, LB, RB };

struct VirtualPad {
  SDL_JoystickID id = 0;
  SDL_Joystick* js = nullptr;
  VirtualPad() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 6;
    desc.nbuttons = 15;
    desc.name = "Uzume hotkey pad";
    desc.vendor_id = 0x1209;
    desc.product_id = 0x4a00;
    id = SDL_AttachVirtualJoystick(&desc);
    if (id != 0) js = SDL_OpenJoystick(id);
  }
  ~VirtualPad() {
    if (js) SDL_CloseJoystick(js);
    if (id != 0) SDL_DetachVirtualJoystick(id);
  }
  void button(int b, bool down) { SDL_SetJoystickVirtualButton(js, b, down); }
};
}  // namespace

class PadHotkeysTest : public QObject {
  Q_OBJECT
 private slots:
  void releasedReportsTheLetGoOfAHoldHotkey() {
    const QString kLt = QStringLiteral("pad:lefttrigger");
    PadHotkeyDetector d;
    d.setBindings(kBack, {{QStringLiteral("rewind"), kLt, false}});
    QVERIFY(d.update({kBack}).isEmpty());
    QVERIFY(d.released().isEmpty());
    QCOMPARE(d.update({kBack, kLt}), QStringList{QStringLiteral("rewind")});
    QVERIFY(d.released().isEmpty());
    QVERIFY(d.update({kBack, kLt}).isEmpty());   // held
    QVERIFY(d.released().isEmpty());
    d.update({kBack});                           // the trigger is let go: rewind stops
    QCOMPARE(d.released(), QStringList{QStringLiteral("rewind")});
    d.update({kBack});
    QVERIFY(d.released().isEmpty());             // reported once
    // letting go of the modifier first: the trigger stays the rewind button until it is let go too
    d.update({kBack, kLt});
    d.update({kLt});
    QVERIFY(d.released().isEmpty());
    d.update({});
    QCOMPARE(d.released(), QStringList{QStringLiteral("rewind")});
  }

  void comboFiresOncePerPress() {
    PadHotkeyDetector d;
    d.setBindings(kBack, defaults());
    QVERIFY(d.update({}).isEmpty());
    QVERIFY(d.update({kBack}).isEmpty());                                 // the hotkey button alone fires nothing
    QCOMPARE(d.update({kBack, kRb}), QStringList{QStringLiteral("speedup")});
    QVERIFY(d.suppressed().contains(kRb));
    QVERIFY(!d.suppressed().contains(kBack));                             // the hotkey button itself still reaches the core
    QVERIFY(d.update({kBack, kRb}).isEmpty());                            // held: no repeat
    QVERIFY(d.update({kBack, kRb}).isEmpty());
    QVERIFY(d.update({kRb}).isEmpty());                                   // modifier released first: RB stays hidden
    QVERIFY(d.suppressed().contains(kRb));
    QVERIFY(d.update({}).isEmpty());
    QVERIFY(d.suppressed().isEmpty());                                    // released: visible again
    QVERIFY(d.update({kRb}).isEmpty());                                   // RB without the hotkey button is a normal button
    QVERIFY(d.suppressed().isEmpty());
    d.update({});
    QCOMPARE(d.update({kBack, kRb}).size(), 1);                           // a second press fires again
  }

  void heldButtonAndSimultaneousPress() {
    PadHotkeyDetector d;
    d.setBindings(kBack, defaults());
    d.update({kRb});
    QVERIFY(d.update({kRb, kBack}).isEmpty());  // RB was held first: playing, not a hotkey
    QVERIFY(d.suppressed().isEmpty());
    d.update({});
    QCOMPARE(d.update({kBack, kLb, kRb}), (QStringList{QStringLiteral("speedup"), QStringLiteral("snapshot")}));  // same poll
    QVERIFY(d.suppressed().contains(kLb) && d.suppressed().contains(kRb));
  }

  void soloAndRebinding() {
    PadHotkeyDetector d;
    d.setBindings(kBack, defaults());
    QCOMPARE(d.update({kPaddle}), QStringList{QStringLiteral("fullscreen")});  // no modifier needed
    QVERIFY(d.suppressed().contains(kPaddle));
    QVERIFY(d.update({kPaddle}).isEmpty());
    QVERIFY(d.update({}).isEmpty());
    QVERIFY(d.suppressed().isEmpty());
    // Another hotkey button.
    d.setBindings(QStringLiteral("pad:start"), {{QStringLiteral("speedup"), kRb, false}});
    d.update({});
    QVERIFY(d.update({kBack, kRb}).isEmpty());  // the old hotkey button does nothing now
    d.update({});
    QCOMPARE(d.update({QStringLiteral("pad:start"), kRb}).size(), 1);
  }
};

class PadHotkeysServiceTest : public QObject {
  Q_OBJECT
 private slots:
  void initTestCase() {  // loading the manifests registers the input tables
    uzume::emu::ManifestRegistry reg;
    QVERIFY(reg.loadBuiltin());
  }
  void comboNeverReachesTheCore() {
    GamepadService svc;
    QVERIFY2(svc.start(0), qPrintable(svc.error()));
    VirtualPad pad;
    QVERIFY(pad.js != nullptr);
    svc.setPadHotkeys(kBack, defaults());
    svc.poll();
    QCOMPARE(svc.devices().size(), 1);
    QSignalSpy fired(&svc, &GamepadService::hotkeyTriggered);
    constexpr quint32 kSelect = 1u << 2, kR = 1u << 11;

    pad.button(Back, true);
    svc.poll();
    QCOMPARE(svc.libretroMask(), kSelect);  // the hotkey button is Select for the game
    QCOMPARE(fired.count(), 0);
    pad.button(RB, true);
    svc.poll();
    QCOMPARE(fired.count(), 1);
    QCOMPARE(fired.last().first().toString(), QStringLiteral("speedup"));
    QCOMPARE(svc.libretroMask(), kSelect);  // R is suppressed ...
    QVERIFY(svc.inputMaskFor(svc.devices().first().id) != svc.inputMask());  // ... but the input test still sees it
    svc.poll();
    svc.poll();
    QCOMPARE(fired.count(), 1);
    pad.button(Back, false);
    svc.poll();
    QCOMPARE(svc.libretroMask(), 0u);       // still held, still hidden
    pad.button(RB, false);
    svc.poll();
    pad.button(RB, true);                   // alone it is a normal R
    svc.poll();
    QCOMPARE(svc.libretroMask(), kR);
    QCOMPARE(fired.count(), 1);
    pad.button(RB, false);
    svc.poll();

    // A running capture neither fires nor suppresses, and the buttons held when it ends do not fire afterwards.
    pad.button(Back, true);
    svc.poll();
    svc.startCapture();
    pad.button(LB, true);
    svc.poll();                             // captured here
    QVERIFY(!svc.capturing());
    QCOMPARE(fired.count(), 1);
    svc.poll();
    QCOMPARE(fired.count(), 1);
    pad.button(LB, false);
    pad.button(Back, false);
    svc.poll();
    QCOMPARE(svc.libretroMask(), 0u);
  }
};

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  int rc = 0;
  PadHotkeysTest a;
  rc |= QTest::qExec(&a, argc, argv);
  PadHotkeysServiceTest b;
  rc |= QTest::qExec(&b, argc, argv);
  return rc;
}

#include "padhotkeys_test.moc"
