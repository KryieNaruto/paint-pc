// tests/test_canvas_input_gate.cpp —— bugfix「画笔大小/硬度/透明度设置无效」App 层回归。
// 根因 C（本测试覆盖）：src/app.cpp OnMouseButton 不检查 ImGui::GetIO().WantCaptureMouse，
// 点/拖面板滑杆会误开笔画（strokeActive=true），改参回调经 paint::ApplyBrushSetting 命中
// strokeActive 门被静默丢弃 → 设置无效。本测试通过共享函数 paint::ShouldHandleCanvasPointer
// + 复刻 App OnMouseButton 逻辑，验证「ImGui 捕获鼠标时画布不响应指针、改参真实生效」。
// 红阶段：ShouldHandleCanvasPointer 恒返回 true（当前 App 行为）→ T2/T3 红；绿阶段翻转。
#include "canvas_input.h"
#include "brush_settings.h"
#include "coords.h"
#include "dgc_paint_c_api.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s\n", name);                      \
            ++failures;                                                    \
        } else {                                                           \
            std::fprintf(stderr, "PASS: %s\n", name);                      \
        }                                                                  \
    } while (0)

using CtxGuard = std::unique_ptr<DgcContext, decltype(&dgcDestroy)>;

namespace {
constexpr int kW = 320;
constexpr int kH = 128;

struct InkStats {
    std::size_t dark_pixels = 0;  // r/g/b < 200
    int min_x = kW, max_x = -1;
    int span_x() const { return max_x >= 0 ? (max_x - min_x + 1) : 0; }
};

bool BytesSame(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

InkStats ComputeStats(const std::vector<std::uint8_t>& buf) {
    InkStats st;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::uint8_t* p = &buf[((std::size_t)y * kW + (std::size_t)x) * 4];
            if (p[0] < 200 && p[1] < 200 && p[2] < 200) {
                ++st.dark_pixels;
                if (x < st.min_x) st.min_x = x;
                if (x > st.max_x) st.max_x = x;
            }
        }
    }
    return st;
}

// 复刻 src/app.cpp OnMouseButton 的坐标映射 + 越界裁剪 + strokeActive/beginStroke 逻辑。
// gated==true 表示修复后：ImGui 捕获鼠标时不处理指针事件（与 OnScroll 同约定）。
static bool SimulatePress(DgcContext* sdk, double winX, double winY, bool gated, bool* strokeActive) {
    const bool wantCaptureMouse = true;  // 模拟：光标悬停在面板滑杆上（ImGui 捕获鼠标）
    if (gated && !paint::ShouldHandleCanvasPointer(wantCaptureMouse)) {
        return false;  // 修复：UI 交互不碰画布/strokeActive
    }
    double cx = 0.0, cy = 0.0;
    paint::MapCursorToCanvasZoomed(winX, winY, kW, kH, kW, kH, 1.0f, &cx, &cy);
    if (cx < 0 || cy < 0 || cx >= kW || cy >= kH) { *strokeActive = false; return false; }
    *strokeActive = true;
    dgcBeginStroke(sdk, (float)cx, (float)cy, 0.5f, 0.0f, 0.0f);
    return true;
}

static void SimulateRelease(DgcContext* sdk, bool* strokeActive) {
    *strokeActive = false;
    dgcEndStroke(sdk);
}

// 渲染固定水平线（y=64, x 40..280）。uiPressGated==true 时先模拟「点面板滑杆改 radius=40」
// 的完整交互：按下（门控）→ 下发 → 抬笔；然后画线。导出 PNG（可空）。
static InkStats RenderLine(bool uiPressGated, std::vector<std::uint8_t>& buf, const char* png) {
    InkStats st;
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) return st;
    if (dgcSetOffscreenSurface(ctx.get(), kW, kH) != DGC_OK) return st;
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);

    bool strokeActive = false;
    if (uiPressGated) {
        SimulatePress(ctx.get(), 200.0, 112.0, true, &strokeActive);  // 点面板滑杆
        const int rc = paint::ApplyBrushSetting(ctx.get(), strokeActive,
                                                DGC_SETTING_RADIUS, 40.0, "radius");
        if (rc != DGC_OK) return st;
        SimulateRelease(ctx.get(), &strokeActive);  // 抬笔
    }

    dgcBeginStroke(ctx.get(), 40.0f, 64.0f, 0.5f, 0.0f, 0.0f);
    for (int x = 48; x <= 280; x += 8) {
        dgcStrokeTo(ctx.get(), (float)x, 64.0f, 0.5f, 0.0f, 0.0f, 0);
    }
    dgcEndStroke(ctx.get());
    dgcFlush(ctx.get());

    buf.assign((std::size_t)kW * kH * 4, 0);
    if (dgcReadbackPixels(ctx.get(), buf.data()) != DGC_OK) return st;
    if (png) dgcExportPNG(ctx.get(), png);
    return ComputeStats(buf);
}
}  // namespace

int main() {
    // ── T1: ImGui 未捕获鼠标时画布该响应 ──
    CHECK(paint::ShouldHandleCanvasPointer(false) == true, "T1 no-capture: canvas handles pointer");

    // ── T2: ImGui 捕获鼠标时画布不响应（本修复核心）──
    CHECK(paint::ShouldHandleCanvasPointer(true) == false, "T2 capture: canvas ignores pointer");

    // ── T3 端到端: 点面板滑杆改 radius=40 → 下一笔线必须生效 ──
    std::vector<std::uint8_t> bufDefault, bufGated;
    const InkStats def = RenderLine(false, bufDefault, "canvas_gate_default.png");
    const InkStats gated = RenderLine(true, bufGated, "canvas_gate_ui_press_radius40.png");
    std::fprintf(stderr,
                 "[canvas_input_gate] T3 default ink=%zu span=%d | ui-press radius40 ink=%zu span=%d\n",
                 def.dark_pixels, def.span_x(), gated.dark_pixels, gated.span_x());
    CHECK(!BytesSame(bufGated, bufDefault), "T3 ui-press radius40 output differs from default");
    CHECK(gated.dark_pixels > def.dark_pixels * 1.3, "T3 ui-press radius40 ink > 1.3x default ink");
    CHECK(gated.span_x() > def.span_x(), "T3 ui-press radius40 span > default span");

    if (failures == 0) {
        std::fprintf(stderr, "[canvas_input_gate] ALL PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
