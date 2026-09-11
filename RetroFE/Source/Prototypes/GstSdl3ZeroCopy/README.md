# GStreamer D3D11 NV12 copy into stock SDL3

This Windows example demonstrates the video path selected for a future SDL3
RetroFE integration. It intentionally contains one implementation:

1. SDL3 creates the Direct3D 11 renderer and exposes its `ID3D11Device`.
2. That device is wrapped in a `GstD3D11Device` and supplied to GStreamer.
3. GStreamer hardware-decodes into
   `video/x-raw(memory:D3D11Memory),format=NV12`.
4. Each selected decoder surface is copied with
   `CopySubresourceRegion` into one of three reusable, single-slice,
   shader-readable NV12 textures on the same device.
5. Stock SDL3 wraps those copy targets with
   `SDL_CreateTextureWithProperties` and draws them normally.

This is not literal zero-copy: it performs one GPU-to-GPU NV12 copy per
accepted video frame. It performs no CPU readback, CPU upload, or colorspace
conversion. The copy is necessary because the tested D3D11 decoder exposes
texture-array slices with decode-only bind flags, while SDL needs a
shader-readable texture.

No SDL source patch is used. CMake fetches the stock SDL 3.4.12 release unless
an installed SDL3 package is explicitly requested.

## RetroFE-like frame lifecycle

Media delivery and rendering are kept separate:

1. The appsink callback only replaces one staged `GstSample`. The appsink
   itself also has `max-buffers=1`, so stale frames do not accumulate.
2. The main thread latches the newest sample once at the preparation boundary.
   The associated GPU copy happens there.
3. The latched SDL texture remains unchanged while the page draws. The example
   can draw a moving video and a conventional RGBA overlay above it to exercise
   mixed component ordering.
4. `SDL_FlushRenderer` submits the queued SDL commands while the shared D3D11
   immediate context is protected.
5. `SDL_RenderPresent` executes after releasing that protection, preventing a
   blocking swap-chain present from holding up GStreamer.
6. The replaced sample is retired after a successful present.

The corresponding minimal RetroFE changes are:

- Keep the GStreamer callback limited to staging the newest sample.
- Use the existing main-thread `VideoComponent::updateFrame` path as the
  once-per-frame latch/copy boundary.
- Remove the opportunistic `updateFrame` call from `VideoComponent::draw`.
- Keep normal layout, layering, animation, and texture drawing unchanged.
- Add renderer-level flush synchronization and post-present sample retirement.

SDL and GStreamer issue commands through the same D3D11 immediate context.
The example enables `ID3D11Multithread` protection and explicitly groups each
copy and SDL flush command batch with its recursive `Enter`/`Leave` lock. It
does not use GStreamer's private device mutex as a cross-library lock.

## Configure and build

From this directory:

```powershell
cmake -S . -B build -A x64 `
  -DGSTREAMER_ROOT=C:\gstreamer\1.0\msvc_x86_64
cmake --build build --config Release
```

To use an installed stock SDL3 CMake package instead:

```powershell
cmake -S . -B build -A x64 `
  -DGSTREAMER_ROOT=C:\gstreamer\1.0\msvc_x86_64 `
  -DGST_SDL3_NV12_COPY_USE_SYSTEM_SDL3=ON
```

## Run

Basic frame-driven playback:

```powershell
.\run.ps1 ".\Fire.mp4"
```

Exercise the RetroFE-like mixed drawing arrangement:

```powershell
.\run.ps1 ".\Fire.mp4" -Bounce -Overlay -Seconds 30
```

Run an unattended measurement:

```powershell
.\run.ps1 ".\Fire.mp4" -Bounce -Overlay -HiddenWindow -Seconds 30
```

Test blocking/vsynced presentation:

```powershell
.\run.ps1 ".\Fire.mp4" -Bounce -Overlay -Vsync -Seconds 30
```

`-Bounce` scales and moves the video at 60 Hz. `-Overlay` creates a normal SDL
RGBA texture and draws it after the video, so it remains topmost. Neither
option changes the media transfer path.

## Expected output

A healthy run should report:

- a `direct3d11` SDL renderer and one shared D3D11 device;
- a `d3d11*dec` hardware decoder;
- negotiated `D3D11Memory` and NV12 caps;
- three shader-readable NV12 copy targets;
- accepted frames matching received frames;
- one GPU copy for each accepted frame;
- zero rejected frames and normally zero callback drops;
- zero forced sample retirements.

The final statistics also report D3D11 synchronization wait/hold time,
presentation time, wall time, and process CPU use.

In the validated 30-second 1920x1080 H.264 run, this lifecycle accepted all 899
received frames, made 899 GPU copies and 1,795 presents, dropped no frames, and
used 9 percent steady-state process CPU.
