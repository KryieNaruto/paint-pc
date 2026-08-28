// tests/test_brush_panel_layout.cpp —— Bug 5 回归用例（先红后绿）。
// 用 ImGui 无头 "null backend"（不建真实窗口 / GL 上下文，只跑 NewFrame/Render 布局
// 计算），重放「画笔参数 (Brush Params)」面板绘制两次：
//   strokeActive=false 与 strokeActive=true，记录同一控件（面板内最后一个滑杆，
//   预测间隔 prediction_interval_ms）的 GetItemRectMin().y，断言两次相等。
//
// 根因：那一行「笔画进行中」提示文字只在 strokeActive==true 才渲染（立即模式 UI），
// 鼠标按下瞬间该行从"无"变"有"，后面所有滑杆整体下移一行 → 布局跳动。
// 修复（else 分支 ImGui::Dummy 占位同高不可见块）后两次 y 完全相等。
#include "brush_panel.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void CheckLayoutStable(const char* name, float yFalse, float yTrue) {
    const float diff = yTrue - yFalse;
    const bool ok = (diff > -0.0001f && diff < 0.0001f);
    if (ok) {
        std::printf("PASS: %s (y_false=%.3f y_true=%.3f diff=%.4f)\n", name, yFalse, yTrue, diff);
    } else {
        std::printf("FAIL: %s (y_false=%.3f y_true=%.3f diff=%.4f —— 相差一行文字高度)\n",
                    name, yFalse, yTrue, diff);
        ++g_failures;
    }
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;          // 不读/写 imgui.ini，保证窗口位置完全由代码决定
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60.0f;
    // null backend（无渲染器后端）下 Font Atlas 不会由后端 NewFrame 触发构建，
    // NewFrame 的 sanity check 要求 IsBuilt()，这里显式 Build（无 GL 上下文即可）。
    io.Fonts->Build();

    float radius = 20.0f, hardness = 0.8f, opacity = 1.0f;
    float wobbleTimeoutMs = 40.0f, wobbleSpeedFloor = 1.31f, minOutputRateHz = 180.0f;
    float endOfStrokeStoppingDistanceMm = 0.1f, springMassConstant = 400.0f;
    float springDragConstant = 40.0f, kalmanProcessNoise = 0.0005f;
    float kalmanMeasurementNoise = 0.004f, predictionIntervalMs = 16.0f;

    paint::BrushPanelParams p;
    p.radius = &radius;
    p.hardness = &hardness;
    p.opacity = &opacity;
    p.wobbleTimeoutMs = &wobbleTimeoutMs;
    p.wobbleSpeedFloor = &wobbleSpeedFloor;
    p.minOutputRateHz = &minOutputRateHz;
    p.endOfStrokeStoppingDistanceMm = &endOfStrokeStoppingDistanceMm;
    p.springMassConstant = &springMassConstant;
    p.springDragConstant = &springDragConstant;
    p.kalmanProcessNoise = &kalmanProcessNoise;
    p.kalmanMeasurementNoise = &kalmanMeasurementNoise;
    p.predictionIntervalMs = &predictionIntervalMs;

    // 第 1 次：strokeActive=false（鼠标未按下）。
    p.strokeActive = false;
    ImGui::NewFrame();
    const float yStrokeInactive = paint::DrawBrushParamsPanel(p);  // radius 滑杆屏幕 y
    ImGui::Render();

    // 第 2 次：strokeActive=true（鼠标按下，提示行出现）。
    p.strokeActive = true;
    ImGui::NewFrame();
    const float yStrokeActive = paint::DrawBrushParamsPanel(p);
    ImGui::Render();

    CheckLayoutStable("brush-panel layout stable across strokeActive",
                      yStrokeInactive, yStrokeActive);

    ImGui::DestroyContext();

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILED %d\n", g_failures);
    return 1;
}
