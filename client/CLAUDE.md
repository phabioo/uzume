# client/ – Uzume app (C++20, Qt 6/QML, SDL3)

Details only as needed from `docs/architecture/` (index: `docs/architecture/README.md`).

## Responsibilities
- Library access, ROM/core/firmware cache (separate, ROM hash-based), save sync, Session manager, emulation, rendering incl. multiview, audio, encoder/decoder, input. See `01-overview.md`, `02-protocols-and-rom-cache.md`.

## Emulation
- `EmulatorBackend` -> `LibretroBackend` (later possibly `StandaloneBackend`). Systems and cores are data-driven (ADR 0020 D6): system manifests `emulation/manifests/systems/<system>.json` and core profiles `manifests/cores/<core_id>.json` (cores without profile are experimental), core choice game > system > server default among the cores the server serves (`uzume.core`), no console-specific launch logic. A save last written by another core/build is snapshotted on the server before the start (D7).
- Systems are data-only: a new system is a manifest JSON (extensions, firmware, `input_profile` + `input.map` to libretro targets, `identity` reader, `display`), optionally core profiles; no code or CMake edit (glob). Built-in: systems `nds`, `3ds`, `gb`, `gbc`, `gba`; core profiles `melondsds`, `desmume`, `azahar`, `mgba` (gb/gbc/gba), `gambatte` (gb/gbc).
- Core options dynamically from Libretro core options, no invented options. See `05-emulation.md`.
- Hardware rendering (ADR 0013): offscreen OpenGL context per game on the emulation thread, app-owned FBO, every frame read back into the XRGB8888 path (ADR 0027, Windows: with zero-copy display active the emulation thread blits into shared D3D11 textures through the GL-D3D11 bridge (`emu::GpuDisplayTarget`) and readback runs only while a CPU consumer needs pixels; `UZUME_DISABLE_ZERO_COPY=1`); unavailable in the CLI, with `UZUME_DISABLE_HW_RENDER=1` or if context creation fails (cores fall back to software). Vulkan (ADR 0034, `emulation/vk_render.*`) runs next to it for cores that ask for it or when `uzume.renderer`/`UZUME_RENDERER` prefers it: headless device, loader opened at runtime (never linked), frames copied into the same readback path; on Windows the zero-copy display also takes Vulkan frames (ADR 0034 D8, external memory into the D3D11 ring, keyed mutex in the Vulkan submit); on Windows a shared Vulkan game's Session frames also skip the readback (ADR 0034 D9: `GpuEncodeTarget::attachExternal`, blit fitted into a shared BGRA texture in the same submit, `D3d11EncodeCapture` Vulkan mode on the Vulkan device's adapter for AMF, QSV and NVENC-with-D3D11-frames, captured right after the submit). ADR 0019: while a Session is shared on NVIDIA the emulation thread also blits a Session encode texture and hands it to `emu::GpuEncodeTarget` (CUDA-GL interop; all interop calls on the emulation thread with GL current).
- Settings are local: global -> system/core -> game override, partial overrides only. Controller profiles are local (`06-controllers.md`).

## Server binding
- Exactly one active server; library, saves, Sessions, registry only from it.
- Local data and pending sync are separated by server ID (and server user ID), even with identical ROM hashes. No global accounts.
- Credentials only in the OS credential store, never in profile files/logs. TOFU/pinning: a certificate mismatch warns and is never silently accepted. See `10-identity-pairing-tls.md`.

## Save sync (app view)
- Upload only when the hash changed; debounce approx. 10-15 s, periodic at most every approx. 60 s; pause/stop/clean exit = immediate final sync.
- Failed uploads are kept as pending sync (server, user, game/slot, base version) and retried only to the originating server. Save states are separate, not in the PoC. See `03-saves.md`.

- Data: all app data under `ProfileStore::baseDir()` (portable `<exe-dir>/data`, ADR 0004); never invent own paths.

## Media and language
- GPU-direct encoding (ADR 0019 NVENC via CUDA, ADR 0027 AMF/QSV via `D3d11EncodeCapture` on the GL-D3D11 bridge, chosen in `GpuEncodeBridge`); readback stays the fallback.
- FFmpeg (libavcodec) with NVENC/QSV/AMF, OpenH264 as software fallback; encoder order h264_nvenc, h264_qsv, h264_amf, libopenh264, libx264 (ADR 0006 D5; the Windows vcpkg FFmpeg enables `nvcodec`, `qsv` and `amf` since 0.1.1, ADR 0009 D7; opening them needs a GPU and is verified only locally, otherwise it falls back to libopenh264); GPU-direct encoding (ADR 0019): `CudaGlCapture` feeds CUDA frames to a second `VideoEncoder` (`h264_nvenc`); failures fall back to readback frames, never to the sticky encoder failure; `UZUME_DISABLE_GPU_ENCODE=1` and a forced `UZUME_H264_ENCODER` keep readback; audio output via Qt Multimedia, SDL3 only for gamepads (ADR 0007); WebRTC with libdatachannel (ADR 0001, `04-sessions-and-multiview.md`).
- C++20 as baseline; C++23 only if MSVC, GCC and AppleClang support it. vcpkg (manifest) + CMake presets.
- Firmware missing: "Firmware required/missing", block launch.
- Design: app UI `docs/design/README.md`, `docs/design/legacy/player.md`, `docs/design/legacy/tokens.md`.

- Runtime environment variables are read through `core/envvar.h` (`uzume::env::get/bytes/isSet/intValue`): `UZUME_<X>` only. Build/test-only variables (`UZUME_MELONDS_DS_CORE`, `UZUME_SHOT_DIR`, ...) are read directly.

## Build targets (ADR 0003, 0006, 0007)
- Targets: `uzume_core` (core/ + network/ sources), `uzume_emulation`, `uzume_input` + `uzume_sdl3` (input/), `uzume_media` + `uzume_audioutil` + `uzume_media_deps` (media/), `uzume_ui` (static lib, QML module `Uzume.App`), executables `uzume` (app/) and `uzume-cli` (network/cli/). Qt >= 6.4, 6.4 API only, not via vcpkg.
- Plugins (0.11, `docs/guides/plugin-api.md`): `uzume_lua` + `uzume_plugins` (`client/plugins/`, vendored Lua 5.4.7) and `uzume_formats` (`client/formats/`, uz.codec/uz.rom helpers); plugin data under `<data>/plugins` (`installed/`, `cache/`, `state.json`).
- Core path: CMake variable/environment `UZUME_MELONDS_DS_CORE` (`UZUME_DESMUME_CORE` for DeSmuME, test `emulation_core_desmume`); empty = tests with `NEEDS_CORE` are skipped. Never check in a core, ROM or BIOS.
