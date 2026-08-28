// tests/font_test.cpp —— Bug 1+2 回归用例（先红后绿）。
// 无头可跑：ImGui 无需 GL 后端也能 CreateContext + 建字体图集（stb_truetype 编译进
// imgui_draw.cpp）。断言与 --font-repro CLI 共用 VerifyUiFontsHeadless（同一套逻辑）。
#include "font_setup.h"

#include <cstdio>

int main() {
    const int failures = paint::VerifyUiFontsHeadless();
    if (failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILED %d\n", failures);
    return 1;
}
