# Upstream Vulkan fixes on GStreamer 1.28.7

Built and tested on September 30, 2026 against the MSVC x64 GStreamer 1.28.7
SDK at `C:/gstreamer/1.0/msvc_x86_64`. The base is the released
`gst-plugins-bad-1.28.7` source archive, retaining RetroFE's existing shared-queue,
context-query and decoder presentation-image patch.

Apply [the upstream backport patch](../../cmake/patches/gstreamer-vulkan-1.28.7-upstream-backports.patch)
after [the existing interop patch](../../cmake/patches/gstreamer-vulkan-shared-queue-lock.patch).
Both patches use paths relative to the `gst-plugins-bad-1.28.7` source root:

```powershell
# From the extracted source root, outside an enclosing Git checkout:
git apply /path/to/gstreamer-vulkan-shared-queue-lock.patch
git apply /path/to/gstreamer-vulkan-1.28.7-upstream-backports.patch
```

If the extracted source is inside RetroFE's ignored `build` directory, set
`GIT_CEILING_DIRECTORIES` to that build directory first. Otherwise `git apply`
can discover the enclosing repository and silently skip the GStreamer paths.
The backport has been checked against pristine release files and reverse-checked
against the actual compiled source. The existing interop patch also reverse-checks.

## Included commits

| Upstream commit | Effect |
| --- | --- |
| [4dcbb37](https://github.com/GStreamer/gstreamer/commit/4dcbb37bd1380f2418b7b307e446176d3e8a55ae) | Select DPB/output usage after querying driver capabilities. |
| [e16fd2f](https://github.com/GStreamer/gstreamer/commit/e16fd2f5a7b5f82107ad6c1d7ee11c56d38d99b1) | Fix cumulative plane offsets in multi-memory image buffers. |
| [239b040](https://github.com/GStreamer/gstreamer/commit/239b0408e46f1e8b9aff4f1d0a42180afb5b72ef) | Handle plane memory offsets and four-plane downloads. |
| [558d7cb](https://github.com/GStreamer/gstreamer/commit/558d7cbcaa227158848bdd7d1cb4b684d0ce8cfa) | Release old swapper caps after intersection. |
| [ee1ddb4](https://github.com/GStreamer/gstreamer/commit/ee1ddb4047e570a31a4ad820fcdc971bc764b072) | Advertise extra H.264 profile variants for both single and list profile caps. |
| [4902f81](https://github.com/GStreamer/gstreamer/commit/4902f8122b10673f3fc1b70f7db2076464ce317c) | Prefer host-cached download memory when supported. |
| [48a93f6](https://github.com/GStreamer/gstreamer/commit/48a93f60bc00263fc67bd04566d3a8ddaf6729f9) | Add planar upload/download and image-pool offset regression coverage. |

The download tests are adapted for 1.28.7 to obtain the downloader's actual
device through its sink-pad context query. Creating another device on the same
physical GPU does not make its images usable by the downloader. Without this
adaptation, the test submitted foreign-device image handles and hung after a
`vkQueueSubmit2KHR` error. Production changes retain the upstream implementations.

The stride/padding fix [9ebd1cf](https://github.com/GStreamer/gstreamer/commit/9ebd1cf0f60170996811e714d9945fd483cfe9f6)
is already present in 1.28.7. Its error logging also avoids the caps allocation
addressed by [97c2972](https://github.com/GStreamer/gstreamer/commit/97c297200288ffa09f3abfcc7543c5285e661eca).
Neither was applied again. Encoder-only changes were excluded.

The newer synchronization-retry change
[36cde52](https://github.com/GStreamer/gstreamer/commit/36cde526a2247707b45548ef40b8d30fba38bab9)
depends on the newer operation/barrier tracking implementation and does not apply
cleanly to 1.28.7. That larger refactor is not included. Existing custom decoder
synchronization remains in place. These backports do not establish that the
previously observed clip corruption is fixed.

User retesting on October 1 confirmed that the GStreamer Vulkan problem remains
with these backports. Keep this decoder path experimental and opt-in.

## Build and staged artifacts

In an x64 Visual Studio Developer PowerShell, using the existing configured
Meson build, run from RetroFE `Source`:

```powershell
ninja -C Prototypes/GStreamerVulkan/build/meson-vulkan `
    gst-libs/gst/vulkan/gstvulkan-1.0-0.dll ext/vulkan/gstvulkan.dll
```

The build succeeded. MSVC reports an existing enum-pointer warning in
`gst_vulkan_image_buffer_pool_config_get_allocation_params`; the warned code is
unchanged from the release archive.

Both rebuilt DLLs are staged in `build/retrofe-final/bin/Release` and
`build/retrofe-final/tests/Release`: the library sits beside the executables,
and the plugin is in `gst-plugins/gstvulkan.dll`. The plugin is also staged in
`build/dist/lib/gstreamer-1.0/gstvulkan.dll`. These paths are relative to this
document's directory. The SDK installation and external RetroFE installations
were not replaced. No RetroFE relink is required for these backports; the public
API and existing linked import library remain compatible.

| DLL | SHA-256 |
| --- | --- |
| `gstvulkan-1.0-0.dll` | `59396A482BE4B1DF52C051B56EDC74181F0780AA1D849A07DC626C720600C7A5` |
| `gstvulkan.dll` | `842D76D8423788288FCCCDCCFC5154789BD66963B9CD6982FB8C6740F234CC52` |

## Validation

All checks below passed on the NVIDIA GeForce RTX 5070 Ti:

- Upstream `vkdownload`: 3 checks, including I420/A420 pixel roundtrips and output stride.
- Upstream `vkupload`: 3 checks, including I420/Y42B/Y444 roundtrips.
- Upstream `vkimagebufferpool`: 4 checks, including cumulative plane offsets and decode pools.
- RetroFE H.264: 90 frames, two streams, two presents per frame, alternating visibility and intermediate flush.
- RetroFE H.265: 90 frames, two streams, three presents per frame.
- RetroFE FFmpeg Vulkan: 90 frames, presented twice each, using the reencoded Spy Hunt fixture.
- RetroFE unlinked decoder context smoke via CTest.

Following the October 1 release cleanup, test and benchmark executables reside
in `build/retrofe-final/tests/Release`; obsolete copies are removed from
`bin/Release`. The patched DLL pair remains available in both runtimes.

Run [Test-UpstreamBackports.ps1](Test-UpstreamBackports.ps1) from an x64 Developer
PowerShell to compile and execute the upstream regression suites. It uses the
patched source, rebuilt import library, staged runtime, and an isolated
`videotestsrc` test plugin directory. It does not enable the entire GStreamer
test suite or require rebuilding unrelated plugins.
