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
  "recap": [
    { "type": "tracker.catch", "glyph": "catch" },
    { "type": "tracker.death", "glyph": "lost" }
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
| `permissions` | `memory.read`, `memory.write`, `rom.read`, `ui`, `run.state`, `video.capture`, `cheats`, `store`, `variants.build`. `tool.exec` is rejected ("permission `<p>` is not available yet"), unknown names are errors. |
| `entry` | Relative path of the `.lua` entry file inside the plugin folder (no `..`, must exist). |
| `assets` | Relative paths of bundled files or folders. |
| `run_settings` | Optional run rules form, see [Run settings](#run-settings). |
| `recap` | Optional run recap mapping, see [Run recap](#run-recap). |
| `settings` | Kept as raw JSON; the schema is not defined yet. |

## Permissions

| Permission | Unlocks | Status |
|---|---|---|
| `memory.read` | `fb.memory.*`, `fb.dev.watch` | available |
| `rom.read` | `fb.rom.*` | available |
| `ui` | `fb.ui.*` and the manifest field `ui` (declarative UI blocks, toasts) | available |
| `run.state` | `fb.run.*` (per-player run state, run events, run board) | available |
| `video.capture` | `fb.video.*` (screen layout and PNG captures of the game picture) | available |
| `memory.write` | `fb.memory.write*`: changes game memory through the one write queue; per-game consent "Change game memory", safety copy "Before memory changes", run log | available (0.20) |
| `cheats` | `fb.cheats.*`: hands cheat codes to the core's own cheat engine; per-game consent, safety copy "Before cheats", run log | available (0.20) |
| `store` | `fb.store.*`: small private data per person and plugin, synced with the server | available (0.20) |
| `variants.build` | `fb.variants.*` (register presets and games, ask Uzume to build a game variant) | available; signed official plugins, local and dev-linked plugins only while Developer mode is on |

Calling a function without its permission raises the Lua error `permission <p> not granted`. Everything else (`fb.log`, `fb.events`, `fb.provide`/`fb.call`, `fb.cache`, `fb.i18n`, `fb.clock`, `fb.on_tick`, `fb.save.snapshot`, `fb.game.hash`) needs no permission.

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

**Pause rule.** A budget or memory overrun pauses the plugin at once. An ordinary Lua error that is not caught is only logged (level Error) and the plugin keeps running; the third such error within 10 seconds pauses it (the log line says "3 errors within 10 s"). The app shows the toast "<Plugin> paused after an error" (with "Open console" in Developer mode). A reload starts a fresh 10-second window.

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
- `fb.game.hash()` returns the SHA-256 of the game file as lower-case hex (the key of the server's cheat database), `nil` without a game. It needs no permission and is missing in the shell context.
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

### Game memory writes (`memory.write`)

Changes the running game's memory. Every write of every plugin goes through the one command queue of the emulation, applied on the emulation thread between two frames (also while paused), so the order is the order of the calls. The checks run in this fixed order, no plugin can skip one:

1. **Run rule.** In a run whose rule `memory_write` is `off` (the default) every call returns `nil, "blocked_by_run_rule"`. `logged` allows it.
2. **Consent.** The first write in a game asks the person once ("Let plugins change game memory in <game>?", button "Allow changes"). Until then calls return `nil, "consent_required"` (the question is shown; try again later). "Allow" is remembered for that game on this PC; "Cancel" refuses for the rest of the play session (`nil, "consent_denied"`) and is not remembered.
3. **Safety copy.** Before the first write of a play session the app saves a snapshot of the save on the server, labelled "Before memory changes". If that fails, the queued writes are dropped and the person sees a notice.
4. **Write.** Uploaded saves of the session carry the marker `X-Uzume-Cheats-Used`, which the app shows as "Made with cheats". In a run, the run log gets an event `memory.write { plugin_id }`, at most one per 10 seconds and plugin.

| Function | Meaning |
|---|---|
| `fb.memory.write(addr, bytes)` | Writes the string `bytes` (1 to 65536 bytes). |
| `fb.memory.write_u8/u16/u32(addr, v)` | Writes a little-endian integer (signed or unsigned range of the size, otherwise a Lua error). |

All return `true`, or `nil, code` with one of `no_game`, `blocked_by_run_rule`, `consent_required`, `consent_denied`, `unmapped` (the range is not fully inside the core's memory map; nothing is written), `queue_full` (more than 256 writes waiting) or `invalid`. `true` means queued, not yet applied.

If two plugins keep writing the same address (different plugins, within 1 second, repeatedly) the app logs it and shows the toast "two plugins fight over game memory"; the last write still wins each time.

### Cheats (`cheats`)

Hands cheat codes to the core's own cheat engine (`retro_cheat_set`; for the 3DS core to its Gateway engine). The plugin keeps its cheat lists itself (see [Plugin data](#plugin-data-store)); the app holds one active list per game. The same order as for memory writes applies: run rule `cheats` (`off` by default, `logged` allows), consent "Turn on cheats for <game>?" (once per game; the confirm button is remembered), safety copy "Before cheats" before the first active cheat of a play session, then the core. Uploaded saves of the session carry `X-Uzume-Cheats-Used`. The run log gets `cheat.enabled { name, format, plugin_id }` and `cheat.disabled` for each change. The game header shows "N cheats on". The active list is re-applied by the app after a reset or a loaded state.

| Function | Meaning |
|---|---|
| `fb.cheats.formats()` | The code formats the core takes (`raw`, `gsa`, `cb`, `ar_ds`, `ar_gba`, `game_genie`, `gateway`), as a list; the second value is the core's display name, or `nil` when unknown. |
| `fb.cheats.allowed()` | `true`, or `false, "run_rule"` when the run's rule forbids cheats. |
| `fb.cheats.apply(list)` | Replaces the active list. Items are `{ id (required, up to 64), name?, format (required), code (required, up to 4096), enabled? (default true) }`, at most 2000. Returns `{ applied = n, rejected = {{ id, reason }, ...} }` with the reasons `unsupported_format`, `invalid_code`, `invalid`; or `nil, code` with `no_game`, `blocked_by_run_rule`, `consent_required` (the question is shown; the app applies the list itself once the person agrees, and `cheats.blocked` follows if not), `consent_denied`. |
| `fb.cheats.clear()` | Turns everything off. Always allowed. `true` or `nil, "no_game"`. |
| `fb.cheats.database()` | The server's cheat list for this game, `{ source, updated_at, cheats = {{ name, code, format_guess }, ...} }`, from the local cache. On failure `nil, code`: `"off"` (the server has the cheat database turned off), `"offline"`, `"not_found"` (the server does not know the game), `"error: <text>"`; `"error: loading"` while the first fetch is under way (the event `cheats.database` follows). The cache refreshes in the background. |

App events (subscribe with `fb.events.on("app", name, fn)`; `fn(value, "app", name)`):

| Event | Value |
|---|---|
| `cheats.blocked` | `{ reason }`: `"run_rule"` (the rule turned cheats off; all active cheats were cleared at once, live when the run is updated), `"consent_denied"`, or `"snapshot_failed"` (the safety copy failed, so no cheat was turned on). |
| `cheats.allowed` | `nil`, when a run rule switches cheats back on. |
| `cheats.database` | `nil`, when `fb.cheats.database()` has new data. |

### Save snapshots (no permission)

`fb.save.snapshot(reason)` takes a manual snapshot of the current save on the server, labelled "<plugin name>: <reason>" (`reason` 1 to 80 characters), e.g. before a risky step. It returns `true` (queued) or `nil, message` (`"reason is empty"`, `"reason is longer than 80 characters"`, `"no_game"`, `"rate_limited"`: at most one per minute and plugin). The person sees the toast "<plugin> saved a snapshot · <reason>" (or a notice that nothing could be saved, e.g. no save on the server yet). A snapshot can be restored in the save history like any other.

### Plugin data (`store`)

`fb.store` keeps small private data per person and plugin (cheat lists, settings, notes). It reads and writes a local copy on this PC at once, never waits for the network, and works offline and with "Only on this PC". The app syncs the copy with the server (`plugin_store_v1`) in the background, per key with revisions; plugins never see other people's data.

| Function | Meaning |
|---|---|
| `fb.store.get(key)` | `value, revision`, or `nil, 0` when the key does not exist. `key` is `[a-z0-9_.-]{1,64}`. |
| `fb.store.put(key, value [, base_revision])` | Stores any JSON-like value (nil, tables with string keys or arrays, numbers, booleans, strings; up to 256 KiB per value, 1024 keys and 16 MiB per plugin). Returns `true, revision`. With `base_revision` (the revision the plugin read) a stale write is refused with `nil, "conflict"`; the attempted value is kept as a conflict. Other failures: `nil, "too_large"`, `nil, "quota_exceeded"`. |
| `fb.store.conflicts(key)` | List of kept second versions `{ value, revision, source }`; `source` is `"local"` (a stale write) or `"server"` (another PC wrote first). Empty when there is none. |

Nothing is overwritten silently: when the server has a newer value for a key that has local changes, or a push gets `409 store_conflict`, both versions are kept. The key stays as it is locally (it is not pushed) until the plugin resolves it with a normal `put`, which clears the conflicts. App events: `store.changed` (value = the key; the server's version replaced the local one) and `store.conflict` (value = the key; both versions are kept).

### Debug tools (MCP)

The debug MCP server (Settings › Diagnostics › Claude debugging, see the [app guide](app.md#claude-debugging-mcp)) has tools for the new APIs: `cheats_list` (active list, core formats, why cheats are blocked, consent state), `cheats_apply` (a JSON list as plugin `mcp`; `bypass_consent: true` skips only the per-game question, the run rule, the safety copy and the run log still apply), `cheats_clear`, `store_get` / `store_put` (any plugin id, local copy of the connected user) and `consent_set { kind: "cheats" | "memory_write", allow, forget?, game? }`. They change the running game or the person's plugin data, like `game_memory_write`.

### Variants (`variants.build`)

For the official Randomizer plugin ([ADR 0031](../adr/0031-game-variants-and-randomizer.md), Uzume 0.21). A plugin never starts a program: it tells Uzume which presets and games it supports and may ask Uzume to build a variant of a game from a recipe; Uzume draws the dialogs, the variant list and the progress itself and runs the engine from the server's tools catalog on the user's PC. The permission is granted to signed official plugins; a local plugin (unsigned package or folder) or a plugin linked from a developer folder can use it only while Developer mode is on, otherwise the manifest shows the problem "permission variants.build needs a signed official plugin or Developer mode" and the plugin cannot be turned on. A plugin with this permission also runs in the shell context (no game), because it registers there. Without the permission `fb.variants` does not exist.

- `fb.variants.register{ engine, presets, games }` stores what the plugin offers (call it again to replace it; the registration ends when the plugin is disabled, reloaded or removed). Returns `true`; errors raise a Lua error.
  - `engine`: `"randomizer"` (default; the only engine).
  - `presets`: up to 32 of `{ id, label, hint, settings }`. `id` is 1 to 32 characters of `a-z 0-9 _ -` and not `"custom"` (Uzume adds the "Custom" entry itself); `label` up to 60 and `hint` up to 200 characters; `settings` maps an engine settings version (three digits, `"428"` for FVX 1.6.2) to a settings string of 1 to 4096 characters, up to 8 versions. Uzume uses the string of the highest version; the engine upgrades older ones itself.
  - `games`: up to 64 of `{ system, codes }` with the system id (`nds`, `3ds`, `gba`, ...) and up to 512 game codes as `fb.game.code` carries them (globs as in the manifest `games` field: `*` any run, `?` one character, ignoring case; no `codes` = the whole system). Randomize… appears on the game page for these games only, and the New run section offers the presets for them.
- `fb.variants.engine()` returns `{ offered, version, on_pc }`: whether the server offers the engine, the version it offers (absent if none) and whether it is already on this PC.
- `fb.variants.list([game_id])` returns the user's variants of the current game as `{ id, name, slot, preset_id?, state, tool_version? }`; `state` is `not_built`, `building`, `ready`, `needs_rebuild` or `failed`. In the shell context pass `game_id`. It never returns the seed or the settings.
- `fb.variants.build{ name, preset_id | settings, seed, spoiler_log, game_id }` asks Uzume to create and build a variant and returns a request id at once. `name` is 1 to 60 characters; give exactly one of `preset_id` (a registered preset) and `settings` (up to 4096 characters); `seed` is a non-negative integer (or a string of digits) below 2^63, absent = Uzume picks one; `spoiler_log` defaults to false; `game_id` is required in the shell context and ignored in a game. Argument errors raise a Lua error; everything that depends on the PC or the server (engine not offered or not downloaded, name taken, base file missing or different, not enough memory or disk space, engine failure) is reported through the event below, never as a Lua error.
- Host event `variants.state` (`fb.events.on("fb", "variants.state", fn)`): `{ id, state, progress, error? }` for the request id; `state` is `building` (with `progress` 0 to 1), `ready`, `failed` (with `error`, a short code such as `engine_missing`, `base_mismatch`, `not_enough_memory`, `disk_full`, `engine_failed`, `timed_out`) or `cancelled`. Errors carry no game data.

```lua
fb.variants.register{
  presets = { { id = "classic", label = "Classic", hint = "Everything shuffled", settings = { ["428"] = "428..." } } },
  games = { { system = "nds", codes = { "IPKE", "IPKD" } } },
}
fb.events.on("fb", "variants.state", function(ev) fb.log.info(ev.id, ev.state, ev.progress) end)
local id = fb.variants.build{ name = "My run", preset_id = "classic", seed = 123 }
```

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
- **In-game screen:** the run tab does not exist while a game runs, so `fb.ui.open("screen")` shows the `screen` tree in a sheet over the play area and the side panel (always dark, content up to 1100 px wide). A `screen` tree whose root `column` has `width = "dialog"` keeps its content at dialog width (560 px, centered) instead of 1100 px, which suits forms such as "Add cheats". The game keeps running; Esc or "← Back to game" closes the sheet, and actions in it reach the plugin with slot `"screen"`.

#### `fb.ui`

All functions raise `permission ui not granted` without the permission. A slot must be declared in the manifest, otherwise `slot <s> not declared in manifest`.

- `fb.ui.set(slot, tree)` replaces the tree of a slot. `slot` is `"panel"`, `"hud"`, `"screen"`, `"detail"`, `"run"` or `"tile:<tileId>"` (`<tileId>` as `[A-Za-z0-9_.-]{1,64}`, needs `ui.tile`). `tree` is one block. An invalid tree raises a Lua error with a path, for example `ui tree: children[2].type: unknown block "foo"`.
- `fb.ui.patch(slot, id, props)` merges `props` into the block with that `id` in the current tree, so you send only changed values. Errors: no tree in the slot yet, unknown `id`, `type`/`id`/`children` in `props`, invalid props. A prop cannot be removed with patch (a `nil` value does not exist in a Lua table); set it to an empty or neutral value or use `set`.
- `fb.ui.clear(slot)` removes the tree.
- `fb.ui.title(slot, text)` overrides the declared title (slots `panel` and `screen` only; 1 to 40 characters; `nil` restores the declared title).
- `fb.ui.open(slot)` asks the app to show the plugin's own `"screen"` or `"panel"` (needs `ui.screen` / `ui.panel`; other slots raise `fb.ui.open only supports the slots screen and panel`). In a game, `"screen"` opens the in-game screen sheet and `"panel"` selects the plugin's tab in the side panel (shown if collapsed; ignored in fullscreen and in the Multiview). Outside a game, `"screen"` selects the plugin's run tab when the plugin runs in the open run view, otherwise it opens the plugin's screen as a sheet (the same sheet as "Open" in Settings › Plugins, where plugins without runs open their screen), and `"panel"` is ignored. Typical use: a button or hotkey action handled in `fb.ui.on_action`.
- `fb.ui.close(slot)` closes what `fb.ui.open` showed: the in-game screen sheet (`"screen"`) or the plugin's panel tab (`"panel"`). Nothing happens when it is not open.
- `fb.ui.pick_file{ title, filters, max_bytes }, callback` opens the system file dialog (`title` up to 100 characters, `filters` up to 12 patterns like `"*.cht"`, `max_bytes` default 1 MiB, at most 8 MiB). The plugin gets the content only, never the path or the folder: `callback(content, { name = "<base name>", size = <bytes> })`. Cancel calls `callback(nil)`; a file larger than `max_bytes` (refused without reading it) or an unreadable one calls `callback(nil, "too_large")` or `callback(nil, "read_failed")`. One dialog per plugin at a time.
- `fb.ui.badge(n)` shows a count on the plugin's run tab. `0` or `nil` hides it; values are clamped to 0..999.
- `fb.ui.dot(kind)` sets the dot on the panel tab: `"news"` or `nil` to clear. The amber "paused" dot is set by the app.
- `fb.ui.notify{ tone, title, text, actions }` shows a toast and returns its id. `tone` is `"info"` (default), `"important"` or `"warning"`; `title` up to 80 characters; `text` 1 to 300 characters; `actions` up to 2 of `{ label (1 to 40), action, value? }`. Rate limit per plugin: a burst of 3, then 1 per second. A dropped toast writes a log warning and returns `nil`. The user can mute a plugin's toasts (all, or all but `important`). At most the newest 3 toasts are visible.
- `fb.ui.on_action(fn)` sets the handler `fn(action, value, slot)`, called on the plugin thread for:
  - buttons, toggles (`value` = new boolean), selects and segmented controls (`value` = chosen option value), tab switches (`value` = 1-based index), text fields on Enter (`value` = text), clickable cards, stats, chips, list and table rows (the row's `value`), empty-state buttons;
  - map selection and filter changes (`value` = `{ kind = "route"|"player", id = ... }` or `{ kind = "filter", value = "all"|"me"|"open" }`);
  - `textarea` changes (`value` = the text, see the block table);
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
| `reloaded` | `{ ms }` after the plugin was reloaded (console Reload, Ctrl+R, reload on file change, Restart). Use it to restore state from `fb.store`. |

#### Blocks

A block is a table `{ type = "...", id = "...", children = { ... }, <props> }`. `id` is optional (`[A-Za-z0-9_.-]{1,64}`, unique within the slot; needed for `fb.ui.patch`). Only containers take `children`. Unknown props and blocks are errors; "required" props must be present.

| `type` | Props | Children |
|---|---|---|
| `column`, `row` | `gap`, `align` (`start`, `center`, `end`, `stretch`); `column` also `width` (`"dialog"`, see below) | yes |
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
| `toggle` | `action` (required), `label`, `accessible`, `checked`, `value`, `disabled` | no; with `value` the action gets `{ checked = <new boolean>, value = <value> }` instead of the plain boolean (to tell several toggles apart); `disabled = true` greys it out and ignores input; the screen reader name is `label`, else `accessible` (give it to every toggle without a visible label, for example one per list row) |
| `select` | `options` (1..100, required), `selected`, `action` (required) | no |
| `segmented` | `options` (2..3, required), `selected`, `action` (required) | no |
| `textfield` | `action` (required), `text`, `placeholder`, `error`, `live` | no; the action fires on Enter; with `live = true` it also fires while typing, debounced to 250 ms |
| `textarea` | `value` (text, up to 64 KiB), `lines` (visible lines, 1..40, default 4), `mono` (monospace), `placeholder`, `error`, `action` | no; multi-line text. The action fires 300 ms after typing pauses and when the focus leaves. It arrives as `on_action(<action>, text, slot)`; without `action` the name is the block `id`, without both `"change"`. (A `textfield` sends its `action` prop as the name.) |
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
| `fb.run.current()` | `{id, name, status, me, settings, rules = {states, rewind, cheats, memory_write}, plugins = {...}, participants = {{user_id, name, color, status, game_id, variant_kind?}, ...}, randomizer?}` from the app's cache; `randomizer = {mode = "each"|"same", preset_id, tool_version, allow_own}` exists only for a randomized run (never seed or settings), `variant_kind` (`"run"`|`"own"`) only when the server sends it. |
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

### Run recap

Manifest field `recap` (optional, at most 32 entries, 0.22) tells the run recap (Runs › run › Recap) which of the plugin's run events to draw and how. Each entry is `{ "type", "glyph" }`:

| Field | Rules |
|---|---|
| `type` | The run event type this plugin posts (`fb.run` events), `^[a-z0-9_.-]{1,64}$`. Unique within the list. |
| `glyph` | `catch` (■), `lost` (✕), `badge` (◆), `note` (□) or `screenshot` (▣). |

- The recap counts `catch`, `lost` and `badge` per person (tiles CATCHES, LOST, BADGES) and draws every mapped event on the lane of the person who posted it, by day. `note` and `screenshot` are drawn, not counted. Events of the plugin without an entry are not shown.
- The app reads the mapping from the plugin copy of the version the run is pinned to (else the installed one). A plugin that is not on this PC contributes nothing to the recap.
- Not mapped by the plugin: run screenshots are drawn as `screenshot` and the core events `rules.changed`, `cheat.enabled`, `cheat.disabled`, `memory.write` and `state.loaded` as `note`; none of them counts.
- An invalid entry (not an object, bad `type`, unknown `glyph`, a type mapped twice, more than 32 entries) is a manifest warning and is ignored; the plugin still loads. A `recap` that is not a list is ignored as a whole.

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
- open the **Developer console** (Settings › This PC › Plugins, and the Plugin console tab in the game panel; Ctrl+Shift+D jumps to it). One console, one data source: the debug MCP tools `plugin_console` and `devconsole_state` read the same model. It offers:
  - a plugin select (unpacked plugins carry an "Unpacked" tag and their folder), **Load unpacked...** (manifest check; an invalid folder shows the reason and installs nothing) and a primary **Reload** (Ctrl+R, only while the console has focus);
  - **Permissions**: granted permissions and "Not granted: <permission>" for permissions the plugin tried to use without declaring them;
  - **Log** (newest first; level filters Error, Warn, Info, Debug with counts; Pause log, Copy, Clear). After a pause caused by errors the console shows "Paused after an error. Your game keeps running." with **Reload and resume**;
  - **Inspect** (run state, `fb.store` keys) and a read-only **Memory viewer** (region, go to address, hex and ASCII rows, search for a value or bytes; there is no edit field);
  - **Reload on file change** for linked folders (debounced, on by default; setting `plugin_reload_on_change`).
- A reload shows the toast "<Plugin> reloaded - <ms> ms". **What a reload keeps:** the run state on the server (`fb.run`), `fb.store`, `fb.cache` and the per-game consent. **What it rebuilds:** the Lua state, UI trees, event subscriptions and watches. The plugin receives the host event `fb`/`reloaded`.
- The game header shows a "Developer mode" tag while the mode is on.

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
