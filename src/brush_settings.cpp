#include "brush_settings.h"
#include <cstdio>
namespace paint {
int ApplyBrushSetting(DgcContext* sdk, bool strokeActive, int settingId, double value, const char* label) {
    if (sdk == nullptr) return DGC_ERR_NULL_CONTEXT;
    if (strokeActive) return DGC_OK;  // 生产约定：两笔画之间改参
    int rc = dgcSetBrushSetting(sdk, DGC_DEFAULT_BRUSH, settingId, value);
    if (rc != DGC_OK) {
        std::fprintf(stderr, "[paint-pc] setBrushSetting(%s): %s\n", label, dgcGetLastError());
    }
    return rc;
}
}
