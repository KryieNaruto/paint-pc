#!/usr/bin/env bash
# =============================================================================
# setup.sh — paint-pc 消费者一键环境搭建脚本（W2，Bash / Linux-WSL / Git Bash）
#
# 用法:
#   scripts/setup.sh            默认（开发）模式：拉主仓库 + 探测 + 补缺指引 + 拉 submodule + 构建
#   scripts/setup.sh --check    只探测不安装，输出缺项清单（不拉取）
#   scripts/setup.sh --test     探测 + 构建 + 跑测试门（tests/smoke.sh）
#   scripts/setup.sh --sln      生成 VS 解决方案 build/msvc/paint_pc.sln 并构建 Debug（仅 Windows）
#   scripts/setup.sh --auto-install  硬缺项时尝试自动安装（Ninja/Vulkan；直下走国内镜像）
#   scripts/setup.sh --yes      配合 --auto-install 跳过 y/N 确认（Windows 装 Vulkan 仍弹 UAC）
#   scripts/setup.sh --help     打印用法
#
# 仓库内自包含：clone paint-pc 后在此仓库内运行即可（SDK 为 submodule）。
# 非 --check 模式会先 `git pull --ff-only` 主仓库本身，再同步 submodule ——
# 避免「主仓库代码是旧的，只拉了 submodule/编译了旧代码」这类假阴性排查。
# 口径来源: docs/env/env-setup.md（E0-1）+ SDK scripts/setup-env.sh / setup-env.ps1。
# 依赖分级: 硬依赖缺失 → 非零退出并给安装指引；软依赖缺失 → 仅警告。
# =============================================================================
set -euo pipefail

# ---------- 输出辅助 ----------
info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
err()  { printf 'ERROR: %s\n' "$*" >&2; }
ok()   { printf '[OK]   %s\n' "$*"; }

# ---------- 基础工具 ----------
has() { command -v "$1" >/dev/null 2>&1; }

# Linux 判定：uname -s = Linux 且非 MSYS/Git Bash（MSYS2 下 uname 可能报 MINGW/MSYS，
# 极端情况 uname 报 Linux 但 MSYSTEM 已设也应视为 Windows 走 MSVC 路径）。
is_linux() { [ -z "${MSYSTEM:-}" ] && [ -z "${MINGW:-}" ] && [ "$(uname -s)" = "Linux" ]; }
is_windows_gitbash() { [ -n "${MSYSTEM:-}" ] || [ -n "${MINGW:-}" ]; }

extract_version() {
  local s="$1"
  if [[ "$s" =~ ([0-9]+(\.[0-9]+)+) ]]; then
    printf '%s' "${BASH_REMATCH[1]}"
  elif [[ "$s" =~ ([0-9]+) ]]; then
    printf '%s' "${BASH_REMATCH[1]}"
  fi
}
ver_seg() {
  local v="$1" idx="$2" seg
  seg="$(printf '%s' "$v" | cut -d. -f"$((idx + 1))" 2>/dev/null || true)"
  case "$seg" in
    ''|*[!0-9]*) printf '0' ;;
    *) printf '%d' "$((10#$seg))" ;;
  esac
}
ver_ge() {
  local a="$1" b="$2" i sa sb
  for i in 0 1 2 3; do
    sa="$(ver_seg "$a" "$i")"; sb="$(ver_seg "$b" "$i")"
    [ "$sa" -gt "$sb" ] && return 0
    [ "$sa" -lt "$sb" ] && return 1
  done
  return 0
}

# ---------- 探测结果存储 ----------
CHK_NAMES=(); CHK_LEVELS=(); CHK_STATUS=(); CHK_DETAILS=()
HARD_MISS=0; SOFT_MISS=0
AUTO_INSTALL=0; YES=0
NINJA_LEVEL=硬   # 默认/开发模式 -G Ninja 构建必需（硬）；--sln 用 VS 生成器不需要 ninja（软）
record() { CHK_NAMES+=("$1"); CHK_LEVELS+=("$2"); CHK_STATUS+=("$3"); CHK_DETAILS+=("$4"); }

# ---------- 各检查项 ----------
probe_cmake() {
  local v vn
  if ! has cmake; then
    record "CMake" "硬" "MISS(硬)" "未安装（configure 必败）"; HARD_MISS=$((HARD_MISS+1)); return
  fi
  v="$(cmake --version 2>/dev/null | head -n1 || true)"; vn="$(extract_version "$v")"
  if [ -n "$vn" ]; then
    if ver_ge "$vn" "3.22"; then record "CMake" "硬" "OK" "cmake $vn（≥ 3.22）"
    else record "CMake" "硬" "MISS(硬)" "cmake $vn 过旧（需 ≥ 3.22，建议 3.31+）"; HARD_MISS=$((HARD_MISS+1)); fi
  else record "CMake" "硬" "OK" "已安装但版本无法确认（请手动确认 ≥ 3.22）"; fi
}

probe_ninja() {
  local level="${NINJA_LEVEL:-硬}"
  if has ninja; then
    record "Ninja" "$level" "OK" "ninja $(ninja --version 2>/dev/null | head -n1 || true)"
    return
  fi
  # Windows：VS 自带 ninja 不在 PATH（VS 的 CMake 集成内部使用）→ 自动定位并加入本进程 PATH，
  # 构建（-G Ninja）才找得到；同 probe_vulkan 认 LunarG 默认路径的『自动识别已装』思路。
  # --sln 用 VS 生成器（MSBuild）本不需要 ninja → NINJA_LEVEL=软 仅提示，不阻断。
  if ! is_linux; then
    local n dir
    if n="$(find_vs_ninja)"; then
      dir="$(dirname "$n")"
      export PATH="$dir:$PATH"
      hash -r
      record "Ninja" "$level" "OK" "ninja $(ninja --version 2>/dev/null | head -n1 || true)（VS 自带，自动定位 @ $dir）"
      return
    fi
  fi
  if [ "$level" = "硬" ]; then
    record "Ninja" "硬" "MISS(硬)" "未安装（build 必败）"; HARD_MISS=$((HARD_MISS+1))
  else
    record "Ninja" "软" "WARN(软)" "未安装（--sln 的 VS 生成器不需要 ninja，仅提示）"; SOFT_MISS=$((SOFT_MISS+1))
  fi
}

# ── Windows/VS 定位辅助（全自动无人值守；假定用户已装 VS2026）──────────────
# vswhere 是 VS 自带定位器，固定路径在 VS Installer 目录；Git Bash 里不在 PATH。

# 把 Windows 路径（C:\foo\bar）转 MSYS 路径（/c/foo/bar），纯 bash、不依赖 cygpath。
# Git Bash 里 Windows 环境变量（LOCALAPPDATA）与 vswhere 输出都是 C:\ 反斜杠形式，
# 而 bash 的 [ -f ]/[ -d ] 不做路径转换，必须转成 /c/ 才能测。
msys_from_win() {
  local p="$1" drive rest
  p="${p//\\//}"                                  # 反斜杠 → 正斜杠
  if [[ "$p" =~ ^([A-Za-z]):(/.*)?$ ]]; then      # C:/foo/bar → /c/foo/bar
    drive="$(printf '%s' "${BASH_REMATCH[1]}" | tr 'A-Z' 'a-z')"
    rest="${BASH_REMATCH[2]:-/}"
    printf '/%s%s' "$drive" "$rest"
  else
    printf '%s' "$p"
  fi
}

find_vswhere() {
  local la p
  local candidates=(
    "/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    "/c/Program Files/Microsoft Visual Studio/Installer/vswhere.exe"
  )
  # LOCALAPPDATA 是 Windows 风格（C:\Users\...），转 MSYS 再拼。
  if [ -n "${LOCALAPPDATA:-}" ]; then
    la="$(msys_from_win "$LOCALAPPDATA")"
    candidates+=("$la/Microsoft/Visual Studio/Installer/vswhere.exe")
  fi
  for p in "${candidates[@]}"; do
    [ -f "$p" ] && { printf '%s' "$p"; return 0; }
  done
  has vswhere && { command -v vswhere; return 0; }
  return 1
}

# 用 vswhere 查含 VC 工具集的 VS 安装路径（输出首行，去 CRLF，转 MSYS 路径）。
# 关键：必须带 -all -prerelease —— -all 包含所有版本，-prerelease 纳入 Insiders/预览版；
# 缺 -prerelease 会过滤掉 2026 Insiders（用户 VS 装在 Microsoft Visual Studio\18\Insiders）。
# 顺序：带 -requires 精确匹配 → 降级不带 -requires（组件变体）→ 无 vswhere 时直接扫
# C:\Program Files\Microsoft Visual Studio\*\*\ 已知目录（找含 VC 工具集的安装）。
find_vs() {
  local vswhere="" vs="" vs_msys=""
  vswhere="$(find_vswhere)" || vswhere=""
  if [ -n "$vswhere" ]; then
    vs="$("$vswhere" -all -prerelease -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>/dev/null | tr -d '\r' | head -n1)"
    [ -z "$vs" ] && vs="$("$vswhere" -all -prerelease -latest -products '*' -property installationPath 2>/dev/null | tr -d '\r' | head -n1)"
  fi
  # 无 vswhere 或 vswhere 查不到 → 直接扫 VS 安装目录（兼容 Insiders/自定义布局）。
  # 注意：VS 可装在 <root>/18/Insiders/VC/...（深层次），不能限 -maxdepth；用 find 全扫后
  # 取第一个含 VC/Tools/MSVC 的路径，向上裁剪出 VS 安装根。
  if [ -z "$vs" ]; then
    vs="$(find "/c/Program Files/Microsoft Visual Studio" -type d -path '*VC/Tools/MSVC' 2>/dev/null | head -n1 | sed 's#/VC/Tools/MSVC$##')"
  fi
  [ -z "$vs" ] && return 1
  vs_msys="$(msys_from_win "$vs")"
  if [ -d "$vs_msys" ]; then
    printf '%s' "$vs_msys"; return 0
  fi
  return 1
}

# 找 VC 工具集里的 cl.exe（Hostx64\x64）。
# 优先经 find_vs 定位；找不到时直接全盘扫 VS 目录下的 cl.exe（最后一搏）。
find_cl() {
  local vs="" cl=""
  vs="$(find_vs 2>/dev/null || true)"
  if [ -n "$vs" ]; then
    cl="$(find "$vs/VC/Tools/MSVC" -path '*Hostx64/x64/cl.exe' -type f 2>/dev/null | head -n1)"
  fi
  if [ -z "$cl" ]; then
    cl="$(find "/c/Program Files/Microsoft Visual Studio" -path '*VC/Tools/MSVC/*/Hostx64/x64/cl.exe' -type f 2>/dev/null | head -n1)"
  fi
  [ -n "$cl" ] && { printf '%s' "$cl"; return 0; }
  return 1
}

# 找 vcvars64.bat（构建时经 cmd 自动进入 MSVC 环境）。
find_vcvars() {
  local vs="" vc=""
  vs="$(find_vs 2>/dev/null || true)"
  if [ -n "$vs" ]; then
    vc="$vs/VC/Auxiliary/Build/vcvars64.bat"
    [ -f "$vc" ] && { printf '%s' "$vc"; return 0; }
  fi
  # fallback：直接扫 VS 目录下的 vcvars64.bat（兼容 Insiders/自定义布局）。
  vc="$(find "/c/Program Files/Microsoft Visual Studio" -path '*VC/Auxiliary/Build/vcvars64.bat' -type f 2>/dev/null | head -n1)"
  [ -n "$vc" ] && { printf '%s' "$vc"; return 0; }
  return 1
}

# 找 VS 自带的 ninja（「C++ CMake tools for Windows」组件）。VS 的 ninja 不在 PATH（只给 VS 内部
# CMake 集成用），构建 -G Ninja 时需手动定位；--sln（VS 生成器）不需要。路径稳定：
# <VS>/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe。
find_vs_ninja() {
  local vs="" n=""
  vs="$(find_vs 2>/dev/null || true)"
  if [ -n "$vs" ]; then
    n="$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
    [ -f "$n" ] && { printf '%s' "$n"; return 0; }
  fi
  n="$(find "/c/Program Files/Microsoft Visual Studio" -path '*Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe' -type f 2>/dev/null | head -n1)"
  [ -n "$n" ] && { printf '%s' "$n"; return 0; }
  return 1
}

# ── VS 版本 / CMake 生成器推导（--sln 模式专用）────────────────────────────
# 从 VS 安装路径提取版本段（版本段在 SKU 上一级）：
#   .../Microsoft Visual Studio/18/Insiders → 18
#   .../Microsoft Visual Studio/2022/Community → 2022
# 注意不能 basename "$vs"（那会得到 Insiders/Community）。
vs_version_from_path() {
  local p="$1"
  printf '%s' "$(basename "$(dirname "$p")")"
}

# 定位 VS 的 installationVersion（与 find_vs 同参数：-all -prerelease -latest），
# 失败退回安装路径版本段（VS2026=18 / VS2022=2022）。
find_vs_version() {
  local vswhere="" ver="" vs=""
  vswhere="$(find_vswhere || true)"
  if [ -n "$vswhere" ]; then
    ver="$("$vswhere" -all -prerelease -latest -products '*' \
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
        -property installationVersion 2>/dev/null | tr -d '\r' | head -n1)"
    [ -z "$ver" ] && ver="$("$vswhere" -all -prerelease -latest -products '*' \
        -property installationVersion 2>/dev/null | tr -d '\r' | head -n1)"
    [ -n "$ver" ] && { printf '%s' "$ver"; return 0; }
  fi
  vs="$(find_vs 2>/dev/null || true)"        # 兜底：路径版本段（VS2026=18 / VS2022=2022）
  [ -n "$vs" ] && { printf '%s' "$(vs_version_from_path "$vs")"; return 0; }
  return 1
}

# 版本/年份 → CMake 生成器名（不支持 VS2017 及更早 / 空串 → 非零失败）。
vs_generator_name() {
  local v="$1" major
  major="${v%%.*}"
  case "$major" in
    16|2019) echo "Visual Studio 16 2019" ;;
    17|2022) echo "Visual Studio 17 2022" ;;
    18|2026) echo "Visual Studio 18 2026" ;;
    *) return 1 ;;
  esac
}

probe_compiler() {
  if is_linux; then
    # Linux/WSL：g++（GCC≥11）。
    local v vn
    if ! has g++; then
      record "C++ 编译器 (g++)" "硬" "MISS(硬)" "未安装 g++（编译必败）"; HARD_MISS=$((HARD_MISS+1)); return
    fi
    v="$(g++ -dumpfullversion 2>/dev/null || g++ -dumpversion 2>/dev/null || true)"; vn="$(extract_version "$v")"
    if [ -n "$vn" ]; then
      if ver_ge "$vn" "11"; then record "C++ 编译器 (g++)" "硬" "OK" "g++ $vn（GCC ≥ 11）"
      else record "C++ 编译器 (g++)" "硬" "MISS(硬)" "g++ $vn 过旧（需 GCC ≥ 11）"; HARD_MISS=$((HARD_MISS+1)); fi
    else record "C++ 编译器 (g++)" "硬" "OK" "已安装但版本无法确认（请手动确认 ≥ 11）"; fi
    return
  fi

  # Windows（Git Bash / MSYS2 / 原生 cmd 内 bash）：MSVC —— 自动定位 VS2026，不要求 cl 在 PATH。
  # 假定用户已装 VS2026（含「使用 C++ 的桌面开发」工作负载），脚本用 vswhere 自动定位。
  # ⚠ 注意：find_* 在未找到时 return 1，命令替换返回非零在 set -e 下会静默退出，
  #   必须 `|| true` 兜底（否则脚本在 print_check 前就无输出退出）。
  local cl vs vc
  cl="$(find_cl 2>/dev/null || true)"
  vs="$(find_vs 2>/dev/null || true)"
  vc="$(find_vcvars 2>/dev/null || true)"
  if [ -n "$cl" ] && [ -n "$vc" ]; then
    record "C++ 编译器 (MSVC)" "硬" "OK" "MSVC cl.exe @ $(dirname "$cl")（VS @ $vs，自动定位，构建时经 vcvars64 进入 MSVC 环境）"
  elif [ -n "$vs" ]; then
    record "C++ 编译器 (MSVC)" "硬" "MISS(硬)" "找到 VS @ $vs 但缺 cl.exe（未装「使用 C++ 的桌面开发」工作负载）"; HARD_MISS=$((HARD_MISS+1))
  else
    record "C++ 编译器 (MSVC)" "硬" "MISS(硬)" "未找到 VS2026（需安装 VS2026 + 「使用 C++ 的桌面开发」；本脚本全自动无人值守，需 VS 已装）"; HARD_MISS=$((HARD_MISS+1))
  fi
}

probe_vulkan() {
  # paint-pc 离屏渲染走真实 VkBackend → Vulkan 对构建/验证是硬依赖。
  # 探测「编译能力」：VULKAN_SDK（Windows/LunarG）→ pkg-config vulkan → vulkan/vulkan.h 头 →
  # deps root → LunarG 默认安装路径（Windows）。注意 ldconfig 里的 libvulkan.so.1 是运行时 mesa
  # 驱动，不是编译头/库，不能当判据。
  local vsdk="${VULKAN_SDK:-}"
  local deps="${DGCPAIN_DEPS_ROOT:-}"
  local vsdk_win="" found=""
  if [ -n "$vsdk" ] && [ -d "$vsdk" ]; then
    found="VULKAN_SDK=$vsdk"
  elif is_linux && pkg-config --exists vulkan 2>/dev/null; then
    found="pkg-config vulkan $(pkg-config --modversion vulkan 2>/dev/null)"
  elif is_linux && [ -f /usr/include/vulkan/vulkan.h ]; then
    found="/usr/include/vulkan/vulkan.h"
  elif is_linux && [ -n "$deps" ] && { [ -f "$deps/include/vulkan/vulkan.h" ] || [ -f "$deps/lib/libvulkan.so" ] || ls "$deps"/lib/*/libvulkan.so >/dev/null 2>&1; }; then
    found="DGCPAIN_DEPS_ROOT=$deps"
  elif ! is_linux && vsdk_win="$(find_vulkan_sdk_win)"; then
    # winget/LunarG 安装器装完后 VULKAN_SDK 环境变量不回传当前 shell → 主动认默认安装路径
    # C:/VulkanSDK/<版本> 并 export（构建阶段 CMake 找头/库、MSVC bat 注入 Bin 都依赖它）。
    # 顺带让「装了 SDK 但没设环境变量」的存量用户也被自动识别。
    export VULKAN_SDK="$vsdk_win"
    found="VULKAN_SDK=$vsdk_win（LunarG 默认安装路径）"
  fi
  if [ -n "$found" ]; then
    record "Vulkan" "硬" "OK" "$found"
  else
    record "Vulkan" "硬" "MISS(硬)" "未找到 Vulkan 编译头/库（paint-pc 离屏渲染真实 VkBackend：Linux 装 libvulkan-dev 或设 DGCPAIN_DEPS_ROOT；Windows 设 VULKAN_SDK）"; HARD_MISS=$((HARD_MISS+1))
  fi
}

probe_python() {
  # 构建 paint_pc 不需要 python；仅 --test 的 smoke.sh 依赖 python3 解码 PNG。
  # 默认/开发模式：python 缺失仅警告（软）；--test 模式：缺失才硬性失败。
  # 多候选探测：python3 / python / py（Windows launcher）。
  local pycmd="" pyver=""
  if has python3; then pycmd=python3
  elif has python; then pycmd=python
  elif has py; then pycmd="py -3"
  fi
  if [ -n "$pycmd" ]; then
    pyver="$($pycmd --version 2>/dev/null | head -n1 || echo "$pycmd")"
    record "python3" "硬" "OK" "$pyver（$pycmd）"
  elif [ "$PY_TEST_NEEDED" = "1" ]; then
    record "python3" "硬" "MISS(硬)" "未安装（--test 的 tests/smoke.sh 依赖 python3 解码 PNG；可装 python.org 或 winget install Python.Python.3.12）"; HARD_MISS=$((HARD_MISS+1))
  else
    record "python3" "软" "WARN(软)" "未安装（仅 --test 需要；默认/开发模式不阻断）"; SOFT_MISS=$((SOFT_MISS+1))
  fi
}

probe_git() {
  if ! has git; then
    record "git" "硬" "MISS(硬)" "未安装（拉 submodule 必败）"; HARD_MISS=$((HARD_MISS+1)); return
  fi
  record "git" "硬" "OK" "git $(git --version 2>/dev/null | head -n1 || true)"
}

probe_glslc() {
  if ! has glslc; then
    record "glslc" "软" "WARN(软)" "未安装（随 Vulkan SDK 提供；shaderc 走库调用，仅提示）"; SOFT_MISS=$((SOFT_MISS+1)); return
  fi
  record "glslc" "软" "OK" "$(glslc --version 2>/dev/null | head -n1 || true)"
}

probe_all() {
  CHK_NAMES=(); CHK_LEVELS=(); CHK_STATUS=(); CHK_DETAILS=(); HARD_MISS=0; SOFT_MISS=0
  probe_cmake; probe_ninja; probe_compiler; probe_vulkan; probe_python; probe_git; probe_glslc
}

# ---------- 输出 ----------
print_check() {
  info "=== 环境探测结果 ==="
  local i
  for i in "${!CHK_NAMES[@]}"; do
    printf '[%s] %s: %s\n' "${CHK_STATUS[$i]}" "${CHK_NAMES[$i]}" "${CHK_DETAILS[$i]}"
  done
  echo
  if [ "$HARD_MISS" -gt 0 ]; then err "硬依赖缺失 $HARD_MISS 项，构建/测试将失败。"
  else info "硬依赖齐全；软依赖缺失不阻断。"; fi
}

print_guidance() {
  info "请按 docs/env/env-setup.md（SDK）与 README 手动补缺："
  info "  - Linux:  sudo apt install build-essential cmake ninja-build libvulkan-dev python3"
  info "  - Windows: VS2026 + 「使用 C++ 的桌面开发」+「C++ CMake tools」；LunarG Vulkan SDK（https://vulkan.lunarg.com/）设 \$env:VULKAN_SDK；git"
  info "  - 脚本在 Windows 下自动定位 VS2026（vswhere）并经 vcvars64 进入 MSVC 环境，无需 cl 在 PATH。"
  info "  - Ninja 随 VS「C++ CMake tools」组件提供（不进 PATH），脚本会自动定位 VS 自带 ninja 并加入本进程 PATH；--sln 用 VS 生成器不需要 ninja。"
  info "  - 也可用同仓库 scripts/setup.ps1（Windows PowerShell 原生）。"
}

print_help() {
  cat <<'EOF'
用法: scripts/setup.sh [--check|--test|--sln] [--auto-install] [--yes|-y|--help]

  一键搭建 paint-pc 开发/测试环境（仓库内自包含）。

模式:
  （默认） 拉主仓库 + 探测 + 补缺指引 + 拉 SDK submodule + 构建 paint_pc
  --check  只探测不安装，输出缺项清单（不拉取）；硬依赖缺失时非零退出
  --test   探测 + 构建 + 跑测试门 tests/smoke.sh（headless 离屏 PNG 真实笔迹断言）
  --sln    生成 VS 解决方案 build/msvc/paint_pc.sln 并构建 Debug 验证链接（仅 Windows，需 VS2019/2022/2026）
  --auto-install
           硬依赖缺失时尝试自动安装可装项，装完重新探测，仍缺才给指引退出；
           安装前默认 y/N 确认。Ninja 直下走国内镜像（ghproxy 前缀，PC_FETCH_MIRROR /
           PC_NINJA_URL 可覆盖）；Vulkan 在 Windows 经 winget 装 LunarG SDK（约 500MB、
           弹 UAC；sdk.lunarg.com 国内直连可达、无镜像承载）、Linux 走包管理器
  --yes    （-y）配合 --auto-install 跳过 y/N 确认（Windows 装 Vulkan SDK 仍会弹一次 UAC 提权）
  --help   打印本帮助

无人值守: 假定 VS2026（含「使用 C++ 的桌面开发」）已装；Windows 下自动用 vswhere 定位
  VS 并经 vcvars64 进入 MSVC 环境构建，不要求 cl 在 PATH。Ninja 同理：自动认 VS 自带的 ninja
  （「C++ CMake tools」组件）并加入本进程 PATH；--sln 用 VS 生成器本不需要 ninja，缺失仅提示。
  python 在默认/开发模式为软依赖，仅 --test 需要（smoke.sh 解码 PNG）。真缺硬依赖时默认给出
  指引并非零退出；加 --auto-install 可自动装 Ninja/Vulkan（VS2026 等大件仍需手动装）。

依赖: CMake≥3.22 / Ninja / C++ 编译器（g++≥11 或 MSVC）/ Vulkan（真实后端）/ python3（--test）/ git。
口径来源: SDK docs/env/env-setup.md。
EOF
}

# ---------- 动作 ----------
# 拉主仓库自身最新代码。只用 --ff-only：本地有未提交改动 / 与远端分叉 / detached HEAD /
# 无追踪分支时 git 会安全拒绝（不 merge、不 rebase、不覆盖本地改动），这里只警告不阻断，
# 让用户能继续用当前本地代码构建，自己决定何时处理分叉。
pull_self() {
  local root="$1"
  if [ ! -d "$root/.git" ]; then
    warn "paint-pc 根目录非 git 仓库（缺 .git），跳过主仓库自拉取"
    return 0
  fi
  local branch
  branch="$(git -C "$root" symbolic-ref --short -q HEAD || true)"
  if [ -z "$branch" ]; then
    warn "当前处于 detached HEAD，跳过主仓库 git pull"
    return 0
  fi
  info "拉取 paint-pc 主仓库最新代码（分支 $branch）…"
  if ! git -C "$root" pull --ff-only; then
    warn "git pull --ff-only 失败（本地有未提交改动 / 与远端分叉 / 无追踪分支等）；请手动处理后重跑。本次继续用本地现有代码构建。"
  fi
}

sync_submodule() {
  local root="$1"
  info "同步 SDK submodule（跟随 paintDemo main 最新）…"
  git -C "$root" submodule update --init --recursive --remote
}

# 拉取三方库（复用 SDK 共享 fetch-deps.sh，--fetch 从国内镜像拉取/解包到 sdk/deps/usr）。
# 关键：fetch-deps 是子进程，其内部 export 不回传本 shell；此处按 deps 落盘位置显式设
# DGCPAIN_DEPS_ROOT（deps 在 SDK submodule 根 sdk/deps/usr，不是 paint-pc 根 deps）。
fetch_deps() {
  local root="$1"
  local sdk_script="$root/sdk/scripts/fetch-deps.sh"
  if [ ! -f "$sdk_script" ]; then
    warn "未找到 sdk/scripts/fetch-deps.sh（submodule 未更新到含共享拉取脚本的版本？跳过依赖拉取）"
    return 0
  fi
  info "拉取三方库（fetch-deps）…"
  bash "$sdk_script" --fetch
  if [ -z "${DGCPAIN_DEPS_ROOT:-}" ] && [ -d "$root/sdk/deps/usr" ]; then
    export DGCPAIN_DEPS_ROOT="$root/sdk/deps/usr"
  fi
  # 可选镜像加速：用户已设 PC_FETCH_MIRROR 时透传给 cmake（默认不设，走官方 GitHub）。
  export PC_FETCH_MIRROR="${PC_FETCH_MIRROR:-}"
}

# 生成 MSVC 构建批处理，返回供 `cmd //c` 调用的参数。
# 关键不变量：cmd 参数必须「无斜杠、无空格、无引号」（裸文件名 `_setup_msvc.bat`）。
#   - 无引号：Git Bash/MSYS2 会把参数内 `"` 重转义为 `\"`，而 cmd.exe 不认 `\"`，
#     会把 `\"C:\...bat\"` 当命令名报 "'\"...\"' 不是内部或外部命令"。
#   - 无斜杠：含 `/` 的相对路径参数会被 MSYS2 做路径转换，且 cmd 对正斜杠批处理路径
#     解析把 `/` 前段当命令名 → 真机报 `'build' 不是内部或外部命令`。
# 引号只出现在 bat 文件内容里（文件内字面），不经参数传递；斜杠通过「只返回裸文件名
# + 调用方把 cwd 切到 build/」消除。
make_msvc_build_bat() {
  local root="$1" vcvars="$2" vsdk_bin="$3"
  local root_win vc_win
  root_win="$(cygpath -w "$root")"
  vc_win="$(cygpath -w "$vcvars")"
  mkdir -p "$root/build"
  cat > "$root/build/_setup_msvc.bat" <<EOF
@echo off
call "$vc_win"
if errorlevel 1 exit /b %errorlevel%
EOF
  # 仅当 VULKAN_SDK 已设（探测阶段已保证）才注入其 Bin 到 PATH；未设时不写脏路径 `/Bin`。
  if [ -n "$vsdk_bin" ]; then
    printf 'set "PATH=%s;%%PATH%%"\n' "$vsdk_bin" >> "$root/build/_setup_msvc.bat"
  fi
  cat >> "$root/build/_setup_msvc.bat" <<EOF
cd /d "$root_win"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF
if errorlevel 1 exit /b %errorlevel%
cmake --build build -j
exit /b %errorlevel%
EOF
  printf '_setup_msvc.bat'
}

build_pc() {
  local root="$1"
  info "构建 paint_pc…"
  # 与 tests/smoke.sh 一致：DGCPAIN_DEPS_ROOT / PC_X11_DEPS_ROOT 可覆盖（Linux）。
  local deps="${DGCPAIN_DEPS_ROOT:-}"
  local x11="${PC_X11_DEPS_ROOT:-}"
  local extra=()
  [ -n "$deps" ] && extra+=(-DDGCPAIN_DEPS_ROOT="$deps")
  [ -n "$x11" ] && extra+=( -DCMAKE_PREFIX_PATH="$x11" -DCMAKE_C_FLAGS="-I$x11/include" -DCMAKE_CXX_FLAGS="-I$x11/include")

  local vcvars

  if ! is_linux; then
    # Windows：自动进入 MSVC 环境（vcvars64），cl.exe / ninja 才可用。全自动无人值守。
    vcvars="$(find_vcvars || true)"
    if [ -n "$vcvars" ]; then
      info "进入 MSVC 环境（vcvars64）…"
      local cmd_arg
      # VULKAN_SDK 未设时传空串 → bat 不写 /Bin 脏路径（探测阶段已保证 VULKAN_SDK 已设）。
      cmd_arg="$(make_msvc_build_bat "$root" "$vcvars" "${VULKAN_SDK:+${VULKAN_SDK}/Bin}")"
      # bat 是裸文件名（无斜杠）→ MSYS2 无路径可转换、cmd 无斜杠可解析。
      # cwd 先切到 build/，cmd 从当前目录（= build/）找到 _setup_msvc.bat；
      # bat 内部 `cd /d "$root_win"` 会切回仓库根执行 cmake，不依赖起始 cwd。
      if ! ( cd "$root/build" && cmd //c "$cmd_arg" ); then
        return 1
      fi
      return 0
    fi
    # 探测阶段应已拦截（MSVC 缺则 HARD_MISS），这里兜底报错。
    err "未找到 vcvars64.bat，无法进入 MSVC 环境"
    return 1
  fi

  # Linux：直接 cmake（g++ + Ninja）。
  cmake -B "$root/build" -S "$root" -DCMAKE_BUILD_TYPE=Debug \
    -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF "${extra[@]}"
  cmake --build "$root/build" -j
}

# --sln 模式：用 CMake Visual Studio 生成器在独立目录 build/msvc/ 生成 paint_pc.sln，
# 并构建 Debug 配置验证链接（VS 内改码 → F5 直接开发）。
build_pc_sln() {
  local root="$1" vs="" vs_win="" ver="" gen="" deps="${DGCPAIN_DEPS_ROOT:-}"
  vs="$(find_vs || true)"
  [ -z "$vs" ] && { err "未找到 VS 安装（--sln 需要 VS2019/2022/2026）"; return 1; }
  vs_win="$(cygpath -w "$vs" 2>/dev/null || printf '%s' "$vs")"
  ver="$(find_vs_version || true)"
  gen="$(vs_generator_name "${ver:-}" || true)"
  [ -z "$gen" ] && { err "无法从 VS 版本『${ver:-未知}』推导 CMake 生成器（支持 VS2019/2022/2026）"; return 1; }
  info "生成 VS 解决方案：${gen}（实例：$vs）…"
  local extra=()
  [ -n "$deps" ] && extra+=(-DDGCPAIN_DEPS_ROOT="$deps")
  cmake -S "$root" -B "$root/build/msvc" -G "$gen" -A x64 \
    -DCMAKE_GENERATOR_INSTANCE="$vs_win" \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL \
    -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF "${extra[@]}"
  # 先 clean 再 build：VS 增量构建只看时间戳/依赖图，git pull 后个别场景下（比如浅拉取、
  # 系统时钟/文件系统时间戳异常）可能不触发重编译，看起来像"拉了新代码但跑的还是旧的"。
  # --sln 模式的定位是"给我一个绝对最新的 Debug 构建去验证"，不是日常迭代增量编译，
  # 这里直接强制干净重建，用速度换确定性。
  info "清空 Debug 配置增量缓存，确保这次是完全重建（避免增量缓存导致的『旧版本』假象）…"
  cmake --build "$root/build/msvc" --config Debug --target clean || true
  info "构建 Debug 配置验证链接（干净重建）…"
  cmake --build "$root/build/msvc" --config Debug -j
  info "已生成：build/msvc/paint_pc.sln（VS 打开，选 Debug 配置开发）"
  info "若 VS 已经开着这个 sln：CMake 重新生成工程文件后 VS 通常会弹『重新加载』提示，"
  info "点重新加载（或直接关闭重开 sln）；不然 IDE 里可能还在用重生成前的旧工程模型，"
  info "点『生成』看着像没生效，其实是 IDE 那份工程状态没跟上，不是代码没编译进去。"
}

# 打印这次构建对应的 pc/sdk 版本戳，跟运行时窗口标题栏 / Performance 面板里的一致，
# 用来在命令行里就能确认"这次到底构建的是哪个提交"，不用打开 app 去看。
print_version() {
  local root="$1" pcSha sdkSha
  pcSha="$(git -C "$root" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
  sdkSha="$(git -C "$root/sdk" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
  info "本次构建版本戳：pc ${pcSha} / sdk ${sdkSha}（运行时窗口标题栏、Performance 面板里应显示同样的值）"
}

run_test() {
  local root="$1"
  info "跑测试门 tests/smoke.sh…"
  (cd "$root" && bash tests/smoke.sh)
}

# ---------- 自动安装（--auto-install） ----------
# winget 装完 PATH/环境变量不回传当前 shell；以下两个定位函数让本进程装完立刻能探测到。

# 定位 winget 包安装目录：%LOCALAPPDATA%\Microsoft\WinGet\Packages\<ID>_<source>_<hash>/。
# 返回含目标文件（如 ninja.exe）的目录；找不到返回空。LOCALAPPDATA 在 Git Bash 里是 C:\
# 反斜杠形式，先经 msys_from_win 转 /c/ 才能 [ -f ] 测（对无盘符路径原样放行，便于测试注入）。
find_winget_package_dir() {
  local id="$1" target="$2" base dir
  base="${LOCALAPPDATA:-}"
  [ -n "$base" ] || return 1
  base="$(msys_from_win "$base")/Microsoft/WinGet/Packages"
  [ -d "$base" ] || return 1
  for dir in "$base/${id}"_*/; do
    [ -d "$dir" ] || continue
    if [ -f "$dir/$target" ]; then printf '%s' "$dir"; return 0; fi
  done
  return 1
}

# 定位 LunarG Vulkan SDK 默认安装路径：C:/VulkanSDK/<最新版本>/（含 Include/vulkan/vulkan.h）。
# 版本号按数字分段比大小取最新（1.3.280 > 1.3.99）。VULKAN_SDK_DEFAULT_DIR 仅供测试注入。
find_vulkan_sdk_win() {
  local base="${VULKAN_SDK_DEFAULT_DIR:-/c/VulkanSDK}"
  local d v latest=""
  [ -d "$base" ] || return 1
  for d in "$base"/*/; do
    [ -f "$d/Include/vulkan/vulkan.h" ] || continue
    v="$(basename "$d")"; v="${v#v}"
    if [ -z "$latest" ] || ver_ge "$v" "$latest"; then latest="$v"; fi
  done
  [ -n "$latest" ] || return 1
  printf '%s/%s' "$base" "$latest"
}

# Linux 包管理器命令前缀：root 直接跑，非 root 走 sudo（CI/无 sudo 时 sudo 失败 → 安装失败 → 指引）。
run_root() { if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi; }

pkg_install() {
  # 用已装的包管理器安装一批包；失败返回非零。apt 先静默 update 一次（首次源列表可能为空/旧）。
  if has apt-get; then
    run_root apt-get update -y >/dev/null 2>&1 || true
    run_root apt-get install -y "$@"
  elif has dnf; then
    run_root dnf install -y "$@"
  elif has pacman; then
    run_root pacman -S --noconfirm "$@"
  elif has zypper; then
    run_root zypper --non-interactive install "$@"
  else
    warn "未识别的 Linux 包管理器（需 apt-get/dnf/pacman/zypper）"
    return 1
  fi
}

# 该依赖是否可自动安装（VS2026 等大件不可自动装，保持指引）。
installable_dep() {
  case "$1" in
    Ninja|Vulkan) return 0 ;;
    *) return 1 ;;
  esac
}
install_desc() {
  case "$1" in
    Ninja)  echo "Ninja 构建系统（国内镜像直下 / winget / Linux 包管理器，约 2MB）" ;;
    Vulkan) echo "Vulkan SDK（Windows 经 winget 装 LunarG，约 500MB、弹 UAC；Linux 为 libvulkan-dev）" ;;
  esac
}

install_ninja() {
  local root="$1" dir url tools
  if is_linux; then
    pkg_install ninja-build || return 1
    hash -r
    return 0
  fi
  # Windows：国内镜像直下 ninja-win.zip 到仓库内 tools/ninja/（无需管理员，跟随 fetch-deps 的
  # 『仓库内自包含 + 国内镜像』风格，同 CMakeLists PC_FETCH_MIRROR=ghproxy 前缀约定；
  # PC_FETCH_MIRROR / PC_NINJA_URL 可覆盖）。镜像失败才退回 winget（官方包源，下载源为
  # GitHub，仅作兜底）。
  tools="$root/tools/ninja"
  url="${PC_NINJA_URL:-${PC_FETCH_MIRROR:-https://ghproxy.com/}https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip}"
  mkdir -p "$tools"
  if curl -fSL --connect-timeout 15 "$url" -o "$tools/ninja-win.zip" \
    && (cd "$tools" && unzip -o ninja-win.zip >/dev/null 2>&1); then
    rm -f "$tools/ninja-win.zip"
    export PATH="$tools:$PATH"
    hash -r
    info "Ninja 已装于 $tools（国内镜像直下，已加入本进程 PATH）"
    return 0
  fi
  rm -f "$tools/ninja-win.zip"
  warn "国内镜像直下 Ninja 失败（镜像源: $url）；退回 winget…"
  if has winget; then
    info "winget 安装 Ninja（Ninja-build.Ninja）…"
    if winget install -e --id Ninja-build.Ninja --silent --accept-package-agreements --accept-source-agreements; then
      if dir="$(find_winget_package_dir "Ninja-build.Ninja" "ninja.exe")"; then
        export PATH="$dir:$PATH"
        hash -r
        info "Ninja 已装于 $dir（已加入本进程 PATH）"
        return 0
      fi
      warn "winget 报告安装成功但未在包目录定位到 ninja.exe"
    fi
  else
    warn "winget 不可用"
  fi
  warn "Ninja 自动安装失败（镜像 + winget 均失败；可用 PC_NINJA_URL 指定可用镜像后重跑）"
  return 1
}

install_vulkan() {
  local root="$1" sdk
  if is_linux; then
    pkg_install libvulkan-dev || return 1
    return 0
  fi
  # Windows：winget 装 LunarG Vulkan SDK。整个 SDK 没有国内镜像承载（deps 规范
  # 2026-08-25 亦注明 Windows 侧『探测已装即用 + LunarG 指引』的务实取舍），但 sdk.lunarg.com
  # 国内直连可达（非 GitHub 下载），winget 是可自动化的最稳路径——这里是 --auto-install 的
  # 显式 opt-in，默认流程仍保持探测+指引。装完 VULKAN_SDK 不回传本进程 → 定位默认路径并
  # export，probe_vulkan 与构建（CMake 找头/库 + MSVC bat 注入 Bin）才能立刻拿到。
  if ! has winget; then
    warn "winget 不可用，无法自动安装 Vulkan SDK（请手动装 LunarG Vulkan SDK 或设 VULKAN_SDK）"
    return 1
  fi
  info "winget 安装 Vulkan SDK（KhronosGroup.VulkanSDK，约 500MB，会弹 UAC 提权确认）…"
  if ! winget install -e --id KhronosGroup.VulkanSDK --silent --accept-package-agreements --accept-source-agreements; then
    warn "winget 安装 Vulkan SDK 失败（winget 源需可达；否则请手动装 LunarG Vulkan SDK）"
    return 1
  fi
  if sdk="$(find_vulkan_sdk_win)"; then
    export VULKAN_SDK="$sdk"
    info "Vulkan SDK 已装于 $sdk（VULKAN_SDK 已 export 给本进程）"
    return 0
  fi
  warn "Vulkan SDK 已装但未在 C:/VulkanSDK 定位到版本目录（确认安装位置后重跑）"
  return 1
}

# 汇总当前硬缺失且可自动安装的项 → 确认 → 逐个安装 → 重新探测。
auto_install() {
  local root="$1" i name todo=()
  for i in "${!CHK_NAMES[@]}"; do
    if [ "${CHK_STATUS[$i]}" = "MISS(硬)" ] && installable_dep "${CHK_NAMES[$i]}"; then
      todo+=("${CHK_NAMES[$i]}")
    fi
  done
  if [ "${#todo[@]}" -eq 0 ]; then
    warn "没有可自动安装的缺项（VS2026 等需手动补装）。"
    return 0
  fi
  if [ "$YES" != "1" ]; then
    info "检测到可自动安装的硬依赖缺失："
    for name in "${todo[@]}"; do info "  - $name: $(install_desc "$name")"; done
    printf '自动安装以上依赖（Windows 装 Vulkan SDK 会弹 UAC 提权）？[y/N] '
    local ans=""; read -r ans || ans=""
    case "$ans" in
      y|Y|yes|YES) ;;
      *) info "已取消自动安装，按指引手动补装。"; return 0 ;;
    esac
  fi
  for name in "${todo[@]}"; do
    case "$name" in
      Ninja)  install_ninja "$root"  && info "Ninja 自动安装完成"  || warn "Ninja 自动安装失败（保留原指引）" ;;
      Vulkan) install_vulkan "$root" && info "Vulkan SDK 自动安装完成" || warn "Vulkan SDK 自动安装失败（保留原指引）" ;;
    esac
  done
  info "重新探测环境…"
  probe_all
  print_check
}

# ---------- 主流程 ----------
main() {
  local mode="dev" a
  AUTO_INSTALL=0; YES=0
  for a in "$@"; do
    case "$a" in
      --check) mode="check" ;;
      --test)  mode="test" ;;
      --sln)   mode="sln" ;;
      --auto-install) AUTO_INSTALL=1 ;;
      --yes|-y) YES=1 ;;
      -h|--help) print_help; exit 0 ;;
      *) err "未知参数：$a（用法: setup.sh [--check|--test|--sln] [--auto-install] [--yes|-y]）"; exit 2 ;;
    esac
  done

  local root
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

  # --sln 仅支持 Windows（VS 生成器需 MSVC 工具链）；Linux 请用默认 Ninja 构建。
  # 必须在 sync/fetch/probe 之前拒绝 → Linux 下立即退出，无网络/探测副作用（回归可断言）。
  if [ "$mode" = "sln" ] && is_linux; then
    err "--sln 仅支持 Windows（VS 生成器需 MSVC 工具链）；Linux 请用默认 Ninja 构建。"
    exit 2
  fi

  # --test 模式：python 升级为硬依赖（smoke.sh 解码 PNG 需要）。
  [ "$mode" = "test" ] && PY_TEST_NEEDED=1 || PY_TEST_NEEDED=0
  export PY_TEST_NEEDED

  if [ "$mode" != "check" ]; then
    # 先拉主仓库自身，再同步 submodule + 拉取三方库（fetch-deps 导出 DGCPAIN_DEPS_ROOT），
    # 最后才探测。否则探测在 fetch 之前跑，Vulkan/shaderc 报 MISS → HARD_MISS 拦截，deps
    # 没机会被拉；主仓库不先拉的话，编译出来的是本地旧代码，构建成功也可能是"假阴性"。
    pull_self "$root"
    sync_submodule "$root"
    fetch_deps "$root"
  fi

  # ninja 依赖分级：默认/开发模式用 -G Ninja 构建 → 必需（硬）；--sln 用 VS 生成器（MSBuild）
  # 构建，不需要 ninja → 软（缺失仅提示）。Windows 下 PATH 无 ninja 时自动认 VS 自带 ninja。
  NINJA_LEVEL="硬"; [ "$mode" = "sln" ] && NINJA_LEVEL="软"
  export NINJA_LEVEL

  probe_all
  print_check

  if [ "$mode" = "check" ]; then
    # --check 语义是「只探测不安装」；配 --auto-install 时额外尝试自动补装（装完重探测）。
    if [ "$AUTO_INSTALL" = "1" ] && [ "$HARD_MISS" -gt 0 ]; then
      auto_install "$root"
    fi
    [ "$HARD_MISS" -gt 0 ] && { print_guidance; exit 1; }
    exit 0
  fi

  if [ "$HARD_MISS" -gt 0 ]; then
    if [ "$AUTO_INSTALL" = "1" ]; then
      auto_install "$root"
    else
      print_guidance
      err "硬依赖缺失 $HARD_MISS 项。脚本假定 VS2026 / 编译工具已装（Windows 用 MSVC 自动定位、Linux 用 g++）；对真缺项请按指引补装后重跑，或用 --auto-install 尝试自动安装（无人值守，默认不做静默安装）。"
      exit 1
    fi
    # 自动安装后仍缺（含不可自动装的项，如缺 VS2026）→ 指引退出。
    if [ "$HARD_MISS" -gt 0 ]; then
      print_guidance
      err "自动安装后仍有 $HARD_MISS 项硬依赖缺失（不可自动装的部分请手动补装）。"
      exit 1
    fi
  fi

  if [ "$mode" = "sln" ]; then
    build_pc_sln "$root" || { err "生成 VS 解决方案失败（build/msvc）"; exit 1; }
  else
    build_pc "$root"
  fi

  if [ "$mode" = "test" ]; then
    run_test "$root"
  fi

  print_version "$root"

  if [ "$mode" = "sln" ]; then
    info "已生成 VS 解决方案：build/msvc/paint_pc.sln（VS 打开选 Debug 开发）"
  else
    info "paint-pc 环境就绪。运行: ./build/paint_pc（有显示）/ ./build/paint_pc --headless out.png（离屏）"
  fi
  exit 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
