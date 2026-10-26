# Uzume design (Round 5 restyle, final 2026-10-22)

The valid design of Uzume from milestone 0.18 on: the app (formerly FrameBeam Player) and the Uzume server (formerly FrameBeam Hub) with the admin area inside the app, drawn in the design system "Uzume" (direction Ma, themes Sumi dark and Kinari light, IBM Plex). It supersedes the FrameBeam design Rounds 1 to 4 ([../README.md](../README.md), legacy reference) in look and in every screen it draws. Decisions and the review of each batch: [decisions.md](decisions.md); decision record for the milestone: [ADR 0030](../../adr/0030-one-app-one-server-uzume.md).

Read only the part you need: the design system once, then the spec of the one screen you build.

## Source

The Claude Design handoff is stored unchanged in [`../source/uzume/`](../source/uzume/):

- `canvas.json`: every board in a `boards` map (file, title, size, page) and the `pages` b0 to b6 ("0 All sheets", "1 Frame and first start", "2 Accounts and server", "3 Runs and server", "4 Play and plugins", "5 Randomizer", "6 Readable pass"; the sheet 7 canvas "Uzume 7 In-game redesign" is not in the repo, see [decisions](decisions.md#sheet-7--in-game-redesign-2026-10-26)).
- `*.dc.html`: the boards. A dark Sumi board `SXxx.dc.html` has a light Kinari twin `SXxxKinari.dc.html` that imports it, so content is identical in both themes (exception: the Windows installer boards 8l are light only; the 0-4 frame boards A, B, C, F, G, H show both themes on one board). Index boards: `SAllIndex`, `Main` (sheet 1), `S2Index`, `S3Index`, `S4Index`; `S2Blocks` is the Ma blocks reference.
- `ds/uzume/`: the design system. `tokens.json` is the source of truth, `tokens.css` is generated from it, `components/bundle.css|js` are the reference components, `fonts/` holds IBM Plex (license in `licenses/`).

The boards need the Claude Design runtime `support.js`, which is not in the repository, so they do not render standalone (as with the older sources). Read them as text: the specs in this folder were extracted from the board HTML (copy, structure, states, actions); when a spec and a board disagree, the board wins and the spec should be fixed.

## How to read a board

- **Names.** `8a-1` is sheet part 8a, screen 1. A suffix `-app-` marks an admin screen that moved from the Hub web into the app (`3j-app-1`). Board titles end in "Sumi" (dark) in the canvas list.
- **Size.** 1280 x 800 unless the spec says otherwise (taller state sheets are 1040; the sheet index boards are 1480 to 2240).
- **States boards.** A board titled "... states" or "... other states" has six small panels labelled `8b-5a`, `8b-5b` ... Their copy is the exact text for that state.
- **Mock data.** Names (Max, Lena, Jonas, Sam), games (Harbor Rally, Lumen Drift, Coral Saga: Ember ...), creatures, versions, devices and times are illustrative. Hosts are placeholders: `server.example.com:8443`, `192.0.2.10`, `192.0.2.20` (documentation range). Systems are always GB, GBC, GBA, NDS, 3DS.
- **Hints.** The bottom hint row shows `GamepadHint` glyphs when the last input was a pad and `Kbd` keys otherwise; specs list both where the board does.

## Specs

| Spec | Sheet and boards | Boards (Sumi) |
|---|---|---|
| [design-system.md](design-system.md) | Tokens (colours for Sumi and Kinari, person colours, spacing, sizes, type, radius, focus ring, markers), the component list and layout rules | `ds/uzume/` |
| [frame.md](frame.md) | Sheet 1: 0-4 A to H frame decisions, 8a first start, 8b join a server, 8c invite link | `Main`, `S04A` to `S04H`, `S8a1` to `S8a6`, `S8b1` to `S8b6`, `S8c1` to `S8c3` |
| [accounts-and-server.md](accounts-and-server.md) | Sheet 2: 8d switch user, 8e my account, 8f confirm it's you, 8g forgot password, 6f status, 3j library, 3s saves, 3n users, 3m devices, 3l systems and cores, 4i plugins, 3q server settings, 6d reports, 6a what's new, 7a setup assistant, Ma blocks | `S2Index`, `S2Blocks`, `S8d1` to `S8g1`, `S6f1` to `S7a2`, `S3j1` to `S3q7` |
| [runs-and-server.md](runs-and-server.md) | Sheet 3: 4j runs, 4jh run board and recap, 8h what the server shows, 8i share with others, 8j on this PC, 8k server problems, 8l Windows installer, 8m Linux | `S3Index`, `S4j1` to `S4jh2`, `S8h1` to `S8m1` |
| [play-and-plugins.md](play-and-plugins.md) | Sheet 4: 3c library and game page, 3g running game, 3r Multiview, 3e Emulation, 9a Controllers, Settings › This PC (9b, 6g, 9c, 6l, 6w, 6r), 4d developer console, 5 SoulLink & Nuzlocke | `S4Index`, `S3c0` to `S3g12`, `S3r1` to `S3r5`, `S3e1`, `S3e2`, `S9a1` to `S6r2`, `S4d1`, `S4d2`, `S5a1` to `S5i1` |
| [cheats.md](cheats.md) | Sheet 4, family 11: 11a Cheats tab in game, 11b add or import, 11c cheat database, 11d consent, 11e and 11f run rule (linked on sheet 3), 11g save markers, 11h cheat database setting | `S11a1` to `S11h3`, linked `S11e1On3` to `S11f3On3` |
| [randomizer.md](randomizer.md) | Sheet 5: 10a game page · Randomize, 10b Randomize dialog, 10c building, 10d variant states, 10e New run with Randomizer, 10f join and play, 10g Library, slots and Storage, 10h Server › Tools | `S5Index`, `S10a1` to `S10h3`, handoff sheets `S10aH` to `S10hH` |
| [readable-pass.md](readable-pass.md) | Sheet 6: the readable revision r6 (yellow for acting, warnings as sentences, status filters, three columns, text size, gamepad hints) on 12 representative screens; wins over older boards | `S6Index`, `S3c1R6` to `S3c3R6`, `S3g1R6`, `S3g6R6`, `S9c1R6`, `S9a1R6`, `S6f1R6`, `S3n1R6`, `S0R6Pad1`, `S0R6Pad2`, `S0R6Kinari` |
| [decisions.md](decisions.md) | The designer's decisions, the accepted review decisions and the open points | `SAllIndex`, index boards |

`SAllIndex` (page "0 All sheets") is the index of all four sheets (293 boards) with the 21 accepted gaps in the design system; it is summarised in [decisions.md](decisions.md). Sheet 5 "Randomizer" (page "5 Randomizer", batch 5, 2026-10-23) was added later with its own index `S5Index`. The cheats family 11 (2026-10-23) was added to sheet 4, with 11e and 11f linked on sheet 3; `SAllIndex` now counts 391 boards and 32 open gaps. `S04H` is the Round 5 index with the "Later" list. Sheet 6 "Readable pass" (page "6 Readable pass", 2026-10-26) redraws 12 screens in design system revision r6 with its own index `S6Index`; it has no Kinari twins.

## Roadmap packages

Each screen entry names the package that builds it. Milestone 0.18 ("Uzume: one app, one server, new name", [roadmap](../../roadmap.md)):

| Package | Topic | Main screens |
|---|---|---|
| 3 | Design system in the app | tokens, components, AppNav (PLAY / SERVER / THIS PC), Settings This PC / Server, app icon, installer bitmaps (0-4, Ma blocks, 9c) |
| 4 | Server accounts | passwords, sign-in on new devices, PIN, confirm it's you, device list (8b-5, 3n, 3m, 3q-3) |
| 5 | Admin API | everything the web admin did, under `/api/v1/admin/...` (data behind sheet 2) |
| 6 | Server commands | `setup-code`, `code`, `reset-admin`, `status`, `update`, `backup`, `restore`, `reset` (0-4 G, 8g, 8h) |
| 7 | App start and accounts | 8a first start, 8b join, 8c invite link, 8d switch user, 8e my account, 8f, 8g |
| 8 | App admin area | sheet 2 admin screens, 8k server problems, What's new, setup assistant |
| 9 | Runs without a plugin | 4j runs, 4jh run board, run tabs, run slot "Run: <Name>" |
| 10 | On this PC and sharing | 8i share with others, 8j on this PC, Settings › This PC pages |
| 11 | Installer | 8l Windows installer, 8m Linux, 8h-3 |
| 12 | Remove the web admin | no web page stays; the OBS stream view (5g) is not built as a web page |

Later waves (build from the boards without their own design round): **0.19** player comfort and library (3c, parts of 3g, 9b, 6g, 6l, 3e, 9a), **0.20** emulation (save states, rewind, run-ahead, diagnostics, cheats: family 11, [cheats.md](cheats.md)), **0.21** randomizer (sheet 5, [randomizer.md](randomizer.md), [ADR 0031](../../adr/0031-game-variants-and-randomizer.md)), **0.22** plugins (developer console 4d, run recap, SoulLink screens), **0.23** together (People and Chat tabs, mixer, voice, notifications, Multiview marks), **0.24** platforms (Linux app, Vulkan; watching in the browser was dropped). Screens that only restyle an existing app screen fall to package 3; the "Built in" line names what each board adds beyond that.

## Terms and rules in one place

- Names: Uzume (the app), the Uzume server ("the server"), `uzume-server`, `uzume://`, `UZUME_*`, Docker image `ghcr.io/<owner>/uzume-server`, invite prefix `UZ-` (`FB-` stays valid). Say "server" only when the person must act on it.
- Admin is in the app: PLAY / SERVER / THIS PC groups, Settings with a This PC / Server switch for admins. No web admin.
- Runs are always visible in Play (also without a plugin) with the save slot "Run: <Name>"; plugin screens are run tabs; plugin IDs `uzume.soullink-core` and `uzume.soullink`; one plugin "SoulLink & Nuzlocke" with a mode.
- Saves are never overwritten silently; the design never shows real ROM, BIOS or firmware names.
