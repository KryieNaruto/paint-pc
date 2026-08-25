cmake_minimum_required(VERSION 3.22)
project(paint_pc LANGUAGES C CXX)

# paint-pc：PC UI 消费者（窗口 / 输入 / present 自备）。
# 外壳期：GLFW + ImGui + OpenGL 清屏。待 SDK B1-4 C API / B2-1 Vulkan 落地后接入。

# --- SDK（git submodule，路径固定 sdk/）--------------------------------------
# 只 add_subdirectory + 链接 dgc_paint，不 include SDK 内部头。
add_subdirectory(sdk)

# --- 第三方：GLFW + ImGui（无系统包，FetchContent 拉源码）--------------------
include(FetchContent)

set(PC_GLFW_TAG "3.3.10")
set(PC_IMGUI_TAG "v1.90.9")
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG ${PC_GLFW_TAG}
    GIT_SHALLOW TRUE
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG ${PC_IMGUI_TAG}
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw imgui)

# ImGui：作为本地源码编译（glfw + opengl3 后端）。
add_library(paint_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(paint_imgui PUBLIC ${imgui_SOURCE_DIR})
target_include_directories(paint_imgui PUBLIC ${imgui_SOURCE_DIR}/backends)
target_link_libraries(paint_imgui PRIVATE glfw)
# GLFW 会链接 OpenGL 与 X11；ImGui 后端代码在 C++ 下编译。
target_compile_features(paint_imgui PUBLIC cxx_std_17)

# --- 应用可执行文件 -----------------------------------------------------------
add_executable(paint_pc
    src/main.cpp
    src/app.cpp
    src/gl_canvas.cpp
)
target_include_directories(paint_pc PRIVATE src)

# 只链接 dgc_paint 与自备 UI 依赖。不要链接 SDK 内部 target。
target_link_libraries(paint_pc PRIVATE dgc_paint paint_imgui glfw)

# OpenGL：不同平台链接名不同（Linux 用 OpenGL::GL）。
find_package(OpenGL REQUIRED)
target_link_libraries(paint_pc PRIVATE OpenGL::GL)
