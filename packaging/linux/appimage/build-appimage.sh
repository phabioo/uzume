#!/usr/bin/env bash
# Builds the Uzume app AppImage (Linux x86-64) from a finished release build of the client.
#
# Usage: build-appimage.sh --build-dir <client build dir> --server <uzume-server binary> --version <X.Y.Z[-pre]> --out <dir>
#                          [--qml-dir <client/ui>] [--tools <dir with linuxdeploy, linuxdeploy-plugin-qt, appimagetool>]
# Output: <out>/Uzume-<version>-x86_64.AppImage  (the AppDir stays in <out>/work/Uzume.AppDir for inspection).
# Tools: scripts/fetch-appimage-tools.sh (default). Extra library directories for the linker lookup (libdatachannel, SDL3
# from ~/.cache/uzume/deps): UZUME_EXTRA_LIB_DIRS (colon separated), needed only when the binaries carry no RUNPATH to them.
# The AppImage tools are run with APPIMAGE_EXTRACT_AND_RUN=1, so FUSE is not needed to build.
# No emulator cores in the image: the build fails when the AppDir contains *_libretro.* or a cores directory.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
build="" server="" version="" out="" qml="$ROOT/client/ui" tools=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) build="$2"; shift 2 ;;
    --server) server="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --qml-dir) qml="$2"; shift 2 ;;
    --tools) tools="$2"; shift 2 ;;
    -h | --help) sed -n 2,11p "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -d "$build" ] || { echo "--build-dir is required (client build directory)" >&2; exit 2; }
[ -x "$server" ] || { echo "--server is required (uzume-server linux/amd64 binary)" >&2; exit 2; }
[ -n "$version" ] || { echo "--version is required" >&2; exit 2; }
[ -n "$out" ] || { echo "--out is required" >&2; exit 2; }
[ "$(uname -m)" = x86_64 ] || { echo "the AppImage is built for x86-64 only" >&2; exit 1; }
build="$(cd "$build" && pwd)"; server="$(cd "$(dirname "$server")" && pwd)/$(basename "$server")"
mkdir -p "$out"; out="$(cd "$out" && pwd)"
[ -n "$tools" ] || tools="$("$ROOT/scripts/fetch-appimage-tools.sh")"

# Executables of the build: uzume (GUI app) and uzume-cli, wherever the build tree puts them.
find_exe() { find "$build" -type f -name "$1" -perm -u+x -not -path '*/CMakeFiles/*' -not -path '*/vcpkg_installed/*' | head -n 1; }
app_exe="$(find_exe uzume)"; cli_exe="$(find_exe uzume-cli)"
[ -n "$app_exe" ] || { echo "uzume not found in $build (build target uzume_app)" >&2; exit 1; }
[ -n "$cli_exe" ] || { echo "uzume-cli not found in $build (build target uzume_cli)" >&2; exit 1; }

work="$out/work"
appdir="$work/Uzume.AppDir"
rm -rf "$work"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" "$appdir/usr/share/metainfo" "$appdir/usr/share/doc/uzume/licenses"
install -m 0755 "$app_exe" "$appdir/usr/bin/uzume"
install -m 0755 "$cli_exe" "$appdir/usr/bin/uzume-cli"
# The server next to the app: "Only on this PC" starts it as a child process (LocalServerProcess::findServerExe).
install -m 0755 "$server" "$appdir/usr/bin/uzume-server"

# Desktop entry, icons, metainfo.
install -m 0644 "$HERE/uzume.desktop" "$appdir/usr/share/applications/uzume.desktop"
for size in 16 24 32 48 64 128 256 512; do
  install -D -m 0644 "$ROOT/client/app/icons/uzume-$size.png" "$appdir/usr/share/icons/hicolor/${size}x${size}/apps/uzume.png"
done
install -D -m 0644 "$ROOT/client/app/icons/uzume.svg" "$appdir/usr/share/icons/hicolor/scalable/apps/uzume.svg"
sed -e "s/@VERSION@/$version/" -e "s/@DATE@/$(date -u +%F)/" "$HERE/io.github.phabioo.Uzume.metainfo.xml" \
  > "$appdir/usr/share/metainfo/io.github.phabioo.Uzume.metainfo.xml"

# Libraries and Qt plugins. linuxdeploy follows the dependencies of all three executables; the Qt plugin finds QML imports
# through QML_SOURCES_PATHS. Platforms: xcb (default), wayland and offscreen (CI smoke test, headless use); tls (openssl backend), imageformats and the multimedia
# backend are loaded at run time and so have to be named. The GStreamer backend is not deployed: AppRun selects FFmpeg.
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake || true)}"
[ -n "$QMAKE" ] || { echo "qmake6 not found (Qt 6 development packages: scripts/qt-packages.txt)" >&2; exit 1; }
export QML_SOURCES_PATHS="$qml"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so;libqoffscreen.so"
export EXTRA_QT_PLUGINS="tls;imageformats;multimedia"
export NO_STRIP=1   # the release build is stripped by CMake/linker flags already; linuxdeploy's strip is too old for .relr.dyn
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export PATH="$tools:$PATH"
[ -z "${UZUME_EXTRA_LIB_DIRS:-}" ] || export LD_LIBRARY_PATH="$UZUME_EXTRA_LIB_DIRS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$tools/linuxdeploy" --appdir "$appdir" \
  --executable "$appdir/usr/bin/uzume" --executable "$appdir/usr/bin/uzume-cli" --executable "$appdir/usr/bin/uzume-server" \
  --desktop-file "$appdir/usr/share/applications/uzume.desktop" \
  --icon-file "$ROOT/client/app/icons/uzume-256.png" --icon-filename uzume \
  --custom-apprun "$HERE/AppRun" \
  --plugin qt >&2

# The Wayland platform plugin needs its shell, decoration and graphics integration plugins (linuxdeploy-plugin-qt does not
# deploy them): copy them from the Qt installation; they use the Qt libraries already in usr/lib.
qt_plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
for d in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
  [ -d "$qt_plugins/$d" ] || { echo "Qt plugin directory $d is missing (qt6-wayland)" >&2; exit 1; }
  cp -r "$qt_plugins/$d" "$appdir/usr/plugins/"
done
# The copies keep the RUNPATH of the Qt installation (none): without one, libqt-plugin-wayland-egl.so cannot find
# usr/lib/libQt6WaylandEglClientHwIntegration.so.6 and AppRun sets no LD_LIBRARY_PATH (#338). Same RUNPATH as the
# platform plugins linuxdeploy deploys (usr/plugins/<dir>/x.so -> usr/lib).
command -v patchelf >/dev/null 2>&1 || { echo "patchelf is required (apt install patchelf)" >&2; exit 1; }
for d in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
  # shellcheck disable=SC2016 # $ORIGIN is for the loader, not the shell
  find "$appdir/usr/plugins/$d" -type f -name '*.so' -exec patchelf --set-rpath '$ORIGIN/../../lib' {} +
done

# The Qt multimedia plugin directory holds the FFmpeg and the GStreamer backend; only FFmpeg is wanted.
find "$appdir/usr/plugins/multimedia" -name 'libgstreamermediaplugin.so' -delete 2>/dev/null || true

# No emulator cores in the image: the app downloads them from the Uzume server on demand.
leak="$(find "$appdir" \( -name '*_libretro.*' -o \( -type d -iname cores \) \) -print)"
if [ -n "$leak" ]; then
  echo "emulator core in the AppDir (cores must not be part of the package):" >&2
  echo "$leak" >&2
  exit 1
fi

# License material (GPL-3.0-or-later): LICENSE, notices, and the Debian copyright file of every bundled library's package.
doc="$appdir/usr/share/doc/uzume"
install -m 0644 "$ROOT/LICENSE" "$doc/LICENSE"
install -m 0644 "$HERE/THIRD-PARTY-NOTICE.txt" "$doc/THIRD-PARTY-NOTICE.txt"
install -m 0644 "$ROOT/server/THIRD-PARTY-NOTICES.txt" "$doc/THIRD-PARTY-NOTICES-server.txt"
if command -v dpkg-query >/dev/null 2>&1; then
  while IFS= read -r lib; do
    # The bundled copy is not a package file: look the file name up among the installed packages (dpkg -S takes globs).
    pkg="$(dpkg-query -S "*/$(basename "$lib")" 2>/dev/null | head -n 1 | cut -d: -f1 || true)"
    [ -n "$pkg" ] || continue
    cr="/usr/share/doc/$pkg/copyright"
    [ ! -f "$cr" ] || install -m 0644 "$cr" "$doc/licenses/$pkg.txt"
  done < <(find "$appdir/usr/lib" "$appdir/usr/plugins" "$appdir/usr/qml" -type f -name '*.so*' 2>/dev/null)
fi
# Libraries from the pinned source builds (not packages): their licenses are listed in the notice.
echo "Bundled license files: $(find "$doc/licenses" -type f | wc -l)" >&2

# Every library must resolve inside the AppDir (or be a system library the image does not carry: glibc, GL, ...).
# The run-time check is the smoke test in CI; this one catches a missing bundle before packaging. It runs without any
# LD_LIBRARY_PATH (also not the one set above for linuxdeploy), so a library or plugin without a working RUNPATH is found.
missing="$(find "$appdir/usr" -type f \( -name 'uzume*' -o -name '*.so*' \) -print0 | while IFS= read -r -d '' f; do
  case "$(file -b "$f")" in ELF*) env -u LD_LIBRARY_PATH ldd "$f" 2>/dev/null | { grep 'not found' || true; } | sed "s|^|$(basename "$f"): |" ;; esac
done | sort -u)"
if [ -n "$missing" ]; then
  echo "unresolved libraries in the AppDir:" >&2
  echo "$missing" >&2
  exit 1
fi

# AppImage (squashfs, zstd). appimagetool validates the desktop file and, when appstreamcli exists, the metainfo.
file="$out/Uzume-$version-x86_64.AppImage"
rm -f "$file"
ARCH=x86_64 "$tools/appimagetool" --comp zstd "$appdir" "$file" >&2
[ -f "$file" ] || { echo "appimagetool did not produce $file" >&2; exit 1; }
chmod 0755 "$file"
echo "$file"
