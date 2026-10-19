# 依赖 3rd-rel-Party：<root>/<库>/<平台>[/<abi>]/{include,lib}
# 布局与 FfmpegCompileShell 的产出一致（iOS 不带 abi 层）。
function(tg_link_thirdparty target platform abi root)
    if(NOT EXISTS "${root}")
        message(FATAL_ERROR "3rd-rel-Party 未找到: ${root}")
    endif()

    if(platform STREQUAL "ios")
        set(ffmpeg_dir "${root}/ffmpeg/${platform}")
    else()
        set(ffmpeg_dir "${root}/ffmpeg/${platform}/${abi}")
    endif()

    if(NOT EXISTS "${ffmpeg_dir}")
        message(FATAL_ERROR "ffmpeg 未编译: ${ffmpeg_dir}（先执行 FfmpegCompileShell）")
    endif()

    target_include_directories(${target} PUBLIC "${ffmpeg_dir}/include")
    target_link_directories(${target} PUBLIC "${ffmpeg_dir}/lib")

    # ffmpeg 组件（Windows 为 import lib，其它平台为 .a）
    target_link_libraries(${target} PUBLIC avcodec avformat avutil swresample swscale swscale)

    # 其余开播依赖（静态库）
    foreach(lib openh264 fdk-aac srt libyuv openssl)
        if(platform STREQUAL "ios")
            set(dir "${root}/${lib}/${platform}")
        else()
            set(dir "${root}/${lib}/${platform}/${abi}")
        endif()
        if(EXISTS "${dir}/include")
            target_include_directories(${target} PUBLIC "${dir}/include")
        endif()
        if(EXISTS "${dir}/lib")
            target_link_directories(${target} PUBLIC "${dir}/lib")
        endif()
    endforeach()

    # 静态链接这些库
    target_link_libraries(${target} PUBLIC openh264 fdk-aac srt yuv ssl crypto)

    message(STATUS "TGCapture: ffmpeg=${ffmpeg_dir}")
endfunction()
