#!/bin/bash
# build_android.sh
# سكريبت بناء libgdffmpeg.so لنظام Android
# ─────────────────────────────────────────────────────────────────────────────
# المتطلبات:
#   1. Android NDK (نسخة 25 أو أحدث)
#      تحميل: https://developer.android.com/ndk/downloads
#   2. CMake 3.22+
#   3. FFmpeg مُجمَّع لـ Android (أنظر قسم "بناء FFmpeg" أدناه)
#
# طريقة الاستخدام:
#   chmod +x build_android.sh
#   ./build_android.sh
# ─────────────────────────────────────────────────────────────────────────────

set -e  # إيقاف عند أي خطأ

# ─── إعدادات (عدّلها حسب بيئتك) ─────────────────────────────────────────────
NDK_PATH="${HOME}/android-ndk-r26c"        # مسار Android NDK
FFMPEG_DIR="./ffmpeg-android"              # مسار FFmpeg المُجمَّع
GODOT_CPP_DIR="./godot-cpp"                # مسار godot-cpp
MIN_API=24                                  # الحد الأدنى لإصدار Android (Android 7)
BUILD_DIR="./build"

# ─── الـ ABI المستهدفة ────────────────────────────────────────────────────────
# أضف أو أزل ABIs حسب الحاجة
ABIS=("arm64-v8a" "armeabi-v7a")

# ─── التحقق من المتطلبات ─────────────────────────────────────────────────────
check_requirements() {
    echo "── فحص المتطلبات ──"
    
    if [ ! -d "${NDK_PATH}" ]; then
        echo "✗ لم يُعثر على NDK في: ${NDK_PATH}"
        echo "  حمّل NDK من: https://developer.android.com/ndk/downloads"
        echo "  ثم حدّث متغير NDK_PATH في هذا السكريبت"
        exit 1
    fi
    echo "✓ NDK: ${NDK_PATH}"

    if [ ! -d "${GODOT_CPP_DIR}" ]; then
        echo "✗ لم يُعثر على godot-cpp. جارٍ الاستنساخ..."
        git clone --recursive https://github.com/godotengine/godot-cpp.git \
            --branch godot-4.3-stable "${GODOT_CPP_DIR}"
    fi
    echo "✓ godot-cpp: موجود"

    if [ ! -d "${FFMPEG_DIR}" ]; then
        echo "✗ لم يُعثر على FFmpeg في: ${FFMPEG_DIR}"
        echo "  أنظر قسم 'بناء FFmpeg' في الوثائق"
        exit 1
    fi
    echo "✓ FFmpeg: ${FFMPEG_DIR}"

    echo ""
}

# ─── بناء لكل ABI ────────────────────────────────────────────────────────────
build_abi() {
    local ABI="$1"
    local OUT_DIR="${BUILD_DIR}/${ABI}"
    
    echo "══ بناء لـ ${ABI} ══"
    mkdir -p "${OUT_DIR}"

    TOOLCHAIN="${NDK_PATH}/build/cmake/android.toolchain.cmake"

    cmake \
        -S . \
        -B "${OUT_DIR}" \
        -G "Ninja" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DANDROID_ABI="${ABI}" \
        -DANDROID_PLATFORM="android-${MIN_API}" \
        -DANDROID_STL="c++_shared" \
        -DCMAKE_BUILD_TYPE=Release \
        -DGODOT_CPP_DIR="${GODOT_CPP_DIR}" \
        -DFFMPEG_DIR="${FFMPEG_DIR}" \
        -DANDROID_ABI="${ABI}" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF

    cmake --build "${OUT_DIR}" --parallel "$(nproc)"

    echo "✓ ${ABI}: مكتمل → ${OUT_DIR}/libgdffmpeg.android.*.so"
    echo ""
}

# ─── نسخ الملفات لمشروع Godot ────────────────────────────────────────────────
copy_to_godot() {
    local GODOT_ADDON="./godot-project/addons/gdffmpeg/bin"
    mkdir -p "${GODOT_ADDON}"

    for ABI in "${ABIS[@]}"; do
        if [ "$ABI" = "arm64-v8a" ]; then
            SO_NAME="libgdffmpeg.android.arm64.so"
        else
            SO_NAME="libgdffmpeg.android.arm32.so"
        fi

        SRC="${BUILD_DIR}/${ABI}/${SO_NAME}"
        if [ -f "${SRC}" ]; then
            cp "${SRC}" "${GODOT_ADDON}/"
            echo "✓ نُسخ: ${SO_NAME} → ${GODOT_ADDON}/"
        fi
    done
}

# ─── التنفيذ ──────────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════╗"
echo "║  بناء libgdffmpeg.so لـ Android       ║"
echo "╚══════════════════════════════════════╝"
echo ""

check_requirements

for ABI in "${ABIS[@]}"; do
    build_abi "${ABI}"
done

copy_to_godot

echo ""
echo "╔══════════════════════════════════════╗"
echo "║  ✓ البناء مكتمل بنجاح!               ║"
echo "╚══════════════════════════════════════╝"
echo ""
echo "الخطوة التالية:"
echo "  افتح مشروع Godot → Project Settings → Plugins → فعّل GDFFmpeg"
