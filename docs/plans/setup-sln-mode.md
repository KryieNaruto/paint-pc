# paint-pc 生成 Visual Studio 解决方案（--sln 模式），供 VS 内开发

> - 需求：用户反馈「希望使用 cmake 将 sln 工程 cmake 出来，方便在 VS 开发；现在只有构建结果」。
> - 依据：`scripts/setup.sh` / `scripts/setup.ps1`（现有 dev/check/test/help 四模式）；SDK `cmake/msvc_runtime.cmake`（MSVC CRT 守卫，commit b596fae）。
> - 状态：计划

---

## 目标

新增 `scripts/setup.sh --sln`（及 `setup.ps1 -Sln`）模式：用 CMake **Visual Studio 生成器**在独立目录
`build/msvc/` 生成 `paint_pc.sln`，并构建 **Debug 配置验证链接**，让用户在 VS 中打开 sln 直接开发（改码 → F5）。

**不回归**：默认 Ninja 路径（dev / `--check` / `--test` + `smoke.sh`）完全不动。

---

## 现状与约束

- `setup.sh` 仅支持 dev/check/test/help；Windows 下经 `make_msvc_build_bat` 用 `-G Ninja` 构建到 `build/`。
- `find_vs()` 返回 **MSYS 路径**（`/c/...`）；现有代码用 `cygpath -w` 转 Windows 路径（`make_msvc_build_bat` 已用）。
- `find_vs()` 只取 `installationPath`，**未取 `installationVersion`** → 生成器推导需要新函数。
- `.gitignore` = `build/` → `build/msvc/` 天然被忽略。
- MSVC CRT 守卫（SDK `cmake/msvc_runtime.cmake`）是**生成器无关**的 CACHE 设置 → VS 生成器同样受益；
  用户已确认当前 Ninja Debug 链接通过，sln 的 Debug 配置沿用同一 /MD 守卫，无新增 CRT 风险。
- 本机（Linux）无 MSVC / VS 生成器 → **sln 生成与链接只能静态推演 + 真机（用户 Windows）交付验证**；
  Linux 可验证：bash 语法、生成器映射、`--sln` 在 Linux 立即拒绝、默认路径零回归。

---

## 方案

### setup.sh

1. **`find_vs_version()`**：vswhere `-property installationVersion`（与 `find_vs` 同参数
   `-all -prerelease -latest -products '*'`），失败退回安装路径版本段。注意安装路径形如
   `...\Microsoft Visual Studio\18\Insiders`（`<major>\<SKU>`），版本段在 SKU **上一级**，
   故用 `vs_version_from_path`（`basename "$(dirname "$vs")"` → `18`/`2022`/`2019`），
   而非 `basename "$vs"`（那会得到 `Insiders`）。
   ```bash
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
   ```

2. **`vs_version_from_path()`**（安装路径 → 版本段，供兜底；独立函数便于回归测试）：
   ```bash
   vs_version_from_path() {
     local p="$1"
     printf '%s' "$(basename "$(dirname "$p")")"
   }
   ```

3. **`vs_generator_name()`**（版本/年份 → CMake 生成器名）：
   ```bash
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
   ```

4. **`build_pc_sln()`**：
   ```bash
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
     info "构建 Debug 配置验证链接…"
     cmake --build "$root/build/msvc" --config Debug -j
     info "已生成：build/msvc/paint_pc.sln（VS 打开，选 Debug 配置开发）"
   }
   ```
   - `-A x64` + `-G "<gen>"`：VS 生成器产出 `.sln`。
   - `-DCMAKE_GENERATOR_INSTANCE=<windows路径>`：钉到 vswhere 定位的具体 VS 实例（正确处理
     Insiders / 多版本并存，配合既有 `-prerelease` 定位）。
   - `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`：与 SDK 守卫一致，自文档化（显式传参，
     守卫因 `DEFINED` 不再动，结果相同——生成器无关，sln 各配置均 /MD，匹配 shaderc Release lib）。
   - 独立目录 `build/msvc/`；构建 Debug 验证链接。

4. **`main()`**：
   - `case` 增 `--sln) mode="sln" ;;`；「参数过多」报错文案补 `--sln`。
   - 模式解析后立即：
     ```bash
     if [ "$mode" = "sln" ] && is_linux; then
       err "--sln 仅支持 Windows（VS 生成器需 MSVC 工具链）；Linux 请用默认 Ninja 构建。"
       exit 2
     fi
     ```
     （在任何 sync/fetch/probe 之前 → Linux 下立即拒绝，无网络/探测副作用，可被回归测试断言。）
   - 构建分发：`mode=sln` → `build_pc_sln`，否则既有 `build_pc`。
   - 结束提示：sln 模式打印 sln 路径，其余模式保持既有文案。

5. **`print_help()` / 文件头用法注释**：补 `--sln` 行。

### setup.ps1（镜像）

- `-Sln` switch；`Find-VsVersion`（vswhere **`-all`** `-prerelease -latest` installationVersion，
  与 setup.sh 对齐）、`Get-VsGeneratorName`（major 16/17/18 → 生成器名）、`Build-Sln`
  （同参数：`-G <gen> -A x64 -DCMAKE_GENERATOR_INSTANCE=<vs> -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`
  + `cmake --build build\msvc --config Debug`）。
- **审阅补充**：现 `Find-VsInstallation` vswhere 查询缺 `-all`，镜像时一并补齐（`-all -prerelease -latest`）。
- 本机无 pwsh，无法语法检查 → 静态镜像审阅，真机验证（交付说明中标注）。

---

## 回归用例（先红后绿）

新增 `tests/test_sln_mode.sh`（无头可执行；`source scripts/setup.sh` 后断言，主 guard 保证不自动跑 main）：

- 生成器映射：`18.14.36231.0 → Visual Studio 18 2026`、`17.8.0 → 17 2022`、`16.11.0 → 16 2019`、
  `2026 → 18 2026`、`2022 → 17 2022`；空串 / `15.9` → 非零失败。
- 兜底路径提取：`vs_version_from_path "/c/Program Files/Microsoft Visual Studio/18/Insiders" → 18`、
  `"/c/Program Files/Microsoft Visual Studio/2022/Community" → 2022`（版本段在 SKU 上一级）。
- Linux 下 `main --sln` → 非零退出 + 输出含 `sln`/`Windows`（Windows-only 拒绝，且不触发网络/探测）。
- **红（实现前）**：`--sln` 是未知参数（exit 2）、`vs_generator_name`/`vs_version_from_path` 不存在
  （command not found）→ 断言失败。
- **绿（实现后）**：断言全过。

---

## 验证方式

| 项 | 命令 | 标准 |
|---|---|---|
| 回归用例 | `bash tests/test_sln_mode.sh` | 全 PASS（先红后绿可查） |
| bash 语法 | `bash -n scripts/setup.sh` | 0 错误 |
| 默认路径零回归 | `bash tests/smoke.sh` | headless 离屏 PNG + 笔迹像素 PASS |
| 真机 Windows | 用户跑 `scripts/setup.sh --sln` | `build/msvc/paint_pc.sln` 生成 + Debug 构建通过 + VS 打开可开发 |

---

## 变更清单

| 文件 | 变更 |
|---|---|
| `scripts/setup.sh` | +`find_vs_version`/`vs_generator_name`/`build_pc_sln` + `--sln` case + Linux 拒绝 + help/头注释 |
| `scripts/setup.ps1` | +`-Sln` switch + 镜像三函数（静态审阅） |
| `tests/test_sln_mode.sh` | 新增回归（先红后绿） |
| `README.md` | +`--sln` 用法 |
| `docs/plans/setup-sln-mode.md` | 本计划 |

## 风险 / 遗留

- R1：本机无 MSVC / VS 生成器，无法端到端验证 sln 生成与 Debug 链接 → 真机交付验证（用户复跑 `--sln`）。
- R2：`Visual Studio 18 2026` 生成器需较新 CMake 支持；若用户 CMake 过旧，configure 报
  「Could not create named generator」→ 脚本输出清晰指引（升级 CMake）。可在 `--sln` 前做一次
  `cmake --help | grep -F "Visual Studio 18 2026"` 预检并给指引（可选增强，本期先靠报错文本）。
- R3：sln 的 Debug 配置沿用同一 CRT 守卫（/MD），与已确认可用的 Ninja Debug 行为一致，无新增 CRT 风险。
- 不引入回退/兜底路径。
