#pragma once
// VoiceEngine (ADR 0033 D7): the media side of the voice chat. A mesh of libdatachannel PeerConnections, one per other device
// of the voice room, each with one sendrecv Opus audio track (mono, 24 kbit/s, 20 ms, in-band FEC). The device with the lower
// device id offers. Received audio goes through a small jitter buffer into one decoder per peer; the decoded 20 ms mono frames
// are handed out through pcmDecoded() (the VoiceController mixes them), together with a speaking flag per peer (decoded level,
// with hysteresis). Signaling is decoupled: signalOut() carries {kind, sdp | candidate, mid}, handleSignal() takes the same.
// Everything runs on the Qt thread of the engine; libdatachannel callbacks post into it.

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <cstdint>
#include <memory>

#include "sessiontypes.h"

namespace uzume {

class ThreadBridge;

class VoiceEngine : public QObject {
  Q_OBJECT
 public:
  struct Member {
    QString userId;
    QString deviceId;
  };
  struct PeerInfo {
    QString deviceId;
    QString userId;
    QString state;       // new | connecting | connected | failed | closed
    QString connection;  // "direct (host)" ... "relay (udp)", empty while unknown
    bool speaking = false;
    double levelDb = -120;
    qint64 framesReceived = 0;
    qint64 framesConcealed = 0;
  };

  static constexpr int kMaxPeers = 7;  // a room has at most 8 devices
  static constexpr int kPrefillFrames = 2;
  static constexpr int kMaxQueuedFrames = 8;
  static constexpr double kSpeakOnDb = -48.0;
  static constexpr double kSpeakOffDb = -54.0;
  static constexpr int kSpeakHoldMs = 300;

  explicit VoiceEngine(QObject* parent = nullptr);
  ~VoiceEngine() override;

  // The deterministic offerer of a pair: the lower device id.
  static bool isOfferer(const QString& selfDeviceId, const QString& peerDeviceId) { return selfDeviceId < peerDeviceId; }

  void setSelf(const QString& userId, const QString& deviceId);
  void setIceServers(const QStringList& stun, const QList<TurnServer>& turn);
  void setForceRelay(bool on) { forceRelay_ = on; }
  // The full member list of the room (including me); peers are created for new devices and closed for devices that are gone.
  void setMembers(const QList<Member>& members);
  void handleSignal(const QString& fromDeviceId, const QJsonObject& body);
  void close();  // closes every peer

  // One 20 ms capture frame (kVoiceFrame samples). `transmit` false only advances the clock; true encodes and sends it to every
  // peer whose track is open. Returns the number of tracks it went to.
  int pushFrame(const int16_t* pcm, bool transmit);

  QList<PeerInfo> peers() const;
  QStringList peerDevices() const { return peers_.keys(); }
  qint64 framesSent() const { return framesSent_; }
  void setTimersForTests(bool on) { decodeTimer_.setInterval(on ? 5 : 10); }

 signals:
  void signalOut(const QString& toDeviceId, const QJsonObject& body);
  void pcmDecoded(const QString& deviceId, const QByteArray& mono16);  // 20 ms, 960 samples
  void speakingChanged(const QString& deviceId, bool speaking);
  void peerStateChanged(const QString& deviceId, const QString& state);

 private:
  struct Peer;
  struct Pipe;
  Peer* ensurePeer(const QString& deviceId, bool wantOffer);
  void buildPeer(Peer& p);
  void teardown(Peer& p);
  void removePeer(const QString& deviceId);
  void onPcState(const QString& deviceId, unsigned gen, int state);
  void decodeTick();
  void decodeOne(Peer& p, const uint8_t* data, size_t size, bool fec, bool conceal);

  QString userId_, deviceId_;
  QStringList stun_;
  QList<TurnServer> turn_;
  bool forceRelay_ = false;
  QHash<QString, std::shared_ptr<Peer>> peers_;
  QTimer decodeTimer_;
  struct Encoder;
  std::unique_ptr<Encoder> enc_;
  qint64 frameCounter_ = 0;
  qint64 framesSent_ = 0;
  std::shared_ptr<ThreadBridge> bridge_;
  qint64 sendErrors_ = 0;
};

}  // namespace uzume
