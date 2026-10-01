# Video interop regression checks

The September 2026 changes keep producer waits and hardware-frame downloads off
the UI thread, retain sources until GPU copies finish, and reuse bounded
presentation allocations. A deferred frame preserves the previous presentation.
Vulkan retires renderer-wide allocations at the frame boundary and polls only
the selected three-slot ring during an import. D3D12 skips copy-fence polling
for idle instances and reuses its per-frame registry snapshot storage.

## Windows smoke matrix

Build with `RETROFE_BUILD_TESTING=ON`. From `RetroFE/Source`, use the staged
runtime and the package's Common directory:

```powershell
$runtime = (Resolve-Path ../Build/tests/Release).Path
$assets = (Resolve-Path ../../Package/Environment/Common).Path
$env:GST_PLUGIN_PATH = "$runtime/gst-plugins"
$env:GST_PLUGIN_SYSTEM_PATH = $env:GST_PLUGIN_PATH
$env:GST_PLUGIN_SCANNER = "$runtime/gst-plugin-scanner.exe"
$env:GST_REGISTRY = "$env:TEMP/retrofe-interop-regression-registry.bin"
$env:RETROFE_TEST_RENDERER = 'direct3d12' # repeat with direct3d11
& "$runtime/retrofe_sdl3_smoke_tests.exe" $assets --hardware
$env:RETROFE_TEST_VIDEO_BACKEND = 'ffmpeg' # repeat both renderers
& "$runtime/retrofe_sdl3_smoke_tests.exe" $assets --hardware
# Run without --hardware to cover software upload/playback as well.
```

D3D11 checks include independent renderer devices, rejecting foreign-device
surfaces, and verifying pixels from a crop with a nonzero origin. D3D12 checks
include an unfinished producer fence, superseding pending frames, releasing
owners after copy completion, invalidation without draining, and alternating
dimensions while reusing at most three wrappers per configuration.

## Vulkan smoke matrix

For the GStreamer 1.28.7 upstream backports, also run
[Test-UpstreamBackports.ps1](../Prototypes/GStreamerVulkan/Test-UpstreamBackports.ps1)
in an x64 Developer PowerShell. Its ten upstream checks cover planar upload,
I420/A420 download pixels, output stride and cumulative image-pool offsets.
The [backport manifest](../Prototypes/GStreamerVulkan/UPSTREAM-BACKPORTS.md)
records the selected commits and build artifacts.

Use the patched GStreamer library and plugin together, as described in the
[Vulkan build instructions](../Prototypes/GStreamerVulkan/README.md).
With that build's `bin/Release` directory as `$runtime`:

```powershell
$env:GST_PLUGIN_PATH = "$runtime/gst-plugins"
$env:GST_PLUGIN_SYSTEM_PATH = $env:GST_PLUGIN_PATH
$env:GST_PLUGIN_SCANNER = "$runtime/gst-plugin-scanner.exe"
$env:GST_REGISTRY = "$env:TEMP/retrofe-vulkan-regression-registry.bin"
& "$runtime/retrofe_vulkan_interop_smoke.exe" clip.h264 h264 90 2 2 alternate flush
& "$runtime/retrofe_vulkan_interop_smoke.exe" clip.h265 h265 90 3 2 alternate flush
& "$runtime/retrofe_ffmpeg_vulkan_interop_smoke.exe" clip.mp4 readback.bmp
```

The GStreamer smoke uses two simultaneous decoders, alternating visibility,
repeated presents, and intermediate SDL flushes. It rejects DPB reference images
and checks UI presentation while a synthetic producer remains unfinished.
Both Vulkan smokes recreate interop objects during playback with old wrappers
still in SDL's queued draw batch and require reuse of the same three wrappers. FFmpeg also verifies its reserved graphics queue,
delayed producer handling, retained frames, and nonuniform rendered pixels.

The current Windows runs pass with SDL 3.4.16, GStreamer 1.28.7, the patched
Vulkan DLLs, and FFmpeg 8/9 SDK builds. These smoke checks establish ordering and
bounded reuse; they do not replace extended memory profiling or comparison
against software-decoded reference pixels. Existing bitstream-specific Vulkan
decoder corruption remains documented in the prototype README.

## Remaining runtime limits

D3D12's first allocation still uses a black upload and one-pixel readback to
establish SDL resource state. Warm configurations are reused; cache exhaustion
requests software decoding. Vulkan allocations persist until renderer shutdown
to avoid SDL wrapper destruction stalls and pipeline accumulation. Teardown and
exceptional driver-error recovery can wait for GPU completion.

GL/EGL pressure, unload, and resize now poll rather than drain. Missing GStreamer
GL sync metadata requests worker-side software fallback. These changes still
need Linux runtime validation; see [Linux checks](LINUX-VIDEO-INTEROP.md).
