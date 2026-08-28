// src/brush_panel.h —— 「画笔参数 (Brush Params)」面板绘制。
// 抽成独立翻译单元供 app.cpp 与 tests/test_brush_panel_layout.cpp 共用：回归测试用
// ImGui 无头模式重放两次面板绘制（strokeActive=false / true），断言同一控件 y 不随
// 笔画状态跳动（Bug 5）。
#pragma once

namespace paint {

// 面板输入：指针指向 app.cpp 内部状态（测试传本地变量）。onChange 在滑杆被拖动时
// 回调（settingId 为 dgc_paint_c_api.h 的 DGC_SETTING_* 值；可为空：测试不用）。
struct BrushPanelParams {
    bool strokeActive = false;
    float* radius = nullptr;
    float* hardness = nullptr;
    float* opacity = nullptr;
    float* wobbleTimeoutMs = nullptr;
    float* wobbleSpeedFloor = nullptr;
    float* minOutputRateHz = nullptr;
    float* endOfStrokeStoppingDistanceMm = nullptr;
    float* springMassConstant = nullptr;
    float* springDragConstant = nullptr;
    float* kalmanProcessNoise = nullptr;
    float* kalmanMeasurementNoise = nullptr;
    float* predictionIntervalMs = nullptr;

    void (*onChange)(void* user, int settingId, double value, const char* label) = nullptr;
    void* user = nullptr;
};

// 绘制面板（纯 ImGui 绘制；无 GLFW / SDK 调用序列依赖，可无头单测）。
// Bug 5：strokeActive 提示行必须用 else 分支 ImGui::Dummy 占位，否则该行文字的存在/
// 缺席会让面板整体纵向布局随鼠标按下/抬起跳动一行（立即模式 UI）。
// 返回面板内第一个滑杆（radius）绘制后的屏幕 y（GetItemRectMin().y，在窗口内
// End() 之前测量——End() 会把 LastItemData 恢复成父级，窗口外读不到本窗口的控件）。
// 该返回值供 tests/test_brush_panel_layout.cpp 对比 strokeActive 切换前后的布局。
float DrawBrushParamsPanel(const BrushPanelParams& p);

}  // namespace paint
