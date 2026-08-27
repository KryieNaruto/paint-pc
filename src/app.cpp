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

    // D6-3：笔刷颜色（straight RGBA，取色器默认黑，与内核默认 ColorH/S/V=0 一致）+
    // 垂直同步开关（初始态与 glfwSwapInterval(1) 一致）。
    float brushColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool vsyncOn = true;

    // 鼠标 / 数位笔按键回调：按下 / 抬起转发到 C API（dgcBeginStroke / dgcEndStroke）。
    static void OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
        (void)mods;
        auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
        if (!impl || !impl->sdk) return;
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            double x, y; glfwGetCursorPos(window, &x, &y);
            // 窗口内容坐标 → 画布像素坐标：非默认分辨率（DPI 缩放 !=1）下 content != canvas，必须按比例换算。
            int cw = 0, ch = 0; glfwGetWindowSize(window, &cw, &ch);
            double cx, cy;
            MapCursorToCanvas(x, y, cw, ch, impl->canvasW, impl->canvasH, &cx, &cy);
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
        // 与 OnMouseButton 同一映射（内容坐标 → 画布像素），保持笔画各点坐标系一致。
        int cw = 0, ch = 0; glfwGetWindowSize(window, &cw, &ch);
        double cx, cy;
        MapCursorToCanvas(x, y, cw, ch, impl->canvasW, impl->canvasH, &cx, &cy);
        if (cx < 0 || cy < 0 || cx >= impl->canvasW || cy >= impl->canvasH) return;  // 越界丢弃
        int rc = dgcStrokeTo(impl->sdk, (float)cx, (float)cy, 0.5f, 0.f, 0.f, 0);
        if (rc != 0) std::fprintf(stderr, "[paint-pc] strokeTo: %s\n", dgcGetLastError());
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
                m->canvas->draw(m->width, m->height);
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

        // D6-3：显示控制——笔刷颜色 + 垂直同步开关。
        ImGui::SetNextWindowPos(ImVec2(12, 120), ImGuiCond_FirstUseEver);
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
