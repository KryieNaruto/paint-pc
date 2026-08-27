// src/headless.cpp —— 离屏自检：固定笔迹 → dgcExportPNG
#include "headless.h"
#include "dgc_paint_c_api.h"
#include "coords.h"
#include <cmath>
#include <cstdio>

namespace paint {

int HeadlessRun(int w, int h, const char* outPng) {
    DgcContext* sdk = dgcCreate();
    if (!sdk) {
        // dgcCreate 默认 VkBackend，无 Vulkan 设备/ICD 时可能失败——给明确错误提示。
        std::fprintf(stderr, "[headless] dgcCreate failed: %s\n", dgcGetLastError());
        return 1;
    }
    int rc = dgcSetOffscreenSurface(sdk, w, h);
    if (rc != 0) { std::fprintf(stderr, "[headless] offscreen: %s\n", dgcGetLastError()); dgcDestroy(sdk); return 2; }
    rc = dgcClear(sdk, 0.96f, 0.95f, 0.91f, 1.0f);
    if (rc != 0) { std::fprintf(stderr, "[headless] clear: %s\n", dgcGetLastError()); }
    dgcSetRandomSeed(sdk, 42);            // 确定性：固定 seed
    dgcSetFixedTime(sdk, 1000.0);          // 固定时间步：1ms
    dgcBeginStroke(sdk, 100.f, 100.f, 0.5f, 0.f, 0.f);
    for (int i = 0; i < 20; ++i) {
        dgcStrokeTo(sdk, 100.f + i * 20.f, 100.f + i * 10.f, 0.5f, 0.f, 0.f, 0);
    }
    dgcEndStroke(sdk);
    dgcFlush(sdk);                          // 等合成完成（B5-2）
    rc = dgcExportPNG(sdk, outPng);
    dgcDestroy(sdk);
    if (rc != 0) { std::fprintf(stderr, "[headless] export failed: %s\n", dgcGetLastError()); return 2; }
    std::printf("[headless] PNG written: %s\n", outPng);
    return 0;
}

// 坐标映射复现（--coord-repro <png>）：
// 离屏 1600×1000 画布，十字标记画在正确位置 (1250,750)；
// 用 MapCursorToCanvas(1000,600,1280,800,1600,1000) 得映射点，在该点画 16 段斜向笔迹。
// 修复前 identity 映射 → 笔迹落 (1000,600)，与十字分离；修复后映射=(1250,750) → 笔迹与十字重合。
int CoordReproRun(const char* outPng) {
    const int canvasW = 1600, canvasH = 1000;
    DgcContext* sdk = dgcCreate();
    if (!sdk) {
        std::fprintf(stderr, "[coord-repro] dgcCreate failed: %s\n", dgcGetLastError());
        return 1;
    }
    int rc = dgcSetOffscreenSurface(sdk, canvasW, canvasH);
    if (rc != 0) { std::fprintf(stderr, "[coord-repro] offscreen: %s\n", dgcGetLastError()); dgcDestroy(sdk); return 2; }
    rc = dgcClear(sdk, 0.96f, 0.95f, 0.91f, 1.0f);
    if (rc != 0) { std::fprintf(stderr, "[coord-repro] clear: %s\n", dgcGetLastError()); }
    dgcSetRandomSeed(sdk, 42);            // 确定性：固定 seed
    dgcSetFixedTime(sdk, 1000.0);          // 固定时间步：1ms

    // 十字标记：正确位置 (1250,750)，两条正交短线段（水平 + 垂直）。
    const float cx = 1250.f, cy = 750.f, arm = 30.f;
    dgcBeginStroke(sdk, cx - arm, cy, 0.5f, 0.f, 0.f);
    dgcStrokeTo(sdk, cx, cy, 0.5f, 0.f, 0.f, 0);
    dgcStrokeTo(sdk, cx + arm, cy, 0.5f, 0.f, 0.f, 0);
    dgcEndStroke(sdk);
    dgcBeginStroke(sdk, cx, cy - arm, 0.5f, 0.f, 0.f);
    dgcStrokeTo(sdk, cx, cy, 0.5f, 0.f, 0.f, 0);
    dgcStrokeTo(sdk, cx, cy + arm, 0.5f, 0.f, 0.f, 0);
    dgcEndStroke(sdk);

    // 映射点：内容 (1000,600) @ 内容 1280×800 → 画布 1600×1000，期望 (1250,750)。
    double mx = 0.0, my = 0.0;
    MapCursorToCanvas(1000.0, 600.0, 1280, 800, canvasW, canvasH, &mx, &my);

    // 16 段斜向笔迹，以映射点为中心（±45° 各延伸 96px），bbox 中心 = 映射点。
    dgcBeginStroke(sdk, (float)(mx - 96.0), (float)(my - 96.0), 0.5f, 0.f, 0.f);
    for (int i = 1; i <= 16; ++i) {
        dgcStrokeTo(sdk, (float)(mx - 96.0 + 12.0 * i),
                    (float)(my - 96.0 + 12.0 * i), 0.5f, 0.f, 0.f, 0);
    }
    dgcEndStroke(sdk);

    dgcFlush(sdk);
    rc = dgcExportPNG(sdk, outPng);
    dgcDestroy(sdk);
    if (rc != 0) { std::fprintf(stderr, "[coord-repro] export failed: %s\n", dgcGetLastError()); return 2; }

    std::printf("map=(%.1f,%.1f) expected=(1250.0,750.0)\n", mx, my);
    if (std::fabs(mx - 1250.0) > 0.5 || std::fabs(my - 750.0) > 0.5) {
        std::fprintf(stderr, "[coord-repro] FAIL: mapping deviates from expected by >0.5px\n");
        return 3;
    }
    std::printf("[coord-repro] PASS: PNG written: %s\n", outPng);
    return 0;
}

}  // namespace paint
