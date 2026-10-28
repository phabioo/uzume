# packaging/linux/appimage

The Uzume app as an AppImage for Linux x86-64 (`Uzume-<version>-x86_64.AppImage`). Background and contents: [docs/guides/packaging.md](../../../docs/guides/packaging.md#linux-appimage).

| File | Purpose |
|---|---|
| `build-appimage.sh` | Builds the AppImage from a finished release build: `--build-dir`, `--server`, `--version`, `--out` (run `--help`) |
| `AppRun` | Launcher inside the image: data folder (`UZUME_DATA_DIR`), multimedia backend |
| `uzume.desktop` | Menu entry (`Game;Emulator;`, handles `uzume://` links) |
| `io.github.phabioo.Uzume.metainfo.xml` | AppStream metadata (version and date are filled in by the script) |
| `THIRD-PARTY-NOTICE.txt` | Notice shipped in `usr/share/doc/uzume/` |

```sh
scripts/bootstrap-vcpkg.sh && make fetch-deps fetch-sdl3
(cd client && cmake --preset linux-release -DCMAKE_PREFIX_PATH=$(../scripts/fetch-libdatachannel.sh) -DSDL3_ROOT=$(../scripts/fetch-sdl3.sh) \
  && cmake --build --preset linux-release --target uzume_app uzume_cli)
make build-hub
make package-app-appimage APP_VERSION=21.1.0     # client/build/appimage/Uzume-21.1.0-x86_64.AppImage
```

The tools (linuxdeploy, linuxdeploy-plugin-qt, appimagetool) are pinned in `scripts/appimage-tools.pin` and downloaded by `scripts/fetch-appimage-tools.sh`; they run with `APPIMAGE_EXTRACT_AND_RUN=1`, so FUSE is not needed to build. The build fails when the image would contain an emulator core (`*_libretro.*`, `cores`) or an unresolved library.
