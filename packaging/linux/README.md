# packaging/linux

Uzume server on Linux (systemd): the installer script, the Debian package build and the units.

| File | Purpose |
|---|---|
| `install-server.sh` | `install`, `upgrade`, `renew-cert`, `import-cores`, `status`, `uninstall` from a local binary (nothing is downloaded); run `--help` |
| `uzume-server.service` | Service unit used by the script |
| `build-deb.sh` | Builds `uzume-server_<version>_{amd64,arm64}.deb` (dpkg-deb only; the binary is `/usr/bin/uzume-server`): `make build-hub package-hub-deb HUB_VERSION=...` |
| `deb/` | Maintainer scripts of the package (`postinst`, `prerm`, `postrm`) |
| `uzume-server-update.path`, `uzume-server-update.service` | Root helper that applies a staged server update |

```sh
sudo ./install-server.sh install --binary ./uzume-server-linux-arm64 --admin <name>   # add --port 8444 if 8443 is taken
```

Full guide (Debian package, options, admin account, TLS, upgrade, migration, uninstall, files): [Server installation (wiki)](https://github.com/phabioo/uzume/wiki/Server-installation).
