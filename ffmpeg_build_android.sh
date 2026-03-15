#!/bin/bash
# ffmpeg_build_android.sh
# ─────────────────────────────────────────────────────────────────────────────
# بناء FFmpeg مُجمَّع ثابتاً (static) لنظام Android
# المخرج: مجلد ffmpeg-android/ يحتوي على:
#   arm64-v8a/include/  ← ملفات الهيدر
#   arm64-v8a/lib/      ← libavcodec.a, libavformat.a, ...
#   armeabi-v7a/...     (اختياري)
# ─────────────────────────────────────────────────────────────────────────────

set -e

# ── الإعدادات ─────────────────────────────────────────────────────────────────
NDK_PATH="${HOME}/android-ndk-r26c"       # عدّل هذا
FFMPEG_VERSION="7.0"
FFMPEG_SRC="./ffmpeg-${FFMPEG_VERSION}"
OUTPUT_DIR="./ffmpeg-android"
API_LEVEL=24                               # Android 7+

# ── تحميل FFmpeg ──────────────────────────────────────────────────────────────
if [ ! -d "${FFMPEG_SRC}" ]; then
    echo "── تحميل FFmpeg ${FFMPEG_VERSION} ──"
    wget -q "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.gz"
    tar xzf "ffmpeg-${FFMPEG_VERSION}.tar.gz"
fi

# ── دالة البناء لكل ABI ───────────────────────────────────────────────────────
build_ffmpeg() {
    local ABI="$1"
    local ARCH="$2"
    local CPU="$3"
    local CROSS_PREFIX="$4"

    echo ""
    echo "══ بناء FFmpeg لـ ${ABI} ══"

    local PREFIX="${OUTPUT_DIR}/${ABI}"
    mkdir -p "${PREFIX}"

    local TOOLCHAIN="${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64"
    local SYSROOT="${TOOLCHAIN}/sysroot"

    # تصدير متغيرات المترجم
    export CC="${TOOLCHAIN}/bin/${CROSS_PREFIX}${API_LEVEL}-clang"
    export CXX="${TOOLCHAIN}/bin/${CROSS_PREFIX}${API_LEVEL}-clang++"
    export AR="${TOOLCHAIN}/bin/llvm-ar"
    export AS="${CC}"
    export NM="${TOOLCHAIN}/bin/llvm-nm"
    export STRIP="${TOOLCHAIN}/bin/llvm-strip"
    export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
    export LD="${TOOLCHAIN}/bin/ld"

    cd "${FFMPEG_SRC}"

    ./configure \
        --prefix="${PREFIX}" \
        --target-os=android \
        --arch="${ARCH}" \
        --cpu="${CPU}" \
        --enable-cross-compile \
        --cross-prefix="${TOOLCHAIN}/bin/llvm-" \
        --sysroot="${SYSROOT}" \
        --cc="${CC}" \
        --cxx="${CXX}" \
        --ar="${AR}" \
        --nm="${NM}" \
        --ranlib="${RANLIB}" \
        --strip="${STRIP}" \
        \
        --enable-static          \
        --disable-shared         \
        --disable-programs       \
        --disable-doc            \
        --disable-debug          \
        \
        --enable-avcodec         \
        --enable-avformat        \
        --enable-avutil          \
        --enable-swscale         \
        --enable-swresample      \
        \
        --enable-decoder=h264    \
        --enable-decoder=hevc    \
        --enable-decoder=vp8     \
        --enable-decoder=vp9     \
        --enable-decoder=av1     \
        --enable-decoder=mpeg4   \
        --enable-decoder=aac     \
        --enable-decoder=mp3     \
        --enable-decoder=ac3     \
        --enable-decoder=flac    \
        --enable-decoder=opus    \
        \
        --enable-demuxer=mp4     \
        --enable-demuxer=matroska \
        --enable-demuxer=avi     \
        --enable-demuxer=mov     \
        \
        --enable-parser=h264     \
        --enable-parser=hevc     \
        --enable-parser=aac      \
        \
        --enable-protocol=file   \
        --enable-protocol=pipe   \
        \
        --disable-everything-else-not-listed-above \
        \
        --extra-cflags="-Os -fPIC -fvisibility=hidden" \
        --extra-ldflags="-Wl,--gc-sections"

    make -j"$(nproc)"
    make install
    make clean

    cd ..
    echo "✓ ${ABI}: مكتمل في ${PREFIX}"
}

# ── بناء arm64-v8a ────────────────────────────────────────────────────────────
build_ffmpeg "arm64-v8a"   "aarch64" "armv8-a" "aarch64-linux-android"

# ── بناء armeabi-v7a (اختياري، أجهزة قديمة) ─────────────────────────────────
# build_ffmpeg "armeabi-v7a" "arm"     "armv7-a" "armv7a-linux-androideabi"

echo ""
echo "✓ FFmpeg لـ Android جاهز في: ${OUTPUT_DIR}/"
echo "  استخدم مسار هذا المجلد في FFMPEG_DIR بسكريبت البناء الرئيسي"
