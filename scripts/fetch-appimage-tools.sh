#!/usr/bin/env bash
# Downloads the pinned AppImage build tools (scripts/appimage-tools.pin) to
# $HOME/.cache/uzume/tools/appimage/<pin id>/ and prints that directory (stdout; it holds linuxdeploy,
# linuxdeploy-plugin-qt and appimagetool). Idempotent: a tool whose checksum matches is not downloaded again.
# All messages go to stderr.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
. "$ROOT/scripts/appimage-tools.pin"
CACHE="${UZUME_CACHE_DIR:-$HOME/.cache/uzume}"
DIR="$CACHE/tools/appimage/${LINUXDEPLOY_SHA256:0:8}-${LINUXDEPLOY_QT_SHA256:0:8}-${APPIMAGETOOL_SHA256:0:8}"
mkdir -p "$DIR"

fetch() { # name url sha256
  local file="$DIR/$1"
  if [ -x "$file" ] && [ "$(sha256sum "$file" | cut -d' ' -f1)" = "$3" ]; then return 0; fi
  echo "fetching $1" >&2
  curl -fsSL --retry 3 --retry-delay 5 -o "$file.part" "$2"
  if [ "$(sha256sum "$file.part" | cut -d' ' -f1)" != "$3" ]; then
    rm -f "$file.part"
    echo "$1: checksum mismatch" >&2
    exit 1
  fi
  chmod +x "$file.part"
  mv "$file.part" "$file"
}
fetch linuxdeploy "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA256"
# linuxdeploy finds its plugins as linuxdeploy-plugin-<name> next to itself or in PATH.
fetch linuxdeploy-plugin-qt "$LINUXDEPLOY_QT_URL" "$LINUXDEPLOY_QT_SHA256"
fetch appimagetool "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA256"
echo "$DIR"
