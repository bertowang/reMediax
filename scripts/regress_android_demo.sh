#!/usr/bin/env bash
# Android demo 端到端回归（L3）：装包 → adb 写设置 → 开播 → 停播 → 拉回录像 → ffprobe 校验。
#
#   scripts/regress_android_demo.sh                    # 默认：测试图案 + 测试音，720p flv，录 8s
#   scripts/regress_android_demo.sh --format mpegts --res 1080 --seconds 15
#   scripts/regress_android_demo.sh --no-build         # 跳过 gradle 装包（已装好时更快）
#
# 选项：
#   --seconds N        开播时长，默认 8
#   --format flv|mpegts  封装格式，默认 flv
#   --res 540|720|1080 分辨率档位（横屏），默认 720
#   --codec <name>     视频编码器，默认 h264_mediacodec（模拟器不支持时内核会回退 libopenh264）
#   --no-build         不重新编译 / 安装 demo
#   --keep             保留拉回的录像（默认也保留，路径打印在最后）
#
# 依赖：adb、ffprobe（PATH 里）；Gradle 需要 JDK 17+（自动尝试 Android Studio 自带的 JBR）。
# 录屏/内录要系统授权弹窗，这里固定用"测试图案 + 测试音"，不需要任何交互。
# 自动化入口见 demo/android MainActivity.handleAutomation（只在 debuggable 包里生效）。
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
pkg="com.tencent.tgcapture.demo"
seconds=8
format="flv"
res="720"
codec="h264_mediacodec"
build=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --seconds) seconds="$2"; shift ;;
        --format)  format="$2"; shift ;;
        --res)     res="$2"; shift ;;
        --codec)   codec="$2"; shift ;;
        --no-build) build=0 ;;
        --keep)    ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
    shift
done

case "$res" in
    540)  want_w=960;  want_h=540 ;;
    720)  want_w=1280; want_h=720 ;;
    1080) want_w=1920; want_h=1080 ;;
    *) echo "--res 只支持 540/720/1080" >&2; exit 2 ;;
esac
ext="$( [[ "$format" == "mpegts" ]] && echo ts || echo flv )"

die() { echo "[regress] 失败: $*" >&2; exit 1; }
log() { echo "[regress] $*"; }
t_start=$(date +%s)

adb get-state >/dev/null 2>&1 || die "没有已连接的 Android 设备/模拟器"
command -v ffprobe >/dev/null || die "PATH 里找不到 ffprobe"

# ---------------------------------------------------------------- 1. 装包
if [[ $build -eq 1 ]]; then
    # gradlew 优先用 JAVA_HOME（不是 PATH 里的 java），AGP 8 要 17+
    java_bin="${JAVA_HOME:+$JAVA_HOME/bin/}java"
    if ! "$java_bin" -version 2>&1 | grep -qE 'version "(1[7-9]|[2-9][0-9])'; then
        jbr="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
        [[ -d "$jbr" ]] && export JAVA_HOME="$jbr"
    fi
    log "gradle :demo:installDebug"
    (cd "$root/ide/android" && ./gradlew -q :demo:installDebug) || die "gradle 装包失败"
fi

# ---------------------------------------------------------------- 2. 写设置（应用必须先停掉，否则退出时会把内存里的设置写回去）
adb shell am force-stop "$pkg"
adb shell pm grant "$pkg" android.permission.POST_NOTIFICATIONS >/dev/null 2>&1 || true
prefs=$(cat <<EOF
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="pref_output_mode">file</string>
    <string name="pref_muxer_format">$format</string>
    <string name="pref_video_source">pattern</string>
    <string name="pref_resolution">$res</string>
    <string name="pref_orientation">landscape</string>
    <string name="pref_fps">30</string>
    <string name="pref_video_codec">$codec</string>
    <boolean name="pref_audio_mic" value="false" />
    <boolean name="pref_audio_speaker" value="false" />
    <boolean name="pref_audio_tone" value="true" />
</map>
EOF
)
echo "$prefs" | adb shell "run-as $pkg sh -c 'mkdir -p shared_prefs && cat > shared_prefs/${pkg}_preferences.xml'" \
    || die "写设置失败（需要 debuggable 包）"

# ---------------------------------------------------------------- 3. 开播 / 停播
wait_state() {   # $1 = 状态名，$2 = 超时秒数
    local deadline=$(( $(date +%s) + $2 ))
    while (( $(date +%s) < deadline )); do
        adb logcat -d -s TGDemo:I 2>/dev/null | grep -q "state=$1" && return 0
        sleep 0.5
    done
    return 1
}

records="/sdcard/Android/data/$pkg/files/records"
before="$(adb shell "ls $records 2>/dev/null" | tr -d '\r' | sort)"

adb logcat -c
adb shell am start -n "$pkg/.MainActivity" >/dev/null
wait_state IDLE 15 || die "demo 没有启动（logcat 里没等到 state=IDLE）"
sleep 1   # 等编码器探测等首屏工作结束，避免与开播抢 worker

adb logcat -c
log "开播 ${want_w}x${want_h} $format ${seconds}s"
adb shell am start -n "$pkg/.MainActivity" --es tg_cmd start >/dev/null 2>&1
if ! wait_state LIVE 20; then
    adb logcat -d -s TGDemo:I | tail -30
    die "20s 内没有进入 LIVE"
fi
sleep "$seconds"

adb logcat -c
log "停播"
adb shell am start -n "$pkg/.MainActivity" --es tg_cmd stop >/dev/null 2>&1
if ! wait_state IDLE 20; then
    adb logcat -d -s TGDemo:I | tail -30
    die "20s 内没有回到 IDLE"
fi

# ---------------------------------------------------------------- 4. 拉回并校验
after="$(adb shell "ls $records 2>/dev/null" | tr -d '\r' | sort)"
newfile="$(comm -13 <(echo "$before") <(echo "$after") | grep "\.$ext$" | tail -1 || true)"
[[ -n "$newfile" ]] || die "$records 下没有新录像"
out_dir="$root/build/regress"
mkdir -p "$out_dir"
adb pull "$records/$newfile" "$out_dir/$newfile" >/dev/null || die "adb pull 失败"
local_file="$out_dir/$newfile"

probe() { ffprobe -v error -select_streams "$1" -show_entries "$2" -of default=nw=1:nk=1 "$local_file" | head -1; }
v_codec="$(probe v:0 stream=codec_name)"
v_w="$(probe v:0 stream=width)"
v_h="$(probe v:0 stream=height)"
a_codec="$(probe a:0 stream=codec_name)"
dur="$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$local_file")"
# 解码一遍：能读完且无错误
dec_err="$(ffprobe -v error -show_frames -of compact "$local_file" 2>&1 >/dev/null | head -5)"

log "文件 $local_file"
log "视频 $v_codec ${v_w}x${v_h}  音频 $a_codec  时长 ${dur}s"
ok=1
[[ "$v_codec" == "h264" ]] || { echo "  视频编码应为 h264"; ok=0; }
[[ "$v_w" == "$want_w" && "$v_h" == "$want_h" ]] || { echo "  分辨率应为 ${want_w}x${want_h}"; ok=0; }
[[ "$a_codec" == "aac" ]] || { echo "  音频编码应为 aac"; ok=0; }
python3 -c "import sys; sys.exit(0 if float('${dur:-0}') >= $seconds - 2 else 1)" \
    || { echo "  时长应 ≥ $((seconds - 2))s"; ok=0; }
[[ -z "$dec_err" ]] || { echo "  解码报错: $dec_err"; ok=0; }

elapsed=$(( $(date +%s) - t_start ))
if [[ $ok -eq 1 ]]; then
    log "PASS（${elapsed}s）"
else
    die "校验未通过（${elapsed}s）"
fi
