#pragma once
// ControllersController: data and actions of the Controllers page (3f) and the bridge between devices and the game.
// Owns the SDL3 GamepadService and the local controller profiles (<data>/settings/controllers.json).
// Mapping chain: physical -> profile -> Uzume input -> system input profile -> libretro joypad (merged with the
// keyboard in GameSession). The mouse is the touch input (fixed, no profile).

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <functional>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <optional>

#include "controllerprofiles.h"
#include "gamepadservice.h"
#include "padhold.h"

namespace uzume::ui {

class ControllersController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  Q_PROPERTY(bool gamepadAvailable READ gamepadAvailable CONSTANT)  // SDL gamepad subsystem initialised
  Q_PROPERTY(QString gamepadNote READ gamepadNote CONSTANT)         // why gamepads are off, else empty
  // [{key, kind: gamepad|keyboard|mouse, name, slot, profile, connected, status}]
  Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
  Q_PROPERTY(QString selectedDevice READ selectedDevice NOTIFY selectionChanged)
  Q_PROPERTY(QVariantMap device READ device NOTIFY selectionChanged)  // selected device row
  // Profiles that fit the selected device (select model): [{value: id, label: name, builtin}]
  Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
  Q_PROPERTY(QString profileId READ profileId NOTIFY profilesChanged)
  Q_PROPERTY(QString profileName READ profileName NOTIFY profilesChanged)
  Q_PROPERTY(bool profileBuiltin READ profileBuiltin NOTIFY profilesChanged)
  // [{input, label, target, binding, mapped, changed, listening}]; empty for the mouse
  Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
  Q_PROPERTY(int changedCount READ changedCount NOTIFY rowsChanged)  // bindings that differ from the default profile
  Q_PROPERTY(QString listening READ listening NOTIFY rowsChanged)  // input id waiting for "Press a button…"
  Q_PROPERTY(bool supportsLid READ supportsLid CONSTANT)           // no input profile has a lid input yet
  // Player hotkeys (global, independent of the selected device): [{action, label, key, set, changed, listening, fixed,
  // conflictInput}]. The last row is the fixed Esc row (fixed = true, no capture). conflictInput = label of the
  // keyboard-profile input mapped to the same key ("" = none; the hotkey wins).
  Q_PROPERTY(QVariantList hotkeyRows READ hotkeyRows NOTIFY hotkeysChanged)
  Q_PROPERTY(int hotkeysChangedCount READ hotkeysChangedCount NOTIFY hotkeysChanged)
  Q_PROPERTY(QString hotkeyListening READ hotkeyListening NOTIFY hotkeysChanged)  // action id waiting for "Press a key…"
  Q_PROPERTY(QString hotkeyNote READ hotkeyNote NOTIFY hotkeysChanged)            // "Already used by <label>" after a rejected key
  // {actionId: key label} for the key hints; "" = unassigned (no hint).
  Q_PROPERTY(QVariantMap hotkeyLabels READ hotkeyLabels NOTIFY hotkeysChanged)
  Q_PROPERTY(QVariantMap fixedPad READ fixedPad NOTIFY hotkeysChanged)  // gamepad combos of the fixed rows (F4: modifier + L3)
  // Gamepad hotkeys (P1): rows above also carry {pad, padSet, padChanged, padListening}; padHotkeyListening = action id
  // (or "modifier") waiting for "Press a button…"; padHotkeyModifier = label of the hotkey button ("Select");
  // padHotkeyModifierChanged = differs from the default. hotkeyHints = {actionId: label} for the hints in the game
  // screen: the gamepad combo while a gamepad was the last input used, else the key label; "" = unassigned.
  Q_PROPERTY(QString padHotkeyListening READ padHotkeyListening NOTIFY hotkeysChanged)
  Q_PROPERTY(QString padHotkeyModifier READ padHotkeyModifier NOTIFY hotkeysChanged)
  Q_PROPERTY(bool padHotkeyModifierChanged READ padHotkeyModifierChanged NOTIFY hotkeysChanged)
  Q_PROPERTY(QVariantMap hotkeyHints READ hotkeyHints NOTIFY hotkeyHintsChanged)
  Q_PROPERTY(bool padActive READ padActive NOTIFY padActiveChanged)  // the gamepad was the last input: pages show the hint row
  // Hold-Select hotkey sheet: holdSheetHeld = the hotkey button of P1 has been held alone for ~400 ms (hidden again on release or
  // when another button is pressed); padSheetRows = the combos the sheet lists, [{action, key, label}], from the current bindings
  // (key = GamepadHint key: "A" "L" "R2" "Start" "↑" ... or "UpDown" for the volume pair).
  Q_PROPERTY(bool holdSheetHeld READ holdSheetHeld NOTIFY holdSheetChanged)
  Q_PROPERTY(QVariantList padSheetRows READ padSheetRows NOTIFY hotkeysChanged)
  // System-specific labels from the system manifest (column header, touch input name).
  // System selector of the Buttons tab (3f-4, P33): [{id, label, profile}] and the system whose buttons are listed. It only
  // changes what the page shows; the mapping in use follows the running game's system.
  Q_PROPERTY(QVariantList systems READ systems NOTIFY systemsChanged)
  Q_PROPERTY(QString viewSystem READ viewSystem NOTIFY labelsChanged)
  Q_PROPERTY(QString systemLabel READ systemLabel NOTIFY labelsChanged)
  Q_PROPERTY(QString touchLabel READ touchLabel NOTIFY labelsChanged)
  Q_PROPERTY(bool hasTouch READ hasTouch NOTIFY labelsChanged)  // the current system has a touch screen
  // Input test: ids of the Uzume inputs that are active on the selected device.
  Q_PROPERTY(QStringList activeInputs READ activeInputs NOTIFY testChanged)
  // Button labels (glyph sets, design 3f-2/3f-3). labelChoice = saved choice of the selected gamepad ("auto" | "xbox" |
  // "playstation" | "generic"; saved per physical device), labelSet = effective set id (adds "keyboard" for the keyboard),
  // labelSetName = its title ("Xbox"), labelChoices = select model [{value, label}] ("Auto (PlayStation)" first).
  Q_PROPERTY(QString labelChoice READ labelChoice NOTIFY labelSetChanged)
  Q_PROPERTY(QString labelSet READ labelSet NOTIFY labelSetChanged)
  Q_PROPERTY(QString labelSetName READ labelSetName NOTIFY labelSetChanged)
  Q_PROPERTY(QVariantList labelChoices READ labelChoices NOTIFY labelSetChanged)
  // Cells of the controller-shaped input test: [{id, glyph, col, row, span, square, active}].
  Q_PROPERTY(QVariantList testCells READ testCells NOTIFY testChanged)

 public:
  ControllersController(const QString& dataDir, QObject* parent = nullptr);

  // Starts SDL gamepads (pollIntervalMs <= 0: no timer, tests call gamepads()->poll()). Without SDL: keyboard only.
  void start(bool enableGamepads = true, int pollIntervalMs = 8);
  input::GamepadService* gamepads() { return &pads_; }
  ControllerProfiles* profileStore() { return &profiles_; }
  bool gamepadAvailable() const { return pads_.available(); }
  QString gamepadNote() const { return note_; }

  QVariantList devices() const;
  QString selectedDevice() const { return selected_; }
  QVariantMap device() const;
  QVariantList profiles() const;
  QString profileId() const;
  QString profileName() const;
  bool profileBuiltin() const;
  QVariantList rows() const;
  QString listening() const { return listening_; }
  int changedCount() const;
  bool supportsLid() const { return false; }
  QStringList activeInputs() const;
  QString labelChoice() const;
  QString labelSet() const;
  QString labelSetName() const;
  QVariantList labelChoices() const;
  QVariantList testCells() const;
  QVariantMap fixedPad() const;
  QString systemLabel() const { return viewLabel_.isEmpty() ? systemLabel_ : viewLabel_; }
  QVariantList systems() const { return systems_; }
  QString viewSystem() const { return viewSystem_; }
  void setSystems(const QVariantList& systems);  // [{id, label, profile, inputLabel}] from the manifests
  Q_INVOKABLE void selectSystem(const QString& id);
  QString touchLabel() const { return touchLabel_; }
  bool hasTouch() const { return !touchLabel_.isEmpty(); }
  void setSystemLabels(const QString& systemLabel, const QString& touchLabel);  // empty touchLabel = no touch screen: no Mouse device

  QVariantList hotkeyRows() const;
  int hotkeysChangedCount() const { return profiles_.hotkeysChangedCount(); }
  QString hotkeyListening() const { return hotkeyListening_; }
  QString hotkeyNote() const { return hotkeyNote_; }
  QVariantMap hotkeyLabels() const;
  QSet<int> hotkeyKeys() const;  // assigned hotkey keys (Esc not included)
  // Uzume keys that plugin hotkeys can never use: combination (Qt::Key | modifiers) -> label of the owner. The hotkeys,
  // Esc, F4 (hide all HUD widgets) and Alt+1 ... Alt+9 (panel tabs).
  QMap<int, QString> reservedKeyOwners() const;
  QString padHotkeyListening() const { return padHotkeyListening_; }
  QString padHotkeyModifier() const;
  bool padHotkeyModifierChanged() const { return profiles_.padHotkeyModifierChanged(); }
  QVariantMap hotkeyHints() const;
  bool holdSheetHeld() const { return hold_.visible(); }
  QVariantList padSheetRows() const;
  void setHoldTimeMs(int ms) { hold_.setHoldMs(ms); }  // tests

  // Per-game controller profiles (Emulation > "uzume.pad_profile" / "uzume.keyboard_profile"): a temporary override for P1's
  // gamepad and the keyboard for the running game. Empty id or an id without a profile of that kind = the device's own profile.
  // The stored device assignments never change; clearGameProfiles() (game end) restores the normal mapping.
  void setGameProfiles(const QString& padProfileId, const QString& keyboardProfileId);
  void clearGameProfiles();
  QString gamePadProfile() const { return gamePad_; }
  QString gameKeyboardProfile() const { return gameKeyboard_; }

  // Qt::Key -> joypad mask of the keyboard profile (input profile of the system applied); keys that are hotkeys are left out.
  QHash<int, quint32> keyboardMap() const;
  // Manifest input profile of the running/starting game: gamepad P1 and keyboard map follow it.
  void setInputProfile(const QString& profile);

  Q_INVOKABLE void selectDevice(const QString& key);
  Q_INVOKABLE void setLabelChoice(const QString& choice);  // "auto" | "xbox" | "playstation" | "generic"; gamepads only
  Q_INVOKABLE void selectProfile(const QString& id);
  Q_INVOKABLE void duplicateProfile();
  Q_INVOKABLE void renameProfile(const QString& name);
  Q_INVOKABLE void deleteProfile();
  Q_INVOKABLE void resetProfile();
  Q_INVOKABLE void beginCapture(const QString& inputId);
  Q_INVOKABLE void cancelCapture();
  Q_INVOKABLE void clearBinding(const QString& inputId);
  Q_INVOKABLE void resetBinding(const QString& inputId);  // one input back to the default profile's binding
  // Keyboard: key while capturing (true = consumed; Escape cancels) / while testing (state of the tiles).
  Q_INVOKABLE bool captureKey(int qtKey);
  Q_INVOKABLE void testKey(int qtKey, bool pressed);
  Q_INVOKABLE void clearTestKeys();
  // Hotkeys. hotkeyAction: action id the Qt key triggers ("" = none; Esc is fixed and not an action).
  Q_INVOKABLE QString hotkeyAction(int qtKey) const { return profiles_.hotkeyAction(qtKey); }
  Q_INVOKABLE void beginHotkeyCapture(const QString& action);
  Q_INVOKABLE void cancelHotkeyCapture();
  Q_INVOKABLE void clearHotkey(const QString& action);
  Q_INVOKABLE void resetHotkey(const QString& action);
  Q_INVOKABLE void resetHotkeys();  // keyboard keys, gamepad buttons and the hotkey button
  // Gamepad hotkeys: capture the next button (hold the hotkey button first for a combo; a lone button is bound without it).
  Q_INVOKABLE void beginPadHotkeyCapture(const QString& action);  // action id or "modifier"
  Q_INVOKABLE void cancelPadHotkeyCapture();
  Q_INVOKABLE void clearPadHotkey(const QString& action);
  Q_INVOKABLE void resetPadHotkeyModifier();
  bool padActive() const { return padActive_; }
  // Gamepad as UI input (readable-pass.md "Gamepad everywhere"): D-pad / left stick -> arrow keys (repeat after 400 ms, then 80 ms),
  // A -> Return, B -> Escape, posted to the focused window; Y -> padCommand("search"), L / R -> "filterPrev" / "filterNext".
  // `gate` says whether the shell has the input (false while a game runs unpaused); nothing is translated while a capture runs.
  // feedPadNav() is what the SDL poll calls with the pressed tokens (tests call it directly).
  void setPadNavGate(std::function<bool()> gate) { padNavGate_ = std::move(gate); }
  // While it returns true and the pad belongs to the game, A / B are also offered as padCommand("togetherReady" / "togetherNotYet") (ready check card).
  void setPadTogetherGate(std::function<bool()> gate) { padTogetherGate_ = std::move(gate); }
  void feedPadNav(const QSet<QString>& tokens, qint64 nowMs);
  Q_INVOKABLE void notePadInput();       // a gamepad button was used last
  Q_INVOKABLE void noteKeyboardInput();  // the keyboard was used last: hints show keys again
  // "Duplicate to edit": duplicates the built-in profile, selects the copy and begins capture for the input on it.
  Q_INVOKABLE void duplicateAndCapture(const QString& inputId);

 signals:
  void systemsChanged();
  void devicesChanged();
  void selectionChanged();
  void profilesChanged();
  void rowsChanged();
  void testChanged();
  void keyboardMapChanged();
  void labelsChanged();
  void labelSetChanged();
  void hotkeysChanged();
  void hotkeyHintsChanged();
  void padActiveChanged();
  void padCommand(const QString& command);   // search | filterPrev | filterNext | togetherReady | togetherNotYet
  void holdSheetChanged();
  void padHotkeyTriggered(const QString& action);  // P1 gamepad hotkey pressed (combo or lone button)
  void padHotkeyReleased(const QString& action);   // ... and its button let go (hold hotkeys: rewind)
  void libretroMaskChanged(quint32 mask);  // P1 gamepad, nds system profile

 private:
  QString systemLabel_;
  QString touchLabel_ = QStringLiteral("Touch");
  struct Selected {
    QString kind;      // gamepad | keyboard | mouse | ""
    QString deviceKey; // assignment key (guid or "keyboard")
    int padId = 0;
  };
  Selected selection() const;
  QString labelSetFor(const Selected& s, const input::PadDevice& d) const;
  std::optional<ControllerProfile> currentProfile() const;
  QString deviceKeyForAssignment() const;
  void profileChanged();   // after a change of profiles/assignments: tell the consumers
  void applyCapture(const QString& token);
  bool captureHotkey(int qtKey);
  void capturePadHotkey(const QString& token, int deviceId);
  void syncPadHotkeys();  // profiles_ -> GamepadService
  void hotkeysUpdated();  // after a change of the hotkeys: consumers refresh (also the keyboard map)
  void ensureSelection();
  void updateHold();
  QString activeProfileId(const QString& deviceKey, const QString& kind) const;  // game override, else the assignment
  bool isP1(const QString& deviceKey) const;

  ControllerProfiles profiles_;
  input::GamepadService pads_;
  QVariantList systems_;
  QString viewSystem_;    // system listed on the Buttons tab; follows the running game's system until the user picks one
  QString viewProfile_;   // its input profile (empty = inputProfile_)
  QString viewLabel_;
  bool viewPinned_ = false;  // picked by the user: a game start does not move it
  void syncView();
  QString inputProfile_;  // manifest input profile; empty until the first manifest or game sets it
  QString selected_ = QStringLiteral("keyboard");
  QString listening_;
  QString note_;
  QString hotkeyListening_;
  QString hotkeyNote_;
  QString padHotkeyListening_;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void sendPadKey(int key);
  std::function<bool()> padNavGate_;
  std::function<bool()> padTogetherGate_;
  QSet<QString> padNavPrev_;
  QString padNavDir_;
  qint64 padNavNext_ = 0;
  bool padNavMute_ = false;   // a capture just ended: its button press is not a UI press
  bool synthesizing_ = false;
  bool padActive_ = false;  // the gamepad was the last input used
  QSet<int> heldKeys_;
  input::PadHoldDetector hold_;
  QTimer holdTimer_;
  QElapsedTimer clock_;
  QString gamePad_;       // temporary per-game profile of P1's gamepad ("" = the assigned one)
  QString gameKeyboard_;  // temporary per-game profile of the keyboard
};

}  // namespace uzume::ui
