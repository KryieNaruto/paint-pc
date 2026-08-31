#include "canvas_input.h"
namespace paint {
bool ShouldHandleCanvasPointer(bool wantCaptureMouse) {
    return !wantCaptureMouse;
}
}
