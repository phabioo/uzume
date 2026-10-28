<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/banner-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset="docs/assets/banner-light.svg">
    <img src="docs/assets/banner-light.svg" alt="Uzume" width="640">
  </picture>
</p>

Uzume (formerly FrameBeam) is a self-hosted retro gaming platform. The **Uzume server** keeps your ROM library, versioned saves, users and devices in one place; the **Uzume app** emulates games locally and shares running Sessions directly with other apps. You administer the server in the app.

> The server manages. The app emulates. Audio and video flow directly between apps wherever possible.

Current version: 0.20 — status and plan: [roadmap](docs/roadmap.md). Platforms: Windows app; Linux server (Debian, Pi), also Windows and Docker.

## Highlights

- **One library, many devices:** ROMs with a hash-verified cache; pairing by admin approval or invite code.
- **Saves that never get lost:** versioned, conflicts are never overwritten silently ([saves](docs/guides/saves.md)).
- **Share a running game:** Sessions and Multiview over direct WebRTC, with an optional relay for apps outside your LAN.
- **Cores from the server:** no cores in the installers; the server fetches them from the libretro buildbot.
- **Systems:** DS, 3DS, Game Boy, Game Boy Color, Game Boy Advance. Firmware is yours; Uzume never ships it.
- **Self-updating:** signed releases, `stable` and `beta` channels.

All features: [docs/features.md](docs/features.md).

## Plugins

Sandboxed Lua plugins add game-specific features ([Plugin API](docs/guides/plugin-api.md)). Official plugins, each in its own repository under github.com/phabioo:

- [`uzume-plugin-soullink-core`](https://github.com/phabioo/uzume-plugin-soullink-core): Pokémon game data for other plugins.
- [`uzume-plugin-soullink`](https://github.com/phabioo/uzume-plugin-soullink): SoulLink and Nuzlocke runs.
- [`uzume-plugin-monster-editor`](https://github.com/phabioo/uzume-plugin-monster-editor): monster editor.
- [`uzume-plugin-cheats`](https://github.com/phabioo/uzume-plugin-cheats): cheats.
- [`uzume-plugin-randomizer`](https://github.com/phabioo/uzume-plugin-randomizer): randomized variants.
- [`uzume-plugin-splits`](https://github.com/phabioo/uzume-plugin-splits): split timer with autosplits.
- [`uzume-plugin-tracker`](https://github.com/phabioo/uzume-plugin-tracker): spoiler-free randomizer tracker.
- [`uzume-plugin-template`](https://github.com/phabioo/uzume-plugin-template): starting point for plugins.

## Quick start

**1. Install the server** (Debian / Raspberry Pi OS, amd64 or arm64). Download the `.deb` from the [releases](https://github.com/phabioo/uzume/releases):

```sh
sudo apt install ./uzume-server_<version>_arm64.deb
```

The server prints a setup code (also `uzume-server setup-code`); you enter it in the app to create the admin account. More: [Server installation](docs/guides/server-install.md), [configuration](docs/guides/server-configuration.md).

**2. Install the app** (Windows): run the MSI from the releases and choose how you will use Uzume (only on this PC, share with others, or server only). See [Using the app](docs/guides/app.md).

**3. Pair:** enter the server address (or open an invite link), compare the security code and sign in; admins create users and invites under SERVER › Users.

Add homebrew ROMs to the library, pick one and play. Friends outside your LAN: [Sessions over the internet](docs/guides/sessions-over-the-internet.md).

## Quick start for developers

```sh
scripts/bootstrap-vcpkg.sh   # once, app build
make check                   # server and app checks, quiet
```

More: [docs/development.md](docs/development.md).

## Repository

- [`server/`](server/README.md): Uzume server (Go, SQLite)
- [`client/`](client/README.md): Uzume app (C++20, Qt 6)
- [`protocol/`](protocol/README.md): OpenAPI and WSS schemas
- [`packaging/`](packaging/README.md): server `.deb`, Windows installer
- `docs/`, `scripts/`: documentation; check and dependency scripts

## Documentation

Index: [docs/README.md](docs/README.md). Most used: [Roadmap](docs/roadmap.md), [Architecture](docs/architecture/README.md), [ADRs](docs/adr/README.md), [Design](docs/design/README.md).

## License

GPL-3.0-or-later, Copyright (C) 2026 Fabio and Uzume contributors: [LICENSE](LICENSE), [NOTICE.md](NOTICE.md), [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Contributing: [CONTRIBUTING.md](CONTRIBUTING.md), [CLA.md](CLA.md).

## Legal

Not affiliated with Nintendo or any other console maker. No games, ROMs, BIOS or firmware are included or may be committed; use homebrew and content you may legally use.
