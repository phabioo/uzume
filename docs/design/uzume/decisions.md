# Uzume design: decisions and open points

What the designer decided on the index boards and what was accepted in the review of each batch (all dated 2026-10-22, Round 5 restyle, final). Status words: **Accepted** (decided, build as drawn), **Resolved** (the design system gained the missing part, boards use it), **Open** (no decision or not drawn yet; do not invent). The overview is in [README.md](README.md).

Boards summarised here: [`SAllIndex`](../source/uzume/SAllIndex.dc.html) (index of all four sheets, 293 boards, 21 open gaps all accepted, list of closed items); the sheet indexes and the "Later" list of `S04H` are summarised in the sections below.

Older decisions of the FrameBeam Rounds 1 to 4 are in [../decisions.md](../legacy/decisions.md); they stay valid only for behaviour the Uzume boards do not cover.

## Open points

Everything below is the complete list of what is still open. The 21 "did not fit the design system" gaps are all *accepted with a workaround*, so nothing blocks the build.

| # | Open | Source | Note |
|---|---|---|---|
| 1 | Network discovery of servers | `S04H` Later 01 | Not drawn; joining is by address, code or `uzume://` link. |
| 2 | Invites into a run | `S04H` Later 02 | Not drawn. |
| 3 | "Replace file..." for games | `S04H` Later 03 | Not drawn. |
| 4 | "New users need: Approval" | `S04H` Later 04 | Joining stays invite only; approval is only for new devices of existing accounts. |
| 5 | Watching in the browser (7b) | `S04H` Later 05 | Dropped (Fabio, 2026-10-22): no browser components in Uzume. |
| 6 | "Keep the previous core version" | `S04H` Later 06 | Not drawn. |
| 7 | Automatic import-folder scan | `S04H` Later 07 | Rescan by hand only. |
| 8 | Rule "apps within one minor version keep working" | `S04H` Later 08 | Not drawn; the "too old / too new" states (8b-5f, 8k-2) show the current behaviour. |
| 9 | Reachability test run by the server itself | `S04H` Later 09 | Only the app's Connection check from another network. |
| 10 | Web page for `uzume://` links when the app is not installed | `S04H` Later 10 | The link does nothing without the app. |
| 11 | Handheld install and Linux installer screens beyond 8m | `S04H` Later 11 | Not drawn. |
| 12 | Pixel icons (32 / 16 px tiles, title bar, tray, ICO) | `SAllIndex` 02 | Named by id, not drawn; need Pixel assets before the installer (package 11) and tray are final. |

The Round 4 behaviours that the final boards deliberately drop are not open: Hub web screens (3j-3, 6j, 6k, 5ah, 5gh, 7b) are skipped because Uzume has no web admin.

## Design decisions (Step 1b and the design system)

| Decision | Status |
|---|---|
| Direction A "Ma" in the version 0-5b: space instead of boxes, hairlines, register marks, open right column. | Accepted |
| Accent **Yamabuki** `#f8b500` (over Kuchinashi, Kin, Tamago): a plane with Sumi text; link text on Kinari is Uguisu-cha `#715c1f`, on Sumi Yamabuki itself. | Accepted |
| Warning = accent (Yamabuki square with "!" plus a sentence); danger = muted red (Sango-shu dark, Enji light), outlined buttons and square icons only; ok = green 6 px square. No cyan, no amber, no tinted banners. | Accepted |
| No rounded corners, no shadows, no cards; the app icon tile (22.5 %) is the only radius; the ensō only in empty and waiting states. | Accepted |
| Logo **03a "Rise"** (U with a rising right arm and a light point); the play-triangle Player mark stays retired; the server glyph only for installer, tray and server About. | Accepted |
| 17 named tokens for Sumi and Kinari (now 31 colour tokens in `tokens.json`); both themes apply to the one app, the old split "Player dark / Hub light" is gone. | Accepted |
| Rules: 8 px grid, row heights 32 / 44 / 48, margins 56, open right column 152 with vertical Mono label. | Accepted |

## Sheet 1 · Frame and first start (Index items 01 to 22)

Review of batch 1: all renames done, only the five systems, passwords at least 12 characters, security code in the invite link, no private hosts.

| # | Item | Outcome | Status |
|---|---|---|---|
| 01 | Cards, radii, shadows | Forms sit on the ground; sections split by space, label and hairlines. | Accepted |
| 02 | Cyan and amber | Signal Cyan became Yamabuki, only on things you can act on; warnings are the "!" square. | Accepted |
| 03 | Tinted banners | StatusIcon + sentence; info is a plain ink-2 sentence. | Accepted |
| 04 | Round gamepad pills | `GamepadHint`; Show password is on X because R3 was not a hint key then. | Accepted |
| 05 | Server glyph in rows | Rows are text plus StatusLine; glyph only for installer, tray, server About. | Accepted |
| 06 | AppNav extras (member view, THIS PC, ONLINE, bell, menus, compact) | The system AppNav has them; every app screen uses compact. | Resolved |
| 07 | Toggle switch | System `Toggle`. | Resolved |
| 08 | Select and open menus | System `Select` and `Menu`; the "Who can join" choice is a Select. | Resolved |
| 09 | SettingsRow | System `SettingsRow` and `SettingsDisclosure`. | Resolved |
| 10 | AppBlocks | Ma blocks (metrics, storage bar, lanes) on sheet 2; the dashed empty box is a `StateBlock` with the ensō. | Resolved |
| 11 | Avatar colours | Person tokens in join order: Max wasurenagusa, Lena momo, Jonas moegi, Sam fuji. | Accepted |
| 12 | Code format | Security codes `K7Q4-9WMP-2HXD` (groups of four, hyphens); the field accepts spaces, hyphens or none. | Accepted |
| 13 | Invite code prefix | **`UZ-`** (e.g. `UZ-8XK2-M4QD`), decided by Fabio. The server accepts `FB-` and `UZ-` so open invites stay valid. | Accepted |
| 14 | System codes | Only GB, GBC, GBA, NDS, 3DS. | Accepted |
| 15 | Hosts | `192.0.2.20:8443` (documentation range, like `192.0.2.10`); home server `server.example.com:8443`. | Accepted |
| 16 | Library details panel | Numbered admin section under the table (no 320 px panel). | Accepted |
| 17 | Settings second column | Text items with the leading Yamabuki marker (not a component). | Accepted (gap) |
| 18 | Sizes | Everything 1280 x 800; 8a-2 is the keyboard version; 8b-5 and 0-4 C are 1040 high. | Accepted |
| 19 | Light check 8b-6 | Every screen has a Kinari twin; 8b-6 is the keyboard sign-in. | Accepted |
| 20 | Tray states | Yamabuki lit (running), dim (stopped), "!" square (needs attention). | Accepted |
| 21 | Pixel logo sizes | 32 / 16 px marks are Pixel assets kept by id. | Accepted (gap, open asset) |
| 22 | tokens.css | Now the system's own generated file. | Resolved |

## Sheet 2 · Accounts and server (Index items 01 to 16)

Review of batch 2: names, ports (8443 TCP, 3478 UDP+TCP, 49160-49199 UDP), cores from the buildbot with nothing preselected, removed accounts keep their saves ("Jonas (removed)"), deleting someone else's saves only explicitly with Confirm it's you.

| # | Item | Outcome | Status |
|---|---|---|---|
| 01 | Dialog | System `Dialog` with actions, danger action and hints. | Resolved |
| 02 | Help column | System `HelpColumn` in the open column. | Resolved |
| 03 | Most urgent problem | Warning-word badge on Status in AppNav ("Backup disk"). | Resolved |
| 04 | Checkbox | System `Checkbox` (plugin permissions in 4i-3a). | Resolved |
| 05 | Second columns | Text items with the marker. | Accepted (gap) |
| 06 | Users sub-pages | `Tabs` People · Invites · Devices instead of a 300 px column. | Accepted |
| 07 | Detail panels | Numbered sections under the table. | Accepted |
| 08 | Storage colours | Five hues became ink tints (72 / 52 / 36 / 24 / 14 %) with a value list. | Accepted (gap) |
| 09 | Lanes | Hairline tracks with square Mono glyphs (drawn in S2 Blocks, used in 4jh-app-2). | Accepted (gap) |
| 10 | Status words | `Tag`s: New / Seen, Official / Not signed, Conflict / Removed, Matches / Missing. | Resolved |
| 11 | Keyboard keys | System `Kbd`. | Resolved |
| 12 | Avatar sizes | Only 20 / 28 / 40; tiles in 8d use 40 with the name below. | Accepted (gap) |
| 13 | Top padding | Admin pages use 48 px so tables fit in 800 px. | Accepted |
| 14 | Sizes | 3j-app-2 is 1040 high; 6a-app-1 and 7a-app are 1280 x 800. | Accepted |
| 15 | Info banners | Ink-2 sentence or StatusIcon + sentence with the action beside it. | Accepted |
| 16 | Link colour | Comes from tokens.css (`accent-text`). | Resolved |

Review fixes for batch 2: `UZUME_TURN` only switches the built-in relay on or off, external TURN servers are only in `UZUME_TURN_EXTERNAL` (3q-app-2 shows `UZUME_TURN_EXTERNAL turn.example.com:3478`, not `UZUME_TURN external`); `uzume-server status` is a server command (list: `setup-code`, `code`, `reset-admin`, `status`, `update`, `backup`, `restore`, `reset`). Both applied (Accepted).

## Sheet 3 · Runs and server (Index items 01 to 15)

Review of batch 3: runs always in Play with a Rules column (mode as neutral Tag, dash for plain runs), own save slot "Run: <Name>" and Main untouched, 1 to 8 people, members may start runs; setup code single use and valid 60 minutes; Docker image `ghcr.io/<owner>/uzume-server` with all ports and `/data`; stopping sharing keeps all accounts and saves; installer paths match ADR 0021 (per-user app under `AppData\Local\Programs`, all users under `Program Files`, server data under `ProgramData\Uzume\server`), uninstall keeps server data.

| # | Item | Outcome | Status |
|---|---|---|---|
| 01 | Terminal | Stage panel in plain Mono; emphasis is weight only. | Accepted (gap) |
| 02 | MSI chrome | Windows draws the installer; boards draw it with Uzume tokens at 1:1, always light; only the two bitmaps and the copy ship. No Sumi twins for 8l. | Accepted |
| 03 | MSI size | Client area 493 x 360: the 493 x 312 bitmap sits above the 48 px button row (matches the `.wxs`: dialog 370 x 270 DLU, rule at Y = 234). | Accepted |
| 04 | Small icons | ICO from the Pixel 32 / 16 tiles, named not drawn. | Accepted (gap) |
| 05 | Banner tile | 58 px banner shows Mark (36) + ServerGlyph (20) side by side. | Accepted (gap) |
| 06 | Lane glyphs | ■ □ ▣ ◆ ✕ instead of round glyphs (nothing round but the ensō). | Accepted |
| 07 | Run tabs | `RunTabs` only in the in-game panel (4j-app-7); run pages use `Tabs`, plugin tab last. | Accepted |
| 08 | Plugin surface | A Mono "Drawn by SoulLink" line over a rule marks plugin content. | Accepted (gap) |
| 09 | Rules chip | Neutral `Tag` with the mode; Yamabuki tags only for New. | Accepted |
| 10 | Run column | No 300 px run list beside a run; details are a section under the Runs table. | Accepted |
| 11 | Shield | Outlined placeholder for the Windows shield on admin-rights buttons. | Accepted (gap) |
| 12 | Progress dialog | 8i-3 is a plain Dialog with a disabled primary "Sharing..." (the busy Dialog dims content). | Accepted (gap) |
| 13 | Old names | The upgrade dialog (8l-10) names FrameBeam Player and Hub, the names Setup finds on old PCs. Not built: FrameBeam installs are not upgraded or migrated, they are replaced by a fresh install ([ADR 0030](../../adr/0030-one-app-one-server-uzume.md)). | Accepted |
| 14 | Light check | Every board except 8l has a Kinari twin. | Accepted |
| 15 | Board size | 8l-1 to 8l-3 are 1040 high to fit four dialogs at 1:1. | Accepted |

The designer's four decisions in batch 3 (run tabs in game 4j-app-7, light-only MSI boards, 493 x 312 bitmap above the button row, old names in the upgrade dialog) were accepted. Fix applied: there is **one** run plugin, "SoulLink & Nuzlocke"; SoulLink or Nuzlocke is a mode chosen in its rules (Rules `Select`: None and the plugin; Mode `Segmented`; the Rules tag shows the mode).

## Sheet 4 · Play and plugins (Index items 01 to 18)

The designer's five decisions in batch 4 were accepted: (1) Toasts at the bottom left instead of the Round 4 top-centre OSD; (2) the F3 diagnostics block in the top right corner (the system got a component); (3) panel tabs Session · People · Chat · Saves and in a run Run · Chat · Saves · plugin; (4) invented content for Controllers, Game data in the plugin settings and Multiview is accepted; (5) Hub web pages are skipped (no web admin); Privacy rows go to 8e and the developer console (4d) is drawn.

| # | Item | Outcome | Status |
|---|---|---|---|
| 01 | OSD | Toasts bottom left of the stage; persistent ones (Paused, Rewinding) drop the close mark. | Accepted |
| 02 | Diagnostics | F3 uses the GameStage diagnostics block, top right; hold Select uses its hotkey sheet. | Resolved |
| 03 | Panel tabs | RunTabs Session · People · Chat · Saves; states live in Saves; diagnostics left the tabs for F3. | Accepted |
| 04 | Plugin tab name | The plugin tab shows the run's mode (SoulLink or Nuzlocke). | Accepted |
| 05 | Slider | Volume, voice and the Multiview mixer use `Slider`. | Resolved |
| 06 | Popovers | Volume, the bell and the map's route card are `Popover`s. | Resolved |
| 07 | Pause overlays | `Dialog` over a scrim on the stage only; RunHeader stays visible. | Accepted |
| 08 | Library tiles | The Round 4 tile grid became the table; favourites are a `FilterChip` plus a Favourite button. | Accepted |
| 09 | Per-game sheet | "Settings for this game" is `Dialog` lg. | Accepted |
| 10 | Creature sprites | Placeholder boxes with two letters; the plugin draws real sprites from the game file; HP is a thin `Progress`. | Accepted (gap) |
| 11 | Region map | Route points and hairlines on the stage; land comes from the game file and is not drawn. | Accepted (gap) |
| 12 | Speaking mark | `Avatar` and `MultiviewTile` show speaking, mic off, muted by host. | Resolved |
| 13 | Pad keys | R2, L2, R3 and the D-pad are `GamepadHint` keys; Round 4's R1 / L1 became R / L. | Resolved |
| 14 | Controllers | Built from the AppNav item, the pad map and the hotkey rules (not in the old files). | Accepted (gap) |
| 15 | Game data | Built from the map notes (read from the game file, stays on the PC); placed in the plugin's settings. | Accepted (gap) |
| 16 | Privacy rows | Privacy section in My account (8e-1, 8e-2): Show what I play and Discord status, both `Toggle`, off by default. | Resolved |
| 17 | Skipped | Developer console is 4d-1 and 4d-2; Hub web screens stay out. | Resolved |
| 18 | Stream view | 1920 x 1080 transparent overlay shown at 61 % on the stage with a dashed frame. | Accepted (gap) |

Review fixes for batch 4 (all applied in the final boards): Select + ↑ ↓ is volume, so the plugin's "Split" cannot take Select + ↓ (hotkeys page shows the conflict with a "Moved" tag and gives the plugin Select + ←); the microphone is Select + R3 everywhere (Voice, Hotkeys); the map hint reads "L / R zoom"; the quick menu (3g-4) shows its own content (states, screenshot, per-game settings, reset, together, help). New design-system parts requested and delivered: Slider, Popover, speaking mark, extra GamepadHint keys, stage overlays (diagnostics block, hold-Select sheet).

## Sheet 5 · Randomizer (batch 5, 2026-10-23)

Canvas page "5 Randomizer", 26 screens 10a to 10h with Kinari twins and eight handoff sheets, no new tokens; spec [randomizer.md](randomizer.md), decision record [ADR 0031](../../adr/0031-game-variants-and-randomizer.md). Fabio accepted the designer's seven decisions on 2026-10-23:

| # | Decision | Status |
|---|---|---|
| 1 | Revisions of existing screens stay in the 10 family and name the board they revise (10a-1 revises 3c-1). | Accepted |
| 2 | The library keeps one row per game with a variant count ("3 VARIANTS"); variants open from the game page and the slot picker. | Accepted |
| 3 | Deleting a variant defaults to Keep saves; the recipe stays with the kept saves. | Accepted |
| 4 | With "Same world for everyone" nobody sees the seed. | Accepted |
| 5 | The spoiler log is off by default, opens only from a variant's menu after a confirm, and for run variants only after the run ends and only the own one. | Accepted |
| 6 | Own variants join a run only with the run's engine version. | Accepted |
| 7 | "FVX" appears only in 10 px Mono lines; copy says "the engine". | Accepted |

Gaps in the design system, accepted as the index describes them: Tools in AppNav after Systems & cores; a step list built from `StatusIcon`, `Enso` and `Progress` (new app part); table row focus with `--focus-ring-inset` (needed for the gamepad); save versions as table rows; a closed `Select` inside a dialog; toast width inside state panels (also fixed on 3g-11, 3m-2, 3s-4, 4d-2, 5b-2 and 8e-5, whose boards were re-imported with this batch).

Review fixes (applied in the canvas before the import): a different dump offers "Download the run's file · 1.8 GB" or "Ask Max to upload the run's file." instead of "Upload mine…", shown with an NDS game because 3DS variants depend only on title and update (10d-1d, 10f-5a); 10d-4 has seven panels, the run variant of a game update (10d-4g) stays on the pinned update with no rebuild button, the own variant (10d-4f) warns "The world may change; your save stays in its slot." Two review notes need no board change: adding a 3DS update on the server (10b-3d) needs the server-side 3DS updates of 0.21 (ADR 0031 D6), and the ready check (10f-5e) comes with 0.23; before that the run board shows Building.

## Sheet 4 · Cheats (family 11, 2026-10-23)

Canvas page "4 Play and plugins", 19 boards 11a to 11h with Kinari twins (11e and 11f linked on sheet 3), no new tokens; spec [cheats.md](cheats.md), built in 0.20. Fabio accepted the designer's four decisions (marked New on `S4Index`) on 2026-10-23:

| # | Decision | Status |
|---|---|---|
| 1 | The run rule is drawn on a plain run (Rally Night on Harbor Rally) started by Lena, a member: the run's creator and admins change the rule. | Accepted |
| 2 | "Cheat database" is an item after Updates in Server › Settings (3q); Systems & cores (3l) is a page, not a settings list. Without a server the block is in Settings › This PC. | Accepted |
| 3 | "Before cheats" is saved each time the first cheat of a play session goes on; consent is asked once per game. | Accepted |
| 4 | Turning cheats off in a run uses the full confirm dialog (it changes other people's games); allowing them a lighter confirm. | Accepted |

Gaps in the design system (items 28 to 32 on `SAllIndex`): a multi-line Mono code field with line numbers and per-line errors (planned as the `textarea` building block in 0.20); five panel tabs fit only without the Chat count; `StateBlock` has one action, so the empty Cheats tab draws two buttons; no disabled list container (shown at 55 % with `aria-disabled` under a notice); a board lives on one canvas page, so 11e and 11f are linked copies on sheet 3.

Review fixes (applied in the canvas before the import): the unsupported example on 11a-1, 11a-3b and 11b-3 is `RAW` instead of `AR V3` (mGBA runs Action Replay MAX codes but no raw codes); the consent on 11d-1 says Main v14 "is kept as" "Before cheats" (a copy, not a rename).

## Sheet 6 · Readable pass (2026-10-26)

Canvas page "6 Readable pass", 13 boards with the index `S6Index`, drawn in design system revision r6; spec [readable-pass.md](readable-pass.md), values in [design-system.md](design-system.md). It follows the UI scan after 0.19 and adds no function. Fabio accepted the revision and all decisions on 2026-10-26:

| # | Decision | Status |
|---|---|---|
| 1 | Yellow is for acting only (one primary per view, focus ring, active nav and tab marker); selection is neutral (`ink` plane or `selected` tint with the `ink` marker). | Accepted |
| 2 | A warning has no hue of its own: an `ink` sentence with the Yamabuki square, with its fix action next to it. | Accepted |
| 3 | The library shows status filter chips with counts (All, Ready, Needs attention, Not downloaded) at all times. | Accepted |
| 4 | The library has a List and a Grid view. | Accepted |
| 5 | A problem is shown once; when it blocks the main action, its fix becomes the primary ("Install core and play"). | Accepted |
| 6 | Saves: "File details" (SHA-256, path, size) are closed by default. | Accepted |
| 7 | Running game header: Pause / Resume is the primary, Fullscreen is back as an icon button, the rest is in `⋯`. | Accepted |
| 8 | Plugin tabs show an icon and a short label; overflow goes into "More". | Accepted |
| 9 | Diagnostics has a visible Details section (encoder, bitrate, per-person path and RTT); the hold-Select sheet gains Diagnostics. | Accepted |
| 10 | At most three columns below 1440 px, AppNav included; help columns and inline tests become disclosures. | Accepted |
| 11 | The controller mapping table shows the bound key once, plus "Default: …" and Reset only on changed rows. | Accepted |
| 12 | Settings › This PC › Appearance gains Text size (100 % · 125 % · TV) and Reduce motion. | Accepted |
| 13 | Nav rows carry neutral badges for problems (Status, Systems & cores, Settings). | Accepted |
| 14 | With a wide font, page header actions wrap below the title and dialog buttons stack inside the dialog. | Accepted |
| 15 | Every main page shows the `HintRow` while the last input was a pad; second columns are `ColumnNav` rows reachable with the D-pad. | Accepted |

New in the design system (r6): tokens `rule-control`, `badge`, `marker-nav`, a raised `dim`, text size scale tokens, `target-min` 32 / `target-tv` 44, `columns-compact-max` 1440; components `HintRow` and `ColumnNav` (45 components). Where a sheet 6 board and an older board disagree, sheet 6 wins.

## Sheet 7 · In-game redesign (2026-10-26)

Canvas page "Uzume 7 In-game redesign" (boards 7-1 to 7-6); it lives as a Claude design artifact and is not stored in the repository. It follows the readable pass for the running game and adds no function. Fabio accepted on 2026-10-26:

- Every control of the game header has a frame and shows its value ("» Speed-up OFF · 2×", "Sound 5 %", "Screens <layout>", Panel Shown / Hidden); Pause / Resume stays the one primary. Narrow windows drop words before values, then move Fullscreen and Screens into "⋯".
- Esc, Pause and Start open a Quick menu (Resume, Game / Play / Together / Help, Quit game…) instead of the pause dialog; pad A chooses, B resumes.
- The panel's Session tab becomes "Game" (Now rows, Snapshot / Screenshot / Share… / Saves…, Sharing, Quit pinned at the bottom); the collapsed rail names its tabs.
- "Settings for this game" is a dialog around a reusable content block that can also sit in a panel; it shows the live screen layout and speed-up of the running game.

Behaviour: [features.md](../../features.md) ("In-game control bar, Quick menu and Game tab"). Where a sheet 7 board and an older board disagree, sheet 7 wins.

## Cross-sheet rules (summary)

- **Terms.** Hub = "Uzume server" or "the server", Player = "Uzume" or "the app"; "Stream" only in diagnostics. Session = a running or shared game session.
- **Plugins.** IDs `uzume.soullink-core` and `uzume.soullink`; one run plugin "SoulLink & Nuzlocke" with a mode; plugin screens are run tabs (in game: after Run · Chat · Saves; on this PC: Settings › This PC › Plugins); no plugin entries in the sidebar.
- **Saves.** Run slot "Run: <Name>"; Main untouched; states are not the save; restore keeps the current version as "Before restore"; removing a user keeps their saves; saves are never overwritten silently and never leave the server.
- **Web.** No web admin and no other server web page (Fabio, 2026-10-22): watching in the browser is dropped, and the OBS stream view (5g) is not built as a web page (OBS captures the app window; an app overlay window is open).
