# 修复计划：画笔设置（粗细/硬度/透明度）+ modeler 参数均无效

- 日期：2026-08-28
- 仓库：paint-pc（`src/app.cpp` + 新增 `src/brush_settings.{h,cpp}` + `tests/` + `CMakeLists.txt`）
- SDK submodule：aca1169（已含 Bug #1/#3 修复，本次不改 SDK）
- 分支：`fix/imgui-ui-bugs`
- 流水线：bugfix-pipeline（审阅门 ≥80 + 测试门 100 分）

---

## Bug 报告（用户原话，2 条）

1. 画笔设置无效，粗细，硬度，透明度都无效。
2. modeler 参数设置无效。结果无变化。

两条都指向「调试面板改了参数，画出来的笔迹不变」。

---

## ① 根因（已实证复现）

### 复现方式：无头离屏（模拟 App 调用序列）

写最小 C 复现 `/tmp/repro_app_brush.cpp`（链接 `libdgc_paint.a`），精确复刻 `src/app.cpp` 的调用序列：
`dgcCreate → dgcSetOffscreenSurface → dgcClear → dgcCreateBrush(ctx, nullptr) → dgcSetBrushSetting(ctx, 句柄, …) → dgcBeginStroke/StrokeTo/EndStroke → dgcFlush → dgcReadbackPixels`。
对 `RADIUS=40`、`PREDICTION_INTERVAL_MS=100/1` 分别渲染并统计墨迹，对照默认输出。

### 根因 A：陈旧二进制（用户实际跑的 paint_pc 链接的是修复前的 SDK）

- paint-pc 提交 `db9acb0`（submodule → aca1169，SDK 修复 0-2 死存储 + modeler 塌缩）提交时间 **02:33:36**；
- `build/paint_pc` 二进制构建时间 **02:09:53**，**早于** submodule 前移——链接的是修复前 SDK。
- 修复前（aca1169 之前）SDK 行为：
  - `dgcSetBrushSetting(0-2)` 只写 `Impl::brush_settings` 表（**全代码库无读取点，死存储**），渲染仍用默认参数 → 粗细/硬度/透明度全无效（Bug #1 原样）。
  - modeler 激活后点流全 0 时间戳，`PositionModeler` 钉死在首点 → 笔画塌缩成点（Bug #3），改参数看不出区别（Bug #2 原样）。
- 本次排查**已重建** `build/paint_pc`（03:09，含 aca1169）；重建后两条均不再复现（见下「验证」），实证：用户报告的 bug 是**陈旧二进制**（SDK 侧修复早已合入，未重建生效）。

### 根因 B：App 侧潜在缺陷——画笔设置句柄依赖 `dgcCreateBrush` 发号器巧合（本次代码修复点）

- `src/app.cpp` `Impl::ApplyBrushSetting` 把设置下发到 `impl->brush`（`dgcCreateBrush` 返回的自增句柄），而不是 SDK 的固定默认笔刷 `DGC_DEFAULT_BRUSH`。
- SDK 契约（`sdk_api/dgc_paint_c_api.cpp` 注释 + `BrushKernel::setBrushSetting`）：**内核基础参数(0-2)只对内核已建笔刷生效**（引擎 `start()` 建的唯一默认笔刷 = 句柄 1 = `DGC_DEFAULT_BRUSH`）；`dgcCreateBrush` 发号器返回的非 1 句柄，`brushes.find(handle)` 找不到 → **静默 no-op**。
- 当前 App 碰巧**第一次** `dgcCreateBrush` 返回 1（发号器从 1 起），等于 `DGC_DEFAULT_BRUSH`，所以重建后能工作——但这纯属巧合：只要再建一个笔刷（句柄变 2）或 SDK 发号器初值变化，设置就**静默丢失**。复现证据（`/tmp/repro_app_brush`）：
  - 句柄 1（当前 App 路径）：`RADIUS=40` → dark 4534→21184、span 256→302（生效）。
  - 句柄 2（第二个 dgcCreateBrush）：输出与默认**逐字节相同**（no-op，设置被静默丢弃）。

### 影响面

- 根因 A（陈旧二进制）：只影响「用户当前跑的进程/二进制」这一交付核对项，代码无改动；重建即解决。
- 根因 B：出错路径 = `App::Impl::ApplyBrushSetting`（仅由面板 `OnBrushPanelChange` 回调触发）。modeler(4-12) 是 context 级单例、与句柄无关，不受 B 影响，但为统一语义一并走 `DGC_DEFAULT_BRUSH`。全代码库无其它调用方共用该句柄。

---

## ② 修复方案

### 改动 1：新增共享设置下发入口 `src/brush_settings.{h,cpp}`（App 与无头回归测试共用，可测性）

```cpp
// src/brush_settings.h
#pragma once
#include "dgc_paint_c_api.h"
namespace paint {
// 画笔/Stroke Modeler 参数下发统一入口（App 面板 + 无头回归测试共用）。
// 生产约定：strokeActive==true（笔画进行中）不下发，改参只在两笔画之间生效。
// 句柄固定 DGC_DEFAULT_BRUSH：内核基础参数(0-2)只对内核已建默认笔刷生效，
// dgcCreateBrush 发号器句柄（非 1）会被内核静默忽略（BrushKernel::setBrushSetting no-op），
// 是"画笔设置无效"类 bug 的根因之一。返回 dgcSetBrushSetting 返回码；跳过时返回 DGC_OK。
int ApplyBrushSetting(DgcContext* sdk, bool strokeActive, int settingId, double value, const char* label);
}
```

```cpp
// src/brush_settings.cpp
#include "brush_settings.h"
#include <cstdio>
namespace paint {
int ApplyBrushSetting(DgcContext* sdk, bool strokeActive, int settingId, double value, const char* label) {
    if (sdk == nullptr) return DGC_ERR_NULL_CONTEXT;
    if (strokeActive) return DGC_OK;  // 生产约定：两笔画之间改参
    int rc = dgcSetBrushSetting(sdk, DGC_DEFAULT_BRUSH, settingId, value);
    if (rc != DGC_OK) {
        std::fprintf(stderr, "[paint-pc] setBrushSetting(%s): %s\n", label, dgcGetLastError());
    }
    return rc;
}
}
```

### 改动 2：`src/app.cpp` 接线共享入口 + 移除发号器句柄依赖

- `App::Impl::ApplyBrushSetting(int,double,const char*)` 改为委托 `paint::ApplyBrushSetting(sdk, strokeActive, settingId, value, label)`。
- 删除 `impl->brush` 成员与 `init()` 里的 `dgcCreateBrush` 调用（其句柄不再用于下发，保留只会继续埋「句柄值巧合」的雷）。
- 更新 D6-1 相关注释：设置统一走 `DGC_DEFAULT_BRUSH`。

### 改动 3：`CMakeLists.txt`

- `paint_pc` 的 `add_executable` 加入 `src/brush_settings.cpp`。
- 新增无头回归测试 `paint_brush_settings_apply_test`：链接 `src/brush_settings.cpp` + `dgc_paint`，注册 ctest。

---

## ③ 回归用例设计（先红后绿）

新增 `tests/test_brush_settings_apply.cpp`（无头，离屏，导出 PNG 供人工对比）。测试通过 **App 的共享设置入口** `paint::ApplyBrushSetting` 下发，验证"面板改参必须真实改变渲染输出"。

| 用例 | 步骤 | 断言（绿） |
|---|---|---|
| T1 半径生效 | `ApplyBrushSetting(radius=40)` 画水平线 vs 默认 | 输出不同且墨迹 >1.3×、span 增大 |
| T2 硬度生效 | `ApplyBrushSetting(hardness=1.0)` | 输出与默认不同 |
| T3 不透明度生效 | `ApplyBrushSetting(opacity=0.1)` | 近黑墨迹 < 默认的一半（变淡） |
| T4 modeler 生效 | `ApplyBrushSetting(prediction_interval_ms=100)` vs `=1` 画波浪线 | 两次输出不同（红阶段自始为绿：modeler 是 context 级单例、与句柄无关，即使红阶段传 handle2 也生效） |
| T5 strokeActive 跳过 | `ApplyBrushSetting(strokeActive=true, radius=40)` | 返回 `DGC_OK`，输出与默认相同（两笔画之间才生效的生产约定） |
| T6 句柄免疫 | 先 `dgcCreateBrush` 两次（句柄到 2）再 `ApplyBrushSetting(radius=40)` | 输出仍改变（证明 App 路径不依赖发号器句柄值） |

**红（先）**：TDD 第一步，`ApplyBrushSetting` 先按「当前 App 行为」移植（带 `DgcBrush handle` 参数、测试传非默认句柄 2）→ 内核 no-op → **T1/T2/T3/T6 全红**（输出与默认逐字节相同）。T4（modeler）因是 context 级单例、与句柄无关，红阶段自始为绿——这正好证明 modeler 本就不受句柄巧合影响。该红与 `/tmp/repro_app_brush` 实测句柄 2 no-op 一致。

**绿（后）**：改 `ApplyBrushSetting` 固定 `DGC_DEFAULT_BRUSH`（去掉句柄参数）→ T1-T6 全绿。

---

## ④ 影响面核对

- 改动 1/2 仅影响 `ApplyBrushSetting` 一个调用点（面板 `OnBrushPanelChange`）；`brush_panel.cpp` 绘制逻辑零改动。
- 删除 `impl->brush`：grep 确认全代码库（app.cpp 内）无其它使用。
- SDK submodule 不动（已 aca1169）；modeler 路径语义不变（context 级单例，`DGC_DEFAULT_BRUSH` 合法）。
- 既有测试（coord/font/brush_panel_layout/imgui_docking + SDK 全套）不受影响。

---

## ⑤ 验证方式（无头 CLI + 离屏图像）

- 无头 CLI：`ctest` 跑新增 `paint_brush_settings_apply` + SDK `test_brush_setting_applies` / `test_modeler_param_changes_output` / `test_modeler_stroke_renders` 全绿。
- 离屏图像：回归测试导出 PNG（`brush_apply_radius40.png` / `brush_apply_default.png` 等）供前后对比与人工复核。
- 陈旧二进制核对（收尾）：`pgrep paint_pc` 无残留进程；`build/paint_pc` 时间戳 ≥ SDK 源码时间戳；窗口标题版本戳 `sdk aca1169`。
- 复现命令（交付留档）：`/tmp/repro_app_brush` 输出对照（句柄 2 no-op 红 → `DGC_DEFAULT_BRUSH` 生效绿）。
