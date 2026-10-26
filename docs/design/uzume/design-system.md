# Uzume design system

The design system "Uzume" (direction **Ma**, themes **Sumi** dark and **Kinari** light, IBM Plex) in the handoff `../source/uzume/ds/uzume/`. [`tokens.json`](../source/uzume/ds/uzume/tokens.json) is the source of truth; [`tokens.css`](../source/uzume/ds/uzume/tokens.css) is generated from it (CSS variables, `sumi` default, `[data-theme="kinari"]` override) and must not be edited by hand. Components: [`components/bundle.js`](../source/uzume/ds/uzume/components/bundle.js) (React, namespace `Uzume`) and [`bundle.css`](../source/uzume/ds/uzume/components/bundle.css). Fonts are in `ds/uzume/fonts/` (OFL license in `ds/uzume/licenses/`). The Qt/QML implementation (0.18 package 3) reproduces tokens and components from these files; the bundle is a reference, not shipped code.

Both themes apply to the one app: the old split "Player dark / Hub light" is gone. Back to the [overview](README.md).

Since the **readable revision** (r6, 2026-10-26, [readable-pass.md](readable-pass.md)) yellow is only for acting, selection is neutral, every control border uses `rule-control`, functional text is at least 11 px Mono or 12 px Sans, there are three text sizes including TV, and a page shows at most three columns below 1440 px. The values below are the revised ones.

## Principles (Ma)

- **Space, not boxes.** No cards, no shadows, no rounded corners (the app icon tile is the only radius). Sections are separated by space, a `SectionLabel` and hairlines. Panels (sidebar, side panel) share the ground colour and are separated by a `rule`.
- **One yellow, for acting.** Yamabuki marks what you can act on: the one primary button per view, focus ring, active nav item and tab marker, progress, the AUDIO tag, the "!" warning square. Selected chips, segments, checkboxes and rows are neutral (an `ink` plane, or the `selected` tint with an `ink` marker); count badges are neutral (`badge`). Never a circle, never behind text longer than a label.
- **Warning = an ink sentence with the Yamabuki square, danger = muted red, ok = a 6 px green square.** No separate warning hue, no amber, no cyan, no tinted banners: warn / error / done are a `StatusIcon` plus a sentence; info is a plain `ink-2` sentence. A problem is shown once, with its fix. Danger is only outlined buttons, square error icons, error text and danger tags, never a filled button. Colour is never alone: every state carries an icon or a word.
- **Hairline rows.** Tables are 44 px rows on `rule-2` separators under a Mono `table-header`; the selected row has a 2 px `ink` start marker, the neutral `selected` tint and weight 500 (never the tint alone), so a selected row never looks like a row that needs attention.
- **Density.** 8 px baseline (4 and 12 are the only half steps); rows 28 / 32 / 36 / 44 / 48; margins 56; the 800 px window uses the compact density (28 px nav items, one-line ONLINE rows). Admin pages use 48 px top padding so tables fit in 800 px. Lines of text are at most 72 characters. Below 1440 px (`columns-compact-max`) a page shows at most three columns including AppNav.
- **Open right column, only when wide.** At 1440 px and wider a list page may have a 152 px open column, 56 px from the content, with a vertical Mono label (`UZUME · LIBRARY · HOME SERVER`) or a `HelpColumn`; below that it collapses into a "Help" disclosure. Register marks and "01 —" numbering only on first-start, empty and waiting states or at 1440 px and wider. Register marks (1 px, 12 px arms, `rule` on pages, `dim` around the game stage) frame the content area.
- **Empty stage.** The running game's stage (`stage` colour) draws only the game. Allowed on it: `Toast`s at the bottom left, the F3 diagnostics block in the top right corner, and the hold-Select hotkey sheet. Pause, load-state and hold-Select dialogs sit over a scrim on the stage only; the 56 px run header stays visible.
- **Ensō only in empty and waiting states** (`StateBlock`, waiting for approval, Multiview waiting tile). Nothing else is round.
- **Focus is always visible** (keyboard and gamepad). The `HintRow` (44 px, `rule-2` above) is on every main page while the last input was a gamepad, with `GamepadHint` glyphs (`Kbd` keys otherwise). Every second-column row (`ColumnNav`) is focusable with arrows and A / B. Gamepad screens keep 36 px controls; the TV text size raises every target to 44 px.
- **Copy.** Say "server" only when the person must act on it; sentence case, verb first, no exclamation marks; system codes are always GB, GBC, GBA, NDS, 3DS; hosts are placeholders (`server.example.com`).

## Tokens

All values below are copied from `tokens.json` (version 1, readable revision). Every colour, length and ring in the components reads from `tokens.css`.

### Colour

#### Planes

| Token | Sumi (dark) | Kinari (light) | Use |
|---|---|---|---|
| `ground` | `#1c1b19` | `#fbfaf5` | Sumi 墨 / Kinari 生成. App background. Panels (sidebar, side panel) use the same colour and are separated by `rule`, never by a tint or a shadow. |
| `surface` | `#242320` | `#ffffff` | Toasts, dialogs and menus only — the one place a plane sits on the ground. Always framed by a 1 px `rule`. |
| `scrim` | `rgba(28,27,25,.8)` | `rgba(251,250,245,.8)` | Ground at 80 % behind a modal dialog. The dialog itself stays on `surface` with a 1 px `rule`; no blur, no shadow. |
| `stage` | `#171614` | `#f3f1e9` | Behind running games, Multiview tiles and cover art: the space the game draws into. |
| `placeholder` | `#262521` | `#e9e6dc` | Fill of a cover or game screen before its image arrives. From MaScreen `gFill`. |
| `placeholder-rule` | `#34322e` | `#d6d2c6` | Frame of a game screen inside the stage. From MaScreen `gLine`. |

#### Text and lines

| Token | Sumi (dark) | Kinari (light) | Use |
|---|---|---|---|
| `ink` | `#efece4` | `#1c1b19` | Titles, item titles, values, primary labels. On ground, surface, stage, hover, pressed and selected: ≥ 12:1 in both themes. |
| `ink-2` | `#d3cfc5` | `#3a3936` | Body copy, chat, dialog text, the ensō stroke. ≥ 9:1 on every ground in both themes. |
| `muted` | `#a19d94` | `#5f5c55` | Secondary text, inactive nav items and tabs, helper lines; meta text on surface, hover, pressed and selected (where `dim` is too weak). ≥ 5.2:1 on every ground in both themes. Also the frame of an unchecked checkbox (≥ 5.2:1, meets 3:1 for controls). |
| `dim` | `#96928a` | `#6a665f` | The quietest text: decoration, disabled hints and Mono labels that repeat their heading. Raised in r6 so it passes 4.5:1 on ground, surface, pressed, selected and stage in both themes (lowest 4.57 on Sumi pressed); still never for information a person needs — use `muted` or stronger. |
| `rule` | `#3a3833` | `#d6d2c6` | Structural hairlines that are not controls: table heads, dialog and menu frames, column and panel edges, register marks, inactive tile frames (1.45:1, a divider has no minimum). |
| `rule-control` | `#7a766d` | `#86817a` | Border of everything you can operate: fields, selects, the search underline, secondary buttons, chips, segments, the toggle track, checkboxes, game tiles. ≥ 3:1 on ground, surface, stage, pressed and selected in both themes (WCAG 1.4.11). Hover and focus go stronger (`ink-2`, `ink`). |
| `badge` | `#efece4` | `#1c1b19` | Fill of count badges (nav, bell, tab counts) with `ground` text: neutral, so a count never looks like a warning. |
| `rule-2` | `#2b2a27` | `#e9e6dd` | Quiet hairlines: row separators, tab baselines, the line above a tile's name bar. |

#### States

| Token | Sumi (dark) | Kinari (light) | Use |
|---|---|---|---|
| `hover` | `#242320` | `#f4f2ea` | Row, nav item and secondary/ghost button hover. |
| `pressed` | `#2c2b28` | `#ebe8de` | Pressed, and toggled-on secondary (e.g. the Panel button while the side panel is open). |
| `selected` | `#2a2925` | `#ece9de` | Neutral tint of a selected row, list item or menu item. Always together with the 2 px `marker-start` in `ink` and weight 500 — never the tint alone, never yellow. |
| `disabled-ground` | `#262521` | `#efede6` | Fill of disabled buttons and fields. From MaKit. |
| `disabled-ink` | `#6b6861` | `#a3a097` | Text of disabled controls. Exempt from 4.5:1 (WCAG inactive); a disabled control carries a reason line in `muted` where space allows. From MaKit. |
| `disabled-rule` | `#2b2a27` | `#e9e6dd` | Border of disabled controls. From MaKit. |

#### Accent (Yamabuki)

| Token | Sumi (dark) | Kinari (light) | Use |
|---|---|---|---|
| `accent` | `#f8b500` | `#f8b500` | Yamabuki 山吹 — the one yellow. A plane with Sumi text (`on-accent`) for exactly: the primary button, focus ring, active nav and tab marker (on Kinari `accent-text`), progress, AUDIO tag, speaking mark and warning square. Not for selection and not for count badges. Never a circle, never behind text longer than a label, one primary per view. |
| `accent-hover` | `#ffc42e` | `#ffc42e` | Primary button hover. `on-accent` text 10.8:1. |
| `accent-pressed` | `#dba000` | `#e0a400` | Primary button pressed. `on-accent` text 7.4 / 7.8 : 1. |
| `accent-text` | `#f8b500` | `#715c1f` | Links, inline actions (Undo, Open Storage →) and the selected row number. Yamabuki itself on Sumi (9.5:1); Uguisu-cha 鶯茶 on Kinari (6.2:1), because Yamabuki as text on paper is 1.7:1. |
| `on-accent` | `#1c1b19` | `#1c1b19` | Sumi text and glyphs on an `accent` plane (9.5:1), in both themes. From MaKit `accInk`. |

#### Status

| Token | Sumi (dark) | Kinari (light) | Use |
|---|---|---|---|
| `danger` | `#ee836f` | `#ac3a41` | Sango-shu 珊瑚朱 / Enji 臙脂. Outlined danger buttons, square error icons, error text and borders, and danger `Tag`s (plane with `ground` text, 6.6 / 5.2 : 1). 6.7 / 5.2 : 1 on ground. Never a filled button, never a disc. |
| `danger-tint` | `rgba(238,131,111,.10)` | `rgba(185,64,71,.07)` | Hover and pressed fill of the outlined danger button. From MaKit `dangerH`. |
| `ok` | `#68be8d` | `#006b3a` | Wakatake 若竹 / Tokiwa 常盤. A 6 px square next to a word (Online, Saved), and the frame or plane of an ok `Tag` (plane with `ground` text and a ✓, 7.6 / 5.1 : 1). Never a larger fill, never text on its own. |

#### People

| Token | Sumi (dark) | Kinari (light) | Use |
|---|---|---|---|
| `person-wasurenagusa` | `#89c3eb` | `#89c3eb` | Wasurenagusa 勿忘草 — person colour 1 (Max in the sheets). Avatar square, 8 px square, 40 × 3 px tile top rule. Always with the person's name; never for state. |
| `person-momo` | `#f09199` | `#f09199` | Momo 桃 — person colour 2 (Lena). Same uses as the other person colours. |
| `person-moegi` | `#aacf53` | `#aacf53` | Moegi 萌黄 — person colour 3 (Jonas). Closest hue to Yamabuki (75° vs 41°) and still distinct. |
| `person-fuji` | `#bbbcde` | `#bbbcde` | Fuji 藤 — person colour 4 (Sam). |
| `on-person` | `#1b1b1b` | `#1b1b1b` | Initial inside a person avatar square (7.6–9.7 : 1 on all four). People colours sit at 1.7–2.2 : 1 on Kinari, so the name next to them carries the meaning. |

Person colours go to people in join order: Max wasurenagusa, Lena momo, Jonas moegi, Sam fuji. They appear as an `Avatar` square, an 8 px square beside a name, or the 40 x 3 px tile top rule, always with the person's name and never for state.

### Typography

Families: Plex Sans (`--font-sans`), Plex Mono (`--font-mono`), Plex Sans JP (`--font-jp`, only for the optional ウズメ). Files:

- IBM Plex Sans 300 (fonts/IBMPlexSans-Light.woff2)
- IBM Plex Sans 400 (fonts/IBMPlexSans-Regular.woff2)
- IBM Plex Sans 500 (fonts/IBMPlexSans-Medium.woff2)
- IBM Plex Sans 600 (fonts/IBMPlexSans-SemiBold.woff2)
- IBM Plex Mono 400 (fonts/IBMPlexMono-Regular.woff2)
- IBM Plex Mono 500 (fonts/IBMPlexMono-Medium.woff2)
- IBM Plex Mono 600 (fonts/IBMPlexMono-SemiBold.woff2)

#### Ma roles

| Style | Family | Size / line | Weight | Tracking | Use |
|---|---|---|---|---|---|
| `title` | Plex Sans | 40px / 40px | 300 | -0.02em | Page title, once per page, under its section label. Plex Sans Light 40/40, −2 %. |
| `section-label` | Plex Mono | 11px / 16px | 500 | 0.14em | Section label: uppercase name, optional two-digit number and em dash (only with three or more sections). `muted` when it carries information. |
| `body` | Plex Sans | 14px / 22px | 400 | - | Body, dialog text, nav items, table cells. Lines ≤ 72 characters. |
| `meta` | Plex Mono | 11px / 16px | 400 | 0.06em | Meta: uppercase, +6 %, parts joined by ' · '. Under covers, in tile name bars, column meta. |
| `meta-12` | Plex Mono | 12px / 16px | 400 | - | Larger meta: row numbers, durations in tables, toggle state words (was `meta-11`). |

#### App sizes

| Style | Family | Size / line | Weight | Tracking | Use |
|---|---|---|---|---|---|
| `dialog-title` | Plex Sans | 20px / 28px | 400 | -0.01em | Dialog title, a question or a statement; under a section-label line naming the area. |
| `header-title` | Plex Sans | 16px / 24px | 500 | - | Title in the 56 px run header bar (running game, Multiview). |
| `item-title` | Plex Sans | 14px / 22px | 500 | - | Item titles in lists and the selected table row; empty-state headings. |
| `item-title-sm` | Plex Sans | 13px / 18px | 500 | - | Cover tile titles, Multiview tile names, runner names, the server name in the nav footer. One line, ellipsis. |
| `button` | Plex Sans | 14px / 20px | 500 | - | 36 px buttons. Primary and danger 500; secondary and ghost 400. |
| `button-sm` | Plex Sans | 13px / 20px | 400 | - | 32 px buttons, header buttons, toast actions (500). |
| `input` | Plex Sans | 13px / 20px | 400 | - | Text in fields and the chat composer. |
| `field-label` | Plex Sans | 12px / 16px | 500 | - | Label above a field; chip and segment text uses 12/16 400. |
| `small` | Plex Sans | 12px / 18px | 400 | - | Helper and error lines, empty-state explanations, notes. |
| `table-header` | Plex Mono | 11px / 16px | 500 | 0.12em | Table column heads, uppercase, `muted`, on a `rule` underline. |
| `system-code` | Plex Mono | 11px / 14px | 500 | 0.06em | System chips: GB, GBC, GBA, NDS, 3DS — always these exact codes. |
| `tag` | Plex Mono | 11px / 16px | 600 | 0.1em | Small accent tags (AUDIO) and nav count badges. |
| `address` | Plex Mono | 12px / 16px | 400 | - | Code and address values: hosts, ports, paths, versions. Never uppercase. |
| `invite-code` | Plex Mono | 28px / 36px | 500 | 0.14em | A large invite or pairing code, shown alone in a dialog. Uppercase, groups of four joined by a hyphen. |
| `vertical-label` | Plex Mono | 10px / 16px | 400 | 0.24em | The vertical label of the open column (writing-mode: vertical-rl), `dim`. Decoration only, at 1440 px and wider. |

Functional text (status words, ON / OFF, counts, shortcut hints, table heads, meta lines) is at least 11 px Mono or 12 px Sans and uses `muted` or stronger.

#### Text sizes

| Token | Value | Use |
|---|---|---|
| `text-scale-standard` | `1` | Default (Settings › This PC › Appearance › Text size: 100 %). |
| `text-scale-large` | `1.25` | 125 %: every type style × 1.25; rows and controls grow only where the text needs it. |
| `text-scale-tv` | `1.25` | TV: type × 1.25 (body 17.5 px), every focus target at least `target-tv`, rows 44 / 48, the `HintRow` always visible. |

#### Brand

| Style | Family | Size / line | Weight | Tracking | Use |
|---|---|---|---|---|---|
| `wordmark-nav` | Plex Sans | 17px / 20px | 600 | -0.01em | The wordmark beside the 20 px mark in the sidebar lockup (gap 10). |
| `wordmark-display` | Plex Sans | 46px / 46px | 600 | -0.035em | Display lockup beside the 52 px mark (gap 16): About, splash, installer welcome. |
| `jp-accent` | Plex Sans JP | 15px / 20px | 400 | - | Optional ウズメ after the display lockup, `muted`. Only on About, splash and installer welcome — nowhere else. |


### Spacing

| Token | Value | Use |
|---|---|---|
| `space-0.5` | `4px` | Gaps inside a control: avatar overlap, label-to-hint, stacked game screens. |
| `space-1` | `8px` | Base step: between buttons, icon and label, field label and field. |
| `space-1.5` | `12px` | Row padding, toast and icon gaps, nav item padding. |
| `space-2` | `16px` | Button padding (36 px), sidebar padding, header gaps. |
| `space-3` | `24px` | Between cover tiles and Multiview tiles; side panel padding; register-mark inset. |
| `space-4` | `32px` | Between page sections; Multiview padding. |
| `space-5` | `40px` | Below the sidebar lockup; bottom page margin. |
| `space-7` | `56px` | Page margins (top, left, right) and the gap between the content and the open column. |

### Sizes

| Token | Value | Use |
|---|---|---|
| `row-28` | `28px` | Compact density (800 px windows): nav items and online rows; chips and segments. |
| `row-32` | `32px` | Nav items, small buttons, header buttons. |
| `row-36` | `36px` | Fields and standard buttons. |
| `row-44` | `44px` | Table rows, toasts. |
| `row-48` | `48px` | Runner rows, run tabs. |
| `header-56` | `56px` | Run header bar of the running game and Multiview. |
| `sidebar` | `232px` | AppNav width. |
| `open-column` | `152px` | The open right column of every list page, with its vertical Mono label. |
| `side-panel` | `320px` | The running game's side panel. |
| `dialog` | `400px` | Standard dialog width: confirmations and short forms. |
| `dialog-lg` | `560px` | Wide dialog: forms with several fields, lists to choose from, invite and pairing flows. |
| `register-mark` | `12px` | Arm length of a register mark (1 px, `rule` on pages, `dim` around the game stage). |
| `person-marker` | `40px` | Width of the 3 px person-colour rule on a tile's top-left edge. |
| `mark-nav` | `20px` | Mark size in the sidebar lockup. |
| `menu` | `200px` | Minimum width of menus and select lists (on `surface`, framed by `rule`). |
| `target-min` | `32px` | Smallest hit target for mouse and keyboard (icon buttons, the × of a field, row actions). |
| `target-tv` | `44px` | Smallest focus target at the TV text size; list rows grow to 48. |
| `columns-compact-max` | `1440px` | Below this width at most three columns including AppNav; open column, help column and vertical label collapse. |

### Radius

| Token | Value | Use |
|---|---|---|
| `radius-none` | `0` | Every UI element: buttons, fields, chips, tiles, toasts, dialogs, menus. |
| `radius-app-icon` | `22.5%` | The app icon tile only (220 → 49.5 px). Pixel-fitted 32/16 tiles use stepped corners instead. |

### Focus ring, markers and elevation

| Token | Value | Use |
|---|---|---|
| `elevation` | `none` | Toasts, dialogs and menus: no shadow; `surface` plus a 1 px `rule` frame separates them. |
| `focus-ring` | sumi: `0 0 0 2px #1c1b19, 0 0 0 4px #f8b500` / kinari: `0 0 0 2px #f8b500, 0 0 0 3px #1c1b19` | Keyboard focus, always visible. Sumi: 2 px gap + 2 px Yamabuki (9.5:1). Kinari: 2 px Yamabuki + 1 px Sumi outside, so the ring reaches 3:1 against paper. |
| `focus-ring-inset` | sumi: `inset 0 0 0 2px #f8b500` / kinari: `inset 0 0 0 2px #f8b500, inset 0 0 0 3px #1c1b19` | Keyboard and gamepad focus inside a container that clips an outer ring: menu and list items, nav items, online rows. Same Yamabuki 2 px, drawn inward; on Kinari with 1 px Sumi inside so it reaches 3:1. |
| `marker-start` | sumi: `inset 2px 0 0 #efece4` / kinari: `inset 2px 0 0 #1c1b19` | Selected table row, list item and menu item: 2 px `ink` on the leading edge. |
| `marker-nav` | sumi: `inset 2px 0 0 #f8b500` / kinari: `inset 2px 0 0 #715c1f` | Active item in AppNav and ColumnNav: Yamabuki on Sumi, `accent-text` on Kinari (Yamabuki alone is 1.7:1 on paper). |
| `marker-under` | sumi: `inset 0 -2px 0 #f8b500` / kinari: `inset 0 -2px 0 #715c1f` | Active tab and run tab underline: Yamabuki on Sumi, `accent-text` on Kinari. |
| `audio-ring` | `inset 0 0 0 1px #f8b500` | The Multiview tile whose audio you hear: with a Yamabuki border, 2 px in total, plus the AUDIO tag. |

## Components

The 45 components named in the bundle header plus helpers exported with them (`TableHead`, `AvatarStack`, `MenuList`, `SettingsDisclosure`, `NavBell`, `IconButton`, `MapPoint`, `VoiceMark`, `StageDiagnostics`, `HotkeySheet`). Props are in `bundle.js`; this list gives the job of each.

### Frame and navigation

- `AppNav`: the 232 px sidebar with the Lockup, groups PLAY (Library, Runs, Emulation, Controllers), SERVER (Status, Users, Systems & cores, Plugins, Reports; admins) or THIS PC (Systems & cores, Plugins; no server), ONLINE people, Settings, the server line (`StatusLine` + `Menu`) and the person menu; views admin / member / on this PC, compact density, count badges and the warning-word badge.
- `NavBell`: the bell with a count (`9+` above nine) that opens a notification `Popover`.
- `OpenColumn`: the 152 px right column with the vertical Mono label.
- `HelpColumn`: the open column with a HELP slot for paragraphs separated by `rule-2`.
- `RegisterMarks`: the 1 px corner marks around a content area.
- `PageHeader`, `SectionLabel`: the one page header (optional Mono eyebrow, Light 40 title, optional meta line, actions that wrap below the title so it never truncates) and the Mono section label.
- `ColumnNav`: the second column of Settings, Emulation, Controllers and Server settings; every row focusable (arrows, Enter / A, Esc / B), active row with `marker-nav`, a row can carry meta or a warning.
- `HintRow`: the footer of every main page while the last input was a gamepad: `GamepadHint`s (or `Kbd` keys) with an optional end group.
- `Tabs`: page tabs (md / lg) with the 2 px Yamabuki underline; a tab can carry an icon and a count (plugin tabs: icon and short label, never a single letter); `RunTabs`: the 48 px tabs of the in-game side panel.
- `Segmented`: a square segmented choice (also the This PC / Server switch and Layout switch).

### Controls

- `Button`: primary (Yamabuki plane), secondary, ghost, danger (outlined); 36 px and 32 px `sm`; toggled state `on`.
- `IconButton`: square icon control for header buttons (volume, bell) that anchors a `Popover`.
- `TextField`, `SearchField`: single-line fields with label, hint, error line (square + sentence) and optional shortcut hint; the search field has the search glyph and a placeholder.
- `Checkbox`: square, Yamabuki plane with a check; indeterminate, disabled and error states.
- `Toggle`: square on/off switch with a square knob (no circle); off has a `muted` knob in a `rule-control` track; used for most settings rows.
- `Select`, `Menu`, `MenuList`: select field with a list; context menu (`⋯`) with headings, separators, shortcuts and a danger item, on `surface` with a `rule` frame (min width 200).
- `Slider`: hairline track, square thumb, Yamabuki fill, Mono value on the right; step keys and gamepad left/right; muted state; optional lead (person square) for mixer rows.
- `FilterChip`, `SystemChip`: toggled chip (28 px; selected = `ink` plane; optional count and the warning square for "Needs attention") and the Mono system code chip (GB, GBC, GBA, NDS, 3DS).
- `SettingsRow`, `SettingsDisclosure`: label + help + control on the right, changed marker (Yamabuki square), "Reset to default", More / Less for advanced rows.
- `GamepadHint`: square key frame with a Mono letter; keys A B X Y L R L2 R2 L3 R3 Select Start and the D-pad directions (↑ ↓ ← →, Up/Down, Left/Right); `Kbd`: the same frame for keyboard keys and combos (Enter, Esc, Ctrl+Shift+U).
- `Progress`: thin or standard bar with label and hint (also the microphone level).

### Surfaces and feedback

- `Dialog`: 400 / 560 px dialog on `surface` with a `rule` frame over the 80 % scrim; Mono label, title, text, content, action row (primary, secondary, danger at the far right) that wraps, and stacks (`stacked`) when the labels do not fit, so no button leaves the dialog; a busy variant dims its content.
- `Popover`: small anchored surface (title, content, action row, hints; closes on Esc or B); used for volume, the bell, the Multiview mixer and the map's route card.
- `Toast`: 44 px notice on `surface` (kinds done / warning / error, optional shortcut, action, close); bottom left of the stage in game.
- `StatusIcon`, `StatusLine`: square status icon + sentence (done, warning, error); online / offline server line with address.
- `StateBlock`: empty / loading / error state with the ensō, text and action; `Enso`: the ensō stroke (waiting states).
- `Tag`: Mono status label (neutral, accent, ok, danger; outline or plane; icon or word) for New, Seen, Official, Not signed, Matches, Missing.

### People and data

- `Avatar`, `AvatarStack`: square person-colour avatar (20 / 28 / 40) with online status square and the voice mark; stack for watchers.
- `VoiceMark`: speaking (bars), mic off, muted by host; never colour alone.
- `TableRow`, `TableHead`: hairline table with number, title, system chip, played, last played, status word and the selected / disabled states.
- `RunnerRow`: 48 px runner with person colour, presence, status and meta; `ChatMessage`: name, person colour, time and text.
- `GameCover`: cover tile with placeholder fill, system chip and meta.
- `MapPoint`: route point on the region map (anchor for the route card popover).

### Running game

- `RunHeader`: 56 px bar with back, title, system chip, sub line, watcher avatars and action buttons.
- `GameStage`: the stage with the game picture, caption (core, size, FPS, latency), Toasts, the `StageDiagnostics` block (label/value rows + Copy) and the `HotkeySheet` ("Hold Select +" grid).
- `MultiviewTile`: tile with person rule, name, system, game, right-side status, AUDIO frame and tag, voice mark and the waiting ensō.

### Brand

- `Mark`, `Lockup`: logo 03a "Rise" (a U with a rising right arm and a light point), nav lockup (mark 20) and display lockup (mark 52, optional ウズメ).
- `AppIcon`: the 22.5 % radius app tile, tone sumi or kinari, optionally with the server glyph; `ServerGlyph`: two stacked units, top one lit, only for installer, tray and server About.

## Gaps accepted in the design system

The boards list 21 items where the system has no component and a workaround was accepted (second columns as text items, pixel icons kept by id, storage ink tints, lanes, avatar sizes, terminal, banner tile, plugin surface line, shield placeholder, step lists, stage overlays, run tabs, plugin tab name, pause dialogs, library table instead of tiles, per-game dialog, creature placeholders, region map drawing, controllers, game data, stream view). See [decisions.md](decisions.md).
