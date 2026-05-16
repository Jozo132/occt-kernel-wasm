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
#   ${OCCT_WASM_CACHE_ROOT:-~/.cache/occt-kernel-wasm}/<OCCT_VERSION>/i/  – OCCT installed for WASM (include/, lib/)
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

OCCT_VERSION="V8_0_0"
OCCT_REPO="https://github.com/Open-Cascade-SAS/OCCT.git"
OCCT_TAG="${OCCT_VERSION}"
OCCT_TOOLKITS="TKernel;TKMath;TKG2d;TKG3d;TKGeomBase;TKBRep;TKGeomAlgo;TKTopAlgo;TKPrim;TKBO;TKBool;TKFeat;TKFillet;TKOffset;TKShHealing;TKMesh;TKXSBase;TKDESTEP"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "${SCRIPT_DIR}")"
THIRD_PARTY_DIR="${REPO_ROOT}/third-party"
if [ -n "${OCCT_WASM_CACHE_ROOT:-}" ]; then
    OCCT_CACHE_ROOT="${OCCT_WASM_CACHE_ROOT}"
elif [ -n "${XDG_CACHE_HOME:-}" ]; then
    OCCT_CACHE_ROOT="${XDG_CACHE_HOME}/occt-kernel-wasm"
else
    OCCT_CACHE_ROOT="${HOME}/.cache/occt-kernel-wasm"
fi
OCCT_VERSION_ROOT="${OCCT_CACHE_ROOT}/${OCCT_VERSION}"
OCCT_SRC_DIR="${THIRD_PARTY_DIR}/occt-src"
OCCT_VERSIONED_SRC_DIR="${OCCT_VERSION_ROOT}/src"
OCCT_BUILD_DIR="${OCCT_VERSION_ROOT}/b"
OCCT_INSTALL_DIR="${OCCT_VERSION_ROOT}/i"
ACTIVE_OCCT_SRC_DIR=""

mkdir -p "${THIRD_PARTY_DIR}"
mkdir -p "${OCCT_VERSION_ROOT}"

git_worktree_dirty() {
    local repo_dir="$1"

    if ! git -C "${repo_dir}" diff --quiet --ignore-submodules --; then
        return 0
    fi
    if ! git -C "${repo_dir}" diff --cached --quiet --ignore-submodules --; then
        return 0
    fi
    if [ -n "$(git -C "${repo_dir}" ls-files --others --exclude-standard)" ]; then
        return 0
    fi

    return 1
}

checkout_occt_tag() {
    local repo_dir="$1"

    if git -C "${repo_dir}" tag --points-at HEAD | grep -Fxq "${OCCT_TAG}"; then
        return
    fi

    if ! git -C "${repo_dir}" rev-parse -q --verify "refs/tags/${OCCT_TAG}" >/dev/null; then
        git -C "${repo_dir}" fetch --depth=1 origin "refs/tags/${OCCT_TAG}:refs/tags/${OCCT_TAG}"
        if ! git -C "${repo_dir}" rev-parse -q --verify "refs/tags/${OCCT_TAG}" >/dev/null; then
            echo "[build-occt] ERROR: Tag ${OCCT_TAG} was not found in ${repo_dir}"
            exit 1
        fi
    fi

    git -C "${repo_dir}" checkout --detach "${OCCT_TAG}"
}

prepare_versioned_source_checkout() {
    if [ -d "${OCCT_VERSIONED_SRC_DIR}/.git" ]; then
        echo "[build-occt] Reusing versioned OCCT source at ${OCCT_VERSIONED_SRC_DIR}"
        if git_worktree_dirty "${OCCT_VERSIONED_SRC_DIR}"; then
            echo "[build-occt] ERROR: ${OCCT_VERSIONED_SRC_DIR} has local changes; refusing to overwrite them"
            exit 1
        fi
        checkout_occt_tag "${OCCT_VERSIONED_SRC_DIR}"
    else
        echo "[build-occt] Cloning clean OCCT ${OCCT_VERSION} source to ${OCCT_VERSIONED_SRC_DIR}"
        git clone --depth=1 --branch "${OCCT_TAG}" "${OCCT_REPO}" "${OCCT_VERSIONED_SRC_DIR}"
    fi

    ACTIVE_OCCT_SRC_DIR="${OCCT_VERSIONED_SRC_DIR}"
}

# ---------------------------------------------------------------------------
# Clone or update OCCT source
# ---------------------------------------------------------------------------

if [ -d "${OCCT_SRC_DIR}/.git" ]; then
    echo "[build-occt] OCCT source already present at ${OCCT_SRC_DIR}"
    if git_worktree_dirty "${OCCT_SRC_DIR}"; then
        echo "[build-occt] Existing source tree has local changes; using a clean versioned checkout instead"
        prepare_versioned_source_checkout
    else
        echo "[build-occt] Checking out ${OCCT_VERSION} in the primary source tree..."
        checkout_occt_tag "${OCCT_SRC_DIR}"
        ACTIVE_OCCT_SRC_DIR="${OCCT_SRC_DIR}"
    fi
else
    echo "[build-occt] Primary source tree is missing; using cache-backed OCCT checkout at ${OCCT_VERSIONED_SRC_DIR}"
    prepare_versioned_source_checkout
fi

# ---------------------------------------------------------------------------
# Configure with Emscripten
# ---------------------------------------------------------------------------

echo "[build-occt] Configuring OCCT for Emscripten..."
mkdir -p "${OCCT_BUILD_DIR}"
echo "[build-occt] Source cache: ${ACTIVE_OCCT_SRC_DIR}"
echo "[build-occt] Build cache:  ${OCCT_BUILD_DIR}"
echo "[build-occt] Install dir:  ${OCCT_INSTALL_DIR}"

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

emcmake cmake -S "${ACTIVE_OCCT_SRC_DIR}" -B "${OCCT_BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${EMSCRIPTEN_TOOLCHAIN}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${OCCT_INSTALL_DIR}" \
    -DBUILD_LIBRARY_TYPE=Static \
    -DBUILD_MODULE_FoundationClasses=OFF \
    -DBUILD_MODULE_ModelingData=OFF \
    -DBUILD_MODULE_ModelingAlgorithms=OFF \
    -DBUILD_MODULE_DataExchange=OFF \
    -DBUILD_MODULE_Visualization=OFF \
    -DBUILD_MODULE_ApplicationFramework=OFF \
    -DBUILD_MODULE_Draw=OFF \
    -DBUILD_ADDITIONAL_TOOLKITS="${OCCT_TOOLKITS}" \
    -DBUILD_USE_PCH=ON \
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
