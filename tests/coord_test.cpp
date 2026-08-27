// tests/coord_test.cpp —— MapCursorToCanvas 回归用例（先红后绿）。
// 纯函数断言，无头可跑，注册 ctest。
#include "coords.h"

#include <cstdio>

namespace {

int g_failures = 0;

void Check(const char* name,
           double x, double y,
           int cw, int ch, int vw, int vh,
           double expX, double expY) {
    double ox = 0.0, oy = 0.0;
    paint::MapCursorToCanvas(x, y, cw, ch, vw, vh, &ox, &oy);
    const double dx = ox - expX, dy = oy - expY;
    const bool ok = (dx > -1e-6 && dx < 1e-6) && (dy > -1e-6 && dy < 1e-6);
    if (ok) {
        std::printf("PASS: %s\n", name);
    } else {
        std::printf("FAIL: %s  actual=(%.1f,%.1f) expected=(%.1f,%.1f)\n",
                    name, ox, oy, expX, expY);
        ++g_failures;
    }
}

// --- D6-2 缩放回归用例 ------------------------------------------------------

void CheckZoom(const char* name,
               double x, double y,
               int cw, int ch, int canvasW, int canvasH, float zoom,
               double expX, double expY) {
    double ox = 0.0, oy = 0.0;
    paint::MapCursorToCanvasZoomed(x, y, cw, ch, canvasW, canvasH, zoom, &ox, &oy);
    const double dx = ox - expX, dy = oy - expY;
    const bool ok = (dx > -1e-6 && dx < 1e-6) && (dy > -1e-6 && dy < 1e-6);
    if (ok) {
        std::printf("PASS: %s\n", name);
    } else {
        std::printf("FAIL: %s  actual=(%.4f,%.4f) expected=(%.4f,%.4f)\n",
                    name, ox, oy, expX, expY);
        ++g_failures;
    }
}

void CheckFloatEq(const char* name, float actual, float expected) {
    const float d = actual - expected;
    const bool ok = (d > -1e-6f && d < 1e-6f);
    if (ok) {
        std::printf("PASS: %s\n", name);
    } else {
        std::printf("FAIL: %s  actual=%.6f expected=%.6f\n", name, actual, expected);
        ++g_failures;
    }
}

}  // namespace

int main() {
    // 用例1：报告场景 scale=1.25，内容(1000,600)→画布(1250,750)。
    Check("case1 scale=1.25 (1000,600)@1280x800->1600x1000", 1000, 600, 1280, 800, 1600, 1000, 1250, 750);
    // 用例2：默认分辨率恒等必须保持正确。
    Check("case2 identity (500,300)@1280x800->1280x800", 500, 300, 1280, 800, 1280, 800, 500, 300);
    // 用例3：右下角边界。
    Check("case3 corner (1280,800)@1280x800->1600x1000", 1280, 800, 1280, 800, 1600, 1000, 1600, 1000);
    // 用例4：scale<1 反缩。
    Check("case4 scale<1 (960,540)@1920x1080->1280x720", 960, 540, 1920, 1080, 1280, 720, 640, 360);
    // 用例5：最小化除零保护。
    Check("case5 zero-size (100,100)@0x0->1600x1000", 100, 100, 0, 0, 1600, 1000, 0, 0);

    // --- D6-2 缩放（评审修复后：zoom clamp 到 [1,8]，缩小 = 朝 1 回退）-------

    // 用例6：ClampZoom 钳制下限（<1 全部钳到 1.0，"缩小"定义为朝 1 回退）。
    CheckFloatEq("case6 ClampZoom below-min clamps to 1.0", paint::ClampZoom(0.1f), 1.0f);
    // 用例7：ClampZoom 钳制上限。
    CheckFloatEq("case7 ClampZoom above-max clamps to 8.0", paint::ClampZoom(100.0f), 8.0f);
    // 用例8：ClampZoom 区间内恒等。
    CheckFloatEq("case8 ClampZoom in-range identity", paint::ClampZoom(2.5f), 2.5f);

    // 用例9：zoom=1（视口=整幅画布）退化为无缩放映射，与 case2 恒等场景一致。
    CheckZoom("case9 zoom=1 degenerates to identity", 500, 300, 1280, 800, 1280, 800, 1.0f, 500, 300);

    // 用例10：zoom=2 居中锚定，窗口正中心像素 → 画布正中心像素（锚点必是不动点）。
    // 内容(640,400) 恰为 1280x800 窗口中心；canvas=内容同尺寸（scale=1）时，
    // 中心点无论 zoom 多少都应映射回画布中心 (640,400)。
    CheckZoom("case10 zoom=2 center anchor is fixed point", 640, 400, 1280, 800, 1280, 800, 2.0f, 640, 400);

    // 用例11：zoom=2，画布左上角(0,0)对应的窗口坐标应映射回画布 (viewX, viewY) = (320,200)。
    // 视口子矩形：viewW=1280/2=640，viewX=1280/2-640/2=320；viewH=800/2=400，viewY=800/2-400/2=200。
    CheckZoom("case11 zoom=2 window origin maps to viewport origin", 0, 0, 1280, 800, 1280, 800, 2.0f, 320, 200);

    // 用例12：zoom=2，窗口右下角(1280,800)应映射回画布 (viewX+viewW, viewY+viewH) = (960,600)。
    CheckZoom("case12 zoom=2 window corner maps to viewport corner", 1280, 800, 1280, 800, 1280, 800, 2.0f, 960, 600);

    // 用例13：ComputeCanvasViewport 在 zoom=1 时视口=整幅画布（"缩小后完整可见"的核心依据）。
    {
        double vx = -1, vy = -1, vw = -1, vh = -1;
        paint::ComputeCanvasViewport(1600, 1000, 1.0f, &vx, &vy, &vw, &vh);
        CheckFloatEq("case13a viewport zoom=1 viewX=0", (float)vx, 0.0f);
        CheckFloatEq("case13b viewport zoom=1 viewY=0", (float)vy, 0.0f);
        CheckFloatEq("case13c viewport zoom=1 viewW=canvasW", (float)vw, 1600.0f);
        CheckFloatEq("case13d viewport zoom=1 viewH=canvasH", (float)vh, 1000.0f);
    }

    // 用例14：ComputeCanvasViewport 在 canvasW/H<=0 时输出全 0（除零保护）。
    {
        double vx = -1, vy = -1, vw = -1, vh = -1;
        paint::ComputeCanvasViewport(0, 0, 2.0f, &vx, &vy, &vw, &vh);
        CheckFloatEq("case14 viewport zero-size guards", (float)(vx + vy + vw + vh), 0.0f);
    }

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILED %d\n", g_failures);
    return 1;
}
