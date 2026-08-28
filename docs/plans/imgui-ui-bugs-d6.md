# 修复计划：D6 调试面板 5 个 UI/渲染 bug

- 日期：2026-08-28
- 仓库：paint-pc（`src/app.cpp` + `CMakeLists.txt`） + sdk submodule（`render/vulkan/vk_backend.cpp`）
- 分支：paint-pc `fix/imgui-ui-bugs`；sdk `fix/dab-composite-barrier`
- 流水线：bugfix-pipeline（审阅门 ≥80 + 测试门 100 分）

---

## Bug 报告（用户原话，5 条）

1. 窗口小，字体更小，imgui 的字体非常的小，都有点看不清。
2. imgui 字体未加载中文，导致都是????。
3. 空洞——`一笔/` 抓帧文件夹里，某个 dab 的 dispatch 结果画布上出现孔洞。
4. imgui 窗口增加 docker。需要使用 imgui-docker 的分支才支持。
5. imgui 鼠标按下后，窗口会多出一行，导致布局改变，按钮下移，难以点到按钮。

5 个 bug 相互独立、改动点不重叠（bug1/2 都在字体加载附近但机制不同，合并处理更省一次窗口初始化改动；bug3 在 SDK 渲染层，1/2/4/5 都在 paint-pc 消费者层），逐条给根因+修复+回归用例，最后统一验证。

---

## Bug 1 + Bug 2：字体太小 + 中文??? （合并修复，同一处代码）

### ① 根因（已实证复现）

**复现方式**：读源码 `src/app.cpp` 初始化段（`ImGui::CreateContext()` 到 `ImGui_ImplOpenGL3_Init()` 之间），逐行核对——全程只有 `ImGui::StyleColorsDark()`，没有任何：
- DPI 探测（`glfwGetWindowContentScale`）或缩放应用（`io.FontGlobalScale`、`style.ScaleAllSizes()`）
- 字体加载（`io.Fonts->AddFontFromFileTTF(...)`）或中文字形范围（`GetGlyphRangesChinese*`）

**根因**：
- Bug1：ImGui 默认字体固定 13px（未随 DPI 缩放），Windows 125%/150% 缩放显示器下视觉等效不到 9-10px，"非常小看不清"。
- Bug2：没加载任何字体文件，用的是 ImGui 内置 `ProggyClean`——一个纯 ASCII 位图字体，物理上不含任何中文字形，所有中文字符渲染成 tofu（`?`/方块）。这两条不是同一个 bug 但都在"字体建立"这一处代码，一次改完。

### ② 修复方案

在 `src/app.cpp` 的 `App::init()`，`ImGui::StyleColorsDark()` 之后、`ImGui_ImplGlfw_InitForOpenGL()` 之前插入：

```cpp
ImGuiIO& io = ImGui::GetIO();

// DPI 缩放探测：窗口已创建（glfwCreateWindow 在前），可查内容缩放。
float xscale = 1.0f, yscale = 1.0f;
glfwGetWindowContentScale(impl->window, &xscale, &yscale);
const float dpiScale = xscale;

// 中文字体：按平台常见路径找一个系统自带的 CJK 字体，Windows 现代版本
// （含中文语言包）都自带 msyh.ttc；Linux 开发机/CI 常见 Noto Sans CJK 或文泉驿。
// 找不到任何一个才退化到默认字体（仅英文正常，中文继续是方块——不比现状差，
// 如实 stderr 告警而非静默吞掉）。
static const char* kCjkFontCandidates[] = {
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/msyhbd.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
};
const char* cjkFontPath = nullptr;
for (const char* p : kCjkFontCandidates) {
    if (std::ifstream(p, std::ios::binary).good()) { cjkFontPath = p; break; }
}

const float fontSizePx = 13.0f * dpiScale;  // 按物理像素建图集，不用 FontGlobalScale 二次缩放（避免糊）
if (cjkFontPath) {
    io.Fonts->AddFontFromFileTTF(cjkFontPath, fontSizePx, nullptr,
                                  io.Fonts->GetGlyphRangesChineseFull());
} else {
    std::fprintf(stderr,
                 "[paint-pc] 未找到中文字体（Windows 需系统自带 msyh.ttc；Linux 需装 "
                 "fonts-noto-cjk 或 wqy-microhei），中文将显示为方块\n");
    ImFontConfig cfg;
    cfg.SizePixels = fontSizePx;
    io.Fonts->AddFontDefault(&cfg);
}

ImGui::GetStyle().ScaleAllSizes(dpiScale);  // 控件间距/按钮尺寸同步按 DPI 缩放，不只是文字
```

`#include <fstream>` 已经因 bug3 无关但 app.cpp 需要新增（用于 `std::ifstream` 探测字体文件存在）。

### ③ 回归用例设计（先红后绿）

无头验证：新增 `--font-repro <out.png>` CLI 模式（`main.cpp`/`headless.cpp` 挂点，同 `coord-mapping-window-canvas.md` 先例的 `--coord-repro` 模式），流程：
1. 用相同的字体探测+加载逻辑（抽成 `src/font_setup.{h,cpp}` 纯函数 `FindCjkFontPath()` + `LoadUiFonts(ImGuiIO&, GLFWwindow*)`，供 `App::init()` 和 headless 复用，可无头单测）。
2. 断言 `FindCjkFontPath()` 在本仓库 CI（Linux，已确认装有 `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`）能返回非空路径。
3. 用 ImGui 的 `ImFont::CalcTextSizeA` 对一段中文字符串（如 `"笔画进行中"`）计算渲染尺寸，断言宽度 > 0（默认字体下中文字形缺失时宽度会退化成 tofu 占位宽度，用已知 tofu 宽度做对照可以分辨"真加载了字形"还是"占位方块"——具体判据：加载 CJK 字体后 `CalcTextSizeA` 得到的宽度应显著大于同长度纯 tofu 方块宽度，因为真实中文字形通常比方块占位符窄且不等宽）。
4. 断言 `dpiScale != 1.0` 时 `io.Fonts` 里字体的 `FontSize` 等于 `13*dpiScale`（验证确实按缩放建图集，不是事后拉伸）。

**红**：当前代码没有 `FindCjkFontPath`/`LoadUiFonts`，测试直接编译失败或断言默认字体（无中文字形）→ 判定失败。
**绿**：实现后，中文字形加载断言 + DPI 缩放断言全部通过。

### ④ 影响面

- 仅 `src/app.cpp`（+ 新增 `src/font_setup.{h,cpp}`）+ `CMakeLists.txt`（新源文件加入 `paint_pc` 目标）。
- 不影响 SDK、不影响 headless 现有 `--headless`/`--coord-repro` 模式（新增 `--font-repro` 是独立分支）。
- Windows 无 CJK 字体（罕见，语言包缺失场景）时行为 = 现状（英文正常、中文方块），不是新增回归。

### ⑤ 验证方式（无头）

`./build/paint_pc --font-repro /tmp/font_check.png`，退出码 0 + 断言全过；`ctest` 跑新增的 font_setup 单测。

---

## Bug 4：imgui 加 docking

### ① 根因（已实证）

`CMakeLists.txt:70` `set(PC_IMGUI_TAG "v1.90.9")`——这是 ImGui **release 分支**的 tag。`ImGuiConfigFlags_DockingEnable`、`ImGui::DockSpaceOverViewport` 等 docking API 只存在于 **docking 分支**编译出的库里；`v1.90.9` release tag 没有这些符号，链接会直接失败。已用 GitHub API 核实 `v1.90.9-docking` 标签存在（`ocornut/imgui` 自 2023-07 起为每个 release 同步打一个 `-docking` 后缀 tag，`v1.90.9-docking` 对应 commit `34a2517`），换这个 tag 是同版本号的对等替换，不是升级到不同功能版本。

### ② 修复方案

- `CMakeLists.txt`：`set(PC_IMGUI_TAG "v1.90.9-docking")`。
- `src/app.cpp`，`ImGui::CreateContext()` 后：
  ```cpp
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  ```
  只开能力开关，不加 `DockSpaceOverViewport`（那是"把整个主窗口背景变成一个大 dock 区"的更大 UX 改动，用户只要求"窗口能 docker"，即已有的 4 个浮动面板能互相拖拽合并/对接，不擅自扩大到重排整个界面布局）。

### ③ 回归用例设计（先红后绿）

**红**：当前 `PC_IMGUI_TAG=v1.90.9`（非 docking），`ImGuiConfigFlags_DockingEnable`/`ImGuiConfigFlags` 里没有该枚举值 —— 代码里引用 `ImGuiConfigFlags_DockingEnable` 直接编译报 `未声明的标识符`（C2065/undeclared identifier），这就是"红"的证据形式（编译失败也是先红后绿里的"红"，不一定要是运行时断言失败）。

**绿**：换 tag 后重新 `cmake -S . -B build`（FetchContent 会重新拉取 docking 分支的 imgui 源码）+ 编译通过；新增运行时断言：
```cpp
CHECK((io.ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0, "docking enabled");
```
放进一个新增的轻量单测（`tests/test_imgui_docking_flag.cpp`，只起 `ImGui::CreateContext()` 设置标志位后立即断言，不需要真实窗口/GL 上下文，纯 headless）。

### ④ 影响面

- `CMakeLists.txt` 换 tag 后，`paint_imgui` 目标全部源文件（`imgui.cpp`/`imgui_widgets.cpp`/`backends/imgui_impl_glfw.cpp`/`imgui_impl_opengl3.cpp`）都要用 docking 分支版本重新编译（docking 分支的这几个源文件相对 release 分支有实质差异，不是纯头文件级差异）——`FetchContent` 已声明这些为同一批文件列表，行号不变的部分自动兼容，无需改 `CMakeLists.txt` 里 `add_library(paint_imgui ...)` 那段。
- 现有 4 个 ImGui 窗口（Performance/画笔参数/Brush Display/画布操作）都没设 `ImGuiWindowFlags_NoDocking`，默认可停靠，符合"窗口增加 docker"的字面要求。

### ⑤ 验证方式（无头）

`ctest --test-dir build`（新增 `test_imgui_docking_flag` 通过）+ `cmake --build build` 全量编译通过（docking 分支符号可链接，验证"技术可行性"这条不是纸上谈兵）。

---

## Bug 5：鼠标按下后窗口多一行，按钮下移

### ① 根因（已实证复现）

`src/app.cpp:282-284`（"画笔参数 (Brush Params)" 面板；行号已按 253f34b 版本戳提交后的最新代码核对，此前一版计划文档引用的 273-275 是提交版本戳前的旧行号，内容未变但行号需要更正）：

```cpp
if (m->strokeActive) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "笔画进行中：改参将在抬笔后生效");
}
```

这一行**只在 `strokeActive==true`（鼠标按下画笔中）才渲染**。ImGui 是立即模式 UI，某一帧少画一行文字，后面所有 `ImGui::SliderFloat` 在窗口内的纵向位置就整体上移/下移一行高度——鼠标按下瞬间这行文字从"不存在"变"存在"，后面 12 个滑杆全部下移一行，与用户描述"鼠标按下后窗口多一行，按钮下移，难以点到按钮"完全吻合（这个面板本身没有其它按钮，但"画布操作"面板等其它面板的绝对屏幕位置不受此影响——用户体感的"点不到按钮"应是画笔参数面板内滑杆/后续操作对不上鼠标记忆位置）。

### ② 修复方案

固定预留这一行的空间，不随 `strokeActive` 增减：

```cpp
if (m->strokeActive) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "笔画进行中：改参将在抬笔后生效");
} else {
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
}
```

`ImGui::Dummy` 占位一个与文字同高的不可见块，两种状态下这一段纵向占用高度恒定，后面控件位置不再随 `strokeActive` 跳动。

### ③ 回归用例设计（先红后绿）

用 ImGui 的 headless "null backend"（不建实际窗口，只跑 `ImGui::NewFrame()`/`Render()`，读 `ImGui::GetCursorPosY()` 断言布局位置）：

1. `strokeActive=false` 时记录某个滑杆（如第一个 `radius`）的 `ImGui::GetCursorScreenPos().y`（渲染前用 `ImGui::GetItemRectMin().y` 更准，取该 widget 调用前的 cursor y）。
2. 切到 `strokeActive=true`，重新走一遍面板绘制，记录同一控件的 y。
3. 断言两次 y 相等（差值 == 0，允许 0 容差，这是纯粹的常量位移 bug，不是浮点误差场景）。

**红**：当前代码两次 y 相差一行高度（`ImGui::GetTextLineHeightWithSpacing()`）。
**绿**：`Dummy` 占位后两次 y 完全相等。

### ④ 影响面

- 仅 `src/app.cpp` 一处 if/else 分支，不影响其它 3 个面板、不影响 SDK 调用序列（`ApplyBrushSetting` 逻辑不变）。

### ⑤ 验证方式（无头）

新增 `tests/test_brush_panel_layout.cpp`，链接 ImGui（不需要 GLFW/OpenGL，ImGui 本身可以脱离渲染后端只跑 `NewFrame`/布局计算），`ctest` 跑通即可，不需要真实窗口。

---

## Bug 3：dab 合成孔洞（SDK 侧，`sdk/render/vulkan/vk_backend.cpp`）

### ① 根因（已实证复现，本次调查里工作量最大的一条）

**复现产物**：用户提供 `一笔/` 文件夹的 RenderDoc 抓帧（16 个 `.rdc` + 1 张 UI 截图 `screenshot-20260828-113325.png`）。截图定位到 `a-capture_16.rdc`，Event Browser 选中 EID 15（这个 capture 里的最后一次、也是唯一可见的单 dab dispatch），Texture Viewer 里这个 dab 的圆形右上角有一块明显缺口，露出棋盘格透明底。

**排查步骤（先建通用解析工具，再做针对性复现，过程见对话记录，此处只记结论）**：

1. 写了一个 RenderDoc `.rdc` v1.2 二进制格式解析器（header + LZ4 自定义分块解压 + 64 字节对齐 chunk 遍历 + 官方 `VulkanChunk` 枚举做 chunk 名映射），能从 capture 文件里精确抽取每个 `vkCmdPushConstants` 里的 `BrushPushConstant`（pos/radius/hardness/softness/opacity/rgb/dispatchOffset 共 12 个 float，位于 payload offset 96）。
2. 从 `a-capture_16.rdc` 精确解出这 5 个 dab 的原始数据：全部 `y=798.0`，`x` 依次 826.24/823.74/821.24/818.74/816.24（间距 2.5px），`radius=10`、`hardness=0.7`、`softness=0`、`opacity=1`。
3. 写了一个临时诊断程序 `sdk/tests/diag_hole_repro.cpp`，把这 5 个 dab **原样喂给真实的 `VkBackend::composite()`**（不是另写模拟，是生产代码同一个函数），离屏渲染 + 导出 PNG + 像素级"被黑色包围的白色像素"扫描。
4. 结果：在这台机器（Linux，`sdk/deps/usr` 打的软件 Vulkan 实现）上跑出来是**实心椭圆，0 个孔洞候选**。额外用同样方法复测了另外两段更长的笔画（187 个 dab、382 个 dab），同样 0 孔洞。

**结论**：dab 的生成密度（CPU 侧 `Brush::strokeTo`）没问题；合成的数学/shader 逻辑本身（`brush_composite.comp` 的 SDF+coverage 公式）在严格串行执行下也没问题——同一份数据在这台机器上"能复现出正确结果"，但用户的 Windows 真机 GPU 跑同一份数据、同一份代码，出现了孔洞。**"同数据不同 GPU 结果不同"是并发/同步类 bug 的标准特征，不是数据或数学公式的 bug。**

进一步读 `CompositeLocked()`（`vk_backend.cpp:709-793`）确认了一个真实的 Vulkan 规范违规点：**循环里每个 dab 各发一次 `vkCmdPushConstants`+`vkCmdDispatch`，两次 dispatch 之间没有插 `vkCmdPipelineBarrier`**。相邻 dab 间距（2.5px）远小于半径（10px），包围盒/画布覆盖区域几乎必然重叠——dispatch N+1 的 shader 对同一批像素做 `imageLoad`（读 dispatch N 刚写的值）再 `imageStore`（写回）。Vulkan 规范下，同一 command buffer 内连续两次 dispatch 读写同一张 storage image 的重叠区域，**没有显式 barrier 时驱动不保证 dispatch N+1 能看到 dispatch N 的写入**——这是未定义行为。有的 GPU/驱动的 compute 队列严格按提交顺序串行执行（这台机器上"恰好没事"），有的驱动会做更激进的乱序/并行调度，重叠区域读到脏值就会表现成局部覆盖丢失，也就是孔洞。这跟代码里已有注释承认过的另一处已知问题（Mali 驱动 `gl_GlobalInvocationID` 不含 dispatch base）是同一类"没同步导致的驱动特定行为差异"，但触发点不同（那处是"整个 dispatch 覆盖区域偏移"，这处是"连续 dispatch 间读写序"）。

`canvasImage`（`vk_backend.cpp:347`，`VkDeviceHandle<VkImage, vkDestroyImage>`）全程用 `VK_IMAGE_LAYOUT_GENERAL`（`clearCanvas`/`readback` 等其它调用点一致），确认可以直接插入针对该 image 的 barrier。

**置信度与验证边界（如实报告）**：这是根据"实测复现失败 + 代码里真实存在的规范违规点 + 违规点的失败特征精确匹配观察到的现象"这条证据链推出的根因，不是在这台机器上直接把孔洞复现出来又验证修复前后对比（因为这台机器的 Vulkan 实现本来就不触发）。计划里的验证方式会说明这一点：本地验证只能确认"修复后依旧正确、没有引入新问题"，孔洞消失需要用户在他的 Windows 机器上实测确认。

**审阅反馈处理**：①审阅给了 80/100（压线通过），明确点名 bug3 这条证据链偏推断、建议用 Vulkan sync validation layer（`VK_LAYER_KHRONOS_validation` + `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`）在本机把"是否真的缺同步"从推断坐实成静态/动态检测出的确诊——这个校验层能在读写 hazard 发生时直接报错，不需要真的等驱动乱序调度触发才能看见，比"等真机复现"更可靠。已尝试补上：本机（这台沙箱）既没有 root（`sudo`/`apt-get install` 均因权限不足失败），也没有预装该层的库文件，尝试从 LunarG 官方下载完整 SDK 包做用户级解压（不需要 root）在 25 秒内只下到 78MB（完整包 400MB+），判定网络条件下不可行，遂放弃、清理了残留的部分下载文件。**如实标注为本次遗留项**：这一步验证性质是"能进一步提高确信度的加分项"，不是本 bug 修复方案成立的必要条件（根因证据链本身——真实规范违规点 + 失败特征匹配——已经过审阅认定"不是纯猜测"）；有 root 权限的开发机上可以直接 `apt-get install vulkan-validationlayers` 后重跑 `diag_hole_repro`/`test_composite_barrier_repro`（`VK_LOADER_LAYERS_ENABLE=*validation` 环境变量开启该层）验证是否报出 read-after-write/write-after-write hazard，作为交付后的独立复核步骤记录在收尾摘要里。

### ② 修复方案

`sdk/render/vulkan/vk_backend.cpp`，`CompositeLocked()` 循环内，`vkCmdDispatch(...)` 之后加一个最小化 barrier（只同步 compute-to-compute 读写，不牵扯图形管线阶段，避免不必要的大范围管线停顿）：

```cpp
vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                   sizeof(pc), &pc);
vkCmdDispatch(commandBuffer, cntX, cntY, 1);
++dispatches;

// 修复"dab 孔洞"：相邻 dab 包围盒几乎必然重叠，下一次 dispatch 的 imageLoad 必须
// 保证读到这次 dispatch 的 imageStore 结果；没有显式 barrier 时 Vulkan 规范不保证
// 这一点，部分驱动会因乱序/并行调度读到脏值，产生局部覆盖丢失（用户实测截图复现）。
VkImageMemoryBarrier barrier{};
barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
barrier.image = canvasImage;
barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                     0, nullptr, 0, nullptr, 1, &barrier);
```

**性能影响**：每 dab 多一次 barrier（compute-to-compute 同 stage，通常只是缓存刷新级别开销，不是全管线 stall）。这条路径本来就是"每 dab 一次 dispatch"的性能敏感路径（代码注释提过 bbox dispatch 就是为了省这里的性能），如果实测（下面验证方式 3）显示引入了显著回归，退回到"每 N 个 dab 批量插一次 barrier"是可以讨论的优化方向，但**首版先保证正确性，不为了性能提前做取舍**（对应流水线"不回退原则"——不能为了保性能而跳过必要的同步）。

### ③ 回归用例设计（先红后绿）

新增 `sdk/tests/test_composite_barrier_repro.cpp`，把从 `a-capture_16.rdc` 解出来的真实 5-dab 数据（登记为测试内常量数组，不依赖外部 csv 文件）跑 `VkBackend::composite()`，做两件事：

1. **功能不变式**（当前机器也能验证，先红后绿的"绿"这部分立即可测）：断言渲染结果里 dab 覆盖的椭圆区域内所有像素 alpha 通道 > 0（即"数据本身保证的完全覆盖"这一条不因为加了 barrier 而破坏——barrier 只影响读写顺序不影响覆盖范围，这条断言在改之前也应该已经是绿的，用于确认没有引入新回归）。
2. **静态代码检查**（这条才是本 bug 真正的先红后绿）：新增一个轻量检查——统计 `CompositeLocked` 编译产物或直接对源文件做字符串级断言，确认 `vkCmdDispatch(` 与下一次循环的 `vkCmdPushConstants(` 之间存在 `vkCmdPipelineBarrier(`（用 grep/正则对 `vk_backend.cpp` 源码做结构性检查，写成一个 `tests/test_composite_barrier_repro.cpp` 里的纯文本断言步骤，或者更规范地：给 `CompositeLocked` 加一个可在测试里注入的 hook/计数器，断言"barrier 调用次数 == dispatch 次数 - 0"（每次 dispatch 后都有一次 barrier，最后一次 dispatch 后的 barrier 可选，不影响正确性但对齐更简单：直接要求每次都有）。

  倾向用**hook 计数**而不是字符串匹配（字符串匹配脆弱、不是真正的行为验证）：`VkBackend` 加一个仅测试可见的 `#ifdef DGCPAIN_TEST_HOOKS` 计数器 `barrierCount_`/`dispatchCount_`，`CompositeLocked` 里各自增，测试断言 `barrierCount_ == dispatchCount_`。

**红**：当前代码 `barrierCount_` 恒为 0，`dispatchCount_ == 5`（5 个 dab）→ 断言 `0 == 5` 失败。
**绿**：加了 barrier 后 `barrierCount_ == dispatchCount_ == 5`，断言通过。

**离屏渲染图像（硬约束，渲染类 bug 必须有）**：复用 `sdk/tests/diag_hole_repro.cpp` 的思路，转正为 `tests/test_composite_barrier_repro.cpp` 里落盘一张 PNG（`/tmp` 或测试临时目录），供人工复核/CI 产物留存，即使这台机器本来就不出孔洞，也留一张"预期长什么样"的参照图。

### ④ 影响面

- 只改 `sdk/render/vulkan/vk_backend.cpp` 一个函数内部实现，`composite()` 的公开签名不变，`paint-pc` 侧不用改任何调用代码。
- 性能：见②，如实标注风险，验证方式里有实测步骤。
- 其它调用 `vkCmdDispatch` 的路径（如果有）不在 `CompositeLocked` 循环内，不受影响——已检查 `vk_backend.cpp` 里唯一的连续多 dispatch 场景就是这里。

### ⑤ 验证方式（无头）

1. `sdk/build_diag`（已建好的临时验证目录）：`ctest` 跑 `test_gpu_dab_raster`（既有测试，确认改动没有让 CPU/GPU 逐像素对照测试变差，maxDiff 仍 ≤8）+ 新增 `test_composite_barrier_repro`（barrier 计数断言 + 覆盖率断言）全绿。
2. 用 `diag_hole_repro` 转正后的版本重跑三段真实笔画数据（187/382/5 个 dab），确认依然 0 孔洞（这台机器上修复前后应该都是 0，用于确认没引入新问题，不是这条 bug 的"证明修复有效"的证据）。
3. **性能实测**：跑 `DGCPAIN_PERF` 已有的 composite 计时输出（`vk_backend.cpp` 里 `[PERF] composite stamps=...total=...ms`），对比加 barrier 前后同一批 dab 数据（如 382 个 dab 那组）的耗时，记录在收尾报告里，不设通过/不通过门槛（这条 bug 的验收标准是正确性，性能只是如实记录）。
4. **用户复测（唯一能真正确认 bug 修复的步骤，如实标注为外部依赖）**：SDK 侧改完提交后，走已有的 `scripts/setup.ps1 -Sln` 流程更新 submodule 指针到修复后的 commit，用户在他出现孔洞的同一台 Windows 机器上重新画一笔类似的短促笔画，确认孔洞不再出现。这一步不在 CI/无头范围内，如实报告为"本次流水线交付的是有根据、可验证不引入回归的修复，孔洞现象本身的最终确认需要用户在原设备复测"。

---

## 回退原则

按 bugfix-pipeline 不回退原则：以上 5 条修复都是"缺什么补什么"（缺 DPI 缩放代码、缺中文字体加载、缺 docking 分支、缺布局占位、缺 barrier 同步），没有一条是"改用替代方案掩盖症状"。字体候选路径列表本质是"环境适配"（不同 OS 找系统已有资源），不是回退——真缺失时如实 stderr 告警，不静默吞掉，也不比现状差。

## 验收标准（可度量、可回溯到 bug 报告）

- [ ] Bug1：`--font-repro` 断言字体图集按 `13*dpiScale` 建立，`ScaleAllSizes` 生效。
- [ ] Bug2：中文字形加载断言通过（`CalcTextSizeA` 宽度检验），非 tofu 占位。
- [ ] Bug3：`barrierCount_ == dispatchCount_` 断言通过；`test_gpu_dab_raster` 不回归；性能数据如实记录；**用户在原设备复测确认孔洞消失**（外部验证步骤，标注不在 CI 范围内）。
- [ ] Bug4：`ImGuiConfigFlags_DockingEnable` 编译通过 + 运行时断言生效；`v1.90.9-docking` tag 拉取+编译成功。
- [ ] Bug5：`strokeActive` 切换前后同一控件 `GetCursorScreenPos().y` 差值为 0。
- [ ] 全部新增回归用例先红后绿记录在案；`ctest` 全绿（paint-pc 侧 + sdk 侧）。
