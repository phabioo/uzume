# Roadmap

Direction of Uzume after the proof of concept: the internal goal, what is finished, what is deliberately left for later. **Open work and tests are not listed here:** they are GitHub issues in the milestones "Uzume 21" and "Uzume 22" ([ADR 0036](adr/0036-issues-milestones-wiki-and-numbering.md), [workflow](workflow.md#issues-and-milestones)). 0.20 to 0.24 were built in parallel and ship as betas 0.20.N; the first stable release is the line "Uzume 21" ([ADR 0016](adr/0016-release-numbering-and-promotion.md), amendment 2026-10-27, numbering in ADR 0036). Finished work and the decisions behind the old numbering are in [history](history.md); what the product does today is in [features](features.md).

## Internal goal

Decided by Fabio on 2026-10-11, restated on 2026-10-28: a group of friends completes a SoulLink run in Pokémon Omega Ruby/Alpha Sapphire (3DS) over Uzume. This is an internal goal only: it is neither a milestone nor a version number ("1.0" is dropped as a version, ADR 0036). Its steps are issues (test evening, the run itself).

- Platforms: Windows and Linux only; macOS is dropped. Metadata and artwork come after the goal.
- The plugins live in their own repositories and are independent of Uzume releases; Uzume delivers the plugin runtime. Plugin IDs: `uzume.soullink-core` and `uzume.soullink`.

## Open work

- Milestones (= releases, same title in every repository): "Uzume 21" (Fabio's local tests and the release steps), "Uzume 22" (leftovers and new work packages); list: <https://github.com/phabioo/uzume/milestones>. Cross-repository view: [open issues in milestone "Uzume 21"](https://github.com/issues?q=is%3Aopen+user%3Aphabioo+milestone%3A%22Uzume+21%22).
- Themes are labels: `emulation`, `randomizer`, `plugins`, `together`, `platforms`, `app`, `install-and-sharing`. Process labels: `work-package`, `leftover`, `local-test`, `triage`, `release`.

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
| 0.22 | Plugins: developer console, run recap, Discord Rich Presence, Splits and Tracker plugins, fb.ui `disabled` | [ADR 0023](adr/0023-plugins.md) |
| 0.23 | Together: online status, notifications, ready check, chat, viewer role, mixer, voice chat, controller hand-over | [ADR 0033](adr/0033-together.md) |
| 0.24 | Platforms: Linux AppImage, Secret Service, Vulkan | [ADR 0034](adr/0034-linux-app-and-vulkan.md) |
| 0.21 | Randomizer: game variants, tools catalog, plugin, runs with their own randomized game. Built; open items in milestone Uzume 21/22 | [ADR 0031](adr/0031-game-variants-and-randomizer.md) |

Not all items of the finished milestones are closed; their leftovers are issues labelled `leftover`, and Fabio's tests are issues labelled `local-test`. Older open points are recorded in their [history](history.md) sections.

## After the goal

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

- **Test evening:** date open (issue #315).

Decided questions (second system, internet reachability, update channels, core sourcing, release notes language and others) are in [history](history.md#decisions-from-the-roadmap).
