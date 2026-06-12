#!/usr/bin/env bash
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

trap 'echo "[build-sketch] Finished in $(format_elapsed "${SECONDS}")"' EXIT

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

EMSCRIPTEN_TOOLCHAIN=""
if command -v emcc &>/dev/null; then
    EMSDK_ROOT="$(dirname "$(dirname "$(command -v emcc)")")"
    EMSCRIPTEN_TOOLCHAIN="${EMSDK_ROOT}/cmake/Modules/Platform/Emscripten.cmake"
fi

if [ -z "${EMSCRIPTEN_TOOLCHAIN}" ] || [ ! -f "${EMSCRIPTEN_TOOLCHAIN}" ]; then
    echo "[build-sketch] ERROR: Cannot find Emscripten toolchain file."
    echo "                Ensure emsdk is activated: source emsdk_env.sh"
    exit 1
fi

BUILD_DIR="${REPO_ROOT}/build-sketch-toolkit-${CMAKE_BUILD_TYPE,,}"
mkdir -p "${BUILD_DIR}"

if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ] || [ "${RECONFIGURE}" -eq 1 ] || ! grep -Fq "sketch-toolkit.wasm" "${BUILD_DIR}/CMakeCache.txt"; then
    echo "[build-sketch] Configuring (${CMAKE_BUILD_TYPE})..."
    emcmake cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${EMSCRIPTEN_TOOLCHAIN}" \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DSKETCH_TOOLKIT_WASM=ON \
        -DSKETCH_TOOLKIT_ARTIFACT_BASENAME=sketch-toolkit.wasm \
        -G "Unix Makefiles"
else
    echo "[build-sketch] Reusing existing configure (${CMAKE_BUILD_TYPE})..."
fi

CPU_COUNT=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
echo "[build-sketch] Building with ${CPU_COUNT} parallel jobs..."
cmake --build "${BUILD_DIR}" --parallel "${CPU_COUNT}"

echo ""
echo "[build-sketch] Build complete."
echo "               dist/sketch-toolkit.wasm.js"
echo "               dist/sketch-toolkit.wasm.wasm"