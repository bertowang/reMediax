cmake_minimum_required(VERSION 3.21)   # 3.21：$<TARGET_RUNTIME_DLLS>（Windows 拷 ffmpeg DLL）
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
set(TG_PLATFORM "windows" CACHE STRING "目标平台: windows|linux|android|ios|macos")
set(TG_ABI "x64" CACHE STRING "ABI: x64 | arm64-v8a | armeabi-v7a | x86_64 | arm64 | fat")
set(TG_3RD_ROOT "" CACHE PATH "TGCapture3rd 根目录（FfmpegCompileShell 的产出目录）")
option(TG_BUILD_DEMO "编译测试 demo" ON)
option(TG_BUILD_TESTS "编译单元测试 tg_tests" ON)
# 内存/线程检查（仅 clang/gcc）：address | thread | undefined，留空不开启。
# 作用于 tgcapture 及所有 demo / 测试目标，便于在 macOS 本机做 L1 回归。
set(TG_SANITIZE "" CACHE STRING "Sanitizer: address | thread | undefined（留空关闭）")
if(TG_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=${TG_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${TG_SANITIZE})
endif()

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
    src/codec/EncoderProbe.cpp
    src/core/AVSyncClock.cpp
    src/core/AudioMixer.cpp
    src/core/PauseFrame.cpp
    src/core/VideoTransform.cpp
    src/core/Watermark.cpp
    src/mux/Muxer.cpp
    src/nodes/AudioNodes.cpp
    src/nodes/VideoNodes.cpp
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
    # Foundation：IOSAudioCapture.mm 用 NSNotificationCenter 订阅 AVAudioSession 中断/路由变更，
    # 不显式声明的话，纯 C++ 宿主（不自带 Foundation）链接 libtgcapture.a 会报
    # _OBJC_CLASS_$_NSNotificationCenter 未定义
    list(APPEND TG_PLATFORM_LIBS
        "-framework Foundation"
        "-framework CoreFoundation" "-framework CoreVideo" "-framework CoreMedia"
        "-framework VideoToolbox" "-framework AudioToolbox" "-framework AVFoundation"
        "-framework ReplayKit" "-framework UIKit")
elseif(TG_PLATFORM STREQUAL "linux")
    # Linux 暂无原生采集实现（屏幕需 X11 / PipeWire，音频需 PulseAudio / ALSA）：
    # IsSupported() 一律返回 false，Create*Capture / Enable*Capture 返回负值。
    # 编码 / 封装 / 音画同步 / 混流与外灌帧（PushExternal*Frame）是全平台通用的，照常可用。
    # pthread/dl/m：ffmpeg（openssl、srt）静态链进来后需要的系统库。
    list(APPEND TG_PLATFORM_LIBS pthread dl m)
elseif(TG_PLATFORM STREQUAL "macos")
    # macOS 本机平台：只编内核（无原生采集，同 linux），用于本机单测 / 命令行 demo / sanitizer。
    # 编码可走 VideoToolbox（h264_videotoolbox）或 openh264；外灌 CVPixelBuffer 路径同 iOS。
    list(APPEND TG_PLATFORM_LIBS
        "-framework CoreFoundation" "-framework CoreVideo" "-framework CoreMedia"
        "-framework VideoToolbox" "-framework AudioToolbox" "-framework Security"
        "-framework CoreServices" c++)
endif()

add_library(tgcapture STATIC ${TG_CORE_SOURCES} ${TG_PLATFORM_SOURCES})
set_target_properties(tgcapture PROPERTIES FOLDER "sdk")   # VS 解决方案文件夹

# 版本号只在 project(VERSION) 写一处，GetVersion() 通过宏取用
target_compile_definitions(tgcapture PRIVATE TGCAPTURE_VERSION_STRING="${PROJECT_VERSION}")

target_include_directories(tgcapture PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>   # 内部以 core/ codec/ 等相对路径引用
    $<INSTALL_INTERFACE:include>)

# ---------------------------------------------------------------- 依赖
# find_package(TGCapture3rd) → TGCapture3rd::* 导入目标，见 cmake/Deps.cmake
include(cmake/Deps.cmake)
tg_link_thirdparty(tgcapture)

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
    # Xcode 生成器不认上面按语言区分的编译选项（.mm 实际以非 ARC 编译：bridge cast 失效、对象泄漏），
    # 必须走 Xcode 自己的构建设置
    set_target_properties(tgcapture PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
endif()

# ---------------------------------------------------------------- demo
# Windows 下 ffmpeg 是 DLL（TGCapture3rd::av* 为 SHARED IMPORTED 目标）：构建后把该 exe
# 实际依赖的 DLL（$<TARGET_RUNTIME_DLLS>）拷到 exe 同目录，VS 里 F5、ctest、直接双击 exe 都能跑
function(tg_copy_runtime_dlls target)
    if(TGCapture3rd_RUNTIME_DLLS)    # 依赖都是静态库时列表为空，copy 命令不能空跑
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}> "$<TARGET_FILE_DIR:${target}>"
            COMMAND_EXPAND_LISTS
            COMMENT "拷贝 ${target} 依赖的 DLL 到输出目录")
    endif()
endfunction()

# Windows 可执行程序（命令行 demo / 图形界面测试工具）共用的运行时配置：
#   - 拷贝依赖的 DLL（见 tg_copy_runtime_dlls）；
#   - VS2022 调试工作目录/环境变量指向 exe 所在目录；
#   - 在 VS 解决方案里归到 demo 文件夹。
function(tg_setup_windows_app target)
    tg_copy_runtime_dlls(${target})
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /EHsc)
        set_target_properties(${target} PROPERTIES
            FOLDER "demo"
            VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
            VS_DEBUGGER_ENVIRONMENT "PATH=$<TARGET_FILE_DIR:${target}>;%PATH%")
    endif()
endfunction()

if(TG_BUILD_DEMO AND TG_PLATFORM STREQUAL "windows")
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)

    add_executable(tg_capture_demo demo/windows/main.cpp)
    # psapi：demo 每轮 Stop 后打印进程句柄/GDI/私有内存快照，用于重开泄漏回归
    target_link_libraries(tg_capture_demo PRIVATE tgcapture psapi)
    tg_setup_windows_app(tg_capture_demo)

    # 图形界面开播测试工具（Win32 对话框），见 demo/windows_gui
    add_subdirectory(demo/windows_gui)

    # VS2022 打开解决方案后 F5 直接启动图形界面工具
    if(MSVC)
        set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                     PROPERTY VS_STARTUP_PROJECT tg_capture_gui)
    endif()
endif()

# Linux demo：本机没有采集设备也能验证"编码 → 封装"链路（走外灌帧，不依赖采集）。
# 同时也是唯一的链接自检 —— 只编静态库不会触发链接，ffmpeg 依赖链不对照不出来。
if(TG_BUILD_DEMO AND (TG_PLATFORM STREQUAL "linux" OR TG_PLATFORM STREQUAL "macos"))
    add_executable(tg_capture_demo demo/linux/main.cpp)
    target_link_libraries(tg_capture_demo PRIVATE tgcapture)
    if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
        target_compile_options(tg_capture_demo PRIVATE -fexperimental-library)
    endif()
endif()

# iOS 开播测试 App（UIKit，真机 arm64）。用 Xcode 生成器打开：scripts/build.py --platform ios --ide
# Android 的 demo 不走这里：由 Gradle（ide/android）驱动，见 demo/android
if(TG_BUILD_DEMO AND TG_PLATFORM STREQUAL "ios")
    add_subdirectory(demo/ios)
endif()

# ---------------------------------------------------------------- 单元测试
# 零依赖的迷你框架（tests/tg_test.h）；只测内核（Bus / Queue / 同步时钟 / 混音 / 脱敏），不依赖采集设备。
# 运行：ctest --test-dir <build> 或直接执行 tg_tests [名称过滤]
# 全平台可编：macos / linux / windows 本机直接跑；android 用 adb push 到设备/模拟器跑，
# ios 用 iphonesimulator SDK 编出后 xcrun simctl spawn 跑（见 scripts/test.sh）。
# Gradle 驱动的 JNI 构建会强制 TG_BUILD_TESTS=OFF，不受影响。
if(TG_BUILD_TESTS)
    enable_testing()
    add_executable(tg_tests
        tests/test_main.cpp
        tests/test_url_redact.cpp
        tests/test_queue.cpp
        tests/test_bus.cpp
        tests/test_bus_topology.cpp
        tests/test_video_nodes.cpp
        tests/test_lifecycle.cpp
        tests/test_audio_sources.cpp
        tests/test_resolution.cpp
        tests/test_health.cpp
        tests/test_traffic_meter.cpp
        tests/test_error_codes.cpp
        tests/test_video_pts.cpp
        tests/test_audio_mixer.cpp
        tests/test_audio_encoder.cpp
        tests/test_encoder_probe.cpp)
    target_include_directories(tg_tests PRIVATE tests)
    set_target_properties(tg_tests PROPERTIES FOLDER "tests")
    target_link_libraries(tg_tests PRIVATE tgcapture)
    if(TG_PLATFORM STREQUAL "windows")
        # 平台层实测：直接驱动真实 WASAPI / GDI / DXGI 源，验证失败与重开路径不泄漏
        target_sources(tg_tests PRIVATE tests/test_win_platform.cpp)
        target_link_libraries(tg_tests PRIVATE psapi ole32)
        tg_copy_runtime_dlls(tg_tests)
    endif()
    if(MSVC)
        target_compile_options(tg_tests PRIVATE /utf-8 /EHsc)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
        target_compile_options(tg_tests PRIVATE -fexperimental-library)
    endif()
    if(TG_PLATFORM STREQUAL "ios")
        # 命令行可执行文件（非 .app），只在模拟器上 simctl spawn 跑，不需要签名
        set_target_properties(tg_tests PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO)
    endif()
    add_test(NAME tg_tests COMMAND tg_tests)
endif()

# ---------------------------------------------------------------- 安装
install(TARGETS tgcapture ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
