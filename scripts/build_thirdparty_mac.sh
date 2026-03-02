#!/usr/bin/env bash
# build_thirdparty_mac.sh
#
# Builds CCL (CIGI Class Library) and FFmpeg (with libx264) for macOS
# and copies the resulting static libraries into the ThirdParty tree.
#
# Supports both Apple Silicon (arm64) and Intel (x86_64).
# On Apple Silicon, x264 and FFmpeg are built for arm64 (asm disabled for x264
# since its hand-written asm targets x86 only; performance impact is minor for
# our use case).
#
# Prerequisites — install via Homebrew:
#   brew install cmake ninja nasm git pkg-config
#
# Usage:
#   ./scripts/build_thirdparty_mac.sh          # native arch
#   ./scripts/build_thirdparty_mac.sh x86_64   # cross-compile on Apple Silicon

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRDPARTY="${REPO_ROOT}/unreal_project/CamSimTest/Source/ThirdParty"
BUILD_DIR="${REPO_ROOT}/.build_tmp"

NATIVE_ARCH="$(uname -m)"  # arm64 or x86_64
TARGET_ARCH="${1:-${NATIVE_ARCH}}"
NCPU="$(sysctl -n hw.logicalcpu)"

echo "==> Building ThirdParty for macOS (${TARGET_ARCH}) with ${NCPU} jobs"

# Deployment target — must match the min macOS version UE5.7 supports
MACOS_MIN_VERSION="12.0"

# CMake toolchain flags for the target arch
CMAKE_ARCH_FLAGS=(
    -DCMAKE_OSX_ARCHITECTURES="${TARGET_ARCH}"
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_MIN_VERSION}"
)

# Autoconf/configure flags for the target arch
if [ "${TARGET_ARCH}" = "${NATIVE_ARCH}" ]; then
    HOST_TRIPLE=""  # native build — omit --host
else
    # Cross-compiling on Apple Silicon → x86_64 (or vice versa)
    HOST_TRIPLE="--host=${TARGET_ARCH}-apple-darwin"
fi

ARCH_CFLAGS="-arch ${TARGET_ARCH} -mmacosx-version-min=${MACOS_MIN_VERSION}"

mkdir -p "${BUILD_DIR}"

# -------------------------------------------------------------------------
# Verify Homebrew prerequisites
# -------------------------------------------------------------------------
check_brew_dep() {
    if ! command -v "$1" &>/dev/null && ! brew list "$2" &>/dev/null 2>&1; then
        echo "[ERROR] '$1' not found. Install with: brew install $2"
        exit 1
    fi
}
check_brew_dep cmake   cmake
check_brew_dep ninja   ninja
check_brew_dep nasm    nasm
check_brew_dep git     git

# =========================================================================
# 1. CCL — CIGI Class Library (LGPL-2.1)
# =========================================================================
CCL_SRC="${BUILD_DIR}/ccl"
CCL_BUILD="${BUILD_DIR}/ccl_build_${TARGET_ARCH}"
CCL_DST="${THIRDPARTY}/CCL/lib/Mac"
CCL_INC="${THIRDPARTY}/CCL/include"

if [ ! -d "${CCL_SRC}" ]; then
    echo "==> Cloning CCL..."
    git clone --depth 1 \
        https://github.com/Hadron/cigi-ccl.git \
        "${CCL_SRC}"
fi

# cigi-ccl's CMakeLists.txt opens with PROJECT() and has cmake_minimum_required
# commented out.  cmake >= 3.27 turns this into a hard error:
#   "cmake_minimum_required must be called before project"
# Patch the file in-place (idempotent) to prepend the missing call.
if ! grep -qiE '^cmake_minimum_required' "${CCL_SRC}/CMakeLists.txt"; then
    echo "==> Patching CMakeLists.txt (prepending cmake_minimum_required)"
    sed -i '' '1s/^/cmake_minimum_required(VERSION 3.10)\n/' "${CCL_SRC}/CMakeLists.txt"
fi

# BUILD_SHARED_LIBS is not honoured because the cmake file defines both targets
# explicitly; we build only cigicl-static instead.
cmake -S "${CCL_SRC}" -B "${CCL_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    "${CMAKE_ARCH_FLAGS[@]}" \
    -GNinja

# Build only the static target — skips the shared lib entirely
cmake --build "${CCL_BUILD}" --target cigicl-static --parallel "${NCPU}"

# cigi-ccl cmake sets LIBRARY_OUTPUT_PATH = ${PROJECT_BINARY_DIR}/lib
# OUTPUT_NAME is cigicl_static only on Windows; on macOS/Linux it is cigicl
CCL_LIB="${CCL_BUILD}/lib/libcigicl.a"
if [ ! -f "${CCL_LIB}" ]; then
    echo "[ERROR] Expected static lib not found: ${CCL_LIB}"
    echo "        Contents of ${CCL_BUILD}/lib/:"
    ls "${CCL_BUILD}/lib/" 2>/dev/null || echo "  (directory missing)"
    exit 1
fi

mkdir -p "${CCL_DST}"
cp "${CCL_LIB}" "${CCL_DST}/"

# Headers live flat in include/*.h in the source tree.
# We copy them into include/cigicl/ so that #include "cigicl/CigiXxx.h" resolves.
mkdir -p "${CCL_INC}/cigicl"
cp "${CCL_SRC}"/include/*.h "${CCL_INC}/cigicl/"

echo "[CCL] done → ${CCL_DST}/libcigicl.a"

# =========================================================================
# 2. x264
# =========================================================================
X264_SRC="${BUILD_DIR}/x264"
X264_INSTALL="${BUILD_DIR}/x264_install_${TARGET_ARCH}"

if [ ! -d "${X264_SRC}" ]; then
    echo "==> Cloning x264..."
    git clone --depth 1 \
        https://code.videolan.org/videolan/x264.git \
        "${X264_SRC}"
fi

cd "${X264_SRC}"

# arm64 macs cannot use x264's x86 asm; disable it explicitly
NASM_OPT=""
if [ "${TARGET_ARCH}" = "arm64" ]; then
    NASM_OPT="--disable-asm"
fi

./configure \
    --prefix="${X264_INSTALL}" \
    --enable-static \
    --disable-shared \
    --enable-pic \
    --disable-cli \
    ${NASM_OPT} \
    ${HOST_TRIPLE} \
    --extra-cflags="${ARCH_CFLAGS}" \
    --extra-ldflags="${ARCH_CFLAGS}"

make -j"${NCPU}"
make install
cd "${REPO_ROOT}"

# =========================================================================
# 3. FFmpeg (GPL + libx264)
# =========================================================================
FFMPEG_SRC="${BUILD_DIR}/ffmpeg"
FFMPEG_INSTALL="${BUILD_DIR}/ffmpeg_install_${TARGET_ARCH}"
FFMPEG_DST="${THIRDPARTY}/FFmpeg/lib/Mac"
FFMPEG_INC="${THIRDPARTY}/FFmpeg/include"

if [ ! -d "${FFMPEG_SRC}" ]; then
    echo "==> Cloning FFmpeg..."
    git clone --depth 1 --branch n7.0 \
        https://github.com/FFmpeg/FFmpeg.git \
        "${FFMPEG_SRC}"
fi

cd "${FFMPEG_SRC}"

# arm64: disable x86 asm; leave the rest auto-detected
FFMPEG_EXTRA=""
if [ "${TARGET_ARCH}" = "arm64" ]; then
    FFMPEG_EXTRA="--disable-x86asm"
fi

./configure \
    --prefix="${FFMPEG_INSTALL}" \
    --enable-gpl \
    --enable-libx264 \
    --enable-muxer=mpegts \
    --enable-protocol=udp \
    --enable-static \
    --disable-shared \
    --enable-pic \
    --disable-debug \
    --disable-doc \
    --disable-programs \
    --extra-cflags="${ARCH_CFLAGS} -I${X264_INSTALL}/include" \
    --extra-ldflags="${ARCH_CFLAGS} -L${X264_INSTALL}/lib" \
    --extra-asmflags="-DPIC" \
    ${HOST_TRIPLE} \
    ${FFMPEG_EXTRA}

make -j"${NCPU}"
make install
cd "${REPO_ROOT}"

mkdir -p "${FFMPEG_DST}"
cp "${FFMPEG_INSTALL}/lib/libavcodec.a"    "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libavformat.a"  "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libavutil.a"    "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libswscale.a"   "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libswresample.a" "${FFMPEG_DST}/"
cp "${X264_INSTALL}/lib/libx264.a"        "${FFMPEG_DST}/"

mkdir -p "${FFMPEG_INC}"
cp -r "${FFMPEG_INSTALL}/include/"* "${FFMPEG_INC}/"

echo "[FFmpeg] done → ${FFMPEG_DST}"
echo ""
echo "==> All ThirdParty libs built for macOS/${TARGET_ARCH}."
echo "    You can now open the project in UE5 or run UBT."
