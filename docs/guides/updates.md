# Updates and releases

At a glance: How the server and the Windows app update themselves, update channels, promoting a beta to a release and rollback. Format and mechanics: [update-index.md](../reference/update-index.md), [server-internals.md](../reference/server-internals.md).

Contents: [Server](#server), [App (Windows MSI install)](#app-windows-msi-install), [Channels](#channels), [Promote a beta to a release](#promote-a-beta-to-a-release), [Rollback](#rollback).

How the server and the Windows app update themselves, and how a beta is promoted to a release. Decisions: [ADR 0011](../adr/0011-automatic-updates.md). Mechanics (index format, install path, root helper): [server-internals.md](../reference/server-internals.md), [app-internals.md](../reference/app-internals.md), [update-index.md](../reference/update-index.md).

## Server

- Checks the signed update index at startup, hourly on the beta channel and daily on stable. Admin Settings, section "Updates": channel and automatic install.
- Beta installs automatically (not during an active Session); stable only on request.
- The install runs through a root helper (systemd path unit) that re-verifies the signature.
- Before a schema migration the server backs up its database to `<data>/backups/` (newest 5).
- Needs internet access to github.com. Only the Debian package can update itself; other installs (including `install-server.sh`) only show the available version.

### Server on Windows

- The unprivileged service `UzumeServer` only downloads and stages the `msi` and writes a request file to `<data>\update-request`. It never installs.
- The service `UzumeServerUpdater` (LocalSystem) runs `uzume-server update watch` (polls every 5 s), re-verifies the signed index, copies the MSI to a private directory (`%ProgramData%\Uzume\ServerUpdater`) and runs `msiexec /i <copy> /qn /norestart /l*v <log>`. The log is copied to `<data>\updates\msiexec.log`.
- The updater passes `ALLUSERS=1` (per-machine product). "Packaged" means the marker file `uzume-server.msi-installed` next to the executable, which the MSI installs together with the services and ACLs ([packaging.md](packaging.md)); the standalone `.exe` cannot update itself and only shows the available version.
- The MSI stops `UzumeServerUpdater` during an upgrade, so the updater cannot await its own msiexec run or copy the log; check the server version after the restart. Details: [ADR 0021](../adr/0021-one-windows-installer.md).

## App (Windows MSI install)

- Same check (10 s after start, then hourly on beta, daily on stable). Settings, section "Updates": channel and automatic install.
- Stable shows a banner and installs after "Install and restart". Beta downloads in the background and applies at the next start, never during a game.
- The app installs the index kind `msi`. It updates itself when `uzume-app.msi-installed` is in its folder; the portable zip and dev builds only show availability. The MSI is unsigned (SmartScreen warning).
- Apply: the app copies its launcher to a temp folder and starts it with `--apply-msi-update <msi> <scope>`. The copy waits until no app runs, runs `msiexec /i <msi> /qn /norestart` with `MSIINSTALLPERUSER=1 ALLUSERS=2` (per user) or `ALLUSERS=1` (per machine, elevated, plus `INSTALL_HUB=1` when the server is installed), then starts the installed app.
- The Windows app is a windowless (GUI-subsystem) app; no console opens on start.

## Channels

- Since 0.14 the app segment reads `Off | Stable | Beta`; Off is stored as an explicit choice like Stable or Beta, so it wins over `update_channel_default`, and the app then performs no update checks. Otherwise `stable` or `beta` (hint in the UI: "Pre-release builds from every change on main. May contain bugs."), switchable in the same Settings sections. `beta` also takes stable releases when newer. Never a downgrade.
- Release builds are compiled with channel `beta`. With no channel selected, the first successful index load stores `update_channel_default` (server: `settings` table; app: `player.json`): `stable` if the index lists the running version as stable, else `beta`. It is never re-resolved, so a beta install stays on beta after its version is promoted, and a fresh install of a stable release follows stable. An explicit selection always wins; `dev` builds do not check until a channel is chosen. Automatic install defaults to on for beta, off for stable.
- Beta naming ([ADR 0016](../adr/0016-release-numbering-and-promotion.md)): every push to `main` is a beta with a plain version `X.Y.Z`, published as prerelease `vX.Y.Z`; the number can have gaps. Older `vX.Y.Z-beta.N` builds still update normally but cannot be promoted. Stored `test` settings are read as `beta`. Installs of the former `test.N` builds do not update to beta builds (different channel, and `beta` < `test` in SemVer); reinstall once from a beta or release.

## Promote a beta to a release

GitHub Actions > "Promote to release" > Run workflow (`.github/workflows/promote.yml`). Optional inputs: `version` (default: newest beta prerelease) and `highlights` (free Markdown text placed at the top of the release notes).

The workflow does not rebuild and creates no tag or `VERSION` PR. It adds the beta's artifacts to the signed `updates-index` under channel `stable` (from the `index-hub.json` / `index-player.json` assets on the beta release) and turns the GitHub release from prerelease into the latest stable release. Stable numbers therefore skip (0.7.1, 0.8.3, 0.8.6). Betas built as `vX.Y.Z-beta.N` cannot be promoted; `v0.7.1` is the last stable made the old way. Promoted releases are never pruned.

Release notes ([ADR 0028](../adr/0028-release-notes-and-changelogs.md)): the stable release text is "Changes since <previous stable>", built from the `## Changelog` sections of all PRs between the previous stable release and the promoted beta (all betas in between, deduplicated), below the optional highlights. The same range, filtered per product, replaces the `notes` field of the stable index entries. The workflow also opens the PR "docs: changelog for X.Y.Z" (branch `changelog/vX.Y.Z`, auto-merge) that adds the section to `CHANGELOG.md`; it dispatches CI and the Changelog check on that branch because `GITHUB_TOKEN` events start no workflows. Merging that PR builds one more beta with the text "No user-visible changes." If the repository setting "Allow GitHub Actions to create and approve pull requests" is off, the PR is not created, the run summary shows a warning and the section is added to `CHANGELOG.md` by hand. A failure there never undoes the promotion.

## Rollback

"Go back" installs one of the last 5 earlier releases of the same product from the signed index ([ADR 0029](../adr/0029-operations-and-safety.md) D4, which finishes ADR 0011 D8). The list holds releases of the channel the install follows (a beta install also sees stable releases) plus the versions this device actually ran, which a small local history remembers with the first and last day they ran. A version is never offered when it is not older than the running one, when it does not speak the current `protocol_version` (the server and the apps would not understand each other), or when it has no installer for this platform. Everything is verified like an update: the signature of the index, its freshness (`expires_at`: an old signed installer cannot come back through a stale index), size and SHA-256 of the file, and that the target is strictly older.

After going back, automatic updates are paused. The version you left is listed as "skipped · available" and is never installed automatically; "Update to X" installs it again, "Resume updates" turns automatic updates back on as they were (the skipped version stays skipped until you update to it or to something newer). The pause ends by itself once the running version has reached the version you went back from. The state is stored where an older program also reads it (the server's `settings` table, the app's `player.json`: `update_auto` / `update_auto_install` is set to off), so the older version does not update itself right back.

### App (Windows installer)

Settings → Updates → EARLIER VERSIONS lists the releases with "Go back…". The dialog says what happens: the app closes, installs the older version and starts again (about 30 s); settings are kept (newer keys are ignored and come back after the update); saves and states are on the server and are not touched; plugins that need a newer app (manifest field `min_player`) are turned off and listed ("X needs 0.16.4 and is turned off until you update."), they turn on again after the update; updates are paused. Never while a game or Session runs: the button is muted and the dialog says so. The installer is downloaded from the index, verified, and run by the same launcher copy as an update. For that the app MSI is authored with `AllowDowngrades` (`packaging/windows/uzume.wxs`): an MSI built without it refuses to install over a newer version, so the index marks the releases built with it (`msi_downgrade`) and only those are offered. Releases from before this change cannot be reached from the list; install them by hand from the release page.

A portable, Linux or development build lists the same releases with a "View release" link only.

### Server

Server › Settings › Updates › "Earlier versions" (loaded on request), the CLI and the admin API do the same:

```
uzume-server rollback --list [--json]          the earlier versions and what going back does to the data
uzume-server rollback <version> [--yes] [--confirm-data-loss]
uzume-server rollback --resume                 resume automatic updates
GET  /api/v1/admin/updates/versions             the list (OpenAPI 1.26.0)
POST /api/v1/admin/updates/rollback {"version": "...", "confirm_data_loss": false}
POST /api/v1/admin/updates/resume
```

Migrations are forward-only, so what happens to the data depends on the database schema the target version uses (`schema_version` in the index entry, from `uzume-server version --json`; an entry without it is "unknown"):

| Target | What happens |
| --- | --- |
| Knows the current schema | The database is kept as it is. |
| Needs an older schema, and the automatic pre-update backup of exactly that step exists (`backups/hub-<from>-to-<to>-*.db`, [ADR 0011](../adr/0011-automatic-updates.md) D6) | The database is replaced by that backup. The dialog states "changes since <time> are lost; kept in the safety backup" and needs an explicit confirmation (`--confirm-data-loss`, the checkbox, `confirm_data_loss`). |
| Needs an older schema and no such backup exists, or the schema is unknown | Refused with the reason. Nothing is dropped silently. |

Before anything changes, a safety backup without ROMs is written (the same archive as [Backup and restore](server-backup.md), marked `-safety`) into the backup folder, or `<data>/backups` when none is set. The older package is staged and handed to the root helper, which re-verifies everything (signature, freshness, protocol, that it is older, the database backup and its schema) and trusts nothing in the data folder; for a database restore it stops the service, moves the live database aside (`uzume.db.rollback-<time>`), puts the backup in place with the pause written into it, installs the package and starts the service again. If the installer fails, the files are put back. On Windows the same runs through the update service and the MSI.

In a container the server never goes back by itself: the Updates page shows how to pull the older image tag (`docker compose pull` with the tag, then recreate), and `rollback --list` prints the same. Server › Overview shows the banner "Running X · automatic updates paused" until updates are resumed.

Manual fallback (also for a server that cannot go back by itself): `sudo apt install --allow-downgrades ./uzume-server_<old>.deb`, and if the schema changed, restore the matching database backup from `<data>/backups/` while the server is stopped. The app is reinstalled from an older installer on the release page.
