#!/usr/bin/env bash
# =============================================================================
# scripts/build-wasm.sh
# Compile occt-kernel-wasm to WebAssembly using Emscripten.
#
# Prerequisites:
#   - emsdk activated (emcc, em++ available on PATH)
#   - OCCT built for WASM (run scripts/build-occt.sh first)
#   - cmake >= 3.20
#
# Usage:
#   bash scripts/build-wasm.sh [release|debug|fast] [--reconfigure]
#
# Output:
#   dist/occt-kernel.js
#   dist/occt-kernel.wasm
# =============================================================================
set -euo pipefail

SECONDS=0

format_elapsed() {
    local total_seconds="$1"
    local hours=$(( total_seconds / 3600 ))
    local minutes=$(( (total_seconds % 3600) / 60 ))
    local seconds=$(( total_seconds % 60 ))

    if [ "${hours}" -gt 0 ]; then
        printf '%sh %sm %ss' "${hours}" "${minutes}" "${seconds}"
    elif [ "${minutes}" -gt 0 ]; then
        printf '%sm %ss' "${minutes}" "${seconds}"
    else
        printf '%ss' "${seconds}"
    fi
}

print_elapsed_time() {
    echo "[build-wasm] Finished in $(format_elapsed "${SECONDS}")"
}

trap print_elapsed_time EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "${SCRIPT_DIR}")"

BUILD_TYPE="release"
RECONFIGURE=0

for arg in "$@"; do
    case "${arg}" in
        release|Release) BUILD_TYPE="release" ;;
        debug|Debug)     BUILD_TYPE="debug" ;;
        fast|Fast)       BUILD_TYPE="fast" ;;
        --reconfigure)   RECONFIGURE=1 ;;
        *)
            echo "Usage: $0 [release|debug|fast] [--reconfigure]"
            exit 1
            ;;
    esac
done

case "${BUILD_TYPE}" in
    release) CMAKE_BUILD_TYPE="Release" ;;
    debug)   CMAKE_BUILD_TYPE="Debug" ;;
    fast)    CMAKE_BUILD_TYPE="Fast" ;;
esac

OCCT_INSTALL_DIR="${REPO_ROOT}/third-party/occt-wasm"
WASM_BUILD_DIR="${REPO_ROOT}/build-wasm-${CMAKE_BUILD_TYPE,,}"

if [ ! -d "${OCCT_INSTALL_DIR}" ]; then
    echo "[build-wasm] OCCT not found at ${OCCT_INSTALL_DIR}"
    echo "             Run scripts/build-occt.sh first."
    exit 1
fi

# Locate Emscripten toolchain
EMSCRIPTEN_TOOLCHAIN=""
if command -v emcc &>/dev/null; then
    EMSDK_ROOT="$(dirname "$(dirname "$(command -v emcc)")")"
    EMSCRIPTEN_TOOLCHAIN="${EMSDK_ROOT}/cmake/Modules/Platform/Emscripten.cmake"
fi

if [ -z "${EMSCRIPTEN_TOOLCHAIN}" ] || [ ! -f "${EMSCRIPTEN_TOOLCHAIN}" ]; then
    echo "[build-wasm] ERROR: Cannot find Emscripten toolchain file."
    echo "             Ensure emsdk is activated: source emsdk_env.sh"
    exit 1
fi

mkdir -p "${WASM_BUILD_DIR}"

if [ ! -f "${WASM_BUILD_DIR}/CMakeCache.txt" ] || [ "${RECONFIGURE}" -eq 1 ]; then
    echo "[build-wasm] Configuring (${CMAKE_BUILD_TYPE})..."
    emcmake cmake -S "${REPO_ROOT}" -B "${WASM_BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${EMSCRIPTEN_TOOLCHAIN}" \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DOCCT_KERNEL_WASM=ON \
        -DOCCT_ROOT="${OCCT_INSTALL_DIR}" \
        -G "Unix Makefiles"
else
    echo "[build-wasm] Reusing existing configure (${CMAKE_BUILD_TYPE})..."
fi

CPU_COUNT=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
echo "[build-wasm] Building with ${CPU_COUNT} parallel jobs..."
cmake --build "${WASM_BUILD_DIR}" --parallel "${CPU_COUNT}"

mkdir -p "${REPO_ROOT}/dist"

echo ""
echo "[build-wasm] Build complete."
echo "             dist/occt-kernel.js"
echo "             dist/occt-kernel.wasm"
