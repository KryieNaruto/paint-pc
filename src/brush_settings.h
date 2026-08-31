#pragma once
#include "dgc_paint_c_api.h"
namespace paint {
// 画笔/Stroke Modeler 参数下发统一入口（App 面板 + 无头回归测试共用）。
// 生产约定：strokeActive==true（笔画进行中）不下发，改参只在两笔画之间生效。
// 句柄固定 DGC_DEFAULT_BRUSH：内核基础参数(0-2)只对内核已建默认笔刷生效，
// dgcCreateBrush 发号器句柄（非 1）会被内核静默忽略（BrushKernel::setBrushSetting no-op），
// 是"画笔设置无效"类 bug 的根因之一。返回 dgcSetBrushSetting 返回码；跳过时返回 DGC_OK。
int ApplyBrushSetting(DgcContext* sdk, bool strokeActive, int settingId, double value, const char* label);
}
