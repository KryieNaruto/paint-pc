# 修复计划：setup.sh Windows 分支 cmd 报 `'build' 不是内部或外部命令`（cmd 参数含 `/`）

## 0. Bug 报告

- **现象**（Windows Git Bash 真机）：上一轮引号转义修复后（已推送 0935a34），`sh scripts/setup.sh` 在「进入 MSVC 环境（vcvars64）…」一步继续失败：
  ```
  'build' 不是内部或外部命令，也不是可运行的程序或批处理文件。
  ```
- **报告者**：用户主会话现场输出（Windows 真机，与上一轮同一环境）。

## 1. 问题查找（①，已完成）

### 复现
- **CLI 复现入口**：Windows Git Bash `sh scripts/setup.sh` → `build_pc` Windows 分支稳定复现。本机 Linux 无法跑 MSYS2 真机转换，用户现场输出即复现证据（错误形态 `'build'` 与根因机制吻合）。
- 期望输出：cmd 成功执行 `_setup_msvc.bat` → 注入 MSVC 环境 → cmake 配置 + 构建。
- 实际输出：`'build' 不是内部或外部命令`。

### 根因（实证 + 机制分析）
上一轮修复后，`cmd //c` 的参数是 `build/_setup_msvc.bat`（`scripts/setup.sh:340` 返回串，含**正斜杠**）：

```bash
( cd "$root" && cmd //c "build/_setup_msvc.bat" )
```

- 该参数含 `/`，MSYS2 运行时对「含 `/` 的相对路径参数」做路径转换，且 cmd.exe 对**正斜杠相对批处理路径**的解析把 `/` 前的段当命令名 → cmd 报 `'build' 不是内部或外部命令`（即把 `build` 当可执行名查找）。
- 上一轮的回归用例断言 `[ "$cmd_arg" = "build/_setup_msvc.bat" ]` —— **锁死了带斜杠的坏契约**；且 Linux 模拟只建模了 MSYS 引号转义（`"`→`\"`），**未建模 MSYS 对 `/` 的路径转换** → 测试门在 Linux 全绿，真机却失败。这是测试覆盖盲区：模拟未建模「斜杠」这个真机触发点。

> 证据充分性说明：MSYS2 对含 `/` 参数做路径转换 / cmd 对正斜杠批处理解析是已知行为；精确到「MSYS 转换产物形态」或「cmd 解析细节」在本机（Linux）无法观测，但**触发点（参数含 `/`）与观测症状（`'build'`）吻合，且修复方向不依赖精确机制**。

### 影响面
- 单一调用点：`scripts/setup.sh` `build_pc` Windows 分支（`make_msvc_build_bat` 返回串 + 调用处 `cmd //c`）。
- 回归测试 `tests/test_setup_msvc_quotes.sh` 断言 1 编码了坏契约，需同步修正。
- SDK 仓库无 setup.sh；无其它 cmd 调用点。

## 2. 修复方案（②）

核心：**让 cmd 参数彻底无斜杠** —— 裸文件名 + 先把 bash cwd 切到 build 目录。

1. `make_msvc_build_bat` 返回串改：
   ```bash
   printf '_setup_msvc.bat'
   ```
   （不再返回 `build/_setup_msvc.bat`。bat 仍写在 `$root/build/_setup_msvc.bat`，位置不变。）

2. `build_pc` Windows 分支调用改：
   ```bash
   # bat 是裸文件名（无斜杠）→ MSYS2 无路径可转换、cmd 无斜杠可解析。
   # cwd 先切到 build/，cmd 从当前目录找到 _setup_msvc.bat；
   # bat 内部 `cd /d "$root_win"` 会切回仓库根执行 cmake，不依赖起始 cwd。
   if ! ( cd "$root/build" && cmd //c "$cmd_arg" ); then
     return 1
   fi
   ```
   （`make_msvc_build_bat` 已 `mkdir -p "$root/build"`，build/ 必存在。）

### 不变量的论证
- **cmd 参数 `_setup_msvc.bat` 无 `/`、无空格、无引号**：
  - MSYS2 路径转换仅作用于「含 `/`」参数 → 裸文件名不转换，原样传递；
  - cmd 解析无斜杠 → 不会把 `build` 当前缀命令名；
  - cmd 在**当前目录**（= `$root/build`）查找裸 `.bat` → 命中。
- bat 内部 `cd /d "$root_win"` 保证 cmake 在仓库根执行（`-B build` 相对 root），与起始 cwd 解耦。
- 不回退原则：不设 `MSYS2_ARG_CONV_EXCL`（依赖环境变量、且可能被其它环节干扰）；裸文件名是主路径。

### 代码改动（脚本）
**`scripts/setup.sh`**：
- `make_msvc_build_bat` 末尾：`printf 'build/_setup_msvc.bat'` → `printf '_setup_msvc.bat'`，更新函数头注释（新增「cmd 参数必须无斜杠」不变量说明）。
- `build_pc` Windows 分支：`( cd "$root" && cmd //c "$cmd_arg" )` → `( cd "$root/build" && cmd //c "$cmd_arg" )`，更新注释。

## 3. 回归用例设计（先红后绿，③ 硬约束）

**修订** `tests/test_setup_msvc_quotes.sh`（既有回归用例，改为断言**新契约**并对旧带斜杠参数做故障形态对照）：

```bash
# …（cygpath shim、source、msys_escape 不变）…

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
```

**先红后绿步骤**：
1. **红**：先改测试为上述新契约（在**未改**的 `setup.sh` 上跑）→ `make_msvc_build_bat` 返回 `build/_setup_msvc.bat` ≠ `_setup_msvc.bat` → 断言 1 失败（红）；同时对照断言证明旧契约含 `/`（故障形态成立）。
2. **修复**：implement 按本计划改 `setup.sh`。
3. **绿**：重跑 → 全部断言通过（绿）。

## 4. 影响面核对（②/④）
- 仅改 `scripts/setup.sh` 的 `make_msvc_build_bat` 返回串与 `build_pc` Windows 分支调用（+ 注释），及 `tests/test_setup_msvc_quotes.sh` 断言。
- Linux 分支、探测逻辑、`smoke.sh`、`CMakeLists.txt` 不动。
- 行为变化：cmd 的起始 cwd 由 `$root` 改为 `$root/build`；bat 内部 `cd /d "$root_win"` 保证 cmake 仍在仓库根执行 → 对最终产物无影响。
- 遗留上一轮计划文档 `setup-msvc-cmd-quote-fix.md` 的回归测试将被本修订覆盖。

## 5. 验证方式（②）
- **无头回归**：`bash tests/test_setup_msvc_quotes.sh`（Linux 可跑，断言无斜杠不变量 + 故障形态对照）。
- **语法自检**：`bash -n scripts/setup.sh`、`bash -n tests/test_setup_msvc_quotes.sh`。
- **冒烟不回归**：`bash scripts/setup.sh --check`（Linux）探测阶段不受影响。
- **诚实标注**：真机终验仍需 Windows 跑 `sh scripts/setup.sh`；本机仅能验证逻辑与模拟。本 bug 不涉及渲染路径，**离屏图像验证不适用**（CLI 复现 + 回归用例已覆盖硬约束）。

## 6. 风险与健壮性
- cmd 从当前目录解析裸 `.bat` 是标准行为（cwd 优先），风险低。
- `build/` 已 gitignore，临时 bat 残留无害；测试末尾自删。
- 失败路径：vcvars / cmake 失败 → bat 内 `if errorlevel 1 exit /b` 逐级传递 → `if ! ( cd ... )` 捕获 → 脚本非零退出，不掩盖。
- 不引入回退/兜底路径。
