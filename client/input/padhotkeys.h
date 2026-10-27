#pragma once
// PadHotkeyDetector: pure (no SDL) detection of gamepad hotkeys from the set of pressed binding tokens of P1.
//
// A hotkey fires once per press of its button: either while the modifier (default Select) is held ("combo") or
// without a modifier ("solo"). The button of a fired combo is suppressed (hidden from the core) until it is released,
// so releasing the modifier first does not leak a stray press. The modifier itself is not suppressed: it keeps working
// as a normal button (Select) for the game, only the combo button never reaches the core. A button that is already
// held when the modifier goes down does not fire (no accidental trigger while playing). Solo buttons are always hidden
// from the core while held, like a keyboard hotkey.

#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

namespace uzume::input {

struct PadHotkeyBinding {
  QString action;
  QString token;       // "pad:rightshoulder"
  bool solo = false;   // true: the button alone, false: modifier + button
};

class PadHotkeyDetector {
 public:
  void setBindings(const QString& modifierToken, const QList<PadHotkeyBinding>& bindings);
  // Feed the currently pressed tokens (once per poll). Returns the actions that fired in this update.
  QStringList update(const QSet<QString>& pressed);
  // Actions whose fired button was let go in the last update() (for hold hotkeys such as rewind).
  const QStringList& released() const { return released_; }
  // Tokens to leave out of the input sent to the core (valid after update()).
  const QSet<QString>& suppressed() const { return suppressed_; }
  void reset();  // pad gone / capture running: nothing fires, nothing is suppressed

 private:
  QString modifier_;
  QList<PadHotkeyBinding> bindings_;
  QSet<QString> prev_;
  QSet<QString> active_;  // tokens of fired combos, hidden until released
  QSet<QString> suppressed_;
  QMap<QString, QString> held_;  // button token of a fired hotkey -> action, until the button is released
  QStringList released_;
};

}  // namespace uzume::input
