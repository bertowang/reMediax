cmake_minimum_required(VERSION 3.20)
project(TGCapture VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------- 基本设置
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 依赖库（openh264 / fdk-aac / srt / libyuv / openssl）均由 MSVC /MT 编译，
# 本工程必须保持一致，否则跨 CRT 传递内存会直接崩溃。
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------- 选项
set(TG_PLATFORM "windows" CACHE STRING "目标平台: windows|android|ios")
set(TG_ABI "x64" CACHE STRING "ABI: x64 | arm64-v8a | armeabi-v7a | x86_64 | fat")
set(TG_3RD_ROOT "" CACHE PATH "3rd-rel-Party 根目录（依赖库）")
option(TG_BUILD_DEMO "编译测试 demo" ON)

if(NOT TG_3RD_ROOT)
    get_filename_component(TG_3RD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../3rd-rel-Party" ABSOLUTE)
endif()
message(STATUS "TGCapture: platform=${TG_PLATFORM} abi=${TG_ABI} 3rd=${TG_3RD_ROOT}")

# ---------------------------------------------------------------- 源码
set(TG_CORE_SOURCES
    src/ITGCapturer.cpp
    src/TGCapturer.cpp
    src/codec/VideoEncoder.cpp
    src/codec/AudioEncoder.cpp
    src/mux/Muxer.cpp
)

set(TG_PLATFORM_SOURCES "")
set(TG_PLATFORM_LIBS "")

if(TG_PLATFORM STREQUAL "windows")
    list(APPEND TG_PLATFORM_SOURCES
        src/platform/windows/WinScreenCapture.cpp
        src/platform/windows/WinAudioCapture.cpp)
    list(APPEND TG_PLATFORM_LIBS
        winmm ole32 user32 gdi32 dsound
        ws2_32 secur32 crypt32 advapi32 bcrypt mfplat mfuuid strmiids vfw32)
elseif(TG_PLATFORM STREQUAL "android")
    list(APPEND TG_PLATFORM_LIBS mediandk EGL GLESv3 log android)
elseif(TG_PLATFORM STREQUAL "ios")
    list(APPEND TG_PLATFORM_LIBS
        "-framework CoreFoundation" "-framework CoreVideo" "-framework CoreMedia"
        "-framework VideoToolbox" "-framework AudioToolbox" "-framework AVFoundation")
endif()

add_library(tgcapture STATIC ${TG_CORE_SOURCES} ${TG_PLATFORM_SOURCES})

target_include_directories(tgcapture PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>   # 内部以 core/ codec/ 等相对路径引用
    $<INSTALL_INTERFACE:include>)

# ---------------------------------------------------------------- 依赖
include(cmake/Deps.cmake)
tg_link_thirdparty(tgcapture ${TG_PLATFORM} ${TG_ABI} "${TG_3RD_ROOT}")

target_link_libraries(tgcapture PUBLIC ${TG_PLATFORM_LIBS})

if(MSVC)
    target_compile_options(tgcapture PRIVATE /W3 /utf-8 /EHsc)
    target_compile_definitions(tgcapture PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(tgcapture PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------- demo
if(TG_BUILD_DEMO AND TG_PLATFORM STREQUAL "windows")
    add_executable(tg_capture_demo demo/windows/main.cpp)
    target_link_libraries(tg_capture_demo PRIVATE tgcapture)
    if(MSVC)
        target_compile_options(tg_capture_demo PRIVATE /utf-8 /EHsc)
    endif()
endif()

# ---------------------------------------------------------------- 安装
install(TARGETS tgcapture ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
