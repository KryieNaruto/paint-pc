// tests/test_imgui_docking_flag.cpp —— Bug 4 回归用例（先红后绿）。
// 红：PC_IMGUI_TAG=v1.90.9（release 分支）时 ImGuiConfigFlags_DockingEnable 不存在，
//     本文件直接编译报「未声明的标识符」（C2065 / undeclared identifier）。
// 绿：换 PC_IMGUI_TAG=v1.90.9-docking 后，CreateContext 设置该标志位并读回断言。
// 纯 headless：不起窗口 / GL 上下文，只 ImGui::CreateContext()。
#include <imgui.h>

#include <cstdio>

int main() {
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // docking 分支才有的枚举值

    const bool ok = (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0;
    if (!ok) {
        std::fprintf(stderr, "FAIL: ImGuiConfigFlags_DockingEnable not set\n");
        ImGui::DestroyContext(ctx);
        return 1;
    }
    std::printf("PASS: ImGuiConfigFlags_DockingEnable set and readable\n");

    ImGui::DestroyContext(ctx);
    return 0;
}
