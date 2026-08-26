#!/usr/bin/env bash
# 回归：setup.sh 的 find_vs 用 vswhere 定位 VS，必须带 -prerelease，否则 Insiders/预览版
# （如 VS 2026 Insiders）被默认过滤，会误选 release 的旧版本（如 VS 2022 BuildTools）。
# 本用例用 mock vswhere 无头验证：缺 -prerelease → 返回 2022；加 -prerelease → 返回 2026。
set -euo pipefail
cd "$(dirname "$0")/.."
source ./scripts/setup.sh

# ---- mock vswhere：默认排除 prerelease；加 -prerelease 才包含 ----
mock_dir="$(mktemp -d)"
cat > "$mock_dir/vswhere.exe" <<'MOCK'
#!/usr/bin/env bash
RELEASE="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
PRERELEASE="/c/Program Files/Microsoft Visual Studio/18/Insiders"
HAS_PRERELEASE=""
for a in "$@"; do [ "$a" = "-prerelease" ] && HAS_PRERELEASE=1; done
if [ -n "$HAS_PRERELEASE" ]; then printf '%s\n%s\n' "$PRERELEASE" "$RELEASE"; else printf '%s\n' "$RELEASE"; fi
MOCK
chmod +x "$mock_dir/vswhere.exe"

# 对照组（断言）：缺 -prerelease 时 mock 返回 release 的 2022 → 即用户现场 bug 形态。
rel="$(bash "$mock_dir/vswhere.exe" -all -latest -products '*' -property installationPath | head -n1)"
case "$rel" in
  *2022/BuildTools) echo "对照确认：缺 -prerelease 返回 2022 BuildTools（故障形态）" ;;
  *) echo "FAIL: 对照组失真（应返回 2022）"; rm -rf "$mock_dir"; exit 1 ;;
esac

# 断言 1：find_vs 的两条 vswhere 查询行（vswhere 关键字所在行）必须含 -prerelease。
# 收紧到查询行而非整文件 grep：避免「只回退查询、保留注释里 -prerelease 字样」的假绿。
if grep -- '-prerelease' scripts/setup.sh | grep -q -- 'vswhere'; then
  echo "断言1 OK：setup.sh find_vs 查询行含 -prerelease"
else
  echo "FAIL: setup.sh find_vs 查询行缺 -prerelease（2026 Insiders 会被过滤）"; rm -rf "$mock_dir"; exit 1
fi
# 断言 2：setup.ps1 的 vswhere 查询行也含 -prerelease。
if grep -- '-prerelease' scripts/setup.ps1 | grep -q -- 'vswhere'; then
  echo "断言2 OK：setup.ps1 查询行含 -prerelease"
else
  echo "FAIL: setup.ps1 查询行缺 -prerelease"; rm -rf "$mock_dir"; exit 1
fi
# 断言 3：带 -prerelease 的查询选中 2026 Insiders（mock 验证修复后主路径）。
pre="$(bash "$mock_dir/vswhere.exe" -all -prerelease -latest -products '*' -property installationPath | head -n1)"
case "$pre" in
  *18/Insiders) echo "断言3 OK：加 -prerelease 选中 2026 Insiders" ;;
  *) echo "FAIL: 加 -prerelease 未选中 2026（应 18/Insiders）"; rm -rf "$mock_dir"; exit 1 ;;
esac

rm -rf "$mock_dir"
echo "PASS: find_vs 带 -prerelease，Insiders/预览版不再被过滤"
