# Plugin API reference (API 1)

At a glance: Reference for authors of Uzume plugins (API 1): manifest, permissions, sandbox, the `fb` API, UI blocks, runs, installing and developer mode. A minimal example is at the end. Publishing an official plugin: [plugin-publishing.md](plugin-publishing.md). Why: [ADR 0023](../adr/0023-plugins.md).

Contents: [Manifest](#manifest), [Permissions](#permissions), [Sandbox and limits](#sandbox-and-limits), [API](#api), [Installing plugins and origins](#installing-plugins-and-origins), [Developer mode](#developer-mode), [Minimal example](#minimal-example).

For authors of Uzume plugins. Design background: `docs/architecture/12-plugins.md`, `docs/adr/0023-plugins.md`.
This page describes what the app runtime implements today. Parts marked as coming later are not available yet.

A plugin is a folder with a `manifest.json`, a Lua entry file and optional `i18n/` and `assets/`. It runs in a sandboxed Lua 5.4 state on the plugin thread of the app, never on the emulation or UI thread. A plugin can read the game's memory, but it can never slow the game down.

## Manifest

```json
{
  "id": "net.example.tracker",
  "name": "Tracker",
  "version": "1.0.0",
  "api": 1,
  "type": "script",
  "requires": { "net.example.core": "^1.2" },
  "provides": ["tracker.team"],
  "games": [{ "system": "nds", "codes": ["IPK*", "IPG?"] }],
  "permissions": ["memory.read", "rom.read"],
  "entry": "main.lua",
  "assets": ["icons/"],
  "run_settings": [
    { "key": "mode", "type": "enum", "label": "rs.mode", "default": "soullink",
      "options": [ { "value": "nuzlocke", "label": "rs.mode.nuzlocke" }, { "value": "soullink", "label": "rs.mode.soullink" } ] },
    { "key": "rules.dupes_clause", "type": "boolean", "label": "rs.dupes", "hint": "rs.dupes.hint", "default": true }
  ],
  "settings": {}
}
```

| Field | Rules |
|---|---|
| `id` | Reverse-DNS, lowercase letters, digits, `-` and `.`, at least two parts (`net.example.tracker`). |
| `name`, `version` | Display name; SemVer (`1.0.0`). |
| `api` | Must be `1`. Otherwise the plugin is rejected: "`<name>` needs plugin API `<n>`; this app supports API 1". |
| `type` | `"script"` (default). `"tool"` is rejected: "tool plugins are not supported yet". |
| `requires` | Map of plugin id to version range: `^1.2` (compatible, below the next major; `^0.2` stays below `0.3`), `~1.2` (below the next minor), `>=1.0`, `1.2.3` (exact), `*`. Required plugins start first. A required plugin must be installed and match the range. |
| `min_player` | Optional SemVer (`"0.16.4"`): the oldest Uzume that runs the plugin. A plugin that needs a newer app has the problem "Needs Player X", is not started and cannot be turned on; an enabled one stays enabled and runs again after the app is updated (also after "Go back" to an older app, see [updates](updates.md#rollback)). The server does not check it. |
| `provides` | Interface names this plugin offers to others with `fb.provide`. |
| `games` | List of `{ "system", "codes" }`. `codes` are globs (`*` any run, `?` one character, case-insensitive); no `codes` = every game of that system. An empty or missing list = all games. |
| `permissions` | `memory.read`, `rom.read`, `ui`, `run.state`, `video.capture`. `memory.write` and `tool.exec` are rejected ("permission `<p>` is not available yet"), unknown names are errors. |
| `entry` | Relative path of the `.lua` entry file inside the plugin folder (no `..`, must exist). |
| `assets` | Relative paths of bundled files or folders. |
| `run_settings` | Optional run rules form, see [Run settings](#run-settings). |
| `settings` | Kept as raw JSON; the schema is not defined yet. |

## Permissions

| Permission | Unlocks | Status |
|---|---|---|
| `memory.read` | `fb.memory.*`, `fb.dev.watch` | available |
| `rom.read` | `fb.rom.*` | available |
| `ui` | `fb.ui.*` and the manifest field `ui` (declarative UI blocks, toasts) | available |
| `run.state` | `fb.run.*` (per-player run state, run events, run board) | available |
| `video.capture` | `fb.video.*` (screen layout and PNG captures of the game picture) | available |

Calling a function without its permission raises the Lua error `permission <p> not granted`. Everything else (`fb.log`, `fb.events`, `fb.provide`/`fb.call`, `fb.cache`, `fb.i18n`, `fb.clock`, `fb.on_tick`) needs no permission.

## Sandbox and limits

- Libraries: base, `string`, `table`, `math`, `utf8`, `coroutine`. There is no `io`, `os`, `package` or `debug`. `load`, `loadstring`, `dofile`, `loadfile`, `require` and `string.dump` do not exist. Only text source is accepted, never precompiled bytecode.
- `collectgarbage("count")` returns the memory in use in KiB; every other option does nothing.
- `print(...)` writes an info line to the plugin log (same as `fb.log.info`).
- Each plugin has its own Lua state, so plugins cannot see each other's globals.

| Limit | Value |
|---|---|
| Memory per plugin | 64 MiB |
| Instructions per entry call | 20,000,000 |
| Wall-clock time per entry call | 500 ms |
| One watched memory range | 4 MiB |
| All watched ranges of one plugin | 16 MiB (at most 256 ranges) |
| `fb.rom.read` length | 16 MiB per call |
| `fb.video.capture` | one pending capture per plugin, at most 1 request per second, PNG up to 2 MiB, rectangle up to 4096x4096 |
| Plugin cache | 256 MiB per plugin; `fb.cache.read` returns files up to 32 MiB |
| Values passed between plugins | depth 32, 1 MiB |
| Pattern matching | `string.find`, `match`, `gmatch` and `gsub` share a matcher budget of 20 million steps (about 0.2 s) per entry call |
| `string.rep` | result up to 16 MiB |
| Event queue | 10000 events, 16 MiB of host memory in total and 4 MiB per emitting plugin (measured by the real size of the copied values, not the 1 MiB reader limit); `fb.events.emit` raises "event queue is full" when a plugin's share is used up |
| Time per host iteration | event handlers of all plugins together get 50 ms per iteration; the remaining events stay queued and are delivered by the next iterations. A plugin whose callbacks use more than half of the 500 ms budget five times in a row is paused |

An *entry call* is anything the host starts: loading the entry file, a tick, an event handler, or a call made with `fb.call`. When a plugin exceeds its budget or memory limit, or raises an error that it does not catch, **only that plugin is paused**: the error and its traceback appear in the developer console, its watched ranges are released, and the game and all other plugins keep running. Fix the file and reload the plugin to start it again. A plugin cannot hide a budget overrun with `pcall`.

Native string functions cannot be interrupted by the instruction hook, so the pattern matcher of the vendored Lua counts its own steps: a pathological pattern such as `("a"):rep(40000):find(".-b")` (about 7 s in stock Lua) ends with the error `time budget exceeded (pattern matching budget used up)` after at most about 0.2 s, and like any budget overrun it pauses only that plugin and cannot be swallowed with `pcall`. Plain `find(s, x, 1, true)`, `rep` and `format` are linear and bounded by the memory limit (and the 16 MiB `rep` cap).

Hardening of the sandbox that plugins can observe:

- `setmetatable` refuses a metatable with `__gc` (a finalizer would run when the state is closed, outside any budget).
- `tostring`, `print` and `string.format("%s")` show `table: #1`, `function: #2` and so on for tables, functions and userdata without `__tostring`, never memory addresses; `string.format("%p")` is not available.
- Text from plugins and from other participants (manifest names, block text, event and state fields, run events) is always shown as plain text in the app: markup such as `<b>` or `<img>` appears literally.
- Plugin images (`cache:` and `asset:`) are PNGs up to 4096x4096 and 8 MiB. The app decodes them scaled to the size they are shown at (never above 2048x2048), and one plugin may have at most 128 megapixels decoded per 10 seconds.
- `fb.rom.romfs` and `fb.rom.romfs_list` keep the parsed RomFS index of the running game between calls.

## API

Everything lives in the global table `fb`.

### Information

- `fb.api` is `1`.
- `fb.plugin` is `{ id, version }`.
- `fb.game` is `{ system, code, sha256, title, language }`. Any field may be `nil`. `language` is the plugin UI language as a tag (`en`, `de`, `de-DE`, ...): the app setting Settings > Plugins > Plugin language, or with Auto the Azahar "3DS System Language" of a 3DS game, else the language of the computer. It is fixed when the game starts.
- `fb.clock()` returns monotonic milliseconds (integer).

### Logging

`fb.log.debug(...)`, `fb.log.info(...)`, `fb.log.warn(...)`, `fb.log.error(...)` join their arguments with tabs (like `print`) and write to the plugin log shown in the developer console.

```lua
fb.log.info("loaded", fb.plugin.version)
```

### Memory (`memory.read`)

Plugins read a **snapshot** that the emulation copies after frames. They register the ranges they need; the data appears from the next snapshot on.

- `fb.memory.watch(key, addr, len)` registers a range under a name of your choice (the same key replaces the range). `fb.memory.unwatch(key)` removes it.
- `fb.memory.u8/u16/u32/u64/s8/s16/s32(addr)` read little-endian integers. `fb.memory.bytes(addr, len)` returns a string (`len` up to 4 MiB). `u64` wraps into the signed Lua integer range.
- All reads return `nil` when the address is not in the latest snapshot. `fb.memory.frame()` returns the frame number of the latest snapshot or `nil` if there is none yet.
- **Late memory maps:** a core may re-send its memory map while the game runs (for example when the game maps its heap later). The app picks up the new regions at once: the console's MEMORY MAP section updates, and addresses that were unreadable become readable from the next snapshot on (ranges you already watch need no re-registration).
- **Pointer following:** read a pointer, then watch the target. The target is readable from a later snapshot.

```lua
fb.memory.watch("party", 0x02234000, 0x1D8)
local followed = false
fb.on_tick(function()
  local ptr = fb.memory.u32(0x02234000)
  if not ptr then return end
  if not followed then fb.memory.watch("target", ptr, 64) followed = true end
  local value = fb.memory.u32(ptr)        -- nil in the first tick, set afterwards
end)
```

### Ticks

`fb.on_tick(fn)` sets the function that is called on the plugin thread whenever a new snapshot exists, at most `tickHz` times per second (host setting, default 30). `fn` receives the snapshot frame number. `fb.on_tick(nil)` removes it.

### Events

- `fb.events.emit(name, value)` publishes an event.
- `fb.events.on(pluginId, name, fn)` subscribes; `pluginId` is a plugin id or `"*"` for any. `fn(value, sourcePluginId, name)` runs on a later host iteration, never inside `emit`.

Values are deep-copied between states: `nil`, booleans, numbers, strings and tables of these (depth up to 32, up to 1 MiB). Functions, userdata and tables used as keys are rejected with a Lua error. Emitted events also show in the developer console.

```lua
fb.events.emit("team.changed", { slot = 1, species = 25 })
fb.events.on("net.example.tracker", "team.changed", function(v, from) fb.log.info(from, v.slot) end)
```

### Interfaces between plugins

- `fb.provide(name, tbl)` publishes a table of functions under `name`, which must be listed in the manifest's `provides`.
- `fb.call(name, fn, ...)` calls `tbl[fn](...)` of the plugin that provides `name`. That plugin must be listed directly in this plugin's `requires`. The call is synchronous on the plugin thread; arguments and results are deep-copied (same rules as events, no shared Lua memory). The callee's budget applies. An error in the callee becomes a Lua error in the caller (catch it with `pcall`). A callee that exceeds its budget or memory is paused.

```lua
-- provider (provides = ["tracker.team"])
fb.provide("tracker.team", { size = function() return 6 end })
-- consumer (requires the provider)
local n = fb.call("tracker.team", "size")
```

### Developer watches (`memory.read`)

`fb.dev.watch(label, addr, kind)` registers the range and shows label, address and live value in the WATCHED list of the developer console. `kind` is `u8`, `u16`, `u32`, `s32` or `hex:<len>` (1 to 64 bytes). `fb.dev.unwatch(label)` removes it.

```lua
fb.dev.watch("party count", 0x02234000, "u8")
fb.dev.watch("rng seed", 0x02100000, "hex:4")
```

### Game file (`rom.read`)

- `fb.rom.size()` returns the size of the game file (or `nil` if there is none).
- `fb.rom.read(offset, len)` returns up to `len` bytes (at most 16 MiB) as a string, shorter at the end of the file, `nil` at or after the end.

- `fb.rom.romfs(path)` reads one file from the RomFS of a decrypted 3DS game (`.cxi`, `.3ds`/`.cci`), e.g. `"/a/0/1"`; up to 256 MiB. Returns the data or `nil, message`. Encrypted games fail with a clear message.
- `fb.rom.romfs_list(dir)` returns an array of `{ name, dir, size }` (`dir` is a boolean) for a RomFS directory, or `nil, message`.

### Format helpers (`fb.codec`)

Pure functions on binary strings, no permission needed. Input is treated as untrusted. Failures never raise a Lua error: they return `nil, message`; only a wrong argument type raises. Size limits are the ones of the C++ helpers (decompressed data up to 64 MiB, images up to 4096 x 4096). All indexes are 1-based.

- `fb.codec.lz_decompress(s)` decompresses Nintendo LZ10/LZ11 data.
- `fb.codec.garc(s)` parses a GARC container (versions 0x0400 and 0x0600) and returns `{ count, data_offset, files }`; `files[i][sub]` is `{ offset, length }` relative to `data_offset`.
- `fb.codec.garc_file(s, i, sub)` returns the bytes of entry `i` (1-based), sub-file `sub` (1-based, default 1).
- `fb.codec.ctr_texture(s, width, height, format)` decodes a 3DS GPU texture (formats 0 RGBA8, 1 RGB8, 2 RGBA5551, 3 RGB565, 4 RGBA4, 5 LA8, 6 HILO8, 7 L8, 8 A8, 9 LA4, 10 L4, 11 A4, 12 ETC1, 13 ETC1A4).
- `fb.codec.bclim(s)` decodes a BCLIM image (footer at the end of the file); besides the normal formats it decodes the Gen 6 palette variant (RGB5A1 data starting with u16 2, an RGBA5551 palette and 4-bit or 8-bit indices).
- `fb.codec.etc1(s, width, height, alpha)` decodes plain ETC1 (`alpha` false) or ETC1A4 (`alpha` true) block data.
  The three image decoders return `{ width, height, rgba }` (RGBA8888, straight alpha, top-down) or `nil, message`.
- `fb.codec.png(img)` encodes such an image table as a PNG string.
- `fb.codec.pk6_decrypt(s)` / `fb.codec.pk6_encrypt(s)` decrypt or encrypt a Gen 6 Pokemon structure (232 or 260 bytes) and return `data, checksum_ok` (`checksum_ok` says whether the stored checksum matches the decrypted data; encrypting recomputes nothing), or `nil, message`.

```lua
local raw = fb.memory.bytes(slot_addr, 260)
local plain, ok = raw and fb.codec.pk6_decrypt(raw)
if plain and ok then species = string.unpack("<I2", plain, 9) end
```

### Game picture (`video.capture`)

Lets a plugin take a picture of what the core renders, for example the game's own map graphic from the 3DS bottom screen while the in-game map is open. The app never writes a capture to disk or to the log; keep what you need with `fb.cache`. The user sees the permission "Capture the game picture" when installing the plugin.

- `fb.video.screens()` returns a list of `{ name, x, y, w, h }`, one per emulated screen, in pixels of the core's output frame at native resolution (not the window): `"top"` and `"bottom"` for Nintendo DS and 3DS (taken from the screen arrangement of the system, scaled with a higher internal resolution), `"main"` for single-screen systems. `nil` when no game runs (shell context).
- `fb.video.capture(target)` asks for the latest frame at native core resolution, before scaling and shaders (for hardware-rendered cores such as Azahar: the existing readback of the frame; nothing is read back while nobody asks, so there is no cost per frame). `target` is a screen name (`"bottom"`) or a rectangle `{ x = 0, y = 0, w = 320, h = 240 }` of integers in the same coordinates. It does not block: it returns a request id (integer, counting from 1 per plugin state) and the result arrives later as the host event `video.captured`. A wrong argument type or a rectangle without integers raises a Lua error; everything else is reported in the event.

```lua
fb.events.on("fb", "video.captured", function(e)
  if e.ok then fb.cache.write("map/hoenn.png", e.png) else fb.log.warn("capture failed: " .. e.error) end
end)
local id = fb.video.capture("bottom")
```

Event `video.captured` (source `"fb"`, delivered on the plugin thread like the other [host events](#host-events-from-fb), only to the requesting plugin): `{ id, ok = true, png = <PNG bytes>, w, h }` or `{ id, ok = false, error }`. The PNG is RGBA and has exactly the size of the rectangle.

| `error` | Meaning |
|---|---|
| `busy` | A capture of this plugin is still pending. |
| `rate_limited` | The last accepted request was less than a second ago. |
| `no_frame` | No game runs, the game stopped, or no frame arrived within a few seconds. |
| `unknown_screen` | The screen name is not in `fb.video.screens()`. |
| `invalid_rect` | The rectangle is empty, negative, larger than 4096 in a side or not inside the frame. |
| `too_large` | The PNG is larger than 2 MiB. |
| `unsupported` | This context has no frame source. Captures work for the game running on this app; in a Session viewed from another app the decoded stream frame is not offered to plugins. |
| `denied` | The frame source refused the capture. |

A capture waits for a frame at native size, so with a hardware-rendered core the result arrives one to a few frames after the request. While the game is paused no new frame is produced and the capture fails with `no_frame` after about 2 seconds, unless the last frame already has native size.

### Cache

A per-plugin folder for data you derive from the game, such as extracted assets. Names use `A-Z a-z 0-9 . _ - /`, no `..`, no leading `/`; anything else raises an error.

- `fb.cache.write(name, data)` returns `true`; the 256 MiB quota per plugin is enforced (error `cache quota exceeded`).
- `fb.cache.read(name)` returns the data or `nil`.
- `fb.cache.exists(name)` returns a boolean.

### Translations

`fb.i18n.t(key, vars)` looks `key` up in `i18n/<lang>.json` of the plugin, where `<lang>` is `fb.game.language` (`de`, `de-DE`, ...); it falls back from the full tag to its base language (`de-DE` -> `de`), then to `i18n/en.json`, then to the key itself. `{name}` placeholders are replaced from `vars`. Files are flat JSON objects; nested objects are addressed as `a.b`. English is the source language, German the first translation.

```lua
-- i18n/en.json: { "team.title": "Team of {player}" }
fb.log.info(fb.i18n.t("team.title", { player = "Ash" }))
```

### UI

Needs the permission `ui` and the matching manifest field `ui`. A plugin ships no QML: it describes each surface as a tree of declarative blocks, and the app draws it with its own components and design tokens. Plugins choose tone names, size names and player numbers only, never colours or pixels. In-game surfaces are always dark.

#### Manifest field `ui`

```json
"ui": {
  "panel":  { "title": "Tracker" },
  "hud":    true,
  "tile":   true,
  "screen": { "title": "Runs" },
  "detail": true,
  "run":    { "title": "Rules" },
  "hotkeys": [ { "id": "toggle-hud", "label": "Show or hide HUD widget", "key": "Ctrl+H" } ]
}
```

| Key | Slot | Where |
|---|---|---|
| `panel` | `panel` | Own tab in the in-game side panel |
| `hud` | `hud` | HUD widget over the game picture (one per plugin) |
| `tile` | `tile:<tileId>` | Strip on a friend's Multiview tile |
| `screen` | `screen` | The plugin's tab in the run view (Runs screen), after Board and Log; content is the `screen` tree, or the `run` tree when the plugin declares no `screen`. Also opened as a sheet from Settings › Plugins and in a game |
| `detail` | `detail` | Section in the game detail (Library), below SAVE |
| `run` | `run` | Section in the run detail (Runs screen); used as the plugin's run tab when the plugin declares no `screen`. Runs in the run view, see below |
| `hotkeys` | - | Entries in Controllers → Hotkeys, see [Hotkeys](#hotkeys) |

- `title`: 1 to 40 characters. `hotkeys`: at most 16; `id` is `[a-z0-9-]{1,32}` and unique, `label` 1 to 60 characters, `key` is an optional portable key sequence (`QKeySequence` text, one combo). The key is a suggestion; the user can change it.
- Unknown keys are errors. Declaring `ui` without the permission `ui` is a manifest error: "field `ui` needs permission `ui`".
- Settings → Plugins shows what a plugin adds, for example "Adds: panel tab · HUD · tile strip · run tab · game detail · run view · 1 hotkey".

#### When plugins run

- In a game: as before, the plugins for the game start with it.
- **Shell context:** while no game runs, every enabled plugin that declares `ui.screen` or `ui.detail` runs with an empty game: `fb.game` has no `language` and its other fields are empty, memory reads return `nil`, `fb.rom.*` returns `nil`. Plugins it requires run too. Starting a game stops the shell set and starts the game set; ending the game restarts the shell set. Turning a plugin or Developer mode on or off re-evaluates the shell set.
- **Run view:** while a run is open in the detail of the Runs screen, the user is its creator or has joined, and no game runs, the run's plugins that declare `ui.run` or `ui.screen` (plus the plugins they require) run in the shell context as the exact server copies pinned by the run; missing copies are fetched like for "Play run". They get a read-only `fb.run` for that run (see [Run state](#run-state)). A plugin that is in both the shell set and the run view set runs once, as the run view instance. Opening or closing the run view restarts the shell plugins; while it is open, the other shell plugins with `run.state` also see that read-only `fb.run`. Starting a game stops the run view set; it comes back when the game ends and the detail is still open.
- A paused plugin keeps its last trees, shown dimmed.
- **Run tabs:** the run view has the tabs Board, Log and one tab per run plugin that declares `ui.run` or `ui.screen`, last. There are no plugin entries in the sidebar (since 0.18, [ADR 0030](../adr/0030-one-app-one-server-uzume.md) D7; the plugin API version stays 1). A run can have no plugin at all (feature `runs_v2`); then it has only Board and Log.
- **In-game screen:** the run tab does not exist while a game runs, so `fb.ui.open("screen")` shows the `screen` tree in a sheet over the play area and the side panel (always dark, content up to 1100 px wide). The game keeps running; Esc or "← Back to game" closes the sheet, and actions in it reach the plugin with slot `"screen"`.

#### `fb.ui`

All functions raise `permission ui not granted` without the permission. A slot must be declared in the manifest, otherwise `slot <s> not declared in manifest`.

- `fb.ui.set(slot, tree)` replaces the tree of a slot. `slot` is `"panel"`, `"hud"`, `"screen"`, `"detail"`, `"run"` or `"tile:<tileId>"` (`<tileId>` as `[A-Za-z0-9_.-]{1,64}`, needs `ui.tile`). `tree` is one block. An invalid tree raises a Lua error with a path, for example `ui tree: children[2].type: unknown block "foo"`.
- `fb.ui.patch(slot, id, props)` merges `props` into the block with that `id` in the current tree, so you send only changed values. Errors: no tree in the slot yet, unknown `id`, `type`/`id`/`children` in `props`, invalid props. A prop cannot be removed with patch (a `nil` value does not exist in a Lua table); set it to an empty or neutral value or use `set`.
- `fb.ui.clear(slot)` removes the tree.
- `fb.ui.title(slot, text)` overrides the declared title (slots `panel` and `screen` only; 1 to 40 characters; `nil` restores the declared title).
- `fb.ui.open(slot)` asks the app to show the plugin's own `"screen"` or `"panel"` (needs `ui.screen` / `ui.panel`; other slots raise `fb.ui.open only supports the slots screen and panel`). In a game, `"screen"` opens the in-game screen sheet and `"panel"` selects the plugin's tab in the side panel (shown if collapsed; ignored in fullscreen and in the Multiview). Outside a game, `"screen"` selects the plugin's run tab when the plugin runs in the open run view, otherwise it opens the plugin's screen as a sheet (the same sheet as "Open" in Settings › Plugins, where plugins without runs open their screen), and `"panel"` is ignored. Typical use: a button or hotkey action handled in `fb.ui.on_action`.
- `fb.ui.badge(n)` shows a count on the plugin's run tab. `0` or `nil` hides it; values are clamped to 0..999.
- `fb.ui.dot(kind)` sets the dot on the panel tab: `"news"` or `nil` to clear. The amber "paused" dot is set by the app.
- `fb.ui.notify{ tone, title, text, actions }` shows a toast and returns its id. `tone` is `"info"` (default), `"important"` or `"warning"`; `title` up to 80 characters; `text` 1 to 300 characters; `actions` up to 2 of `{ label (1 to 40), action, value? }`. Rate limit per plugin: a burst of 3, then 1 per second. A dropped toast writes a log warning and returns `nil`. The user can mute a plugin's toasts (all, or all but `important`). At most the newest 3 toasts are visible.
- `fb.ui.on_action(fn)` sets the handler `fn(action, value, slot)`, called on the plugin thread for:
  - buttons, toggles (`value` = new boolean), selects and segmented controls (`value` = chosen option value), tab switches (`value` = 1-based index), text fields on Enter (`value` = text), clickable cards, stats, chips, list and table rows (the row's `value`), empty-state buttons;
  - map selection and filter changes (`value` = `{ kind = "route"|"player", id = ... }` or `{ kind = "filter", value = "all"|"me"|"open" }`);
  - toast buttons (`slot` = `"toast"`);
  - plugin hotkeys (`slot` = `"hotkey"`, `action` = hotkey id, `value` = `nil`).

  `fb.ui.on_action(nil)` removes the handler. Blocks without their own `value` send `nil` unless stated above.

Tree size limits, per slot: depth 24, 2000 blocks, strings 2000 characters, 512 KiB in total. Action `value`s are plain data up to 8 KiB. The app coalesces updates to about 30 per second and slot.

#### Host events from `fb`

Subscribe with `fb.events.on("fb", name, fn)`; `fn(value, "fb", name)`.

| Event | Value |
|---|---|
| `detail.game` | `{ system, code, sha256, title }` when the game detail shows a game, `nil` for none. The host clears the `detail` slot on every change; the plugin sets a new tree. Posted when the selected game in the Library changes. The game code is known only once the game file is cached, so plugins whose `games` entry has code globs match a game from then on. |
| `tiles` | Array of `{ id, player, user_id, game }` for the visible remote Multiview tiles; `user_id` is the remote player's server user id, an empty string when unknown; `id` is stable for a session and is the `<tileId>` of `tile:<tileId>`. |
| `hud.visible` | `true` or `false` when the user shows or hides this plugin's HUD widget. |

#### Blocks

A block is a table `{ type = "...", id = "...", children = { ... }, <props> }`. `id` is optional (`[A-Za-z0-9_.-]{1,64}`, unique within the slot; needed for `fb.ui.patch`). Only containers take `children`. Unknown props and blocks are errors; "required" props must be present.

| `type` | Props | Children |
|---|---|---|
| `column`, `row` | `gap`, `align` (`start`, `center`, `end`, `stretch`) | yes |
| `grid` | `gap`, `columns` (1..6) | yes |
| `tabs` | `tabs` (1..12 labels, required), `selected` (1-based), `action` | yes, child `i` is page `i` |
| `scroll` | `max_height` (`s`, `m`, `l`; omitted = fill) | yes |
| `card` | `title`, `eyebrow`, `link_label`, `action`, `value` | yes; clickable when `action` is set |
| `section` | `title` (required), `link_label`, `action` | yes; header plus content |
| `text` | `text` (required), `style` (`title`, `heading`, `body`, `meta`, `eyebrow`, `value`), `tone`, `wrap` (default true) | no |
| `chip` | `text` (required), `tone`, `action`, `selected` | no; focusable filter chip when `action` is set |
| `badge` | `text` (required), `tone` | no |
| `avatar` | `initial` (1..4 chars, required), `color` (1..8), `size` (`s`, `m`, `l`) | no |
| `image` | `src` (required), `size` (`l`, `m`, `s`), `state` (`alive`, `dead`, `missed`, `empty`), `owner` (1..8), `label` | no |
| `bar` | `value` (0..1, required), `tone`, `label` | no; default tone by value: above 0.5 ok, 0.2 to 0.5 warning, below 0.2 danger |
| `stat` | `label`, `value` (required), `action` | no; shows "›" when `action` is set |
| `list` | `rows` (up to 500, required), `selected` (row id) | no |
| `table` | `columns` (1..12 labels), `rows` (up to 500) (both required) | no |
| `button` | `label`, `action` (required), `variant` (`primary`, `secondary`, `quiet`, `danger`), `value`, `confirm` | no |
| `toggle` | `action` (required), `label`, `checked` | no |
| `select` | `options` (1..100, required), `selected`, `action` (required) | no |
| `segmented` | `options` (2..3, required), `selected`, `action` (required) | no |
| `textfield` | `action` (required), `text`, `placeholder`, `error` | no; the action fires on Enter |
| `empty` | `title` (required), `text`, `action_label`, `action`, `tone` | no; `tone = "danger"` is an error state |
| `location` | `title` (required), `sub`, `status` (`open`, `encounter`, `caught`, `missed`, `none`) | yes, optional |
| `map` | see [Map block](#map-block) | no |

Prop details:

- `list` row: `{ id?, title (required), sub?, meta?, image?, tone?, action?, value? }`; rows with an `action` are focusable. `table` row: `{ cells = {...} (up to 12), action?, value? }`.
- `options` entries: `{ value, label }`. `confirm` on a button: `{ title, text, confirm_label?, danger? }` shows a confirmation dialog first; for `danger` the initial focus is Cancel.
- `action` is a name of 1 to 64 characters handed to `fb.ui.on_action`. `value` is any plain data up to 8 KiB. Most short strings (titles, labels, subtitles) are limited to 200 characters, `text` and `textfield` text to 2000.
- `tone`: `neutral`, `info`, `ok`, `warning`, `danger`.
- `gap`: 4, 8, 12, 16 or 20; other numbers are rounded down to the nearest allowed value (below 4 becomes 4).
- Sizes: `image` `l` = 68x56, `m` = 40x30, `s` = 24x18 (pixel art is scaled nearest-neighbour); `avatar` `s` = 20, `m` = 26, `l` = 32; `scroll` `s` = 160, `m` = 280, `l` = 420.
- App colours: `color` and `owner` are numbers 1 to 8. Colours 1 to 4 are designed in the design tokens; 5 to 8 are derived from them in the same style.

#### Map block

`type = "map"` shows a background image with markers in image coordinates, with zoom and pan (wheel, drag, shoulder buttons, right stick), fit (the whole image inside the block, centered), clusters below 60 % zoom per `area`, a popover above the selected marker, a filter and a legend. Keyboard and controller move the selection to the nearest marker in a direction.

| Prop | Meaning |
|---|---|
| `image` | `src` of the map image; omitted = loading state "map not read yet" |
| `image_w`, `image_h` | Image size in pixels (1..16384) |
| `players` | Up to 16 of `{ id, initial, color, x, y, label? }` |
| `routes` | Up to 1000 of `{ id, x, y, area?, number?, done?, icons? }`; `icons` is up to 8 of `{ src, owner? }` |
| `filter` | `all`, `me` or `open` |
| `me` | App `id` used by the "Only me" filter |
| `selected` | `id` of the selected route or player |
| `legend` | Show the legend (default true) |
| `popover` | Up to 16 rows `{ owner, name, src?, status }` for the selected marker |
| `action` | Receives `{ kind = "route"\|"player", id }` on selection and `{ kind = "filter", value }` on filter changes |

Marker ids are strings (`[A-Za-z0-9_.-]{1,64}`) or non-negative integers; coordinates are numbers.

#### Images

`src` (and `image` of list rows, `src` of map icons) is `cache:<path>` for a file in the plugin's cache (`fb.cache.write`) or `asset:<path>` for a file declared in the manifest `assets`. Paths use `A-Z a-z 0-9 . _ - /`, no `..`, no leading `/`, up to 200 characters. Only PNG files are served. The app rejects files above 8 MiB, images above 4096x4096, paths that leave the folder (including through symlinks) and undeclared assets. Convert game textures to PNG with `fb.codec.png` and cache them.

#### Hotkeys

- Controllers → Hotkeys shows an Uzume group first (these keys always win), then one group per enabled plugin that declares hotkeys, ordered by plugin id, each with an origin marker and its key count. The user can choose a key, clear it or reset it to the declared default.
- Overrides are stored locally in the app settings (`plugin_hotkeys`): `"<pluginId>/<hotkeyId>"` → key text, `"none"` = unassigned.
- Conflicts: Uzume keys win (configured Uzume hotkeys, Esc, F4, Alt+1 to Alt+9), then the earlier plugin. A losing key is not active; it is shown as "<key> · not active" in amber with "Already used by <owner>. Unassigned until you choose."
- In a game, an active plugin key calls the plugin's `fb.ui.on_action` handler as `fn(id, nil, "hotkey")`. Keys do not fire while a text field has focus or on auto-repeat. Plugin keys and Alt+n are not passed to the core.
- Alt+1 to Alt+9 open panel tab n.

#### HUD, toasts and tile strips

- HUD widgets stack in the bottom left (default), bottom right or top left; the top right belongs to the diagnostics overlay. At most 4 widgets per corner are shown. The user can move or hide a widget, and hide all HUD widgets with F4. While widgets are hidden, a pill at the bottom of the game view offers "Show <plugin> widget" per hidden widget and Show/Hide all.
- Toasts appear at the bottom right.
- A remote Multiview tile shows one plugin strip (20 px high). The strip uses the first text row of the tree. Tile content usually comes with runs (package 3).

#### Example

`manifest.json`:

```json
{
  "id": "net.example.counter",
  "name": "Counter",
  "version": "0.1.0",
  "api": 1,
  "permissions": ["memory.read", "ui"],
  "entry": "main.lua",
  "ui": { "panel": { "title": "Counter" }, "hud": true }
}
```

`main.lua`:

```lua
local hits = 0

local function widget()
  return { type = "row", gap = 8, align = "center", children = {
    { type = "text", id = "n", text = tostring(hits), style = "value" },
    { type = "bar", id = "bar", value = (hits % 10) / 10, label = "to 10" },
  } }
end

fb.ui.set("hud", widget())
fb.ui.set("panel", { type = "column", gap = 12, children = {
  { type = "section", title = "Counter", children = {
    { type = "stat", id = "total", label = "Hits", value = hits },
    { type = "button", label = "Reset", variant = "danger", action = "reset",
      confirm = { title = "Reset counter?", text = "The value goes back to 0." } },
  } },
} })

fb.ui.on_action(function(action)
  if action == "reset" then
    hits = 0
    fb.ui.patch("panel", "total", { value = hits })
    fb.ui.patch("hud", "n", { text = "0" })
  end
end)

fb.events.on("*", "counter.hit", function()
  hits = hits + 1
  fb.ui.patch("panel", "total", { value = hits })
  fb.ui.patch("hud", "n", { text = tostring(hits) })
  fb.ui.patch("hud", "bar", { value = (hits % 10) / 10 })
  fb.ui.dot("news")
end)
```

### Run state

Permission `run.state`. `fb.run` exists only when the game was started with "Play run" from the Runs screen (a run needs a server with `runs_v1`; a run without any plugin needs `runs_v2`) or in the [run view](#when-plugins-run) (read-only, see below); otherwise it is `nil`, so check `if fb.run then`. The run's plugins are the exact server copies pinned by the run, so all participants run the same versions.

| Function | Meaning |
|---|---|
| `fb.run.current()` | `{id, name, status, me, settings, plugins = {...}, participants = {{user_id, name, color, status, game_id, variant_kind?}, ...}, randomizer?}` from the app's cache; `randomizer = {mode = "each"|"same", preset_id, tool_version, allow_own}` exists only for a randomized run (never seed or settings), `variant_kind` (`"run"`|`"own"`) only when the server sends it. |
| `fb.run.state([user_id [, plugin_id]])` / `fb.run.states([plugin_id])` | One participant's state object for a plugin of the run (defaults: the own user and the calling plugin; `nil` if none yet) / table `user_id -> state` for that plugin (default: the calling plugin). Another plugin's state of the same run may be read (read-only), for example state published by another run plugin. Updated live; no network call. |
| `fb.run.setState(tbl)` | Replaces the calling plugin's state object of the own participant (JSON-able table, at most 64 KiB per plugin). Coalesced per plugin to at most 4 sends per second: the latest value wins. On `409 state_conflict` the app adopts the server revision of that plugin and sends the latest value again. |
| `fb.run.event{type=, text=, payload=, source=}` | Posts a run event (`type` required, `text` at most 280 characters, `payload` an object of at most 4 KiB, `source` `"automatic"` (default) or `"manual"`). Returns the `client_id` (a UUID); the app queues the event and retries it idempotently until the server confirms it (at most 2 per second). |
| `fb.run.events(after_seq, limit)` | Events from the local cache, ascending by `seq` (limit 1 to 500, default 100). Missed events are paged from the server after a reconnect. |
| `fb.run.on(kind, fn)` | Callbacks on the plugin thread like ticks (at most 16): `"event"` `fn(event)` (own and other participants' events), `"state"` `fn(user_id, state, plugin_id)` (every plugin's state change, own writes included), `"status"` `fn(status, previous)`, `"participants"` `fn(list)`. |
| `fb.run.setBoard(tree)` | Sets the plugin's board for the server run board. `tree` is checked with the same validator as `fb.ui` blocks (64 KiB at most); at most 1 per second (the latest tree is sent after the pause). |

Rules:

- Run view: `fb.run.current()` has `view = true`; reads and `fb.run.on` work; `setState`, `event` and `setBoard` return `nil, "read only outside the game"`.
- Writes (`setState`, `event`, `setBoard`) are accepted while the run is `starting` or `active`. In `paused`, `finished` or `abandoned` they are refused locally and return `nil, "message"` instead of raising, so a paused run never pauses the plugin. Reads keep working.
- Bad arguments (not a table, too large, unknown field) raise a Lua error as everywhere else in the API.
- Every plugin of the run has its own state object per participant, each with its own revision on the server (`PUT /runs/{run_id}/state/{plugin_id}`). `setState` replaces only the calling plugin's object; plugins never overwrite each other's state. The app keeps the states per (user, plugin) in its run cache.
- Join and Start dialogs list the library games that every plugin of the run accepts: the system must match a `games` entry of the plugin and, when the entry has `codes`, the server's game code (`game_code`, same identity as `fb.game.code`) must match one of the globs (`*`, `?`, case-insensitive). A game whose code the server does not know stays selectable after the matching ones with the hint "Game not recognized"; a known code that matches nothing is not listed.
- The server may refuse a write (`403`, `409 run_not_active`, `413`, `429`); the app drops the value and logs it. Nothing is retried except events (idempotent by `client_id`) and the latest state after a conflict.
- Required plugins: the Start dialog adds the `requires` of the picked plugins automatically (newest server-offered version that fits, transitive), shown as "Included: <name> <version> · needed by <name>". They need no `run.state` permission, count toward the 8-plugin limit and take part in the game filter. A missing or conflicting requirement blocks the start with a message.
- Saves of a run go to the slot `run-<first 8 hex of the run id>` (the run's `save_slot`, shown in the app as "Run: <name>"); the main slot of the game is never touched.

### Run settings

Manifest field `run_settings` (optional, 1 to 32 entries) declares the rules a plugin wants when a run is started. App and server enforce the same rules; violations are manifest errors, unknown fields inside an entry are ignored.

| Field | Rules |
|---|---|
| `key` | 1 or 2 dot-separated segments, each `^[a-z][a-z0-9_]{0,31}$`. Unique; no key is a segment-prefix of another (`rules` and `rules.dupes_clause` conflict). |
| `type` | `"boolean"` or `"enum"`. |
| `label`, `hint` | i18n keys `^[A-Za-z0-9_.-]{1,64}$`, looked up in the plugin's i18n files. `hint` is optional. |
| `default` | Required: a boolean, or an option `value` for `enum`. |
| `options` | `enum` only, 2 to 8 of `{ value, label }`; `value`s are unique and follow the `key` segment pattern; `label` is an i18n key. |

- The server plugin list carries `run_settings` and `run_settings_strings` (`{ lang: { key: text } }`, only the referenced keys; OpenAPI 1.19.0).
- The app's "Start a run" dialog shows a Rules section per plugin: boolean as toggle, enum with up to 3 options as segments, more as select. Label language: the plugin language setting (base language), then `en`, then the key.
- Every entry is sent as run settings `settings[<plugin_id>]`, nested by key segments. The keys `mode` and `rules.dupes_clause` give `{ "<plugin_id>": { "mode": "soullink", "rules": { "dupes_clause": true } } }`. Plugins without `run_settings` get no member.
- Plugins read them with `fb.run.current().settings[<own plugin id>]`. The raw manifest `settings` field is unchanged (schema still undefined).

## Installing plugins and origins

A plugin reaches the app in three ways, and the app always shows where it came from (Settings -> Plugins):

| Origin | Pill | How | Runs |
|---|---|---|---|
| `official` | "✓ Official" | signed by the Uzume release key, from the server, a `.fbplugin` file or the plugin index | always |
| `unverified` | "! Hub upload · unverified" | unsigned, offered by the server (an admin uploaded it) | always; the app warns before it is turned on |
| `local` | "Local file" | unsigned `.fbplugin` file or plugin folder on this device | only while Developer mode is on |

- **From the server:** "Browse plugins on your Hub" lists what the server offers (REST `GET /plugins`, handshake feature `plugins_v1`). Install or Update downloads the file with the SHA-256 from the list as a pin: a file that does not match is rejected and nothing is installed; a network error leaves the previous state. The file is checked like any `.fbplugin` (below) and stored as a server copy under `<data>/plugins/hub/<id>/<version>/` (several versions side by side, `origin.json` written by the app). A new install is turned off. An update that asks for permissions the previous version did not have is turned off.
- **From a file:** "Install from file..." takes a `.fbplugin` (or a plugin folder). A valid Uzume signature makes it Official, otherwise it is Local. It is copied to `<data>/plugins/installed/<id>/`. A SIGNATURE.json or `origin.json` inside a folder is never trusted.
- **Outside runs** one copy per plugin id is active: a linked developer folder, else the installed file/folder, else the newest server copy. A run uses exactly the server version of the run (`PluginDelivery::ensureHubCopy(id, version, sha256)` downloads it when missing); a local copy never replaces it.

### `.fbplugin` file

A zip (stored or deflated) with `manifest.json` at the root, the entry file, assets and i18n as in a plugin folder, and an optional `SIGNATURE.json`. Limits, identical in server and app: file <= 32 MiB, sum of unpacked sizes <= 64 MiB, <= 2000 entries; names are relative paths with `/`, no leading `/`, no `\`, no drive letter or `:`, no `.` or `..` or empty segment, <= 255 bytes, no duplicates ignoring case, no symbolic links (directory entries are ignored); `manifest.json` valid, `api` 1, entry file present. Not a valid zip, encrypted entries, zip64 and multi-part archives are rejected.

```json
{ "format": 1, "files": { "<path>": "<sha256 hex>", ... }, "signature": "<base64 Ed25519 signature>" }
```

`files` lists exactly every file except `SIGNATURE.json`. The signed message is `"uzume-plugin-v1\n"` followed by `"<sha256>  <path>\n"` for every file sorted by path (UTF-8 byte order). A signature that does not verify against the trusted key is reported as invalid and never counts as Official.

## Developer mode

Plugins that are not signed ("Local file") only run while **Developer mode** is on; Official and server-offered plugins need no Developer mode. In Developer mode you can:

- install a plugin from a folder (the app copies it),
- link a folder (`linkDevFolder`): the plugin runs straight from your folder, and saving a file reloads it after about 300 ms,
- read the developer console: status, WATCHED values, EVENTS, LOG, and the error with traceback when a plugin was paused.

A new install is turned off. A plugin whose requirements are missing, too old or cyclic cannot be turned on; the app shows the reason (for example "needs Creature Core ^2.0 · you have 1.2.0", "needs net.example.core, not installed", "dependency cycle: a -> b -> a").

## Minimal example

`manifest.json`:

```json
{
  "id": "net.example.hello",
  "name": "Hello",
  "version": "0.1.0",
  "api": 1,
  "permissions": ["memory.read"],
  "entry": "main.lua"
}
```

`main.lua`:

```lua
fb.log.info(fb.i18n.t("hello", { title = fb.game.title or "?" }))

fb.memory.watch("counter", 0x02000000, 4)
fb.dev.watch("counter", 0x02000000, "u32")

local last
fb.on_tick(function()
  local v = fb.memory.u32(0x02000000)
  if v and v ~= last then
    last = v
    fb.events.emit("counter.changed", { value = v })
  end
end)
```

`i18n/en.json`: `{ "hello": "Hello from {title}" }`
