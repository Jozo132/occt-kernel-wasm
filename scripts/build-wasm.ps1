# Build occt-kernel-wasm WASM library
$ErrorActionPreference = "Stop"

$REPO_ROOT = "c:\Users\HP\OneDrive\Documents\C++ Projects\occt-kernel-wasm"
$EMSDK_ROOT = "C:\Users\HP\OneDrive\Documents\node\WASM"
$TOOLCHAIN = "$EMSDK_ROOT\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"
$OCCT_INSTALL = "$REPO_ROOT\third-party\occt-wasm"
$BUILD_DIR = "$REPO_ROOT\build-wasm-release"

# Ensure emsdk Python 3 is used
$env:EMSDK_PYTHON = "$EMSDK_ROOT\python\3.9.2-1_64bit\python.exe"
$env:EMSDK = $EMSDK_ROOT
$env:EM_CONFIG = "$EMSDK_ROOT\.emscripten"

Write-Host "[build-wasm] Configuring (Release)..."
New-Item -ItemType Directory -Force -Path $BUILD_DIR | Out-Null

$cmakeArgs = @(
    "-S", $REPO_ROOT,
    "-B", $BUILD_DIR,
    "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_MAKE_PROGRAM=ninja",
    "-DOCCT_KERNEL_WASM=ON",
    "-DOCCT_ROOT=$OCCT_INSTALL",
    "-DOpenCASCADE_DIR=$OCCT_INSTALL\lib\cmake\opencascade",
    "-G", "Ninja"
)

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }

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
