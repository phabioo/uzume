#pragma once
// Optional Discord Rich Presence over the local Discord client's IPC (QLocalSocket, no SDK).
// Frames: little-endian uint32 opcode, uint32 length, JSON payload. Off by default; it never blocks the UI thread,
// fails silently when Discord is not running and sends only the game title, the system name and the start time.
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <optional>

class QLocalSocket;

namespace uzume {

struct DiscordActivity {
  QString details;       // game title
  QString state;         // system name, "· Session" or the run name appended
  qint64 startMs = 0;    // epoch ms the game started
  bool operator==(const DiscordActivity& o) const = default;
};

class DiscordPresence : public QObject {
  Q_OBJECT
 public:
  static constexpr const char* kApplicationId = "1564614458993545296";
  enum Opcode : quint32 { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };

  explicit DiscordPresence(QObject* parent = nullptr);
  ~DiscordPresence() override;

  // Socket names to try in order. Default: discord-ipc-0..9 under the runtime dirs (Linux/macOS) or the pipe names (Windows).
  static QStringList defaultSocketNames();
  void setSocketNames(const QStringList& names) { socketNames_ = names; }
  void setThrottleMs(int ms) { throttleMs_ = ms; }
  void setReconnectMs(int ms) { reconnectMs_ = ms; }

  // The switch. Turning it off clears the activity and closes the connection.
  void setEnabled(bool on);
  bool enabled() const { return enabled_; }
  // The latest wanted activity; no value = no game runs (clears). Coalesced, at most one update per throttle interval.
  void setActivity(const std::optional<DiscordActivity>& activity);

  bool connected() const { return ready_; }  // handshake answered with READY
  QString lastDetails() const { return lastDetails_; }  // of the last SET_ACTIVITY sent (empty after a clear)
  QString lastState() const { return lastState_; }
  QJsonObject stateJson() const;

  // Frame helpers (also used by the tests).
  static QByteArray encodeFrame(quint32 opcode, const QByteArray& json);

 signals:
  void connectedChanged();

 private:
  void wantConnectionChanged();
  void tryConnect();
  void tryNextName();
  void onConnected();
  void onReadyRead();
  void onDisconnected();
  void handleFrame(quint32 opcode, const QByteArray& payload);
  void flush();
  void sendActivity(const std::optional<DiscordActivity>& a);
  void sendFrame(quint32 opcode, const QByteArray& json);
  void closeSocket();
  void setReady(bool ready);
  bool wantsConnection() const { return enabled_ && desired_.has_value(); }

  QStringList socketNames_;
  int throttleMs_ = 15000;
  int reconnectMs_ = 30000;
  bool enabled_ = false;
  std::optional<DiscordActivity> desired_;
  std::optional<DiscordActivity> sent_;  // what Discord shows now
  bool dirty_ = false;
  bool ready_ = false;
  bool connecting_ = false;
  int nameIndex_ = 0;
  QLocalSocket* socket_ = nullptr;
  QByteArray buffer_;
  QElapsedTimer sinceSend_;
  QTimer flushTimer_;
  QTimer reconnectTimer_;
  QString lastDetails_, lastState_;
  bool loggedDown_ = false;  // one info line per state change
};

}  // namespace uzume
