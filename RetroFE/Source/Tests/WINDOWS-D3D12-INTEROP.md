# Windows D3D12 video interop

Build with GStreamer 1.28 or newer (development SDK and matching runtime).
`RETROFE_ENABLE_D3D12` defaults to ON; older SDKs retain the D3D11 implementation.
The implementation was tested against GStreamer 1.28.7 and unmodified SDL 3.4.16.

With D3D12 compiled in, an unspecified SDLRenderDriver tries direct3d12 then
direct3d11 at renderer creation. Explicit renderer settings remain authoritative.
To select either path for testing:

```ini
HardwareVideoAccel=true
SDLRenderDriver=direct3d12
log=INFO,WARNING,ERROR
```

Use `SDLRenderDriver=direct3d11` for the existing D3D11 backend. A per-video native
interop failure requests software decoding on the worker thread; it does not replace
an already-live renderer. Submission/device errors stop rendering rather than
continuing with resources whose synchronization is uncertain.

## Ownership and ordering

- GStreamer owns the decoder device, output surfaces and producer fence.
  SDL owns its renderer, graphics queue and three persistent NV12 destinations per configuration (up to four cached configurations).
- Decoder resources are opened on SDL's device on the same adapter; opened
  resources are cached on the GstMemory lifetime, not a reusable handle value.
- Copies use the plane subresource indices supplied by GStreamer. Source
  resources must permit simultaneous access and must not be reference-only.
- Updates prepare the latest frame. `SDL::beginVideoFrame` submits copies on
  SDL's graphics queue before the next frame's SDL draw commands. This must run
  before any video draws, after the previous frame's presentation.
- Source samples remain referenced until a copy fence completes. Busy rings
  drop incoming frames without CPU waiting or triggering upload fallback.
- Retarget/unload cancel pending frames and poll submitted copies without waiting.
  Submitted sources remain owned until their fence completes. Compatible rings
  are reused across size/colorspace changes; exceeding the cache requests fallback.
- Native producer readiness is polled before submission. Multiple pending updates
  coalesce into the newest frame; an unsubmitted texture is never published.
- Initialization uses one black NV12 upload per texture and a one-pixel readback
  per ring allocation. This establishes/submits SDL's internal resource state.
  Steady playback has no CPU pixel uploads/readbacks. SDL_FlushRenderer alone
  does not submit the D3D12 command list in SDL 3.4.16.

Native format support is NV12, including bounded crops with even origins.
P010/HDR and cross-adapter resources use worker-side software fallback.

## Validation

From RetroFE/Source after a Release build, set the staged plugin paths and run:

```powershell
$runtime = (Resolve-Path ../Build/tests/Release).Path
$env:GST_PLUGIN_PATH = "$runtime/gst-plugins"
$env:GST_PLUGIN_SYSTEM_PATH = $env:GST_PLUGIN_PATH
$env:GST_PLUGIN_SCANNER = "$runtime/gst-plugin-scanner.exe"
$env:GST_REGISTRY = "$env:TEMP/retrofe-d3d12-test-registry.bin"
$env:RETROFE_TEST_RENDERER = 'direct3d12'
& "$runtime/retrofe_sdl3_smoke_tests.exe" ../../Package/Environment/Common --hardware
```

Repeat with RETROFE_TEST_RENDERER=direct3d11 for regression coverage. The hardware
test checks native playback, seven concurrent decoders across three unload/reopen
cycles, advancing frames, and stale-frame suppression on list recycling. Its
concurrent playback loop has no readbacks to force GPU completion each frame.
The runtime log records decoder selection, interop activation, and allocation.

Before deploying broadly, run real attract-mode/playlist switching overnight on
both discrete and integrated GPUs, including different sizes, codecs and PARs.
The short automated test cannot establish overnight stability or N100 behavior.

The smoke now covers delayed native fences, pending-frame supersession, source
ownership after invalidation, and alternating cached dimensions. D3D11 checks
include renderer-device isolation and pixel verification of a nonzero crop.
See [the regression matrix](VIDEO-INTEROP-REGRESSIONS.md).
