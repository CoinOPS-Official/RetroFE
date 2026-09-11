# SDL3 branch

This branch ports the RetroFE application and CMake build to SDL3. It is based
on the `openhi2txt` branch. SDL3 is the application's normal CMake build;
the historical prototype is not required to build or run RetroFE.

## Windows build and test

From this directory, run:

```powershell
./Build.ps1
```

Requires Visual Studio 2022 with C++/Windows SDK, CMake 3.24+, and the GStreamer
MSVC x64 development/runtime installation. Override its location with
`-GStreamerRoot 'C:/path/to/gstreamer'`. Existing non-SDL dependencies remain
in `../ThirdParty`, including the current OpenHi2txt submodule.
The submodule is updated to upstream OpenHi2txt 0.5.0 (`277723c`). Its five
tests are enabled by the build script along with RetroFE's tests.

Open `../Build/retrofe.sln` in Visual Studio; CMake sets `retrofe` as the
startup project and includes the D3D11 interop source and header.

The script downloads official development archives, verifies pinned SHA-256
checksums, configures, builds, and runs CTest. Versions used:

| Library | Version |
| --- | --- |
| SDL3 | 3.4.12 |
| SDL3_image | 3.2.4 |
| SDL3_ttf | 3.2.2 |
| SDL3_mixer | 3.2.4 |

The executable and matching runtime files are in
`../Build/bin/Release/`. To test a full frontend installation, copy the
contents of that directory into the `retrofe` directory of a **separate test
copy** of your RetroFE installation. Keep that test copy's settings, layouts,
collections, fonts, and controller mappings. The executable alone is not a
complete frontend installation. Runtime staging includes matching GStreamer
plugins and its plugin scanner. The test executables need not be copied.

GStreamer staging uses the curated plugin list in `cmake/StageRuntime.cmake`,
based on the previous pruned SDL2 runtime, with current visualizer, perspective,
and hardware-decoder support. CMake resolves dependent DLLs from the current
installation instead of copying every GStreamer DLL. `runtime-files.txt` in the
output lists the staged runtime files. Known surplus GStreamer DLLs are removed
only from the configured build output; external installations are not pruned.
When updating an existing frontend, copying these files over a full runtime does
not remove its old plugins. Use the manifest to prepare a clean runtime folder.

## Changes

- Native SDL3 headers and imported CMake targets throughout the application.
- Property-based window creation, SDL3 display IDs mapped from existing
  zero-based screen settings, renderer selection, per-renderer vsync and
  texture filtering. Existing Windows `direct3d` settings select `direct3d11`.
  An empty renderer setting selects SDL's platform default.
- One floating-point rendering path for integer and fractional destinations,
  with SDL3 float vertex colors, existing clipping, rotation, mirrors and
  reflections. Frame pacing uses `SDL_DelayPrecise`.
- SDL3 joystick/gamepad events and instance IDs, retaining configured `joy0`
  style slots and clearing held state after disconnect. SDL event handling
  remains on the main thread.
- SDL3_mixer audio objects and reusable tracks replace SDL2 channels/music.
  Video audio continues through AudioBus. Visualizers consume the new native
  floating-point mixer format, with matching FFT conversion and GStreamer
  format/rate/channel timing.
- SDL3 surfaces, image I/O, glyph metrics, font atlases and video texture uploads.
- Video-only teardown retains audio/input; full shutdown releases the mixer
  before quitting SDL.

## Validation

### Windows GPU video testing

In the test installation's `settings.conf`, set:

```ini
HardwareVideoAccel=true
SDLRenderDriver=direct3d11
log=INFO,WARNING,ERROR
```

The D3D11 path wraps SDL's device for GStreamer and accepts NV12 D3D11 memory.
Decoder array slices are copied on the GPU into a three-texture ring imported
with `SDL_CreateTextureWithProperties`. This avoids CPU readback and upload;
it still performs one GPU copy. Color range/matrix and padded allocations are
preserved. The perspective filter and incompatible renderer/decoder output use
the existing system-memory upload path.

Look for `GPU texture interop ACTIVE` in the runtime log. This is emitted only
after an actual GPU frame copy and SDL texture import succeed. It includes the
monitor and video filename. `D3D11 hardware decoding requested` alone does not
prove that path is active. `GPU texture interop unavailable`,
`GPU texture interop fallback`, and `Video uses CPU texture upload` explain
other paths. Logging is once per playback/path, not once per frame.

The smoke test accepts an optional `--hardware` after its media-assets path to
require D3D11 interop and exercise repeated playback with an overlay. The normal
CTest run uses software rendering and checks the fallback independently.
Both paths have passed on this Windows machine. The hardware check requires
multiple distinct decoded GPU frames, verifies the overlay pixels, and tears
down the video before testing renderer reinitialization. Full frontend layouts,
multiple simultaneous videos, and long-running playback still need testing.

The Release build and CTest suite were run on Windows x64. The SDL3 smoke test
uses dummy video/audio devices and the actual application implementations to
check rendered tint/alpha pixels, container clipping, reflections, repeated
video initialization, rotated/mirrored rendering, virtual joystick slot
mapping/disconnect, overlapping WAV playback, gradient/outline font atlas
creation, and packaged MP4 decoding into an SDL3 texture. Existing high-score
change and MAME software resolver tests are also included.

The executable's import table was checked: it imports the four SDL3 DLLs and
has no SDL2 imports. Tests do not establish hardware rendering performance,
physical audio output, every controller, or every user layout. Multi-monitor,
hardware decoder, launch/return and long-running music tests still need a real
frontend session.

## Other platforms and existing packaging

An experimental Linux GStreamer GL backend now shares EGL/GLX contexts and
imports GL textures through SDL3. See [Linux video interop testing](Tests/LINUX-VIDEO-INTEROP.md)
for dependencies, settings, logging interpretation, and test commands. Linux
compilation and playback validation are still pending; do not infer hardware
decoding or zero CPU transfers from the GL texture-import message alone.

CMake also supports installed SDL3 config packages via `CMAKE_PREFIX_PATH` or
pinned source fallback via `RETROFE_FETCH_SDL3=ON`. Linux/macOS builds need
their existing GStreamer, GLib, USB, serial-port, curl and platform development
dependencies. They have not been built on this Windows machine.

Use this CMake entry point for the port. The Windows workflow uses `Build.ps1`
and uploads the SDL3 runtime without the historical SDL2 DLL bundle.
Historical Linux/macOS installation instructions, bundled SDL2 binaries, the
hand-maintained Xcode project and other release/Flatpak workflows have not been
converted or validated. They are not used by the Windows SDL3 build. CMake
runtime output now lives under the selected build directory's `bin` folder;
packaging that assumes `Build/retrofe` must be updated before release.

API references: [SDL3 migration](https://wiki.libsdl.org/SDL3/README-migration),
[SDL3_mixer migration](https://wiki.libsdl.org/SDL3_mixer/README-migration).
