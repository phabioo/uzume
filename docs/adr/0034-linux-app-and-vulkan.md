# ADR 0034: Platforms: Linux app and Vulkan (0.24)

- Status: Accepted
- Date: 2026-10-27
- Deciders: Fabio (milestone, 2026-10-21/23), Claude (package decisions)

## Context

Roadmap section "0.24 Platforms" asks for a Linux app package (Secret Service instead of in-memory credentials, updater, core distribution, gamepads, audio, Steam Deck) and Vulkan next to OpenGL, with a spike first. Until now the Linux app ran from a build tree, kept credentials in memory only and had no self-update. The Windows app is an MSI ([ADR 0021](0021-one-windows-installer.md)); one app with a bundled server child process is the rule ([ADR 0030](0030-one-app-one-server-uzume.md)).

## Decisions

### D1 AppImage, not Flatpak

- One file that the existing signed updater replaces. No sandbox, so the bundled `uzume-server` child for "Only on this PC" (ADR 0030 D6), libretro cores from the data folder and SDL3/udev gamepads work. Runs on the Steam Deck in desktop mode and as a non-Steam game.
- Built on Ubuntu 24.04 in CI with linuxdeploy, linuxdeploy-plugin-qt and appimagetool (pinned by SHA-256). Bundles Qt 6.4, the FFmpeg multimedia backend (`QT_MEDIA_BACKEND=ffmpeg` in AppRun), the xcb and wayland platform plugins, SDL3, libdatachannel, `uzume-cli` and `uzume-server`. It never bundles a libretro core.
- glibc floor 2.38 (Ubuntu 24.04, SteamOS 3.x and other current distributions). Ubuntu 22.04 is not supported.
- Data lives in XDG data (`~/.local/share/Uzume/Uzume`). With `APPIMAGE` set, the portable `<exe-dir>/data` is never used.

### D2 Self-update of the AppImage

- The update index gets the player platform `linux-x64` with kind `appimage` (validation in `server/internal/updates`).
- The app downloads the image, verifies signature and SHA-256, writes `.<name>.part` next to the image, keeps `<image>.previous`, renames atomically and restarts after the save sync.
- A small watcher restores `.previous` when the new image does not confirm its start within about 5 s.
- Only when the image's folder is writable.

### D3 Credentials on Linux

- The freedesktop Secret Service over D-Bus with QtDBus (no libsecret): plain session on the local bus, items in the default collection with the attributes `application=uzume` and `uzume-target`, prompts awaited with a 60 s bound.
- Without a session bus or service: memory only, as before. `UZUME_CREDENTIAL_STORE=memory` forces it.

### D4 Core distribution

Unchanged. The server already serves the `linux-x64` buildbot builds (platform mapping in [ADR 0020](0020-cores-from-the-libretro-buildbot.md)). linux-arm64 has no app.

### D5 Vulkan spike result (2026-10-27, source reading)

- Azahar's libretro core offers Vulkan: core option `citra_graphics_api` (Auto, Vulkan, OpenGL, Software; restart required). With Auto it follows `GET_PREFERRED_HW_RENDER`, and it uses the context negotiation interface.
- melonDS DS is OpenGL only (`RETRO_HW_CONTEXT_OPENGL_CORE`). mGBA, Gambatte and DeSmuME render in software.
- Open: not yet run with a real Azahar build (no buildbot access from the cloud). Fabio checks locally.

### D6 Vulkan in the app

- The libretro Vulkan hardware interface (v5) plus context negotiation v1/v2 on a headless Vulkan device of Uzume's own. The Vulkan loader is opened at runtime (no link dependency; headers from libvulkan-dev or vcpkg vulkan-headers).
- Every frame is copied into a host-visible buffer and fed into the existing XRGB8888 path, so display, screenshots and Session encoding work as with the OpenGL readback. No zero-copy display or GPU-direct encode for Vulkan (later, on demand).
- Choice per core: setting `uzume.renderer` (`opengl` default, `vulkan`) at system or game level. The core profile's `renderer` field maps it to the core's own option (Azahar: `citra_graphics_api`). `GET_PREFERRED_HW_RENDER` answers Vulkan when chosen. `UZUME_RENDERER` overrides.
- No Vulkan device: software fallback with the reason "no Vulkan device".
- Tested in CI on lavapipe (mesa-vulkan-drivers) with the test core's Vulkan mode.

### D7 UI

One Renderer row (OpenGL | Vulkan, tag "Next start") in Settings › Emulation for cores that offer it, as drawn in [play-and-plugins.md](../design/uzume/play-and-plugins.md) (Renderer row). Hidden for software and OpenGL-only cores. No new screen.

## Rejected

- **Flatpak:** sandbox portals would be needed for the server child process, gamepads and the cores folder; Flatpak updates would bypass the signed index.
- **.deb for the app:** not usable on SteamOS, needs root.
- **Plain tarball:** no self-update story.
- **libsecret:** extra dependency; QtDBus is enough.
- **Encrypted file for credentials:** a secret in a file breaks the hard rule.

## Consequences

- New CI job for the AppImage; the update index and `server/internal/updates` accept `appimage`; the app gets a Vulkan backend next to the OpenGL one.
- Open, Fabio-only checks: real GPUs with Vulkan, an Azahar Vulkan run, Steam Deck desktop and game mode, gamepads, audio through PipeWire and PulseAudio, KWallet and GNOME Keyring, the AppImage self-update and its restore on a real desktop, Wayland.
