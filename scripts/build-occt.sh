#!/usr/bin/env bash
# =============================================================================
# scripts/build-occt.sh
# Download and build Open CASCADE Technology for the Emscripten (WASM) target.
#
# Prerequisites:
#   - emsdk activated (emcc, em++ available on PATH)
#   - cmake >= 3.20
#   - python3, git
#
# Usage:
#   bash scripts/build-occt.sh
#
# Output:
#   third-party/occt-wasm/  – OCCT installed for WASM (include/, lib/)
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
    echo "[build-occt] Finished in $(format_elapsed "${SECONDS}")"
}

trap print_elapsed_time EXIT

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

OCCT_VERSION="V7_8_1"
OCCT_REPO="https://git.dev.opencascade.org/repos/occt.git"
OCCT_TAG="${OCCT_VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "${SCRIPT_DIR}")"
THIRD_PARTY_DIR="${REPO_ROOT}/third-party"
OCCT_SRC_DIR="${THIRD_PARTY_DIR}/occt-src"
OCCT_BUILD_DIR="${THIRD_PARTY_DIR}/occt-build-wasm"
OCCT_INSTALL_DIR="${THIRD_PARTY_DIR}/occt-wasm"

mkdir -p "${THIRD_PARTY_DIR}"

# ---------------------------------------------------------------------------
# Clone or update OCCT source
# ---------------------------------------------------------------------------

if [ -d "${OCCT_SRC_DIR}/.git" ]; then
    echo "[build-occt] OCCT source already present at ${OCCT_SRC_DIR}"
    echo "[build-occt] Fetching latest tags..."
    git -C "${OCCT_SRC_DIR}" fetch --tags
else
    echo "[build-occt] Cloning OCCT ${OCCT_VERSION} ..."
    git clone --depth=1 --branch "${OCCT_TAG}" "${OCCT_REPO}" "${OCCT_SRC_DIR}"
fi

# ---------------------------------------------------------------------------
# Configure with Emscripten
# ---------------------------------------------------------------------------

echo "[build-occt] Configuring OCCT for Emscripten..."
mkdir -p "${OCCT_BUILD_DIR}"

# Locate Emscripten toolchain file
EMSCRIPTEN_TOOLCHAIN=""
if command -v emcc &>/dev/null; then
    EMSDK_ROOT="$(dirname "$(dirname "$(command -v emcc)")")"
    EMSCRIPTEN_TOOLCHAIN="${EMSDK_ROOT}/cmake/Modules/Platform/Emscripten.cmake"
fi

if [ -z "${EMSCRIPTEN_TOOLCHAIN}" ] || [ ! -f "${EMSCRIPTEN_TOOLCHAIN}" ]; then
    echo "[build-occt] ERROR: Cannot find Emscripten toolchain file."
    echo "             Ensure emsdk is activated: source emsdk_env.sh"
    exit 1
fi

emcmake cmake -S "${OCCT_SRC_DIR}" -B "${OCCT_BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${EMSCRIPTEN_TOOLCHAIN}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${OCCT_INSTALL_DIR}" \
    -DBUILD_LIBRARY_TYPE=Static \
    -DBUILD_MODULE_FoundationClasses=ON \
    -DBUILD_MODULE_ModelingData=ON \
    -DBUILD_MODULE_ModelingAlgorithms=ON \
    -DBUILD_MODULE_DataExchange=ON \
    -DBUILD_MODULE_Visualization=ON \
    -DBUILD_MODULE_ApplicationFramework=OFF \
    -DBUILD_MODULE_Draw=OFF \
    -DBUILD_SAMPLES_MFC=OFF \
    -DBUILD_SAMPLES_QT=OFF \
    -DUSE_FREETYPE=OFF \
    -DUSE_TK=OFF \
    -DUSE_GL2PS=OFF \
    -DUSE_FREEIMAGE=OFF \
    -DUSE_RAPIDJSON=OFF \
    -DUSE_TBB=OFF \
    -DUSE_VTK=OFF \
    -G "Unix Makefiles"

# ---------------------------------------------------------------------------
# Build and install
# ---------------------------------------------------------------------------

CPU_COUNT=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
echo "[build-occt] Building OCCT with ${CPU_COUNT} parallel jobs..."
cmake --build "${OCCT_BUILD_DIR}" --parallel "${CPU_COUNT}"

echo "[build-occt] Installing OCCT to ${OCCT_INSTALL_DIR}..."
cmake --install "${OCCT_BUILD_DIR}"

echo ""
echo "[build-occt] Done. OCCT for WASM installed at: ${OCCT_INSTALL_DIR}"
echo "             Pass -DOCCT_ROOT=${OCCT_INSTALL_DIR} to the WASM CMake build."
