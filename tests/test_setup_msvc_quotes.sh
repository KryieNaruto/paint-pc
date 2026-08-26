#!/usr/bin/env bash
# 回归：setup.sh Windows 分支不再把含引号命令串传给 cmd（Git Bash/MSYS2 会重转义 `"`→`\"`，
# cmd 不认 `\"` → 报 "'\"...\"' 不是内部或外部命令"）。本用例在 Linux 亦可无头运行：
# 模拟 MSYS2 引号转义规则，断言修复后 cmd 收到的参数干净、bat 内容正确。
#
# 本修订新增「cmd 参数无斜杠」不变量：上一轮修复后 cmd 参数为 `build/_setup_msvc.bat`
# （含正斜杠），MSYS2 对含 `/` 参数做路径转换 / cmd 对正斜杠批处理路径解析把 `/` 前段
# 当命令名 → 真机报 `'build' 不是内部或外部命令`。修复：cmd 参数改裸文件名 `_setup_msvc.bat`
# + cwd 切到 build/，使参数彻底无斜杠、无空格、无引号。
set -euo pipefail
cd "$(dirname "$0")/.."

# cygpath shim：本 Linux 开发机无 cygpath，而 make_msvc_build_bat 依赖 cygpath -w。
# 测试只校验 bat 的 call/cmake 行（不校验 cd 行），故最小模拟即可：
#   - 已是 Windows 路径（C:\...）→ 原样返回；
#   - 其它（/home/... 这类测试 root）→ 简单转成 C:\...（精度无关紧要，不用于断言）。
if ! command -v cygpath >/dev/null 2>&1; then
  cygpath() {
    local s="$2"
    case "$s" in
      [A-Za-z]:*) printf '%s' "$s" ;;
      *)          printf 'C:%s' "${s//\//\\}" ;;
    esac
  }
  export -f cygpath
fi

# main guard 使 source 只装载函数不执行 main
source ./scripts/setup.sh

# 模拟 Git Bash/MSYS2 runtime 对「传给原生 Windows 程序」参数的引号重转义。
msys_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

# 对照（断言）：旧代码把含引号命令串整体作为 cmd 参数 → 模拟转义后含 `\"` → 即 cmd 报错形态。
# 此断言守护用例对根因的理解；若故障形态不成立（转义模拟失真）用例直接失败。
old_arg='call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" && cmake'
case "$(msys_escape "$old_arg")" in
  *'\"'*) echo "对照确认：旧模式经 MSYS 转义含 \\\"（本用例的故障形态）" ;;
  *)      echo "FAIL: MSYS 转义模拟失真（旧模式应含 \\\" 才对应真实故障）"; exit 1 ;;
esac

# 对照（断言）：旧契约返回带斜杠路径 → / 是触发点（MSYS 路径转换 / cmd 斜杠解析 → 报 'build'）。
old_arg='build/_setup_msvc.bat'
case "$old_arg" in
  */*) echo "对照确认：旧契约参数含斜杠（本用例的故障触发点）" ;;
  *)   echo "FAIL: 对照失真（旧契约应含 / 才对应真实故障）"; exit 1 ;;
esac

cmd_arg="$(make_msvc_build_bat "$PWD" \
  'C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat' \
  'C:\VulkanSDK\1.4.357.0\Bin')"

# 断言 1：cmd 参数是裸文件名（无斜杠 → MSYS2 无路径可转换、cmd 无斜杠可解析）
[ "$cmd_arg" = "_setup_msvc.bat" ]
# 断言 2：参数不含斜杠（硬性不变量）
case "$cmd_arg" in
  */*) echo "FAIL: cmd 参数含斜杠（真机会报 'build' 不是内部或外部命令）"; exit 1 ;;
esac
# 断言 3：模拟 MSYS 引号转义后不产生 `\"`
case "$(msys_escape "$cmd_arg")" in
  *'\"'*) echo "FAIL: cmd 参数转义后含 \\\""; exit 1 ;;
esac
# 断言 4~6：bat 内容正确（文件内字面引号，不经过参数转义）
grep -q 'call "C:\\Program Files\\Microsoft Visual Studio\\18\\Insiders\\VC\\Auxiliary\\Build\\vcvars64.bat"' build/_setup_msvc.bat
grep -q 'if errorlevel 1 exit /b' build/_setup_msvc.bat
grep -q 'cmake -S . -B build -G Ninja' build/_setup_msvc.bat
grep -q 'cmake --build build -j' build/_setup_msvc.bat

rm -f build/_setup_msvc.bat
echo "PASS: MSVC 构建命令无斜杠，不受 MSYS 路径转换 / cmd 斜杠解析影响"
