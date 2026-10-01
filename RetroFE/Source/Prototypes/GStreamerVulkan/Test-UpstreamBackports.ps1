param(
    [string]$GStreamerRoot = 'C:/gstreamer/1.0/msvc_x86_64',
    [string]$SourceRoot = "$PSScriptRoot/build/gst-plugins-bad-1.28.7",
    [string]$MesonRoot = "$PSScriptRoot/build/meson-vulkan",
    [string]$VulkanSdkRoot = "$PSScriptRoot/build/sdk",
    [string]$RuntimeRoot = "$PSScriptRoot/build/retrofe-final/tests/Release"
)

# Run in an x64 Visual Studio Developer PowerShell after staging both DLLs.
$ErrorActionPreference = 'Stop'
$testPlugins = "$MesonRoot/upstream-test-plugins"
New-Item -ItemType Directory -Path $testPlugins -Force | Out-Null
Copy-Item -LiteralPath "$GStreamerRoot/lib/gstreamer-1.0/gstvideotestsrc.dll" -Destination $testPlugins -Force
$env:GST_PLUGIN_PATH = "$RuntimeRoot/gst-plugins;$testPlugins"
$env:GST_PLUGIN_SYSTEM_PATH = "$RuntimeRoot/gst-plugins"
$env:GST_PLUGIN_SCANNER = "$RuntimeRoot/gst-plugin-scanner.exe"
$env:GST_REGISTRY = "$MesonRoot/upstream-tests-registry.bin"
$env:PATH = "$RuntimeRoot;$GStreamerRoot/bin;$env:PATH"
Remove-Item Env:GST_CHECKS -ErrorAction SilentlyContinue

foreach ($spec in @(@('elements', 'vkdownload'), @('elements', 'vkupload'), @('libs', 'vkimagebufferpool'))) {
    $name = $spec[1]
    $binary = "$RuntimeRoot/upstream-$name.exe"
    & cl /nologo /MD /O2 /std:c11 /utf-8 /DGST_USE_UNSTABLE_API /DHAVE_CONFIG_H `
        "/I$MesonRoot" "/I$MesonRoot/gst-libs" "/I$SourceRoot/gst-libs" "/I$VulkanSdkRoot/Include" `
        "/I$GStreamerRoot/include" "/I$GStreamerRoot/include/gstreamer-1.0" `
        "/I$GStreamerRoot/include/glib-2.0" "/I$GStreamerRoot/lib/glib-2.0/include" `
        "$SourceRoot/tests/check/$($spec[0])/$name.c" "/Fo$MesonRoot/$name.obj" "/Fe$binary" `
        /link "/LIBPATH:$GStreamerRoot/lib" "$MesonRoot/gst-libs/gst/vulkan/gstvulkan-1.0.lib" `
        "$VulkanSdkRoot/Lib/vulkan-1.lib" gstcheck-1.0.lib gstvideo-1.0.lib gstbase-1.0.lib `
        gstreamer-1.0.lib gobject-2.0.lib glib-2.0.lib
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "Upstream regression failed: $name" }
}
