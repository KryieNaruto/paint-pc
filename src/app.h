#pragma once

// paint-pc 应用外壳。窗口 / 输入 / present 由消费者实现。
//
// 本轮（SDK C API 未接入）只做：
//   - GLFW 窗口 + OpenGL 清屏（画布纸白底色）
//   - ImGui 浮层（状态 / FPS）
//   - 输入桩：鼠标 / 笔回调记录坐标，供下一轮接 dgcBeginStroke → dgcStrokeTo → dgcEndStroke
//
// present 后端：OpenGL。待 B2-1（Vulkan 渲染后端真实实现）落地后，
// 将 Renderer 替换为 Vulkan swapchain 并贴 dgc_paint 渲染结果；窗口 / 输入结构复用。

namespace paint {

class App {
public:
    App() = default;
    ~App();

    // 初始化窗口 + GL + ImGui。失败返回 false（headless / 无显示环境时优雅退出）。
    bool init(int width, int height, const char* title);
    // 进入主循环，直到窗口关闭。
    void run();
    // 释放资源。
    void shutdown();

private:
    struct Impl;
    Impl* m = nullptr;
};

} // namespace paint
