# ADR 0027: GPU paths (zero-copy display, GPU-direct QSV and AMF)

- Status: Accepted
- Date: 2026-10-13
- Deciders: Fabio

## Context

Hardware-rendered games are read back to the CPU for every frame, for the game view and for the Session encoder ([ADR 0013](0013-opengl-hardware-rendering.md) D3). [ADR 0019](0019-gpu-direct-nvenc.md) removed the readback and the CPU conversion for the encoder on NVIDIA only (CUDA-GL interop). Everything else still pays for readback, QImage upload and, in a shared Session, a CPU conversion; Intel QSV via readback measured about 5-6 ms per frame on a tester's notebook (2026-10-11). Roadmap 0.15 "GPU paths" ([roadmap.md](../roadmap.md)) asks for two things on Windows: show the game's frame in the game view straight from the GPU, and GPU-direct encoding for AMD and Intel. Both need an OpenGL to D3D11 bridge (`WGL_NV_DX_interop2`).

Guiding rule (as ADR 0019): readback stays the default and the safety net. Any failure falls back silently (log only); nothing stops a game or a Session.

This ADR amends ADR 0013 D3 and the "Not in scope" list of ADR 0019 (D10). Code only; no QML layout change (diagnostics strings are C++ text).

## Decisions

### D1 Scope and gates

- Windows only. Linux (Qt Quick on OpenGL) and any other Qt Quick graphics API keep readback.
- Zero-copy display requires:
  - a hardware-rendered game;
  - Qt Quick on Direct3D 11 (`QQuickWindow::graphicsApi()`); any other API makes the display "not available (Qt Quick renders with <api>)";
  - `WGL_NV_DX_interop2` in the WGL extension string;
  - `wglDXOpenDeviceNV` succeeds for our D3D11 device on Qt Quick's adapter (it fails when the game's GL context is on another adapter, for example on Optimus or hybrid laptops; the game view then keeps readback);
  - not `FRAMEBEAM_DISABLE_ZERO_COPY=1`.
- GPU-direct QSV and AMF require the conditions of GPU-direct NVENC (ADR 0019 D10) except CUDA, plus an open bridge and:
  - GL vendor not NVIDIA (NVIDIA stays on CUDA),
  - adapter vendor 0x1002 (AMD) with `h264_amf` in the probed encoders, or 0x8086 (Intel) with `h264_qsv` in the probed encoders.
- No `NvOptimusEnablement` or `AmdPowerXpressRequestHighPerformance` exports. They change the GPU choice of laptop users; out of scope.

### D2 Module layout

- `framebeam_emulation`: the header-only interface `emu::GpuDisplayTarget` (`gpu_display_target.h`). `HwRenderContext` gets a display path; `LibretroBackend`, `EmulatorBackend` and `EmulationRunner` get `setGpuDisplayTarget`, `setDisplayLimit` and `setReadbackEnabled`.
- `framebeam_media` (Qt Core only, no Qt Quick headers; compiled on all platforms, bodies under `_WIN32`, elsewhere "Unavailable"): `d3d11glbridge` (device, interop, GL loader), `d3d11displayring` (producer and consumer), `d3d11encodecapture` (AMF/QSV capture, same shape as `CudaGlCapture`), `displayslots.h` (pure slot state machine) and `gpudisplay.h` (platform-neutral interfaces). Windows links `d3d11 dxgi opengl32`.
- `framebeam_ui`: `GpuDisplayBridge` (adapter between the emulation target and the ring, state, confirmation), `DisplayTextureSource` (seam used by `GameView`; the Windows implementation wraps ring textures with `QNativeInterface::QSGD3D11Texture::fromNative`, a test implementation wraps an image so the GPU path runs under the software backend in CI), `makePlatformDisplay()` (Windows implementation or a stub) and a D3D11 backend in `GpuEncodeBridge` next to CUDA.

### D3 The bridge

- `GlD3d11Bridge` is one per game and shared by display and encoding. `GameSession` creates it on the GUI thread; the constructor does no D3D or GL work.
- It uses our own `ID3D11Device` (BGRA support, video support, multithread protected), never Qt's. Which adapter:
  - The display opens it on Qt Quick's adapter LUID, reported by the `GameView` from the render thread once the scene graph exists. Until then `open()` returns "not ready".
  - D3D11 encoding without a display bridge (for example with `FRAMEBEAM_DISABLE_ZERO_COPY=1`, or Qt Quick not on D3D11) opens its own bridge on "any adapter" the game's GL context can open. So `FRAMEBEAM_DISABLE_ZERO_COPY=1` does not disable QSV or AMF.
- `open()` runs once on the emulation thread with the game's GL current: check the extension, resolve `wglDX*`, create the device, `wglDXOpenDeviceNV`. Result Ok, NotReady, Unavailable (reason) or Failed (reason).
- Thread rules: every `wglDX*` and GL call on the emulation thread with GL current. D3D11 calls of the producer and the encode capture on the emulation thread; `avcodec_send_frame` on the encode worker uses the same device (multithread protected). Consumer calls only on Qt's device on the render thread.
- Teardown: encode target, display target, bridge, then GL objects. Objects are unregistered only with GL current, else they are leaked.
- `GameSession` recreates the platform display at every game launch, also on a live-save restart, unless the display of that game already failed or was unavailable.

### D4 Display ring and keyed-mutex protocol

- 3 slots, each `R8G8B8A8_UNORM` (Qt's `fromNative` views RGBA8), shader-resource and render-target, NT shared handle with a keyed mutex, registered for GL with write-discard access.
- Grow-only: the slot size is the largest requested size rounded up to 64; a frame uses the top-left rectangle. Growth creates a new generation of 3 slots, the consumer drops the old one after it switched.
- Orientation: the image's top row is GL row 0 = D3D row 0; the blit flips the bottom-left core frame.
- Producer, per hardware frame on the emulation thread:
  1. `acquire(w, h)`: a slot that is neither shown nor pending; `AcquireSync(0, 0)`; on timeout or no free slot the frame is skipped and counted; lock the interop object.
  2. Blit the core frame (flip and halving chain) into the top-left w x h, `glFlush`.
  3. `present(w, h)`: unlock, `ReleaseSync(1)`, publish {generation, slot, w, h, sequence}; newest wins. `abort()` instead when the blit failed.
- Consumer (render thread, one per window and ring, created by the first `GameView` that draws): opens the shared handles on Qt's device. Once per Qt frame, a newer published slot is acquired with key 1 and becomes "shown"; older pending slots are acquired and released at once; the previous shown slot is released with key 0 after the frame (`afterFrameEnd`). On timeout the old slot stays.
- `GameView` draws one image node (one per screen for split layouts) from the texture with a source rectangle; linear filtering when the texture is drawn smaller than it is, nearest otherwise. After the first frame drawn from the GPU texture it calls `confirmShown()`.
- No GPU wait after the encode capture: only `Flush`. The keyed mutex orders display access.

### D5 Readback policy

- `GpuDisplayBridge::State`: Pending, Active (a GPU frame was drawn), Unavailable, Failed.
- Pure plan `planReadback(displayActive, viewLimit, shareSize)`, unit-tested as a truth table:
  - display not active: readback on, limit as before ADR 0027;
  - display active and a share size (shared Session without GPU-direct encoding): readback on, limit = share size;
  - display active and no share size: readback off.
  The display blit limit is the largest reported view size.
- With readback off, `frameReady` is still emitted for every displayed frame (with a stale image), so `GameView` updates, the GPU encode mailbox is pulled and `SessionHost` keeps its watchdog and encoder start. `sourceFrameSize` is stored on every hardware frame.
- A display failure (producer or consumer) sets the state to Failed for this game; readback resumes on the next frame and the `GameView` draws the QImage again. It is not retried within the game; the next game start tries again.
- GPU frames for AMF and QSV are tried only when the GL vendor is not NVIDIA.

### D6 GPU-direct QSV and AMF

- The `GpuEncodeTarget` contract is unchanged. `GpuEncodeBridge` chooses the backend at creation: CUDA (NVIDIA GL vendor, ADR 0019) or D3D11 (bridge open, AMD or Intel).
- `D3d11EncodeCapture` runs on the emulation thread: an interop texture `B8G8R8A8_UNORM` of the encode size (FFmpeg D3D11 frames have no RGBA software format), blit 1:1 from the encode texture, then:
  - AMF: a BGRA D3D11 frame from the FFmpeg frames context, `CopySubresourceRegion`, `Flush`.
  - QSV: an NV12 D3D11 frame (render target, dynamic pool, 16-aligned size); the D3D11 video processor converts BGRA to NV12 with BT.601 studio output colour space; the frame is mapped to a QSV frame (derived QSV device and frames context); `Flush`.
- Frames are published to the existing mailbox as `std::shared_ptr<AVFrame>`; the FFmpeg pool and buffer references protect reuse.
- `VideoEncoder::openGpu`: CUDA/RGB0 opens `h264_nvenc` (unchanged); D3D11/BGRA opens `h264_amf` (async_depth 1, ultralowlatency, SMPTE170M, MPEG range); QSV/NV12 opens `h264_qsv` (async_depth 1, bf 0, low_delay_brc, look_ahead 0). The encoder reopens when `hw_frames_ctx` changes; a frame of another frames context is never sent to an open encoder.
- Fallback, watchdog and the fatal-CUDA rule are as in ADR 0019 D9; the fatal-CUDA rule stays CUDA-only.

### D7 Switches

- `FRAMEBEAM_DISABLE_ZERO_COPY=1`: no zero-copy display; the game view uses readback frames. It does not disable QSV or AMF (D3).
- `FRAMEBEAM_DISABLE_GPU_ENCODE=1` and a forced `FRAMEBEAM_H264_ENCODER` keep readback for encoding, for all three encoders (ADR 0019 D10).

### D8 Observability

Exact strings as the code writes them.

- `framebeam.gpubridge`, info:
  - `GL-D3D11 bridge on "<adapter>" (vendor 0x1002) ready in 12 ms`
  - `GL-D3D11 bridge unavailable: <reason>`
- `framebeam.gpudisplay`, info, once per game:
  - `Zero-copy display active: D3D11 textures on "<adapter>" (no readback for the game view)`
  - `Zero-copy display off for this game (<reason>); the game view uses readback frames`
  - `Zero-copy display not available (<reason>); the game view uses readback frames`
- `framebeam.gpuencode`:
  - `D3D11 capture on "<adapter>": h264_amf, BGRA frames ready in 30 ms` (QSV: `h264_qsv, NV12 frames`)
  - warning: `D3D11 capture failed at <step>: <hresult or detail>`
- `framebeam.sessionhost`, info, at the first GPU encode: `GPU-direct encoding active: h264_amf takes D3D11 frames 1280 x 1920 (no readback, no CPU conversion)`; for QSV `h264_qsv takes QSV frames ...`. The other GPU-direct lines of ADR 0019 D11 apply to all encoders.
- Diagnostics overlay, Frame row: with zero-copy and no readback `9.4 ms · emu 7.1 · zero-copy 0.2 (60/s)`; with both parts `... · readback 1.1 (60/s) · zero-copy 0.2 (60/s)`. Resolution row: ` → shown W×H` replaces ` → read back W×H` while no readback runs. The Streaming host row suffix ` · GPU-direct` is unchanged for all three encoders.

### D9 Tests

- Linux CI (llvmpipe, xvfb): a fake `GpuDisplayTarget` (own GL texture, enforces lock/unlock pairing, orientation, size, present after acquire) runs through the real `HwRenderContext` and `LibretroBackend` with the fake hardware core; readback gating (off while active and not shared, resumes on failure, `frameReady` still emitted, `sourceFrameSize` updated); the `planReadback` truth table; the slot state machine; `GameSession` wiring with a fake display; `GameView` drawing from a fake `DisplayTextureSource` including split layouts and `confirmShown()`; `GpuEncodeBridge` backend selection; `VideoEncoder` option mapping; diagnostics strings.
- Windows CI: everything compiles; a ring producer/consumer test on two WARP devices (exit 77 when WARP lacks shared resources, on the skip allow-list in `ci.yml`). The interop and the encoders are verified only on real GPUs.
- No real GPU, driver, ROM or game file is used in tests.

### D10 Amendments

- ADR 0013 D3 ("every hardware frame is read back"): on Windows, when zero-copy display is Active, the game view is drawn from the GPU texture and readback runs only while a CPU consumer needs pixels (a shared Session without GPU-direct encoding) or until the first GPU frame is shown. In every other case D3 applies unchanged.
- ADR 0019 "Not in scope": QSV and AMF, the OpenGL to D3D11 bridge and direct GPU display of the local game view are now decided here. Other GPU vendors and software encoders keep readback.

## Not in scope

- Linux zero-copy; D3D cores (Vulkan display and encode: [ADR 0034](0034-linux-app-and-vulkan.md) D8, D9); NVENC via D3D11 frames for OpenGL games (NVIDIA stays on CUDA; D3D11 frames are used for Vulkan games only, ADR 0034 D9); HEVC and AV1; Optimus exports (D1).
- Zero-copy for Multiview remote tiles (decoded on the CPU).
- Runtime encoder reconfigure (ADR 0019).

## Rejected

- Opening Qt's own D3D11 device for interop: interop calls run on the emulation thread with the game's GL current, while Qt's device belongs to the render thread. Our own device on Qt's adapter plus shared textures keeps each side on its thread.
- Linux shared-context zero-copy now: it needs a GL context shared with Qt Quick, which breaks the isolated core context of ADR 0013. Deferred.
- GPU NV12 via QSV `RGB4` input: not used. The D3D11 video processor converts BGRA to NV12 explicitly, with a chosen colour space (BT.601 studio, as the CPU path).
- Optimus exports (`NvOptimusEnablement`, `AmdPowerXpressRequestHighPerformance`): they change the GPU choice for laptop users. Hybrid laptops where the game runs on another adapter than Qt Quick keep readback.

## Consequences

- VRAM: 3 display slots at the view size (grow-only) plus, for GPU-direct QSV/AMF, a few encoder frames and the encode texture. They are held while the game runs or the share is open.
- A game whose GL context is on another adapter than Qt Quick (hybrid laptops) keeps readback for the game view; D3D11 encoding can still open its own bridge on an adapter the GL context can open.
- Windows only; the Linux Player is unchanged.
- QSV and AMF open the encoder twice, like NVENC (CPU frames first, then GPU frames).
- Verification happens on a beta. CI has no GPU; it covers the GL side, the protocol and the call sequence with fakes (D9). Real GPUs, drivers and the interop are verified only locally.
- User-visible additions: `FRAMEBEAM_DISABLE_ZERO_COPY`, the `zero-copy` part of the Frame row and ` → shown W×H` ([player guide](https://github.com/phabioo/uzume/wiki/Using-the-app)).

### Verification on hardware (for Fabio)

- NVIDIA: zero-copy display.
- AMD: zero-copy display and GPU-direct AMF.
- Intel: zero-copy display and GPU-direct QSV.
- `player.log` lines in order: `GL-D3D11 bridge on ...`, `Zero-copy display active: ...`, `D3D11 capture on ...` (AMD, Intel), `H.264 encoder h264_amf|h264_qsv ...`, `GPU-direct encoding active: ...`.
- Orientation and colours of the game view and of the viewer picture match the picture with `FRAMEBEAM_DISABLE_ZERO_COPY=1` and `FRAMEBEAM_DISABLE_GPU_ENCODE=1`.
- Split layouts (DS two screens, 3DS), window resize, 8x speed-up, background game and resume, live-save restart (the display comes back or logs why not), start and stop of a share while the game runs.
- Diagnostics: Frame row shows `zero-copy` and no `readback` while not shared; Resolution row shows `→ shown W×H`; the host row ends with `· GPU-direct` on AMD and Intel.
