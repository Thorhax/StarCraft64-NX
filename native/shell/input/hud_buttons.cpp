// Small always-available corner buttons in the HUD style, one set per screen:
//   game         1P mission: sandwich (Start = pause menu), minimap (Z+L)
//   encyclopedia main-menu Encyclopedia: back (B), previous/next article (D-pad)
// They are controller buttons like every other pointer action and never touch
// guest memory. Geometry is in 320x240 units; 640x480 menu images are scaled by
// one half, so the buttons keep the same relative size and place.
// Mission: the left strip x<16 is free of original HUD (selection text and the
// minimap start at x=17), so hit areas may grow inside it and outward into the
// letterbox. Encyclopedia: the top-left and bottom-right corners are empty.
#include "gestures.hpp"

namespace sc64::input::hud_buttons {
namespace {
constexpr unsigned button_z_l = 0x2020;
const std::array<Set, 2> button_sets = {{
    {"game", {{
        {"menu", button_start, Glyph::menu, 2, 2, {-16, -16, 16, 28}},
        {"map", button_z_l, Glyph::map, 2, 226, {-16, 212, 16, 256}},
        {},
    }}, 2},
    {"encyclopedia", {{
        {"back", button_b, Glyph::menu, 2, 2, {-16, -16, 16, 28}},
        {"prev", button_left, Glyph::prev, 292, 226, {280, 208, 305, 256}},
        {"next", button_right, Glyph::next, 306, 226, {305, 208, 336, 256}},
    }}, 3},
}};
bool live(const Info& info) {
    return info.running && !info.paused && info.input_active != false && fresh_ui(info);
}
bool idle(const GestureState& s) {
    // A finished menu pulse leaves s.ui behind until its timeout; only a live
    // contact or a HUD hint candidate owns the pointer.
    return s.contacts.empty() && !(s.drag || s.two || s.pan || s.minimap || s.seq || s.pulse || s.radial
        || (s.ui && (s.ui->hud || s.ui->contact)));
}
const Def* hit(std::string_view set_name, double x, double y) {
    for (const auto& set : button_sets) {
        if (set.name != set_name) continue;
        for (std::size_t i = 0; i < set.count; ++i) {
            const auto& h = set.defs[i].hit;
            if (x >= h[0] && x < h[2] && y >= h[1] && y < h[3]) return &set.defs[i];
        }
    }
    return nullptr;
}
}

const std::array<Set, 2>& sets() { return button_sets; }

std::optional<std::string_view> set(const Info& info) {
    if (!live(info)) return std::nullopt;
    if (mode_is(info, "game") && info.width && info.height && mission_image(*info.width, *info.height)) return "game";
    if (mode_is(info, "menu") && info.ui_token == encyclopedia_token && info.width == 640 && info.height == 480)
        return "encyclopedia";
    return std::nullopt;
}

PointerResult pointer(GestureState& s, const ContactId& id, Phase phase, double x, double y, const Info& info,
                      bool is_touch) {
    const auto current = set(info);
    if (phase == Phase::cancel || (s.hud_button && std::optional<std::string_view>(s.hud_button->set) != current)) {
        const bool had = s.hud_button.has_value();
        if (s.hud_button && (phase != Phase::cancel || s.hud_button->contact == id)) s.hud_button.reset();
        s.button_hover.reset();
        return {had, std::nullopt};
    }
    const Def* b = nullptr;
    if (current) {
        const double k = 240 / *info.height; // 320x240 units from the image's top-left
        b = hit(*current, x * k, y * k);
    }
    if (!s.hud_button) {
        if (!is_touch && idle(s) && b) s.button_hover = std::string(b->id);
        else s.button_hover.reset();
        if (!b || !idle(s)) return {false, std::nullopt};
        if (phase == Phase::down) {
            s.hud_button = ButtonContact{id, std::string(b->id), std::string(*current), {id}, false};
            s.hover.reset();
        } else if (is_touch) return {false, std::nullopt};
        return {true, std::nullopt};
    }
    auto& c = *s.hud_button;
    if (phase == Phase::down && !c.contacts.contains(id)) {
        c.contacts.insert(id); c.cancelled = true; // a second finger abandons the tap
        return {true, std::nullopt};
    }
    if (!c.contacts.contains(id)) return {false, std::nullopt};
    if (id == c.contact && (!b || b->id != c.id)) c.cancelled = true;
    if (phase == Phase::up) {
        c.contacts.erase(id);
        std::optional<unsigned> fire;
        if (id == c.contact && !c.cancelled && b && b->id == c.id) fire = b->bits;
        const bool owner = id == c.contact;
        if (c.contacts.empty()) s.hud_button.reset();
        if (owner) s.button_hover.reset();
        return {true, fire};
    }
    return {true, std::nullopt};
}

std::optional<std::string_view> state(const GestureState& s, std::string_view id) {
    const auto& c = s.hud_button;
    if (c && c->id == id && !c->cancelled) return "pressed";
    if (!c && s.button_hover == id) return "hover";
    return std::nullopt;
}
}
