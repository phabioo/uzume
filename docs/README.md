# Documentation

Uzume documentation in one screen. New here: the [root README](../README.md) (what Uzume is, quick start). Using Uzume: Guides. Where the project stands: [Roadmap](roadmap.md). Working on the code: [Development](development.md).

Every document starts with a short overview; detail follows further down or in linked documents. Each fact lives in one place: status and plan in the [roadmap](roadmap.md), finished work in [history](history.md) and `CHANGELOG.md`, reasons in the [ADRs](adr/README.md), what Uzume does today in [features](features.md), how to use it in the guides, how to develop in [development](development.md).

## Guides (running Uzume)

- [Using the app](guides/app.md): install, pairing, playing, data directory, CLI and environment
- [Server installation](guides/server-install.md): Debian package and systemd script on Linux / Raspberry Pi
- [Server in Docker](guides/server-docker.md): official image (amd64, arm64), volumes, compose, upgrade by pulling
- [Server backup and restore](guides/server-backup.md): one archive, schedule, retention, `uzume-server backup` / `restore`, admin API, restore on a new machine
- [Server configuration](guides/server-configuration.md): commands, all flags and environment variables, certificates
- [Sessions: LAN and internet](guides/sessions-over-the-internet.md): testing on a LAN, port forwards, TURN relay
- [coturn on a VPS](guides/coturn-on-a-vps.md): own external TURN relay for the server
- [Saves](guides/saves.md): sync, conflicts, history, restore, retention
- [Updates and releases](guides/updates.md): channels, automatic updates, promoting a beta to a release, rollback
- [Packaging](guides/packaging.md): Windows installer, server package
- [Walkthrough](guides/walkthrough.md): try the main features
- [Adding a system](guides/adding-a-system.md): server JSON, app manifest, core profile, input map, identity
- [Local ROMs](guides/local-roms.md): library folders, upload to the server, what others see
- [Plugin API](guides/plugin-api.md): Lua API for app plugins (0.11 runtime), developer mode
- [Publishing an official plugin](guides/plugin-publishing.md): sign a `.uzplugin`, add it to the plugin index

## Reference

- [Features as built](features.md)
- [Protocol](reference/protocol.md) (endpoints, errors, WSS messages) and [update index](reference/update-index.md)
- [Server internals](reference/server-internals.md), [App internals](reference/app-internals.md)

## Project

- [Development](development.md): build, test, dependency scripts, CI, versioning
- [Roadmap](roadmap.md): status and plan by version; [History](history.md): finished milestones, PoC phases, renumbering
- [Working with Claude Code](workflow.md): roles, briefs, phase plan
- Legal: [LICENSE](../LICENSE), [NOTICE.md](../NOTICE.md) (trademarks, disclaimer), [third-party notices](../THIRD-PARTY-NOTICES.md), [CONTRIBUTING.md](../CONTRIBUTING.md), [CLA](../CLA.md)
- [Architecture](architecture/README.md): design basis of the PoC (index; read only the file you need)
- [Design](design/README.md): screens, tokens, logo; [decisions](design/legacy/decisions.md); [Uzume design](design/uzume/README.md) (valid from 0.18 on)
- [Decisions (ADRs)](adr/README.md): one table of all decision records

## Conventions

English only. Terms: "Uzume server", "Uzume", "Session" (not "Stream", except in diagnostics). Architecture files are not rewritten; deviations are recorded as ADRs. No ROMs, BIOS or firmware, no secrets, no private hostnames or addresses (use placeholders such as `hub.example.com`).
