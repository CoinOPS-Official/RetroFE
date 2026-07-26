param(
    [Parameter(Position = 0, Mandatory = $true)]
    [string]$Media,

    [int]$Seconds = 0,

    [switch]$HiddenWindow,

    [switch]$Bounce,

    [switch]$Overlay,

    [switch]$Vsync,

    [string]$BuildDirectory = "$PSScriptRoot\build",

    [string]$GStreamerRoot = "C:\gstreamer\1.0\msvc_x86_64"
)

$ErrorActionPreference = "Stop"

$executable = Join-Path $BuildDirectory "Release\gst_sdl3_nv12_copy.exe"
if (-not (Test-Path -LiteralPath $executable)) {
    throw "Prototype executable not found: $executable"
}

$gstreamerBin = Join-Path $GStreamerRoot "bin"
$env:PATH = "$gstreamerBin;$env:PATH"

$arguments = @($Media)

if ($Seconds -gt 0) {
    $arguments += @("--seconds", "$Seconds")
}
if ($HiddenWindow) {
    $arguments += "--hidden"
}
if ($Bounce) {
    $arguments += "--bounce"
}
if ($Overlay) {
    $arguments += "--overlay"
}
if ($Vsync) {
    $arguments += "--vsync"
}

& $executable @arguments
exit $LASTEXITCODE
