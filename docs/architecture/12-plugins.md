# Architecture: plugins (runtime, Player UI, Hub plugins, Hub runs and Player runs implemented)

## 16. Plugin interface [0.11: runtime, Player UI, Hub plugins, Hub runs and Player runs implemented]

Status: package 1 (plugin runtime in the Player), package 2 (plugin UI in the Player: building blocks, map view, in-game tab, HUD, Multiview tile strip, screen (a run tab since 0.18), toasts, game detail section, shell context, hotkeys) and package 3 on the Hub side (plugin store and delivery, `.uzplugin` files with `SIGNATURE.json`, signed plugin index, runs with REST and WSS, Hub web pages Plugins and Runs with the JS renderer) plus the Player side of plugin delivery (install from the Hub and from files) are implemented. The Player side of runs (`uz.run` Lua API, Runs screen) is implemented. Plugin API emulation (0.20, built): the permissions `memory.write`, `cheats` and `store` (`uz.memory.write*`, `uz.cheats.*`, `uz.store.*`), `uz.save.snapshot`, the blocks `textarea` and `uz.ui.pick_file`, with consent, safety copy and run rules. Still design only: settings schema, the variant build permission of [ADR 0031](../adr/0031-game-variants-and-randomizer.md) (replaces `tool.exec`). What the runtime implements today is described in [guides/plugin-api.md](../guides/plugin-api.md). Decisions: [ADR 0023](../adr/0023-plugins.md) (proposed). Screens and building-block sheet: [design/legacy/plugins.md](../design/legacy/plugins.md). Where this file names code, it names what is built or today's extension points. Open items are marked **open**.

Since 0.18 package 12 ([ADR 0030](../adr/0030-one-app-one-server-uzume.md) D2) the Hub web pages named in this file (run page, Plugins, Runs, browser source) no longer exist; their content is in the app (Server › Plugins, Runs) and a browser source is not built. 0.18 package 9 ([ADR 0030](../adr/0030-one-app-one-server-uzume.md) D7, "Runs without a plugin"): a run needs no plugin (0 to 8 plugins, handshake feature `runs_v2`, OpenAPI 1.31.0), Runs is always in the app and web navigation, and every run plays in its own save slot `run-<first 8 hex of the run id>` (field `save_slot`, shown as "Run: <name>"). The sidebar slot is gone: the `screen` slot is the plugin's run tab next to the fixed tabs Board and Log, and plugins have no sidebar entries. The plugin API version stays 1. Official plugin ids are `uzume.soullink-core` (was `net.framebeam.pokemon-core`) and `uzume.soullink` (was `net.framebeam.soullink`; one plugin "SoulLink & Nuzlocke" with the run setting `mode`).

Decided defaults of the runtime (package 1): plugins run only with developer mode on (Settings → Plugins); the only install paths are local folders (copy or linked dev folder with live reload), all unsigned; Lua 5.4.7 is vendored in `client/plugins/third_party/`; plugins resolve (`requires`/`provides`) once at game start; the plugin tick is limited to 30 Hz by default (`tickHz`); budgets per entry call are 20M instructions / 500 ms and 64 MiB per state.

Principle: FrameBeam stays a generic emulator Player. Everything game-specific (for example Pokémon SoulLink/Nuzlocke runs) lives in plugins. The Hub manages (plugins, run state), the Player emulates (reads memory, shows UI).

Plugin-specific documentation (rules, per-game adapters, plugin roadmaps) lives in the plugin repositories, not in this repository.

### 16.1 Goals and non-goals

Goals:

- Plugins feel like part of FrameBeam: same components, tokens, light/dark, controller operation, hotkeys and languages.
- Plugins read emulator memory in the device's address space, identical across cores for the same system.
- Several plugins run at once, isolated from each other and from the game.
- Official plugins come from their own repositories; Hub upload and local install work without GitHub.
- Run state is shared through the Hub and stays on the user's own Hub.

Non-goals:

- Plugin-provided QML, native code in plugins, free network access, sending controller input.
- Pokémon (or any game) knowledge in the core code.
- Game data, sprites, maps or any ROM-derived content in any repository.
- Switch / Gen 8 (no libretro core).
- Randomizer plugin: planned for 0.21, without a generic `tool.exec` (see 16.11, [ADR 0031](../adr/0031-game-variants-and-randomizer.md)).
- `memory.write` is built since 0.20 (see [guides/plugin-api.md](../guides/plugin-api.md#game-memory-writes-memorywrite)). `tool.exec` is not built: ADR 0031 replaces it with a narrow build permission.

### 16.2 Extension points in today's code

| Area | Where | Today | Needed |
|---|---|---|---|
| Memory | `client/emulation/libretro_backend.cpp`, `memory_map.h` | Implemented: accepts `RETRO_ENVIRONMENT_SET_MEMORY_MAPS`; descriptors with `select` 0 match by address range; fallback when a core gives no map: `RETRO_MEMORY_SYSTEM_RAM` mapped at address 0. `RETRO_MEMORY_SAVE_RAM` is used as before | Open: whether the buildbot Azahar build exposes the map and covers ORAS data (local check with the Plugin console) |
| Backend interface | `client/emulation/emulator_backend.h` (`EmulatorBackend`) | Implemented: memory map access and watch set (`MemoryWatchSet`, `MemorySnapshot` in `memory_snapshot.h`) | - |
| Threading | `EmulationRunner` | Implemented: merged watched ranges are copied after `runFrame()` every N frames (default every frame; limits 4 MiB per range, 16 MiB per tick); the plugin thread works on the copy | - |
| Game start | `GameStarter::launch` in `client/ui/gamestarter.cpp` | Implemented: `game_identity.h` provides the NDS game code and the 3DS title ID | Open: GB/GBA header code (needs those systems) |
| Saves | keyed by `romSha256` (`gamestarter.cpp`) | A changed ROM (for example externally randomized) gets its own saves | Unchanged; "never overwrite saves silently" stays |
| Hub channel | WSS envelope, `protocol/schemas/ws-envelope.schema.json` (`type` + `payload`) | Implemented (package 3): `run_updated`, `run_state`, `run_event`, optional `runs` in `hello` (feature `runs_v1`) | - |
| Peer channel | DataChannel `fb-diag` | Exists only while someone watches the Session | Not suitable for run state |
| Distribution | Core distribution with SHA-256 pin ([ADR 0020](../adr/0020-cores-from-the-libretro-buildbot.md)); signed `updates-index`, `server/cmd/uzume-sign` | Cores, updates, plugins | Implemented (package 3): `.uzplugin` signature and plugin index reuse the updates-index key and `uzume-sign` (16.10) |
| UI | `client/ui/*.qml`, `Fb*` components, `Theme.qml`; Hub web: Go templates + htmx (`server/internal/web`) | Implemented in the Player (package 2): `uz.ui`, validation in `client/plugins/plugin_ui.*`, `PluginsController`, `PluginBlock.qml`, `PluginMapView.qml`, `PluginImageProvider`; slots panel, HUD, tile strip, screen, game detail, toasts. [API guide](../guides/plugin-api.md#ui) | Implemented in the Hub web UI (package 3): `server/internal/web/static/plugin-blocks.js` renders the same tree read-only |

Core-side facts (3DS): see [ADR 0023](../adr/0023-plugins.md) context (Azahar exposes memory maps with virtual addresses; `retro_get_memory_data` returns NULL; `retro_cheat_set` is empty).

### 16.3 Runtime

- **Lua 5.4**, one Lua state per plugin, on a dedicated plugin thread (never the emulation or UI thread).
- Sandbox: no `io`, `os`, `package`, `load` or `debug`; an instruction budget per call; a step budget inside the pattern matcher (commented patch of the vendored `lstrlib.c`), `setmetatable` without `__gc`, no host addresses in `tostring`/`%p`; a memory limit per state through a custom allocator; an event queue bounded in bytes; 50 ms per host iteration for event handlers across all plugins. File and network access only through the API. The Player shows all plugin- and peer-supplied text as plain text and decodes plugin images scaled and budgeted.
- **Memory snapshots:** a plugin registers address ranges. The emulation thread copies all registered ranges after `runFrame()` every N frames into a buffer; the plugin reads the copy. The emulation can never be slowed by a plugin. If two plugins register the same range, it is copied once per tick.
- **Virtual addresses:** plugins address the device's address space, resolved through the core's memory maps (RetroAchievements-style), so cores of the same system look the same.
- **Pointer following:** data may live on a heap. A plugin reads a pointer from a snapshot, then registers the target range; it is included from the next tick.
- **Native helper functions** called from Lua, generic and testable: decompression (LZ77/LZ11 and similar), containers (NARC, GARC), image decoding (2bpp/4bpp tiles, NCGR/NCLR, BCLIM/ETC1) to PNG, and format helpers such as Pokémon data decryption. Knowledge about individual games stays in the plugin.
- A crash or an exceeded budget pauses only that plugin, with a visible notice; the game and other plugins continue.

### 16.4 Plugin types and trust levels

| Type | Example | May | Source |
|---|---|---|---|
| Script plugin | SoulLink/Nuzlocke, tracker | Read memory, read ROM (for assets), UI, run state; runs in the Lua sandbox | Any source, including Hub upload |
| Tool plugin | Randomizer (0.21) | Additionally ask Uzume to build a game variant with a tool from the server's tools catalog; the plugin never starts a program itself ([ADR 0031](../adr/0031-game-variants-and-randomizer.md) D5) | Signed official plugins only |

### 16.5 Permissions

| Permission | Meaning | Status |
|---|---|---|
| `memory.read` | Read registered ranges | first release |
| `rom.read` | Read the user's own game files at runtime (assets) | first release |
| `ui` | Provide building-block trees for slots, including notices (toasts) outside the game; no separate notice permission | first release |
| `run.state` | Read/write per-player run state, append run events | first release |
| `video.capture` | Capture a screen or region of the game picture as PNG at native resolution (`uz.video`); on demand through the existing readback, no write to disk, 1 per second, local game only | API 1 addition |
| `memory.write` | Write memory through a write queue | built (0.20) |
| `cheats` | Hand cheat codes to the core's own cheat engine (`retro_cheat_set`); consent, snapshot and run log as for `memory.write` | built (0.20); in a run game the plugin starts only while `rules.cheats` is `logged` (`memory.write`: `rules.memory_write`), see [guides/plugin-api.md](../guides/plugin-api.md#when-plugins-run) |
| `store` | Small private data per person and plugin (`uz.store`), a local copy synced with the server (`plugin_store_v1`), conflicts keep both versions | built (0.20) |
| `sound` | Play short 16-bit PCM WAV sounds of the plugin package into the plugin sound bus of the audio mixer (`uz.sound.play` / `stop_all`, 10 s and 2 MiB per file, 8 voices per plugin; [ADR 0033](../adr/0033-together.md) D6) | built (0.23) |
| `tool.exec` | Start an external program | not built; replaced by the variant build permission of [ADR 0031](../adr/0031-game-variants-and-randomizer.md) D5 (name open), signed official only, planned for 0.21 |

`memory.write`: all plugins' writes go through one queue applied on the emulation thread between two frames in a fixed order. If two plugins persistently set the same address, the Player shows the conflict instead of silently letting one win. Before the first write in a play session the Player takes a save snapshot (the Hub already versions saves). The consent ("Change game memory") is asked once per game. In a run, every change is written to the run log that all participants see ("cheat used" event), and a run rule can forbid memory writes; the plugin then stays off in that run.

Not planned: sending input, free network access.

The Player shows name, origin (Official / Unverified / Local) and permissions before a plugin is switched on. In-game surfaces (tab, HUD widget, tile strip, toast) show a small "Unverified" or "Local" marker on hover or focus for non-official plugins; Official shows none.

### 16.6 Manifest (sketch)

```json
{
  "id": "net.uzume.example",
  "name": "Example",
  "version": "1.0.0",
  "api": 1,
  "type": "script",
  "requires": { "net.uzume.example-core": "^1.0" },
  "provides": ["example.team"],
  "games": [{ "system": "nds", "codes": ["ABC*"] }],
  "permissions": ["memory.read", "rom.read", "ui", "run.state"],
  "entry": "main.lua",
  "assets": ["icons/", "i18n/"],
  "run_settings": [ { "key": "mode", "type": "boolean", "label": "rs.mode", "default": true } ],
  "settings": { }
}
```

`run_settings` (implemented) declares the rules form of the "Start a run" dialog; the values travel in the run's `settings[<plugin_id>]` ([guide](../guides/plugin-api.md#run-settings)). `api` versions the interface; a plugin with an unknown API version is rejected with a clear message instead of being half-loaded. `settings` holds a schema from which the settings page is generated. **Open:** the exact schema, the Lua API and the building-block catalog are fixed in the design handoff and the runtime work package.

### 16.7 Several plugins at once

- Isolation: own state, memory and compute budget per plugin.
- Load order: `requires` first; cycles and missing or incompatible versions are reported on activation.
- Cooperation: `provides` exposes an interface for other plugins; calls between plugins are messages through the Player, not shared Lua memory. Events can be subscribed to by permitted plugins.
- UI: several plugins in one slot get tabs (side panel), are stacked (HUD, tile overlay) or listed separately (run tabs, settings, hotkeys). Hotkey conflicts are shown in the Hotkeys tab: FrameBeam keys win, then the earlier plugin; a losing key is inactive.
- Management: Settings → Plugins lists version, origin, permissions, dependencies and a switch per plugin, per game and per run.

### 16.8 UI

Plugins ship **no QML** (QML can run JavaScript and load native modules, which would void the sandbox and let the look drift). A plugin describes its UI as a tree of declarative building blocks; the Player renders it with its own `Fb*` components, and a small JS renderer in the Hub web UI draws the same tree. On state changes the plugin sends only the changed values.

Building blocks v1 (implemented in the Player; the exact catalog with props and limits is in the [API guide](../guides/plugin-api.md#ui); the list below is the design outline):

- Layout: column, row, grid, tabs, scroll area, card
- Content: text, label, chip/badge, icon, image/sprite, progress/HP bar, table/list
- Interaction: button (calls a plugin action), toggle, select, text field, confirmation dialog
- Map view
- Settings generated from the manifest schema

A new building block needs a Player update; the `api` version keeps that clean. A second tier with signed plugin QML is conceivable only if the building blocks provably do not suffice.

Slots (implemented in the Player; the Hub web run page is built, see 16.11):

| Slot | Where |
|---|---|
| In-game side panel (own tab) | `GameScreen`, next to Saves/Diagnostics |
| In-game overlay (HUD) | over the game picture, can be switched off |
| Multiview tile overlay | on a friend's tile |
| Run tab (`ui.screen`, run view) | Runs screen, after Board and Log; no plugin entries in the sidebar since 0.18 ([ADR 0030](../adr/0030-one-app-one-server-uzume.md) D7) |
| Section in game detail | `DetailPane` |
| Section in run detail (`ui.run`, run view) | Runs screen, between participants and event log; plugins run read-only outside the game ([guide](../guides/plugin-api.md#when-plugins-run)) |
| Settings page | Settings → Plugins |
| Hotkeys | Controllers → Hotkeys |
| Notifications | toasts |
| Hub web: run page, game detail | Hub web UI (also usable as a browser source) |

HUD corners: widgets stack in the bottom left (default), bottom right and top left; the top right is reserved for diagnostics. F4 hides all HUD widgets, Ctrl+H shows or hides one plugin's widget (the keys come later in package 2; the hide actions exist in the Player) (expanding is a click or controller A). A run's `requires` are included automatically when the run is started. Remote Multiview tiles show one plugin strip; the user picks it when several plugins offer one. In-game views stay dark in both themes.

Map view: a background image, markers at coordinates (player symbols in the player's colour, sprites, state), zoom and pan with mouse and controller, selection details and filters. Positions are at location or route level (map ID), not exact overworld position.

### 16.9 Assets

Game artwork (sprites, icons, region maps) belongs to the rights holders. It is **never** in any repository. A plugin extracts it at runtime from the user's own game files with `rom.read` and the native helpers and stores it in a local plugin cache. Repositories hold only facts the project records itself (for example location tables and map coordinates). Falling back to user-supplied asset packs is possible but not the default.

### 16.9a Languages and i18n

Decided 2026-10-11 (Fabio): the group plays German/EU releases first.

- Plugins support all language versions of a game where feasible, at least German and English.
- Names of species, moves, items and locations are read from the game at runtime, so they follow the game's language; plugins do not ship name tables.
- Plugin UI strings come from i18n files (`i18n/`): English is the source, German the first translation.
- Plugin UI language (0.17): the Player setting Settings > Plugins > "Plugin language" is Auto | English | Deutsch (list from `client/plugins/plugin_language.*`). It becomes `GameContext.language` when a game starts, so `uz.game.language` returns the resolved tag and `uz.i18n.t` looks up `i18n/<tag>.json`, then the base language (`de-DE` -> `de`), then `en`. Auto: for 3DS games the explicit Azahar core option `citra_language_value` ("3DS System Language", mapped to a code) if set, otherwise the OS UI language (`QLocale::system()`, for example `de-DE`), otherwise `en`. An explicit choice wins over both. A change applies when the next game starts. The shell context (no game) uses the setting without the 3DS option.
- 3DS: EU 3DS cartridges contain several languages, and the game language follows the emulated console's system language. Azahar offers the core option "3DS System Language" (Citra language setting). FrameBeam must let the user set it (per-game override or a 3DS default); **open:** the exact setting and where it lives. Game plugins such as `pokemon-core` must detect the active language from the game instead of assuming it.

### 16.10 Distribution

Built in package 3 (Hub) and in the Player's delivery part.

**`.uzplugin` file.** A zip (deflate or store) with `manifest.json` at the root (the Player manifest format, [API guide](../guides/plugin-api.md)), the entry file, assets and i18n as in a plugin folder, and an optional `SIGNATURE.json`. Hub (`server/internal/pluginpkg`) and Player (`client/plugins/uzplugin.*`) validate identically:

- compressed file at most 32 MiB, sum of uncompressed sizes at most 64 MiB, at most 2000 entries;
- every name is a relative path with `/` separators: no leading `/`, no `\`, no drive letter, no `.` or `..` segment, no empty segment, at most 255 bytes, no duplicate names (case-insensitive), no symlink entries; directory entries are ignored;
- `manifest.json` parses, `api` is 1, `id`, `version` and `entry` are valid and the entry file exists.

**`SIGNATURE.json`:** `{ "format": 1, "files": { "<path>": "<sha256 hex>" }, "signature": "<base64 Ed25519>" }`. `files` lists exactly every non-directory entry except `SIGNATURE.json` with the lowercase hex SHA-256 of its content. The signed message is the UTF-8 text `uzume-plugin-v1\n` followed by one line `<sha256>  <path>\n` (two spaces) per file, sorted by path in byte order. The key is checked against the same trusted keys as the updates index (Hub: release key plus `-core-trust-key`; Player: the compiled-in release key). A missing signature leaves the plugin unsigned; an invalid one is reported as invalid and never counts as official.

**Origins.** `official`: valid Uzume signature, wherever it was installed from (index, Hub upload, file). `unverified`: unsigned, offered by the Hub. `local`: unsigned file or folder on this device; only `local` plugins need developer mode in the Player.

**Plugin index.** `plugin-index.json` (`format` 1, `generated_at`, `plugins[]` with `id`, `name`, `version`, `api`, `description`, `permissions`, `url`, `sha256`, `size`) plus a detached `plugin-index.json.sig` in the scheme of `updates-index`. `uzume-sign plugin-sign` adds the signature to a `.uzplugin`, `plugin-add` puts a signed file into the index (it refuses unsigned files and a different file for an id+version that is already in the index, unless `-replace`), `sign -index` writes the `.sig` ([publishing guide](../guides/plugin-publishing.md)). Hub flag `-plugin-index-url` / `UZUME_PLUGIN_INDEX_URL` (https or `file://`); the default is the official index, the release `plugin-index` of the Uzume repository (`https://github.com/phabioo/uzume/releases/download/plugin-index/plugin-index.json`, published by the workflow "Publish plugin"); the value `none` disables the index for offline Hubs, which then have no catalog. The index is read at start, every 24 h and with "Check now"; a failure is shown on the Plugins page and never stops the Hub. Install and Update on the Plugins page download the file, check size and SHA-256 against the signed index and the embedded signature, and store it as `official` (source `index`). Official plugins come from their own repositories whose release workflow builds and signs the `.uzplugin`.

**Hub plugin store.** Files live under `<data-dir>/plugins/<id>/<version>.uzplugin`, metadata in the table `plugins` (migration `0012_plugins`). Every stored version is offered to all Players of the Hub:

- `GET /api/v1/plugins` (device token, feature `plugins_v1`): `PluginInfo` per version: `plugin_id`, `version`, `name`, `description`, `api`, `origin`, `sha256`, `size`, `permissions`, `requires`, `provides`, `games`, `created_at`.
- `GET /api/v1/plugins/{plugin_id}/{version}/file`: the zip bytes, `Content-Type: application/zip`, `ETag` = SHA-256; 404 when unknown.
- Admin upload in the Hub web UI (admin only; **open:** whether non-admins may upload): the Hub validates the file and shows name, version, origin and one tick per requested permission; "Offer to Players" works only with every tick set (checked on the server). The same id and version with a different SHA-256 is rejected; remove the old version first.
- Offline: `uzume-server import-plugins <dir>` offers every `*.uzplugin` of a folder (source `import`), like `import-cores`; a Hub without internet works completely with uploads and imports.
- Remove: deletes file and row, refused with 409 while a run that is not `finished` or `abandoned` uses that version. Copies Players already installed stay.

**Player.** "Browse plugins on your Hub" lists the offered plugins; Install downloads the file with the list's SHA-256 as pin (a mismatch installs nothing), validates it as above and stores a Hub copy under `<plugins-root>/hub/<id>/<version>/` with `origin.json`. "Install plugin from file": signed files become `official` without developer mode, unsigned ones `local` (developer mode). The developer mode also reloads a plugin from a folder on change and shows a log console.

**Runs** use exactly the plugin versions the run pins (id, version, SHA-256); a locally installed copy is not enough and never replaces the Hub version a run uses.

Official plugin repositories (created 2026-10-11, no code yet, each with its own docs and roadmap): `uzume-plugin-pokemon-core`, `uzume-plugin-soullink`, `uzume-plugin-monster-editor`, `uzume-plugin-cheats`, `uzume-plugin-template`.

### 16.11 Hub: runs

Hub side built in package 3 (`server/internal/hub/runs.go`, `server/internal/httpapi/runs.go`, migration `0013_runs`; feature `runs_v1`, OpenAPI 1.16.0). Player side built too: `RunClient` (`client/network/runclient.*`), Lua `uz.run` (permission `run.state`, only when the game is started in a run; [plugin-api.md](../guides/plugin-api.md)) and the Runs screen in the Player. Each participant has one state object per plugin of the run, each with its own revision (migration `0013_runs`, table `run_states`); the game picker matches the plugin's `games` patterns against the game code the Hub reads from the ROM header (migration `0014_game_codes`, see below).

**Data model.**

- **Run:** `run_id` (uuid), `name` (1 to 64 characters), `status` (`starting`, `active`, `paused`, `finished`, `abandoned`), `archived` (web only), `created_by`, `created_at`, `updated_at`, `status_by`, `settings` (JSON object up to 16 KiB, opaque to the Hub: the plugin's run rules), `plugins` (1 to 8 of `{plugin_id, version, sha256}`, each an offered plugin version at creation), `last_seq`, `participants`.
- **Participant:** `user_id`, `user_name`, `status` (`invited`, `joined`, `declined`, `left`), `game_id` (a Hub library game, set on join; the creator joins on create), `color` (0 to 7, in join order), `states` (object `plugin_id` -> `{state, state_rev, updated_at}`, only for plugins that have a row; `state` is a JSON object up to 64 KiB per plugin, a missing plugin means `{}` with revision 0, `state_rev` starts at 1 with the first write). At most 8 participants.
- **Event** (append-only): `seq` (per run from 1, gapless), `created_at`, `user_id`, `user_name`, `device_id`, `plugin_id` (the last three null for Hub events), `source` (`automatic`, `manual`, `hub`), `type` (`^[a-z0-9_.-]{1,64}$`), `text` (up to 280 characters), `payload` (JSON object up to 4 KiB), `client_id` (uuid, unique per run, makes appending idempotent). The Hub appends `hub` events itself: `run.created`, `run.invited`, `run.joined`, `run.declined`, `run.left`, `run.status`. At most 50 000 events per run.
- **Board:** per run and plugin a building-block tree (JSON object up to 64 KiB) that a joined participant's Player publishes for the Hub web run page; `rev`, `updated_by`, `updated_at`.

The plugin computes the rules in the Player from the shared state; the Hub validates only shape and size and executes no plugin code. Run data stays on the user's Hub and never goes to another Hub (as for saves).

**Limits.** Per participant token buckets: events 2/s burst 30, state writes 4/s burst 20 (all plugins of a participant together), board writes 1/s burst 5; exceeded: 429 `rate_limited`. `starting` and `active` runs accept events, state and board writes; `paused`, `finished` and `abandoned` refuse them with 409 `run_not_active`. Too large: 413 `too_large`; wrong shape: 400.

**REST** (device token; only participants of any status see a run, others get 404; participants who have not joined (invited, declined, left) see the title, game and participant list only, with empty `states`, and get 403 `forbidden` on `GET .../events`; events and plugin states are for joined participants): `GET /runs` (not archived), `POST /runs` (name, plugins, `game_id`, `invite_user_ids`, `settings`; 201, creator joined, status `starting`), `GET /runs/{run_id}`, `POST .../join` (`game_id`), `.../invite` (any joined participant while the run is not over; more than 8 participants: 409), `.../decline`, `.../leave`, `.../status`, `PUT .../state/{plugin_id}` (`base_rev`, `state`; own state of that plugin only, which must be a plugin of the run, else 404; a different `base_rev` returns 409 `state_conflict` with the current `state_rev` and `state` of that plugin), `POST .../events` (201 `{seq}`, the same `client_id` again 200 with the original `seq`), `GET .../events?after=<seq>&limit=<1..500, default 200>`, `PUT .../boards/{plugin_id}` (joined participant, plugin must be in the run). Status transitions: `active` from `starting` by the creator and from `paused` by any joined participant; `paused` from `active` by any joined participant; `finished` or `abandoned` by the creator; other transitions 409 `conflict`, the right transition by the wrong user 403. Users to invite come from `GET /users`. Details: [protocol reference](../reference/protocol.md).

**WSS.** `run_updated` `{run}` to every participant (all statuses; with `states` for joined participants, without for the others) after create, invite, join, decline, leave and status change; `run_state` `{run_id, user_id, plugin_id, state_rev, state}` to the other joined participants and the writer's other devices; `run_event` `{run_id, event}` to every joined participant's connections. Catch-up: `hello` may carry `runs: [{run_id, last_seq}]`. After `hello_ack` the Hub sends `run_updated` (with states only where the user has joined) for every non-archived run of the user, then for the listed runs the missed `run_event`s with `seq > last_seq`, at most 500 per run; when more are missing it sends none and the Player pages with `GET .../events`. Runs not listed get only `run_updated`.

**Game codes and the game picker.** The Hub reads the identity of a library game from the ROM header (header bytes only, no full read): the NDS 4-character game code at 0x0C (4 printable ASCII bytes), or the 3DS program ID as 16 uppercase hex digits from a `.3ds`/`.cci` (NCSD, first partition's NCCH) or a bare `.cxi` (NCCH), the GBA game code at 0xAC or the Game Boy header title at 0x134 (only with a valid header checksum; 0.13), the same rules as the Player (`game_identity.cpp`). It does this on every ROM upload and folder import, and once at Hub start for games that were never tried (one log line; a game without a readable identity is marked as tried and not read again). ROMs are stored as uploaded, so a compressed or encrypted dump (for example `.zcci`) has no readable header and keeps an empty code. `GET /games` and `GET /games/{game_id}` carry `game_code` and `game_code_kind` (`nds-gamecode`, `3ds-titleid` or empty). The Player's Start and Join dialogs list a game when its system matches one of the plugin's `games[].system` and either the pattern has no `codes` or the `game_code` matches one of the `codes` globs (`*`, `?`, case-insensitive); every plugin of the run must accept the game. A game with an empty `game_code` stays selectable when its system matches (sorted after the matching games, hint "Game not recognized"); a known code that matches no pattern is not listed. At game start the Player still reads the identity from the local file.

**Hub web.** The pages Plugins and Runs are admin-only. Runs: list, detail with status actions (the admin may Start now, Start without the players who have not joined, Pause/Resume, Finish, Archive and Delete any run, and may resend or remove open invitations), participants, the board of each plugin drawn by the JS renderer `static/plugin-blocks.js` (no framework, no external fetch, plugin strings only as text, images only `data:image/png;base64`; interactive blocks disabled, unknown block types shown as "Unsupported block"; a "Paused" banner) and the event log with a JSON export. The Hub board is read-only.

- **Planned for 0.21:** randomizer plugin ([ADR 0031](../adr/0031-game-variants-and-randomizer.md)). A run stores a recipe and a run seed; each participant's app builds its own variant locally with the engine from the server's tools catalog, so no ROM or patch travels (the earlier idea of the run creator uploading patches is rejected there). Variants have their own save slots; 3DS variants are LayeredFS folders in the Azahar dir of the slot, and `uz.rom.*` lays them over the RomFS.

### 16.12 Security and trust model

- The Hub admin is already trusted for cores; plugins add no new kind of trust for official and admin-uploaded plugins.
- The sandbox protects against plugin bugs and third-party plugins: no file system, process or network access, bounded CPU and memory.
- Tool plugins can only be official (signed).
- Plugins never touch saves directly; `memory.write` is preceded by a save snapshot.
- Secrets never enter plugin manifests, state or logs.

### 16.13 Platforms

Lua, the building blocks and the helper functions are platform-neutral and run with the Player on Windows now and on Linux with the Linux Player after 1.0 (roadmap; macOS dropped).

### 16.14 Testing

- Synthetic data only: the in-repo test core `uzume_test_core` gets a synthetic memory map with self-generated data. No ROMs, no RAM dumps of real games in the repository or in cloud CI.
- Hub, protocol and renderer run fully in CI.
- Real games are tested only locally by the maintainer; recordings stay local.
