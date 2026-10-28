# Documentation

Uzume documentation in one screen. New here: the [root README](../README.md) (what Uzume is, quick start). Using Uzume: Guides. Where the project stands: [Roadmap](roadmap.md). Working on the code: [Development](development.md).

Every document starts with a short overview; detail follows further down or in linked documents. Each fact lives in one place: open work in GitHub issues and milestones, direction in the [roadmap](roadmap.md), finished work in [history](history.md) and `CHANGELOG.md`, reasons in the [ADRs](adr/README.md), what Uzume does today in [features](features.md), how to use it in the [wiki](https://github.com/phabioo/uzume/wiki) (source `wiki/`, mirrored to the GitHub wiki; user guides are no longer in `docs/guides/`), how to develop in [development](development.md).

## Developer guides

User guides (install, using the app, server, saves, sessions, updates) live in the [wiki](https://github.com/phabioo/uzume/wiki); their source is the folder `wiki/`.

- [Releasing](guides/releasing.md): promote a beta to a stable release (maintainers)
- [Packaging](guides/packaging.md): Windows installer, server package
- [Adding a system](guides/adding-a-system.md): server JSON, app manifest, core profile, input map, identity
- [Plugin API](guides/plugin-api.md): Lua API for app plugins (0.11 runtime), developer mode
- [Publishing an official plugin](guides/plugin-publishing.md): sign a `.uzplugin`, add it to the plugin index

## Reference

- [Features as built](features.md)
- [Protocol](reference/protocol.md) (endpoints, errors, WSS messages) and [update index](reference/update-index.md)
- [Server internals](reference/server-internals.md), [App internals](reference/app-internals.md)

## Project

- [Development](development.md): build, test, dependency scripts, CI, versioning
- [Roadmap](roadmap.md): direction and finished milestones; [History](history.md): finished milestones, PoC phases, renumbering
- [Working with Claude Code](workflow.md): roles, briefs, phase plan
- Legal: [LICENSE](../LICENSE), [NOTICE.md](../NOTICE.md) (trademarks, disclaimer), [third-party notices](../THIRD-PARTY-NOTICES.md), [CONTRIBUTING.md](../CONTRIBUTING.md), [CLA](../CLA.md)
- [Architecture](architecture/README.md): design basis of the PoC (index; read only the file you need)
- [Design](design/README.md): screens, tokens, logo; [decisions](design/legacy/decisions.md); [Uzume design](design/uzume/README.md) (valid from 0.18 on)
- [Decisions (ADRs)](adr/README.md): one table of all decision records

## Conventions

English only. Terms: "Uzume server", "Uzume", "Session" (not "Stream", except in diagnostics). Architecture files are not rewritten; deviations are recorded as ADRs. No ROMs, BIOS or firmware, no secrets, no private hostnames or addresses (use placeholders such as `hub.example.com`).
