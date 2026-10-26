# Frame and first start (sheet 1 and the 0-4 frame boards)

Covers canvas pages "0 All sheets" (index only) and "1 Frame and first start": the 0-4 frame decisions A to H, first start (8a), join a server (8b) and the invite link (8c). All boards are 1280 x 800 unless noted; each has a Sumi (dark) board and a Kinari (light) twin that imports it, so the content is identical in both themes. The 0-4 boards A, B, C, F, G and H show both themes on one board; D and E have Kinari twins. Tokens and components: [design-system.md](design-system.md). Screens of this sheet not listed in a "Built in" line below are index or reference boards.

Conventions that hold for every screen in this file:

- Full-window first-start screens have **no sidebar** until a library exists; the top bar is 84 px high with the Lockup at 32 / 28 and a Mono screen label on the right; a 152 px open column with the vertical Mono label `UZUME · ...` is on the right.
- Gamepad screens end with a hint row of `GamepadHint` (A Choose, B Back, Y Keyboard, X Show password); keyboard screens use `Kbd` (Tab, Enter, Esc, Ctrl+V). The hint row follows the last input device.
- Passwords are at least 12 characters. Sign-in lockout is per person per PC: 5 tries, then 5 minutes.
- Codes: security code `K7Q4-9WMP-2HXD` (groups of four, hyphens), invite code `UZ-8XK2-M4QD` (prefix `UZ-`; the field accepts spaces, hyphens or none), setup code `7H4K-2Q` (6 letters and numbers). Hosts are placeholders: `server.example.com:8443`, `192.0.2.10` and `192.0.2.20` (documentation range).

## Index boards

### 1 Frame and first start · Index

Boards: [`Main`](../source/uzume/Main.dc.html). Not built.

Index of sheet 1 (15 screens, Sumi and Kinari, 41 boards): table of boards, what they show and the rules that matter in build, plus 22 "did not fit the design system" items. Their outcome is in [decisions.md](decisions.md) (section "Sheet 1", items 01 to 22).

### 0-4 H Round 5 index and Later

Boards: [`S04H`](../source/uzume/S04H.dc.html). Not built.

Lists the 27 screen groups of Round 5 with the batch each was drawn in, and the "Later" list of things not drawn in Round 5 (network discovery of servers, invites into a run, "Replace file..." for games, "New users need: Approval", watching in the browser (7b), keep the previous core version, automatic import-folder scan, the rule "apps within one minor version keep working", a reachability test run by the server itself, a web page for `uzume://` links when the app is not installed, handheld install and Linux installer screens beyond 8m). These stay open; see [decisions.md](decisions.md).

## 0-4 Frame decisions

### 0-4 A Name, mark, server glyph

Boards: [`S04A`](../source/uzume/S04A.dc.html). Built in: 0.18 package 3 (design system in the app), 0.18 package 11 (installer).

- One name: **Uzume**, Plex Sans 600, no suffix anywhere (title bar, sidebar, installer, About, tray). The Japanese ウズメ appears only on About, splash and installer welcome.
- One mark: Logo 03a "Rise" (a U with a rising right arm and a light point inside). It replaces the FrameBeam frame-and-beam mark; the play-triangle Player mark stays retired.
- App icon: `AppIcon` tile (22.5 % radius, the only rounded shape), tone `sumi` or `kinari` in fixed brand colours; the server installer adds the server glyph at 40 % bottom right. Below 64 px: Pixel tiles kept by id (32 / 16 px), not drawn in the boards.
- Server glyph (two stacked units, the top one lit): only on the installer tile, the server's tray icon and the server About; never as the app icon and never in app rows.
- Tray (background service): lit Yamabuki square while running, dim when stopped; "needs attention" leads the tray menu with the Yamabuki "!" square and a sentence. No amber.
- Sidebar lockup: mark 20 px, wordmark 17/20 600, gap 10, 32 px from the top and 28 px from the left; the same for every role and on the no-sidebar first-start screens.

### 0-4 B AppNav

Boards: [`S04B`](../source/uzume/S04B.dc.html) (1280 x 1240). Built in: 0.18 package 3 (design system in the app), 0.18 package 8 (app admin area).

- Four views of one sidebar (232 px): B-1 admin with the server menu open, B-2 member with the user menu open, B-3 on this PC (no server), B-4 compact admin at 800 px.
- Groups: **PLAY** (Library, Runs; Emulation, Controls and Hotkeys are sections of Settings since the usability pass), **SERVER** (one item, "Server"; admins only; it opens the Server screen with the tabs Overview, People, Games, Systems & cores, Plugins, Reports, Settings), **ONLINE** (people with a person-colour square and what they play; server only), Settings, server line, person.
- Member: no SERVER group. On this PC (no server): **THIS PC** (Systems & cores, Plugins) instead of SERVER, no ONLINE block, no server line. Storage and Backups are never in the sidebar.
- Server line `⋯` opens upward: Switch server, Details, Share... The person at the foot opens My account, Switch user (Ctrl+Shift+U), Sign out. The bell shows a count, `9+` above nine.
- Compact (below 900 px, used by every 1280 x 800 screen): 28 px items, one-line ONLINE rows, no address line.

### 0-4 C Server line, sign-in, blocks, density

Boards: [`S04C`](../source/uzume/S04C.dc.html) (1280 x 1040). Built in: 0.18 package 3 (design system in the app), 0.18 package 7 (app start and accounts).

- Server line states: `StatusLine` online (ok square + ONLINE, address in Mono) opens Settings › This PC › Servers; offline (empty dim square + OFFLINE) with the page text "Games on this PC still run. Saves upload when it's back." No amber border. On this PC: no server line.
- Updating and "needs an update" are a Toast with the warning square, not a StatusLine state: "Home server needs an update to work with this version."
- Sign-in pattern (replaces the old AuthCard): Mono label naming area or step, title (Light 40 on a full screen, 20 in columns and dialogs), fields 16 apart with errors below as square + sentence that says what to do, one primary then secondary or ghost, disabled buttons carry a reason line, a foot note in small muted above a `rule-2` line. No card, frame or shadow. Over the app: `Dialog` 400. Full window: a 440 to 520 column on the ground.
- Blocks (replace the old AppBlocks): `StatusIcon` warning / error / done + sentence (info has no icon: a plain ink-2 sentence), `Progress`, `StateBlock` (empty, loading, error with ensō), `Toast` with an action (Undo, Open Storage).
- Density: rows 32 / 44 / 48, fields and buttons 36 (32 `sm` in toolbars), margins 56, top bar 84, forms 400 to 520, sections 32 apart and never boxed, hint row 44 px with `rule-2` above.

### 0-4 D One library, admin inline

Boards: [`S04D`](../source/uzume/S04D.dc.html) · [`S04DKinari`](../source/uzume/S04DKinari.dc.html). Built in: 0.18 package 8 (app admin area).

- One Library for everyone: title, `SearchField`, primary "Add games..." (admins), `Tabs` (Games, and for admins the Saves tab), section label "All games · LAST PLAYED FIRST", `TableRow` list (no., title, system code, played, last played; selected row has the Yamabuki marker).
- Admin actions are inline: a `⋯` `Menu` per row (Rename..., Delete... as danger) and a "details" section under the table, visible to admins only: Uploaded by, Folder, SHA-256 (with Copy), Saves (people, versions), buttons Rename... and Delete... (danger).
- No 320 px details panel: it would take the open column.

### 0-4 E Settings This PC / Server

Boards: [`S04E`](../source/uzume/S04E.dc.html) · [`S04EKinari`](../source/uzume/S04EKinari.dc.html). Built in: 0.18 package 8 (app admin area).

- Server settings are the Settings tab of the Server screen (header: label "Server", title = server name, meta = address · Online/Offline · version); app Settings has no scope switch and covers this PC and your account only. Settings › Servers has a "Server settings" link for admins. The Settings tab says "Affects everyone on Home server."
- Server sub-pages (second column as text items with the leading Yamabuki marker, not a component): General, Network & sharing (INTERNET), Sign-in (PASSWORD), Updates (version), Backups (time), Storage (percent), Diagnostics.
- General uses `SettingsRow`: Server name (Rename...), Address (Open Network & sharing), Joining (`Select`: Invite only; approval is only for new devices of people who already have an account), Admin tools on this PC (`Toggle`; hides the SERVER group on this PC only).
- Members and on-this-PC-only users see **This PC** only, with no switch: My account, Appearance, Library folders, Plugins, Voice, Notifications, Servers, Updates, Diagnostics.

### 0-4 F Copy pass

Boards: [`S04F`](../source/uzume/S04F.dc.html) (1280 x 1040). Built in: 0.18 package 3 (design system in the app) and every package that touches UI text.

- Table FrameBeam to Uzume for every Hub and Player string: sidebar title "Uzume"; sidebar card "Home server · server.example.com:8443"; connect title "Welcome to Uzume"; connect field "Address or invite code"; Settings sections "This PC › Servers" and "Server › Settings"; update rows "Uzume 0.16.4 · this PC" and "Server 0.16.4 · Home server"; log names "App log (this PC)" / "Server log"; diagnostics bundle `uzume-diag-<device>.zip`; "Run on Home server"; "Requires server 0.16+"; "Install for everyone"; device approval "Waiting for Max to approve Desktop-LivingRoom."; tray "Uzume server is running"; window title "Uzume — Library"; sharing segment "Private · Everyone here · Invite only".
- Errors: "Can't reach Home server. Games on this PC still run; saves upload when it's back." and "Home server needs an update to work with this version."
- Renames everywhere: `framebeam-server` to `uzume-server`, `framebeam://join/...` to `uzume://join/...`, `FRAMEBEAM_*` env vars to `UZUME_*` (`UZUME_PUBLIC_HOST`, `_TURN`, `_TURN_EXTERNAL`, `_DATA_DIR`), Docker image `ghcr.io/<owner>/uzume-server`, invite prefix `FB-` to `UZ-`.
- Rule for new copy: say "server" only when the person must act on it (address, offline, update, set up); otherwise name the person ("Ask Max") or the thing ("your saves"). Never "host", "instance", "backend". Sentence case, verb first, no exclamation marks.

### 0-4 G Tokens, hotkeys, server commands

Boards: [`S04G`](../source/uzume/S04G.dc.html). Built in: 0.18 package 3 (design system in the app), 0.18 package 6 (server commands).

- Tokens used by the frame, with their roles (full list in [design-system.md](design-system.md)).
- Server commands (help text only): `uzume-server setup-code` (setup code for first setup, 8a-4), `code` (security code to compare when joining, 8b-2), `reset-admin` (prints a one-time admin password, no data touched), `status`, `update` (newest version on its channel), `backup` (backup now into the backup folder), `restore <file>` (replaces everything, makes a safety backup first), `reset` (deletes everything, asks for the server name, only a backup brings it back). In Docker: `docker exec -it uzume-server uzume-server reset-admin`.
- Hotkeys: Ctrl+Shift+U Switch user; Ctrl+, Settings; Ctrl+1 to 4 Library, Runs, Settings › Emulation, Settings › Controls; Ctrl+V pastes an `uzume://` invite link anywhere; gamepad Hold Select + ↑ opens "Who's playing?" (one modifier, no Guide button). Admin pages have no hotkeys.
- AppNav measures: width 232 on the ground with a 1 px `rule` on the right; items 32 (Plex Sans 14 muted, hover fill, active ink 500 + marker); group heads Mono 500 10/16 uppercase dim led by a 12 px rule dash.

## 8a First start

### 8a-1 First start (gamepad)

Boards: [`S8a1`](../source/uzume/S8a1.dc.html) · [`S8a1Kinari`](../source/uzume/S8a1Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

- Top-bar label FIRST START, section label "Welcome · 3 WAYS TO START", title "Welcome to Uzume", line "How do you want to play? You can change this later in Settings › This PC › Servers."
- Three numbered rows (list, hairline separators, the focused row has the focus ring and hover fill): 01 **Play on this PC** (games and saves stay on this computer, nothing to set up, no account; meta NO ADMIN RIGHTS NEEDED), 02 **Join a server** (a friend runs Uzume for a group; MOST PEOPLE IN A SOULLINK RUN START HERE), 03 **Set up a server** (home or internet; this PC, Raspberry Pi, NAS or another PC; YOU BECOME THE ADMIN).
- Foot line: "Have an invite link? Paste it anywhere in this window, or press Ctrl+V".
- Hints: A Choose, B Back. No sidebar.

### 8a-2 First start (keyboard)

Boards: [`S8a2`](../source/uzume/S8a2.dc.html) · [`S8a2Kinari`](../source/uzume/S8a2Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

Same content as 8a-1. Foot line "Paste it here with Ctrl+V"; hints ↑ ↓ Move, Enter Choose, Esc Back.

### 8a-3 Play on this PC

Boards: [`S8a3`](../source/uzume/S8a3.dc.html) · [`S8a3Kinari`](../source/uzume/S8a3Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts), 0.18 package 10 (on this PC and sharing).

- Label PLAY ON THIS PC · 1 OF 1, "NO ACCOUNT". Title "Where are your games?" "Pick the folder with your game files. Uzume looks inside sub-folders too and never changes the files."
- Field "Game folder" + Browse...; result line (done icon) "Found 214 games for 5 systems." plus "NDS and 3DS need firmware. You can add it later in Systems & cores."
- Actions: primary **Start playing**, secondary **Add later**. Foot note: saves are kept in the user folder and backed up every day; sharing later via Settings › This PC › Servers › Share with others.
- States: scanning ("Looking... 1,204 files checked", 2 px progress, Start playing waits), nothing found ("No games in this folder. Uzume reads .gb, .gbc, .gba, .nds and .3ds." action Pick another folder), no access (field error "Uzume can't read this folder. Pick another one or check its permissions."), Add later (empty Library whose `StateBlock` with the ensō offers Add a folder).
- Hints: A Choose, B Back.

### 8a-4 Set up a server · steps 1–3

Boards: [`S8a4`](../source/uzume/S8a4.dc.html) · [`S8a4Kinari`](../source/uzume/S8a4Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts), 0.18 package 6 (server commands).

- Label SET UP A SERVER · 1–3 OF 4. Step 1 "Where should it run?": **On this PC** (background service, also when Uzume is closed, needs admin rights once) or **On another device** (Raspberry Pi, NAS with Docker, Windows server; no screen needed). "On this PC skips steps 2 and 3."
- Step 2 "Connect to the device": the server shows its address, a setup code and a security code. Fields Address (`192.0.2.10:8443`) and Setup code (`7H4K-2Q`; hint "Or run `uzume-server setup-code` on the device."), primary Connect.
- Step 3 security check "Is this really pi-livingroom?": compare with the security code shown next to the setup code (`R2M8-X4TQ-7KDW`, address below); buttons **It matches** (primary) / **It doesn't**; also shown by `uzume-server code`. Same check as 8b-2.
- Hints: A Choose, Y Keyboard, B Back.

### 8a-5 Set up a server · step 4

Boards: [`S8a5`](../source/uzume/S8a5.dc.html) · [`S8a5Kinari`](../source/uzume/S8a5Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts), 0.18 package 4 (server accounts).

- Step 4 "Your admin account", "Set up Home server": fields Server name, Your name, Password (required for admins, at least 12 characters), `Select` "Who can join" (default "Home network and internet"; hint "Home network only is safest. Change it later in Server › Settings › Network & sharing."). Primary **Create server**, secondary Back.
- Progress state "Setting up...": `Progress` + checklist (done icons): Server installed, Admin account, Internet (public address and relay prepared), then "Add games: next, upload or pick a folder on the server". Note: the server cannot test itself from outside; run Connection check on a PC in another network.
- Followed by the setup assistant (7a-app-1/2, see [accounts-and-server.md](accounts-and-server.md)).

### 8a-6 Set up a server · errors

Boards: [`S8a6`](../source/uzume/S8a6.dc.html) · [`S8a6Kinari`](../source/uzume/S8a6Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

Six states, each with copy and actions:

- 6a wrong setup code: field error "That code doesn't match. Codes are 6 letters and numbers." (Connect)
- 6b device not reachable: "No answer from 192.0.2.10:8443. Is the server started and on the same network?" (Try again)
- 6c on this PC without admin rights: "Installing the background service needs admin rights once. Windows will ask." (Install / Cancel); without admin rights you can still play on this PC only or join a server
- 6d already set up: "pi-livingroom is already set up. It belongs to Home server with admin Max." (Join Home server / Back; or reset on the device with `uzume-server reset`)
- 6e install failed: "Port 8443 is in use by another program on this PC." (Use port 8444 / Details); nothing changed, local library untouched
- 6f security code differs: "Stop: the codes don't match. The device shows a different security code." (Don't connect / Details); nothing set up

## 8b Join a server

### 8b-1 Join · address or code

Boards: [`S8b1`](../source/uzume/S8b1.dc.html) · [`S8b1Kinari`](../source/uzume/S8b1Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

- JOIN A SERVER · 1 OF 3. Title "Where do you want to go?" Field "Address or invite code" (`server.example.com:8443`), primary Continue. Help: an invite code looks like `UZ-8XK2-M4QD`; spaces, hyphens or none work; an `uzume://` link fills the field.
- Notes: recent servers appear above the field as 44 px rows (name, Mono address, last used); an address without a port gets `:8443`; a typed invite code skips to the invite step (8b-3) because the code names the server.
- Hints: A Choose, Y Keyboard, B Back.

### 8b-2 Join · security code

Boards: [`S8b2`](../source/uzume/S8b2.dc.html) · [`S8b2Kinari`](../source/uzume/S8b2Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

- Label "Security check", title "Is this really Home server?" "Ask Max to read the code shown on the server. If it matches, nobody is in between." Large code `K7Q4-9WMP-2HXD` with the address below; buttons **It matches** (primary, focused) / **It doesn't match**.
- Where Max finds it: Server › Settings › Network & sharing, or `uzume-server code`.
- Matches: Uzume remembers the certificate (existing confirmed-fingerprint flow) and goes on to invite or sign-in. Does not match: hard stop with an error square and the copy of 8c-3e; nothing is sent to the server.

### 8b-3 Join · with an invite

Boards: [`S8b3`](../source/uzume/S8b3.dc.html) · [`S8b3Kinari`](../source/uzume/S8b3Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts), 0.18 package 4 (server accounts).

- Title "Join Home server"; `Avatar` Max (person colour wasurenagusa) "Max invited you. Pick the name others will see."
- Fields: Invite code (valid line "Valid · 1 use · until 29 Oct 2026"), Your name (hint "Shown to everyone on Home server."), Password (optional; "Recommended. Lets you sign in on another PC without waiting for Max."). Primary **Join**, ghost "I already have an account".
- Variants: password required for everyone (label drops "(optional)", hint "Required on Home server."); name taken ("Someone on Home server is already called Jonas. Add a letter or pick another name."); a PIN for this PC can be added later in My account (8e).

### 8b-4 Join · sign in (gamepad)

Boards: [`S8b4`](../source/uzume/S8b4.dc.html) · [`S8b4Kinari`](../source/uzume/S8b4Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts), 0.18 package 4 (server accounts).

- Title "Sign in" on Home server. Fields Name and Password, link "Forgot password?", primary Sign in, secondary "I have an invite". Line: "No password yet? Sign in with your name only and Max approves this PC."
- Name is a text field. A list of names appears only when Server › Settings › Sign-in › Show names on sign-in is on (off by default) and only from the home network.
- Gamepad: Y opens the on-screen keyboard with masking, X shows the password, A or Enter on Sign in submits. Lockout per person per PC: 5 tries, then 5 minutes; passwords at least 12 characters.
- Hints: A Choose, Y Keyboard, X Show password, B Back.

### 8b-5 Sign in · every state

Boards: [`S8b5`](../source/uzume/S8b5.dc.html) · [`S8b5Kinari`](../source/uzume/S8b5Kinari.dc.html) (1280 x 1040). Built in: 0.18 package 7 (app start and accounts), 0.18 package 4 (server accounts).

Nine states of the sign-in form:

- 5a signing in (button "Signing in...")
- 5b wrong password ("Wrong name or password. 4 tries left before a short wait.")
- 5c too many tries ("Too many wrong tries for Lena on this PC. Try again in 4:52."; wait or ask Max to reset the password)
- 5d account turned off ("Max has turned off the account Jonas on Home server. Your saves are kept.")
- 5e server offline ("No answer from server.example.com:8443 after 10 seconds."; Try again / Change address; games on this PC still run)
- 5f server too old ("Home server runs 0.15.2. This version of Uzume needs 0.17.0 or newer."; Max updates in Server › Settings › Updates or `uzume-server update`)
- 5g no password, ask for approval ("Ask Max to approve this PC once"; device chip `Lena-PC · WINDOWS · NEW DEVICE`)
- 5h waiting for approval (ensō, "Max sees your request in Users › Devices and gets a notification."; Cancel request)
- 5i request declined ("Lena-PC was not approved for Lena."; Ask again; a new request is possible after 10 minutes)

### 8b-6 Sign in · keyboard

Boards: [`S8b6`](../source/uzume/S8b6.dc.html) · [`S8b6Kinari`](../source/uzume/S8b6Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

Same form as 8b-4 for keyboard and mouse. Tab order: Name, Password, Sign in, I have an invite, Forgot password?. Enter submits from either field; Esc goes back to the security check (8b-2); Ctrl+V with an `uzume://` link opens the invite (8c-1). Focus ring always visible; hints Tab Next, Enter Sign in, Esc Back.

## 8c Invite link

### 8c-1 Invite link · first start

Boards: [`S8c1`](../source/uzume/S8c1.dc.html) · [`S8c1Kinari`](../source/uzume/S8c1Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

- Title "Lena invited you to Lena's server": server name and address (`192.0.2.20:8443`), done line "Security code matches", `Avatar` Lena (momo) "Invited by Lena", meta `VALID UNTIL 29 OCT 2026 · 1 USE`. Primary Continue, secondary Not now.
- The link carries the server's security code, so there is no code to compare; next is the name step (8b-3).
- Link format: `uzume://join/<server>/<invite>#<fingerprint>`. Opened by clicking it in a chat or pasting it with Ctrl+V anywhere in the window. A web page for the case where the app is not installed is not built (no web pages since 0.18, [ADR 0030](../../adr/0030-one-app-one-server-uzume.md) D2).

### 8c-2 Invite link · already on a server

Boards: [`S8c2`](../source/uzume/S8c2.dc.html) · [`S8c2Kinari`](../source/uzume/S8c2Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts), 0.18 package 10 (on this PC and sharing).

- Title "Add Lena's server?" "You stay on Home server too. Switch between them from the server line at the foot of the sidebar." Two rows: current (Home server, signed in as Jonas) and new (Lena's server, "CODE MATCHES").
- Actions: **Add and switch** (primary), **Add only** (joins Settings › This PC › Servers, nothing changes now), Cancel. One account per server; saves stay with the server they were made on. If a game runs, the invite waits until quit (8c-3a). Hints Enter Add and switch, Esc Cancel.

### 8c-3 Invite link · other states

Boards: [`S8c3`](../source/uzume/S8c3.dc.html) · [`S8c3Kinari`](../source/uzume/S8c3Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

Six states: 3a game running ("The invite opens when you quit; your save uploads first."; Open after I quit / Dismiss; in game it arrives as a Toast, bottom left), 3b already a member (Open Lena's server), 3c expired or used up ("It expired on 20 Oct 2026, or all its uses are gone."), 3d broken link (paste the invite code instead; field `UZ-XXXX-XXXX`), 3e security code differs (hard stop, "Someone may be in between."; Don't connect / Details; nothing sent), 3f server offline (Try again / Keep for later; kept in Settings › This PC › Servers until it expires).
