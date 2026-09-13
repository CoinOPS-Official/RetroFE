# Linux EGL DMA-BUF integration

Direct-mode GL texture handles and SDL wrappers now enter a bounded idle cache (up to four slots) after their final-read fence completes. Matching dimensions reuse both; different dimensions reuse the native handle but recreate its SDL wrapper. Slots survive unload/reopen and are destroyed with the interop instance. EGL images are still imported per frame, never cached by FD. Idle slots hold no GstSample or EGLImage handle, although their GL image siblings may retain bounded backing storage until rebound or destroyed.

For reuse validation, enable DEBUG logging and look for `Created reusable EGL direct texture slot`. Creation should stop after warm-up for fixed-size playback and compatible reopen cycles. Then alternate resolutions and direct/RGBA media, check scaling and orientation, and verify memory remains bounded during playlist switches. This slot-rebinding change requires Linux runtime testing; a clean diff check is not a GPU correctness test.

Direct SDL external sampling is now preferred for whole-image NV12 imports. The ACTIVE description includes `direct SDL external texture` and `no RGBA intermediate`. Cropped/padded visible images and other formats use the existing RGBA conversion; SDL wrapper rejection also selects RGBA. Failed EGL imports still use CPU fallback.

For comparison, run `env RETROFE_EGL_DIRECT=0 ./retrofe` to force the overnight-tested RGBA path. With the variable unset, direct sampling is preferred. RETROFE_GL_DIRECT controls the older GStreamer GL backend, not this importer.

Direct buffers and SDL wrappers remain retained while displayed. On replacement, unload or teardown, SDL reads are flushed before inserting a completion fence. Retired imports are released only after the fence signals. This covers repeated draws of a paused frame without treating presentation as GPU completion. There is no steady-state glFinish; fence-error recovery may use it. Pending imports remain bounded with backpressure.

Repeat attract-mode and playlist-switch stress tests for this production integration, including shutdown, paused/repeated frames and transitions between direct and cropped RGBA media. The prototype passed playback; production direct sampling still needs Linux build/runtime and overnight validation. Existing CMake caches that disabled RETROFE_ENABLE_EGL_DMABUF need it explicitly enabled; fresh builds default it ON when dependencies are available.

Configure from the repository root:

```sh
cmake -S RetroFE/Source -B RetroFE/Build-linux -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRETROFE_ENABLE_GSTREAMER_GL=ON -DRETROFE_ENABLE_EGL_DMABUF=ON
cmake --build RetroFE/Build-linux --parallel
```

Requires GStreamer video/allocators >= 1.24, EGL and GLES2 development libraries, plus the existing GStreamer GL build dependencies. Check configuration reports `Linux EGL DMA-BUF video interop enabled`.

Use these settings:

```ini
HardwareVideoAccel=true
SDLRenderDriver=opengles2
log=INFO,WARNING,ERROR
```

The ACTIVE log identifies direct sampling or RGBA conversion, plus `GPU fences` and `no GStreamer GL context`. RETROFE_GL_DIRECT does not select this backend. This build option replaces the old GL backend; desktop opengl does not use the EGL importer. Configure EGL_DMABUF OFF to return to the previous GL implementation.

The importer retains decoder samples, EGL images and external textures until their last GPU reads complete. The RGBA output, SDL wrapper, shader and FBO survive unload/reopen; output allocation changes when visible dimensions change. Imports are recreated per frame to avoid stale FD reuse. Capability caching is bounded. Up to four pending conversions are retained before waiting. Teardown and resize may wait; ordinary frames poll fences. Driver-error recovery may still call glFinish.

Plane offsets and pitches come from VideoMeta, including multi-FD layouts. EGL must advertise the exact FourCC/modifier and accept the import. Crop is applied during conversion. SDR BT.601/709 YUV is supported; P010 is reduced to RGBA8 when importable. HDR tone mapping is not implemented. Unsupported imports renegotiate system-memory upload; this fallback is not a guarantee of correct HDR rendering.

Linux runtime validation is required:

Unspecified YUV colorimetry fields now use GStreamer's defaults for the actual DRM-mapped pixel format and coded dimensions (`gst_video_info_set_format`). Explicit fields are preserved. For untagged 1080p NV12 this selects limited-range BT.709; SD at 576 lines or below uses limited-range BT.601. A defaults message is emitted on change and after unload, not every frame. Explicit unsupported matrices and HDR transfer functions still take the fallback path.

Compare untagged `videoFULL/espgal2.mp4` with tagged `video/espgal2.mp4`: the untagged clip should log `EGL colorimetry defaults applied` with `resolved=bt709`, then ACTIVE rather than CPU fallback. The tagged clip should remain ACTIVE without a defaults message. Check colors and black levels visually; defaults cannot recover the encoding intent of untagged content.

- Repeated attract-mode unload/reopen, then rapid playlist changes and shutdown.
- Alternate resolutions, aspect ratios, NV12 and SDR P010 media on the same instance.
- Exercise crop metadata and multi-FD samples when available; inspect color and orientation.
- Run multiple simultaneous videos and watch process memory and FD counts over time.
- Confirm unsupported inputs log fallback and continue playback.
- Play a rejected clip followed by a previously working clip on the same instance. The second clip must retry GPU import and log ACTIVE. Internal recovery of the rejected clip must remain on CPU without repeatedly reopening. Fallback logs include negotiated caps and parsed matrix/range/transfer/primaries values; retain these when reporting colorimetry failures.

Renderer flushes remain per-video for this first integration. Renderer-wide batching and shared shader resources are follow-up optimizations; resources currently persist per instance. Windows D3D11 is unchanged.
