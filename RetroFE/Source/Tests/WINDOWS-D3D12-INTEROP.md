# Windows D3D12 video interop

Build with GStreamer 1.28 or newer (development SDK and matching runtime).
`RETROFE_ENABLE_D3D12` defaults to ON; older SDKs retain the D3D11 implementation.
The implementation was tested against GStreamer 1.28.7 and unmodified SDL 3.4.12.

With D3D12 compiled in, an unspecified SDLRenderDriver tries direct3d12 then
direct3d11 at renderer creation. Explicit renderer settings remain authoritative.
To select either path for testing:

```ini
HardwareVideoAccel=true
SDLRenderDriver=direct3d12
log=INFO,WARNING,ERROR
```

Use `SDLRenderDriver=direct3d11` for the existing D3D11 backend. A per-video native
interop failure uses the existing CPU upload/recovery path; it does not replace
an already-live renderer. Submission/device errors stop rendering rather than
continuing with resources whose synchronization is uncertain.

## Ownership and ordering

- GStreamer owns the decoder device, output surfaces and producer fence.
  SDL owns its renderer, graphics queue and three persistent NV12 destinations.
- Decoder resources are opened on SDL's device on the same adapter; opened
  resources are cached on the GstMemory lifetime, not a reusable handle value.
- Copies use the plane subresource indices supplied by GStreamer. Source
  resources must permit simultaneous access and must not be reference-only.
- Updates prepare the latest frame. `SDL::beginVideoFrame` submits copies on
  SDL's graphics queue before the next frame's SDL draw commands. This must run
  before any video draws, after the previous frame's presentation.
- Source samples remain referenced until a copy fence completes. Busy rings
  drop incoming frames without CPU waiting or triggering upload fallback.
- Retarget cancels pending old frames. Unload drains submitted copies but keeps
  compatible textures/allocators; size or colorspace changes rebuild the ring.
- Initialization uses one black NV12 upload per texture and a one-pixel readback
  per ring allocation. This establishes/submits SDL's internal resource state.
  Steady playback has no CPU pixel uploads/readbacks. SDL_FlushRenderer alone
  does not submit the D3D12 command list in SDL 3.4.12.

Initial native format support is NV12. P010/HDR, cross-adapter resources and
unexpected crop metadata are not handled by this backend. Do not advertise crop
metadata support to the decoder until cropping is implemented here.

## Validation

From RetroFE/Source after a Release build, set the staged plugin paths and run:

```powershell
$runtime = (Resolve-Path ../Build/tests/Release).Path
$env:GST_PLUGIN_PATH = $runtime
$env:GST_PLUGIN_SYSTEM_PATH = $runtime
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
