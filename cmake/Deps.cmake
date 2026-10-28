# 第三方依赖 TGCapture3rd（FfmpegCompileShell 的产出目录），按标准 CMake 包引入：
#
#   find_package(TGCapture3rd CONFIG COMPONENTS ffmpeg libyuv)
#   → TGCapture3rd::avformat / avcodec / swscale / swresample / avutil / libyuv 等导入目标
#
# 包配置 TGCapture3rdConfig.cmake 由 FfmpegCompileShell 在每次 build 结束时写到产物根目录；
# 旧产物没有的话补一份：python build.py export -o <产物根目录>。
# 导入目标自带头文件目录、库文件完整路径、传递依赖（静态 ffmpeg 的 openssl / srt / fdk-aac /
# openh264 / zlib 与系统库，取自 ffmpeg 自己的 .pc）以及 Windows 下 DLL 的位置，
# 所以这里不再手写 include / link 目录，也不用操心 GNU ld 单遍解析的链接顺序。
#
# 包的查找顺序（CMake 标准规则）：
#   -DTGCapture3rd_DIR=<根> / -DTGCapture3rd_ROOT=<根> / -DCMAKE_PREFIX_PATH=<根>
#   > TG_3RD_ROOT（作为 HINTS；由根 CMakeLists.txt 解析：--3rd > 环境变量 > 同级目录）

# 平台 / ABI 明确交给包配置，与本工程的 TG_PLATFORM / TG_ABI 保持一致；
# Windows 上包配置还会核对编译器的目标架构（-A），不一致时在配置阶段就报错，而不是等到 LNK1112
if(NOT DEFINED TGCapture3rd_PLATFORM)
    set(TGCapture3rd_PLATFORM "${TG_PLATFORM}")
endif()
if(NOT DEFINED TGCapture3rd_ABI AND NOT TG_PLATFORM STREQUAL "ios")   # iOS 是 fat 库，没有 abi 层
    set(TGCapture3rd_ABI "${TG_ABI}")
endif()

# 产物目录在、但还是旧版本（没有包配置）：直接给出补救命令，比 find_package 的通用报错好懂
if(EXISTS "${TG_3RD_ROOT}" AND NOT EXISTS "${TG_3RD_ROOT}/TGCapture3rdConfig.cmake"
   AND NOT TGCapture3rd_DIR AND NOT TGCapture3rd_ROOT AND NOT CMAKE_PREFIX_PATH)
    message(FATAL_ERROR
        "${TG_3RD_ROOT} 里没有 TGCapture3rdConfig.cmake（产物由旧版 FfmpegCompileShell 生成）。\n"
        "  在 FfmpegCompileShell 下执行一次：python build.py export -o \"${TG_3RD_ROOT}\"")
endif()

# NO_CMAKE_FIND_ROOT_PATH：Android / OHOS 工具链把包查找限定在 sysroot 里（CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY），
# 而 TGCapture3rd 在宿主机上
find_package(TGCapture3rd 1 CONFIG REQUIRED
    COMPONENTS ffmpeg libyuv
    HINTS "${TG_3RD_ROOT}"
    NO_CMAKE_FIND_ROOT_PATH)

# 内核直接用到的只有 ffmpeg 与 libyuv（采集帧的格式转换 / 缩放）；
# openssl / srt / fdk-aac / openh264 / zlib 由 ffmpeg 的导入目标按需传递（Windows 下已并入 ffmpeg DLL）
function(tg_link_thirdparty target)
    target_link_libraries(${target} PUBLIC
        TGCapture3rd::avformat
        TGCapture3rd::avcodec
        TGCapture3rd::swscale
        TGCapture3rd::swresample
        TGCapture3rd::avutil
        TGCapture3rd::libyuv)
endfunction()
