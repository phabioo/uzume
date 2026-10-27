#pragma once

#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <optional>

namespace uzume {

// Session data model and WSS messages (ADR 0006 D1-D3, protocol/openapi/uzume.yaml tag `sessions`,
// protocol/schemas/ws-*.schema.json). Plain data, no I/O.

inline constexpr const char* kSessionsFeature = "sessions_v1";

struct SessionOwner {
  QString userId;
  QString displayName;
  QString deviceName;
};

struct SessionViewerEntry {
  QString viewerId;
  QString displayName;
  QString deviceName;
};

struct SessionInviteEntry {
  QString userId;
  QString displayName;
  QString state;  // invited | declined | joined
  bool online = false;
};

struct SessionInfo {
  QString sessionId;
  QString gameId;
  QString gameTitle;
  SessionOwner owner;
  QString visibility;  // private | hub_users | invite_only
  QString createdAt;
  int viewerCount = 0;
  QList<SessionViewerEntry> viewers;  // owner device only
  QList<SessionInviteEntry> invites;  // owner device only
  bool isOwner = false;
  bool invited = false;
};
std::optional<SessionInfo> parseSession(const QJsonObject& o);

struct SessionPermissions {
  bool viewVideo = true;
  bool hearAudio = true;
  bool sendInput = false;
};

// Short-lived relay credentials of the Hub's embedded TURN server (`turn_servers`, protocol turn_v1).
struct TurnServer {
  QStringList urls;  // turn:host:port?transport=udp|tcp
  QString username;
  QString credential;
  QString expiresAt;  // RFC 3339, informational
  bool operator==(const TurnServer&) const = default;
};
// Parses a `turn_servers` array; entries without urls, username or credential are dropped.
QList<TurnServer> parseTurnServers(const QJsonValue& v);

struct SessionJoinInfo {
  QString viewerId;
  SessionPermissions permissions;
  QStringList iceServers;  // stun: URLs
  QList<TurnServer> turnServers;
};
std::optional<SessionJoinInfo> parseJoinInfo(const QJsonObject& o);

struct HubUser {
  QString id;
  QString displayName;
  bool online = false;
};
std::optional<HubUser> parseHubUser(const QJsonObject& o);

// signal {session_id, viewer_id, kind: offer|answer|candidate, sdp?, candidate?, mid?}
struct SessionSignal {
  QString sessionId;
  QString viewerId;
  QString kind;
  QString sdp;
  QString candidate;
  QString mid;
  QJsonObject toPayload() const;
};
std::optional<SessionSignal> parseSignal(const QJsonObject& payload);

struct HelloAck {
  int protocolVersion = 0;
  QString hubVersion;
  QStringList features;
  QStringList iceServers;
  QList<TurnServer> turnServers;
};

struct ViewerJoined {
  QString sessionId;
  QString viewerId;
  QString displayName;
  QString deviceName;
  QList<TurnServer> turnServers;  // fresh relay credentials for the owner device (optional)
};

struct ViewerLeft {
  QString sessionId;
  QString viewerId;
  QString reason;  // left | removed | revoked | disconnected
};

struct SessionEnded {
  QString sessionId;
  QString reason;  // ended | replaced | owner_disconnected | device_revoked | hub_restarted | no_longer_visible
};

struct PresenceUpdate {
  QString userId;
  QString deviceId;
  QString state;  // online | offline | in_game
  QString gameId;
  // together_v1 (Uzume 0.23): per-receiver fields; empty when the server does not send them or the receiver may not see them.
  QString displayName;
  QString gameTitle;
  QString system;
  QString sessionId;  // the device's shared Session, only when the receiver may see it
};

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::SessionInfo)
Q_DECLARE_METATYPE(uzume::SessionSignal)
Q_DECLARE_METATYPE(uzume::HelloAck)
Q_DECLARE_METATYPE(uzume::ViewerJoined)
Q_DECLARE_METATYPE(uzume::ViewerLeft)
Q_DECLARE_METATYPE(uzume::SessionEnded)
Q_DECLARE_METATYPE(uzume::PresenceUpdate)
