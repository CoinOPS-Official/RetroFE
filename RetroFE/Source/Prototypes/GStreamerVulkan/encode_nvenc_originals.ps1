param(
    [ValidateSet('All', 'Full', 'Long')]
    [string]$Stage = 'All',
    [ValidateSet('h264', 'hevc')]
    [string]$VideoCodec = 'h264',
    [int]$MaxFiles = 0,
    [string]$MediaRoot = 'E:\CoinOPS ARISE Max (2026)\collections\_common\medium_artwork',
    [string]$ToolRoot = (Join-Path $PSScriptRoot 'build\ffmpeg-2026-09-28-git-84779ade26-full_build\bin')
)

$ErrorActionPreference = 'Stop'
$ffmpeg = Join-Path $ToolRoot 'ffmpeg.exe'
$ffprobe = Join-Path $ToolRoot 'ffprobe.exe'
$destinationName = if ($VideoCodec -eq 'hevc') { 'HEVC' } else { 'NVENC' }
$encoder = if ($VideoCodec -eq 'hevc') { 'hevc_nvenc' } else { 'h264_nvenc' }
$quality = if ($VideoCodec -eq 'hevc') { 30 } else { 23 }
$logFolder = if ($VideoCodec -eq 'hevc') { 'build\hevc-batch-cq30' } else { 'build\nvenc-batch-restored' }
$logRoot = Join-Path $PSScriptRoot $logFolder
$eventLog = Join-Path $logRoot 'events.jsonl'
$progressPath = Join-Path $logRoot 'progress.json'

if (!(Test-Path -LiteralPath $ffmpeg -PathType Leaf) -or !(Test-Path -LiteralPath $ffprobe -PathType Leaf)) {
    throw "FFmpeg tools missing in $ToolRoot"
}
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null

$pairs = @()
if ($Stage -in @('All', 'Full')) {
    $pairs += @{ Name = 'Full'; Source = 'videoFULL'; Destination = "videoFULL\$destinationName" }
}
if ($Stage -in @('All', 'Long')) {
    $pairs += @{ Name = 'Long'; Source = 'video'; Destination = "video\$destinationName" }
}

$jobs = @()
foreach ($pair in $pairs) {
    $sourceRoot = (Resolve-Path -LiteralPath (Join-Path $MediaRoot $pair.Source)).Path
    New-Item -ItemType Directory -Path (Join-Path $MediaRoot $pair.Destination) -Force | Out-Null
    $destRoot = (Resolve-Path -LiteralPath (Join-Path $MediaRoot $pair.Destination)).Path
    # Source MP4s are direct children. Never recurse into the destination subfolder.
    foreach ($file in (Get-ChildItem -LiteralPath $sourceRoot -File -Filter '*.mp4' | Sort-Object FullName)) {
        # Export variants and interrupted original-side transcodes are not collection assets.
        if ($file.BaseName -match '(?i)(?:^out(?:_|$)|_out(?:_\d+)?$)' -or
            $file.Name -match '(?i)\.__fftmp__') {
            continue
        }
        $relative = [IO.Path]::GetRelativePath($sourceRoot, $file.FullName)
        if ([IO.Path]::IsPathRooted($relative) -or $relative.StartsWith('..')) {
            throw "Unexpected relative path: $relative"
        }
        $target = [IO.Path]::GetFullPath((Join-Path $destRoot $relative))
        if (!$target.StartsWith($destRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Output escaped destination: $target"
        }
        $jobs += [pscustomobject]@{ Stage = $pair.Name; Source = $file.FullName; Target = $target }
    }
}

$encoded = 0
$skipped = 0
$failed = 0
$processed = 0
$started = Get-Date

function Write-Event([string]$status, $job, [string]$details = '') {
    $event = [ordered]@{
        time = (Get-Date).ToString('o')
        status = $status
        stage = if ($job) { $job.Stage } else { $Stage }
        source = if ($job) { $job.Source } else { $null }
        target = if ($job) { $job.Target } else { $null }
        details = $details
    }
    $eventJson = $event | ConvertTo-Json -Compress -Depth 3
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        try {
            Add-Content -LiteralPath $eventLog -Value $eventJson
            return
        }
        catch {
            Start-Sleep -Milliseconds 100
        }
    }
}

function Write-Progress($job, [string]$state) {
    $progress = [ordered]@{
        started = $started.ToString('o')
        updated = (Get-Date).ToString('o')
        state = $state
        pid = $PID
        total = $jobs.Count
        processed = $processed
        encoded = $encoded
        skipped = $skipped
        failed = $failed
        current = if ($job) { $job.Source } else { $null }
    }
    $progressJson = $progress | ConvertTo-Json -Depth 3
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        try {
            Set-Content -LiteralPath $progressPath -Value $progressJson
            return
        }
        catch {
            Start-Sleep -Milliseconds 100
        }
    }
    # A status-file reader must never terminate an otherwise healthy encode.
    Write-Event 'progress-update-skipped' $job 'progress.json remained locked for two seconds'
}

$settingsSummary = if ($VideoCodec -eq 'hevc') {
    'GOP=180 previews/300 long; B-frames=3; lookahead=32; b_ref=middle; temporal_aq=1; multipass=fullres'
} else {
    'GOP=60'
}
Write-Event 'batch-start' $null "Jobs=$($jobs.Count); MaxFiles=$MaxFiles; codec=$VideoCodec; CQ=$quality; preset=p7; $settingsSummary"
Write-Progress $null 'running'

foreach ($job in $jobs) {
    if ($MaxFiles -gt 0 -and $encoded -ge $MaxFiles) { break }
    if (Test-Path -LiteralPath $job.Target -PathType Leaf) {
        $skipped++
        $processed++
        Write-Event 'skipped-existing' $job
        Write-Progress $job 'running'
        continue
    }

    $targetDirectory = Split-Path -Parent $job.Target
    New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
    $temporary = "$($job.Target).$PID.encoding.mp4"
    if (Test-Path -LiteralPath $temporary) {
        throw "Temporary output already exists: $temporary"
    }

    Write-Event 'encode-start' $job
    Write-Progress $job 'running'
    $errorText = ''
    try {
        $gop = if ($VideoCodec -eq 'hevc') {
            if ($job.Stage -eq 'Long') { '300' } else { '180' }
        } else { '60' }
        $bFrames = if ($VideoCodec -eq 'hevc') { '3' } else { '2' }
        $encodeArgs = @(
            '-nostdin', '-hide_banner', '-loglevel', 'error', '-n',
            '-i', $job.Source, '-map', '0:v:0', '-map', '0:a?',
            '-c:v', $encoder, '-preset', 'p7', '-tune', 'hq',
            '-rc', 'vbr', '-cq', [string]$quality, '-b:v', '0',
            '-g', $gop, '-bf', $bFrames, '-pix_fmt', 'yuv420p'
        )
        if ($VideoCodec -eq 'hevc') {
            $encodeArgs += @(
                '-b_ref_mode', 'middle', '-rc-lookahead', '32', '-temporal-aq', '1',
                '-multipass', 'fullres',
                '-profile:v', 'main', '-tag:v', 'hvc1'
            )
        }
        $encodeArgs += @('-c:a', 'copy', '-movflags', '+faststart', '-f', 'mp4', $temporary)
        $output = & $ffmpeg @encodeArgs 2>&1
        $encodeExit = $LASTEXITCODE
        if ($encodeExit -ne 0) {
            throw "ffmpeg exit $encodeExit`: $($output | Out-String)"
        }
        if (!(Test-Path -LiteralPath $temporary -PathType Leaf) -or (Get-Item -LiteralPath $temporary).Length -eq 0) {
            throw 'Encoder produced no MP4'
        }
        $probeOutput = & $ffprobe -v error -select_streams 'v:0' `
            -show_entries 'stream=codec_name' -of 'default=noprint_wrappers=1:nokey=1' $temporary 2>&1
        if ($LASTEXITCODE -ne 0 -or ($probeOutput | Out-String).Trim() -ne $VideoCodec) {
            throw "Encoded MP4 failed $VideoCodec probe: $($probeOutput | Out-String)"
        }
        if (Test-Path -LiteralPath $job.Target) {
            throw "Target appeared during encode: $($job.Target)"
        }
        Move-Item -LiteralPath $temporary -Destination $job.Target
        $encoded++
        Write-Event 'encoded' $job "Bytes=$((Get-Item -LiteralPath $job.Target).Length)"
    }
    catch {
        $failed++
        $errorText = $_.Exception.Message
        Write-Event 'failed' $job $errorText
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    $processed++
    Write-Progress $job 'running'
}

$finalState = if ($failed -gt 0) { 'completed-with-errors' } else { 'completed' }
Write-Event 'batch-end' $null "Encoded=$encoded; Skipped=$skipped; Failed=$failed"
Write-Progress $null $finalState
if ($failed -gt 0) { exit 1 }
