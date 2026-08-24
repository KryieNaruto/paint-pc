// paint-pc 占位入口。窗口 / 输入 / present 由消费者实现（ImGui / GLFW 自备）。
// 后续接入 dgc_paint_c_api.h：dgcBeginStroke → dgcStrokeTo → dgcEndStroke → dgcRender。

int main() {
    // TODO: 初始化窗口（GLFW/ImGui），把窗口句柄经 dgcSetSurface 传入 SDK，
    //       将鼠标 / 数位板事件转成 C API 调用并 present 渲染结果。
    return 0;
}
