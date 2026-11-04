#include "gestures.hpp"

namespace sc64::input::hotkeys {
namespace {
constexpr double press_fields = 8, wait_fields = 45;
// Group N is C-Up, C-Left, C-Right, C-Down (research.md, group assign/recall).
constexpr std::array<unsigned, 4> group_c = {0x0008, 0x0002, 0x0001, 0x0004};

bool usable(const Info& info) {
    return info.running && !info.paused && info.input_active != false && fresh_ui(info);
}
const UiItem* find(const Info& info, double group, char key) {
    for (const auto& item : info.ui_items)
        if (item.group == group && item.hotkey == key && (group != 1 || item.activate)) return &item;
    return nullptr;
}
// The cell is clicked like a mouse click: ui_input navigates to it and presses A.
void click(GestureState& s, const UiItem& item, const Info& info) {
    const double x = (item.x0 + item.x1) / 2, y = (item.y0 + item.y1) / 2;
    ui_input::pointer(s, "hotkey", Phase::down, x, y, info, false, false);
    ui_input::pointer(s, "hotkey", Phase::up, x, y, info, false, false);
}
}

bool key(GestureState& s, char key, bool ctrl, const Info& info) {
    if (!mode_is(info, "game") && !mode_is(info, "build")) return false;
    if (!usable(info)) return true;
    const double field = info.fields;
    if (key >= '0' && key <= '9') {
        if (!mode_is(info, "game")) return true;
        if (key < '1' || key > '4') { s.notice = group_notice; return true; }
        // Assign: R, R+C, R; recall: Z, Z+C, Z (C released inside the modifier).
        const unsigned modifier = ctrl ? button_r : button_z, c = group_c[unsigned(key - '1')];
        sequence(s, Steps{{modifier, 4}, {modifier | c, 8}, {modifier, 6}}, field);
        return true;
    }
    s.hotkey.reset();
    if (mode_is(info, "build")) {
        if (const auto* cell = find(info, 1, key)) click(s, *cell, info);
        else s.notice = missing_notice;
        return true;
    }
    if (const auto* hint = find(info, 4, key)) { button(s, unsigned(hint->buttons.value_or(0)), field); return true; }
    HotkeySearch search;
    search.letter = key;
    for (const auto& item : info.ui_items)
        if (item.group == 4 && item.grid && item.buttons) search.openers.push_back(unsigned(*item.buttons));
    if (search.openers.empty()) { s.notice = missing_notice; return true; }
    clear(s);
    search.press_until = field + press_fields; search.give_up = field + wait_fields;
    s.hotkey = std::move(search);
    return true;
}

std::optional<PadOutput> read(GestureState& s, const Info& info) {
    if (!s.hotkey) return std::nullopt;
    auto& h = *s.hotkey;
    const double field = info.fields;
    if (!info.running || info.paused || field > h.give_up) {
        s.hotkey.reset(); s.notice = missing_notice; return PadOutput{};
    }
    if (field < h.press_until) return PadOutput{h.closing ? button_b : h.openers[h.index]};
    if (!fresh_ui(info)) return PadOutput{};
    if (h.closing) {
        if (!mode_is(info, "game")) return PadOutput{};
        h.closing = false;
        if (++h.index >= h.openers.size()) { s.hotkey.reset(); s.notice = missing_notice; return PadOutput{}; }
        h.press_until = field + press_fields; h.give_up = field + wait_fields;
        return PadOutput{h.openers[h.index]};
    }
    if (!mode_is(info, "build")) return PadOutput{};
    if (const auto* cell = find(info, 1, h.letter)) {
        s.hotkey.reset();
        click(s, *cell, info);
        return PadOutput{};
    }
    h.closing = true;
    h.press_until = field + press_fields; h.give_up = field + wait_fields;
    return PadOutput{button_b};
}
}
