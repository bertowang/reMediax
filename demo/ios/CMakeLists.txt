# TGCapture 开播测试 App（iOS）
#
# 由仓库根 CMakeLists.txt 在 TG_PLATFORM=ios 且 TG_BUILD_DEMO=ON 时引入。生成 Xcode 工程：
#   python3 scripts/build.py --platform ios --ide [--team <开发者 Team ID>]
# 依赖 TGCapture3rd 的 iOS 库只有 arm64 真机切片，没有模拟器切片 —— 只能跑在真机上。
#
# 签名：Team ID 用 -DTG_IOS_TEAM_ID=<id>（或环境变量 TG_IOS_TEAM_ID）注入；
# 不给时在 Xcode 的 Signing & Capabilities 里手动选（重新生成工程会被覆盖，建议走参数）。

set(TG_IOS_TEAM_ID "$ENV{TG_IOS_TEAM_ID}" CACHE STRING "iOS 签名用的开发者 Team ID（10 位）")
set(TG_IOS_BUNDLE_ID "com.tencent.tgcapture.demo" CACHE STRING "demo 的 Bundle ID（个人账号需改成全局唯一）")

set(TG_IOS_DEMO_SOURCES
    Sources/main.mm
    Sources/TGAppDelegate.h
    Sources/TGAppDelegate.mm
    Sources/TGDemoSettings.h
    Sources/TGDemoSettings.mm
    Sources/TGLiveSession.h
    Sources/TGLiveSession.mm
    Sources/TGLiveViewController.h
    Sources/TGLiveViewController.mm)

add_executable(TGCaptureDemo MACOSX_BUNDLE ${TG_IOS_DEMO_SOURCES})
target_link_libraries(TGCaptureDemo PRIVATE tgcapture)
target_compile_options(TGCaptureDemo PRIVATE -fobjc-arc -Wall)
# 内核所需的系统 framework 已由 tgcapture 以 PUBLIC 传递；这里只补 demo 自己用到的（CGRect*、CABasicAnimation）
target_link_libraries(TGCaptureDemo PRIVATE "-framework CoreGraphics" "-framework QuartzCore")

set_target_properties(TGCaptureDemo PROPERTIES
    OUTPUT_NAME "TGCaptureDemo"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${TG_IOS_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "TGCaptureDemo"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    XCODE_GENERATE_SCHEME ON
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${TG_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    XCODE_ATTRIBUTE_ENABLE_BITCODE NO
    # 只有 arm64 真机库：禁止 Xcode 往模拟器 / 其它架构上编
    XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphoneos"
    XCODE_ATTRIBUTE_ARCHS "arm64"
    XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${TG_IOS_TEAM_ID}")

# Xcode 里默认选中 demo 作为运行目标
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY XCODE_STARTUP_PROJECT TGCaptureDemo)
source_group("Sources" FILES ${TG_IOS_DEMO_SOURCES})
