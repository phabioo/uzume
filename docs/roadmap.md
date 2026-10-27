# Roadmap

Status and plan of Uzume after the proof of concept: what is finished, what comes next up to 1.0 and what is deliberately left for later. Work is cut into one work package per thread and PR (see [workflow](workflow.md)). 0.20 to 0.24 are built in parallel and all ship as betas 0.20.N; when all features and fixes are in, one release PR raises `VERSION` to 0.21.0, the first stable release ([ADR 0016](adr/0016-release-numbering-and-promotion.md), amendment 2026-10-27). 0.20 and 0.24 are finished (their leftovers and Fabio's tests are below), 0.21 is built (real-engine test and 3DS updates open), 0.22 and 0.23 are open. Finished milestones and the decisions behind the old numbering are in [history](history.md); what the product does today is in [features](features.md).

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
| 0.20 | Emulation: save states, rewind, run-ahead, `memory.write`, snapshots, cheats, plugin store | [ADR 0023](adr/0023-plugins.md), [cheats design](design/uzume/cheats.md) |
| 0.24 | Platforms: Linux AppImage, Secret Service, Vulkan | [ADR 0034](adr/0034-linux-app-and-vulkan.md) |
| 0.21 to 0.23, 1.0 | Planned, see below | |

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

## 0.24 leftovers

- Vulkan has no GPU-direct Session encode; frames for the Session are read back.
- The debug MCP `settings_get` does not list emulation settings.
- The AppImage is unsigned and needs glibc 2.38 or newer.

Tests only Fabio can do at the end: Azahar with Vulkan on a real GPU (and the Renderer row switching it); OpenGL unchanged on NVIDIA, AMD and Intel; the AppImage on a desktop (X11 and Wayland); audio through PipeWire and PulseAudio; gamepads; the Steam Deck in desktop mode and as a non-Steam game in game mode; GNOME Keyring and KWallet (sign-in survives a restart); the AppImage self-update and its restore of `.previous`; "Only on this PC" with the bundled server on Linux.

## 0.20 leftovers

- Real-core checks are open: save-state serialization (size, time, determinism) on melonDS DS, DeSmuME and Azahar, run-ahead on a real core, and `retro_cheat_set` per format, including the Azahar Gateway engine from the `phabioo/azahar` fork. CI covers all of it only with the test core.
- The Pokémon editor's ORAS offsets and encodings (plugin `uzume-plugin-pokemon-editor` v0.1.0 with `uzume-plugin-soullink-core` v0.5.0) are unverified on real hardware.
- Run-ahead is off for 3DS by design; rewind and states on Azahar depend on the real-core check above.

Tests only Fabio can do at the end: save states, rewind and run-ahead in a real game on Windows (hotkeys, preview images, Undo after a load, frame pacing); cheats from the database in a DS, GBA and 3DS game, and switching `rules.cheats` to off in a running run; a plugin memory write with the consent, the "Before memory changes" copy and the run log; "Before gym N" from SoulLink; Windows hardware checks of the new Game rules and Cheat database screens.

## Open milestones

Planned, in this order. Each section is the work list; check an item off here and, when the milestone is finished, move the section to [history](history.md) and add a row above.

| Version | Theme |
|---|---|
| [0.21](#021-randomizer) | Randomizer: game variants, tools catalog, plugin, runs with their own randomized game |
| [0.22](#022-plugins) | Plugins: developer console, run recap, Discord Rich Presence, tracker plugins |
| [0.23](#023-together) | Together: online status, notifications, ready check, chat, viewer role, audio mixer, voice chat, controller hand-over |
| [1.0](#10-test-phase) | Test phase and the SoulLink run |

## 0.21 Randomizer

Decided by Fabio on 2026-10-23 (as 0.22; renumbered on 2026-10-23); [ADR 0031](adr/0031-game-variants-and-randomizer.md) accepted on 2026-10-27; design sheet 5, [randomizer.md](design/uzume/randomizer.md); behaviour in [features](features.md#game-variants-and-the-randomizer). Built (2026-10-27): server (OpenAPI 1.40.0, migration 0025), app core, app UI, plugin `uzume-plugin-randomizer` v0.1, with the test engine `uzume_test_engine` in CI. Open: 3DS updates and Fabio's test with the real engine on his PC (determinism per system, LayeredFS in a slot); the milestone moves to [history](history.md) when both are done. Friends want a SoulLink or Nuzlocke run where each person plays their own randomized game. The engine is the Universal Pokémon Randomizer FVX (GPL-3.0, Gen 1 to 7, never in a repository); each app builds its own variant from a recipe, so no ROM data travels. Gen 1 to 5 are built as a whole ROM, Gen 6 and 7 as a LayeredFS folder in the Azahar dir of the slot.

- [ ] Fabio's test on his PC with the real engine: server › Tools gets FVX, ORAS is randomized twice with the same seed and `variant_files` gives the same hashes, the variant starts with the mod loaded and Main is original, a run "Each person gets their own" with two accounts, SoulLink sees randomized encounters; one game per system (GB, GBC, GBA, NDS) for determinism (S)
- [x] Game variants: variant = base game + recipe + owner + own save slot `var-<8 hex>`; REST, OpenAPI and migration; start exactly the variant; variant cache with limit and Storage entry; LayeredFS into the slot's Azahar dir; `fb.rom.*` reads the started file and lays mod files over the RomFS; variant count and slot picker (M-L)
- [ ] 3DS updates on the server and in the app, so a variant can be built for the update the app plays (10b-3d, 10d-4f/g); decided after Fabio's ORAS test, 0.21 builds and plays without an update (M)
- [x] Tools catalog: the server downloads the engine per platform like cores (SHA-256 pin, `import-tools`, versions kept while in use, Server › Tools); the app starts it with a fixed argument list, no shell, progress, cancel, time and memory limits; permission `variants.build`, no `tool.exec` (M)
- [x] Plugin `uzume-plugin-randomizer` v0.1 in its own repository: presets as settings strings, supported games and the overview card; Uzume draws the dialogs, the variant list and the run setting itself (ADR 0031 amendment) (M)
- [x] Runs with variants: run seed and pinned engine version, per-person seed derived from the run seed and the user ID or "Same world for everyone", own variants if allowed, "Building your variant", run board column Game and Randomizer block (M)
- [ ] SoulLink: show the variant in the run, read encounter data from the variant, test with ORAS (S, plugin repositories)

## 0.22 Plugins

Decided by Fabio on 2026-10-21 (as 0.21; renumbered on 2026-10-22 and 2026-10-23). The UI parts are drawn in the Uzume design (developer console 4d-1/4d-2, run recap 4jh-app-2); no own design round. The plugin UI language moved forward to 0.17.

- [ ] Developer console: live log with filter, reload, memory viewer and search, inspect state (M)
- [ ] Run recap: timeline from the run event log, recap page per run in the app, export as image or HTML (M, server and SoulLink)
- [ ] Discord Rich Presence as a local app option, switch in My account (S)
- [ ] Tracker plugins (speedrun splits and autosplitter, randomizer tracker) in their own plugin repositories (M per plugin)

## 0.23 Together

Decided by Fabio on 2026-10-21 (as 0.20; renumbered on 2026-10-22 and 2026-10-23). The UI is drawn in the Uzume design (speaking mark, mixer popover, People and Chat tabs, notifications); no own design round. Dependency chains: online status -> notifications -> ready check (same presence over WSS); chat -> audio mixer stage 2 -> voice chat; viewer role -> controller hand-over. Spike first for voice chat (echo and noise without libwebrtc). Decisions: [ADR 0033](adr/0033-together.md).

- [ ] Online status: who is online and what they play, "Watch" and "Invite" from the list (S)
- [ ] Notifications (app toasts and bell): Session started, invite, run event; switchable per kind (S-M)
- [ ] Ready check and shared pause for runs and Sessions (S-M)
- [ ] Session and run chat with emotes and pings; history on the Hub for runs (M)
- [ ] Viewer role cleanly separated; quality per viewer (selection and latency display) (M)
- [ ] Audio mixer stage 2: channels game, tiles and plugin sounds; mixing in Multiview optional (M)
- [ ] Voice chat: spike first, then microphone, push-to-talk, volume per person, ducking (L)
- [ ] Controller hand-over: a viewer controls P2 or takes over P1, the host confirms (M-L)

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
