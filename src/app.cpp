#include "app.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "dgc_paint_c_api.h"
#include "coords.h"
#include "gl_canvas.h"

#include <cstdint>
#include <cstdio>
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

    impl->window = glfwCreateWindow(width, height, title, nullptr, nullptr);
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
        ImGui::End();

        // 画布操作浮层：缩放（+ / − / 重置 / 滚轮已在 OnScroll 处理）+ 清空。
        ImGui::SetNextWindowPos(ImVec2(12, 140), ImGuiCond_Once);
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
