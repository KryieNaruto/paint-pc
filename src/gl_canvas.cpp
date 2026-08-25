// src/gl_canvas.cpp
#include "gl_canvas.h"

// GL 函数指针加载：复用 ImGui 自带的内嵌 gl3w（imgl3w），零新增依赖。
// 该头在 CMake 的 FetchContent 拉取的 imgui-src/backends/ 下，
// 由 paint_imgui target 的 PUBLIC include 路径提供。
//
// 必须最先 include 本 loader：它在文件内定义 __gl_h_ / __gl_glcorearb_h_，
// GLFW 据此跳过 <GL/gl.h>。若先 include <GLFW/glfw3.h>（进而 <GL/gl.h>），
// GL_VERSION_1_x 宏会令 loader 的 PFNGL* typedef 整段被 #ifndef 跳过，
// ImGL3WProcs 结构体成员类型未定义 → 编译失败（实测）。
// loader 自带全部 shader/纹理/缓冲函数指针（glGenTextures、glDrawElements、
// glShaderSource、glUniform1i …），但「过滤版」不含 glDrawArrays 与
// GL_TRIANGLE_FAN / GL_CLAMP_TO_EDGE / GL_RGBA8 / GL_NEAREST /
// GL_TEXTURE_WRAP_S/T / GL_STATIC_DRAW —— 这里按 OpenGL 规范补常量并
// 声明 glDrawArrays（符号由已链接的 OpenGL::GL 提供）。
#include <imgui_impl_opengl3_loader.h>

#include <cstdio>

// 过滤版 loader 缺失的 GL 枚举（标准 OpenGL 值）。
#ifndef GL_NEAREST
#define GL_NEAREST 0x2600
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_TRIANGLE_FAN
#define GL_TRIANGLE_FAN 0x0006
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x2802
#endif
#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T 0x2803
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif

// glDrawArrays 不在过滤版 loader 内，直接声明并链接 OpenGL::GL（CMake 已链接）。
extern "C" void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count);

namespace paint {

static bool g_glLoaded = false;
static void EnsureGlLoaded() {
    if (!g_glLoaded) {
        if (imgl3wInit() != 0) { std::fprintf(stderr, "[gl_canvas] imgl3wInit failed\n"); return; }
        g_glLoaded = true;
    }
}

static const char* kVS = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
)";

static const char* kFS = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uCanvas;
void main(){ fragColor = texture(uCanvas, vec2(vUV.x, 1.0 - vUV.y)); }
)";

static GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log); std::fprintf(stderr, "[gl_canvas] shader err: %s\n", log); }
    return s;
}

static GLuint Link(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, 1024, nullptr, log); std::fprintf(stderr, "[gl_canvas] link err: %s\n", log); }
    return p;
}

GlCanvas::GlCanvas(int w, int h) : canvas_w(w), canvas_h(h) {
    EnsureGlLoaded();  // imgl3w：dlopen libOpenGL 加载 GL 函数指针
    unsigned vs = Compile(GL_VERTEX_SHADER, kVS), fs = Compile(GL_FRAGMENT_SHADER, kFS);
    prog_ = Link(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);

    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const float verts[] = { -1,-1, 0,0,   1,-1, 1,0,   1,1, 1,1,   -1,1, 0,1 };
    glGenVertexArrays(1, &vao_); glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
}

GlCanvas::~GlCanvas() { destroy(); }

void GlCanvas::upload(const uint8_t* rgba, int w, int h) {
    canvas_w = w; canvas_h = h;
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

void GlCanvas::draw(int viewW, int viewH) {
    glViewport(0, 0, viewW, viewH);
    glUseProgram(prog_);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_);
    glUniform1i(glGetUniformLocation(prog_, "uCanvas"), 0);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
}

void GlCanvas::destroy() {
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }
}

}  // namespace paint
