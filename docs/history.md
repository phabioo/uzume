# History

Finished work, in order: the milestones 0.1.1 to 0.19 as they were planned and built, the PoC phases and the decisions taken along the way. Open work lives in the [roadmap](roadmap.md); what the product does today in [features](features.md); the reasons in the [ADRs](adr/README.md).

Contents: [Renumbering history](#renumbering-history), [PoC phases](#poc-phases), [Finished milestones](#finished-milestones), [Decisions from the roadmap](#decisions-from-the-roadmap).

## Renumbering history

Read this first when an old ADR or doc quotes a milestone number that does not match the roadmap: the numbers changed several times.

The PoC (phases 0-5) is complete. From here on, work is planned as versions 0.1.1 to 0.24 and then 1.0, each cut into one work package per thread/PR (see `docs/workflow.md`). The order was decided by Fabio on 2026-10-06; on 2026-10-07 he moved Sessions over the internet ahead of the UI passes (renumbered 0.4 to 0.6). On 2026-10-07 Fabio also moved OpenGL hardware rendering for the Player (all emulators/cores, not only 3DS) ahead of the UI passes as the new 0.5; the UI passes, 3DS and everything after shifted by one. ADRs written before 2026-10-07 use the old numbering: old 0.5/0.6/0.7/0.8/0.9/0.10 = new 0.6/0.7/0.8/0.9/0.10/0.11. On 2026-10-09 Fabio reordered the planned milestones again: cores come from the libretro buildbot first ([ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md), new 0.8) and one installer for Player, Hub or both follows (new 0.9, which absorbs the Hub-for-Windows milestone). Old (2026-10-07) 0.8 3DS / 0.9 Metadata and artwork / 0.10 Hub for Windows / 0.11 Linux and macOS Player = new 0.10 / 0.11 / 0.9 / 0.12. On 2026-10-11 Fabio added plugins as 0.11; Metadata and Linux/macOS moved to 0.12/0.13. ADRs and docs written before 2026-10-09 may still use the older numbers for these milestones (the "new" numbers above are those of 2026-10-09). On 2026-10-11 Fabio renumbered again and fixed a goal for 1.0 (see "Goal 1.0"): 0.10.x (connections, small items) comes before new testers; 0.11 is the plugin runtime only, 0.12 Local ROMs, 0.13 GPU paths, 0.14 system template with GB/GBC/GBA, 0.15 UI gaps and Multiview, then the 1.0 test phase. Metadata and artwork and the Linux Player come after 1.0; macOS is dropped (Windows and Linux only). The sentence before about Metadata and Linux/macOS as 0.12/0.13 is therefore obsolete. On 2026-10-12 Fabio reordered 0.13 to 0.15: 0.13 system template with GB/GBC/GBA, 0.14 UI gaps and Multiview, 0.15 GPU paths (2026-10-11 numbers 0.13/0.14/0.15 = now 0.15/0.13/0.14); 0.12 Local ROMs stays. On 2026-10-21 Fabio added milestones 0.16 to 0.22 from the feature brainstorming, all before 1.0. On 2026-10-22 Fabio inserted 0.18 "Uzume" (one app, one server, new name, [ADR 0030](adr/0030-one-app-one-server-uzume.md)) directly after 0.17; the waves moved by one (2026-10-21 numbers 0.18/0.19/0.20/0.21/0.22 = now 0.19/0.20/0.21/0.22/0.23). From 0.18 on the product is called Uzume: the FrameBeam Player is the app, the FrameBeam Hub the Uzume server; older entries below keep the old names. On 2026-10-23 Fabio inserted 0.22 "Randomizer" ([ADR 0031](adr/0031-game-variants-and-randomizer.md)); Plugins and Platforms moved by one (2026-10-22 numbers 0.22/0.23 = now 0.23/0.24). On 2026-10-23 Fabio moved the plugin milestones ahead after 0.19: 0.20 Emulation, 0.21 Randomizer, 0.22 Plugins, 0.23 Together, 0.24 Platforms (earlier 2026-10-23 numbers 0.21/0.22/0.23 = now 0.23/0.21/0.22). Items marked open are not decided yet; decisions are recorded as ADRs in `docs/adr/`.

## PoC phases

Phase plan: [workflow.md](workflow.md#phase-plan).

| Phase | Status | Scope |
|---|---|---|
| 0 Foundation | done | Monorepo, CLAUDE.md, architecture, CI (Linux/Windows), build scaffolding, check scripts |
| 1 Protocol and Hub basics | done | OpenAPI `/api/v1`, WSS schemas, Hub with SQLite, admin setup, TLS, pairing, tokens, library, ROM download, web interface |
| 2 Playable vertical slice | done | Player core (profile, pairing, library, ROM cache), melonDS DS via Libretro, minimal Qt UI |
| 3 Saves | done | Save storage, sync, versions, conflict model ([ADR 0005](adr/0005-saves-phase3.md)) |
| 4 Session sharing and multiview | done (tested locally on two Windows PCs) | Presence, signaling, WebRTC, multiview ([ADR 0006](adr/0006-sessions-phase4.md), accepted) |
| 5 Remainder and polish | done (tested locally) | Users and invites, user uploads, systems and firmware, Emulation and Controllers pages, appearance, Windows installer ([ADR 0007](adr/0007-phase5.md), accepted) |

## Finished milestones

Milestone sections in order. Names of the products in old text (FrameBeam Hub, Player) are the historical names of the Uzume server and app.

- [0.1.1 Finish the PoC](#011-finish-the-poc)
- [0.2 Cores from the Hub](#02-cores-from-the-hub)
- [0.3 Automatic updates](#03-automatic-updates)
- [0.4 Sessions over the internet and save comfort](#04-sessions-over-the-internet-and-save-comfort)
- [0.5 OpenGL hardware rendering](#05-opengl-hardware-rendering)
- [0.6 Player UI pass](#06-player-ui-pass)
- [0.7 Hub UI pass](#07-hub-ui-pass)
- [0.7.x Speed-up (fast-forward)](#07x-speed-up-fast-forward)
- [0.7.x Direct GPU display (zero-copy)](#07x-direct-gpu-display-zero-copy)
- [0.7.x GPU-direct hardware encoding (NVENC first)](#07x-gpu-direct-hardware-encoding-nvenc-first)
- [0.7.x Hotkeys and controller profiles](#07x-hotkeys-and-controller-profiles)
- [0.7.x Background game](#07x-background-game)
- [0.7.x UI revision after the playtest](#07x-ui-revision-after-the-playtest)
- [0.7.x Hub admin gaps, identity and save upload](#07x-hub-admin-gaps-identity-and-save-upload)
- [0.7.x Hardening of the internet-facing ports](#07x-hardening-of-the-internet-facing-ports)
- [0.7.x License and legal notices](#07x-license-and-legal-notices)
- [0.7.x Player fixes](#07x-player-fixes)
- [0.8 Cores from the libretro buildbot](#08-cores-from-the-libretro-buildbot)
- [0.9 One installer: Player, Hub or both](#09-one-installer-player-hub-or-both)
- [0.10 Second system: Nintendo 3DS with Azahar](#010-second-system-nintendo-3ds-with-azahar)
- [0.10.x Before new testers](#010x-before-new-testers)
- [0.11 Plugin runtime](#011-plugin-runtime)
- [0.12 Local ROMs](#012-local-roms)
- [0.13 System template and GB/GBC/GBA](#013-system-template-and-gbgbcgba)
- [0.14 UI gaps and Multiview](#014-ui-gaps-and-multiview)
- [0.15 GPU paths](#015-gpu-paths)
- [0.16 Release workflow and changelogs](#016-release-workflow-and-changelogs)
- [0.17 Operations and safety](#017-operations-and-safety)
- [0.18 Uzume: one app, one server, new name](#018-uzume-one-app-one-server-new-name)
- [0.19 Player comfort and library](#019-player-comfort-and-library)
- [0.19.x Readable UI pass](#019x-readable-ui-pass)

### 0.1.1 Finish the PoC

PoC leftovers. Decisions: [ADR 0009](adr/0009-finish-poc.md) (accepted).

- [x] Done: Hardware encoders NVENC/QSV/AMF: the Windows FFmpeg in `client/vcpkg.json` enables `nvcodec`, `qsv` and `amf`; a Windows-only test checks that `h264_nvenc`, `h264_qsv` and `h264_amf` are compiled in. ADR 0006 is corrected. Opening the encoders needs a GPU and is verified only locally by Fabio.
- [x] Done: Multiview picker lists all Sessions in a scrollable list; the "+N more in the Library" pointer is gone.
- [x] Done: Settings → Hubs (switch, remove with confirmation, auto-connect, current Hub marked); switching ends running work and secures saves like the connection screen.
- [x] Done: Certificate renewal and confirmed pin change. The Hub renews its self-generated certificate at startup when expired or expiring within 30 days (`framebeam-hub renew-cert`, Settings badge); the Player shows both fingerprints and re-pins after a two-step confirmation, keeping the credential (`--accept-fingerprint` in the CLI). Own certificate and key are never modified. Verified only locally by Fabio against a real Hub certificate.
- [x] Done: Session encoding and RTP send run on a worker thread (bounded queue, oldest video frame dropped); RTT is reported in diagnostics via the negotiated DataChannel `fb-diag`.
- [x] Done (0.7.x, ADR 0017 D1): A core whose major version differs from the Hub's expected version blocks the launch; other differences only warn. Since 0.8 it applies only where both sides report a semver version.
- [x] Done: Codebase cleanup: system display name and controller labels from the system manifest, phase-named files and tests renamed (migration `0004_phase5.sql` kept), MSVC C4804 fixed, E2E scripts run in the Linux CI job, staticcheck in `make check-hub`, narrower libdatachannel CI cache path. The `PlayerController` split moves to the Player UI pass (0.6).

Completed 2026-10-06: Windows CI runs on `main` pushes to prime its vcpkg binary cache after merges. The release preset uses a release-only dependency triplet. Cache keys distinguish the triplet and MSVC version and cover the manifest, presets and overlay triplets. The first main run with the new triplet is expected to build cold; later runtimes depend on cache hits and runner performance. See [ADR 0008](adr/0008-windows-ci-cache.md).

### 0.2 Cores from the Hub

Decisions: [ADR 0010](adr/0010-cores-from-the-hub.md) (accepted). The source, the signed index and the package origin (D1 to D3, D6) are superseded by [ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md) with 0.8; what is described here is the state as built until then.

Goal: installers ship no emulator cores. melonDS DS leaves the Windows installer and comes from the Hub.

Decision (replaced in 0.8 by ADR 0020): the Hub obtains signed core packages from a fixed trusted source and caches them. There is no admin upload.

- [x] Done: Hub: Core Package Cache (storage per core ID, version, platform/architecture, SHA-256, origin, license text). The Hub downloads signed packages from the trusted source, verifies the signature and caches them. On "Systems & Cores" (replaces the "LATER" placeholder) the admin only selects the version. Download endpoint for Players with ETag.
- [x] Done: Protocol: endpoints for package metadata and download, a handshake feature (for example `cores_v1`); `protocol_version` stays 1.
- [x] Done: Player: core cache `cache/cores/<core-id>/<version>/<platform>/`, download on demand with hash, version and platform checks, `CoreLocator` searches there. Library and Emulation page show "core loading / missing / incompatible".
- [x] Done: Trust: Ed25519-signed index (`framebeam-sign`, `.github/workflows/cores.yml`), checked by the Hub against compiled-in and `--core-trust-key` keys; the Player checks size and SHA-256 (ADR 0010). The key is reused by the updater in 0.3. Retired in 0.8 (ADR 0020): `cores.yml` is deleted and cores are no longer signed; the key and `framebeam-sign` stay for the updates index.
- [x] Done: FrameBeam release key generated by Fabio (secret `FRAMEBEAM_SIGNING_KEY` of the GitHub environment `release`, main only, public key in `corepkg.DefaultTrustedKeys`).
- [x] Done: Package format and index defined (ADR 0010, `docs/reference/protocol.md`). Source: FrameBeam's own GitHub Releases; the Hub needs internet access to github.com or uses `framebeam-hub import-cores <dir>` offline.
- [x] Done: Windows installer and package no longer contain cores.
- Template: the firmware path from phase 5.

### 0.3 Automatic updates

Goal: a change on `main` lands on the test devices (Windows Player, Hub on the Pi) without manual work.

- [x] Done: Release pipeline: `VERSION` file, version and channel computed by CI; `.github/workflows/release.yml` publishes a stable release per `vX.Y.Z` tag and a beta prerelease per `main` build (newest 5 kept) with Hub and Player artifacts (ADR 0011 D1, D3).
- [x] Done: Windows installer completed: launcher plus `bin\` layout instead of a DLL subfolder, upgrade over an existing flat installation, data is preserved (ADR 0011 D7).
- [x] Done: Hub as a Linux package (.deb amd64/arm64) with systemd units; migration from `install-hub.sh` installs; the script stays for non-Debian systems (ADR 0011 D5).
- [x] Done: Integrated updater for Hub and Player: Ed25519-signed update index (`updates-index`, same key as 0.2), channel setting, automatic install on beta and confirmation on stable, compatibility via `protocol_version` (ADR 0011 D2, D4, D5, D7). The Hub applies updates through a root helper started by a systemd path unit.
- [x] Done: Hub: database backup before schema migration, service restart by the package after the update (ADR 0011 D6).
- [x] Done: Windows Player and Linux Hub only; other platforms follow later.
- Rollback: no automatic or one-click rollback in 0.3 (manual downgrade plus DB backup restore); decided and built in 0.17 ([ADR 0029](adr/0029-operations-and-safety.md) D4, finishes ADR 0011 D8).
- Moved: the Player-side signature check of core packages (ADR 0010 D5) is not part of 0.3; it is in 0.4 (done there, see below).

### 0.4 Sessions over the internet and save comfort

Moved ahead of the UI passes by Fabio on 2026-10-07: testing with friends outside the LAN comes first.

- [x] Done: Supported setups decided and documented: LAN, internet with port forward (recommended), VPN; no external TURN server in 0.4 (ADR 0012 D1; `docs/guides/sessions-over-the-internet.md`).
- [x] Done: Hub: embedded STUN/TURN relay (pion/turn, off by default) with short-lived credentials in `hello_ack` and the join response; Settings shows TURN status and the router port forwards (ADR 0012 D2-D4).
- [x] Done: Player: TURN via libdatachannel, connection type (direct / relay) in diagnostics, `FRAMEBEAM_FORCE_RELAY` / `--force-relay`, AIMD bitrate adaptation to the worst viewer (ADR 0012 D5).
- [x] Done: Multiview with up to 4 surfaces (several remote Sessions) and the audio focus rule (ADR 0012 D8).
- [x] Done: Player-side signature check of core packages (moved from 0.3, ADR 0010 D5): the Player verifies the signed core index served by the Hub (ADR 0012 D6). Retired in 0.8 (ADR 0020 D6).
- [x] Done: Saves: retention/thinning, restore from history, manual snapshot, WebSocket push `save_updated`, multiple slots (ADR 0012 D7; `docs/architecture/03-saves.md`).
- Open: verification across real networks (two Players behind different routers, forced relay, more than one remote Session) is done locally by Fabio.

### 0.5 OpenGL hardware rendering

Decided by Fabio on 2026-10-07: OpenGL hardware rendering for the Player, for all emulators/cores and not only 3DS, comes before the UI passes.

- [x] Done: Libretro hardware rendering (ADR 0013 D1-D4; largest work item): the Player's libretro backend handles only software framebuffers today (no `RETRO_ENVIRONMENT_SET_HW_RENDER`). Needed: an OpenGL 3.3 core context (shared with the Qt Quick scene or offscreen), FBO handed to the core via `get_current_framebuffer`, `context_reset`/`context_destroy` handling, presenting the FBO without a CPU copy, and a GPU readback (or zero-copy path to the encoder) when the Session is shared. Delivered as an offscreen context per game, Player-owned FBO and a readback of every frame into the existing frame path. Deviation: no zero-copy presentation yet (ADR 0013 D3); revisit if profiling shows the readback costs frame time. `FRAMEBEAM_DISABLE_HW_RENDER=1` switches it off.
- [x] Done: CI (ADR 0013 D6): Mesa llvmpipe under xvfb on Linux, GL tests skipped without a GL 3.3 context. Original note: headless CI cannot run hardware-rendered cores end to end; core tests need a GL context (for example Mesa llvmpipe) or stay local.
- [x] Done: melonDS DS OpenGL renderer and internal resolution (ADR 0013 D5). Original note: melonDS DS then gets its OpenGL renderer and the internal resolution option; both are hidden today because the Player offers no GL context (see the Emulation page in `docs/design/legacy/player.md`).
- Open: verification on real GPUs and drivers and frame time (4x internal resolution) is done locally by Fabio.
- Later Nintendo systems rely on it: the 3DS (0.10) and, among the later systems, N64 and GameCube/Wii.

### 0.6 Player UI pass

Done in code (decisions: [ADR 0014](adr/0014-player-ui-pass.md), accepted); looks, GPU values and real sessions are verified only locally by Fabio.

- [x] Done: Clarity: rework information density and grouping per screen (Library, Detail, game view, Emulation, Controllers, Settings).
- [x] Done: Spacing, sizes and alignment consistently from the design tokens (`docs/design/legacy/tokens.md`) instead of single values.
- [x] Done: Responsiveness: nothing blocking on the UI thread (network, hashing, encoding), loading and progress states instead of freezing, immediate click feedback.
- [x] Done: Dynamics: transitions and animations (page change, hover, lists); live updates of Library, Sessions and sync status without manual reload.
- [x] Done: One pass with screenshots per screen: the screenshot tests (`FRAMEBEAM_SCREENSHOT_DIR`, `ctest -R screenshots`) render all screens at 1440x900 and produce the "after" set, which the PR carries. There is no "before" set.
- [x] Done (D15): Core state refresh: after a core download the "core missing" notice on NDS games stays until the Player restarts (found by Fabio on 2026-10-07); Library and Detail must re-evaluate the core state live.
- [x] Done (D3, D9, D12, D13): From 0.4 (requested by Fabio on 2026-10-07): Settings → Hubs lets the user edit a Hub's address and port (for example after the Hub port changed or when switching between LAN address and public name), keeping the pinned fingerprint and credential; connection type (direct / relay) and target bitrate are shown in the Streaming section of the diagnostics (next item); multiview layouts and the save history, restore, snapshot and slot picker get their final design.
- [x] Done (D4-D8, D10, D11): Diagnostics split (requested by Fabio on 2026-10-07; design 3t-3y in `docs/design/legacy/player.md`, accepted by Fabio on 2026-10-08): one overlay with two separately collapsible sections, Emulation and Streaming, opened by the Diagnostics tab, the side-panel toggle or F3, open/closed remembered per section; the same in window, multiview (per tile) and fullscreen (Emulation only); Streaming shows "No active session" without a session. New measurements needed (today missing in the Player): actual emulation fps and frame time with the emu/readback split, audio buffer and underruns, GPU/driver string and the reason for an OpenGL fallback as UI values, decoder name, and remote viewer values on the host (loss and bitrate arrive already in the viewer's receiver report and only need to be kept; decoded fps is new); the host line shows the target bitrate next to the measured one. Open points m-t are decided in ADR 0014. Built: split overlay (Emulation / Streaming) with F3 and persisted section states, new measurements, viewer rx report with optional `fps`/`dec` fields kept on the host, target bitrate on the host line; fullscreen with F11/Esc; Library filter chips, Settings jump list, Hub switch/edit/remove with address and port validation, layout switch from the manifest, Multiview Grid 2×2.

Open in 0.6:

- [x] Done (0.7.x): the sidebar shows the Hub user name from the handshake `user` (API 1.7.0) above the device name.
- [x] Done (0.10, ADR 0022 D6): per-game settings (Game override on the Emulation page: core, speed-up, core options, reset).
- [x] Done (0.7.x): Hotkeys tab (3f) and "Duplicate to edit" for built-in controller profiles, see "0.7.x Hotkeys and controller profiles".
- [x] Done (0.7.x, ADR 0017 D3): `PlayerController` split into `CoreCatalog`, `GameDetail`, `HubPresenter` and `GameStarter`. 2026-10-19: ROM upload state moved into `UploadController`; open: the connection lifecycle (needs a navigation state object first).
- Verification of look and feel, GPU values and real sessions is done locally by Fabio.

### 0.7 Hub UI pass

Implemented, see [ADR 0015](adr/0015-hub-ui-pass.md) (accepted).

- [x] Done: pages restyled to the v4 design (Library with saves column and conflict marker, Saves with slot tabs, history timeline, snapshot filter, inline restore confirmation and retention box from the real rules, Systems list and detail tabs, Clients, Users); nav badge "firmware" is now "{n} issues".
- [x] Done: no full page loads on navigation; sidebar swaps only the content area (ADR 0015 D1).
- [x] Done: live updates without reload via an in-process event bus and Server-Sent Events; badges and affected tables refresh themselves, Clients polling removed (D2).
- [x] Done: Settings sub-pages Updates, General, Network, Security with per-field autosave (D3).
- [x] Done: network settings form (Hub port, embedded TURN, public host, TURN port, relay range and IP, STUN servers, save retention). Decided by Fabio on 2026-10-07: web settings win; `hub.env` and flags only give the initial value (D4). Changes that need it are applied by a restart the Hub triggers itself, without root (D5). Ports below 1024 are refused on the web, use `install-hub.sh --port` (D6). Startup fallbacks keep the Hub reachable after a bad setting (D7). The TURN status and port forward list moved into Network; reachability is derived from the configuration (D8).
- Open (left out of 0.7, remaining): thinned-out history marker, metadata actions, Library "of total" disk capacity, external TURN servers with login (only a STUN list; done in 0.10.x, see Connections), transport option "Custom cert/key" ("Reverse proxy" done 2026-10-19 as `-trusted-proxy`, ADR 0015). A reachability probe from outside is rejected (ADR 0015 D8: reachability is derived from the configuration). Core packages are listed per system since 0.8.
- [x] Done (0.7.x follow-up): Hub Saves page "+ New slot" (new slot starts from the current version of the selected slot; web only, no API) and deleting a snapshot (inline confirmation; `DELETE /api/v1/games/{game_id}/saves/{slot}/history/{version}`, handshake feature `saves_v3`, OpenAPI 1.6.0, `protocol_version` unchanged). Renaming/deleting a slot stay open (0.10.x, together with blocking save upload while a session of that game runs).
- [x] Done (0.7.x follow-up): Library "Rescan folder" (non-recursive import from `-library-import-dir`), invite "Copy link" (only in the creating response; public `/invite` page), "Player too old" marker in the Clients table, certificate "Renew now" (swap without restart; tlsutil writes key/cert atomically).

### 0.7.x Speed-up (fast-forward)

See [ADR 0018](adr/0018-player-speed-up.md) (accepted).

- [x] Done: "Speed-up" (libretro fast-forward) in the Player for cores that do not inhibit it; Space toggles; speed 1.5×, 2×, 3×, 4×, 6×, 8× (default 2×, the user's choice wins over a core ratio); screen frames limited to the base rate.
- [x] Done: header button "Speed-up" ("»" when narrow) and a speed select in the Session panel (running game only) and indicator "Speed-up ×N"; hidden when the core inhibits it; stays usable while the Session is shared (viewers get the normal stream frame rate).
- [x] Done: Emulation settings "Speed-up speed", "Speed-up on start" (default off) and "Audio during speed-up" (default on, resampled; off drops audio) with the global > system > game hierarchy.
- [x] Done (0.7.x): the speed-up key is configurable in Controllers → Hotkeys.
- [x] Done (0.10.x): gamepad hotkeys, see "0.7.x Hotkeys and controller profiles".
- [x] Done (0.7.x follow-up): hardware frames are scaled down on the GPU to the size the view (and, while shared, the stream) needs before the readback; diagnostics show "read back W×H", an honest readback mean and reads per second. melonDS DS ignores `GET_AUDIO_VIDEO_ENABLE`, so it still renders every frame during speed-up.

### 0.7.x Direct GPU display (zero-copy)

Follow-up to ADR 0013 D3 (requested 2026-10-08 after speed-up stutter at 8× internal resolution).

- [x] Done (0.15, [ADR 0027](adr/0027-gpu-paths.md)): on Windows with Qt Quick on D3D11, hardware-rendered frames reach the game view through a ring of shared D3D11 textures, without CPU readback and QImage upload; readback runs only while a shared Session needs pixels (no GPU-direct encoding) or until the first GPU frame is shown. `FRAMEBEAM_DISABLE_ZERO_COPY=1` turns it off. Verified only locally on real GPUs.
- [x] Done (0.15, ADR 0027): an OpenGL to D3D11 bridge (WGL_NV_DX_interop2) on our own D3D11 device, shared by the display and the QSV and AMF paths of "GPU-direct hardware encoding"; readback stays the fallback.
- Picture quality is unchanged (the frame is still downscaled to the window); the gain is CPU time and about one frame of latency.

### 0.7.x GPU-direct hardware encoding (NVENC first)

Requested 2026-10-09. Today a shared Session reads the frame back, converts it to YUV on the CPU and uploads it again to NVENC/QSV/AMF.

- [x] Done: NVIDIA first ([ADR 0019](adr/0019-gpu-direct-nvenc.md)): the core's OpenGL texture goes to NVENC through CUDA-GL interop and FFmpeg CUDA hardware frames (NVENC takes RGB and converts on the GPU); no readback or CPU conversion while sharing. Verified by Fabio on 2026-10-11 on an RTX 4080 SUPER (driver 617.42): expected log order, correct picture, all lifecycle cases including two viewers; encode about 1.8 ms per frame at 1280x1920 against 3.6 ms per frame for readback at 1024x1536. Every failure still falls back to readback frames.
- [x] Done (0.15, [ADR 0027](adr/0027-gpu-paths.md)): AMD `h264_amf` (BGRA D3D11 frames) and Intel `h264_qsv` (NV12 via the D3D11 video processor) via the OpenGL to D3D11 bridge from "Direct GPU display"; verified only locally. Testers have unknown GPUs; Intel QSV via readback measured about 5-6 ms per frame on a tester's notebook (2026-10-11).
- Software encoders, cores without hardware rendering and failed interop keep today's readback path. Verifiable only on real GPUs (CI has none).

### 0.7.x Hotkeys and controller profiles

- [x] Done: Hotkeys tab in Controllers (3f): Player keyboard hotkeys for fullscreen (F11), diagnostics (F3), snapshot (F5) and speed-up (Space) are configurable, can be cleared and reset; Esc stays fixed (leave fullscreen, else pause). Stored locally in `settings/controllers.json` (`hotkeys`, only non-default entries); a key used by another hotkey is refused; a hotkey wins over the keyboard profile and is never sent to the core. Key hints in the game screen, diagnostics and Session panel follow the configured keys.
- [x] Done: built-in controller profiles stay read-only; "Duplicate to edit" (and clicking a mapping of a built-in profile) creates a copy, assigns it to the device and starts editing it.
- [x] Done (0.10.x): gamepad hotkeys: hotkey button (default Select) plus a button, configurable per action in Controllers → Hotkeys, see [guides/app.md](guides/app.md).

### 0.7.x Background game

Decided by the project owner on 2026-10-08.

- [x] Done (interim UI): "← Library" in the game view pauses the game and keeps it loaded (a shared Session stays shared); the Library shows "Now running" with Resume and Quit game, the tile and the detail pane mark the game, input does not reach the core, and starting another game asks "Quit {running} and start {new}?" first. Quitting is an explicit action (header "Quit", Session panel, Library).

### 0.7.x UI revision after the playtest

Design handoff of 2026-10-08 after a friends playtest, imported in [docs/design/README.md](design/README.md) (screens 3c-2 to 3c-5, 3e-2, 3p-2, 3f-2/3, 3g-2/3, 3r-2, 3h-2, 3i-2, 3t-2, 3x-2, 3s-2/3). Open points and the defaults decided: [design/legacy/decisions.md](design/legacy/decisions.md) (u-al). Per screen specs: [design/legacy/player.md](design/legacy/player.md), [design/legacy/hub.md](design/legacy/hub.md).

- [x] Settings rows: one SettingsRow component for Emulation and Settings (3e-2, 3p-2): grid 14 | flex | 280 | 72, changed dot, "More" description with option list, segment/select rule, disabled reason, hover and focus, core-agnostic rendering of the core's options and categories.
- [x] Library sort and Ready first, compact SAVE summary, saves view and upload confirmation (3c-2, 3c-3, 3c-4): toolbar with count, Sort menu and "Ready first" (saved per device), NOT READY group divider, empty-result state, "Upload ROM" in the header, saves mode in the 392 column (slot tabs or switcher, CURRENT bar, snapshot, new slot, restore and delete confirmations, file details, offline/running/conflict states, upload with local backup).
- [x] Saves view in the running game: "Manage saves" in the in-game side panel opens it; snapshot, delete, restore and upload work live (restore/upload: Hub upload, local backup, atomic write, game reload; refused on SAVE_RAM size mismatch); "Resolve conflict" resolves inline without starting the game.
- [x] Controllers glyphs (3f-2, 3f-3): PadGlyph chips and drawn D-pad icons, "Button labels" select (Auto, Xbox, PlayStation, Generic; saved per device), mapping table `14 | flex | 240 | 72`, controller-shaped input test grid.
- [x] Hub saves upload (3s-2, 3s-3): "Upload save" panel in the CURRENT bar, standalone form removed, Library row link opens the panel, timeline reasons "Uploaded" and "Before upload", upload counts in retention, states (identical file, blocked while a session runs), undo link.
- [x] In-game header, panel and diagnostics (3g-2, 3g-3, 3r-2, 3h-2, 3i-2, 3t-2, 3x-2, 3c-5): GameHeader in five zones with Reset popover and compact 1280 mode, GamePanel (Sharing, Save, Quit game pinned; collapsible rail; remote-tile panel), multiview tile selection vs audio focus and "+ Add" picker, one diagnostics toggle (header + F3) with the overlay at the top right, "Now running" strip, paused background game with Resume/Quit and the quit-and-start dialog. The 3c-5 Library look (tile marker, running detail column, quit-and-start dialog; interim strip removed) is done in the line below.
- [x] Library with a running game (3c-5): "❚❚ Paused" marker and status line on the running tile, detail column of the running game (Running since / Sharing / Core, SAVE "Syncs on quit", Resume and Quit game), quit-and-start dialog with the "After confirming" steps; the interim Library strip is removed (the sidebar "Now running" strip stays).

### 0.7.x Hub admin gaps, identity and save upload

- [x] Done: Hub: admins delete users and devices for good (confirmation page; deleting a user removes their devices, invites, saves and save history, uploaded games are reassigned to the deleting admin). The handshake carries the optional `user` object (API 1.7.0).
- [x] Done: Player: app and taskbar icon from the logo spec (also the Windows executable and installer icon), Hub user name in the sidebar.
- [x] Done: Upload a save file into a slot (`saves_v4`, API 1.8.0, migration 0008): Hub API and web, Player "Upload save file…" in the game detail; the previous checkpoint goes to history as `before_upload`.
- [x] Done: Saves in the running game: "Manage saves" works live; restore and upload apply the save as a full game restart (stop, write, start) instead of reloading inside the running core.

### 0.7.x Hardening of the internet-facing ports

- [x] Done: Embedded TURN relay: relay peers in internal ranges are refused unless they are a currently connected Player, limits per device and for TCP connections, credential expiry checked ([ADR 0012](adr/0012-internet-sessions-and-save-comfort.md); [server-configuration.md](guides/server-configuration.md#turn-security)).
- [x] Done: Web port and Player API: CSRF token on multipart forms, login attempt limits (IPv6 per /64, per-username backoff, at most two password checks at once), `__Host-` cookies with TLS, minimum password length 12, read deadlines and a cap of 512 connections, rate limits for WebSocket messages, pairing and invite redemption.

### 0.7.x License and legal notices

- [x] Done: The repository is licensed GPL-3.0-or-later with `LICENSE`, `NOTICE.md`, `THIRD-PARTY-NOTICES.md`, `CLA.md`, `CONTRIBUTING.md` and a PR template with a CLA checkbox. The Hub `.deb`, Hub release assets and the Windows installer carry the license and third-party license texts; `server/THIRD-PARTY-NOTICES.txt` is generated (`make notices`) and checked by `make check-hub`.

### 0.7.x Player fixes

- [x] Done: Audio output in pull mode (Qt 6.8 WASAPI push mode went idle and stayed silent on Windows); underruns are counted once per dry spell.
- [x] Done: The core cache re-hashes core files that were rewritten shortly before the check (the verified-hash memo keys on size, mtime and change time).

### 0.8 Cores from the libretro buildbot

Done in code. Decisions: [ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md) (accepted 2026-10-09). Supersedes the signed core mirror of 0.2 (ADR 0010 D1 to D3, D6) and the Player-side index check.

- [x] Done: Systems instead of fixed cores: the Hub registry keeps the supported systems with their libretro system ids (migration 0009); every libretro core for a system that the buildbot provides is offered.
- [x] Done: Hub downloads cores RetroArch-style from the libretro buildbot (catalog from `info.zip` and `.index-extended`, refreshed at start, every 24 h and on "Check source now"; flags `-core-buildbot-url`, `-core-info-url`). The admin installs, updates and removes cores per system on "Systems & Cores" (Cores tab) and picks the default core; SHA-256 pinned at install; versions `YYYY.MM.DD[.N]`; automatic updates were added on 2026-10-11 (see below); the expected-version selector is gone. Packages of the retired signed source (`melonds_ds`) are removed when the Hub starts (migration 0011 also replaces the old provisioning text); a system left without a core asks the admin to install one, and the handshake core check accepts any core the Hub serves for the system (legacy ids resolve via aliases).
- [x] Done: Offline `framebeam-hub import-cores <dir>` with buildbot zips (`<dir>/<platform>/<core>_libretro.<suffix>.zip`, optional `info.zip` and `.index-extended`); `install-hub.sh import-cores` updated.
- [x] Done: Protocol: handshake feature `cores_v2` (`default_core_id`, `cores` per system), OpenAPI 1.9.0, `protocol_version` stays 1; `cores_index_v1` and the signed core index endpoints are removed.
- [x] Done: Player: system manifest and core profiles are split (`manifests/systems/nds.json`, `manifests/cores/{melondsds,desmume}.json`); cores without a profile are experimental; core choice game > system > Hub default with a "Core" select per system on the Emulation page; the signed core index check is removed (size and SHA-256 against the Hub stay).
- [x] Done: Snapshot of the save on the Hub before a core change (`writer_core_id`/`writer_core_version` in `sync.json`, label `Before core change: ...`); a failed snapshot blocks the start. The ADR 0017 major-version block applies only when both sides report a semver version.
- [x] Done: DeSmuME is the second NDS core (with a core profile); CI fetches it from the buildbot (`scripts/fetch-buildbot-core.sh|ps1`, `make fetch-desmume`, `FRAMEBEAM_REQUIRE_CORES=1`).
- [x] Done: The signed core mirror (`cores.yml`, release `cores-index`) is retired; the releases `cores-index` and `core-melonds_ds-1.4.0` were deleted on 2026-10-23.
- [x] Done in code: Automatic core updates (decided 2026-10-11, [ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md) update): setting `core_auto_update` (default on, toggle "Automatic updates" on Systems & Cores); after every successful catalog refresh the Hub updates installed cores with a newer build (same safeguards as a manual Update); a failing core does not stop the others, the last error is shown; when off, the manual Update button applies.
- Open: The real buildbot download (catalog refresh and install against `buildbot.libretro.com`) is only exercised by CI, not by a local test.
- [x] Done (0.10, ADR 0022 D6): game-level core choice in the Game override UI on the Emulation page.
- [x] Done: Player core warnings follow the local state (a core usable on this device is not reported missing; the handshake is resent after a core download or update) and the fullscreen diagnostics show Emulation and Streaming like windowed mode.
- Open: Real play with DeSmuME and melonDS DS is verified by Fabio locally.

### 0.9 One installer: Player, Hub or both

Done in code ([ADR 0021](adr/0021-one-windows-installer.md), proposed). Absorbs the former "Hub for Windows / Windows Server" milestone. The installer, upgrades, services and the Inno migration are verified only locally by Fabio; CI tests silent install and upgrade.

- [x] Done in code: One Windows installer as MSI built with the WiX Toolset (replaces Inno Setup) with the features Player and Hub (`packaging/windows/framebeam.wxs`, local build `build-msi.ps1`).
- [x] Done in code: "Set up a Hub on this PC" in the Player: installs the Hub (MSI of the Player's own version) if needed, creates the admin and pairs over localhost without a pairing code (local setup API, protocol 1.10.0, `local_setup_v1`).
- [x] Done in code: Standalone mode like RetroArch: a Hub bound only to loopback (`-network-sharing`, off for Player plus Hub) with a local ROM folder (the import folder, now a stored setting, `grant-folder`); network sharing can be switched on later in the Hub settings or the Player. A Player that wants cores directly from the internet uses this local Hub (ADR 0020 D1).
- [x] Done in code: Install scope: a Player-only install is per user; with the Hub it is per machine under Program Files, where Player data falls back to AppData (existing fallback, [ADR 0004](adr/0004-player-portable-data.md)) and is migrated once from the old portable folder; Hub data lives under ProgramData.
- Rejected: MSIX. It needs a paid, trusted signing certificate, virtualizes the install directory so the portable `<exe-dir>/data` cannot work, limits services, and its own update mechanism conflicts with the FrameBeam updater.
- [x] Done in code: Hub for Windows: Windows amd64 Hub build in CI and release (job `hub-windows`); own Windows service `FrameBeamHub`, never inside the Player process; update path via a LocalSystem updater service and artifact kind `msi`; the MSI registers the services, ACLs, firewall rules and markers.
- [x] Done in code: Migration of existing Inno installations (no bridge release): the release keeps publishing a small Inno `setup.exe` under kind `installer` that only installs the MSI silently, so older Players update normally. The MSI uninstalls the Inno install silently, keeping `data\`. Credentials stay in the Credential Manager. The Pi Hub (.deb) is unaffected.
- [x] Done in code: Hub installer part (data directory, port, firewall rules by the installer, admin setup in the Player); updates via the updater from 0.3.
- Open: Review paths, file permissions (instead of 0600) and certificate storage on Windows beyond the service ACLs.
- Open: the MSI is unsigned. Decided by Fabio on 2026-10-11: the Inno shell (`setup.exe` wrapper that migrated old Inno installs) is removed, since all test clients are on the MSI (done, see 0.10.x).

### 0.10 Second system: Nintendo 3DS with Azahar

Decided by Fabio on 2026-10-07: the second system is the Nintendo 3DS with the Azahar libretro core (replaces the mGBA proposal).

- Core: `azahar_libretro` (GPLv2+), available prebuilt on the libretro buildbot (checked 2026-10-07: `nightly/windows/x86_64/latest/azahar_libretro.dll.zip`) and described by `azahar_libretro.info` in libretro-core-info. It arrives as a buildbot core through 0.8 and is installed by the Hub admin like any other core; FrameBeam mirrors nothing ([ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md)).
- Hardware rendering: the core info sets `hw_render = true`, `required_hw_api = OpenGL Core >= 3.3`. The Player's OpenGL hardware rendering is a prerequisite and arrived in 0.5; this version only uses it.
- Display: two screens of different sizes (top 400 x 240, bottom 320 x 240 touch); the system manifest describes layout and touch mapping, and the Session encoder sends the composed frame.
- Games: decrypted dumps only (`.3ds`/`.cci`/`.cxi`/`.3dsx`, plus the compressed `z*` variants); FrameBeam never decrypts. Some games need system files such as Mii data dumped from the user's own console; these follow the firmware path from phase 5 and are never shipped.
- Performance: 3DS emulation needs a considerably faster CPU/GPU than DS; check on Fabio's devices whether play plus encoding fits.
- It needs no installer changes: the Hub installs the core from the buildbot (0.8).
- ~~Re-evaluate the core version check (warning vs. block, ADR 0007).~~ Decided in ADR 0017 D1.
- [x] Done in code: Hub system `3ds` (migration 0010, no default core until the admin installs `azahar`; Systems page opens on the first system with an installed default core); Player manifests `systems/3ds.json` (400x240 + 320x240 touch, per-screen `align`, 400x480 frame) and `cores/azahar.json`; `requires_hw_render` (no OpenGL Core >= 3.3: clear error, no software fallback). [ADR 0022](adr/0022-nintendo-3ds-with-azahar.md).
- [x] Done in code: 3DS saves were local-only first (save source `none`); superseded by the sync item below.
- [x] Done in code: input profile `3ds` (ZL/ZR, circle pad, C-stick); built-in gamepad profile binds triggers to ZL/ZR and the left stick to the circle pad.
- [x] Done in code: Game override UI on the Emulation page (per-game core, speed-up and core options, reset).
- [x] Done in code: ROM cache limit (`rom_cache_limit_bytes`, default 20 GB, LRU) and "Clear ROM cache".
- [x] Done in code (decided by Fabio on 2026-10-11): 3DS saves sync with the Hub (save source `core_tree`, format `azahar_sdmc`: deterministic tar of the game's save data and extdata; backup before tree replacement; local-only saves adopted on first start). [ADR 0022](adr/0022-nintendo-3ds-with-azahar.md) D3 amendment.
- Open: Mii and other system files (not wired yet).
- Decided by Fabio on 2026-10-11: the 3DS screen layouts are the same set as NDS. The 3DS system language is an existing Azahar core option (not locked in the manifest); it is only a check item for the test evening (1.0).
- Open (Fabio, local): real Azahar run on the RTX 4080 (GL 3.3 core context), frame time including Session encoding, resolution factor, option values against the real core.
- Core sourcing: superseded by [ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md) (0.8); the signed mirror, core list and weekly pin bumps of the 2026-10-07 plan are dropped.

### 0.10.x Before new testers

Decided by Fabio on 2026-10-11, in this order.

#### 1. Connections (first)

In the test of 2026-10-11 most WebRTC connections ran relayed over TURN. Cause found for one direction: the Hub's STUN list is empty by default and the embedded STUN is advertised under the request host (a LAN address for Players in the Hub's network), so those Players gathered no public server-reflexive candidate. Adding a public STUN server (stun.cloudflare.com:3478) to the Hub made that direction direct. The other direction (host on a mobile hotspot) stayed relayed even after both Players restarted with the new STUN list; most likely carrier-grade NAT with per-destination ports, where TURN is the expected path (not measured, the logs had no ICE data).

- [x] Done in code: public default STUN server (`stun:stun.cloudflare.com:3478`) when no STUN URL is configured; `-ice-servers none` (or "Use no external server" in Settings → Network) switches it off.
- [x] Done in code: ICE logging per connection on host and viewer (gathered candidates by type, remote candidates, selected pair, failures).
- [x] Done in code: overlay and diagnostics show the connection type and, for a relayed path, the reason per viewer (forced, no STUN reachable here, other side has no public candidate, direct checks failed).
- [x] Done in code: running Players pick up a changed ICE list through the WSS message `ice_update` (`ice_update_v1`); new connections use it, running ones stay.
- [x] Done in code: Cloudflare TURN (Realtime) as an optional TURN provider next to the embedded `pion/turn`: the Hub hands out short-lived Cloudflare TURN credentials (API token as a secret on the Hub host); moved out of "Later". Cloudflare Tunnel stays in "Later" to evaluate.
- [x] Done in code: external TURN servers (static or shared-secret credentials): `-turn-external` (own coturn on a VPS or a third-party provider, several at once next to the embedded TURN and Cloudflare TURN), secrets only from env/file, listed in Settings → Network and the diagnostics bundle (`network.external_turn`); OpenAPI 1.13.0 (`expires_at` optional), ADR 0012 amendment 2026-10-12, guide [coturn-on-a-vps.md](guides/coturn-on-a-vps.md).
- [x] Done in code: external TURN editable in the Hub web interface (Settings → Network: add, edit, delete by an admin; saved list wins over `-turn-external`, write-only secrets in the Hub database, applies without restart, diagnostics show `source`); guides updated, ADR 0012 amendment line.
- Open: confirm the relayed direction with the host on a mobile hotspot from the new ICE logging.
- Open (Fabio, 2026-10-12): external TURN servers (any provider, with login) configurable in the Hub web interface, as a follow-up to the provider-neutral TURN configuration through environment and secret file.

#### 2. Small items

- [x] Done: gamepad hotkeys: each Player hotkey has an optional gamepad binding next to its key (Controllers → Hotkeys, "Gamepad" column): the hotkey button (default Select, configurable) held plus a button; defaults Select+RB speed-up, Select+LB snapshot, Select+R3 diagnostics, fullscreen unbound (there is no pause action, so Select+Start stays unbound). A single button without the hotkey button can be bound too. The combo button never reaches the core, the hotkey button still does; P1 only. Stored in `settings/controllers.json` (`padHotkeys`, `padHotkeyModifier`, only non-default entries). Hints show the gamepad combo while a gamepad was the last input used. Not tried with a real gamepad yet (SDL virtual joysticks only).
- [x] Done in code: speed-up frame loss for viewers: during speed-up the Session gets evenly spaced base-rate frames with wall-clock timestamps backdated to the base-rate slot (`FramePacer`, ADR 0018 amendment 2026-10-11).
- [x] Done in code: repo-scan leftovers: the core probe runs on a worker thread (`CoreProber`, one core at a time, never twice for one library, loading row on the Emulation page, handshake core list resent when done), the Library grid is virtualized (`GameGrid.qml`, only the visible rows have tiles), a failed final save write at stop is reported in a Library banner.
- Open: Library loading slows down sharply with many games (500/1000/2000 games via a fake Hub took about 15 s/25 s/226 s, GUI thread busy converting QVariant maps for QML); found while testing the virtualized grid.
- [x] Done in code: save slots rename and delete (Hub and Player, `saves_v5`, OpenAPI 1.12.0) and block save upload while a session of that game runs. Hub: Saves page Rename/Delete, upload lock (`session_running`). Player: Rename slot / Delete slot… in the saves view (confirmation with the latest checkpoint, `confirm_revision`), local slot moved or removed only after the Hub confirmed, `save_updated` `renamed`/`deleted` handled without losing unsynced local data, rename and delete blocked while the game runs on this Player, save file upload blocked while that game is shared as a Session (live upload for an unshared running game stays).
- [x] Done: invite link port fix: `-public-host` (env, Settings → Network) accepts an optional external port (`hub.example.org:8443`, IPv6 in brackets) that invite links and the Player address use; TURN/STUN keep the host part.
- [x] Done: Hub library entries: Rename and Delete as row actions in the web UI (inline forms, admin only, web only, no API change); deleting keeps the saves.
- [x] Done: Go dependency updates (Dependabot PR #96).
- [x] Done in code: "Collect logs/diagnostics" bundle (Hub and Player). Hub: Settings → Diagnostics and `framebeam-hub diagnostics <out.zip>` (`docs/guides/server-configuration.md`). Player: Settings → Diagnostics → "Collect logs…" (save dialog, zip built off the GUI thread, "Show in folder") and `framebeam_player --collect-logs <out.zip>`; same layout (`player-info.json`, redacted `logs/`, 50 MiB cap), see `docs/guides/app.md`.
- [x] Done in code: The Player updates cores in the background without starting a game (`CoreUpdater`, `network/coreupdater.*`): after the handshake it compares the Hub's core list with its cache, downloads newer builds (size and SHA-256 checked), never touches a core in use, re-probes updated cores asynchronously (`CoreProber`) and resends the handshake; progress shows on the Emulation page.
- [x] Done: remove the Inno shell (see 0.9) and its release/CI parts (shell script, CI build and migration test, release asset and `installer` index entry, Player `installer` fallback and `unins000.exe` detection).

### 0.11 Plugin runtime

Packages 1 to 3 of the plugin work. Package 1 (runtime in the Player) is done in code; package 2 (UI in the Player) is done in code; package 3 (Hub plugins and runs) is done in code on the Hub side and for plugin delivery in the Player; the Player side of runs (`fb.run`, Runs screen) is done in code. Interface: [12-plugins.md](architecture/12-plugins.md); decisions: [ADR 0023](adr/0023-plugins.md) (proposed). FrameBeam stays a generic emulator Player; everything game-specific lives in plugins.

Decided by Fabio on 2026-10-11: this milestone is the runtime only. The plugins themselves (`pokemon-core`, `soullink`; the former packages 4 and 5: plugin repositories, template, first game) live in their own repositories and are independent of FrameBeam milestones; Fabio aims to have them ready for testing before 1.0. Plugin-specific docs and roadmaps (SoulLink rules, per-game adapters) live in the plugin repositories, not here. The plugin design standards ao-bf from the design handoff are accepted.

- [x] Done in code: Package 1: Plugin runtime in the Player (PR #105): vendored Lua 5.4.7 sandbox (64 MiB, 20M instructions / 500 ms per call), one state and budget per plugin, plugin thread, manifest, permissions, `requires`/`provides`, `RETRO_ENVIRONMENT_SET_MEMORY_MAPS` support in the libretro backend, memory snapshots over virtual addresses (default every frame), pointer following, ROM read API, native helpers (LZ10/LZ11, GARC, 3DS RomFS of decrypted NCCH/NCSD, CTR textures/BCLIM/ETC1 to PNG, PK6 decrypt/encrypt as `fb.codec`/`fb.rom.romfs`), install from a folder and linked dev folders with live reload, developer mode, Settings → Plugins and the in-game Plugin console. Tests with the test core (synthetic memory map with an encrypted Gen 6 party) and synthetic data. API: [guides/plugin-api.md](guides/plugin-api.md).
- [x] Done in code: the Player picks up a memory map that a core re-sends with `SET_MEMORY_MAPS` while the game runs (region list, Plugin console and plugin API follow the new map).
- [x] Done in code: Package 2: plugin UI in the Player: manifest field `ui`, permission `ui`, `fb.ui` (`set`, `patch`, `clear`, `title`, `badge`, `dot`, `notify`, `on_action`), host events from `fb` (`detail.game`, `tiles`, `hud.visible`), validated block catalog on the `Fb*` components with tones, sizes and player colours, map view, images from the plugin cache or assets (`image://fbplugin`), slots in-game tab, HUD (bottom-left, bottom-right, top-left), Multiview tile strip, own sidebar screen, section in the game detail and toasts (burst 3, then 1 per second), shell context (plugins with `ui.screen` or `ui.detail` run without a game), several plugins per slot, Settings → Plugins shows what a plugin adds. API: [guides/plugin-api.md](guides/plugin-api.md#ui). Design: [design/legacy/plugins.md](design/legacy/plugins.md).
- [x] Done in code: Package 2 plugin hotkeys: `ui.hotkeys` in Controllers → Hotkeys (FrameBeam keys win, overrides in `plugin_hotkeys`), Alt+1..9 panel tabs, F4 hides/shows HUD widgets with a restore pill. Guide: [plugin-api.md](guides/plugin-api.md#hotkeys).
- [x] Done in code: Package 3 Hub plugins: `.fbplugin` files (zip, limits 32 MiB / 64 MiB / 2000 entries, identical validation in Hub and Player), `SIGNATURE.json` with the release key, origins `official`/`unverified`/`local`, Hub store with admin upload (permission ticks, admin-only), `framebeam-hub import-plugins`, signed plugin index (`-plugin-index-url`, default the official `plugin-index` release, `none` = off; `framebeam-sign plugin-sign`/`plugin-add`), `GET /api/v1/plugins` and file download with SHA-256 pin (feature `plugins_v1`), Player install from the Hub and from files. Guide: [guides/plugin-publishing.md](guides/plugin-publishing.md).
- [x] Done in code: Package 3 Hub runs: runs with participants, per-player state per plugin of the run (own revision each, `PUT /api/v1/runs/{run_id}/state/{plugin_id}`), append-only event log, limits and rate limits, REST (feature `runs_v1`), WSS `run_updated`/`run_state`/`run_event`, catch-up on `hello`, Hub web pages Plugins and Runs (admin, read-only board) with the JS renderer `plugin-blocks.js`. OpenAPI 1.16.0, `protocol_version` stays 1.
- [x] Done in code: Package 3 Player runs: `RunClient` (`client/network/runclient.*`: `runs_v1` REST, WSS `run_updated`/`run_state`/`run_event`, `runs` catch-up in `hello` from a persisted cache, event dedup by `client_id`, state coalesced to 4/s, board 1/s); Lua `fb.run` (permission `run.state`, only when the game is started in a run: `current`, `state`, `states`, `setState`, `event`, `events`, `on`, `setBoard`; [plugin-api.md](guides/plugin-api.md)); Runs screen (sidebar entry when the Hub announces `runs_v1` and an installed plugin has `run.state` or the user has a run or invite; invite cards with game picker and plugin consent list incl. unverified warning, Active/Paused and Finished groups, run detail with status actions and event log, "Start a run" dialog); "Play run" uses the exact Hub copies of the run's plugin versions and the save slot `run-<first 8 hex of run_id>`.
- [x] Done in code: per-plugin participant state with its own revision (`PUT /api/v1/runs/{run_id}/state/{plugin_id}`, `states` in the Run object, OpenAPI 1.16.0).
- [x] Done in code: game codes in the library: the Hub reads the NDS game code / 3DS title ID from the ROM header at upload/import (backfill at Hub start), the Player's run game picker matches the plugins' `games[].codes`; games with an unknown code stay selectable, marked "Game not recognized".
- [x] Done in code: plugin platform gaps for run plugins: manifest `run_settings` (rules form in "Start a run", values in `settings[<plugin_id>]`, Hub plugin list `run_settings`/`run_settings_strings`, OpenAPI 1.17.0); required plugins are included in runs automatically; slot `run` (`ui.run`) with a read-only `fb.run` run view in the Runs screen; host event `tiles` carries `user_id`. Guide: [plugin-api.md](guides/plugin-api.md#run-settings).
- Open: whether non-admin users may upload plugins to the Hub.
- Not built yet: `memory.write`, `tool.exec`, settings schema.
- Open: Azahar exposes game memory. Checked by Fabio on 2026-10-11 with Pokémon Moon and a probe plugin: the buildbot core sends `SET_MEMORY_MAPS` only once at game load, when only VRAM is mapped, so heap and linear heap are missing (the RomFS reads fine). A small upstream Azahar patch re-sends the map when the game maps memory; it is built and tested from Fabio's fork before it goes upstream.
- Open: plugin API v1 surface (exact Lua API, building block catalog) is fixed in the design handoff and package 1.
- Out of scope for this milestone: the plugins themselves, more games, cheats, editor, randomizer (see "Later").

### 0.12 Local ROMs

Decided by Fabio on 2026-10-10 and 2026-10-12 (before 1.0; owner-only visibility, merge by game identity, no P2P). Decisions: [ADR 0025](adr/0025-local-roms.md). Guide: [guides/local-roms.md](guides/local-roms.md).

- [x] Done in code: Hub: several ROM files per game (`game_roms`, `rom_owners`, migration 0015), identity merge (system + game code + version; hash-only fallback for files without a readable header such as `.zcci`), visibility (owners until a file is stored; admins see all with a "Local only" badge), `POST /api/v1/games/register`, `DELETE /api/v1/games/{id}/registration`, `Game.on_hub`/`roms`/`game_version`, `RomInfo.stored`. OpenAPI 1.17.0, feature `local_roms_v1`, `protocol_version` stays 1.
- [x] Done in code: resumable uploads: `POST/GET/PUT/DELETE /api/v1/uploads` (64 MiB chunks, persisted SHA-256 state, 7-day janitor), web upload with 8 MiB chunks and multipart fallback; the old `POST /api/v1/games` stays.
- [x] Done in code: Player: Settings → Library folders (scan, hash cache by path + size + mtime, game code from NDS/3DS headers, registration), launch from the folder without a cache copy, "Local" and "Missing" markers, "Upload to Hub" queue that survives restarts, "Upload ROM" via the resumable API.
- No P2P ROM transfer between Players.
- Open: automatic cleanup of local-only games without owners (admins can delete them); a real-Hub test with a 4 GiB file over a slow uplink. The chunked upload is a prerequisite of the Cloudflare Tunnel evaluation in "Later".

### 0.13 System template and GB/GBC/GBA

Decided by Fabio on 2026-10-11 (moved from 0.14 to 0.13 on 2026-10-12): part of the 1.0 goal ("a new system costs minimal effort").

- [x] Done: data-driven Hub system registry ([ADR 0024](adr/0024-system-template-and-game-boy.md) D1): one embedded JSON per system in `server/internal/hub/systems/`, validated at load and reconciled into SQLite at start (admin state untouched); no SQL migration per system. A Go test cross-checks the Hub definitions against the Player manifests.
- [x] Done: Player system manifest carries `input` (map and labels), `identity` and `display.layout`; input tables are registered from the manifests, systems sharing an `input_profile` must have identical maps, manifests are globbed by CMake (D2).
- [x] Done: GB, GBC and GBA as the proof (moved out of "Later"; also needed for the Pokémon Gen 1-3 plugins): systems `gb`, `gbc`, `gba`, core discovery by `systemid` and extensions (mGBA is offered for all three, gpSP only for GBA, Gambatte/SameBoy only for GB/GBC), shared cores are one installed build (D3, D4). Game identity for plugins: `gba-gamecode`, `gb-title` (D6).
- [x] Done (Fabio, 2026-10-12): no default core on the Hub; the Player ships profiles for mGBA (`gb`, `gbc`, `gba`) and Gambatte (`gb`, `gbc`); other cores run as experimental (D5).
- [x] Done: guide [Adding a system](guides/adding-a-system.md).
- Open (verified only locally): real mGBA and Gambatte cores from the buildbot, real games, saves (`.sav`), BIOS switch behaviour and the GB/GBC/GBA input on keyboard and gamepad. CI uses the test core and synthetic data only.

### 0.14 UI gaps and Multiview

Decided by Fabio on 2026-10-11; moved from 0.15 to 0.14 on 2026-10-12.

Design: Claude Design handoff "Round 3" (2026-10-12), imported in [design/](design/legacy/README.md#round-3-screens-2026-10-12-014-ui-gaps-and-multiview); decisions 20-23 and open points bg-bk in [design/legacy/decisions.md](design/legacy/decisions.md); ADR: [0026](adr/0026-ui-gaps-and-multiview.md). Work packages (one thread/PR each):

- [x] Design import: Round 3 sources, specifications, tokens and decisions in `docs/design/` (this PR).
- [x] Hub pass: Systems and Cores (3l-2 to 3l-4), Library (3j-2), Saves (3s-4), Clients (3m-2), Users (3n-2), Settings (3q-2), Plugins and Runs (4i-3, 4jh-3); cross-cutting rules (dates, row actions, pills, add-to-list, cards below 720 px).
- [x] Player library, settings, plugins and runs pass: Connect (3a-3), Library (3c-6, 3c-7), Settings (3p-3), Emulation (3e-3), Controllers and hotkeys (3f-4), Plugins (4a-3, 4a-4), SoulLink screen and Runs (4f-3, 4j-3); 880 px and 1680 px content widths.
- [x] Player in-game and Multiview pass: Row for 2-3 Sessions and Grid 2x2 from 4 (3r-3), docked diagnostics (3h-3), PiP (3i-3), pause overlay and fullscreen (3y-2: Esc always pauses, F11 toggles fullscreen), paused state signalled to viewers ("Paused by <name>"), one selection style, Add popover below the toolbar. The changed Esc role (ADR 0014 D4) and the Multiview layout names (D13) are in [ADR 0026](adr/0026-ui-gaps-and-multiview.md).

### 0.15 GPU paths

Decided by Fabio on 2026-10-11; moved from 0.13 to 0.15 on 2026-10-12. Testers have unknown GPUs; Intel QSV via readback measured about 5-6 ms per frame on a tester's notebook.

- [x] Done (0.15): OpenGL to D3D11 bridge (WGL_NV_DX_interop2).
- [x] Done (0.15): zero-copy display, see "0.7.x Direct GPU display (zero-copy)".
- [x] Done (0.15): GPU-direct QSV and AMF, see "0.7.x GPU-direct hardware encoding (NVENC first)". The readback path stays the fallback.
- Decided in [ADR 0027](adr/0027-gpu-paths.md) (Proposed). Verifiable only on real GPUs (CI has none); the hardware checklist is in the ADR.

### 0.16 Release workflow and changelogs

Decided by Fabio on 2026-10-21. First in the series, because every later package profits from real release notes. Today `release.yml` gives every beta the same fixed text and `promote.yml` keeps it for the stable release; there is no `CHANGELOG.md`, and the Player only knows `notes_url` from the index. The plugin repositories use `--generate-notes` (PR titles). Changelog language is English (repository rule).

- [x] Done (0.16, [ADR 0028](adr/0028-release-notes-and-changelogs.md)): Changelog section per PR: PR template with `## Changelog` (categories Added / Changed / Fixed / Hub / Player, one user-language line per change, `none` for pure CI or docs PRs) and a CI check that requires it. No files in the repo, so no merge conflicts with auto-merge (S)
- [x] Done (0.16): Beta notes: `release.yml` collects the changelog sections of all PRs since the previous tag (GitHub API, commit range), grouped by category with PR links and commit; falls back to the PR title when a section is missing (S-M)
- [x] Done (0.16): Stable notes: `promote.yml` summarizes all betas since the last stable release ("Changes since X.Y.Z"), deduplicated, replacing the beta text; optional "Highlights" input when triggering (S)
- [x] Done (0.16): `CHANGELOG.md` as archive: `promote.yml` opens a small docs PR with the stable section (auto-merge; `main` has a ruleset, so PR instead of push) (S)
- [x] Data done (0.16), screen open: "What's new" data: the update index gets an optional `notes` field (short text, signed with the index), the Player and Hub web can show it once after an update, else a link to `notes_url`. The Player screen waits for the Claude Design handoff; 0.16 builds the data for it (S-M). Done: Hub and Player parse `notes` (the Hub passes it on in its update status, the Player keeps it); the Player screen is open until the handoff.
- [ ] Plugin repositories (`pokemon-core`, `soullink`, `template`): same PR template and notes from the sections instead of `--generate-notes` (S)

### 0.17 Operations and safety

Decided by Fabio on 2026-10-21. Useful before a test evening. UI parts (backup settings, problem reports, connection assistant, health page) get one Claude Design handoff.

- [x] Done (0.17, [ADR 0029](adr/0029-operations-and-safety.md) D2): Hub backup and restore: one archive (database snapshot, saves, plugins, settings; ROMs optional), schedule, target folder, retention, `framebeam-hub backup` / `restore`, admin API `/api/v1/admin/backups`, plain Settings → Backups page, Status card "Disk · backup" ([guide](guides/server-backup.md)). Not yet: restore from the Player app, compressed or off-site backups
- [x] Done (0.17, [ADR 0029](adr/0029-operations-and-safety.md) D4): Update rollback: "Go back" for Player and Hub from the signed index (last 5 releases, protocol floor, history of versions that ran), Hub with the pre-update database backup and a safety backup, pause and "skipped · available" afterwards, `framebeam-hub rollback`, admin API `/api/v1/admin/updates/*`, plain Settings → Updates page, Player Settings → Updates → EARLIER VERSIONS; plugins can declare `min_player` ([guide](guides/updates.md#rollback)). Not included: rolling back a single plugin (decision 32)
- [x] Done (0.17, [ADR 0029](adr/0029-operations-and-safety.md) D3): "Report a problem": builds on "Collect logs", sends the bundle with a short description to the user's own Hub (Player dialog, `POST /api/v1/reports`, feature `reports_v1`); admins see the list in ADMIN → Reports (REST first, plain web page) (S)
- [x] Done (0.17, [ADR 0029](adr/0029-operations-and-safety.md) D5): Connection assistant: Settings → Hubs / Diagnostics → "Connection check…" runs Hub reachable, STUN, TURN relay, NAT type and a test connection with a plain-language result; "Add to problem report" puts it into the next "Collect logs" bundle as `connection-check.json` ([guide](guides/app.md#connection-check)). The test connection is a WebRTC loopback inside the Player, not a connection to a Hub peer (the Hub has none). Open: entry points from a failed Session and the "Relayed" pill, hand-over to the "Report a problem" dialog, a Settings → Network section.
- [x] Hub health page: CPU, RAM, disk, active Sessions, TURN load, recent errors; optional `/metrics` (Prometheus) (S). Done: admin page Status (`/status`, refreshes every 10 s), sidebar in groups Play / SETUP / ADMIN (Round 4), Settings → Metrics with a bearer-token `GET /metrics`. Not yet: TURN bitrate, relayed/direct per Session, Backup disk and "Backup failed" (come with the backup feature), the Reports entry (there since "Report a problem").
- [x] Done (0.17, [ADR 0029](adr/0029-operations-and-safety.md) D1): Docker image for the Hub (`ghcr.io/<owner>/framebeam-hub`, arm64 and amd64, volume `/data`, non-root, no self-update in the container, [guide](guides/server-docker.md), CI build, push by `release.yml`, `stable`/`latest` by `promote.yml`) (S)
- [x] Done in code: Plugin UI language (pulled forward from the plugins wave): setting "Plugin language" (Auto | English | Deutsch) in Settings > Plugins; Auto = the 3DS system language (Azahar core option) for 3DS games, else the OS language; `GameContext.language` is that tag (S)

### 0.18 Uzume: one app, one server, new name

Decided by Fabio on 2026-10-22 ([ADR 0030](adr/0030-one-app-one-server-uzume.md)). FrameBeam becomes Uzume: one app that also administers the server, no web admin, accounts for every user, server commands for setup and emergencies. The design is final ([docs/design/uzume/](design/uzume/README.md)). Rule for the whole milestone: every beta stays usable, so the web admin goes only at the end, when the app can do everything. After package 2, packages 3, 4 and 5 can run in parallel, 3b follows 3 directly; 6 and 9 run next to 7 and 8.

- [x] Done (PR #181): 1 Docs: roadmap, ADR 0030, terms in all docs and CLAUDE.md files, design import to `docs/design/uzume/`, `VERSION` 0.18.0 (M)
- [x] Done in code: 2 Rename (two rounds, no compatibility with FrameBeam, [ADR 0030](adr/0030-one-app-one-server-uzume.md) D1 amendment): Go module path, programs `Uzume` and `uzume-server`, `uzume-sign`, only `UZUME_*`, Docker only `uzume-server`, invites `UZ-` only, metrics `uzume_*`, headers `X-Uzume-*`, `/.well-known/uzume`, `uzume.db`, option keys `uzume.*`, signature contexts `uzume-*-v1`, Linux package, units, user and paths `uzume`, Windows services, folders, registry, markers and AppUserModelId `Uzume`, update URLs on the new repository; only the GitHub secret name `FRAMEBEAM_SIGNING_KEY` stays (L)
- [x] Done in code: 3 Design system in the app (the SERVER group fills with package 8; screens move to the new layout in 3b): Uzume tokens and IBM Plex fonts, mark, QML building blocks (Toggle, Select, Menu, SettingsRow, Dialog, HelpColumn, Tag, Kbd, Slider, Popover, Avatar with speaking mark, GamepadHint with all keys, stage overlays), AppNav with PLAY / SERVER / THIS PC, Settings with the This PC / Server switch, app icon and installer bitmaps (L)
- [x] Done in code: 3b Existing screens in the new design: library and game page (3c), the running game with side panel, overlays and toasts (3g), Multiview (3r), Emulation, Controllers, Hotkeys, Settings › This PC (Audio & video, Storage, Appearance …), Runs and the run view, plugin slots; only look and arrangement after the boards, the legacy Theme names are gone; gamepad fullscreen Select + B, speed-up Select + RT (L)
- [x] Done in code: 4 Server accounts: password for every user (required for admins), sign-in on new devices, user switching with PIN, "Confirm it's you" before dangerous steps, device list per user, migration, OpenAPI 1.29.0 (M-L)
- [x] Done (0.18, [ADR 0030](adr/0030-one-app-one-server-uzume.md) D3): 5 Admin API: everything the web admin does under `/api/v1/admin/...` (users and invites, devices, systems and cores, plugins, reports, library, saves of all users, server settings, status), handshake feature `admin_v1` (admins only), OpenAPI 1.28.0, see [reference/protocol.md](reference/protocol.md) (L)
- [x] Done (0.18, [ADR 0030](adr/0030-one-app-one-server-uzume.md) D4 amendment): 6 Server commands: `setup-code`, `code`, `reset-admin`, `status`, `update`, `backup`, `restore`, `reset` (Windows, Linux, Docker), setup code redeemed with `POST /api/v1/setup`, security code from the certificate, handshake feature `setup_code_v1`, OpenAPI 1.30.0, see [guides/server-configuration.md](guides/server-configuration.md#server-commands) and [reference/protocol.md](reference/protocol.md#security-code-and-setup-code) (M)
- [x] Done in code: 7 App start and accounts (also adds the `uzume://` link scheme; as built: [ADR 0030](adr/0030-one-app-one-server-uzume.md) D5 amendment, [join link](reference/protocol.md#join-link); 8f is built by package 8): first start (8a), join a server with a security code (8b), invite link (8c), switch user (8d), My account with privacy (8e), confirm it's you (8f), forgot password (8g) (L)
- [x] Done in code (0.18, [ADR 0030](adr/0030-one-app-one-server-uzume.md) D3): 8 App admin area: SERVER group (Status, Users with People / Invites / Devices, Systems & cores with Firmware, Plugins with the official index, Reports), "Confirm it's you" before dangerous steps, Settings › Server (General, Network & sharing, Sign-in, Updates, Backups, Storage, Diagnostics), admin parts of Library (rescan, rename, delete) and Saves (all users, restore for another person, conflicts, upload), run admin actions, What's new, the setup assistant and the server/app version lines; every web admin form has its place in the app. Spec items the admin API does not carry are left out (XL, 2 PRs)
- [x] 9 Runs without a plugin: runs always visible, own save slot "Run: <Name>", run tabs; plugin API: the sidebar slot becomes a run-tab slot; plugin repositories: IDs `uzume.*`, one plugin "SoulLink & Nuzlocke" with a mode, plugin index republished (M plus plugin PRs) - done except the plugin index republish (Publish plugin with `remove_ids`, pending), see [reference/protocol.md](reference/protocol.md) (OpenAPI 1.31.0, `runs_v2`)
- [x] Done in code (0.18, [ADR 0030](adr/0030-one-app-one-server-uzume.md) D6): 10 On this PC and sharing: the app runs `uzume-server -child` on loopback without UAC (hidden admin, no service), "Share with others" runs the MSI elevated once and moves the data with `import-data`, "Stop sharing" / "Share again" use `uzume-server sharing stop|start` and keep accounts and saves; the installer side is package 11. Real Windows UAC, MSI and service switch verified only locally by Fabio (M)
- [x] Done in code (0.18, [ADR 0030](adr/0030-one-app-one-server-uzume.md) D6 amendment): 11 Installer: MSI dialogs after design 8l (How will you use Uzume? with three options, destination folder, server options, finish page that starts Uzume or shows the setup code of a server-only install, Remove with "Also delete all server data"), the app package carries `bin\uzume-server.exe` for "Only on this PC", `IMPORT_SERVER_DATA` for "Share with others", `REMOVE_SERVER_DATA`, the `uzume-server` package conflicts with `framebeam-hub` and prints the setup hint on first install; bitmaps came with package 3. Not built: the port-in-use check (8L-11) and the FrameBeam upgrade dialog (8L-10). The Linux app package `uzume` moves to 0.24 with the Linux app, see [guides/packaging.md](guides/packaging.md) (M)
- [x] Done in code (0.18, [ADR 0030](adr/0030-one-app-one-server-uzume.md) D2): 12 Remove the web admin and finish: all Hub web pages and browser components are deleted (cookie login, templates, htmx and static assets, no OBS stream view); the server serves only the REST API, WebSocket signaling and file transfer; docs, roadmap and the test list for Fabio are updated (M)

### 0.19 Player comfort and library

Decided by Fabio on 2026-10-21 (as 0.18; renumbered on 2026-10-22). The screens are drawn in the Uzume design (library, game page, in-game panel, Settings › This PC); no own design round, and no Hub web parts any more.

Cut into 8 packages on 2026-10-23, one thread and PR each: wave A packages 1 to 4 in parallel, wave B screenshots (after play time, migration and OpenAPI numbers) and screen layouts (after scaling, same display path), wave C storage overview (after screenshots) and per-game overrides (after screen layouts).

- [x] Done (PRs #208, #209): Start and 0.18 leftovers: drop the `web_sessions` table, dead web paths in `action_href`, unused server functions, the "Server web interface" device label, old "Set up a Hub on this PC" wording in the docs (S)
- [x] Done (PR #211): Audio stage 1: master volume, mute, hotkeys, "mute in background" (S)
- [x] Done (PR #210): Integer scaling and aspect ratio options; "Continue playing" tile in the Library (S)
- [x] Done (PR #218): Screenshot hotkey with storage on the server (per game, optionally per run) and a gallery in the app (S-M)
- [x] Done (PR #213): Play time per game, run and user (the server counts from Session and game start events); favorites, collections and recently played (per user on the server, filters in the app) (M)
- [x] Done (PR #220): Storage overview in the app (Settings › This PC › Storage and Server › Storage: ROMs, saves, snapshots, cores, backups, screenshots; clean-up suggestions) (S)
- [x] Done (PR #217): Per-game overrides for core options, layout and controller profile (builds on the Game override page of 0.10) (M)
- [x] Done (PR #212): NDS and 3DS screen layouts: hotkey switch, bottom screen in its own window or on a second monitor (M)

### 0.19.x Readable UI pass

Decided by Fabio on 2026-10-26 (package B of the UI scan after 0.19): the app takes the readable revision r6 of the design system, spec [readable-pass.md](design/uzume/readable-pass.md), decisions in [decisions.md](design/uzume/decisions.md#sheet-6--readable-pass-2026-10-26). Built before 0.20 in one thread, PRs one after another (plan PR #223); it adds no function. Released as 0.19.x betas.

- [x] Done (PR #224): Tokens and controls: r6 tokens in `Theme.qml` (`ruleControl`, `badge`, `markerNav`, raised `dim`, `selected`), control borders at 3:1 or more, Toggle off with a filled knob, neutral selection, warnings as an ink sentence with the Yamabuki square, functional text at least 11 px Mono / 12 px Sans, accessible names on toggles, selects, fields and "×" buttons
- [x] Done (PR #226): Text size and Reduce motion: scale 100 % / 125 % / TV from one Theme factor, Settings › This PC › Appearance rows, Reduce motion follows the system until changed
- [x] Done (PR #227): Keyboard and gamepad everywhere: second columns as focusable `ColumnNav` rows (Settings, Emulation, Controllers, admin), binding fields, screenshot strip, Continue tile and panel rail; B / Esc goes one level back; `UzHintRow` on every main page after pad input; a test that walks each page by keys only
- [x] Done (PR #230): Layout and wide font: one `UzPageHeader` on every page with actions that wrap below the title, dialog button rows that wrap and stack, at most three columns below 1440 px (help columns and input tests as disclosures, decoration only at 1440 px and wider), wide-font tests in CI at 16 to 18 px letter spacing
- [x] Done (PR #231): Library: status filter chips with counts, List / Grid switch, game detail with one problem sentence whose fix becomes the primary, start checklist only while starting or after a failure, Saves with the start actions on top and File details closed, Remove from library with Undo
- [x] Done (PR #233): Running game: header with Pause / Resume as the primary and Fullscreen back, plugin tabs with icon, short label and More, Diagnostics with a Details section (encoder, bitrate, per-person path, RTT, loss, relay reason), Diagnostics in the hold-Select sheet
- [x] Done (PR #234): Settings, controllers and admin: nav badges (Status, Systems & cores, Settings), mapping table with the key shown once and Default / Reset only on changed rows, Systems & cores and Plugins only in the nav (Settings links there), docs closed (features, history)

Not built: the panel tabs Run · Chat · Saves of board 3g-1-r6 (Chat comes with 0.23; the first tab stays Session). Qt has no system "reduce motion" hint, so Reduce motion is off until the user turns it on.

Tests only Fabio can do: the gamepad on the shell pages with a real pad, Text size TV on a TV, the wide Windows font, Kinari and Sumi visually.

## Decisions from the roadmap

Decided questions that used to be listed as open decisions in the roadmap.

- **Second system:** decided on 2026-10-07: Nintendo 3DS with Azahar (see 0.10). The earlier proposal GBA with mGBA is dropped.
- **Internet reachability:** decided in 0.4 (ADR 0012); the text below is the original proposal. Which setups FrameBeam supports and documents (Hub port forward, VPN, own TURN server on the Hub host or elsewhere). Proposal (2026-10-07): the Hub embeds an optional STUN/TURN server (for example `pion/turn`, pure Go, same binary) on UDP/TCP 3478 plus a small UDP relay port range, hands out short-lived TURN credentials in `hello_ack`/join instead of static secrets, and learns its public address from a configured hostname (DynDNS). A typical setup: a DynDNS name pointing at the Hub's router, port forwards for the Hub port, 3478 and the relay range; check first that the connection has a public IPv4 (no CGNAT/DS-Lite).
- **Update channels:** decided on 2026-10-06: the pre-release channel (renamed from test to beta on 2026-10-07) updates automatically, the stable channel only after confirmation. Details and rollback proposal: [ADR 0011](adr/0011-automatic-updates.md). Numbering, "Promote to release" and the default channel changed on 2026-10-08: [ADR 0016](adr/0016-release-numbering-and-promotion.md).
- **0.2:** decided in ADR 0010 (source, index, signing tooling, offline import, license file per package, cached cores stay usable without a Hub connection to GitHub). The FrameBeam release key exists since 2026-10-07.
- **Core sourcing:** decided on 2026-10-09: libretro buildbot cores downloaded by the Hub, no FrameBeam mirror ([ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md), see 0.8). The 2026-10-07 mirror plan is dropped.
- **1.0 goal, platforms and order:** decided by Fabio on 2026-10-11 (see "Goal 1.0"): SoulLink run in Pokémon Omega Ruby/Alpha Sapphire; Windows and Linux only, macOS dropped; Linux Player and metadata after 1.0; milestone order 0.10.x, 0.11 to 0.15, 1.0; on 2026-10-12 Fabio reordered 0.13 to 0.15 (system template, UI gaps and Multiview, GPU paths).
- **Release notes language:** decided on 2026-10-21: English.
