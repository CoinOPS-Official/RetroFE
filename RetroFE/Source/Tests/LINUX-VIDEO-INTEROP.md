# Linux GPU video interop testing

This backend is experimental and has not yet been compiled or run on Linux.
The Windows D3D11 path is retained. Linux uses GStreamer GL with SDL's OpenGL
or OpenGL ES renderer; Vulkan rendering currently uses CPU texture uploads.

## Dependencies and build

In addition to RetroFE's normal Linux dependencies and SDL3 libraries, install
GStreamer GL development files and the runtime `glupload` and `glcolorconvert`
elements. On Debian/Ubuntu these normally come from
`libgstreamer-plugins-base1.0-dev` and `gstreamer1.0-gl`. Install the appropriate
hardware decoder plugin and GPU driver for the machine. CMake needs
`gstreamer-gl-1.0` >= 1.20 and either `gstreamer-gl-egl-1.0` or
`gstreamer-gl-x11-1.0` through pkg-config.

From the repository root, with the normal development dependencies installed:

```sh
git submodule update --init --recursive
cmake -S RetroFE/Source -B RetroFE/Build-linux \
  -DCMAKE_BUILD_TYPE=Release -DRETROFE_BUILD_TESTING=ON -DBUILD_TESTING=ON \
  -DRETROFE_ENABLE_GSTREAMER_GL=ON
cmake --build RetroFE/Build-linux --parallel
ctest --test-dir RetroFE/Build-linux --output-on-failure
```

CMake must report `Linux GStreamer GL texture interop enabled`. If development
packages are missing, it warns and builds the CPU-upload path instead.
SDL3 config packages are used when present; otherwise the existing CMake helper
fetches pinned SDL3 sources. The legacy README's SDL2 package list is insufficient.

## Frontend settings

```ini
HardwareVideoAccel=true
SDLRenderDriver=opengles2
log=INFO,WARNING,ERROR
```

`opengles2` selects EGL, suitable for testing on Wayland or X11. `opengl` is also
supported through EGL or GLX, depending on SDL's chosen context. Desktop GLX
does not guarantee that a decoder's DMA-BUF format can be imported efficiently.
Keep perspective disabled for the first video test: that filter intentionally
uses the existing CPU RGBA path.

Useful log lines:

- `Video decoder selected:` identifies the actual decoder, including software
  decoders. HardwareVideoAccel alone is not proof of hardware decoding.
- `GL upload input caps:` containing `memory:DMABuf` or `memory:GLMemory`
  indicates GPU-memory input to the GL bridge. Plain `video/x-raw` indicates
  system-memory input and a CPU upload at that bridge.
- `GPU texture interop ACTIVE: OpenGL RGBA GPU copy` confirms that the final
  shared GL frame was copied into an SDL-wrapped texture. It does not prove
  the absence of CPU transfers upstream.
- `GL interop synchronization:` reports GPU fences or a blocking completion
  fallback for older contexts lacking sync objects.
- `GL pipeline failed; retrying...` switches that instance to the ordinary
  CPU-upload pipeline once. Missing context support also selects CPU upload.

The GL bridge converts to RGBA on the GPU and copies into a three-texture ring.
The ring survives unload/reopen when allocation dimensions match. Source samples
stay referenced until copy completion; destination reuse submits preceding SDL
draws first. The code preserves the GL bindings it changes.

## Focused test

Run from the build directory so logs stay with generated files:

```sh
cd RetroFE/Build-linux
RETROFE_TEST_RENDERER=opengles2 ./bin/retrofe_sdl3_smoke_tests \
  ../../Package/Environment/Common --hardware
```

This requires the GPU texture path, multiple copied frames, correct overlay
pixels, and successful unload/reopen. It fails if interop falls back to CPU.
It can pass with a software decoder feeding glupload: check decoder/input-caps
logs separately for end-to-end hardware acceleration.

Also inspect a real layout for video orientation, color, aspect ratio,
reflections, pause/resume, rapid scrolling, multiple videos, and multiple
monitors. Compare `opengles2` and `opengl` if context sharing fails. Include
`log.txt`, driver/GPU details, session type (Wayland/X11), GStreamer version,
and CMake output when reporting a failure. A clean software comparison can be
built with `-DRETROFE_ENABLE_GSTREAMER_GL=OFF`.
