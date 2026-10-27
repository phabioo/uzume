#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "controllerprofiles.h"

using namespace uzume;

class ControllerProfilesTest : public QObject {
  Q_OBJECT
 private slots:
  void builtinProfilesAreReadOnlyDefaults() {
    QTemporaryDir dir;
    ControllerProfiles p(dir.path());
    QCOMPARE(p.all().size(), 2);
    const auto pad = p.find(QString::fromLatin1(ControllerProfiles::kBuiltinGamepadId));
    QVERIFY(pad && pad->builtin);
    QCOMPARE(pad->name, QStringLiteral("Standard Gamepad"));
    QCOMPARE(pad->bindings.value(QStringLiteral("a")), QStringList{QStringLiteral("pad:b")});  // east button = NDS A
    const auto kb = p.find(QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId));
    QVERIFY(kb && kb->builtin);
    QCOMPARE(kb->name, QStringLiteral("Keyboard · Standard"));
    QCOMPARE(kb->bindings.value(QStringLiteral("a")), QStringList{keyToken(Qt::Key_X)});
    // read-only
    const QString id = pad->id;
    QVERIFY(!p.rename(id, QStringLiteral("x")));
    QVERIFY(!p.remove(id));
    QVERIFY(!p.setBinding(id, QStringLiteral("a"), {}));
    QVERIFY(!p.resetToDefault(id));
    QVERIFY(!QFile::exists(p.filePath()));  // nothing written until something changes
    // Every Uzume input has a default in both built-in profiles.
    for (const InputDef& in : uzumeInputs()) {
      QVERIFY2(pad->bindings.contains(in.id), qPrintable(in.id));
      QVERIFY2(kb->bindings.contains(in.id), qPrintable(in.id));
    }
  }

  void roundTrip() {
    QTemporaryDir dir;
    QString id;
    {
      ControllerProfiles p(dir.path());
      id = p.duplicate(QString::fromLatin1(ControllerProfiles::kBuiltinGamepadId), QStringLiteral("Mine"));
      QVERIFY(!id.isEmpty());
      QVERIFY(p.setBinding(id, QStringLiteral("a"), {padToken(QStringLiteral("x")), padToken(QStringLiteral("lefttrigger"))}));
      QVERIFY(p.setBinding(id, QStringLiteral("b"), {}));  // not mapped
      QVERIFY(p.assign(QStringLiteral("guid-1"), id));
      QVERIFY(p.assign(QString::fromLatin1(ControllerProfiles::kKeyboardDevice), QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId)));
    }
    ControllerProfiles p(dir.path());
    QCOMPARE(p.all().size(), 3);
    const auto mine = p.find(id);
    QVERIFY(mine && !mine->builtin);
    QCOMPARE(mine->name, QStringLiteral("Mine"));
    QCOMPARE(mine->kind, QStringLiteral("gamepad"));
    QCOMPARE(mine->bindings.value(QStringLiteral("a")), (QStringList{QStringLiteral("pad:x"), QStringLiteral("pad:lefttrigger")}));
    QVERIFY(mine->bindings.value(QStringLiteral("b")).isEmpty());
    QVERIFY(mine->bindings.contains(QStringLiteral("b")));
    QCOMPARE(p.assignedProfileId(QStringLiteral("guid-1"), QStringLiteral("gamepad")), id);
    // Unassigned device -> built-in of its kind.
    QCOMPARE(p.assignedProfileId(QStringLiteral("other"), QStringLiteral("gamepad")), QString::fromLatin1(ControllerProfiles::kBuiltinGamepadId));
    // A profile of the wrong kind is not used for a device.
    QCOMPARE(p.assignedProfileId(QStringLiteral("guid-1"), QStringLiteral("keyboard")), QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId));
  }

  void renameResetDelete() {
    QTemporaryDir dir;
    ControllerProfiles p(dir.path());
    const QString id = p.duplicate(QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId));
    QCOMPARE(p.find(id)->name, QStringLiteral("Keyboard · Standard copy"));
    const QString id2 = p.duplicate(QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId));
    QCOMPARE(p.find(id2)->name, QStringLiteral("Keyboard · Standard copy 2"));  // unique names
    QVERIFY(p.rename(id, QStringLiteral("  WASD  ")));
    QCOMPARE(p.find(id)->name, QStringLiteral("WASD"));
    QVERIFY(!p.rename(id, QStringLiteral("   ")));
    QVERIFY(p.setBinding(id, QStringLiteral("a"), {keyToken(Qt::Key_J)}));
    QVERIFY(p.resetToDefault(id));
    QCOMPARE(p.find(id)->bindings, ControllerProfiles::builtinProfile(QStringLiteral("keyboard")).bindings);
    QVERIFY(p.assign(QString::fromLatin1(ControllerProfiles::kKeyboardDevice), id));
    QVERIFY(p.remove(id));
    QVERIFY(!p.find(id));
    QCOMPARE(p.assignedProfileId(QString::fromLatin1(ControllerProfiles::kKeyboardDevice), QStringLiteral("keyboard")),
             QString::fromLatin1(ControllerProfiles::kBuiltinKeyboardId));
  }

  void tokens() {
    QCOMPARE(keyFromToken(keyToken(Qt::Key_X)).value_or(-1), static_cast<int>(Qt::Key_X));
    QVERIFY(!keyFromToken(QStringLiteral("pad:a")));
    QVERIFY(isPadToken(padToken(QStringLiteral("a"))));
    QCOMPARE(tokenLabel(padToken(QStringLiteral("leftshoulder"))), QStringLiteral("LB"));
    QCOMPARE(inputIndex(QStringLiteral("start")), 6);
    QCOMPARE(inputIndex(QStringLiteral("lid")), -1);
  }

  void hotkeyDefaultsAndConflicts() {
    QTemporaryDir dir;
    ControllerProfiles p(dir.path());
    QCOMPARE(hotkeyDefs().size(), 12);
    QCOMPARE(p.hotkey(QStringLiteral("saveState")), int(Qt::Key_F6));
    QCOMPARE(p.hotkey(QStringLiteral("loadState")), int(Qt::Key_F8));
    QCOMPARE(p.hotkey(QStringLiteral("rewind")), int(Qt::Key_Backspace));
    QCOMPARE(p.hotkey(QStringLiteral("layout")), int(Qt::Key_F7));
    QCOMPARE(p.padHotkey(QStringLiteral("layout")), QStringLiteral("pad:dpright"));
    // Audio hotkeys (0.19): +, -, M and Select + D-pad up / down / X; no key or button is used twice.
    QCOMPARE(p.hotkey(QStringLiteral("volumeUp")), int(Qt::Key_Plus));
    QCOMPARE(p.hotkey(QStringLiteral("volumeDown")), int(Qt::Key_Minus));
    QCOMPARE(p.hotkey(QStringLiteral("mute")), int(Qt::Key_M));
    QCOMPARE(p.padHotkey(QStringLiteral("volumeUp")), QStringLiteral("pad:dpup"));
    QCOMPARE(p.padHotkey(QStringLiteral("volumeDown")), QStringLiteral("pad:dpdown"));
    QCOMPARE(p.padHotkey(QStringLiteral("mute")), QStringLiteral("pad:x"));
    QCOMPARE(p.hotkey(QStringLiteral("screenshot")), int(Qt::Key_F12));
    QCOMPARE(p.padHotkey(QStringLiteral("screenshot")), QStringLiteral("pad:y"));
    // No default shares a key or a pad button with another one.
    QSet<int> keys;
    QSet<QString> pads;
    for (const HotkeyDef& d : hotkeyDefs()) {
      QVERIFY2(!keys.contains(d.defaultKey), qPrintable(d.id));
      QVERIFY2(!pads.contains(d.defaultPad), qPrintable(d.id));
      keys.insert(d.defaultKey);
      pads.insert(d.defaultPad);
    }
    {
      QSet<int> keys;
      QSet<QString> pads;
      for (const HotkeyDef& d : hotkeyDefs()) {
        QVERIFY2(!keys.contains(d.defaultKey), qPrintable(d.id));
        keys.insert(d.defaultKey);
        if (!d.defaultPad.isEmpty()) {
          QVERIFY2(!pads.contains(d.defaultPad), qPrintable(d.id));
          pads.insert(d.defaultPad);
        }
      }
    }
    QCOMPARE(p.hotkey(QStringLiteral("fullscreen")), int(Qt::Key_F11));
    QCOMPARE(p.hotkey(QStringLiteral("diagnostics")), int(Qt::Key_F3));
    QCOMPARE(p.hotkey(QStringLiteral("snapshot")), int(Qt::Key_F5));
    QCOMPARE(p.hotkeyAction(Qt::Key_F3), QStringLiteral("diagnostics"));
    QCOMPARE(p.hotkeyAction(Qt::Key_Z), QString());
    QCOMPARE(p.hotkeysChangedCount(), 0);
    QVERIFY(!QFile::exists(p.filePath()));
    QVERIFY(!p.setHotkey(QStringLiteral("fullscreen"), Qt::Key_F3));      // used by another action
    QVERIFY(!p.setHotkey(QStringLiteral("fullscreen"), Qt::Key_Escape));  // Esc is fixed
    QVERIFY(!p.setHotkey(QStringLiteral("nope"), Qt::Key_Q));
    QCOMPARE(p.hotkey(QStringLiteral("fullscreen")), int(Qt::Key_F11));
    QVERIFY(p.setHotkey(QStringLiteral("fullscreen"), Qt::Key_F11));  // same key again is fine
    QVERIFY(p.setHotkey(QStringLiteral("fullscreen"), Qt::Key_F10));
    QCOMPARE(p.hotkeyAction(Qt::Key_F11), QString());
    QCOMPARE(p.hotkeysChangedCount(), 1);
    QVERIFY(p.clearHotkey(QStringLiteral("snapshot")));
    QCOMPARE(p.hotkey(QStringLiteral("snapshot")), 0);
    QCOMPARE(p.hotkeyAction(0), QString());
    QVERIFY(p.resetHotkey(QStringLiteral("snapshot")));
    QCOMPARE(p.hotkey(QStringLiteral("snapshot")), int(Qt::Key_F5));
    QVERIFY(p.resetHotkeys());
    QCOMPARE(p.hotkeysChangedCount(), 0);
  }

  void padHotkeyDefaultsConflictsAndRoundTrip() {
    QTemporaryDir dir;
    {
      ControllerProfiles p(dir.path());
      QCOMPARE(p.padHotkeyModifier(), QStringLiteral("pad:back"));
      QCOMPARE(p.padHotkey(QStringLiteral("speedup")), QStringLiteral("pad:righttrigger"));
      QCOMPARE(p.padHotkey(QStringLiteral("snapshot")), QString());  // its default button went to Load state (0.20)
      QCOMPARE(p.padHotkey(QStringLiteral("saveState")), QStringLiteral("pad:rightshoulder"));
      QCOMPARE(p.padHotkey(QStringLiteral("loadState")), QStringLiteral("pad:leftshoulder"));
      QCOMPARE(p.padHotkey(QStringLiteral("rewind")), QStringLiteral("pad:lefttrigger"));
      QCOMPARE(p.padHotkey(QStringLiteral("diagnostics")), QStringLiteral("pad:rightstick"));
      QCOMPARE(p.padHotkey(QStringLiteral("fullscreen")), QStringLiteral("pad:b"));   // Uzume hotkeys have gamepad defaults (3f-4)
      QCOMPARE(p.hotkeysChangedCount(), 0);
      QCOMPARE(padHotkeyLabel(p.padHotkey(QStringLiteral("speedup")), p.padHotkeyModifier()), QStringLiteral("Select+RT"));
      QCOMPARE(padHotkeyLabel(p.padHotkey(QStringLiteral("diagnostics")), p.padHotkeyModifier()), QStringLiteral("Select+R3"));
      QCOMPARE(padHotkeyLabel(QStringLiteral("solo:pad:paddle1"), p.padHotkeyModifier()), tokenLabel(QStringLiteral("pad:paddle1")));
      QVERIFY(!QFile::exists(p.filePath()));

      QVERIFY(!p.setPadHotkey(QStringLiteral("fullscreen"), QStringLiteral("pad:righttrigger")));   // used by speed-up
      QVERIFY(!p.setPadHotkey(QStringLiteral("fullscreen"), QStringLiteral("solo:pad:righttrigger")));
      QVERIFY(!p.setPadHotkey(QStringLiteral("fullscreen"), QStringLiteral("pad:back")));            // the hotkey button
      QVERIFY(!p.setPadHotkey(QStringLiteral("fullscreen"), QStringLiteral("pad:guide")));
      QVERIFY(!p.setPadHotkey(QStringLiteral("fullscreen"), QStringLiteral("key:65")));
      QVERIFY(!p.setPadHotkey(QStringLiteral("nope"), QStringLiteral("pad:a")));
      QVERIFY(!p.setPadHotkeyModifier(QStringLiteral("pad:leftshoulder")));                          // used by Load state
      QVERIFY(p.setPadHotkey(QStringLiteral("fullscreen"), QStringLiteral("solo:pad:paddle1")));
      QCOMPARE(p.padHotkeyAction(QStringLiteral("pad:paddle1")), QStringLiteral("fullscreen"));
      QVERIFY(p.clearPadHotkey(QStringLiteral("loadState")));
      QVERIFY(p.setPadHotkeyModifier(QStringLiteral("pad:start")));
      QCOMPARE(p.hotkeysChangedCount(), 3);
      QVERIFY(!p.setPadHotkey(QStringLiteral("loadState"), QStringLiteral("pad:start")));
    }
    {
      // Only non-default entries are written; the keyboard hotkeys are untouched.
      QFile f(dir.path() + QStringLiteral("/settings/controllers.json"));
      QVERIFY(f.open(QIODevice::ReadOnly));
      const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
      QVERIFY(!o.contains(QStringLiteral("hotkeys")));
      QCOMPARE(o.value(QStringLiteral("padHotkeys")).toObject().size(), 2);
      QCOMPARE(o.value(QStringLiteral("padHotkeys")).toObject().value(QStringLiteral("loadState")).toString(), QStringLiteral("none"));
      QCOMPARE(o.value(QStringLiteral("padHotkeyModifier")).toString(), QStringLiteral("pad:start"));
    }
    ControllerProfiles p(dir.path());
    QCOMPARE(p.padHotkeyModifier(), QStringLiteral("pad:start"));
    QCOMPARE(p.padHotkey(QStringLiteral("fullscreen")), QStringLiteral("solo:pad:paddle1"));
    QCOMPARE(p.padHotkey(QStringLiteral("loadState")), QString());
    QCOMPARE(p.padHotkey(QStringLiteral("speedup")), QStringLiteral("pad:righttrigger"));
    QVERIFY(p.resetPadHotkey(QStringLiteral("loadState")));
    QCOMPARE(p.padHotkey(QStringLiteral("loadState")), QStringLiteral("pad:leftshoulder"));
    QVERIFY(p.resetPadHotkeyModifier());
    QVERIFY(p.resetHotkeys());
    QCOMPARE(p.hotkeysChangedCount(), 0);
    QCOMPARE(p.padHotkey(QStringLiteral("fullscreen")), QStringLiteral("pad:b"));
  }

  void padHotkeysOldFilesAndBrokenEntries() {
    QTemporaryDir dir;
    QDir().mkpath(dir.path() + QStringLiteral("/settings"));
    QFile f(dir.path() + QStringLiteral("/settings/controllers.json"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    // An existing file without the gamepad keys keeps its keyboard hotkeys and gets the gamepad defaults.
    f.write(R"({"hotkeys": {"fullscreen": "key:16777273"}})");
    f.close();
    {
      ControllerProfiles p(dir.path());
      QCOMPARE(p.hotkey(QStringLiteral("fullscreen")), int(Qt::Key_F10));
      QCOMPARE(p.padHotkey(QStringLiteral("speedup")), QStringLiteral("pad:righttrigger"));
      QCOMPARE(p.padHotkeyModifier(), QStringLiteral("pad:back"));
    }
    QVERIFY(f.open(QIODevice::WriteOnly));
    // Broken / conflicting entries: invalid values and duplicates fall back; an explicit choice wins over a clashing default.
    f.write(R"({"padHotkeyModifier": "key:1", "padHotkeys": {"fullscreen": "pad:leftshoulder", "diagnostics": "pad:guide",
               "speedup": "pad:back", "snapshot": 7, "future": "pad:a"}})");
    f.close();
    ControllerProfiles p(dir.path());
    QCOMPARE(p.padHotkeyModifier(), QStringLiteral("pad:back"));
    QCOMPARE(p.padHotkey(QStringLiteral("fullscreen")), QStringLiteral("pad:leftshoulder"));
    QCOMPARE(p.padHotkey(QStringLiteral("snapshot")), QString());                                 // default clashes with fullscreen
    QCOMPARE(p.padHotkey(QStringLiteral("diagnostics")), QStringLiteral("pad:rightstick"));       // guide is invalid
    QCOMPARE(p.padHotkey(QStringLiteral("speedup")), QStringLiteral("pad:righttrigger"));        // the modifier is not an action button
  }

  void hotkeysPersistAndUnknownKeysSurvive() {
    QTemporaryDir dir;
    QDir().mkpath(dir.path() + QStringLiteral("/settings"));
    QFile f(dir.path() + QStringLiteral("/settings/controllers.json"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"futureKey": {"x": 1}, "hotkeys": {"future": "key:65"}})");
    f.close();
    {
      ControllerProfiles p(dir.path());
      QVERIFY(p.setHotkey(QStringLiteral("diagnostics"), Qt::Key_F4));
      QVERIFY(p.clearHotkey(QStringLiteral("snapshot")));
    }
    ControllerProfiles p(dir.path());
    QCOMPARE(p.hotkey(QStringLiteral("diagnostics")), int(Qt::Key_F4));
    QCOMPARE(p.hotkey(QStringLiteral("snapshot")), 0);
    QCOMPARE(p.hotkey(QStringLiteral("fullscreen")), int(Qt::Key_F11));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    QVERIFY(o.contains(QStringLiteral("futureKey")));
    const QJsonObject hk = o.value(QStringLiteral("hotkeys")).toObject();
    QCOMPARE(hk.size(), 2);  // only non-default entries
    QCOMPARE(hk.value(QStringLiteral("snapshot")).toString(), QStringLiteral("none"));
    QVERIFY(p.resetHotkeys());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(!QJsonDocument::fromJson(f.readAll()).object().contains(QStringLiteral("hotkeys")));
  }

  void corruptedHotkeysFallBackToDefault() {
    QTemporaryDir dir;
    QDir().mkpath(dir.path() + QStringLiteral("/settings"));
    QFile f(dir.path() + QStringLiteral("/settings/controllers.json"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"hotkeys": {"fullscreen": "bogus", "diagnostics": 7, "snapshot": "key:16777216"}})");  // last = Esc
    f.close();
    ControllerProfiles p(dir.path());
    QCOMPARE(p.hotkey(QStringLiteral("fullscreen")), int(Qt::Key_F11));
    QCOMPARE(p.hotkey(QStringLiteral("diagnostics")), int(Qt::Key_F3));
    QCOMPARE(p.hotkey(QStringLiteral("snapshot")), int(Qt::Key_F5));
    QDir().mkpath(dir.path() + QStringLiteral("/settings"));
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(R"({"hotkeys": "oops"})");
    f.close();
    QCOMPARE(ControllerProfiles(dir.path()).hotkey(QStringLiteral("fullscreen")), int(Qt::Key_F11));
  }

  void corruptedFileYieldsBuiltinOnly() {
    QTemporaryDir dir;
    QDir().mkpath(dir.path() + QStringLiteral("/settings"));
    QFile f(dir.path() + QStringLiteral("/settings/controllers.json"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("[[[");
    f.close();
    QCOMPARE(ControllerProfiles(dir.path()).all().size(), 2);
  }
};

QTEST_APPLESS_MAIN(ControllerProfilesTest)
#include "controllerprofiles_test.moc"
