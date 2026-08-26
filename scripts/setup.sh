#!/usr/bin/env bash
# =============================================================================
# setup.sh — paint-pc 消费者一键环境搭建脚本（W2，Bash / Linux-WSL / Git Bash）
#
# 用法:
#   scripts/setup.sh            默认（开发）模式：探测 + 补缺指引 + 拉 submodule + 构建
#   scripts/setup.sh --check    只探测不安装，输出缺项清单
#   scripts/setup.sh --test     探测 + 构建 + 跑测试门（tests/smoke.sh）
#   scripts/setup.sh --help     打印用法
#
# 仓库内自包含：clone paint-pc 后在此仓库内运行即可（SDK 为 submodule）。
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
  if ! has ninja; then
    record "Ninja" "硬" "MISS(硬)" "未安装（build 必败）"; HARD_MISS=$((HARD_MISS+1)); return
  fi
  record "Ninja" "硬" "OK" "ninja $(ninja --version 2>/dev/null | head -n1 || true)"
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
# 先带 -requires 精确匹配 VC 工具集；找不到则降级不带 -requires（组件名变体/未装工作负载
# 时仍能定位 VS 安装目录，让脚本报更精确的「缺组件」而非「没找到 VS」）。
find_vs() {
  local vswhere vs vs_msys
  vswhere="$(find_vswhere)" || return 1
  vs="$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>/dev/null | tr -d '\r' | head -n1)"
  [ -z "$vs" ] && vs="$("$vswhere" -latest -products '*' -property installationPath 2>/dev/null | tr -d '\r' | head -n1)"
  [ -z "$vs" ] && return 1
  vs_msys="$(msys_from_win "$vs")"
  if [ -d "$vs_msys" ]; then
    printf '%s' "$vs_msys"; return 0
  fi
  return 1
}

# 找 VC 工具集里的 cl.exe（Hostx64\x64）。
find_cl() {
  local vs cl
  vs="$(find_vs)" || return 1
  cl="$(find "$vs/VC/Tools/MSVC" -path '*Hostx64/x64/cl.exe' -type f 2>/dev/null | head -n1)"
  [ -n "$cl" ] && { printf '%s' "$cl"; return 0; }
  return 1
}

# 找 vcvars64.bat（构建时经 cmd 自动进入 MSVC 环境）。
find_vcvars() {
  local vs vc
  vs="$(find_vs)" || return 1
  vc="$vs/VC/Auxiliary/Build/vcvars64.bat"
  [ -f "$vc" ] && { printf '%s' "$vc"; return 0; }
  return 1
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
  # deps root。注意 ldconfig 里的 libvulkan.so.1 是运行时 mesa 驱动，不是编译头/库，不能当判据。
  local vsdk="${VULKAN_SDK:-}"
  local deps="${DGCPAIN_DEPS_ROOT:-}"
  local found=""
  if [ -n "$vsdk" ] && [ -d "$vsdk" ]; then
    found="VULKAN_SDK=$vsdk"
  elif is_linux && pkg-config --exists vulkan 2>/dev/null; then
    found="pkg-config vulkan $(pkg-config --modversion vulkan 2>/dev/null)"
  elif is_linux && [ -f /usr/include/vulkan/vulkan.h ]; then
    found="/usr/include/vulkan/vulkan.h"
  elif is_linux && [ -n "$deps" ] && { [ -f "$deps/include/vulkan/vulkan.h" ] || [ -f "$deps/lib/libvulkan.so" ] || ls "$deps"/lib/*/libvulkan.so >/dev/null 2>&1; }; then
    found="DGCPAIN_DEPS_ROOT=$deps"
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
  info "  - 也可用同仓库 scripts/setup.ps1（Windows PowerShell 原生）。"
}

print_help() {
  cat <<'EOF'
用法: scripts/setup.sh [--check|--test|--help]

  一键搭建 paint-pc 开发/测试环境（仓库内自包含）。

模式:
  （默认） 探测 + 补缺指引 + 拉 SDK submodule + 构建 paint_pc
  --check  只探测不安装，输出缺项清单；硬依赖缺失时非零退出
  --test   探测 + 构建 + 跑测试门 tests/smoke.sh（headless 离屏 PNG 真实笔迹断言）
  --help   打印本帮助

无人值守: 假定 VS2026（含「使用 C++ 的桌面开发」）已装；Windows 下自动用 vswhere 定位
  VS 并经 vcvars64 进入 MSVC 环境构建，不要求 cl 在 PATH。python 在默认/开发模式为软依赖，
  仅 --test 需要（smoke.sh 解码 PNG）。真缺硬依赖（如未装 VS）时给出指引并非零退出。

依赖: CMake≥3.22 / Ninja / C++ 编译器（g++≥11 或 MSVC）/ Vulkan（真实后端）/ python3（--test）/ git。
口径来源: SDK docs/env/env-setup.md。
EOF
}

# ---------- 动作 ----------
sync_submodule() {
  local root="$1"
  info "同步 SDK submodule…"
  git -C "$root" submodule update --init --recursive
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
      # 用 cmd /c 包裹：先 call vcvars64.bat 注入 MSVC 环境，再执行 cmake 配置 + 构建。
      # VULKAN_SDK 的 Bin 需在 PATH（vulkan-1.lib 链接）。工作目录切到仓库根。
      local root_win vc_win vsdk_bin
      root_win="$(cygpath -w "$root")"
      vc_win="$(cygpath -w "$vcvars")"
      vsdk_bin="${VULKAN_SDK:-}/Bin"
      cmd //c "call \"$vc_win\" && set PATH=$vsdk_bin;%PATH% && cd /d \"$root_win\" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF && cmake --build build -j"
      local rc=$?
      [ $rc -ne 0 ] && return $rc
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

run_test() {
  local root="$1"
  info "跑测试门 tests/smoke.sh…"
  (cd "$root" && bash tests/smoke.sh)
}

# ---------- 主流程 ----------
main() {
  local mode="dev"
  if [ "$#" -gt 1 ]; then err "参数过多：$*（用法: setup.sh [--check|--test]）"; exit 2; fi
  if [ "$#" -eq 1 ]; then
    case "$1" in
      --check) mode="check" ;;
      --test)  mode="test" ;;
      -h|--help) print_help; exit 0 ;;
      *) err "未知参数：$1"; exit 2 ;;
    esac
  fi

  local root
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

  # --test 模式：python 升级为硬依赖（smoke.sh 解码 PNG 需要）。
  [ "$mode" = "test" ] && PY_TEST_NEEDED=1 || PY_TEST_NEEDED=0
  export PY_TEST_NEEDED

  probe_all
  print_check

  if [ "$mode" = "check" ]; then
    [ "$HARD_MISS" -gt 0 ] && { print_guidance; exit 1; }
    exit 0
  fi

  if [ "$HARD_MISS" -gt 0 ]; then
    print_guidance
    err "硬依赖缺失 $HARD_MISS 项。脚本假定 VS2026 / 编译工具已装（Windows 用 MSVC 自动定位、Linux 用 g++）；对真缺项请按指引补装后重跑（无人值守，不做静默安装）。"
    exit 1
  fi

  sync_submodule "$root"
  build_pc "$root"

  if [ "$mode" = "test" ]; then
    run_test "$root"
  fi

  info "paint-pc 环境就绪。运行: ./build/paint_pc（有显示）/ ./build/paint_pc --headless out.png（离屏）"
  exit 0
}

main "$@"
