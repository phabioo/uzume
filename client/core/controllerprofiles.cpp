#include "controllerprofiles.h"
#include "fsutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <Qt>

namespace uzume {

const QList<InputDef>& uzumeInputs() {
  static const QList<InputDef> inputs = {
      {QStringLiteral("a"), QStringLiteral("A"), QStringLiteral("A")},
      {QStringLiteral("b"), QStringLiteral("B"), QStringLiteral("B")},
      {QStringLiteral("x"), QStringLiteral("X"), QStringLiteral("X")},
      {QStringLiteral("y"), QStringLiteral("Y"), QStringLiteral("Y")},
      {QStringLiteral("l"), QStringLiteral("L"), QStringLiteral("L")},
      {QStringLiteral("r"), QStringLiteral("R"), QStringLiteral("R")},
      {QStringLiteral("start"), QStringLiteral("Start"), QStringLiteral("START")},
      {QStringLiteral("select"), QStringLiteral("Select"), QStringLiteral("SELECT")},
      {QStringLiteral("up"), QStringLiteral("D-pad up"), QStringLiteral("D-PAD ▲")},
      {QStringLiteral("down"), QStringLiteral("D-pad down"), QStringLiteral("D-PAD ▼")},
      {QStringLiteral("left"), QStringLiteral("D-pad left"), QStringLiteral("D-PAD ◀")},
      {QStringLiteral("right"), QStringLiteral("D-pad right"), QStringLiteral("D-PAD ▶")},
      // Appended so that the bit indices of the inputs above stay the same. Used by the "3ds" input profile (ZL/ZR,
      // C-stick); the nds profile maps ZL/ZR to L/R and the circle pad to the D-pad.
      {QStringLiteral("zl"), QStringLiteral("ZL"), QStringLiteral("ZL")},
      {QStringLiteral("zr"), QStringLiteral("ZR"), QStringLiteral("ZR")},
      {QStringLiteral("cup"), QStringLiteral("C-stick up"), QStringLiteral("C-STICK ▲")},
      {QStringLiteral("cdown"), QStringLiteral("C-stick down"), QStringLiteral("C-STICK ▼")},
      {QStringLiteral("cleft"), QStringLiteral("C-stick left"), QStringLiteral("C-STICK ◀")},
      {QStringLiteral("cright"), QStringLiteral("C-stick right"), QStringLiteral("C-STICK ▶")},
      {QStringLiteral("lup"), QStringLiteral("Circle pad up"), QStringLiteral("CIRCLE PAD ▲")},
      {QStringLiteral("ldown"), QStringLiteral("Circle pad down"), QStringLiteral("CIRCLE PAD ▼")},
      {QStringLiteral("lleft"), QStringLiteral("Circle pad left"), QStringLiteral("CIRCLE PAD ◀")},
      {QStringLiteral("lright"), QStringLiteral("Circle pad right"), QStringLiteral("CIRCLE PAD ▶")},
  };
  return inputs;
}

int inputIndex(const QString& id) {
  const auto& in = uzumeInputs();
  for (int i = 0; i < in.size(); ++i) {
    if (in.at(i).id == id) return i;
  }
  return -1;
}

const QList<HotkeyDef>& hotkeyDefs() {
  static const QList<HotkeyDef> defs = {
      {QStringLiteral("fullscreen"), QStringLiteral("Toggle fullscreen"), Qt::Key_F11, QStringLiteral("pad:b")},
      {QStringLiteral("diagnostics"), QStringLiteral("Diagnostics overlay"), Qt::Key_F3, QStringLiteral("pad:dpleft")},  // R3 went to push-to-talk (0.23)
      {QStringLiteral("snapshot"), QStringLiteral("Save snapshot"), Qt::Key_F5},
      {QStringLiteral("saveState"), QStringLiteral("Save state"), Qt::Key_F6, QStringLiteral("pad:rightshoulder")},  // Shift+F6 = next slot
      {QStringLiteral("loadState"), QStringLiteral("Load state"), Qt::Key_F8, QStringLiteral("pad:leftshoulder")},
      {QStringLiteral("rewind"), QStringLiteral("Rewind (hold)"), Qt::Key_Backspace, QStringLiteral("pad:lefttrigger")},
      {QStringLiteral("speedup"), QStringLiteral("Speed-up"), Qt::Key_Space, QStringLiteral("pad:righttrigger")},
      {QStringLiteral("volumeUp"), QStringLiteral("Volume up"), Qt::Key_Plus, QStringLiteral("pad:dpup")},
      {QStringLiteral("volumeDown"), QStringLiteral("Volume down"), Qt::Key_Minus, QStringLiteral("pad:dpdown")},
      {QStringLiteral("mute"), QStringLiteral("Mute"), Qt::Key_M, QStringLiteral("pad:x")},
      {QStringLiteral("screenshot"), QStringLiteral("Take screenshot"), Qt::Key_F12, QStringLiteral("pad:y")},
      {QStringLiteral("layout"), QStringLiteral("Screen layout"), Qt::Key_F7, QStringLiteral("pad:dpright")},  // Shift+F7 = swap screens
      {QStringLiteral("talk"), QStringLiteral("Push-to-talk (hold)"), Qt::Key_V, QStringLiteral("pad:rightstick")},  // voice chat (0.23); Shift+M = mic on/off
  };
  return defs;
}

int hotkeyIndex(const QString& id) {
  const auto& d = hotkeyDefs();
  for (int i = 0; i < d.size(); ++i) {
    if (d.at(i).id == id) return i;
  }
  return -1;
}

namespace {
const QString kSoloPrefix = QStringLiteral("solo:");
const QString kDefaultPadModifier = QStringLiteral("pad:back");
bool validPadToken(const QString& t) { return isPadToken(t) && t.size() > 4 && t != QLatin1String("pad:guide"); }
bool validPadBinding(const QString& b) { return validPadToken(padHotkeyToken(b)); }
}  // namespace

QString padHotkeyToken(const QString& binding) { return binding.startsWith(kSoloPrefix) ? binding.mid(kSoloPrefix.size()) : binding; }
bool padHotkeySolo(const QString& binding) { return binding.startsWith(kSoloPrefix); }
QString padHotkeyBinding(const QString& token, bool solo) { return solo ? kSoloPrefix + token : token; }

QString padHotkeyLabel(const QString& binding, const QString& modifierToken) {
  if (binding.isEmpty()) return {};
  const auto shortName = [](const QString& token) {
    static const QMap<QString, QString> kShort = {
        {QStringLiteral("pad:back"), QStringLiteral("Select")},
        {QStringLiteral("pad:start"), QStringLiteral("Start")},
        {QStringLiteral("pad:leftstick"), QStringLiteral("L3")},
        {QStringLiteral("pad:rightstick"), QStringLiteral("R3")},
    };
    return kShort.value(token, tokenLabel(token));
  };
  const QString btn = shortName(padHotkeyToken(binding));
  return padHotkeySolo(binding) ? btn : shortName(modifierToken) + QStringLiteral("+") + btn;
}

QString padToken(const QString& sdlName) { return QStringLiteral("pad:") + sdlName; }
bool isPadToken(const QString& token) { return token.startsWith(QLatin1String("pad:")); }
QString keyToken(int qtKey) { return QStringLiteral("key:") + QString::number(qtKey); }
std::optional<int> keyFromToken(const QString& token) {
  if (!token.startsWith(QLatin1String("key:"))) return std::nullopt;
  bool ok = false;
  const int k = token.mid(4).toInt(&ok);
  return ok ? std::optional<int>(k) : std::nullopt;
}

QString tokenLabel(const QString& token) {
  if (const auto k = keyFromToken(token)) {
    // Qt::Key has a meta enum in QtCore (no QtGui needed): "Key_Return" -> "Return".
    if (const char* name = QMetaEnum::fromType<Qt::Key>().valueToKey(*k)) {
      QString n = QString::fromLatin1(name);
      if (n.startsWith(QLatin1String("Key_"))) n = n.mid(4);
      return n;
    }
    const char32_t ucs = static_cast<char32_t>(*k);
    return *k > 0x20 && *k < 0x110000 ? QString::fromUcs4(reinterpret_cast<const char32_t*>(&ucs), 1) : QStringLiteral("Key %1").arg(*k);
  }
  static const QMap<QString, QString> kNames = {
      {QStringLiteral("a"), QStringLiteral("A")},
      {QStringLiteral("b"), QStringLiteral("B")},
      {QStringLiteral("x"), QStringLiteral("X")},
      {QStringLiteral("y"), QStringLiteral("Y")},
      {QStringLiteral("back"), QStringLiteral("View / Back")},
      {QStringLiteral("guide"), QStringLiteral("Guide")},
      {QStringLiteral("start"), QStringLiteral("Menu / Start")},
      {QStringLiteral("leftstick"), QStringLiteral("Left stick (press)")},
      {QStringLiteral("rightstick"), QStringLiteral("Right stick (press)")},
      {QStringLiteral("leftshoulder"), QStringLiteral("LB")},
      {QStringLiteral("rightshoulder"), QStringLiteral("RB")},
      {QStringLiteral("lefttrigger"), QStringLiteral("LT")},
      {QStringLiteral("righttrigger"), QStringLiteral("RT")},
      {QStringLiteral("dpup"), QStringLiteral("D-Pad ▲")},
      {QStringLiteral("dpdown"), QStringLiteral("D-Pad ▼")},
      {QStringLiteral("dpleft"), QStringLiteral("D-Pad ◀")},
      {QStringLiteral("dpright"), QStringLiteral("D-Pad ▶")},
      {QStringLiteral("leftx-"), QStringLiteral("Left stick ◀")},
      {QStringLiteral("leftx+"), QStringLiteral("Left stick ▶")},
      {QStringLiteral("lefty-"), QStringLiteral("Left stick ▲")},
      {QStringLiteral("lefty+"), QStringLiteral("Left stick ▼")},
      {QStringLiteral("rightx-"), QStringLiteral("Right stick ◀")},
      {QStringLiteral("rightx+"), QStringLiteral("Right stick ▶")},
      {QStringLiteral("righty-"), QStringLiteral("Right stick ▲")},
      {QStringLiteral("righty+"), QStringLiteral("Right stick ▼")},
  };
  if (isPadToken(token)) {
    const QString n = token.mid(4);
    return kNames.value(n, n);
  }
  return token;
}

namespace {
bool isLabelSet(const QString& v) {
  return v == QLatin1String("xbox") || v == QLatin1String("playstation") || v == QLatin1String("generic");
}
}  // namespace

ControllerProfile ControllerProfiles::builtinProfile(const QString& kind) {
  ControllerProfile p;
  p.builtin = true;
  p.kind = kind;
  if (kind == QLatin1String("keyboard")) {
    p.id = QString::fromLatin1(kBuiltinKeyboardId);
    p.name = QStringLiteral("Keyboard · Standard");
    // Same map as the former fixed keyboard mapping: arrows, X=A, Z=B, S=X, A=Y, Q=L, W=R, Enter=Start, Tab=Select (Backspace is the rewind hotkey since 0.20).
    p.bindings = {
        {QStringLiteral("a"), {keyToken(Qt::Key_X)}},
        {QStringLiteral("b"), {keyToken(Qt::Key_Z)}},
        {QStringLiteral("x"), {keyToken(Qt::Key_S)}},
        {QStringLiteral("y"), {keyToken(Qt::Key_A)}},
        {QStringLiteral("l"), {keyToken(Qt::Key_Q)}},
        {QStringLiteral("r"), {keyToken(Qt::Key_W)}},
        {QStringLiteral("start"), {keyToken(Qt::Key_Return), keyToken(Qt::Key_Enter)}},
        {QStringLiteral("select"), {keyToken(Qt::Key_Tab)}},
        {QStringLiteral("up"), {keyToken(Qt::Key_Up)}},
        {QStringLiteral("down"), {keyToken(Qt::Key_Down)}},
        {QStringLiteral("left"), {keyToken(Qt::Key_Left)}},
        {QStringLiteral("right"), {keyToken(Qt::Key_Right)}},
        {QStringLiteral("zl"), {keyToken(Qt::Key_E)}},
        {QStringLiteral("zr"), {keyToken(Qt::Key_R)}},
        {QStringLiteral("cup"), {keyToken(Qt::Key_I)}},
        {QStringLiteral("cdown"), {keyToken(Qt::Key_K)}},
        {QStringLiteral("cleft"), {keyToken(Qt::Key_J)}},
        {QStringLiteral("cright"), {keyToken(Qt::Key_L)}},
        {QStringLiteral("lup"), {keyToken(Qt::Key_T)}},
        {QStringLiteral("ldown"), {keyToken(Qt::Key_G)}},
        {QStringLiteral("lleft"), {keyToken(Qt::Key_F)}},
        {QStringLiteral("lright"), {keyToken(Qt::Key_H)}},
    };
  } else {
    p.id = QString::fromLatin1(kBuiltinGamepadId);
    p.name = QStringLiteral("Standard Gamepad");
    p.kind = QStringLiteral("gamepad");
    // The DS layout follows the positions of the buttons: NDS A = east button, B = south, X = north, Y = west.
    p.bindings = {
        {QStringLiteral("a"), {padToken(QStringLiteral("b"))}},
        {QStringLiteral("b"), {padToken(QStringLiteral("a"))}},
        {QStringLiteral("x"), {padToken(QStringLiteral("y"))}},
        {QStringLiteral("y"), {padToken(QStringLiteral("x"))}},
        {QStringLiteral("l"), {padToken(QStringLiteral("leftshoulder"))}},
        {QStringLiteral("r"), {padToken(QStringLiteral("rightshoulder"))}},
        {QStringLiteral("zl"), {padToken(QStringLiteral("lefttrigger"))}},
        {QStringLiteral("zr"), {padToken(QStringLiteral("righttrigger"))}},
        {QStringLiteral("cup"), {padToken(QStringLiteral("righty-"))}},
        {QStringLiteral("cdown"), {padToken(QStringLiteral("righty+"))}},
        {QStringLiteral("cleft"), {padToken(QStringLiteral("rightx-"))}},
        {QStringLiteral("cright"), {padToken(QStringLiteral("rightx+"))}},
        {QStringLiteral("start"), {padToken(QStringLiteral("start"))}},
        {QStringLiteral("select"), {padToken(QStringLiteral("back"))}},
        {QStringLiteral("up"), {padToken(QStringLiteral("dpup"))}},
        {QStringLiteral("down"), {padToken(QStringLiteral("dpdown"))}},
        {QStringLiteral("left"), {padToken(QStringLiteral("dpleft"))}},
        {QStringLiteral("right"), {padToken(QStringLiteral("dpright"))}},
        // The left stick is the circle pad; the DS profile folds it into the D-pad, so DS play is unchanged.
        {QStringLiteral("lup"), {padToken(QStringLiteral("lefty-"))}},
        {QStringLiteral("ldown"), {padToken(QStringLiteral("lefty+"))}},
        {QStringLiteral("lleft"), {padToken(QStringLiteral("leftx-"))}},
        {QStringLiteral("lright"), {padToken(QStringLiteral("leftx+"))}},
    };
  }
  return p;
}

ControllerProfiles::ControllerProfiles(const QString& baseDir)
    : path_(QDir(baseDir).filePath(QStringLiteral("settings/controllers.json"))) {
  for (const HotkeyDef& d : hotkeyDefs()) {
    hotkeys_.insert(d.id, d.defaultKey);
    padHotkeys_.insert(d.id, d.defaultPad);
  }
  padModifier_ = kDefaultPadModifier;
  raw_ = fsutil::readJsonObject(path_);  // a corrupt file is moved aside before anything can overwrite it
  if (raw_.isEmpty()) {
    return;
  }
  for (const QJsonValue& v : raw_.value(QStringLiteral("profiles")).toArray()) {
    const QJsonObject o = v.toObject();
    ControllerProfile p;
    p.id = o.value(QStringLiteral("id")).toString();
    p.name = o.value(QStringLiteral("name")).toString();
    p.kind = o.value(QStringLiteral("kind")).toString();
    if (p.id.isEmpty() || p.name.isEmpty() || (p.kind != QLatin1String("gamepad") && p.kind != QLatin1String("keyboard"))) {
      continue;
    }
    const QJsonObject b = o.value(QStringLiteral("bindings")).toObject();
    for (auto it = b.begin(); it != b.end(); ++it) {
      if (inputIndex(it.key()) < 0) continue;
      QStringList tokens;
      for (const QJsonValue& t : it.value().toArray()) {
        if (t.isString() && !t.toString().isEmpty()) tokens.append(t.toString());
      }
      p.bindings.insert(it.key(), tokens);
    }
    user_.append(p);
  }
  const QJsonObject a = raw_.value(QStringLiteral("assignments")).toObject();
  for (auto it = a.begin(); it != a.end(); ++it) {
    if (it.value().isString()) assignments_.insert(it.key(), it.value().toString());
  }
  const QJsonObject ls = raw_.value(QStringLiteral("labelSets")).toObject();
  for (auto it = ls.begin(); it != ls.end(); ++it) {
    if (it.value().isString() && isLabelSet(it.value().toString())) labelSets_.insert(it.key(), it.value().toString());
  }
  // Hotkeys: a missing, corrupted or conflicting entry falls back to the action's default.
  const QJsonObject hk = raw_.value(QStringLiteral("hotkeys")).toObject();
  QSet<int> taken{Qt::Key_Escape};
  QMap<QString, int> chosen;
  for (const HotkeyDef& d : hotkeyDefs()) {
    const QJsonValue v = hk.value(d.id);
    if (!v.isString()) continue;
    if (v.toString() == QLatin1String("none")) {
      chosen.insert(d.id, 0);
      continue;
    }
    const auto k = keyFromToken(v.toString());
    if (k && *k > 0 && *k != Qt::Key_Escape && !taken.contains(*k)) {
      taken.insert(*k);
      chosen.insert(d.id, *k);
    }
  }
  for (const HotkeyDef& d : hotkeyDefs()) {
    int k = chosen.value(d.id, -1);
    if (k < 0) k = d.defaultKey;
    hotkeys_.insert(d.id, k);
  }
  // A default that now clashes with a chosen key of another action is unassigned (the explicit choice wins).
  QSet<int> seen;
  for (const HotkeyDef& d : hotkeyDefs()) {
    if (chosen.contains(d.id) && chosen.value(d.id) != 0) seen.insert(chosen.value(d.id));
  }
  for (const HotkeyDef& d : hotkeyDefs()) {
    if (!chosen.contains(d.id) && seen.contains(hotkeys_.value(d.id))) hotkeys_.insert(d.id, 0);
  }
  // Gamepad hotkeys: same rules (an invalid or conflicting entry falls back to the default, a default that clashes is unassigned).
  const QString mod = raw_.value(QStringLiteral("padHotkeyModifier")).toString();
  if (validPadToken(mod)) padModifier_ = mod;
  const QJsonObject ph = raw_.value(QStringLiteral("padHotkeys")).toObject();
  QSet<QString> padTaken{padModifier_};
  QMap<QString, QString> padChosen;
  for (const HotkeyDef& d : hotkeyDefs()) {
    const QJsonValue v = ph.value(d.id);
    if (!v.isString()) continue;
    if (v.toString() == QLatin1String("none")) {
      padChosen.insert(d.id, QString());
    } else if (validPadBinding(v.toString()) && !padTaken.contains(padHotkeyToken(v.toString()))) {
      padTaken.insert(padHotkeyToken(v.toString()));
      padChosen.insert(d.id, v.toString());
    }
  }
  for (const HotkeyDef& d : hotkeyDefs()) {
    QString b = padChosen.contains(d.id) ? padChosen.value(d.id) : d.defaultPad;
    if (!padChosen.contains(d.id) && !b.isEmpty() && padTaken.contains(padHotkeyToken(b))) b.clear();
    padHotkeys_.insert(d.id, b);
  }
}

int ControllerProfiles::hotkey(const QString& actionId) const { return hotkeys_.value(actionId, 0); }

QString ControllerProfiles::hotkeyAction(int qtKey) const {
  if (qtKey <= 0) return {};
  for (const HotkeyDef& d : hotkeyDefs()) {
    if (hotkeys_.value(d.id) == qtKey) return d.id;
  }
  return {};
}

bool ControllerProfiles::setHotkey(const QString& actionId, int qtKey) {
  if (hotkeyIndex(actionId) < 0 || qtKey <= 0 || qtKey == Qt::Key_Escape) return false;
  const QString owner = hotkeyAction(qtKey);
  if (!owner.isEmpty() && owner != actionId) return false;
  hotkeys_.insert(actionId, qtKey);
  return save();
}

bool ControllerProfiles::clearHotkey(const QString& actionId) {
  if (hotkeyIndex(actionId) < 0) return false;
  hotkeys_.insert(actionId, 0);
  return save();
}

bool ControllerProfiles::resetHotkey(const QString& actionId) {
  const int i = hotkeyIndex(actionId);
  if (i < 0) return false;
  const int def = hotkeyDefs().at(i).defaultKey;
  // The default may be held by another action meanwhile: that one is unassigned (the reset wins).
  const QString owner = hotkeyAction(def);
  if (!owner.isEmpty() && owner != actionId) hotkeys_.insert(owner, 0);
  hotkeys_.insert(actionId, def);
  return save();
}

bool ControllerProfiles::resetHotkeys() {
  for (const HotkeyDef& d : hotkeyDefs()) {
    hotkeys_.insert(d.id, d.defaultKey);
    padHotkeys_.insert(d.id, d.defaultPad);
  }
  padModifier_ = kDefaultPadModifier;
  return save();
}

int ControllerProfiles::hotkeysChangedCount() const {
  int n = padModifier_ != kDefaultPadModifier ? 1 : 0;
  for (const HotkeyDef& d : hotkeyDefs()) {
    n += hotkeys_.value(d.id) != d.defaultKey ? 1 : 0;
    n += padHotkeys_.value(d.id) != d.defaultPad ? 1 : 0;
  }
  return n;
}

QString ControllerProfiles::padHotkey(const QString& actionId) const { return padHotkeys_.value(actionId); }

QString ControllerProfiles::padHotkeyAction(const QString& token) const {
  if (token.isEmpty()) return {};
  for (const HotkeyDef& d : hotkeyDefs()) {
    if (padHotkeyToken(padHotkeys_.value(d.id)) == token) return d.id;
  }
  return {};
}

bool ControllerProfiles::setPadHotkey(const QString& actionId, const QString& binding) {
  if (hotkeyIndex(actionId) < 0 || !validPadBinding(binding)) return false;
  const QString token = padHotkeyToken(binding);
  if (token == padModifier_) return false;
  const QString owner = padHotkeyAction(token);
  if (!owner.isEmpty() && owner != actionId) return false;
  padHotkeys_.insert(actionId, binding);
  return save();
}

bool ControllerProfiles::clearPadHotkey(const QString& actionId) {
  if (hotkeyIndex(actionId) < 0) return false;
  padHotkeys_.insert(actionId, QString());
  return save();
}

bool ControllerProfiles::resetPadHotkey(const QString& actionId) {
  const int i = hotkeyIndex(actionId);
  if (i < 0) return false;
  const QString def = hotkeyDefs().at(i).defaultPad;
  // The default may be held by another action or be the modifier meanwhile: the other action is unassigned and the
  // modifier falls back to its default (the reset of this action wins).
  if (!def.isEmpty()) {
    const QString owner = padHotkeyAction(padHotkeyToken(def));
    if (!owner.isEmpty() && owner != actionId) padHotkeys_.insert(owner, QString());
    if (padModifier_ == padHotkeyToken(def)) padModifier_ = kDefaultPadModifier;
  }
  padHotkeys_.insert(actionId, def);
  return save();
}

bool ControllerProfiles::setPadHotkeyModifier(const QString& token) {
  if (!validPadToken(token) || !padHotkeyAction(token).isEmpty()) return false;
  padModifier_ = token;
  return save();
}

bool ControllerProfiles::padHotkeyModifierChanged() const { return padModifier_ != kDefaultPadModifier; }

bool ControllerProfiles::resetPadHotkeyModifier() {
  // The default modifier may be held by an action meanwhile: that action is unassigned.
  const QString owner = padHotkeyAction(kDefaultPadModifier);
  if (!owner.isEmpty()) padHotkeys_.insert(owner, QString());
  padModifier_ = kDefaultPadModifier;
  return save();
}

QList<ControllerProfile> ControllerProfiles::all() const {
  QList<ControllerProfile> l{builtinProfile(QStringLiteral("gamepad")), builtinProfile(QStringLiteral("keyboard"))};
  l.append(user_);
  return l;
}

QList<ControllerProfile> ControllerProfiles::ofKind(const QString& kind) const {
  QList<ControllerProfile> l;
  for (const ControllerProfile& p : all()) {
    if (p.kind == kind) l.append(p);
  }
  return l;
}

std::optional<ControllerProfile> ControllerProfiles::find(const QString& id) const {
  for (const ControllerProfile& p : all()) {
    if (p.id == id) return p;
  }
  return std::nullopt;
}

QString ControllerProfiles::uniqueName(const QString& base) const {
  const auto exists = [this](const QString& n) {
    for (const ControllerProfile& p : all()) {
      if (p.name.compare(n, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
  };
  if (!exists(base)) return base;
  for (int i = 2;; ++i) {
    const QString n = QStringLiteral("%1 %2").arg(base).arg(i);
    if (!exists(n)) return n;
  }
}

QString ControllerProfiles::duplicate(const QString& id, const QString& newName) {
  const auto src = find(id);
  if (!src) return {};
  ControllerProfile p = *src;
  p.builtin = false;
  p.id = QStringLiteral("p-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
  p.name = uniqueName(newName.trimmed().isEmpty() ? src->name + QStringLiteral(" copy") : newName.trimmed());
  user_.append(p);
  save();
  return p.id;
}

bool ControllerProfiles::rename(const QString& id, const QString& name) {
  const QString n = name.trimmed();
  if (n.isEmpty()) return false;
  for (ControllerProfile& p : user_) {
    if (p.id == id) {
      p.name = n;
      return save();
    }
  }
  return false;
}

bool ControllerProfiles::remove(const QString& id) {
  for (qsizetype i = 0; i < user_.size(); ++i) {
    if (user_.at(i).id == id) {
      user_.removeAt(i);
      for (auto it = assignments_.begin(); it != assignments_.end();) {
        it = it.value() == id ? assignments_.erase(it) : std::next(it);
      }
      return save();
    }
  }
  return false;
}

bool ControllerProfiles::setBinding(const QString& id, const QString& inputId, const QStringList& tokens) {
  if (inputIndex(inputId) < 0) return false;
  for (ControllerProfile& p : user_) {
    if (p.id == id) {
      p.bindings.insert(inputId, tokens);
      return save();
    }
  }
  return false;
}

bool ControllerProfiles::resetToDefault(const QString& id) {
  for (ControllerProfile& p : user_) {
    if (p.id == id) {
      p.bindings = builtinProfile(p.kind).bindings;
      return save();
    }
  }
  return false;
}

QString ControllerProfiles::assignedProfileId(const QString& deviceKey, const QString& kind) const {
  const QString id = assignments_.value(deviceKey);
  if (!id.isEmpty()) {
    const auto p = find(id);
    if (p && p->kind == kind) return id;
  }
  return QString::fromLatin1(kind == QLatin1String("keyboard") ? kBuiltinKeyboardId : kBuiltinGamepadId);
}

bool ControllerProfiles::assign(const QString& deviceKey, const QString& profileId) {
  if (deviceKey.isEmpty() || !find(profileId)) return false;
  assignments_.insert(deviceKey, profileId);
  return save();
}

bool ControllerProfiles::setLabelSet(const QString& deviceKey, const QString& value) {
  if (deviceKey.isEmpty() || (!value.isEmpty() && !isLabelSet(value))) return false;
  if (value.isEmpty()) {
    if (labelSets_.remove(deviceKey) == 0) return true;
  } else {
    if (labelSets_.value(deviceKey) == value) return true;
    labelSets_.insert(deviceKey, value);
  }
  return save();
}

bool ControllerProfiles::save() const {
  if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
    return false;
  }
  QJsonObject root = raw_;
  QJsonArray profiles;
  for (const ControllerProfile& p : user_) {
    QJsonObject b;
    for (auto it = p.bindings.cbegin(); it != p.bindings.cend(); ++it) {
      b.insert(it.key(), QJsonArray::fromStringList(it.value()));
    }
    profiles.append(QJsonObject{{QStringLiteral("id"), p.id},
                                {QStringLiteral("name"), p.name},
                                {QStringLiteral("kind"), p.kind},
                                {QStringLiteral("bindings"), b}});
  }
  root.insert(QStringLiteral("profiles"), profiles);
  QJsonObject a;
  for (auto it = assignments_.cbegin(); it != assignments_.cend(); ++it) {
    a.insert(it.key(), it.value());
  }
  root.insert(QStringLiteral("assignments"), a);
  if (labelSets_.isEmpty()) {
    root.remove(QStringLiteral("labelSets"));
  } else {
    QJsonObject ls;
    for (auto it = labelSets_.cbegin(); it != labelSets_.cend(); ++it) ls.insert(it.key(), it.value());
    root.insert(QStringLiteral("labelSets"), ls);
  }
  QJsonObject hk;
  for (const HotkeyDef& d : hotkeyDefs()) {
    const int k = hotkeys_.value(d.id, d.defaultKey);
    if (k != d.defaultKey) hk.insert(d.id, k == 0 ? QStringLiteral("none") : keyToken(k));
  }
  if (hk.isEmpty()) {
    root.remove(QStringLiteral("hotkeys"));
  } else {
    root.insert(QStringLiteral("hotkeys"), hk);
  }
  QJsonObject ph;
  for (const HotkeyDef& d : hotkeyDefs()) {
    const QString b = padHotkeys_.value(d.id, d.defaultPad);
    if (b != d.defaultPad) ph.insert(d.id, b.isEmpty() ? QStringLiteral("none") : b);
  }
  if (ph.isEmpty()) {
    root.remove(QStringLiteral("padHotkeys"));
  } else {
    root.insert(QStringLiteral("padHotkeys"), ph);
  }
  if (padModifier_ == kDefaultPadModifier) {
    root.remove(QStringLiteral("padHotkeyModifier"));
  } else {
    root.insert(QStringLiteral("padHotkeyModifier"), padModifier_);
  }
  QSaveFile f(path_);
  if (!f.open(QIODevice::WriteOnly)) {
    return false;
  }
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  return f.commit();
}

}  // namespace uzume
