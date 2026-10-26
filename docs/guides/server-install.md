# Install the server on Linux

At a glance: Install the server on Linux as a systemd service: the Debian package (recommended) or the script `install-server.sh`; then first setup from the app, upgrade, certificate renewal, offline cores and uninstall. Flags: [server-configuration.md](server-configuration.md). Docker instead: [server-docker.md](server-docker.md).

Contents: [Install the Debian package (recommended)](#install-the-debian-package-recommended), [Install with install-server.sh](#install-with-install-serversh), [1. Get the binary](#1-get-the-binary), [2. Install](#2-install), [First setup](#first-setup), [Admin account](#admin-account), [TLS fingerprint](#tls-fingerprint), [Upgrade](#upgrade), [Renew the certificate](#renew-the-certificate), [Import cores offline](#import-cores-offline), [Uninstall](#uninstall), [Files](#files), [Coming from FrameBeam](#coming-from-framebeam), [Migrating an existing manual data dir](#migrating-an-existing-manual-data-dir).

Two ways to run the Uzume server as a systemd service: the Debian package (recommended on Debian / Raspberry Pi OS, the only type that can update itself) or the script `packaging/linux/install-server.sh` (any systemd Linux). For flags and environment variables see [server-configuration.md](server-configuration.md); for apps outside your LAN see [sessions-over-the-internet.md](sessions-over-the-internet.md). Already running Docker? Use the official image instead: [server-docker.md](server-docker.md).

## Install the Debian package (recommended)

Download `uzume-server_<version>_<amd64|arm64>.deb` from the [GitHub releases](https://github.com/phabioo/uzume/releases) (or the CI artifact `uzume-server-linux`) and install it:

```sh
sudo apt install ./uzume-server_<version>_arm64.deb
```

The package is `uzume-server`. It installs `/usr/bin/uzume-server`, the units `uzume-server.service`, `uzume-server-update.path` and
`uzume-server-update.service` (in `/lib/systemd/system`) and is the only install type the server can update itself
(Server › Settings › Updates). Configure flags (port, admin) in `/etc/uzume/server.env`, which an existing install keeps. The package starts the server on port 8443 right away; if that port is taken, create `/etc/uzume/server.env` with `UZUME_LISTEN=:<port>` and `UZUME_DATA_DIR=/var/lib/uzume` before you install (the package never overwrites it), then run `sudo chown root:uzume /etc/uzume/server.env && sudo chmod 0640 /etc/uzume/server.env` and `sudo systemctl restart uzume-server`. Set up the server from the app with the setup code ([First setup](#first-setup)), or create the admin account as described under [Admin account](#admin-account).

Package details:

- Published betas and releases have a plain version (`uzume-server_0.8.3_arm64.deb`). The control Version field uses `~` for other pre-release versions (`0.8.3~dev.57`, legacy `0.3.0~beta.57`), so they sort before the release; file names keep the SemVer because GitHub rewrites special characters in asset names.
- The postinst creates the `uzume` user, `/var/lib/uzume` (only if missing) and `/etc/uzume/server.env` (only
  if missing: an existing file is never overwritten), then enables and (re)starts the server and the update path unit.
- Migration from `install-server.sh`: the old unit in `/etc/systemd/system` is moved to
  `/etc/uzume/uzume-server.service.pre-deb`, `/usr/local/bin/uzume-server` is removed; drop-ins in
  `uzume-server.service.d/`, `server.env` and the data directory are kept.
- Remove keeps config and data; purge removes `/etc/uzume` only. The data directory (database, saves, ROM library)
  and the `uzume` user are never deleted by the package.
- Updates: the server (user `uzume`) stages a verified `.deb` and creates `/run/uzume/update-request`; the path
  unit starts `uzume-server update apply-staged` as root, which verifies the staged package again and runs `dpkg -i`.
- Build (dpkg-deb only): `packaging/linux/build-deb.sh --binary PATH --arch amd64|arm64 --version X.Y.Z[-pre] --out DIR`;
  all of it: `make build-hub package-hub-deb HUB_VERSION=...`. Checked by `make check-hub` (control, contents, units,
  shellcheck of the maintainer scripts in `deb/`).
- `scripts/e2e-hub-update.sh` installs and updates a server on a real systemd host (CI only; skips without root/systemd).

## Install with `install-server.sh`

`install-server.sh` installs the server as a persistent service. Primary target: Raspberry Pi 5 (64-bit Raspberry Pi OS / Debian, aarch64); amd64 works too. It installs a local binary and downloads nothing.

## 1. Get the binary

- CI: download the artifact `uzume-server-linux` from a workflow run of `.github/workflows/ci.yml` (contains `uzume-server-linux-arm64` and `-amd64`).
- Or build: `make build-hub` produces `server/dist/uzume-server-linux-{amd64,arm64}`.

Copy the binary and the files of `packaging/linux/` to the Pi, for example:

```sh
scp uzume-server-linux-arm64 packaging/linux/install-server.sh packaging/linux/uzume-server.service LICENSE server/THIRD-PARTY-NOTICES.txt pi@raspberrypi:
```

## 2. Install

```sh
sudo ./install-server.sh install --binary ./uzume-server-linux-arm64 --name "Living Room Hub" --admin alice
```

> **Port 8443 already in use? Pick another port: `--port 8444`**
>
> ```sh
> sudo ./install-server.sh install --binary ./uzume-server-linux-arm64 --port 8444
> ```
>
> The script warns if the chosen port is busy. Change it later by re-running `install` with `--port N` (only the given options are updated in `server.env`, then restart: `sudo systemctl restart uzume-server`).

Options: `--binary PATH`, `--port N` or `--listen ADDR`, `--data-dir DIR`, `--name NAME` (first start only), `--admin USER`, `--no-start`; see `--help`. If `--binary` is omitted, `uzume-server-linux-<arch>` next to the script or in the current directory is used.

At the end the script prints the service status, the URL (`https://<ip>:<port>/`) and the TLS fingerprint.

## First setup

A server without an admin prints a **setup banner** at every start (address, security code, setup code) to its output; on systemd read it with `journalctl -u uzume-server -n 30`. In Uzume on another PC choose Set up a server › On another device, enter the address and the setup code, compare the security code and create the admin account. Ask for the banner values or a new code any time:

```sh
sudo uzume-server status        # version, address, security code, setup code while the server runs (or: set up)
sudo uzume-server setup-code    # a new setup code, valid 60 minutes, works once
sudo uzume-server code          # the security code of the certificate
```

The commands read `/etc/uzume/server.env` like the service does, so a custom data folder or port is found without extra options. A forgotten admin password: `sudo uzume-server reset-admin` prints a one-time password (nothing else changes). `sudo uzume-server reset` deletes all data and starts fresh (stop the service first: `sudo systemctl stop uzume-server`; it asks for the server name and writes a safety backup without ROMs first). `sudo uzume-server update` installs the newest version of the update channel. Details: [Server commands](server-configuration.md#server-commands). On Windows the same commands run in a terminal as administrator.

## Admin account

`--admin USER` asks for the password twice (or reads one line from stdin when not a terminal, e.g. `echo ... | sudo ./install-server.sh ...`). It runs `uzume-server setup-admin` as the `uzume` user and is skipped if an admin already exists. The setup code is the way without the script; `setup-admin` is the way without the app. Without the script (for example after the `.deb` install) run `setup-admin` as the `uzume` user with the data directory from `server.env`, for example `sudo runuser -u uzume -- env UZUME_DATA_DIR=/var/lib/uzume uzume-server setup-admin -username <name>` (use the `UZUME_DATA_DIR` value from `/etc/uzume/server.env` if you changed it; a plain `sudo uzume-server setup-admin -username <name>` finds it by itself).

## TLS fingerprint

The server creates a self-signed certificate under `<data dir>/tls/` and logs its SHA-256 fingerprint at startup. The app shows a short **security code** derived from it (for example `K7Q4-9WMP-2HXD`) and asks you to compare it with the one the server shows (`uzume-server code`, or the setup banner) on first contact:

```sh
sudo uzume-server code
journalctl -u uzume-server | grep -i fingerprint
```

## Upgrade

```sh
sudo ./install-server.sh upgrade --binary ./uzume-server-linux-arm64
```

Replaces the binary atomically, restarts the service and prints the old and new version. Data and config are untouched.

## Renew the certificate

```sh
sudo ./install-server.sh renew-cert
```

Renews the self-generated TLS certificate as the `uzume` user in the service's data dir (read from `/etc/uzume/server.env`), restarts the service and prints the new fingerprint. Refused when `UZUME_TLS_CERT`/`UZUME_TLS_KEY` are set. Do not call `uzume-server renew-cert` directly on a service install: another user or `-data-dir` renews a different certificate, and root would create root-owned files the service cannot read.

## Import cores offline

```sh
sudo ./install-server.sh import-cores /path/to/dir
```

For a server that cannot reach the libretro buildbot. `dir` holds buildbot core zips as `<platform>/<core>_libretro.<suffix>.zip` (platform `linux-x64` or `windows-x64`, for example `linux-x64/desmume_libretro.so.zip`), optionally `info.zip` and `<platform>/.index-extended`. The script copies the directory to a temporary location the service user can read and runs `uzume-server import-cores` as the `uzume` user in the service's data dir (read from `/etc/uzume/server.env`, like `renew-cert`). Cores are not signed; the SHA-256 is pinned at import. The service keeps running. Details: [server-configuration.md](server-configuration.md).

## Uninstall

```sh
sudo ./install-server.sh uninstall            # keeps /etc/uzume and the data dir
sudo ./install-server.sh uninstall --purge    # also deletes them (asks first; --yes skips)
```

The `uzume` system user is kept. `status` shows the service state and URL.

## Files

| Path | Purpose |
| --- | --- |
| `/usr/local/bin/uzume-server` | Server binary |
| `/etc/uzume/server.env` | Config (`UZUME_LISTEN`, `UZUME_DATA_DIR`, optional `UZUME_NAME`), `0640 root:uzume` |
| `/var/lib/uzume` | Default data dir (database, saves, `tls/`), `0750 uzume:uzume` |
| `/etc/systemd/system/uzume-server.service` | Service unit |
| `/etc/systemd/system/uzume-server.service.d/data-dir.conf` | Drop-in, only for a non-default data dir or ports below 1024 |

## Coming from FrameBeam

The Uzume server does not take over a FrameBeam install ([ADR 0030](../adr/0030-one-app-one-server-uzume.md)). Names, paths and formats changed everywhere, and nothing is migrated:

- **Set up the server fresh.** Install the `uzume-server` package (or run `install-server.sh`) as described above and create the admin account. It uses its own user `uzume`, `/etc/uzume/server.env` and `/var/lib/uzume` and does not read the old `framebeam` user, `/etc/framebeam` or `/var/lib/framebeam`. The `uzume-server` package conflicts with `framebeam-hub`, so installing it removes the old package (both would use port 8443); the old data in `/var/lib/framebeam` stays until you delete it. Still remove the old install first (a script install is no package and keeps running), after a backup of `/var/lib/framebeam`:

  ```sh
  # Debian package framebeam-hub
  sudo systemctl disable --now framebeam-hub framebeam-hub-update.path
  sudo apt purge framebeam-hub
  # or a script install (install-hub.sh)
  sudo systemctl disable --now framebeam-hub
  sudo rm -f /etc/systemd/system/framebeam-hub.service /usr/local/bin/framebeam-hub
  sudo rm -rf /etc/systemd/system/framebeam-hub.service.d /usr/local/share/doc/framebeam-hub /etc/framebeam
  sudo systemctl daemon-reload
  # both, once Uzume works
  sudo userdel framebeam; sudo rm -rf /var/lib/framebeam
  ```
- **Install the app again.** Run the new MSI. It replaces the old product, but it does not take over the old app data folder: pair the app with the server again (certificate fingerprint, device approval). Users sign in again.
- **Environment:** only `UZUME_*` variables are read. A `FRAMEBEAM_*` variable is ignored.
- **Docker:** use `ghcr.io/<owner>/uzume-server`. The image `framebeam-hub` is not updated any more. Start with a new volume.
- **Data:** backups made by FrameBeam (`framebeam-hub-*.tar`) cannot be restored, because the archive and database names changed. If you want old ROMs or saves, copy the files by hand: upload saves per slot ("Upload a save file"). ROMs are stored as `roms/<sha[:2]>/<sha>`; their names are in `framebeam.db`, so export them with their names into the import folder (`sudo apt install sqlite3` first) and use "Rescan folder" in the app, which reads only files directly in that folder:

  ```sh
  sudo sqlite3 -separator $'\t' /var/lib/framebeam/framebeam.db "SELECT rom_sha256, filename FROM games" |
  while IFS=$'\t' read -r sha name; do
    f=/var/lib/framebeam/roms/${sha:0:2}/$sha
    [ -f "$f" ] && sudo install -m 0644 -o uzume -g uzume "$f" "/var/lib/uzume/library-import/$name"
  done
  ```
- **Scripts and monitoring:** call `uzume-server`. Metrics are `uzume_*`. Backups are named `uzume-server-<name>-<UTC timestamp>.tar`. Invite codes look like `UZ-XXXX-XXXX`; `FB-` codes are not accepted.

## Migrating an existing manual data dir

Do not point `--data-dir` into `/home` or `/root` (the script refuses it): home directories are typically `0700`, so the `uzume` user cannot reach them, and `ProtectHome` applies as well. Copy the data into the default location instead:

```sh
# 1. stop the old, manually started server process
sudo ./install-server.sh install --binary ./uzume-server-linux-arm64 --no-start
sudo cp -a /home/pi/uzume-data/. /var/lib/uzume/
sudo ./install-server.sh install --binary ./uzume-server-linux-arm64   # fixes ownership, starts the service
```

Keep the old directory as a backup until the service works. `--data-dir DIR` remains available for directories outside `/home` and `/root`, e.g. `/srv/uzume`; the script chowns them to `uzume` and never deletes or overwrites their content.

