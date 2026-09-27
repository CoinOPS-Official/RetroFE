param(
    [string]$BuildDirectory = "$PSScriptRoot/../Build",
    [string]$GStreamerRoot = "C:/gstreamer/1.0/msvc_x86_64",
    [switch]$EnableFFmpeg,
    [string]$FFmpegRoot = "C:/ffmpeg",
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
if (!(Test-Path -LiteralPath "$PSScriptRoot/../ThirdParty/openhi2txt/CMakeLists.txt")) {
    throw 'OpenHi2txt is missing. Run git submodule update --init --recursive from the repository root.'
}
$packages = @(
    @{ Repo='SDL'; Name='SDL3'; Version='3.4.16'; Archive='sdl'; Hash='1A784CB2A5C64D56FE7A62090FE9D242D9865F235E4EA9678F1A6BA4E693E7DE' },
    @{ Repo='SDL_image'; Name='SDL3_image'; Version='3.4.6'; Archive='image'; Hash='03C6B313623EDADF707A7C187E2036A5BE5F12E693025C0697833379970BB4C0' },
    @{ Repo='SDL_ttf'; Name='SDL3_ttf'; Version='3.2.2'; Archive='ttf'; Hash='67805C5BABFC49CA0C56882DC9B8CABBCDD1E6F9EDDE10DDAC91DDB38F3AFB8C' },
    @{ Repo='SDL_mixer'; Name='SDL3_mixer'; Version='3.2.4'; Archive='mixer'; Hash='F4263ED5082FB7018059D64952017534E26821E9E878CE6B8C924B77CB17C4FB' }
)
New-Item -ItemType Directory -Force "$BuildDirectory/downloads", "$BuildDirectory/deps" | Out-Null
$prefixes = foreach ($package in $packages) {
    $archive = "$BuildDirectory/downloads/$($package.Archive)-$($package.Version).zip"
    if (!(Test-Path -LiteralPath $archive)) {
        $url = "https://github.com/libsdl-org/$($package.Repo)/releases/download/release-$($package.Version)/$($package.Name)-devel-$($package.Version)-VC.zip"
        Invoke-WebRequest -Uri $url -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $package.Hash) {
        throw "Unexpected checksum for $archive"
    }
    $prefix = "$BuildDirectory/deps/$($package.Name)-$($package.Version)"
    # Restore the verified upstream files even when a developer previously
    # replaced headers or DLLs in this cache while testing an SDL patch.
    Expand-Archive -LiteralPath $archive -DestinationPath "$BuildDirectory/deps" -Force
    Write-Host "Using upstream $($package.Name) $($package.Version)"
    (Resolve-Path -LiteralPath $prefix).Path
}
$packageArguments = for ($index = 0; $index -lt $packages.Count; ++$index) {
    # Override stale CMake package paths pointing at a local patched build.
    "-D$($packages[$index].Name)_DIR=$($prefixes[$index])/cmake"
}
$backendArguments = @()
if ($EnableFFmpeg) {
    $backendArguments = @('-DRETROFE_ENABLE_FFMPEG=ON', "-DFFMPEG_ROOT=$FFmpegRoot")
}
& cmake -S $PSScriptRoot -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_PREFIX_PATH=$($prefixes -join ';')" "-DGSTREAMER_ROOT=$GStreamerRoot" `
    @packageArguments @backendArguments -DRETROFE_FETCH_SDL3=OFF -DRETROFE_BUILD_TESTING=ON -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'SDL3 configuration failed' }
& cmake --build $BuildDirectory --config $Configuration --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'SDL3 build failed' }
& ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'SDL3 tests failed' }
Write-Host "Visual Studio solution: $BuildDirectory/retrofe.sln"
Write-Host "Executable and runtime libraries: $BuildDirectory/bin/$Configuration"
