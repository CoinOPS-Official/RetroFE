# Standalone Linux GStreamer / SDL3 plane prototype

Experimental, not yet compiled or run on Linux. This builds independently of
RetroFE and does not change its video implementation. Uses installed SDL3 and
GStreamer development packages; no SDL_image, SDL_ttf, SDL_mixer or codec builds.

## Build and run (repository root)

The build also requires EGL and GStreamer allocators development files, exposed
through the `egl`, `glesv2` and `gstreamer-allocators-1.0` pkg-config packages.

```sh
cmake -S RetroFE/Source/Prototypes/GstSdl3Linux -B RetroFE/Source/Prototypes/GstSdl3Linux/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build RetroFE/Source/Prototypes/GstSdl3Linux/build --parallel
./RetroFE/Source/Prototypes/GstSdl3Linux/build/gst_sdl3_linux /absolute/path/video.mp4 nv12
./RetroFE/Source/Prototypes/GstSdl3Linux/build/gst_sdl3_linux /absolute/path/video.mp4 rgba
```

Requires EGL, the SDL opengles2 renderer, VA-API H.264 decoding (`vah264dec`),
MP4 demuxing and parsing, glupload, glcolorconvert (RGBA comparison only), appsink.
This is intentionally an AMD/Intel VA experiment, not a general media player.
Use the same H.264 MP4 in both modes.

- NV12: decoder -> DMA-BUF -> glupload -> NV12 GLMemory -> SDL Y/UV wrappers.
- RGBA: same import, then glcolorconvert -> RGBA GLMemory -> SDL wrapper.
- Space synchronously unloads to READY, waits, then plays on the same pipeline.
- R destroys and rebuilds the pipeline while retaining the SDL renderer/context.
- Escape exits; EOS loops the file.

### DMA-BUF inspection

```sh
./RetroFE/Source/Prototypes/GstSdl3Linux/build/gst_sdl3_linux /absolute/path/video.mp4 dmabuf-inspect
```

Receives one hardware-decoded DMA_DRM sample directly from appsink, without
glupload, glcolorconvert or a GStreamer GL context. It logs caps, DRM FourCC and
modifier, GstVideoMeta dimensions/strides, memory objects and borrowed FDs,
and resolves each plane start to its backing memory and FD-relative offset.
It never maps pixel memory, closes borrowed FDs, or assumes one FD per plane.

Queries the SDL renderer's EGL display for the exact format/modifier and reports
`external_only`. A match does not prove that separate SDL-compatible Y/UV planes
can be created. This mode does not yet create EGLImages or display video; the
window is only used to establish SDL's EGL context. It exits after inspection
or a 15-second sample timeout. Stress environment variables apply only to the
rendering modes. Missing metadata/query support fails explicitly.

### Direct EGL playback

```sh
./RetroFE/Source/Prototypes/GstSdl3Linux/build/gst_sdl3_linux /absolute/path/video.mp4 dmabuf-egl
```

Runs the inspection first, then imports each two-plane NV12 DMA-BUF as an
EGLImage using its actual plane FD-relative offsets, pitches and DRM modifier.
An external-texture shader samples it into one persistent RGBA texture, wrapped
by SDL. That RGBA allocation/wrapper is reused until dimensions change. There is
no glupload, glcolorconvert or GstGLContext in this mode and no RGBA-to-RGBA copy.
It still performs a conversion draw into an RGBA intermediate. Verify colors and
orientation against `rgba` mode; successful import alone is insufficient.

The prototype supports BT.601/709 full/limited-range hints and rejects unknown
colorimetry, non-NV12, extra planes and explicit crop metadata. It relies on the
VA export/DMA-BUF implicit producer synchronization path; no explicit native
producer fence is imported. Consumer glFinish holds the sample alive until the
conversion completes. This deliberately blocking experiment is not a benchmark.
Borrowed FDs are never closed. Import/shader/layout failures are visible errors,
not silent CPU fallbacks. The first inspected sample remains referenced by the
inspection harness until playback returns.

Space performs a READY unload/reopen. R performs a NULL reset/restart of the
same GstPipeline object (not object destruction/recreation). The three PROTO_*
stress options work here too; PROTO_REBUILD_EVERY selects periodic NULL resets.
Escape exits; EOS seeks to the beginning. True pipeline-object recreation and
multiple videos remain follow-up tests. This mode has not been compiled/run
locally on Linux. Reconfigure CMake to pick up the GLES dependency/source file.

### Rendering stress controls

```sh
env PROTO_CYCLE_MS=2000 PROTO_REBUILD_EVERY=10 PROTO_DURATION_MS=600000 ./RetroFE/Source/Prototypes/GstSdl3Linux/build/gst_sdl3_linux /absolute/path/video.mp4 nv12
```

This runs for ten minutes, reopening every two seconds and rebuilding every
tenth cycle. Each variable defaults to zero (disabled). Repeat with `rgba`.
Startup logs identify SDL/GStreamer versions; each cycle logs its operation,
and the first frame after it logs caps and plane layout. The final summary
reports total frames/cycles/time, not a correctness or leak-test verdict.
Short cycles may interrupt preroll; inspect frame progress, not just cycle count.

Logs report native plane formats, allocated sizes, strides and frame counts.
The red rectangle checks SDL drawing alongside video. Inspect colors, cropping,
orientation and motion against RGBA mode: successful texture creation is not
proof that GL plane channel layouts match SDL's shaders.

The prototype requires unpadded 2D plane allocations and rejects other layouts
explicitly. Strides are diagnostic; they are not CPU upload pitches in this GL
mapping. Do not remove the allocation checks without handling visible crop and
chroma scaling. BT.601/709 SDR are the initial color cases; other colorimetry
needs additional handling before production use.

There is deliberately no CPU fallback: negotiation/import errors fail visibly.
No RetroFE environment variables are used. Producer glFinish dispatch and
consumer glFinish before release establish a conservative synchronization
baseline. This is NOT a performance benchmark. Wrappers are created per frame;
held samples prevent buffer recycling until all consumer reads complete.

## Linux Codex handoff

Build and debug this standalone target first. Preserve the running RetroFE
diagnostic executable and its source revision. Do not modify the production
interop or migrate lifecycle changes while diagnosing this prototype.

1. Fix Linux compilation errors and run RGBA as the control case.
2. Test NV12 negotiation and inspect native plane formats versus the installed
   SDL GLES NV12 shader's expected channel layout. Record unsupported paths.
3. Verify frame colors, orientation and crop with a known reference video.
4. Exercise repeated Space/R and long playback, then add timed cycling and
   multiple independent videos using the same renderer.
5. Only after correctness, replace blocking waits with producer sync metadata
   and consumer fences, and measure frame time/power against the control.

If a crash occurs, use GDB with debuginfod and capture all thread backtraces.
Do not treat successful startup or a short run as long-term stability.
