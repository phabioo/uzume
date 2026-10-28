# ADR 0019: GPU-direct NVENC encoding

- Status: Accepted (Fabio, via the 0.7.x GPU-direct request, 2026-10-09)
- Date: 2026-10-09
- Deciders: Fabio

## Context

A shared Session of a hardware-rendered game reads every frame back to the CPU, converts it to YUV and uploads it again to the encoder ([ADR 0013](0013-opengl-hardware-rendering.md) D3, [ADR 0006](0006-sessions-phase4.md)). At 8x internal resolution that is 2048x3072 pixels per frame, plus a conversion on the emulation or encoder side. Roadmap 0.7.x "GPU-direct hardware encoding (NVENC first)" ([roadmap.md](../roadmap.md)) asks for the core's OpenGL texture to reach the encoder without readback or CPU conversion. This ADR covers NVIDIA only. The Session design is in [04-sessions-and-multiview.md](../architecture/04-sessions-and-multiview.md).

Guiding rule: today's readback path stays the default and the safety net.

- GPU-direct takes over only after `h264_nvenc` has encoded one CUDA frame.
- Any problem sends the Session back to readback frames. It never stops the Session, never sets the sticky encoder failure and never shows an error toast.

This ADR amends ADR 0013 D3 ("every hardware frame is read back").

## Decisions

### D1 Scope

- NVIDIA only. The core's GL texture goes through CUDA-GL interop into FFmpeg CUDA frames (`RGB0`) and on to `h264_nvenc`. NVENC converts RGB to YUV on the GPU.
- Player only. The Hub, the protocol and the QML are unchanged. The handshake still advertises the encoders probed with CPU frames.

### D2 Module boundaries

- The interface `emu::GpuEncodeTarget` (header only) lives in `framebeam_emulation`. The CUDA and FFmpeg producer `CudaGlCapture` lives in `framebeam_media` and knows nothing about emulation. The adapter `GpuEncodeBridge` lives in `framebeam_ui`, which already links both.
- No new library edge.

### D3 No new build dependency

- An own CUDA shim (`client/media/cuda_shim.h`) declares the 18 driver functions FrameBeam needs, with the `_v2` names hard-coded. There is no CUDA SDK and no ffnvcodec. `cuda_shim.h` fails to compile when combined with the SDK's `cuda.h`.
- The driver is loaded at runtime (`libcuda.so.1`; `nvcuda.dll` from System32 only, never from the application directory) and never unloaded.
- `cuInit` runs once per process, only after the game's GL vendor is NVIDIA, during a share with a running encoder.
- No CI, script or cloud-hook change beyond the Windows skip allow-list (D12).

### D4 CUDA context

- An own CUDA context on the GL device (`cuGLGetDevices_v2`) with `BLOCKING_SYNC`. `cuGLGetDevices_v2` runs on the emulation thread with GL current. The context, stream, FFmpeg device and frames context are created on a `QThreadPool` thread, so a share has no creation hitch. Until then the capture reports "not ready" and readback keeps encoding.
- The context is handed to FFmpeg with `stream = NULL`. The device's free callback destroys it when the last frame or encoder is gone.

### D5 Encode texture

- A separate `GL_RGBA8` texture and FBO in `HwRenderContext`, a new texture object per size. It holds the frame downscaled and flipped by a generalised blit chain into at most 1280x1920, even, aspect kept, never upscaled. The size depends on the core's frame size only, not on the window, so a window resize does not reopen the encoder. An internal-resolution change does.
- Register and unregister only with GL current, always on the emulation thread. Detach comes before the texture is deleted. Without GL (context lost) the registration is leaked, never released.
- GL errors left by the core are drained before every encode-side check. `BindingGuard` saves and restores the read and draw framebuffer bindings separately (a separate commit).

### D6 Synchronisation

- Frame N is blitted in `handleVideo`, then `glFlush`. At the start of `runFrame` N+1, before `retro_run`, the emulation thread maps, copies and unmaps on its own non-blocking stream and calls `cuStreamSynchronize`.
- Only complete frames leave the emulation thread, so NVENC's stream choice does not matter. One frame of latency, the same as the display PBO ([ADR 0013](0013-opengl-hardware-rendering.md) D3 update).
- The cost is measured, not assumed (D11).

### D7 Hand-off

- Emulation thread, one-slot mailbox in the bridge, UI thread (the existing `GameSession::frameChanged` lambda), `SessionHost::pushGpuFrame`, the `EncodeWorker` queue (2 pending video jobs, drop oldest).
- The frame is a `std::shared_ptr<AVFrame>` (FFmpeg CUDA pool frame) that is complete in device memory. It never goes through a Qt signal, so no metatype is needed.
- At most 5 CUDA buffers exist at once (mailbox 1, queue 2, encoding 1, capturing 1). Nothing that may release a CUDA buffer runs under a mutex.
- pts is the `SessionHost` clock at push time, as for readback frames.
- A duplicate frame re-captures the last encode texture, so GPU frames stay 1:1 with displayed frames.
- No blit and no capture for frames that are not wanted for video (speed-up above the base rate) or while no viewer is connected.

### D8 Encoder

- A second `VideoEncoder` instance opened with `hw_frames_ctx` and `delay=0` (no 2-frame NVENC delay). The CPU encoder keeps running until the GPU encoder has encoded its first CUDA frame; only then is the CPU encoder closed. A failed GPU open therefore costs no extra open and no extra keyframe.
- The CPU path is unchanged, except that after a fatal CUDA error (D9) `h264_nvenc` is removed from the CPU encoder order.
- The 5 s reopen rule for bitrate changes applies in both modes. A runtime NVENC reconfigure is not used (see Not in scope).

### D9 Fallback

- GPU-path errors never set the sticky encoder failure. They switch to readback frames for the rest of the share. Detection paths: the bridge `fail()` (every GL-side drop), a failed map, copy or sync, a failed GPU open or encode in the worker, and a `SessionHost` watchdog that turns GPU input off only after 60 readback frames and at least 1 s without a GPU frame (a burst of queued frame events after a UI stall does not trip it). The log text stays `no GPU frames for 60 frames`.
- A failure is not retried within a share; a new share tries again.
- The 12 documented fatal CUresult codes (confirmed by a health check after a GPU encode or open failure) switch GPU-direct and `h264_nvenc` off for the process. Any other CUresult, including an unknown code after a GPU reset, is scoped to the share.
- Not applicable from the start, silent: a software-rendered game, a game whose OpenGL context is not on an NVIDIA GPU (this includes Optimus laptops where the game runs on the iGPU while NVENC would open on the dGPU), no `libcuda`, FFmpeg without CUDA input for `h264_nvenc`, `h264_nvenc` missing from the probed encoders, the kill switch or a forced encoder (D10).

### D10 Switches and gates

- `FRAMEBEAM_DISABLE_GPU_ENCODE=1` is the kill switch.
- A set `FRAMEBEAM_H264_ENCODER` (even `h264_nvenc`) keeps readback and is logged at share open. It is the A/B baseline for comparing against GPU-direct.
- A live-save restart keeps the GPU-direct bridge while the core restarts; the plan is re-evaluated when the game state changes.
- The path also requires a hardware-rendered game, FFmpeg CUDA input support for `h264_nvenc`, `h264_nvenc` in the probed caps, an open share with a running encoder, and a CUDA driver that is not unavailable or dead.

### D11 Observability

Exact strings (quoted as the code writes them; the `framebeam.sessionhost` lines and the host-line suffixes follow the design and are written with the Session wiring).

- Success, `framebeam.sessionhost`, info, at the first CUDA encode of each worker run:
  `GPU-direct encoding active: h264_nvenc takes CUDA frames 1280 x 1920 (no readback, no CPU conversion)`
- Failure, warning, once per share: `GPU-direct encoding off for this Session (<reason>); encoding readback frames`
- Not available, info: `GPU-direct encoding not available (<reason>); encoding readback frames`
- Not used at share open, info: `GPU-direct encoding not used: FRAMEBEAM_DISABLE_GPU_ENCODE=1`, `GPU-direct encoding not used: FRAMEBEAM_H264_ENCODER=<name> forces the readback path` or `GPU-direct encoding not used: CUDA unavailable (<reason>)`
- `framebeam.sessionhost`, info, every 10 s: `Encode timing (10 s): input gpu-direct, 600 frames, 0.4 ms/frame (max 1.2 ms), 0 dropped` (or `input readback`).
- `framebeam.gpuencode`:
  - `CUDA driver ready: 12.8 (cuInit 182 ms)`
  - `CUDA unavailable: <reason>`
  - `CUDA-GL interop on "<GPU name>": context ready in 74 ms (off the emulation thread)`
  - `GPU-direct capture unavailable: <reason>`
  - `GPU-direct capture failed at <context|stream|frames context|register|frame buffer|map|mapped array|copy|unmap|synchronize>: <describe>`
  - `CUDA error <NAME> (<code>) at <step> is fatal; GPU-direct encoding and h264_nvenc stay off until the Player restarts`
  - `GPU copy timing (10 s): 600 captures, mean 0.21 ms, max 0.90 ms (map max 0.12 ms, sync max 0.70 ms)`
- `framebeam.videoencoder`: `H.264 encoder h264_nvenc 1280 x 1920 @ 60 2000 kbit/s (CUDA frames)` (also on bitrate reopens) and `Encoder h264_nvenc does not open with CUDA frames: <av_strerror>`.
- `framebeam.emulation.hw`:
  - `Session encode texture 1280x1920 for a 2048x3072 frame`
  - Warning, real failures only: `Session encode target dropped (<attach failed|blit failed|capture failed|encode texture could not be created>); the Session encoder gets readback frames`
  - Info, not applicable (the game's OpenGL context is not on an NVIDIA GPU, D9; `GpuEncodeTarget::Attach::Unavailable`; the target is released): `Session encode target not applicable; the Session encoder gets readback frames`
- Diagnostics overlay, Streaming host row, line 2: ` · GPU-direct` is appended while the GPU input is active, ` · readback (GPU-direct off)` after a failure. Example: `Encoder h264_nvenc · H.264 · 6.0 / target 6.5 Mbit/s · 60.0 fps · GPU-direct`. Otherwise the string is byte-identical to today's.
- Diagnostics overlay, Emulation Frame row: with GPU captures it reads `9.4 ms · emu 7.1 · readback 0.4 (60/s) · GPU copy 0.3 (60/s)`; otherwise unchanged. "GPU copy" is the emulation thread's time for the encode blit and the capture.

### D12 Tests

- CI has no GPU and no CUDA. A fake target, a fake `cuda::Api` (enforces GL-current, context-pushed, map/unmap pairing and texture liveness) and a host-memory frame pool run the real GL side and the real `CudaGlCapture` code on Mesa llvmpipe under xvfb.
- The real-GPU test `media_cuda_gl_hw` exits 77 (skipped) without an NVIDIA GPU. It is an optional local check: the test executables are not part of the Windows artifact, so it runs only in a local build. It resizes to 192x144 (see Consequences, NVENC minimum size).
- The Windows CI skip allow-list (which already holds `emulation_hw_render` and `emulation_core_gl`) gains `media_cuda_gl_capture`, `media_cuda_gl_hw` and `ui_gpu_encode`, because Windows runners have no GL 3.3 context. Linux needs no change, because ctest treats exit 77 as skipped.

### D13 ADR 0013 D3 amended

- While a Session is shared and GPU-direct is active, frames reach the encoder without a readback and without a CPU conversion. The display readback remains, sized to the view instead of the Session encode size.
- Without GPU-direct (every case in D9 and D10), D3 applies unchanged.

## Not in scope

- QSV and AMF. They need the OpenGL to D3D11 bridge (roadmap "Direct GPU display"). Open.
- The OpenGL to D3D11 bridge (`WGL_NV_DX_interop2`). Open.
- Direct GPU display of the local game view (Qt Quick scene graph interop). Open, unchanged.
- Runtime NVENC reconfigure (bitrate or size without reopen): FFmpeg forces an IDR on it anyway, so the 5 s reopen rule stays.
- Changing the CPU-mode NVENC probe (a System32 preload of `nvcuda.dll` in the capability probe is deferred to its own change).
- Other GPU vendors and software encoders: they keep the readback path.

## Rejected

- Dropping a pending capture older than 250 ms after a pause: the pending capture is frame N, the newest game state, and viewers have not received it. The display PBO is also one frame behind and pts is stamped at push time, so the frame is not stale. Dropping it would show the first frame after resume one frame later than the local display.
- Map right after the blit in the same frame: whether `cuGraphicsMapResources` blocks the CPU until the frame's GL work is done is unverified. Deferring to N+1 lets the GPU finish while the emulation thread sleeps and keeps the Session picture in step with the display.
- `delay=0` and a runtime bitrate change on the CPU-mode NVENC path: the safety-net path stays as verified, and a reconfigure forces an IDR anyway.
- A forced `FRAMEBEAM_H264_ENCODER=h264_nvenc` enabling GPU-direct: a forced encoder is the A/B baseline.
- ffnvcodec instead of an own shim: it is not on Linux CI or in the cloud hook (a new mandatory build dependency on both platforms), and its bare `LoadLibrary("nvcuda.dll")` is a DLL-planting risk for a portable app. FrameBeam needs 18 functions.
- Creating the CUDA context on the emulation thread: context creation takes tens of milliseconds, a visible hitch per share.
- Gating on the GL vendor alone: NVIDIA GL without NVENC would cost a failed open and a keyframe per share. The probed caps and FFmpeg CUDA input support are checked too.
- Closing the CPU encoder before the GPU open: a failed GPU open would cost a third NVENC open and a gap. The CPU encoder closes only after the first CUDA encode.
- Retrying GPU-direct within a share: simpler, and a failure is likely to repeat. A new share retries.
- Letting a GL-side drop only detach: viewers would see a frozen picture, because the UI would keep skipping readback frames. Every drop calls `fail()`, and the watchdog covers the unforeseen.

## Consequences

- VRAM: up to five CUDA frames (about 10 MB each at 1280x1920), the encode texture and the chain levels, about 60 MB in total. They are held while the share is open, also while no viewer is connected.
- Double encoder open: every encoder run opens NVENC twice (CPU frames first, then CUDA frames) and sends two keyframes, about 100 to 200 ms before GPU mode starts. Two Players on one GPU count against its NVENC session limit; an open that fails for that reason falls back to readback.
- WGL risk: CUDA-GL interop with WGL and `QOffscreenSurface` on Windows is unproven; no reference project does it. `cuGLGetDevices_v2`, register or map may fail, and the Session then silently stays on readback. The beta log lines and the picture (Verification) answer this on real hardware; if it fails there, Sessions stay on readback and a follow-up fixes it.
- Other unverified assumptions: the CUDA array byte order equals GL `RGBA8` (`RGB0`), NVENC's RGB to YUV matrix (BT.601 limited per FFmpeg) matches the CPU path, and the emulation-thread stall of `cuGraphicsMapResources` under a saturated GPU. The frame row and the 10 s lines (D11) measure the last one; there is no automatic switch.
- Driver floor: the Windows FFmpeg (NVENC API 13.0) needs driver 570 or newer; older drivers fail the GPU open and fall back to readback, like today's CPU NVENC.
- NVENC minimum size: on Turing and newer, NVENC rejects H.264 below 145x49. Encode textures of real cores are larger; a smaller one fails the GPU open and the Session falls back to readback frames.
- Verification happens on the beta. CI has no NVIDIA GPU, so it covers the GL side and the call sequence with fakes (D12). The repo auto-merges every PR when CI is green and each merge becomes a beta; Fabio checks on his NVIDIA GPU with that beta. The merge does not wait for him: if GPU-direct misbehaves, `FRAMEBEAM_DISABLE_GPU_ENCODE=1` turns it off, and every failure already falls back to readback. What he checks:
  - `media_cuda_gl_hw` (optional, local build only; not part of the Windows artifact) passes. If it fails, Sessions stay on readback and a follow-up fixes it.
  - `player.log` shows, in this order, `CUDA driver ready: …`, `CUDA-GL interop on "<GPU name>": …`, `H.264 encoder h264_nvenc … (CUDA frames)` and `GPU-direct encoding active: …`; the host row ends with `· GPU-direct`.
  - The viewer picture has the right colours and orientation and matches the picture with `FRAMEBEAM_DISABLE_GPU_ENCODE=1`.
  - Lifecycle cases (speed-up, back to the library and resume, last viewer leaves and rejoins, stop and share again, quit while shared, internal-resolution change, window resize, two viewers, bitrate adaptation).
- Verified by Fabio on 2026-10-11 on an RTX 4080 SUPER (driver 617.42, CUDA 13.4) with a beta build. The log showed the four lines in order, GPU-direct was active about 0.25 s after the viewer joined, the picture matched the readback picture, and all lifecycle cases passed, including two viewers. GPU copy took about 0.3 ms per frame and NVENC about 1.8 ms per frame at 1280x1920; readback with `FRAMEBEAM_DISABLE_GPU_ENCODE=1` took 3.6 ms per frame at 1024x1536. At 8x speed-up the viewer gets slightly fewer than 60 fps on both paths, so that is not a GPU-direct issue. The roadmap item is done; QSV and AMF stay open.
- User-visible additions: the host-line suffixes and the `GPU copy` part of the Frame row (D11), and two environment variables, `FRAMEBEAM_DISABLE_GPU_ENCODE` and `FRAMEBEAM_H264_ENCODER` ([player guide](https://github.com/phabioo/uzume/wiki/Using-the-app)).
