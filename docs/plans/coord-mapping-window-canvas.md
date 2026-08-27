# 修复计划：非默认分辨率下窗口尺寸变化 → 笔迹落点与光标不一致

- 日期：2026-08-26
- 仓库：paint-pc（`git@github.com:KryieNaruto/paint-pc.git`）
- 分支：`fix/coord-mapping`
- 修复对象：`src/app.cpp` 输入回调的坐标映射 + 新增 `src/coords.{h,cpp}`
- 流水线：bugfix-pipeline（审阅门 ≥80 + 测试门 100 分）

---

## Bug 报告

> 在分辨率不是默认时，我修改了 main 的窗口大小，此时笔画实际位置与现实位置不一致。

即：非默认分辨率（DPI 缩放 ≠ 1，如 Windows 125%/150%）下，修改窗口大小后，鼠标/数位笔画出笔迹的**实际落点**与**光标显示位置**不符（笔迹偏向光标左上方）。

## ① 根因（已实证复现）

### 复现方式（无头 CLI + 离屏渲染）

`paint-pc --headless` 已能无头离屏渲染（SDK Vulkan 离屏 + `dgcExportPNG`，本机验证通过）。

复现场景：窗口**内容区** 1280×800、**帧缓冲/画布** 1600×1000（模拟 scale=1.25 的非默认分辨率），光标在内容坐标 (1000,600)。

- **正确**画布坐标 = `cursor × (canvas / content)` = (1250,750)
- **当前 app** 把 (1000,600) 原样当画布坐标传入 SDK

离屏渲染结果（解码 PNG 定位笔迹像素 bbox）：

| 对象 | 预期中心 | 实际 bbox 中心 | 偏差 |
|---|---|---|---|
| 光标正确位置十字标记 | (1250,750) | (1250,750) | 0 |
| app 当前画出的笔迹 | (1250,750) | **(1000,600)** | **(-250,-150)px** |

复现输出：`correct canvas pos=(1250,750)  app sends=(1000,600)  delta=(-250,-150)px`。

### 根因

`app.cpp` 输入回调把 `glfwGetCursorPos` 返回的**窗口内容坐标**（逻辑屏幕单位，与 `glfwGetWindowSize` 同一空间）**原样**当作**画布像素坐标**传给 SDK：

- `OnMouseButton`（app.cpp:42-47）：`dgcBeginStroke(sdk, (float)x, (float)y, …)`
- `OnCursorPos`（app.cpp:57-63）：`dgcStrokeTo(sdk, (float)x, (float)y, …)`

而画布离屏表面尺寸被设为**帧缓冲尺寸**（`OnFramebufferSize` app.cpp:65-77：`dgcSetOffscreenSurface(sdk, w, h)`）。

在 DPI 缩放 ≠ 1（非默认分辨率）时，**帧缓冲尺寸 = 内容尺寸 × scale**，两者不等。identity 映射 `canvas = cursor` 仅在 scale == 1（默认分辨率 100%）时成立；scale > 1 时笔迹落点整体偏移到 `cursor / scale` 处。

> 用户报告中的「修改 main 的窗口大小」：窗口 resize 触发 `OnFramebufferSize` 按新帧缓冲尺寸重建画布；在 scale ≠ 1 的显示上，内容与帧缓冲持续不同，identity 映射持续错误 → 每笔都偏移。默认分辨率 + 纯 resize 因 scale==1 恰好正确，故用户仅在非默认分辨率下观察到。

### 影响面

同一错误代码路径的三处调用方：

1. `OnMouseButton` 按下 → `dgcBeginStroke`（坐标错误）
2. `OnMouseButton` 抬起 → 仅状态，无坐标（不受影响）
3. `OnCursorPos` 移动 → `dgcStrokeTo`（坐标错误）
4. 两处越界裁剪（app.cpp:44、60）：用**内容坐标**去和**画布尺寸**比较，坐标系不一致（scale≠1 时裁剪边界同样错误）

修复收敛为**一个映射函数**供以上路径复用，避免各改一处引入不一致。

## ② 修复方案

### 新增 `src/coords.h` / `src/coords.cpp`

纯函数，不做任何 GLFW 依赖（可无头单测）：

```cpp
namespace paint {
// 窗口内容坐标 → 画布像素坐标。
// contentW/H：glfwGetWindowSize 内容区尺寸（逻辑单位）；canvasW/H：离屏画布尺寸（帧缓冲像素）。
// 非默认分辨率/DPI 缩放 !=1 时 content != canvas，必须按比例换算，不能当恒等。
// 任一输入尺寸 <= 0（如最小化窗口）时输出 (0,0)，避免除零。
void MapCursorToCanvas(double x, double y,
                       int contentW, int contentH,
                       int canvasW, int canvasH,
                       double* outX, double* outY);
}
```

实现：

```cpp
void MapCursorToCanvas(double x, double y,
                       int contentW, int contentH,
                       int canvasW, int canvasH,
                       double* outX, double* outY) {
    if (contentW <= 0 || contentH <= 0 || canvasW <= 0 || canvasH <= 0) {
        *outX = 0; *outY = 0; return;
    }
    *outX = x * (double)canvasW / (double)contentW;
    *outY = y * (double)canvasH / (double)contentH;
}
```

### 改造 `src/app.cpp`

**a) 初始帧缓冲同步（审阅点 4）**：回调注册在 `glfwCreateWindow` 之后，GLFW 是否在创建时触发 framebuffer 回调依赖平台事件时机（Windows 靠 WM_SIZE 消息泵，不能假设必然发生）。在 `init()` 注册回调后显式查询一次帧缓冲尺寸并同步 `width/height/canvasW/canvasH`，保证非默认分辨率下首帧画布尺寸正确：

```cpp
glfwSetFramebufferSizeCallback(impl->window, App::Impl::OnFramebufferSize);
// 显式同步初始帧缓冲尺寸：创建时回调是否触发依赖平台，不能假设。
int fbw = impl->width, fbh = impl->height;
glfwGetFramebufferSize(impl->window, &fbw, &fbh);
impl->width = fbw; impl->height = fbh;
impl->canvasW = fbw; impl->canvasH = fbh;   // 后续 dgcSetOffscreenSurface/GlCanvas 用真实帧缓冲尺寸
```

**b) 两个输入回调**统一改为：取实时内容尺寸 → `MapCursorToCanvas` 换算 → 用换算后画布坐标做越界裁剪 + 传 SDK。

```cpp
static void OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
    auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
    if (!impl || !impl->sdk) return;
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    double x, y; glfwGetCursorPos(window, &x, &y);
    int cw = 0, ch = 0; glfwGetWindowSize(window, &cw, &ch);
    double cx, cy;
    MapCursorToCanvas(x, y, cw, ch, impl->canvasW, impl->canvasH, &cx, &cy);
    if (cx < 0 || cy < 0 || cx >= impl->canvasW || cy >= impl->canvasH) { impl->strokeActive = false; return; }
    impl->strokeActive = (action == GLFW_PRESS);
    if (impl->strokeActive)
        dgcBeginStroke(impl->sdk, (float)cx, (float)cy, 0.5f, 0.f, 0.f);
    else
        dgcEndStroke(impl->sdk);
}
```

`OnCursorPos` 同构：换算 + 裁剪 + `dgcStrokeTo((float)cx, (float)cy, …)`。

边界与异常路径：
- 最小化窗口（内容或画布尺寸为 0）→ 换算得 (0,0) → 裁剪丢弃，笔画不产生错位点。
- scale<1（如 4K 屏缩到小窗）→ 比例换算同样正确，裁剪防止超界。

## ③ 回归用例设计（先红后绿）

新增 `tests/coord_test.cpp`（独立可执行 `paint_pc_coord_test`，纯函数断言，无头可跑，注册 ctest）。

用例：

| # | 内容尺寸 | 画布尺寸 | 光标(内容) | 期望(画布) | 说明 |
|---|---|---|---|---|---|
| 1 | 1280×800 | 1600×1000 | (1000,600) | (1250,750) | **报告场景** scale=1.25 |
| 2 | 1280×800 | 1280×800 | (500,300) | (500,300) | 默认分辨率恒等必须保持正确 |
| 3 | 1280×800 | 1600×1000 | (1280,800) | (1600,1000) | 右下角边界 |
| 4 | 1920×1080 | 1280×720 | (960,540) | (640,360) | scale<1 反缩 |
| 5 | 0×0 | 1600×1000 | (100,100) | (0,0) | 最小化除零保护 |

**红**：当前代码无 `MapCursorToCanvas`（内联 identity），测试断言期望值 → 失败（identity 返回 (1000,600) ≠ (1250,750)）。
**绿**：实现 `MapCursorToCanvas` + app.cpp 接入后，全部用例通过。

### 离屏渲染图像（硬约束）

新增 `--coord-repro <png>` 模式（复用现有 SDK C API 离屏路径，复用 `HeadlessRun` 的 dgcCreate/offscreen/export 结构）：
- 挂点（审阅点 2）：`main.cpp` 的 `--headless` 分支旁加 `--coord-repro` 分支；`headless.h` 加 `int CoordReproRun(const char* outPng);` 声明；实现放 `headless.cpp`。
1. 建 1600×1000 离屏画布，画十字标记于**正确**位置 (1250,750)；
2. 用 `MapCursorToCanvas(1000,600,1280,800,1600,1000,…)` 得映射点，画笔迹于该点；
3. `dgcExportPNG` 落盘 + 打印映射结果与期望，偏差超 0.5px → 退出非 0。

- **修复前**：映射=identity → 笔迹落 (1000,600)，与十字标记分离（已实证）。
- **修复后**：映射=(1250,750) → 笔迹与十字标记重合，PNG 无偏移，断言通过。

### 构建接线（审阅点 1，缺则验证跑不通）

`CMakeLists.txt`：
1. `add_executable(paint_pc ...)`（CMakeLists.txt:64-69 显式列源）**追加 `src/coords.cpp`**，否则 `paint_pc` 链接缺 `MapCursorToCanvas` 符号。
2. 新增 `enable_testing()` + `add_executable(paint_pc_coord_test tests/coord_test.cpp src/coords.cpp)` + `target_link_libraries(paint_pc_coord_test PRIVATE dgc_paint)`（单测只用纯函数，仍需链接 SDK 以解析 dgc_paint target 的 include/依赖，或仅链接无 GLFW 的最小集——以能链接通为准）+ `add_test(NAME coord_test COMMAND paint_pc_coord_test)`。
3. 测试可经 `ctest --test-dir build` 运行（"0 失败 0 跳过"以此为准）。

## ④ 影响面核对

- 仅改 paint-pc 消费者（`src/app.cpp` + `src/headless.cpp` + `src/main.cpp` + 新增 `src/coords.*` + `CMakeLists.txt`），SDK 不动（C API 语义「入参是画布像素坐标」本就是正确契约）。
- 默认分辨率（scale==1）：换算结果恒等，行为与修复前一致（回归用例 #2 兜底）。
- `dgcBeginStroke`/`dgcStrokeTo` 调用点共 4 处：`app.cpp` 两处（输入路径，需改）＋ `headless.cpp:21,23`（`--headless` 固定坐标自检，**不受影响**，不改）。审阅点 3：上一版"仅 app.cpp 两处"表述不实，已更正。
- `gl_canvas.cpp` 显示路径（全屏 quad 拉伸）不变，画布↔帧缓冲仍 1:1，与换算结果一致。
- **已知既有缺陷（本次不改，文档留痕）**：笔落在画布外时按下早退（app.cpp:44），SDK 侧已 begin 的笔画不会因后续越界/抬起而 endStroke。该问题独立于本坐标 bug，本次修复不改动此行为，避免扩范围；回归用例聚焦坐标映射本身。

## ⑤ 验证方式（无头）

1. 构建：`cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF -DDGCPAIN_DEPS_ROOT=<repo>/sdk/deps/usr -DCMAKE_PREFIX_PATH=/home/qiansenwei/.local/dgc-x11dev/usr` + `cmake --build build -j`。
2. 回归（先红后绿记录在案）：`ctest --test-dir build`（`paint_pc_coord_test` 全用例 0 失败 0 跳过）。
3. 离屏图像：`./build/paint_pc --coord-repro /tmp/coord_after.png` → 断言通过 + PNG 笔迹与十字重合（像素定位验证）。
4. 既有冒烟：`./build/paint_pc --headless /tmp/x.png` 仍出 PNG 且含笔迹像素（复用 smoke.sh 的 PNG 解码断言）。

## 回退原则

按 bugfix-pipeline 不回退原则：不设计「失败改走其它路径」的兜底；依赖（SDK C API、GLFW）齐备，走主路径。除零保护是**正确性处理**而非回退路径。

## 验收标准（可度量、可回溯到 bug 报告）

- [ ] 回归用例 #1（报告场景）先红后绿，代码库中可见测试断言 + 红绿记录。
- [ ] `--coord-repro` PNG 中笔迹与正确位置十字重合（偏差 ≤0.5px），像素级验证。
- [ ] 全部 5 条映射用例 0 失败 0 跳过。
- [ ] 默认分辨率行为不回归（用例 #2）。
