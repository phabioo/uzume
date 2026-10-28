# Working with Claude Code

How Uzume is built with Claude Code: Opus orchestrates and delegates packages by brief to Sonnet agents, Haiku searches; one package per thread and PR. Below: [Roles](#roles) and the [brief template](#brief-template), [token-saving rules](#token-saving-rules), [issues and milestones](#issues-and-milestones), [packages, PRs and docs rules](#packages-and-prs), the [phase plan](#phase-plan), and what the cloud can verify. Build and test commands: [development.md](development.md).

## Hybrid mode

- Development happens in Claude Code cloud sessions.
- Windows build and tests run via GitHub Actions.
- Fabio tests milestones locally (real Windows, GPU, audio, gamepads, P2P between two machines, Raspberry Pi 5 as server) or via a Remote Control session on his machine.

## What can be verified in the cloud

Verifiable:
- Server in full: Go server, SQLite, API, WSS signaling, pairing, tokens, TLS, save conflicts, firmware path, admin API; Linux builds x86-64 and ARM64 (cross-compile).
- Protocol: OpenAPI, JSON schemas, handshake, contract tests.
- App core without GUI on Linux (server client, caches, save sync, server profiles, settings hierarchy, manifests).
- Libretro backend headless with melonDS DS and a homebrew ROM (frame hashes).
- WebRTC between two processes on one machine (loopback).
- QML under Xvfb with software rendering (Qt 6.4 from apt).
- Windows build and tests via GitHub Actions.

Not verifiable (only locally by Fabio):
- Windows Credential Manager, NVENC/QSV/AMF, installer, interactive use.
- GPU, audio, gamepads, game feel, latency.
- Real P2P (NAT, firewall, two devices on the LAN).
- Running on Raspberry Pi 5 (cloud: ARM64 build or QEMU smoke test only).
- Real ROMs/BIOS: must not enter the repository or the cloud.
- Containers are short-lived: dependencies need a setup script or caching.

## Roles

**Opus is exclusively the orchestrator** and writes no product code. It bundles tasks into sensible packages, delegates via brief (template below) to the Sonnet 5.5 agents in `.claude/agents/`, reviews the result and either accepts it or sends it back to the same agent with concrete corrections.

Flow:

1. Break the task into 2-3 sensible packages.
2. Write the brief (template below).
3. The subagent implements and reports in the report format; it does not commit.
4. Opus reviews via `git diff --stat`, a targeted diff and the test result.
5. Acceptance, or correction via `SendMessage` to the same agent.
6. Commit/PR by Opus.

| Agent | Model | Responsibility |
|---|---|---|
| `hub-implementer` | Sonnet 5.5 (`sonnet`) | Go server (`server/`) |
| `player-implementer` | Sonnet 5.5 (`sonnet`) | C++/Qt app (`client/`) |
| `protocol-implementer` | Sonnet 5.5 (`sonnet`) | `protocol/` |
| `build-ci-implementer` | Sonnet 5.5 (`sonnet`) | CMake/vcpkg, Go build, GitHub Actions, `packaging/` |
| `docs-writer` | Sonnet 5.5 (`sonnet`) | `docs/`, ADRs |
| `scout` | Haiku 5.5 (`haiku`) | read-only: search, read logs/CI output, summarize |

Agents use the model aliases so they follow new releases and stay portable across providers. On the Anthropic API `haiku` resolves to Haiku 5.5 from Claude Code 2.1.293 on; on Bedrock, Google Cloud and Foundry it still resolves to Haiku 4.5 unless the deployment pins `ANTHROPIC_DEFAULT_HAIKU_MODEL`. Haiku 5.5 bills prompts over 100K tokens at a higher rate, so scout tasks stay narrow. Implementation stays on Sonnet 5.5: Haiku is meant for search, extraction and summaries, not for C++/Go changes that need judgment.

## Brief template

```text
Goal: <one sentence>
Context: <docs/architecture/NN-file.md, section X; optionally a short quote>
Files/paths: create/modify: <...>; touch nothing else
Acceptance criteria:
- <...>
Check command: <command, output trimmed>
Report: changed files; commands + result, one line each; open points.
Do not return full logs or file contents. Do not commit.
```

## Token-saving rules

- Do not split tasks too finely: prefer 2-3 sensible packages over many mini-assignments, since every agent starts cold.
- Agents do not explore freely; they get paths and the spec location.
- Read only the relevant architecture file (index: `docs/architecture/README.md`), never all of them.
- Opus reviews via `git diff --stat`, a targeted diff and the test result, not by re-reading whole files.
- Always filter/trim command output (`| tail -n 30`, errors only). Long logs are read and summarized by `scout`.
- For corrections, continue the agent via `SendMessage` instead of starting a new one; the context is preserved.
- One work package per thread; new threads instead of long histories.
- Since phase 0: quiet check scripts (`make check` or similar, errors + summary only), dependency cache in the SessionStart hook, codegen from OpenAPI.

## Issues and milestones

Open work is tracked as GitHub issues in milestones in `phabioo/uzume` and all official plugin repositories ([ADR 0036](adr/0036-issues-milestones-wiki-and-numbering.md)). A milestone is a release with the same title everywhere: "Uzume 21" (Fabio's local tests and the release steps), "Uzume 22" (leftovers and new work packages), and "Later" for issues deliberately deferred (kept, but outside the release milestones until Fabio pulls them back). Overview: <https://github.com/phabioo/uzume/milestones>; across repositories: `is:open user:phabioo milestone:"Uzume 21"` in the GitHub issue search.

| Label | Meaning |
|---|---|
| `emulation`, `randomizer`, `plugins`, `together`, `platforms`, `app`, `install-and-sharing` | Theme |
| `work-package` | One thread, one PR (issue form "Work package") |
| `leftover` | Left open when finished in code |
| `local-test` | Needs Fabio's real hardware, games or networks |
| `triage` | Not yet sorted into a milestone |
| `release` | Release steps |
| `priority:high`, `priority:medium`, `priority:low` | Order within a milestone (exactly one per issue in Uzume 21/22) |

- One thread works one issue; the PR body says "Closes #N".
- Every new issue in a release milestone gets one priority label; work goes high before medium before low (`milestone:"Uzume 21" label:priority:high`). Issues in "Later" need none until they move back.
- To defer an issue, move it to "Later" and say why in a comment; issues without a milestone are untriaged (only #317 and #192 stay without one on purpose).
- Findings become issues in the right milestone, `triage` when unsure. Error reports from "Report on GitHub" ([ADR 0035](adr/0035-error-groups-and-github-issues.md)) arrive without milestone and get triaged.

## Packages and PRs

- One PR per package with green CI, one topic per PR; the PR closes its issue ("Closes #N").
- Every PR description has a `## Changelog` section (`### Added|Changed|Fixed|Removed|Security`, optional `Server:`/`App:` prefix (`Hub:`/`Player:` still accepted), or `none` for CI, docs, test and refactoring-only PRs); the check "Changelog section" requires it ([ADR 0028](adr/0028-release-notes-and-changelogs.md)).
- At every milestone, the PR updates the docs by the rules below; READMEs stay short, detail goes to `docs/`.

### Docs rules (decided by Fabio, 2026-10-23)

- Every document starts with a short overview that fits on one screen: what it is, where we stand, where to go next. Detail follows further down or in a linked document; long lists, history and special cases never come first.
- Each fact lives in exactly one place: open work in GitHub issues and milestones, direction in [roadmap.md](roadmap.md), finished work in [history.md](history.md) and `CHANGELOG.md`, reasons in the [ADRs](adr/README.md), what Uzume does today in [features.md](features.md), how to use it in the wiki (source `wiki/`, mirrored to the GitHub wiki by `.github/workflows/wiki.yml`; developer guides stay in `docs/guides/`), how to develop in [development.md](development.md).
- A finished milestone becomes one row in the roadmap's "Where we stand" table; its record goes to `docs/history.md`. The README status stays one sentence (version and a link to the roadmap). The check `scripts/check-doc-links.py` fails when a README paragraph exceeds 600 characters.
- Update `docs/features.md` and the affected guides when behaviour changes.

## Phase plan

- **Phase 0 – Foundation:** monorepo structure, CLAUDE.md, architecture under `docs/`; CI (Linux, Windows, ARM64 cross-build of the server); SessionStart hook for dependencies.
- **Phase 1 – Protocol and server basics:** OpenAPI `/api/v1`, WSS messages, `protocol_version`, handshake, error codes; server with SQLite schema, admin setup, TLS, info endpoint, pairing, tokens, revoke, library, ROM upload/download.
- **Phase 2 – Playable vertical slice:** app core (server profile, TOFU, pairing, library, ROM cache); `LibretroBackend` with melonDS DS; minimal Qt UI. First local Windows test by Fabio.
- **Phase 3 – Saves:** start/auto/final sync, current checkpoint vs. history, `base_version`, conflict model, pending sync; server saves page, app conflict dialog.
- **Phase 4 – Session sharing and multiview:** presence, signaling, visibility/ACL, WebRTC with software H.264/Opus, PiP/side-by-side, diagnostics; afterwards hardware encoders (testable locally only).
- **Phase 5 – Remainder and polish:** firmware path, users/invites, systems & cores, emulation and controllers pages, SDL3 gamepads, dark/light, remaining server pages, packaging (Windows installer, systemd unit).
- Status: phases 0-5 are done and merged; the PoC is complete. Direction and finished milestones: [roadmap.md](roadmap.md).
- **After the PoC:** work follows the issues in the milestones and the direction in [roadmap.md](roadmap.md).

The mapping of screens to phases is in [design/README.md](design/README.md).

## Rules

- The repository language is English (code, UI, docs, commits, PRs).
- No real ROMs, BIOS or firmware in the repository or in tests; only homebrew ROMs or dummy files.
- melonDS DS is GPL-3.0; this affects the later distribution of the app.
