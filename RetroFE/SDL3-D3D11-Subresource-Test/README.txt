SDL3 D3D11 subresource test
=============================

Target: SDL release-3.4.12

Files:
  Build-SDL3-Patched.ps1
  Patches/SDL3-d3d11-subresource.patch

The build script:
  1. Clones SDL release-3.4.12.
  2. Applies the D3D11 subresource patch.
  3. Locates fxc.exe from the Windows SDK.
  4. Compiles D3D11_PixelShader_AdvancedArray.hlsl to the generated header.
  5. Builds and installs patched SDL3.
  6. Builds RetroFE against the patched SDL3.
  7. Copies the patched SDL3.dll beside the RetroFE executable.

The patch adds:
  SDL_PROP_TEXTURE_CREATE_D3D11_SUBRESOURCE_NUMBER
  Texture2DArray SRVs selecting a single D3D11 subresource
  NV12/NV21/P010 array-backed texture handling
  a Texture2DArray variant of SDL's existing advanced D3D11 pixel shader

RetroFE still has to pass the property when wrapping GstD3D11Memory in order
to exercise this path.

Important diagnostic:
If SDL reports:
  "External D3D11 texture subresource is not shader-readable"
then the decoder-created texture array does not carry D3D11_BIND_SHADER_RESOURCE
and true direct sampling of that decoder surface is not available through this
route. In that case the GPU-to-GPU copy remains necessary.

Note: compile_shaders.bat is intentionally not patched; Build-SDL3-Patched.ps1 invokes fxc directly for D3D11_PixelShader_AdvancedArray.h.
