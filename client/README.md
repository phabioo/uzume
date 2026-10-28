# client: Uzume app

C++20/Qt 6 application: server profiles and pairing, library and ROM cache, local emulation with libretro cores (melonDS DS), save sync, Sessions and multiview, Emulation and Controllers pages. Rules for agents: `CLAUDE.md`.

## Layout

`core/` (profiles, cache, settings, updater), `network/` (server connection, sync, CLI `uzume-cli`), `emulation/` (libretro backend, manifests), `input/` (SDL3 gamepads), `media/` (H.264/Opus, WebRTC), `ui/` (QML), `app/` (executable `uzume`), `launcher/` (Windows), `testutil/`. Details: [docs/reference/app-internals.md](../docs/reference/app-internals.md).

## Build, run, test

```sh
scripts/bootstrap-vcpkg.sh                 # once
make check-client                          # builds, fetches pinned deps, runs tests (CLIENT_PRESET, default linux-debug)
make fetch-core                            # melonDS DS core (libretro buildbot); without it, real-core tests are skipped, test-core tests always run
client/build/linux-debug/app/uzume [--data-dir <path>] [--dev-allow-http]
```

Qt >= 6.4 comes from apt (Linux) or install-qt-action (Windows), not vcpkg. Prerequisites and dependency scripts: [docs/development.md](../docs/development.md).

## Documentation

- Using the app (pairing, data directory, CLI, environment): [Using the app (wiki)](https://github.com/phabioo/uzume/wiki/Using-the-app)
- Saves: [Saves (wiki)](https://github.com/phabioo/uzume/wiki/Saves); Sessions: [Sessions over the internet (wiki)](https://github.com/phabioo/uzume/wiki/Sessions-over-the-internet); updates: [Updates (wiki)](https://github.com/phabioo/uzume/wiki/Updates)
- Dependencies, features per area, firmware, controllers, updater internals: [docs/reference/app-internals.md](../docs/reference/app-internals.md)
- Design: [docs/design/README.md](../docs/design/README.md); decisions: [ADR 0003](../docs/adr/0003-player-phase2.md), [0004](../docs/adr/0004-player-portable-data.md), [0006](../docs/adr/0006-sessions-phase4.md), [0007](../docs/adr/0007-phase5.md), [0013](../docs/adr/0013-opengl-hardware-rendering.md), [0014](../docs/adr/0014-player-ui-pass.md)
