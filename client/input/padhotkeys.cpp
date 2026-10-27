#include "padhotkeys.h"

namespace uzume::input {

void PadHotkeyDetector::setBindings(const QString& modifierToken, const QList<PadHotkeyBinding>& bindings) {
  modifier_ = modifierToken;
  bindings_ = bindings;
}

void PadHotkeyDetector::reset() {
  prev_.clear();
  active_.clear();
  suppressed_.clear();
  held_.clear();
  released_.clear();
}

QStringList PadHotkeyDetector::update(const QSet<QString>& pressed) {
  QStringList fired;
  released_.clear();
  for (auto it = held_.begin(); it != held_.end();) {
    if (!pressed.contains(it.key())) {
      released_.append(it.value());
      it = held_.erase(it);
    } else {
      ++it;
    }
  }
  const bool modHeld = !modifier_.isEmpty() && pressed.contains(modifier_);
  for (const PadHotkeyBinding& b : std::as_const(bindings_)) {
    if (b.token.isEmpty() || b.token == modifier_) continue;
    const bool down = pressed.contains(b.token);
    if (down && !prev_.contains(b.token) && (b.solo || modHeld)) {
      fired.append(b.action);
      held_.insert(b.token, b.action);
      if (!b.solo) active_.insert(b.token);
    }
  }
  active_.intersect(pressed);  // released: visible again
  suppressed_ = active_;
  for (const PadHotkeyBinding& b : std::as_const(bindings_)) {
    if (b.solo && !b.token.isEmpty() && b.token != modifier_ && pressed.contains(b.token)) suppressed_.insert(b.token);
  }
  prev_ = pressed;
  return fired;
}

}  // namespace uzume::input
