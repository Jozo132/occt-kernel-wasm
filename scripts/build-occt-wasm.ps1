# Build OCCT for WebAssembly (Emscripten)
$ErrorActionPreference = "Stop"

$REPO_ROOT = "c:\Users\HP\OneDrive\Documents\C++ Projects\occt-kernel-wasm"

$EMSDK_ROOT = "C:\Users\HP\OneDrive\Documents\node\WASM"
$TOOLCHAIN = "$EMSDK_ROOT\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"

# Ensure emsdk Python 3 is used (default 'python' may be 2.7)
$env:EMSDK_PYTHON = "$EMSDK_ROOT\python\3.9.2-1_64bit\python.exe"
$env:EMSDK = $EMSDK_ROOT
$env:EM_CONFIG = "$EMSDK_ROOT\.emscripten"
$OCCT_SRC = "$REPO_ROOT\third-party\occt-src"
$OCCT_BUILD = "$REPO_ROOT\third-party\occt-build-wasm"
$OCCT_INSTALL = "$REPO_ROOT\third-party\occt-wasm"

if (-not (Test-Path $TOOLCHAIN)) {
    Write-Error "Emscripten toolchain not found at $TOOLCHAIN"
    exit 1
}

if (-not (Test-Path "$OCCT_SRC\CMakeLists.txt")) {
    Write-Error "OCCT source not found at $OCCT_SRC"
    exit 1
}

Write-Host "[build-occt] Configuring OCCT for Emscripten..."
New-Item -ItemType Directory -Force -Path $OCCT_BUILD | Out-Null

$cmakeArgs = @(
    "-S", $OCCT_SRC,
    "-B", $OCCT_BUILD,
    "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_INSTALL_PREFIX=$OCCT_INSTALL",
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
    "-DCMAKE_MAKE_PROGRAM=ninja",
    "-DBUILD_LIBRARY_TYPE=Static",
    "-DBUILD_MODULE_FoundationClasses=ON",
    "-DBUILD_MODULE_ModelingData=ON",
    "-DBUILD_MODULE_ModelingAlgorithms=ON",
    "-DBUILD_MODULE_DataExchange=ON",
    "-DBUILD_MODULE_Visualization=OFF",
    "-DBUILD_MODULE_ApplicationFramework=OFF",
    "-DBUILD_MODULE_Draw=OFF",
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
