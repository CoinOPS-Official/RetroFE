# Compatibility entry point for the original isolated SDL3 build instructions.
param(
    [string]$BuildDirectory = "$PSScriptRoot/../Build",
    [string]$GStreamerRoot = "C:/gstreamer/1.0/msvc_x86_64",
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release'
)
& "$PSScriptRoot/Build.ps1" @PSBoundParameters
