#!/usr/bin/env bash
# build_thirdparty.sh
#
# Builds CCL (CIGI Class Library) and FFmpeg (with libx264) and copies
# the resulting static libraries into the ThirdParty tree.
# Supports macOS (arm64 / x86_64) and Linux (x86_64).
#
# On macOS, uses the native toolchain.  Supports cross-compiling from
# Apple Silicon to x86_64 (pass x86_64 as the first argument).
# On Linux, uses UE's bundled clang toolchain when UE_ROOT is set or
# discoverable, so the resulting libraries are ABI-compatible with UBT.
#
# macOS prerequisites (Homebrew):
#   brew install cmake ninja nasm git pkg-config
#
# Linux prerequisites (Ubuntu 22.04):
#   sudo apt-get install -y cmake ninja-build nasm git build-essential
#
# Usage:
#   ./scripts/build_thirdparty.sh            # native arch
#   ./scripts/build_thirdparty.sh x86_64     # macOS cross-compile only

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRDPARTY="${REPO_ROOT}/unreal_project/CamSimTest/Source/ThirdParty"
BUILD_DIR="${REPO_ROOT}/.build_tmp"

# -------------------------------------------------------------------------
# Platform detection
# -------------------------------------------------------------------------
OS="$(uname -s)"
case "${OS}" in
    Darwin) PLATFORM="mac"   ;;
    Linux)  PLATFORM="linux" ;;
    *)
        echo "[ERROR] Unsupported platform: ${OS}"
        exit 1
        ;;
esac

# =========================================================================
# macOS — arch / toolchain setup
# =========================================================================
if [ "${PLATFORM}" = "mac" ]; then
    NATIVE_ARCH="$(uname -m)"
    TARGET_ARCH="${1:-${NATIVE_ARCH}}"
    NCPU="$(sysctl -n hw.logicalcpu)"
    MACOS_MIN_VERSION="12.0"

    CMAKE_ARCH_FLAGS=(
        -DCMAKE_OSX_ARCHITECTURES="${TARGET_ARCH}"
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_MIN_VERSION}"
    )
    ARCH_CFLAGS="-arch ${TARGET_ARCH} -mmacosx-version-min=${MACOS_MIN_VERSION}"

    if [ "${TARGET_ARCH}" = "${NATIVE_ARCH}" ]; then
        HOST_TRIPLE=""
    else
        HOST_TRIPLE="--host=${TARGET_ARCH}-apple-darwin"
    fi

    # arm64 cannot use x264's x86-only hand-written asm
    if [ "${TARGET_ARCH}" = "arm64" ]; then
        X264_NASM_OPT="--disable-asm"
        FFMPEG_EXTRA="--disable-x86asm"
    else
        X264_NASM_OPT=""
        FFMPEG_EXTRA=""
    fi

    SED_INPLACE() { sed -i '' "$@"; }

    check_brew_dep() {
        if ! command -v "$1" &>/dev/null && ! brew list "$2" &>/dev/null 2>&1; then
            echo "[ERROR] '$1' not found. Install with: brew install $2"
            exit 1
        fi
    }
    check_brew_dep cmake cmake
    check_brew_dep ninja ninja
    check_brew_dep nasm  nasm
    check_brew_dep git   git

    echo "==> Building ThirdParty for macOS (${TARGET_ARCH}) with ${NCPU} jobs"

# =========================================================================
# Linux — compiler / toolchain setup
# =========================================================================
else
    NCPU="$(nproc)"
    CMAKE_ARCH_FLAGS=()
    ARCH_CFLAGS=""
    HOST_TRIPLE=""
    X264_NASM_OPT=""

    # Enable x86 SIMD in FFmpeg when nasm is available (recommended for correct
    # sws_scale code paths); fall back to C-only if nasm is missing.
    if command -v nasm &>/dev/null; then
        FFMPEG_EXTRA=""
        echo "==> nasm found — FFmpeg x86asm ENABLED (recommended)"
    else
        FFMPEG_EXTRA="--disable-x86asm"
        echo "[WARN] nasm not found — FFmpeg x86asm DISABLED (may cause color issues)"
    fi

    SED_INPLACE() { sed -i "$@"; }

    # Use UE's bundled clang when available so libs are ABI-compatible with UBT
    if [ -z "${CC:-}" ] && [ -z "${CXX:-}" ]; then
        if [ -z "${UE_ROOT:-}" ]; then
            for CANDIDATE in \
                "${HOME}/UnrealEngine" \
                "/opt/UnrealEngine" \
                "/opt/Epic/UE_5.7" \
                "${HOME}/.local/share/UnrealEngine"
            do
                if [ -f "${CANDIDATE}/Engine/Binaries/Linux/UnrealEditor" ]; then
                    UE_ROOT="${CANDIDATE}"; break
                fi
            done
            if [ -z "${UE_ROOT:-}" ]; then
                _BIN="$(find /opt "${HOME}" -maxdepth 6 -name UnrealEditor \
                    -path '*/Binaries/Linux/UnrealEditor' 2>/dev/null | head -1 || true)"
                [ -n "${_BIN}" ] && UE_ROOT="${_BIN%%/Engine/Binaries/*}"
            fi
        fi

        if [ -n "${UE_ROOT:-}" ]; then
            _TC_BASE="${UE_ROOT}/Engine/Extras/ThirdPartyNotUE/SDKs/HostLinux/Linux_x64"
            _TC_DIR="$(ls -d "${_TC_BASE}"/v*_clang-* 2>/dev/null | sort -V | tail -1 || true)"
            _TC_CLANG="${_TC_DIR}/x86_64-unknown-linux-gnu/bin/clang"
            if [ -f "${_TC_CLANG}" ]; then
                export CC="${_TC_CLANG}"
                export CXX="${_TC_CLANG}++"
                CCL_CXX_FLAGS="-stdlib=libc++"
                echo "==> Compiler: UE bundled clang (libc++)  CC=${CC}"
            else
                echo "[WARN] UE found at ${UE_ROOT} but bundled clang not at expected path."
                echo "       Falling back to system compiler — may not be ABI-compatible."
                CCL_CXX_FLAGS=""
            fi
        else
            echo "[WARN] UE_ROOT not set and UnrealEditor not found in standard locations."
            echo "       Falling back to system compiler.  Fix: export UE_ROOT=/path/to/UE_5.7"
            CCL_CXX_FLAGS=""
        fi
    else
        echo "==> Compiler: CC/CXX from environment  CC=${CC:-<unset>}  CXX=${CXX:-<unset>}"
        CCL_CXX_FLAGS=""
    fi

    echo "==> Building ThirdParty for Linux (x86_64) with ${NCPU} jobs"
fi

mkdir -p "${BUILD_DIR}"

# =========================================================================
# 1. CCL — CIGI Class Library (LGPL-2.1)
# =========================================================================
CCL_SRC="${BUILD_DIR}/ccl"
CCL_DST="${THIRDPARTY}/CCL/lib/$([ "${PLATFORM}" = "mac" ] && echo "Mac" || echo "Linux")"
CCL_INC="${THIRDPARTY}/CCL/include"

if [ ! -d "${CCL_SRC}" ]; then
    echo "==> Cloning CCL..."
    git clone --depth 1 https://github.com/Hadron/cigi-ccl.git "${CCL_SRC}"
fi

# cmake >= 3.27 requires cmake_minimum_required before project(); patch if missing
if ! grep -qiE '^cmake_minimum_required' "${CCL_SRC}/CMakeLists.txt"; then
    echo "==> Patching CMakeLists.txt (prepending cmake_minimum_required)"
    SED_INPLACE '1s/^/cmake_minimum_required(VERSION 3.10)\n/' "${CCL_SRC}/CMakeLists.txt"
fi

CCL_BUILD="${BUILD_DIR}/ccl_build"
cmake -S "${CCL_SRC}" -B "${CCL_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    "${CMAKE_ARCH_FLAGS[@]+"${CMAKE_ARCH_FLAGS[@]}"}" \
    ${CC:+-DCMAKE_C_COMPILER="${CC}"} \
    ${CXX:+-DCMAKE_CXX_COMPILER="${CXX}"} \
    ${CCL_CXX_FLAGS:+-DCMAKE_CXX_FLAGS="${CCL_CXX_FLAGS}"} \
    -GNinja

cmake --build "${CCL_BUILD}" --target cigicl-static --parallel "${NCPU}"

CCL_LIB="${CCL_BUILD}/lib/libcigicl.a"
if [ ! -f "${CCL_LIB}" ]; then
    echo "[ERROR] Expected static lib not found: ${CCL_LIB}"
    ls "${CCL_BUILD}/lib/" 2>/dev/null || echo "  (directory missing)"
    exit 1
fi

mkdir -p "${CCL_DST}" "${CCL_INC}/cigicl"
cp "${CCL_LIB}" "${CCL_DST}/"
cp "${CCL_SRC}"/include/*.h "${CCL_INC}/cigicl/"
echo "[CCL] done → ${CCL_DST}/libcigicl.a"

# =========================================================================
# 2. x264
# =========================================================================
X264_SRC="${BUILD_DIR}/x264"
X264_INSTALL="${BUILD_DIR}/x264_install"

if [ ! -d "${X264_SRC}" ]; then
    echo "==> Cloning x264..."
    git clone --depth 1 https://code.videolan.org/videolan/x264.git "${X264_SRC}"
fi

cd "${X264_SRC}"
./configure \
    --prefix="${X264_INSTALL}" \
    --enable-static \
    --disable-shared \
    --enable-pic \
    --disable-cli \
    ${X264_NASM_OPT} \
    ${HOST_TRIPLE} \
    ${ARCH_CFLAGS:+--extra-cflags="${ARCH_CFLAGS}"} \
    ${ARCH_CFLAGS:+--extra-ldflags="${ARCH_CFLAGS}"} \
    ${CC:+--cc="${CC}"}
make -j"${NCPU}"
make install
cd "${REPO_ROOT}"

# -------------------------------------------------------------------------
# Linux only: inject glibc C23 compat stubs into libx264.a
#
# glibc 2.38+ redirects strtol/sscanf etc. to __isoc23_* symbols.  These
# don't exist in UE's glibc 2.17 sysroot.  Compile the shim WITHOUT
# _GNU_SOURCE and inject it into the archive.
# -------------------------------------------------------------------------
if [ "${PLATFORM}" = "linux" ]; then
    echo "==> Injecting glibc C23 compat stubs into libx264.a ..."
    ${CC:-cc} -O2 -fPIC -fvisibility=hidden -std=c11 \
        -c "${REPO_ROOT}/scripts/isoc23_compat.c" \
        -o "${BUILD_DIR}/isoc23_compat.o"
    ar r "${X264_INSTALL}/lib/libx264.a" "${BUILD_DIR}/isoc23_compat.o"
    echo "[isoc23_compat] done"
fi

# =========================================================================
# 3. FFmpeg (GPL + libx264)
# =========================================================================
FFMPEG_SRC="${BUILD_DIR}/ffmpeg"
FFMPEG_INSTALL="${BUILD_DIR}/ffmpeg_install"
FFMPEG_DST="${THIRDPARTY}/FFmpeg/lib/$([ "${PLATFORM}" = "mac" ] && echo "Mac" || echo "Linux")"
FFMPEG_INC="${THIRDPARTY}/FFmpeg/include"

if [ ! -d "${FFMPEG_SRC}" ]; then
    echo "==> Cloning FFmpeg (n7.0)..."
    git clone --depth 1 --branch n7.0 \
        https://github.com/FFmpeg/FFmpeg.git "${FFMPEG_SRC}"
fi

cd "${FFMPEG_SRC}"

FFMPEG_CONFIGURE_EXTRA=(
    --prefix="${FFMPEG_INSTALL}"
    --enable-gpl
    --enable-libx264
    --enable-muxer=mpegts
    --enable-protocol=udp
    --enable-static
    --disable-shared
    --enable-pic
    --disable-debug
    --disable-doc
    --disable-programs
    --extra-cflags="-I${X264_INSTALL}/include ${ARCH_CFLAGS:+${ARCH_CFLAGS}} -fvisibility=hidden"
    --extra-ldflags="-L${X264_INSTALL}/lib ${ARCH_CFLAGS:+${ARCH_CFLAGS}}"
)
[ -n "${FFMPEG_EXTRA}" ] && FFMPEG_CONFIGURE_EXTRA+=("${FFMPEG_EXTRA}")
[ -n "${HOST_TRIPLE}"  ] && FFMPEG_CONFIGURE_EXTRA+=("${HOST_TRIPLE}")
[ -n "${CC:-}"         ] && FFMPEG_CONFIGURE_EXTRA+=(--cc="${CC}")
[ -n "${CXX:-}"        ] && FFMPEG_CONFIGURE_EXTRA+=(--cxx="${CXX}")

if [ "${PLATFORM}" = "linux" ]; then
    TMPDIR="${BUILD_DIR}" PKG_CONFIG_PATH="${X264_INSTALL}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
        ./configure "${FFMPEG_CONFIGURE_EXTRA[@]}"
else
    PKG_CONFIG_PATH="${X264_INSTALL}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
        ./configure "${FFMPEG_CONFIGURE_EXTRA[@]}"
fi

make -j"${NCPU}"
make install
cd "${REPO_ROOT}"

mkdir -p "${FFMPEG_DST}" "${FFMPEG_INC}"
cp "${FFMPEG_INSTALL}/lib/libavcodec.a"     "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libavformat.a"   "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libavutil.a"     "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libswscale.a"    "${FFMPEG_DST}/"
cp "${FFMPEG_INSTALL}/lib/libswresample.a" "${FFMPEG_DST}/"
cp "${X264_INSTALL}/lib/libx264.a"         "${FFMPEG_DST}/"
cp -r "${FFMPEG_INSTALL}/include/"*        "${FFMPEG_INC}/"

# Linux: inject compat stubs into FFmpeg archives too
if [ "${PLATFORM}" = "linux" ]; then
    for _LIB in "${FFMPEG_DST}/"{libavcodec,libavformat,libavutil}.a "${FFMPEG_DST}/libx264.a"; do
        [ -f "${_LIB}" ] && ar r "${_LIB}" "${BUILD_DIR}/isoc23_compat.o"
    done
fi

echo "[FFmpeg] done → ${FFMPEG_DST}"
echo ""
echo "==> All ThirdParty libs built. You can now run UBT / UnrealBuildTool."
