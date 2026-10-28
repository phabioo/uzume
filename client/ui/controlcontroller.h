#pragma once
// ControlController: controller hand-over (ADR 0033 D8) for the QML UI.
//  - Host: shows the confirm card for a request ("max wants to play as P2", Allow / Deny, auto-deny after 20 s), applies the input of a
//    viewer that holds a port to the running game (GameSession::setRemoteInput, P1 take-over suspends the local P1 input), and takes the
//    controller back ("Take back", Esc twice). Ports come from the system manifest of the running game.
//  - Viewer: captures keyboard, gamepad and mouse (touch) for the watched tile that has the focus and feeds the fb-input channel of the
//    SessionViewer while the host has granted the port.
// The wire work is SessionHost / SessionViewer (media/), the arbitration ControlArbiter; this class is the glue and the UI state.

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>

#include "controlproto.h"
#include "inputmapping.h"

namespace uzume::ui {

class GameSession;
class SessionController;

class ControlController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the PlayerController")

  // Host: the open request (the card) and the seconds left to answer.
  Q_PROPERTY(bool askOpen READ askOpen NOTIFY changed)
  Q_PROPERTY(QString askText READ askText NOTIFY changed)       // "max wants to play as P2"
  Q_PROPERTY(int askSeconds READ askSeconds NOTIFY secondsChanged)
  // Host: somebody plays (the card turns into "max plays P1 · Take back").
  Q_PROPERTY(bool hasHolder READ hasHolder NOTIFY changed)
  Q_PROPERTY(QString holderText READ holderText NOTIFY changed)
  Q_PROPERTY(QVariantList holders READ holders NOTIFY changed)  // [{viewerId, name, port}]
  // Viewer: the Session id I play in ("" = none).
  Q_PROPERTY(QString playingSession READ playingSession NOTIFY changed)

 public:
  ControlController(SessionController* sessions, GameSession* game, QObject* parent = nullptr);

  // Ports of the running game's system (1 or 2); 0 = no game. Asked again whenever the game or the Session changes.
  void setPortsProvider(std::function<int()> provider);
  // Local input sources for the viewer side: the keyboard profile (Qt::Key -> libretro mask), the P1 gamepad mask, and the keys that
  // belong to the app (hotkeys, Esc) and are never sent.
  void setKeyboardMap(const QHash<int, quint32>& map);
  void setPadMask(quint32 mask);
  void setReservedKeys(const QSet<int>& keys) { reserved_ = keys; }

  bool askOpen() const { return !ask_.viewerId.isEmpty(); }
  QString askText() const;
  int askSeconds() const { return askSeconds_; }
  bool hasHolder() const;
  QString holderText() const;
  QVariantList holders() const;
  QString playingSession() const;

  // Host actions. Keys 1 / 2 on the card call allow() / deny().
  Q_INVOKABLE void allow() { answer(true); }
  Q_INVOKABLE void deny() { answer(false); }
  Q_INVOKABLE void takeBack();  // every holder
  // Esc while somebody plays P1: the first press says so, the second within 2 s takes P1 back. True = the key was used (a guest on
  // P2 does not take it: Esc pauses as usual).
  Q_INVOKABLE bool escapePressed();

  // Viewer capture for the tile of `sessionId`. remoteKey: true if the key was used (mapped); touch: normalized over the whole frame.
  Q_INVOKABLE bool controlling(const QString& sessionId) const;
  Q_INVOKABLE bool canTouch(const QString& sessionId) const;  // P1 is mine
  bool remoteKey(const QString& sessionId, int key, bool pressed, Qt::KeyboardModifiers mods);
  Q_INVOKABLE void remoteTouch(const QString& sessionId, double x, double y, bool pressed);
  Q_INVOKABLE void tileFocus(const QString& sessionId, bool focused);
  void releaseKeys(const QString& sessionId);

  // MCP / tests.
  bool request(const QString& sessionId, int port, QString* error);
  bool answerViewer(const QString& viewerId, bool allow);
  bool release(const QString& sessionId);
  // Holds `buttons` (libretro mask, 16 bits) on the fb-input channel of the session I play in; 0 releases.
  bool remoteInput(const QString& sessionId, quint32 buttons);
  QVariantMap state() const;  // {ports, holders: [..], pending: [..], watching: [{session_id, state, port, host_ports, reason}]}

 signals:
  void changed();
  void secondsChanged();
  void notice(const QString& text);  // toast

 private:
  struct Ask {
    QString viewerId;
    int port = 0;
    qint64 deadlineMs = 0;
  };
  void answer(bool allow);
  void onHostControl();
  void refreshPorts();
  void syncGame();
  void tick();
  void pushInput(const QString& sessionId);
  void onViewerControl();

  SessionController* sessions_;
  GameSession* game_;
  std::function<int()> ports_;
  Ask ask_;
  int askSeconds_ = 0;
  QTimer ticker_;
  QElapsedTimer escClock_;
  QElapsedTimer askSwitched_;  // valid while the card switched from one request to another
  QString lastGameTitle_;
  bool escArmed_ = false;
  // Viewer
  KeyboardJoypad keys_;
  quint32 pad_ = 0;
  quint32 debug_ = 0;
  QSet<int> reserved_;
  QString focus_;
  bool touching_ = false;
  double touchX_ = 0, touchY_ = 0;
  QHash<QString, int> lastState_;
};

}  // namespace uzume::ui
