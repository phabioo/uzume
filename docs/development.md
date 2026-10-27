# Development

Build, test, dependencies, CI and versioning. Quick start: `scripts/bootstrap-vcpkg.sh` once, then `make check`. Sections below: [prerequisites](#prerequisites), [make targets](#make-targets), [dependency scripts](#dependency-scripts), [end-to-end tests](#end-to-end-and-ui-tests), [CI and releases](#ci-and-releases), [versions](#versions-and-channels), [documentation checks](#documentation-checks). Agent workflow and briefs: [workflow.md](workflow.md). Agent-facing summary of commands: root `CLAUDE.md`.

## Prerequisites

- **Server:** Go >= 1.25 (per `server/go.mod`; CI uses the pinned toolchain go1.26.10).
- **App:** CMake >= 3.25, Ninja, a C++20 compiler and Qt >= 6.4 (not via vcpkg). Linux: apt (package list `scripts/qt-packages.txt`, shared by CI and `.claude/hooks/session-start.sh`). Windows: Qt 6.8 LTS via install-qt-action with the modules `qtmultimedia` and `qtwebsockets`.
- **Sessions** additionally need libdatachannel, FFmpeg and Opus (and OpenSSL for the update signature check): on Linux the media apt packages from the same list plus `make fetch-deps`; on Windows via vcpkg (`client/vcpkg.json`).
- **Gamepads** need SDL3 >= 3.2: on Linux `make fetch-sdl3`; on Windows via vcpkg.
- vcpkg (pinned) is bootstrapped once with `scripts/bootstrap-vcpkg.sh` into `$HOME/.cache/uzume/vcpkg`; it is a prerequisite for the app build. The only pin is `builtin-baseline` in `client/vcpkg.json` (the script reads it from there; `scripts/check-vcpkg-pin.sh`, part of `make check-hub`, rejects a second copy). Baseline bump (monthly, Dependabot ecosystem `vcpkg` proposes it): use a vcpkg release tag commit, change only `builtin-baseline`, and let the Windows CI run test it (the MSI ships the vcpkg DLLs); `scripts/check-vcpkg-pin.sh --online` (weekly, workflow `dependencies.yml`) warns when a newer release tag exists. The Linux pins `scripts/libdatachannel.pin` and `scripts/sdl3.pin` are separate and are bumped by hand together with the baseline.

## Make targets

```sh
make check            # server and app checks, quiet (errors + summary only)
make check-hub        # server only: gofmt, vet, staticcheck, tests, codegen freshness, server third-party notices, packaging checks
make check-hub-race   # server tests under the race detector (CGO_ENABLED=1, needs gcc); the Linux CI job runs it after check-hub
make check-client     # app only (preset via CLIENT_PRESET, default linux-debug)
make build-hub        # server/dist/uzume-server-linux-{amd64,arm64} and uzume-server-windows-amd64.exe (HUB_VERSION, HUB_CHANNEL, HUB_COMMIT)
make package-hub-deb HUB_VERSION=...   # server .deb for amd64/arm64 in server/dist/ (after build-hub)
make package-app-appimage APP_VERSION=...   # app AppImage (x86-64) in client/build/appimage/ (after a linux-release client build and build-hub)
make generate         # regenerate Go code from OpenAPI (oapi-codegen)
make notices          # regenerate server/THIRD-PARTY-NOTICES.txt (needed after Go dependency bumps; make check-hub fails when stale)
make fetch-core       # download melonDS DS from the libretro buildbot (nightly, CRC32-checked) and print the .so path
make fetch-desmume    # download DeSmuME from the libretro buildbot (nightly, CRC32-checked) and print the .so path
make fetch-deps       # build libdatachannel (pinned) and print the prefix
make fetch-sdl3       # build SDL3 (pinned) and print the prefix
```

`make check-client` runs the dependency scripts itself and passes their results to CMake (`-DUZUME_MELONDS_DS_CORE=...`, `-DCMAKE_PREFIX_PATH=...`, `-DSDL3_ROOT=...`); `CMAKE_BUILD_PARALLEL_LEVEL` defaults to 3. Without a core, tests with `NEEDS_CORE` are skipped. Tests run headless (`QT_QPA_PLATFORM=offscreen`) against a fake server and a homebrew test ROM generated at build time. Run the app after a build: `client/build/linux-debug/app/uzume [--data-dir <path>] [--dev-allow-http]`.

## Dependency scripts

| Script | What it does |
|---|---|
| `scripts/bootstrap-vcpkg.sh` | vcpkg (pinned) into `$HOME/.cache/uzume/vcpkg` |
| `scripts/fetch-buildbot-core.sh <core>` / `.ps1` | Downloads a core from the libretro buildbot nightly (`UZUME_BUILDBOT_URL` overrides the base URL), checks the CRC32 against `.index-extended`, extracts one library into `$HOME/.cache/uzume/cores/buildbot/`, prints the path. Not pinned on purpose: CI tests against the current nightly ([ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md) D10). Cores used: `melondsds`, `desmume`. `UZUME_REQUIRE_CORES=1` (nightly) makes a failed fetch or a skipped real-core test fail the check; `UZUME_SKIP_CORES=1` (merge CI) fetches no real core, `UZUME_REQUIRE_TESTCORE=1` fails the check when a `*_testcore`/`emulation_test_core*` test skips, `UZUME_CTEST_REGEX` limits `make check-client` to matching tests |

| `scripts/fetch-libdatachannel.sh` | Builds libdatachannel (pin: `scripts/libdatachannel.pin`, media on, no own WebSocket) into `$HOME/.cache/uzume/deps/libdatachannel/`, idempotent, prints the prefix (mandatory for the app build) |
| `scripts/fetch-sdl3.sh` | Builds SDL3 (pin: `scripts/sdl3.pin`, gamepad only, no video/audio) into `$HOME/.cache/uzume/deps/sdl3/`, idempotent, prints the prefix. Windows: vcpkg port `sdl3` |
| `scripts/check.sh <step>` | The quiet check steps behind the make targets |
| `scripts/check-trusted-keys.sh` | Server and app embed the same release key; part of `make check-hub` |
| `scripts/changelog.py` | `check` (PR body has a valid `## Changelog` section) and `collect` (release notes from the PR sections of a commit range; formats markdown, index, json). Stdlib only, generic. Unit tests in `scripts/tests`, run by `make check-hub` (step `script-tests`) |
| `scripts/check-signing-key.sh` | Signing key from `UZUME_SIGNING_KEY` (the workflows fill it from the GitHub secret `FRAMEBEAM_SIGNING_KEY`, the only name that keeps FrameBeam, ADR 0030) is trusted by the server and signs a throwaway updates index; run by release.yml and promote.yml |

Windows release builds use the `x64-windows-release` overlay triplet (Release-only dependency builds); the debug preset uses the standard `x64-windows` triplet.

## End-to-end and UI tests

- `scripts/e2e-player-hub.sh`: app CLI against a locally built server, including a save round trip and a core round trip (dummy library zipped in the buildbot layout, `import-cores`, `fetch-core`).
- `scripts/e2e-session.sh`: two CLI processes share and watch a Session against a local server; a second round forces the relay against a server with TURN on loopback.
- `scripts/e2e-hub-update.sh`: real install on a systemd host (`install-server.sh`, `.deb` migration, signed update round trip); skips without root/systemd.
- All three run in the Linux CI job.
- Screenshot tests (`ctest -R screenshots`) render all app screens at 1440x900 into the directory named by `UZUME_SCREENSHOT_DIR`.
- Live tests (cloud session): `scripts/live-app.sh up [--apps N]` builds and starts a local server with test users (`lena`, `max`, `sam`) and a dummy game plus N apps under Xvfb, each with the debug MCP server (`--mcp --mcp-port`, `UZUME_MCP_TOKEN`), and writes ports, tokens and passwords to `~/.cache/uzume/live/live.env`; `down` stops it. `scripts/mcp-call.sh <app> <tool> '<json>'` calls one tool (for example `server_sign_in`, `app_screenshot`, see [Claude debugging](guides/app.md#claude-debugging-mcp)).
- OpenGL tests need a GL 3.3 context (Linux CI uses Mesa llvmpipe under xvfb) and are skipped without one (ADR 0013). `UZUME_DISABLE_HW_RENDER=1` forces the cores' software path.
- Randomizer tests (0.21) never run the real engine or a real ROM: `uzume_test_engine` (`client/testutil/test_engine.cpp`, built next to the core tests) copies the command line of the engine (`cli -i <in> -o <out> -S <settings> -z <seed> [-d] [-l]`) and writes deterministic synthetic output (the input XOR a keystream of `SHA-256(settings + seed)`, or a fake LayeredFS folder with `-d`); settings starting with `999` make it fail, `UZUME_TEST_ENGINE_DELAY_MS` delays it for cancel and time-limit tests. Tests that need an engine zip build one around it. The real engine is verified only on Fabio's PC (`variant_files` over the debug MCP compares the hashes of two builds).
- Windows test package: unpack the CI artifact `uzume-app-windows-x64` from the Windows job and start `Uzume.exe` (it ships no core; the server provides it on first game start).

## CI and releases

- `.github/workflows/ci.yml`: Linux on every push, pushes to `main` run it directly; pushes to other branches go through `ci-branch.yml`, which calls `ci.yml` unless the branch has an open PR (the PR run builds that commit; the lookup falls back to building on API errors; the first push that opens a branch still builds because the PR does not exist yet). The branch run's checks are named `CI (branch) / ...`, so they never collide with the required PR check names. Runs on `main` use one concurrency group per commit, so none is cancelled or replaced; other refs cancel older runs. Runners are pinned (`ubuntu-24.04`, `windows-2025-vs2026` with Visual Studio 2026). Windows on PRs against `main`, manually, and on pushes to `main` (primes the Windows vcpkg binary cache after merges, [ADR 0008](adr/0008-windows-ci-cache.md); cache misses still need a cold dependency build). The Windows job builds the layout launcher plus `bin\`, the zip and the MSI and tests silent install and upgrade.
- Every workflow job has `timeout-minutes` (Windows client 300, so a cold vcpkg cache still fits), and the long steps have their own (Windows Configure 240, Build 60, Test 30, MSI steps 15; Linux dependency builds 45-60, `Check Player` 60). The Linux app job builds libdatachannel and SDL3 in explicit steps and saves their caches right after, uses ccache (saved by `main` runs only), and the server job runs a pinned, checksum-verified actionlint through `make check-hub`. The Windows vcpkg binary cache is saved only when a run changed it. The Windows app job compiles through a pinned, checksum-verified ccache (MSVC Release has no `/Zi`, so objects are cacheable; keyed by the MSVC version, saved by `main` runs only). `make check-client` and the Windows job run the app tests in parallel (`ctest -j`, `CTEST_PARALLEL_LEVEL` overrides it, `1` for a serial run). Failed app tests are retried once in CI: `scripts/check.sh` with `UZUME_CTEST_RETRY=1` re-runs the failed tests serially (`ctest --rerun-failed`), the Windows Test step uses its direct rerun of the failed test exe. A test that passes on the retry is reported as a "Flaky test" warning annotation and job summary instead of failing the job, and on `main` the job `flaky-report` adds it to the issue "Flaky tests" (label `flaky-tests`; it is never closed automatically); a test that fails twice still fails the job. Flakes are to be fixed, the retry only keeps `main` and PRs mergeable. The published beta MSI keeps `high` compression; dev builds use `mszip` and the two test-only MSIs `none` (`build-msi.ps1 -CompressionLevel`). Dependabot (`.github/dependabot.yml`) proposes weekly updates for the actions and the server Go modules. Third-party actions are pinned to a full commit SHA with a `# vX.Y.Z` comment (Dependabot keeps both current); `actions/*` keep tags except in `plugin-index.yml`, which holds the release key. `dependabot-notices.yml` (pull_request_target, only for same-repo `dependabot/go_modules/` PRs touching `server/go.mod|go.sum`) runs `make notices`, pushes the regenerated `server/THIRD-PARTY-NOTICES.txt` to the PR branch and dispatches `ci.yml` there (a `GITHUB_TOKEN` push starts no run, `workflow_dispatch` does), so Go bump PRs pass `make check-hub`. `plugin-index.yml` (manual "Publish plugin") signs official plugin files and the plugin index with the release key; the key is only in the environment of the signing steps, not of the step that unzips the downloaded plugin.
- Linux apt steps go through `scripts/ci-apt-install.sh` (update limited to 120 s and install to 480 s per attempt, apt retries and network timeouts, 3 attempts, from the second attempt on `archive.ubuntu.com` instead of the runner's Azure mirror) plus a 30 minute step `timeout-minutes`, so a hanging Ubuntu mirror cannot block CI for long.
- Job `hub-windows` (windows-2025-vs2026): `go vet`, `go test` and a version check of the Windows server binary; artifact `uzume-server-windows`. The release adds `uzume-server-<version>-windows-amd64.exe` to the index as platform `windows-amd64`, kind `binary`.
- Job `hub-docker`: builds the server image for amd64 and arm64 and starts it; it never pushes. `release.yml` pushes `ghcr.io/<owner>/uzume-server:X.Y.Z` and `beta`, `promote.yml` re-tags `stable` and `latest` ([ADR 0029](adr/0029-operations-and-safety.md)).
- Changes that touch only `docs/*` or `*.md` files are detected as docs-only on branches and PRs; the server, app and Windows jobs are skipped for them.
- `.github/workflows/release.yml`: after a successful CI run on `main` it publishes the CI artifacts as GitHub prerelease `vX.Y.Z` (all beta prereleases are kept; the updates index lists the newest 5 per product and channel) and adds server and app to the signed `updates-index` release under channel `beta` (`uzume-sign release-add`, GitHub secret `FRAMEBEAM_SIGNING_KEY`, passed to the tool as `UZUME_SIGNING_KEY`). The per-product index entries are stored as `index-hub.json` / `index-player.json` assets on the release. Pushing a `v*` tag builds nothing. The release notes are built by `scripts/changelog.py collect` from the `## Changelog` sections of all PRs since the previous plain `vX.Y.Z` release (a PR without a section: its title under Changed; bot PRs under Dependencies; if collection fails the old fixed text is used and the release is not blocked). A product-filtered short `notes` text (at most 4000 bytes) is added to the server and app index entries.
- `.github/workflows/promote.yml` ("Promote to release", manual; optional inputs `version` and `highlights`): see [guides/updates.md](guides/updates.md). It writes "Changes since <previous stable>" over all betas in between (deduplicated) into the release and the index `notes`, and opens the PR "docs: changelog for X.Y.Z" (branch `changelog/vX.Y.Z`, auto-merge) that adds the section to `CHANGELOG.md`. Events of `GITHUB_TOKEN` start no workflows, so it dispatches `ci.yml` and `pr-changelog.yml` on that branch (`ci.yml` treats `workflow_dispatch` on a non-`main` ref like a branch push, so docs-only skips the builds). Needs the repository setting "Allow GitHub Actions to create and approve pull requests"; otherwise the summary warns and the section is added by hand.
- `.github/workflows/pr-changelog.yml` ("Changelog", job "Changelog section", required check of the `main` ruleset): fails a PR without a valid `## Changelog` section; bot PRs are exempt; `workflow_dispatch` with input `pr` re-checks a PR ([ADR 0028](adr/0028-release-notes-and-changelogs.md)).
- `.github/workflows/plugin-index.yml` ("Publish plugin", manual, environment `release`): signs a plugin's files and the signed plugin index with the release key and publishes them as release `plugin-index`, the server default of `-plugin-index-url`; see [guides/plugin-publishing.md](guides/plugin-publishing.md).
- Release pipeline hardening (amendment 2026-10-19 of [ADR 0016](adr/0016-release-numbering-and-promotion.md); code review package 9):
  - The Linux server job runs `make check-hub-race` (`go test -race ./...`, CGO on) after `make check-hub` (CGO off). `.github/workflows/dependencies.yml` (daily, manual, and PRs/pushes touching the Go modules; never a required check, so a new advisory cannot block an unrelated merge) runs the pinned `govulncheck` over the server and the vcpkg pin check `--online`. govulncheck cannot run in the cloud development container (`vuln.go.dev` is blocked by the proxy); CI is the only place it runs.
  - Index updates are queued, not cancelled: `release.yml`, `promote.yml` and `index-refresh.yml` have one concurrency group per run and wait in `scripts/wait-index-turn.sh` until no older run of the three is active (a shared group keeps only one pending run and would drop the middle one, leaving its tag without prerelease and index entry).
  - Tags: CI still reserves `vX.Y.Z` before building (needed so parallel main runs get different numbers) and `release.yml` still publishes only after a green CI run. Tags of builds that failed stay reserved so a re-run of the failed jobs keeps its number; `release.yml` deletes plain tags without a release whose commit is older than 8 days (the CI artifacts, retention 7 days, are gone by then, so they can never be released).
  - Signed index files: `update-index.sh` and `plugin-index.yml` upload `<index>.json.signed` (one file: a domain-separated signature line, then the exact index bytes, which cannot be read as a mismatched pair) first, then the legacy pair `<index>.json` + `.json.sig` (raw-byte signature) for servers and apps that predate the amendment. Current servers and apps read the bundle and fall back to the pair only while the bundle is not published (HTTP error); a fetched bundle is never replaced by the pair. Signing contexts: `uzume-updates-index-v1`, `uzume-plugin-index-v1`, `uzume-cores-index-v1`; line format `ed25519ctx <key_id> <context> <base64 sig>`, signed bytes `context + "\n" + index`. `uzume-sign sign -bundle`, `verify -bundle`, `index-refresh`.
  - Expiry: updates and plugin indexes carry `expires_at` (`generated_at` + 60 days, set by `release-add`, `plugin-add` and `index-refresh`). Server and app refuse an index after that date (an index without `expires_at`, as published before the amendment, is still accepted during the transition). `index-refresh.yml` (weekly) and `plugin-index.yml` (weekly, scheduled run without inputs) re-sign with new dates so the index stays valid in quiet weeks.
  - Transition and end: for at least two minor lines every index is published in both forms. A later amendment then makes the bundle mandatory (`VerifyContext(..., legacy=false)`, no pair fallback, `expires_at` required) and stops publishing the legacy pair; until then an attacker who can block the bundle download can downgrade a new client to the legacy pair, which has no domain separation (expiry is still checked by the client when the index carries it).
  - Server manual update command (HC-13): the shown `curl -fLO` URL is single-quoted and `validateRelease` rejects spaces, quotes, shell metacharacters and control characters in artifact URLs.
- `.github/workflows/cores.yml` was deleted in 0.8 ([ADR 0020](adr/0020-cores-from-the-libretro-buildbot.md) D10): cores are no longer built or signed by Uzume; the releases `cores-index` and `core-melonds_ds-1.4.0` were deleted on 2026-10-23 (nothing reads them any more). Merge CI downloads no real core: the `*_testcore` tests run against the in-repo test core `uzume_test_core`, the real-core tests skip, and a skipped test-core test fails the check. `.github/workflows/cores-nightly.yml` ("Cores (nightly)", daily 03:30 UTC and manual, never a required check) fetches melonDS DS and DeSmuME from the libretro buildbot (`scripts/fetch-buildbot-core.sh|ps1`, base URL override `UZUME_BUILDBOT_URL`), sets `UZUME_REQUIRE_CORES=1` and runs only the real-core tests on Linux and Windows; a failed scheduled run opens or updates the issue "Nightly core tests failed" (label `nightly-cores`), a passing one closes it. The Windows package job and `scripts/check.sh packaging` fail if a core would end up in the app dist or the server `.deb`. Tooling: `server/cmd/uzume-sign` (keygen / add / sign / verify / pubkey / release-add) now serves the updates index only. Offline server: `uzume-server import-cores <dir>` or `install-server.sh import-cores <dir>`.
- The SessionStart hook `.claude/hooks/session-start.sh` only prepares cloud sessions (vcpkg, Go modules, Qt apt packages); cores are fetched only via `make fetch-core` / `make fetch-desmume`.

## Versions and channels

- The root file `VERSION` (`X.Y.Z`) is shared by server and app and is the floor for the next beta ([ADR 0016](adr/0016-release-numbering-and-promotion.md)). CI computes version and channel once: a push to `main` gives the plain `X.Y.Z` on channel `beta` (the larger of `VERSION` and the highest existing tag `vX.Y.*` plus 1; the version job creates the tag on the commit before building, so a failed or re-run build can leave gaps); anything else `X.Y.Z-dev.<run>`. Main CI runs do not cancel each other.
- They are passed to `make build-hub` (`HUB_VERSION`, `HUB_CHANNEL`, `HUB_COMMIT`) and to the app configure (`UZUME_VERSION`, `UZUME_CHANNEL`, `UZUME_COMMIT`, also read by `make check-client` from the environment).
- Only a milestone PR that starts a new minor line raises `VERSION` (to `X.(Y+1).0`, for example 0.8.0); otherwise nobody touches it. The milestones 0.20 to 0.24 were built in parallel and ship as betas `0.20.N`; no package raises it until one release PR sets 0.21.0, the first stable of that batch ([ADR 0016](adr/0016-release-numbering-and-promotion.md), amendment 2026-10-27). Stable releases are promoted betas and keep the beta number.
- `uzume-server version [--json]` and `uzume --version-json` print `{"product", "version", "channel", "commit", "protocol_version", "min_protocol_version"}`.
- `protocol_version` is separate from product versions ([protocol reference](reference/protocol.md)).

## Server package and installer builds

- `make package-hub-deb`: via `packaging/linux/build-deb.sh --binary PATH --arch amd64|arm64 --version X.Y.Z[-pre] --out DIR` (dpkg-deb only; units and maintainer scripts in `packaging/linux/deb/`). `scripts/check.sh packaging` (part of `make check-hub`) checks control, contents, units, runs shellcheck (the packaging scripts fully, all `scripts/*.sh` and the SessionStart hook at warning level) and a smoke test of `install-server.sh` with `UZUME_INSTALL_ROOT`.
- Windows installer (WiX MSI for app and server, local build `packaging/windows/build-msi.ps1`), built in the Windows CI job: [guides/packaging.md](guides/packaging.md).

## Documentation checks

`scripts/check-doc-links.py [repo-root]` (stdlib only) checks relative links and heading anchors in all Markdown files and exits 1 on broken ones. It also fails when a paragraph or list item of the root `README.md` exceeds 600 characters (docs rules in [workflow.md](workflow.md#docs-rules-decided-by-fabio-2026-10-23)). It is not part of `make check`; CI runs it in the job `Detect docs-only changes`.

## Writing the Changelog section

Every PR description has a `## Changelog` section (template: `.github/pull_request_template.md`). Write one English line per user-visible change under `### Added`, `### Changed`, `### Fixed`, `### Removed` or `### Security` (delete unused headings); start with `Server:` or `App:` when only one product is affected (the older `Hub:` and `Player:` still work). For CI, docs, test or refactoring-only PRs write `none` alone. Example:

```
## Changelog

### Fixed
- Server: Saves are no longer listed twice after an import.
```

Betas and stable releases are built from these sections, so write for users, not for developers. Check a draft locally with `PR_BODY="$(cat body.md)" python3 -I scripts/changelog.py check`.

## Contributions

Contributions require agreeing to the CLA; see [CONTRIBUTING.md](../CONTRIBUTING.md).

## Hard rules

No ROMs, BIOS or firmware in the repository or in tests (homebrew ROMs and dummy files only); never overwrite saves silently or send them to another server; no secrets in files, profiles or logs.
