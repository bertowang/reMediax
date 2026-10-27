#!/usr/bin/env bash
# CI 入口（与具体 CI 平台无关：GitHub Actions / GitLab / 蓝盾 里直接调这一个脚本）。
#
#   依赖目录：环境变量 TG_3RD_ROOT 或工程同级的 TGCapture3rd（见 README 9.3）
#   TG_CI_ACCEPT=1  额外跑音画同步验收（需要素材：TG_MEDIA_DIR，以及系统 ffmpeg/ffprobe）
#
# 步骤：Linux 构建（同时链接 demo，等于 ffmpeg 依赖链自检）→ 单元测试 → 可选验收。任一步失败即非 0。
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

python3 scripts/build.py --platform linux
bin_dir="$(dirname "$(find build -name tg_tests -type f | head -1)")"
"$bin_dir/tg_tests"

if [[ "${TG_CI_ACCEPT:-0}" == "1" ]]; then
    scripts/accept_av.sh "$bin_dir/tg_capture_demo"
fi
