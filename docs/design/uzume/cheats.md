# Cheats (sheet 4, family 11)

Canvas page "4 Play and plugins" (Round 5, cheats addition, 2026-10-23): the Cheats tab in game (11a), adding and importing cheats (11b), the cheat database (11c), first-use consent (11d), the run rule at the start (11e) and during the run (11f), save versions made with cheats (11g) and the cheat database setting (11h). 11e and 11f also sit on page "3 Runs and server" as linked copies (`S11e1On3`, `S11f1On3` to `S11f3On3`, with Kinari twins) that import the originals. Index: [`S4Index`](../source/uzume/S4Index.dc.html) (decisions and gaps), [`S3Index`](../source/uzume/S3Index.dc.html) (linked row). Tokens and components: [design-system.md](design-system.md). Built in 0.20 ([roadmap](../../roadmap.md), section Cheats); background: [12-plugins.md](../../architecture/12-plugins.md) 16.5 (permission `cheats`).

Rules that hold on every screen of this family:

- **Where cheats come from.** The UI comes from the official plugin **Cheats**. The codes (Action Replay, GameShark, CodeBreaker, Game Genie, Gateway for 3DS) run inside the emulator core; the app owns consent, the "Before cheats" copy and the run log, so no plugin can skip them.
- **Format codes** are 10 px Mono tags on every row: `GSA`, `CB`, `AR DS`, `AR GBA`, `RAW`, `GATEWAY`. A cheat in a format the running core cannot use stays in the list, toggle off and disabled, with the reason in the meta line ("not supported by mGBA").
- **Groups** in the list: From the cheat database, Mine, Imported, each with a count.
- **Runs.** Cheats are **Off** by default; the run's creator or an admin allows them ("Allowed, logged") at the start or at any time during the run. Every rule change and every cheat turned on or off is a run log row with the person colour. Turning cheats off turns everyone's active cheats off; the cheats stay in the lists.
- **Only on this PC** works the same: cheats and safety copies stay on this PC, and the cheat database setting moves to Settings › This PC.
- Mock data only: games Lumen Drift, Harbor Rally, Coral Saga: Ember; made-up hex like `0212A4C8 000F423F`; source `server.example.com`. Boards are 1280 x 800, Sumi with a Kinari twin. New tokens: none.

## 11a Cheats tab in game

### 11a-1 Cheats tab

Boards: [`S11a1`](../source/uzume/S11a1.dc.html) · [`S11a1Kinari`](../source/uzume/S11a1Kinari.dc.html). Revises the side panel of 3g.

Running game (Lumen Drift, GBA, header sub "Shared · 2 watching · 3 cheats on"), side panel tabs Session · People · Chat · Saves · **Cheats** (Chat without its count, see gaps). Eyebrow "DRAWN BY CHEATS" with "3 ON". Buttons **Add cheat** and Cheat database (ghost), `SearchField` "Search cheats" (Ctrl+F). Grouped rows: name, format tag, `Toggle` (aria-label = name); "Debug menu" `RAW` "not supported by mGBA" disabled. Hints Ctrl+F Search, F4 Panel, A On / off, X Add.

### 11a-2 Cheats off in a run

Boards: [`S11a2`](../source/uzume/S11a2.dc.html) · [`S11a2Kinari`](../source/uzume/S11a2Kinari.dc.html).

Harbor Rally (NDS) in run Rally Night, tabs Run · Chat · Saves · Cheats, eyebrow meta "OFF IN THIS RUN". Notice (`StatusIcon` warning) "Cheats are off in this run" · "Lena started Rally Night; she or an admin can allow them in the run's Rules." The whole list below is dimmed to 55 % with `aria-disabled` and disabled controls.

### 11a-3 States

Boards: [`S11a3`](../source/uzume/S11a3.dc.html) · [`S11a3Kinari`](../source/uzume/S11a3Kinari.dc.html).

a none yet ("No cheats for Lumen Drift yet", "Add your own code, or see what the cheat database has for this game.", Add cheat, Look in the cheat database) · b format not supported (row stays, toggle off and disabled, reason in the meta line) · c search with no match ("No cheats match "jump".", Clear search) · d on this PC only ("Cheats work the same when Uzume runs only on this PC. Your cheats and the safety copies stay on this PC.") · e acts after a reset (`Tag` "After reset", "This code works only from the start of the game.", Reset game…) · f turned on (`Toast` done "Infinite money is on", action Turn off; in a run the log also gets "Max turned on Infinite money").

## 11b Add or import

### 11b-1 Enter code

Boards: [`S11b1`](../source/uzume/S11b1.dc.html) · [`S11b1Kinari`](../source/uzume/S11b1Kinari.dc.html).

Dialog lg "Add cheats to Lumen Drift" (label "CHEATS · GBA · MGBA") over 11a-1, `Tabs` Enter code · Import file · Cheat database. Name `TextField`; Format `Select` (Detect "Found: GSA", GameShark (GSA), CodeBreaker (CB), Action Replay (AR GBA)); Code: framed Mono box with line numbers (role textbox, aria-multiline). Under it `StatusIcon` done "3 lines · GameShark (GSA)" and "It goes to Mine. Turning it on asks once per game (11d)." Actions **Add cheat**, Cancel; hints L/R Tab, A Choose, Start Add, B Cancel.

### 11b-2 Line errors

Boards: [`S11b2`](../source/uzume/S11b2.dc.html) · [`S11b2Kinari`](../source/uzume/S11b2Kinari.dc.html).

Code box with danger border; bad lines get "✕ <n>" in the gutter and the danger tint, plus one `StatusIcon` error line each ("Line 2: 7 digits before the space. GSA lines are 8 + 8 hex digits.", "Line 3: "G" isn't a hex digit."). Add cheat disabled.

### 11b-3 Import file

Boards: [`S11b3`](../source/uzume/S11b3.dc.html) · [`S11b3Kinari`](../source/uzume/S11b3Kinari.dc.html).

File row "LUMEN-DRIFT.CHT · 4 KB" with Choose another…; "14 cheats found; 12 work with mGBA." Checkbox list (All 12, rows with format tags, "…and 8 more"); an unsupported row (`RAW` "not supported") is not selectable. Action **Import 11 cheats**, Cancel.

### 11b-4 States

Boards: [`S11b4`](../source/uzume/S11b4.dc.html) · [`S11b4Kinari`](../source/uzume/S11b4Kinari.dc.html).

a valid ("Detect picked the format from the code.") · b wrong format picked ("These lines look like CodeBreaker, not GSA.", Use CodeBreaker) · c duplicate name (field error) · d duplicate names on import ("3 names already exist", "Keep both adds "(2)" to the new names.") · e no cheats in file ("Uzume reads .cht and .txt files with one cheat name, then its code lines.") · f imported ("11 cheats imported to Imported"; the sheet closes and the tab scrolls to Imported).

## 11c Cheat database

### 11c-1 Found

Boards: [`S11c1`](../source/uzume/S11c1.dc.html) · [`S11c1Kinari`](../source/uzume/S11c1Kinari.dc.html).

Tab Cheat database: MATCHED "LUMEN DRIFT · GBA · 9F3A…C21E" (Mono, short file hash), category chips (All · 18, Money · 3, Items · 6, Movement · 4, Debug · 5), rows with format tag and Add (or `Tag` Added), "12 more", source line "FROM SERVER.EXAMPLE.COM · UPDATED 21 OCT". Actions **Add all 14**, Close.

### 11c-2 States

Boards: [`S11c2`](../source/uzume/S11c2.dc.html) · [`S11c2Kinari`](../source/uzume/S11c2Kinari.dc.html).

a no match ("No cheats for this game in the database") · b off on the server ("The cheat database is off", "Admins turn it on in Server › Settings › Cheat database."; admins see Open settings) · c offline (server offline, last downloaded copy with its date) · d searching ("Looking up Lumen Drift…") · e all added (Add all turns off) · f on this PC only ("Without a server, this PC downloads the database itself.", 11h-3).

## 11d First-use consent

Boards: [`S11d1`](../source/uzume/S11d1.dc.html) · [`S11d1Kinari`](../source/uzume/S11d1Kinari.dc.html).

Dialog "Turn on cheats for Lumen Drift?" (label "CHEATS · ONCE PER GAME"), numbered list: 01 "Uzume first saves a safety copy of your save: Main v14 is kept as "Before cheats"." 02 "Later saves are marked "Made with cheats"." 03 "In a run, everyone sees each cheat in the run log." Meta "Asked once per game. Codes run inside the emulator core." Actions **Turn on cheats** (focused), Cancel.

## 11e Run rule at the start

Boards: [`S11e1`](../source/uzume/S11e1.dc.html) · [`S11e1Kinari`](../source/uzume/S11e1Kinari.dc.html); linked on sheet 3: [`S11e1On3`](../source/uzume/S11e1On3.dc.html) · [`S11e1On3Kinari`](../source/uzume/S11e1On3Kinari.dc.html). Revises the Rules section of Start a run (4j).

Start a run (Rally Night, Harbor Rally, Lena starts it), section Rules: `Segmented` **Cheats: Off** (default) / Allowed, logged; "Allowed cheats show in the run log for everyone. You can change this later." The SAVE SLOT line stays.

## 11f Run rule during the run

### 11f-1 Rule and log

Boards: [`S11f1`](../source/uzume/S11f1.dc.html) · [`S11f1Kinari`](../source/uzume/S11f1Kinari.dc.html); linked [`S11f1On3`](../source/uzume/S11f1On3.dc.html) · [`S11f1On3Kinari`](../source/uzume/S11f1On3Kinari.dc.html). Revises the run view (4jh).

Run header gets **Rules…**. The Log tab shows the row CHEATS "Allowed, logged · changed by Lena today 21:30" with Change…; filter chips All · Cheats · 3 · Rules · 1 · Export. Log table WHEN · WHO · EVENT · KIND with kinds Cheat, Rule, Run ("Lena changed Cheats: Off → Allowed, logged", "Max turned on Infinite money").

### 11f-2 Turn cheats off

Boards: [`S11f2`](../source/uzume/S11f2.dc.html) · [`S11f2Kinari`](../source/uzume/S11f2Kinari.dc.html); linked [`S11f2On3`](../source/uzume/S11f2On3.dc.html) · [`S11f2On3Kinari`](../source/uzume/S11f2On3Kinari.dc.html).

Full confirm dialog "Turn cheats off in Rally Night?" (label "RUN · RULES"), actions **Turn cheats off** (focused), Cancel.

### 11f-3 States

Boards: [`S11f3`](../source/uzume/S11f3.dc.html) · [`S11f3Kinari`](../source/uzume/S11f3Kinari.dc.html); linked [`S11f3On3`](../source/uzume/S11f3On3.dc.html) · [`S11f3On3Kinari`](../source/uzume/S11f3On3Kinari.dc.html).

a allow, lighter confirm ("Allow cheats in Rally Night?", "Everyone sees each cheat in the run log.") · b toast "Lena allowed cheats in this run" (to the other players, in game and on any page, action Open log) · c toast "Cheats are off again, your active cheats were turned off" · d member view ("Off · set by Lena"; only the creator and admins see Change… and Rules…) · e log rows · f Rules… opens the same rows as Start a run.

## 11g Saves

### 11g-1 Cheats marker

Boards: [`S11g1`](../source/uzume/S11g1.dc.html) · [`S11g1Kinari`](../source/uzume/S11g1Kinari.dc.html). Revises saves (3s).

Saves list: the LATEST column shows `Tag` Cheats. History rows: v12/v11 `Tag` Cheats "made with cheats", v10 `Tag` **Before cheats** "Automatic · made before Infinite money was turned on", older versions unmarked; Restore… as today; "Restoring your own save never deletes anything; the current version stays as "Before restore"."

### 11g-2 Restore before cheats

Boards: [`S11g2`](../source/uzume/S11g2.dc.html) · [`S11g2Kinari`](../source/uzume/S11g2Kinari.dc.html).

Dialog "Restore v10 "Before cheats"?" with "Cheats you turned on stay on; turn them off in the game's Cheats tab." Actions **Restore v10** (focused), Cancel.

## 11h Cheat database setting

### 11h-1 Server › Settings

Boards: [`S11h1`](../source/uzume/S11h1.dc.html) · [`S11h1Kinari`](../source/uzume/S11h1Kinari.dc.html). Revises the settings list of 3q.

Item **Cheat database** (value ON) after Updates. Page: `Toggle` Download the cheat database, Source (Mono address; the built default is the libretro database, see roadmap), Last updated with Update now, Import from file…, status "Up to date · DB 2026-10-23 · 18 MB". Help column: What this is ("A list of known cheats per game, matched by the game file. Cheats themselves come from the official Cheats plugin."; "People can always enter or import their own cheats, with or without the database.").

### 11h-2 States

Boards: [`S11h2`](../source/uzume/S11h2.dc.html) · [`S11h2Kinari`](../source/uzume/S11h2Kinari.dc.html).

a up to date · b updating ("10 OF 18 MB", "The current copy stays in use until this finishes.") · c failed ("Update failed: server.example.com didn't answer within 30 s.", "The copy from 21 Oct stays in use.", Try again) · d off ("People can still enter and import their own cheats.") · e import from file (zip name, size, "2,412 games · 5 systems", Use this file) · f wrong file ("This file isn't a cheat database.").

### 11h-3 Settings › This PC

Boards: [`S11h3`](../source/uzume/S11h3.dc.html) · [`S11h3Kinari`](../source/uzume/S11h3Kinari.dc.html).

The same block in Settings › This PC ("THIS PC · NO SERVER", "Only affects this PC.") when the server runs only on this PC; it configures the app's own child server.
