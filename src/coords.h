// src/coords.h —— 窗口内容坐标 → 画布像素坐标映射（纯函数，无 GLFW 依赖，可无头单测）。
#pragma once

namespace paint {

// 窗口内容坐标 → 画布像素坐标。
// contentW/H：glfwGetWindowSize 内容区尺寸（逻辑单位）；canvasW/H：离屏画布尺寸（帧缓冲像素）。
// 非默认分辨率/DPI 缩放 !=1 时 content != canvas，必须按比例换算，不能当恒等。
// 任一输入尺寸 <= 0（如最小化窗口）时输出 (0,0)，避免除零。
void MapCursorToCanvas(double x, double y,
                       int contentW, int contentH,
                       int canvasW, int canvasH,
                       double* outX, double* outY);

// --- 画布缩放（D6-2）-------------------------------------------------------
// 缩放显示与坐标映射全部在消费端完成（SDK 离屏渲染路径零改动，见 D6-2 计划 §3）。
//
// 缩放范围钳到 [kZoomMin, kZoomMax]：下限钳到 1.0（而非 <1 的“缩小画布子矩形”），
// 因为 zoom<1 会令视口子矩形 viewW=canvasW/zoom > canvasW、viewX<0，超出画布边界，
// 与当前固定全屏 quad + GL_CLAMP_TO_EDGE 采样方案不兼容（UV 越界会被钳到边缘，
// 导致“缩小”视觉上无效）。故“缩小”定义为朝 zoom=1 回退；zoom=1 时视口子矩形等于
// 整幅画布，天然满足“缩小后完整可见”。
constexpr float kZoomMin = 1.0f;
constexpr float kZoomMax = 8.0f;

// 将 zoom 钳制到 [kZoomMin, kZoomMax]。
float ClampZoom(float zoom);

// 计算画布可视子矩形（画布像素坐标系，居中锚定）。
// zoom 内部会先经 ClampZoom 钳制；zoom=1 时 viewX=viewY=0、viewW=canvasW、viewH=canvasH
// （即整幅画布）；zoom>1 时子矩形按比例缩小并居中，实现“放大显示”。
// canvasW/canvasH <= 0 时全部输出 0，避免除零。
void ComputeCanvasViewport(int canvasW, int canvasH, float zoom,
                           double* viewX, double* viewY,
                           double* viewW, double* viewH);

// 窗口内容坐标 → 画布像素坐标（叠加 zoom 的完整逆映射，落点正确性的关键）。
// 链路：内容坐标 → 帧缓冲/画布像素（沿用 MapCursorToCanvas 的 DPI 换算）
//      → 叠加视口逆映射：px = viewX + fx/zoom，py = viewY + fy/zoom。
// zoom=1 时退化为 MapCursorToCanvas 原始结果（视口 = 整幅画布）。
void MapCursorToCanvasZoomed(double x, double y,
                             int contentW, int contentH,
                             int canvasW, int canvasH,
                             float zoom,
                             double* outX, double* outY);

}  // namespace paint
