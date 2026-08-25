// src/headless.cpp —— 离屏自检：固定笔迹 → dgcExportPNG
#include "headless.h"
#include "dgc_paint_c_api.h"
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

}  // namespace paint
