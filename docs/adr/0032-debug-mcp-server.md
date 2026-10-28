# ADR 0032: Debug MCP server in the app

- Status: Accepted. Fabio asked on 2026-10-26 for an MCP server in the app through which Claude can control the whole app and read its data, plugins included, to speed up development and debugging (the trigger: sprite and map loading of the SoulLink plugins, which so far needed a test round by Fabio per change). On the same day he chose to build it now as its own package, before 0.20.
- Date: 2026-10-26
- Deciders: Fabio

## Context

Debugging plugins and the app on Fabio's Windows PC runs through screenshots, log lines and "Collect logs" bundles that Fabio sends into a cloud thread. Every round costs him a test. Everything a developer needs already exists inside the app process: the log, the plugin Lua states with their console (log, events, watched values, UI trees), the core's memory map, the RomFS of the running game, the decoded plugin images, the game frame, the QML scene and the controllers behind it.

The [Model Context Protocol](https://modelcontextprotocol.io) (MCP) lets a Claude Code session call tools of a local server. Claude Code runs on Fabio's PC (directly, or as a Remote Control session started from a project thread) and can add an HTTP MCP server with a header. Cloud sessions cannot reach `127.0.0.1` on his PC.

## Decisions

### D1 Server in the app process, localhost only

- The app embeds an MCP server ("Streamable HTTP" transport, JSON responses, no SSE stream) at `http://127.0.0.1:<port>/mcp`. Default port 47615, changeable in the settings; it never binds to another interface.
- Requests must carry `Authorization: Bearer <token>`. `Host` must be `127.0.0.1:<port>` or `localhost:<port>`; a request with an `Origin` header is rejected unless the origin is on loopback (DNS rebinding).
- No new dependency: JSON-RPC 2.0 over `QTcpServer` with a small HTTP/1.1 parser (bounded request size).
- It speaks the MCP lifecycle (`initialize`, `notifications/initialized`, `ping`) and `tools/list`, `tools/call`. Tool results are JSON text, images are PNG `image` content.

### D2 Off by default, explicit opt-in, visible while on

- The switch "Allow Claude to control this app" lives in Settings › Diagnostics and is off by default, also in release builds (there is no separate debug build: the bugs appear in the normal build). `--mcp` turns it on for one start.
- While the server listens, the app shows a band "Claude can control this app" with a "Stop" button in every screen, the game included.
- The token is random (32 bytes, base64url), stored only in the OS credential store (target `Uzume/mcp`), never in a file or log. "Copy setup command" copies the complete `claude mcp add --transport http uzume http://127.0.0.1:<port>/mcp --header "Authorization: Bearer <token>"`; "New token" replaces it.

### D3 Tools

Read and control, grouped; the full list with arguments is in [guides/app.md](../guides/app.md#claude-debugging-mcp).

- **App:** status (versions, data directory, servers, screen, running game), log tail and search, redacted settings, connection check, diagnostics bundle, reports (read-only, admins).
- **Plugins:** list, console (log, events, watched rows, status, budgets), UI trees per slot, send a UI action, reload, call a `provides` function, run a Lua chunk in a plugin's state (`plugin_eval`), list and read the plugin cache, return a cached or decoded image as PNG.
- **Game:** state, memory regions, read and search memory, RomFS list and read, screenshots per screen, input (buttons for N frames, touch), pause, resume, frame step.
- **UI control:** the visible QML tree, click, type, keys, scroll, navigate to screens and settings pages, window screenshot.

Long reads are bounded (memory 1 MiB per call, RomFS 4 MiB per call, logs 2000 lines, images 4096x4096).

### D4 Brakes that stay with full control

- Saves are never overwritten, deleted or sent to another server through the MCP server: no tool does it directly, and UI control refuses to activate controls marked destructive (deleting saves, games, users, slots, plugins; "Share with others", installs and updates that need elevation) unless Fabio confirms a dialog in the app.
- Passwords, tokens, relay credentials and the MCP token are never returned; text fields marked as passwords can be typed into but never read.
- `plugin_eval` runs only in plugins with origin `local` (developer mode); signed and run plugins can be called and read, never evaluated. The eval chunk runs in the same sandbox, with the same budgets and API, as the plugin itself.
- Game data (RomFS bytes, decoded sprites, maps, memory) only goes to the Claude session on Fabio's PC; the repository rule (no game data in the repository or in tests) is unchanged.
- Every call is logged by name and target, never with its arguments' content or results.
- `memory.write` comes with 0.20 and follows its rules (snapshot before the first write, one write queue, run log event).

#### Amendment (0.20, 2026-10-27): state and memory tools

`game_state_save`, `game_state_load` and `game_memory_write` exist since 0.20. They are available only with the same opt-in as every other tool (`--mcp` / developer switch and token), nothing changes about who can call them. Writes go through the same between-frames command queue as plugin memory writes (one queue, applied on the emulation thread); states stay in app memory under a name, never on disk or on a server, so the rule that saves are never overwritten or sent elsewhere holds. The per-game consent and the run rules of `memory.write` apply to plugins; the debug tools are developer tools behind the opt-in and are not run-ruled.

`states_list`, `states_save`, `states_load`, `states_delete` and `rewind` exist since 0.20 package 4. Unlike `game_state_save` (raw, in app memory, named) they are the real save states of the app: the same slots (Auto and 1 to 4), local cache and server sync (`states_v1`) as the F6 and F8 keys, and the run rules `rules.states` and `rules.rewind` apply to them like to a person; `states_load` skips only the confirmation dialog. `rewind` steps back through the rewind buffer by seconds or frames and needs the "Rewind length" setting. They never touch the game's save.

`cheats_list`, `cheats_apply`, `cheats_clear`, `store_get`, `store_put` and `consent_set` follow in 0.20 package 5. Only the debug MCP can skip the per-game consent (`cheats_apply` with `bypass_consent`, or `consent_set`); the run rule, the safety copy and the run log still apply, and plugins have no such path.

#### Amendment (0.20 package 6, 2026-10-27): run and plugin helper tools

`runs_list` (runs of the connected server with core rules, plugins and people), `run_set_rules` (`PUT /runs/{id}/rules`, the whole object; creator or admin), `run_events` (event log from the app cache), `developer_mode`, `plugin_link` (link a development folder) and `file_dialog_pick` (answers an open `uz.ui.pick_file` request with a path, empty = cancel) let a debug session drive runs, rules and plugin file pickers without a mouse. `cheats_list` and `cheats_apply` now work in a game without the cheats plugin (the app service is there for every game, so the run rule and the live rule toast also work there). `store_get` of a plugin that is neither installed nor has data answers `plugin_not_found`. Nothing changes about the opt-in, the token or what plugins can do.

#### Amendment (0.20 final live check, 2026-10-27): gaps closed

| Tool or change | Use |
|---|---|
| `ui_key` `hold_ms` | holds the key down for 1 to 10000 ms before the release (rewind key, held hotkeys) |
| `settings_set` | `key`, `value`, `system?`, `game_id?`: sets a `uzume.*` option of Settings › Emulation (rewind length takes any 0 to 600 s, run-ahead, ...) in the same store as the rows and applies it to the running game; errors `unknown_key`, `invalid_value` |
| `confirm: true` on `ui_click`, `ui_type`, `ui_key` | operates a control the brake refuses, only when the app was started with `UZUME_MCP_CONFIRM=1` (test runs); without the variable the refusal stays |
| `saves_history` | read only: slot, current checkpoint and versions of the game in the saves view (no restore) |
| `toasts_list` | read only: the last toasts (states, plugins, app notices) with text and time |
| `id` and `plugin_id` | every tool that takes a plugin accepts both names |
| `ui_type` on `plugin_*` blocks | the keys go to the text field inside a plugin `textarea` / `textfield` block, not to the block |

#### Amendment (0.23, 2026-10-27): session sharing tools

`session_share` and `session_stop_sharing` start and stop the Session of the running game through the same path as the Share button. Sharing a running game's Session is reversible, visible in the app (share pill, the person can stop it) and needed for live tests of 0.23 (online list, Watch, Invite). The tools only share the running game and never change the sharing of the local server; "Share with others" stays a destructive control that UI control refuses.

## Consequences

- The developer console of 0.22 can use the same data sources; the MCP server does not replace it.
- Tests run a real HTTP client against the server in ctest (offscreen, test core `uzume_test_core`, synthetic data).
- A local process that knows the token has full control of the app while the switch is on; the switch, the band and the credential store are the mitigation.
