#!/usr/bin/env bash
# tests/smoke.sh —— paint-pc 无头冒烟：构建 + headless 离屏导出 PNG
#
# 本 workspace 无系统 libvulkan-dev / libshaderc-dev / libxrandr-dev，SDK B2-1
# 与 GLFW 的依赖由两处 deps root 补齐（可用环境变量覆盖；指向不存在路径时
# find_path 自动落到系统默认搜索路径，语义无害）：
#   DGCPAIN_DEPS_ROOT —— libvulkan-dev + libshaderc-dev（sdk/render/vulkan 用）
#   PC_X11_DEPS_ROOT  —— libx11 / libxrandr / libopengl 开发头（GLFW 用）
set -euo pipefail
cd "$(dirname "$0")/.."
DGCPAIN_DEPS_ROOT="${DGCPAIN_DEPS_ROOT:-/tmp/dgc-deps/usr}"
PC_X11_DEPS_ROOT="${PC_X11_DEPS_ROOT:-/home/qiansenwei/.local/dgc-x11dev/usr}"
# 必须带 -DDGCPAIN_BUILD_TESTS=OFF：消费者根下 SDK tests 因 CMAKE_SOURCE_DIR 错位会失效。
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
    -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF \
    -DDGCPAIN_DEPS_ROOT="$DGCPAIN_DEPS_ROOT" \
    -DCMAKE_PREFIX_PATH="$PC_X11_DEPS_ROOT" \
    -DCMAKE_C_FLAGS="-I$PC_X11_DEPS_ROOT/include" \
    -DCMAKE_CXX_FLAGS="-I$PC_X11_DEPS_ROOT/include"
cmake --build build -j
out=$(mktemp /tmp/paint_pc_headless.XXXXXX.png)
./build/paint_pc --headless "$out"
[ -s "$out" ] || { echo "FAIL: PNG empty/missing: $out"; exit 1; }
echo "PASS: headless PNG $(stat -c%s "$out") bytes"
