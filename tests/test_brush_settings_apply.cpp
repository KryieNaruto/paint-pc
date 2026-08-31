// tests/test_brush_settings_apply.cpp —— bugfix「画笔设置（粗细/硬度/透明度）+ modeler 参数无效」
// App 消费端回归测试（先红后绿）。
//
// 根因 B（App 侧，本测试覆盖）：src/app.cpp Impl::ApplyBrushSetting 把设置下发到
// dgcCreateBrush 发号器句柄（非 1 即被内核 BrushKernel::setBrushSetting 静默 no-op），
// 而非固定默认笔刷 DGC_DEFAULT_BRUSH。本测试通过 App 共享设置入口 paint::ApplyBrushSetting
// 下发，验证「面板改参必须真实改变渲染输出」。
//
// 红阶段：ApplyBrushSetting 忠实移植当前 App 行为（带 DgcBrush handle 参数）。每个渲染在全新
// context 上先 dgcCreateBrush 两次（第 2 次句柄 = 2），把 handle2 传给 ApplyBrushSetting →
// 内核 no-op → T1/T2/T3/T6 红（输出与默认逐字节相同）。T4（modeler）是 context 级单例、与句柄
// 无关 → 自始为绿；T5（strokeActive 跳过）同理自始为绿。
// 绿阶段：ApplyBrushSetting 固定 DGC_DEFAULT_BRUSH（去掉 handle 参数）→ T1-T6 全绿。
#include "brush_settings.h"
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

// 水平线渲染画布（与 SDK test_brush_setting_applies.cpp 一致）。
constexpr int kW = 320;
constexpr int kH = 128;
// 波浪线渲染画布（与 SDK test_modeler_param_changes_output.cpp 一致）。
constexpr int kMW = 400;
constexpr int kMH = 160;
constexpr int kWavyPoints = 48;

struct InkStats {
    std::size_t dark_pixels = 0;  // r/g/b 均 < 200 的「墨迹像素」（含浅灰，opacity 0.1 也计入）
    std::size_t black_pixels = 0; // r/g/b 均 < 100 的「强墨迹像素」（仅近不透明笔迹达到）
    int min_x = kW, max_x = -1;
    int span_x() const { return max_x >= 0 ? (max_x - min_x + 1) : 0; }
};

bool BytesSame(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size()) == 0;
}

std::size_t BytesDiff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::size_t d = 0;
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) ++d;
    }
    return d + (a.size() > b.size() ? (a.size() - b.size()) : (b.size() - a.size()));
}

InkStats ComputeStats(const std::vector<std::uint8_t>& buf, int w, int h) {
    InkStats st;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &buf[((std::size_t)y * w + (std::size_t)x) * 4];
            if (p[0] < 200 && p[1] < 200 && p[2] < 200) {
                ++st.dark_pixels;
                if (x < st.min_x) st.min_x = x;
                if (x > st.max_x) st.max_x = x;
            }
            if (p[0] < 100 && p[1] < 100 && p[2] < 100) {
                ++st.black_pixels;
            }
        }
    }
    return st;
}

// 渲染一条固定水平线笔画（y=64，x 40..280）。每个渲染在全新 context 上先 dgcCreateBrush 两次
// （把发号器推进到 2，复刻「App 第 2 个笔刷」场景，T6 句柄免疫的载体），再经 paint::ApplyBrushSetting
// 设置一个笔刷内核参数（settingId < 0 表示不设置）。填充 buf 并统计墨迹，同时导出 PNG（可空）。
// strokeActive 透传给 ApplyBrushSetting（T5 用）；applyRcOut 接收 ApplyBrushSetting 返回码（可空）。
InkStats RenderLine(int settingId, double value, std::vector<std::uint8_t>& buf, const char* png,
                    bool strokeActive, int* applyRcOut) {
    InkStats st;
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) {
        return st;
    }
    if (dgcSetOffscreenSurface(ctx.get(), kW, kH) != DGC_OK) {
        return st;
    }
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);
    // 两次 dgcCreateBrush 把发号器推进到 2（非默认句柄）：绿阶段证明共享入口固定
    // DGC_DEFAULT_BRUSH、不受发号器句柄值影响（T6 句柄免疫的载体）。
    dgcCreateBrush(ctx.get(), nullptr);
    dgcCreateBrush(ctx.get(), nullptr);
    if (settingId >= 0) {
        const int rc = paint::ApplyBrushSetting(ctx.get(), strokeActive,
                                                settingId, value, "test");
        if (applyRcOut) *applyRcOut = rc;
        if (rc != DGC_OK) {
            return st;
        }
    }
    dgcBeginStroke(ctx.get(), 40.0f, 64.0f, 0.5f, 0.0f, 0.0f);
    for (int x = 48; x <= 280; x += 8) {
        dgcStrokeTo(ctx.get(), (float)x, 64.0f, 0.5f, 0.0f, 0.0f, 0);
    }
    dgcEndStroke(ctx.get());
    dgcFlush(ctx.get());  // drain 屏障：入队 → 三线程合成完毕 → 读回确定性

    buf.assign((std::size_t)kW * kH * 4, 0);
    if (dgcReadbackPixels(ctx.get(), buf.data()) != DGC_OK) {
        return st;
    }
    if (png) {
        dgcExportPNG(ctx.get(), png);
    }
    return ComputeStats(buf, kW, kH);
}

// 画同一波浪笔画（T4 modeler 场景）：先 dgcCreateBrush 两次，再经 paint::ApplyBrushSetting 设
// PREDICTION_INTERVAL_MS=intervalMs 激活 modeler，读回 RGBA 并导出 PNG。
bool RenderWavy(double intervalMs, std::vector<std::uint8_t>& buf, const char* png) {
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) {
        return false;
    }
    if (dgcSetOffscreenSurface(ctx.get(), kMW, kMH) != DGC_OK) {
        return false;
    }
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);
    dgcCreateBrush(ctx.get(), nullptr);
    dgcCreateBrush(ctx.get(), nullptr);
    if (paint::ApplyBrushSetting(ctx.get(), false,
                                 DGC_SETTING_PREDICTION_INTERVAL_MS, intervalMs,
                                 "test") != DGC_OK) {
        return false;
    }
    dgcBeginStroke(ctx.get(), 20.0f, 80.0f, 0.5f, 0.0f, 0.0f);
    for (int i = 1; i < kWavyPoints; ++i) {
        const float x = 20.0f + 320.0f * (float(i) / (float)(kWavyPoints - 1));
        const float y = 80.0f + 20.0f * float(std::sin(2.0 * M_PI * (double(i) / (double)(kWavyPoints - 1))));
        dgcStrokeTo(ctx.get(), x, y, 0.5f, 0.0f, 0.0f, 0);
    }
    dgcEndStroke(ctx.get());
    dgcFlush(ctx.get());
    buf.assign((std::size_t)kMW * kMH * 4, 0);
    if (dgcReadbackPixels(ctx.get(), buf.data()) != DGC_OK) {
        return false;
    }
    dgcExportPNG(ctx.get(), png);
    return true;
}

}  // namespace

int main() {
    // ── T1: radius=40 vs 默认：墨迹显著增大 + 横向跨度显著增大 ──
    std::vector<std::uint8_t> bufDefault, bufR40;
    const InkStats def = RenderLine(-1, 0.0, bufDefault, "brush_apply_default.png", false, nullptr);
    const InkStats r40 = RenderLine(DGC_SETTING_RADIUS, 40.0, bufR40, "brush_apply_radius40.png",
                                    false, nullptr);
    std::fprintf(stderr,
                 "[brush_settings_apply] T1 default ink=%zu span_x=%d | radius40 ink=%zu span_x=%d\n",
                 def.dark_pixels, def.span_x(), r40.dark_pixels, r40.span_x());
    // 红判据（红阶段）：handle2 被内核 no-op → 两画布逐字节相同；绿判据：不同且半径更大。
    CHECK(!BytesSame(bufR40, bufDefault), "T1 radius40 output differs byte-wise from default");
    CHECK(r40.dark_pixels > def.dark_pixels * 1.3, "T1 radius40 ink > 1.3x default ink");
    CHECK(r40.span_x() > def.span_x(), "T1 radius40 horizontal span > default span");

    // ── T2: hardness=1.0 改变输出（非逐字节相同）──
    std::vector<std::uint8_t> bufHard;
    const InkStats hard = RenderLine(DGC_SETTING_HARDNESS, 1.0, bufHard, "brush_apply_hardness.png",
                                     false, nullptr);
    CHECK(!BytesSame(bufHard, bufDefault), "T2 hardness=1.0 changes output");

    // ── T3: opacity=0.1 使「强墨迹」（近黑像素）大幅减少（变淡）──
    // 用 black_pixels（<100）而非 dark_pixels（<200）：opacity 0.1 的 dab 叠加后中心仍可到
    // ~167，dark 阈值区分度差；近黑像素只有近不透明笔迹才达到，区分度大。
    std::vector<std::uint8_t> bufOp;
    const InkStats op = RenderLine(DGC_SETTING_OPACITY, 0.1, bufOp, "brush_apply_opacity.png",
                                   false, nullptr);
    std::fprintf(stderr,
                 "[brush_settings_apply] T2 hardness ink=%zu | T3 opacity ink=%zu black=%zu\n",
                 hard.dark_pixels, op.dark_pixels, op.black_pixels);
    CHECK(!BytesSame(bufOp, bufDefault), "T3 opacity=0.1 changes output");
    CHECK(op.black_pixels < def.black_pixels / 2, "T3 opacity=0.1 near-black ink < half of default");

    // ── T4: modeler PREDICTION_INTERVAL_MS=100 vs =1 画波浪线：输出逐字节不同 ──
    // （红阶段自始为绿：modeler 是 context 级单例，与句柄无关，即使传 handle2 也生效。）
    std::vector<std::uint8_t> bufMl, bufMs;
    const bool okMl = RenderWavy(100.0, bufMl, "brush_apply_modeler_large.png");
    const bool okMs = RenderWavy(1.0, bufMs, "brush_apply_modeler_small.png");
    CHECK(okMl && okMs, "T4 both prediction-interval renders succeed");
    const std::size_t diffT4 = BytesDiff(bufMl, bufMs);
    std::fprintf(stderr, "[brush_settings_apply] T4 interval=100ms vs 1ms: %zu differing bytes\n",
                 diffT4);
    CHECK(diffT4 > 0, "T4 PREDICTION_INTERVAL_MS large vs small produces different output");

    // ── T5: strokeActive 跳过 —— 返回 DGC_OK 且输出与默认相同（两笔画之间才生效）──
    std::vector<std::uint8_t> bufSkip;
    int applyRc = -999;
    const InkStats skip = RenderLine(DGC_SETTING_RADIUS, 40.0, bufSkip, nullptr, true, &applyRc);
    (void)skip;
    CHECK(applyRc == DGC_OK, "T5 strokeActive=true returns DGC_OK");
    CHECK(BytesSame(bufSkip, bufDefault), "T5 strokeActive=true output identical to default");

    // ── T6: 句柄免疫 —— 先 dgcCreateBrush 两次（发号器推进到 2）再改参，输出仍改变 ──
    // RenderLine 内部每次渲染都先 dgcCreateBrush 两次，T6 与 T1 同一渲染路径；这里独立成例，
    // 强调「即使发号器句柄已 ≠ 1，共享入口仍必须让设置生效」（红阶段传 handle2 应红）。
    std::vector<std::uint8_t> bufImmune;
    const InkStats imm = RenderLine(DGC_SETTING_RADIUS, 40.0, bufImmune, nullptr, false, nullptr);
    std::fprintf(stderr,
                 "[brush_settings_apply] T6 create-brush-twice then radius40 ink=%zu span_x=%d\n",
                 imm.dark_pixels, imm.span_x());
    CHECK(!BytesSame(bufImmune, bufDefault), "T6 create-brush-twice then radius=40 still changes output");
    CHECK(imm.dark_pixels > def.dark_pixels * 1.3, "T6 create-brush-twice ink > 1.3x default ink");
    CHECK(imm.span_x() > def.span_x(), "T6 create-brush-twice span > default span");

    if (failures == 0) {
        std::fprintf(stderr, "[brush_settings_apply] ALL PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
