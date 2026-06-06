# Build occt-kernel-wasm WASM library
param(
    [ValidateSet("release", "debug", "fast", "Release", "Debug", "Fast")]
    [string]$BuildType = "release",
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

$REPO_ROOT = "c:\Users\HP\OneDrive\Documents\C++ Projects\occt-kernel-wasm"
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
$OCCT_INSTALL = Join-Path (Join-Path $OcctCacheRoot $OCCT_VERSION) "i"

$normalizedBuildType = $BuildType.ToLowerInvariant()
switch ($normalizedBuildType) {
    "release" { $cmakeBuildType = "Release" }
    "debug" { $cmakeBuildType = "Debug" }
    "fast" { $cmakeBuildType = "Fast" }
    default { throw "Unsupported build type: $BuildType" }
}

$BUILD_DIR = "$REPO_ROOT\build-wasm-$normalizedBuildType"

try {
    # Ensure emsdk Python 3 is used
    $env:EMSDK_PYTHON = "$EMSDK_ROOT\python\3.9.2-1_64bit\python.exe"
    $env:EMSDK = $EMSDK_ROOT
    $env:EM_CONFIG = "$EMSDK_ROOT\.emscripten"

    New-Item -ItemType Directory -Force -Path $BUILD_DIR | Out-Null

    $cacheFile = Join-Path $BUILD_DIR "CMakeCache.txt"
    $needsConfigure = $Reconfigure -or -not (Test-Path $cacheFile)
    if (-not $needsConfigure) {
        $cacheContents = Get-Content $cacheFile -Raw
        if ($cacheContents -notmatch [regex]::Escape($OCCT_INSTALL)) {
            $needsConfigure = $true
        }
    }

    if ($needsConfigure) {
        Write-Host "[build-wasm] Configuring ($cmakeBuildType)..."
        $cmakeArgs = @(
            "-S", $REPO_ROOT,
            "-B", $BUILD_DIR,
            "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN",
            "-DCMAKE_BUILD_TYPE=$cmakeBuildType",
            "-DCMAKE_MAKE_PROGRAM=ninja",
            "-DOCCT_KERNEL_WASM=ON",
            "-DOCCT_ROOT=$OCCT_INSTALL",
            "-DOpenCASCADE_DIR=$OCCT_INSTALL\lib\cmake\opencascade",
            "-G", "Ninja"
        )

        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }
    } else {
        Write-Host "[build-wasm] Reusing existing configure ($cmakeBuildType)..."
    }

    $cpuCount = (Get-CimInstance Win32_Processor).NumberOfLogicalProcessors
    if (-not $cpuCount) { $cpuCount = 4 }
    Write-Host "[build-wasm] Building with $cpuCount parallel jobs..."

    & cmake --build $BUILD_DIR --parallel $cpuCount
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake build failed"; exit 1 }

    New-Item -ItemType Directory -Force -Path "$REPO_ROOT\dist" | Out-Null

    Write-Host ""
    Write-Host "[build-wasm] Build complete."
    Write-Host "             dist/occt-kernel.js"
    Write-Host "             dist/occt-kernel.wasm"
}
finally {
    $buildTimer.Stop()
    Write-Host "[build-wasm] Finished in $(Format-ElapsedTime $buildTimer.Elapsed)"
}
