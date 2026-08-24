#include "app.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>

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

    // 鼠标 / 数位笔按键回调：记录按下 / 抬起，为下一轮转发到 C API 做准备。
    static void OnMouseButton(GLFWwindow* window, int button, int action, int mods) {
        (void)mods;
        auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
        if (!impl) return;
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            impl->mouseDown = (action == GLFW_PRESS);
            if (impl->mouseDown) {
                glfwGetCursorPos(window, &impl->lastX, &impl->lastY);
                // TODO(SDK C API 接入后): dgcBeginStroke(ctx, x, y, pressure, tiltX, tiltY);
            } else {
                // TODO(SDK C API 接入后): dgcEndStroke(ctx);
            }
        }
    }

    // 鼠标 / 数位笔移动回调：记录位置，为下一轮转发到 C API 做准备。
    static void OnCursorPos(GLFWwindow* window, double x, double y) {
        auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
        if (!impl) return;
        impl->lastX = x;
        impl->lastY = y;
        // TODO(SDK C API 接入后): if (impl->mouseDown) dgcStrokeTo(ctx, x, y, pressure, tiltX, tiltY, 0);
    }

    static void OnFramebufferSize(GLFWwindow* window, int w, int h) {
        (void)window;
        glViewport(0, 0, w, h);
        // TODO(SDK C API 接入后): dgcResize(ctx, w, h);
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

    // ImGui 初始化（GLFW + OpenGL3 后端）。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(impl->window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    return true;
}

void App::run() {
    if (!m || !m->window) return;

    while (!glfwWindowShouldClose(m->window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 画布底色清屏（OpenGL 外壳期；待 B2-1 Vulkan 落地后替换为渲染结果贴图）。
        glViewport(0, 0, m->width, m->height);
        glClearColor(m->clearColor[0], m->clearColor[1], m->clearColor[2], m->clearColor[3]);
        glClear(GL_COLOR_BUFFER_BIT);

        // 浮层：状态 + FPS。
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("DGCamp Paint", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("paint-pc 外壳 · SDK C API 未接入");
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Text("指针: (%.0f, %.0f) %s", m->lastX, m->lastY,
                    m->mouseDown ? "按下" : "抬起");
        ImGui::End();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(m->window);
    }
}

void App::shutdown() {
    if (!m) return;
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
