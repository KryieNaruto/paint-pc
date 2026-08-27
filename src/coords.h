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

}  // namespace paint
