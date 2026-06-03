#!/usr/bin/env bash
# =============================================================================
# scripts/build-native.sh
# Build the occt-kernel native shared library for local development and testing.
#
# Prerequisites:
#   - OCCT installed on the host (e.g. via apt, brew, or built from source)
#   - cmake >= 3.20, gcc or clang with C++17 support
#
# Usage:
#   bash scripts/build-native.sh [release|debug]
#
# Output:
#   build-native/libocct-kernel.so  (Linux)
#   build-native/libocct-kernel.dylib  (macOS)
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
    echo "[build-native] Finished in $(format_elapsed "${SECONDS}")"
}

trap print_elapsed_time EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "${SCRIPT_DIR}")"

BUILD_TYPE="${1:-release}"
case "${BUILD_TYPE}" in
    release|Release) CMAKE_BUILD_TYPE="Release" ;;
    debug|Debug)     CMAKE_BUILD_TYPE="Debug"   ;;
    *)
        echo "Usage: $0 [release|debug]"
        exit 1
        ;;
esac

BUILD_DIR="${REPO_ROOT}/build-native-${CMAKE_BUILD_TYPE,,}"
mkdir -p "${BUILD_DIR}"

echo "[build-native] Configuring (${CMAKE_BUILD_TYPE})..."
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
    -DOCCT_KERNEL_NATIVE=ON \
    -G "Unix Makefiles"

CPU_COUNT=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
echo "[build-native] Building with ${CPU_COUNT} parallel jobs..."
cmake --build "${BUILD_DIR}" --parallel "${CPU_COUNT}"

echo ""
echo "[build-native] Build complete. Output in ${BUILD_DIR}/"
