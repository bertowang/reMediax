# Android JNI 绑定：libtgcapture_jni.so = JNI 胶水 + 内核 libtgcapture.a（+ TGCapture3rd 静态库）
# 由 bindings/android/build.gradle.kts 的 externalNativeBuild 驱动；ABI 跟随 Gradle 的 ANDROID_ABI。
cmake_minimum_required(VERSION 3.22)
project(tgcapture_jni LANGUAGES CXX)

get_filename_component(TG_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# 内核选项：用 CACHE FORCE 写入 —— 内核 CMakeLists 里是 set(... CACHE)，
# 只设普通变量会在 CMP0126 OLD 下被缓存默认值（windows / x64）覆盖
set(TG_PLATFORM "android" CACHE STRING "" FORCE)
set(TG_ABI "${ANDROID_ABI}" CACHE STRING "" FORCE)
set(TG_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(TG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${TG_REPO_ROOT}" tgcapture-core)

add_library(tgcapture_jni SHARED tgcapture_jni.cpp)
target_link_libraries(tgcapture_jni PRIVATE tgcapture log)
target_compile_options(tgcapture_jni PRIVATE -Wall -Wextra)
# 胶水自身的符号也隐藏：.so 对外只导出 JNI_OnLoad（JNIEXPORT 显式标了 default 可见性）
set_target_properties(tgcapture_jni PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
# 所有 Java native 方法都在 JNI_OnLoad 里 RegisterNatives 显式注册（含内核里 Java 采集回调那几个），
# 不依赖 Java_xxx 符号导出，因此可以放心把静态库符号全部隐藏：
#   --exclude-libs,ALL  ffmpeg / openssl / libyuv 符号不泄漏给宿主，避免与宿主自带的同名库冲突
#   --no-undefined      缺依赖在链接期就报出来，而不是运行时 dlopen 失败
target_link_options(tgcapture_jni PRIVATE
    "LINKER:--no-undefined"
    "LINKER:--exclude-libs,ALL"
    "LINKER:--gc-sections")
