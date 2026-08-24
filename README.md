# paint-pc 消费者

PC UI 消费者仓库：自备 **ImGui / GLFW** 窗口与输入，把鼠标 / 数位板事件送入 SDK C API，再把 `dgcRender` 结果 present 到窗口。

## 拓扑

```
paint-pc ──submodule: sdk/──▶ KryieNaruto/paintDemo（SDK 基座）
```

## 目录

```text
paint-pc/
├── src/
│   └── main.cpp               # 窗口 / 输入 / present（消费者自备，当前为占位）
├── CMakeLists.txt             # add_subdirectory(sdk) + 链接 dgc_paint
├── sdk/                       # git submodule: paintDemo（路径固定 sdk/）
└── .gitmodules
```

## 构建

```bash
cmake --preset host-linux        # 或手动: cmake -S . -B build -DGCPAIN_BUILD_TESTS=OFF
cmake --build --preset host-linux
```

需要 host 工具链（cmake ≥ 3.22 + ninja + g++/clang）。SDK 环境搭建见 [paintDemo docs/env/env-setup.md](https://github.com/KryieNaruto/paintDemo/blob/main/docs/env/env-setup.md)。

## 接入 SDK

```bash
/path/to/paintDemo/scripts/bootstrap-consumer.sh --tag <tag>
git add .gitmodules sdk && git commit -m "chore: submodule paintDemo SDK 到 sdk/"
```

钉 commit / tag，禁止长期漂浮跟踪 main。详见 [docs/git/README.md](https://github.com/KryieNaruto/paintDemo/blob/main/docs/git/README.md)。

## CMake 约定

- 只 `add_subdirectory(sdk)`，只链接 `dgc_paint`。
- 不要链接 SDK 内部 target，不要 include `core/`。
- 唯一 include：`#include "dgc_paint_c_api.h"`。

## 窗口 / 输入（消费者自备）

- 用 GLFW 创建窗口，把 HWND / window 句柄经 `dgcSetSurface` 传入 SDK。
- 鼠标 / 数位板事件转成 C API 调用：
  `dgcBeginStroke` → `dgcStrokeTo`（`isPredicted` 按消费者策略送）→ `dgcEndStroke` → `dgcRender`。
