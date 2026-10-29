#pragma once
// VideoController (Settings > This PC > Audio & video > Video, board 6g-1): the monitor F11 opens the game on and vertical sync.
// The state lives in PlayerSettings. The monitor is applied when the window goes fullscreen (moveToFullscreenScreen, called by
// the game view and "Fullscreen on start"); vsync is the swap interval of the default surface format, read once at the start
// (applyStartupFormat), so a change takes effect after a restart.

#include <QObject>
#include <QScreen>
#include <QWindow>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>


namespace uzume {
class PlayerSettings;
}

namespace uzume::ui {

class VideoController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  Q_PROPERTY(QString fullscreenScreen READ fullscreenScreen WRITE setFullscreenScreen NOTIFY changed)  // QScreen::name(), "" = the window's monitor
  Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged)                                  // [{value, label}], "current monitor" first
  Q_PROPERTY(bool vsync READ vsync WRITE setVsync NOTIFY changed)

 public:
  explicit VideoController(PlayerSettings* settings, QObject* parent = nullptr);

  QString fullscreenScreen() const { return screen_; }
  void setFullscreenScreen(const QString& name);
  QVariantList screens() const;
  bool vsync() const { return vsync_; }
  void setVsync(bool on);

  // The chosen monitor if it is connected, else nullptr (the window stays where it is).
  QScreen* targetScreen() const;
  // Moves `window` to the chosen monitor before it goes fullscreen; no-op without a choice or when the monitor is gone.
  Q_INVOKABLE void moveToFullscreenScreen(QWindow* window) const;

  // Sets the default surface format's swap interval from the stored choice; call before the first window is created.
  void applyStartupFormat() const;
  static int swapIntervalFor(bool vsync) { return vsync ? 1 : 0; }

 signals:
  void changed();
  void screensChanged();

 private:
  PlayerSettings* settings_;
  QString screen_;
  bool vsync_ = true;
};

}  // namespace uzume::ui
