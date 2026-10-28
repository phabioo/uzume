# packaging

Release artifacts of Uzume.

- `linux/`: server as a systemd service. `install-server.sh` (any systemd Linux), the Debian package build `build-deb.sh` with its units and maintainer scripts (`deb/`), and the service and update units. Guide: [Server installation (wiki)](https://github.com/phabioo/uzume/wiki/Server-installation); packaging checks run in `make check-hub`.
- `windows/`: Uzume MSI `uzume.wxs` (WiX v5, app and server), built in the Windows CI job. Details: [docs/guides/packaging.md](../docs/guides/packaging.md).
- `macos/`: empty placeholder; a macOS app is planned in the [roadmap](../docs/roadmap.md) (0.12).

Versions, channels and the release pipeline: [docs/development.md](../docs/development.md), [Updates (wiki)](https://github.com/phabioo/uzume/wiki/Updates).
