#!/usr/bin/env python3
"""TGCapture 构建脚本（python 驱动 cmake）。

用法:
    python scripts/build.py --platform windows [--abi x64] [--clean] [--no-demo] [--3rd <依赖目录>]
    python scripts/build.py --platform android --abi arm64-v8a --ndk <path>
    python scripts/build.py --platform ios

    # 只生成 IDE 工程并打开（VS2022 / Android Studio / Xcode），不编译
    python scripts/build.py --platform windows --ide
    python scripts/build.py --platform android --ide
    python scripts/build.py --platform ios --ide [--team <Team ID>]   # 含 demo/ios 开播测试 App

依赖目录（3rd）不给时由 CMake 解析：环境变量 TG_3RD_ROOT > 工程同级 TGCapture3rd。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = PROJECT_ROOT / "build"

PLATFORM_ABI = {
    "windows": "x64",
    "linux": "x86_64",
    "android": "arm64-v8a",
    "ios": "fat",
}

# 依赖根目录不在本脚本里猜：默认值统一由 CMakeLists.txt 解析（--3rd > $TG_3RD_ROOT > 同级探测），
# 避免出现"脚本和 CMake 两套默认值"的漂移。本脚本只在显式给出 --3rd 时透传。


def run(cmd, cwd=None, check=True):
    print("$ " + " ".join(str(c) for c in cmd))
    result = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None)
    if check and result.returncode != 0:
        sys.exit(result.returncode)
    return result.returncode


def open_in_ide(platform: str, build_dir: Path) -> None:
    """生成完工程后用对应 IDE 打开（只负责打开，编译交给 IDE）。"""
    if platform == "windows":
        sln = build_dir / "TGCapture.sln"
        if sln.exists():
            print(f"用 VS2022 打开: {sln}")
            os.startfile(str(sln))
        else:
            print(f"未找到 {sln}，请确认生成器是 Visual Studio 17 2022")
    elif platform == "ios":
        xcodeproj = build_dir / "TGCapture.xcodeproj"
        if xcodeproj.exists():
            print(f"用 Xcode 打开: {xcodeproj}")
            subprocess.run(["open", str(xcodeproj)], check=False)
        else:
            print(f"未找到 {xcodeproj}，请确认生成器是 Xcode")
    elif platform == "android":
        host = PROJECT_ROOT / "ide" / "android"
        print("用 Android Studio 打开（File > Open）: " + str(host))
        print("  模块 :tgcapture = bindings/android（SDK/AAR），:demo = demo/android（开播测试 App）；")
        print("  Native 构建由 bindings/android/build.gradle.kts 的 externalNativeBuild 驱动。")
        if sys.platform.startswith("win"):
            os.startfile(str(host))
        elif sys.platform == "darwin":
            subprocess.run(["open", str(host)], check=False)


def main() -> int:
    parser = argparse.ArgumentParser(description="构建 TGCapture")
    parser.add_argument("-p", "--platform", required=True, choices=["windows", "linux", "android", "ios"])
    parser.add_argument("--abi", default=None, help="ABI（默认按平台）")
    parser.add_argument("--3rd", dest="third", default=None,
                        help="依赖根目录；不给则由 CMake 解析（环境变量 TG_3RD_ROOT > 同级 TGCapture3rd）")
    parser.add_argument("-t", "--build-type", default="Release")
    parser.add_argument("--clean", action="store_true", help="清理后重新构建")
    parser.add_argument("--no-demo", action="store_true", help="不编译 demo")
    parser.add_argument("--ndk", default=None, help="Android NDK 路径")
    parser.add_argument("--generator", default=None, help="cmake 生成器")
    parser.add_argument("--ide", action="store_true", help="只生成 IDE 工程并打开，不编译")
    parser.add_argument("--team", default=None,
                        help="iOS 签名用的开发者 Team ID（写入 demo 工程；也可用环境变量 TG_IOS_TEAM_ID）")
    parser.add_argument("--bundle-id", default=None,
                        help="iOS demo 的 Bundle ID（默认 com.tencent.tgcapture.demo；个人账号需改成唯一值）")
    args = parser.parse_args()

    abi = args.abi or PLATFORM_ABI[args.platform]
    build_dir = DEFAULT_BUILD_DIR / f"{args.platform}-{abi}"
    if args.platform == "ios" and (args.ide or args.generator == "Xcode"):
        # Xcode 工程与命令行构建（Makefiles）分目录：同一目录换生成器 CMake 会直接报错
        build_dir = DEFAULT_BUILD_DIR / f"{args.platform}-{abi}-xcode"

    if args.clean and build_dir.exists():
        print(f"清理 {build_dir}")
        shutil.rmtree(build_dir, ignore_errors=True)

    build_dir.mkdir(parents=True, exist_ok=True)

    # demo：windows / linux 为命令行程序，ios 为 App（demo/ios）；android 的 demo 由 Gradle 构建
    with_demo = not args.no_demo and args.platform in ("windows", "linux", "ios")
    configure = [
        "cmake",
        "-S", PROJECT_ROOT,
        "-B", build_dir,
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
        f"-DTG_PLATFORM={args.platform}",
        f"-DTG_ABI={abi}",
        f"-DTG_BUILD_DEMO={'ON' if with_demo else 'OFF'}",
    ]
    if args.third:
        # 显式指定才透传；不给时由 CMakeLists.txt 按 环境变量 > 同级目录 解析
        configure.append(f"-DTG_3RD_ROOT={args.third}")
    if args.team:
        configure.append(f"-DTG_IOS_TEAM_ID={args.team}")
    if args.bundle_id:
        configure.append(f"-DTG_IOS_BUNDLE_ID={args.bundle_id}")
    generator = args.generator
    if args.platform == "windows" and not generator:
        # ffmpeg 等依赖由 MSVC 编译，必须使用 MSVC 生成器（ABI 一致）
        generator = "Visual Studio 17 2022"
    if args.ide and args.platform == "ios" and not generator:
        generator = "Xcode"   # --ide 时 iOS 用 Xcode 工程，命令行构建仍用默认生成器
    if generator:
        configure += ["-G", generator]
        if generator.startswith("Visual Studio"):
            configure += ["-A", "x64"]

    if args.platform == "android":
        if not args.ndk:
            print("提示: 未指定 --ndk，将使用环境变量 ANDROID_NDK_HOME")
        toolchain = None
        if args.ndk:
            toolchain = Path(args.ndk) / "build" / "cmake" / "android.toolchain.cmake"
        if toolchain and toolchain.exists():
            configure += [f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DANDROID_ABI={abi}"]
        else:
            print("警告: 未找到 android.toolchain.cmake，请确认 NDK 路径")

    if args.platform == "ios":
        configure += [
            "-DCMAKE_SYSTEM_NAME=iOS",
            # 不给 sysroot 时 CMake 会按宿主的 macOS SDK 推断成 macabi，
            # 编译器直接报 invalid version number in '--target=arm64-apple-ios12.0-macabi'
            "-DCMAKE_OSX_SYSROOT=iphoneos",
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            # 12.0 编不过：libc++ 的 <filesystem> 要 iOS 13.0、std::jthread/stop_token 要 iOS 14.0
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0",
        ]

    if args.ide and args.platform == "android":
        # Android Studio 通过 gradle 自己驱动 CMake，这里不重复 configure（也不强求 NDK 就位）
        open_in_ide("android", build_dir)
        return 0

    run(configure)
    if args.ide:
        open_in_ide(args.platform, build_dir)
        return 0

    run(["cmake", "--build", build_dir, "--config", args.build_type, "--parallel"])

    print(f"\n构建完成: {build_dir}")
    if args.platform in ("windows", "linux") and not args.no_demo:
        name = "tg_capture_demo.exe" if args.platform == "windows" else "tg_capture_demo"
        exe = build_dir / args.build_type / name
        if not exe.exists():
            exe = build_dir / name
        print(f"demo: {exe if exe.exists() else '(见 build 目录)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
