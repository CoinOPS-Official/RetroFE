param(
    [Parameter(Mandatory = $true, Position = 0)] [string[]]$Media,
    [ValidateSet('all', 'mf', 'ffmpeg', 'gstreamer')] [string]$Backend = 'all',
    [ValidateSet('both', 'fresh', 'reuse')] [string]$Mode = 'both',
    [int]$Rounds = 5,
    [int]$CycleMs = 500,
    [string]$Csv = "$PSScriptRoot\build\results.csv",
    [switch]$HiddenWindow,
    [string]$BuildDirectory = "$PSScriptRoot\build",
    [string]$GStreamerRoot = 'C:\gstreamer\1.0\msvc_x86_64',
    [string]$FFmpegRoot = 'E:\ffmpeg-9.0.2-full_build-shared'
)

$ErrorActionPreference = 'Stop'
$executable = Join-Path $BuildDirectory 'Release\mediafoundation_cycle.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "Build the prototype first: $executable" }
$env:PATH = "$(Join-Path $GStreamerRoot 'bin');$(Join-Path $FFmpegRoot 'bin');$env:PATH"
$arguments = @('--backend', $Backend, '--mode', $Mode, '--rounds', "$Rounds", '--cycle-ms', "$CycleMs", '--csv', $Csv)
if ($HiddenWindow) { $arguments += '--hidden' }
foreach ($file in $Media) { $arguments += (Resolve-Path -LiteralPath $file).Path }
& $executable @arguments
exit $LASTEXITCODE
