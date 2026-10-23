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

# IDE 生成器（VS / Xcode）是多配置的，只保留 Debug / Release，避免配置列表过长；
# 单配置生成器（Ninja / Makefiles）继续用上面的 CMAKE_BUILD_TYPE。
if(CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
    # 多配置生成器默认给 4 个配置（Debug/Release/MinSizeRel/RelWithDebInfo），这里只留两个；
    # 用户若已显式设过自己的配置列表则不覆盖。
    if(CMAKE_CONFIGURATION_TYPES STREQUAL "Debug;Release;MinSizeRel;RelWithDebInfo")
        set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "IDE 可用配置" FORCE)
    endif()
endif()

# ---------------------------------------------------------------- 选项
set(TG_PLATFORM "windows" CACHE STRING "目标平台: windows|android|ios")
set(TG_ABI "x64" CACHE STRING "ABI: x64 | arm64-v8a | armeabi-v7a | x86_64 | fat")
set(TG_3RD_ROOT "" CACHE PATH "TGCapture3rd 根目录（FfmpegCompileShell 的产出目录）")
option(TG_BUILD_DEMO "编译测试 demo" ON)

# 依赖目录解析优先级（默认值只在这里定义一处，build.py / CI 都不再重复猜，避免多处漂移）：
#   1) 命令行 -DTG_3RD_ROOT=<dir>（scripts/build.py 的 --3rd 即透传此值）
#   2) 环境变量 TG_3RD_ROOT（CI 推荐）
#   3) 工程同级自动探测：TGCapture3rd（FfmpegCompileShell 的产出目录名）→ 旧名 3rd-rel-Party
set(TG_3RD_SOURCE "命令行/缓存 -DTG_3RD_ROOT")
if(TG_3RD_ROOT AND NOT EXISTS "${TG_3RD_ROOT}")
    # 常见于换了构建目录或产物被移走后，缓存里还留着旧路径：回退自动探测，不让用户卡在 FATAL
    message(WARNING "TG_3RD_ROOT 指向的目录不存在，回退自动探测: ${TG_3RD_ROOT}")
    set(TG_3RD_ROOT "")
endif()
if(NOT TG_3RD_ROOT)
    if(NOT "$ENV{TG_3RD_ROOT}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{TG_3RD_ROOT}" TG_3RD_ROOT)
        set(TG_3RD_SOURCE "环境变量 TG_3RD_ROOT")
    else()
        set(TG_3RD_SOURCE "默认 ../TGCapture3rd（未找到）")
        foreach(candidate "TGCapture3rd" "3rd-rel-Party")
            get_filename_component(_tg_3rd_candidate "${CMAKE_CURRENT_SOURCE_DIR}/../${candidate}" ABSOLUTE)
            if(EXISTS "${_tg_3rd_candidate}")
                set(TG_3RD_ROOT "${_tg_3rd_candidate}")
                set(TG_3RD_SOURCE "自动探测 ../${candidate}")
                break()
            endif()
        endforeach()
        if(NOT TG_3RD_ROOT)
            get_filename_component(TG_3RD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../TGCapture3rd" ABSOLUTE)
        endif()
    endif()
endif()
message(STATUS "TGCapture: platform=${TG_PLATFORM} abi=${TG_ABI}")
message(STATUS "TGCapture: 3rd=${TG_3RD_ROOT}  (来源: ${TG_3RD_SOURCE})")

# ---------------------------------------------------------------- 源码
set(TG_CORE_SOURCES
    src/ITGCapturer.cpp
    src/TGCapturer.cpp
    src/codec/VideoEncoder.cpp
    src/codec/AudioEncoder.cpp
    src/core/AVSyncClock.cpp
    src/core/AudioMixer.cpp
    src/core/VideoTransform.cpp
    src/mux/Muxer.cpp
)

set(TG_PLATFORM_SOURCES "")
set(TG_PLATFORM_LIBS "")

if(TG_PLATFORM STREQUAL "windows")
    # 屏幕：DXGI Desktop Duplication（d3d11/dxgi）+ GDI 兜底；窗口：GetWindowDC/PrintWindow
    # 音频：WASAPI（麦克风 + 扬声器 loopback）
    list(APPEND TG_PLATFORM_SOURCES
        src/platform/windows/WinScreenCapture.cpp
        src/platform/windows/WinAudioCapture.cpp)
    list(APPEND TG_PLATFORM_LIBS
        winmm ole32 user32 gdi32 dsound
        d3d11 dxgi
        ws2_32 secur32 crypt32 advapi32 bcrypt mfplat mfuuid strmiids vfw32)
elseif(TG_PLATFORM STREQUAL "android")
    # 屏幕：MediaProjection；音频：AudioRecord / AudioPlaybackCapture（经 JNI 桥接）
    list(APPEND TG_PLATFORM_SOURCES
        src/platform/android/AndroidJni.cpp
        src/platform/android/AndroidScreenCapture.cpp
        src/platform/android/AndroidAudioCapture.cpp)
    list(APPEND TG_PLATFORM_LIBS mediandk EGL GLESv3 log android)
elseif(TG_PLATFORM STREQUAL "ios")
    enable_language(OBJCXX)
    # 屏幕：ReplayKit；音频：AVAudioSession + AVAudioEngine（.mm）
    list(APPEND TG_PLATFORM_SOURCES
        src/platform/ios/IOSScreenCapture.mm
        src/platform/ios/IOSAudioCapture.mm)
    list(APPEND TG_PLATFORM_LIBS
        "-framework CoreFoundation" "-framework CoreVideo" "-framework CoreMedia"
        "-framework VideoToolbox" "-framework AudioToolbox" "-framework AVFoundation"
        "-framework ReplayKit" "-framework UIKit")
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
    # Apple 的 libc++ 把 stop_token / jthread 归为"未完备"特性（_LIBCPP_HAS_NO_EXPERIMENTAL_STOP_TOKEN），
    # 不加这个开关会报 no type named 'jthread' in namespace 'std'
    if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
        target_compile_options(tgcapture PRIVATE -fexperimental-library)
    endif()
    # iOS 的 .mm 用了 __weak / __bridge_transfer，必须开 ARC（CMake 默认不给 OBJCXX 加）
    target_compile_options(tgcapture PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

# ---------------------------------------------------------------- demo
if(TG_BUILD_DEMO AND TG_PLATFORM STREQUAL "windows")
    add_executable(tg_capture_demo demo/windows/main.cpp)
    target_link_libraries(tg_capture_demo PRIVATE tgcapture)
    if(MSVC)
        target_compile_options(tg_capture_demo PRIVATE /utf-8 /EHsc)
    endif()

    # ffmpeg 是 DLL：把 <3rd>/ffmpeg/windows/bin/*.dll 拷到 exe 同目录，
    # 这样 VS 里 F5、以及直接双击 exe 都能跑，不必手工改 PATH。
    if(TG_FFMPEG_BIN_DIR)
        file(GLOB TG_FFMPEG_DLLS "${TG_FFMPEG_BIN_DIR}/*.dll")
        foreach(dll IN LISTS TG_FFMPEG_DLLS)
            add_custom_command(TARGET tg_capture_demo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${dll}" "$<TARGET_FILE_DIR:tg_capture_demo>"
                COMMENT "拷贝 ${dll} 到 demo 输出目录")
        endforeach()
    endif()

    # VS2022 体验：把 demo 设为启动工程，调试工作目录/环境变量指向 exe 所在目录
    if(MSVC)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
            set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                         PROPERTY VS_STARTUP_PROJECT tg_capture_demo)
        endif()
        set_target_properties(tg_capture_demo PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:tg_capture_demo>"
            VS_DEBUGGER_ENVIRONMENT "PATH=$<TARGET_FILE_DIR:tg_capture_demo>;%PATH%")
    endif()
endif()

# ---------------------------------------------------------------- 安装
install(TARGETS tgcapture ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
