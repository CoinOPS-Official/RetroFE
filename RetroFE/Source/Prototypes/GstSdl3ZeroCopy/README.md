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
# Decoder allocation investigation

Source/runtime follow-up: GStreamer 1.28.7 `gstd3d11decoder.cpp` creates its own decoder pool; it uses the downstream pool for copied output, not decoding. Its separate shader-readable decoder textures are enabled only when the selected DXVA configuration reports ConfigDecoderSpecific bit 0x4000. This machine's H.264 runtime trace reports `ConfigDecoderSpecific 0x0`, so that path is not selected. For direct decoder output it returns the internal buffer; otherwise it allocates a downstream buffer and calls crop_and_copy_buffer. Offering the shader pool therefore does not force shader-readable decoding. Trace saved in `build/decoder-policy-1.28.7.log`. Debug logging also emitted a GStreamer `category != NULL` assertion; this diagnostic run is not a clean stress test.

For the tested H.264 configuration, retain the one-copy NV12 path. Forcing downstream output could move a GPU copy into GStreamer but would not establish zero-copy. Results do not rule out other codecs, driver configurations or decoder backends.

After updating to GStreamer 1.28.7, the identical `--shader-pool` kof99.mp4 test produced the same allocation result: ArraySize=6, BindFlags=0x200, shaderReadableFlag=0, and both SRV probes failed with 0x80070057. Playback completed with 181 accepted frames, zero rejected/dropped frames and three reusable copy wrappers. Output: `build/shader-pool-1.28.7.log`. Updating alone did not enable direct sampling in this configuration.

GStreamer 1.28.1 allocation experiment: `--shader-pool` offers a shared-device output pool with SHADER_RESOURCE | RENDER_TARGET (0x28) and VideoMeta. Testing the same kof99.mp4 still delivered decoder-only NV12 arrays (ArraySize=6, BindFlags=0x200); both plane SRV probes failed with 0x80070057. No standalone shader-readable output was observed during that run. Playback completed: 180 accepted, zero rejected, one callback drop, three copy wrappers. Full baseline output is saved locally in `build/shader-pool-1.28.1.log`. Repeat this exact test after updating GStreamer; offering the pool does not guarantee the decoder will use it for every output.

The prototype logs native dimensions, visible dimensions, array size, subresource, bind flags and colorimetry on the first frame and allocation-layout changes. It probes Y/UV shader-resource views of the actual input texture and, for standalone subresource-zero textures, SDL wrapping. These are creation-only probes; playback continues through the existing NV12 copy ring. Wrapper success would not establish safe direct playback or zero internal decoder copies.

Local test (2026-09-13), `videoFULL/kof99.mp4`: initial NV12 1920x1088 allocation (visible 1920x1080), ArraySize=10, BindFlags=0x200 (decoder only). Later input used ArraySize=1, BindFlags=0x20 (render target only). Both lacked shader-resource binding; Y/UV probes returned 0x80070057. SDL rejected wrapping the standalone texture. Playback completed with 180 accepted frames, zero rejected frames and three copy wrappers (one callback-dropped frame).

Next investigation: request shader-readable downstream allocation through GStreamer's allocation negotiation, then repeat these probes and determine whether GStreamer performs an internal copy. Merely inserting a conversion/copy element would not demonstrate end-to-end zero-copy. Production D3D11 interop is unchanged.
