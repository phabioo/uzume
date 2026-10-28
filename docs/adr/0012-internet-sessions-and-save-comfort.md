# ADR 0012: Sessions over the internet and save comfort (0.4)

- Status: accepted; the Player-side core index check (D6) is superseded by [ADR 0020](0020-cores-from-the-libretro-buildbot.md) (0.8, implemented; the `cores_index_v1` endpoints are removed)
- Date: 2026-10-07
- Decided by: Fabio (proposal by the orchestrator, accepted on 2026-10-07 after the merge of PR #31)

## Context

Roadmap 0.4 (`docs/roadmap.md`) makes Sessions work beyond the LAN and adds save comfort. Until now the Hub signals over its HTTPS/WSS port and media flows Player to Player over WebRTC with host candidates and optional `stun:` URLs (ADR 0006 D4). Players behind two different home routers often reach each other with STUN alone, but not always (symmetric NAT, mobile networks, strict firewalls), so a relay is needed. A typical Hub runs on a small home server (for example a Raspberry Pi) behind a DSL router with a public dynamic IPv4 and IPv6, reachable through a DynDNS name.

The save model (`docs/architecture/03-saves.md`) left retention, restore, manual snapshots, live updates and slot selection open. The Player-side signature check of core packages moved here from 0.3 (ADR 0010 D5, ADR 0011 D7).

## Decisions

### D1 Supported setups

| Setup | What it needs | Media path |
|---|---|---|
| LAN | nothing | host candidates |
| Internet with port forwarding (recommended) | public IPv4 at the Hub's router (no CGNAT/DS-Lite), a DNS name for it (DynDNS), forwards for the Hub port (TCP), TURN port 3478 (UDP and TCP) and the relay range (UDP), embedded TURN switched on | direct (STUN) when possible, else relayed by the Hub |
| VPN (WireGuard, Tailscale, ...) | Players pair with the Hub's VPN address | host candidates over the VPN |

An external TURN server (for example coturn) is not supported in 0.4; `-ice-servers` keeps accepting `stun:` URLs only. (Superseded for relays by the amendment of 2026-10-12 below.)

### D2 Embedded STUN/TURN server

- The Hub binary embeds a STUN/TURN server based on `github.com/pion/turn/v4` (pure Go). It is off by default.
- Configuration (flag / environment, like the other Hub options; no web form in 0.4):
  - `-turn` / `FRAMEBEAM_TURN=1`: switch on.
  - `-public-host` / `FRAMEBEAM_PUBLIC_HOST`: DNS name (or IPv4) of the Hub's public address, required with `-turn`.
  - `-turn-port` / `FRAMEBEAM_TURN_PORT`: default `3478`, UDP and TCP listeners on all addresses.
  - `-turn-relay-ports` / `FRAMEBEAM_TURN_RELAY_PORTS`: UDP relay range, default `49160-49199` (40 allocations; one Session with 4 viewers needs at most 8).
  - `-turn-relay-ip` / `FRAMEBEAM_TURN_RELAY_IP`: optional fixed public IPv4; otherwise the Hub resolves the A record of `-public-host` at start and every 5 minutes (DynDNS address changes) and uses the newest result as relayed address. Without a resolved IPv4 the TURN server answers allocations with an error and logs a warning once per change.
- Relay is IPv4 only in 0.4. Direct IPv6 paths still work when both Players have IPv6 and their firewalls allow it; FrameBeam does not configure router firewalls.
- Peer filter: the relay refuses peers with loopback, unspecified, multicast or link-local addresses, unless the relay address itself is loopback (tests). Private LAN addresses are allowed on purpose: a remote Player relayed by the Hub reaches a Player in the Hub's LAN this way without NAT loopback at the router.
  - Amended 2026-10-09: the relay now also refuses internal addresses (RFC 1918, CGNAT 100.64.0.0/10, 0.0.0.0/8, 192.0.0.0/24, 198.18.0.0/15, 240.0.0.0/4 including broadcast, IPv6 ULA fc00::/7, also IPv4-mapped). Exception: an internal address is allowed when it is the remote IP of a Player currently connected to the Hub over WSS, which keeps the use case above. Reason: any paired device could otherwise send UDP through the relay to any host in the Hub's LAN (router DNS, SSDP, NAS, the Hub machine). Also: at most 10 concurrent allocations per device (TURN 486 beyond), at most 64 concurrent TURN TCP connections, and credentials whose expiry lies more than the 12 h TTL (+1 min) ahead are refused. TCP allocations stay unsupported.
- Realm `framebeam`. The Hub's Settings page shows TURN state (off/on, public host, resolved IPv4 and when, ports, active allocations) and the list of required port forwards.

### D3 TURN credentials

- Short-lived credentials in the TURN REST API scheme: username `<expiry-unix-seconds>:<device_id>`, password `base64(HMAC-SHA1(secret, username))`. Lifetime 12 hours.
- The secret is 32 random bytes generated on first start with TURN on and stored in the SQLite `settings` table. It is never logged, shown or written to a file.
- The auth handler rejects expired usernames and devices that are unknown or revoked.
- Credentials are delivered in `hello_ack` and in the Session join response, so a Player gets fresh ones on every WSS (re)connect and every join. They are also in `viewer_joined` for the owner, so each viewer PeerConnection of the host uses fresh credentials; a single relayed viewer connection longer than 12 h still drops and needs a rejoin.
- TURN URLs use the host name the Player used to reach the Hub (the `Host` of the WSS upgrade or the join request, without port), falling back to `-public-host`. Reason: a Player in the Hub's LAN reaches TURN via the LAN address without NAT loopback; a remote Player uses the public name it paired with. The relayed address is always the public IPv4 (D2).

### D4 Protocol (additive, `protocol_version` stays 1)

- `hello_ack` and `SessionJoinResponse` gain optional `turn_servers: [{urls: [string], username, credential, expires_at}]`. With TURN on, the Hub returns one entry with `turn:<host>:<port>?transport=udp` and `turn:<host>:<port>?transport=tcp`, and adds `stun:<host>:<port>` to `ice_servers` (0.3 Players benefit from STUN without understanding `turn_servers`). `viewer_joined` gains the same optional `turn_servers` (fresh, for the owner device). Handshake feature `turn_v1` while TURN is on.
- Save comfort endpoints and the `save_updated` WSS message (D7) are advertised by feature `saves_v2`; the core index endpoints (D6) by `cores_index_v1`.
- OpenAPI version 1.5.0.

### D5 Player media: TURN, connection type, bitrate adaptation

- The Player passes `ice_servers` and `turn_servers` to libdatachannel. Setting `FRAMEBEAM_FORCE_RELAY=1` (CLI `--force-relay`) sets the ICE transport policy to relay only, for testing the relay on a LAN.
- Diagnostics show the connection type of the selected candidate pair per PeerConnection: `direct (host)`, `direct (srflx)`, `direct (prflx)` or `relay (udp|tcp)`, on the viewer's surface and per viewer on the host.
- Bitrate adaptation: the viewer reports `{"t":"rx","loss":<0..1>,"kbps":<received video kbit/s>}` every second over the `fb-diag` DataChannel. The host adapts the single Session encoder to the worst viewer with AIMD: start 2000 kbit/s; loss above 5 % multiplies by 0.7 (minimum 300 kbit/s); three consecutive reports below 1 % add 10 % (maximum 4000 kbit/s); at most one change per 2 seconds. If the encoder cannot change the bitrate at runtime it is reopened with a keyframe, at most every 5 seconds. Diagnostics show the target bitrate. Opus stays at 96 kbit/s.

### D6 Player-side signature check of core packages

- The Hub keeps the raw bytes and signature of the last verified core index (sync or import) in its data directory and serves them as `GET /api/v1/cores/index` and `GET /api/v1/cores/index.sig` (Bearer auth, feature `cores_index_v1`).
- Before installing a downloaded core package, the Player fetches both, verifies the Ed25519 signature against its compiled-in keys (plus `FRAMEBEAM_PLAYER_TRUST_KEYS`, the same keys as the updater) and requires the package entry (core id, version, platform) to exist with matching file names, sizes and SHA-256. Otherwise the core is shown as untrusted and not loaded.
- A Hub without `cores_index_v1` (0.3 and older) keeps the 0.3 behavior (size and SHA-256 from the Hub) with a log warning.

### D7 Save comfort

- **Retention:** per slot the Hub keeps the newest 20 history versions, plus the newest version of each day for the last 30 days and of each week for the last 26 weeks. Never thinned: `manual_snapshot` versions and versions referenced by an unresolved conflict. Thinning runs after each history insert for that slot and as a daily sweep; content files are deleted once no checkpoint or version references them. Flags `-save-keep-recent`, `-save-keep-daily`, `-save-keep-weekly` (0 = unlimited for that rule).
- **Restore:** `POST /api/v1/games/{game_id}/saves/{slot}/history/{version}/restore` with `{expected_revision}`. The Hub first secures the current checkpoint into history (reason `before_restore`, unless already captured), then makes the version's content the new checkpoint (revision + 1, reason `restore`, device = caller). A different `expected_revision` returns 409 `save_conflict_stale` (as for conflict resolution). The checkpoint reason enum gains `restore`, the history reason enum `before_restore`. Available in the Player's save history (not while that game runs on this Player, and not with a pending upload for the slot) and on the Hub's Saves page.
- **Manual snapshot:** `POST /api/v1/games/{game_id}/saves/{slot}/snapshots` with optional `{label}` (up to 64 characters) creates a history version with reason `manual_snapshot` from the current checkpoint (404 when there is none). History versions gain an optional `label`. In game, the Player first uploads a changed save, then creates the snapshot.
- **Push:** when a slot's checkpoint changes (upload, restore, conflict resolution), the Hub sends `save_updated {game_id, slot, revision, sha256, device_id, device_name, reason}` over WSS to the other connected devices of the same user. The Player refreshes the sync state; if that game is running there, it shows that the save changed on another device and that the next upload will become a conflict (existing conflict logic).
- **Slots:** the Player lets the user pick the slot per game in the detail pane (existing slots from `GET /saves` plus `default`, "New slot" with the Hub's slot name rule); the choice is stored per game and Hub profile, and start sync, checkpoints and final sync use it.

### D8 Multiview with several remote Sessions and audio focus

- Up to 4 surfaces: the local game plus up to 3 remote Sessions, or up to 4 remote Sessions without a local game. Each remote surface is its own viewer join.
- Layouts: side-by-side (2 surfaces), grid 2 x 2 (3 or 4), picture-in-picture (one main surface, others as small tiles stacked bottom right). "Swap" makes a surface the main one, "Remove" leaves that Session.
- Audio focus: exactly one surface is audible; "Audio here" moves the focus. Default is the local game; when the focused surface is removed, focus goes to the local game, else the first remaining surface.

## Verification in the cloud

- Hub: Go tests for credentials (expiry, revoked device, HMAC), the peer filter, `turn_servers` in `hello_ack`/join with the request host, retention rules, restore/snapshot/stale cases, `save_updated` delivery, and the core index endpoints.
- Player: unit tests for the AIMD controller, index verification (good, bad signature, mismatched hash, missing entry), slot selection and the `save_updated` handling; `scripts/e2e-session.sh` additionally runs a Session with `--force-relay` against a Hub with TURN on loopback and checks that frames arrive and that the connection type is `relay`.
- Locally by Fabio: two Players in different networks through the Pi, direct and forced relay, more than one remote Session, audio focus.

## Consequences

- New Go dependency `pion/turn/v4`; the Hub opens UDP/TCP 3478 and a UDP range when TURN is on.
- Relayed Sessions use the Hub's uplink: about 2 Mbit/s per relayed viewer (both directions through the Pi).
- Fabio has to configure router port forwards and DynDNS himself (documented in [sessions-over-the-internet.md](https://github.com/phabioo/uzume/wiki/Sessions-over-the-internet); update 2026-10-08: this was moved there from `packaging/linux/README.md` and the root README).

## Implementation notes (0.4 PR)

- **Hub TURN:** the secret lives in settings key `turn_secret`. The pion `AuthHandler` has no context, so the device lookup uses a 3 s background context (one DB read per TURN auth). `turn_v1` is in the REST handshake features only; `saves_v2` is also in `hello_ack`. The web Settings page shows TURN status and the router forwards.
- **Hub saves:** migration 0006 rebuilds `save_slots` and `save_history` (CHECK constraints) and adds `label`. Web-originated changes record device `web:<user id>`; they appear in the API and in pushes as nil UUID `00000000-0000-0000-0000-000000000000` with name "Hub web interface". `save_updated` reason `conflict_resolution` is used for `use_local` resolutions. Pushes are sent only when the checkpoint actually changes. Retention counts protected versions toward "recent". A resolved conflict whose secured upload is thinned away is deleted (only unresolved conflicts protect versions). Restoring identical content still creates a revision. `-save-keep-recent 0` disables thinning.
- **Core index:** served from `<data>/cores/index.json` and `index.json.sig`, written after verification and a successful apply on sync/import, and served byte-exact. The Player verifies on every `prepare()` (also with all files cached); a 404 or a failure marks the core "untrusted".
- **Player saves:** slot `default` keeps the existing directory; other slots live in `<game dir>/slots/<slot>/`. The slot choice is stored per Hub profile and game in `player.json` (`save_slots`). Restore is blocked while the game runs, with a pending or conflict state, or when the local file differs from the last synced hash; the replaced local file is kept as `.restore-*.bak`. The CLI always uses `default`.
- **Player media:** ICE/TURN lists in the join response win; `hello_ack` is the fallback when they are empty. libjuice reports relayed candidates as UDP, so "relay (tcp)" is shown only when the transport says so or only TCP TURN URLs were offered. libdatachannel takes at most 2 TURN servers. libx264 changes the bitrate at runtime; other encoders are reopened with a keyframe (at most every 5 s). The CLI prints `CONNECTION <type>`; `STATS` link values use underscores.

## Amendment 2026-10-12: provider-neutral relay

D1 ("no external TURN server") no longer holds. The relay is provider-neutral: besides the embedded TURN server (`-turn`) and Cloudflare TURN, the admin can add any number of standard external TURN servers (`-turn-external`, [server-configuration.md](https://github.com/phabioo/uzume/wiki/Server-configuration)): an own coturn on a VPS ([coturn-on-a-vps.md](https://github.com/phabioo/uzume/wiki/coturn-on-a-VPS)) or a third-party provider.

- Per server one of two credential modes: shared secret (TURN REST API, as the embedded TURN: username `<expiry>:<device-id>`, password `base64(HMAC-SHA1(secret, username))`, 12 h, renewed by `ice_update` every 6 h) or a static username and password.
- All active providers go into `turn_servers`; `turn_v1` means any relay provider is on. The Player needs no change: it already offers every `turn_servers` entry to ICE and ignores `expires_at`. OpenAPI 1.13.0 makes `TurnServer.expires_at` optional (absent for static credentials).
- Secrets and passwords of flag-configured servers come from the environment or files only; none is ever logged or shown (Settings, diagnostics `network.external_turn`).
- Later the same day: the list is editable in Settings → Network (add, edit, delete by an admin). The 0.6 rule applies: a list saved in the web interface wins over `-turn-external`, which is the initial value (the first edit copies it without secrets; flag secrets keep working). Secrets entered in the web interface are stored in the Hub database settings like the embedded TURN secret and are write-only (never rendered, logged, in diagnostics or API). Changes apply without a restart (`hello_ack`, join, `ice_update`). This supersedes "environment or files only, never stored" for servers created in the web interface.
- `-ice-servers` still accepts `stun:` URLs only. D2 to D7 (embedded TURN, per-device credentials, limits, save retention) are unchanged.
- Update 2026-10-19 (D2, reverse proxy): behind a reverse proxy the TCP peer of a WSS connection is the proxy. With `-trusted-proxy` the Hub takes the Player's remote IP (`hub.Service.ConnectedPlayerIP`, TURN LAN-peer check) from the forwarded client address (ADR 0015 update of the same date); without it the proxy's address is used and the LAN check sees the proxy. The embedded TURN server itself is not proxied (UDP/TCP on `-turn-port`).
