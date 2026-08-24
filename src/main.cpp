// paint-pc 消费者入口。
//
// 当前为外壳：GLFW 窗口 + OpenGL 清屏 + ImGui 浮层，输入桩记录指针。
// 下一轮（SDK B1-4 C API 落地后）：
//   #include "dgc_paint_c_api.h"
//   dgcBeginStroke → dgcStrokeTo → dgcEndStroke → dgcRender
//   并把窗口句柄经 dgcSetSurface 传入 SDK。

#include "app.h"

int main() {
    paint::App app;
    if (!app.init(1280, 800, "DGCamp Paint - paint-pc")) {
        return 1; // headless / 无显示环境：优雅退出
    }
    app.run();
    return 0;
}
