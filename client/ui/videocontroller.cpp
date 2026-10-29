#include "videocontroller.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSurfaceFormat>
#include <QWindow>

#include "playersettings.h"

namespace uzume::ui {

namespace {
// The stored name of a monitor: QScreen::name(), or "screen-N" for the rare platform that gives none (an empty value would mean
// "where the window is").
QString screenKey(const QScreen* s) {
  if (!s->name().isEmpty()) return s->name();
  return QStringLiteral("screen-%1").arg(QGuiApplication::screens().indexOf(const_cast<QScreen*>(s)) + 1);
}
}  // namespace

VideoController::VideoController(PlayerSettings* settings, QObject* parent) : QObject(parent), settings_(settings) {
  if (settings_) {
    screen_ = settings_->videoFullscreenScreen();
    vsync_ = settings_->videoVsync();
  }
  if (auto* app = qGuiApp) {
    connect(app, &QGuiApplication::screenAdded, this, &VideoController::screensChanged);
    connect(app, &QGuiApplication::screenRemoved, this, &VideoController::screensChanged);
  }
}

void VideoController::setFullscreenScreen(const QString& name) {
  if (name == screen_) return;
  screen_ = name;
  if (settings_) settings_->setVideoFullscreenScreen(name);
  emit changed();
}

void VideoController::setVsync(bool on) {
  if (on == vsync_) return;
  vsync_ = on;
  if (settings_) settings_->setVideoVsync(on);
  emit changed();
}

QVariantList VideoController::screens() const {
  QVariantList l;
  l.append(QVariantMap{{QStringLiteral("value"), QString()}, {QStringLiteral("label"), tr("Where the window is")}});
  const auto all = QGuiApplication::screens();
  for (QScreen* s : all) {
    QString label = tr("%1 · %2 × %3").arg(screenKey(s)).arg(s->geometry().width()).arg(s->geometry().height());
    if (s == QGuiApplication::primaryScreen()) label += tr(" · main");
    l.append(QVariantMap{{QStringLiteral("value"), screenKey(s)}, {QStringLiteral("label"), label}});
  }
  // A stored monitor that is not connected right now stays in the list, so the row still shows the choice.
  if (!screen_.isEmpty()) {
    bool found = false;
    for (QScreen* s : all) found = found || screenKey(s) == screen_;
    if (!found) l.append(QVariantMap{{QStringLiteral("value"), screen_}, {QStringLiteral("label"), tr("%1 · not connected").arg(screen_)}});
  }
  return l;
}

QScreen* VideoController::targetScreen() const {
  if (screen_.isEmpty()) return nullptr;
  for (QScreen* s : QGuiApplication::screens()) {
    if (screenKey(s) == screen_) return s;
  }
  return nullptr;
}

void VideoController::moveToFullscreenScreen(QWindow* window) {
  QScreen* target = targetScreen();
  if (!window || !target) return;
  if (!saved_) {
    savedScreen_ = window->screen();
    savedGeometry_ = window->geometry();
    saved_ = true;
  }
  if (window->screen() != target) window->setScreen(target);
  window->setPosition(target->geometry().topLeft());  // setScreen alone can keep the old coordinates on some platforms
}

void VideoController::restoreWindowedPosition(QWindow* window) {
  if (!saved_) return;
  saved_ = false;
  if (!window) return;
  if (savedScreen_ && window->screen() != savedScreen_) window->setScreen(savedScreen_);
  window->setGeometry(savedGeometry_);   // on a virtual desktop the coordinates are what moves the window back
}

void VideoController::applyStartupFormat() const {
  QSurfaceFormat f = QSurfaceFormat::defaultFormat();
  f.setSwapInterval(swapIntervalFor(vsync_));
  QSurfaceFormat::setDefaultFormat(f);
}

}  // namespace uzume::ui
