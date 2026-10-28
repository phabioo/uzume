# ADR 0011: Automatic updates (0.3)

- Status: accepted (Fabio, 2026-10-07, with the merge of the PR); Player Inno update path superseded in part by [ADR 0021](0021-one-windows-installer.md); version numbering and promotion superseded by [ADR 0016](0016-release-numbering-and-promotion.md) (see Amendment 2026-10-08)
- Date: 2026-10-07
- Decided by: Fabio (proposal by the orchestrator; accepted with the PR)
- Amended 2026-10-08: version numbering, promotion and the default channel are superseded by [ADR 0016](0016-release-numbering-and-promotion.md); D1, D3 and the channel default in D5 below are the original decisions.

## Context

Milestone 0.3 ([Roadmap](../roadmap.md)) lets a change on `main` reach the test devices (Windows Player, Hub on a Raspberry Pi) without manual work. Channels were decided on 2026-10-06: the test channel updates automatically, the stable channel only after confirmation. The signing key and tooling come from [ADR 0010](0010-cores-from-the-hub.md) D2. The roadmap left the install layout, the privilege model of the Hub updater and rollback open (`docs/architecture/08-repo-and-open-points.md`). Scope: Windows Player and Linux Hub (amd64/arm64) only.

## Decisions

### D1 Versions and channels

- Root file `VERSION` holds the next product version `X.Y.Z` (start `0.3.0`); Hub and Player share it.
- CI computes version and compiled-in default channel:
  - tag `vX.Y.Z` (must equal `VERSION`, else CI fails): `X.Y.Z`, channel `stable`.
  - push to `main`: `X.Y.Z-test.<CI run number>`, channel `test`.
  - anything else (PRs, branches, local builds): `X.Y.Z-dev.<run number>` (local `X.Y.Z-dev`), channel `dev`.
- Debian version replaces `-` with `~` (`0.3.0~test.57`), so test builds sort before the release.
- SemVer 2.0 precedence, build metadata ignored. A non-SemVer running version (Hub `dev`) disables the updater.
- Channel `test` considers test and stable releases (highest wins); `stable` only stable. Channel `dev` does not check unless a channel is selected explicitly; a selected channel overrides the compiled default.
- `framebeam-hub version --json` and `framebeam_player --version-json` print `{product, version, channel, commit, protocol_version, min_protocol_version}`.

### D2 Signed update index

- Release `updates-index` holds `updates-index.json` and `updates-index.json.sig`. Default URL `https://github.com/phabioo/framebeam/releases/download/updates-index/updates-index.json`.
- Signature, key and compiled-in trust list are those of the core index (ADR 0010 D2): Ed25519 over the exact bytes, one line `ed25519 <key_id> <base64>`. Extra trust keys keep the existing flags and env (now trust keys for cores and updates).
- Schema 1 contract: [`docs/reference/update-index.md`](../reference/update-index.md). Unknown fields are ignored, an unknown schema is an error, invalid releases are skipped and reported, duplicate (product, channel, version) rejects the index.
- `framebeam-sign release-add` maintains the index (newest 5 releases per product and channel); `sign` accepts both index kinds.
- Selection (both consumers): own product, channel per D1, artifact for own platform and kind, version strictly greater than the running one, protocol-compatible (D4). Highest wins. Never an automatic downgrade.

### D3 GitHub releases and pipeline

- `.github/workflows/release.yml` runs on `workflow_run` of a successful CI push on `main` or a `v*` tag and reuses that run's artifacts (no rebuild).
- Stable: release `vX.Y.Z`, marked latest. Test: prerelease `v<X.Y.Z-test.N>` per main build, not latest; the workflow deletes test prereleases and tags (`v*-test.*` only) beyond the newest 5.
- Assets: Hub `.deb` and raw binary per architecture, Player setup exe and portable zip, `SHA256SUMS`.
- The workflow adds the releases to the index, signs, verifies against the compiled-in key and uploads (concurrency group `updates-release`). The secret `FRAMEBEAM_SIGNING_KEY` (environment `release`, main only) exists only in the signing step.
- A stable release needs a deliberate tag push by Fabio; that is the confirmation on the publishing side.

### D4 Protocol compatibility

- Each release carries `protocol_version` and `min_protocol_version`; `protocol_version` itself stays 1.
- Player: a candidate is compatible with the current Hub if `candidate.protocol_version >= hub.min_protocol_version` and `hub.protocol_version >= candidate.min_protocol_version` (Hub values from the last handshake; no known Hub counts as compatible). Incompatible candidates are shown ("needs a newer Hub") and never installed automatically.
- Hub: a candidate is "breaking for Players" if its `min_protocol_version` exceeds the `protocol_version` of any device seen in the last 30 days. Shown as a warning; automatic install skips it; manual install remains possible after confirmation.

### D5 Hub as .deb and Hub updater

- The Hub ships as a `.deb` (amd64/arm64): `/usr/bin/framebeam-hub`, the service unit, a path unit `framebeam-hub-update.path` (`PathExists=/run/framebeam/update-request`) and a oneshot root unit `framebeam-hub-update.service` running `framebeam-hub update apply-staged`. The service keeps its hardening (`ProtectSystem=strict`) and gets `RuntimeDirectory=framebeam`.
- postinst creates user/group `framebeam`, the data dir `/var/lib/framebeam` (0750) and `/etc/framebeam/hub.env` only if missing (never overwritten), and migrates a script install: a unit in `/etc/systemd/system` that references `/usr/local/bin/framebeam-hub` moves to `/etc/framebeam/framebeam-hub.service.pre-deb`, that binary is removed, drop-ins in `framebeam-hub.service.d/` are kept. Then daemon-reload, enable and (re)start both units. Purge removes `/etc/framebeam`; the package never deletes the data dir.
- `packaging/linux/install-hub.sh` stays for non-Debian systems.
- Settings (Hub DB, admin only, Settings page, section "Updates"): channel (default compiled channel; `dev` builds default to off) and automatic install (default on for test, off for stable). Flags/env: `--update-index-url`, `--update-request-dir` (default `/run/framebeam`).
- Check at startup (background, never blocks), every 1 h on test, 24 h on stable, and via "Check now".
- Stage (as the service user): download the `.deb` for `linux-<GOARCH>`, verify size and SHA-256 against the signed index, write it to `<data>/updates/staged/` together with the exact index and signature bytes and `staged.json`, then create the request file. Without a packaged install (request dir not writable or executable not `/usr/bin/framebeam-hub`) the page shows the version, link and manual command only.
- Automatic install (test default) stages after a check but not while a Session is active (retry next check) and not for breaking candidates.
- Security argument: the data dir is writable by the service user, so a compromised Hub process can place any file there. The root helper therefore trusts nothing in it. It deletes the request file first, re-verifies the staged index signature against root-owned keys (compiled-in plus `/etc/framebeam/hub.env`), looks up the release and `.deb` for the staged version and its own architecture, requires a version strictly greater than its own (no replay of old signed releases), copies the `.deb` into a root-owned private temp dir, verifies size and SHA-256 of the copy (no swap after check), and only then runs `dpkg -i`. The result goes to `<data>/updates/last-result.json`. The worst a compromised Hub can do is trigger the install of an already signed, newer FrameBeam release.
- CLI: `framebeam-hub update check` (selection as JSON), `framebeam-hub update stage`.

### D6 Database backup before migrations

- Before pending schema migrations on an existing database: `VACUUM INTO <data>/backups/hub-<from>-to-<to>-<UTC timestamp>.db`, newest 5 kept. A failed backup aborts startup with a clear error.

### D7 Player install layout and updater

- Windows install: `{app}\framebeam_player.exe` is a small launcher (no Qt, static CRT) that starts `{app}\bin\framebeam_player.exe` with the same arguments and std handles, waits and returns its exit code. DLLs, Qt plugins and qml live in `bin\`. Notice file and uninstaller stay at the top. Data stays `{app}\data` (ADR 0004); if the executable's directory is `bin` and its parent holds `framebeam_player.exe`, the install root is the parent. Both executables and the shortcuts set AppUserModelID `FrameBeam.Player`.
- The installer upgrades a flat install in place (explicit list of old DLLs and plugin/qml directories removed, never `data\`; same AppId). Parameter `/UPDATE` waits up to 60 s for the mutex `FrameBeamPlayer` to be free and relaunches the Player after a silent install.
- Settings (portable settings file; Settings screen, "Updates"): channel (default compiled; `dev` = off) and automatic install (default on for test).
- Check 10 s after start, then every 1 h (test) or 24 h (stable); signature verified with OpenSSL Ed25519 against the compiled-in keys plus env `FRAMEBEAM_PLAYER_TRUST_KEYS` (tests). The installer is downloaded to `<data>/cache/updates/<version>/` and verified for size and SHA-256.
- Apply only for an installed Player (Windows, `unins000.exe` in the install root): `<setup> /SILENT /SUPPRESSMSGBOXES /NORESTART /UPDATE` with `/ALLUSERS` (UAC) when the install root is not writable, else `/CURRENTUSER`; then the Player quits.
  - stable: banner with "Install and restart", install only after that confirmation.
  - test with automatic install: download in the background, apply at the next start before the main window; banner "Restart to update" meanwhile; never during a running game.
  - zip, dev build, Linux: availability and release link only.
- The Player-side signature check of core packages (ADR 0010 D5) is not part of 0.3 and moves to 0.4 (Sessions over the internet and save comfort, [Roadmap](../roadmap.md); numbered 0.6 until 2026-10-07). The Player's own update check does verify the signature (above).

### D8 Rollback (decided: [ADR 0029](0029-operations-and-safety.md) D4)

- 0.3 shipped without rollback: the index keeps the last 5 releases per product and channel, GitHub keeps the assets, the Hub keeps the last 5 DB backups. Manual procedure: Hub `sudo apt install --allow-downgrades ./framebeam-hub_<old>.deb`, plus restoring the matching backup if the schema changed (migrations are forward-only); Player: run an older installer.
- Decided in 0.17: "Go back" for Player and Hub from the signed index, the Hub with its pre-update database backup; details in ADR 0029 D4 and [the guide](https://github.com/phabioo/uzume/wiki/Updates#rollback). The manual procedure stays as the fallback.

## Rejected

- **Hub replacing its own binary:** needs write access to `/usr/bin`, conflicts with `ProtectSystem=strict` and with package ownership of the file.
- **polkit/sudoers rule for the Hub user:** a broader privilege than the path unit, which can only run one fixed, self-verifying command.
- **Player updates via the Hub:** the Hub would mirror installers. Possible later for offline LANs.
- **DLL subfolder via `SetDllDirectory` or delay-load:** Qt is linked at load time; the launcher plus `bin\` layout avoids it.
- **Separate update signing key:** one key and one secret, already trusted (ADR 0010).

## Consequences

- The Hub needs egress to github.com for updates; without it a cached `.deb` is installed manually.
- Test devices receive every `main` build.
- `VERSION` must be bumped after each stable tag, otherwise later test builds sort below the release. (Superseded by [ADR 0016](0016-release-numbering-and-promotion.md): `VERSION` is only a floor, CI takes max(`VERSION`, highest tag + 1), no bump PR.)
- The Windows installer stays unsigned (SmartScreen warning).
- macOS and Linux Player and the Windows Hub follow later (0.9, 0.10; old numbering, see the numbering note below. Windows Hub: 0.9 with the one installer, ADR 0021; Linux Player: after 1.0; macOS dropped on 2026-10-11).
- Open: rollback (D8), Player-side core signature check (0.4).

## Amendment 2026-10-07

Decided by Fabio after 0.3 merged. The decisions above stay as accepted; where they say "test" for the channel, versions, prereleases or tags, read "beta" from this date.

- The pre-release channel is renamed from `test` to `beta`: channel id `beta`, versions `X.Y.Z-beta.<CI run number>`, prereleases tagged `vX.Y.Z-beta.N`, titled "beta build". Debian version `0.3.0~beta.57`.
- Users can still pick Beta in Hub and Player settings, hint: "Pre-release builds from every change on main. May contain bugs." Stable builds default to stable, beta builds to beta. Stored `test` settings are read as `beta`.
- Existing `test.N` installs do not update to beta builds (different channel, and `beta` < `test` in SemVer precedence). They must be reinstalled once from a beta or stable release.
- New workflow `.github/workflows/promote.yml` ("Promote to stable", one button in GitHub Actions). Inputs: `beta_version` (default newest beta), `next_version` (default next minor). It tags `vX.Y.Z` on the commit of that beta build, dispatches CI for the tag (stable build, release, signed index) and opens a PR bumping `VERSION` to the next version; if Actions may not create PRs it warns with a compare link. Pushing the tag manually still works. Update 2026-10-08: the workflow also dispatches CI on the bump branch (GITHUB_TOKEN PRs start no workflows) and enables auto-merge so the PR passes the required checks of the `main` ruleset.
- The Windows Player is a windowless (GUI-subsystem) app; no console opens on start.

## Update 2026-10-08 (milestone numbering)

Milestone numbers above 0.4 in this ADR use the numbering from before the roadmap renumbering of 2026-10-07: old 0.5/0.6/0.7/0.8/0.9/0.10 are now 0.6 (Player UI pass) / 0.7 (Hub UI pass) / 0.8 (3DS) / 0.9 (metadata) / 0.10 (Hub for Windows) / 0.11 (Linux and macOS Player); 0.5 is now OpenGL hardware rendering. See [roadmap.md](../roadmap.md). Later renumbering (2026-10-09 and 2026-10-11): Hub for Windows is 0.9 (one installer), 3DS is 0.10, plugins 0.11, metadata 0.12, Linux and macOS Player 0.13.

## Amendment 2026-10-10 (forward compatibility, legacy companion)

Hubs <= 0.8.0 rejected a whole release when one artifact had an unknown platform or kind, so they skipped every release since 0.8.1 (which added `windows-amd64`) and reported "up to date". Consumers now drop such artifacts and keep the release; `framebeam-sign release-add` stays strict. For the old Hubs the signer adds a legacy companion entry `X.Y.Z-legacy` (only `linux-amd64`/`linux-arm64` `deb`/`binary`) per hub channel for the newest release; see [update-index.md](../reference/update-index.md).
