// src/font_setup.cpp —— Bug 1+2 修复实现（DPI 缩放 + 中文字体加载）。
// 见 docs/plans/imgui-ui-bugs-d6.md「Bug 1 + Bug 2」。
#include "font_setup.h"

#include <GLFW/glfw3.h>

#include <cfloat>
#include <cstdio>
#include <fstream>

namespace paint {

// 回归用例的中文探针字符串（无头 CalcTextSizeA 宽度检验用）。
static const char* kReproText = "笔画进行中";
// 同字符串的码点数组（避免测试代码里的 UTF-8 编码歧义，直接给 Unicode 码点）：
// 笔U+7B14 画U+753B 进U+8FDB 行U+884C 中U+4E2D，全部落在 GetGlyphRangesChineseFull
// 的 CJK 表意区（0x4E00-0x9FAF），用于「真实字形存在性」断言。
static const ImWchar kReproCodepoints[] = {0x7B14, 0x753B, 0x8FDB, 0x884C, 0x4E2D, 0};

const char* FindCjkFontPath() {
    // Windows 现代版本（含中文语言包）都自带 msyh.ttc；Linux 开发机/CI 常见
    // Noto Sans CJK 或文泉驿。
    static const char* kCjkFontCandidates[] = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/arphic/uming.ttc",
    };
    for (const char* p : kCjkFontCandidates) {
        if (std::ifstream(p, std::ios::binary).good()) {
            return p;
        }
    }
    return nullptr;
}

void LoadUiFontsForScale(ImGuiIO& io, float dpiScale) {
    // 按物理像素建图集（13px 基准 × DPI），不额外用 FontGlobalScale 二次缩放（避免糊）。
    const float fontSizePx = 13.0f * dpiScale;
    const char* cjkFontPath = FindCjkFontPath();
    if (cjkFontPath) {
        io.Fonts->AddFontFromFileTTF(cjkFontPath, fontSizePx, nullptr,
                                     io.Fonts->GetGlyphRangesChineseFull());
    } else {
        // 找不到任何一个才退化到默认字体（仅英文正常，中文继续是方块——不比现状差，
        // 如实 stderr 告警而非静默吞掉）。
        std::fprintf(stderr,
                     "[paint-pc] 未找到中文字体（Windows 需系统自带 msyh.ttc；Linux 需装 "
                     "fonts-noto-cjk 或 wqy-microhei），中文将显示为方块\n");
        ImFontConfig cfg;
        cfg.SizePixels = fontSizePx;
        io.Fonts->AddFontDefault(&cfg);
    }
    ImGui::GetStyle().ScaleAllSizes(dpiScale);  // 控件间距/按钮尺寸同步按 DPI 缩放，不只是文字
}

void LoadUiFonts(ImGuiIO& io, GLFWwindow* window) {
    float dpiScale = 1.0f;
    if (window) {
        float xscale = 1.0f, yscale = 1.0f;
        glfwGetWindowContentScale(window, &xscale, &yscale);
        dpiScale = xscale;
    }
    LoadUiFontsForScale(io, dpiScale);
}

int VerifyUiFontsHeadless() {
    int failures = 0;
    IMGUI_CHECKVERSION();

    auto check = [&failures](bool ok, const char* name, const char* detail) {
        if (ok) {
            std::printf("PASS: %s\n", name);
        } else {
            std::printf("FAIL: %s  %s\n", name, detail);
            ++failures;
        }
    };

    // 断言 2（计划）：FindCjkFontPath() 在本仓库 CI（Linux，装有 Noto Sans CJK）非空。
    const char* cjk = FindCjkFontPath();
    if (cjk) std::printf("INFO: FindCjkFontPath() -> %s\n", cjk);
    check(cjk != nullptr, "FindCjkFontPath returns non-null on Linux CI",
          "未找到候选字体（本机需装 fonts-noto-cjk 或 wqy-microhei）");

    // tofu 对照基线：仅默认字体（无中文字形），中文全部走 fallback '?' 占位字形。
    float tofuW = 0.0f;
    {
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        ImFont* f = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
        if (f) tofuW = f->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, kReproText).x;
        ImGui::DestroyContext();
    }
    std::printf("INFO: tofu width (default font) = %.2f\n", tofuW);

    // 断言 3（计划）：加载 CJK 字体后，中文字符解析出「真实字形」而非 tofu 占位。
    // 直接判据：5 个探针码点都命中非 fallback 的字形（FindGlyph 返回的指针 !=
    // FallbackGlyph，即真在字体里有字形，不是 '?' 占位方块）。
    // 辅助判据：整串 CalcTextSizeA 宽度显著大于默认字体（ProggyClean，无中文字形）
    // 下的 tofu 宽度（计划原文判据）。实测 Noto Sans CJK 表意字形 advance≈0.69em
    // （8.98px@13px），tofu('?')≈0.54em，比值约 1.28，取 1.2 阈值留余量。
    float cjkW = 0.0f;
    {
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiIO& io = ImGui::GetIO();
        LoadUiFontsForScale(io, 1.0f);
        io.Fonts->Build();
        ImFont* f = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
        if (f) {
            cjkW = f->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, kReproText).x;
            int realGlyphs = 0;
            for (const ImWchar* cp = kReproCodepoints; *cp; ++cp) {
                const ImFontGlyph* g = f->FindGlyph(*cp);
                if (g && g != f->FallbackGlyph) ++realGlyphs;
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "real=%d/5", realGlyphs);
            check(realGlyphs == 5,
                  "5 CJK codepoints resolve to real glyphs (not fallback tofu)", buf);
            check(f->FontSize == 13.0f, "FontSize == 13 at dpiScale=1.0 (base size)",
                  "dpiScale=1.0 时字号应为 13");
        } else {
            check(false, "Fonts[0] exists after CJK load", "图集无字体");
        }
        ImGui::DestroyContext();
    }
    std::printf("INFO: CJK width (loaded font) = %.2f\n", cjkW);
    check(cjkW > 0.0f, "CalcTextSizeA width > 0 after CJK font load", "宽度为 0");
    check(cjkW > tofuW * 1.2f,
          "CJK glyphs really loaded (width significantly > tofu baseline)",
          "真实中文字形宽度应显著大于同长度纯 tofu 方块宽度");

    // 断言 4（计划）：dpiScale != 1 时字体按缩放建图集（非事后拉伸）。注意 ImGui
    // 1.90+ 在 atlas build 时把字号取整为整数（ImFontAtlasBuildInit 的
    // `cfg.SizePixels = ImTrunc(cfg.SizePixels)`），13*1.25=16.25 → 16。断言
    // FontSize == (int)(13*dpiScale)，且 dpiScale=1 时 =13，证明缩放确实改变建图集
    // 字号（16 > 13），只是被 ImGui 取整到整数。
    {
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiIO& io = ImGui::GetIO();
        LoadUiFontsForScale(io, 1.25f);
        io.Fonts->Build();
        ImFont* f = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
        if (f) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "actual FontSize=%.3f (13*1.25=16.25 -> ImGui取整)",
                          f->FontSize);
            check(f->FontSize == (float)(int)(13.0f * 1.25f),
                  "FontSize scales with dpiScale=1.25 (ImGui rounds font size to int)", buf);
        } else {
            check(false, "Fonts[0] exists after scaled load", "图集无字体");
        }
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "actual WindowPadding.x=%.3f",
                          ImGui::GetStyle().WindowPadding.x);
            check(ImGui::GetStyle().WindowPadding.x == 8.0f * 1.25f,
                  "ScaleAllSizes applied (WindowPadding.x == 8*1.25)", buf);
        }
        ImGui::DestroyContext();
    }
    return failures;
}

}  // namespace paint
