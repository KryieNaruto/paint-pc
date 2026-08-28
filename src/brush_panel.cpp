// src/brush_panel.cpp —— 「画笔参数 (Brush Params)」面板绘制。
#include "brush_panel.h"

#include <imgui.h>

#include "dgc_paint_c_api.h"  // 仅用 DGC_SETTING_* 常量；头文件自包含（只依赖 <stdint.h>）

namespace paint {

float DrawBrushParamsPanel(const BrushPanelParams& p) {
    ImGui::SetNextWindowPos(ImVec2(12, 100), ImGuiCond_FirstUseEver);
    ImGui::Begin("画笔参数 (Brush Params)");
    // Bug 5：strokeActive==true 时才有的「笔画进行中」提示行，会让面板整体纵向布局
    // 随鼠标按下/抬起跳动一行（立即模式 UI：某帧少画一行，后面所有控件整体上移/下移，
    // 用户「鼠标按下后窗口多一行，按钮下移」）。
    // 修复：else 分支用 ImGui::Dummy 占位同高的不可见块，保证这段纵向占用恒定。
    if (p.strokeActive) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "笔画进行中：改参将在抬笔后生效");
    } else {
        // 与文字行同高的不可见占位（文字高 + 行距，见 ItemSize 语义），两种状态下
        // 这段纵向占用恒定，后面控件位置不再随 strokeActive 跳动（Bug 5 修复）。
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }
    ImGui::SeparatorText("笔刷内核基础参数（存参，暂不作用于默认笔刷）");
    if (ImGui::SliderFloat("半径 radius", p.radius, 1.0f, 100.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_RADIUS, (double)*p.radius, "radius");
    }
    const float firstSliderY = ImGui::GetItemRectMin().y;  // 布局探针：radius 滑杆屏幕 y
    if (ImGui::SliderFloat("硬度 hardness", p.hardness, 0.0f, 1.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_HARDNESS, (double)*p.hardness, "hardness");
    }
    if (ImGui::SliderFloat("不透明度 opacity", p.opacity, 0.0f, 1.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_OPACITY, (double)*p.opacity, "opacity");
    }
    ImGui::SeparatorText("Stroke Modeler 参数（改参后笔迹明显变化）");
    if (ImGui::SliderFloat("抖动消除超时 wobble_timeout_ms", p.wobbleTimeoutMs, 0.0f, 200.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_WOBBLE_TIMEOUT_MS, (double)*p.wobbleTimeoutMs, "wobble_timeout_ms");
    }
    if (ImGui::SliderFloat("抖动消除最低速度 wobble_speed_floor", p.wobbleSpeedFloor, 0.0f, 10.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_WOBBLE_SPEED_FLOOR, (double)*p.wobbleSpeedFloor, "wobble_speed_floor");
    }
    if (ImGui::SliderFloat("最小输出采样率 min_output_rate_hz", p.minOutputRateHz, 20.0f, 500.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_MIN_OUTPUT_RATE_HZ, (double)*p.minOutputRateHz, "min_output_rate_hz");
    }
    if (ImGui::SliderFloat("抬笔停止距离 end_of_stroke_stopping_distance_mm",
                           p.endOfStrokeStoppingDistanceMm, 0.01f, 5.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_END_OF_STROKE_STOPPING_DISTANCE_MM,
                                   (double)*p.endOfStrokeStoppingDistanceMm, "end_of_stroke_stopping_distance_mm");
    }
    // 滑杆范围对齐 SDK 新默认（bugfix Fix B：K/m=40000、C/m=400），默认值须落在范围内。
    if (ImGui::SliderFloat("弹簧质量常量 spring_mass_constant", p.springMassConstant, 1000.0f, 100000.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_SPRING_MASS_CONSTANT, (double)*p.springMassConstant, "spring_mass_constant");
    }
    if (ImGui::SliderFloat("弹簧阻尼常量 spring_drag_constant", p.springDragConstant, 10.0f, 2000.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_SPRING_DRAG_CONSTANT, (double)*p.springDragConstant, "spring_drag_constant");
    }
    if (ImGui::SliderFloat("卡尔曼过程噪声 kalman_process_noise", p.kalmanProcessNoise, 0.00001f, 0.01f, "%.5f")) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_KALMAN_PROCESS_NOISE, (double)*p.kalmanProcessNoise, "kalman_process_noise");
    }
    if (ImGui::SliderFloat("卡尔曼测量噪声 kalman_measurement_noise", p.kalmanMeasurementNoise, 0.0001f, 0.1f, "%.4f")) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_KALMAN_MEASUREMENT_NOISE, (double)*p.kalmanMeasurementNoise, "kalman_measurement_noise");
    }
    if (ImGui::SliderFloat("预测间隔 prediction_interval_ms", p.predictionIntervalMs, 0.0f, 100.0f)) {
        if (p.onChange) p.onChange(p.user, DGC_SETTING_PREDICTION_INTERVAL_MS, (double)*p.predictionIntervalMs, "prediction_interval_ms");
    }
    ImGui::End();
    return firstSliderY;
}

}  // namespace paint
