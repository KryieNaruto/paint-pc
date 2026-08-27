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

float ClampZoom(float zoom) {
    if (zoom < kZoomMin) return kZoomMin;
    if (zoom > kZoomMax) return kZoomMax;
    return zoom;
}

void ComputeCanvasViewport(int canvasW, int canvasH, float zoom,
                           double* viewX, double* viewY,
                           double* viewW, double* viewH) {
    if (canvasW <= 0 || canvasH <= 0) {
        *viewX = 0.0; *viewY = 0.0; *viewW = 0.0; *viewH = 0.0;
        return;
    }
    const float z = ClampZoom(zoom);
    const double vw = (double)canvasW / (double)z;
    const double vh = (double)canvasH / (double)z;
    *viewW = vw;
    *viewH = vh;
    // 居中锚定：zoom 绕窗口中心缩放（显示层选择，与落点正确性无关，见 D6-2 计划 §5）。
    *viewX = (double)canvasW / 2.0 - vw / 2.0;
    *viewY = (double)canvasH / 2.0 - vh / 2.0;
}

void MapCursorToCanvasZoomed(double x, double y,
                             int contentW, int contentH,
                             int canvasW, int canvasH,
                             float zoom,
                             double* outX, double* outY) {
    double fx = 0.0, fy = 0.0;
    MapCursorToCanvas(x, y, contentW, contentH, canvasW, canvasH, &fx, &fy);
    if (canvasW <= 0 || canvasH <= 0 || contentW <= 0 || contentH <= 0) {
        *outX = 0.0; *outY = 0.0;
        return;
    }
    const float z = ClampZoom(zoom);
    double viewX = 0.0, viewY = 0.0, viewW = 0.0, viewH = 0.0;
    ComputeCanvasViewport(canvasW, canvasH, z, &viewX, &viewY, &viewW, &viewH);
    *outX = viewX + fx / (double)z;
    *outY = viewY + fy / (double)z;
}

}  // namespace paint
