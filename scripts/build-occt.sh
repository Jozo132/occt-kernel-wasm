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
#   bash scripts/build-occt.sh [st|mt|all] [--reconfigure]
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

VARIANT="st"
RECONFIGURE=0

for arg in "$@"; do
    case "$arg" in
        st|single|single-thread) VARIANT="st" ;;
        mt|pthread|threads|multi-thread) VARIANT="mt" ;;
        all) VARIANT="all" ;;
        --reconfigure) RECONFIGURE=1 ;;
        *)
            echo "[build-occt] ERROR: Unsupported argument '$arg'. Use st, mt, all, or --reconfigure."
            exit 1
            ;;
    esac
done

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
ACTIVE_OCCT_SRC_DIR=""

resolve_variants() {
    case "$1" in
        all) echo "st mt" ;;
        st|single|single-thread) echo "st" ;;
        mt|pthread|threads|multi-thread) echo "mt" ;;
        *)
            echo "[build-occt] ERROR: Unsupported variant '$1'. Use st, mt, or all."
            exit 1
            ;;
    esac
}

variant_paths() {
    local variant_name="$1"
    local common_em_flags="-sDISABLE_EXCEPTION_CATCHING=0 -sSUPPORT_LONGJMP=emscripten -msimd128"
    if [ "${variant_name}" = "mt" ]; then
        printf '%s\n%s\n%s\n%s\n' "${OCCT_VERSION_ROOT}/b-mt" "${OCCT_VERSION_ROOT}/i-mt" "-pthread ${common_em_flags}" "-pthread ${common_em_flags}"
        return
    fi

    printf '%s\n%s\n%s\n%s\n' "${OCCT_VERSION_ROOT}/b" "${OCCT_VERSION_ROOT}/i" "${common_em_flags}" "${common_em_flags}"
}

variant_signature() {
    local variant_name="$1"
    local source_root="$2"
    local toolchain="$3"
    local c_flags="$4"
    local cxx_flags="$5"
    printf 'variant=%s\nsource=%s\ntoolchain=%s\ntoolkits=%s\ncflags=%s\ncxxflags=%s\nbuildType=Release\nscriptSchema=occt-cache-v2' \
        "${variant_name}" "${source_root}" "${toolchain}" "${OCCT_TOOLKITS}" "${c_flags}" "${cxx_flags}"
}

cache_contains_path() {
    local cache_content="$1"
    local path_value="$2"
    local forward_path="${path_value//\\//}"
    [[ "${cache_content}" == *"${path_value}"* ]] || [[ "${cache_content}" == *"${forward_path}"* ]]
}

variant_cache_matches() {
    local build_dir="$1"
    local install_dir="$2"
    local source_root="$3"
    local toolchain="$4"
    local c_flags="$5"
    local cxx_flags="$6"
    local cache_file="${build_dir}/CMakeCache.txt"
    local config_file="${install_dir}/lib/cmake/opencascade/OpenCASCADEConfig.cmake"
    local cache_content

    [ -f "${cache_file}" ] || return 1
    [ -f "${config_file}" ] || return 1

    cache_content="$(cat "${cache_file}")"
    [[ "${cache_content}" == *"BUILD_ADDITIONAL_TOOLKITS:STRING=${OCCT_TOOLKITS}"* ]] || return 1
    [[ "${cache_content}" == *"CMAKE_TOOLCHAIN_FILE:FILEPATH=${toolchain}"* ]] || [[ "${cache_content}" == *"CMAKE_TOOLCHAIN_FILE:UNINITIALIZED=${toolchain}"* ]] || cache_contains_path "${cache_content}" "${toolchain}" || return 1
    [[ "${cache_content}" == *"CMAKE_C_FLAGS:STRING=${c_flags}"* ]] || return 1
    [[ "${cache_content}" == *"CMAKE_CXX_FLAGS:STRING=${cxx_flags}"* ]] || return 1
    [[ "${cache_content}" == *"INSTALL_DIR:PATH=${install_dir//\\//}"* ]] || cache_contains_path "${cache_content}" "${install_dir}" || return 1
    cache_contains_path "${cache_content}" "${source_root}" || return 1
}

variant_ready() {
    local build_dir="$1"
    local install_dir="$2"
    local signature="$3"
    local stamp_file="${install_dir}/.occt-build-stamp"
    local config_file="${install_dir}/lib/cmake/opencascade/OpenCASCADEConfig.cmake"

    [ -f "${build_dir}/CMakeCache.txt" ] || return 1
    [ -f "${config_file}" ] || return 1
    [ -f "${stamp_file}" ] || return 1
    [ "$(cat "${stamp_file}")" = "${signature}" ] || return 1
}

write_variant_stamp() {
    local install_dir="$1"
    local signature="$2"
    printf '%s' "${signature}" > "${install_dir}/.occt-build-stamp"
}

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
    echo "[build-occt] ERROR: Cannot find Emscripten toolchain file."
    echo "             Ensure emsdk is activated: source emsdk_env.sh"
    exit 1
fi

for variant_name in $(resolve_variants "${VARIANT}"); do
    mapfile -t path_values < <(variant_paths "${variant_name}")
    OCCT_BUILD_DIR="${path_values[0]}"
    OCCT_INSTALL_DIR="${path_values[1]}"
    OCCT_C_FLAGS="${path_values[2]}"
    OCCT_CXX_FLAGS="${path_values[3]}"
    VARIANT_SIGNATURE="$(variant_signature "${variant_name}" "${ACTIVE_OCCT_SRC_DIR}" "${EMSCRIPTEN_TOOLCHAIN}" "${OCCT_C_FLAGS}" "${OCCT_CXX_FLAGS}")"

    if [ "${RECONFIGURE}" -eq 0 ] && variant_ready "${OCCT_BUILD_DIR}" "${OCCT_INSTALL_DIR}" "${VARIANT_SIGNATURE}"; then
        echo "[build-occt] Reusing cached OCCT ${variant_name} install at ${OCCT_INSTALL_DIR}"
        continue
    fi

    if [ "${RECONFIGURE}" -eq 0 ] && variant_cache_matches "${OCCT_BUILD_DIR}" "${OCCT_INSTALL_DIR}" "${ACTIVE_OCCT_SRC_DIR}" "${EMSCRIPTEN_TOOLCHAIN}" "${OCCT_C_FLAGS}" "${OCCT_CXX_FLAGS}"; then
        write_variant_stamp "${OCCT_INSTALL_DIR}" "${VARIANT_SIGNATURE}"
        echo "[build-occt] Adopted existing OCCT ${variant_name} cache at ${OCCT_INSTALL_DIR}"
        continue
    fi

    echo "[build-occt] Configuring OCCT for Emscripten (${variant_name})..."
    mkdir -p "${OCCT_BUILD_DIR}"
    echo "[build-occt] Source cache: ${ACTIVE_OCCT_SRC_DIR}"
    echo "[build-occt] Build cache:  ${OCCT_BUILD_DIR}"
    echo "[build-occt] Install dir:  ${OCCT_INSTALL_DIR}"

    "${EMSCRIPTEN_CMAKE}" cmake -S "${ACTIVE_OCCT_SRC_DIR}" -B "${OCCT_BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${EMSCRIPTEN_TOOLCHAIN}" \
        -DEMSCRIPTEN=1 \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${OCCT_INSTALL_DIR}" \
        -DCMAKE_C_FLAGS="${OCCT_C_FLAGS}" \
        -DCMAKE_CXX_FLAGS="${OCCT_CXX_FLAGS}" \
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

    CPU_COUNT=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
    echo "[build-occt] Building OCCT ${variant_name} with ${CPU_COUNT} parallel jobs..."
    cmake --build "${OCCT_BUILD_DIR}" --parallel "${CPU_COUNT}"

    echo "[build-occt] Installing OCCT ${variant_name} to ${OCCT_INSTALL_DIR}..."
    cmake --install "${OCCT_BUILD_DIR}"

    write_variant_stamp "${OCCT_INSTALL_DIR}" "${VARIANT_SIGNATURE}"
done

echo ""
echo "[build-occt] Done. OCCT for WASM installed under: ${OCCT_VERSION_ROOT}"
echo "             Use ${OCCT_VERSION_ROOT}/i for st and ${OCCT_VERSION_ROOT}/i-mt for mt."
