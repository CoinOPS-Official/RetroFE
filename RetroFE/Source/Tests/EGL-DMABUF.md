# Linux EGL DMA-BUF integration (experimental)

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

The ACTIVE log must mention `EGL DMA-BUF conversion`, `GPU fences`, and `no GStreamer GL context`. RETROFE_GL_DIRECT does not select this backend. This build option replaces the old GL backend; desktop opengl does not use the EGL importer. Configure EGL_DMABUF OFF to return to the previous GL implementation.

The importer retains decoder samples, EGL images and external textures until conversion fences complete. The RGBA output, SDL wrapper, shader and FBO survive unload/reopen; output allocation changes when visible dimensions change. Imports are recreated per frame to avoid stale FD reuse. Capability caching is bounded. Up to four pending conversions are retained before waiting. Teardown and resize may wait; ordinary frames poll fences. Driver-error recovery may still call glFinish.

Plane offsets and pitches come from VideoMeta, including multi-FD layouts. EGL must advertise the exact FourCC/modifier and accept the import. Crop is applied during conversion. SDR BT.601/709 YUV is supported; P010 is reduced to RGBA8 when importable. HDR tone mapping is not implemented. Unsupported imports renegotiate system-memory upload; this fallback is not a guarantee of correct HDR rendering.

Linux runtime validation is required:

- Repeated attract-mode unload/reopen, then rapid playlist changes and shutdown.
- Alternate resolutions, aspect ratios, NV12 and SDR P010 media on the same instance.
- Exercise crop metadata and multi-FD samples when available; inspect color and orientation.
- Run multiple simultaneous videos and watch process memory and FD counts over time.
- Confirm unsupported inputs log fallback and continue playback.

Renderer flushes remain per-video for this first integration. Renderer-wide batching and shared shader resources are follow-up optimizations; resources currently persist per instance. Windows D3D11 is unchanged.
