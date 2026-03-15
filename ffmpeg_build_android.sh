#!/bin/bash
# ffmpeg_build_android.sh
# يعمل محلياً وفي GitHub Actions
# ─────────────────────────────────────────────────────────────────────────────
set -e

# ── الإعدادات: تأتي من متغيرات البيئة أو القيم الافتراضية ───────────────────
NDK_PATH="${NDK_PATH:-${HOME}/android-ndk-r26c}"
FFMPEG_VERSION="${FFMPEG_VERSION:-7.0}"
OUTPUT_DIR="${OUTPUT_DIR:-${PWD}/ffmpeg-android}"
API_LEVEL="${API_LEVEL:-24}"

echo ""
echo "╔══════════════════════════════════════════════╗"
echo "║  بناء FFmpeg ${FFMPEG_VERSION} لـ Android (arm64-v8a)  ║"
echo "╚══════════════════════════════════════════════╝"
echo "  NDK_PATH   : ${NDK_PATH}"
echo "  OUTPUT_DIR : ${OUTPUT_DIR}"
echo "  API Level  : ${API_LEVEL}"
echo ""

# ── تحميل FFmpeg إذا لم يوجد ─────────────────────────────────────────────────
FFMPEG_SRC="ffmpeg-${FFMPEG_VERSION}"
if [ ! -d "${FFMPEG_SRC}" ]; then
    echo "── تحميل FFmpeg ${FFMPEG_VERSION} ──"
    wget -q --show-progress \
        "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz" \
        -O "ffmpeg-${FFMPEG_VERSION}.tar.gz"
    tar xzf "ffmpeg-${FFMPEG_VERSION}.tar.gz"
    rm "ffmpeg-${FFMPEG_VERSION}.tar.gz"
fi

# ── دالة البناء ───────────────────────────────────────────────────────────────
build_abi() {
    local ABI="$1"
    local ARCH="$2"
    local CPU="$3"
    local CROSS_PREFIX_BIN="$4"

    local PREFIX="${OUTPUT_DIR}/${ABI}"
    mkdir -p "${PREFIX}"

    echo "══ بناء FFmpeg لـ ${ABI} ══"

    local TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"
    local SYSROOT="${TOOLCHAIN}/sysroot"

    export CC="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang"
    export CXX="${TOOLCHAIN}/bin/${CROSS_PREFIX_BIN}${API_LEVEL}-clang++"
    export AR="${TOOLCHAIN}/bin/llvm-ar"
    export AS="${CC}"
    export NM="${TOOLCHAIN}/bin/llvm-nm"
    export STRIP="${TOOLCHAIN}/bin/llvm-strip"
    export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"

    cd "${FFMPEG_SRC}"

    ./configure \
        --prefix="${PREFIX}" \
        --target-os=android \
        --arch="${ARCH}" \
        --cpu="${CPU}" \
        --enable-cross-compile \
        --sysroot="${SYSROOT}" \
        --cc="${CC}" \
        --cxx="${CXX}" \
        --ar="${AR}" \
        --nm="${NM}" \
        --ranlib="${RANLIB}" \
        --strip="${STRIP}" \
        \
        --enable-static \
        --disable-shared \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --disable-everything \
        \
        --enable-avcodec \
        --enable-avformat \
        --enable-avutil \
        --enable-swscale \
        --enable-swresample \
        \
        --enable-decoder=h264 \
        --enable-decoder=hevc \
        --enable-decoder=vp8 \
        --enable-decoder=vp9 \
        --enable-decoder=av1 \
        --enable-decoder=mpeg4 \
        --enable-decoder=aac \
        --enable-decoder=mp3 \
        --enable-decoder=opus \
        --enable-decoder=flac \
        --enable-decoder=ac3 \
        \
        --enable-demuxer=mp4 \
        --enable-demuxer=matroska \
        --enable-demuxer=mov \
        --enable-demuxer=avi \
        \
        --enable-parser=h264 \
        --enable-parser=hevc \
        --enable-parser=aac \
        \
        --enable-protocol=file \
        --enable-protocol=pipe \
        \
        --extra-cflags="-Os -fPIC -fvisibility=hidden" \
        --extra-ldflags="-Wl,--gc-sections"

    make -j"$(nproc)"
    make install
    make clean

    cd ..
    echo "✓ ${ABI} → ${PREFIX}"
}

# ── التنفيذ ───────────────────────────────────────────────────────────────────
build_abi "arm64-v8a" "aarch64" "armv8-a" "aarch64-linux-android"
build_abi "armeabi-v7a" "arm" "armv7-a" "armv7a-linux-androideabi"

echo ""
echo "✓ FFmpeg جاهز في: ${OUTPUT_DIR}/"
ls -lh "${OUTPUT_DIR}/arm64-v8a/lib/"
