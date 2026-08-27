// paint-pc 消费者入口。
//
// 窗口路径：GLFW 窗口 + OpenGL 画布 + ImGui FPS 浮层，输入转发 SDK C API。
// headless 路径：--headless [out.png] 离屏渲染固定笔迹并导出 PNG（CI/无显示自检）。
// coord-repro 路径：--coord-repro [out.png] 坐标映射复现（十字 vs 笔迹落点）。

#include "app.h"
#include "headless.h"

#include <cstring>

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--headless") == 0) {
        const char* out = (argc >= 3) ? argv[2] : "out.png";
        return paint::HeadlessRun(1280, 800, out);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--coord-repro") == 0) {
        const char* out = (argc >= 3) ? argv[2] : "/tmp/coord_repro.png";
        return paint::CoordReproRun(out);
    }

    paint::App app;
    if (!app.init(1280, 800, "DGCamp Paint - paint-pc")) {
        return 1; // headless / 无显示环境：优雅退出
    }
    app.run();
    return 0;
}
