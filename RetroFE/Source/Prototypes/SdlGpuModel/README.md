# SDL_GPU cabinet prototype

This standalone Windows prototype checks that RetroFE can render a 3D cabinet
through SDL_GPU, then treat the finished image as a normal SDL renderer texture.
It does not change the RetroFE application or its component system.

## Build and run

Requires the RetroFE repository's SDL 3.4.12 and SDL3_image 3.2.4 SDKs in
`RetroFE/Build/deps`, Visual Studio 2022, the Windows SDK `dxc` compiler,
Python 3 only to regenerate the included fixture, and the GStreamer MSVC x64
development/runtime packages (including `avdec_h264` for video). The SDK root
defaults to `C:\gstreamer\1.0\msvc_x86_64`.

From PowerShell in this directory:

```powershell
./run.ps1
```

The default scene uses a procedural cabinet and an animated test pattern. To
exercise GLB loading and H.264 software decoding with repository assets:

```powershell
./run.ps1 -ProgramArgs @(
    '--model', 'Assets/demo-cabinet.glb',
    '--video', '../../../../Package/Environment/Common/layouts/Arcades/video/splash.mp4'
)
```

Paths passed to the program are resolved from the process's current working
directory. `run.ps1` uses the caller's current directory. Supply absolute paths
if launching from elsewhere. The video is optional. It must be an H.264 MP4
that GStreamer's `qtdemux`, `h264parse`, and `avdec_h264` can decode.

Useful program options: `--single`, `--duration 10`, `--screenshot capture.bmp`,
`--uncapped`, `--checker-background`, `--msaa 1|2|4|8`,
`--anisotropy 1|2|4|8|16`, `--render-scale 1.2`, `--fov 48`,
`--window-width 1280`, `--window-height 720`,
`--screen-material CRT_Image`,
`--material-metallic Aluminum=0.9`, `--material-roughness Coin_Plaque=0.2`,
`--rotate-x 10`, `--rotate-y 15`, `--rotate-z 0`,
`--scale 1`, `--camera-x/y/z`, `--target-x/y/z`, `--screen-emissive 2.8`, and
`--marquee-emissive 2.5`. `--screenshot` is saved at the end of a timed run.
Press Escape to close an interactive run. The default loop is limited to about
60 FPS; `--uncapped` disables that limit and requests an immediate or mailbox
present mode when supported.

Lighting options: `--key-light 4`, `--fill-light 1`, `--ambient-light 0.6`,
`--screen-light 1.5`, `--marquee-light 1`, and `--matcap-strength 0`.
`--environment-intensity` is an alias for `--ambient-light`; use
`--environment-rotation 180` to rotate the generated HDR studio environment in degrees.
Geometric specular antialiasing and rough-metal energy compensation are on by
default; `--no-specular-aa` and `--no-multiscatter` isolate their visual effect.
Authored glTF occlusion textures use the red channel and their `strength`
setting. A small screen-space AO pass adds contact shading when an asset has no
AO map; `--no-ssao` isolates that pass. `--no-shadows` disables the four
shadow-map passes for the key, fill, screen, and marquee lights.
Material overrides are repeatable, match names exactly, and take values from
0 to 1. They adjust the effective glTF factor without changing the asset.
`--key-x/y/z` and `--fill-x/y/z` move the two studio lights in world space.
Their default positions are `(-1.8, 2.9, -2.3)` and `(1.6, 1.7, -1.2)`.
The video-derived screen light and warm marquee light move with each cabinet
instance. The generated matcap is an optional camera-relative softbox effect;
raise its strength above `0` to enable it. These lights are approximate and do
cast 512 × 512 depth-map shadows with a 3 × 3 filter. The maps cover a cone
aimed at the cabinet, so they are not omnidirectional point-light shadows.
The default display mapping is `--tonemap filmic --exposure 1`. The HDR
studio lights may clip light cabinet paint if exposure is raised much above 1.
Exposure changes the final image without changing the screen, marquee, or
studio light intensities; lower exposure for a darker image.

The cabinet pass uses **4× MSAA and a 32-bit floating-point depth buffer by
default**. `--msaa 1` disables MSAA, while
`--msaa 2`, `4`, or `8` requests that sample count. The program checks HDR color
and depth format support and falls back to the highest supported count no
greater than the request. MSAA smooths model silhouette and geometry edges;
it does not address texture shimmer or temporal aliasing.
`--checker-background` draws alternating light and dark 2D tiles beneath the
cabinet textures to make transparency and edge fringes easy to inspect.
The loader generates full mip chains for GLB textures, and the live CRT video
gets new mip levels after each upload. Minification uses trilinear filtering;
8× anisotropic filtering is the default for angled surfaces. Set
`--anisotropy 1` to disable anisotropy. These filters reduce texture shimmer;
they do not provide temporal antialiasing for moving geometry.
Offscreen targets resize with the SDL output size, including after a window
resize. `--render-scale 1.2` renders at 120% of each cabinet's displayed size;
lower it toward `1` if the larger HDR/MSAA targets use too much GPU memory.
Target height is capped at 2048 pixels.

To regenerate the small original GLB fixture:

```powershell
python make_demo_glb.py
```

For a material comparison fixture, run `python make_material_swatch_glb.py`
and load `Assets/metal-roughness-swatch.glb`. Its columns increase metallic
from 0 to 1; its rows increase roughness from 0.09 to 1.

## Architecture checked

1. One `SDL_GPUDevice` is passed to `SDL_CreateGPURenderer`. The program checks
   that `SDL_GetGPURendererDevice` returns that same pointer.
2. A single custom GPU command buffer uploads a new screen frame when available,
   renders three cabinet instances into separate multisampled HDR/depth targets,
   resolves each to a single-sample HDR texture, applies a bright pass and two
   bloom blur passes, and tone maps each result to RGBA8.
3. Each final `SDL_GPUTexture` is wrapped with
   `SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER` and drawn by ordinary
   `SDL_RenderTexture` calls over an SDL-drawn background. The wrappers are
   destroyed before their GPU textures.
   The HDR pass clears to transparent black. MSAA resolves fractional coverage
   into the color and alpha channels. The final shader recovers straight color
   for tone mapping, then stores premultiplied SDR color; the SDL texture uses
   `SDL_BLENDMODE_BLEND_PREMULTIPLIED`. Areas outside the cabinet have zero
   alpha except for the intended bloom glow.
4. All three instances share one immutable `ModelResource` (mesh and material
   GPU resources). Target sizes follow their displayed sizes: at a 1280 × 720
   output with the default render scale, the center is 512 × 768 and the sides
   are 320 × 480. A single-cabinet view allocates only the center target. One
   video source and one screen GPU texture are shared across all instances.
5. The GLB loader handles static triangle meshes, indexed or non-indexed
   primitives, node transforms, embedded and local image textures, base color,
   metallic/roughness, normal maps, emissive materials, alpha mask/blend,
   `KHR_materials_specular` factors, and
   a simple approximation of `KHR_materials_transmission` for glass.
   It computes missing normals/tangents, generates filtered mip levels, and
   converts glTF's right handed +Z front orientation into the scene's left
   handed -Z front orientation. A
   material named `retrofe.screen` receives the live screen texture;
   `--screen-material name` selects another material, such as `CRT_Image` in
   the Gorf cabinet. `retrofe.marquee` retains its image and is rendered
   emissively.
6. The video path explicitly uses GStreamer `avdec_h264`, `videoconvert`,
   `videoscale`, 640 × 480 RGBA caps, and `appsink`. The render thread keeps the
   newest sample, copies it into owned CPU memory, computes a small average
   color sample for screen light spill, and uploads only when a frame arrives.
   EOS seeks to the start; shutdown sets the pipeline to `NULL` before release.

The PBR shader uses metallic/roughness factors and textures, normal maps,
world-space studio lights, local screen/marquee lights, Fresnel/GGX specular
response, diffuse irradiance, GGX-prefiltered reflections, a BRDF lookup,
authored and screen-space AO, shadow maps, and an optional generated matcap.
Color images and the video screen use sRGB
texture formats; normal and metallic/roughness maps stay linear data. A
transparent `ScreenGlass` material is included in the fixture. Bloom remains
inside `ModelRenderer`, and the surrounding SDL renderer stays SDR.

## Validation and limits

On the development machine, a two-second three-cabinet Gorf run on SDL
3.4.12's Direct3D 12 backend with 4× MSAA, AO, shadows, trilinear mipmaps,
and 8× anisotropy reported about 60 FPS and 111.6 MiB estimated GPU resource
payload at 1280 × 720. This is a capped smoke test, not a sustained or
low-end-device benchmark.

The program reports FPS, process CPU, decoded frame count, upload count,
average CPU time spent staging each GPU upload, and an approximate GPU resource
payload. It does not measure GPU utilization, exact GPU memory allocation,
GPU execution time, decoder-only CPU time, or frame-time percentiles. Use
Windows GPU performance counters or a GPU profiler for those measurements,
and repeat on an N100-class machine before judging production viability.

The fixture is deliberately blocky. The user-supplied Gorf cabinet GLB was
also rendered as a larger import test: 27,557 vertices, 33 triangle primitives,
32 materials, and 14 images. Its `CRT_Image` material accepted the live video
override. The AO and shadow passes have not yet been profiled on an N100-class
machine or tested at 2560 × 1392.
The Gorf model has coin door layers only about 0.001 model units apart; the
32-bit depth buffer resolves them cleanly in the test view. The GLB is not
included in this prototype. The loader does not implement skeletal/morph
animation, most glTF material extensions and extension textures, multiple UV sets,
or complete alpha sorting. Authored occlusion maps currently use UV set 0 only;
the screen-space pass cannot see hidden geometry. The built-in environment is a
linear HDR studio probe with floating-point diffuse and prefiltered specular
IBL, not a user-supplied HDR image. The Gorf GLB has roughness factors but no
metallic/roughness texture; no authored surface variation can appear from it.
See `PBR_AUDIT.md` for the Filament comparison and remaining gaps.
Transmission preserves the CRT image under the glass but does
not refract the scene as a full glTF viewer would. All three screens share one
video stream. The source video
is decoded at its encoded resolution before conversion to the 640 × 480
upload texture. This first build uses DXIL and therefore targets Windows.

SDL API references:
[GPU renderer](https://wiki.libsdl.org/SDL3/SDL_CreateGPURenderer),
[GPU texture wrapping](https://wiki.libsdl.org/SDL3/SDL_CreateTextureWithProperties),
[GPU texture upload](https://wiki.libsdl.org/SDL3/SDL_UploadToGPUTexture),
[MSAA format support](https://wiki.libsdl.org/SDL3/SDL_GPUTextureSupportsSampleCount),
[render pass resolve](https://wiki.libsdl.org/SDL3/SDL_GPUColorTargetInfo).
