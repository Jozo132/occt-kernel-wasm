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
#   bash scripts/build-wasm.sh [release|debug|fast] [st|mt|all] [--reconfigure]
#
# Output:
#   dist/occt-kernel.st.js
#   dist/occt-kernel.st.wasm
#   dist/occt-kernel.mt.js
#   dist/occt-kernel.mt.wasm
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
VARIANT="st"
RECONFIGURE=0

for arg in "$@"; do
    case "${arg}" in
        release|Release) BUILD_TYPE="release" ;;
        debug|Debug)     BUILD_TYPE="debug" ;;
        fast|Fast)       BUILD_TYPE="fast" ;;
        st|ST)           VARIANT="st" ;;
        mt|MT)           VARIANT="mt" ;;
        all|ALL)         VARIANT="all" ;;
        --reconfigure)   RECONFIGURE=1 ;;
        *)
            echo "Usage: $0 [release|debug|fast] [st|mt|all] [--reconfigure]"
            exit 1
            ;;
    esac
done

case "${BUILD_TYPE}" in
    release) CMAKE_BUILD_TYPE="Release" ;;
    debug)   CMAKE_BUILD_TYPE="Debug" ;;
    fast)    CMAKE_BUILD_TYPE="Fast" ;;
esac

OCCT_VERSION="V8_0_0"
if [ -n "${OCCT_WASM_CACHE_ROOT:-}" ]; then
    OCCT_CACHE_ROOT="${OCCT_WASM_CACHE_ROOT}"
elif [ -n "${XDG_CACHE_HOME:-}" ]; then
    OCCT_CACHE_ROOT="${XDG_CACHE_HOME}/occt-kernel-wasm"
else
    OCCT_CACHE_ROOT="${HOME}/.cache/occt-kernel-wasm"
fi
OCCT_INSTALL_DIR="${OCCT_CACHE_ROOT}/${OCCT_VERSION}/i"

resolve_occt_install_dir() {
    local variant_name="$1"
    if [ "${variant_name}" = "mt" ]; then
        printf '%s\n' "${OCCT_CACHE_ROOT}/${OCCT_VERSION}/i-mt"
        return
    fi
    printf '%s\n' "${OCCT_INSTALL_DIR}"
}

resolve_emscripten_bin_dir() {
    local candidates=()
    if [ -n "${EMSDK:-}" ]; then
        candidates+=("${EMSDK}/upstream/emscripten")
    fi
    candidates+=(
        "${HOME}/emsdk/upstream/emscripten"
        "${HOME}/.cache/emsdk/upstream/emscripten"
    )
    if command -v emcc &>/dev/null; then
        candidates+=("$(dirname "$(command -v emcc)")")
    fi

    for candidate in "${candidates[@]}"; do
        if [ -x "${candidate}/emcc" ] && [ -x "${candidate}/em++" ] && [ -x "${candidate}/emcmake" ]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    return 1
}

EMSCRIPTEN_BIN_DIR="$(resolve_emscripten_bin_dir || true)"
EMSCRIPTEN_TOOLCHAIN=""
EMSCRIPTEN_CMAKE=""
if [ -n "${EMSCRIPTEN_BIN_DIR}" ]; then
    EMSDK_ENV_SCRIPT="$(dirname "$(dirname "${EMSCRIPTEN_BIN_DIR}")")/emsdk_env.sh"
    if [ -f "${EMSDK_ENV_SCRIPT}" ]; then
        # shellcheck disable=SC1090
        source "${EMSDK_ENV_SCRIPT}" >/dev/null
        EMSCRIPTEN_BIN_DIR="$(dirname "$(command -v emcc)")"
    fi
    for candidate in \
        "${EMSCRIPTEN_BIN_DIR}/cmake/Modules/Platform/Emscripten.cmake" \
        "$(dirname "${EMSCRIPTEN_BIN_DIR}")/cmake/Modules/Platform/Emscripten.cmake" \
        "/usr/share/emscripten/cmake/Modules/Platform/Emscripten.cmake"; do
        if [ -f "${candidate}" ]; then
            EMSCRIPTEN_TOOLCHAIN="${candidate}"
            EMSCRIPTEN_CMAKE="${EMSCRIPTEN_BIN_DIR}/emcmake"
            break
        fi
    done
fi

if [ -z "${EMSCRIPTEN_TOOLCHAIN}" ] || [ ! -f "${EMSCRIPTEN_TOOLCHAIN}" ]; then
    echo "[build-wasm] ERROR: Cannot find Emscripten toolchain file."
    echo "             Ensure emsdk is activated: source emsdk_env.sh"
    exit 1
fi

build_variant() {
    local variant="$1"
    local build_dir="${REPO_ROOT}/build-wasm-${CMAKE_BUILD_TYPE,,}-${variant}"
    local artifact_basename="occt-kernel.${variant}"
    local enable_pthreads="OFF"
    local occt_install_dir
    occt_install_dir="$(resolve_occt_install_dir "${variant}")"
    if [ "${variant}" = "mt" ]; then
        enable_pthreads="ON"
    fi

    if [ ! -d "${occt_install_dir}" ]; then
        echo "[build-wasm] OCCT ${variant} install not found at ${occt_install_dir}"
        echo "             Run scripts/build-occt.sh ${variant} first."
        exit 1
    fi

    mkdir -p "${build_dir}"

    if [ ! -f "${build_dir}/CMakeCache.txt" ] || [ "${RECONFIGURE}" -eq 1 ] || ! grep -Fq "${occt_install_dir}" "${build_dir}/CMakeCache.txt" || ! grep -Fq "${artifact_basename}" "${build_dir}/CMakeCache.txt"; then
        echo "[build-wasm] Configuring (${CMAKE_BUILD_TYPE} / ${variant})..."
        "${EMSCRIPTEN_CMAKE}" cmake -S "${REPO_ROOT}" -B "${build_dir}" \
            -DCMAKE_TOOLCHAIN_FILE="${EMSCRIPTEN_TOOLCHAIN}" \
            -DEMSCRIPTEN=1 \
            -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
            -DOCCT_KERNEL_WASM=ON \
            -DOCCT_ROOT="${occt_install_dir}" \
            -DOpenCASCADE_DIR="${occt_install_dir}/lib/cmake/opencascade" \
            -DOCCT_KERNEL_ARTIFACT_BASENAME="${artifact_basename}" \
            -DOCCT_KERNEL_ENABLE_PTHREADS="${enable_pthreads}" \
            -G "Unix Makefiles"
    else
        echo "[build-wasm] Reusing existing configure (${CMAKE_BUILD_TYPE} / ${variant})..."
    fi

    CPU_COUNT=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
    echo "[build-wasm] Building ${variant} with ${CPU_COUNT} parallel jobs..."
    cmake --build "${build_dir}" --parallel "${CPU_COUNT}"
}

copy_st_aliases() {
    if [ ! -f "${REPO_ROOT}/dist/occt-kernel.st.js" ] || [ ! -f "${REPO_ROOT}/dist/occt-kernel.st.wasm" ]; then
        return
    fi

    cp -f "${REPO_ROOT}/dist/occt-kernel.st.js" "${REPO_ROOT}/dist/occt-kernel.js"
    cp -f "${REPO_ROOT}/dist/occt-kernel.st.wasm" "${REPO_ROOT}/dist/occt-kernel.wasm"
    if [ -f "${REPO_ROOT}/dist/occt-kernel.st.worker.js" ]; then
        cp -f "${REPO_ROOT}/dist/occt-kernel.st.worker.js" "${REPO_ROOT}/dist/occt-kernel.worker.js"
    fi
}

case "${VARIANT}" in
    all)
        build_variant st
        build_variant mt
        ;;
    st|mt)
        build_variant "${VARIANT}"
        ;;
esac

if [ "${VARIANT}" = "st" ] || [ "${VARIANT}" = "all" ] || [ -f "${REPO_ROOT}/dist/occt-kernel.st.js" ]; then
    copy_st_aliases
fi

mkdir -p "${REPO_ROOT}/dist"

echo ""
echo "[build-wasm] Build complete."
echo "             dist/occt-kernel.st.js"
echo "             dist/occt-kernel.st.wasm"
echo "             dist/occt-kernel.mt.js"
echo "             dist/occt-kernel.mt.wasm"
echo "             dist/occt-kernel.js (compat alias -> st)"
echo "             dist/occt-kernel.wasm (compat alias -> st)"
