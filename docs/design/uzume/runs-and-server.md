# Runs and server (sheet 3)

Canvas page "3 Runs and server": runs for everyone (4j, 4jh), what the server shows on first start (8h), sharing from this PC and working without a server (8i, 8j), server problems seen from the app (8k), the Windows installer (8l) and the Linux note (8m). Tokens and components: [design-system.md](design-system.md).

Rules that hold on every screen of this sheet:

- **Runs are always in Play**, also without a plugin, and every run has its own save slot **"Run: <Name>"**; each person's Main slot is never touched. Runs hold 1 to 8 people; members may start runs. There is **one run plugin, "SoulLink & Nuzlocke"** (plugin IDs `uzume.soullink-core` and `uzume.soullink`); SoulLink or Nuzlocke is a *mode* chosen in its rules, and the Rules column shows the mode as a neutral `Tag` (a dash for plain runs).
- Plugin screens are run tabs; there are no plugin rows in the sidebar. Run pages use `Tabs` lg (Board · Recap · Log · Chat · plugin tab last); `RunTabs` is only for the in-game side panel.
- Content a plugin draws is marked with a Mono "DRAWN BY SOULLINK 1.4.0" line over a rule (there is no plugin-surface component).
- The terminal is drawn as a stage panel in plain Mono; emphasis is weight only. Windows installer dialogs are drawn light, 1:1, and have no Kinari twin.
- Boards are 1280 x 800 unless noted; Sumi with a Kinari twin except 8l.

## Index

### 3 Runs and server · Index

Boards: [`S3Index`](../source/uzume/S3Index.dc.html). Not built.

Index of sheet 3 (57 boards) with the rules above and 15 "did not fit" items (terminal, MSI chrome and size, small icons, banner tile, lane glyphs, run tabs, plugin surface, rules chip, run column, shield, progress dialog, old names, board size); see [decisions.md](decisions.md). It also lists six changes to batch 2 boards (Dialog, HelpColumn, Tag, Checkbox, nav warning badge, Kbd used everywhere; `UZUME_TURN` fix in 3q-app-2).

## 4j Runs

### 4j-app-1 Runs · admin

Boards: [`S4j1`](../source/uzume/S4j1.dc.html) · [`S4j1Kinari`](../source/uzume/S4j1Kinari.dc.html). Built in: 0.18 package 9 (runs without plugin).

- Play group, label `PLAY · EVERYONE ON HOME SERVER`, title "Runs", primary **New run...**. Table: run (name, game), Rules (`Tag` with the mode, or dash), people, play time, status `Tag` (Active ok, Paused, Ended).
- Detail section for the selected run: SAVE SLOT "Run: Ember Route · each person's Main slot is not touched", RULES "SoulLink & Nuzlocke 1.4.0 · mode SoulLink", PEOPLE "Max, Lena, Jonas · 3 of 8", STARTED; button Open run. Admins also see Edit rules, Remove runner and End run inside the run.

### 4j-app-2 Runs · member

Boards: [`S4j2`](../source/uzume/S4j2.dc.html) · [`S4j2Kinari`](../source/uzume/S4j2Kinari.dc.html). Built in: 0.18 package 9 (runs without plugin).

Same table limited to runs you are in and runs open to everyone on the server. Note "Members can start runs too."

### 4j-app-3 Runs · New run

Boards: [`S4j3`](../source/uzume/S4j3.dc.html) · [`S4j3Kinari`](../source/uzume/S4j3Kinari.dc.html). Built in: 0.18 package 9 (runs without plugin), 0.22 (plugins).

`Dialog` lg "Start a run": Name, Game (`Select`), Rules (`Select`: None, "SoulLink & Nuzlocke"; hint "Only installed run plugins are listed."), Mode (`Segmented` SoulLink / Nuzlocke; "SoulLink links catches from the same route; Nuzlocke plays without links. Set by SoulLink & Nuzlocke; can't change after the start."), People (`Checkbox` per person, 1 to 8, each plays their own copy, you are always in your own run), and the line "Each person gets Run: Ember Route. Main is not touched."

### 4j-app-4 Run · plain

Boards: [`S4j4`](../source/uzume/S4j4.dc.html) · [`S4j4Kinari`](../source/uzume/S4j4Kinari.dc.html). Built in: 0.18 package 9 (runs without plugin).

Label `PLAIN RUN · LUMEN DRIFT · STARTED 3 OCT`; Export..., primary **Continue**. Tabs Board / Recap / Log / Chat, no plugin tab. Board table: person, save slot ("Run: 100 % Lumen Drift"), version, play time, last played. Recap shows sessions, screenshots and notes (Recap content: 0.23; Chat tab: 0.21).

### 4j-app-5 Run · SoulLink tab

Boards: [`S4j5`](../source/uzume/S4j5.dc.html) · [`S4j5Kinari`](../source/uzume/S4j5Kinari.dc.html). Built in: 0.22 (plugins).

Plugin tab (named after the mode, last in the tabs) with the line DRAWN BY SOULLINK 1.4.0. Section "LINKED BY ROUTE · Pairs · 5 alive · 2 lost": table route × person columns (creature names in person colour labels MAX / LENA / JONAS) with a link `Tag` (Alive ok, Open neutral, Lost danger). "Jonas hasn't caught on Route 3 yet; the pair stays open until he does." Details of the plugin screens are on sheet 4 ([play-and-plugins.md](play-and-plugins.md)).

### 4j-app-6 Runs · states

Boards: [`S4j6`](../source/uzume/S4j6.dc.html) · [`S4j6Kinari`](../source/uzume/S4j6Kinari.dc.html). Built in: 0.18 package 9 (runs without plugin), 0.22 (plugins).

Six states: no run plugin on the server (Rules: None only; admins add the plugin in Plugins), plugin not on this PC (Install; Board, Recap, Log and Chat work without it), run full (8 people is the limit), plugin removed (run keeps board, recap, log, chat and saves; its tab is hidden until the plugin is back), where other plugin screens open (in game: a tab in the side panel after Run · Chat · Saves; on this PC: Settings › This PC › Plugins; never a sidebar row), empty (`StateBlock` "No runs yet. A run is a playthrough with its own save slot, for 1-8 people." New run...).

### 4j-app-7 In game · run tabs

Boards: [`S4j7`](../source/uzume/S4j7.dc.html) · [`S4j7Kinari`](../source/uzume/S4j7Kinari.dc.html). Built in: 0.18 package 9 (runs without plugin), 0.22 (plugins).

`RunHeader` (back to Runs, game, system, sub "Run: Ember Route", "2 watching"), `GameStage`, side panel (320) with `RunTabs` Run · Chat · Saves · plugin tab. The plugin tab shows "Next pair" and the route list (DRAWN BY SOULLINK) and Runners (`RunnerRow` with presence, status and badges). Hints Ctrl+Tab Next tab, F1 Hide panel.

## 4jh Run board and recap

### 4jh-app-1 Run board · admin

Boards: [`S4jh1`](../source/uzume/S4jh1.dc.html) · [`S4jh1Kinari`](../source/uzume/S4jh1Kinari.dc.html). Built in: 0.18 package 9 (runs without plugin).

Header "Ember Route", Export..., primary **Play**; Tabs Board / Recap / ... Runner table (runner, progress "4 badges · team 6", status, last, `Menu`). Save-slot section "Each runner plays in Run: Ember Route. Their Main slot is not touched." **Manage run** (admins only): Edit rules..., Pause for everyone, Remove runner... and End run... (danger). Ending or removing keeps board, recap, chat and saves.

### 4jh-app-2 Run recap · member

Boards: [`S4jh2`](../source/uzume/S4jh2.dc.html) · [`S4jh2Kinari`](../source/uzume/S4jh2Kinari.dc.html). Built in: 0.22 (plugins; run recap page per run).

Buttons Export as image, Share link (shares with everyone on the server). Four Ma metrics (Catches 41, Lost 9, Badges 11, Play time 62 h, each with a per-person split) and the **Lanes** block (DAY 1 to DAY 9, one lane per person, square glyphs; see Ma blocks in [accounts-and-server.md](accounts-and-server.md)).

## 8h What the server shows

### 8h-1 Raspberry Pi · terminal

Boards: [`S8h1`](../source/uzume/S8h1.dc.html) · [`S8h1Kinari`](../source/uzume/S8h1Kinari.dc.html). Built in: 0.18 package 6 (server commands).

Terminal (stage panel, Mono) after `sudo apt install ./uzume-server_0.17.0_arm64.deb` and `sudo systemctl enable --now uzume-server`. `uzume-server status` prints the block: "Uzume server 0.17.0 · ready for setup", Address (`192.0.2.10:8443`), Security code, Setup code (one use, valid 60 min), the instruction "Open Uzume on another PC → Set up a server → On another device", and the commands `uzume-server setup-code` / `uzume-server code`. Help column explains the block.

### 8h-2 Docker · logs

Boards: [`S8h2`](../source/uzume/S8h2.dc.html) · [`S8h2Kinari`](../source/uzume/S8h2Kinari.dc.html). Built in: 0.18 package 6 (server commands).

`docker run -d --name uzume -p 8443:8443 -p 3478:3478/udp -p 3478:3478/tcp -p 49160-49199:49160-49199/udp -e UZUME_PUBLIC_HOST=server.example.com:8443 -v /srv/uzume:/data ghcr.io/<owner>/uzume-server:0.17.0`. `docker logs uzume` shows log lines (data dir `/data` (`UZUME_DATA_DIR`), listening on 8443, relay 3478 udp/tcp, 49160-49199 udp, "no admin yet: waiting for setup") followed by the same setup block. Environment variables are read once at start.

### 8h-3 Windows · installer last page

Boards: [`S8h3`](../source/uzume/S8h3.dc.html) · [`S8h3Kinari`](../source/uzume/S8h3Kinari.dc.html). Built in: 0.18 package 11 (installer).

Shows the installer's last page for "Only the server" (same dialog as 8l-7, 1:1): "The Uzume server is running", address, Security code, Setup code `7H4K-2Q · 60 min`, "Enter the setup code in Uzume on another PC: Set up a server › On another device.", "New code later: `uzume-server setup-code`", Finish.

### 8h-4 Setup code · states

Boards: [`S8h4`](../source/uzume/S8h4.dc.html) · [`S8h4Kinari`](../source/uzume/S8h4Kinari.dc.html). Built in: 0.18 package 6 (server commands), 0.18 package 7 (app start and accounts).

Three states: code expired ("Codes work once and for 60 minutes."; get a new one with `uzume-server setup-code`), already used ("This server is already being set up"; if it was not you run `uzume-server reset`, which deletes everything), server restarted (new setup code needed, the security code stays the same).

## 8k Server problems

### 8k-1 Server offline · member

Boards: [`S8k1`](../source/uzume/S8k1.dc.html) · [`S8k1Kinari`](../source/uzume/S8k1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 7 (app start and accounts).

Library with the label `HOME SERVER · OFFLINE SINCE 14:02` and the line "Can't reach Home server since 14:02. Games on this PC still run; saves upload when it's back." (Details). Table with a status per row: ON THIS PC (playable) and NEEDS HOME SERVER (disabled rows).

### 8k-2 Server problems · states

Boards: [`S8k2`](../source/uzume/S8k2.dc.html) · [`S8k2Kinari`](../source/uzume/S8k2Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 7 (app start and accounts).

Six states: service stopped, admin on this PC (Start server; Windows asks for admin rights) and member (ask the admin), server older than the app (Update server; members "Ask Max to update Home server"; on the device `uzume-server update`), app older than the server (Update Uzume; games on this PC still run), security code changed (new code `P9TL-3QWX-H6ZE` vs the previous one; Uzume does not connect until compared again; It matches / It doesn't), starting (`StateBlock` loading, about 10 seconds).

## 8j On this PC and 8i Share with others

### 8j-1 On this PC · Library

Boards: [`S8j1`](../source/uzume/S8j1.dc.html) · [`S8j1Kinari`](../source/uzume/S8j1Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing).

AppNav without a server (THIS PC group). Library `ON THIS PC · 214 GAMES · 5 SYSTEMS`: Rescan folders, primary Add folder.... Saves belong to the person; there is no All users. "Upload, invites, Online and the Server group appear after Share with others."

### 8j-2 On this PC · Backups

Boards: [`S8j2`](../source/uzume/S8j2.dc.html) · [`S8j2Kinari`](../source/uzume/S8j2Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing).

Settings › This PC › Backups (second column: Your name, Appearance, Library folders, Plugins, Notifications, Backups 03:00, Storage, Servers "SHARE...", Updates, Diagnostics). Back up now; rows Schedule (runs when Uzume is open, otherwise at the next start), Keep the last (7), Include game files, Folder; Recent backups with **Restore...**.

### 8j-3 On this PC · Runs

Boards: [`S8j3`](../source/uzume/S8j3.dc.html) · [`S8j3Kinari`](../source/uzume/S8j3Kinari.dc.html). Built in: 0.18 package 9 (runs without plugin), 0.18 package 10 (on this PC and sharing).

Runs work with one person: own save slot, rules, recap. "Chat and people need a server."

### 8j-4 On this PC · states

Boards: [`S8j4`](../source/uzume/S8j4.dc.html) · [`S8j4Kinari`](../source/uzume/S8j4Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing).

Three states: Users from a link ("Users needs a server. On this PC there is only you."; Share with others...), join a server while local (the local library stays as On this PC; switch from the server line), run with friends (People checkboxes disabled: "Needs a server · Share with others or join one").

### 8i-1 Share with others · what changes

Boards: [`S8i1`](../source/uzume/S8i1.dc.html) · [`S8i1Kinari`](../source/uzume/S8i1Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing).

`Dialog` lg 1 of 3 "Run a server on this PC": library and saves stay and move into the server; four points: keeps running when Uzume is closed (Windows service, stop in Server › Settings), friends join with an invite (home or internet), admin rights needed once, you become the admin (with a password, nothing is lost).

### 8i-2 Share with others · set up

Boards: [`S8i2`](../source/uzume/S8i2.dc.html) · [`S8i2Kinari`](../source/uzume/S8i2Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing).

Dialog 2 of 3 "Set up Home server": Server name, Your name, Password (12+), `Select` Who can join (default "Home network only"; internet needs a public address and the relay), Port (8443). Note: Windows asks to allow the server through the firewall (private networks); for the internet also forward 8443 TCP, 3478 UDP+TCP and 49160-49199 UDP.

### 8i-3 Share with others · progress

Boards: [`S8i3`](../source/uzume/S8i3.dc.html) · [`S8i3Kinari`](../source/uzume/S8i3Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing).

Dialog 3 of 3 "Setting up...": plain dialog (not the busy one, so the step list stays readable) with a disabled primary "Sharing..."; steps Install the server service, Move your library (214 games stay in their folder), Move your saves (48 saves, 312 versions; ensō while working), Create admin, Start the server; thin `Progress`. If anything fails, everything goes back to On this PC and nothing is lost.

### 8i-4 Share with others · done

Boards: [`S8i4`](../source/uzume/S8i4.dc.html) · [`S8i4Kinari`](../source/uzume/S8i4Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing).

Library now with Upload... and primary **Invite a friend**; done line "Home server is running on this PC. Home network only." Dialog "You're sharing" with SERVER (`Home server · 192.0.2.10:8443 · on this PC`) and JOIN (Home network only). Internet sharing, users and backups are now on the Server screen (People, Settings).

### 8i-5 Stop sharing

Boards: [`S8i5`](../source/uzume/S8i5.dc.html) · [`S8i5Kinari`](../source/uzume/S8i5Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing).

Server › Settings › General rows: Server name, Runs on ("This PC · Windows service", Open Overview), **Stop sharing...** (danger). Dialog "Stop sharing?": others cannot connect ("Can't reach Home server"), their accounts and saves are kept in `C:\ProgramData\Uzume\server` and come back if you share again, your library and saves stay on this PC. Asks Confirm it's you and admin rights.

### 8i-6 Share with others · states

Boards: [`S8i6`](../source/uzume/S8i6.dc.html) · [`S8i6Kinari`](../source/uzume/S8i6Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing).

Six states: admin rights refused (nothing changed; Try again), port in use (Use 8444), install failed and rolled back (error 1053; library and saves untouched), stop while someone plays (Stop anyway / Wait; her app keeps the save and uploads later), stopped (data of others kept, 1.2 GB; Share again...; to delete for good run `uzume-server reset`), firewall rule missing (Add firewall rule; test with Connection check on another PC).

## 8l Windows installer

All 8l boards are light only (no Sumi twin), drawn at 1:1 with Uzume tokens: Windows draws the real installer in Segoe UI 9 pt with system colours; only the two bitmaps and the copy ship. The MSI client area is 493 x 360: the 493 x 312 bitmap sits above the 48 px button row; the old names in the upgrade dialog are intentional.

### 8l-0 Installer bitmaps

Boards: [`S8l0`](../source/uzume/S8l0.dc.html). Built in: 0.18 package 11 (installer).

Bitmaps: `WixUIDialogBmp` 493 x 312, 24-bit BMP (Welcome, Finish): Sumi strip with the installer tile (mark + server glyph, because one installer sets up the app, the server or both), Kinari text area right of x = 164 left plain for Windows text; `WixUIBannerBmp` 493 x 58 (every other page) with the vector Mark (36) and ServerGlyph (20), 14 px from the right edge. Icon (`ARPPRODUCTICON`, title bar) from the Pixel 32 / 16 tiles as ICO, kept by id, not drawn.

### 8l-1 Installer · first install

Boards: [`S8l1`](../source/uzume/S8l1.dc.html) (1280 x 1040). Built in: 0.18 package 11 (installer).

Four dialogs: 8L-1 Welcome ("Welcome to Uzume Setup", Next / Cancel); 8L-2 How will you use Uzume? (radio: **Uzume** - for you only; **Uzume with a server on this PC** - needs admin rights, installs for all users; **Only the server** - needs admin rights, shows address and codes at the end); 8L-3 Destination folder (per user `C:\Users\<name>\AppData\Local\Programs\Uzume\`, all users `C:\Program Files\Uzume`; needs 180 MB; games and saves are not in this folder); 8L-4 Options with a server (desktop shortcut, server port 8443, share in the home network = firewall rule; internet sharing is set up later in Server › Settings › Network & sharing).

### 8l-2 Installer · finish

Boards: [`S8l2`](../source/uzume/S8l2.dc.html) (1280 x 1040). Built in: 0.18 package 11 (installer).

Four dialogs: 8L-5 Progress ("Installing the server service...", status "Registering Uzume server"); 8L-6 Finish for Uzume (checkbox Start Uzume); 8L-7 Finish for the server only (address, security code, setup code, same as 8h-3); 8L-11 error "Port 8443 is used by another program. Pick another port, for example 8444."

### 8l-3 Installer · maintenance

Boards: [`S8l3`](../source/uzume/S8l3.dc.html) (1280 x 1040). Built in: 0.18 package 11 (installer).

Three dialogs: 8L-8 Change / Repair / Remove (Change switches between the three modes; Repair fixes files and the service, data stays; Remove keeps games, saves and server data); 8L-9 Remove ("Your saves, games and server data in `C:\ProgramData\Uzume` stay"; optional checkbox "Also delete all server data", cannot be undone); 8L-10 Upgrade from FrameBeam Player and Hub (found FrameBeam Player 0.16.4 and Hub 0.16.4, becomes "Uzume with a server on this PC"; library, saves, users and settings kept; old names and shortcuts removed). Not built: there is no upgrade from FrameBeam, the MSI replaces the product but takes over no data ([ADR 0030](../../adr/0030-one-app-one-server-uzume.md)).

## 8m Linux

### 8m-1 Linux · first start

Boards: [`S8m1`](../source/uzume/S8m1.dc.html) · [`S8m1Kinari`](../source/uzume/S8m1Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts), 0.18 package 11 (installer).

First start when the `uzume-server` package is installed: done line "A server is installed on this PC (uzume-server). Set it up here, or ignore it." Three numbered rows: 01 Set up the server on this PC (uses the installed package, you become the admin; SYSTEMD SERVICE · UZUME-SERVER), 02 Play on this PC, 03 Join a server. Without the server package that row is not shown and Set up a server offers "On another device" only. Hints A Choose, B Back. Handheld install and Linux installer screens beyond this board are in Later.
