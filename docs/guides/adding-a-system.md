# Adding a system

At a glance: How to add a game system (for example `snes`) to the server and the app. A system is mostly data: one JSON on the server, one manifest on the app, optionally a core profile. Start at step 1, verify with step 8. Why: [ADR 0024](../adr/0024-system-template-and-game-boy.md).

Contents: [1. Server definition](#1-server-definition), [2. App system manifest](#2-app-system-manifest), [3. Core profile (optional)](#3-core-profile-optional), [4. Firmware](#4-firmware), [5. Library and saves](#5-library-and-saves), [6. Game identity (plugins)](#6-game-identity-plugins), [7. Tests that cover it automatically](#7-tests-that-cover-it-automatically), [8. Verify locally](#8-verify-locally), [9. Docs](#9-docs).

Since 0.13 ([ADR 0024](../adr/0024-system-template-and-game-boy.md)) a system is mostly data: one JSON file on the server, one on the app, optionally a core profile. The only code step is a new game-header reader (step 6) when no existing one fits. No SQL migration, no CMake edit. Worked example: `gba`.

Pick an id (lowercase, for example `snes`). Use it everywhere below. The server and app files must agree; a test checks it.

## 1. Server definition

`server/internal/hub/systems/<id>.json`, embedded in the server and validated at start (an invalid file makes the server fail to start; the Go tests catch it first):

```json
{
  "id": "gba",
  "display_name": "Game Boy Advance",
  "extensions": [".gba"],
  "libretro_ids": ["game_boy_advance"],
  "platforms": ["windows-x86_64", "linux-x86_64"],
  "provisioning": "Installed by the admin",
  "input_profile": "gba",
  "display_profile": "single_screen",
  "firmware": [
    {"id": "gba_bios", "display_name": "Game Boy Advance BIOS", "file": "gba_bios.bin", "sizes": [16384]}
  ],
  "profiled_cores": ["mgba"]
}
```

- `extensions`: lowercase with the dot. The server maps a library file to a system by extension.
- `libretro_ids`: the `systemid` values of the libretro core info files (`.info`, field `systemid`) that may serve this system. A buildbot core is offered when its `systemid` is in this list and its `supported_extensions` are empty or share an extension with `extensions`. A core can serve several systems (mGBA: `gb`, `gbc`, `gba`); it is then one installed build.
- `firmware`: files the system can use. `sizes` lists accepted byte sizes. Whether they are required is the admin's firmware mode, not this file. Leave `[]` if the system needs none. Never add files or hashes of real firmware.
- `profiled_cores`: core ids that have an app profile (step 3). Other cores run as experimental.
- Existing systems are updated from the file at the next server start; admin state (default core, firmware mode, installed cores, uploaded firmware, pins) is never touched. The server never preselects a core.

## 2. App system manifest

`client/emulation/manifests/systems/<id>.json`, globbed by CMake:

```json
{
  "system_id": "gba",
  "display_name": "Game Boy Advance",
  "order": 320,
  "extensions": [".gba"],
  "firmware": {"required": false, "files": [{"id": "gba_bios", "name": "gba_bios.bin"}]},
  "input_profile": "gba",
  "input": {
    "map": {"a": "a", "b": "b", "start": "start", "select": "select",
            "up": "up", "down": "down", "left": "left", "right": "right",
            "l": "l", "r": "r", "zl": "l", "zr": "r"},
    "labels": {"zl": "L (alt)"}
  },
  "display_profile": "single_screen",
  "identity": "gba_header",
  "labels": {"input": "GBA"},
  "display": {"layout": "single", "gap": 0, "screens": [{"id": "main", "width": 240, "height": 160}]}
}
```

- `order` sorts systems in lists (lower first).
- `firmware.files[].id` and `name` must equal the server's `id` and `file`.
- `input_profile` and `display_profile` equal the server values. Systems that share an `input_profile` must have identical `input.map` (checked at load).
- `display.screens`: native size of each screen. Two screens or unequal widths: see `3ds.json` (`align`, composed frame). Use `"layout": "single"` for one screen.

### Input map

`input.map` maps Uzume input ids (keys) to libretro targets (values). The Uzume input ids (`uzumeInputs()` in `client/core/controllerprofiles.cpp`) are: `a b x y l r start select up down left right zl zr cup cdown cleft cright lup ldown lleft lright`. Targets:

| Targets | Meaning |
|---|---|
| `b y select start up down left right a x l r` | RetroPad face, shoulder and D-pad buttons |
| `l2 r2` | triggers (3DS ZL/ZR) |
| `l3 r3` | stick clicks |
| `circle_pad_*`, `c_stick_*` | analog sticks (3DS) |

Only mapped inputs appear on the Controllers page, so map exactly what the system has. Several Uzume inputs may point at one target (GBA maps `zl` to `l` and `zr` to `r` so a gamepad with triggers still works). Optional `labels` rename an input on the page.

## 3. Core profile (optional)

`client/emulation/manifests/cores/<core_id>.json`. Needed only to set option defaults, lock options, or wire the BIOS switch; without it the core runs as experimental. `system_ids` lists every system the profile serves (one file for a multi-system core).

```json
{
  "_source": "Option keys from libretro/mgba src/platform/libretro/libretro_core_options.h",
  "core_id": "mgba",
  "display_name": "mGBA",
  "library_basename": "mgba_libretro",
  "system_ids": ["gb", "gbc", "gba"],
  "order": 0,
  "save": {"source": "save_ram", "extension": ".sav"},
  "firmware": {"sysfile_option": "mgba_use_bios", "sysfile_native": "ON", "sysfile_builtin": "OFF"},
  "core_options": {"mgba_sgb_borders": "OFF"},
  "locked_core_options": ["mgba_sgb_borders"],
  "always_shown_core_options": ["mgba_color_correction"]
}
```

How to find option keys: open the core's `libretro_core_options.h` in its public repository (or read the options the Emulation page shows after installing the core). Record the repository file in `_source`. Never guess a key or value; values must match the core's option values exactly.

What to lock (hidden on the Emulation page, user overrides never reach the core):

- Options that change the frame size or composition (mGBA `mgba_sgb_borders` ON gives a 256x224 frame; the manifest `display` size would be wrong).
- Options Uzume controls: the firmware switch (`firmware.sysfile_*`: native when the admin provides the file, builtin otherwise).
- Options that need what cores do not get: network access (Gambatte's link cable network options, `gambatte_gb_link_*`, are locked with link mode "Not Connected").

Leave everything else unlocked. `always_shown_core_options` pins useful options to the top. `save.source` is `save_ram` for a cartridge save (`.sav`); other sources: see `3ds.json`/`azahar.json` and the DS profiles. If a core serves a system but the profile does not list it, the core is experimental there.

List the core id in `profiled_cores` of each server system file it should be profiled for.

## 4. Firmware

Boot ROMs and BIOS files belong to the admin: uploaded on Systems & Cores, validated by size and an optional admin-pinned SHA-256, delivered to the app only in firmware mode `native`. The repository contains none. Document the file name only (in the manifest, the guide and the ADR), never contents or hashes of real files. Tests use dummy files.

## 5. Library and saves

Nothing to add: the server imports files by extension and keeps the save as an opaque blob; the app stores the core's save (`save_ram` plus `.sav`) in the slot like other systems. A system whose save is not one file needs a new save source, which is code (see [Saves](saves.md), [ADR 0022](../adr/0022-nintendo-3ds-with-azahar.md) D3).

## 6. Game identity (plugins)

`"identity"` selects a reader in `client/emulation/game_identity.cpp`: `nds_header`, `3ds_ncsd`, `gb_header`, `gba_header`. The result (`code`, `kind`, optional title, maker, version) goes to plugins. For a new header format add a reader there and a constant in `game_identity.h` (the only code step) and extend `client/emulation/tests/game_identity_test.cpp` with a synthetic header. Cite the public format source for every offset in the code comment and the ADR. Without a reader the identity is empty, which plugins must handle.

## 7. Tests that cover it automatically

- Server: `TestSystemDefsLoad` and `TestValidateSystemDefs` (the file is valid), reconcile tests (insert and update, admin state kept), `TestHubSystemsMatchPlayerManifests` (server definitions against app manifests: names, extensions, firmware ids and files, profiled cores). Run `make check-hub`.
- App: the manifest tests load every manifest (including identical maps per `input_profile`) and the input and Controllers tests use the registered tables. Run `make check-client`.
- If you add a reader or a profile rule, add a synthetic-data test; no ROMs, no dumps.

## 8. Verify locally

CI has no real core. Before calling a system done, on a real machine with the server and the app:

1. Start the server, open Systems & Cores: the new system is listed, the buildbot offers the expected cores (and not others).
2. Install a core; check that Update, Remove and a second system sharing the core behave.
3. Add a homebrew or dummy ROM with the extension; start it in the app (core choice, frame size equals the manifest, input from keyboard and gamepad).
4. Save in the game, quit, start again: the save is there; check the slot on the server (Saves tab › All users in the app); restore a history version.
5. With firmware mode `native` and your own firmware file: the app reports "Firmware required/missing" correctly and the core uses it.
6. Plugins: with the plugin runtime, check the game identity.

Record in the roadmap what was verified only locally.

## 9. Docs

Add the system to the server and app bullets in [features.md](../features.md), the roadmap entry, and an ADR only if a decision deviates from this template.
