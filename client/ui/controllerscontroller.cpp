#include "controllerscontroller.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickWindow>
#include <Qt>

#include "controllersglyphs.h"
#include "padtype.h"

namespace uzume::ui {

namespace {
const QString kKeyboard = QStringLiteral("keyboard");
const QString kMouse = QStringLiteral("mouse");
const QString kPadPrefix = QStringLiteral("pad:");
}  // namespace

ControllersController::ControllersController(const QString& dataDir, QObject* parent)
    : QObject(parent), profiles_(dataDir) {
  pads_.setBindingsProvider([this](const QString& deviceKey) {
    const QString id = activeProfileId(deviceKey, QStringLiteral("gamepad"));
    const auto p = profiles_.find(id);
    return p ? p->bindings : ControllerProfiles::builtinProfile(QStringLiteral("gamepad")).bindings;
  });
  connect(&pads_, &input::GamepadService::devicesChanged, this, [this]() {
    ensureSelection();
    emit devicesChanged();
    emit labelSetChanged();
    emit profilesChanged();
    emit rowsChanged();
    emit testChanged();
  });
  connect(&pads_, &input::GamepadService::stateChanged, this, &ControllersController::testChanged);
  connect(&pads_, &input::GamepadService::libretroMaskChanged, this, &ControllersController::libretroMaskChanged);
  connect(&pads_, &input::GamepadService::polled, this, [this]() {
    QSet<QString> all;
    for (const auto& d : pads_.devices()) all.unite(pads_.pressedTokens(d.id));
    feedPadNav(all, clock_.elapsed());
  });
  connect(&pads_, &input::GamepadService::captured, this, [this](const QString& token, int deviceId) {
    padNavMute_ = true;
    if (!padHotkeyListening_.isEmpty()) {
      capturePadHotkey(token, deviceId);
    } else {
      applyCapture(token);
    }
  });
  connect(&pads_, &input::GamepadService::hotkeyTriggered, this, [this](const QString& action) {
    notePadInput();
    emit padHotkeyTriggered(action);
  });
  connect(&pads_, &input::GamepadService::stateChanged, this, [this]() {
    const auto devs = pads_.devices();
    if (!padActive_ && !devs.isEmpty() && !pads_.pressedTokens(devs.first().id).isEmpty()) {
      notePadInput();
    }
  });
  if (auto* app = QCoreApplication::instance()) app->installEventFilter(this);
  clock_.start();
  holdTimer_.setSingleShot(true);
  connect(&holdTimer_, &QTimer::timeout, this, &ControllersController::updateHold);
  connect(&pads_, &input::GamepadService::stateChanged, this, &ControllersController::updateHold);
  connect(&pads_, &input::GamepadService::devicesChanged, this, &ControllersController::updateHold);
  syncPadHotkeys();
}

void ControllersController::updateHold() {
  const auto devs = pads_.devices();
  QSet<QString> pressed;
  // While a button is being captured for a binding the sheet stays away.
  if (!devs.isEmpty() && !pads_.capturing() && padHotkeyListening_.isEmpty()) pressed = pads_.pressedTokens(devs.first().id);
  hold_.setModifier(profiles_.padHotkeyModifier());
  const bool was = hold_.visible();
  const qint64 now = clock_.elapsed();
  hold_.update(pressed, now);
  const int wait = hold_.msUntilShow(now);
  if (wait >= 0) {
    holdTimer_.start(wait + 1);
  } else {
    holdTimer_.stop();
  }
  if (hold_.visible() != was) emit holdSheetChanged();
}

namespace {
// GamepadHint key of a pad token.
QString sheetKey(const QString& token) {
  static const QHash<QString, QString> k = {
      {QStringLiteral("pad:a"), QStringLiteral("A")},          {QStringLiteral("pad:b"), QStringLiteral("B")},
      {QStringLiteral("pad:x"), QStringLiteral("X")},          {QStringLiteral("pad:y"), QStringLiteral("Y")},
      {QStringLiteral("pad:leftshoulder"), QStringLiteral("L")},  {QStringLiteral("pad:rightshoulder"), QStringLiteral("R")},
      {QStringLiteral("pad:lefttrigger"), QStringLiteral("L2")},  {QStringLiteral("pad:righttrigger"), QStringLiteral("R2")},
      {QStringLiteral("pad:leftstick"), QStringLiteral("L3")},    {QStringLiteral("pad:rightstick"), QStringLiteral("R3")},
      {QStringLiteral("pad:start"), QStringLiteral("Start")},     {QStringLiteral("pad:back"), QStringLiteral("Select")},
      {QStringLiteral("pad:dpup"), QStringLiteral("↑")},          {QStringLiteral("pad:dpdown"), QStringLiteral("↓")},
      {QStringLiteral("pad:dpleft"), QStringLiteral("←")},        {QStringLiteral("pad:dpright"), QStringLiteral("→")},
  };
  return k.value(token, tokenLabel(token));
}
}  // namespace

// The combos of the hold-Select sheet from the bindings in use (the same ones syncPadHotkeys() hands to the GamepadService):
// buttons that fire while the hotkey button is held. Lone ("solo") buttons are not part of a hold sheet. Pause (hotkey button +
// Start) and "hide all HUD widgets" (+ L3) are fixed unless the user gave that button to another action.
QVariantList ControllersController::padSheetRows() const {
  struct Entry {
    QString action, token, label;
  };
  QList<Entry> entries;
  QSet<QString> used;
  for (const HotkeyDef& d : hotkeyDefs()) {
    const QString b = profiles_.padHotkey(d.id);
    if (!b.isEmpty()) used.insert(padHotkeyToken(b));
  }
  if (!used.contains(QStringLiteral("pad:start"))) entries.append({QStringLiteral("pause"), QStringLiteral("pad:start"), tr("Pause")});
  for (const HotkeyDef& d : hotkeyDefs()) {
    const QString b = profiles_.padHotkey(d.id);
    if (b.isEmpty() || padHotkeySolo(b)) continue;
    entries.append({d.id, padHotkeyToken(b), d.label});
  }
  if (!used.contains(QStringLiteral("pad:leftstick"))) entries.append({QStringLiteral("hud"), QStringLiteral("pad:leftstick"), tr("Hide all HUD widgets")});
  // Volume up and down on the D-pad up/down are one row, as in the design.
  QVariantList rows;
  const auto find = [&](const QString& action) {
    for (const Entry& e : std::as_const(entries)) {
      if (e.action == action) return e;
    }
    return Entry{};
  };
  const Entry up = find(QStringLiteral("volumeUp")), down = find(QStringLiteral("volumeDown"));
  const bool pair = up.token == QLatin1String("pad:dpup") && down.token == QLatin1String("pad:dpdown");
  for (const Entry& e : std::as_const(entries)) {
    if (pair && e.action == QLatin1String("volumeDown")) continue;
    if (pair && e.action == QLatin1String("volumeUp")) {
      rows.append(QVariantMap{{QStringLiteral("action"), QStringLiteral("volume")}, {QStringLiteral("key"), QStringLiteral("UpDown")}, {QStringLiteral("label"), tr("Volume")}});
      continue;
    }
    rows.append(QVariantMap{{QStringLiteral("action"), e.action}, {QStringLiteral("key"), sheetKey(e.token)}, {QStringLiteral("label"), e.label}});
  }
  return rows;
}

bool ControllersController::isP1(const QString& deviceKey) const {
  const auto devs = pads_.devices();
  return !devs.isEmpty() && devs.first().key == deviceKey;
}

QString ControllersController::activeProfileId(const QString& deviceKey, const QString& kind) const {
  const bool pad = kind == QLatin1String("gamepad");
  const QString& over = pad ? gamePad_ : gameKeyboard_;
  if (!over.isEmpty() && (!pad || isP1(deviceKey))) {
    const auto p = profiles_.find(over);
    if (p && p->kind == kind) return over;  // a deleted profile is ignored here, the device's own one is used
  }
  return profiles_.assignedProfileId(deviceKey, kind);
}

void ControllersController::setGameProfiles(const QString& padProfileId, const QString& keyboardProfileId) {
  if (gamePad_ == padProfileId && gameKeyboard_ == keyboardProfileId) return;
  gamePad_ = padProfileId;
  gameKeyboard_ = keyboardProfileId;
  profileChanged();
}

void ControllersController::clearGameProfiles() { setGameProfiles(QString(), QString()); }

void ControllersController::syncPadHotkeys() {
  QList<input::PadHotkeyBinding> bindings;
  for (const HotkeyDef& d : hotkeyDefs()) {
    const QString b = profiles_.padHotkey(d.id);
    if (!b.isEmpty()) bindings.append({d.id, padHotkeyToken(b), padHotkeySolo(b)});
  }
  // Fixed Uzume combos (3f-4): modifier + Start pauses (the Esc of the gamepad), modifier + L3 hides all HUD widgets. A
  // button the user gave to another hotkey stays theirs.
  QSet<QString> used;
  for (const input::PadHotkeyBinding& b : std::as_const(bindings)) used.insert(b.token);
  for (const auto& f : {std::pair{QStringLiteral("pause"), QStringLiteral("pad:start")}, std::pair{QStringLiteral("hud"), QStringLiteral("pad:leftstick")}}) {
    if (!used.contains(f.second)) bindings.append({f.first, f.second, false});
  }
  pads_.setPadHotkeys(profiles_.padHotkeyModifier(), bindings);
}

void ControllersController::notePadInput() {
  if (!padActive_) {
    padActive_ = true;
    emit hotkeyHintsChanged();
    emit padActiveChanged();
  }
}

void ControllersController::noteKeyboardInput() {
  if (padActive_) {
    padActive_ = false;
    emit hotkeyHintsChanged();
    emit padActiveChanged();
  }
}

bool ControllersController::eventFilter(QObject* watched, QEvent* event) {
  if (synthesizing_) return false;
  // Keys and the mouse mean the keyboard was used last (the hint row goes away); never consumes an event.
  if (padActive_ && (event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress)) noteKeyboardInput();
  return QObject::eventFilter(watched, event);
}

void ControllersController::start(bool enableGamepads, int pollIntervalMs) {
  if (!enableGamepads) {
    note_ = tr("Gamepads are turned off.");
    return;
  }
  if (!pads_.start(pollIntervalMs)) {
    note_ = tr("Gamepads are not available (%1). Keyboard and mouse still work.").arg(pads_.error());
  }
  ensureSelection();
}

ControllersController::Selected ControllersController::selection() const {
  Selected s;
  if (selected_ == kKeyboard) {
    s.kind = kKeyboard;
    s.deviceKey = kKeyboard;
  } else if (selected_ == kMouse) {
    s.kind = kMouse;
  } else if (selected_.startsWith(kPadPrefix)) {
    const input::PadDevice d = pads_.device(selected_.mid(kPadPrefix.size()).toInt());
    if (d.id != 0) {
      s.kind = QStringLiteral("gamepad");
      s.deviceKey = d.key;
      s.padId = d.id;
    }
  }
  return s;
}

void ControllersController::ensureSelection() {
  if (selection().kind.isEmpty()) {
    const auto pads = pads_.devices();
    selected_ = pads.isEmpty() ? kKeyboard : kPadPrefix + QString::number(pads.first().id);
    listening_.clear();
    pads_.cancelCapture();
    emit selectionChanged();
    emit labelSetChanged();
  }
}

std::optional<ControllerProfile> ControllersController::currentProfile() const {
  const Selected s = selection();
  if (s.kind != QLatin1String("gamepad") && s.kind != kKeyboard) return std::nullopt;
  return profiles_.find(profiles_.assignedProfileId(s.deviceKey, s.kind));
}

void ControllersController::setSystemLabels(const QString& systemLabel, const QString& touchLabel) {
  if (systemLabel_ == systemLabel && touchLabel_ == touchLabel) return;
  systemLabel_ = systemLabel;
  touchLabel_ = touchLabel;
  viewPinned_ = false;
  syncView();
  if (touchLabel_.isEmpty() && selected_ == kMouse) selected_.clear();  // the Mouse row is hidden: pick another device
  ensureSelection();
  emit labelsChanged();
  emit devicesChanged();
}

QVariantList ControllersController::devices() const {
  QVariantList l;
  for (const input::PadDevice& d : pads_.devices()) {
    const auto p = profiles_.find(profiles_.assignedProfileId(d.key, QStringLiteral("gamepad")));
    l.append(QVariantMap{{QStringLiteral("key"), kPadPrefix + QString::number(d.id)},
                         {QStringLiteral("kind"), QStringLiteral("gamepad")},
                         {QStringLiteral("name"), d.name},
                         {QStringLiteral("slot"), d.slot == 1 ? QStringLiteral("P1") : QStringLiteral("—")},
                         {QStringLiteral("connected"), true},
                         {QStringLiteral("status"), tr("Connected")},
                         {QStringLiteral("profile"), p ? p->name : QString()},
                         {QStringLiteral("padType"), input::padTypeId(d.type)},
                         {QStringLiteral("padTypeName"), input::padTypeName(d.type)}});
  }
  const auto kb = profiles_.find(profiles_.assignedProfileId(kKeyboard, kKeyboard));
  l.append(QVariantMap{{QStringLiteral("key"), kKeyboard},
                       {QStringLiteral("kind"), kKeyboard},
                       {QStringLiteral("name"), tr("Keyboard")},
                       {QStringLiteral("slot"), pads_.devices().isEmpty() ? QStringLiteral("P1") : QStringLiteral("—")},
                       {QStringLiteral("connected"), true},
                       {QStringLiteral("status"), tr("Connected")},
                       {QStringLiteral("profile"), kb ? kb->name : QString()}});
  if (!touchLabel_.isEmpty())  // no touch screen (gb, gba): no Mouse row
  l.append(QVariantMap{{QStringLiteral("key"), kMouse},
                       {QStringLiteral("kind"), kMouse},
                       {QStringLiteral("name"), tr("Mouse")},
                       {QStringLiteral("slot"), tr("Touch")},
                       {QStringLiteral("connected"), true},
                       {QStringLiteral("status"), tr("Connected")},
                       {QStringLiteral("profile"), touchLabel_}});
  return l;
}

QVariantMap ControllersController::device() const {
  for (const QVariant& v : devices()) {
    if (v.toMap().value(QStringLiteral("key")).toString() == selected_) return v.toMap();
  }
  return {};
}

QVariantList ControllersController::profiles() const {
  QVariantList l;
  const Selected s = selection();
  if (s.kind != QLatin1String("gamepad") && s.kind != kKeyboard) return l;
  for (const ControllerProfile& p : profiles_.ofKind(s.kind)) {
    l.append(QVariantMap{{QStringLiteral("value"), p.id}, {QStringLiteral("label"), p.name}, {QStringLiteral("builtin"), p.builtin}});
  }
  return l;
}

QString ControllersController::profileId() const {
  const auto p = currentProfile();
  return p ? p->id : QString();
}
QString ControllersController::profileName() const {
  const auto p = currentProfile();
  return p ? p->name : QString();
}
bool ControllersController::profileBuiltin() const {
  const auto p = currentProfile();
  return !p || p->builtin;
}

QVariantList ControllersController::rows() const {
  QVariantList l;
  const auto p = currentProfile();
  if (!p) return l;
  const ControllerProfile def = ControllerProfiles::builtinProfile(p->kind);
  const QString set = labelSet();
  const QString prof = viewProfile_.isEmpty() ? inputProfile_ : viewProfile_;
  QList<quint32> seenBits;
  for (const InputDef& in : uzumeInputs()) {
    if (!input::inputProfileMaps(prof, in.id)) continue;  // the Controllers page lists what the system maps
    const QString profileLabel = input::inputProfileLabel(prof, in.id);
    // An input that only repeats another one's libretro button without a text of its own is an alias (DS: ZL = L, the left
    // stick = D-pad) and is not listed: the DS has no ZL, ZR or sticks (P33). The binding keeps working.
    const quint32 bits = input::inputProfileBits(prof, in.id);
    if (profileLabel.isEmpty() && seenBits.contains(bits)) continue;
    seenBits.append(bits);
    const QString target = profileLabel.isEmpty() ? in.targetLabel : profileLabel;
    const QStringList tokens = p->bindings.value(in.id);
    const bool changed = !p->builtin && tokens != def.bindings.value(in.id);
    QStringList labels;
    for (const QString& t : tokens) labels.append(tokenLabel(t));
    const QVariantMap d = describeBinding(set, tokens);
    l.append(QVariantMap{{QStringLiteral("input"), in.id},
                         {QStringLiteral("label"), in.label},
                         {QStringLiteral("target"), target},
                         {QStringLiteral("binding"), labels.isEmpty() ? tr("Unassigned") : labels.join(QStringLiteral(" / "))},
                         {QStringLiteral("glyphs"), d.value(QStringLiteral("glyphs"))},
                         {QStringLiteral("bindingName"), labels.isEmpty() ? tr("Unassigned") : d.value(QStringLiteral("name"))},
                         {QStringLiteral("mapped"), !labels.isEmpty()},
                         {QStringLiteral("changed"), changed},
                         {QStringLiteral("listening"), listening_ == in.id}});
  }
  return l;
}

int ControllersController::changedCount() const {
  int n = 0;
  for (const QVariant& v : rows()) {
    n += v.toMap().value(QStringLiteral("changed")).toBool() ? 1 : 0;
  }
  return n;
}

void ControllersController::resetBinding(const QString& inputId) {
  const auto p = currentProfile();
  if (!p || p->builtin || inputIndex(inputId) < 0) return;
  cancelCapture();
  if (profiles_.setBinding(p->id, inputId, ControllerProfiles::builtinProfile(p->kind).bindings.value(inputId))) profileChanged();
}

QStringList ControllersController::activeInputs() const {
  const Selected s = selection();
  quint32 mask = 0;
  if (s.kind == QLatin1String("gamepad")) {
    mask = pads_.inputMaskFor(s.padId);
  } else if (s.kind == kKeyboard) {
    if (const auto p = currentProfile()) {
      const QHash<QString, quint32> c = input::compileBindings(p->bindings);
      for (int k : std::as_const(heldKeys_)) mask |= c.value(keyToken(k), 0u);
    }
  }
  QStringList out;
  const auto& inputs = uzumeInputs();
  for (int i = 0; i < inputs.size(); ++i) {
    if ((mask & (1u << i)) != 0) out.append(inputs.at(i).id);
  }
  return out;
}

QString ControllersController::labelSetFor(const Selected& s, const input::PadDevice& d) const {
  if (s.kind == kKeyboard) return kKeyboard;
  if (s.kind != QLatin1String("gamepad")) return QStringLiteral("generic");
  const QString override = profiles_.labelSet(s.deviceKey);
  return override.isEmpty() ? input::padTypeId(d.type) : override;
}

QString ControllersController::labelChoice() const {
  const Selected s = selection();
  if (s.kind != QLatin1String("gamepad")) return QStringLiteral("auto");
  const QString o = profiles_.labelSet(s.deviceKey);
  return o.isEmpty() ? QStringLiteral("auto") : o;
}

QString ControllersController::labelSet() const {
  const Selected s = selection();
  return labelSetFor(s, s.kind == QLatin1String("gamepad") ? pads_.device(s.padId) : input::PadDevice{});
}

QString ControllersController::labelSetName() const { return labelSetTitle(labelSet()); }

QVariantList ControllersController::labelChoices() const {
  const Selected s = selection();
  const input::PadDevice d = s.kind == QLatin1String("gamepad") ? pads_.device(s.padId) : input::PadDevice{};
  const auto entry = [](const QString& value, const QString& label) {
    return QVariantMap{{QStringLiteral("value"), value}, {QStringLiteral("label"), label}};
  };
  return {entry(QStringLiteral("auto"), tr("Auto (%1)").arg(input::padTypeName(d.type))), entry(QStringLiteral("xbox"), tr("Xbox")),
          entry(QStringLiteral("playstation"), tr("PlayStation")), entry(QStringLiteral("generic"), tr("Generic"))};
}

void ControllersController::setLabelChoice(const QString& choice) {
  const Selected s = selection();
  if (s.kind != QLatin1String("gamepad")) return;
  if (choice != QLatin1String("auto") && choice != QLatin1String("xbox") && choice != QLatin1String("playstation") &&
      choice != QLatin1String("generic")) {
    return;
  }
  if (!profiles_.setLabelSet(s.deviceKey, choice == QLatin1String("auto") ? QString() : choice)) return;
  emit labelSetChanged();
  emit rowsChanged();
  emit testChanged();
}

QVariantList ControllersController::testCells() const {
  const Selected s = selection();
  QSet<QString> tokens;
  QStringList inputs;
  if (s.kind == QLatin1String("gamepad")) {
    tokens = pads_.pressedTokens(s.padId);
  } else {
    inputs = activeInputs();
  }
  QVariantList cells = inputTestCells(labelSet());
  for (QVariant& v : cells) {
    QVariantMap c = v.toMap();
    bool on = false;
    if (s.kind == QLatin1String("gamepad")) {
      for (const QString& t : c.value(QStringLiteral("tokens")).toStringList()) on = on || tokens.contains(t);
    } else {
      on = inputs.contains(c.value(QStringLiteral("id")).toString());
    }
    c.insert(QStringLiteral("active"), on);
    c.remove(QStringLiteral("tokens"));
    v = c;
  }
  return cells;
}

void ControllersController::setSystems(const QVariantList& systems) {
  systems_ = systems;
  syncView();
  emit systemsChanged();
  emit labelsChanged();
  emit rowsChanged();
}

// Without a pick the page lists the buttons of the system in use (the running game's, else the first system).
void ControllersController::syncView() {
  if (viewPinned_) return;
  viewSystem_.clear();
  viewProfile_.clear();
  viewLabel_.clear();
  for (const QVariant& v : std::as_const(systems_)) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("profile")).toString() == inputProfile_ && m.value(QStringLiteral("inputLabel")).toString() == systemLabel_) {
      viewSystem_ = m.value(QStringLiteral("id")).toString();
      break;
    }
  }
  if (viewSystem_.isEmpty() && !systems_.isEmpty()) viewSystem_ = systems_.first().toMap().value(QStringLiteral("id")).toString();
  for (const QVariant& v : std::as_const(systems_)) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("id")).toString() == viewSystem_) {
      viewProfile_ = m.value(QStringLiteral("profile")).toString();
      viewLabel_ = m.value(QStringLiteral("inputLabel")).toString();
    }
  }
}

void ControllersController::selectSystem(const QString& id) {
  if (id == viewSystem_) return;
  for (const QVariant& v : std::as_const(systems_)) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("id")).toString() != id) continue;
    cancelCapture();
    viewPinned_ = true;
    viewSystem_ = id;
    viewProfile_ = m.value(QStringLiteral("profile")).toString();
    viewLabel_ = m.value(QStringLiteral("inputLabel")).toString();
    emit labelsChanged();
    emit rowsChanged();
    emit testChanged();  // the input test follows the listed system's inputs
    return;
  }
}

void ControllersController::setInputProfile(const QString& profile) {
  if (profile == inputProfile_) return;
  inputProfile_ = profile;
  viewPinned_ = false;   // a game start moves the page to its system again
  syncView();
  pads_.setInputProfile(profile);
  emit labelsChanged();  // the listed system may have moved with the game
  emit rowsChanged();
  emit testChanged();
  emit keyboardMapChanged();
}

QHash<int, quint32> ControllersController::keyboardMap() const {
  const auto p = profiles_.find(activeProfileId(kKeyboard, kKeyboard));
  QHash<int, quint32> map = input::keyMapFor(inputProfile_, p ? p->bindings : ControllerProfiles::builtinProfile(kKeyboard).bindings);
  for (auto it = map.begin(); it != map.end();) {  // a hotkey wins over the keyboard profile
    it = profiles_.hotkeyAction(it.key()).isEmpty() ? std::next(it) : map.erase(it);
  }
  return map;
}

namespace {
QString keyLabelOf(int key) { return key > 0 ? tokenLabel(keyToken(key)) : QString(); }
}  // namespace

QVariantList ControllersController::hotkeyRows() const {
  QVariantList l;
  const auto kb = profiles_.find(profiles_.assignedProfileId(kKeyboard, kKeyboard));
  for (const HotkeyDef& d : hotkeyDefs()) {
    const int key = profiles_.hotkey(d.id);
    QString conflict;
    if (key > 0 && kb) {
      const QString token = keyToken(key);
      for (const InputDef& in : uzumeInputs()) {
        if (kb->bindings.value(in.id).contains(token)) {
          conflict = in.label;
          break;
        }
      }
    }
    const QString pad = profiles_.padHotkey(d.id);
    l.append(QVariantMap{{QStringLiteral("action"), d.id},
                         {QStringLiteral("label"), d.label},
                         {QStringLiteral("pad"), pad.isEmpty() ? tr("not set") : padHotkeyLabel(pad, profiles_.padHotkeyModifier())},
                         {QStringLiteral("padSet"), !pad.isEmpty()},
                         {QStringLiteral("padChanged"), pad != d.defaultPad},
                         {QStringLiteral("padListening"), padHotkeyListening_ == d.id},
                         {QStringLiteral("key"), key > 0 ? keyLabelOf(key) : tr("not set")},
                         {QStringLiteral("set"), key > 0},
                         {QStringLiteral("changed"), key != d.defaultKey},
                         {QStringLiteral("listening"), hotkeyListening_ == d.id},
                         {QStringLiteral("fixed"), false},
                         {QStringLiteral("conflictInput"), conflict}});
  }
  l.append(QVariantMap{{QStringLiteral("action"), QStringLiteral("escape")},
                       {QStringLiteral("label"), tr("Pause / pause overlay")},
                       {QStringLiteral("pad"), padHotkeyLabel(QStringLiteral("pad:start"), profiles_.padHotkeyModifier())},
                       {QStringLiteral("padSet"), true},
                       {QStringLiteral("padChanged"), false},
                       {QStringLiteral("padListening"), false},
                       {QStringLiteral("key"), QStringLiteral("Esc")},
                       {QStringLiteral("set"), true},
                       {QStringLiteral("changed"), false},
                       {QStringLiteral("listening"), false},
                       {QStringLiteral("fixed"), true},
                       {QStringLiteral("conflictInput"), QString()}});
  return l;
}

QVariantMap ControllersController::fixedPad() const {
  const QString mod = profiles_.padHotkeyModifier();
  return {{QStringLiteral("hudAll"), padHotkeyLabel(QStringLiteral("pad:leftstick"), mod)}, {QStringLiteral("panelTabs"), QString()}};
}

QVariantMap ControllersController::hotkeyLabels() const {
  QVariantMap m;
  for (const HotkeyDef& d : hotkeyDefs()) m.insert(d.id, keyLabelOf(profiles_.hotkey(d.id)));
  return m;
}

QString ControllersController::padHotkeyModifier() const {
  return padHotkeyLabel(padHotkeyBinding(profiles_.padHotkeyModifier(), true), QString());
}

QVariantMap ControllersController::hotkeyHints() const {
  QVariantMap m;
  for (const HotkeyDef& d : hotkeyDefs()) {
    const QString pad = profiles_.padHotkey(d.id);
    m.insert(d.id, padActive_ && !pad.isEmpty() ? padHotkeyLabel(pad, profiles_.padHotkeyModifier()) : keyLabelOf(profiles_.hotkey(d.id)));
  }
  return m;
}

QSet<int> ControllersController::hotkeyKeys() const {
  QSet<int> s;
  for (const HotkeyDef& d : hotkeyDefs()) {
    if (const int k = profiles_.hotkey(d.id); k > 0) s.insert(k);
  }
  return s;
}

QMap<int, QString> ControllersController::reservedKeyOwners() const {
  QMap<int, QString> m;
  for (const HotkeyDef& d : hotkeyDefs()) {
    if (const int k = profiles_.hotkey(d.id); k > 0) {
      m.insert(k, d.label);
      if (d.id == QLatin1String("layout")) m.insert(k | int(Qt::ShiftModifier), tr("Swap screens"));  // Shift + the layout key
    }
  }
  m.insert(Qt::Key_Escape, tr("Leave fullscreen / pause"));
  m.insert(Qt::Key_F4, tr("Hide all HUD widgets"));
  for (int n = 1; n <= 9; ++n) m.insert(int(Qt::Key_0) + n + int(Qt::AltModifier), tr("Panel tabs"));
  return m;
}

void ControllersController::hotkeysUpdated() {
  syncPadHotkeys();
  updateHold();
  emit hotkeysChanged();
  emit hotkeyHintsChanged();
  emit keyboardMapChanged();
}

void ControllersController::beginHotkeyCapture(const QString& action) {
  if (hotkeyIndex(action) < 0) return;
  cancelCapture();
  cancelPadHotkeyCapture();
  hotkeyListening_ = action;
  hotkeyNote_.clear();
  emit hotkeysChanged();
}

void ControllersController::cancelHotkeyCapture() {
  cancelPadHotkeyCapture();
  if (!hotkeyListening_.isEmpty() || !hotkeyNote_.isEmpty()) {
    hotkeyListening_.clear();
    hotkeyNote_.clear();
    emit hotkeysChanged();
  }
}

void ControllersController::clearHotkey(const QString& action) {
  cancelHotkeyCapture();
  if (profiles_.clearHotkey(action)) hotkeysUpdated();
}

void ControllersController::resetHotkey(const QString& action) {  // keyboard key and gamepad button
  cancelHotkeyCapture();
  const bool key = profiles_.resetHotkey(action);
  const bool pad = profiles_.resetPadHotkey(action);
  if (key || pad) hotkeysUpdated();
}

void ControllersController::clearPadHotkey(const QString& action) {
  cancelHotkeyCapture();
  if (profiles_.clearPadHotkey(action)) hotkeysUpdated();
}

void ControllersController::resetPadHotkeyModifier() {
  cancelHotkeyCapture();
  if (profiles_.resetPadHotkeyModifier()) hotkeysUpdated();
}

void ControllersController::beginPadHotkeyCapture(const QString& action) {
  if (action != QLatin1String("modifier") && hotkeyIndex(action) < 0) return;
  cancelCapture();
  cancelHotkeyCapture();
  padHotkeyListening_ = action;
  hotkeyNote_.clear();
  pads_.startCapture();
  emit hotkeysChanged();
}

void ControllersController::cancelPadHotkeyCapture() {
  if (padHotkeyListening_.isEmpty()) return;
  padHotkeyListening_.clear();
  pads_.cancelCapture();
  hotkeyNote_.clear();
  emit hotkeysChanged();
}

void ControllersController::capturePadHotkey(const QString& token, int deviceId) {
  const QString action = padHotkeyListening_;
  const QString modifier = profiles_.padHotkeyModifier();
  bool ok = false;
  if (action == QLatin1String("modifier")) {
    ok = profiles_.setPadHotkeyModifier(token);
  } else {
    if (token == modifier) {  // the hotkey button itself: wait for the button to combine with it
      pads_.startCapture();
      return;
    }
    const bool combo = pads_.pressedTokens(deviceId).contains(modifier);
    ok = profiles_.setPadHotkey(action, padHotkeyBinding(token, !combo));
  }
  if (!ok) {
    const QString owner = profiles_.padHotkeyAction(token);
    hotkeyNote_.clear();
    for (const HotkeyDef& d : hotkeyDefs()) {
      if (d.id == owner) hotkeyNote_ = tr("Already used by %1").arg(d.label);
    }
    if (hotkeyNote_.isEmpty()) hotkeyNote_ = tr("Not available as a hotkey button");
    pads_.startCapture();  // the field keeps listening
    emit hotkeysChanged();
    return;
  }
  padHotkeyListening_.clear();
  hotkeyNote_.clear();
  hotkeysUpdated();
}

void ControllersController::resetHotkeys() {
  cancelHotkeyCapture();
  if (profiles_.resetHotkeys()) hotkeysUpdated();
}

bool ControllersController::captureHotkey(int qtKey) {
  if (qtKey == Qt::Key_Escape) {
    cancelHotkeyCapture();
    return true;
  }
  if (qtKey == 0 || qtKey == Qt::Key_unknown || qtKey == Qt::Key_Shift || qtKey == Qt::Key_Control || qtKey == Qt::Key_Alt ||
      qtKey == Qt::Key_Meta) {
    return true;
  }
  const QString action = hotkeyListening_;
  if (qtKey == Qt::Key_F4) {  // fixed: hide all HUD widgets
    hotkeyNote_ = tr("Already used by %1").arg(tr("Hide all HUD widgets"));
    emit hotkeysChanged();
    return true;
  }
  if (!profiles_.setHotkey(action, qtKey)) {
    const QString owner = profiles_.hotkeyAction(qtKey);
    for (const HotkeyDef& d : hotkeyDefs()) {
      if (d.id == owner) hotkeyNote_ = tr("Already used by %1").arg(d.label);
    }
    emit hotkeysChanged();  // the binding stays, the field keeps listening
    return true;
  }
  hotkeyListening_.clear();
  hotkeyNote_.clear();
  hotkeysUpdated();
  return true;
}

void ControllersController::duplicateAndCapture(const QString& inputId) {
  if (!profileBuiltin()) {
    beginCapture(inputId);
    return;
  }
  duplicateProfile();
  if (!profileBuiltin()) beginCapture(inputId);
}

void ControllersController::selectDevice(const QString& key) {
  if (key == selected_) return;
  const QString before = selected_;
  selected_ = key;
  if (selection().kind.isEmpty()) {
    selected_ = before;
    return;
  }
  cancelCapture();
  heldKeys_.clear();
  emit selectionChanged();
  emit labelSetChanged();
  emit profilesChanged();
  emit rowsChanged();
  emit testChanged();
}

void ControllersController::profileChanged() {
  pads_.invalidateBindings();
  emit devicesChanged();
  emit profilesChanged();
  emit rowsChanged();
  emit testChanged();
  emit keyboardMapChanged();
  emit hotkeysChanged();  // conflictInput depends on the keyboard profile
}

void ControllersController::selectProfile(const QString& id) {
  const Selected s = selection();
  const auto p = profiles_.find(id);
  if (s.deviceKey.isEmpty() || !p || p->kind != s.kind) return;
  cancelCapture();
  profiles_.assign(s.deviceKey, id);
  profileChanged();
}

void ControllersController::duplicateProfile() {
  const Selected s = selection();
  const auto p = currentProfile();
  if (!p || s.deviceKey.isEmpty()) return;
  const QString id = profiles_.duplicate(p->id);
  if (!id.isEmpty()) {
    cancelCapture();
    profiles_.assign(s.deviceKey, id);
    profileChanged();
  }
}

void ControllersController::renameProfile(const QString& name) {
  const auto p = currentProfile();
  if (p && !p->builtin && profiles_.rename(p->id, name)) profileChanged();
}

void ControllersController::deleteProfile() {
  const auto p = currentProfile();
  if (p && !p->builtin) {
    cancelCapture();
    profiles_.remove(p->id);
    profileChanged();
  }
}

void ControllersController::resetProfile() {
  const auto p = currentProfile();
  if (p && !p->builtin) {
    cancelCapture();
    profiles_.resetToDefault(p->id);
    profileChanged();
  }
}

void ControllersController::beginCapture(const QString& inputId) {
  cancelHotkeyCapture();
  const auto p = currentProfile();
  const Selected s = selection();
  if (!p || p->builtin || inputIndex(inputId) < 0) return;  // built-in profiles are read-only
  listening_ = inputId;
  if (s.kind == QLatin1String("gamepad")) pads_.startCapture();
  emit rowsChanged();
}

void ControllersController::cancelCapture() {
  pads_.cancelCapture();
  if (!listening_.isEmpty()) {
    listening_.clear();
    emit rowsChanged();
  }
}

void ControllersController::clearBinding(const QString& inputId) {
  const auto p = currentProfile();
  if (!p || p->builtin) return;
  cancelCapture();
  if (profiles_.setBinding(p->id, inputId, {})) profileChanged();
}

void ControllersController::applyCapture(const QString& token) {
  const auto p = currentProfile();
  if (listening_.isEmpty() || !p || p->builtin) return;
  const QString input = listening_;
  listening_.clear();
  // A token drives one input: it is taken away from the input that had it before ("not mapped" there).
  for (auto it = p->bindings.cbegin(); it != p->bindings.cend(); ++it) {
    if (it.key() != input && it.value().contains(token)) {
      QStringList rest = it.value();
      rest.removeAll(token);
      profiles_.setBinding(p->id, it.key(), rest);
    }
  }
  profiles_.setBinding(p->id, input, {token});
  profileChanged();
}

bool ControllersController::captureKey(int qtKey) {
  if (!padHotkeyListening_.isEmpty()) {  // a gamepad button is awaited: Esc cancels, other keys are swallowed
    if (qtKey == Qt::Key_Escape) cancelPadHotkeyCapture();
    return true;
  }
  if (!hotkeyListening_.isEmpty()) return captureHotkey(qtKey);
  const Selected s = selection();
  if (listening_.isEmpty() || s.kind != kKeyboard) return false;
  if (qtKey == Qt::Key_Escape) {
    cancelCapture();
    return true;
  }
  if (qtKey == 0 || qtKey == Qt::Key_unknown || qtKey == Qt::Key_Shift || qtKey == Qt::Key_Control || qtKey == Qt::Key_Alt ||
      qtKey == Qt::Key_Meta) {
    return true;  // modifiers alone are not bindable; swallow them while listening
  }
  applyCapture(keyToken(qtKey));
  return true;
}

void ControllersController::testKey(int qtKey, bool pressed) {
  if (selection().kind != kKeyboard) return;
  if (pressed ? !heldKeys_.contains(qtKey) : heldKeys_.contains(qtKey)) {
    if (pressed) heldKeys_.insert(qtKey); else heldKeys_.remove(qtKey);
    emit testChanged();
  }
}

void ControllersController::clearTestKeys() {
  if (!heldKeys_.isEmpty()) {
    heldKeys_.clear();
    emit testChanged();
  }
}

}  // namespace uzume::ui

namespace uzume::ui {

void ControllersController::sendPadKey(int key) {
  QWindow* w = QGuiApplication::focusWindow();
  if (w == nullptr) {
    for (QWindow* t : QGuiApplication::topLevelWindows()) {
      if (qobject_cast<QQuickWindow*>(t) != nullptr && t->isVisible()) { w = t; break; }
    }
  }
  if (qobject_cast<QQuickWindow*>(w) == nullptr) return;
  notePadInput();
  synthesizing_ = true;
  QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
  QCoreApplication::sendEvent(w, &press);
  QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
  QCoreApplication::sendEvent(w, &release);
  synthesizing_ = false;
}

void ControllersController::feedPadNav(const QSet<QString>& tokens, qint64 nowMs) {
  const QSet<QString> fresh = QSet<QString>(tokens).subtract(padNavPrev_);
  padNavPrev_ = tokens;
  const bool mute = padNavMute_;
  padNavMute_ = false;
  if (mute || pads_.capturing() || !padHotkeyListening_.isEmpty() || !hotkeyListening_.isEmpty() || !listening_.isEmpty() ||
      (padNavGate_ && !padNavGate_())) {
    padNavDir_.clear();
    return;
  }
  const auto has = [&](const char* a, const char* b) { return tokens.contains(padToken(QString::fromLatin1(a))) || (b != nullptr && tokens.contains(padToken(QString::fromLatin1(b)))); };
  const auto isNew = [&](const char* n) { return fresh.contains(padToken(QString::fromLatin1(n))); };
  // Direction: one at a time, the first of up, down, left, right that is held.
  QString dir;
  int dirKey = 0;
  if (has("dpup", "lefty-")) { dir = QStringLiteral("up"); dirKey = Qt::Key_Up; }
  else if (has("dpdown", "lefty+")) { dir = QStringLiteral("down"); dirKey = Qt::Key_Down; }
  else if (has("dpleft", "leftx-")) { dir = QStringLiteral("left"); dirKey = Qt::Key_Left; }
  else if (has("dpright", "leftx+")) { dir = QStringLiteral("right"); dirKey = Qt::Key_Right; }
  if (dir != padNavDir_) {
    padNavDir_ = dir;
    if (!dir.isEmpty()) {
      sendPadKey(dirKey);
      padNavNext_ = nowMs + 400;
    }
  } else if (!dir.isEmpty() && nowMs >= padNavNext_) {
    sendPadKey(dirKey);
    padNavNext_ = nowMs + 80;
  }
  if (isNew("a")) sendPadKey(Qt::Key_Return);
  if (isNew("b")) sendPadKey(Qt::Key_Escape);
  if (isNew("y")) { notePadInput(); emit padCommand(QStringLiteral("search")); }
  if (isNew("leftshoulder")) { notePadInput(); emit padCommand(QStringLiteral("filterPrev")); }
  if (isNew("rightshoulder")) { notePadInput(); emit padCommand(QStringLiteral("filterNext")); }
}

}  // namespace uzume::ui
