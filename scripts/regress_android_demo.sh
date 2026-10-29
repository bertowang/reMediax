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
#   --mic              同时开麦克风（原生采集源，自动 pm grant RECORD_AUDIO）
#   --audio-file F     开播到一半时运行期加文件音频源（本机文件，自动 push 到 demo 的外部目录；循环）
#   --switch-res R     开播到一半时运行期切分辨率（R = 档位名如 360p，或 宽x高），校验录像前后两段分辨率
#   --video-file F     主画面改用本地视频文件 F（push 到设备，CreateLocalFileCapture；音轨一并混入）
#   --overlay          叠加层回归：LIVE+2s 暂存 Logo → +4s 提交，校验录像里 Logo 只在提交后出现（暂存不泄漏到推流）
#   --overlay-video F  同上并加画中画（F push 到设备）：+2s 与 Logo 一起暂存，+4s 提交，+6s 移到左下角再提交；
#                      校验 Logo 与画中画同一帧出现（提交原子）、移动前后无中间帧（右下消失与左下出现同一帧）
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
mic=false
switch_res=""
audio_file=""
video_file=""
overlay=0
overlay_video=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --seconds) seconds="$2"; shift ;;
        --format)  format="$2"; shift ;;
        --res)     res="$2"; shift ;;
        --codec)   codec="$2"; shift ;;
        --no-build) build=0 ;;
        --mic)     mic=true ;;
        --switch-res) switch_res="$2"; shift ;;
        --audio-file) audio_file="$2"; shift ;;
        --video-file) video_file="$2"; shift ;;
        --overlay) overlay=1 ;;
        --overlay-video) overlay=1; overlay_video="$2"; shift ;;
        --keep)    ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
    shift
done
if [[ $overlay -eq 1 ]]; then
    [[ -z "$switch_res" && -z "$audio_file" ]] || { echo "--overlay 不能与 --switch-res / --audio-file 同时用" >&2; exit 2; }
    (( seconds >= 10 )) || seconds=10
fi

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
$mic && adb shell pm grant "$pkg" android.permission.RECORD_AUDIO >/dev/null 2>&1

media_dir="/sdcard/Android/data/$pkg/files"
push_media() {   # $1 = 本机文件，$2 = 设备上的文件名；输出设备路径
    [[ -f "$1" ]] || die "找不到 $1"
    adb shell "mkdir -p $media_dir" >/dev/null 2>&1
    adb push "$1" "$media_dir/$2" >/dev/null 2>&1 || die "adb push $1 失败"
    echo "$media_dir/$2"
}
video_source="pattern"
dev_video_file="$media_dir/main.mp4"
dev_overlay_video="$media_dir/pip.mp4"
if [[ -n "$video_file" ]]; then
    video_source="file"
    dev_video_file="$(push_media "$video_file" "regress_main.${video_file##*.}")"
    log "主画面文件 $dev_video_file"
fi
[[ -n "$overlay_video" ]] && dev_overlay_video="$(push_media "$overlay_video" "regress_pip.${overlay_video##*.}")"

prefs=$(cat <<EOF
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="pref_output_mode">file</string>
    <string name="pref_muxer_format">$format</string>
    <string name="pref_video_source">$video_source</string>
    <string name="pref_resolution">$res</string>
    <string name="pref_orientation">landscape</string>
    <string name="pref_fps">30</string>
    <string name="pref_video_codec">$codec</string>
    <boolean name="pref_audio_mic" value="$mic" />
    <boolean name="pref_audio_speaker" value="false" />
    <boolean name="pref_audio_tone" value="true" />
    <string name="pref_video_file">$dev_video_file</string>
    <boolean name="pref_video_file_loop" value="true" />
    <boolean name="pref_video_file_audio" value="true" />
    <string name="pref_overlay_video">$dev_overlay_video</string>
    <boolean name="pref_preview" value="true" />
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
dev_audio_file=""
if [[ -n "$audio_file" ]]; then
    [[ -f "$audio_file" ]] || die "找不到 $audio_file"
    dev_audio_file="/sdcard/Android/data/$pkg/files/regress_$(basename "$audio_file")"
    adb push "$audio_file" "$dev_audio_file" >/dev/null 2>&1 || die "adb push $audio_file 失败"
fi

cmd() { adb shell am start -n "$pkg/.MainActivity" --es tg_cmd "$1" >/dev/null 2>&1; }

if [[ $overlay -eq 1 ]]; then
    sleep 2
    log "暂存 Logo${overlay_video:+ + 画中画}（推流画面此时不应出现）"
    cmd ovl:logo
    [[ -n "$overlay_video" ]] && cmd ovl:pip
    sleep 2
    log "提交"
    cmd ovl:commit
    sleep 2
    if [[ -n "$overlay_video" ]]; then
        log "画中画移到左下角并提交"
        cmd ovl:move
        cmd ovl:commit
    fi
    sleep $(( seconds - 6 ))
    adb logcat -d -s TGDemo:I 2>/dev/null | grep -E "stageOverlay|commitOverlays|overlays |OverlayCommitted|LocalFileEOF" | sed 's/^/  /' || true
elif [[ -n "$switch_res" || -n "$audio_file" ]]; then
    half=$(( seconds / 2 ))
    rest=$(( seconds - half ))
    sleep "$half"
    if [[ -n "$switch_res" ]]; then
        log "运行期切分辨率 -> $switch_res"
        adb shell am start -n "$pkg/.MainActivity" --es tg_cmd "res:$switch_res" >/dev/null 2>&1
    fi
    if [[ -n "$audio_file" ]]; then
        log "运行期加文件音频源 $dev_audio_file"
        adb shell am start -n "$pkg/.MainActivity" --es tg_cmd "audiofile:$dev_audio_file" >/dev/null 2>&1
    fi
    sleep "$rest"
    adb logcat -d -s TGDemo:I TGCapture:I 2>/dev/null \
        | grep -E "switchResolution|运行期切换|VideoMetaChanged|参数集更新|addFileAudio|FileAudioSource|AudioSource" | sed 's/^/  /' || true
else
    sleep "$seconds"
fi

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
adb pull "$records/$newfile" "$out_dir/$newfile" >/dev/null 2>&1 || die "adb pull 失败"
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
if [[ -n "$switch_res" ]]; then
    # 按帧解码出的分辨率序列（去重）：应恰好两段，前一段为开播分辨率
    sizes="$(ffprobe -v error -select_streams v:0 -show_entries frame=width,height -of csv=p=0 "$local_file" | uniq -c | awk '{print $2"("$1")"}' | tr '\n' ' ')"
    log "分辨率序列 $sizes"
    n_seg="$(echo "$sizes" | wc -w | tr -d ' ')"
    [[ "$n_seg" == "2" ]] || { echo "  切换后应恰好两段分辨率"; ok=0; }
    [[ "$sizes" == "${want_w},${want_h}("* ]] || { echo "  第一段应为 ${want_w}x${want_h}"; ok=0; }
fi
if [[ $overlay -eq 1 && "$video_source" != "pattern" ]]; then
    log "主画面不是测试图案，跳过叠加层像素校验"
elif [[ $overlay -eq 1 ]]; then
    # 逐帧取区域均值（signalstats）：Logo 区域看色度（绿底 U 远低于彩条），画中画区域看与首帧的偏差
    region() {   # $1 = crop 表达式，$2 = 输出 json
        ffprobe -v error -f lavfi "movie=$local_file,crop=$1,signalstats" \
            -show_entries frame=pts_time:frame_tags=lavfi.signalstats.YAVG,lavfi.signalstats.UAVG -of json > "$2"
    }
    tmp_dir="$(mktemp -d)"
    region "iw*0.18:ih*0.10:iw*0.75:ih*0.08" "$tmp_dir/logo.json"
    region "iw*0.30:ih*0.30:iw*0.64:ih*0.62" "$tmp_dir/br.json"
    region "iw*0.30:ih*0.30:iw*0.05:ih*0.62" "$tmp_dir/bl.json"
    python3 - "$tmp_dir" "${overlay_video:+pip}" <<'PY' || ok=0
import json, sys
d, pip = sys.argv[1], sys.argv[2] == "pip"
def load(name):
    fr = json.load(open(f"{d}/{name}.json"))["frames"]
    return [(float(f["pts_time"]), float(f["tags"]["lavfi.signalstats.YAVG"]), float(f["tags"]["lavfi.signalstats.UAVG"])) for f in fr]
def segments(flags, times):
    segs = []
    for t, f in zip(times, flags):
        if not segs or segs[-1][0] != f:
            segs.append([f, t])
    return segs
logo = load("logo")
times = [t for t, _, _ in logo]
logo_on = [u < 140 for _, _, u in logo]
seg = segments(logo_on, times)
print("  Logo 区域:", " → ".join(f"{'有' if f else '无'}@{t:.2f}s" for f, t in seg))
good = True
if [f for f, _ in seg] != [False, True]:
    print("  Logo 应恰好一次 无→有（提交后出现且不再闪回）"); good = False
elif seg[1][1] < 3.5:
    print(f"  Logo 在 {seg[1][1]:.2f}s 就出现了：暂存（+2s）不应进推流，提交在 +4s"); good = False
if pip and good:
    def on(name):
        v = load(name); y0, u0 = v[0][1], v[0][2]
        return [abs(y - y0) > 2 or abs(u - u0) > 2 for _, y, u in v]
    br, bl = segments(on("br"), times), segments(on("bl"), times)
    print("  右下:", " → ".join(f"{'有' if f else '无'}@{t:.2f}s" for f, t in br))
    print("  左下:", " → ".join(f"{'有' if f else '无'}@{t:.2f}s" for f, t in bl))
    if [f for f, _ in br] != [False, True, False] or [f for f, _ in bl] != [False, True]:
        print("  画中画应 右下出现 → 移到左下"); good = False
    else:
        if abs(br[1][1] - seg[1][1]) > 0.001:
            print("  Logo 与画中画不是同一帧上推流（提交应原子）"); good = False
        if abs(br[2][1] - bl[1][1]) > 0.001:
            print("  移动时右下消失与左下出现不在同一帧"); good = False
sys.exit(0 if good else 1)
PY
    rm -rf "$tmp_dir"
fi

elapsed=$(( $(date +%s) - t_start ))
if [[ $ok -eq 1 ]]; then
    log "PASS（${elapsed}s）"
else
    die "校验未通过（${elapsed}s）"
fi
