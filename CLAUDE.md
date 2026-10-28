# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Uzume (formerly FrameBeam) is a self-hosted retro gaming platform (monorepo): a central ROM library and versioned saves, local emulation, and sessions between apps over WebRTC. Motto: **"The server manages. The app emulates. Audio and video flow directly between apps wherever possible."**

## Terminology

- **Uzume server** (`server/`, Go; "the server") and **Uzume app** (`client/`, C++/Qt; "the app"). Code, binaries and env vars use the Uzume names (`uzume-server`, `uzume-sign`, `UZUME_*`); what deliberately keeps the old name is listed in ADR 0030 (amendment of package 2).
- **Session** = a running/shared game session; "Stream" only in diagnostics.

## Working rules

- Repository language is English: code, comments, UI, docs, commit messages and PRs.
- Opus only orchestrates and writes no product code; implementation is done by the Sonnet 5.5 agents in `.claude/agents/`, searching and log reading by the Haiku 5.5 agent `scout` (alias `haiku`).
- Briefs follow the template in `docs/workflow.md` (which also holds the token-saving rules, phase plan and cloud limits).
- Opus verifies via `git diff --stat`, a targeted diff and the test result; commit/PR by Opus. One work package per thread, one topic per PR.

## Hard rules

- No ROMs, BIOS or firmware in the repo or in tests; only homebrew ROMs or dummy files.
- Never overwrite saves silently, never send them to another server.
- Never put secrets in files, profiles or logs.

## Docs rules

Every document starts with a short overview that fits on one screen; each fact lives in exactly one place (open work in GitHub issues and milestones, direction in `docs/roadmap.md`, finished work in `docs/history.md` and `CHANGELOG.md`, reasons in ADRs, behaviour in `docs/features.md`, usage in the wiki (source `wiki/`, mirrored to the GitHub wiki), developer guides in `docs/guides/`, development in `docs/development.md`). The README status stays one sentence, `scripts/check-doc-links.py` enforces its paragraph length. Details: `docs/workflow.md`, section "Docs rules".

## Status and commands

Details, flags, CI, versioning and packaging: `docs/development.md`, developer guides in `docs/guides/`, user guides in `wiki/` (mirrored to the GitHub wiki by `.github/workflows/wiki.yml`). Server flags and env: `wiki/Server-configuration.md` (runtime variables are `UZUME_*`; only these names are read). App environment: `wiki/Using-the-app.md`.

- `make check` (server and app, quiet), `make check-hub`, `make check-hub-race` (needs CGO/gcc), `make check-client` (preset via `CLIENT_PRESET`, default `linux-debug`; filter with `UZUME_CTEST_REGEX`), `make build-hub` (`HUB_VERSION`/`HUB_CHANNEL`/`HUB_COMMIT`), `make package-hub-deb HUB_VERSION=...`, `make generate` (oapi-codegen), `make notices` (regenerates `server/THIRD-PARTY-NOTICES.txt`; `make check-hub` fails when it is stale after Go dependency changes).
- Dependency scripts (pins in `scripts/*.pin`, idempotent, prefix under `$HOME/.cache/uzume/`): `scripts/bootstrap-vcpkg.sh` (prerequisite of the app build), `make fetch-core` (melonDS DS), `make fetch-desmume`, `make fetch-deps` (libdatachannel, mandatory for the app), `make fetch-sdl3`. Without a real core the real-core tests skip. Merge CI sets `UZUME_SKIP_CORES=1` and `UZUME_REQUIRE_TESTCORE=1` (the `*_testcore` tests use the in-repo test core `uzume_test_core`); `UZUME_REQUIRE_CORES=1` (nightly only) makes a failed fetch or a skipped real-core test fail. Qt >= 6.4 is not vcpkg (Linux apt packages: `scripts/qt-packages.txt`).
- E2E: `scripts/e2e-player-hub.sh`, `scripts/e2e-session.sh`, `scripts/e2e-hub-update.sh` (Linux CI job).
- CI and releases (`.github/workflows/`): `ci.yml`, `release.yml` (beta prerelease of each `main` build), `promote.yml` (manual, stable), `pr-changelog.yml` (required check "Changelog section": every PR needs a `## Changelog` section or `none`, ADR 0028), `plugin-index.yml`, `cores-nightly.yml` (never required). Cores come from the libretro buildbot (ADR 0020), not Uzume. GitHub secret `FRAMEBEAM_SIGNING_KEY` (workflows pass it as `UZUME_SIGNING_KEY`); tool `server/cmd/uzume-sign`.
- Versions (ADR 0016): `VERSION` is only the floor; CI uses max(`VERSION`, highest tag `vX.Y.*` + 1). Only a milestone PR that starts a new minor line raises `VERSION` (to `X.(Y+1).0`). A `v*` tag push builds nothing.
- Packaging: `packaging/linux/` (server `.deb`, package `uzume-server`, ADR 0030), `packaging/windows/uzume.wxs` (WiX MSI for app and server, ADR 0021). The Windows package job fails if the dist contains `*_libretro.*` or a `cores` directory.

The SessionStart hook `.claude/hooks/session-start.sh` only prepares cloud sessions (vcpkg, Go modules, Qt apt packages).

## Pointers

- Architecture: `docs/architecture/README.md` (index, read only the file you need); deviations: `docs/adr/`. Area rules: `server/`, `client/`, `protocol/` each have a `CLAUDE.md`.
- Design (screens, tokens): `docs/design/README.md`; `docs/design/uzume/` is the valid design from 0.18 on.
- Open work: GitHub issues and milestones (`docs/workflow.md`, "Issues and milestones"); direction: `docs/roadmap.md`; finished milestones are moved to `docs/history.md` (see Docs rules). Docs index: `docs/README.md`; READMEs stay short, detail belongs in `docs/`.
