#pragma once

#include <QString>

namespace uzume {

// Port a server address without a port gets (docs/guides/server-configuration.md; HubConnection uses it too).
inline constexpr int kDefaultHubPort = 8443;

// What a person typed or pasted into "Address or invite code" (docs/design/uzume/frame.md 8b-1, 8c-3) or what came in as
// an `uzume://` link from the command line, the single-instance socket or Ctrl+V.
struct JoinInput {
  enum class Kind {
    Empty,       // nothing typed
    InviteCode,  // UZ-XXXX-XXXX: inviteCode; the code names no server, the caller has to know one
    Link,        // uzume://join/<host[:port]>/<invite>#<fingerprint>: address, inviteCode, fingerprint
    Address,     // a server address: address
    BrokenLink,  // starts like a link but is incomplete or malformed (8c-3d): paste the invite code instead
    Invalid      // neither an address, an invite code nor a link
  };
  Kind kind = Kind::Empty;
  QString inviteCode;   // normalized "UZ-XXXX-XXXX" (InviteCode, Link)
  QString address;      // "host:port" (Link) or "[scheme://]host:port" as typed (Address); the default port is added
  QString fingerprint;  // normalized: 64 uppercase hex digits with colons (Link)
  // Stable error kind for the UI: "" (none) | "empty" | "broken_link" | "invalid_address" | "invalid_invite_code".
  QString error;

  bool ok() const { return kind == Kind::InviteCode || kind == Kind::Link || kind == Kind::Address; }
};

JoinInput classifyJoinInput(const QString& text);

// "UZ-XXXX-XXXX" for `UZ` + 8 Crockford characters with spaces, hyphens or none in any case; empty otherwise.
QString normalizeInviteCode(const QString& text);
// 64 hex digits with or without colons, any case -> uppercase with colons; empty if it is not one.
QString normalizeFingerprint(const QString& text);

// uzume://join/<address>/<invite>#<fingerprint> (the fingerprint as 64 uppercase hex digits without colons). `address` is
// host[:port] (a scheme is dropped). Empty if the address, the invite code or the fingerprint is invalid.
QString makeJoinLink(const QString& address, const QString& inviteCode, const QString& fingerprint);


// A link to a run's recap (0.22): uzume://run/<host[:port]>/<run id>/recap. It names no secret: only a person signed in to that
// server can open the run.
struct RunLink {
  bool ok = false;
  QString address;  // "host:port" (the default port is added)
  QString runId;    // lowercase uuid
};
RunLink parseRunLink(const QString& text);
// Empty if the address or the run id (a uuid) is invalid. `address` may carry a scheme, it is dropped.
QString makeRunLink(const QString& address, const QString& runId);

}  // namespace uzume
