// src/coords.cpp —— MapCursorToCanvas 实现。
// 窗口内容坐标（逻辑单位）→ 画布像素坐标（帧缓冲像素），按 canvas/content 比例换算。
#include "coords.h"

namespace paint {

void MapCursorToCanvas(double x, double y,
                       int contentW, int contentH,
                       int canvasW, int canvasH,
                       double* outX, double* outY) {
    // 任一尺寸 <= 0（如最小化窗口）→ 输出 (0,0)，避免除零。
    if (contentW <= 0 || contentH <= 0 || canvasW <= 0 || canvasH <= 0) {
        *outX = 0.0;
        *outY = 0.0;
        return;
    }
    *outX = x * (double)canvasW / (double)contentW;
    *outY = y * (double)canvasH / (double)contentH;
}

}  // namespace paint
