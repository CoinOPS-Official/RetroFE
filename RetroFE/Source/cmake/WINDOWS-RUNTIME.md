# Windows release runtime

The application runtime is `Build/bin/Release`. Tests and benchmarks go in
`Build/tests/Release`; they are not part of the application package.
For a release-only configuration, use:

```powershell
cmake -S Source -B Build -DRETROFE_BUILD_TESTING=OFF -DBUILD_TESTING=OFF
cmake --build Build --config Release --target retrofe
```

Keep the directory structure when deploying:

| Location | Contents |
| --- | --- |
| `retrofe/retrofe.exe` | Application |
| `retrofe/*.dll` | Shared libraries: GStreamer core/helper libraries, GLib, SDL, FFmpeg and their resolved dependencies |
| `retrofe/gst-plugin-scanner.exe` | Plugin scanning helper; its imported libraries are beside it |
| `retrofe/gst-plugins/*.dll` | Curated loadable GStreamer plugins from the SDK's `lib/gstreamer-1.0`, plus the patched Vulkan plugin only when enabled |
| `retrofe/licenses/` | Staged dependency notices |
| `retrofe/runtime-files.txt` | Manifest of staged DLLs, scanner and notices, with relative paths |

For example, `gstvideo-1.0-0.dll` is a shared library and stays beside the
application. `gstvideoconvertscale.dll` is a plugin and goes in `gst-plugins`.
`gstvulkan-1.0-0.dll` is a library; `gstvulkan.dll` is a plugin.
Moving every GStreamer DLL into the plugin folder would break startup import
resolution unless an additional loader/bootstrap mechanism were introduced.

Windows startup sets `GST_PLUGIN_PATH` and `GST_PLUGIN_SYSTEM_PATH` to the
packaged plugin directory and `GST_PLUGIN_SCANNER` to the packaged scanner.
It uses `registry-plugins.bin` as the plugin cache. The new cache name avoids
reusing the previous flat-layout registry. No application-root or installed
SDK plugin directory is scanned. GStreamer documents custom plugin paths in
[Installing on Windows](https://gstreamer.freedesktop.org/documentation/installing/on-windows.html)
and [Running GStreamer Applications](https://gstreamer.freedesktop.org/documentation/gstreamer/running.html).

`StageRuntime.cmake` selects plugins and dependency seeds.
`PruneRuntime.cmake.in` also inspects the executable's own imports, resolves
transitive DLL dependencies, copies plugins
to the scan directory and libraries to the application directory, removes known
obsolete GStreamer/FFmpeg DLLs and old test programs, and writes the manifest.
Keep all resolved dependencies: GStreamer's libav plugin and RetroFE's FFmpeg
backend can require different FFmpeg DLL majors in the same package.

The experimental Vulkan source, backports and tests remain opt-in. The normal
release configuration leaves `RETROFE_ENABLE_GSTREAMER_VULKAN=OFF`.
