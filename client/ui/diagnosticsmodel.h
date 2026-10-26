#pragma once
// DiagnosticsModel: what the diagnostics overlay (3t-3y) shows, as plain values for QML, plus the three persisted
// open/closed states (0.6 D5: overlay, Emulation section, Streaming section; shared by window, multiview and fullscreen).
// The model formats; the SessionController feeds it from the emulation runner, the audio output, the SessionHost and the
// viewers (about twice a second while the overlay is open, nothing is measured or computed on the UI thread otherwise).

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "gamesession.h"
#include "mediastats.h"
#include "playersettings.h"
#include "sessiontypes.h"

namespace uzume::ui {

class DiagnosticsModel : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("Provided by the SessionController")

  // Persisted (settings/player.json): overlay open (default false), Emulation section open (true), Streaming section (true).
  Q_PROPERTY(bool open READ isOpen WRITE setOpen NOTIFY statesChanged)
  Q_PROPERTY(bool emulationOpen READ emulationOpen WRITE setEmulationOpen NOTIFY statesChanged)
  Q_PROPERTY(bool streamingOpen READ streamingOpen WRITE setStreamingOpen NOTIFY statesChanged)

  // Emulation of the local game: {valid, core, renderer, rendererSub, fallback, fallbackText, resolution, resolutionSub,
  // fps, frame, audio, audioSub, summary, spark{total[], emu[], targetMs}, lines[] (multiview tile text)}; {} without a game.
  Q_PROPERTY(QVariantMap emulation READ emulation NOTIFY dataChanged)
  // Multiview columns: [{surface, title, sub, local, emulation, note}] in tile order.
  Q_PROPERTY(QVariantList tiles READ tiles NOTIFY dataChanged)
  // Streaming per Session: [{surface, title, details:[[label, value]], participants:[{name, role, pill, tone, line2, line3,
  // path, rtt, loss, reason}]}]; empty without a Session.
  Q_PROPERTY(QVariantList streaming READ streaming NOTIFY dataChanged)
  Q_PROPERTY(QString emulationSummary READ emulationSummary NOTIFY dataChanged)
  Q_PROPERTY(QString tilesSummary READ tilesSummary NOTIFY dataChanged)
  Q_PROPERTY(QString streamingSummary READ streamingSummary NOTIFY dataChanged)
  Q_PROPERTY(int participantCount READ participantCount NOTIFY dataChanged)

 public:
  explicit DiagnosticsModel(QObject* parent = nullptr) : QObject(parent) {}

  // The shared settings object of the Player (not owned). Without one the states live in memory only.
  void setSettings(PlayerSettings* settings);

  bool isOpen() const { return open_; }
  bool emulationOpen() const { return emulationOpen_; }
  bool streamingOpen() const { return streamingOpen_; }
  void setOpen(bool on);
  void setEmulationOpen(bool on);
  void setStreamingOpen(bool on);
  Q_INVOKABLE void toggle() { setOpen(!open_); }
  Q_INVOKABLE void toggleEmulation() { setEmulationOpen(!emulationOpen_); }
  Q_INVOKABLE void toggleStreaming() { setStreamingOpen(!streamingOpen_); }

  QVariantMap emulation() const { return emulation_; }
  QVariantList tiles() const { return tiles_; }
  QVariantList streaming() const { return streaming_; }
  QString emulationSummary() const { return emulationSummary_; }
  QString tilesSummary() const { return tilesSummary_; }
  QString streamingSummary() const { return streamingSummary_; }
  int participantCount() const { return participants_; }

  // Feeding (SessionController, tests, screenshots).
  void setEmulation(const EmulationDiagnostics& d);
  void setTiles(const QVariantList& tiles);
  void setStreaming(const QVariantList& groups);

  // ---- pure formatting, public for tests ----
  static QVariantMap emulationMap(const EmulationDiagnostics& d);
  struct Pill {
    QString text;
    QString tone;  // "ok" | "warn" | "neutral"
  };
  // D9: "" -> Connecting (neutral), "direct (...)" -> Direct (ok), "relay (udp)" -> Relayed (TURN) (warn),
  // "relay (tcp)" -> Relayed (TURN/TCP) (warn).
  static Pill connectionPill(const QString& connectionType);
  static bool isRelayed(const QString& connectionType) { return connectionType.startsWith(QLatin1String("relay")); }
  static QString mbit(double kbps);  // "5.8" -> used as "5.8 Mbit/s" by the callers
  // Host line 2: "Encoder h264_nvenc · H.264 · 6.0 / target 6.5 Mbit/s · 60.0 fps" (D8, D11).
  static QString hostLine(const SessionStats& s);
  // Host line 3: "Sending to 1 viewer · adaptive bitrate".
  static QString hostSendingLine(int viewers);
  // Viewer line 2: "Decoder h264 · H.264 · 3.9 Mbit/s · 59.7 fps"; missing values show "—" (older Players).
  static QString decoderLine(const QString& decoder, std::optional<double> kbps, std::optional<double> fps);
  // Line 3 of a viewer or remote host: "RTT 48 ms · Loss 0.4 % · via hub.example.com:3478" (via only when relayed).
  // A relayed link appends the reason ("... · via hub.example.com:3478 · no STUN reachable on this side").
  static QString linkLine(std::optional<double> rttMs, std::optional<double> lossPercent, const QString& connectionType,
                          const QString& turnEndpoint, RelayReason reason = RelayReason::None);
  // Short reason for a relayed connection, empty for RelayReason::None.
  static QString relayReasonText(RelayReason reason);
  // "host:port" of the first TURN URL ("turn:hub.example.com:3478?transport=udp"), empty if there is none.
  static QString turnEndpoint(const QList<TurnServer>& servers);
  // `link` carries the structured link values of the Details table (see linkFields); merged into the map.
  static QVariantMap participant(const QString& name, const QString& role, const Pill& pill, const QString& line2, const QString& line3,
                                 const QVariantMap& link = {});
  // Details table of one person: {path: "Direct" | "Relay" | "" (not known yet), rtt: "18 ms" | "", loss: "0.1 %" | "",
  // reason: "Relay because ..." | ""}. Only values that exist are non-empty.
  static QVariantMap linkFields(std::optional<double> rttMs, std::optional<double> lossPercent, const QString& connectionType,
                                RelayReason reason = RelayReason::None);
  // One sentence for a relayed path ("Relay because no direct path was found: strict NAT or a firewall."), empty for None.
  static QString relayReasonSentence(RelayReason reason);
  // Details rows of a Session group, [[label, value]], only for values that exist: host "Encoder", "Bitrate", "Resolution";
  // viewer "Decoder", "Bitrate", "Resolution".
  static QVariantList hostDetailRows(const SessionStats& s);
  static QVariantList viewerDetailRows(const SessionStats& s);

 signals:
  void statesChanged();
  void dataChanged();

 private:
  void persist();

  PlayerSettings* settings_ = nullptr;
  bool open_ = false;
  bool emulationOpen_ = true;
  bool streamingOpen_ = true;
  QVariantMap emulation_;
  QVariantList tiles_;
  QVariantList streaming_;
  QString emulationSummary_;
  QString tilesSummary_;
  QString streamingSummary_ = QStringLiteral("No session");
  int participants_ = 0;
};

}  // namespace uzume::ui
