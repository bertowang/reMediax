# 依赖 FfmpegCompileShell 的产出目录（默认 TGCapture3rd）：<root>/<库>/<平台>[/<abi>]/{include,lib}
#
# 布局不写死，按 FfmpegCompileShell 的 PLATFORM_LAYOUT 依次探测：
#   abi     : <库>/<平台>/<abi>/{include,lib}          —— android / ohos，或 Windows 同时编多 ABI 时回落
#   lib     : <库>/<平台>/include + <库>/<平台>/lib/<abi> —— spdlog
#   flat    : <库>/<平台>/{include,lib}                —— Windows 开播用到的库（ffmpeg/libyuv/openssl/srt/fdk-aac/openh264）
# iOS 为 lipo 合并的 fat 库，天然无 abi 层，flat 直接命中。
function(_tg_dep_dir out root lib platform abi)
    set(${out} "" PARENT_SCOPE)
    foreach(candidate
            "${root}/${lib}/${platform}/${abi}"
            "${root}/${lib}/${platform}/lib/${abi}"
            "${root}/${lib}/${platform}")
        if(EXISTS "${candidate}/include" OR EXISTS "${candidate}/lib")
            set(${out} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

function(tg_link_thirdparty target platform abi root)
    if(NOT EXISTS "${root}")
        message(FATAL_ERROR
            "依赖目录未找到: ${root}\n"
            "  用 --3rd <目录> 或 -DTG_3RD_ROOT=<目录> 指向 FfmpegCompileShell 的输出目录（如 ../TGCapture3rd）")
    endif()

    _tg_dep_dir(ffmpeg_dir "${root}" "ffmpeg" "${platform}" "${abi}")
    if(NOT ffmpeg_dir)
        message(FATAL_ERROR
            "在 ${root}/ffmpeg/${platform} 下找不到 {include,lib}（当前 abi=${abi}）；"
            "请先执行 FfmpegCompileShell 编译，或用 --3rd 指向正确的输出目录")
    endif()

    target_include_directories(${target} PUBLIC "${ffmpeg_dir}/include")
    target_link_directories(${target} PUBLIC "${ffmpeg_dir}/lib")

    # ffmpeg 组件（Windows 为 import lib，其它平台为 .a）
    target_link_libraries(${target} PUBLIC avcodec avformat avutil swresample swscale)

    # 其余开播依赖（静态库）
    foreach(lib openh264 fdk-aac srt libyuv openssl)
        _tg_dep_dir(dir "${root}" "${lib}" "${platform}" "${abi}")
        if(NOT dir)
            # libyuv 是应用直接链接的独立库，缺了必然链接失败；
            # 其余库已静态并入 ffmpeg，缺失只影响单独使用的场景
            if(lib STREQUAL "libyuv")
                message(FATAL_ERROR "libyuv 未找到: ${root}/libyuv/${platform}（abi=${abi}）")
            endif()
            message(WARNING "依赖缺失，跳过: ${root}/${lib}/${platform}（abi=${abi}）")
            continue()
        endif()
        if(EXISTS "${dir}/include")
            target_include_directories(${target} PUBLIC "${dir}/include")
        endif()
        if(EXISTS "${dir}/lib")
            target_link_directories(${target} PUBLIC "${dir}/lib")
        endif()
    endforeach()

    # 静态链接这些库（libyuv 由应用直接链接；其余已静态并入 ffmpeg，重复链无符号需求时不会引入目标文件）
    target_link_libraries(${target} PUBLIC openh264 fdk-aac srt yuv ssl crypto)

    # 导出给上层：Windows 下 ffmpeg 的 DLL 在 <ffmpeg>/bin，用于 demo 的调试环境与产物拷贝
    if(EXISTS "${ffmpeg_dir}/bin")
        set(TG_FFMPEG_BIN_DIR "${ffmpeg_dir}/bin" PARENT_SCOPE)
    else()
        set(TG_FFMPEG_BIN_DIR "" PARENT_SCOPE)
    endif()

    message(STATUS "TGCapture: ffmpeg=${ffmpeg_dir}")
endfunction()
