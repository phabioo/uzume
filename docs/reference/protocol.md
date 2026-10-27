# Protocol reference

At a glance: The shared protocol of server and app: OpenAPI spec `protocol/openapi/uzume.yaml`, WSS message schemas in `protocol/schemas/`, handshake features, endpoint table, security and setup codes, join link, WSS messages and the history per milestone. `protocol_version` is 1.

Contents: [Security code and setup code](#security-code-and-setup-code), [Join link](#join-link), [WSS messages (/api/v1/ws)](#wss-messages-apiv1ws).

Shared protocol definition for server and app: `protocol/openapi/uzume.yaml` (OpenAPI 3.0.3) and `protocol/schemas/` (WSS message schemas, draft 2020-12, with examples). Summary and navigation: [protocol/README.md](../../protocol/README.md). Rules for agents: `protocol/CLAUDE.md`.

- Current `protocol_version`: **1** (integer, separate from product versions). Server and app each report `protocol_version` and `min_protocol_version`.
- Compatibility: `Player.protocol_version < Hub.min_protocol_version` -> `player_too_old`; `Hub.protocol_version < Player.min_protocol_version` -> `hub_too_old`.
- Error format: `{"error": {"code": <enum>, "message": string}}`. Auth: Bearer (`fba_` access token, 15 min, `fbd_` device credential, `fbp_` poll token).
- Current OpenAPI spec version: 1.43.1 (history per milestone below). Handshake features: `saves_v1`, `sessions_v1`, `users_v1`, `uploads_v1` (only advertised when the caller may upload), `firmware_v1`, `cores_v1`, `turn_v1` (only while a relay provider is on: embedded TURN, Cloudflare TURN and/or an external TURN server), `saves_v2`, `saves_v3`, `saves_v4`, `saves_v5`, `saves_v6`, `cores_v2`, `local_setup_v1`, `ice_update_v1`, `plugins_v1`, `runs_v1`, `runs_v2`, `local_roms_v1`, `reports_v1`, `accounts_v1`, `setup_code_v1`, `states_v1`, `run_rules_v1`, `tools_v1`, `variants_v1`, `plugin_store_v1`, `cheat_db_v1`, `admin_v1` (only advertised to admins); `cores_index_v1` existed in 0.4 to 0.7 and was removed in 0.8.

| Method | Path | Auth | operationId |
|---|---|---|---|
| GET | `/.well-known/uzume` | none | getHubInfo (`setup_required`: true while the server has no admin) |
| POST | `/api/v1/pairing/requests` | none | createPairingRequest |
| GET | `/api/v1/pairing/requests/{request_id}` | poll token | getPairingRequest |
| POST | `/api/v1/auth/token` | none | createAccessToken |
| POST | `/api/v1/auth/revoke` | Bearer | revokeSelf |
| POST | `/api/v1/handshake` | Bearer | postHandshake |
| GET | `/api/v1/games` | Bearer | listGames |
| GET | `/api/v1/games/{game_id}` | Bearer | getGame |
| GET | `/api/v1/roms/{sha256}` | Bearer | downloadRom (Range, ETag) |
| GET | `/api/v1/saves` | Bearer | listSaves |
| GET | `/api/v1/games/{game_id}/saves/{slot}` | Bearer | getSaveSlot |
| PUT | `/api/v1/games/{game_id}/saves/{slot}` | Bearer | putSave (raw body, `X-Uzume-Base-Revision`/`-Content-SHA256`/`-Sync-Reason`, 409 `save_conflict`) |
| GET | `/api/v1/games/{game_id}/saves/{slot}/content` | Bearer | downloadSaveContent (ETag, `X-Uzume-Save-Revision`) |
| GET | `/api/v1/games/{game_id}/saves/{slot}/history` | Bearer | listSaveHistory |
| GET | `/api/v1/games/{game_id}/saves/{slot}/history/{version}/content` | Bearer | downloadSaveHistoryContent |
| POST | `/api/v1/games/{game_id}/saves/{slot}/history/{version}/restore` | Bearer | restoreSaveHistoryVersion (`{expected_revision}`, 200 SaveSlot, 409 `save_conflict_stale`; `saves_v2`) |
| POST | `/api/v1/games/{game_id}/saves/{slot}/snapshots` | Bearer | createSaveSnapshot (optional `{label}`, 201 SaveHistoryVersion, 404 without checkpoint; `saves_v2`) |
| DELETE | `/api/v1/games/{game_id}/saves/{slot}/history/{version}` | Bearer | deleteSaveVersion (204; any history version since `saves_v6`, before only `manual_snapshot` (409 `save_not_snapshot`, no longer returned); 409 `conflict` for the secured version of an open conflict; 404 when missing; `saves_v3`) |
| POST | `/api/v1/games/{game_id}/saves/{slot}/upload` | Bearer | uploadSaveFile (409 `session_running` while a Session of the caller runs the game since `saves_v5`; raw `application/octet-stream`, headers `X-Uzume-Content-SHA256` and `X-Uzume-Expected-Revision`, 0 = slot must not exist; 200 SaveSlot, 400 `bad_request`, 404, 409 `save_conflict_stale`, 413 `payload_too_large`; `saves_v4`) |
| POST | `/api/v1/games/{game_id}/saves/{slot}/rename` | Bearer | renameSaveSlot (`{name}`, 200 SaveSlot under the new name; 400 invalid or same name incl. Windows reserved names, 404 unknown slot, 409 `save_slot_exists`, 409 `save_slot_protected` for `default` (rename only; the main slot can be deleted since `saves_v6`), 409 `session_running`; `saves_v5`) |
| DELETE | `/api/v1/games/{game_id}/saves/{slot}` | Bearer | deleteSaveSlot (required query `confirm_revision` = the checkpoint revision the caller saw; 204, hard delete of checkpoint, history, conflicts and content; 400 missing or invalid `confirm_revision`, 404 unknown slot, 409 `save_conflict_stale` on a different revision, 409 `save_slot_protected` for `default`, 409 `session_running`, 409 `conflict` while the slot has an open conflict; `saves_v5`) |
| POST | `/api/v1/games/{game_id}/saves/{slot}/conflicts/{conflict_id}/resolve` | Bearer | resolveSaveConflict (409 `save_conflict_stale`) |
| GET | `/api/v1/users` | Bearer | listUsers (`{id, display_name, online}` for the invite field) |
| GET | `/api/v1/sessions` | Bearer | listSessions (Sessions the caller may join or owns, plus inviting) |
| POST | `/api/v1/sessions` | Bearer | publishSession (ends the device's previous Session; 409 `capability_missing` without H.264 encode) |
| GET | `/api/v1/sessions/{session_id}` | Bearer | getSession (403 `session_forbidden`, 404 `session_not_found`, 410 `session_ended`) |
| PATCH | `/api/v1/sessions/{session_id}` | Bearer (owner device) | updateSession (visibility; revokes viewers no longer allowed) |
| DELETE | `/api/v1/sessions/{session_id}` | Bearer (owner device) | endSession |
| PUT | `/api/v1/sessions/{session_id}/invites/{user_id}` | Bearer (owner device) | inviteSessionUser |
| DELETE | `/api/v1/sessions/{session_id}/invites/{user_id}` | Bearer (owner device) | withdrawSessionInvite (removes that user's viewers) |
| POST | `/api/v1/sessions/{session_id}/decline` | Bearer (invited user) | declineSession |
| POST | `/api/v1/sessions/{session_id}/join` | Bearer | joinSession (`{viewer_id, permissions, ice_servers, turn_servers?}`; 409 `session_full` / `capability_missing` without H.264 decode) |
| DELETE | `/api/v1/sessions/{session_id}/viewers/{viewer_id}` | Bearer (owner device or that viewer) | removeSessionViewer |
| POST | `/api/v1/invites/redeem` | none | redeemInvite |
| POST | `/api/v1/setup` | none, any address | setupWithCode (`setup_code_v1`: setup code + admin user name and password + device -> first admin and paired device in one step; 401 `invalid_setup_code`, 410 `setup_code_expired`, 409 `already_set_up`, 429) |
| GET | `/api/v1/local/status` | none, loopback only | getLocalStatus (`local_setup_v1`) |
| POST | `/api/v1/local/setup` | none, loopback only | localSetup (first admin plus pairing; 409 when an admin exists) |
| POST | `/api/v1/local/pair` | none, loopback only | localPair (enabled admin credentials, else 401) |
| PUT | `/api/v1/local/settings` | Bearer (admin), loopback only | updateLocalSettings |
| POST | `/api/v1/games` | Bearer | uploadGame (raw body, 403 `uploads_disabled`) |
| POST | `/api/v1/games/register` | Bearer | registerGame (`{sha256, size, filename, system?, title?, game_code?, game_version?}`; match by sha256, else identity, else a new local-only game; 201 new game, 200 existing; `local_roms_v1`) |
| DELETE | `/api/v1/games/{game_id}/registration` | Bearer | deleteGameRegistration (the caller's local registrations; 204; the game and its saves stay; `local_roms_v1`) |
| POST | `/api/v1/uploads` | Bearer | createUpload (`{filename, size, sha256?, title?, system?}` -> 201 `{upload_id, offset, chunk_size}`; returns the open session of the same user for the same sha256 or file name and size; 409 `existing_game_id` for a stored sha256; same permission as uploadGame; `local_roms_v1`) |
| GET | `/api/v1/uploads/{upload_id}` | Bearer | getUpload (`{upload_id, offset, size}`) |
| PUT | `/api/v1/uploads/{upload_id}` | Bearer | putUploadChunk (raw body, `Content-Range: bytes a-b/total`, `a` = offset else 409, chunk at most 64 MiB; the completing chunk answers `{offset, game}`) |
| DELETE | `/api/v1/uploads/{upload_id}` | Bearer | deleteUpload (abort; 204) |
| GET | `/api/v1/systems` | Bearer | listSystems |
| GET | `/api/v1/systems/{system_id}/firmware/{file_id}` | Bearer | getFirmwareFile (ETag) |
| GET | `/api/v1/cores/{core_id}/packages/{version}/{platform}` | Bearer | getCorePackage |
| GET | `/api/v1/cores/{core_id}/packages/{version}/{platform}/files/{name}` | Bearer | getCorePackageFile (ETag, 304) |
| GET | `/api/v1/plugins` | Bearer | listPlugins (`{plugins:[PluginInfo]}`, every offered version; `plugins_v1`) |
| GET | `/api/v1/plugins/{plugin_id}/{version}/file` | Bearer | getPluginFile (`application/zip`, ETag = SHA-256, 404 unknown; `plugins_v1`) |
| GET | `/api/v1/tools` | Bearer | listTools (`{tools:[Tool]}`, offered tools only, with `versions` and `platforms` that are cached; `tools_v1`) |
| GET | `/api/v1/tools/{tool_id}/packages/{version}/{platform}` | Bearer | getToolPackage (`application/zip`, ETag = SHA-256, 304, 404 when not offered or not cached; `tools_v1`) |
| GET | `/api/v1/variants` | Bearer | listVariants (the caller's own, `include_deleted=1` for kept recipes; `variants_v1`) |
| PATCH | `/api/v1/variants/{variant_id}` | Bearer | renameVariant (`{name}`; 409 `name_taken`) |
| DELETE | `/api/v1/variants/{variant_id}` | Bearer | deleteVariant (`?saves=keep` default or `delete`; 204; 409 `session_running` / `conflict` with `saves=delete`) |
| GET | `/api/v1/games/{game_id}/variants` | Bearer | listGameVariants |
| POST | `/api/v1/games/{game_id}/variants` | Bearer | createVariant (`{name, tool_id, tool_version, settings, seed, base_sha256?, title_id?, update_version?, spoiler_log?}`, 201 `Variant` with `slot` `var-<8 hex>`; 409 `name_taken`) |
| GET | `/api/v1/runs/{run_id}/variant` | Bearer | getRunVariant (joined participants: `{settings, seed, tool_id, tool_version, base_sha256, title_id, slot, kind}`; 403 not joined, 409 no randomizer) |
| POST | `/api/v1/runs/{run_id}/variant-state` | Bearer | setRunVariantState (`{state: not_built\|building\|ready}`, the caller's own; pushes `run_updated`) |
| GET | `/api/v1/runs` | Bearer | listRuns (runs the caller participates in, not archived; `runs_v1`) |
| POST | `/api/v1/runs` | Bearer | createRun (`{name, plugins, game_id, invite_user_ids, settings}`, 201 Run, creator joined, status `starting`; 413 `too_large`) |
| GET | `/api/v1/runs/{run_id}` | Bearer | getRun (with every participant's per-plugin `states`; 404 for non-participants) |
| POST | `/api/v1/runs/{run_id}/join` | Bearer | joinRun (`{game_id, variant?}`, `variant {kind: run\|own, variant_id?}` in a randomizer run, `variants_v1`) |
| POST | `/api/v1/runs/{run_id}/invite` | Bearer | inviteRun (`{user_ids}`, any joined participant; 409 above 8 participants) |
| POST | `/api/v1/runs/{run_id}/decline` | Bearer | declineRun |
| POST | `/api/v1/runs/{run_id}/leave` | Bearer | leaveRun |
| POST | `/api/v1/runs/{run_id}/status` | Bearer | setRunStatus (`{status}`; 403 wrong user, 409 `conflict` for other transitions) |
| PUT | `/api/v1/runs/{run_id}/state/{plugin_id}` | Bearer | putRunState (`{base_rev, state}`, own state of one plugin of the run, 404 for a plugin not in the run, 409 `state_conflict` with the current `state_rev` and `state` of that plugin, 409 `run_not_active`, 413 `too_large`, 429 `rate_limited`) |
| POST | `/api/v1/runs/{run_id}/events` | Bearer | appendRunEvent (`{client_id, plugin_id, source, type, text, payload}`, 201 `{seq}`, 200 for a repeated `client_id`) |
| GET | `/api/v1/runs/{run_id}/events` | Bearer | listRunEvents (`after`, `limit` 1 to 500, default 200; `{events, last_seq}`) |
| PUT | `/api/v1/runs/{run_id}/boards/{plugin_id}` | Bearer | putRunBoard (`{tree}`, at most 64 KiB, `{rev}`) |
| POST | `/api/v1/auth/sign-in` | none | signIn (`{username, password, device_id, ...}`; a new device signs in with user name and password instead of admin approval; 401 `invalid_credentials`, 429 `rate_limited` with `retry_after`) |
| GET | `/api/v1/account` | Bearer | getAccount (`accounts_v1`) |
| PATCH | `/api/v1/account` | Bearer | updateAccount |
| PUT | `/api/v1/account/password` | Bearer | setAccountPassword (min 12 characters; forced change after an admin reset) |
| DELETE | `/api/v1/account/password` | Bearer | removeAccountPassword (403 `password_required` for admins and under the policy `require_passwords`) |
| POST | `/api/v1/account/confirm` | Bearer | confirmAccount ("Confirm it's you": password check, valid 120 / 300 / 900 s for this user and device; 3 wrong passwords lock the device for 5 min) |
| DELETE | `/api/v1/account/confirm` | Bearer | endAccountConfirmation |
| GET | `/api/v1/account/devices` | Bearer | listAccountDevices (own devices) |
| DELETE | `/api/v1/account/devices/{device_id}` | Bearer | signOutAccountDevice (signs one own device out) |
| POST | `/api/v1/account/devices/sign-out-all` | Bearer | signOutAllAccountDevices |
| POST | `/api/v1/admin/users/{user_id}/password-reset` | Bearer (admin, confirmed) | resetAdminUserPassword (temporary password, the user must change it) |
| GET | `/api/v1/admin/accounts/settings` | Bearer (admin) | getAdminAccountSettings (`require_passwords`) |
| PUT | `/api/v1/admin/accounts/settings` | Bearer (admin, confirmed) | updateAdminAccountSettings |
| GET | `/api/v1/admin/tools` | Bearer (admin) | listAdminTools (every version and platform, `source` state, `latest_version`) |
| PUT | `/api/v1/admin/tools/{tool_id}` | Bearer (admin) | updateAdminTool (`{offered}`) |
| POST | `/api/v1/admin/tools/{tool_id}/sync` | Bearer (admin) | syncAdminTool (reads the release list, downloads nothing; 400 when the source is `none`) |
| POST | `/api/v1/admin/tools/{tool_id}/versions/{version}/install` | Bearer (admin) | installAdminToolVersion (downloads every platform, pins the SHA-256) |
| DELETE | `/api/v1/admin/tools/{tool_id}/versions/{version}` | Bearer (admin) | removeAdminToolVersion (409 `tool_in_use` with `used_by`) |
| GET | `/api/v1/ws` | Bearer | connectWebSocket (WSS upgrade, documentation only) |

Phase 3 (OpenAPI 1.1.0, handshake feature `saves_v1`, `protocol_version` stays 1). Error codes added: `save_conflict` (409), `save_conflict_stale` (409), `payload_too_large` (413). Rules: ADR 0005.

Phase 4 (OpenAPI 1.2.0, handshake feature `sessions_v1`, `protocol_version` stays 1). Error codes added: `session_not_found` (404), `session_forbidden` (403), `session_full` (409), `session_ended` (410); `capability_missing` is returned as 409. Rules: ADR 0006.

Phase 5 (OpenAPI 1.3.0, handshake features `users_v1`, `uploads_v1`, `firmware_v1`, `protocol_version` stays 1). Added: `redeemInvite` (`POST /api/v1/invites/redeem`, no auth), `uploadGame` (`POST /api/v1/games`, raw body, 4 GiB limit), `listSystems` (`GET /api/v1/systems`), `getFirmwareFile` (`GET /api/v1/systems/{system_id}/firmware/{file_id}`). Error codes added: `invite_invalid` (404), `display_name_taken` (409), `user_disabled` (401), `uploads_disabled` (403); duplicate uploads reuse `conflict` (409, with `existing_game_id`). `core_missing` / `core_version_mismatch` are handshake warnings (`compatible` stays true). A disabled owner's Sessions end with the existing `session_ended` reason `owner_disconnected` (no schema change).

### Security code and setup code

Both are shown to people, so they use the Crockford base32 alphabet `0123456789ABCDEFGHJKMNPQRSTVWXYZ` (no I, L, O, U). The app accepts spaces, hyphens or none and any case when a person types or pastes them; comparison is done on the normalized form (remove spaces and hyphens, upper case).

**Security code** (`K7Q4-9WMP-2HXD`): proves that the server a person set up or joined is the one they see on the server's own screen or terminal (`uzume-server code`, the setup banner, `uzume-server status`). It is derived from the leaf certificate the server serves, so the app computes it from the certificate it pinned, with no extra field on the wire:

1. `digest` = SHA-256 of the **DER encoding of the leaf certificate** (the first certificate of the TLS handshake; the same bytes the pinned SHA-256 fingerprint is computed from).
2. Take the first **60 bits** of `digest` (the first 7 bytes and the high nibble of the 8th), most significant bit first.
3. Split them into 12 groups of 5 bits; each group is an index 0 to 31 into the alphabet above.
4. Write the 12 characters in three groups of four joined by hyphens: `XXXX-XXXX-XXXX`.

In code (this is `tlsutil.SecurityCode` in `server/internal/tlsutil/securitycode.go`): for `i` in 0..11, take the 16-bit big-endian window `digest[5i/8]<<8 | digest[5i/8+1]` and use the 5 bits starting at bit `5i mod 8` from its top, i.e. `(window >> (11 - 5i mod 8)) & 31`. Test vectors (input is the DER bytes; the unit test `TestSecurityCodeVectors` checks them):

| DER bytes | Security code |
|---|---|
| ASCII `uzume test certificate` | `QX7A-7H6Y-J729` |
| empty | `WERC-8GMR-ZGE1` |
| the bytes `00 01 02 ... 1F` (32 bytes) | `CC6W-TAB6-RGSP` |

It changes whenever the certificate changes (`uzume-server renew-cert`, `reset`, an expired self-generated certificate that was renewed). An app that pinned a server and sees a different code must not connect until the person compared again.

**Setup code** (`7H4K-2Q`): six characters, 30 bits from the system random generator. A server without an admin holds exactly one, valid 60 minutes and usable once; it is created at every server start while there is no admin (a restart replaces it) and by `uzume-server setup-code`. `POST /api/v1/setup` (`setup_code_v1`) takes `setup_code`, `username`, `password` (at least 12 characters), the device fields of `localSetup` and an optional `install_id`, and answers like an approved pairing poll (`status: approved`, `hub_id`, `user_id`, `device_credential`, delivered once). The device gets the sign-in kind `password`. Order of checks and errors:

| Situation | Answer |
|---|---|
| Field problem (user name rules, password shorter than 12, device fields) | 400 `bad_request`; the code is not used up |
| More than 10 requests a minute from one IP (IPv6 per /64), like sign-in | 429 `rate_limited` with `retry_after` |
| An admin already exists | 409 `already_set_up` (checked before the code) |
| Wrong, replaced, already used or invalidated code | 401 `invalid_setup_code`; each wrong try counts and the 10th deletes the code |
| Right code older than 60 minutes | 410 `setup_code_expired`; the code is deleted |
| Right code | 200; the code is consumed together with the admin in one transaction, so of two parallel requests exactly one wins and the other gets 409 `already_set_up` |

An app should check `setup_required` in `getHubInfo` before it asks for the code (`false` means the server is set up already: offer to join it), and compare the security code before it sends the setup code over a connection to a self-generated certificate.


### Join link

`uzume://join/<host[:port]>/<invite>#<fingerprint>` opens the app on "Join a server" with the invite and the server's certificate pinned in one step.

- `host[:port]`: the server address (the default port 8443 is added when missing; a scheme is dropped).
- `invite`: the invite code, normalized to `UZ-XXXX-XXXX` (`UZ` plus 8 Crockford characters; spaces, hyphens and case are ignored).
- `fingerprint`: the SHA-256 fingerprint of the server's leaf certificate, 64 hex digits written without colons in a generated link; the app also accepts colons and any case and normalizes to upper case with colons.

Example: `uzume://join/server.example.com:8443/UZ-7H4K-2Q9W#<64 hex digits>`. The app connects, compares the fingerprint of the certificate the server presents with the one in the link and shows the security code. When they differ it stops with a hard stop and sends nothing (no invite, no password). A link that is incomplete or malformed asks the person to paste the invite code instead. There is no web landing page for the link ([ADR 0030](../adr/0030-one-app-one-server-uzume.md) D2). The app takes the link from the command line, from a second start of the app (which hands it to the running one) or from Ctrl+V on the start screen.

### WSS messages (`/api/v1/ws`)

Envelope `{type, id?, payload}` (JSON text frames); schema `schemas/ws-<type>.schema.json`, example `schemas/examples/ws-<type>.example.json`. The first client message must be `hello`. The server pings every 20 s.

| Type | Direction | Purpose |
|---|---|---|
| `hello` | App -> server | `{protocol_version, device_id, runs?}`, first message; optional `runs: [{run_id, last_seq}]` (feature `runs_v1`) is the last event seq the app saw per run |
| `hello_ack` | Server -> app | `{protocol_version, hub_version, features, ice_servers, turn_servers?}` (`turn_servers` only while a relay provider is on, feature `turn_v1`; `ice_servers` defaults to `stun:stun.cloudflare.com:3478`, empty with `-ice-servers none`) |
| `ice_update` | Server -> app | `{ice_servers, turn_servers?}` same shape as the ICE part of `hello_ack`; sent to every connected app when the server's effective ICE configuration changes (STUN list, embedded TURN on/off or public host, refreshed Cloudflare TURN credentials; feature `ice_update_v1`); replaces the old lists for PeerConnections created afterwards, running connections are not touched |
| `presence_update` | both | App: `{state: online\|in_game, game_id?, run_id?}` (`run_id` only for a run game; the server counts play time from it, see 0.19 "Play time and collections" below); server: plus `user_id`, `device_id`, state may be `offline` |

0.19 "Storage" (package 7, OpenAPI 1.38.0, `protocol_version` stays 1, no new feature flag). Added (admin only): `GET /admin/storage` (disk and backup disk, size and file count per kind: games, saves, snapshots, states, screenshots, cores, firmware and, with a backup folder, backups; screenshot usage per user and game, largest first, at most 20; the three clean-up suggestions with what each would free) and `POST /admin/storage/cleanup` with `{"id"}` (`old_snapshots`, `upload_leftovers` or `unused_cores`; answers what was freed; 400 for an unknown id). Clean-up never offers saves: `old_snapshots` removes only automatic history versions older than 90 days (never manual snapshots, never anything of a slot with an open conflict, never a current checkpoint), `upload_leftovers` upload sessions idle for more than 7 days, `unused_cores` core package versions that no installed core of any system uses.

0.21 "Randomizer" (package 1, OpenAPI 1.40.0, migration 0025, `protocol_version` stays 1, features `tools_v1` and `variants_v1`). Added: the tools catalog (`GET /tools`, `GET /tools/{tool_id}/packages/{version}/{platform}` with ETag and 304; platforms `windows-x64`, `linux-x64`, `linux-arm64`; the admin side `GET /admin/tools`, `PUT /admin/tools/{tool_id}` `{offered}`, `POST .../sync`, `POST .../versions/{version}/install`, `DELETE .../versions/{version}`), game variants (`GET /variants`, `GET/POST /games/{game_id}/variants`, `PATCH/DELETE /variants/{variant_id}`; `Variant` carries `slot` `var-<8 hex>`, the recipe fields, `run_id` and `deleted_at`; only the owner sees any variant) and runs with a recipe: optional `randomizer {mode: off|each|same, preset_id, settings, tool_id, tool_version, allow_own}` in `POST /runs` (the server adds a run seed that is never delivered and pins `base_sha256` or the 3DS `title_id`), `randomizer {mode, preset_id, tool_id, tool_version, allow_own, base_sha256, title_id}` without settings and seed in the run object, optional `variant {kind: run|own, variant_id}` in `POST /runs/{run_id}/join`, `GET /runs/{run_id}/variant`, `POST /runs/{run_id}/variant-state`, and `variant_kind` (`run|own`) and `variant_state` (`not_built|building|ready`) per participant. Error codes added: `name_taken` (409), `tool_in_use` (409, with `used_by {variants, runs}`), `tool_hash_mismatch` (409), `own_variants_off` (409), `own_variant_mismatch` (409). Server flag `-tools-source-url` / `UZUME_TOOLS_SOURCE_URL`, command `uzume-server import-tools`.

0.20 "States and run rules" (package 2, OpenAPI 1.39.0, `protocol_version` stays 1, features `states_v1` and `run_rules_v1`). Added: `GET /games/{game_id}/states` (optional `?slot=`; with `used_bytes` and `quota_bytes`), `GET/PUT/DELETE /games/{game_id}/states/{slot}/{n}` (n 0 to 10, 0 = quick slot) and `GET .../preview`; the PUT is multipart with the parts `meta` (`core_id`, `core_version`), `state` (up to 64 MiB) and optionally `preview` (PNG up to 512 KiB); 413 `state_quota_exceeded` above the per-user quota, 413 `payload_too_large`, 400 `invalid_image`. `rules` on the run (`states`, `rewind`, `cheats` and `memory_write` as `off` or `logged`; defaults all off), optional `rules` in `POST /runs`, `PUT /runs/{run_id}/rules` (whole object; creator or admin, 403 for other participants, 404 for strangers, 409 for a finished run; writes the run event `rules.changed` with `{before, after, by_user_id, by_user_name}` and pushes `run_updated` and `run_event`). `plugin_id` is optional in `POST /runs/{run_id}/events` for the app's own events `cheat.enabled`, `cheat.disabled`, `memory.write` and `state.loaded`. Optional header `X-Uzume-Cheats-Used` on `PUT .../saves/{slot}`; `cheats_used` in `current` and in history versions. Snapshot labels up to 120 characters (was 64). Admin: `state_quota_bytes` in `/admin/settings` and the clean-up id `deleted_game_states` in `/admin/storage`.

0.20 "Plugin store and cheat database" (package 3, OpenAPI 1.41.0, `protocol_version` stays 1, features `plugin_store_v1` and `cheat_db_v1`). Added: the plugin user store `GET /plugins/{plugin_id}/store` (keys with `revision` and `size_bytes`, `used_bytes`, `quota_bytes`) and `GET/PUT/DELETE /plugins/{plugin_id}/store/{key}` (key `^[a-z0-9_.-]{1,64}$`; the value is any JSON value of up to 256 KiB; the PUT body is `{base_revision, value}` with `base_revision` 0 for a new key; a stale base is 409 `store_conflict` with the server's `revision` and `value`, revision 0 and no value when the key is gone; DELETE takes an optional `?base_revision=`; 413 `payload_too_large` for a value above 256 KiB, 413 `store_quota_exceeded` above 16 MiB or 1024 keys per person and plugin). `GET /games/{game_id}/cheats` (`{source, updated_at, matched_by, list_name, cheats: [{name, code, format_guess}]}`, `format_guess` one of `gsa`, `cb`, `ar_gba`, `ar_ds`, `game_genie`, `gateway`, `raw`; an unmatched game is 200 with an empty list). Admin: `GET /admin/cheat-db` (source URLs `url` and `url_3ds`, size, lists, DATs, last check, last success, last error), `PUT /admin/cheat-db` (`{url, url_3ds}`, each optional, https or `none`; `url_3ds` is the optional source of 3DS Gateway lists; needs "Confirm it's you") and `POST /admin/cheat-db/refresh` (202; 400 while off). The route `GET /plugins/{plugin_id}/{version}/file` is unchanged; a store key named `file` is read through the store route.

0.20 ".uzplugin" (OpenAPI 1.43.1, `protocol_version` stays 1, no new feature flag). Changed: descriptions only; plugin files are `.uzplugin` (legacy `.fbplugin` uploads are still accepted, the bytes are the same format).

0.20 "Cheat database answer" (final check, OpenAPI 1.42.0, `protocol_version` stays 1, no new feature flag). Changed: `GET /games/{game_id}/cheats` has the new `source` value `unavailable` (the cheat database is on but holds no lists yet: not loaded, or the last update failed); `none` now only means the database is off.

0.19 "Screenshots" (package 5, OpenAPI 1.37.0, `protocol_version` stays 1, no new feature flag). Added: `POST /games/{game_id}/screenshots` (raw `image/png`, at most 16 MiB, optional `run_id` that the caller joined with this game; 400 `invalid_image`, 413 `payload_too_large`), `GET /games/{game_id}/screenshots` (the caller's own, newest first, with `total_bytes`), `GET /runs/{run_id}/screenshots` (all members' screenshots tagged with the run), `GET /screenshots/{screenshot_id}/content` (owner or run member) and `DELETE /screenshots/{screenshot_id}` (owner). Deleting a run keeps the screenshots untagged; removing a user or a game removes their screenshots; backups hold them as the optional group `screenshots`.
| `session_update` | Server -> app | `{session}` created/changed, personalised, to every device that may see the Session |
| `save_updated` | Server -> app | `{game_id, slot, revision, sha256, device_id, device_name, reason}` a slot's checkpoint changed; to the user's other connected devices (feature `saves_v2`); `reason` is `checkpoint`, `final`, `final_session_end`, `restore`, `upload` or `conflict_resolution`; `device_id` is the nil UUID for changes made by an admin through the admin API (before 0.18: the server web admin); since `saves_v5` also `renamed` (slot renamed: `slot` is the new name, extra field `previous_slot` the old one, checkpoint unchanged) and `deleted` (slot deleted: the other fields are those of the deleted checkpoint) |
| `session_ended` | Server -> app | `{session_id, reason}` |
| `session_invite` | Server -> app | `{session}` for the invited user's devices |
| `viewer_joined` | Server -> owner device | `{session_id, viewer_id, display_name, device_name, turn_servers?}` (`turn_servers`: fresh relay credentials for the owner while TURN is on) |
| `viewer_left` | Server -> owner and viewer device | `{session_id, viewer_id, reason: left\|removed\|revoked\|disconnected}` |
| `signal` | both | `{session_id, viewer_id, kind: offer\|answer\|candidate, sdp?, candidate?, mid?}` relayed unchanged between owner device and the authorized viewer's device |
| `run_updated` | Server -> app | `{run}` full run incl. the participants' per-plugin `states`, to every participant (all statuses) after create, invite, join, decline, leave, status change and in the catch-up (feature `runs_v1`) |
| `run_state` | Server -> app | `{run_id, user_id, plugin_id, state_rev, state}` to the other joined participants and the writer's other devices |
| `run_event` | Server -> app | `{run_id, event}` to every joined participant's connections; the author's own device dedups by `client_id` |
| `error` | Server -> app | `{code, message}`, `id` echoes the request |

0.2 "Cores from the Hub" (OpenAPI 1.4.0, handshake feature `cores_v1`, `protocol_version` stays 1). Added: `getCorePackage`, `getCorePackageFile` (ETag = SHA-256, `If-None-Match` 304), optional nullable `SystemInfo.core_package_version`. Error codes added: `core_package_not_found` (404), `core_file_not_available` (404).

0.4 "Internet sessions and save comfort" (OpenAPI 1.5.0, ADR 0012, `protocol_version` stays 1). Added: handshake features `turn_v1`, `saves_v2`, `cores_index_v1` (retired in 0.8); optional `turn_servers` (`TurnServer`) in `hello_ack` and `SessionJoinResponse`; `restoreSaveHistoryVersion`, `createSaveSnapshot`, optional nullable `SaveHistoryVersion.label`, `SaveSyncReason` `restore`, `SaveHistoryReason` `before_restore`; `getCoresIndex`, `getCoresIndexSignature`; WSS message `save_updated`.

0.7.x "Hub saves: new slot and deleting a snapshot" (OpenAPI 1.6.0, `protocol_version` stays 1). Added: handshake feature `saves_v3`; `deleteSaveSnapshot` (`DELETE .../history/{version}`, 204; only `manual_snapshot`, else 409 `save_not_snapshot`; 404 when missing; no `save_updated`). Slot creation from the server web interface has no API.

0.7.x "Handshake user" (OpenAPI 1.7.0, `protocol_version` stays 1). Added: optional `user` (`HandshakeUser`: `id`, `display_name`, `role` admin|user) in `HandshakeResponse`, the server user the authenticated device belongs to.

The release feed of the updaters is specified in [update-index.md](update-index.md).

0.7.x "Upload a save file" (OpenAPI 1.8.0, `protocol_version` stays 1). Added: handshake feature `saves_v4`; `uploadSaveFile` (`POST .../saves/{slot}/upload`): deliberate replacement of a slot's checkpoint with a file from outside the sync flow (for example a `.sav` from another emulator); it never creates a conflict, a stale `X-Uzume-Expected-Revision` returns 409 `save_conflict_stale`, an existing checkpoint is secured in the history first; `SaveSyncReason` `upload`, `SaveHistoryReason` `before_upload`; `save_updated` `reason` `upload`. Admins upload through the app (Saves tab › All users).

0.8 "Cores from the libretro buildbot" (OpenAPI 1.9.0, ADR 0020, `protocol_version` stays 1). Added: handshake feature `cores_v2`; optional `default_core_id` and `cores` (`SystemCore`: `core_id`, `display_name`, `version`, `license`, `experimental`, `required_hw_api`, `origin`, optional `build_date`) in `SystemInfo`; optional `origin` in `CorePackage`; core ids relaxed to `^[a-z0-9][a-z0-9_-]{0,63}$`. `preferred_core_id`, `expected_core_version` and `core_package_version` keep describing the default core for apps without `cores_v2`. Removed: `cores_index_v1`, `getCoresIndex`, `getCoresIndexSignature`. Packages from the buildbot carry no license file.

0.9 "One installer" (OpenAPI 1.10.0, ADR 0021, `protocol_version` stays 1). Added: handshake feature `local_setup_v1`; loopback-only endpoints (other callers get 403, rate-limited like pairing): `GET /api/v1/local/status` (no auth: `admin_exists`, `network_sharing`, `import_dir`, `hub_version`), `POST /api/v1/local/setup` (no auth; creates the first admin and pairs the calling app in one step, 409 when an admin exists), `POST /api/v1/local/pair` (no auth; pairs as an enabled admin with username and password, 401 otherwise), `PUT /api/v1/local/settings` (device token of an admin; `network_sharing`, `import_dir`; 400 `import_dir_unreadable`). Setup and pair answer with the credential payload of the normal pairing poll.

0.10.x "Connections" (OpenAPI 1.11.0, `protocol_version` stays 1). Added: handshake feature `ice_update_v1` and the WSS message `ice_update`; `turn_servers` can come from the embedded TURN server and/or an optional Cloudflare TURN provider (`turn_v1` is announced while any relay provider is on); the default STUN list is `stun:stun.cloudflare.com:3478` (`-ice-servers none` switches it off).

0.10.x "Rename and delete save slots" (OpenAPI 1.12.0, `protocol_version` stays 1). Added: handshake feature `saves_v5`; `renameSaveSlot` (`POST .../saves/{slot}/rename`, body `{name}`, 200 SaveSlot) and `deleteSaveSlot` (`DELETE .../saves/{slot}?confirm_revision=N`, 204); error codes `save_slot_exists` (now in the `ErrorCode` enum, was already sent by the server), `save_slot_protected` and `session_running`; `save_updated` reasons `renamed` and `deleted` plus the optional payload field `previous_slot`; `uploadSaveFile` returns 409 `session_running` while an active Session of the calling user runs the game. The `default` slot can be neither renamed nor deleted. Rename and delete are also refused with `session_running` during a Session of the game. Deleting is a hard delete (no undo) that needs the matching `confirm_revision`, so a stale client cannot remove newer progress; the server logs it.

0.10.x "External TURN servers" (OpenAPI 1.13.0, `protocol_version` stays 1). Changed: `TurnServer.expires_at` is optional. The server sends it for every short-lived credential (embedded TURN, Cloudflare, external TURN in shared-secret mode) and omits it for the static username and password of an external TURN server. `turn_servers` (hello_ack, join response, `viewer_joined`, `ice_update`) may now hold entries of several providers at once. An app that ignores `expires_at` (the Uzume does) needs no change.

0.11 "Hub plugins" (OpenAPI 1.14.0, ADR 0023, `protocol_version` stays 1). Added: handshake feature `plugins_v1`; `listPlugins` (`GET /api/v1/plugins`, `PluginInfo`) and `getPluginFile` (`GET /api/v1/plugins/{plugin_id}/{version}/file`, ETag = SHA-256). `PluginInfo.origin` is `official` or `unverified`.

0.11 "Hub runs" (OpenAPI 1.15.0, ADR 0023, `protocol_version` stays 1). Added: handshake feature `runs_v1`; the run endpoints above (tag `runs`); optional `runs` in the WSS `hello`; WSS messages `run_updated`, `run_state`, `run_event` (schemas `ws-run-updated`, `ws-run-state`, `ws-run-event`, shared `ws-run`, `ws-run-event-entry`). Error codes added: `run_not_active` (409), `state_conflict` (409), `too_large` (413), `rate_limited` (429). Run catch-up after `hello`: `run_updated` for every non-archived run of the user, then `run_event`s with `seq > last_seq` for the listed runs, at most 500 per run (more missing: none sent, the app pages with `GET .../events`). Limits: [12-plugins.md](../architecture/12-plugins.md#1611-hub-runs).

0.11 "Per-plugin run state and game codes" (OpenAPI 1.16.0, ADR 0023, `protocol_version` stays 1, no new feature; amends 1.15.0, which only dev builds had). Changed: `PUT /runs/{run_id}/state` is replaced by `PUT /runs/{run_id}/state/{plugin_id}` (one state and revision per participant and plugin; 404 for a plugin not in the run); the Run participant loses `state`, `state_rev`, `state_updated_at` and gets `states` (plugin id -> `{state, state_rev, updated_at}`); the WSS `run_state` payload gains `plugin_id`. Added: `game_code` and `game_code_kind` (`''`, `nds-gamecode`, `3ds-titleid`) on `Game`, read by the server from the ROM header.

0.12 "Local ROMs" (OpenAPI 1.17.0, `protocol_version` stays 1). Added: handshake feature `local_roms_v1`; `registerGame`, `deleteGameRegistration`, `createUpload`, `getUpload`, `putUploadChunk`, `deleteUpload` (tag `library`); `RomInfo.stored`; `Game.on_hub`, `Game.roms` (all known files, primary first) and `Game.game_version` (NDS header byte 0x1E, 3DS NCCH version; absent when unknown). `Game.rom` is a stored file if the game has one, else the primary file. A game is one identity: (system, `game_code`, `game_version`) with a non-empty code; other dumps of it are further files of the same game (saves stay on one `game_id`). Visibility: `GET /games` and `GET /games/{id}` return games with a file stored on the server plus local-only games the caller registered a file of; `GET /roms/{sha256}` serves stored files only (404 otherwise). Upload sessions keep the SHA-256 state, expire after 7 idle days and finish on the last chunk (hash check, then stored file of the game with that sha256, extra file of the identity match, or a new game). Details: `docs/architecture/` (library) and the 0.12 plan.

0.13 "Game Boy and GBA systems" (OpenAPI 1.18.0, `protocol_version` stays 1, no new feature). Changed: `Game.game_code_kind` also has `gba-gamecode` (4-character code at 0xAC) and `gb-title` (header title at 0x134, only with a valid header checksum) for the systems `gb`, `gbc` and `gba`; `game_version` is the GBA byte 0xBC or the Game Boy byte 0x14C. Same rules as the app (`client/emulation/game_identity.cpp`).

0.13 "Plugin run settings" (OpenAPI 1.19.0, ADR 0023, `protocol_version` stays 1, no new feature). Added: optional `run_settings` (schema `PluginRunSetting`, `PluginRunSettingOption`) and `run_settings_strings` (language -> i18n key -> text, only the keys the schema references) on `PluginInfo` in the plugin list; uploads and index imports with an invalid `run_settings` are refused like other manifest errors. Rules: [plugin-api.md](../guides/plugin-api.md#run-settings).

0.15.x "Delete any save version and the main slot" (OpenAPI 1.20.0, `protocol_version` stays 1). Added: handshake feature `saves_v6`. Changed: `deleteSaveSnapshot` is now `deleteSaveVersion` and deletes any history version (409 `conflict` for the secured version of an open conflict; `save_not_snapshot` is no longer returned); `deleteSaveSlot` also accepts `default` (rename of `default` stays 409 `save_slot_protected`). Older servers without `saves_v6` keep the old limits.

0.15.x "Run visibility for non-joined participants" (OpenAPI 1.21.0, `protocol_version` stays 1, no new feature). Changed: `listRunEvents` returns 403 `forbidden` for invited, declined and left participants (404 for non-participants as before); `listRuns`, `getRun`, the membership answers and the WSS `run_updated` carry empty `states` for every participant when the caller has not joined (title, game and participant list stay visible). Joined participants are unaffected. The server also rejects plugin manifests the app rejects (unsupported or unknown permissions, invalid `requires` ranges, `ui` without permission `ui`), so such plugins are no longer offered.

0.15.x "Protocol clean-up" (OpenAPI 1.22.0, `protocol_version` stays 1, no new feature). Changed: a JSON body above the 1 MiB cap answers 413 `payload_too_large` (was 400 `bad_request`) and every operation with a JSON body lists 413; `getUpload`, `putUploadChunk` and `deleteUpload` document 403 `uploads_disabled`; `POST /handshake` answers 400 when an identity field is missing (`platform`, `arch`, `player_version`, `protocol_version`, `min_protocol_version`); a missing `cores` or `video` is treated as not reported (before, a missing `video` was stored as no H.264, which made Sessions refuse the device) and `audio`/`input` are ignored, so older installed apps can still reach the server and its update; `hello_ack.features` stays the short WSS list, now documented as non-authoritative (the handshake response is), and contains `turn_v1` whenever `turn_servers` is sent.

0.17 "Report a problem" (OpenAPI 1.25.0, `protocol_version` stays 1, handshake feature `reports_v1`, ADR 0029 D3). Added: `createReport` (`POST /api/v1/reports`, any paired device, `multipart/form-data` with the JSON part `meta` first, then `files`; 201 `{id}`; 400 `bad_request`, 413 `payload_too_large` above 25 MiB or 12 files, 429 `rate_limited` above 5 per device and hour); admin only: `listAdminReports` (`GET /api/v1/admin/reports?status=new|seen`), `getAdminReport`, `deleteAdminReport` (`/api/v1/admin/reports/{id}`), `markAdminReportSeen` (`POST .../seen`), `getAdminReportFile` (`GET .../files/{name}`) and `getAdminReportArchive` (`GET .../archive`, `application/zip`). Reports are kept until an admin deletes them.

0.17 "Update rollback" (OpenAPI 1.26.0, `protocol_version` stays 1, no new feature, ADR 0029 D4). Added, admin only: `getAdminUpdateVersions` (`GET /api/v1/admin/updates/versions`: earlier versions from the signed index, pause state), `rollbackAdminUpdate` (`POST /api/v1/admin/updates/rollback`; 409 when the version is refused or needs `confirm_data_loss`) and `resumeAdminUpdates` (`POST /api/v1/admin/updates/resume`). The update index gains the optional entry fields `schema_version` (server) and `msi_downgrade` (app MSI); the plugin manifest gains the optional `min_player`.

0.18 "Rename to Uzume" (OpenAPI 1.26.1, `protocol_version` stays 1, no new feature, [ADR 0030](../adr/0030-one-app-one-server-uzume.md)). Changed: the file is `protocol/openapi/uzume.yaml` with the title "Uzume server API", texts say server and app, the JSON schema `$id` host is `uzume.invalid`, the invite example is `UZ-...`. Unchanged: all message names and JSON fields and the error codes (`player_too_old`, `hub_too_old`). New invite codes look like `UZ-XXXX-XXXX`.

0.18 "Rename, round 2" (OpenAPI 1.27.0, `protocol_version` stays 1, no new feature, [ADR 0030](../adr/0030-one-app-one-server-uzume.md)). Changed on the wire, without compatibility: the info path is `/.well-known/uzume`, the headers are `X-Uzume-Base-Revision`, `X-Uzume-Content-SHA256`, `X-Uzume-Sync-Reason`, `X-Uzume-Save-Revision` and `X-Uzume-Expected-Revision`, the core origin value is `legacy-mirror`. Only `UZ-` invite codes are accepted. An app and a server of the two naming generations do not work together.

0.18 "Admin API" (package 5, OpenAPI 1.28.0, `protocol_version` stays 1, handshake feature `admin_v1`, only advertised to admins, [ADR 0030](../adr/0030-one-app-one-server-uzume.md) D3). Added: everything the web admin does, under `/api/v1/admin/...`. All endpoints are admin only (401 without a token, 403 for members). The web admin and the API call the same server functions. Groups: `users` (incl. `{user_id}/code`, `role`, `disable`, `enable`), `invites`, `pairing-requests` (`allow`, `deny`), `devices` (`revoke`), `systems/{system_id}` with `cores/{core_id}` (`install`, `update`) and `firmware/{file_id}` (`pin`), `cores/sync` and `cores/settings`, `plugins` (`stage`, `stage/{stage_id}` with `offer`, remove by `{plugin_id}/{version}`), `plugin-index/refresh` and `plugin-index/install`, `library/rescan`, `games/{game_id}` (rename, delete), saves of all users (`saves` and `users/{user_id}/games/{game_id}/saves/{slot}/...`: history, content, restore, snapshots, conflict resolve, slots, rename, upload; `users/{user_id}/saves/export`: all saves and history of a person as one zip with `saves.json`, read-only, offered before removing the person), `runs` (pause, resume, archive, unarchive, finish, resend, start, invites, export), `settings` (`network/{key}`, `network/restart`, `external-turn`, `external-turn/reset`), `updates/check` and `updates/install`, `security/renew-cert` and `diagnostics/bundle`. Process-level settings (network, TURN, certificate, restart) go through `server/internal/adminhost.Host`, which the web server implemented while the web admin existed. The web admin was removed in package 12 (0.18); the API is the only admin interface. Not included: passwords and accounts (package 4) and appearance (web theme only).

0.18 "Server accounts" (package 4, OpenAPI 1.29.0, tag `account`, `protocol_version` stays 1, handshake feature `accounts_v1`, [ADR 0030](../adr/0030-one-app-one-server-uzume.md) D5). Added: the sign-in, account and admin account endpoints above. `redeemInvite` and the pairing request take an optional `install_id` (one install of the app on one PC; several users on it get one device row each with its own device id and the shared `install_id`); `redeemInvite` takes an optional `password`. Error codes added: `invalid_credentials` (401, generic, also for unknown names), `confirmation_required` (403), `password_required` (403), `password_not_set` (409); `rate_limited` (429) answers may carry `retry_after` (seconds). Dangerous admin steps (backup restore and delete, update rollback, password reset, accounts settings) answer 403 `confirmation_required` until the caller confirmed with `POST /api/v1/account/confirm`. Guarded operations (403 `confirmation_required` without a valid confirmation): `deleteAdminGame`, `disableAdminUser`, `deleteAdminUser`, `resetAdminUserPassword`, `deleteAdminSaveSlot` and `deleteAdminSaveVersion` and `restoreAdminSaveVersion` (only for another person's saves), `restoreAdminBackup`, `deleteAdminBackup`, `installAdminUpdate`, `rollbackAdminUpdate`, `setAdminNetSetting`, `resetAdminNetSetting`, `saveAdminExternalTurn`, `deleteAdminExternalTurn`, `resetAdminExternalTurn`, `updateAdminAccountSettings`. Limits: 10 sign-in attempts per minute per IP; 15 min lock of a user name after 10 failures (unknown names count too). The cookie login of the web admin was removed in package 12.

0.18 "Server commands" (package 6, OpenAPI 1.30.0, `protocol_version` stays 1, handshake feature `setup_code_v1`, [ADR 0030](../adr/0030-one-app-one-server-uzume.md) D4). Added: `setupWithCode` (`POST /api/v1/setup`, no auth, any address), `setup_required` in `getHubInfo`, the error codes `invalid_setup_code` (401), `setup_code_expired` (410) and `already_set_up` (409), and the security code derivation above. The server commands themselves (`setup-code`, `code`, `reset-admin`, `status`, `update`, `backup`, `restore`, `reset`) are not part of the wire protocol: [server-configuration.md](../guides/server-configuration.md#server-commands).

0.18 "Runs without plugins" (package 9, OpenAPI 1.31.0, `protocol_version` stays 1, handshake feature `runs_v2`, [ADR 0030](../adr/0030-one-app-one-server-uzume.md) D7). Added: `POST /runs` accepts 0 to 8 plugins, the run object has `save_slot` ("run-" plus the first 8 hex characters of the run id). `runs_v2` is announced where `runs_v1` is (handshake, `hello_ack`). The web Runs page is always visible.

0.18 "Saves export" (OpenAPI 1.32.0, `protocol_version` stays 1, no new feature, `admin_v1`). Added, admin only: `exportAdminUserSaves` (`GET /api/v1/admin/users/{user_id}/saves/export`, `application/zip`, streamed; per game and slot `current.sav` and `history/v<version>-<label>.sav`, plus `saves.json`; read-only, so no "Confirm it's you"; 404 for an unknown user). The app offers it in the Remove dialog.

0.18 "On this PC and sharing" (package 10, OpenAPI 1.33.0, `protocol_version` stays 1, [ADR 0030](../adr/0030-one-app-one-server-uzume.md) D6). Added: `child` (optional boolean) in the local status, true while the server runs as the app's child process (`uzume-server -child`); then `network_sharing` is always false, and `PUT /local/settings` with `network_sharing: true` answers 409 with the new error code `child_mode` (the stored value is not changed).

0.18 "Remove the web admin" (package 12, OpenAPI 1.34.0, `protocol_version` stays 1, [ADR 0030](../adr/0030-one-app-one-server-uzume.md) D2). Descriptions only: no endpoint, field or feature flag changed. The server no longer serves web pages; `/`, `/login` and `/static/...` answer 404.

0.19 "Health problems" (OpenAPI 1.35.0, `protocol_version` stays 1, no new feature, `admin_v1`). Removed: `action_href` from the admin status problems (it held dead web paths; the app maps `action_label` itself).

0.19 "Play time and collections" (package 4, OpenAPI 1.36.0, `protocol_version` stays 1, no new feature flag). Added: `personal` (`play_seconds`, `sessions`, `last_played_at`, `favorite`, `collection_ids`) in `GET /games` and `/games/{id}`; `PUT`/`DELETE /games/{game_id}/favorite`; collections under `/api/v1/collections` (name 1 to 40 characters, unique per user ignoring case, 409 on a duplicate); `play_seconds`, `sessions`, `last_played_at` in `/account` and the admin users; per-participant `play_seconds`/`last_played_at` and a total in runs; optional `run_id` in the app's `presence_update`. The server counts play time from `presence_update` `in_game` (`game_id`, optional `run_id`): a play closes on a state, game or run change, disconnect, replaced connection and shutdown; plays under 10 s are dropped; `run_id` only counts if the user joined that run and plays the run's game. See [features](../features.md).
