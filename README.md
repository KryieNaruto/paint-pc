# paint-pc 消费者

PC UI 消费者仓库：自备 **ImGui / GLFW** 窗口与输入，把鼠标 / 数位板事件送入 SDK C API，再把 `dgcRender` 结果 present 到窗口。

**当前状态**：外壳期 —— GLFW 窗口 + OpenGL 清屏画布 + ImGui 浮层 + 输入桩（记录指针坐标）。SDK C API 未接入（等 SDK B1-4 落地）。不绘制真实笔迹。

## 拓扑

```
paint-pc ──submodule: sdk/──▶ KryieNaruto/paintDemo（SDK 基座）
```

## 目录

```text
paint-pc/
├── src/
│   ├── main.cpp               # 入口：初始化 + 主循环
│   ├── app.h / app.cpp        # 窗口 / 输入 / present（GLFW + OpenGL + ImGui）
├── CMakeLists.txt             # add_subdirectory(sdk) + FetchContent(GLFW/ImGui) + 链接 dgc_paint
├── sdk/                       # git submodule: paintDemo（路径固定 sdk/）
└── .gitmodules
```

## 构建

```bash
cmake -S . -B build -DDGCPAIN_BUILD_TESTS=OFF
cmake --build build
```

- 首次构建会 `FetchContent` 从 GitHub 拉 **GLFW 3.3.10** 与 **ImGui v1.90.9**（网络需要）。
- 需要 host 工具链（cmake ≥ 3.22 + ninja + g++/clang + X11/OpenGL dev 头）。SDK 环境搭建见 [paintDemo docs/env/env-setup.md](https://github.com/KryieNaruto/paintDemo/blob/main/docs/env/env-setup.md)。

## 运行

```bash
./build/paint_pc
```

需要显示环境（X11 / Wayland）；无显示时 `glfwInit` 失败并优雅退出（headless 服务器不会崩）。

## 当前外壳行为

- 纸白画布清屏（OpenGL）。
- 左上角 ImGui 浮层：FPS、指针坐标、按下状态。
- 鼠标左键按下 / 移动 / 抬起：只记录坐标（输入桩），**不**转发 C API。

## 接入 SDK（下一轮，等 B1-4 C API 落地）

1. `#include "dgc_paint_c_api.h"`（sdk/ 内，重钉 submodule 到含 C API 的 commit）。
2. `dgcCreate` → 窗口句柄经 `dgcSetSurface` 传入。
3. 输入桩替换为：`dgcBeginStroke` → `dgcStrokeTo`（`isPredicted` 按消费者策略送）→ `dgcEndStroke` → `dgcRender`。
4. present 由 OpenGL 清屏升级为贴 `dgc_paint` 渲染结果（B2-1 Vulkan 落地后走 swapchain）。

## CMake 约定

- 只 `add_subdirectory(sdk)`，只链接 `dgc_paint`。
- 不要链接 SDK 内部 target，不要 include `core/`。
- 唯一 include：`#include "dgc_paint_c_api.h"`。
