// src/headless.cpp —— 离屏自检：固定笔迹 → dgcExportPNG
#include "headless.h"
#include "dgc_paint_c_api.h"
#include "coords.h"
#include "font_setup.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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

// --- 极简 PNG 编码器（stored deflate，无外部 zlib 依赖）------------------------
// 只支持 RGBA8。用途：--font-repro 把 ImGui 字体图集落盘供人工复核/CI 产物留存。
// PNG 结构：signature + IHDR + IDAT(zlib: 0x78 0x01 + stored deflate blocks) + IEND。

namespace {

uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t len) {
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void PngChunk(FILE* f, const char type[4], const uint8_t* data, size_t len) {
    uint8_t lenbe[4];
    lenbe[0] = (uint8_t)(len >> 24); lenbe[1] = (uint8_t)(len >> 16);
    lenbe[2] = (uint8_t)(len >> 8);  lenbe[3] = (uint8_t)len;
    std::fwrite(lenbe, 1, 4, f);
    std::fwrite(type, 1, 4, f);
    if (len) std::fwrite(data, 1, len, f);
    const uint32_t crc = Crc32(Crc32(0, (const uint8_t*)type, 4), data, len);
    uint8_t crcbe[4];
    crcbe[0] = (uint8_t)(crc >> 24); crcbe[1] = (uint8_t)(crc >> 16);
    crcbe[2] = (uint8_t)(crc >> 8);  crcbe[3] = (uint8_t)crc;
    std::fwrite(crcbe, 1, 4, f);
}

}  // namespace

static int WriteMinimalPng(const char* path, int w, int h, const uint8_t* rgba) {
    if (!path || w <= 0 || h <= 0 || !rgba) return 1;
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;

    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::fwrite(kSig, 1, 8, f);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // color type: RGBA
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = 0;  // interlace
    PngChunk(f, "IHDR", ihdr, 13);

    // 原始 scanline：每行前缀一个 filter byte 0（None），后跟 w*4 RGBA 字节。
    const size_t stride = (size_t)w * 4 + 1;
    std::vector<uint8_t> raw(stride * (size_t)h);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = &raw[(size_t)y * stride];
        row[0] = 0;
        std::memcpy(row + 1, rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }

    // zlib 流：0x78 0x01（CMF/FLG 满足 mod31==0）+ stored deflate blocks。
    std::vector<uint8_t> zlib;
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    const size_t kMaxBlock = 65535;
    for (size_t start = 0; start < raw.size(); start += kMaxBlock) {
        const size_t len = raw.size() - start > kMaxBlock ? kMaxBlock : raw.size() - start;
        uint8_t hdr[5];
        hdr[0] = (start + len >= raw.size()) ? 0x01 : 0x00;  // BFINAL + BTYPE=00(stored)
        hdr[1] = (uint8_t)(len & 0xFF);
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        const uint16_t nlen = (uint16_t)(~len & 0xFFFF);
        hdr[3] = (uint8_t)(nlen & 0xFF);
        hdr[4] = (uint8_t)((nlen >> 8) & 0xFF);
        zlib.insert(zlib.end(), hdr, hdr + 5);
        zlib.insert(zlib.end(), raw.begin() + (long)start, raw.begin() + (long)start + len);
    }
    PngChunk(f, "IDAT", zlib.data(), zlib.size());
    PngChunk(f, "IEND", nullptr, 0);

    std::fclose(f);
    return 0;
}

// 字体设置复现（--font-repro <png>）：
// 用与 App::init() 相同的 LoadUiFonts 逻辑无头加载字体（ImGui 无 GL 后端也能建图集），
// 跑 VerifyUiFontsHeadless 全部断言（CJK 路径非空 / 中文字形宽度 / FontSize 按
// 13*dpiScale / ScaleAllSizes 生效）；全过后把字体图集落盘 PNG 供人工复核。
int FontReproRun(const char* outPng) {
    const int failures = VerifyUiFontsHeadless();
    if (failures != 0) {
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    LoadUiFonts(io, nullptr);  // headless 无窗口 → dpiScale=1.0
    io.Fonts->Build();

    uint8_t* pixels = nullptr;
    int w = 0, h = 0, bpp = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h, &bpp);
    const int rc = WriteMinimalPng(outPng, w, h, pixels);
    ImGui::DestroyContext();
    if (rc != 0) {
        std::fprintf(stderr, "[font-repro] PNG write failed: %s\n", outPng);
        return 1;
    }
    std::printf("[font-repro] PASS: atlas %dx%d written: %s\n", w, h, outPng);
    return 0;
}

}  // namespace paint
