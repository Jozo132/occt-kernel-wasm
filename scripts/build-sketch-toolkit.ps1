# Build the sketch-toolkit WASM target
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

$REPO_ROOT = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$EMSDK_ROOT = "C:\Users\HP\OneDrive\Documents\node\WASM"
$TOOLCHAIN = "$EMSDK_ROOT\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"
$normalizedBuildType = $BuildType.ToLowerInvariant()

switch ($normalizedBuildType) {
    "release" { $cmakeBuildType = "Release" }
    "debug" { $cmakeBuildType = "Debug" }
    "fast" { $cmakeBuildType = "Fast" }
    default { throw "Unsupported build type: $BuildType" }
}

try {
    $env:EMSDK_PYTHON = "$EMSDK_ROOT\python\3.9.2-1_64bit\python.exe"
    $env:EMSDK = $EMSDK_ROOT
    $env:EM_CONFIG = "$EMSDK_ROOT\.emscripten"

    $buildDir = "$REPO_ROOT\build-sketch-toolkit-$normalizedBuildType"
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    $cacheFile = Join-Path $buildDir "CMakeCache.txt"
    if ($Reconfigure -or (Test-Path $cacheFile)) {
        if ($Reconfigure) {
            Remove-Item -Force -ErrorAction SilentlyContinue $cacheFile
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $buildDir "CMakeFiles")
        }
    }

    if (-not (Test-Path $cacheFile) -or $Reconfigure) {
        Write-Host "[build-sketch] Configuring ($cmakeBuildType)..."
        $cmakeArgs = @(
            "-S", $REPO_ROOT,
            "-B", $buildDir,
            "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN",
            "-DCMAKE_BUILD_TYPE=$cmakeBuildType",
            "-DCMAKE_MAKE_PROGRAM=ninja",
            "-DSKETCH_TOOLKIT_WASM=ON",
            "-DSKETCH_TOOLKIT_ARTIFACT_BASENAME=sketch-toolkit.wasm",
            "-G", "Ninja"
        )
        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    } else {
        Write-Host "[build-sketch] Reusing existing configure ($cmakeBuildType)..."
    }

    $cpuCount = (Get-CimInstance Win32_Processor).NumberOfLogicalProcessors
    if (-not $cpuCount) { $cpuCount = 4 }
    Write-Host "[build-sketch] Building with $cpuCount parallel jobs..."
    & cmake --build $buildDir --parallel $cpuCount
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

    Write-Host ""
    Write-Host "[build-sketch] Build complete."
    Write-Host "               dist/sketch-toolkit.wasm.js"
    Write-Host "               dist/sketch-toolkit.wasm.wasm"
}
finally {
    $buildTimer.Stop()
    Write-Host "[build-sketch] Finished in $(Format-ElapsedTime $buildTimer.Elapsed)"
}