// src/gl_canvas.h —— 把读回的 RGBA 画布贴到屏幕的最小 GL 模块（core profile 3.3）。
#pragma once
#include <cstdint>

namespace paint {

struct GlCanvas {
    explicit GlCanvas(int w, int h);
    ~GlCanvas();

    void upload(const uint8_t* rgba, int w, int h);  // 上传/更新画布纹理
    void draw(int viewW, int viewH);                 // 绘制全屏 quad（处理 Y 翻转）
    void destroy();

    int canvas_w = 0, canvas_h = 0;
    unsigned tex_ = 0, vao_ = 0, vbo_ = 0, prog_ = 0;
};

}  // namespace paint
