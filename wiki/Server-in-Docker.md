# Run the server in Docker

At a glance: Run the server as a non-root container (amd64, arm64) with the volume `/data`: image, `docker run` or compose, volumes and permissions, upgrade, backup. Flags: [server-configuration.md](Server-configuration).

Contents: [Image](#image), [docker run](#docker-run), [docker compose](#docker-compose), [Volumes and permissions](#volumes-and-permissions), [Upgrade](#upgrade), [Backup](#backup), [Build the image yourself](#build-the-image-yourself).

The official image runs the Uzume server as a non-root container for `linux/amd64` and `linux/arm64` (Raspberry Pi 5 included). Decisions: [ADR 0029](https://github.com/phabioo/uzume/blob/main/docs/adr/0029-operations-and-safety.md) D1. For flags and environment variables see [server-configuration.md](Server-configuration); for apps outside your LAN see [sessions-over-the-internet.md](Sessions-over-the-internet). The Debian package ([server-install.md](Server-installation)) stays the recommended install on Debian; use the image when you already run Docker (NAS, home server).

## Image

`ghcr.io/phabioo/uzume-server`

| Tag | Meaning |
|---|---|
| `X.Y.Z` | One exact version (every beta and release). Pin this for repeatable setups. |
| `stable` and `latest` | The newest promoted release ("Promote to release", no rebuild). They exist only after the first promotion; until then pin `X.Y.Z` (or use `beta`). |
| `beta` | The newest build of `main`. Not for production. |

The image of FrameBeam (`framebeam-hub`) is no longer pushed. Coming from it: start with a new volume, see [Coming from FrameBeam](Server-installation#coming-from-framebeam).

The image is `distroless/static` with the server binary only: no shell, no package manager. It runs as user `65532` and keeps all data in the volume `/data`. The image sets `UZUME_SERVER_CONTAINER=1`, which makes `/data` the default data directory; the server listens on `:8443` by default.

## docker run

```sh
docker run -d --name uzume-server --restart unless-stopped \
  -v uzume-data:/data \
  -p 8443:8443 \
  ghcr.io/phabioo/uzume-server:stable
```

Open `https://<docker-host>:8443` in Uzume, not in a browser. The server creates a self-signed certificate (the app pins it after you compare the security code). A server in a container is set up from the app on another PC with a **setup code**: the container prints it when it starts with no admin, and you can get a new one any time:

```sh
docker logs uzume-server                                  # the setup banner: address, security code, setup code
docker exec -it uzume-server uzume-server setup-code      # a new setup code (valid 60 minutes, works once)
docker exec -it uzume-server uzume-server code            # the security code of the certificate
```

In Uzume choose Set up a server › On another device, enter the address and the setup code, compare the security code and create the admin account. Without the app, `setup-admin` creates the admin (password as a single line from stdin):

```sh
printf '%s\n' 'a long password' | docker exec -i uzume-server uzume-server setup-admin -username alice
```

The address in the banner inside a container is the address of the Docker host (set `UZUME_PUBLIC_HOST` to show your DNS name).

### Server commands in Docker

These work next to the running server (the container is the server; `-it` only for commands that ask or print secrets):

| Command | Docker form |
|---|---|
| `setup-code`, `code`, `status` | `docker exec -it uzume-server uzume-server <command>` |
| `reset-admin` | `docker exec -it uzume-server uzume-server reset-admin` (prints a one-time password; nothing else changes) |
| `backup` | `docker exec -it uzume-server uzume-server backup` (see [server-backup.md](Server-backup#docker)) |
| `update -check` | `docker exec uzume-server uzume-server update -check` prints the newer version; in a container `update` never installs, it names the tag to pull: see [Upgrade](#upgrade) |

`restore` and `reset` need the server **stopped** (they refuse while it holds the data folder lock). Stop the service and run them in a one-off container that mounts the same volumes; the image's entrypoint is the server binary, so the command follows the service name:

```sh
docker compose stop hub                                       # "hub" is the service name in the example below
docker compose run --rm --no-deps hub reset                   # asks for the server name; safety backup first
docker compose run --rm --no-deps hub restore /backup/<file>  # see server-backup.md
docker compose start hub
```

`reset` deletes everything in `/data` except `backups/` and `logs/` (the volume itself and anything mounted elsewhere stay); the next start prints a new setup code and creates a new certificate. With `docker run`: `docker stop uzume-server`, then `docker run --rm -v uzume-data:/data ghcr.io/phabioo/uzume-server:stable reset`.

Other commands work the same way, for example `docker exec -i uzume-server uzume-server import-cores /data/import` (put the zips in the volume first) and `docker exec uzume-server uzume-server diagnostics /data/diagnostics.zip`.

## docker compose

```yaml
services:
  hub:
    image: ghcr.io/phabioo/uzume-server:stable
    container_name: uzume-server
    restart: unless-stopped
    environment:
      UZUME_NAME: "Living Room Hub"        # first start only
      # Sessions over the internet (embedded TURN relay, see sessions-over-the-internet.md):
      # UZUME_TURN: "1"
      # UZUME_PUBLIC_HOST: "hub.example.com"
    ports:
      - "8443:8443"                  # API and signaling (TCP)
      # With UZUME_TURN=1 (same numbers as -turn-port and -turn-relay-ports):
      # - "3478:3478/udp"
      # - "3478:3478/tcp"
      # - "49160-49199:49160-49199/udp"
    volumes:
      - uzume-data:/data
      # Optional: a ROM folder for Server › Games › "Rescan folder" (read only). Set UZUME_LIBRARY_IMPORT_DIR=/roms.
      # - /srv/roms:/roms:ro

volumes:
  uzume-data:
```

Bridge networking is fine for the server port. For the TURN relay the server announces the address from `UZUME_PUBLIC_HOST` (or `UZUME_TURN_RELAY_IP`), so publish exactly the relay range you configured and forward the same ports on the router. If relaying is unreliable behind NAT, `network_mode: host` removes Docker's port translation (then drop the `ports` section).

## Volumes and permissions

- Back up to a second volume, not into `/data`: see [server-backup.md](Server-backup#docker).
- `/data` holds everything: `uzume.db`, `saves/`, `firmware/`, the ROM library, `tls/`, `cores/`, plugins and `backups/`. Nothing else in the container is state.
- A new named volume starts owned by the container user. For a bind mount (`-v /srv/uzume:/data`) make the folder writable for uid `65532` first: `sudo chown 65532:65532 /srv/uzume`.
- Own certificate: mount the PEM files read only (for example `-v /srv/certs:/certs:ro`) and set `UZUME_TLS_CERT=/certs/hub.pem`, `UZUME_TLS_KEY=/certs/hub.key`.

## Upgrade

The server never updates itself inside a container (`UZUME_SERVER_CONTAINER=1`): Server › Settings › Updates shows that a newer version exists and tells you to pull the image. Sessions end while the container restarts. `docker exec uzume-server uzume-server update` (or `update -check`) says whether a newer version exists and prints the image tag to pull; it installs nothing.

```sh
docker compose pull && docker compose up -d
# or, with docker run: docker pull ghcr.io/phabioo/uzume-server:stable, then
# docker rm -f uzume-server and run it again with the same -v option
```

The server migrates its database at start and keeps a copy of the old one in `/data/backups`. To go back, start the older `X.Y.Z` tag with a restored copy (the server refuses a database from a newer version, see [Data directory](Server-configuration#data-directory)).

## Backup

Back up the `/data` volume. For a consistent copy stop the container first:

```sh
docker stop uzume-server
docker run --rm -v uzume-data:/data:ro -v "$PWD":/backup alpine tar czf /backup/uzume-data.tar.gz -C /data .
docker start uzume-server
```

Restore into an empty volume with the same command in reverse (`tar xzf ... -C /data`, then `chown -R 65532:65532 /data`). Saves are never sent to another server; keep the archive private.

## Build the image yourself

From the repository root (the context must be the root):

```sh
docker build -f packaging/docker/Dockerfile -t uzume-server \
  --build-arg HUB_VERSION=0.17.0 --build-arg HUB_CHANNEL=stable .
docker buildx build --platform linux/amd64,linux/arm64 -f packaging/docker/Dockerfile .
```

The Go build stage always runs on the build host and cross-compiles, so arm64 needs no emulation. CI builds both platforms on every change; `release.yml` pushes `X.Y.Z` and `beta` from the CI binaries and `promote.yml` adds `stable` and `latest` to the promoted version without a rebuild.
