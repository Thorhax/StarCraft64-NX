#include "gestures.hpp"
#include <cmath>

namespace sc64::input::minimap_input {
namespace {
// Minimap bounds are half-open: x1/y1 lie outside.
bool ready(const Info& info) {
    return info.running && !info.paused && info.input_active != false && mode_is(info, "game")
        && info.width && info.height && mission_image(*info.width, *info.height) && info.minimap && fresh(info, info.cursor_field);
}
void request(GestureState& s, double x, double y, const Info& info) {
    s.minimap_request = MinimapRequest{x, y, info.minimap->context, info.cursor_field.value_or(0)};
}
}

bool pointer(GestureState& s, const ContactId& id, Phase phase, double x, double y, const Info& info,
             bool right, bool is_touch) {
    if (s.minimap) {
        auto& m = *s.minimap;
        if (!ready(info) || info.minimap->context != m.context) {
            m.cancelled = true; s.minimap_request.reset();
        }
        if (phase == Phase::down && id != m.contact) {
            m.contacts.insert(id); m.cancelled = true; s.minimap_request.reset();
        } else if (m.contacts.contains(id)) {
            if (phase == Phase::cancel || right || !inside_half_open(m.bounds, x, y)) {
                m.cancelled = true; s.minimap_request.reset();
            }
            if (!m.cancelled && id == m.contact) {
                if (is_touch) {
                    if ((x - m.x) * (x - m.x) + (y - m.y) * (y - m.y) > 100) m.cancelled = true;
                    if (phase == Phase::up && !m.cancelled) request(s, x, y, info);
                } else if (phase == Phase::move && (std::floor(x) != std::floor(m.last_x)
                                                    || std::floor(y) != std::floor(m.last_y))) {
                    request(s, x, y, info); m.last_x = x; m.last_y = y;
                }
            }
            if (phase == Phase::up || phase == Phase::cancel) m.contacts.erase(id);
        }
        if (m.contacts.empty()) s.minimap.reset();
        return true;
    }
    if (!s.enabled || !ready(info) || right || !inside_half_open(info.minimap, x, y)
        || !s.contacts.empty() || s.drag || s.two || s.pan
        || (s.ui && (!s.ui->hud || !s.ui->contacts.empty())))
        return false;
    // Hover over the minimap must not start the ordinary cursor servo either.
    const auto pending = s.minimap_request;
    clear(s);
    if (phase == Phase::move) s.minimap_request = pending;
    if (phase == Phase::down) {
        MinimapGesture m;
        m.contact = id; m.contacts = {id}; m.bounds = *info.minimap; m.context = info.minimap->context;
        m.x = x; m.y = y; m.last_x = x; m.last_y = y;
        s.minimap = std::move(m);
        if (!is_touch) request(s, x, y, info);
    }
    return true;
}

std::optional<MinimapRequest> take(GestureState& s, const Info& info) {
    auto r = std::exchange(s.minimap_request, std::nullopt);
    if (!r || !ready(info) || r->context != info.minimap->context
        || info.fields < r->field || info.fields - r->field > max_feedback_age)
        return std::nullopt;
    return r;
}
}
