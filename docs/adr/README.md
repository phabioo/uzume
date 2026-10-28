# Decisions (ADRs)

Architecture decision records: one file per decision with context, decision, rejected options and consequences. They are historical records and are not rewritten; a later ADR supersedes an earlier one. Old ADRs use old milestone numbers and the old product names (FrameBeam Hub, Player): see [Renumbering history](../history.md#renumbering-history). Architecture files in [`docs/architecture/`](../architecture/README.md) are not rewritten either; deviations are recorded here.

New ADR: `docs/adr/NNNN-title.md` with status, date, deciders, context, decision, rejected options and consequences, then a row in this table.

| ADR | Topic | Status |
|---|---|---|
| [0001](0001-stack-additions.md) | Stack additions: Added libraries and tools beyond the architecture | Accepted |
| [0002](0002-protocol-and-hub-phase1.md) | Protocol and server, phase 1: OpenAPI, WSS schemas, first server | Accepted |
| [0003](0003-player-phase2.md) | App, phase 2: First playable vertical slice | Accepted; storage location superseded by 0004, audio plan by 0007 |
| [0004](0004-player-portable-data.md) | Portable data directory: Where the app keeps its data | Accepted |
| [0005](0005-saves-phase3.md) | Saves, phase 3: Save sync, versions, conflicts | Accepted |
| [0006](0006-sessions-phase4.md) | Sessions, phase 4: Signaling, WebRTC, Multiview | Accepted |
| [0007](0007-phase5.md) | Phase 5: Users, firmware, settings pages, gamepads, installer | Accepted; Inno installer superseded by 0021 |
| [0008](0008-windows-ci-cache.md) | Windows CI cache: Windows CI cache and release triplet | Accepted |
| [0009](0009-finish-poc.md) | Finish the PoC (0.1.1): Close PoC leftovers | Accepted |
| [0010](0010-cores-from-the-hub.md) | Cores from the server (0.2): Core distribution by the server | Accepted; partly superseded by 0020 |
| [0011](0011-automatic-updates.md) | Automatic updates (0.3): Update channels, signed index, rollback proposal | Accepted; Inno update path partly superseded by 0021 |
| [0012](0012-internet-sessions-and-save-comfort.md) | Sessions over the internet and save comfort (0.4): STUN/TURN, relay, save retention and restore | Accepted; core index check superseded by 0020 |
| [0013](0013-opengl-hardware-rendering.md) | OpenGL hardware rendering (0.5): OpenGL context for libretro cores | Accepted |
| [0014](0014-player-ui-pass.md) | App UI pass (0.6): Design tokens, diagnostics | Accepted |
| [0015](0015-hub-ui-pass.md) | Server UI pass (0.7): Web UI pass (web admin removed in 0.18) | Accepted |
| [0016](0016-release-numbering-and-promotion.md) | Release numbering and promotion: Beta numbering, promote to stable | Accepted |
| [0017](0017-player-tech-debt.md) | App tech debt (0.7.x): Session visibility, core version check, controller split | Accepted |
| [0018](0018-player-speed-up.md) | App speed-up: Fast-forward | Accepted |
| [0019](0019-gpu-direct-nvenc.md) | GPU-direct NVENC encoding: No readback while sharing on NVIDIA | Accepted |
| [0020](0020-cores-from-the-libretro-buildbot.md) | Cores from the libretro buildbot (0.8): Systems and cores from the buildbot | Accepted |
| [0021](0021-one-windows-installer.md) | One Windows installer (0.9): MSI with app and server | Proposed (acceptance after local installer test) |
| [0022](0022-nintendo-3ds-with-azahar.md) | Nintendo 3DS with Azahar (0.10): Second system | Proposed (acceptance after a real Azahar run) |
| [0023](0023-plugins.md) | Plugins (0.11): Lua plugin runtime, UI blocks, runs, signing | Proposed (parts implemented) |
| [0024](0024-system-template-and-game-boy.md) | System template and Game Boy family (0.13): Systems as data, GB/GBC/GBA | Proposed (acceptance after real-core test) |
| [0025](0025-local-roms.md) | Local ROMs (0.12): Library folders, identity merge, upload | Accepted |
| [0026](0026-ui-gaps-and-multiview.md) | UI gaps and Multiview (0.14): Round 3 gaps, Multiview layouts | Accepted |
| [0027](0027-gpu-paths.md) | GPU paths: Zero-copy display, GPU-direct QSV and AMF | Accepted |
| [0028](0028-release-notes-and-changelogs.md) | Release notes and changelogs: PR changelog sections, `CHANGELOG.md` | Accepted |
| [0029](0029-operations-and-safety.md) | Operations and safety: Docker, backup, status, rollback, problem reports | Accepted (D1 to D5; more follow) |
| [0030](0030-one-app-one-server-uzume.md) | One app, one server, new name Uzume (0.18): Rename, no web admin, accounts, server commands | Accepted |
| [0031](0031-game-variants-and-randomizer.md) | Game variants and the randomizer (0.21): Variants with own save slots, tools catalog | Accepted |
| [0032](0032-debug-mcp-server.md) | Debug MCP server in the app: localhost, opt-in, token, tools to read and control | Accepted |
| [0033](0033-together.md) | Together (0.23): presence, notifications, ready check, chat, viewer quality, mixer, voice chat, controller hand-over | Accepted for implementation |
| [0034](0034-linux-app-and-vulkan.md) | Platforms (0.24): Linux AppImage with self-update, Secret Service, Vulkan path | Accepted |
| [0035](0035-error-groups-and-github-issues.md) | Error groups and GitHub issues: grouped errors on the own server, one-click prefilled issue, issue templates | Proposed |
