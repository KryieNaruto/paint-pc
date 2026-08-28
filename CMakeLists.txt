cmake_minimum_required(VERSION 3.22)
project(paint_pc LANGUAGES C CXX)

enable_testing()

# paint-pc：PC UI 消费者（窗口 / 输入 / present 自备）。
# 外壳期：GLFW + ImGui + OpenGL 清屏。待 SDK B1-4 C API / B2-1 Vulkan 落地后接入。

# --- 版本戳：消费者 + SDK submodule 的 git short sha（见 src/version.h.in）--------
# 取的是 configure 那一刻的 git 状态；non-git checkout（源码包）/ git 不可用时退化为
# "unknown"，不阻断构建。
function(pc_git_short_sha out_var repo_dir)
  set(${out_var} "unknown" PARENT_SCOPE)
  find_package(Git QUIET)
  if(NOT GIT_FOUND OR NOT EXISTS "${repo_dir}/.git")
    return()
  endif()
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${repo_dir}
    OUTPUT_VARIABLE sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc
    ERROR_QUIET
  )
  if(rc EQUAL 0 AND sha)
    set(${out_var} ${sha} PARENT_SCOPE)
  endif()
endfunction()

function(pc_git_is_dirty out_var repo_dir)
  set(${out_var} "false" PARENT_SCOPE)
  find_package(Git QUIET)
  if(NOT GIT_FOUND OR NOT EXISTS "${repo_dir}/.git")
    return()
  endif()
  execute_process(
    COMMAND ${GIT_EXECUTABLE} status --porcelain
    WORKING_DIRECTORY ${repo_dir}
    OUTPUT_VARIABLE st
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc
    ERROR_QUIET
  )
  if(rc EQUAL 0 AND NOT "${st}" STREQUAL "")
    set(${out_var} "true" PARENT_SCOPE)
  endif()
endfunction()

pc_git_short_sha(PAINT_PC_GIT_SHA "${CMAKE_SOURCE_DIR}")
pc_git_is_dirty(PAINT_PC_GIT_DIRTY "${CMAKE_SOURCE_DIR}")
pc_git_short_sha(PAINT_SDK_GIT_SHA "${CMAKE_SOURCE_DIR}/sdk")
pc_git_is_dirty(PAINT_SDK_GIT_DIRTY "${CMAKE_SOURCE_DIR}/sdk")
message(STATUS "版本戳: pc ${PAINT_PC_GIT_SHA}(dirty=${PAINT_PC_GIT_DIRTY}) / sdk ${PAINT_SDK_GIT_SHA}(dirty=${PAINT_SDK_GIT_DIRTY})")

configure_file(
  "${CMAKE_SOURCE_DIR}/src/version.h.in"
  "${CMAKE_BINARY_DIR}/generated/version.h"
  @ONLY
)

# --- SDK（git submodule，路径固定 sdk/）--------------------------------------
# 只 add_subdirectory + 链接 dgc_paint，不 include SDK 内部头。
add_subdirectory(sdk)

# --- 第三方：GLFW + ImGui（无系统包，FetchContent 拉源码）--------------------
include(FetchContent)

set(PC_GLFW_TAG "3.3.10")
set(PC_IMGUI_TAG "v1.90.9")

# 国内镜像加速 FetchContent（可选）：PC_FETCH_MIRROR=https://ghproxy.com/ 时替换 GitHub 前缀。
# PC_GLFW_TAG / PC_IMGUI_TAG 已在上方定义，此处不重复 set。
set(PC_FETCH_MIRROR "$ENV{PC_FETCH_MIRROR}" CACHE STRING "GitHub 镜像前缀（国内加速）")

if(PC_FETCH_MIRROR)
  set(PC_GLFW_REPO "${PC_FETCH_MIRROR}https://github.com/glfw/glfw.git")
  set(PC_IMGUI_REPO "${PC_FETCH_MIRROR}https://github.com/ocornut/imgui.git")
else()
  set(PC_GLFW_REPO "https://github.com/glfw/glfw.git")
  set(PC_IMGUI_REPO "https://github.com/ocornut/imgui.git")
endif()

FetchContent_Declare(
    glfw
    GIT_REPOSITORY ${PC_GLFW_REPO}
    GIT_TAG ${PC_GLFW_TAG}
    GIT_SHALLOW TRUE
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY ${PC_IMGUI_REPO}
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
    src/headless.cpp
    src/coords.cpp
)
target_include_directories(paint_pc PRIVATE src "${CMAKE_BINARY_DIR}/generated")

# MSVC 在非 UTF-8 系统代码页（如简体中文 936/GBK）下默认按代码页解释源文件，
# 源码里的中文字符串字面量（app.cpp 等）会被错误解码，导致 C2001「字符串字面量中的
# 换行符」等报错；/utf-8 强制源码 + 执行字符集按 UTF-8 解释，与文件的实际编码一致。
# GCC/Clang 默认按 UTF-8 读源码，不需要也不识别该选项。
if(MSVC)
  target_compile_options(paint_pc PRIVATE /utf-8)
endif()

# 只链接 dgc_paint 与自备 UI 依赖。不要链接 SDK 内部 target。
target_link_libraries(paint_pc PRIVATE dgc_paint paint_imgui glfw)

# OpenGL：不同平台链接名不同（Linux 用 OpenGL::GL）。
find_package(OpenGL REQUIRED)
target_link_libraries(paint_pc PRIVATE OpenGL::GL)

# --- 坐标映射纯函数回归测试（无头可跑，不链接 dgc_paint/GLFW）----------------
add_executable(paint_pc_coord_test tests/coord_test.cpp src/coords.cpp)
target_include_directories(paint_pc_coord_test PRIVATE src)
if(MSVC)
  target_compile_options(paint_pc_coord_test PRIVATE /utf-8)
endif()
add_test(NAME coord_test COMMAND paint_pc_coord_test)
