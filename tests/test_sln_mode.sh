#!/usr/bin/env bash
# 回归：setup.sh --sln 模式（VS 解决方案生成）—— 无头可执行。
# 断言：生成器映射 / 兜底路径版本提取 / Linux 下 --sln 立即拒绝（不触发网络/探测）。
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/setup.sh   # 主 guard 保证不自动跑 main
fail() { echo "FAIL: $1"; exit 1; }
pass() { echo "PASS: $1"; }

# 1) 生成器映射
[ "$(vs_generator_name '18.14.36231.0')" = "Visual Studio 18 2026" ] || fail "gen 18→2026"
[ "$(vs_generator_name '17.8.0')" = "Visual Studio 17 2022" ] || fail "gen 17→2022"
[ "$(vs_generator_name '16.11.0')" = "Visual Studio 16 2019" ] || fail "gen 16→2019"
[ "$(vs_generator_name '2026')" = "Visual Studio 18 2026" ] || fail "gen 2026→18"
[ "$(vs_generator_name '2022')" = "Visual Studio 17 2022" ] || fail "gen 2022→17"
if vs_generator_name "15.9" >/dev/null 2>&1; then fail "gen 15 应失败"; fi
if vs_generator_name "" >/dev/null 2>&1; then fail "gen 空应失败"; fi
pass "生成器映射"

# 2) 兜底路径版本提取（版本段在 SKU 上一级）
[ "$(vs_version_from_path '/c/Program Files/Microsoft Visual Studio/18/Insiders')" = "18" ] || fail "path 18"
[ "$(vs_version_from_path '/c/Program Files/Microsoft Visual Studio/2022/Community')" = "2022" ] || fail "path 2022"
pass "兜底路径版本提取"

# 3) Linux 下 --sln 立即拒绝（不触发网络/探测）
set +e
out="$(main --sln 2>&1)"; rc=$?
set -e
[ "$rc" -ne 0 ] || fail "--sln 在 Linux 应非零退出"
case "$out" in *sln*|*Windows*) : ;; *) fail "--sln 拒绝信息未含 sln/Windows：$out" ;; esac
pass "Linux --sln 拒绝"

echo "ALL PASS"
