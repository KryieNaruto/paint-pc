// src/font_setup.h —— UI 字体设置纯函数（Bug 1+2：DPI 缩放 + 中文字体加载）。
// 供 App::init() 与 --font-repro / 无头单测复用；FindCjkFontPath / LoadUiFontsForScale
// / VerifyUiFontsHeadless 不依赖 GLFW 库（可纯无头单测），仅 LoadUiFonts 需要 GLFW。
#pragma once

#include <imgui.h>

struct GLFWwindow;  // 前置声明，避免在头文件引入 GLFW 头

namespace paint {

// 在平台常见路径里找一个系统自带的 CJK 字体文件；找不到返回 nullptr。
// Windows 现代版本（含中文语言包）自带 msyh.ttc；Linux 常见 Noto Sans CJK / 文泉驿。
// 返回的是 static 字符串字面量指针，调用方不得释放。
const char* FindCjkFontPath();

// 按 DPI 缩放建立 UI 字体图集（无 GLFW 依赖，可无头单测）：
//   - 找到 CJK 字体：按 fontSizePx = 13*dpiScale 物理像素加载其中文全量字形
//     （GetGlyphRangeChineseFull），不额外用 FontGlobalScale 二次缩放（避免糊）；
//   - 找不到：退回默认字体并按 fontSizePx 建图集，如实 stderr 告警（中文显示方块，
//     不比现状差——见 Bug2 根因）。
//   同时 ImGui::GetStyle().ScaleAllSizes(dpiScale)：控件间距/按钮尺寸同步按 DPI 缩放，
//   不只是文字。
void LoadUiFontsForScale(ImGuiIO& io, float dpiScale);

// App::init() 入口：查窗口内容缩放 glfwGetWindowContentScale 后调 LoadUiFontsForScale。
// window 为 nullptr（headless / 无窗口）时按 dpiScale=1.0 处理（无窗口即无缩放）。
void LoadUiFonts(ImGuiIO& io, GLFWwindow* window);

// 无头自检：FindCjkFontPath 非空 + 中文字形宽度检验（真加载非 tofu 方块）+
// FontSize == 13*dpiScale + ScaleAllSizes 生效。返回失败断言数（0 = 全过）。
// 供 --font-repro CLI 与 tests/font_test.cpp（ctest）共用同一套断言。
int VerifyUiFontsHeadless();

}  // namespace paint
