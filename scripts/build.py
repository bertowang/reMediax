#!/usr/bin/env python3
"""TGCapture 构建脚本（python 驱动 cmake）。

用法:
    python scripts/build.py --platform windows [--abi x64] [--clean] [--demo/--no-demo]
    python scripts/build.py --platform android --abi arm64-v8a --ndk <path>
    python scripts/build.py --platform ios
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = PROJECT_ROOT / "build"

PLATFORM_ABI = {
    "windows": "x64",
    "android": "arm64-v8a",
    "ios": "fat",
}


def run(cmd, cwd=None, check=True):
    print("$ " + " ".join(str(c) for c in cmd))
    result = subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None)
    if check and result.returncode != 0:
        sys.exit(result.returncode)
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description="构建 TGCapture")
    parser.add_argument("-p", "--platform", required=True, choices=["windows", "android", "ios"])
    parser.add_argument("--abi", default=None, help="ABI（默认按平台）")
    parser.add_argument("--3rd", dest="third", default=None, help="3rd-rel-Party 根目录")
    parser.add_argument("-t", "--build-type", default="Release")
    parser.add_argument("--clean", action="store_true", help="清理后重新构建")
    parser.add_argument("--no-demo", action="store_true", help="不编译 demo")
    parser.add_argument("--ndk", default=None, help="Android NDK 路径")
    parser.add_argument("--generator", default=None, help="cmake 生成器")
    args = parser.parse_args()

    abi = args.abi or PLATFORM_ABI[args.platform]
    third = args.third or str(PROJECT_ROOT.parent / "3rd-rel-Party")
    build_dir = DEFAULT_BUILD_DIR / f"{args.platform}-{abi}"

    if args.clean and build_dir.exists():
        print(f"清理 {build_dir}")
        shutil.rmtree(build_dir, ignore_errors=True)

    build_dir.mkdir(parents=True, exist_ok=True)

    configure = [
        "cmake",
        "-S", PROJECT_ROOT,
        "-B", build_dir,
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
        f"-DTG_PLATFORM={args.platform}",
        f"-DTG_ABI={abi}",
        f"-DTG_3RD_ROOT={third}",
        f"-DTG_BUILD_DEMO={'OFF' if (args.no_demo or args.platform != 'windows') else 'ON'}",
    ]
    generator = args.generator
    if args.platform == "windows" and not generator:
        # ffmpeg 等依赖由 MSVC 编译，必须使用 MSVC 生成器（ABI 一致）
        generator = "Visual Studio 17 2022"
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
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0",
        ]

    run(configure)
    run(["cmake", "--build", build_dir, "--config", args.build_type, "--parallel"])

    print(f"\n构建完成: {build_dir}")
    if args.platform == "windows" and not args.no_demo:
        exe = build_dir / args.build_type / "tg_capture_demo.exe"
        if not exe.exists():
            exe = build_dir / "tg_capture_demo.exe"
        print(f"demo: {exe if exe.exists() else '(见 build 目录)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
