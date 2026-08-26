# 修复计划：Windows `setup.sh` 进入 MSVC 环境报 `'\"...vcvars64.bat\"' 不是内部或外部命令`

## 0. Bug 报告

- **现象**：Windows（Git Bash / MSYS2）下运行 `sh scripts/setup.sh`，环境探测全 OK（MSVC 自动定位成功），但在「进入 MSVC 环境（vcvars64）…」一步失败：
  ```
  '\"C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat\"' 不是内部 或外部命令，也不是可运行的程序或批处理文件。
  ```
- **报告者**：用户主会话现场输出（Windows 真机）。

## 1. 问题查找（①，已完成）

### 复现
- **CLI 复现入口**：`sh scripts/setup.sh`（Windows Git Bash），执行到 `build_pc` 的 Windows 分支即稳定复现。**本机为 Linux，无法跑 MSYS2 真机转义**，但用户提供的现场输出即稳定复现证据（报错格式与根因机制完全吻合）。
- 期望输出：成功 `call vcvars64.bat` → 注入 MSVC 环境 → cmake 配置 + 构建。
- 实际输出：cmd.exe 报 `'\"C:\...vcvars64.bat\"' 不是内部或外部命令`。

### 根因（实证）
`scripts/setup.sh:337` 把**含引号命令串整体**作为 `cmd //c` 的参数：

```bash
cmd //c "call \"$vc_win\" && set PATH=$vsdk_bin;%PATH% && cd /d \"$root_win\" && cmake ... && cmake --build build -j"
```

- Git Bash/MSYS2 运行时启动原生 Windows 程序（cmd.exe）时，会按 Windows 命令行规则**重转义每个参数**：参数内的 `"` → `\"`。
- 于是 cmd.exe 收到的命令行中，命令名部分变成了 `\"C:\...\vcvars64.bat\"`（一个以反斜杠引号包围的 token）。
- cmd.exe 的解析器**不认 `\"` 为转义**：`\` 是字面字符、`"` 是引号开关，因此把 `\"C:\...\vcvars64.bat\"` 整体当作可执行文件名去查找 → 找不到 → 报「不是内部或外部命令」。

> 现场输出里的 `'\"C:\Program Files\...vcvars64.bat\"'` 正是 cmd 对「带反斜杠引号命令名」的原样回显，铁证。

### 影响面
- 全仓库唯一调用点：`scripts/setup.sh:337`（grep `cmd //c` 仅此一处）。
- SDK 仓库 `scripts/setup.sh` 不存在；`tests/smoke.sh` 无同类 cmd 调用。→ 影响面单一，仅 Windows 构建分支。
- 受影响路径：所有在 Git Bash/MSYS2 下走 `setup.sh` Windows 分支的消费者（paint-pc 一键搭建 W2）。

## 2. 修复方案（②）

核心：**不再把含引号命令串作为 `cmd //c` 的参数**（那会被 MSYS 重转义）。改为：

1. **生成临时批处理** `build/_setup_msvc.bat`（内容含 `call "C:\...vcvars64.bat"` 等**文件内字面引号**，不经过 bash→cmd 参数传递，无转义问题）。
2. **`cmd //c` 只接收无空格无引号的相对路径参数** `build/_setup_msvc.bat`：
   - 无空格、无引号 → MSYS2 原样传递，不触发 `\"` 转义；
   - cmd 从继承的工作目录（bash cwd = 仓库根）解析该相对路径，仓库根含空格也安全（空格在 cwd 属性里，不在命令行参数里）。
3. **加 main guard**（`${BASH_SOURCE[0]}" = "$0"`），使 setup.sh 可被 source 测试，回归用例得以无头驱动真实函数。

### 代码改动

**`scripts/setup.sh`**：

a) 新增 `make_msvc_build_bat` 函数（放在 `build_pc` 附近）：

```bash
# 生成 MSVC 构建批处理，返回供 `cmd //c` 调用的相对路径参数。
# 关键：cmd 只接收「无空格、无引号」的相对路径 —— Git Bash/MSYS2 会把参数内 `"` 重转义
# 为 `\"`，而 cmd.exe 不认 `\"` 转义，会把 `\"C:\...bat\"` 当命令名报
# "'\"...\"' 不是内部或外部命令"。引号只出现在 bat 文件内容里（文件内字面），不经参数传递。
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
  printf 'build/_setup_msvc.bat'
}
```

b) `build_pc` Windows 分支改为：

```bash
if ! is_linux; then
  vcvars="$(find_vcvars || true)"
  if [ -n "$vcvars" ]; then
    info "进入 MSVC 环境（vcvars64）…"
    local cmd_arg
    # VULKAN_SDK 未设时传空串 → bat 不写 /Bin 脏路径（探测阶段已保证 VULKAN_SDK 已设）。
    cmd_arg="$(make_msvc_build_bat "$root" "$vcvars" "${VULKAN_SDK:+${VULKAN_SDK}/Bin}")"
    # 显式 cd 到仓库根再调 cmd —— bat 是相对路径，必须保证 cmd 的 cwd == $root。
    # （原实现把 `cd /d "$root_win"` 放 cmd 串内不依赖调用方 cwd；本实现等价恢复该性质。）
    if ! ( cd "$root" && cmd //c "$cmd_arg" ); then
      return 1
    fi
    return 0
  fi
  err "未找到 vcvars64.bat，无法进入 MSVC 环境"
  return 1
fi
```

c) 文件末尾 `main "$@"` 改为 main guard：

```bash
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
```

### 设计要点
- **不回退原则**：临时 bat 是主路径（不依赖缺失工具；`cygpath` 为 Git Bash 自带，脚本已依赖）。
- bat 内命令顺序命令独立成行、**无括号块** → cmd 对 LF 行尾批处理的兼容无风险。
- `set -e` 语义：`cmd //c` 失败 → `return 1` → 主流程非零退出（与现行为一致）。
- 临时 bat 落 `build/`（已 gitignore），残留无害；`build_pc` 内不再复用则下次覆盖。

## 3. 回归用例设计（先红后绿，③ 硬约束）

**新增** `tests/test_setup_msvc_quotes.sh`（无头，Linux/Windows 均跑）：

```bash
#!/usr/bin/env bash
# 回归：setup.sh Windows 分支不再把含引号命令串传给 cmd（Git Bash/MSYS2 会重转义 `"`→`\"`，
# cmd 不认 `\"` → 报 "'\"...\"' 不是内部或外部命令"）。本用例在 Linux 亦可无头运行：
# 模拟 MSYS2 引号转义规则，断言修复后 cmd 收到的参数干净、bat 内容正确。
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

cmd_arg="$(make_msvc_build_bat "$PWD" \
  'C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat' \
  'C:\VulkanSDK\1.4.357.0\Bin')"

# 断言 1：cmd 参数就是无空格无引号的相对路径
[ "$cmd_arg" = "build/_setup_msvc.bat" ]
# 断言 2：模拟转义后不产生 `\"` → cmd 不会报"不是内部或外部命令"
case "$(msys_escape "$cmd_arg")" in
  *'\"'*) echo "FAIL: cmd 参数转义后含 \\\""; exit 1 ;;
esac

# 断言 3~5：bat 内容正确（文件内字面引号，不经过参数转义）
grep -q 'call "C:\\Program Files\\Microsoft Visual Studio\\18\\Insiders\\VC\\Auxiliary\\Build\\vcvars64.bat"' build/_setup_msvc.bat
grep -q 'if errorlevel 1 exit /b' build/_setup_msvc.bat
grep -q 'cmake -S . -B build -G Ninja' build/_setup_msvc.bat
grep -q 'cmake --build build -j' build/_setup_msvc.bat

rm -f build/_setup_msvc.bat
echo "PASS: MSVC 构建命令不再被 MSYS 引号转义破坏"
```

**先红后绿步骤**：
1. **红**：在未修复的 `setup.sh` 上运行 → `make_msvc_build_bat` 不存在 → source 后调用报 `command not found` → 用例失败（红）。同时旧模式的故障形态对照（旧代码即把含引号命令串整体传给 cmd）断言存在，证明用例对根因有区分度。
2. **修复**：implement 按本计划改造 setup.sh。
3. **绿**：重跑 → 全部断言通过（绿），且本 Linux 开发机经 cygpath shim 即可达成绿步。

## 4. 影响面核对（②/④）
- 仅改 `scripts/setup.sh` 的 Windows 构建分支 + 新增一个函数 + main guard + 新增测试文件。
- Linux 分支（g++）不改、`find_vs/find_vcvars` 探测不改、`smoke.sh` 不改、`CMakeLists.txt` 不改。
- 直接执行 `setup.sh` 主流程不变（main guard 不影响直接执行）。
- **行为变化（如实声明）**：Windows 构建分支由「cmd 串内 `cd /d "$root_win"`」改为「bash 侧 `( cd "$root" && cmd //c build/_setup_msvc.bat )`」。二者语义等价（都保证 cmake 在仓库根执行、不依赖调用方 cwd），但实现路径不同：原实现靠 cmd 内部 cd（不依赖 bash cwd），新实现显式 `cd "$root"` 后以相对路径调 bat。对从任意目录调用 `setup.sh` 的用户行为一致。

## 5. 验证方式（②）
- **无头回归**：`bash tests/test_setup_msvc_quotes.sh`（Linux 本机可跑，模拟 MSYS 转义验证根因消除）。
- **语法自检**：`bash -n scripts/setup.sh`、`bash -n tests/test_setup_msvc_quotes.sh`。
- **冒烟不回归**：`bash scripts/setup.sh --check`（Linux）仍全绿（探测阶段未动）。
- **诚实标注**：本 bug 的**真机终验**只能在 Windows 跑 `sh scripts/setup.sh`。本机 Linux 仅能验证逻辑与模拟；交付时明确提示用户在 Windows 重跑确认（修复后应正常进入 MSVC 环境完成构建）。
- 本 bug 不涉及渲染路径，**离屏图像验证不适用**（CLI 复现 + 回归用例已覆盖硬约束）。

## 6. 风险与健壮性
- `cygpath` 依赖：Git Bash 自带，脚本已有使用；保持既有依赖面。回归测试经 cygpath shim 在无 cygpath 的开发机仍可跑。
- cwd 不变量：`( cd "$root" && cmd //c "$cmd_arg" )` 显式建立「cmd 的 cwd == 仓库根」前提，消除对调用方 cwd 的隐式依赖。
- VULKAN_SDK 未设：`${VULKAN_SDK:+...}` 传空 → bat 不写 `/Bin` 脏路径（探测阶段本就保证已设，双保险）。
- bat LF 行尾：cmd 对无括号块的顺序命令兼容良好。
- 失败路径：vcvars 调用失败、cmake 配置/构建失败 → `if errorlevel 1 exit /b` 逐级传递 → 脚本非零退出，不掩盖。
- 不引入回退/兜底路径。
