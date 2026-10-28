# Sessions: LAN and internet

At a glance: How to share a running game as a Session: first on a LAN, then over the internet with the embedded TURN relay, Cloudflare TURN or your own coturn ([coturn-on-a-vps.md](coturn-on-a-VPS)); what to do when a Session does not connect. Network settings are in the app (Server › Settings › Network & sharing).

A Session shares a running game from one app with other apps. Audio and video flow directly between the apps (WebRTC, H.264 + Opus); the server only handles presence, signaling and, optionally, a relay. Decisions: [ADR 0006](https://github.com/phabioo/uzume/blob/main/docs/adr/0006-sessions-phase4.md), [ADR 0012](https://github.com/phabioo/uzume/blob/main/docs/adr/0012-internet-sessions-and-save-comfort.md).

## Test Sessions on a LAN

Two apps on the LAN, both paired to the same server. With only the admin, both devices belong to the admin: Private (own devices only), server users and Invite only can be tested. With a second server user (create an invite, see [walkthrough.md](Getting-started)) Private rejects the foreign user and server users lets them in. STUN (`-ice-servers`) is not needed on a LAN. `scripts/e2e-session.sh` runs the same flow headless with two CLI processes against a local server; it also runs a second round with forced relay against a server with TURN on loopback.

## Sessions over the internet

Apps on the same LAN need nothing (see above). For apps behind other routers the server embeds a STUN/TURN relay (off by default; ADR 0012). Media goes directly between apps when possible and through the server otherwise.

Requirements:

- A public IPv4 address at the server's router. DS-Lite and CGNAT (shared addresses) do not work; ask your provider or compare the router's WAN address with what a "what is my IP" site shows.
- A DNS name for that address, for example a DynDNS name such as `hub.example.org`. The server resolves its A record at start and every 5 minutes, so address changes are followed.
- Router port forwards to the server host:
  - TCP `<hub port>` (default 8443; the server's HTTPS/WSS port)
  - UDP and TCP `3478` (STUN/TURN)
  - UDP `49160-49199` (relay range, 40 allocations)

Switch it on in the app (Server › Settings › Network & sharing, then restart the server), or in `/etc/uzume/server.env` and restart the service:

```sh
UZUME_TURN=1
UZUME_PUBLIC_HOST=hub.example.org
# optional: UZUME_TURN_PORT=3478, UZUME_TURN_RELAY_PORTS=49160-49199, UZUME_TURN_RELAY_IP=<fixed public IPv4>
```

Values saved in Server › Settings › Network & sharing override `server.env` and the flags (reset per field to return to the `server.env` value); `server.env` only gives the initial values. This also applies to the port and the save retention values. The web form refuses ports below 1024; set those with `install-server.sh --port` (note that a port saved in the web form then still wins until reset). The Network page shows the state (on/off, resolved IPv4, ports, active allocations) and the forwards needed. Apps pair with the DNS name; credentials are issued automatically and expire after 12 hours. Relay is IPv4 only; direct IPv6 paths still work when both apps allow them. The server does not configure the router.
## Supported setups

LAN, internet with a router port forward (recommended, this page), or a VPN such as WireGuard or Tailscale. The relay behind it can be the server's embedded TURN (this page), Cloudflare TURN or an external TURN server such as your own coturn on a VPS ([coturn-on-a-vps.md](coturn-on-a-VPS), [server-configuration.md](Server-configuration)); several can be active at once. An external relay needs no router port forward on the server side (ADR 0012, amendment 2026-10-12).

## Testing the relay

Set `UZUME_FORCE_RELAY=1` (CLI `--force-relay`) on an app to force the relay path. The diagnostics show the connection type (direct / relay) and the target bitrate; the host adapts the encoder bitrate to the worst viewer (AIMD).

## When a Session does not connect or is relayed

Run the app's connection check first (Settings → Hubs → "Connection check…", [player.md](Using-the-app#connection-check)). It tells in plain language whether the server is reachable, whether STUN answers, whether the relay works with the credentials the server hands out, which NAT type the network has and whether a test connection through the server's servers succeeds. "Add to problem report" attaches the result to the next "Collect logs" bundle. The reading below is for the details behind it.

## Reading the connection logs and reasons

Host and viewer apps log one compact `info` line per connection (category `uzume.sessionhost` on the host, `uzume.sessionviewer` on the viewer; the connection is identified by viewer id or session id, no credentials are logged):

- `ICE viewer <id>: gathering complete, local host=2 srflx=1 prflx=0 relay=1`: local candidates by type once gathering finished. `srflx=0` means no STUN server answered (or none is configured).
- `ICE viewer <id>: connected relay (udp), pair local srflx 1.2.3.4:5000 udp <-> remote relay 5.6.7.8:49160 udp, local ..., remote ..., relay reason no_local_srflx`: the selected pair, both candidate counts (remote ones are those received over signaling) and, for a relayed path, the reason.
- `ICE viewer <id>: failed (never connected), local ..., remote ...`: the connection failed; compare the counts.

The host overlay (Diagnostics) shows type, RTT and reason per viewer, and a viewer sees its own connection the same way. A path is Relayed when either side of the selected pair is a relay candidate. Reasons:

| Reason (log token) | Shown as | Meaning |
|---|---|---|
| `forced` | relay forced | `UZUME_FORCE_RELAY=1` / `--force-relay` |
| `no_local_srflx` | no STUN reachable on this side | this app got no server-reflexive candidate: STUN blocked or not configured |
| `no_remote_srflx` | other side has no public address | the other app offered no server-reflexive candidate |
| `direct_failed` | direct path blocked (NAT or firewall) | both sides had srflx candidates but the direct checks failed: likely symmetric or carrier-grade NAT, or a firewall |

When the server changes its ICE/TURN servers it sends `ice_update` (feature `ice_update_v1`); the app logs `ICE servers updated: N stun, M turn` and uses the new servers for connections created afterwards. Running connections stay untouched.

Server flags: `-turn`, `-public-host`, `-turn-port`, `-turn-relay-ports`, `-turn-relay-ip` (env `UZUME_TURN*`, `UZUME_PUBLIC_HOST`), see [server-configuration.md](Server-configuration).
