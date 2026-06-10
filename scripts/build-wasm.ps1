# Build occt-kernel-wasm WASM library
param(
    [ValidateSet("release", "debug", "fast", "Release", "Debug", "Fast")]
    [string]$BuildType = "release",
    [ValidateSet("st", "mt", "all", "ST", "MT", "ALL")]
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

$buildTimer = [System.Diagnostics.Stopwatch]::StartNew()

function Get-VariantSettings {
    param([string]$RequestedVariant)

    $normalizedVariant = $RequestedVariant.ToLowerInvariant()
    switch ($normalizedVariant) {
        "st" {
            return @{
                Variant = "st"
                ArtifactBasename = "occt-kernel.st"
                EnablePthreads = $false
            }
        }
        "mt" {
            return @{
                Variant = "mt"
                ArtifactBasename = "occt-kernel.mt"
                EnablePthreads = $true
            }
        }
        default {
            throw "Unsupported kernel variant: $RequestedVariant"
        }
    }
}

function Resolve-OcctInstallPath {
    param(
        [string]$CacheRoot,
        [string]$Version,
        [string]$VariantName
    )

    $versionRoot = Join-Path $CacheRoot $Version
    if ($VariantName -eq 'mt') {
        return Join-Path $versionRoot 'i-mt'
    }
    return Join-Path $versionRoot 'i'
}

function Publish-CompatibilityAlias {
    param([string]$RepoRoot, [string]$ArtifactBasename)

    $distDir = Join-Path $RepoRoot "dist"
    $jsSource = Join-Path $distDir "$ArtifactBasename.js"
    $wasmSource = Join-Path $distDir "$ArtifactBasename.wasm"
    if (-not (Test-Path $jsSource) -or -not (Test-Path $wasmSource)) {
        return
    }

    Copy-Item -Force $jsSource (Join-Path $distDir "occt-kernel.js")
    Copy-Item -Force $wasmSource (Join-Path $distDir "occt-kernel.wasm")
    $workerPath = Join-Path $distDir "$ArtifactBasename.worker.js"
    if (Test-Path $workerPath) {
        Copy-Item -Force $workerPath (Join-Path $distDir "occt-kernel.worker.js")
    }
}

function Invoke-WasmBuild {
    param(
        [string]$RepoRoot,
        [string]$Toolchain,
        [string]$OcctInstall,
        [string]$BuildTypeName,
        [string]$CMakeBuildTypeName,
        [hashtable]$VariantSettings,
        [bool]$ForceReconfigure,
        [bool]$RequiresBuildDirReset
    )

    $variantName = $VariantSettings.Variant
    $buildDir = "$RepoRoot\build-wasm-$BuildTypeName-$variantName"

    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    $cacheFile = Join-Path $buildDir "CMakeCache.txt"
    $needsConfigure = $ForceReconfigure -or -not (Test-Path $cacheFile)
    $requiresReset = $RequiresBuildDirReset
    if (-not $needsConfigure) {
        $cacheContents = Get-Content $cacheFile -Raw
        if ($cacheContents -notmatch [regex]::Escape($OcctInstall)) {
            $needsConfigure = $true
        }
        elseif ($cacheContents -notmatch [regex]::Escape($RepoRoot)) {
            $needsConfigure = $true
            $requiresReset = $true
        }
        elseif ($cacheContents -notmatch [regex]::Escape($VariantSettings.ArtifactBasename)) {
            $needsConfigure = $true
            $requiresReset = $true
        }
    }

    if ($needsConfigure) {
        if ($ForceReconfigure -or $requiresReset) {
            Write-Host "[build-wasm] Resetting stale CMake cache metadata in $buildDir..."
            Remove-Item -Force -ErrorAction SilentlyContinue $cacheFile
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $buildDir "CMakeFiles")
        }
        Write-Host "[build-wasm] Configuring ($CMakeBuildTypeName / $variantName)..."
        $cmakeArgs = @(
            "-S", $RepoRoot,
            "-B", $buildDir,
            "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
            "-DCMAKE_BUILD_TYPE=$CMakeBuildTypeName",
            "-DCMAKE_MAKE_PROGRAM=ninja",
            "-DOCCT_KERNEL_WASM=ON",
            "-DOCCT_ROOT=$OcctInstall",
            "-DOpenCASCADE_DIR=$OcctInstall\lib\cmake\opencascade",
            "-DOCCT_KERNEL_ARTIFACT_BASENAME=$($VariantSettings.ArtifactBasename)",
            "-DOCCT_KERNEL_ENABLE_PTHREADS=$(if ($VariantSettings.EnablePthreads) { 'ON' } else { 'OFF' })",
            "-G", "Ninja"
        )

        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }
    } else {
        Write-Host "[build-wasm] Reusing existing configure ($CMakeBuildTypeName / $variantName)..."
    }

    $cpuCount = (Get-CimInstance Win32_Processor).NumberOfLogicalProcessors
    if (-not $cpuCount) { $cpuCount = 4 }
    Write-Host "[build-wasm] Building $variantName with $cpuCount parallel jobs..."

    & cmake --build $buildDir --parallel $cpuCount
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake build failed"; exit 1 }
}

$REPO_ROOT = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$EMSDK_ROOT = "C:\Users\HP\OneDrive\Documents\node\WASM"
$TOOLCHAIN = "$EMSDK_ROOT\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"
$OCCT_VERSION = "V8_0_0"
if ($env:OCCT_WASM_CACHE_ROOT) {
    $OcctCacheRoot = $env:OCCT_WASM_CACHE_ROOT
}
elseif ($env:LOCALAPPDATA) {
    $OcctCacheRoot = Join-Path $env:LOCALAPPDATA "occt-kernel-wasm"
}
else {
    $OcctCacheRoot = Join-Path $env:TEMP "occt-kernel-wasm"
}
$normalizedBuildType = $BuildType.ToLowerInvariant()
switch ($normalizedBuildType) {
    "release" { $cmakeBuildType = "Release" }
    "debug" { $cmakeBuildType = "Debug" }
    "fast" { $cmakeBuildType = "Fast" }
    default { throw "Unsupported build type: $BuildType" }
}

$requiresBuildDirReset = $false

try {
    # Ensure emsdk Python 3 is used
    $env:EMSDK_PYTHON = "$EMSDK_ROOT\python\3.9.2-1_64bit\python.exe"
    $env:EMSDK = $EMSDK_ROOT
    $env:EM_CONFIG = "$EMSDK_ROOT\.emscripten"

    New-Item -ItemType Directory -Force -Path "$REPO_ROOT\dist" | Out-Null

    $requestedVariant = $Variant.ToLowerInvariant()
    $variantsToBuild = if ($requestedVariant -eq 'all') { @('st', 'mt') } else { @($requestedVariant) }

    foreach ($variantName in $variantsToBuild) {
        $variantOcctInstall = Resolve-OcctInstallPath -CacheRoot $OcctCacheRoot -Version $OCCT_VERSION -VariantName $variantName
        if (-not (Test-Path $variantOcctInstall)) {
            throw "OCCT install for variant '$variantName' not found at $variantOcctInstall. Run build-occt-wasm.ps1 $variantName first."
        }
        Invoke-WasmBuild -RepoRoot $REPO_ROOT -Toolchain $TOOLCHAIN -OcctInstall $variantOcctInstall -BuildTypeName $normalizedBuildType -CMakeBuildTypeName $cmakeBuildType -VariantSettings (Get-VariantSettings $variantName) -ForceReconfigure:$Reconfigure -RequiresBuildDirReset:$requiresBuildDirReset
    }

    if ($variantsToBuild -contains 'st' -or (Test-Path (Join-Path "$REPO_ROOT\dist" 'occt-kernel.st.js'))) {
        Publish-CompatibilityAlias -RepoRoot $REPO_ROOT -ArtifactBasename "occt-kernel.st"
    }

    Write-Host ""
    Write-Host "[build-wasm] Build complete."
    Write-Host "             dist/occt-kernel.st.js"
    Write-Host "             dist/occt-kernel.st.wasm"
    Write-Host "             dist/occt-kernel.mt.js"
    Write-Host "             dist/occt-kernel.mt.wasm"
    Write-Host "             dist/occt-kernel.js (compat alias -> st)"
    Write-Host "             dist/occt-kernel.wasm (compat alias -> st)"
}
finally {
    $buildTimer.Stop()
    Write-Host "[build-wasm] Finished in $(Format-ElapsedTime $buildTimer.Elapsed)"
}
