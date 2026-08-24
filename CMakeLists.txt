cmake_minimum_required(VERSION 3.22)
project(paint_pc LANGUAGES C CXX)

# paint-pc：PC UI 消费者（窗口 / 输入 / present 自备）。
# 本骨架只演示如何把 paintDemo SDK 编进消费者的 PC 应用。

# sdk/ 是 git submodule（路径固定 sdk/），指向 paintDemo 仓库。
add_subdirectory(sdk)

# 消费者的可执行文件（窗口 / 输入 / present，消费者自备实现）。
add_executable(paint_pc
    src/main.cpp
)

# 只链接 dgc_paint。不要链接 SDK 内部 target。
target_link_libraries(paint_pc PRIVATE dgc_paint)
