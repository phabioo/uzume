At a glance: Reference for running the server: commands, flags and environment variables (`UZUME_*`), TURN, reverse proxy, certificate, limits, data directory. Everyday settings are in the app (Server › Settings). Install first: [server-install.md](Server-installation).

Contents: [Commands](#commands), [Server commands](#server-commands), [Flags and environment](#flags-and-environment), [Settings in the app](#settings-in-the-app), [Diagnostics bundle](#diagnostics-bundle), [Problem reports](#problem-reports), [Overview and metrics endpoint](#overview-and-metrics-endpoint), [Reverse proxy](#reverse-proxy), [Certificate](#certificate), [Resumable ROM uploads](#resumable-rom-uploads), [Limits for unauthenticated requests](#limits-for-unauthenticated-requests), [Core packages](#core-packages), [Run export](#run-export), [Plugins](#plugins), [Tools](#tools), [Windows](#windows), [Data directory](#data-directory).

Reference for running the Uzume server binary: commands, flags and environment variables, certificates. For installation see [server-install.md](Server-installation).

## Commands

```sh
uzume-server [flags]                          # run the server (HTTPS, self-signed certificate under <data-dir>/tls/)
uzume-server -dev -listen 127.0.0.1:8443 -data-dir /tmp/fb   # development: HTTP instead of HTTPS
uzume-server help                             # all commands, one line each
```

Commands for setup and emergencies (ADR 0030 D4; Windows, Linux and Docker; details in [Server commands](#server-commands)):

```sh
uzume-server setup-code [-json]               # a new setup code to set up this server from the app
uzume-server code [-json]                     # the security code (and SHA-256 fingerprint) of the certificate the server serves
uzume-server reset-admin [-user NAME] [-json] # a one-time admin password; nothing else is touched
uzume-server status [-json]                   # version, address, codes, set up or not, backups
uzume-server update [-check] [-yes]           # install the newest version of the channel (-check only looks)
uzume-server backup [--out DIR]               # make a backup now; also `backup list|config|verify` (see server-backup.md)
uzume-server restore <file>                   # restore a backup; the server must be stopped (see server-backup.md)
uzume-server reset [-confirm NAME] [-no-backup]  # delete all data and start fresh; the server must be stopped
```

Other commands:

```sh
uzume-server setup-admin -username <name>     # password as a single line from stdin; refused if an admin already exists
uzume-server renew-cert [-data-dir <dir>]     # renew the self-generated certificate now (refused with own cert/key; changes the security code)
uzume-server import-cores <dir>               # import libretro buildbot core zips offline
uzume-server import-cheats <zip>              # import a libretro-database archive as the cheat database offline (see "Cheat database")
uzume-server import-plugins <dir>             # offer every *.uzplugin file of a folder to Players (offline, see "Plugins")
uzume-server import-tools <zip>...            # add randomizer engine release zips to the tools catalog (offline, see "Tools")
uzume-server import-data --from DIR [-data-dir DEST]  # copy the data folder of a stopped server (the app's "On this PC" server) into DEST (see "Windows")
uzume-server sharing stop|start               # Windows, administrator: stop (disable) or start the server service (see "Windows")
uzume-server diagnostics <out.zip>            # write the diagnostics bundle without a running server (see "Diagnostics bundle")
uzume-server version [--json]                 # version info
uzume-server update check|stage [-channel stable|beta]   # machine-readable update selection, see updates.md
uzume-server rollback --list                  # earlier versions, see updates.md
```

On a systemd install use the script instead of calling the binary for `renew-cert` and `import-cores`: `sudo packaging/linux/install-server.sh renew-cert` / `import-cores <dir>` (it uses the service's data dir and user and restarts the service where needed), see [server-install.md](Server-installation).

Without an admin the server still starts and prints a setup banner with a **setup code** (see [Server commands](#server-commands)): the app on another PC sets the server up with it. A server on the same PC is set up by the app through the loopback endpoints (`/api/v1/local/*`); `setup-admin` creates the first admin without the app. A request counts as local only if the TCP peer is loopback, it carries none of `Forwarded`, `X-Forwarded-For`, `X-Real-IP`, `X-Forwarded-Host`, `X-Forwarded-Proto`, and its `Host` header is `localhost` or a loopback IP. Requests that arrive through a trusted proxy (`-trusted-proxy`) are never local. See [Reverse proxy](#reverse-proxy).

## Server commands

All commands find the data folder and settings like the service does: `-data-dir` and the other flags work on every command, and so do the `UZUME_*` variables. On Linux, a command typed in a shell also reads the `UZUME_*` lines of `/etc/uzume/server.env` (the file systemd gives the service) for variables that are not set in the shell, so a custom `UZUME_DATA_DIR`, port or public host is seen without extra work (`UZUME_SERVER_ENV_FILE=<path>` names another file, `none` turns this off). A flag wins over the shell, the shell over the file.

Where to run them:

| Platform | How |
|---|---|
| Linux (package) | `sudo uzume-server <command>` (or `sudo -u uzume uzume-server <command>`); the data folder belongs to the service user |
| Windows | in a terminal **as administrator** (the data folder is under `%ProgramData%\Uzume\Server`); without administrator rights the commands answer "access denied" and say so |
| Docker | `docker exec -it uzume-server uzume-server <command>` for `setup-code`, `code`, `reset-admin`, `status`, `update` and `backup`; `restore` and `reset` need the server stopped, see [server-docker.md](Server-in-Docker) |

All of them except `reset` and `restore` work with the server running (they share the database like `setup-admin`) and also with it stopped.

| Command | What it does |
|---|---|
| `setup-code [-json]` | Writes a **new setup code** into the database and prints it with the security code and the address. Only while no admin exists; otherwise "This server is already set up". An earlier code stops working, and so does this one when the server starts (the command says so while the server is not running). |
| `code [-json]` | Prints the **security code** and the SHA-256 fingerprint of the certificate the server serves (the self-generated one in `<data-dir>/tls/` or your own `-tls-cert`). It creates nothing: before the first start there is no certificate yet. |
| `reset-admin [-user NAME] [-json]` | Sets a **one-time temporary password** for an admin and prints it once; the admin must choose a new one at the next sign-in. Ends that user's sign-in lock. Saves, users, games and devices stay untouched (devices stay signed in). With one admin `-user` is optional; with several it is required and the error lists the names. Without an admin: "Not set up yet: use uzume-server setup-code". |
| `status [-json]` | Version and channel, whether the server is running (the data folder lock is tried and released at once), "ready for setup" (with the current setup code, a new one is created if none is valid; only while the server runs, because every start replaces the code, so a stopped server shows a hint to start it instead) or "set up" (users and admins), the address(es), the security code, the backup folder with the last backup, and the commands. Before the first start nothing is created. |
| `update [-check] [-yes] [-confirm-breaking]` | Looks for a newer version on the server's update channel and installs it like **Install update** in the app: the update is staged and handed to the root helper (Linux) or the update service (Windows). `-check` only looks (`-channel` then picks the channel). In a container it prints the image tag to pull instead. The sub-commands `update check`, `stage`, `apply-staged` and `watch` stay (see [updates.md](Updates)). |
| `backup`, `restore <file>` | See [server-backup.md](Server-backup). `restore` needs the server stopped. |
| `reset [-confirm NAME] [-no-backup]` | Deletes **all server data** in the data folder: database (accounts, devices, run state), saves, ROM library, firmware, plugins, the self-generated certificate and the import folder inside it. The server must be stopped (the data folder lock is taken like for `restore`; the error names how to stop it on your platform). You type the server name, or pass it with `-confirm`. First a safety backup **without ROMs** is written to the configured backup folder, else `<data-dir>/backups`; `-no-backup` skips it. Kept: the `backups` and `logs` folders, and everything outside the data folder (own certificate and key, `server.env`, a ROM library or import folder elsewhere). Afterwards the next start creates a new database and certificate (the security code changes) and prints a new setup code. |

### Setup code and security code

- **Security code**: derived from the certificate the server serves, for example `K7Q4-9WMP-2HXD` (12 characters from the Crockford base32 alphabet, groups of four). The app computes the same code from the certificate it sees and shows both so you can compare them before you trust a server; see [protocol reference](https://github.com/phabioo/uzume/blob/main/docs/reference/protocol.md#security-code-and-setup-code). It changes when the certificate changes (`renew-cert`, `reset`, or an expired self-generated certificate that was renewed).
- **Setup code**: six characters from the same alphabet, shown as `7H4K-2Q`. It works **once**, for **60 minutes**, and only while the server has no admin. The serving process creates a fresh one at every start while there is no admin (a restart replaces the old code) and prints a banner to its output (terminal, `journalctl -u uzume-server`, `docker logs`, the Windows installer's last page) and to the log. The code is stored in the database; it is written to the log only in that banner.
- The app redeems it with `POST /api/v1/setup` (user name, password of at least 12 characters, device): the first admin and the paired device are created in one step. After 10 wrong tries in total the code is deleted (run `setup-code` for a new one); 10 requests a minute per IP are allowed.

```
==================================================================
 Uzume server 0.18.0 is ready for setup

 Address        192.0.2.10:8443
 Security code  K7Q4-9WMP-2HXD
 Setup code     7H4K-2Q  (works once, valid 60 minutes, until 14:32)

 Enter the setup code in Uzume on another PC: Set up a server › On another device.
 New code later: uzume-server setup-code
==================================================================
```

## Flags and environment

A flag wins over the environment variable. Under systemd the variables live in `/etc/uzume/server.env`. Only `UZUME_*` names are read ([ADR 0030](https://github.com/phabioo/uzume/blob/main/docs/adr/0030-one-app-one-server-uzume.md)). Values saved in the app (Server › Settings › Network & sharing) win over both, see below.

| Flag | Environment | Default | Meaning |
|---|---|---|---|
| `-data-dir` | `UZUME_DATA_DIR` | `/var/lib/uzume` | Database, saves, firmware, certificate |
| `-listen` | `UZUME_LISTEN` | `:8443` | Listen address and port |
| `-name` | `UZUME_NAME` | empty | Server name, first start only |
| `-tls-cert`, `-tls-key` | `UZUME_TLS_CERT`, `UZUME_TLS_KEY` | empty | Own certificate and key (PEM), only together |
| `-dev` | `UZUME_DEV` | off | HTTP instead of HTTPS; excludes `-tls-cert` |
| `-ice-servers` | `UZUME_ICE_SERVERS` | `stun:stun.cloudflare.com:3478` | Comma-separated `stun:` URLs; `none` = no external STUN |
| `-turn` | `UZUME_TURN` | off | Embedded STUN/TURN relay; needs `-public-host` |
| `-cloudflare-turn-key-id` | `UZUME_CLOUDFLARE_TURN_KEY_ID` | none | Cloudflare Realtime TURN key (optional relay next to `-turn`) |
| `-cloudflare-turn-token-file` | `UZUME_CLOUDFLARE_TURN_TOKEN_FILE` | none | File holding the Cloudflare API token (alternative to the token variable) |
| (env only) | `UZUME_CLOUDFLARE_TURN_API_TOKEN` | none | Cloudflare API token; wins over the token file |
| `-turn-external` (repeatable) | `UZUME_TURN_EXTERNAL` (one server per line) | none | External TURN server (own coturn or a third-party provider), see "External TURN servers" below |
| (env only) | `UZUME_TURN_EXTERNAL_<n>_SECRET`, `UZUME_TURN_EXTERNAL_<n>_PASSWORD` | none | Shared secret (mode `secret`) or password (mode `static`) of the n-th external TURN server (1-based); wins over its `secret-file` / `password-file` |
| `-public-host` | `UZUME_PUBLIC_HOST` | empty | DNS name or IPv4 of the server's public address, optionally `host:port` (IPv6 in brackets, `[2001:db8::1]:8443`). The port is the external port a router forwards to the server; it is used for invite links and the app address, while TURN/STUN URLs use only the host part with their own ports. Without a port, invite links use the server's listen port (nothing for 443) |
| `-turn-port` | `UZUME_TURN_PORT` | `3478` | TURN UDP and TCP port |
| `-turn-relay-ports` | `UZUME_TURN_RELAY_PORTS` | `49160-49199` | TURN UDP relay range `min-max` |
| `-turn-relay-ip` | `UZUME_TURN_RELAY_IP` | empty | Fixed public IPv4 for relayed addresses instead of resolving `-public-host` |
| `-trusted-proxy` | `UZUME_TRUSTED_PROXY` | empty | Comma or space separated CIDRs/IPs of reverse proxies in front of the server, for example `127.0.0.1,10.0.0.0/8`; see [Reverse proxy](#reverse-proxy) |
| `-child` | `UZUME_CHILD` | `false` | Run as the app's child process ("Only on this PC", ADR 0030 D6): loopback only whatever the stored Network sharing setting says (the stored value is kept, not changed), no TURN, no self-update (the app updates the binary), a restart runs in-process, and the server shuts down gracefully when stdin reaches EOF or fails, so it never outlives the app. After the listener is bound it prints exactly one line to stdout, `UZUME_READY <port>` (the actual port, so `-listen 127.0.0.1:0` works; printed again after an in-process restart); everything else goes to stderr, the setup banner is not printed. If the port is taken it exits with status 1 and a message containing `address in use` on stderr. `PUT /api/v1/local/settings` with `network_sharing: true` answers 409 `child_mode`; `GET /api/v1/local/status` reports `network_sharing: false` and `child: true`. |
| `-library-import-dir` | `UZUME_LIBRARY_IMPORT_DIR` | `<data-dir>/library-import` | Folder that Server › Games › "Rescan folder" imports ROMs from (created at start; files are only read, never moved or deleted) |
| `-save-keep-recent` | `UZUME_SAVE_KEEP_RECENT` | `20` | Save history: newest versions per slot to keep |
| `-save-keep-daily` | `UZUME_SAVE_KEEP_DAILY` | `30` | Save history: days of which the newest version is kept |
| `-save-keep-weekly` | `UZUME_SAVE_KEEP_WEEKLY` | `26` | Save history: weeks of which the newest version is kept |
| `-core-buildbot-url` | `UZUME_SERVER_CORE_BUILDBOT_URL` | `https://buildbot.libretro.com/nightly` | Base URL of the libretro buildbot builds (https only) |
| `-core-info-url` | `UZUME_SERVER_CORE_INFO_URL` | `https://buildbot.libretro.com/assets/frontend/info.zip` | libretro core info archive (https only) |
| `-cheat-db-url` | `UZUME_CHEAT_DB_URL` | libretro-database archive on GitHub | Source of the cheat database (https only); `none` = off (see "Cheat database") |
| `-cheat-db-3ds-url` | `UZUME_CHEAT_DB_3DS_URL` | `none` | Optional source of 3DS cheat lists: a zip with `<TITLEID>.txt` files (https only); see "Cheat database" |
| `-core-index-url` | `UZUME_SERVER_CORE_INDEX_URL` | none | Deprecated and ignored since 0.8 (the signed core index was retired) |
| `-core-trust-key` | `UZUME_SERVER_CORE_TRUST_KEYS` | none | Extra trusted Ed25519 public key (base64) for the updates index only (cores are not signed); flag repeatable, env comma-separated |
| `-update-index-url` | `UZUME_SERVER_UPDATE_INDEX_URL` | Uzume's `updates-index` release | Signed updates index (https, or `file://` for tests) |
| `-plugin-index-url` | `UZUME_PLUGIN_INDEX_URL` | Uzume's `plugin-index` release | Signed plugin index `plugin-index.json` (+ `.sig` next to it), https or `file://`; `none` (or empty) turns the index off for offline servers, the Plugins page then has no catalog. Checked at start, every 24 h and with "Check now"; a failure is shown on the page and never stops the server. Trusted keys are the same as for the updates index (`-core-trust-key`) |
| `-tools-source-url` | `UZUME_TOOLS_SOURCE_URL` | GitHub releases API of the randomizer engine (`https://api.github.com/repos/upr-fvx/universal-pokemon-randomizer-fvx/releases`) | Release list of the tools catalog, https only; `none` turns the source off for offline servers (then use `import-tools`). Read at start, every 24 h and with "Check now" (Server › Tools); a failure is shown there and never stops the server (see "Tools") |
| `-update-request-dir` | `UZUME_SERVER_UPDATE_REQUEST_DIR` | `/run/uzume` | Directory of the update request file read by the root helper |

`0` for a `-save-keep-*` rule means unlimited for that rule. Invalid combinations (for example `-turn` without `-public-host`, a `-public-host` that is not `host[:port]` (scheme, path, spaces, port outside 1 to 65535, IPv6 without brackets), a non-`stun:` ICE URL, a non-IPv4 `-turn-relay-ip`) stop the start with an error.

### TURN security

The embedded TURN server caches the "device known and not revoked" result for 30 seconds (revoking a device drops its entry) and answers at most about 20 lookups per second for device IDs it has not seen, so unauthenticated packets cannot saturate the database. TURN/TCP connections must send their first message within 60 seconds and are closed after 10 minutes without data; one remote IP may hold at most 16 of the 64 TCP slots.

**STUN and ICE updates.** Without a configured STUN list the server hands apps `stun:stun.cloudflare.com:3478`, so they learn their public address even when the server itself sits in a LAN. Set `-ice-servers none` (or "Use no external server" in Settings → Network) for a purely local setup; adding a server in Settings replaces the default. Whenever the effective ICE configuration changes (STUN list, embedded TURN on/off, Cloudflare credentials refreshed) the server sends every connected app an `ice_update` message (feature `ice_update_v1`), so no reconnect is needed.

**Cloudflare TURN (optional).** As a relay without port forwarding: create a TURN key in the Cloudflare dashboard (Realtime → TURN), set `UZUME_CLOUDFLARE_TURN_KEY_ID` and put the key's API token in `UZUME_CLOUDFLARE_TURN_API_TOKEN` or in a file readable only by the server (`-cloudflare-turn-token-file /etc/uzume/cloudflare-turn.token`, mode 0600). The token is never accepted as a flag value, never logged, never written to the database or shown in Settings. The server requests 24 h credentials, refreshes them at half the TTL and offers them to apps in addition to the embedded TURN; URLs on port 53 are dropped. On errors it logs one warning per failure streak (without token or response body), retries with backoff and keeps the embedded TURN working. It stays off while Network sharing is off.

**External TURN servers (optional).** Any standard TURN server can be a relay provider: your own coturn on a VPS ([guide](coturn-on-a-VPS)) or a third-party provider. It runs next to the embedded TURN and Cloudflare TURN; apps get the credentials of every active provider (`turn_servers` in `hello_ack`, the join response, `viewer_joined` and `ice_update`) and ICE picks a path. Admins can manage the servers in the app (Server › Settings › Network & sharing › External TURN servers: add, edit, delete; see below) or with the flags. One `-turn-external` per server, as `key=value` fields separated by `;`:

| Field | Meaning |
| --- | --- |
| `urls` | Required. Comma-separated `turn:` / `turns:` URLs with optional `?transport=udp` or `?transport=tcp` |
| `mode` | `secret` (default) or `static` |
| `secret-file` | Mode `secret`: file holding the shared secret (alternative: `UZUME_TURN_EXTERNAL_<n>_SECRET`) |
| `username`, `password-file` | Mode `static`: the login (not secret) and the file holding its password (alternative: `UZUME_TURN_EXTERNAL_<n>_PASSWORD`) |
| `name` | Optional label for Settings, diagnostics and logs (default `external-<n>`) |

- **Shared secret (TURN REST API, coturn `use-auth-secret`).** The server never sends the secret. For each app it issues time-limited credentials: username `<expiry-unix>:<device-id>`, password `base64(HMAC-SHA1(secret, username))`, valid 12 h like the embedded TURN, renewed by an `ice_update` push every 6 h.
- **Static login.** One fixed username and password for every app (for providers that give you only that). The credentials carry no expiry, so anyone with a paired app can read them from the server; use a shared secret where the server supports it.

```
UZUME_TURN_EXTERNAL="name=vps;urls=turn:turn.example.org:3478?transport=udp,turn:turn.example.org:3478?transport=tcp,turns:turn.example.org:5349?transport=tcp;secret-file=/etc/uzume/turn-vps.secret"
# or as flags (repeatable):
#   -turn-external 'mode=static;username=uzume;urls=turn:relay.example.net:3478;password-file=/etc/uzume/relay.password'
UZUME_TURN_EXTERNAL_2_PASSWORD=...   # alternative to password-file for the second server
```

Secrets and passwords of flag-configured servers come only from the environment or a file (mode 0600, readable only by the server), never from a flag value; the server does not log them and does not show them. A flag server whose secret is missing is listed as "not set", is not handed to apps and logs one warning.

**Editing in the app.** Server › Settings › Network & sharing › External TURN servers lets an admin add, edit and delete servers (name, URLs, mode `secret` or `static`, username for static, secret or password). The same rule as for the other Network settings applies: what is saved in the app wins, flags and `server.env` are only the initial value. As long as nothing is saved, the list from `-turn-external` applies. The first add, edit or delete copies that list into the server database (names, URLs, modes and usernames, but not the secrets: a copied server keeps using its environment/file secret until you enter a new one in the form, also after a rename) and from then on the saved list is the only list; "Use hub flags again" deletes it. Secrets and passwords entered in the app are stored in the server database settings like the embedded TURN secret and are write-only: never shown again, not in logs, the diagnostics bundle or API responses; the app only shows "set" or "not set". Leave the field empty on edit to keep the stored value, tick "Clear" to remove it (a server without a secret is not used); changing the mode requires entering the new secret. Changes apply at once without a restart: new joins and `hello_ack` use the new list and connected apps receive it by `ice_update`. External servers stay off while Network sharing is off (the app says so; the list can still be edited, and applies after sharing is on and the server restarted). Server › Settings › Network & sharing and the diagnostics bundle (`network.external_turn` in `hub-info.json`, with `source` `web` (saved in the app) or `flags` and `active`) list the configured servers with URLs and mode and whether a secret is set, never the secret.

With `-turn`, forwarding the TURN ports exposes only the TURN port (`-turn-port`, 3478 UDP and TCP by default) and the UDP relay range (`-turn-relay-ports`). The relay works only with short-lived (12 h) credentials that the server issues to paired, non-revoked devices; every TURN authentication re-checks the device. Relay targets in internal address ranges (private LAN, CGNAT, ULA, loopback, link-local and similar) are refused, except the address of an app currently connected to the server, so a remote app can still reach an app in the server's LAN. Limits: UDP allocations only, at most 10 concurrent allocations per device and 64 concurrent TURN TCP connections. Decisions: [ADR 0012](https://github.com/phabioo/uzume/blob/main/docs/adr/0012-internet-sessions-and-save-comfort.md).

Recommendations for a home network:

- Forward only the TURN port and the relay range (for example `hub.example.com`, 3478 and 49160-49199), nothing else.
- Keep SSH and the server port unforwarded unless you need them from outside.
- Keep the server machine updated.
- Optionally put the server host in a separate guest or DMZ network or VLAN.
- Revoke devices you no longer trust.

When the server's HTTPS port is forwarded too, the sign-in and the app API are reachable from the internet. The server limits sign-in attempts per address (IPv6 per /64) and per username (addresses in the local network and addresses that signed in successfully in the last 30 days are exempt from the username limit), runs at most two password checks at once, applies a 30 s read deadline to ordinary requests and caps open connections at 512. Pairing requests, invite redemptions and WebSocket messages are rate limited. New passwords need at least 12 characters; use a long, unique admin password.

## Settings in the app

Library (admin, in the app): the game page's Manage tab has Rename (display title only, up to 200 bytes of UTF-8) and Delete (with confirmation); Saves tab › All users has the save upload. Deleting removes the entry and the ROM file; saves of the game are kept and stay under Saves tab › All users.

Server › Settings › Network & sharing edits port, embedded TURN, public host, TURN port, relay range and IP, STUN servers and save retention. These values live in the server database and win over flags and `server.env`, which only give the initial values (reset per field to return to the `server.env` value). Changes that need it are applied by a restart the server triggers itself (no root). The app refuses ports below 1024; set those with `install-server.sh --port` (a port saved in the app still wins until reset). Startup fallbacks keep the server reachable after a bad setting. Decisions: [ADR 0015](https://github.com/phabioo/uzume/blob/main/docs/adr/0015-hub-ui-pass.md).

Save state quota: each person may store up to 2 GiB of save states on the server (blobs and previews). The admin changes the limit with `state_quota_bytes` (bytes, at least 65 MiB) in `PATCH /api/v1/admin/settings`; uploads beyond it answer 413 `state_quota_exceeded`. The Storage page lists states as their own kind and offers "States of deleted games" as a clean-up.

## Diagnostics bundle

When something does not work, hand over one file instead of describing it: Server › Settings › Diagnostics › "Download diagnostics bundle" (admin only, `POST /api/v1/admin/diagnostics/bundle`, answered with `uzume-diagnostics-<UTC timestamp>.zip`). The bundle contains:

| Entry | Content |
|---|---|
| `README.txt` | what is in the bundle and what was removed |
| `hub-info.json` | Server version, channel, commit, protocol; OS, architecture, uptime, free space of the data directory; effective network configuration (listen, public host, TLS, TURN on/off and ports, STUN list); core source state and installed cores per system with versions; counts (users, devices, games, running Sessions); paired apps with app version, status, connected and last seen; feature list |
| `logs/hub.log`, `logs/hub.log.1`, `logs/updater.log` ... | the server's log files in `<data-dir>/logs/` (Windows service), newest first, capped at about 50 MiB in total (the oldest file that does not fit is cut to its newest lines) |
| `logs/recent.log` | the server's last 5000 log lines (at most 4 MiB) kept in memory since the start, so logs exist even without a log file or journal access (bundle from a running server only) |
| `logs/journal.txt` | Linux: the last 5000 lines of `journalctl -u uzume-server` when `journalctl` is available and the server user may read it |

Everything in it passes a redaction pass: TURN secret and credentials (`username`/`credential`), tokens, passwords, invite codes, `Authorization` headers and session cookies are replaced by `[redacted]`. IP addresses and host names stay because connection problems cannot be debugged without them; the page says so ("contains IP addresses and host names, no passwords or tokens"). Redaction is pattern based: look through the files before you post them publicly.

If the server does not come up or the app cannot reach it, `uzume-server diagnostics out.zip [-data-dir <dir>]` writes the same layout from the command line (as the service user, for example `sudo -u uzume uzume-server diagnostics /tmp/out.zip -data-dir /var/lib/uzume`). It never opens the database, so a running server is not disturbed; `hub-info.json` then has `"mode": "offline"` and lacks cores, counts and apps, and the network values come from `server.env`/flags, not the settings saved in the app. It refuses to overwrite an existing file.

## Problem reports

Apps send reports with Settings → Diagnostics → "Report a problem…" ([app.md](Using-the-app)). Admins find them under Server › Reports ("n new" badge; "n new reports" is information, not a Status problem). The list shows reports newest first with the filters All / New / Seen; a report shows who sent it (user, device, app version, OS), the text, the linked event if any, and its files for download or as one zip. Opening a report does not mark it seen: "Mark as seen" does. Delete is in the "⋯" menu and asks first. Reports are kept until an admin deletes them (the files live in `<data>/reports/<id>/`; they are part of the data directory backup).

Limits: 25 MB and 12 files per report, description up to 8000 characters, 5 reports per device and hour (429). Text attachments pass the server redactor again on receipt (tokens, passwords, TURN credentials, cookies become `[redacted]`). The server logs only report id and device, never the content.

REST (admin device token; the app uses it): `GET /api/v1/admin/reports?status=new|seen`, `GET`/`DELETE /api/v1/admin/reports/{id}`, `POST .../{id}/seen`, `GET .../{id}/files/{name}`, `GET .../{id}/archive`. Sending is `POST /api/v1/reports` (any paired device, feature `reports_v1`).

## Overview and metrics endpoint

**Overview** (admin only, Server › Overview in the app, formerly the Status page) shows the health of the server and refreshes itself every 10 s: server name, version, uptime and public address; a banner for the single most urgent problem (the navigation badge carries the same short text); CPU, memory (server process, and the whole machine where the platform reports it) and the disk of the data directory; running Sessions with viewers and the TURN relay allocations; connected apps with version and state (an app older than the server is amber); the warnings and errors of the last 24 hours with "Download log" (the last log lines held in memory, redacted like the diagnostics bundle). Problems are: data directory above 90 % used (error above 97 %), the built-in TURN relay not started with the saved settings, and a failed server update install. CPU, memory and disk are read on Linux and Windows; elsewhere the cards show "n/a".

**Server › Settings › Diagnostics (Metrics)** switches an optional Prometheus endpoint; it is off by default (`GET /metrics` then answers 404). Turning it on creates a token that is shown once; the server keeps only its SHA-256 and never logs it. "Create new token" replaces it at once. A scrape sends the token as a bearer header and gets the text format 0.0.4; without a valid token the server answers 401.

```yaml
scrape_configs:
  - job_name: uzume-server
    scheme: https
    authorization: { credentials_file: /etc/prometheus/uzume.token }   # the token, nothing else in the file
    static_configs: [{ targets: ['hub.example.com:8443'] }]
    tls_config: { insecure_skip_verify: true }   # only for the server's self-signed certificate; prefer a reverse proxy with a real one
```

| Metric | Type | Meaning |
|---|---|---|
| `uzume_server_info{version}` | gauge | always 1, carries the server version |
| `uzume_server_uptime_seconds` | gauge | seconds since the process started |
| `uzume_sessions_active`, `uzume_session_viewers` | gauge | running Sessions and their viewers |
| `uzume_apps_connected` | gauge | Apps with an open connection |
| `uzume_turn_enabled` | gauge | 1 when the built-in TURN relay runs |
| `uzume_turn_allocations`, `uzume_turn_relay_ports` | gauge | live relay allocations and the size of the relay port range (only with TURN) |
| `uzume_disk_free_bytes`, `uzume_disk_total_bytes` | gauge | the volume holding the data directory |
| `uzume_process_memory_bytes`, `uzume_cpu_usage_ratio` | gauge | memory held by the server process; machine-wide CPU load 0 to 1 |
| `uzume_log_records_total{level}` | counter | WARN and ERROR log records since the start |

Dashboards and alert rules written for the metrics of FrameBeam need the `uzume_*` names above.

The same data is available over the REST API for the admin in the app (admin access token; 401 without a token, 403 for users): `GET /api/v1/admin/status` (the Overview as JSON, problems most urgent first), `GET /api/v1/admin/status/log` (redacted log text), `GET`/`PUT /api/v1/admin/metrics` (`{"enabled": bool}`; switching on for the first time returns the token once) and `POST /api/v1/admin/metrics/token` (new token, returned once). The Storage page reads `GET /api/v1/admin/storage` (sizes per kind, clean-up suggestions) and `POST /api/v1/admin/storage/cleanup` runs one clean-up. Both the page and the API read one data builder (`server/internal/statusinfo`).

The endpoint carries no names, paths or secrets. It lives on the server's own listener: put it behind your reverse proxy like the rest.

## Reverse proxy

The server can run behind a TLS-terminating reverse proxy (nginx, Caddy, Traefik, HAProxy; run the server with `-dev` or on loopback, the proxy provides HTTPS). Tell the server which peers are proxies:

```
UZUME_TRUSTED_PROXY=127.0.0.1,::1        # proxy on the same host
UZUME_TRUSTED_PROXY=172.17.0.0/16        # proxy in a Docker network
```

For a request whose TCP peer is inside the list:

- The client IP is the right-most `X-Forwarded-For` entry that is not itself a trusted proxy (else `Forwarded: for=`). Entries added by the client further left are ignored. Rate limits, the per-username login backoff, pairing records and the app's WSS address (used by the TURN LAN check) all use this address. When the proxy sends no usable address, all its clients share one bucket and are never exempt.
- The scheme is the right-most `X-Forwarded-Proto` (else `Forwarded: proto=`). With `https` `Strict-Transport-Security: max-age=31536000` is sent. Make the proxy overwrite (not append to) these headers and forward `Host`.
- The request is never local: `/api/v1/local/*` stays closed, and the proxy's own address never gets the LAN exemption from the username backoff. A real LAN client address from the header does. An app on the server's own machine that talks to `127.0.0.1` directly is also "from the proxy" when loopback is listed; use `setup-admin` and pair it through the proxy URL then.

Peers outside the list are never believed: their forwarding headers are ignored (and only make a loopback request non-local). Without `-trusted-proxy` a loopback request is local only when no proxy header is present and `Host` is a loopback name or IP.

Residual risk without `-trusted-proxy`: a proxy on the same host that sends no forwarding header and forwards `Host: localhost` or `127.0.0.1` (nginx `proxy_pass http://127.0.0.1:8443;` sets `Host` to the upstream address by default) is indistinguishable from a local client, so every internet client could open `/api/v1/local/*` before an admin exists and use it. Always set `-trusted-proxy` for such a setup, or `proxy_set_header Host $host;`. The server's own self-signed certificate never gets HSTS; HSTS is sent only when a trusted proxy reports https. Server › Settings › Network & sharing lists the configured proxy networks (read-only; set by flag or `server.env`).

## Certificate

The certificate's SHA-256 fingerprint is printed to the log at startup (`journalctl -u uzume-server | grep -i fingerprint`); apps confirm it on first contact.

- The server creates a self-signed certificate (ECDSA P-256, 10 years) under `<data-dir>/tls/`. At startup it renews it when it is expired or expires within 30 days; the previous pair stays as `cert.pem.prev` / `key.pem.prev`. The Settings page shows "Expires within 30 days".
- `uzume-server renew-cert [-data-dir <dir>]` renews on demand and prints the certificate directory it renewed. It refuses when `-tls-cert` / `-tls-key` are set. Own certificates are never modified.
- A renewed certificate has a new fingerprint, which apps must confirm ([ADR 0009](https://github.com/phabioo/uzume/blob/main/docs/adr/0009-finish-poc.md)); see [app.md](Using-the-app).
- On a systemd install do not call `uzume-server renew-cert` directly: another user or `-data-dir` renews a different certificate, and root would create files the service cannot read. Use `install-server.sh renew-cert`.

## Resumable ROM uploads

Apps upload ROMs in chunks (up to 64 MiB). Unfinished uploads are kept in `<data>/tmp/uploads/<id>` and removed after 7 days without activity; at most 16 open uploads per user, 4 GiB per file. Reading one chunk has its own deadline (15 minutes); the old one-shot upload routes may take up to 2 hours. No flag configures these values. See [local ROMs](Local-ROMs).

- **Same file only.** A session is resumed by SHA-256, not by file name and size alone: the app hashes the chosen file first and the server verifies the received file against that hash. API clients that send no hash still resume by name and size (ADR 0025).
- **Free space.** A ROM upload is refused with 413 `payload_too_large` ("Not enough free space on the Hub's disk") when its size plus what other open uploads still have to write would leave less than 1 GiB free on the data volume (reserve for the database and saves). An unknown free size (platforms where the server cannot read it) never refuses.
- **Damaged sessions.** If the data file of a session is shorter than its stored offset (cleaned up by hand, antivirus), the next chunk is answered 409 and the session restarts at offset 0, so a ROM never ends up with a hash that does not match its content.

## Limits for unauthenticated requests

Pairing requests and invite redemptions need no login, so they are limited per source (IPv4 address or IPv6 /64) in addition to the global caps: 5 requests per minute and 5 open pairing requests per source, 20 open pairing requests in total (invited users have their own budget). Failed invite or local-pairing attempts count per source; the global cap of 1000 failures per minute only holds back sources that failed themselves, so failures from elsewhere never lock out a source without failures. The Local setup (`/api/v1/local/setup`) creates the admin and pairs the app as one step: if pairing fails the admin is removed again, so setup can simply be repeated.

## Core packages

The server downloads cores RetroArch-style from the libretro buildbot (nightly channel) and serves them to apps; apps never contact the buildbot. The catalog comes from the libretro core info archive (`info.zip`) and the buildbot's `.index-extended`; it is refreshed at start, every 24 h and on "Check source now" on Systems & Cores. Installing a core is always an admin action. With "Automatic core updates" (On by default, toggle in the source panel of Systems & Cores = server default, setting `core_auto_update`) the server updates every installed core that has a newer build after each catalog refresh; turning it on triggers a check. Since 0.14 each system can use the server default, On or Off, and each core can override its system (migration 0017). A failing core does not stop the others, and the last auto-update error is shown on Systems & Cores. When off, updates are manual (see below).

- On Systems & Cores the Cores tab lists the installed cores per system with Update (when the buildbot has a newer build and automatic updates are off or have not run yet; updating keeps the old build until the new one is cached), Remove and Make default, and "Available from the libretro buildbot" with Install. A core without an Uzume profile runs as experimental in the app (marked "experimental" there). A system without an installed core shows a notice; apps cannot start its games until a core is installed.
- A download is checked against the CRC32 of `.index-extended` and must contain exactly one library file; the server pins the SHA-256 of that file at install and serves exactly it. On a CRC32 mismatch (the buildbot replaces nightly builds in place, so the cached catalog can be stale) the server refreshes the catalog once and retries once when a build changed; otherwise the install fails with a message that the download does not match the buildbot index. The buildbot signs nothing; trust equals HTTPS to the buildbot.
- Versions are build ids `YYYY.MM.DD`, with `.N` appended for a second install on the same day. There is no expected-version selector; all apps of a server run the build the server serves.
- The server needs HTTPS egress to `buildbot.libretro.com`. Offline: `uzume-server import-cores <dir>` (systemd: `install-server.sh import-cores <dir>`) with the layout `<dir>/<platform>/<core>_libretro.<suffix>.zip` (platform `linux-x64` or `windows-x64`, suffix `.so` or `.dll`) plus an optional `<dir>/info.zip` and `<dir>/<platform>/.index-extended` (the CRC32 is checked only when that file is present). The SHA-256 is pinned at import.
- Nintendo 3DS (0.10, [ADR 0022](https://github.com/phabioo/uzume/blob/main/docs/adr/0022-nintendo-3ds-with-azahar.md)): the server knows system `3ds` but has no default core. Install `azahar` from "Available from the libretro buildbot" on Systems & Cores, then Make default if needed. It is a profiled core. No firmware or system files are required or shipped; use decrypted dumps only. Apps need OpenGL Core >= 3.3.
- Servers upgraded from 0.7: packages from the retired signed source (origin `legacy-mirror`, e.g. `melonds_ds`) are removed on start (database rows and stored files; one info log line per core; saves are untouched). A removed default core is replaced by another installed core of the system, else the system has no core and the Systems page asks the admin to install one from the libretro buildbot (`import-cores` on an offline server). Apps cannot start games of a system without a core.

A catalog refresh in which the build index of one platform (for example `windows-x64`) fails still succeeds for the other platforms: that platform keeps its previous builds, the failure is shown as a warning on Systems & Cores and in the log, and automatic updates continue. Only when every platform fails is the refresh a failure.

Details: [ADR 0020](https://github.com/phabioo/uzume/blob/main/docs/adr/0020-cores-from-the-libretro-buildbot.md).

## Cheat database

The server downloads the cheat lists of the libretro-database (`cht/` of the systems gb, gbc, gba and nds) and the No-Intro DATs it needs from `-cheat-db-url` (default `https://github.com/libretro/libretro-database/archive/refs/heads/master.zip`, a zip; https only) and serves them to apps and the cheats plugin (`GET /api/v1/games/{game_id}/cheats`). It fetches in the background, at start when nothing is stored or the lists are older than 7 days (retry after 6 hours on failure), and on `POST /api/v1/admin/cheat-db/refresh`; a failed download keeps the stored lists. The lists live in `<data-dir>/cheat-db/` (not part of backups). The admin changes the source with `PUT /api/v1/admin/cheat-db` (needs "Confirm it's you"); that setting wins over the flag. `none` stops the downloads; stored or imported lists are still served. The list of a game answers `source: "none"` when the database is off and nothing is stored, and `source: "unavailable"` when it is on but holds no lists yet (not downloaded, or the first download failed); the cheats plugin shows the two differently.

3DS: libretro-database has no 3DS lists. The server serves Gateway lists per title ID from files `<TITLEID>.txt` (16 hex digits, any case; `[Cheat name]` header lines followed by code lines `XXXXXXXX YYYYYYYY`; `*citra_enabled` and other `*` lines are ignored, Citra/Azahar style). They come from a `3ds/` folder in an archive given to `import-cheats` (next to `cht/`) or from `-cheat-db-3ds-url`, a zip with such files anywhere in it (default `none`), which is refreshed together with the main source and changed with `url_3ds` in `PUT /api/v1/admin/cheat-db`. Every code of these lists is guessed as `gateway`.

Offline: `uzume-server import-cheats <zip>` takes a libretro-database archive (with `cht/` and `metadat/no-intro/`, with or without the top folder) and replaces the stored lists; no restart is needed. The server needs HTTPS egress to the source host. Games match by the SHA-1/CRC32 of the stored ROM through the DAT, otherwise by file name or title, then by the base title when the region fits (region tag of the name or region letter of the game code, and the same revision tag such as `Rev 1` or `v1.1`, none meaning the base release; a list of another region or revision is never used, cheats are region specific, and the app log names it); 3DS games by title ID only; a game without a list gets an empty answer.

Plugin store: each person may keep up to 16 MiB and 1024 keys per plugin (values of up to 256 KiB); the limits are fixed. Rows are in the database (backups include them) and go with a removed user.

## Run export

`GET /api/v1/admin/runs/<id>/export` (admin) streams the event log as one JSON document. A database error before the first byte answers HTTP 500. If the export cannot be completed after it started (the status is already 200), the document ends with an `"error"` member after the `events` array (for example `"export incomplete after event 3 of 7"`) and the error is logged; a complete export never has an `"error"` member, so check for it after downloading.

## Plugins

Plugins (0.11, [ADR 0023](https://github.com/phabioo/uzume/blob/main/docs/adr/0023-plugins.md)) are `.uzplugin` files (zip with `manifest.json`, Lua code and assets, at most 32 MiB packed, 64 MiB unpacked, 2000 entries; optional `SIGNATURE.json`). Legacy `.fbplugin` files are still accepted for upload and import. The server stores them under `<data-dir>/plugins/<plugin_id>/<version>.uzplugin` (metadata in the database) and offers every stored version to all apps (`GET /api/v1/plugins`, handshake feature `plugins_v1`). The server never runs plugin code.

- Origin: `official` when `SIGNATURE.json` lists exactly the files of the zip and carries a valid Ed25519 signature of a trusted key (the Uzume release key plus `-core-trust-key`); otherwise `unverified` (a present but invalid signature is reported as such and never counts as official).
- Plugins page (admin): table of the offered versions, detail with permissions, download and remove. Upload shows name, version, origin and one tick per requested permission; "Offer to Players" works only with every tick set (checked on the server). The same id and version with a different file is refused; remove the old version first. A newer version replaces the older ones of the same id (index update, upload or import); an older version stays while a run that is not finished or abandoned uses it and is removed when the run ends or is deleted (also checked at server start). Uploading a version older than the newest removes nothing. Removal keeps the copies apps already installed and is refused (409) while a run that is not finished or abandoned uses the version.
- Official catalog: the page lists the signed official plugin index (default, `-plugin-index-url`; `none` disables it) with Install / Update. Install downloads the file, checks size and SHA-256 against the signed index and the embedded signature against the trusted keys, and stores it as `official` (source `index`).
- Offline: `uzume-server import-plugins <dir>` offers every `*.uzplugin` file directly in `<dir>` (source `import`; running the command is the confirmation of the permissions; unsigned files become `unverified`). Run it as the service user with the service's `-data-dir`.
- Publishing (see [plugin-publishing.md](https://github.com/phabioo/uzume/blob/main/docs/guides/plugin-publishing.md)): `uzume-sign plugin-sign -in x.uzplugin -out signed.uzplugin` (seed from `UZUME_SIGNING_KEY`), `uzume-sign plugin-add -index plugin-index.json -plugin signed.uzplugin -url https://.../signed.uzplugin` (refuses unsigned plugins), then `uzume-sign sign -index plugin-index.json` writes `plugin-index.json.sig`.

## Tools

Tools (0.21, [ADR 0031](https://github.com/phabioo/uzume/blob/main/docs/adr/0031-game-variants-and-randomizer.md) D3) are programs the server offers to apps the way it offers cores; the only tool is the randomizer engine (id `randomizer`, UPR FVX, GPL-3.0, source link in the entry). The server never runs a tool: apps download the zip for their platform from the server (`GET /api/v1/tools`, handshake feature `tools_v1`) and run it themselves.

- Source: the GitHub releases list of `-tools-source-url`. Check now (Server › Tools, `POST /admin/tools/randomizer/sync`) and a check every 24 h record the stable releases (no drafts or prereleases) with their zips per platform (`windows-x64`, `linux-x64`, `linux-arm64`; names `UPR_FVX-v1_6_2-Windows.zip`, `-Linux_x86.zip`, `-Linux_ARM.zip`) and the newest version as an update hint. Nothing is downloaded by a check.
- Install downloads every platform zip of a version (https only, at most 400 MB each), checks that it holds `UPR-FVX.jar` and `java/bin/java` or `java/bin/java.exe` (at the zip root or below one top folder; the folder is reported as `root`), pins the SHA-256 at the first download and stores the zip under `<data-dir>/tools/<tool>/<version>/<platform>.zip`. A later download with another hash is refused (409 `tool_hash_mismatch`). Backups do not contain the folder; after a restore, Install fetches the files again against the pinned hashes.
- Offered: the admin decides. A newly installed tool is not offered (default off); apps see only offered tools and only cached versions, and the package download answers 404 otherwise. Un-offering keeps the copies apps already have.
- Remove a version: refused (409 `tool_in_use`, with `used_by` counts) while a variant that is not deleted or a run that is not finished or abandoned pins it; there is no force.
- Offline: `uzume-server import-tools <zip>...` takes the release zips as downloaded (version and platform come from the file name, content checked like a download, SHA-256 pinned, origin `import`; one line per file; the offered switch is not touched). Run it as the service user with the service's `-data-dir`.
- Apps: an app lists `GET /api/v1/tools` (any trusted device), downloads the zip of its platform with `GET /api/v1/tools/{id}/packages/{version}/{platform}` (ETag = SHA-256, 304) and checks the hash; the app uses the version a variant or run pins, so keep old versions while they are used. Using it is explained in [app.md](Using-the-app#randomize-a-game). The server stores only recipes of game variants and runs (engine version, settings string, seed, base pin) in the database, so they are part of the normal backup; the engine zips are not (see Install above). The server never runs the engine and never stores ROM data, patches or spoiler logs.

## Windows

Install with the Uzume MSI (feature server; see [packaging.md](https://github.com/phabioo/uzume/blob/main/docs/guides/packaging.md)), usually through "Set up a server on this PC" in the app, or manually: `msiexec /i uzume.msi /qn ALLUSERS=1 INSTALL_HUB=1 INSTALL_PLAYER=0 HUB_PORT=8443 NETWORK_SHARING=1`. The binary `uzume-server-windows-amd64.exe` alone is for manual and test use. Decisions: [ADR 0021](https://github.com/phabioo/uzume/blob/main/docs/adr/0021-one-windows-installer.md).

- Service `UzumeServer` runs as `NT SERVICE\UzumeServer` with `-listen :<port> -network-sharing=<true|false>` from the MSI properties. A restart from the app (Server › Settings) re-runs the server in-process.
- Defaults: data `%ProgramData%\Uzume\Server`, import dir `<data>\library-import`, update request dir `<data>\update-request`. Logs when running as a service: `<data>\logs\hub.log` (rotated at 10 MB, one old file kept).
- Updates: service `UzumeServerUpdater` (LocalSystem), see [updates.md](Updates).
- `-network-sharing` (env `UZUME_NETWORK_SHARING`, default `true`): initial value only; after the first start the stored setting wins. Off = the server listens only on 127.0.0.1 and ::1 (standalone mode, TURN off). Changing it restarts the server in-process.
- Server › Settings › Network & sharing: "Network sharing" and "Import folder" are stored settings. `-library-import-dir` is only the initial value or fallback. The import folder must be an absolute, existing folder the server can read (else `import_dir_unreadable`).
- `uzume-server grant-folder <path>` (Windows, run as administrator): gives `NT SERVICE\UzumeServer` read and list access (inherited) to a ROM folder outside the server's data directory. Other systems print "not supported" and exit 2.
- `uzume-server import-data --from DIR [-data-dir DEST]`: copies the data folder DIR of a **stopped** server (the app's "Only on this PC" server) into DEST (default: the data folder of the service, `%ProgramData%\Uzume\Server`); the MSI runs it elevated (`IMPORT_SERVER_DATA=<dir>`) after the service ACL grants and before the service starts. It copies all files and folders except `hub.lock`, checks the copied database (`PRAGMA integrity_check`) and points absolute paths stored in the database settings (import folder, backup folder) that lie inside DIR to DEST (ROMs, saves, firmware, cores and plugins are stored relative to the data folder). It never deletes or overwrites anything in DEST and removes its own partial copy when anything fails. On success DIR gets `moved.json` (`{"to": DEST, "at": RFC 3339}`) and DEST is printed on stdout (a summary goes to stderr). Exit status 3 means it refused before changing anything: DEST already has a server database, DIR has none, DIR's server is still running, or the folders are the same or nested; 2 is a usage error, 1 any other failure.
- `uzume-server sharing stop --grant-user DOMAIN\name [--result FILE] [-data-dir DIR]` and `uzume-server sharing start [--result FILE]` (Windows, elevated; other systems print that they are Windows only and exit 2) are what the app's "Stop sharing" and "Share again" run. `stop` gives the account an inherited Modify entry on the data folder, stops `UzumeServer` (and `UzumeServerUpdater` if present; waits up to 30 s) and sets both to **disabled** (they stay registered); afterwards the app runs the server as its child on the service's data folder, so accounts and saves stay, nobody else can connect. `start` sets both to automatic and starts `UzumeServer` (wait up to 30 s); stop the app's child first, the data folder has one server only. With `--result` both write `{"ok": bool, "error": string, "data_dir": string, "port": int}` (`port` from `HKLM\Software\Uzume\Server` value `Port`, 0 if unknown; `data_dir` is the service data folder) and exit 0 or 1.
- Local API (`/api/v1/local/status|setup|pair|settings`, feature `local_setup_v1`): loopback only, admin credentials needed, used by the app's "Set up a server on this PC". Five wrong passwords lock local pairing for a minute. See [protocol.md](https://github.com/phabioo/uzume/blob/main/docs/reference/protocol.md).

## Data directory

Default `/var/lib/uzume`: `uzume.db` (SQLite, metadata only), `saves/`, `firmware/<system>/`, `tls/`, `cores/`, `updates/`, and `backups/` (database backups before schema migrations, newest 5), `plugins/`, `hub.lock` (held by the running server). Full server backups (database, saves, plugins, settings, optionally ROMs) go to a folder of your choice, not into the data folder: see [server-backup.md](Server-backup). ROMs, saves and firmware are files; they are never stored in SQLite.

- **Staging area.** `tmp/` holds uploads and staged files of the running server. Only the serving process cleans it at start (leftovers of a crash); CLI commands such as `import-cores`, `import-plugins` and `setup-admin` open the data directory next to the running service and leave `tmp/` alone. A firmware `.bak` left by a crash is restored when the firmware file is missing, otherwise removed.
- **Newer database.** The server refuses to start when `uzume.db` has a schema newer than the binary knows (after a downgrade or a rolled-back update): "database schema N is newer than this server knows". Install the matching version or restore a copy from `backups/hub-*.db`. "Go back" in Settings → Updates (or `uzume-server rollback`) does this with the right backup and a safety backup, see [updates](Updates#rollback).
