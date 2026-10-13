#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace sc64::runtime {
// Touch cursor warp, x/y in 320x240 image units. Applied on the guest thread
// at the start of the next cursor query (one-player mission only).
void request_cursor_warp(double x, double y);
struct UiItem {
    unsigned id = 0, group = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool selected = false, horizontal = false, activate = true;
    unsigned columns = 0;
    unsigned buttons = 0;
    std::uint32_t context = 0;
    std::uint32_t label = 0;
};
struct UiBounds {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::uint32_t context = 0;
};
struct MainMenuFeedback {
    unsigned campaign = 0;
    bool ready = false;
    int warning = -1;
    std::optional<UiBounds> warning_bounds;
};
struct CursorPick {
    std::uint32_t context = 0;
    float x = 0, y = 0;
    std::int32_t camera_x = 0, camera_y = 0;
    unsigned buttons = 0x8000;
    int target_type = -1;
    bool worker_selected = false;
    // The hit-tested unit is one of the player's currently selected units.
    bool target_selected = false;
};
// Owned metadata from read-only hooks. Font boxes are painted into temporary
// sprites; their screen positions are known only when those sprites are blitted.
struct FrontendFeedback {
    std::vector<UiItem> items;
    bool briefing = false;
    std::optional<UiBounds> hud_bounds;
    std::optional<MainMenuFeedback> main;
    std::optional<CursorPick> pick;
};
FrontendFeedback take_frontend_feedback();
// Selection and command context stamp: the original selected-list pointers
// plus the command-table selector. Do not retain targets when another
// selection reuses the same HUD pixel buffer. byte_at(physical) returns the
// canonical (big-endian) byte at that physical RDRAM address.
template<class ByteAt>
std::uint32_t hud_context_hash(ByteAt&& byte_at) {
    std::uint32_t hash = 2166136261u;
    for (std::uint32_t a : {0xc6c14u, 0xc6c18u, 0xfd0b0u})
        for (std::uint32_t i = 0; i < 4; ++i) hash = (hash ^ byte_at(a + i)) * 16777619u;
    for (std::uint32_t i = 0; i < 72; ++i) hash = (hash ^ byte_at(0xc6c20 + i)) * 16777619u;
    return hash;
}
// Live guest RDRAM, stored word-swapped by the recompiled runtime.
inline std::uint32_t hud_context(const std::uint8_t* ram) {
    return hud_context_hash([ram](std::uint32_t physical) { return ram[physical ^ 3]; });
}
}
