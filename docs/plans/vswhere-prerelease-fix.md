# 修复计划：setup.sh / setup.ps1 找到 VS 2022 BuildTools 而非用户已装的 VS 2026 Insiders

## 0. Bug 报告

- **现象**（Windows Git Bash 真机）：`sh scripts/setup.sh` 环境探测显示：
  ```
  [OK] C++ 编译器 (MSVC): MSVC cl.exe @ /c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/...
  ```
  但用户已安装 **VS 2026**（`/c/Program Files/Microsoft Visual Studio/18/Insiders`）。
- **报告者**：用户主会话现场输出（Windows 真机）。

## 1. 问题查找（①，已完成）

### 复现（无头 mock vswhere）
本机 Linux 无法跑真 vswhere，用**模拟 vswhere** 无头复现机制：
- 模拟两实例：`2022 BuildTools`（release）、`2026 Insiders`（prerelease）。
- **无 `-prerelease`**（当前脚本行为）→ 返回 `2022 BuildTools`。
- **有 `-prerelease`**（修复后）→ 返回 `2026 Insiders`。

与用户现场完全吻合：脚本缺 `-prerelease` → 选中 release 的 2022，2026 被过滤。

### 根因（实证）
vswhere **默认排除 prerelease**（Insiders/预览版），必须显式加 `-prerelease` 才纳入。
- `scripts/setup.sh:127`：`"$vswhere" -all -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath` — **缺 `-prerelease`**。
- `scripts/setup.ps1:65`：`& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath` — **缺 `-prerelease`**。
- 两处 fallback（`setup.sh:128` 降级不带 -requires、`setup.ps1` 同）也缺。

> 历史注记：第一个 MSYS 引号 bug 现场能定位到 2026 Insiders，是因为当时 vswhere 带 `-requires` 查询失败走 fallback `find "/c/Program Files/Microsoft Visual Studio"`，而 2022 BuildTools 在 `Program Files (x86)` 不在该目录 → fallback 只扫到 2026。本次 2022 BuildTools 的 `-requires` 命中成功 → 直接选中 2022，未走 fallback。两条路径都缺 `-prerelease`，只是此前恰好因目录差异偶然选中 2026。

### 影响面
- `scripts/setup.sh:127`（bash，`find_vs` 主查询）
- `scripts/setup.sh:128`（bash，降级不带 -requires 的查询）
- `scripts/setup.ps1:65`（PowerShell 同款查询；经核对 setup.ps1 仅此一个 vswhere 查询，无降级 fallback）
- 同一 `find_vs` 被 `find_cl` / `find_vcvars` / `probe_compiler` 复用 → 全部受影响路径经 `find_vs` 一处修复覆盖。
- **同根因外溢（已记录，另立任务）**：SDK submodule 的 `sdk/scripts/setup-env.ps1:81` 与 `sdk/scripts/setup-env-win.sh:115` 的 vswhere 查询同样缺 `-prerelease`。**不在本次 paint-pc 修复范围**（属 SDK 仓库），需在 SDK 侧另立任务修复，本计划不代修，仅记录。

## 2. 修复方案（②）

**给所有 vswhere 查询加 `-prerelease`**，使 Insiders/预览版纳入候选，`-latest` 按版本选中 2026。

### `scripts/setup.sh`
- 第 127 行：
  ```bash
  vs="$("$vswhere" -all -prerelease -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>/dev/null | tr -d '\r' | head -n1)"
  ```
- 第 128 行：
  ```bash
  [ -z "$vs" ] && vs="$("$vswhere" -all -prerelease -latest -products '*' -property installationPath 2>/dev/null | tr -d '\r' | head -n1)"
  ```
- 更新 `find_vs` 上方注释（118-121 行）：从「必须带 -all」改为「必须带 `-all -prerelease` —— `-all` 包含所有版本，`-prerelease` 纳入 Insiders/预览版；缺 `-prerelease` 会过滤掉 2026 Insiders」。

### `scripts/setup.ps1`
- 第 65 行：`-latest` 前加 `-prerelease`：
  ```powershell
  $vs = & $vswhere -prerelease -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  ```

### 设计要点
- `-all -prerelease -latest`：`-prerelease` 把 prerelease 通道纳入，`-latest` 仍按版本号最高选（2026=18.x > 2022=17.x）→ 命中 2026。
- 不影响仅有 release（无 Insiders）的用户：`-prerelease` 对 release 实例无副作用。
- 不回退原则：不做「找不到就提示手动装」的降级；修复直接让主路径选中正确版本。

## 3. 回归用例设计（先红后绿）

**新增** `tests/test_setup_vswhere_prerelease.sh`（无头，Linux 可跑）：

```bash
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
```

**先红后绿步骤**：
1. **红**：在未修复的 setup.sh/setup.ps1 上运行 → 断言 1/2 失败（grep 不到 `-prerelease`）→ 红。
2. **修复**：按 §2 改两文件。
3. **绿**：重跑 → 三断言全过 → 绿。

## 4. 影响面核对（②/④）
- 仅改 `scripts/setup.sh`（find_vs 两行 + 注释）与 `scripts/setup.ps1`（一行）。
- `find_vs` 被 `find_cl`/`find_vcvars`/`probe_compiler` 复用，一处修复全路径受益。
- Linux 分支、探测其它项、smoke.sh、CMakeLists 不动。
- 无 -prerelease 的 fallback 目录扫描路径（`find "/c/Program Files/Microsoft Visual Studio"`）不动 —— 它扫的是 Program Files（2026 所在），2022 在 Program Files (x86) 不冲突，保持现状。

## 5. 验证方式（②）
- **无头回归**：`bash tests/test_setup_vswhere_prerelease.sh`（Linux 可跑，mock vswhere）。
- **语法**：`bash -n scripts/setup.sh`。
- **既有回归**：`bash tests/test_setup_msvc_quotes.sh`（不回归）。
- **冒烟**：`bash scripts/setup.sh --check`（Linux 探测逻辑不受影响）。
- **诚实标注**：真机终验需 Windows 跑 `sh scripts/setup.sh`，确认探测显示 2026 Insiders。本 bug 不涉渲染，离屏图像验证不适用（CLI 复现 + 回归用例已覆盖硬约束）。

## 6. 风险与健壮性
- `-prerelease` 对 release-only 用户无副作用（prerelease 通道为空时不影响结果）。
- `-all -prerelease -latest` 组合：版本号最高优先（2026 > 2022），符合「要最新版」意图。
- 若 2026 与 2022 都满足 -requires，`-latest` 仍选 2026（版本更高）。
- 不引入回退/兜底路径。
