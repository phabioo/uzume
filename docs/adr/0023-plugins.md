# ADR 0023: Plugins (0.11)

- Status: proposed (D1 and D2 are implemented in 0.11 package 1, D6 in package 2, D8 to D10 on the Hub side and in the Player's plugin delivery in package 3; the Player side of runs is implemented; the rest is design only)
- Superseded in part: plugins get no sidebar entries, their screens become run tabs, and the official plugin IDs become `uzume.*` ([ADR 0030](0030-one-app-one-server-uzume.md) D7).
- Date: 2026-10-11
- Deciders: Fabio (plugins as 0.11, 2026-10-11); proposal by the orchestrator
- Builds on: [ADR 0013](0013-opengl-hardware-rendering.md), [ADR 0020](0020-cores-from-the-libretro-buildbot.md) (SHA-256 pin, buildbot cores), [ADR 0022](0022-nintendo-3ds-with-azahar.md) (3DS with Azahar). Interface description: [12-plugins.md](../architecture/12-plugins.md).

## Context

Fabio's friends use FrameBeam mainly for Pokémon runs (SoulLink, Nuzlocke). Instead of building Pokémon features into the core, FrameBeam gets a plugin interface; all game-specific logic lives in plugins. Requirements decided on 2026-10-11:

- Plugins integrate in UI/UX and function like part of FrameBeam; several run at once.
- Official plugins live in their own GitHub repositories; plugins can also be uploaded to a Hub or installed locally, so GitHub is not required.
- The first plugin is SoulLink/Nuzlocke; the first game is Pokémon Omega Ruby/Alpha Sapphire on the 3DS. Gen 1 to 7 are in scope later, Gen 8 (Switch) is not.
- A map with player symbols and sprites; later a cheats plugin and a separate Pokémon editor plugin. The randomizer plugin is deferred.

Finding (Azahar, checked 2026-10-11 in `src/citra_libretro/citra_libretro.cpp` of azahar-emu/azahar): the libretro core exposes heap, linear heap, New 3DS linear heap and VRAM through `RETRO_ENVIRONMENT_SET_MEMORY_MAPS` with virtual addresses. `retro_get_memory_data` deliberately returns NULL and `retro_cheat_set` is empty. FrameBeam rejects `SET_MEMORY_MAPS` today (`client/emulation/libretro_backend.cpp`, ~line 825) and uses only `RETRO_MEMORY_SAVE_RAM`.

Open: whether the buildbot build of Azahar contains this code, and whether the map built once at game load covers the regions in which the game keeps its data.

## Decisions

### D1 Runtime: Lua 5.4 in a sandbox

- One Lua state per plugin on a dedicated plugin thread. No `io`, `os`, `package`, `load`, `debug`; instruction budget per call; memory limit through a custom allocator. File and network access only through the API.
- Native helper functions (decompression, containers, image decoding, format decryption) are provided by the Player; game knowledge stays in plugins.

### D2 Memory access by virtual address with snapshots

- The libretro backend accepts `SET_MEMORY_MAPS`; plugins address the device's virtual address space. Registered ranges are copied by the emulation thread after `runFrame()` every N frames; plugins read the copy on their own thread, so a plugin can never slow the emulation. Pointer following is supported by registering the target range after reading a pointer.

### D3 Plugin types and trust

- Script plugins (sandboxed) from any source. Tool plugins (`tool.exec`) only if signed official. Permissions: `memory.read`, `rom.read`, `ui`, `run.state`; later `memory.write` and `tool.exec`.

### D4 `memory.write` later, with safeguards

- Writes go through one queue applied between frames in a fixed order; conflicts between plugins are reported, not resolved silently. A save snapshot precedes the first write in a play session. A "cheat used" event goes to the run log; runs decide whether cheats are allowed. The API describes `memory.write`; it is built with the cheats/editor plugins, not in the first release.
- Classic cheat codes via `retro_cheat_set` are not usable on the 3DS (empty in Azahar); the cheats plugin applies codes itself through `memory.write`.

### D5 Several plugins at once, from the start

- Isolation per plugin (state, budget; a failing plugin is paused alone), dependency order through `requires`, cooperation through `provides` and messages (no shared Lua memory), registered ranges copied once, UI slots with tabs or stacking.

### D6 UI: declarative building blocks, no plugin QML

- Plugins describe UI as a tree of building blocks (layout, content, interaction, map view, generated settings). The Player renders them with `Fb*` components; a JS renderer in the Hub web UI draws the same tree. Slots: in-game tab, HUD, Multiview tile overlay, sidebar screen, game detail, Settings → Plugins, hotkeys, toasts, Hub run page.

### D7 Assets only from the user's own game files

- Sprites, icons and maps are extracted at runtime with `rom.read` into a local plugin cache. No game artwork or ROM-derived content in any repository. Repositories carry only self-recorded facts.

### D8 Distribution

- Official plugins: own repositories, signed `.fbplugin` releases (FrameBeam release key), signed plugin index in the style of `updates-index` (tool `framebeam-sign`), SHA-256 pin as for cores. Hub admin upload (marked "Unverified" unless signed, permissions confirmed by the admin) and local install from file with a developer mode. Run participants get the same plugin version from the Hub.
- Implementation (package 3): `.fbplugin` is a zip with an optional `SIGNATURE.json` (SHA-256 per file, Ed25519 signature, same trusted keys as `updates-index`), identical validation in Hub and Player; origins `official`, `unverified`, `local`. Hub store under `<data-dir>/plugins/`, REST `GET /api/v1/plugins` and `.../{plugin_id}/{version}/file` (feature `plugins_v1`), `framebeam-hub import-plugins`, `framebeam-sign plugin-sign` / `plugin-add`. Chosen defaults: the plugin index URL defaults to the official index (release `plugin-index` of the FrameBeam repository, published by the workflow "Publish plugin", decided 2026-10-12); `-plugin-index-url none` turns it off for offline Hubs; upload is admin-only (open point: whether non-admins may upload); the admin must tick every requested permission before "Offer to Players"; the same id and version with a different file is rejected. Details: [12-plugins.md](../architecture/12-plugins.md#1610-distribution).
- Planned official repositories: `framebeam-plugin-pokemon-core`, `-soullink`, `-pokemon-editor`, `-cheats`, `-template`. Plugin-specific docs and roadmaps live there, not in the FrameBeam repository.

### D9 Hub runs

- Run object with participants, per-player, per-plugin state (JSON) and an append-only event log, with size and rate limits. WSS fan-out over the existing envelope, catch-up on `hello`. The Hub validates shape and size and executes no plugin code. Data never leaves the Hub.
- Implementation (package 3, Hub side): runs with status, participants (at most 8), per-player state per plugin of the run (64 KiB each, own revision per plugin, `PUT /runs/{run_id}/state/{plugin_id}`), event log (280-character text, 4 KiB payload, 50 000 events, idempotent by `client_id`), per-plugin boards (64 KiB), token-bucket rate limits, REST under `/api/v1/runs` (feature `runs_v1`), WSS `run_updated`, `run_state`, `run_event` and catch-up through the optional `runs` field of `hello`. The Hub web run page is read-only and admin-only; the JS renderer is `static/plugin-blocks.js`. Implementation (package 3, Player side): `RunClient` (REST, WSS, persisted `hello` catch-up cache, event dedup by `client_id`, state coalesced to 4/s, board 1/s), Lua `fb.run` (permission `run.state`, only in a run) and the Runs screen; "Play run" uses the exact Hub copies of the run's plugin versions and the save slot `run-<first 8 hex of run_id>`. The Hub reads a game code (NDS game code, 3DS program ID) from the ROM header of library games on upload/import and at start, and the game picker matches the plugin's `games` patterns against it (games without a known code stay selectable with a hint). Details: [12-plugins.md](../architecture/12-plugins.md#1611-hub-runs).

### D10 Versioning

- The manifest field `api` versions the interface; unknown versions are rejected with a clear message. Decided with package 3: `protocol_version` stays 1; the additions are advertised as handshake features `plugins_v1` and `runs_v1` (OpenAPI 1.14.0 and 1.15.0, additive).

### D11 Languages and i18n

- The group plays German/EU releases; plugins support all language versions of a game where feasible, at least German and English. Game names (species, moves, locations) are read from the game at runtime and follow its language; plugin UI strings use i18n files (English source, German first translation) (Fabio, 2026-10-11).
- 3DS: EU cartridges hold several languages and the game language follows the emulated console's system language. Azahar offers the core option "3DS System Language"; FrameBeam must let the user set it (per-game override or 3DS default), and `pokemon-core` detects the active language rather than assuming it. Open: where the setting lives.

### Amendment 2026-10-28: switching a run's plugin versions (issue #335)

A run still pins exact plugin versions (id, version, SHA-256); what changes is that the creator of the run or an admin can now switch a pinned plugin to another version server offers while the run is not over (`PUT /api/v1/runs/{run_id}/plugins`, handshake feature `run_plugin_updates_v1`, see [protocol](../reference/protocol.md)). Fabio decided the defaults on 2026-10-28:

1. **Who:** the creator or an admin; other participants see the change in the run log. The creator carries the run, and a switch restarts everyone's plugins.
2. **Upgrade guard:** a new version may declare the manifest field `run_update_from` (SemVer, the lowest pinned version it can take over); an older pin is refused with `plugin_update_incompatible`. Only the plugin author knows whether the run data survives, and absent means any older version.
3. **Downgrade:** only back to the previous version (`previous_version` of the run plugin), once. Free downgrades would run old code on data a newer version migrated.
4. **Requirements:** the resulting set must satisfy every `requires`; the app adds or raises required plugins in the same call, the server refuses otherwise (`plugin_requires_missing`). A required plugin that the run lacks is appended, nothing is ever removed.
5. **Data stays:** run state, boards, settings and events are keyed by plugin id and are not touched; plugins migrate on load (convention in the [plugin guide](../guides/plugin-api.md#run-state)). The recap uses the mapping of the version pinned now, for all events.
6. **Consent and pruning:** a participant agrees again, per run, plugin and version, when the new version asks for permissions the pinned one did not. The server keeps both the pinned and the previous version of an unfinished run (not removable, not pruned) until the run ends.

## Consequences

- Large, staged work (see roadmap 0.11): runtime, UI building blocks, Hub plugins and runs, plugin repositories, first game. Metadata moves to 0.12, Linux and macOS Player to 0.13 (superseded by the roadmap reorders of 2026-10-11 and 2026-10-12: metadata and the Linux Player come after 1.0, macOS is dropped; see `docs/roadmap.md`).
- Update (2026-10-11, Fabio): renumbered. 0.11 is the plugin runtime only (packages 1 to 3); plugin repositories and the first game (old packages 4 and 5) live in their own repositories and are independent of FrameBeam milestones. 0.12 is Local ROMs; Metadata and the Linux Player come after 1.0; macOS is dropped. See [roadmap](../roadmap.md).
- A new building block needs a Player update.
- Plugins add maintenance load per game, language version and generation; the plugin repositories carry it, not the core.
- Real-game behavior (memory map coverage, offsets, encounters) can only be verified locally by the maintainer; tests use synthetic data and the test core.
- The first game depends on the open Azahar points above. If the buildbot build lacks the map or the map misses the game's data, a small Azahar patch (upstream) is needed.
- Open: exact Lua API, building-block catalog and manifest settings schema (design handoff and runtime package); whether a second runtime (WASM) is ever added.

## Alternatives rejected

- **WASM** as the runtime: strongest sandbox, but authors need a toolchain. Possible later as a second runtime with the same API.
- **QJSEngine (JavaScript)**: tied to QML, no real sandbox.
- **Native plugins (DLL/.so)**: no sandbox, per-platform builds.
- **Plugin-provided QML**: can run JavaScript and load native modules, voiding the sandbox, and makes the look drift. A signed-only QML tier stays conceivable if the building blocks prove insufficient.
- **Pokémon features in the core**: contradicts the generic Player; every game would grow the core.
- **Game artwork in plugin repositories**: rights and takedown risk, and against the rule of no ROM-derived content.
- **Using the `fb-diag` DataChannel for run state**: it exists only while someone watches.
- **Patching Azahar first**: not needed on current evidence; revisit only if the open checks fail.
