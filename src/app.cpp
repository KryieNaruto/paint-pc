#include "app.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "dgc_paint_c_api.h"
#include "canvas_input.h"
#include "coords.h"
#include "gl_canvas.h"
#include "font_setup.h"
#include "brush_panel.h"
#include "brush_settings.h"
#include "version.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace paint {

struct App::Impl {
    GLFWwindow* window = nullptr;
    int width = 1280;
    int height = 800;
    // 画布清屏色（纸白）。后续替换为 Vulkan 渲染结果的底色。
    float clearColor[4] = {0.96f, 0.95f, 0.91f, 1.0f};

    // 输入桩状态：最近一次指针（鼠标 / 笔）位置与按键。
    bool mouseDown = false;
    double lastX = 0.0, lastY = 0.0;

    // SDK C API + 画布贴图（新增）
    DgcContext* sdk = nullptr;
    GlCanvas* canvas = nullptr;
    int canvasW = 1280, canvasH = 800;
    std::vector<uint8_t> rgba;        // RGBA8, canvasW*canvasH*4
    double lastReadMs = 0.0;          // 读回耗时
    bool strokeActive = false;

    // 笔刷内核基础参数（settingId 0-2，经 paint::ApplyBrushSetting 下发到 DGC_DEFAULT_BRUSH，
    // 见 src/brush_settings.cpp；滑杆初值取自 SDK docs/brush_settings_mapping.md）。
    float brushRadius = 20.0f;
    float brushHardness = 0.8f;
    float brushOpacity = 1.0f;

    // stroke modeler 9 个参数（settingId 4-12），初值取自 core/stroke_predictor.h 的
    // StrokeModelParams 字面默认值（惰性激活：面板未拖动前 SDK 不会创建/注入预测器，
    // 与「默认零回归」一致；一旦拖动任一滑杆才透传到 dgcSetBrushSetting）。
    float wobbleTimeoutMs = 40.0f;
    float wobbleSpeedFloor = 1.31f;
    float minOutputRateHz = 180.0f;
    float endOfStrokeStoppingDistanceMm = 0.1f;
    // bugfix Fix B 联动：SDK 弹簧默认值上调（K/m=400→40000、C/m=40→400，ωn=200 rad/s
    // 临界阻尼，见 core/stroke_predictor.h），此处对齐新默认，避免面板初值偏离 SDK。
    float springMassConstant = 40000.0f;
    float springDragConstant = 400.0f;
    float kalmanProcessNoise = 0.0005f;
    float kalmanMeasurementNoise = 0.004f;
    float predictionIntervalMs = 16.0f;

    // 面板改参统一入口：委托共享函数 paint::ApplyBrushSetting（src/brush_settings.cpp）。
    // 设置固定下发 DGC_DEFAULT_BRUSH（内核基础参数只对内核已建默认笔刷生效；dgcCreateBrush
    // 发号器句柄非 1 会被内核静默 no-op，是「画笔设置无效」bug 的根因 B）。
    // strokeActive==true（笔画进行中）时不下发，避免笔画中途改参只影响后续点、
    // 不回溯当前笔画（见 SDK plan D6-1 风险 R7 的生产约定）。
    int ApplyBrushSetting(int settingId, double value, const char* label) {
        return paint::ApplyBrushSetting(sdk, strokeActive, settingId, value, label);
    }

    // 「画笔参数」面板的改参回调适配（面板绘制抽到 brush_panel.cpp，回调接回本类）。
    static void OnBrushPanelChange(void* user, int settingId, double value, const char* label) {
        auto* impl = static_cast<Impl*>(user);
        impl->ApplyBrushSetting(settingId, value, label);
    }

    // D6-3：笔刷颜色（straight RGBA，取色器默认黑，与内核默认 ColorH/S/V=0 一致）+
    // 垂直同步开关（初始态与 glfwSwapInterval(1) 一致）。
    float brushColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool vsyncOn = true;

    // D6-2：画布缩放状态（消费端视口变换，SDK 零改动，见 docs/plans/D6-2.md §3）。
    // zoom 钳到 [kZoomMin, kZoomMax]；pan 恒为 0（任务书本期不要求平移）。
    float zoom = 1.0f;

    void SetZoom(float z) { zoom = ClampZoom(z); }

    // 鼠标 / 数位笔按键回调：按下 / 抬起转发到 C API（dgcBeginStroke / dgcEndStroke）。
    static void OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
        (void)mods;
        auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
        if (!impl || !impl->sdk) return;
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            // ImGui 面板捕获鼠标（光标悬停在面板/控件上）时按下不开始画布笔画：否则点/拖
            // 面板滑杆会误开笔画（strokeActive=true），且改参回调经 paint::ApplyBrushSetting
            // 命中 strokeActive 门被静默丢弃 →「画笔大小/硬度/透明度设置无效」（根因 C，见
            // src/canvas_input.h）。与 OnScroll 的 WantCaptureMouse 判断同一约定。抬笔不受
            // 门控：画布上已开始的笔画即使拖到面板上抬笔也必须 dgcEndStroke 正常结束。
            if (action == GLFW_PRESS && !paint::ShouldHandleCanvasPointer(ImGui::GetIO().WantCaptureMouse)) {
                return;
            }
            double x, y; glfwGetCursorPos(window, &x, &y);
            // 窗口内容坐标 → 画布像素坐标：非默认分辨率（DPI 缩放 !=1）下 content != canvas，必须按比例换算；
            // 叠加 zoom 逆映射（D6-2），保证缩放态下画笔落点与光标一致。
            int cw = 0, ch = 0; glfwGetWindowSize(window, &cw, &ch);
            double cx, cy;
            MapCursorToCanvasZoomed(x, y, cw, ch, impl->canvasW, impl->canvasH, impl->zoom, &cx, &cy);
            // 越界裁剪（spec §6）：用换算后的画布坐标与画布尺寸比较，坐标系一致；超出则丢弃该点，避免负坐标进 SDK。
            if (cx < 0 || cy < 0 || cx >= impl->canvasW || cy >= impl->canvasH) { impl->strokeActive = false; return; }
            impl->strokeActive = (action == GLFW_PRESS);
            if (impl->strokeActive) {
                int rc = dgcBeginStroke(impl->sdk, (float)cx, (float)cy, 0.5f, 0.f, 0.f);
                if (rc != 0) std::fprintf(stderr, "[paint-pc] beginStroke: %s\n", dgcGetLastError());
            } else {
                int rc = dgcEndStroke(impl->sdk);
                if (rc != 0) std::fprintf(stderr, "[paint-pc] endStroke: %s\n", dgcGetLastError());
            }
        }
    }

    // 鼠标 / 数位笔移动回调：笔画进行中转发到 C API（dgcStrokeTo）。
    static void OnCursorPos(GLFWwindow* window, double x, double y) {
        auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
        if (!impl || !impl->sdk || !impl->strokeActive) return;
        // 与 OnMouseButton 同一映射（内容坐标 → 画布像素，叠加 zoom），保持笔画各点坐标系一致。
        int cw = 0, ch = 0; glfwGetWindowSize(window, &cw, &ch);
        double cx, cy;
        MapCursorToCanvasZoomed(x, y, cw, ch, impl->canvasW, impl->canvasH, impl->zoom, &cx, &cy);
        if (cx < 0 || cy < 0 || cx >= impl->canvasW || cy >= impl->canvasH) return;  // 越界丢弃
        int rc = dgcStrokeTo(impl->sdk, (float)cx, (float)cy, 0.5f, 0.f, 0.f, 0);
        if (rc != 0) std::fprintf(stderr, "[paint-pc] strokeTo: %s\n", dgcGetLastError());
    }

    // 滚轮缩放（D6-2）：注册顺序在 ImGui_ImplGlfw_InitForOpenGL 之前，ImGui 会把本回调存为
    // PrevUserCallbackScroll 并在自身 ImGui_ImplGlfw_ScrollCallback 处理完 io.MouseWheel 后
    // 链式转发到这里（见 imgui_impl_glfw.cpp ImGui_ImplGlfw_ScrollCallback 尾部 chain 调用）——
    // 不需要（也不能）在此再手动调用 ImGui_ImplGlfw_ScrollCallback，否则 io.MouseWheel 会被重复累加。
    // 用 WantCaptureMouse 判断光标是否悬停在 ImGui 面板上，是则滚轮交给面板、不缩放画布
    // （避免 §9 风险3“滚轮与 ImGui 抢事件”）。
    static void OnScroll(GLFWwindow* window, double xoffset, double yoffset) {
        auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
        if (!impl) return;
        if (ImGui::GetIO().WantCaptureMouse) return;
        // 指数感缩放：每滚动一格约 ±10%，clamp 到 [kZoomMin, kZoomMax]。
        const float factor = 1.0f + 0.1f * (float)yoffset;
        impl->SetZoom(impl->zoom * (factor > 0.01f ? factor : 0.01f));
    }

    static void OnFramebufferSize(GLFWwindow* window, int w, int h) {
        auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
        if (!impl) return;
        glViewport(0, 0, w, h);
        impl->width = w; impl->height = h;
        if (impl->sdk) {
            int rc = dgcSetOffscreenSurface(impl->sdk, w, h);
            if (rc != 0) std::fprintf(stderr, "[paint-pc] resize offscreen: %s\n", dgcGetLastError());
            // 同步画布尺寸与读回缓冲，避免 resize 后 readback 缓冲不匹配。
            impl->canvasW = w; impl->canvasH = h;
            impl->rgba.resize((size_t)w * h * 4);
        }
    }
};

App::~App() { shutdown(); }

bool App::init(int width, int height, const char* title) {
    if (m) return true;

    if (!glfwInit()) {
        std::fprintf(stderr, "[paint-pc] glfwInit 失败（headless / 无显示环境？）\n");
        return false;
    }

    // 要求 OpenGL 3.3 core（ImGui OpenGL3 后端下限）。
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // 预留：后续 ImGui 需要 alpha 混合画布。
    glfwWindowHint(GLFW_SAMPLES, 0);

    auto* impl = new Impl();
    m = impl;
    impl->width = width;
    impl->height = height;

    // 版本戳拼进窗口标题：不经 ImGui 渲染管线，哪怕 ImGui 浮层整体没画出来，
    // 标题栏也能看到当前跑的到底是哪个 pc/sdk 提交（排查"是不是构建了旧代码"）。
    std::string titleWithVersion = std::string(title) + "  [pc " + kPcGitSha +
                                    (kPcGitDirty ? "+dirty" : "") + " / sdk " + kSdkGitSha +
                                    (kSdkGitDirty ? "+dirty" : "") + "]";
    impl->window = glfwCreateWindow(width, height, titleWithVersion.c_str(), nullptr, nullptr);
    if (!impl->window) {
        std::fprintf(stderr, "[paint-pc] 创建 GLFW 窗口失败\n");
        shutdown();
        return false;
    }
    glfwMakeContextCurrent(impl->window);
    glfwSwapInterval(1); // vsync
    glfwSetWindowUserPointer(impl->window, impl);
    glfwSetMouseButtonCallback(impl->window, App::Impl::OnMouseButton);
    glfwSetCursorPosCallback(impl->window, App::Impl::OnCursorPos);
    glfwSetFramebufferSizeCallback(impl->window, App::Impl::OnFramebufferSize);
    glfwSetScrollCallback(impl->window, App::Impl::OnScroll);
    // 显式同步初始帧缓冲尺寸：创建时回调是否触发依赖平台事件时机，不能假设必然发生；
    // 非默认分辨率（DPI 缩放 !=1）下保证首帧画布尺寸为真实帧缓冲尺寸。
    int fbw = impl->width, fbh = impl->height;
    glfwGetFramebufferSize(impl->window, &fbw, &fbh);
    impl->width = fbw; impl->height = fbh;
    impl->canvasW = fbw; impl->canvasH = fbh;   // 后续 dgcSetOffscreenSurface/GlCanvas 用真实帧缓冲尺寸

    // ImGui 初始化（GLFW + OpenGL3 后端）。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    // Bug 4：docking 能力开关。枚举值只有 docking 分支 tag（PC_IMGUI_TAG=
    // v1.90.9-docking）编译出的 imgui 才有；release 分支编译会在此报「未声明的
    // 标识符」（先红后绿的"红"）。只开能力开关，不加 DockSpaceOverViewport。
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Bug 1+2：DPI 缩放 + 中文字体加载（抽取到 font_setup，headless 可复用）。
    // 窗口已创建（glfwCreateWindow 在前），可查内容缩放；找不到 CJK 字体时如实
    // stderr 告警（中文方块，不比现状差），不静默吞掉。
    LoadUiFonts(ImGui::GetIO(), impl->window);
    ImGui_ImplGlfw_InitForOpenGL(impl->window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // SDK C API 接入：离屏画布 + 清屏 + 读回缓冲 + GL 画布纹理。
    impl->sdk = dgcCreate();
    if (!impl->sdk) {
        std::fprintf(stderr, "[paint-pc] dgcCreate failed\n");
        shutdown();
        return false;
    }
    int rc = dgcSetOffscreenSurface(impl->sdk, impl->canvasW, impl->canvasH);
    if (rc != 0) { std::fprintf(stderr, "[paint-pc] offscreen surface: %s\n", dgcGetLastError()); shutdown(); return false; }
    rc = dgcClear(impl->sdk, 0.96f, 0.95f, 0.91f, 1.0f);
    if (rc != 0) { std::fprintf(stderr, "[paint-pc] clear: %s\n", dgcGetLastError()); }
    impl->rgba.assign((size_t)impl->canvasW * impl->canvasH * 4, 255);
    impl->canvas = new GlCanvas(impl->canvasW, impl->canvasH);

    // 画笔参数设置不再依赖 dgcCreateBrush 发号器句柄：paint::ApplyBrushSetting 统一走
    // DGC_DEFAULT_BRUSH（见 src/brush_settings.cpp）。init 不再创建笔刷，避免把
    // 「句柄值巧合=1」的隐式依赖埋进 App 侧（根因 B）。

    return true;
}

void App::run() {
    if (!m || !m->window) return;

    while (!glfwWindowShouldClose(m->window)) {
        glfwPollEvents();

        // 画布底色 + 读回贴图（必须先于 ImGui，避免全屏 quad 盖住浮层）
        glViewport(0, 0, m->width, m->height);
        glClearColor(m->clearColor[0], m->clearColor[1], m->clearColor[2], m->clearColor[3]);
        glClear(GL_COLOR_BUFFER_BIT);

        if (m->sdk) {
            auto t0 = glfwGetTime();
            int rc = dgcReadbackPixels(m->sdk, m->rgba.data());   // 检查返回值
            m->lastReadMs = (glfwGetTime() - t0) * 1000.0;
            if (rc != 0) {
                std::fprintf(stderr, "[paint-pc] readback: %s\n", dgcGetLastError());
            } else if (m->canvas) {
                m->canvas->upload(m->rgba.data(), m->canvasW, m->canvasH);
                // D6-2 缩放显示：画布子矩形（画布像素）→ 纹理归一化 UV，交给 GlCanvas 采样。
                // zoom=1 时子矩形=整幅画布（UV [0,0]-[1,1]），等价于缩放前行为。
                double vx = 0.0, vy = 0.0, vw = 0.0, vh = 0.0;
                ComputeCanvasViewport(m->canvasW, m->canvasH, m->zoom, &vx, &vy, &vw, &vh);
                const double u0 = vx / m->canvasW, v0 = vy / m->canvasH;
                const double u1 = (vx + vw) / m->canvasW, v1 = (vy + vh) / m->canvasH;
                m->canvas->draw(m->width, m->height, u0, v0, u1, v1);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 浮层：FPS / 帧时 / 读回耗时 / 画布尺寸。
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("Performance", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Text("Frame: %.2f ms", 1000.0 / (ImGui::GetIO().Framerate > 0 ? ImGui::GetIO().Framerate : 1.0));
        ImGui::Text("Readback: %.2f ms", m->lastReadMs);
        ImGui::Text("Canvas: %dx%d", m->canvasW, m->canvasH);
        ImGui::Text("Version: pc %s%s / sdk %s%s", kPcGitSha, kPcGitDirty ? "+dirty" : "",
                    kSdkGitSha, kSdkGitDirty ? "+dirty" : "");
        ImGui::End();

        // D6-1：画笔参数调试面板——9 个 stroke modeler 滑杆 + 既有 radius/hardness/
        // opacity。改参在 strokeActive==false（两笔画之间）时才经 Impl::ApplyBrushSetting →
        // paint::ApplyBrushSetting（src/brush_settings.cpp）下发，统一走 DGC_DEFAULT_BRUSH
        // （不依赖 dgcCreateBrush 发号器句柄，见根因 B 修复）。滑杆范围取自
        // SDK docs/brush_settings_mapping.md。
        // 面板绘制抽到 brush_panel.cpp（App::run 与无头布局回归测试共用同一函数，
        // Bug 5 的 else-Dummy 占位修复就在那里）。
        BrushPanelParams panel;
        panel.strokeActive = m->strokeActive;
        panel.radius = &m->brushRadius;
        panel.hardness = &m->brushHardness;
        panel.opacity = &m->brushOpacity;
        panel.wobbleTimeoutMs = &m->wobbleTimeoutMs;
        panel.wobbleSpeedFloor = &m->wobbleSpeedFloor;
        panel.minOutputRateHz = &m->minOutputRateHz;
        panel.endOfStrokeStoppingDistanceMm = &m->endOfStrokeStoppingDistanceMm;
        panel.springMassConstant = &m->springMassConstant;
        panel.springDragConstant = &m->springDragConstant;
        panel.kalmanProcessNoise = &m->kalmanProcessNoise;
        panel.kalmanMeasurementNoise = &m->kalmanMeasurementNoise;
        panel.predictionIntervalMs = &m->predictionIntervalMs;
        panel.onChange = &Impl::OnBrushPanelChange;
        panel.user = m;
        DrawBrushParamsPanel(panel);

        // D6-3：显示控制——笔刷颜色 + 垂直同步开关。
        ImGui::SetNextWindowPos(ImVec2(340, 100), ImGuiCond_FirstUseEver);
        ImGui::Begin("Brush / Display");
        // 取色器变更即桥接到 SDK 默认笔刷（DGC_DEFAULT_BRUSH），下一笔画用新色；
        // 注意：颜色变更应在引擎空闲 / 笔画之间调用（与 SDK 侧 setBrushColor 并发
        // caveat 一致），此处 UI 线程回调天然满足（笔画进行中鼠标按下不会触发取色器）。
        if (m->sdk && ImGui::ColorEdit4("Brush Color", m->brushColor)) {
            int rc = dgcSetBrushColor(m->sdk, DGC_DEFAULT_BRUSH, m->brushColor[0],
                                       m->brushColor[1], m->brushColor[2], m->brushColor[3]);
            if (rc != 0) {
                std::fprintf(stderr, "[paint-pc] setBrushColor: %s\n", dgcGetLastError());
            }
        }
        // 垂直同步：SDK 纯离屏无 vsync 概念（见 SDK README「垂直同步（vsync）归属」），
        // 此处纯消费端 present 模式切换，即时生效，不经过 SDK。
        if (ImGui::Checkbox("VSync", &m->vsyncOn)) {
            glfwSwapInterval(m->vsyncOn ? 1 : 0);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(m->vsyncOn ? "(ON — 帧率应被限制到显示器刷新率附近)"
                                           : "(OFF — 帧率应明显偏离/超过刷新率，或抖动)");
        // 人工核验辅助：打印显示器标称刷新率，便于对照上方 Performance 面板的 FPS 读数
        // 做「开 vsync≈刷新率 / 关 vsync 明显偏离」判断，记录见
        // docs/manual-verification/D6-3-vsync-fps.md（该文档需人工在真实设备上填写）。
        if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
            if (const GLFWvidmode* mode = glfwGetVideoMode(mon)) {
                ImGui::Text("Monitor refresh rate (nominal): %d Hz", mode->refreshRate);
            }
        }
        ImGui::End();

        // D6-2：画布操作浮层：缩放（+ / − / 重置 / 滚轮已在 OnScroll 处理）+ 清空。
        ImGui::SetNextWindowPos(ImVec2(670, 100), ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("画布操作", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("缩放: %.2fx", m->zoom);
        if (ImGui::Button("+")) m->SetZoom(m->zoom * 1.25f);
        ImGui::SameLine();
        if (ImGui::Button("-")) m->SetZoom(m->zoom / 1.25f);
        ImGui::SameLine();
        if (ImGui::Button("重置")) m->SetZoom(1.0f);
        ImGui::Separator();
        if (m->sdk && ImGui::Button("清空画布")) {
            // 触发前若有进行中笔画，先强制结束，避免半笔画残留（D6-2 计划 §6）。
            if (m->strokeActive) {
                int endRc = dgcEndStroke(m->sdk);
                if (endRc != 0) std::fprintf(stderr, "[paint-pc] endStroke(clear): %s\n", dgcGetLastError());
                m->strokeActive = false;
            }
            // 顺序为正确性关键：先 flush 排空已提交未合成的笔画，再 clear，避免残留笔迹回写
            // （任务书字面「clear+flush」若反序会导致 clear 后未合成笔画又 composite 回画布）。
            int flushRc = dgcFlush(m->sdk);
            if (flushRc != 0) std::fprintf(stderr, "[paint-pc] flush(clear): %s\n", dgcGetLastError());
            int clearRc = dgcClear(m->sdk, m->clearColor[0], m->clearColor[1], m->clearColor[2], m->clearColor[3]);
            if (clearRc != 0) std::fprintf(stderr, "[paint-pc] clear: %s\n", dgcGetLastError());
            // 下一帧主循环自动 dgcReadbackPixels 显示干净画布，无需在此额外读回。
        }
        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(m->window);
    }
}

void App::shutdown() {
    if (!m) return;
    delete m->canvas; m->canvas = nullptr;
    if (m->sdk) { dgcDestroy(m->sdk); m->sdk = nullptr; }
    if (m->window) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(m->window);
        m->window = nullptr;
    }
    glfwTerminate();
    delete m;
    m = nullptr;
}

} // namespace paint
