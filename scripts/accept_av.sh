#!/usr/bin/env bash
# 音画同步验收（Linux demo 外灌帧，不依赖采集设备）。
#
#   用法: scripts/accept_av.sh [demo 可执行文件] [输出目录]
#   依赖: 系统 ffmpeg / ffprobe；素材默认取 $TG_MEDIA_DIR（含 dance-girl.n.mp4 / iqiyi.ts）
#   退出码: 0 全部通过；非 0 为失败场景数
#
# 场景（每个都是 demo 的一组故障注入参数，对应 README「音画同步」里的验收表）：
#   A  基线：两路音频 + 视频均按标称速度
#   B  两路音频一快一慢（0.95x / 1.05x）：混流样本域补偿
#   C  音频 PTS 随机抖动 ±20ms（采集端时间戳不准）
#   D  音频 PTS 周期性回退（设备时钟回退）
#   E  视频卡顿后积压帧一次性灌入（burst）
#   E2 视频卡顿期间帧丢失（freeze）
#
# 通过标准（所有场景共用）：
#   - 视频相邻 PTS 间隔 < 10ms 的次数 == 0（回归：旧实现压缩 PTS 会挤出这种间隔）
#   - 音频相邻包间隔异常（<16ms 或 >30ms）次数 == 0（音频时间轴严格线性）
#   - 音视频末尾 PTS 差 < 300ms
#   - 视频有产出，且时长不短于 seconds-2
#   - E / E2 额外要求：存在且仅存在一次 >100ms 的视频间隔（卡顿本身），恢复后不再出现
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
demo="${1:-$(find "$root/build" -name tg_capture_demo -type f 2>/dev/null | head -1)}"
out="${2:-$(mktemp -d /tmp/tg_accept.XXXXXX)}"
media="${TG_MEDIA_DIR:-/data/home/bertonwang/mov}"
seconds="${TG_ACCEPT_SECONDS:-20}"

if [[ -z "$demo" || ! -x "$demo" ]]; then
    echo "找不到 tg_capture_demo，先执行: python3 scripts/build.py --platform linux" >&2
    exit 2
fi
for f in "$media/dance-girl.n.mp4" "$media/iqiyi.ts"; do
    [[ -f "$f" ]] || { echo "缺少素材: $f（可用 TG_MEDIA_DIR 指定目录）" >&2; exit 2; }
done
command -v ffprobe >/dev/null || { echo "需要 ffprobe" >&2; exit 2; }
mkdir -p "$out"

common=(--video "$media/dance-girl.n.mp4" --audio1 "$media/dance-girl.n.mp4"
        --audio2 "$media/iqiyi.ts" --audio2-volume 0.1 --seconds "$seconds"
        --video-loops 0 --audio1-loops 0 --audio2-loops 0)

declare -A extra=(
    [A]=""
    [B]="--audio1-speed 0.95 --audio2-speed 1.05"
    [C]="--audio-pts-jitter-ms 20"
    [D]="--audio-pts-back-every 50"
    [E]="--video-stall-start 8 --video-stall-dur 4 --video-stall-mode burst"
    [E2]="--video-stall-start 8 --video-stall-dur 4 --video-stall-mode freeze"
)
order=(A B C D E E2)

# 场景并行跑（互相独立，各自一个进程和输出文件）
for n in "${order[@]}"; do
    # shellcheck disable=SC2086
    "$demo" "${common[@]}" ${extra[$n]} --out "$out/$n.flv" >"$out/$n.log" 2>&1 &
done
wait

fails=0
check() {   # check <场景> <描述> <条件结果 0/1>
    if [[ "$3" == "1" ]]; then
        echo "    ok   $2"
    else
        echo "    FAIL $2"
        fails=$((fails + 1))
    fi
}

for n in "${order[@]}"; do
    f="$out/$n.flv"
    echo "[$n] ${extra[$n]:-（基线）}"
    if [[ ! -s "$f" ]]; then
        check "$n" "产出文件存在" 0
        continue
    fi
    ffprobe -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 "$f" | sort -g >"$out/$n.vpts"
    ffprobe -v error -select_streams a:0 -show_entries packet=pts_time -of csv=p=0 "$f" >"$out/$n.apts"

    vcount=$(wc -l <"$out/$n.vpts")
    small=$(awk 'NR>1{if(($1-p)*1000<10)c++} {p=$1} END{print c+0}' "$out/$n.vpts")
    big=$(awk 'NR>1{if(($1-p)*1000>100)c++} {p=$1} END{print c+0}' "$out/$n.vpts")
    agap=$(awk 'NR>1{d=($1-p)*1000; if(d>30||d<16)c++} {p=$1} END{print c+0}' "$out/$n.apts")
    vlast=$(tail -n1 "$out/$n.vpts")
    alast=$(tail -n1 "$out/$n.apts")
    skew=$(awk -v a="$alast" -v v="$vlast" 'BEGIN{d=(a-v)*1000; if(d<0)d=-d; printf "%d", d}')

    echo "    视频包=$vcount 视频<10ms间隔=$small 视频>100ms间隔=$big 音频异常间隔=$agap 末尾差=${skew}ms"
    check "$n" "视频有产出且时长足够" "$(awk -v v="$vlast" -v s="$seconds" 'BEGIN{print (v>=s-2)?1:0}')"
    check "$n" "视频无 <10ms 的 PTS 间隔" "$([[ $small -eq 0 ]] && echo 1 || echo 0)"
    check "$n" "音频 PTS 间隔均匀" "$([[ $agap -eq 0 ]] && echo 1 || echo 0)"
    check "$n" "音视频末尾 PTS 差 < 300ms" "$([[ $skew -lt 300 ]] && echo 1 || echo 0)"
    if [[ "$n" == E || "$n" == E2 ]]; then
        check "$n" "卡顿只造成 1 次 >100ms 间隔" "$([[ $big -eq 1 ]] && echo 1 || echo 0)"
    else
        check "$n" "无 >100ms 的视频间隔" "$([[ $big -eq 0 ]] && echo 1 || echo 0)"
    fi
done

echo
if [[ $fails -eq 0 ]]; then
    echo "验收通过（输出目录: $out）"
else
    echo "验收失败: $fails 项（输出目录: $out）"
fi
exit "$fails"
