#pragma once
namespace paint {
// 指针按下是否应由画布笔画处理。ImGui 捕获鼠标（光标悬停在面板/控件上）时返回 false。
// 原因：imgui_impl_glfw（单窗口下 ShouldChainCallback 恒 true）把每次鼠标事件无条件
// 链回消费者回调，App 的 OnMouseButton 不去判断 WantCaptureMouse 就会在点/拖面板滑杆时
// 误开笔画（strokeActive=true），且改参回调经 paint::ApplyBrushSetting 命中 strokeActive
// 门被静默丢弃 →「画笔大小/硬度/透明度设置无效」。与 OnScroll 的 WantCaptureMouse 判断
// 同一约定（src/app.cpp）。返回 true=ImGui 未捕获（画布该响应）；false=ImGui 捕获（不响应）。
bool ShouldHandleCanvasPointer(bool wantCaptureMouse);
}
