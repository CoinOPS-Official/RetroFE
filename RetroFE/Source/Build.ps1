param(
    [string]$BuildDirectory = "$PSScriptRoot/../Build",
    [string]$GStreamerRoot = "C:/gstreamer/1.0/msvc_x86_64",
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
if (!(Test-Path -LiteralPath "$PSScriptRoot/../ThirdParty/openhi2txt/CMakeLists.txt")) {
    throw 'OpenHi2txt is missing. Run git submodule update --init --recursive from the repository root.'
}
$packages = @(
    @{ Repo='SDL'; Name='SDL3'; Version='3.4.12'; Archive='sdl'; Hash='8793A153C7EBA93B1EB8022FD2356383EC446B2584E43724A72EF68D682813AB' },
    @{ Repo='SDL_image'; Name='SDL3_image'; Version='3.2.4'; Archive='image'; Hash='76141D9535B77B1D6561368DE934B3797D87F834906016E1087940C85A8DAB85' },
    @{ Repo='SDL_ttf'; Name='SDL3_ttf'; Version='3.2.2'; Archive='ttf'; Hash='67805C5BABFC49CA0C56882DC9B8CABBCDD1E6F9EDDE10DDAC91DDB38F3AFB8C' },
    @{ Repo='SDL_mixer'; Name='SDL3_mixer'; Version='3.2.4'; Archive='mixer'; Hash='F4263ED5082FB7018059D64952017534E26821E9E878CE6B8C924B77CB17C4FB' }
)
New-Item -ItemType Directory -Force "$BuildDirectory/downloads", "$BuildDirectory/deps" | Out-Null
$prefixes = foreach ($package in $packages) {
    $archive = "$BuildDirectory/downloads/$($package.Archive).zip"
    if (!(Test-Path -LiteralPath $archive)) {
        $url = "https://github.com/libsdl-org/$($package.Repo)/releases/download/release-$($package.Version)/$($package.Name)-devel-$($package.Version)-VC.zip"
        Invoke-WebRequest -Uri $url -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $package.Hash) {
        throw "Unexpected checksum for $archive"
    }
    $prefix = "$BuildDirectory/deps/$($package.Name)-$($package.Version)"
    if (!(Test-Path -LiteralPath "$prefix/cmake")) {
        Expand-Archive -LiteralPath $archive -DestinationPath "$BuildDirectory/deps"
    }
    (Resolve-Path -LiteralPath $prefix).Path
}
& cmake -S $PSScriptRoot -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_PREFIX_PATH=$($prefixes -join ';')" "-DGSTREAMER_ROOT=$GStreamerRoot" `
    -DRETROFE_FETCH_SDL3=OFF -DRETROFE_BUILD_TESTING=ON -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'SDL3 configuration failed' }
& cmake --build $BuildDirectory --config $Configuration --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'SDL3 build failed' }
& ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'SDL3 tests failed' }
Write-Host "Visual Studio solution: $BuildDirectory/retrofe.sln"
Write-Host "Executable and runtime libraries: $BuildDirectory/bin/$Configuration"
