# Filament PBR comparison

Reference: [Filament's PBR model](https://google.github.io/filament/main/filament.html),
[material color handling](https://google.github.io/filament/main/materials.html),
[Filament's tone-mapper source](https://github.com/google/filament/blob/main/filament/src/ToneMapper.cpp),
and, for the Gorf asset's extension,
[KHR_materials_specular](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_specular/README.md).
This is an audit of the standalone SDL_GPU prototype, not a claim of pixel parity.
The built-in HDR studio probe is a test light rig, not Sketchfab's environment.

| Area | Current implementation | Remaining difference or check |
| --- | --- | --- |
| sRGB to linear; base color | Color and emissive GLB images use sRGB GPU textures; raw data images use UNORM. Shared images get separate color/data views. Texture filtering and mip generation therefore occur after color decoding. glTF factors are multiplied in linear space. | Validate color-management behavior on a color chart under a neutral light. |
| Metallic/roughness | glTF green channel controls perceptual roughness, blue channel controls metallic; factors multiply the channels. | No texture-coordinate set selection or texture transform support yet. |
| Normal mapping | Tangent-space RGB is read as data, remapped to [-1, 1], XY scaled, and transformed by TBN and tangent handedness. The normal uses inverse-transpose for nonuniform transforms. | Generated tangents are not MikkTSpace. Gorf has no normal maps, so it cannot validate this path. |
| Dielectric F0; Fresnel | Default dielectric F0 is 0.04, metal F0 is base color. Schlick Fresnel uses the material's F0/F90. The Gorf model's `KHR_materials_specular` factor/color values now affect these terms; `Full_Black` has zero dielectric specular. | `KHR_materials_specular` textures and `KHR_materials_ior` remain unsupported. |
| GGX; Smith; diffuse/specular energy | GGX NDF uses alpha = perceptual roughness squared; visibility uses Filament's height-correlated Smith GGX. Direct diffuse is reduced by the maximum Fresnel component. The existing DFG LUT's scale+bias gives directional albedo `r`; Filament's `1 + F0(1/r - 1)` compensation now restores rough-metal specular energy for direct and indirect lighting. `--no-multiscatter` isolates this change. | This is Filament's scaled-lobe approximation, not a stochastic multiple-scattering BRDF. |
| Roughness | Perceptual roughness is clamped to 0.089 before BRDF and IBL lookup, matching Filament's documented safe floor. Geometric normal derivatives broaden narrow highlights using Filament's default variance 0.15 and threshold 0.2; `--no-specular-aa` isolates the change. | Normal-map texture variance is not yet folded into roughness. |
| Diffuse IBL | The generated linear HDR environment is integrated with cosine weighting into a 32×16 floating-point diffuse irradiance map. The shader multiplies it by diffuse color. | No user-supplied HDR probe input or bent-normal/environment visibility. |
| Specular IBL; DFG | GGX importance sampling generates floating-point roughness mip levels without clipping radiance above 1. A 64×64 two-channel split-sum DFG LUT supplies the view/roughness BRDF term and energy-compensation denominator. The shader selects LOD from perceptual roughness. | The procedural softboxes are a useful material test but do not match the source viewer's lighting or reflected surroundings. The DFG remains 8-bit UNORM. |
| Environment controls | `--environment-intensity` scales indirect light and `--environment-rotation` rotates both diffuse and specular lookups. Longitude wraps. Matcap is off by default. | A real HDR environment loader and matching probe are the next major visual comparison step. |
| Exposure; tone mapping | `--exposure` is a linear multiplier before tone mapping. Output uses exact linear-to-sRGB transfer. `--tonemap filmic` uses the same Narkowicz curve as Filament's *FilmicToneMapper* and is the prototype default. | Filament's default is *ACESLegacyToneMapper* with a broader color-grading pipeline. Exposure here is not tied to camera EV or photometric light units. |
| Emissive | Emissive texture values decode from sRGB and combine with linear factors and `KHR_materials_emissive_strength`. Video and marquee emission enter HDR before bloom and tone mapping. Their approximate light spill now uses shadow visibility. | Screen/marquee spill still uses a local cone and receiver-height approximation rather than integrating emitter geometry. |
| Ambient occlusion | glTF `occlusionTexture` samples its linear red channel with the authored `strength`. A 16-sample screen-space pass adds contact AO for assets like Gorf, which has no AO map. The two terms multiply and reduce indirect diffuse and specular only; `--no-ssao` isolates the screen-space pass. The one-pixel occlusion swatch tests the authored path. | Screen-space AO cannot account for hidden surfaces and may show view-dependent edge artifacts. Occlusion textures using UV set 1 or a texture transform are not yet supported. |
| Direct attenuation | The existing key, fill, screen, and marquee use an artist-tuned `1/(1 + k d²)` falloff. | Filament uses inverse-square attenuation with a finite source radius and smooth distance window. Changing this will require retuning the current light intensities. |
| Shadows; bloom | Each active key, fill, screen, and marquee light renders a 512² depth map per cabinet. Opaque and alpha-mask geometry casts 3×3 filtered shadows on direct light; `--no-shadows` isolates them. | Perspective shadow cones do not cover all directions around a point light. Blended surfaces do not cast; the studio IBL remains unshadowed. Bloom extraction uses a fixed pre-exposure threshold. |

## Reproduce the Gorf comparison

From PowerShell in `Prototypes/SdlGpuModel`, use the installed Gorf GLB:

```powershell
./run.ps1 -ProgramArgs @(
  '--model', 'C:\Users\ohmys\Downloads\gorf_-_midway_arcade_machine.glb',
  '--single', '--window-width', '800', '--window-height', '900',
  '--camera-z', '-2.8', '--rotate-y', '-8.59',
  '--tonemap', 'filmic', '--exposure', '1',
  '--duration', '1', '--screenshot', 'build\gorf-pbr.bmp'
)
```

For isolated IBL inspection add `--key-light 0 --fill-light 0
--matcap-strength 0`. Change `--environment-rotation 0` to `180` to check
whether light and reflections move coherently. Change `--environment-intensity`
without changing exposure to compare indirect-light response. Keep camera,
exposure, environment, and light settings identical when comparing screenshots.

For a material sweep, run `python make_material_swatch_glb.py` and render
`Assets/metal-roughness-swatch.glb`. The four columns are metallic values 0,
1/3, 2/3, and 1; the five rows, top down, are roughness 0.09, 0.25, 0.45,
0.70, and 1. Compare with `--no-multiscatter` to see rough metal darkening in
the bottom-right sphere, or `--no-specular-aa` to inspect the sharpest row.
Use repeatable `--material-metallic name=value` and
`--material-roughness name=value` to tune individual imported materials at
runtime. A metallic value near 1 needs a reflective base color: turning a
near-black painted coin door into a fully metallic material makes it look black.
The Gorf asset uses metallic and roughness factors but has no metallic/roughness
texture, so its surfaces cannot show spatial roughness variation. The swatch
confirms the roughness sweep is working: glossy rows reflect distinct softboxes,
while rough rows blur them. Shadow maps and screen-space AO now darken some
recesses and contacts. The synthetic environment still lacks the reflected
scene detail in Sketchfab's viewer.

Run `python make_material_swatch_glb.py --occlusion-test` to produce
`Assets/occlusion-swatch.glb`. It embeds a red-channel AO value of 64/255
with strength 0.8. Render it with `--no-ssao --no-shadows` and compare it to
the standard swatch under the same light settings to isolate authored AO.

## Next visual passes

1. Load a real linear HDR environment and run the same irradiance, GGX
   prefilter, and DFG path on it. Match environment orientation and exposure
   to the reference viewer before adjusting material parameters.
2. Compare AO and direct shadows against a reference view with known light
   positions. Add temporal or spatial filtering if screen-space AO bands or
   shadow-map edges become noticeable during motion.
3. Offer Filament's ACES legacy tone mapping and an exposure-value control,
   then compare neutral color and bright emissive patches.
4. Switch point lights to finite, windowed inverse-square attenuation and
   retune their intensity. Replace the local screen/marquee cone approximation
   with emitter geometry and a physically based visibility calculation. Revisit
   bloom thresholds after exposure is calibrated.
