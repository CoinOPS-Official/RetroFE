# GStreamer Vulkan Video interop on Windows

This is an opt-in GStreamer 1.28.7 path for H.264 and H.265. RetroFE creates a Vulkan device shared by SDL and GStreamer, copies ready decoded images on the GPU into persistent SDL presentation textures. Producer timelines are polled without waiting; unfinished frames are retried while the previous texture remains drawable. Decoder images stay referenced until their copy completes. Three textures per configuration are reused across interop lifetimes, with a renderer-wide limit of 32 configurations and four leases per interop. The build option is off by default. Set `SDLRenderDriver=vulkan` and enable hardware video acceleration to use it.

## Required GStreamer library patch

GStreamer 1.28.7 creates a new `GstVulkanQueue` wrapper for each request. Its submit mutex belongs to the wrapper, so two decoders can submit to the same underlying `VkQueue` without a shared lock. Apply [gstreamer-vulkan-shared-queue-lock.patch](../../cmake/patches/gstreamer-vulkan-shared-queue-lock.patch) to the `gst-plugins-bad-1.28.7` source tree **before** building `gstvulkan-1.0-0.dll` and `gstvulkan.dll`. The patch uses a bounded set of submit locks selected by the `VkQueue` handle, requests a second graphics queue when the GPU provides one, and makes the H.264/H.265 decoders prefer the graphics queue supplied through GStreamer's context query. It also lets a decoder created dynamically by `playbin3` read the device and queue contexts installed on that decoder before its pads are linked. It selects a separate DPB/output image where the driver supports one and keeps the allocation caps alive while creating the DPB pool. SDL uses graphics queue 0; GStreamer uses graphics queue 1 for output work and a video queue for decoding. It fixes queue-reference ownership in Vulkan operations. For coincident DPB/output images or dedicated decode queues, H.264/H.265 copy a separate, concurrently shared presentation image inside the decode submission, preserving the reference-picture layout. Rebuild both DLLs together because the private decoder-picture structure changes.

Build the GStreamer Vulkan library and plugin with Meson/Ninja, then stage the library, plugin, and their GStreamer runtime dependencies together. The local proof-of-concept build uses `Prototypes/GStreamerVulkan/build/meson-vulkan` and `Prototypes/GStreamerVulkan/build/dist`; those build outputs are ignored by Git.

The current DLLs also contain six selected upstream fixes backported onto 1.28.7, plus planar-image regression tests. Apply [gstreamer-vulkan-1.28.7-upstream-backports.patch](../../cmake/patches/gstreamer-vulkan-1.28.7-upstream-backports.patch) after the interop patch. See [UPSTREAM-BACKPORTS.md](UPSTREAM-BACKPORTS.md) for the commit manifest, exclusions, staged binaries and test results.

On Windows, all builds scan plugins from `retrofe/gst-plugins` and use `registry-plugins.bin`. Shared libraries and `gst-plugin-scanner.exe` stay beside `retrofe.exe`. This keeps older plugins in backup subdirectories from being selected by GStreamer's recursive plugin scan. Tests and benchmarks build into `tests/Release`, outside the release runtime.

Configure RetroFE with:

```text
-DRETROFE_ENABLE_GSTREAMER_VULKAN=ON
-DRETROFE_GSTREAMER_VULKAN_QUEUE_LOCK_PATCHED=ON
-DGSTREAMER_VULKAN_SOURCE_DIR=<gst-plugins-bad-1.28.7 source>
-DGSTREAMER_VULKAN_BUILD_DIR=<Meson build directory>
-DGSTREAMER_VULKAN_RUNTIME_ROOT=<staged GStreamer Vulkan runtime root>
```

`RETROFE_GSTREAMER_VULKAN_QUEUE_LOCK_PATCHED=ON` confirms that the linked and staged Vulkan library **and plugin** contain the patch. It is required because unpatched concurrent decoders can stall or fail, and GStreamer output work can otherwise submit to SDL's graphics queue while SDL flushes an intermediate batch. The staged root must contain `lib/gstreamer-1.0/gstvulkan.dll`; the Meson build directory must contain `gst-libs/gst/vulkan/gstvulkan-1.0-0.dll`. A GPU with fewer than two queues in the selected graphics family uses the existing CPU video path.

With `RETROFE_BUILD_TESTING=ON`, build `retrofe_vulkan_context_smoke` and run it from the Release binary directory. It creates an unlinked `vulkanh264dec`, installs device and queue contexts, and verifies that GStreamer's context queries return those exact Vulkan objects. This catches the dynamic decoder setup failure seen with `playbin3` without opening an SDL window.

The smoke also checks delayed producer handling, reference-image rejection, bounded wrapper reuse across interop recreation, and UI presentation while a producer is unfinished. The separate `retrofe_vulkan_interop_smoke` accepts an H.264 or H.265 elementary stream, optional frame count, presents per frame, and stream count (1 or 2). For example, `retrofe_vulkan_interop_smoke clip.h265 h265 10 3 2` tests two simultaneous streams and repeated presentation of each frame. Add `alternate` to draw only one of two active streams per frame, simulating a scroll across prepared video items. Add `flush` to force an SDL renderer submission between drawing and presentation.

## FFmpeg Vulkan decoder on the shared renderer

Build with `RETROFE_ENABLE_FFMPEG=ON` and the Vulkan options above, using an FFmpeg 8 or newer SDK with Vulkan decoding. Select `VideoBackend=ffmpeg`, `SDLRenderDriver=vulkan`, and `HardwareVideoAccel=true`. RetroFE creates one FFmpeg Vulkan hardware device context per SDL renderer, reusing its Vulkan instance, physical device, logical device, queue families, enabled extensions, and queue locks. The FFmpeg decoder produces `AVVkFrame` images. The adapter copies supported single-image NV12, IYUV, or P010 frames into persistent SDL textures, restoring the producer layout and signaling its timeline after the copy. It polls readiness before submitting, so a slow decode cannot hold up SDL draws. FFmpeg graphics queue index 0 is mapped to the reserved native queue 1; SDL retains native queue 0 for all of its submissions. Unsupported images fall back to software decoding.

This first FFmpeg path uses the existing GStreamer-created shared Vulkan renderer, so the patched GStreamer Vulkan library remains a build and runtime dependency even though FFmpeg performs video decoding. GStreamer Vulkan decoding is still experimental on the NVIDIA GPU tested here.

With `RETROFE_BUILD_TESTING=ON`, build `retrofe_ffmpeg_vulkan_interop_smoke` and run `retrofe_ffmpeg_vulkan_interop_smoke clip.mp4 [readback.bmp]`. It decodes 90 frames using FFmpeg Vulkan, presents each frame twice, then draws two retained frames in one SDL presentation. The optional BMP is a renderer readback for checking actual pixels. H.264 Exerion and an H.265 test stream passed this test locally.

## Standalone decoder check

Check the plugin path with `gst-inspect-1.0 vulkanh264dec`; its `Filename` must point to the intended `gstvulkan.dll`. On Windows, use forward slashes for `gst-launch-1.0` file locations because its pipeline parser treats backslashes as escapes. For example:

```text
gst-launch-1.0 -e filesrc location=E:/path/clip.mp4 ! qtdemux ! h264parse ! vulkanh264dec ! vulkandownload ! videoconvert ! video/x-raw,format=RGB ! pngenc ! multifilesink location=C:/temp/vulkan-%03d.png
```

Replace `vulkanh264dec ! vulkandownload` with `d3d11h264dec ! d3d11download` or `avdec_h264` to compare decoders. On the NVIDIA GeForce RTX 5070 Ti used for the prototype, `vulkanh264dec` plus `vulkandownload` produces macroblock corruption in later PNGs from `exerion.mp4`, while D3D11 and software output are clean. The original, unmodified GStreamer Vulkan library produces the same corruption. The driver reports no support for distinct DPB/output images. This isolates the artifact to the GStreamer Vulkan decode/download path; the exact fault within that path remains unresolved. The RetroFE Vulkan video path is experimental on this GPU.

A separate FFmpeg 2026-09-28 Vulkan decode of the same clip selected the RTX 5070 Ti, reported `pix_fmt: vulkan` and `Vulkan decoder initialization successful`, and produced clean PNGs through the full six-second clip. That makes a general GPU or Vulkan Video failure less likely. The local FFmpeg build from 2024 does not list Vulkan in `-hwaccels`; the comparison used a current full build with Vulkan enabled. A reproducible FFmpeg command is:

```text
ffmpeg -init_hw_device vulkan=vk:0 -filter_hw_device vk -hwaccel vulkan -hwaccel_device vk -hwaccel_output_format vulkan -i clip.mp4 -map 0:v:0 -vf "hwdownload,format=nv12,format=rgb24" -fps_mode passthrough frame-%03d.png
```

`spyhunt.mp4` exposed a different bitstream-specific failure on the same GPU: software and D3D11 decode match for all 180 frames, while two FFmpeg Vulkan builds match software only through frame 43 and show damaged blocks thereafter. A container-only remux preserves the bad Vulkan output. Re-encoding with libx264 at CRF 18, a 60-frame GOP and two B-frames makes all 180 Vulkan-decoded frame hashes match software decode. SDL's direct Vulkan texture readback of the re-encoded clip is also clean. The re-encode copies the audio stream and retains the original 1920x1080, 30 fps video dimensions/rate:

```text
ffmpeg -i spyhunt.mp4 -map 0 -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -g 60 -bf 2 -c:a copy -movflags +faststart spyhunt-reencoded.mp4
```

The original clip is playable and decodes cleanly through D3D11 and software, so this is a Vulkan Video compatibility issue with its original H.264 bitstream rather than general file damage. Do not assume every H.264 stream will behave like either Exerion or Spy Hunt; compare representative assets before bulk re-encoding.

### Spy Hunt reference-number investigation

The original and a fresh x264 `faster` encode both declare `log2_max_frame_num_minus4=0`, so H.264 `frame_num` rolls over after 16 values. The original's first Vulkan/software frame-hash difference is frame 44, three pictures after its rollover at frame 41. The new `faster` encode first differs at frame 74, one picture after its rollover at frame 73. The 2026-09-28 FFmpeg build reproduces both failures, as does the older 2026-01-26 build. Changing FFmpeg decoder thread count or extra hardware frames does not change the result.

In a controlled encode with no B-frames and fixed 18-picture GOPs, the first bad frames are 17, 35, 53, and so on, immediately after each rollover; a 17-picture GOP decodes cleanly because its IDR arrives before the affected picture. Fixed GOPs of 12 or 16 also decode cleanly. Changing bitrate mode, number of reference frames, B-frame count, or CABAC alone does not remove the problem. x264 `weightp=2` with at least two references made the tested longer-GOP encodes clean; those encodes explicitly modify the reference list, while the failing ones generally use the default list. `weightp=2` with the `veryfast` preset and one reference still failed. Together these results point to H.264 reference handling across `frame_num` rollover in this machine's FFmpeg/Vulkan decode stack. They do not identify whether FFmpeg or the NVIDIA driver is the faulty layer, and a single encoder option is not a universal fix.

The behavior also reproduces without any collection footage. A six-second `testsrc2` H.264 encode with `bframes=0:keyint=18:min-keyint=18:scenecut=0:ref=2:weightp=1` differs at frames 17, 35, 53, and so on. Changing only the GOP to 17 makes all 180 frames match. Software and D3D11VA decode of the failing synthetic clip match exactly on all 180 frames, while both the January and September 2026 FFmpeg Vulkan builds fail at the same frames. This is a small, shareable reproducer for an upstream Vulkan decoder/driver investigation:

```text
ffmpeg -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=6" -an -c:v libx264 -preset faster -crf 18 -pix_fmt yuv420p -x264-params "bframes=0:keyint=18:min-keyint=18:scenecut=0:ref=2:weightp=1" synthetic-gop18.mp4
python Prototypes/GStreamerVulkan/check_ffmpeg_vulkan_decode.py --ffmpeg C:/ffmpeg/bin/ffmpeg.exe synthetic-gop18.mp4
```

Additional affected collection clips corroborate the reference-number pattern. In the first 180 frames, `video/narc.mp4` first differs at frame 146, `videoFULL/narc.mp4` at 32, `video/blitz.mp4` (NFL Blitz) at 37, and `videoFULL/blitz.mp4` at 29. All four signal four-bit `frame_num` and have a non-IDR wrap before the first difference. Narc's long video also has earlier wraps that decode correctly, so a wrap alone is not sufficient to predict failure. The `video/blitz.mp4` stream is H.264 Main encoded by x264 core 164; the other three use x264 core 149. Thus the problem is not limited to old x264 or the High profile. The existing `NVENC` alternative for each of these four clips matched software for the first 180 frames (the complete six-second `videoFULL` clips, but only a sample of the 200-second `video` clips).

Workspace-only re-encodes of `videoFULL/narc.mp4` and `videoFULL/blitz.mp4` with the medium/CRF 18/60-frame-GOP command above matched software over all 180 frames. The installed collection copies were not changed during this investigation.

The collection also contains `videoorig` and `videoFULLorig`. For Narc and Blitz, the SHA-256 hash of the compressed H.264 stream matches the corresponding active `video`/`videoFULL` file exactly, even though the MP4 file hashes differ. The Spy Hunt long-video files are byte-identical; its active `videoFULL` file is the separately re-encoded copy. Directly checking the first 180 frames of all six `orig` files gives Vulkan/software mismatches: `videoorig/spyhunt` starts at frame 154, `videoFULLorig/spyhunt` at 44, `videoorig/narc` at 146, `videoFULLorig/narc` at 32, `videoorig/blitz` at 37, and `videoFULLorig/blitz` at 29. Thus these `orig` copies are also affected on this Vulkan path; the active Narc/Blitz copies have the same encoded video, regardless of changes elsewhere in the MP4 container.

All six existing NVENC alternatives were checked in full with `check_ffmpeg_vulkan_decode.py`: the `video/nvenc` Spy Hunt, Narc, and Blitz files each matched software for all 6,000 frames, and their `videoFULL/NVENC` versions each matched for all 180 frames. These are offline decoder comparisons; the NVENC files have not yet been installed as active collection media or checked visually in RetroFE.

The affected `orig` MP4 containers report `Lavf61.4.100`, the libavformat 61 generation used by FFmpeg 7, while the clean NVENC examples report `Lavf62.3.100` and `Lavc62.11.100 h264_nvenc`, the FFmpeg 8 generation. A `Lavf` tag records the muxer that wrote the container, not necessarily the library that originally encoded its video; the video itself reports x264 core 149 on most affected clips and core 164 on long-form Blitz. A fresh synthetic encode made with a 2026 FFmpeg/x264 build still reproduces the rollover failure, and medium-preset re-encodes from that same build pass. Thus the release number alone does not explain the failure.

### H.264 Vulkan encoder trial

The 2026-09-28 FFmpeg build successfully encoded the six-second Spy Hunt, Narc, and Blitz previews with `h264_vulkan`, `format=nv12,hwupload`, 6 Mb/s, a 60-frame GOP, and two B-frames. Software and Vulkan decoding matched for all 180 frames of each output. This establishes that Vulkan encoding can avoid the specific reference issue in these samples, but it does not by itself make it the best collection encoder.

At similar file sizes for Spy Hunt, Vulkan's default encode yielded 43.40 dB average PSNR at 4.54 MB; `-tune hq -quality 2 -rc_mode vbr` yielded 44.44 dB at 4.51 MB. A matched 6 Mb/s `h264_nvenc -preset p5 -tune hq` encode yielded 47.62 dB at 4.44 MB and also decoded cleanly. The existing smaller NVENC previews of Narc and Blitz had essentially the same PSNR as the larger default Vulkan outputs (about 39.7–39.9 dB). The `libx264 -preset medium -crf 18` previews were larger and substantially higher fidelity. These are three short samples and PSNR is only one quality measure, but there is no observed quality or compatibility reason to prefer the Vulkan encoder over the already validated NVENC or x264 options on this GPU.

The user's visible quality concern about the existing NVENC files is supported by the measurements. The existing Narc and Blitz NVENC previews are only 2.74 and 3.16 MB and yield 39.71 and 39.86 dB against their originals. Fresh `h264_nvenc -preset p5 -tune hq -b:v 6M -g 60 -bf 2` encodes improve Narc to 43.30 dB at 4.10 MB and Blitz to 43.14 dB at 3.92 MB, but remain below their `libx264 -preset medium -crf 18` copies (47.32 dB at 5.27 MB and 47.43 dB at 5.70 MB). A quality-targeted Narc encode with `-preset p7 -tune hq -rc vbr -cq 23 -b:v 0` reaches 47.51 dB at 6.24 MB; CQ 18 reaches 50.50 dB at 9.18 MB. All four new Narc/Blitz NVENC outputs match software decoding for all 180 frames through the local FFmpeg Vulkan decoder. This indicates that the observed NVENC quality loss is a rate-control/bit-budget choice rather than an inherent requirement of Vulkan-compatible output. In this short Narc sample, x264 achieves comparable measured quality in a smaller file; visual comparisons and more clips would be needed before making a collection-wide choice.

### Quality-targeted NVENC collection batch

After the user cleaned the collection and restored the original MP4s directly under `video` and `videoFULL`, `encode_nvenc_originals.ps1` maps only those folders' top-level MP4s to `video/nvenc` and `videoFULL/NVENC`. It does not descend into either NVENC destination. It also excludes `_out`/`_out_N` filename variants and interrupted `.__fftmp__` files; ordinary game names such as `OutRun` remain included. The clean source set is 1,049 previews and 959 long videos. It encodes sequentially with NVENC p7, HQ tune, VBR/CQ 23, a 60-frame GOP, two B-frames, and copied audio. It writes each output under a temporary name, probes the H.264 stream, then moves it into place. Existing outputs are skipped so the batch can resume without replacing completed files. Progress and per-file results for the current run are in `build/nvenc-batch-restored/progress.json` and `build/nvenc-batch-restored/events.jsonl`; failed files are recorded and the script continues. The selected settings favor visible fidelity, and file growth varies considerably with the source. The first restored-preview output passed a complete 180-frame software/Vulkan comparison. The earlier trial batch from the now-removed `videoorig`/`videoFULLorig` folders is recorded in `build/nvenc-batch` for historical reference.

The collection batch finished on 2026-09-29 at 21:26 local time with zero encode failures. A final inventory found 1,049 preview and 959 long outputs, each matching a source filename, with no missing or temporary files. The first 180 frames of each newly encoded Spy Hunt, Narc, and Blitz preview and long video matched between software and Vulkan decode. Full-length frame-by-frame Vulkan comparison of all 2,008 outputs has not been performed.

### HEVC collection trial

`encode_nvenc_originals.ps1 -VideoCodec hevc` writes HEVC Main 8-bit MP4s into separate `video/HEVC` and `videoFULL/HEVC` folders. The original HEVC trial used `hevc_nvenc`, p7, HQ tune, VBR/CQ 20, a 60-frame GOP, two B-frames, `hvc1` MP4 tagging, and copied audio. Its historical progress and events are in `build/hevc-batch`. The same 2,008 top-level source MP4s are eligible.

CQ 23 pilots for Spy Hunt, Narc, and Blitz passed complete 180-frame software/Vulkan FFmpeg comparisons but measured lower PSNR than the H.264 NVENC collection copies. CQ 20 also passed those comparisons and brought Spy Hunt's measured quality above its H.264 copy; Narc and Blitz remained about 1–2 dB lower at similar sizes. Lowering CQ further from 20 to 16 brought little improvement in those two clips. PSNR alone does not settle visual quality. The active RetroFE GStreamer plugin folder exposes `vulkanh265dec`, `d3d11h265dec`, `d3d12h265dec`, and `avdec_h265`; the CQ 20 Narc pilot completed standalone `gst-launch-1.0` decode pipelines through all four. These offline checks do not prove RetroFE rendering performance or absence of GStreamer Vulkan image corruption during playback.

### Tuned HEVC rebuild (2026-09-30)

The original HEVC outputs were cleared at the user's request. A first tuned run used p7/HQ/VBR/CQ 20, a 180-frame GOP for six-second previews, a 300-frame GOP for 200-second videos, two B-frames, middle B-frame references, 20-frame rate-control lookahead, and temporal AQ. It was stopped after 883 previews because the 857 completed outputs measured at the time totaled 4.761 GiB versus 3.077 GiB for the matching originals (55% larger); only 98 were smaller. Those outputs and the interrupted temporary file were removed. Historical progress is in `build/hevc-batch-tuned`.

The final run uses p7/HQ/VBR/CQ 30, the same 180/300-frame GOPs, three B-frames, middle B-frame references, 32-frame rate-control lookahead, temporal AQ, and full-resolution per-frame multipass. It copies audio, tags video `hvc1`, and uses `-movflags +faststart` so the MP4 index is at the front. The opening frame is a keyframe; longer GOPs fit normal RetroFE playback because pause does not seek, while loop and restart seek to time zero. Progress and events are in `build/hevc-batch-cq30`. Existing completed outputs are skipped on rerun. The encoder ran from 12:19 EDT on 2026-09-30 until 00:32 EDT on 2026-10-01, completing all 2,008 files with zero failures.

Final paired file-size totals are 80.325 GiB for the original H.264 set versus 48.758 GiB for the new HEVC set, saving 31.567 GiB (39.3%). The 1,049 previews fell from 3.793 to 2.296 GiB (39.48%); 1,041 previews are individually smaller. The 959 long videos fell from 76.532 to 46.462 GiB (39.29%); every long video is individually smaller. The output directories contain exactly 1,049 preview and 959 long MP4 files, with no temporary files or subdirectories. Full software/Vulkan frame comparison was done for selected pilots, not every collection output.

Before the rebuild, four long-form Pac-Man and Out Run pilots (H.264 and HEVC, 300-frame GOP) matched software/Vulkan frame hashes for all 6,000 frames each. Compared with otherwise identical 60-frame-GOP pilots, the 300-frame-GOP HEVC files were 53% smaller for Pac-Man and 13% smaller for Out Run. First-30-second PSNR changed from 59.68 to 58.16 dB for Pac-Man and from 51.25 to 51.50 dB for Out Run. The short-preview pilot files also passed full Vulkan/software frame comparisons. These are limited pilots; in-app first-frame, loop, and visual checks remain necessary after the collection rebuild.

For the CQ 30 choice, six preview pilots totaled 29% less than their originals, versus CQ 27 at approximately the same total size and CQ 24 at 34% larger. A four-title option sweep found that spatial AQ reduced measured fidelity relative to temporal AQ; the selected three-B-frame/lookahead-32/full-resolution-multipass combination saved about 4% more bytes than the simpler CQ 30 configuration at nearly the same average PSNR. Full-length Pac-Man fell from 9.02 to 8.08 MB (10%) and Out Run from 86.36 to 62.88 MB (27%). The four selected preview pilots matched software/Vulkan decoding for every frame; both long pilots matched for all 6,000 frames. These measurements do not establish collection-wide size savings or in-app first-frame latency.

```powershell
& 'C:\Program Files\PowerShell\7\pwsh.exe' -NoProfile -File Prototypes/GStreamerVulkan/encode_nvenc_originals.ps1 -Stage All
```

For future encodes, the `medium`, CRF 18, 60-frame GOP command above is a tested starting point that keeps hardware decoding. Validate each candidate by comparing all decoded frame hashes before replacing a collection file. The offline checker runs one file at a time and never modifies media:

```text
python Prototypes/GStreamerVulkan/check_ffmpeg_vulkan_decode.py --ffmpeg C:/ffmpeg/bin/ffmpeg.exe clip.mp4
```

It also accepts a folder for a recursive scan of H.264 8-bit MP4/MKV/MOV files; add `--mp4-only` to enumerate only MP4 files. `PASS` means every software and Vulkan frame hash matched; `FAIL` names the first divergent frames. Use a Vulkan-enabled FFmpeg build on the target GPU. Run the scan while RetroFE is closed so it does not compete with playback.

See [video interop regression checks](../../Tests/VIDEO-INTEROP-REGRESSIONS.md) for the current test matrix and remaining limits.
