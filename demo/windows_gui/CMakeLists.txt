# TGCapture 图形界面开播测试工具（Win32 原生对话框，不依赖 MFC）
# 由根 CMakeLists.txt 在 TG_BUILD_DEMO=ON 且 TG_PLATFORM=windows 时 add_subdirectory 进来。

enable_language(RC)

# 版本号只在根 project(VERSION) 定义一处，资源里的 VERSIONINFO 从这里生成
configure_file(res/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

set(TGUI_SOURCES
    src/App.cpp
    src/Devices.cpp
    src/LiveConfig.cpp
    src/LiveSession.cpp
    src/LogBuffer.cpp
    src/MainDialog.cpp)

set(TGUI_HEADERS
    src/Devices.h
    src/LiveConfig.h
    src/LiveSession.h
    src/LogBuffer.h
    src/MainDialog.h
    src/StringUtil.h)

set(TGUI_RESOURCES
    res/tg_capture_gui.rc
    res/resource.h
    res/version.h.in
    res/tg_capture_gui.manifest
    res/app.ico)

add_executable(tg_capture_gui WIN32 ${TGUI_SOURCES} ${TGUI_HEADERS} ${TGUI_RESOURCES})

source_group("Source Files" FILES ${TGUI_SOURCES})
source_group("Header Files" FILES ${TGUI_HEADERS})
source_group("Resource Files" FILES ${TGUI_RESOURCES})

target_include_directories(tg_capture_gui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/res"
    "${CMAKE_CURRENT_BINARY_DIR}")       # 生成的 version.h

target_compile_definitions(tg_capture_gui PRIVATE
    UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)

# tgcapture 已 PUBLIC 带出 include/、src/（平台层 WinAudioCapture）与 ffmpeg 头文件
target_link_libraries(tg_capture_gui PRIVATE
    tgcapture
    comctl32 comdlg32 shell32 shlwapi dwmapi ole32 uuid)

if(MSVC)
    target_compile_options(tg_capture_gui PRIVATE /W3)
    # 清单已由 .rc 以 RT_MANIFEST 嵌入（Common Controls v6 / DPI 感知 / UTF-8 代码页），
    # 关掉链接器自动生成的清单，避免重复资源
    target_link_options(tg_capture_gui PRIVATE /MANIFEST:NO)
endif()

tg_setup_windows_app(tg_capture_gui)
