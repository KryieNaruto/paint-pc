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

    if (g_failures == 0) {
        std::printf("ALL PASS (5/5)\n");
        return 0;
    }
    std::printf("FAILED %d/5\n", g_failures);
    return 1;
}
