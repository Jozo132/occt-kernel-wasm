# Build OCCT for WebAssembly (Emscripten)
$ErrorActionPreference = "Stop"

function Format-ElapsedTime {
    param([TimeSpan]$Elapsed)

    if ($Elapsed.TotalHours -ge 1) {
        return "{0}h {1}m {2}s" -f [int]$Elapsed.TotalHours, $Elapsed.Minutes, $Elapsed.Seconds
    }
    if ($Elapsed.TotalMinutes -ge 1) {
        return "{0}m {1}s" -f [int]$Elapsed.TotalMinutes, $Elapsed.Seconds
    }
    return "{0}s" -f [math]::Round($Elapsed.TotalSeconds, 1)
}

function Test-GitWorktreeDirty {
    param([string]$Repository)

    & git -C $Repository diff --quiet --ignore-submodules --
    if ($LASTEXITCODE -ne 0) { return $true }

    & git -C $Repository diff --cached --quiet --ignore-submodules --
    if ($LASTEXITCODE -ne 0) { return $true }

    $untracked = & git -C $Repository ls-files --others --exclude-standard
    return -not [string]::IsNullOrWhiteSpace(($untracked | Out-String))
}

function Checkout-OcctTag {
    param(
        [string]$Repository,
        [string]$Tag
    )

    $headTags = @(& git -C $Repository tag --points-at HEAD)
    if ($headTags -contains $Tag) {
        return
    }

    & git -C $Repository rev-parse -q --verify "refs/tags/$Tag" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        & git -C $Repository fetch --depth=1 origin "refs/tags/$Tag:refs/tags/$Tag"
        if ($LASTEXITCODE -ne 0) { throw "Failed to fetch tag $Tag for $Repository" }

        & git -C $Repository rev-parse -q --verify "refs/tags/$Tag" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Tag $Tag was not found in $Repository" }
    }

    & git -C $Repository checkout --detach $Tag
    if ($LASTEXITCODE -ne 0) { throw "Failed to checkout $Tag in $Repository" }
}

function Ensure-OcctSource {
    param(
        [string]$PrimarySource,
        [string]$FallbackSource,
        [string]$Tag,
        [string]$RepositoryUrl
    )

    if (Test-Path "$PrimarySource\.git") {
        Write-Host "[build-occt] OCCT source already present at $PrimarySource"
        if (Test-GitWorktreeDirty $PrimarySource) {
            Write-Host "[build-occt] Existing source tree has local changes; using a clean versioned checkout instead"
        }
        else {
            Write-Host "[build-occt] Checking out $Tag in the primary source tree..."
            Checkout-OcctTag -Repository $PrimarySource -Tag $Tag
            return $PrimarySource
        }
    }

    if (Test-Path "$FallbackSource\.git") {
        Write-Host "[build-occt] Reusing versioned OCCT source at $FallbackSource"
        if (Test-GitWorktreeDirty $FallbackSource) {
            throw "$FallbackSource has local changes; refusing to overwrite them"
        }
        Checkout-OcctTag -Repository $FallbackSource -Tag $Tag
        return $FallbackSource
    }

    Write-Host "[build-occt] Cloning clean OCCT $Tag source to $FallbackSource"
    & git clone --depth=1 --branch $Tag $RepositoryUrl $FallbackSource
    if ($LASTEXITCODE -ne 0) { throw "Failed to clone OCCT $Tag" }
    return $FallbackSource
}

$buildTimer = [System.Diagnostics.Stopwatch]::StartNew()

$REPO_ROOT = "c:\Users\HP\OneDrive\Documents\C++ Projects\occt-kernel-wasm"

$EMSDK_ROOT = "C:\Users\HP\OneDrive\Documents\node\WASM"
$TOOLCHAIN = "$EMSDK_ROOT\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"

try {
    # Ensure emsdk Python 3 is used (default 'python' may be 2.7)
    $env:EMSDK_PYTHON = "$EMSDK_ROOT\python\3.9.2-1_64bit\python.exe"
    $env:EMSDK = $EMSDK_ROOT
    $env:EM_CONFIG = "$EMSDK_ROOT\.emscripten"
    $OCCT_VERSION = "V8_0_0"
    $OCCT_REPO = "https://github.com/Open-Cascade-SAS/OCCT.git"
    $OCCT_TOOLKITS = "TKernel;TKMath;TKG2d;TKG3d;TKGeomBase;TKBRep;TKGeomAlgo;TKTopAlgo;TKPrim;TKBO;TKBool;TKFillet;TKOffset;TKShHealing;TKMesh;TKXSBase;TKDESTEP"
    if ($env:OCCT_WASM_CACHE_ROOT) {
        $OcctCacheRoot = $env:OCCT_WASM_CACHE_ROOT
    }
    elseif ($env:LOCALAPPDATA) {
        $OcctCacheRoot = Join-Path $env:LOCALAPPDATA "occt-kernel-wasm"
    }
    else {
        $OcctCacheRoot = Join-Path $env:TEMP "occt-kernel-wasm"
    }
    $OcctVersionRoot = Join-Path $OcctCacheRoot $OCCT_VERSION
    $OCCT_SRC = "$REPO_ROOT\third-party\occt-src"
    $OCCT_VERSIONED_SRC = Join-Path $OcctVersionRoot "src"
    $OCCT_BUILD = Join-Path $OcctVersionRoot "b"
    $OCCT_INSTALL = Join-Path $OcctVersionRoot "i"

    if (-not (Test-Path $TOOLCHAIN)) {
        Write-Error "Emscripten toolchain not found at $TOOLCHAIN"
        exit 1
    }

    New-Item -ItemType Directory -Force -Path $OcctVersionRoot | Out-Null

    $ActiveOcctSource = Ensure-OcctSource -PrimarySource $OCCT_SRC -FallbackSource $OCCT_VERSIONED_SRC -Tag $OCCT_VERSION -RepositoryUrl $OCCT_REPO

    Write-Host "[build-occt] Configuring OCCT for Emscripten..."
    Write-Host "[build-occt] Source cache: $ActiveOcctSource"
    Write-Host "[build-occt] Build cache:  $OCCT_BUILD"
    Write-Host "[build-occt] Install dir:  $OCCT_INSTALL"
    New-Item -ItemType Directory -Force -Path $OCCT_BUILD | Out-Null

    $cmakeArgs = @(
        "-S", $ActiveOcctSource,
        "-B", $OCCT_BUILD,
        "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=$OCCT_INSTALL",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-DCMAKE_MAKE_PROGRAM=ninja",
        "-DBUILD_LIBRARY_TYPE=Static",
        "-DBUILD_MODULE_FoundationClasses=OFF",
        "-DBUILD_MODULE_ModelingData=OFF",
        "-DBUILD_MODULE_ModelingAlgorithms=OFF",
        "-DBUILD_MODULE_DataExchange=OFF",
        "-DBUILD_MODULE_Visualization=OFF",
        "-DBUILD_MODULE_ApplicationFramework=OFF",
        "-DBUILD_MODULE_Draw=OFF",
        "-DBUILD_ADDITIONAL_TOOLKITS=$OCCT_TOOLKITS",
        "-DBUILD_USE_PCH=ON",
        "-DBUILD_SAMPLES_MFC=OFF",
        "-DBUILD_SAMPLES_QT=OFF",
        "-DUSE_FREETYPE=OFF",
        "-DUSE_TK=OFF",
        "-DUSE_GL2PS=OFF",
        "-DUSE_FREEIMAGE=OFF",
        "-DUSE_RAPIDJSON=OFF",
        "-DUSE_TBB=OFF",
        "-DUSE_VTK=OFF",
        "-DUSE_GLES2=OFF",
        "-G", "Ninja"
    )

    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }

    $cpuCount = (Get-CimInstance Win32_Processor).NumberOfLogicalProcessors
    if (-not $cpuCount) { $cpuCount = 4 }
    Write-Host "[build-occt] Building with $cpuCount parallel jobs..."

    & cmake --build $OCCT_BUILD --parallel $cpuCount
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake build failed"; exit 1 }

    Write-Host "[build-occt] Installing to $OCCT_INSTALL..."
    & cmake --install $OCCT_BUILD
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake install failed"; exit 1 }

    Write-Host "[build-occt] OCCT build complete at $OCCT_INSTALL"
}
finally {
    $buildTimer.Stop()
    Write-Host "[build-occt] Finished in $(Format-ElapsedTime $buildTimer.Elapsed)"
}
