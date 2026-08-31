# 修复计划：画笔大小/硬度/透明度设置无效（App 层 ImGui 捕获门缺失）

- 日期：2026-08-30
- 仓库：paint-pc（`src/app.cpp` + 新增 `src/canvas_input.{h,cpp}` + `tests/` + `CMakeLists.txt`）
- SDK submodule：aca1169（不动，SDK 侧已修 0-2 落地内核）
- 分支：`fix/imgui-ui-bugs`
- 流水线：bugfix-pipeline（审阅门 ≥80 + 测试门 100 分）

---

## Bug 报告（用户原话，1 条）

1. 画笔大小、硬度、透明度设置无效。

与已交付修复（提交 1bab360，SDK 层 DGC_DEFAULT_BRUSH 句柄问题）**同号但未根治**：即使用户跑的是含 1bab360 的二进制，拖动面板滑杆改参仍无效。本次定位到 App 层另一处根因。

---

## ① 根因（已实证复现）

### 复现方式：无头离屏（复刻 App 鼠标回调逻辑）

写最小 C 复现 `/tmp/repro_ui_gate.cpp`，链接 `paint::ApplyBrushSetting`（src/brush_settings.cpp）+ `paint::MapCursorToCanvasZoomed`（src/coords.cpp）+ `libdgc_paint.a`，**逐行复刻 `src/app.cpp OnMouseButton` 的坐标映射 / 越界裁剪 / strokeActive / dgcBeginStroke 逻辑**，三种路径渲染同一水平线并读回统计：

| 路径 | 行为 | 墨迹(dark) / span |
|---|---|---|
| A default | 不碰面板，直接画线 | 4534 / 256 |
| B nogate（当前 App） | 点滑杆（误开笔画）→ 改 radius=40 → 画线 | **4534 / 256（与 A 逐字节相同）** |
| C gated（修复后） | ImGui 捕获时不处理指针 → 改 radius=40 → 画线 | 21184 / 302（radius 生效） |

离屏图像落盘：`/tmp/repro_ui_gate_default.png`（2378B）、`repro_ui_gate_nogate.png`（2378B，与 default **逐字节相同**）、`repro_ui_gate_gated.png`（6148B，不同）。B 路径稳定复现「改参无效」。

### 根因 C（本次）：App 指针回调不检查 ImGui 鼠标捕获 → UI 交互把改参静默丢弃

- `src/app.cpp` `OnMouseButton`（90 行）与 `OnCursorPos`（115 行）**不检查 `ImGui::GetIO().WantCaptureMouse`**；全文件只有 `OnScroll`（136 行）检查了。
- imgui_impl_glfw.cpp（v1.90.9-docking，`build/_deps/imgui-src/backends/`）的 `ImGui_ImplGlfw_ShouldChainCallback` 返回 `bd->CallbacksChainForAllWindows ? true : (window == bd->Window)` —— 单窗口 App 下**恒为 true**：ImGui 把**每一次**鼠标事件无条件链回 App 的 `OnMouseButton`/`OnCursorPos`，不因光标悬停在面板上而拦截。
- 因此用户点击/拖动「画笔参数」面板滑杆时：
  1. 鼠标按下 → `OnMouseButton` 把面板坐标映射到画布 → 在界内 → `strokeActive=true` + `dgcBeginStroke`（**误开笔画**，且拖动时 `OnCursorPos` 因 strokeActive=true 会沿路 `dgcStrokeTo` 画花）；
  2. 滑杆 `onChange` → `Impl::ApplyBrushSetting` → `paint::ApplyBrushSetting(sdk, strokeActive=true, …)` → 命中 `if (strokeActive) return DGC_OK`（src/brush_settings.cpp:7 生产约定）→ **静默丢弃**；
  3. 抬笔 → `dgcEndStroke`。
- 结果：radius/hardness/opacity（以及 modeler 4-12）在面板上改参**全部无效**——下一笔画仍用默认参数。这解释了「即使 1bab360 已修复 SDK 层，改参依然无效」：SDK 层（T1-T6 全绿）没问题，是 App 层 UI 交互路径把改参挡在门外。

### 影响面

- 出错路径 = `App::Impl::OnMouseButton` / `OnCursorPos`（仅 App 主循环消费，全代码库无其它调用方共用）。
- 影响所有经「鼠标按下 → 改参回调」的 UI 交互：画笔参数面板滑杆（0-2 + modeler 4-12）、取色器、VSync 勾选、画布操作按钮——点它们都会误开画布笔画；滑杆类会连带把改参丢弃。
- 附带现象：点击任何 ImGui 控件会在画布上留下笔迹（误开笔画），本次一并修复。

---

## ② 修复方案

### 改动 1：新增共享指针门控 `src/canvas_input.{h,cpp}`（App 与无头回归测试共用，可测性）

```cpp
// src/canvas_input.h
#pragma once
namespace paint {
// 指针按下是否应由画布笔画处理。ImGui 捕获鼠标（光标悬停在面板/控件上）时返回 false。
// 原因：imgui_impl_glfw（单窗口下 ShouldChainCallback 恒 true）把每次鼠标事件无条件
// 链回消费者回调，App 的 OnMouseButton 不去判断 WantCaptureMouse 就会在点/拖面板滑杆时
// 误开笔画（strokeActive=true），且改参回调经 paint::ApplyBrushSetting 命中 strokeActive
// 门被静默丢弃 →「画笔大小/硬度/透明度设置无效」。与 OnScroll 的 WantCaptureMouse 判断
// 同一约定（src/app.cpp）。返回 true=ImGui 未捕获（画布该响应）；false=ImGui 捕获（不响应）。
bool ShouldHandleCanvasPointer(bool wantCaptureMouse);
}
```

```cpp
// src/canvas_input.cpp
#include "canvas_input.h"
namespace paint {
bool ShouldHandleCanvasPointer(bool wantCaptureMouse) {
    return !wantCaptureMouse;
}
}
```

### 改动 2：`src/app.cpp` 接线门控

- `OnMouseButton`：GLFW_PRESS 分支先判断 `paint::ShouldHandleCanvasPointer(ImGui::GetIO().WantCaptureMouse)`，为 false（ImGui 捕获）则直接 return，**不碰 canvas / strokeActive**。
- **抬笔（GLFW_RELEASE）不受门控**：画布上已开始的笔画即使拖到面板上抬笔也必须 `dgcEndStroke` 正常结束，避免 strokeActive 卡死。
- `OnCursorPos` 不改：已被 `strokeActive` 门挡住（不点面板就不会误开笔画；真笔画划过面板区域属正常连续笔画）。
- 更新 `OnMouseButton` 注释说明根因与约定（与 OnScroll 对齐）。

### 改动 3：`CMakeLists.txt`

- `paint_pc` 的 `add_executable` 加入 `src/canvas_input.cpp`。
- 新增无头回归测试 `paint_canvas_input_gate_test`：链接 `src/canvas_input.cpp` + `src/brush_settings.cpp` + `src/coords.cpp` + `dgc_paint`，注册 ctest。

---

## ③ 回归用例设计（先红后绿）

新增 `tests/test_canvas_input_gate.cpp`（无头，离屏，导出 PNG 供人工对比）。

| 用例 | 步骤 | 断言（绿） |
|---|---|---|
| T1 画布响应 | `ShouldHandleCanvasPointer(false)` | 返回 true（ImGui 未捕获时画布照常处理） |
| T2 捕获门控 | `ShouldHandleCanvasPointer(true)` | 返回 false（ImGui 捕获时不处理——本修复核心） |
| T3 端到端 | 复刻 App 交互：UI 按下（capture=true → 门控不开笔画，strokeActive 保持 false）→ `ApplyBrushSetting(strokeActive=false, radius=40)` → 画水平线 → 读回 | 输出与默认**不同**且 ink >1.3×、span 增大（改参真实生效，不被丢弃） |

**红（先）**：TDD 第一步，`ShouldHandleCanvasPointer` 先按「当前 App 行为」实现（恒返回 true，即不检查捕获）→ **T2、T3 红**（T2 断言 false 得 true；T3 因 UI 按下仍开笔画、strokeActive=true、改参被丢弃，输出与默认逐字节相同）。T1 恒绿（恒 true 也满足「未捕获时响应」）。

**绿（后）**：改 `ShouldHandleCanvasPointer` 为 `return !wantCaptureMouse`，`OnMouseButton` 按下分支接线 → T1-T3 全绿。T3 与 `/tmp/repro_ui_gate` 的 B/C 路径实测一致。

---

## ④ 影响面核对

- 改动 1/2 仅影响 `OnMouseButton` 按下分支（面板取色器/按钮/滑杆的点击不再误开笔画）；`OnCursorPos`、`OnScroll`、其它回调零改动。
- `ShouldHandleCanvasPointer` 只被 `OnMouseButton` 调用；无头测试单独链接它，不影响运行路径。
- SDK submodule 不动（aca1169）；modeler 4-12 在 SDK 层语义不变。
- 既有测试（brush_settings_apply / brush_panel_layout / coord / font / imgui_docking + SDK 全套）不受影响。

---

## ⑤ 验证方式（无头 CLI + 离屏图像）

- 无头 CLI：`ctest` 跑新增 `canvas_input_gate` + 既有 `brush_settings_apply` 全绿；复现命令 `/tmp/repro_ui_gate`（B 路径输出=默认=红证据，C 路径=改参生效=绿证据）留档。
- 离屏图像：新增测试导出 `canvas_gate_ui_press_radius40.png` / `canvas_gate_default.png` 等；复现已导出 `/tmp/repro_ui_gate_{default,nogate,gated}.png` 供前后对比。
- 陈旧二进制核对（收尾）：`pgrep paint_pc` 无残留；重建 `build/paint_pc` 时间戳 ≥ 源码时间戳；窗口标题版本戳 `sdk aca1169`。
