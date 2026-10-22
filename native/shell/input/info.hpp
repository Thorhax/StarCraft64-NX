#pragma once
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// Owned per-frame feedback snapshot consumed by the input adapters. Optional
// members mirror values the presenter omits (and synthetic fixtures may omit);
// the adapters treat a missing value exactly like the former Lua nil.
namespace sc64::input {
struct Bounds {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::optional<double> context;
};
struct Carousel {
    double selected = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool ready = false;
};
struct UiItem {
    double id = 0, group = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::optional<double> columns, buttons, context, label;
    bool selected = false, horizontal = false, activate = false;
};
struct CursorPick {
    double x = 0, y = 0, context = 0, buttons = 0, target_type = 0;
    bool worker_selected = false, target_selected = false;
};
struct Info {
    double fields = 0;
    std::optional<double> ui_field, cursor_field;
    std::optional<double> width, height;
    std::optional<std::string> ui_mode;
    std::optional<double> ui_token, ui_context;
    bool running = false, paused = false;
    std::optional<bool> input_active;
    double cursor_x = 0, cursor_y = 0;
    std::optional<double> cursor_divisor;
    bool cursor_valid = false, camera_valid = false;
    double camera_x = 0, camera_y = 0;
    double camera_min_x = 0, camera_max_x = 0, camera_min_y = 0, camera_max_y = 0;
    std::optional<double> camera_divisor;
    std::optional<Bounds> minimap, ui_bounds;
    std::optional<Carousel> ui_carousel;
    std::vector<UiItem> ui_items;
    bool pick_required = false;
    std::optional<CursorPick> cursor_pick;
    double minimap_jumps = 0, minimap_field = 0;
    double presented = 0, message_id = 0, guest_mode = -1;
    std::string error;
};

constexpr double max_feedback_age = 12;
inline bool finite(std::optional<double> value) { return value && std::isfinite(*value); }
inline bool finite(double value) { return std::isfinite(value); }
// Closed rectangle: x1/y1 are the last covered pixel (frontend/menu items).
template <class Rect>
bool inside_closed(const Rect& b, double x, double y) {
    return x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1;
}
template <class Rect>
bool inside_closed(const std::optional<Rect>& b, double x, double y) { return b && inside_closed(*b, x, y); }
// Half-open rectangle: x1/y1 are one past the last pixel (HUD/minimap bounds).
template <class Rect>
bool inside_half_open(const Rect& b, double x, double y) {
    return x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1;
}
template <class Rect>
bool inside_half_open(const std::optional<Rect>& b, double x, double y) {
    return b && inside_half_open(*b, x, y);
}
// True when the source field is no newer than now and at most
// max_feedback_age fields old. `missing` substitutes an absent source field.
inline bool fresh(const Info& info, std::optional<double> source, double missing = -100) {
    const double age = info.fields - source.value_or(missing);
    return age >= 0 && age <= max_feedback_age;
}
inline bool fresh_ui(const Info& info) { return fresh(info, info.ui_field); }
// The one-player mission image: 320x240, or up to 512x240 with the widescreen
// option (widescreen.hpp), which keeps the same 2:1 cursor and camera units.
inline bool mission_image(double width, double height) { return height == 240 && width >= 320 && width <= 512; }
// Map gestures, group commands and the cursor transform are verified only for
// the one-player mission image (research.md, D-042). Fixtures that omit
// dimensions describe that layout.
inline bool map_layout(const Info& info) {
    return !info.width || (info.height && mission_image(*info.width, *info.height));
}
inline bool mode_is(const Info& info, const char* mode) { return info.ui_mode && *info.ui_mode == mode; }
// Lua's floored modulo, used for ring and grid arithmetic.
inline double floored_mod(double a, double b) { return a - std::floor(a / b) * b; }
}
