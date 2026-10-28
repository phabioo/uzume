#pragma once
// RemoteView: draws the current frame of one watched Session (SessionController::remoteFrame(surfaceId)) scaled
// with the aspect ratio kept (nearest-neighbour).

#include <QPointer>
#include <QtQml/qqmlregistration.h>
#include <QtQuick/QQuickPaintedItem>

#include <optional>

#include "controlcontroller.h"
#include "sessioncontroller.h"

namespace uzume::ui {

class RemoteView : public QQuickPaintedItem {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(uzume::ui::SessionController* controller READ controller WRITE setController NOTIFY controllerChanged)
  Q_PROPERTY(QString surfaceId READ surfaceId WRITE setSurfaceId NOTIFY surfaceIdChanged)  // Session id
  // Controller hand-over (ADR 0033 D8): while the host has granted a port for this Session, keys, the pad and (P1) the mouse on the
  // touch screen go to the host while this tile has the focus.
  Q_PROPERTY(uzume::ui::ControlController* control READ control WRITE setControl NOTIFY controlChanged)
  // Screen layout (0.6 D12, 0.19) "stacked" | "side" | "large" | "top" | "bottom" and swap; applies to frames with two
  // stacked screens, others stay as they are.
  Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY layoutChanged)
  Q_PROPERTY(bool swap READ swap WRITE setSwap NOTIFY layoutChanged)
 public:
  explicit RemoteView(QQuickItem* parent = nullptr);
  SessionController* controller() const { return controller_; }
  void setController(SessionController* c);
  QString surfaceId() const { return surfaceId_; }
  void setSurfaceId(const QString& id);
  QString layout() const { return layout_; }
  void setLayout(const QString& layout);
  bool swap() const { return swap_; }
  void setSwap(bool on);
  void paint(QPainter* painter) override;
  ControlController* control() const { return control_; }
  void setControl(ControlController* c);
  // Item position -> normalized position in the whole remote frame for the touch (bottom) screen of a two-screen picture.
  std::optional<QPointF> touchTarget(const QPointF& itemPos, bool clamp) const;

 signals:
  void controllerChanged();
  void surfaceIdChanged();
  void layoutChanged();
  void controlChanged();

 protected:
  void keyPressEvent(QKeyEvent* e) override;
  void keyReleaseEvent(QKeyEvent* e) override;
  void focusInEvent(QFocusEvent* e) override;
  void focusOutEvent(QFocusEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseUngrabEvent() override;

 private:
  QPointer<ControlController> control_;
  bool wasControlling_ = false;  // the tile was the one I play in at the last change
  bool touching_ = false;
  QPointer<SessionController> controller_;
  QString surfaceId_;
  QString layout_ = QStringLiteral("stacked");
  bool swap_ = false;
};

}  // namespace uzume::ui
