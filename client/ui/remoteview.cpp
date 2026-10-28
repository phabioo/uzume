#include "remoteview.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include "inputmapping.h"
#include "screenlayout.h"

namespace uzume::ui {

RemoteView::RemoteView(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAntialiasing(false);
  setAcceptedMouseButtons(Qt::LeftButton);
}

void RemoteView::setControl(ControlController* c) {
  if (control_ == c) return;
  if (control_) control_->disconnect(this);
  control_ = c;
  wasControlling_ = false;
  if (control_) {
    // A grant for this Session gives the tile the keyboard.
    connect(control_, &ControlController::changed, this, [this]() {
      const bool now = control_->controlling(surfaceId_);
      if (now && !wasControlling_ && !hasActiveFocus()) forceActiveFocus();  // only on the transition, not on every change
      wasControlling_ = now;
    });
  }
  emit controlChanged();
}

std::optional<QPointF> RemoteView::touchTarget(const QPointF& itemPos, bool clamp) const {
  if (!controller_ || width() <= 0 || height() <= 0) return std::nullopt;
  const QImage img = controller_->remoteFrame(surfaceId_);
  if (img.isNull()) return std::nullopt;
  emu::DisplayProfile prof = profileForRemoteFrame(img.size());
  if (prof.screens.size() != 2) return std::nullopt;  // one picture: no touch screen
  prof.screens[1].touch = true;                      // the bottom screen
  if ((layout_ != QLatin1String("stacked") || (swap_ && layoutSupportsSwap(layout_)))) {
    const ScreenPlacement pl = placeScreens(prof, layout_, size(), false, false, AspectMode::Core, swap_);
    return touchToFrameFor(prof, pl, itemPos, clamp);
  }
  const double s = (std::min)(width() / img.width(), height() / img.height());
  const QSizeF fs(img.width() * s, img.height() * s);
  const QRectF target(QPointF((width() - fs.width()) / 2, (height() - fs.height()) / 2), fs);
  return touchToFrame(prof, target, itemPos, clamp);
}

void RemoteView::keyPressEvent(QKeyEvent* e) {
  if (e->isAutoRepeat()) {
    if (control_ && control_->controlling(surfaceId_)) e->accept();
    else e->ignore();
    return;
  }
  if (control_ && control_->remoteKey(surfaceId_, e->key(), true, e->modifiers())) e->accept();
  else e->ignore();
}

void RemoteView::keyReleaseEvent(QKeyEvent* e) {
  if (e->isAutoRepeat()) {
    e->ignore();
    return;
  }
  if (control_ && control_->remoteKey(surfaceId_, e->key(), false, e->modifiers())) e->accept();
  else e->ignore();
}

void RemoteView::focusInEvent(QFocusEvent* e) {
  QQuickPaintedItem::focusInEvent(e);
  if (control_) control_->tileFocus(surfaceId_, true);
}

void RemoteView::focusOutEvent(QFocusEvent* e) {
  QQuickPaintedItem::focusOutEvent(e);
  touching_ = false;
  if (control_) control_->tileFocus(surfaceId_, false);
}

void RemoteView::mousePressEvent(QMouseEvent* e) {
  if (!control_ || !control_->controlling(surfaceId_)) {
    e->ignore();  // not playing: the tile's own click handling (selection) goes on
    return;
  }
  forceActiveFocus();
  touching_ = false;
  if (control_->canTouch(surfaceId_)) {
    if (const auto p = touchTarget(e->position(), false)) {
      touching_ = true;
      control_->remoteTouch(surfaceId_, p->x(), p->y(), true);
    }
  }
  e->setAccepted(true);
}

void RemoteView::mouseMoveEvent(QMouseEvent* e) {
  if (touching_ && control_) {
    if (const auto p = touchTarget(e->position(), true)) control_->remoteTouch(surfaceId_, p->x(), p->y(), true);
  }
  e->setAccepted(true);
}

void RemoteView::mouseReleaseEvent(QMouseEvent* e) {
  if (touching_ && control_) control_->remoteTouch(surfaceId_, 0, 0, false);
  touching_ = false;
  e->setAccepted(true);
}

void RemoteView::mouseUngrabEvent() {
  if (touching_ && control_) control_->remoteTouch(surfaceId_, 0, 0, false);
  touching_ = false;
}

void RemoteView::setController(SessionController* c) {
  if (controller_ == c) {
    return;
  }
  if (controller_) {
    controller_->disconnect(this);
  }
  controller_ = c;
  if (controller_) {
    connect(controller_, &SessionController::remoteFrameChanged, this, [this](const QString& id) {
      if (id == surfaceId_) {
        update();
      }
    });
  }
  update();
  emit controllerChanged();
}

void RemoteView::setSurfaceId(const QString& id) {
  if (surfaceId_ == id) {
    return;
  }
  surfaceId_ = id;
  update();
  emit surfaceIdChanged();
}

void RemoteView::setLayout(const QString& layout) {
  const QString next = isScreenLayout(layout) ? layout : QStringLiteral("stacked");
  if (next == layout_) {
    return;
  }
  layout_ = next;
  update();
  emit layoutChanged();
}

void RemoteView::setSwap(bool on) {
  if (on == swap_) {
    return;
  }
  swap_ = on;
  update();
  emit layoutChanged();
}

void RemoteView::paint(QPainter* p) {
  p->fillRect(boundingRect(), Qt::black);
  if (!controller_) {
    return;
  }
  const QImage img = controller_->remoteFrame(surfaceId_);
  if (img.isNull() || width() <= 0 || height() <= 0) {
    return;
  }
  const emu::DisplayProfile prof = profileForRemoteFrame(img.size());
  if ((layout_ != QLatin1String("stacked") || (swap_ && layoutSupportsSwap(layout_))) && prof.screens.size() == 2) {
    const ScreenPlacement pl = placeScreens(prof, layout_, size(), false, false, AspectMode::Core, swap_);
    p->setRenderHint(QPainter::SmoothPixmapTransform, false);
    for (int i = 0; i < pl.targets.size(); ++i) {
      if (!pl.targets.at(i).isEmpty()) {
        p->drawImage(pl.targets.at(i), img, screenSourceRect(prof, img.size(), i));
      }
    }
    return;
  }
  const double s = (std::min)(width() / img.width(), height() / img.height());
  const QSizeF size(img.width() * s, img.height() * s);
  const QRectF target(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
  p->setRenderHint(QPainter::SmoothPixmapTransform, false);
  p->drawImage(target, img);
}

}  // namespace uzume::ui
