#include "gamepadservice.h"

#include <SDL3/SDL.h>

#include "controllerprofiles.h"

namespace uzume::input {

namespace {
constexpr int kPressThreshold = 16384;   // about 50 % of the axis range
constexpr int kReleaseThreshold = 10000; // hysteresis, so a stick on the edge does not flutter

QString buttonToken(SDL_GamepadButton b) {
  const char* n = SDL_GetGamepadStringForButton(b);
  return n != nullptr ? padToken(QString::fromLatin1(n)) : QString();
}
}  // namespace

struct GamepadService::Dev {
  int id = 0;
  SDL_Gamepad* pad = nullptr;
  QString name;
  QString key;
  int slot = 0;
  PadType type = PadType::Generic;
  QSet<QString> pressed;
};

GamepadService::GamepadService(QObject* parent) : QObject(parent) {
  connect(&timer_, &QTimer::timeout, this, &GamepadService::poll);
}

GamepadService::~GamepadService() {
  timer_.stop();
  for (Dev* d : std::as_const(devs_)) {
    SDL_CloseGamepad(d->pad);
    delete d;
  }
  devs_.clear();
  if (sdlInited_) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
}

bool GamepadService::start(int pollIntervalMs) {
  if (sdlInited_) return available_;
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  // keep playing while the window is not focused
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_Init(SDL_INIT_GAMEPAD)) {
    error_ = QString::fromUtf8(SDL_GetError());
    available_ = false;
    return false;
  }
  sdlInited_ = true;
  available_ = true;
  error_.clear();
  // Pads that are already connected come in as ADDED events on the first poll.
  poll();
  if (pollIntervalMs > 0) {
    timer_.start(pollIntervalMs);
  }
  return true;
}

GamepadService::Dev* GamepadService::find(int id) {
  for (Dev* d : std::as_const(devs_)) {
    if (d->id == id) return d;
  }
  return nullptr;
}
const GamepadService::Dev* GamepadService::find(int id) const {
  for (const Dev* d : devs_) {
    if (d->id == id) return d;
  }
  return nullptr;
}

QList<PadDevice> GamepadService::devices() const {
  QList<PadDevice> l;
  for (const Dev* d : devs_) l.append({d->id, d->name, d->key, d->slot, d->type});
  return l;
}

PadDevice GamepadService::device(int id) const {
  const Dev* d = find(id);
  return d ? PadDevice{d->id, d->name, d->key, d->slot, d->type} : PadDevice{};
}

void GamepadService::setBindingsProvider(BindingsProvider provider) {
  provider_ = std::move(provider);
  invalidateBindings();
}

void GamepadService::invalidateBindings() {
  compiledByKey_.clear();
  poll();
}

const QHash<QString, quint32>& GamepadService::compiled(const Dev& d) const {
  auto it = compiledByKey_.find(d.key);
  if (it == compiledByKey_.end()) {
    const Bindings b = provider_ ? provider_(d.key) : ControllerProfiles::builtinProfile(QStringLiteral("gamepad")).bindings;
    it = compiledByKey_.insert(d.key, compileBindings(b));
  }
  return it.value();
}

void GamepadService::refreshSlots() {
  for (int i = 0; i < devs_.size(); ++i) devs_[i]->slot = i == 0 ? 1 : 0;
}

void GamepadService::openDevice(int id) {
  if (find(id) != nullptr) return;
  SDL_Gamepad* pad = SDL_OpenGamepad(static_cast<SDL_JoystickID>(id));
  if (pad == nullptr) return;
  auto* d = new Dev;
  d->id = id;
  d->pad = pad;
  const char* n = SDL_GetGamepadName(pad);
  d->name = n != nullptr && n[0] != '\0' ? QString::fromUtf8(n) : QStringLiteral("Gamepad");
  char guid[64] = {};
  SDL_GUIDToString(SDL_GetGamepadGUIDForID(static_cast<SDL_JoystickID>(id)), guid, sizeof(guid));
  d->key = QString::fromLatin1(guid);
  d->type = classifyPad(static_cast<int>(SDL_GetGamepadType(pad)), SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad));
  devs_.append(d);
  refreshSlots();
  emit devicesChanged();
}

void GamepadService::closeDevice(int id) {
  for (qsizetype i = 0; i < devs_.size(); ++i) {
    if (devs_.at(i)->id == id) {
      Dev* d = devs_.takeAt(i);
      SDL_CloseGamepad(d->pad);
      delete d;
      refreshSlots();
      emit devicesChanged();
      return;
    }
  }
}

QSet<QString> GamepadService::readTokens(Dev& d) const {
  QSet<QString> out;
  for (int b = SDL_GAMEPAD_BUTTON_SOUTH; b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT; ++b) {
    if (SDL_GetGamepadButton(d.pad, static_cast<SDL_GamepadButton>(b))) {
      out.insert(buttonToken(static_cast<SDL_GamepadButton>(b)));
    }
  }
  const auto axisOn = [&](SDL_GamepadAxis a, const QString& token, bool positive) {
    const int v = SDL_GetGamepadAxis(d.pad, a);
    const int signedV = positive ? v : -v;
    const int thr = d.pressed.contains(token) ? kReleaseThreshold : kPressThreshold;
    if (signedV > thr) out.insert(token);
  };
  axisOn(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, padToken(QStringLiteral("lefttrigger")), true);
  axisOn(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, padToken(QStringLiteral("righttrigger")), true);
  axisOn(SDL_GAMEPAD_AXIS_LEFTX, padToken(QStringLiteral("leftx-")), false);
  axisOn(SDL_GAMEPAD_AXIS_LEFTX, padToken(QStringLiteral("leftx+")), true);
  axisOn(SDL_GAMEPAD_AXIS_LEFTY, padToken(QStringLiteral("lefty-")), false);
  axisOn(SDL_GAMEPAD_AXIS_LEFTY, padToken(QStringLiteral("lefty+")), true);
  axisOn(SDL_GAMEPAD_AXIS_RIGHTX, padToken(QStringLiteral("rightx-")), false);
  axisOn(SDL_GAMEPAD_AXIS_RIGHTX, padToken(QStringLiteral("rightx+")), true);
  axisOn(SDL_GAMEPAD_AXIS_RIGHTY, padToken(QStringLiteral("righty-")), false);
  axisOn(SDL_GAMEPAD_AXIS_RIGHTY, padToken(QStringLiteral("righty+")), true);
  return out;
}

void GamepadService::poll() {
  if (!sdlInited_) return;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
      openDevice(static_cast<int>(e.gdevice.which));
    } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
      closeDevice(static_cast<int>(e.gdevice.which));
    }
  }
  SDL_UpdateGamepads();

  const bool wasCapturing = capturing_;
  bool changed = false;
  for (Dev* d : std::as_const(devs_)) {
    const QSet<QString> now = readTokens(*d);
    if (now != d->pressed) {
      d->pressed = now;
      changed = true;
    }
  }
  if (capturing_) {
    QSet<QString> held;
    for (const Dev* d : std::as_const(devs_)) held.unite(d->pressed);
    captureBaseline_.intersect(held);  // released inputs may be captured again
    QString token;
    int deviceId = 0;
    for (const Dev* d : std::as_const(devs_)) {
      for (const QString& t : d->pressed) {
        if (token.isEmpty() && !captureBaseline_.contains(t) && t != padToken(QStringLiteral("guide"))) {
          token = t;
          deviceId = d->id;
        }
      }
    }
    if (!token.isEmpty()) {
      capturing_ = false;
      emit captured(token, deviceId);
    }
  }
  if (changed) {
    emit stateChanged();
  }
  QSet<QString> hidden;
  if (devs_.isEmpty()) {
    hotkeys_.reset();
  } else {
    // While (or right after) a capture the detector only tracks the state: the buttons that were just bound must not fire.
    const QStringList fired = hotkeys_.update(devs_.first()->pressed);
    if (!wasCapturing && !capturing_) {
      hidden = hotkeys_.suppressed();
      for (const QString& a : fired) emit hotkeyTriggered(a);
    }
  }
  const quint32 inMask = devs_.isEmpty() ? 0u : maskFor(*devs_.first(), hidden);
  const quint32 lr = libretroMaskFor(inputProfile_, inMask);
  p1InputMask_ = inMask;
  if (lr != p1Libretro_) {
    p1Libretro_ = lr;
    emit libretroMaskChanged(lr);
  }
  emit polled();
}

quint32 GamepadService::maskFor(const Dev& d, const QSet<QString>& hidden) const {
  const QHash<QString, quint32>& c = compiled(d);
  quint32 m = 0;
  for (const QString& t : d.pressed) {
    if (!hidden.contains(t)) m |= c.value(t, 0u);
  }
  return m;
}

quint32 GamepadService::inputMaskFor(int deviceId) const {
  const Dev* d = find(deviceId);
  return d == nullptr ? 0u : maskFor(*d, {});
}

void GamepadService::setPadHotkeys(const QString& modifierToken, const QList<PadHotkeyBinding>& bindings) {
  hotkeys_.setBindings(modifierToken, bindings);
}

QSet<QString> GamepadService::pressedTokens(int deviceId) const {
  const Dev* d = find(deviceId);
  return d ? d->pressed : QSet<QString>();
}

void GamepadService::startCapture() {
  captureBaseline_.clear();
  for (const Dev* d : std::as_const(devs_)) captureBaseline_.unite(d->pressed);
  capturing_ = true;
}

void GamepadService::cancelCapture() { capturing_ = false; }

}  // namespace uzume::input
