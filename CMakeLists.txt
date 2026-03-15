cmake_minimum_required(VERSION 3.22)
project(gdffmpeg VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── مسارات التوقعات ─────────────────────────────────────────────────────────
#
# البنية المتوقعة للمجلد:
#
#   project/
#   ├── CMakeLists.txt          (هذا الملف)
#   ├── src/
#   │   └── ffmpeg_player.cpp
#   ├── include/
#   │   └── ffmpeg_player.h
#   ├── godot-cpp/              (استنسخ من: github.com/godotengine/godot-cpp)
#   └── ffmpeg-android/         (مخرجات بناء FFmpeg لـ Android)
#       ├── arm64-v8a/
#       │   ├── include/
#       │   └── lib/
#       └── armeabi-v7a/
#           ├── include/
#           └── lib/
#
# ─────────────────────────────────────────────────────────────────────────────

set(GODOT_CPP_DIR    "${CMAKE_SOURCE_DIR}/godot-cpp"      CACHE PATH "مسار godot-cpp")
set(FFMPEG_DIR       "${CMAKE_SOURCE_DIR}/ffmpeg-android" CACHE PATH "مسار FFmpeg المُجمَّع لـ Android")

# ABI المستهدف (arm64-v8a أو armeabi-v7a)
set(ANDROID_ABI      "arm64-v8a"                          CACHE STRING "Android ABI")

set(FFMPEG_ABI_DIR   "${FFMPEG_DIR}/${ANDROID_ABI}")

# ─── بناء godot-cpp ──────────────────────────────────────────────────────────
set(GODOT_CPP_BUILD_TYPE "Release")

if(NOT TARGET godot-cpp)
    add_subdirectory(${GODOT_CPP_DIR} godot-cpp-build)
endif()

# ─── المصادر ─────────────────────────────────────────────────────────────────
set(SOURCES
    "${CMAKE_SOURCE_DIR}/src/ffmpeg_player.cpp"
)

set(HEADERS
    "${CMAKE_SOURCE_DIR}/include/ffmpeg_player.h"
)

# ─── إنشاء المكتبة الديناميكية ───────────────────────────────────────────────
add_library(gdffmpeg SHARED ${SOURCES} ${HEADERS})

# ─── Include paths ───────────────────────────────────────────────────────────
target_include_directories(gdffmpeg PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${FFMPEG_ABI_DIR}/include"
    "${GODOT_CPP_DIR}/include"
    "${GODOT_CPP_DIR}/gen/include"
)

# ─── ربط مكتبات FFmpeg ───────────────────────────────────────────────────────
# الترتيب مهم: avcodec يعتمد على avutil، avformat يعتمد على avcodec
target_link_libraries(gdffmpeg PRIVATE
    godot-cpp
    "${FFMPEG_ABI_DIR}/lib/libavformat.a"
    "${FFMPEG_ABI_DIR}/lib/libavcodec.a"
    "${FFMPEG_ABI_DIR}/lib/libswscale.a"
    "${FFMPEG_ABI_DIR}/lib/libswresample.a"
    "${FFMPEG_ABI_DIR}/lib/libavutil.a"
    # مكتبات نظام Android المطلوبة
    android
    log
    z
    m
)

# ─── خيارات التجميع ──────────────────────────────────────────────────────────
target_compile_options(gdffmpeg PRIVATE
    -Wall
    -Wno-unused-parameter
    -fvisibility=hidden          # إخفاء الرموز الداخلية
    -ffunction-sections
    -fdata-sections
)

target_link_options(gdffmpeg PRIVATE
    -Wl,--gc-sections             # حذف الأقسام غير المستخدمة
    -Wl,--strip-all               # تصغير حجم الملف
)

# ─── اسم الملف المخرج ────────────────────────────────────────────────────────
# Godot يبحث عن: libgdffmpeg.android.arm64.so  أو  libgdffmpeg.android.arm32.so
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set_target_properties(gdffmpeg PROPERTIES
        OUTPUT_NAME "gdffmpeg.android.arm64"
        PREFIX      "lib"
        SUFFIX      ".so"
    )
else()
    set_target_properties(gdffmpeg PROPERTIES
        OUTPUT_NAME "gdffmpeg.android.arm32"
        PREFIX      "lib"
        SUFFIX      ".so"
    )
endif()

# ─── رسالة ما بعد البناء ─────────────────────────────────────────────────────
add_custom_command(TARGET gdffmpeg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
        "✓ تم البناء: $<TARGET_FILE:gdffmpeg>"
    COMMAND ${CMAKE_COMMAND} -E echo
        "  انسخ هذا الملف إلى: <مشروع Godot>/addons/gdffmpeg/bin/"
)
