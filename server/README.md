# Server: Uzume server

Go service with SQLite: ROM library, versioned saves, users and devices, Session signaling, optional STUN/TURN relay, core package cache, updater and the admin API. It serves only the REST API (`/api/v1`), WebSocket signaling and file transfer, no web pages; administration is in the Uzume app. It never emulates, encodes or renders. Rules for agents: `CLAUDE.md`.

## Build, run, test

```sh
make check-hub                                   # gofmt, vet, staticcheck, tests, codegen freshness, packaging checks
make build-hub                                   # server/dist/uzume-server-linux-{amd64,arm64}
make generate                                    # Go code from protocol/openapi (oapi-codegen)
uzume-server -dev -listen 127.0.0.1:8443 -data-dir /tmp/fb   # development: HTTP instead of HTTPS
uzume-server setup-admin -username <name>       # first admin (password as one line from stdin)
uzume-server help                               # setup-code, code, reset-admin, status, update, backup, restore, reset, ...
```

Requires Go >= 1.25 (`go.mod`). `cmd/uzume-server` is the server, `cmd/uzume-sign` signs core and update indexes.

## Documentation

- Install on Linux (`.deb`, systemd script): [Server installation (wiki)](https://github.com/phabioo/uzume/wiki/Server-installation)
- Flags, environment, certificates, data directory: [Server configuration (wiki)](https://github.com/phabioo/uzume/wiki/Server-configuration)
- Updates and releases: [Updates (wiki)](https://github.com/phabioo/uzume/wiki/Updates); Sessions and the relay: [Sessions over the internet (wiki)](https://github.com/phabioo/uzume/wiki/Sessions-over-the-internet)
- Code layout, users/invites, firmware, update internals: [docs/reference/server-internals.md](../docs/reference/server-internals.md)
- API: [protocol/README.md](../protocol/README.md); decisions: [ADR 0002](../docs/adr/0002-protocol-and-hub-phase1.md), [0005](../docs/adr/0005-saves-phase3.md), [0006](../docs/adr/0006-sessions-phase4.md), [0010](../docs/adr/0010-cores-from-the-hub.md), [0011](../docs/adr/0011-automatic-updates.md), [0012](../docs/adr/0012-internet-sessions-and-save-comfort.md), [0015](../docs/adr/0015-hub-ui-pass.md)
