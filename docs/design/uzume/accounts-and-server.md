# Accounts and server (sheet 2)

Canvas page "2 Accounts and server": accounts on a shared PC (8d to 8g) and everything an admin runs from the app (server status, library, saves, users, devices, systems and cores, plugins, server settings, reports, what's new, setup assistant), plus the "Ma blocks" reference board. This is the Uzume replacement of the Hub web admin: all of these screens live in the app (AppNav groups PLAY / SERVER / THIS PC), and the web admin is removed at the end of 0.18 (package 12). Tokens and components: [design-system.md](design-system.md).

Rules that hold on every screen of this sheet:

- Tables are hairline rows with the Yamabuki marker on the selected row; row details are a numbered or labelled **section under the table**, never a side panel (the open column stays free). Long admin pages put help text in the `HelpColumn` of the open column (label `UZUME · <AREA> · HOME SERVER`).
- Admin pages use 48 px top padding (not 56) so tables fit in 800 px. Second columns (Settings, Systems, This PC) are text items with the leading Yamabuki marker; Users uses `Tabs` (People · Invites · Devices).
- Dangerous admin steps ask "Confirm it's you" first (8f). Member versions of admin pages show a short "for admins" block and a way back; members never see the Server item and reach these pages only through old links and notifications.
- Boards are 1280 x 800 unless noted. Each has a Sumi board and a Kinari twin.
- "Built in" gives the roadmap package; the admin API behind these screens is package 5 and the server accounts and device approval are package 4.

## Index and reference

### 2 Accounts and server · Index

Boards: [`S2Index`](../source/uzume/S2Index.dc.html). Not built.

Index of sheet 2 (49 screens, 1280 x 800) with the rules above and 16 "did not fit" items; see [decisions.md](decisions.md) (section "Sheet 2", items 01 to 16).

### 2 Ma blocks

Boards: [`S2Blocks`](../source/uzume/S2Blocks.dc.html) (1280 x 1040, Sumi and Kinari on one board). Built in: 0.18 package 3 (design system in the app), 0.18 package 8 (app admin area).

- **Metrics** (6f-app-1): four columns split by `rule-2` lines, no boxes: Mono label, Light 28/32 value, thin `Progress`, one muted line (CPU 18 % / last hour 11 %, Memory 1.2 GB of 4 GB, Disk 61 %, Backup disk 92 % / 4.1 GB free). A warning puts the "!" square before the label. No coloured bars.
- **Storage bar** (3q-app-6): one 8 px bar on `rule-2`; kinds are ink tints (72 / 52 / 36 / 24 / 14 %), never hues; the legend is a hairline list carrying the values (Games, Saves and snapshots, Save states, Screenshots, Cores and firmware) so the bar is never read alone.
- **Lanes** (run recap 4jh-app-2): one 48 px lane per person on a `rule-2` track; square Mono glyphs: ■ catch, ✕ lost, ◆ badge, □ note, ▣ screenshot; nothing round but the ensō. Persons in `Avatar` order.
- **Dialog frame**: the system `Dialog` (400 / 560) over the scrim token; Mono label, title, text, content, actions, danger actions at the far right; no radius, no shadow.

## 8d Switch user

### 8d-1 Switch user (gamepad)

Boards: [`S8d1`](../source/uzume/S8d1.dc.html) · [`S8d1Kinari`](../source/uzume/S8d1Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

- Label `DESKTOP-LIVINGROOM · HOME SERVER`, "3 PEOPLE ON THIS PC". Title "Who's playing?" "People signed in on Desktop-LivingRoom. Saves always go to the person who is signed in."
- Tiles of people: `Avatar` 40 with the name below and a meta line (Max "Admin · today", Lena "Last used 2 days ago" with a PIN tag, Jonas "Last week"), plus "+ Add user / Sign in someone else".
- Hints: A Choose, X Remove from this PC, B Back. Opened by Ctrl+Shift+U or Hold Select + ↑.

### 8d-2 PIN · 2 of 4

Boards: [`S8d2`](../source/uzume/S8d2.dc.html) · [`S8d2Kinari`](../source/uzume/S8d2Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

- "PIN for this PC", avatar Lena, "Enter your PIN for this PC." Square digits fill in ink (never round); keypad 1-9, ⌫, 0, OK; link "Use password instead". Number keys work too.
- The PIN never leaves this PC (stored hashed, bound to the device). After 3 wrong PINs the password is needed (8d-3, 8d-7e).
- Gamepad: D-pad moves, A presses, B deletes, Y goes back to the people.

### 8d-3 PIN wrong

Boards: [`S8d3`](../source/uzume/S8d3.dc.html) · [`S8d3Kinari`](../source/uzume/S8d3Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

Label "TRY 2 OF 3", error line "Wrong PIN. 1 try left, then your password is needed." After the third try the PIN locks for that person on this PC only and the screen asks for the password once (8d-7e).

### 8d-4 Switch user (keyboard)

Boards: [`S8d4`](../source/uzume/S8d4.dc.html) · [`S8d4Kinari`](../source/uzume/S8d4Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

Same tiles as 8d-1; hints ← → Move, Enter Choose, Del Remove, Esc Back. The hint row follows the last input device.

### 8d-5 Switch while a game runs

Boards: [`S8d5`](../source/uzume/S8d5.dc.html) · [`S8d5Kinari`](../source/uzume/S8d5Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

- "Quit the game first": Uzume saves the running game, uploads that person's save, then shows who is playing. Three steps: Save Harbor Rally (MAIN · V15) → Upload to Home server (2.1 MB · 60 %) → Switch user (NEXT).
- Actions: **Save, quit and switch** (primary), **Keep playing**. Offline: the save stays on this PC under the first person and uploads at their next sign-in; others cannot open it.
- Rule: Uzume never switches with an unsaved game. Hints A Choose, B Keep playing.

### 8d-6 Add a user to this PC

Boards: [`S8d6`](../source/uzume/S8d6.dc.html) · [`S8d6Kinari`](../source/uzume/S8d6Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

Title "Add someone to this PC": Name, Password, link Forgot password?, primary Sign in, secondary Use an invite. No password: the admin approves this PC (as 8b-5g). After signing in the person joins those on this PC, can add a PIN in My account (8e); others stay signed in.

### 8d-7 Switch user · other states

Boards: [`S8d7`](../source/uzume/S8d7.dc.html) · [`S8d7Kinari`](../source/uzume/S8d7Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts).

Six states: 7a remove from this PC (nothing deleted on the server, all saves uploaded; danger button; undo by signing in again), 7b saves not uploaded ("1 save isn't uploaded yet: Copper Valley · v4 (server offline)."; wait or let the person sign in to upload), 7c session expired after 90 days on this PC (password once), 7d server offline (only people who used this PC before can continue, "Plays offline · saves upload later"; others "needs the server"), 7e PIN locked (password once to unlock), 7f only one person on this PC (Add user; Ctrl+Shift+U opens this screen).

## 8e My account

### 8e-1 My account · admin

Boards: [`S8e1`](../source/uzume/S8e1.dc.html) · [`S8e1Kinari`](../source/uzume/S8e1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 10 (on this PC and sharing).

- Settings with the This PC / Server `Segmented` ("Only affects this PC and your account."). Second column: My account, Appearance, Library folders (count), Plugins (count), Voice, Notifications, Servers (saved), Updates (version), Diagnostics.
- My account: `Avatar` with online status, "ADMIN · HOME SERVER". `SettingsRow`s: Display name (Rename...), Password (required for admins; Change...), PIN for this PC (4 digits; Set PIN...), "Ask for my password" (`Select` 2 / 5 / 15 min, how long a confirmation lasts on this PC).
- **Privacy** ("WHAT OTHERS SEE"): `Toggle` Show what I play (off: others see Online, Idle or Offline, never the game) and Discord status (shows game or run in the Discord app on this PC; nothing is sent when Discord is not running). Both off by default.
- My devices: "Sign out everywhere..." (danger) and a list (device, OS and app version, last seen, sign-in kind, Sign out).

### 8e-2 My account · member

Boards: [`S8e2`](../source/uzume/S8e2.dc.html) · [`S8e2Kinari`](../source/uzume/S8e2Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Same page for a member under the THIS PC switch-less Settings: password is optional ("Not set. Optional on Home server. With a password you can sign in on another PC without waiting for Max." Set password...), PIN Change... and Remove, Privacy toggles, device list with "Approved device".

### 8e-3 Change password

Boards: [`S8e3`](../source/uzume/S8e3.dc.html) · [`S8e3Kinari`](../source/uzume/S8e3Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 4 (server accounts).

`Dialog`: fields Current password, New password (error "At least 12 characters."), New password again. "Your other devices stay signed in. To sign them out, use Sign out everywhere."

### 8e-4 Sign out everywhere

Boards: [`S8e4`](../source/uzume/S8e4.dc.html) · [`S8e4Kinari`](../source/uzume/S8e4Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

`Dialog` "Sign out on all devices?": all devices need the password again; uploaded saves are safe. Warning line when a device holds a save that is not uploaded yet (it uploads after the next sign-in there; it is not lost).

### 8e-5 My account · other states

Boards: [`S8e5`](../source/uzume/S8e5.dc.html) · [`S8e5Kinari`](../source/uzume/S8e5Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 10 (on this PC and sharing).

Six states: 5a on this PC only ("No account needed", a name field for save files and screenshots, Share with others...), 5b server offline (name, password and devices cannot be changed; PIN still works), 5c remove password (member; not offered when the server requires passwords; admins cannot remove theirs), 5d set PIN (4 digits, works only on this PC), 5e rename with name taken, 5f device signed out as a Toast with Undo for 10 seconds.

## 8f Confirm it's you and 8g Forgot password

### 8f-1 Confirm it's you

Boards: [`S8f1`](../source/uzume/S8f1.dc.html) · [`S8f1Kinari`](../source/uzume/S8f1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 5 (admin API).

Two dialogs in sequence. First "Confirm it's you": "Enter your password. You won't be asked again for 5 minutes on this PC." (label names the next step). Then the action dialog, for example "Turn off Jonas?" with the note that Remove instead deletes the account and its devices but keeps all saves under Library › Saves › All users as "Jonas (removed)"; deleting a removed person's saves is a separate step that names every save.

### 8f-2 Confirmed · time left

Boards: [`S8f2`](../source/uzume/S8f2.dc.html) · [`S8f2Kinari`](../source/uzume/S8f2Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

After confirming, the page shows "Confirmed · 4:12 left" (done icon) near the title. Example Users › People: Jonas turned off ("Turned off · saves kept", status CAN'T SIGN IN), Toast "Jonas is turned off. Saves, play time and devices stay." with Undo.

### 8f-3 Confirm it's you · states

Boards: [`S8f3`](../source/uzume/S8f3.dc.html) · [`S8f3Kinari`](../source/uzume/S8f3Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Six states: wrong password (2 tries left), locked ("Admin steps are locked on this PC for 5:00. Everything else keeps working."), checking, server unreachable (cannot confirm), the list of steps that ask (delete a game, turn off / remove / reset password, delete a removed person's saves, restore a backup, update or go back, who can join and sharing), expired ("Your 5 minutes ran out. Nothing has changed yet.").

### 8g-1 Forgot password

Boards: [`S8g1`](../source/uzume/S8g1.dc.html) · [`S8g1Kinari`](../source/uzume/S8g1Kinari.dc.html). Built in: 0.18 package 7 (app start and accounts), 0.18 package 6 (server commands).

No email reset. Six states: member asks an admin ("Max gets a notification"), request sent (visible in the bell and Users), admin with other admins asks one of them, only admin runs `uzume-server reset-admin` on the server (Copy; prints a one-time password; saves, users and games untouched; Docker form `docker exec -it uzume-server uzume-server reset-admin`), server on this PC (terminal as administrator), admin resets a member (temporary password, devices stay signed in; asks Confirm it's you).

## Server

### 6f-app-1 Server status

Boards: [`S6f1`](../source/uzume/S6f1.dc.html) · [`S6f1Kinari`](../source/uzume/S6f1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 5 (admin API).

- Title "Home server", label `0.17.0 · UP 12 DAYS 4 H · PI-LIVINGROOM`. Actions: Connection check, Collect server logs... (ghost). Most urgent problem as a `StatusIcon` warning line with an action ("Backup disk is 92 % full. Tonight's backup may not fit." Open Storage); it also shows as a warning badge on Status in AppNav.
- Four metrics (see Ma blocks). Table **Sessions · 2** (host, game, system, viewers, Direct / Relayed with Mbit/s, since; section meta shows relay load, e.g. RELAY 3.1 OF 20 MBIT/S). Table **Connected apps · 3** (person, device, app version, since; an app older than the server shows "Update offered"). Last errors in the help column.

### 6f-app-2 Status · states

Boards: [`S6f2`](../source/uzume/S6f2.dc.html) · [`S6f2Kinari`](../source/uzume/S6f2Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Six states: all fine (nothing extra in the sidebar), server unreachable (last known version and uptime; Try again, Connection check; on the device `uzume-server status`), restarting after update (ensō; games keep running, saves upload after), loading (`StateBlock`), member from a link ("Only admins see server status"; Report a problem...), app out of date ("Jonas sees 'Home server needs a newer app' until he updates").

### 3j-app-1 Library · admin

Boards: [`S3j1`](../source/uzume/S3j1.dc.html) · [`S3j1Kinari`](../source/uzume/S3j1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 5 (admin API).

- "Library", label `128 GAMES · 5 SYSTEMS`. Primary Add games... (Rescan folders and Upload ROM are on Server › Games) `Tabs` Games / Saves. Resumable upload strip ("Uploading Tide & Lantern.nds · 41 OF 128 MB · 2.4 MB/S", Pause, Cancel, thin `Progress`).
- `TableRow`s; a status word per row ("On this PC only", "Uploading 32 %" with the row disabled). Selected row opens the admin **details section** under the table: Uploaded by, File path, SHA-256, Saves (people, versions); actions Rename..., Copy SHA-256, Rescan folder, Delete game... (danger).

### 3j-app-2 Library · dialogs and results

Boards: [`S3j2`](../source/uzume/S3j2.dc.html) · [`S3j2Kinari`](../source/uzume/S3j2Kinari.dc.html) (1280 x 1040). Built in: 0.18 package 8 (app admin area).

Nine states: delete game (file removed for everyone, all saves kept; Confirm it's you; undo by adding the file again), rescan running (progress, people keep playing), rescan result ("3 new games, 1 moved, 0 missing", files not added), upload interrupted (Resume; nothing is added until the file is complete and its SHA-256 checked), already in the library (same SHA-256), not a game file (.gb, .gbc, .gba, .nds, .3ds only), rename (display name only; the file keeps its name), game on this PC only (Upload to Home server / Keep local; saves stay yours), member uploads off ("Max adds games on Home server.").

### 3j-app-3 Library · member

Boards: [`S3j3`](../source/uzume/S3j3.dc.html) · [`S3j3Kinari`](../source/uzume/S3j3Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.19 (player comfort and library).

Member Library: `SearchField`, Games / Saves tabs, the same table without admin parts; the selected game shows a short block (system and size, play time, own saves "Main v9 · Slot 2 v2") with the primary **Play**. Game page and Continue are on sheet 4 ([play-and-plugins.md](play-and-plugins.md)).

### 3s-app-1 Saves · all users · conflict

Boards: [`S3s1`](../source/uzume/S3s1.dc.html) · [`S3s1Kinari`](../source/uzume/S3s1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 5 (admin API).

- Library tab Saves with `Segmented` "Show saves of": Mine / All users. Table person, game, slot, latest; tags Conflict (accent plane with "!"), Removed ("Sam (removed)"). Every save belongs to one person.
- Detail section for the selected save (slot, snapshots, states). Conflict: "Two versions were made on two devices at nearly the same time. Pick the one to keep; the other stays in the history." Two blocks (v15 per device, time and game time, "Further in the game"), each with **Keep this**.

### 3s-app-2 Saves · restore for another person

Boards: [`S3s2`](../source/uzume/S3s2.dc.html) · [`S3s2Kinari`](../source/uzume/S3s2Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Confirm it's you, then "Restore v6 for Lena?": v6 becomes the newest version (v9), v8 stays as "Before restore". If the owner is playing the game, the restore waits until she quits and she gets a notification.

### 3s-app-3 Saves · member

Boards: [`S3s3`](../source/uzume/S3s3.dc.html) · [`S3s3Kinari`](../source/uzume/S3s3Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

"YOUR SAVES": game, system, slot, latest. Selected save shows its history (version, time, device, Restore...). Restoring your own save never deletes anything; the current version stays as "Before restore".

### 3s-app-4 Saves · states

Boards: [`S3s4`](../source/uzume/S3s4.dc.html) · [`S3s4Kinari`](../source/uzume/S3s4Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Six states: delete a removed person's saves (names every save; deletes for good; still in the last 7 backups; Confirm it's you), conflict resolved (Toast "Kept v15 from Jonas-Laptop", the other is "Not kept" in the history), empty, server offline (only Mine, on this PC), state from an older core (may not load; states can be downloaded, not uploaded), restore while the game runs (quit first).

### 3n-app-1 Users › People

Boards: [`S3n1`](../source/uzume/S3n1.dc.html) · [`S3n1Kinari`](../source/uzume/S3n1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 4 (server accounts).

"Users", primary Create invite..., Tabs People · Invites · Devices. Table person (since), role, status (ONLINE, PLAYING, OFFLINE · 2 H), play time, password (Set / Not set), device count. Detail section for the selected person: play time and sessions, last played, password, devices; actions Make admin..., Reset password..., Turn off..., Remove... (danger).

### 3n-app-2 Users › Invites

Boards: [`S3n2`](../source/uzume/S3n2.dc.html) · [`S3n2Kinari`](../source/uzume/S3n2Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 4 (server accounts).

Table code (partly hidden `UZ-8XK2-····`), role, uses, expires, made by; Withdraw. Dialog "Create an invite": Role (Admins must set a 12+ character password when they join), Valid for (7 days), Uses (1 person). Result dialog "Send this to your friend": the code `UZ-8XK2-M4QD` shown once in the large invite-code style plus Copy link (Ctrl+C); the link contains the security code, so the friend skips the code check.

### 3n-app-3 Users · actions and states

Boards: [`S3n3`](../source/uzume/S3n3.dc.html) · [`S3n3Kinari`](../source/uzume/S3n3Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 4 (server accounts).

Six states: make admin (a person without a password must set one of 12+ characters before admin tools open), temporary password shown once, turn off (signed out everywhere; saves, play time and devices stay; undo = Turn on), last admin (Turn off is disabled: "Make someone else admin first."), member from a link (Users is for admins), no open invites (empty state with Create invite...).

### 3m-app-1 Users › Devices

Boards: [`S3m1`](../source/uzume/S3m1.dc.html) · [`S3m1Kinari`](../source/uzume/S3m1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 4 (server accounts).

Tab Devices with a count badge. **Waiting for approval** on top: device, OS, asked time and network; "Asks to sign in as Lena, who has no password. Approve only if you know this PC."; Approve (primary), Deny, `Select` "Approve as". Below, **All devices** (person, device, app version, last seen, Sign out which revokes the device).

### 3m-app-2 Devices · states

Boards: [`S3m2`](../source/uzume/S3m2.dc.html) · [`S3m2Kinari`](../source/uzume/S3m2Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Six states: Approve as open, approved (Toast with Undo), deny (the PC shows "Max declined this PC"; can ask again after 10 minutes), sign out someone's device (warns when a save on it is not uploaded), nothing waiting (empty state), request expired after 24 hours.

### 3l-app-1 Systems & cores › Cores

Boards: [`S3l1`](../source/uzume/S3l1.dc.html) · [`S3l1Kinari`](../source/uzume/S3l1Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Second column "SYSTEMS" (GB, GBC, GBA, NDS, 3DS, each with its picked core or "No core" and a warning) - same for everyone on the server. Page NDS: warning "No core picked. NDS games can't start until you pick one." Table "Cores from the buildbot · CHECKED TODAY 09:00": core, version, size, "good to know", **Use this**. Nothing is preselected. `SettingsRow` + `Toggle` "Update cores automatically" (nightly; saves never change; states from an older core may not load).

### 3l-app-2 Systems & cores › Firmware

Boards: [`S3l2`](../source/uzume/S3l2.dc.html) · [`S3l2Kinari`](../source/uzume/S3l2Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Tab Firmware: table file, system chip, status `Tag` (Matches ok, Missing neutral for optional, Missing danger for needed) and Add.... Files are the user's own; none ship with Uzume.

### 3l-app-3 Systems & cores · states

Boards: [`S3l3`](../source/uzume/S3l3.dc.html) · [`S3l3Kinari`](../source/uzume/S3l3Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Six states: buildbot unreachable (installed cores keep working), core download failed (Try again / Pick another), switch core (saves keep working, states from the old core will not load), firmware does not match (kept; Replace... / Keep anyway), member from a link (use Emulation), core update ready (installs tonight or Update now).

### 4i-app-1 Plugins · server

Boards: [`S4i1`](../source/uzume/S4i1.dc.html) · [`S4i1Kinari`](../source/uzume/S4i1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.22 (plugins).

"Plugins" (server group), primary Upload plugin.... Table plugin (name, version), origin `Tag` (Official ok, Not signed danger with "!"), permissions, users, `Toggle` On. **Official index** (signed, checked today): name, version, description, Add. Detail section for an uploaded plugin: "Uzume hasn't checked this file. People see a warning before turning it on.", Use previous version, Download file, Remove from server.... Plugin ID of the run plugin is `uzume.soullink`, the shared base `uzume.soullink-core`.

### 4i-app-2 Settings › This PC › Plugins

Boards: [`S4i2`](../source/uzume/S4i2.dc.html) · [`S4i2Kinari`](../source/uzume/S4i2Kinari.dc.html). Built in: 0.18 package 10 (on this PC and sharing), 0.22 (plugins).

Per-person plugin list in Settings › This PC: `SettingsRow` + `Toggle` per plugin ("1.4.0 · Official. Run board and panel in game. Hotkey Ctrl+H."; "Turned off on Home server. Offered once Max turns it on."), "Plugin panel position" `Select` (Left / Right), `SettingsDisclosure`. Removing one here uninstalls it from this PC only.

### 4i-app-3 Plugins · dialogs and states

Boards: [`S4i3`](../source/uzume/S4i3.dc.html) · [`S4i3Kinari`](../source/uzume/S4i3Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.22 (plugins).

Six states: upload of an unsigned plugin (Checkbox per permission: Show panels in game, Share run progress, Read game memory; saved with the form), use previous version, index unreachable (last good copy), needs a newer server ("Splits Timer 0.6 needs server 0.18+"), member (Max manages plugins; Open my plugins), remove from server (people keep their copy; run data stays).

### 3q-app-1 Server › General

Boards: [`S3q1`](../source/uzume/S3q1.dc.html) · [`S3q1Kinari`](../source/uzume/S3q1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 5 (admin API).

Server settings page 1 of 7, on the Server › Settings tab. `SettingsRow`s: Server name (Rename...), Import folder (`/srv/uzume/import`, Change...; files are added on Rescan folders in Server › Games), Members can add games (`Toggle`), Joining (`Select`: Invite only), Run setup again (Open...; nothing is reset). Help in the open column.

### 3q-app-2 Server › Network & sharing

Boards: [`S3q2`](../source/uzume/S3q2.dc.html) · [`S3q2Kinari`](../source/uzume/S3q2Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 5 (admin API).

Done line "Prepared for the internet: public address and relay are set." Rows: Share over the internet (`Toggle`; asks Confirm it's you), Public address (Change...), Ports (`8443 TCP · 3478 UDP+TCP · 49160-49199 UDP`, Copy), Relay (TURN) (`Segmented` Built-in / External / Off; `UZUME_TURN` only switches the built-in relay, external servers are only in `UZUME_TURN_EXTERNAL`, e.g. `turn.example.com:3478`), Security code (Show). Read-only table of `UZUME_*` variables from the server's environment (`UZUME_PUBLIC_HOST`, `UZUME_TURN_EXTERNAL`, `UZUME_DATA_DIR`).

### 3q-app-3 Server › Sign-in

Boards: [`S3q3`](../source/uzume/S3q3.dc.html) · [`S3q3Kinari`](../source/uzume/S3q3Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 4 (server accounts).

Rows: Passwords (`Select`: Optional for members / Required for everyone; admins always need 12+ characters), Show names on sign-in (`Toggle`, home network only, off by default), Wrong tries before a wait (5 tries, then 5 minutes), Stay signed in (90 days). Changed rows carry the changed marker; warning when a change affects people ("Lena has no password and must set one at her next sign-in. Nobody is signed out."); Save / Cancel.

### 3q-app-4 Server › Updates

Boards: [`S3q4`](../source/uzume/S3q4.dc.html) · [`S3q4Kinari`](../source/uzume/S3q4Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 6 (server commands).

Done line "Server 0.17.1 is ready · fixes 3 problems." Update now (primary). Channel (`Segmented` Stable / Beta), Install updates (`Segmented` Automatically / Ask me; automatic updates run at 04:00 when nobody plays). **Earlier versions** (3 kept): version, period used, Go back... (asks Confirm it's you).

### 3q-app-5 Server › Backups

Boards: [`S3q5`](../source/uzume/S3q5.dc.html) · [`S3q5Kinari`](../source/uzume/S3q5Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 6 (server commands).

Back up now (primary). Rows: Schedule (`Segmented` Off / Daily / Weekly; daily at 03:00), Keep the last (7 backups), Include game files (+38 GB per backup), Folder (`/mnt/backup/uzume`). Table Recent backups (when, size, kind Scheduled / Safety · before restore, Download).

### 3q-app-6 Server › Storage

Boards: [`S3q6`](../source/uzume/S3q6.dc.html) · [`S3q6Kinari`](../source/uzume/S3q6Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Storage bar block (see Ma blocks) and a warning when the backup disk is nearly full (nav warning "92 % BACKUP"). **Clean-up** suggestions with sizes: snapshots older than 90 days (plugin snapshots excluded), upload leftovers older than 7 days, unused cores; each Clean up... asks for confirmation. Clean-up never offers saves.

### 3q-app-7 Server › Diagnostics

Boards: [`S3q7`](../source/uzume/S3q7.dc.html) · [`S3q7Kinari`](../source/uzume/S3q7Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 6 (server commands).

Rows: Log detail (`Segmented` Normal / Detailed; switches back after 24 h), Metrics endpoint (`Toggle`, `server.example.com:8443/metrics` with an admin token), Connection check (Run...: server reachable → STUN → TURN → NAT type → test; the server cannot test itself from outside). Collect server logs (last 7 days; progress; zip `uzume-diag-home-server.zip`). INCLUDED: server log, settings without secrets, versions, disk and network summary. NEVER INCLUDED: passwords, invite codes, saves, chat, game files.

### 6d-app-1 Reports

Boards: [`S6d1`](../source/uzume/S6d1.dc.html) · [`S6d1Kinari`](../source/uzume/S6d1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 5 (admin API).

Server group, `Tabs` All · New · Seen (with the New count); inbox table: status `Tag` (New accent plane, Seen neutral), report, from, sent. Detail section: from (person, device, app version), linked event ("Audio underrun · 14:29"), the person's text, attachments (app-log.txt, diagnostics.json, screenshot.png, total size, Download all), Mark as seen. Sidebar Reports shows a count of New.

### 6d-app-2 Reports · states

Boards: [`S6d2`](../source/uzume/S6d2.dc.html) · [`S6d2Kinari`](../source/uzume/S6d2Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Three states: empty, member ("Reports go to admins"; send one from Settings › This PC › Diagnostics or from any error), attachments expired (kept 30 days; the description stays).

### 6a-app-1 What's new · admin

Boards: [`S6a1`](../source/uzume/S6a1.dc.html) · [`S6a1Kinari`](../source/uzume/S6a1Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

One `Dialog` lg "Uzume 0.17.0" (label with date). Sections ADDED / CHANGED / FIXED with a tag column App / Server / Both per item. Admins also get the server-version line ("Home server runs 0.16.4. Update it to get the server items." Open Updates).

### 6a-app-2 What's new · member

Boards: [`S6a2`](../source/uzume/S6a2.dc.html) · [`S6a2Kinari`](../source/uzume/S6a2Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

Same dialog without server-only items and without the server-version line: members see only items that affect them.

### 7a-app-1 Setup assistant · internet

Boards: [`S7a1`](../source/uzume/S7a1.dc.html) · [`S7a1Kinari`](../source/uzume/S7a1Kinari.dc.html). Built in: 0.18 package 8 (app admin area), 0.18 package 7 (app start and accounts).

Runs after 8a-5 (also from Server › Settings › General › Run setup again). Six-step stepper on hairlines: 01 Systems, 02 Cores, 03 ROM folders, 04 Internet access, 05 Backups, 06 Done; every step is skippable. Step 4: public address (field), relay (built-in on), ports to forward, result "Prepared for the internet". Buttons Only home network, Skip this step (ghost), Next (primary). Hints A Next, B Skip.

### 7a-app-2 Setup assistant · done

Boards: [`S7a2`](../source/uzume/S7a2.dc.html) · [`S7a2Kinari`](../source/uzume/S7a2Kinari.dc.html). Built in: 0.18 package 8 (app admin area).

"Home server is ready": a summary per step with done or warning icons (e.g. Cores "4 of 5 · NDS has no core" with Set up); anything open also shows in Status. Next: invite friends from Users › Invites. Buttons Create an invite, Open Library (primary).
