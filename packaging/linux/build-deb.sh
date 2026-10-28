#!/usr/bin/env bash
# Builds the Uzume server .deb with dpkg-deb only (no debhelper): wiki/Server-installation.md.
# Usage: build-deb.sh --binary PATH --arch amd64|arm64 --version X.Y.Z[-pre] --out DIR
# Control Version: SemVer with - replaced by ~ (0.3.0-beta.57 -> 0.3.0~beta.57); the file name keeps the SemVer. Prints the .deb path.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="" ARCH="" VERSION="" OUT=""

die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --binary)  [ $# -ge 2 ] || die "--binary needs a value"; BINARY="$2"; shift ;;
    --arch)    [ $# -ge 2 ] || die "--arch needs a value"; ARCH="$2"; shift ;;
    --version) [ $# -ge 2 ] || die "--version needs a value"; VERSION="$2"; shift ;;
    --out)     [ $# -ge 2 ] || die "--out needs a value"; OUT="$2"; shift ;;
    -h|--help) sed -n '2,5p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

[ -f "$BINARY" ] || die "--binary: file not found: '$BINARY'"
case "$ARCH" in amd64|arm64) ;; *) die "--arch must be amd64 or arm64" ;; esac
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || die "--version must be X.Y.Z or X.Y.Z-pre (got '$VERSION')"
[ -n "$OUT" ] || die "--out needs a value"
command -v dpkg-deb >/dev/null 2>&1 || die "dpkg-deb not found"

DEBVER="${VERSION//-/\~}"
DEB="$OUT/uzume-server_${VERSION}_${ARCH}.deb"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
chmod 0755 "$STAGE"

DOCDIR="$STAGE/usr/share/doc/uzume-server"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
[ -f "$REPO_ROOT/server/THIRD-PARTY-NOTICES.txt" ] || die "server/THIRD-PARTY-NOTICES.txt missing (make notices)"
install -d -m 0755 "$STAGE/usr/bin" "$STAGE/lib/systemd/system" "$STAGE/DEBIAN" "$DOCDIR" "$OUT"
install -m 0755 "$BINARY" "$STAGE/usr/bin/uzume-server"
# The script-install unit points at /usr/local/bin; the package unit runs /usr/bin and gets a runtime dir for the
# update request file (kept across server restarts so a request made right before a restart is not lost).
unit="$STAGE/lib/systemd/system/uzume-server.service"
sed -e 's#^ExecStart=/usr/local/bin/uzume-server$#ExecStart=/usr/bin/uzume-server#' \
    -e 's#^RestartSec=5$#RestartSec=5\nRuntimeDirectory=uzume\nRuntimeDirectoryPreserve=yes#' \
    "$SCRIPT_DIR/uzume-server.service" >"$unit"
grep -qx 'ExecStart=/usr/bin/uzume-server' "$unit" || die "could not rewrite ExecStart in uzume-server.service"
grep -qx 'RuntimeDirectory=uzume' "$unit" || die "could not add RuntimeDirectory to uzume-server.service"
chmod 0644 "$unit"
install -m 0644 "$SCRIPT_DIR/uzume-server-update.path" "$SCRIPT_DIR/uzume-server-update.service" \
  "$STAGE/lib/systemd/system/"

# Debian machine-readable copyright (format 1.0); the third-party Go module licenses ship next to it.
install -m 0644 "$REPO_ROOT/server/THIRD-PARTY-NOTICES.txt" "$DOCDIR/THIRD-PARTY-NOTICES.txt"
cat >"$DOCDIR/copyright" <<'COPYRIGHT'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Uzume
Source: https://github.com/phabioo/uzume

Files: *
Copyright: 2026 Fabio and Uzume contributors
License: GPL-3+

License: GPL-3+
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 .
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 .
 On Debian systems, the complete text of the GNU General Public
 License version 3 can be found in /usr/share/common-licenses/GPL-3.
 .
 The licenses of the third-party Go modules compiled into the binary are
 listed in /usr/share/doc/uzume-server/THIRD-PARTY-NOTICES.txt.
COPYRIGHT
chmod 0644 "$DOCDIR/copyright"

for s in postinst prerm postrm; do install -m 0755 "$SCRIPT_DIR/deb/$s" "$STAGE/DEBIAN/$s"; done

size_kb="$(du -sk "$STAGE/usr" "$STAGE/lib" | awk '{s+=$1} END {print s}')"
cat >"$STAGE/DEBIAN/control" <<CONTROL
Package: uzume-server
Version: $DEBVER
Section: net
Priority: optional
Architecture: $ARCH
Maintainer: Uzume <uzume@users.noreply.github.com>
Installed-Size: $size_kb
Depends: systemd
Conflicts: framebeam-hub
Homepage: https://github.com/phabioo/uzume
Description: Uzume server - self-hosted retro gaming library and session server
 Central ROM library, versioned saves and session coordination for Uzume
 apps. Runs as the systemd service uzume-server; configuration in
 /etc/uzume/server.env, data in /var/lib/uzume (never removed by the
 package).
CONTROL

# Reproducible mtimes; xz for old dpkg versions (Raspberry Pi OS).
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$SCRIPT_DIR" log -1 --format=%ct 2>/dev/null || date +%s)}"
find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
dpkg-deb --root-owner-group -Zxz --build "$STAGE" "$DEB" >/dev/null
echo "$DEB"
