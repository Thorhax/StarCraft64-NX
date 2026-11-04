#pragma once

// The USB mouse pointer on Switch, drawn by app.cpp as a software cursor. It shows
// after mouse use and hides on touch or controller input; platform.cpp's
// sc64_update_mouse_cursor hides it where the game's own cursor follows the mouse.
struct SwitchPointer {
    bool shown = false, game_cursor = false;
    double x = 0, y = 0; // screen pixels, top-left origin
};
SwitchPointer& switch_pointer();
