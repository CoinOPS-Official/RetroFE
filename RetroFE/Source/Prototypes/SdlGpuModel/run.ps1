param(
    [string]$GStreamerRoot = 'C:\gstreamer\1.0\msvc_x86_64',
    [string[]]$ProgramArgs = @(),
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$build = Join-Path $PSScriptRoot 'build'
$sdlRoot = Join-Path $projectRoot 'Build\deps\SDL3-3.4.12\cmake'
$imageRoot = Join-Path $projectRoot 'Build\deps\SDL3_image-3.2.4\cmake'

if (-not $SkipBuild) {
    & cmake -S $PSScriptRoot -B $build -G 'Visual Studio 17 2022' -A x64 `
        "-DSDL3_DIR=$sdlRoot" "-DSDL3_image_DIR=$imageRoot" "-DGSTREAMER_ROOT=$GStreamerRoot"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & cmake --build $build --config Release --parallel 4
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$env:PATH = (Join-Path $GStreamerRoot 'bin') + ';' + $env:PATH
$executable = Join-Path $build 'Release\retrofe_sdl_gpu_model.exe'
& $executable @ProgramArgs
exit $LASTEXITCODE
