#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace ir::gui {

// Modal snipping overlay over one monitor. Returns false on Esc / cancel.
// out_crop is in capture-pixel coordinates (0..capture_w, 0..capture_h), even size.
bool pick_screen_region(
    HWND owner,
    const RECT& monitor_screen,
    int capture_w,
    int capture_h,
    RECT* out_crop);

}  // namespace ir::gui
