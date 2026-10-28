#!/usr/bin/env bash
# 单元测试入口（L1）：编译并运行 tg_tests，可在一次调用里跑多种方式，最后汇总耗时。
#
#   scripts/test.sh                       # 默认：macOS 本机 + ASan（最快的回归方式）
#   scripts/test.sh --tsan                # macOS 本机 + TSan（Bus / 多线程相关改动必跑）
#   scripts/test.sh --asan --tsan         # 两种都跑
#   scripts/test.sh --plain               # macOS 本机，不开 sanitizer
#   scripts/test.sh --android             # NDK 编 arm64-v8a，adb push 到已连接的设备/模拟器上跑
#   scripts/test.sh --ios-sim             # iphonesimulator(x86_64) 编，xcrun simctl spawn 到已启动的模拟器上跑
#   scripts/test.sh --all                 # asan + tsan + android + ios-sim
#   scripts/test.sh --tsan -- Bus         # "--" 之后的参数透传给 tg_tests（名称过滤）
#
# 环境变量：TG_3RD_ROOT（依赖目录）、ANDROID_SERIAL（多设备时选设备）、ANDROID_NDK_HOME
# 任一方式失败即整体返回非 0，但会先跑完其余方式再汇总。
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

modes=()
filter=()
build_type="Debug"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --asan)     modes+=(asan) ;;
        --tsan)     modes+=(tsan) ;;
        --ubsan)    modes+=(ubsan) ;;
        --plain)    modes+=(plain) ;;
        --android)  modes+=(android) ;;
        --ios-sim)  modes+=(ios-sim) ;;
        --all)      modes+=(asan tsan android ios-sim) ;;
        --release)  build_type="Release" ;;
        -h|--help)  sed -n '2,15p' "$0"; exit 0 ;;
        --)         shift; filter=("$@"); break ;;
        *)          echo "未知参数: $1（--help 查看用法）" >&2; exit 2 ;;
    esac
    shift
done
[[ ${#modes[@]} -eq 0 ]] && modes=(asan)

host_abi="$( [[ "$(uname -m)" == "arm64" ]] && echo arm64 || echo x86_64 )"

now() { python3 -c 'import time; print(time.time())'; }

# 与 build.py 的 _default_ndk 一致：ANDROID_NDK_HOME > ~/Library/Android/sdk/ndk 下最新版本
find_ndk() {
    if [[ -n "${ANDROID_NDK_HOME:-}" ]]; then echo "$ANDROID_NDK_HOME"; return; fi
    local sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
    ls -d "$sdk"/ndk/*/ 2>/dev/null | sort -V | tail -1 | sed 's#/$##'
}

run_macos() {   # $1 = sanitizer（空 = 不开）
    local san="$1" dir="build/macos-${host_abi}" args=(--platform macos -t "$build_type")
    if [[ -n "$san" ]]; then
        args+=(--sanitize "$san")
        # 与 build.py 的构建目录后缀一致（macOS 自带 bash 3.2 不能在 $(...) 里写 case）
        if   [[ "$san" == "address" ]]; then dir+="-asan"
        elif [[ "$san" == "thread" ]];  then dir+="-tsan"
        else dir+="-ubsan"; fi
    fi
    mkdir -p build
    python3 scripts/build.py "${args[@]}" > "$dir.build.log" 2>&1 \
        || { echo "  构建失败，见 $dir.build.log"; tail -20 "$dir.build.log"; return 1; }
    # halt_on_error：TSan 默认只报不停，这里要求一报就失败；second_deadlock_stack 便于定位锁序问题
    ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
    TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
        "$dir/tg_tests" "${filter[@]+"${filter[@]}"}"
}

run_android() {
    command -v adb >/dev/null || { echo "  找不到 adb"; return 1; }
    adb get-state >/dev/null 2>&1 || { echo "  没有已连接的 Android 设备/模拟器"; return 1; }
    local abi ndk libcxx dir="build/android-arm64-v8a"
    abi="$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
    [[ "$abi" == "arm64-v8a" ]] || dir="build/android-$abi"
    python3 scripts/build.py --platform android --abi "$abi" -t "$build_type" > "$dir.build.log" 2>&1 \
        || { echo "  构建失败，见 $dir.build.log"; tail -20 "$dir.build.log"; return 1; }
    ndk="$(find_ndk)"
    local triple
    case "$abi" in
        arm64-v8a)   triple=aarch64-linux-android ;;
        armeabi-v7a) triple=arm-linux-androideabi ;;
        x86_64)      triple=x86_64-linux-android ;;
        x86)         triple=i686-linux-android ;;
    esac
    libcxx="$(ls "$ndk"/toolchains/llvm/prebuilt/*/sysroot/usr/lib/$triple/libc++_shared.so 2>/dev/null | head -1)"
    [[ -f "$libcxx" ]] || { echo "  NDK 里找不到 libc++_shared.so（$ndk）"; return 1; }
    adb push "$dir/tg_tests" "$libcxx" /data/local/tmp/ >/dev/null || return 1
    local q=""
    for f in "${filter[@]+"${filter[@]}"}"; do q+=" '$f'"; done
    # adb shell 透传退出码（shell 协议 v2）
    adb shell "cd /data/local/tmp && chmod 755 tg_tests && LD_LIBRARY_PATH=. ./tg_tests$q"
}

run_ios_sim() {
    local dir="build/ios-sim-x86_64" udid
    python3 scripts/build.py --platform ios --sim -t "$build_type" > "$dir.build.log" 2>&1 \
        || { echo "  构建失败，见 $dir.build.log（模拟器需要 TGCapture3rd 的 iOS fat 库含 x86_64）"; tail -20 "$dir.build.log"; return 1; }
    udid="$(xcrun simctl list devices booted | grep -Eo '\([0-9A-F-]{36}\)' | head -1 | tr -d '()')"
    if [[ -z "$udid" ]]; then
        udid="$(xcrun simctl list devices available | grep -E 'iPhone' | grep -Eo '\([0-9A-F-]{36}\)' | head -1 | tr -d '()')"
        [[ -n "$udid" ]] || { echo "  没有可用的 iOS 模拟器"; return 1; }
        echo "  启动模拟器 $udid"
        xcrun simctl boot "$udid" || return 1
    fi
    # iOS 下 CMake 把可执行文件默认打成 .app 包
    local exe="$root/$dir/tg_tests.app/tg_tests"
    [[ -x "$exe" ]] || exe="$root/$dir/tg_tests"
    xcrun simctl spawn "$udid" "$exe" "${filter[@]+"${filter[@]}"}"
}

summary=()
failed=0
for m in "${modes[@]}"; do
    echo "================ tg_tests [$m] ================"
    t0="$(now)"
    case "$m" in
        asan)    run_macos address ;;
        tsan)    run_macos thread ;;
        ubsan)   run_macos undefined ;;
        plain)   run_macos "" ;;
        android) run_android ;;
        ios-sim) run_ios_sim ;;
    esac
    rc=$?
    t1="$(now)"
    secs="$(python3 -c "print(f'{$t1-$t0:.1f}')")"
    if [[ $rc -eq 0 ]]; then summary+=("  [PASS] $m  ${secs}s"); else summary+=("  [FAIL] $m  ${secs}s (rc=$rc)"); failed=1; fi
done

echo "================ 汇总（含编译） ================"
printf '%s\n' "${summary[@]}"
exit $failed
