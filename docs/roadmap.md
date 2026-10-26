# Roadmap

Status and plan of Uzume after the proof of concept: what is finished, what comes next up to 1.0 and what is deliberately left for later. Work is cut into one work package per thread and PR (see [workflow](workflow.md)). Next up: 0.20 (emulation). Finished milestones and the decisions behind the old numbering are in [history](history.md); what the product does today is in [features](features.md).

## Goal 1.0

Decided by Fabio on 2026-10-11: 1.0 is reached when a group of friends completes a SoulLink run in Pokémon Omega Ruby/Alpha Sapphire (3DS) over Uzume. For that, NDS and 3DS run cleanly, and the foundations and templates for further systems are clean, so a new system costs minimal effort.

- Platforms: Windows and Linux only; macOS is dropped. Metadata and artwork come after 1.0.
- The plugins live in their own repositories and are independent of Uzume milestones; Uzume delivers the plugin runtime (0.11). Plugin IDs: `uzume.soullink-core` and `uzume.soullink`.
- Decided on 2026-10-21: milestones 0.16 to 0.24 ship before 1.0. A test evening with friends stays possible at any time (date open).
- Decided by Fabio on 2026-10-23: after 0.19 the plugin milestones come first (0.20 Emulation, 0.21 Randomizer, 0.22 Plugins), then 0.23 Together and 0.24 Platforms.

## Where we stand

"Done" means done in code; real GPUs, drivers, networks, installers and real games are verified only locally by Fabio. Details per milestone: [history](history.md).

| Version | Theme | Decisions |
|---|---|---|
| 0.1.1 | Finish the PoC | [ADR 0009](adr/0009-finish-poc.md) |
| 0.2 | Cores from the server | [ADR 0010](adr/0010-cores-from-the-hub.md) |
| 0.3 | Automatic updates | [ADR 0011](adr/0011-automatic-updates.md) |
| 0.4 | Sessions over the internet, save comfort | [ADR 0012](adr/0012-internet-sessions-and-save-comfort.md) |
| 0.5 | OpenGL hardware rendering | [ADR 0013](adr/0013-opengl-hardware-rendering.md) |
| 0.6 | App UI pass | [ADR 0014](adr/0014-player-ui-pass.md) |
| 0.7 | Server UI pass | [ADR 0015](adr/0015-hub-ui-pass.md) |
| 0.7.x | Revision, tech debt, speed-up, GPU-direct NVENC, hotkeys, background game, playtest revision, admin gaps, hardening, license | [ADR 0017](adr/0017-player-tech-debt.md), [0018](adr/0018-player-speed-up.md), [0019](adr/0019-gpu-direct-nvenc.md) |
| 0.8 | Cores from the libretro buildbot | [ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md) |
| 0.9 | One Windows installer | [ADR 0021](adr/0021-one-windows-installer.md) |
| 0.10 | Second system: Nintendo 3DS with Azahar | [ADR 0022](adr/0022-nintendo-3ds-with-azahar.md) |
| 0.10.x | Connections and small items before new testers | [ADR 0012](adr/0012-internet-sessions-and-save-comfort.md) |
| 0.11 | Plugin runtime | [ADR 0023](adr/0023-plugins.md) |
| 0.12 | Local ROMs | [ADR 0025](adr/0025-local-roms.md) |
| 0.13 | System template, GB/GBC/GBA | [ADR 0024](adr/0024-system-template-and-game-boy.md) |
| 0.14 | UI gaps and Multiview | [ADR 0026](adr/0026-ui-gaps-and-multiview.md) |
| 0.15 | GPU paths | [ADR 0027](adr/0027-gpu-paths.md) |
| 0.16 | Release workflow and changelogs | [ADR 0028](adr/0028-release-notes-and-changelogs.md) |
| 0.17 | Operations and safety | [ADR 0029](adr/0029-operations-and-safety.md) |
| 0.18 | Uzume: one app, one server, new name | [ADR 0030](adr/0030-one-app-one-server-uzume.md) |
| 0.19 | App comfort and library | [ADR 0030](adr/0030-one-app-one-server-uzume.md) D8 |
| 0.19.x | Readable UI pass (design system r6) | [decisions](design/uzume/decisions.md#sheet-6--readable-pass-2026-10-26) |
| 0.19.x | In-game redesign (sheet 7): control bar, Quick menu, Game tab | [decisions](design/uzume/decisions.md#sheet-7--in-game-redesign-2026-10-26) |
| 0.20 to 0.24, 1.0 | Planned, see below | |

Not all items of the finished milestones are closed. Open leftovers are recorded in their [history](history.md) sections, among them: the 0.11 plugin open points (non-admin plugin upload, Azahar memory maps), the 0.16 "What's new" screen and plugin repository PR templates, the 0.10.x library speed with many games.

## 0.18 leftovers

- The plugin index republish (workflow "Publish plugin" with `remove_ids`) is pending; plugins are then signed again with the new IDs.

Tests only Fabio can do at the end: "Play on this PC" on a fresh Windows user (no UAC), "Share with others" (one UAC, data and pairing kept, service port), "Stop sharing" (others cannot connect, accounts and saves stay), "Share again", fresh install of the server and the app on a new PC, sign-in on a second PC and user switching, the GHCR package `uzume-server` set to public, plugin index and plugins signed again with "Publish plugin".

## 0.19 leftovers

- The Continue tile meta shows only the system (no save slot or version).
- The audio output device switch applies only on the next start.
- The video rows of board 6g-1 (Audio & video) are missing.
- Per-game layout, swap and bottom window apply on the next start.
- Screenshot usage per user and game has no own endpoint beyond the storage overview.
- The clean-up "states of deleted games" waits for save states (0.20).

Tests only Fabio can do at the end: audio on Windows (volume, mute, mute in background); screenshots: real capture with GPU cores, Save as dialog on Windows, Select + Y on a gamepad; screen layouts: bottom window on a second monitor, Windows zero-copy with the window open, touch in that window, Select + Right on a pad; per-game overrides: hold-Select hotkey sheet on a pad, per-game pad and keyboard profiles in a real game, the dialog in dark and light; both Storage pages visually and clean-ups on real data; integer scaling and the Continue block visually.

## Open milestones

Planned, in this order. Each section is the work list; check an item off here and, when the milestone is finished, move the section to [history](history.md) and add a row above.

| Version | Theme |
|---|---|
| [0.20](#020-emulation) | Emulation: save states, rewind, run-ahead, `memory.write`, snapshots, cheats |
| [0.21](#021-randomizer) | Randomizer: game variants, tools catalog, plugin, runs with their own randomized game |
| [0.22](#022-plugins) | Plugins: developer console, run recap, Discord Rich Presence, tracker plugins |
| [0.23](#023-together) | Together: online status, notifications, ready check, chat, viewer role, audio mixer, voice chat, controller hand-over |
| [0.24](#024-platforms) | Platforms: Linux app package, Vulkan |
| [1.0](#10-test-phase) | Test phase and the SoulLink run |

## 0.20 Emulation

Decided by Fabio on 2026-10-21 (as 0.19; renumbered on 2026-10-22). Spike first for save states. Dependency chain: save states -> rewind -> run-ahead (all use `retro_serialize`). The UI parts are drawn in the Uzume design; no own design round.

- [ ] Spike: serialization in melonDS DS, DeSmuME, Azahar, mGBA and Gambatte (size, time, determinism), with a note, then cut the packages (S)
- [ ] Save states: slots per game, stored on the server next to saves (own limit), hotkeys, preview image; run rule "states allowed" through `run_settings`, run log event on use (M-L)
- [ ] Rewind: ring buffer, hold a hotkey, memory limit, can be blocked in runs by rule (M)
- [ ] `memory.write` in the plugin API: consent per game, one write queue, snapshot before the first write, run log, run rule, as described in [12-plugins.md](architecture/12-plugins.md) (M)
- [ ] Automatic snapshot: plugin API `fb.save.snapshot(reason)` (usable without `memory.write`); SoulLink triggers it before gym battles. Later the editor plugin builds on `memory.write` (S)

Cheats (decided by Fabio on 2026-10-23). Action Replay and the other formats run in the cores' own cheat engines (`retro_cheat_set`), not in Lua; `memory.write` is only needed for raw codes a core cannot handle. Everything works the same with "Only on this PC" (the app's own child server) and with a shared server. No cheat lists of real games in any repository. Order: core cheats and Azahar fork first, then rules and snapshots together with the save state rules, then the plugin API, then the database, then the plugin `uzume-plugin-cheats` v0.1 in its own repository.

- [x] Design addition in the Uzume canvas: family 11 (11a to 11h) on sheet 4, 11e and 11f linked on sheet 3, spec [design/uzume/cheats.md](design/uzume/cheats.md) (S)
- [ ] Spike: which formats `retro_cheat_set` accepts in melonDS DS, DeSmuME, mGBA and Gambatte (synthetic codes on homebrew or the test core); size and layout of libretro-database `cht/` and `metadat/` (S)
- [ ] Core cheats in the app: load `retro_cheat_reset`/`retro_cheat_set` optionally, re-apply the active list between frames after every change, reset and state load; core profile block `cheats` with the supported formats (a core without it has no cheats, even with the symbol) (S-M)
- [ ] Azahar fork (`phabioo/azahar`): connect `retro_cheat_set` to the existing Gateway engine (`GatewayCheat`), Azahar's own cheat file stays unused; core profile format `gateway` (S)
- [ ] Run rule `rules.cheats` (`off` by default, `logged`) owned by the core, not by a plugin; the run creator sets it at the start and changes it at any time (`PUT /runs/{id}/rules`, pushed live over the run WSS, `rules.changed` in the run log); switching to `off` clears all active cheats at once; run log events `cheat.enabled`/`cheat.disabled` written by the app (S-M)
- [ ] Snapshot "Before cheats" at the first active cheat of a play session; later save versions carry `cheats_used`, shown as a badge in the save history (S)
- [ ] Plugin API: permission `cheats`, `fb.cheats.formats()`, `fb.cheats.apply(list)`, `fb.cheats.clear()`, event `cheats.blocked`; consent per game, snapshot and run log stay in the app (M)
- [ ] UI building blocks: multi-line `textarea` and `fb.ui.pick_file` (the plugin only gets the content of the picked file) (S-M)
- [ ] Plugin user store on the server: `fb.store.get/put` per user and plugin with revisions and a local copy in the app (works offline and with "Only on this PC"); on a conflict both versions are kept (M)
- [ ] Cheat database: the server downloads libretro-database `cht/` and the No-Intro DATs at runtime (`-cheat-db-url`, `none` turns it off; refreshed like the core catalog), offline `import-cheats <zip>`; match by ROM hash -> No-Intro name, 3DS by title ID; `GET /games/{id}/cheats` (M)
- [ ] Run-ahead: 1-2 frames, only for cores with fast serialization, off for 3DS (M)

## 0.21 Randomizer

Decided by Fabio on 2026-10-23 (as 0.22; renumbered on 2026-10-23; [ADR 0031](adr/0031-game-variants-and-randomizer.md), proposed; design sheet 5, [randomizer.md](design/uzume/randomizer.md); assessment in the project notes). Friends want a SoulLink or Nuzlocke run where each person plays their own randomized game. The engine is the Universal Pokémon Randomizer FVX (GPL-3.0, Gen 1 to 7, never in a repository); each app builds its own variant from a recipe, so no ROM data travels. ORAS is tested first, every generation from GB to 3DS is built: Gen 1 to 5 as a whole ROM, Gen 6 and 7 as a LayeredFS folder in the Azahar dir of the slot. Order by dependency; the spike comes first.

- [ ] Spike on Fabio's PC: same base, settings, seed and engine version give the same bytes, for one game per system (GB, GBC, GBA, NDS, 3DS); a LayeredFS folder in the Azahar dir of a run slot loads and leaves Main untouched (S)
- [ ] Game variants: variant = base game + recipe (engine version, settings, seed, base dump SHA-256 or 3DS update) + owner + own save slot; REST, OpenAPI and migration; start exactly the variant (not another dump); variant cache with limit and Storage entry; LayeredFS into the slot's Azahar dir; `fb.rom.*` reads the started file and lays mod files over the RomFS; library row with variant count, slot picker (M-L)
- [ ] 3DS updates on the server and in the app, so a variant can be built for the update the app plays (10b-3d, 10d-4f/g) (M)
- [ ] Tools catalog: the server downloads the engine per platform like cores (SHA-256 pin, offline import, versions kept while in use, Server › Tools); the app starts it with a fixed argument list, no shell, progress, cancel, time and memory limits; build permission for signed official plugins only, no generic `tool.exec` (M)
- [ ] Plugin `uzume-plugin-randomizer` v0.1 in its own repository (Fabio creates it): presets as settings strings, Randomize dialog, variant list, run setting, spoiler log only on request (M)
- [ ] Runs with variants: run seed and pinned engine version and update, per-person seed derived from the run seed and the user ID or "Same world for everyone", own variants if allowed, "Building your variant" before Play run, run board column Game (M)
- [ ] SoulLink: show the variant in the run, read encounter data from the variant, test with ORAS (S, plugin repositories)

## 0.22 Plugins

Decided by Fabio on 2026-10-21 (as 0.21; renumbered on 2026-10-22 and 2026-10-23). The UI parts are drawn in the Uzume design (developer console 4d-1/4d-2, run recap 4jh-app-2); no own design round. The plugin UI language moved forward to 0.17.

- [ ] Developer console: live log with filter, reload, memory viewer and search, inspect state (M)
- [ ] Run recap: timeline from the run event log, recap page per run in the app, export as image or HTML (M, server and SoulLink)
- [ ] Discord Rich Presence as a local app option, switch in My account (S)
- [ ] Tracker plugins (speedrun splits and autosplitter, randomizer tracker) in their own plugin repositories (M per plugin)

## 0.23 Together

Decided by Fabio on 2026-10-21 (as 0.20; renumbered on 2026-10-22 and 2026-10-23). The UI is drawn in the Uzume design (speaking mark, mixer popover, People and Chat tabs, notifications); no own design round. Dependency chains: online status -> notifications -> ready check (same presence over WSS); chat -> audio mixer stage 2 -> voice chat; viewer role -> controller hand-over. Spike first for voice chat (echo and noise without libwebrtc).

- [ ] Online status: who is online and what they play, "Watch" and "Invite" from the list (S)
- [ ] Notifications (app toasts and bell): Session started, invite, run event; switchable per kind (S-M)
- [ ] Ready check and shared pause for runs and Sessions (S-M)
- [ ] Session and run chat with emotes and pings; history on the Hub for runs (M)
- [ ] Viewer role cleanly separated; quality per viewer (selection and latency display) (M)
- [ ] Audio mixer stage 2: channels game, tiles and plugin sounds; mixing in Multiview optional (M)
- [ ] Voice chat: spike first, then microphone, push-to-talk, volume per person, ducking (L)
- [ ] Controller hand-over: a viewer controls P2 or takes over P1, the host confirms (M-L)

## 0.24 Platforms

Decided by Fabio on 2026-10-21 (as 0.22; renumbered on 2026-10-22 and 2026-10-23). May be split into 0.24 and 0.25, one milestone per topic. Spike first for Vulkan (which cores offer the libretro Vulkan path: melonDS DS, Azahar?). Browser watching was dropped on 2026-10-22. The UI follows the Uzume boards ([ADR 0030](adr/0030-one-app-one-server-uzume.md) D8).

- Moved to 0.18: the Hub setup assistant is built as the setup assistant in the app (0.18 package 8).
- [ ] Linux app package (Flatpak or AppImage): Secret Service instead of in-memory credentials, updater and core distribution for Linux (buildbot platform mapping, [ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md)), check gamepads and audio, check the Steam Deck (L)
- Dropped on 2026-10-22 (Fabio): watching a Session in the browser; Uzume keeps no browser components ([ADR 0030](adr/0030-one-app-one-server-uzume.md) D2).
- [ ] Vulkan next to OpenGL: libretro Vulkan hardware interface, display and encode path; spike first, switchable per core option in the Emulation settings (L)

## 1.0 Test phase

- Open: test evening(s) with friends; date not set.
- Open: checklist: installer (MSI), buildbot cores, Azahar, 3DS saves, 3DS system language (Azahar core option "3DS System Language"), internet Sessions.
- Open: then the SoulLink run in Pokémon Omega Ruby/Alpha Sapphire (3DS) with the plugins from their repositories. Completing it is the 1.0 goal.

## After 1.0

### Metadata and artwork

- Hub Metadata Service with provider abstraction, hash matching, overrides, artwork cache (`docs/architecture/11-metadata-future.md`).
- Hub: Settings → Metadata, actions in the library entry. Player: boxart and basic data in Library and game view.

## Later, only on demand

- Netplay, emulation settings sync, StandaloneBackend. (Save states moved to 0.20, controller hand-over to 0.23.)
- Further library features: paging (row actions delete and rename are done in 0.10.x).
- Remaining Nintendo systems from NES to GameCube/Wii (Fabio, 2026-10-07), as plain buildbot cores after 0.10, rendered with OpenGL first: NES, SNES, N64, Virtual Boy (GB, GBC and GBA are scheduled in 0.13) and GameCube/Wii (Dolphin; the libretro port lags upstream). N64 and GameCube/Wii build on the hardware rendering from 0.5.
- Plugin follow-ups (Fabio, 2026-10-11; plugin-specific plans live in the plugin repositories): more Pokémon generations for the official plugins, in the order Gen 7 (Sun/Moon, Ultra Sun/Ultra Moon), Gen 4/5 (NDS), Gen 3 and Gen 1/2 (the last two need the GB, GBC and GBA systems with mGBA from 0.13); a Pokémon editor plugin (needs `memory.write`, which the API describes but the first release does not build; it is now planned in 0.20) and a cheats plugin (built on the core cheats of 0.20); the randomizer plugin moved to 0.21 on 2026-10-23 ([ADR 0031](adr/0031-game-variants-and-randomizer.md)). Gen 8 (Switch) is out of scope: no libretro core.
- Wii U deferred (Fabio, 2026-10-07): Cemu has no libretro core and would need the StandaloneBackend.
- To evaluate (Fabio, 2026-10-07): Cloudflare Tunnel in front of a self-hosted Hub, so no router port forwards are needed. The Hub itself stays self-hosted; running it on Workers/Pages would be a rewrite and is not planned.
  - Cloudflare TURN (Realtime) moved to 0.10.x (Connections) on 2026-10-11.
  - Cloudflare Tunnel (`cloudflared` on the Hub host) for HTTPS, API and WebSocket signaling. Prerequisites: a Player Hub profile mode that validates the certificate by hostname/CA instead of pinning (Cloudflare terminates TLS with rotating certificates), and chunked uploads in the Player (the resumable upload of 0.12 is a start), the Hub web uploader and the Hub API, because proxied requests are limited to 100 MB on the free plan while ROMs may be up to 4 GiB (otherwise large uploads have to happen locally). WebRTC media and TURN never go through the Tunnel (no raw UDP).
  - Check Cloudflare's terms for large binary downloads through the proxy and the plan limits before deciding; record the decision as an ADR.

## Out of scope

Hosted emulation, friends list, public Session links, guest access, email/password recovery, own ACME client, simultaneous multi-Hub use, federation, cross-Hub Sessions, OAuth/central accounts, web client. They stay out until actively wanted. Watching in the browser was dropped on 2026-10-22, and since 0.18 there is no web admin and no other web page either ([ADR 0030](adr/0030-one-app-one-server-uzume.md)).

## Open decisions

- **Test evening:** date open.

Decided questions (second system, internet reachability, update channels, core sourcing, release notes language and others) are in [history](history.md#decisions-from-the-roadmap).
