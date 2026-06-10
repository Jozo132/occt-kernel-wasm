# Build OCCT for WebAssembly (Emscripten)
param(
    [ValidateSet("st", "mt", "all", "single", "single-thread", "pthread", "threads", "multi-thread")]
    [string]$Variant = "st",
    [switch]$Reconfigure
)

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

function Resolve-VariantNames {
    param([string]$Requested)

    switch ($Requested.ToLowerInvariant()) {
        "all" { return @("st", "mt") }
        "single" { return @("st") }
        "single-thread" { return @("st") }
        "st" { return @("st") }
        "pthread" { return @("mt") }
        "threads" { return @("mt") }
        "multi-thread" { return @("mt") }
        "mt" { return @("mt") }
        default { throw "Unsupported variant: $Requested" }
    }
}

function Get-OcctVariantPaths {
    param(
        [string]$VariantName,
        [string]$VersionRoot
    )

    if ($VariantName -eq "mt") {
        return @{
            Build = Join-Path $VersionRoot "b-mt"
            Install = Join-Path $VersionRoot "i-mt"
            CFlags = "-pthread -msimd128"
            CxxFlags = "-pthread -msimd128"
        }
    }

    return @{
        Build = Join-Path $VersionRoot "b"
        Install = Join-Path $VersionRoot "i"
        CFlags = "-msimd128"
        CxxFlags = "-msimd128"
    }
}

function Get-OcctVariantSignature {
    param(
        [string]$VariantName,
        [string]$SourceRoot,
        [string]$Toolchain,
        [string]$Toolkits,
        [hashtable]$VariantPaths
    )

    return @(
        "variant=$VariantName",
        "source=$SourceRoot",
        "toolchain=$Toolchain",
        "toolkits=$Toolkits",
        "cflags=$($VariantPaths.CFlags)",
        "cxxflags=$($VariantPaths.CxxFlags)",
        "buildType=Release",
        "scriptSchema=occt-cache-v1"
    ) -join "`n"
}

function Test-CacheContainsPath {
    param(
        [string]$CacheContent,
        [string]$PathValue
    )

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return $false
    }

    $forwardSlashes = $PathValue.Replace("\", "/")
    return $CacheContent.Contains($PathValue) -or $CacheContent.Contains($forwardSlashes)
}

function Test-OcctVariantCacheMatches {
    param(
        [string]$BuildDir,
        [string]$InstallDir,
        [string]$SourceRoot,
        [string]$Toolchain,
        [string]$Toolkits
    )

    $cacheFile = Join-Path $BuildDir "CMakeCache.txt"
    $configFile = Join-Path $InstallDir "lib\cmake\opencascade\OpenCASCADEConfig.cmake"
    if (-not (Test-Path $cacheFile) -or -not (Test-Path $configFile)) {
        return $false
    }

    $cacheContent = Get-Content $cacheFile -Raw
    $toolchainMatches = $cacheContent.Contains("CMAKE_TOOLCHAIN_FILE:FILEPATH=$Toolchain") -or $cacheContent.Contains("CMAKE_TOOLCHAIN_FILE:UNINITIALIZED=$Toolchain") -or (Test-CacheContainsPath -CacheContent $cacheContent -PathValue $Toolchain)
    $toolkitsMatches = $cacheContent.Contains("BUILD_ADDITIONAL_TOOLKITS:STRING=$Toolkits")
    $installMatches = $cacheContent.Contains("INSTALL_DIR:PATH=$($InstallDir.Replace("\", "/"))") -or (Test-CacheContainsPath -CacheContent $cacheContent -PathValue $InstallDir)
    $sourceMatches = Test-CacheContainsPath -CacheContent $cacheContent -PathValue $SourceRoot

    return $toolchainMatches -and $toolkitsMatches -and $installMatches -and $sourceMatches
}

function Test-OcctVariantReady {
    param(
        [string]$BuildDir,
        [string]$InstallDir,
        [string]$Signature
    )

    $cacheFile = Join-Path $BuildDir "CMakeCache.txt"
    $configFile = Join-Path $InstallDir "lib\cmake\opencascade\OpenCASCADEConfig.cmake"
    $stampFile = Join-Path $InstallDir ".occt-build-stamp"

    if (-not (Test-Path $cacheFile) -or -not (Test-Path $configFile) -or -not (Test-Path $stampFile)) {
        return $false
    }

    return (Get-Content $stampFile -Raw) -eq $Signature
}

function Write-OcctVariantStamp {
    param(
        [string]$InstallDir,
        [string]$Signature
    )

    Set-Content -Path (Join-Path $InstallDir ".occt-build-stamp") -Value $Signature -NoNewline
}

$buildTimer = [System.Diagnostics.Stopwatch]::StartNew()

$REPO_ROOT = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

$EMSDK_ROOT = "C:\Users\HP\OneDrive\Documents\node\WASM"
$TOOLCHAIN = "$EMSDK_ROOT\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"

try {
    # Ensure emsdk Python 3 is used (default 'python' may be 2.7)
    $env:EMSDK_PYTHON = "$EMSDK_ROOT\python\3.9.2-1_64bit\python.exe"
    $env:EMSDK = $EMSDK_ROOT
    $env:EM_CONFIG = "$EMSDK_ROOT\.emscripten"
    $OCCT_VERSION = "V8_0_0"
    $OCCT_REPO = "https://github.com/Open-Cascade-SAS/OCCT.git"
    $OCCT_TOOLKITS = "TKernel;TKMath;TKG2d;TKG3d;TKGeomBase;TKBRep;TKGeomAlgo;TKTopAlgo;TKPrim;TKBO;TKBool;TKFeat;TKFillet;TKOffset;TKShHealing;TKMesh;TKXSBase;TKDESTEP"
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
    if (-not (Test-Path $TOOLCHAIN)) {
        Write-Error "Emscripten toolchain not found at $TOOLCHAIN"
        exit 1
    }

    New-Item -ItemType Directory -Force -Path $OcctVersionRoot | Out-Null

    $ActiveOcctSource = Ensure-OcctSource -PrimarySource $OCCT_SRC -FallbackSource $OCCT_VERSIONED_SRC -Tag $OCCT_VERSION -RepositoryUrl $OCCT_REPO

    foreach ($variantName in (Resolve-VariantNames $Variant)) {
        $variantPaths = Get-OcctVariantPaths -VariantName $variantName -VersionRoot $OcctVersionRoot
        $occtBuild = $variantPaths.Build
        $occtInstall = $variantPaths.Install
        $variantSignature = Get-OcctVariantSignature -VariantName $variantName -SourceRoot $ActiveOcctSource -Toolchain $TOOLCHAIN -Toolkits $OCCT_TOOLKITS -VariantPaths $variantPaths

        if (-not $Reconfigure -and (Test-OcctVariantReady -BuildDir $occtBuild -InstallDir $occtInstall -Signature $variantSignature)) {
            Write-Host "[build-occt] Reusing cached OCCT $variantName install at $occtInstall"
            continue
        }

        if (-not $Reconfigure -and (Test-OcctVariantCacheMatches -BuildDir $occtBuild -InstallDir $occtInstall -SourceRoot $ActiveOcctSource -Toolchain $TOOLCHAIN -Toolkits $OCCT_TOOLKITS)) {
            Write-OcctVariantStamp -InstallDir $occtInstall -Signature $variantSignature
            Write-Host "[build-occt] Adopted existing OCCT $variantName cache at $occtInstall"
            continue
        }

        Write-Host "[build-occt] Configuring OCCT for Emscripten ($variantName)..."
        Write-Host "[build-occt] Source cache: $ActiveOcctSource"
        Write-Host "[build-occt] Build cache:  $occtBuild"
        Write-Host "[build-occt] Install dir:  $occtInstall"
        New-Item -ItemType Directory -Force -Path $occtBuild | Out-Null

        $cmakeArgs = @(
            "-S", $ActiveOcctSource,
            "-B", $occtBuild,
            "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_INSTALL_PREFIX=$occtInstall",
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            "-DCMAKE_MAKE_PROGRAM=ninja",
            "-DCMAKE_C_FLAGS=$($variantPaths.CFlags)",
            "-DCMAKE_CXX_FLAGS=$($variantPaths.CxxFlags)",
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
        Write-Host "[build-occt] Building $variantName with $cpuCount parallel jobs..."

        & cmake --build $occtBuild --parallel $cpuCount
        if ($LASTEXITCODE -ne 0) { Write-Error "CMake build failed"; exit 1 }

        Write-Host "[build-occt] Installing $variantName to $occtInstall..."
        & cmake --install $occtBuild
        if ($LASTEXITCODE -ne 0) { Write-Error "CMake install failed"; exit 1 }

        Write-OcctVariantStamp -InstallDir $occtInstall -Signature $variantSignature

        Write-Host "[build-occt] OCCT $variantName build complete at $occtInstall"
    }
}
finally {
    $buildTimer.Stop()
    Write-Host "[build-occt] Finished in $(Format-ElapsedTime $buildTimer.Elapsed)"
}
