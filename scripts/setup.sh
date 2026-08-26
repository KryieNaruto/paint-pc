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

is_linux() { [ "$(uname -s)" = "Linux" ]; }
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

probe_compiler() {
  # Linux/WSL 用 g++（GCC≥11）；Git Bash 下 MSVC 由 VS 开发者命令行提供，另测。
  if is_linux || [ -z "${MSYSTEM:-}" ]; then
    local v vn
    if ! has g++; then
      record "C++ 编译器 (g++)" "硬" "MISS(硬)" "未安装 g++（编译必败）"; HARD_MISS=$((HARD_MISS+1)); return
    fi
    v="$(g++ -dumpfullversion 2>/dev/null || g++ -dumpversion 2>/dev/null || true)"; vn="$(extract_version "$v")"
    if [ -n "$vn" ]; then
      if ver_ge "$vn" "11"; then record "C++ 编译器 (g++)" "硬" "OK" "g++ $vn（GCC ≥ 11）"
      else record "C++ 编译器 (g++)" "硬" "MISS(硬)" "g++ $vn 过旧（需 GCC ≥ 11）"; HARD_MISS=$((HARD_MISS+1)); fi
    else record "C++ 编译器 (g++)" "硬" "OK" "已安装但版本无法确认（请手动确认 ≥ 11）"; fi
  else
    # Git Bash：MSVC cl.exe 需在 VS 开发者命令行内。用 vswhere 探测 VS 安装。
    if ! has cl && ! has vswhere; then
      record "C++ 编译器 (MSVC)" "硬" "MISS(硬)" "未找到 cl.exe / vswhere（需 VS2026 + 「使用 C++ 的桌面开发」）"; HARD_MISS=$((HARD_MISS+1)); return
    fi
    record "C++ 编译器 (MSVC)" "硬" "OK" "MSVC（请确认在 VS 开发者命令行内运行，或经 setup.ps1）"
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
  # smoke.sh 的滤波感知解码断言依赖 python3（测试门实际依赖）。
  if ! has python3; then
    record "python3" "硬" "MISS(硬)" "未安装（--test 的 tests/smoke.sh 依赖 python3 解码 PNG）"; HARD_MISS=$((HARD_MISS+1)); return
  fi
  record "python3" "硬" "OK" "python3 $(python3 --version 2>/dev/null | head -n1 || true)"
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

依赖: CMake≥3.22 / Ninja / C++ 编译器（g++≥11 或 MSVC）/ Vulkan（真实后端）/ python3（smoke）/ git。
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
  # 与 tests/smoke.sh 一致：DGCPAIN_DEPS_ROOT / PC_X11_DEPS_ROOT 可覆盖。
  local deps="${DGCPAIN_DEPS_ROOT:-}"
  local x11="${PC_X11_DEPS_ROOT:-}"
  local extra=()
  [ -n "$deps" ] && extra+=(-DDGCPAIN_DEPS_ROOT="$deps")
  [ -n "$x11" ] && extra+=( -DCMAKE_PREFIX_PATH="$x11" -DCMAKE_C_FLAGS="-I$x11/include" -DCMAKE_CXX_FLAGS="-I$x11/include")
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

  probe_all
  print_check

  if [ "$mode" = "check" ]; then
    [ "$HARD_MISS" -gt 0 ] && { print_guidance; exit 1; }
    exit 0
  fi

  if [ "$HARD_MISS" -gt 0 ]; then
    print_guidance
    err "硬依赖缺失 $HARD_MISS 项。本脚本不静默安装（vs/apt 需交互），请按指引补缺后重跑。"
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
