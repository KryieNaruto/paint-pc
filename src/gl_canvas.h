// src/gl_canvas.h —— 把读回的 RGBA 画布贴到屏幕的最小 GL 模块（core profile 3.3）。
#pragma once
#include <cstdint>

namespace paint {

struct GlCanvas {
    explicit GlCanvas(int w, int h);
    ~GlCanvas();

    void upload(const uint8_t* rgba, int w, int h);  // 上传/更新画布纹理
    // 绘制全屏 quad（处理 Y 翻转）。screenW/screenH：窗口帧缓冲尺寸（glViewport）。
    // u0,v0,u1,v1：画布纹理归一化采样子矩形（D6-2 缩放显示，见 coords.h ComputeCanvasViewport），
    // 默认 [0,0]-[1,1] 即整幅画布（zoom=1 时的等价形态），向后兼容旧调用。
    void draw(int screenW, int screenH, double u0 = 0.0, double v0 = 0.0, double u1 = 1.0, double v1 = 1.0);
    void destroy();

    int canvas_w = 0, canvas_h = 0;
    unsigned tex_ = 0, vao_ = 0, vbo_ = 0, prog_ = 0;
};

}  // namespace paint
