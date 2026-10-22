#pragma once
// 1P widescreen (tools/recompile/apply_widescreen.py rewrites the recompiled 1P
// constants to read these). Defaults are the original 320x240 mission view;
// set before the guest starts.
extern "C" {
extern int sc64_ws_width, sc64_ws_fog_width, sc64_ws_cursor_max, sc64_ws_command_x, sc64_ws_status_x;
}
namespace sc64::runtime {
// width: mission canvas width in 320x240 pixels (multiple of 16, 320..512).
inline void set_widescreen(int width) {
    sc64_ws_width = width;
    sc64_ws_fog_width = 2 * (((2 * width + 31) >> 5) + 4); // 2x the visibility grid (80034578)
    sc64_ws_cursor_max = 2 * width - 32;                    // 1P cursor X limit, 640-unit space
    sc64_ws_command_x = 176 + width - 320;                  // command canvas, as far from the right edge
    sc64_ws_status_x = 86 + (width - 320) / 2;              // status line, centred between the panels
}
}
