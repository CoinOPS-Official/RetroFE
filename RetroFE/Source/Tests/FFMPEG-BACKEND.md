# Optional FFmpeg video backend

GStreamer remains the default. FFmpeg implements the same IVideo controls and
participates in the existing VideoPool and VideoComponent lifecycle. Backend
selection is a startup setting, not a live switch between populated pools.

## Build and select

FFmpeg 8+ development headers and libraries are required. On Windows:

```powershell
./Build.ps1 -EnableFFmpeg -FFmpegRoot C:/ffmpeg
```

Alternatively, enable `RETROFE_ENABLE_FFMPEG=ON` in CMake. Set `FFMPEG_ROOT` to a
development SDK prefix; Unix builds can find installed libraries in normal
system paths. Windows runtime staging includes the FFmpeg DLLs and SDK license.
This remains a dual-backend build, so GStreamer is still a build/runtime dependency.

```ini
VideoBackend=ffmpeg
HardwareVideoAccel=true
SDLRenderDriver=direct3d12
log=INFO,WARNING,ERROR
```

Use `VideoBackend=gstreamer` to return to the existing backend. Selecting FFmpeg
in a build without the feature produces an explicit initialization error.

## Playback and reuse

- One worker per retained instance owns demuxing, codec contexts and resampling.
  Opening, seeking and retargeting are asynchronous and interrupt obsolete work.
- A bounded three-frame video queue preserves AVFrame ownership. Presentation
  follows media timestamps on a monotonic clock; late frames can be skipped.
- Audio is converted to the existing AudioBus interleaved float format. The
  presentation clock schedules audio blocks; this is not an audio-device-clock
  feedback implementation. Long-running A/V drift still needs real playback testing.
- Paused preroll, pause/resume, 60-second and five-percent seeking, rewind/pause,
  finite/infinite loops, volume, soft overlay and perspective are implemented.
- Unload invalidates old pixels immediately and retains allocations. Same-file
  reopen seeks/flushes retained demuxer and decoder contexts. Different files open
  new media contexts while retaining the hardware device and compatible textures.
- Stop closes media asynchronously; final destruction joins the worker before
  releasing its audio source or rendering resources.

## Hardware paths and limits

Windows D3D12 NV12 playback uses SDL's native device, FFmpeg's frame/fence metadata,
and the existing bounded D3D12 copy ring. AVFrame ownership is retained until the
copy completes. There is no CPU pixel transfer during normal native playback.
The native path requires the D3D12 interop build feature and its GStreamer 1.28+
SDK dependency, even though decoding itself is performed by FFmpeg.

Linux VAAPI uses the shared EGL DMA-BUF importer when available (details below).
D3D11 and VideoToolbox currently download decoded frames for SDL upload.
Software decoding also works.
Logs distinguish native copying, hardware download/upload, and software decoding.
Linux/macOS hardware behavior has not been validated on this Windows host.

Perspective uses a CPU RGBA transform with the same corner mapping and transparent
exterior as the existing implementation. D3D12 non-NV12/cropped frames use the
download path. EGL supports NV12/P010 SDR and cropping through GPU conversion.
HDR tone mapping is not implemented. Existing music visualizers remain on their GStreamer implementation.

## Tests

With the feature enabled, CTest adds `retrofe_ffmpeg_smoke_tests`. To run the shared
hardware test manually, use the staged plugin environment from
[the Windows interop test](WINDOWS-D3D12-INTEROP.md), plus:

```powershell
$env:RETROFE_TEST_VIDEO_BACKEND = 'ffmpeg'
$env:RETROFE_TEST_RENDERER = 'direct3d12'
& ../Build/tests/Release/retrofe_sdl3_smoke_tests.exe ../../Package/Environment/Common --hardware
```

This exercises seven simultaneous videos and three unload/reopen cycles.
Additional lifecycle checks run when RETROFE_TEST_MEDIA_A and RETROFE_TEST_MEDIA_B
point to these generated two-second fixtures:

```powershell
ffmpeg -y -f lavfi -i 'testsrc2=size=96x64:rate=24:duration=2' -f lavfi -i 'sine=frequency=440:duration=2' -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$env:TEMP/retrofe-ffmpeg-a.mp4"
ffmpeg -y -f lavfi -i 'color=blue:size=128x72:rate=24:duration=2' -c:v libx264 -pix_fmt yuv420p "$env:TEMP/retrofe-ffmpeg-b.mp4"
$env:RETROFE_TEST_MEDIA_A = "$env:TEMP/retrofe-ffmpeg-a.mp4"
$env:RETROFE_TEST_MEDIA_B = "$env:TEMP/retrofe-ffmpeg-b.mp4"
```

The extra checks cover seeking, frozen pause time, rewind, texture retention,
dimension changes, decoded pixel colors, perspective transparency, finite loops,
and recovery after a missing file. An expected missing-file error appears in the
test log. Overnight attract mode and N100 comparison remain manual validation.

Compatible video streams retain the codec context across URI changes. Codec
initialization bytes, dimensions, format and color metadata must match; unknown
extradata or stream side data uses a fresh decoder. Audio is reopened. INFO logs
report `Reused video decoder` or `Created video decoder`. This retains the codec
context, but does not guarantee every internal hardware allocation survives a flush.

Set RETROFE_TEST_MEDIA_C to a red version of fixture B (same encoding settings)
to test twelve compatible retargets with pixel checks for stale frames.

## Linux VAAPI / EGL

Build with `RETROFE_ENABLE_FFMPEG=ON` and `RETROFE_ENABLE_EGL_DMABUF=ON`.
EGL import requires GStreamer video/allocators >=1.24 development packages,
EGL and GLES2 development packages; it does not require GStreamer GL contexts.
Use:

```ini
VideoBackend=ffmpeg
HardwareVideoAccel=true
SDLRenderDriver=opengles2
log=INFO,WARNING,ERROR
```

VAAPI exports DMA-BUF on the decode worker with READ|DIRECT mapping. Producer
completion can block that worker, but no decoded pixels are downloaded for a
successful EGL import. Mapped AVFrames keep the descriptors and surfaces alive
through the EGL consumer fence. Compatible SDL wrappers are reused.
NV12 SDR uses the direct external-texture path where possible; padded/cropped
frames and P010 SDR use the reusable RGBA conversion output. Multi-FD storage is
supported. Unknown modifiers, unsupported layouts/colorimetry, export/import
failure, and perspective rendering retain the CPU transfer fallback.
INFO reports `Playback ACTIVE: VAAPI / EGL DMA-BUF ...; no CPU pixel transfer`.
Fallbacks report their reason. `RETROFE_EGL_DIRECT=0` selects EGL RGBA conversion
for comparison without forcing CPU pixel transfer.

After building, run the smoke executable with
`RETROFE_TEST_VIDEO_BACKEND=ffmpeg RETROFE_TEST_RENDERER=opengles2` and `--hardware`.
Verify logs, colors/crop, rapid scrolling, unload/reopen and playlist switching.
Linux driver/runtime validation is required; Windows compile checks cannot
validate VAAPI export or EGL modifier support.

Automatic loops retain the final presented texture and its GPU ownership until
the restarted stream provides a replacement. This prevents a blank during seek/
preroll; it does not promise gapless audio or eliminate the brief last-frame hold.
Explicit seeks, unloads and different-file opens still invalidate old pixels.

Windows FFmpeg 9.0.2 shared SDK upgrade: select it with
`./Build.ps1 -EnableFFmpeg -FFmpegRoot E:/ffmpeg-9.0.2-full_build-shared`.
Changing FFMPEG_ROOT refreshes cached include/library discovery. Runtime staging
removes obsolete FFmpeg DLL majors unless another staged dependency needs them.

## Hardware diagnostics

FFmpeg warning/error messages now pass through RetroFE's logger under `FFmpeg`.
For a targeted diagnostic run in PowerShell:

```powershell
$env:RETROFE_FFMPEG_DIAGNOSTICS='1'
./retrofe.exe
```

Diagnostic mode bypasses log-category filters for FFmpeg messages and the first
D3D12 frame description per media change, including DXGI format, allocation,
array slice, flags and producer fence. It does not enable CPU transfer or the
D3D12 debug layer. Remove the environment variable after collecting the log.
