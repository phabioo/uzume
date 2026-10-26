# Readable pass (sheet 6, design system r6)

A usability review of the built app after 0.19 (2026-10-23) found that Uzume reads as more cluttered and harder to scan than the FrameBeam design, although almost no function is missing. The causes were in the system and in a few screens: control borders at 1.45:1, small `dim` text, yellow meaning action, selection and warning at once, the library as a text list without visible status filters, up to four columns at 1280 px, actions hidden in `⋯`, second columns that only a mouse could reach, and layouts that break with a wide Windows font. The **readable revision** ("Ma, but readable", r6) keeps the identity of Ma and changes only that. Fabio accepted it on 2026-10-26; the design system in [`../source/uzume/ds/uzume/`](../source/uzume/ds/uzume/) carries it, the values are in [design-system.md](design-system.md), the accepted decisions in [decisions.md](decisions.md#sheet-6--readable-pass-2026-10-26).

Sheet 6 redraws 12 representative screens in the revised system. It adds no function: content, copy and flows are those of sheets 2 and 4; only layout, hierarchy and emphasis change. Where a sheet 6 board and an older board disagree, sheet 6 wins; every other screen follows the same rules (below) when it is built.

## Boards

Canvas page "6 Readable pass" in [`../source/uzume/canvas.json`](../source/uzume/canvas.json), 1280 x 800, Sumi unless noted. No Kinari twins; `S0R6Kinari` is the Kinari check.

| Board | File | Replaces or extends |
|---|---|---|
| Index and decisions | `S6Index` | — |
| 3c-1-r6 Library list: 12 games that all need attention, one selected, game detail | `S3c1R6` | 3c library and game page |
| 3c-2-r6 Library grid with a focused tile | `S3c2R6` | 3c library |
| 3c-3-r6 Game saves | `S3c3R6` | 3c game page › Saves |
| 3g-1-r6 Running game: header at 1280 with a long title, plugin tabs | `S3g1R6` | 3g running game |
| 3g-6-r6 Diagnostics with details, hold-Select sheet | `S3g6R6` | 3g diagnostics and hotkey sheet |
| 9c-1-r6 Settings › This PC › Appearance | `S9c1R6` | 9c |
| 9a-1-r6 Controllers | `S9a1R6` | 9a |
| 6f-app-1-r6 Server status, header with a wide font | `S6f1R6` | 6f-app status |
| 3n-app-1-r6 Users | `S3n1R6` | 3n-app users |
| 0-r6-pad-1 Library at the TV text size with the HintRow | `S0R6Pad1` | gamepad use of 3c |
| 0-r6-pad-2 Settings at TV size, danger confirm with a wide font | `S0R6Pad2` | 8e my account, dialogs |
| 0-r6-kinari Kinari: selection, warning and control borders labelled | `S0R6Kinari` | Kinari twins of 3c and 9c |

The boards mount the real components from `ds/uzume/components/bundle.js` (`x-import`), so they show the revised `FilterChip`, `ColumnNav`, `HintRow`, `PageHeader`, `Toggle` and `Dialog`. The wide font is simulated with letter spacing (about 30 % wider); the real check is the app's wide-font test (`UZUME_TEST_WIDE_FONT`).

## Rules for every screen

- **Yellow is for acting:** one primary per view, focus ring, active nav and tab marker. Selection is an `ink` plane (chips, segments, checkboxes) or the `selected` tint with the `ink` marker (rows). Counts are neutral `badge` squares.
- **A problem is one sentence with its fix**, shown once: `StatusIcon` warning (Yamabuki square, ink text) plus the fix action. When the problem blocks the main action, the fix becomes the primary ("Install core and play").
- **Controls look operable:** every interactive border is `rule-control` (≥ 3:1); secondary buttons have a border; ghost buttons only in toolbars and rows, never as the only action of a section.
- **Text you can read:** functional text ≥ 11 px Mono / 12 px Sans in `muted` or stronger; `dim` is decoration. Text size 100 % / 125 % / TV in Settings › This PC › Appearance.
- **Three columns below 1440 px**, AppNav included. Help columns and inline input tests become disclosures; the open column, the vertical label, register marks and "01 —" numbering come back only at 1440 px and wider (and on first-start, empty and waiting states).
- **One `PageHeader`** on every page; its actions wrap below the title, the title and meta line never shorten. Dialog button rows wrap and stack inside the dialog.
- **Gamepad everywhere:** `HintRow` on every main page while the last input was a pad; second columns are `ColumnNav` rows reachable with the D-pad.

## Screens

### Library (3c-1-r6, 3c-2-r6, 3c-3-r6)

- **Toolbar:** status `FilterChip`s with counts always visible: All · Ready · Needs attention (with the warning square) · Not downloaded, then the collection `Select`. Search, sort and a List / Grid `Segmented` in the row below.
- **List:** columns No · Game · System (`SystemChip`) · Status (warning square + word, or a plain word) · Played. The selected row: `selected` tint, 2 px `ink` marker, title weight 500, while the warning squares stay on every row that needs attention.
- **Grid:** five tiles per row at 1280; tile = 4:3 placeholder with a two-letter monogram and the system chip, title, status line (warning square + word, or `muted` "Ready · play time"). Focus ring on the tile.
- **Game detail** (right pane, 360 px): cover, title, meta line; the primary action directly below; at most one problem sentence; tabs Overview · Saves · Details; facts as label/value rows; secondary actions (Add to collection, Start a run) and `⋯` (Edit details, Show in folder, Remove from library …). The start checklist appears only while starting or after a failure.
- **Saves:** header with Play from Main (primary) and Start a run; Slots table (Main, other slots, run slots) with Play and `⋯`; snapshots of Main with Restore …; "File details" (SHA-256, local path, size) closed by default.

### Running game (3g-1-r6, 3g-6-r6)

- **Header** (56 px): ← Library, title (ellipsis, full title as tooltip), system chip, Layout, Multiview, Panel, Fullscreen icon button (tooltip "Fullscreen · F11"), **Pause / Resume as the primary**, `⋯` with Diagnostics (F3), Screenshot (F12), Hotkeys, Reset game …, Report a problem …. Fits at 1280 with a long title.
- **Panel tabs:** Run · Chat · Saves and each plugin tab with its icon and a short label; overflow goes into "More".
- **Diagnostics:** the summary (FPS, frame time, audio, session) plus a visible "Details" section: encoder, bitrate, resolution and a table per person (path Direct / Relay, RTT, loss, and the reason for a relay in a sentence). Copy stays.
- **Hold-Select sheet** gains Diagnostics.

### Settings and controllers (9c-1-r6, 9a-1-r6)

- **Appearance:** Theme (Sumi · Kinari · Follow system), **Text size** (100 % · 125 % · TV), **Reduce motion** (follows the system until changed), System chips on covers, Language; "Help" disclosure.
- **ColumnNav:** sections with meta lines and a warning ("Storage · 92 % full"); focus state drawn on a row.
- **Controllers:** devices in the ColumnNav (connected and seen before); the device header with the input test as a disclosure below it; system `Segmented`; mapping table that shows the bound key once (key frame + name) and "Default: …" with Reset only on changed rows; focus drawn on a binding field.

### Admin in the app (6f-app-1-r6, 3n-app-1-r6)

- **Status:** `PageHeader` (Server · Status · version, address, uptime; Connection check, Back up now); "Needs attention · 3" as warning sentences with one action each; health facts in three columns; "Help" disclosure. Nav badges: Status, Systems & cores (core problems) and Settings (server update).
- **Users:** `PageHeader` with Invite someone; role `FilterChip`s with counts; rows with avatar, role, what the person does now, Copy invite for open invites and `⋯` (Make admin, Reset password …, Sign out everywhere …, Remove user …); "Help" disclosure.

### Gamepad, wide font and Kinari (0-r6-pad-1, 0-r6-pad-2, 0-r6-kinari)

- At the TV size rows are 48 to 52 px, chips and buttons 44 px, body 17 px; the `HintRow` shows A Choose · B Back · Y Search · Start Options (L / R switch the filter in the library).
- A danger confirm ("Sign out everywhere?") with a very wide font stacks its buttons (danger first, Cancel below) inside the 400 px dialog.
- Kinari: selected row `#ece9de` with the `ink` marker (14.2:1), warning rows keep the square and an ink sentence (16.5:1), chip and toggle borders `rule-control` (3.7:1), active nav marker `accent-text` (6.2:1).

## Contrast

Every pair used for information meets 4.5:1 for text and 3:1 for control borders and markers in both themes (lowest: `dim` on Sumi `pressed` 4.57, `rule-control` on Sumi `pressed` 3.13). Dividers (`rule`, 1.45:1) carry no information and are never the border of a control. The warning square's meaning is carried by its sentence; its "!" glyph is 9.5:1.

## Building it

The Qt app takes the tokens from `tokens.json` into `client/ui/Theme.qml` and the components into the `Uz*` controls; the scope and order of the UI pass (packages A, B, C of the scan) is planned in [the roadmap](../../roadmap.md) once chosen. Until a screen is rebuilt, the rules above apply to every change touching it.
