# Building RetroFE

RetroFE uses SDL3, SDL3_image, SDL3_ttf and SDL3_mixer on every platform.
No SDL patches or standalone prototypes are required. Use CMake 3.24 or newer
and a C++20 compiler. Initialize the OpenHi2txt submodule before configuring:

```sh
git submodule update --init --recursive
```

## Windows

Install Visual Studio 2022 with Desktop development with C++ and the Windows
SDK, CMake, and the GStreamer MSVC x64 runtime and development packages.
From the repository root:

```powershell
./RetroFE/Source/Build.ps1
```

The script downloads SDL's official VC development archives, checks their
pinned SHA-256 hashes, and restores their unmodified contents before each
configure. Explicit package paths prevent a cached CMake configuration from
selecting a previous patched build. Versions are SDL 3.4.12, SDL_image 3.2.4,
SDL_ttf 3.2.2 and SDL_mixer 3.2.4. The runtime DLLs come from these archives;
SDL is not rebuilt locally.

Overrides: `-GStreamerRoot 'C:/path/to/gstreamer'`, `-Configuration RelWithDebInfo`
or `-BuildDirectory 'C:/path/to/build'`. The default GStreamer root is
`C:/gstreamer/1.0/msvc_x86_64`.

Open `RetroFE/Build/retrofe.sln` in Visual Studio. The executable and matching
runtime are in `RetroFE/Build/bin/Release`. `runtime-files.txt` lists the staged
DLLs, plugin scanner and SDL redistribution notices. GStreamer plugins are
selected in `cmake/StageRuntime.cmake`; dependent DLLs are resolved from the
installed SDK. Surplus known GStreamer plugins and obsolete SDL2 DLLs are pruned
only inside the build output.

`Build.ps1` runs CTest, including the software-rendering smoke test and
OpenHi2txt tests. Test executables and their runtime dependencies are in
`RetroFE/Build/tests/Release`, separate from the application runtime.
For a separate GPU check, run from `RetroFE/Build`:

```powershell
$env:GST_PLUGIN_PATH = (Resolve-Path ./tests/Release).Path
$env:GST_PLUGIN_SYSTEM_PATH = $env:GST_PLUGIN_PATH
$env:GST_PLUGIN_SCANNER = "$env:GST_PLUGIN_PATH/gst-plugin-scanner.exe"
$env:GST_REGISTRY = "$PWD/hardware-test-registry.bin"
./tests/Release/retrofe_sdl3_smoke_tests.exe ../../Package/Environment/Common --hardware
```

## Linux

Install development packages for GStreamer (core, app, audio, video and GL),
GLib, zlib, libusb, libevdev, libudev, libserialport and libcurl, plus pkg-config.
For DMA-BUF video import, install GStreamer 1.24+ video/allocators headers,
EGL and GLES development packages. Runtime plugins must include playback,
demuxers, parsers, audio/video conversion and the appropriate hardware decoder;
gst-libav supplies software fallback codecs.

CMake prefers installed SDL3 config packages at the versions listed above or
newer. Otherwise it fetches pinned, unmodified upstream sources and their
codec submodules. This requires network access and SDL's platform build
dependencies (window-system, graphics and audio development libraries).
See [SDL's Linux dependencies](https://wiki.libsdl.org/SDL3/README-linux).
AVIF artwork decoding is disabled by default for source builds; GStreamer
video decoding is unaffected.

```sh
cmake -S RetroFE/Source -B RetroFE/Build-linux -DCMAKE_BUILD_TYPE=Release -DRETROFE_BUILD_TESTING=ON -DBUILD_TESTING=ON
cmake --build RetroFE/Build-linux --parallel
ctest --test-dir RetroFE/Build-linux --output-on-failure
```

The executable is `RetroFE/Build-linux/bin/retrofe`. CMake reports which Linux
interop backends are enabled. See [video validation](Tests/LINUX-VIDEO-INTEROP.md).

## macOS

Use the same CMake build with Xcode command-line tools and Homebrew dependencies:

```sh
brew install cmake pkg-config gstreamer glib libusb libserialport curl zlib
cmake -S RetroFE/Source -B RetroFE/Build-macos -DCMAKE_BUILD_TYPE=Release -DRETROFE_BUILD_TESTING=ON -DBUILD_TESTING=ON
cmake --build RetroFE/Build-macos --parallel
ctest --test-dir RetroFE/Build-macos --output-on-failure
```

CMake uses installed SDL3 packages or builds pinned upstream sources as on Linux.
The executable is `RetroFE/Build-macos/bin/retrofe`. This builds for the host
architecture and depends on those installed libraries. The macOS workflow is a
build/test check, not a standalone universal app-bundle release. The historical
SDL2 Xcode project is no longer used; CMake can generate an Xcode project with
`-G Xcode` in a separate build directory.

## Dependency selection

Use `-DCMAKE_PREFIX_PATH=/path/to/prefix` for installed SDL3 packages and
`-DRETROFE_FETCH_SDL3=OFF` to require them without downloading sources.
Pinned source revisions live in `cmake/SDL3Dependencies.cmake`; Windows archive
versions and checksums live in `Build.ps1`. Update them together.
If intentionally testing local SDL sources with `FETCHCONTENT_SOURCE_DIR_*`,
use a separate build directory; those overrides are developer-owned inputs.

## Packaging and playback

From the repository root, after a Windows build:

```sh
python Scripts/Package.py --os windows --build full
```

For a different build directory, pass `--build-directory RetroFE/Build-linux`
with `--os linux` (or `--os mac` and the macOS build directory).
Windows packaging uses the generated runtime manifest, not a separately
versioned DLL bundle. Unix packaging copies the executable; matching shared
libraries and GStreamer plugins must remain installed on the target system.
The combined CoinOPS workflows handle their own runtime bundling.

An executable alone is not a complete frontend installation: retain settings,
collections, layouts, fonts and media. For hardware video set
`HardwareVideoAccel=true`, `SDLRenderDriver=direct3d12` on Windows with GStreamer
1.28+ (or `direct3d11` for the existing fallback), or
`SDLRenderDriver=opengles2` for Linux EGL, and `log=INFO,WARNING,ERROR`.
Check the decoder selection and `GPU texture interop ACTIVE` messages for the
actual playback path. Hardware support depends on the decoder, driver and media.
See [D3D12 ownership and validation](Tests/WINDOWS-D3D12-INTEROP.md) for the
renderer selection rules, supported formats and hardware test instructions.
