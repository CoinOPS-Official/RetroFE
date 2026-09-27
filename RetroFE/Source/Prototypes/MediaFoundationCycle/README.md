# Windows video cycle prototype

This standalone SDL3 program rapidly switches local videos and records time to the first decoded frame, time to the first SDL-presented frame, and stop time. It compares Media Foundation, FFmpeg, and GStreamer on the same SDL D3D11 renderer. It does not link to RetroFE.

## Build

Requirements: Windows x64, Visual Studio 2022 C++ tools, SDL3 CMake package, GStreamer MSVC x64 development/runtime SDK, and an MSVC-compatible shared FFmpeg SDK. The paths below match this workstation; set the three paths for another installation.

```powershell
cmake -S Prototypes/MediaFoundationCycle -B Prototypes/MediaFoundationCycle/build -A x64 `
  -DSDL3_DIR=C:/Users/ohmys/source/repos/RetroFE/RetroFE/Build/deps/SDL3-3.4.12/cmake `
  -DGSTREAMER_ROOT=C:/gstreamer/1.0/msvc_x86_64 `
  -DFFMPEG_ROOT=E:/ffmpeg-9.0.2-full_build-shared
cmake --build Prototypes/MediaFoundationCycle/build --config Release --parallel
```

## Run

```powershell
./Prototypes/MediaFoundationCycle/run.ps1 `
  -Media @('C:\videos\a.mp4', 'C:\videos\b.mp4') `
  -Backend all -Mode both -Rounds 10 -CycleMs 500
```

Use `-HiddenWindow` for repeatable unattended runs and `-Csv` to choose the CSV path. `-Backend` accepts `all`, `mf`, `ffmpeg`, or `gstreamer`; `-Mode` accepts `fresh`, `reuse`, or `both`. `run.ps1` adds the GStreamer and FFmpeg runtime DLL directories to `PATH`. Override `-GStreamerRoot`, `-FFmpegRoot`, and `-BuildDirectory` if needed. Escape literal commas in PowerShell file names or pass paths in the `-Media` array.

For visible end-to-end presentation checks, omit `-HiddenWindow`. With a hidden window, `presented_ms` measures the return from `SDL_RenderPresent`, not a monitor scanout or compositor acknowledgement.

The console prints every cycle and a median first-present time for each backend/mode. The CSV preserves each file, round, timing, frame count, decoder path, and error. A missed frame has an empty timing and an error field. Start-to-first-frame timing includes opening or retargeting the media source, decoder setup, and the first presentation; it does not include the black frame shown between cycles. `stop_ms` measures stop request through worker completion. The last line measures release of retained backends.

The process exits with code 2 if any cycle misses its first frame or reports a backend error, so unattended runs can detect invalid measurements.

## Reuse behavior

- Media Foundation uses a Source Reader to demux compressed H.264 samples and a D3D11-aware decoder MFT to produce DXGI NV12 frames. `fresh` creates both per cycle. `reuse` opens a new Source Reader for each file but retains and flushes the **same decoder MFT instance** when frame dimensions and H.264 sequence headers match exactly. An incompatible file creates a new decoder MFT. The `path` column reports which happened. The Source Reader itself has no URI retarget operation.
- FFmpeg `fresh` creates and destroys its D3D11VA context, demuxer, and decoder each cycle. `reuse` retains the hardware device context. It seeks and flushes the retained demuxer/decoder for the same file; for another file it reopens the demuxer and keeps the decoder only when its stream parameters and codec extradata match exactly, as in RetroFE's conservative exact reuse path. The `path` column identifies which transition occurred. A reused mode can still create a decoder when the clips are incompatible.
- GStreamer `fresh` builds and tears down `playbin3`/`appsink` each cycle. `reuse` keeps them in PAUSED state, drains old sink samples, and uses `instant-uri` to retarget the pipeline, matching RetroFE's retained pipeline approach.

The program uses RetroFE's D3D11 interop pattern: set `SDL_HINT_RENDER_DIRECT3D_THREADSAFE` **before** creating the SDL renderer, wrap its D3D11 device for GStreamer, lock that wrapper while copying GStreamer samples, and copy decoder-owned NV12 surfaces into a three slot ring of SDL-owned NV12 textures before drawing. This lets decoder samples retire promptly. The prototype requires SDL's D3D11 thread protection interface and fails early if it is unavailable.

## Interpretation

FFmpeg requires `AV_PIX_FMT_D3D11` output and GStreamer requires `D3D11Memory`. Media Foundation selects a D3D11-aware H.264 decoder MFT, passes it the DXGI device manager, and requires DXGI NV12 output. These checks confirm hardware-surface delivery; they do not independently measure how much decoding work the GPU performed. MF's direct decoder path currently supports H.264 files with a sequence header. Incompatible headers or dimensions force MFT recreation in reuse mode.

This is a file-open and first-frame experiment, not playback quality testing: it skips audio, does not pace frames by presentation timestamps, and presents the newest available frame until the cycle deadline. The first run may include codec/plugin initialization and disk-cache effects. Use multiple rounds and compare matching files and modes; for a pure FFmpeg same-file reuse check, give one file with `-Backend ffmpeg -Mode reuse -Rounds 3`.
