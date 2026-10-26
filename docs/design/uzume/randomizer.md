# Randomizer (sheet 5)

Canvas page "5 Randomizer" (Round 5, batch 5, 2026-10-23): randomized game variants on the game page (10a), the Randomize dialog (10b), building a variant (10c), variant states (10d), the Randomizer section of New run (10e), joining and playing a randomized run (10f), Library, slots and Storage (10g) and Server › Tools (10h). Index: [`S5Index`](../source/uzume/S5Index.dc.html). Tokens and components: [design-system.md](design-system.md). Decision record: [ADR 0031](../../adr/0031-game-variants-and-randomizer.md). Built in 0.21 ([roadmap](../../roadmap.md)).

Rules that hold on every screen of this sheet:

- **A variant** is a library game plus a **recipe**: engine version, preset or settings string, seed (and for 3DS the update version). The app builds it on this PC from the user's own game file; only the recipe goes to the server. Each variant plays in its **own save slot** named like the variant (a run variant in the run slot "Run: <Name>"); Main is never touched.
- The UI comes from the official plugin **Randomizer**; it has **no sidebar row**. Randomizing starts from the game page (Randomize… next to Play) or from New run. The engine is a **tool** the server offers to apps like cores (Server › Tools).
- **Spoilers stay hidden.** The app never shows what was randomized. The spoiler log is off by default, opens only from a variant's menu after a confirm, and for run variants only after the run ends and only the own one. Build error details hold no spoiler (no creature, item or trainer names).
- **Engine name.** "FVX" appears only in 10 px Mono lines (version, license, source link); normal copy says "the randomizer engine" or "the engine".
- Revisions stay in the 10 family; each spec below names the board it revises. Boards are 1280 x 800, Sumi with a Kinari twin; each family has a handoff sheet `S10xH` (grid and spacing, components, exact copy in reading order, keys and gamepad, states copy). New tokens: none.

## 10a Game page · Randomize

### 10a-1 Game page · Randomize

Boards: [`S10a1`](../source/uzume/S10a1.dc.html) · [`S10a1Kinari`](../source/uzume/S10a1Kinari.dc.html). Revises 3c-1. Handoff [`S10aH`](../source/uzume/S10aH.dc.html).

Header actions: Favourite, **Randomize…** (secondary), primary **Play · Main v31**. Facts gain the row RANDOMIZER "Supported · update 1.4". Section 2 **Variants** (meta "2 VARIANTS · OWN SLOTS"): table VARIANT (name, "Created 14 Oct") · PRESET · SLOT (Mono "Randomizer #1 · v6") · STATUS (`Tag` Ready / Needs rebuild / Building) · actions (Play or Rebuild, `Menu`: Play Enter, Rename… F2, Rebuild, Open spoiler log…, Delete… Del). The focused row uses `--focus-ring-inset`. Line under the table: "Each variant plays in its own slot. Main is not touched." Help column: Variants ("A variant is this game plus a recipe: preset, settings and seed. Your PC builds it from your own game file; only the recipe goes to Home server."), Spoilers; hints X Randomize…, Y Variant menu, F2 Rename, Del Delete….

### 10a-2 Delete a variant

Boards: [`S10a2`](../source/uzume/S10a2.dc.html) · [`S10a2Kinari`](../source/uzume/S10a2Kinari.dc.html).

Dialog lg "Delete Chaos night?": "The variant's files go from this PC. Its slot has 9 save versions on Home server." `Segmented` **Keep saves** (default) / Delete saves too; "Kept saves stay on Home server in the slot Chaos night, with the recipe. Build it again later to play them." Actions Delete variant (danger), Cancel (focused).

### 10a-3 Variant states

Boards: [`S10a3`](../source/uzume/S10a3.dc.html) · [`S10a3Kinari`](../source/uzume/S10a3Kinari.dc.html).

a game not supported (Randomize… visible but off; "Meadow Courier can't be randomized.", also the tooltip) · b revision not supported ("Only revisions 1.0 and 1.1 of Coral Saga: Tide can be randomized. Your file is revision 1.2.") · c plugin off (no Randomize…; "Your 2 variants and their saves stay. They play again when Randomizer is on." Open plugin settings) · d no variants yet (`StateBlock` with Randomize…) · e rename ("The slot is renamed with it. Saves stay.") · f spoiler log confirm ("It lists everything the randomizer changed in Randomizer #1. Uzume never shows it in game or to others." Show spoiler log / Cancel focused).

## 10b Randomize dialog

### 10b-1 Randomize dialog

Boards: [`S10b1`](../source/uzume/S10b1.dc.html) · [`S10b1Kinari`](../source/uzume/S10b1Kinari.dc.html). Handoff [`S10bH`](../source/uzume/S10bH.dc.html).

Dialog lg "Randomize Coral Saga: Ember" (label "RANDOMIZER · 3DS"): Name (default "Randomizer #2"), Preset `Select` (Nuzlocke friendly "Fair start, game stays finishable", Classic "Creatures and items shuffled", Chaos "Everything that can change", Custom "Paste a settings string"), `Checkbox` Use a seed ("Off: Uzume picks a new random seed."), SAVE SLOT line "Plays in its own slot: Randomizer #2. Main is not touched.", 3DS update line (`StatusIcon` "Built for update 1.4, matches this PC."), Mono "FVX 2.3.1 · GPL-3.0 · SOURCE". Actions **Build variant**, Cancel; hints A Choose, Start Build variant, B Cancel.

### 10b-2 Custom settings

Boards: [`S10b2`](../source/uzume/S10b2.dc.html) · [`S10b2Kinari`](../source/uzume/S10b2Kinari.dc.html).

Preset Custom shows Settings string with Paste and validation ("Valid · 38 settings", Mono "MADE WITH FVX 2.3"), Use a seed on with Seed field; "The same settings string and seed build the same world on every PC."

### 10b-3 Randomize dialog · states

Boards: [`S10b3`](../source/uzume/S10b3.dc.html) · [`S10b3Kinari`](../source/uzume/S10b3Kinari.dc.html).

a preset list open · b string from another engine version ("STRING FVX 3.0 · HOME SERVER FVX 2.3.1"; Build variant stays off until the string is valid) · c name taken · d 3DS update mismatch ("Built for update 1.4. This PC has update 1.3." "Add update 1.4 to Coral Saga: Ember on Home server, or build for 1.3." Build for 1.3) · e share the same world (Copy seed and settings; "Someone who pastes both builds the same world.") · f spoiler log `Checkbox` Keep a spoiler log ("Off by default. Open it later from the variant's menu; never shown in game.").

## 10c Building a variant

### 10c-1 Building

Boards: [`S10c1`](../source/uzume/S10c1.dc.html) · [`S10c1Kinari`](../source/uzume/S10c1Kinari.dc.html). Handoff [`S10cH`](../source/uzume/S10cH.dc.html).

Dialog "Building Randomizer #2": "Coral Saga: Ember is a large game, so this takes a few minutes. You can keep using Uzume meanwhile." Step list (StatusIcon, Enso, Progress; no component): Reading game file "1.8 GB · checked" · Randomizing "46 % · ABOUT 3 MIN LEFT" · Writing Randomizer #2; SAVE SLOT line. Actions **Run in background** (primary, focused), Cancel.

### 10c-2 Ready

Boards: [`S10c2`](../source/uzume/S10c2.dc.html) · [`S10c2Kinari`](../source/uzume/S10c2Kinari.dc.html).

Game page with the new row "Randomizer #2 · Created just now · Randomizer #2 · new · Ready" and the toast "Randomizer #2 is ready" with Play.

### 10c-3 Building · states

Boards: [`S10c3`](../source/uzume/S10c3.dc.html) · [`S10c3Kinari`](../source/uzume/S10c3Kinari.dc.html).

a small game (GB, about 4 s: no dialog, the button shows Building…, then the toast) · b cancel ("Stop building Randomizer #2?" "Nothing is kept. Main is not touched." Stop building / Keep building) · c in background, Library (row status "BUILDING 46 %"; Play still starts Main) · d in background, game page ("Building · about 3 min left"; Play turns on when done) · e ready on another page (toast also in the bell; Y plays it from a gamepad) · f quit while building ("Quit and stop building Randomizer #2?" "The build starts over next time." Quit / Keep Uzume open).

## 10d Variant states

Handoff [`S10dH`](../source/uzume/S10dH.dc.html).

### 10d-1 Before and while building

Boards: [`S10d1`](../source/uzume/S10d1.dc.html) · [`S10d1Kinari`](../source/uzume/S10d1Kinari.dc.html).

a engine not on this PC ("Home server offers it · 38 MB. It downloads once, then builds Randomizer #2." Download and build) · b server offers no engine ("Ask an admin to turn on Offer to apps in Server › Tools."; admins see Open Tools) · c game file missing ("Uzume downloads it from Home server first · 1.8 GB." Download and build) · d different dump (Coral Saga: Tide, NDS: "Your file of Coral Saga: Tide is a different dump.", Mono "NDS · RUN EXPECTS 9F3A…C21E · YOURS 41B7…0D9A", **Download the run's file · 1.8 GB**; when the run's file is not on the server the button is replaced by "Ask Max to upload the run's file.") · e not enough memory ("needs about 3 GB free; 1.2 GB is free now") · f build failed (Mono exit line, Copy details, Try again, "Nothing was kept. Main is not touched.").

### 10d-2 Get the engine

Boards: [`S10d2`](../source/uzume/S10d2.dc.html) · [`S10d2Kinari`](../source/uzume/S10d2Kinari.dc.html).

Dialog "Get the randomizer engine" ("Building variants needs the randomizer engine. Home server offers it, like cores."): VERSION (the version Home server offers), SIZE (downloads once), FROM Home server › Tools, LICENSE GPL-3.0 · source; progress "DOWNLOADING · 24 OF 38 MB", "Then Randomizer #2 builds."

### 10d-3 Build failed · details

Boards: [`S10d3`](../source/uzume/S10d3.dc.html) · [`S10d3Kinari`](../source/uzume/S10d3Kinari.dc.html).

Dialog "Randomizer #2 couldn't be built": "The engine stopped while writing the game data. Nothing was kept; Main is not touched." Mono log with timestamps and the exit code; "Details hold no spoilers: no creature, item or trainer names." Try again, Copy details, Close.

### 10d-4 Offline and upkeep

Boards: [`S10d4`](../source/uzume/S10d4.dc.html) · [`S10d4Kinari`](../source/uzume/S10d4Kinari.dc.html). Seven panels in a 4 x 2 grid.

a server offline, engine on this PC ("You can still build", `Tag` Not synced, "The recipe goes to Home server when it's back.") · b server offline, no engine (Try again) · c needs rebuild ("Its files were removed to free space. Rebuilding uses the same recipe, so the world is the same." The recipe pins an older engine version; Uzume gets it from Home server. Rebuild · about 3 min) · d unknown dump ("It may still work. Main is not touched either way." Build anyway / Cancel) · e disk full (Open Storage) · f game updated, own variant ("Randomizer #1 was built for 1.4 and still plays. Rebuild for 1.5 to get the update." "The world may change; your save stays in its slot." Rebuild for 1.5) · g game updated, run variant ("Ember Shuffle pins update 1.4. Your variant stays on 1.4 until the run ends."; no button).

## 10e New run · Randomizer

Handoff [`S10eH`](../source/uzume/S10eH.dc.html).

### 10e-1 New run with Randomizer

Boards: [`S10e1`](../source/uzume/S10e1.dc.html) · [`S10e1Kinari`](../source/uzume/S10e1Kinari.dc.html). Revises 4j-app-3.

Dialog "Start a run" as 4j-app-3 (Name, Game, Rules, Mode, People) plus section **Randomizer**: `Segmented` Off / **Each person gets their own** / Same world for everyone, Preset `Select`, `Checkbox` Allow own variants ("Only with the run's engine version."), Mono "PINS FVX 2.3.1 FOR THIS RUN", "Each person's app builds their copy before the first start. Spoilers stay hidden." SAVE SLOT: "Each person gets Run: Ember Shuffle. Main and their own variants are not touched." Hints UpDown Move, LeftRight Choose, Start Start run.

### 10e-2 Randomizer in a run · states

Boards: [`S10e2`](../source/uzume/S10e2.dc.html) · [`S10e2Kinari`](../source/uzume/S10e2Kinari.dc.html).

a Off (preset and Allow own variants hidden) · b same world ("One seed for everyone. Uzume picks it at the start; nobody sees it, the run creator neither.") · c engine pinned ("Updating the engine on Home server never changes a running run.") · d no engine on the server (row off: "Home server offers no randomizer. Admins: Server › Tools.") · e game not supported · f after the start (read-only summary, "Can't change after the start.").

## 10f Join and play

Handoff [`S10fH`](../source/uzume/S10fH.dc.html).

### 10f-1 Join a randomized run

Boards: [`S10f1`](../source/uzume/S10f1.dc.html) · [`S10f1Kinari`](../source/uzume/S10f1Kinari.dc.html).

Dialog "Join Ember Shuffle" ("Max's Nuzlocke run · Coral Saga: Ember · 2 of 8 people"): YOUR GAME "Coral Saga: Ember · on this PC · update 1.4", "Same dump as the run."; RANDOMIZER "Your variant is built on this PC.", "Each person gets their own · Nuzlocke friendly. Spoilers stay hidden.", Mono "FVX 2.3.1 · PINNED BY THE RUN"; SAVE SLOT "You get Run: Ember Shuffle. Main and your variants are not touched." Join run / Cancel. Runs list shows the run with a "Randomized" tag.

### 10f-2 Bring my own variant

Boards: [`S10f2`](../source/uzume/S10f2.dc.html) · [`S10f2Kinari`](../source/uzume/S10f2Kinari.dc.html).

`Checkbox` Bring my own variant, `Select` Your variant (Build the run's variant / Randomizer #1 "same engine" / Chaos night "Older engine · doesn't match", disabled), shown closed and focused; "Max's run allows your own variant if it uses the run's engine version."

### 10f-3 Building your variant

Boards: [`S10f3`](../source/uzume/S10f3.dc.html) · [`S10f3Kinari`](../source/uzume/S10f3Kinari.dc.html).

Dialog "Building your variant" (label "RUN · EMBER SHUFFLE"): "Before your first start, this PC builds your copy of Coral Saga: Ember. Spoilers stay hidden." Steps Reading your game file (same dump as the run) · Randomizing your copy · Writing your variant; `Checkbox` "Start Ember Shuffle when it's ready"; Run in background, Cancel.

### 10f-4 Run board · Game column

Boards: [`S10f4`](../source/uzume/S10f4.dc.html) · [`S10f4Kinari`](../source/uzume/S10f4Kinari.dc.html). Revises 4jh-app-1.

Board table RUNNER · GAME (only "Run variant" or "Own variant") · PROGRESS · STATUS (Playing, Paused, Not started, Building) · LAST. Side: Save slot line and the Randomizer block (WORLD, PRESET, ENGINE "2.3.1 · pinned", OWN "Allowed, same engine"): "Nobody sees another person's world, seed or settings, admins neither."

### 10f-5 Join and play · states

Boards: [`S10f5`](../source/uzume/S10f5.dc.html) · [`S10f5Kinari`](../source/uzume/S10f5Kinari.dc.html).

a different dump (as 10d-1d, with Cancel) · b no game file ("Uzume downloads it from Home server · 1.8 GB, then builds your variant." Join and download) · c own variants off · d own variant doesn't match ("Chaos night uses engine 2.2.0; this run pins 2.3.1.") · e waiting for others (`RunnerRow` "Building variant · 2 min"; the ready check of 0.23 waits until every variant is built; before 0.23 the board shows Building) · f spoiler log after the run ("Each person can open the spoiler log of their own variant from Saves." "Never during the run, never someone else's.").

## 10g Library and slots

Handoff [`S10gH`](../source/uzume/S10gH.dc.html).

### 10g-1 Library

Boards: [`S10g1`](../source/uzume/S10g1.dc.html) · [`S10g1Kinari`](../source/uzume/S10g1Kinari.dc.html). Revises 3c-0.

One row per game; the status shows the variant count ("3 VARIANTS"). Continue resumes the last slot, variants included ("Coral Saga: Ember · Randomizer #1", "3DS · variant slot Randomizer #1 v6 · Desktop-LivingRoom"). Note: "One row per game. The status shows how many variants it has; pick one on the game page or from Continue."

### 10g-2 Saves and states with variant slots

Boards: [`S10g2`](../source/uzume/S10g2.dc.html) · [`S10g2Kinari`](../source/uzume/S10g2Kinari.dc.html). Revises 3c-2.

Slot `Select` (Main v31, Randomizer #1 "Variant · v6", Randomizer #2 "Variant · new", Chaos night, Run: Ember Route "Run · v44", Run: Ember Shuffle "Run · v3"), open. "Pick a slot. Variants and runs each have their own; restoring in one never changes another." States: "States made in a variant load only in that variant." (Save versions as table rows; there is no SaveRow component.)

### 10g-3 Settings › This PC › Storage

Boards: [`S10g3`](../source/uzume/S10g3.dc.html) · [`S10g3Kinari`](../source/uzume/S10g3Kinari.dc.html). Revises 6l-1.

New bar "Variants 2.4 GB"; tools count under "Cores, firmware, tools". Section Variants ("4 VARIANTS · 2.4 GB IN CACHE"): "Free space: variants are rebuilt when needed. Their recipes and saves stay on Home server." Free 2.4 GB…; table VARIANT · LAST PLAYED · SIZE (a removed one: "Files removed · rebuilds when played"); `Select` "Remove variants not played for" Never / 30 / 60 / 90 days ("This only removes copies on this PC. Saves and recipes are always on Home server.").

## 10h Server › Tools

Handoff [`S10hH`](../source/uzume/S10hH.dc.html). The Server screen gains a **Tools** tab after Systems & cores (passed as AppNav groups on this sheet; earlier sheets keep the default).

### 10h-1 Tools

Boards: [`S10h1`](../source/uzume/S10h1.dc.html) · [`S10h1Kinari`](../source/uzume/S10h1Kinari.dc.html).

"Programs Home server offers to apps, like cores. Apps run them on their own PC; game files never come here." Randomizer engine (USED BY THE RANDOMIZER PLUGIN): ENGINE, VERSION ("2.3.1 · checked today 09:00"), SIZE ("38 MB per PC · downloaded once"), PLATFORMS (Windows · Linux), LICENSE (GPL-3.0 · source and license), `Toggle` **Offer to apps**, update line "Version 2.4.0 is available · 18 Oct" with Update to 2.4.0. Versions kept (RUNS AND RECIPES PIN A VERSION): VERSION · USED BY · SIZE, Remove…; "Updating never changes a running run or an existing recipe. Home server keeps every version still in use."

### 10h-2 Tools · states

Boards: [`S10h2`](../source/uzume/S10h2.dc.html) · [`S10h2Kinari`](../source/uzume/S10h2Kinari.dc.html).

a no internet on the server (Import from file…) · b wrong file ("This file isn't a randomizer engine release.") · c Offer to apps off ("Apps can't build new variants. Existing variants keep playing.") · d updating ("2.3.1 stays: Ember Shuffle pins it.") · e no tools yet (Get the engine) · f version in use ("Remove version 2.2.0?" "2 variant recipes use it. Their owners can still play; rebuilding them needs 2.2.0 back.").

### 10h-3 Plugins with Randomizer

Boards: [`S10h3`](../source/uzume/S10h3.dc.html) · [`S10h3Kinari`](../source/uzume/S10h3Kinari.dc.html). Revises 4i-app-1.

Randomizer 1.0.0 Official, permissions "Variants, run settings, engine"; detail: "Adds Randomize… to game pages and the Randomizer section to New run. It needs the engine from Server › Tools." "Engine 2.3.1 is offered to apps." Open Tools; "Turning it off hides Randomize… and keeps every variant, recipe and save."
