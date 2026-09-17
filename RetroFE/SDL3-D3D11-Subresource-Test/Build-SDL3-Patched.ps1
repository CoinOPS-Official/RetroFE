param(
    [string]$BuildDirectory = "$PSScriptRoot/../Build",
    [string]$GStreamerRoot = "C:/gstreamer/1.0/msvc_x86_64",
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',

    [string]$SDLVersion = '3.4.12',
    [string]$SDLPatch = "$PSScriptRoot/Patches/SDL3-d3d11-subresource.patch",

    [switch]$SkipSDLPatch,
    [switch]$CleanSDL
)

$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Command,
        [Parameter(Mandatory = $true)]
        [string]$ErrorMessage
    )

    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$ErrorMessage (exit code $LASTEXITCODE)"
    }
}

function Require-Command {
    param([Parameter(Mandatory = $true)][string]$Name)

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command '$Name' was not found in PATH."
    }
}

function Find-Fxc {
    $cmd = Get-Command fxc.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    if (Test-Path -LiteralPath $kitsRoot) {
        $candidates = Get-ChildItem -LiteralPath $kitsRoot -Directory |
            Sort-Object Name -Descending |
            ForEach-Object {
                Join-Path $_.FullName 'x64/fxc.exe'
            } |
            Where-Object { Test-Path -LiteralPath $_ }

        if ($candidates) {
            return $candidates[0]
        }
    }

    throw @"
fxc.exe was not found.

Install the Windows SDK component from Visual Studio Installer.
The patched SDL D3D11 path adds one Texture2DArray pixel shader that must
be compiled before SDL itself is built.
"@
}

Require-Command git
Require-Command cmake

$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

if (!(Test-Path -LiteralPath "$RepoRoot/ThirdParty/openhi2txt/CMakeLists.txt")) {
    throw 'OpenHi2txt is missing. Run git submodule update --init --recursive from the repository root.'
}

$DownloadsDirectory = Join-Path $BuildDirectory 'downloads'
$DepsDirectory      = Join-Path $BuildDirectory 'deps'
$SourcesDirectory   = Join-Path $BuildDirectory 'sources'

$SDLSourceDirectory = Join-Path $SourcesDirectory "SDL-$SDLVersion"
$SDLBuildDirectory  = Join-Path $BuildDirectory "SDL3-build-$SDLVersion"
$SDLInstallDirectory = Join-Path $DepsDirectory "SDL3-$SDLVersion-patched"
$SDLTag = "release-$SDLVersion"

New-Item -ItemType Directory -Force `
    $DownloadsDirectory, `
    $DepsDirectory, `
    $SourcesDirectory | Out-Null

Write-Host ''
Write-Host '=== SDL3 source ===' -ForegroundColor Cyan

if (!(Test-Path -LiteralPath (Join-Path $SDLSourceDirectory '.git'))) {
    Write-Host "Cloning SDL $SDLVersion..."
    Invoke-Checked {
        git clone `
            --branch $SDLTag `
            --depth 1 `
            https://github.com/libsdl-org/SDL.git `
            $SDLSourceDirectory
    } "Failed to clone SDL $SDLVersion"
}
else {
    Write-Host "Reusing SDL source checkout: $SDLSourceDirectory"

    Invoke-Checked {
        git -C $SDLSourceDirectory fetch origin `
            "refs/tags/$SDLTag`:refs/tags/$SDLTag" `
            --force
    } "Failed to refresh SDL tag $SDLTag"
}

# This checkout lives entirely under Build/sources and is treated as disposable.
# Reset it every run so the patch is applied to a known clean SDL release.
Write-Host "Resetting SDL source to $SDLTag..."
Invoke-Checked {
    git -C $SDLSourceDirectory reset --hard $SDLTag
} "Failed to reset SDL source"

Invoke-Checked {
    git -C $SDLSourceDirectory clean -fdx
} "Failed to clean SDL source"

if (-not $SkipSDLPatch) {
    $SDLPatch = [System.IO.Path]::GetFullPath($SDLPatch)

    if (!(Test-Path -LiteralPath $SDLPatch)) {
        throw @"
SDL patch was not found:

    $SDLPatch

Place the D3D11 subresource patch there, pass -SDLPatch <path>,
or use -SkipSDLPatch to build an unmodified SDL baseline.
"@
    }

    Write-Host ''
    Write-Host '=== Applying SDL3 patch ===' -ForegroundColor Cyan
    Write-Host $SDLPatch

    Invoke-Checked {
        git -C $SDLSourceDirectory apply --check --whitespace=nowarn $SDLPatch
    } 'SDL patch does not apply cleanly'

    Invoke-Checked {
        git -C $SDLSourceDirectory apply --whitespace=nowarn $SDLPatch
    } 'Failed to apply SDL patch'

    # The SDL repository stores precompiled D3D11 shader headers. Our patch
    # adds a new Texture2DArray shader source but intentionally does not carry
    # a machine-generated DXBC header. Generate just that header locally.
    $Fxc = Find-Fxc
    $ShaderDirectory = Join-Path $SDLSourceDirectory 'src/render/direct3d11'
    $ArrayShader = Join-Path $ShaderDirectory 'D3D11_PixelShader_AdvancedArray.hlsl'
    $ArrayShaderHeader = Join-Path $ShaderDirectory 'D3D11_PixelShader_AdvancedArray.h'

    if (!(Test-Path -LiteralPath $ArrayShader)) {
        throw "Patched array shader source was not found at $ArrayShader"
    }

    Write-Host ''
    Write-Host '=== Compiling SDL3 D3D11 array shader ===' -ForegroundColor Cyan
    Write-Host "fxc: $Fxc"

    Push-Location $ShaderDirectory
    try {
        Invoke-Checked {
            & $Fxc /nologo /T ps_5_0 /Fh D3D11_PixelShader_AdvancedArray.h D3D11_PixelShader_AdvancedArray.hlsl
        } 'Failed to compile SDL D3D11 Texture2DArray shader'
    }
    finally {
        Pop-Location
    }

    if (!(Test-Path -LiteralPath $ArrayShaderHeader)) {
        throw "fxc completed but did not create $ArrayShaderHeader"
    }
}
else {
    Write-Warning 'Building SDL3 without the local D3D11 subresource patch.'
}

if ($CleanSDL) {
    if (Test-Path -LiteralPath $SDLBuildDirectory) {
        Remove-Item -LiteralPath $SDLBuildDirectory -Recurse -Force
    }
    if (Test-Path -LiteralPath $SDLInstallDirectory) {
        Remove-Item -LiteralPath $SDLInstallDirectory -Recurse -Force
    }
}

Write-Host ''
Write-Host '=== Configuring SDL3 ===' -ForegroundColor Cyan

Invoke-Checked {
    cmake `
        -S $SDLSourceDirectory `
        -B $SDLBuildDirectory `
        -G 'Visual Studio 17 2022' `
        -A x64 `
        "-DCMAKE_INSTALL_PREFIX=$SDLInstallDirectory" `
        -DSDL_SHARED=ON `
        -DSDL_STATIC=OFF `
        -DSDL_TEST_LIBRARY=OFF `
        -DSDL_TESTS=OFF `
        -DSDL_EXAMPLES=OFF
} 'SDL3 configuration failed'

Write-Host ''
Write-Host '=== Building SDL3 ===' -ForegroundColor Cyan

Invoke-Checked {
    cmake --build $SDLBuildDirectory `
        --config $Configuration `
        --parallel 6
} 'SDL3 build failed'

Write-Host ''
Write-Host '=== Installing SDL3 ===' -ForegroundColor Cyan

Invoke-Checked {
    cmake --install $SDLBuildDirectory `
        --config $Configuration
} 'SDL3 install failed'

$SDLConfig = Join-Path $SDLInstallDirectory 'lib/cmake/SDL3/SDL3Config.cmake'
if (!(Test-Path -LiteralPath $SDLConfig)) {
    # Some SDL install layouts use a share/cmake location.
    $SDLConfig = Get-ChildItem -LiteralPath $SDLInstallDirectory `
        -Filter SDL3Config.cmake `
        -Recurse `
        -File |
        Select-Object -First 1 -ExpandProperty FullName
}

if (!$SDLConfig -or !(Test-Path -LiteralPath $SDLConfig)) {
    throw "SDL3 built, but SDL3Config.cmake was not found under $SDLInstallDirectory"
}

Write-Host "Patched SDL3 install: $SDLInstallDirectory"

Write-Host ''
Write-Host '=== SDL companion packages ===' -ForegroundColor Cyan

# Keep the existing official VC development packages for SDL_image/ttf/mixer.
# CMAKE_PREFIX_PATH places our locally built SDL3 first, so these packages resolve
# SDL3 against the patched install instead of the stock SDL3 development package.
$packages = @(
    @{
        Repo='SDL_image'
        Name='SDL3_image'
        Version='3.2.4'
        Archive='image'
        Hash='76141D9535B77B1D6561368DE934B3797D87F834906016E1087940C85A8DAB85'
    },
    @{
        Repo='SDL_ttf'
        Name='SDL3_ttf'
        Version='3.2.2'
        Archive='ttf'
        Hash='67805C5BABFC49CA0C56882DC9B8CABBCDD1E6F9EDDE10DDAC91DDB38F3AFB8C'
    },
    @{
        Repo='SDL_mixer'
        Name='SDL3_mixer'
        Version='3.2.4'
        Archive='mixer'
        Hash='F4263ED5082FB7018059D64952017534E26821E9E878CE6B8C924B77CB17C4FB'
    }
)

$AddonPrefixes = foreach ($package in $packages) {
    $archive = Join-Path $DownloadsDirectory "$($package.Archive).zip"

    if (!(Test-Path -LiteralPath $archive)) {
        $url = "https://github.com/libsdl-org/$($package.Repo)/releases/download/release-$($package.Version)/$($package.Name)-devel-$($package.Version)-VC.zip"
        Write-Host "Downloading $($package.Name) $($package.Version)..."
        Invoke-WebRequest -Uri $url -OutFile $archive
    }

    $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    if ($actualHash -ne $package.Hash) {
        throw "Unexpected checksum for $archive`nExpected: $($package.Hash)`nActual:   $actualHash"
    }

    $prefix = Join-Path $DepsDirectory "$($package.Name)-$($package.Version)"

    if (!(Test-Path -LiteralPath (Join-Path $prefix 'cmake'))) {
        Expand-Archive -LiteralPath $archive -DestinationPath $DepsDirectory -Force
    }

    (Resolve-Path -LiteralPath $prefix).Path
}

$Prefixes = @(
    (Resolve-Path -LiteralPath $SDLInstallDirectory).Path
) + $AddonPrefixes

Write-Host ''
Write-Host 'CMAKE_PREFIX_PATH:' -ForegroundColor DarkCyan
$Prefixes | ForEach-Object { Write-Host "  $_" }

Write-Host ''
Write-Host '=== Configuring RetroFE ===' -ForegroundColor Cyan

Invoke-Checked {
    cmake `
        -S (Join-Path $RepoRoot 'Source') `
        -B $BuildDirectory `
        -G 'Visual Studio 17 2022' `
        -A x64 `
        "-DCMAKE_PREFIX_PATH=$($Prefixes -join ';')" `
        "-DSDL3_DIR=$(Split-Path -Parent $SDLConfig)" `
        "-DGSTREAMER_ROOT=$GStreamerRoot" `
        -DRETROFE_FETCH_SDL3=OFF `
        -DRETROFE_BUILD_TESTING=ON `
        -DBUILD_TESTING=ON
} 'RetroFE configuration failed'

Write-Host ''
Write-Host '=== Building RetroFE ===' -ForegroundColor Cyan

Invoke-Checked {
    cmake --build $BuildDirectory `
        --config $Configuration `
        --parallel 6
} 'RetroFE build failed'

# Be explicit about which SDL3.dll ends up beside RetroFE. This prevents a
# stock SDL3.dll bundled with one of the companion VC packages from accidentally
# replacing the patched build at runtime.
$PatchedSDLRuntime = Join-Path $SDLInstallDirectory 'bin/SDL3.dll'
$RetroFERuntimeDirectory = Join-Path $BuildDirectory "bin/$Configuration"

if (!(Test-Path -LiteralPath $PatchedSDLRuntime)) {
    throw "Patched SDL3.dll was not found at $PatchedSDLRuntime"
}

New-Item -ItemType Directory -Force $RetroFERuntimeDirectory | Out-Null
Copy-Item -LiteralPath $PatchedSDLRuntime `
    -Destination (Join-Path $RetroFERuntimeDirectory 'SDL3.dll') `
    -Force

Write-Host ''
Write-Host '=== Running tests ===' -ForegroundColor Cyan

Invoke-Checked {
    ctest `
        --test-dir $BuildDirectory `
        -C $Configuration `
        --output-on-failure
} 'RetroFE tests failed'

Write-Host ''
Write-Host '=== Build complete ===' -ForegroundColor Green
Write-Host "SDL source:                  $SDLSourceDirectory"
Write-Host "SDL build:                   $SDLBuildDirectory"
Write-Host "Patched SDL install:         $SDLInstallDirectory"
Write-Host "Visual Studio solution:      $BuildDirectory/retrofe.sln"
Write-Host "Executable/runtime libraries: $RetroFERuntimeDirectory"
Write-Host ''
Write-Host "Runtime SDL3.dll:"
Get-Item -LiteralPath (Join-Path $RetroFERuntimeDirectory 'SDL3.dll') |
    Format-List FullName, Length, LastWriteTime
