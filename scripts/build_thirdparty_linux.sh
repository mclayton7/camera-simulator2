#!/usr/bin/env bash
# build_thirdparty_linux.sh
#
# Builds CCL (CIGI Class Library) and FFmpeg (with libx264) for Linux
# and copies the resulting static libraries into the ThirdParty tree.
#
# Prerequisites (Ubuntu 22.04):
#   sudo apt-get install -y cmake ninja-build nasm git \
#       libx264-dev build-essential

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRDPARTY="${REPO_ROOT}/unreal_project/CamSimTest/Source/ThirdParty"
BUILD_DIR="${REPO_ROOT}/.build_tmp"

mkdir -p "${BUILD_DIR}"

# =========================================================================
# 1. CCL — CIGI Class Library
#    LGPL-2.1 — may be linked statically (LGPL allows static link with
#    appropriate notice and relinking provision).
# =========================================================================
CCL_SRC="${BUILD_DIR}/ccl"
CCL_DST="${THIRDPARTY}/CCL/lib/Linux"
CCL_INC="${THIRDPARTY}/CCL/include"

if [ ! -d "${CCL_SRC}" ]; then
    git clone --depth 1 \
        https://github.com/Hadron/cigi-ccl.git \
        "${CCL_SRC}"
fi

# cmake >= 3.27 requires cmake_minimum_required before project(); patch if missing
if ! grep -qiE '^cmake_minimum_required' "${CCL_SRC}/CMakeLists.txt"; then
    echo "==> Patching CMakeLists.txt (prepending cmake_minimum_required)"
    sed -i '1s/^/cmake_minimum_required(VERSION 3.10)\n/' "${CCL_SRC}/CMakeLists.txt"
fi

cmake -S "${CCL_SRC}" -B "${CCL_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -GNinja

# Build only the static target — skips the shared lib
cmake --build "${CCL_SRC}/build" --target cigicl-static --parallel

# cigi-ccl sets LIBRARY_OUTPUT_PATH = ${PROJECT_BINARY_DIR}/lib
# OUTPUT_NAME is cigicl_static on Windows only; on Linux it is cigicl
CCL_LIB="${CCL_SRC}/build/lib/libcigicl.a"
if [ ! -f "${CCL_LIB}" ]; then
    echo "[ERROR] Expected static lib not found: ${CCL_LIB}"
    ls "${CCL_SRC}/build/lib/" 2>/dev/null || echo "  (directory missing)"
    exit 1
fi

mkdir -p "${CCL_DST}"
cp "${CCL_LIB}" "${CCL_DST}/"

# Headers live flat in include/*.h — copy into cigicl/ subdir
mkdir -p "${CCL_INC}/cigicl"
cp "${CCL_SRC}"/include/*.h "${CCL_INC}/cigicl/"

echo "[CCL] done: ${CCL_DST}/libcigicl.a"

# =========================================================================
# 2. x264
# =========================================================================
X264_SRC="${BUILD_DIR}/x264"
X264_INSTALL="${BUILD_DIR}/x264_install"

if [ ! -d "${X264_SRC}" ]; then
    git clone --depth 1 \
        https://code.videolan.org/videolan/x264.git \
        "${X264_SRC}"
fi

cd "${X264_SRC}"
./configure \
    --prefix="${X264_INSTALL}" \
    --enable-static \
    --disable-shared \
    --enable-pic \
    --disable-cli
make -j"$(nproc)"
make install
cd -

# =========================================================================
# 3. FFmpeg (GPL, with libx264)
# =========================================================================
FFMPEG_SRC="${BUILD_DIR}/ffmpeg"
FFMPEG_INSTALL="${BUILD_DIR}/ffmpeg_install"
FFMPEG_DST="${THIRDPARTY}/FFmpeg/lib/Linux"
FFMPEG_INC="${THIRDPARTY}/FFmpeg/include"

if [ ! -d "${FFMPEG_SRC}" ]; then
    git clone --depth 1 --branch n7.0 \
        https://github.com/FFmpeg/FFmpeg.git \
        "${FFMPEG_SRC}"
fi

cd "${FFMPEG_SRC}"
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
    --extra-cflags="-I${X264_INSTALL}/include" \
    --extra-ldflags="-L${X264_INSTALL}/lib"
make -j"$(nproc)"
make install
cd -

mkdir -p "${FFMPEG_DST}"
cp "${FFMPEG_INSTALL}/lib/libavcodec.a"    "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libavformat.a"  "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libavutil.a"    "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libswscale.a"   "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libswresample.a" "${FFMPEG_DST}/"
cp "${X264_INSTALL}/lib/libx264.a"        "${FFMPEG_DST}/"

mkdir -p "${FFMPEG_INC}"
cp -r "${FFMPEG_INSTALL}/include/"* "${FFMPEG_INC}/"

echo "[FFmpeg] done: ${FFMPEG_DST}"
echo ""
echo "All ThirdParty libs built. You can now run UBT / UnrealBuildTool."
