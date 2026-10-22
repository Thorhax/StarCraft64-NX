// Port of tests/test_hud_buttons.lua: always-available corner buttons, the
// letterbox viewport and the Encyclopedia pager/keyboard mapping.
#include "devices.hpp"
#include "gestures.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void fail(const char* file, int line, const char* expr) {
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
    std::abort();
}
}
#define CHECK(expr) do { if (!(expr)) fail(__FILE__, __LINE__, #expr); } while (0)

using namespace sc64::input;
namespace buttons = hud_buttons;
namespace pads = pad_state;

int main() {
    using enum Phase;
    GestureState s;
    Info info;
    info.fields = 100; info.ui_field = 100; info.ui_token = 3; info.ui_mode = "game";
    info.running = true; info.paused = false;
    info.cursor_valid = true; info.cursor_field = 100; info.width = 320; info.height = 240;
    const auto tick = [&](double field) {
        info.fields = field; info.ui_field = field; info.cursor_field = field;
        return read(s, info, false).buttons;
    };
    const auto stamp = [&](double field) { info.fields = field; info.ui_field = field; info.cursor_field = field; };
    const auto pointer = [&](const ContactId& id, Phase phase, double x, double y, bool touch) {
        return button_pointer(s, id, phase, x, y, info, touch);
    };
    // Tap on the sandwich: Start for one pulse, no cursor or terrain action.
    CHECK(pointer("1", down, 8, 8, true) && buttons::state(s, "menu") == "pressed");
    CHECK(tick(100) == 0);
    CHECK(pointer("1", up, 8, 8, true) && !s.hud_button);
    CHECK(tick(101) == 0x1000 && !s.hover && s.queue.empty());
    CHECK(tick(108) == 0);
    // Minimap toggle is original Z+L.
    clear(s); stamp(119);
    CHECK(pointer("1", down, 8, 230, true) && pointer("1", up, 8, 230, true));
    CHECK(tick(120) == 0x2020);
    // Hit areas reach the letterbox and the free left strip, never the HUD.
    clear(s); stamp(129);
    CHECK(pointer("1", down, -10, -8, true)); // letterbox, above the sandwich
    CHECK(pointer("1", up, -10, -8, true) && tick(130) == 0x1000);
    clear(s);
    CHECK(!pointer("1", down, 20, 8, true)); // selection text / minimap start at x=17
    CHECK(!pointer("1", down, 8, 100, true)); // between the two buttons
    CHECK(!pointer("1", down, 8, 205, true)); // above the minimap button area
    // Sliding off, second finger, right-hand state and stale feedback cancel.
    clear(s);
    CHECK(pointer("1", down, 8, 8, true)); CHECK(pointer("1", move, 8, 100, true));
    CHECK(pointer("1", up, 8, 100, true) && tick(140) == 0);
    CHECK(pointer("1", down, 8, 8, true)); CHECK(pointer("2", down, 8, 8, true));
    CHECK(pointer("2", up, 8, 8, true) && pointer("1", up, 8, 8, true) && tick(141) == 0);
    CHECK(pointer("1", down, 8, 8, true)); info.ui_mode = "build";
    CHECK(pointer("1", up, 8, 8, true) && !s.hud_button && tick(142) == 0);
    info.ui_mode = "game";
    info.fields = 200; info.ui_field = 100; // stale owned feedback is not trusted
    CHECK(!pointer("1", down, 8, 8, true));
    // Hidden when paused, modal, or on the unverified 2P layout.
    {
        const auto old = info.paused; info.paused = true; tick(150);
        CHECK(!buttons::set(info) && !pointer("1", down, 8, 8, true));
        info.paused = old;
    }
    {
        // Split screen (512x384) has no verified button layout.
        const auto old_w = info.width, old_h = info.height; info.width = 512; info.height = 384; tick(150);
        CHECK(!buttons::set(info) && !pointer("1", down, 8, 8, true));
        info.width = old_w; info.height = old_h;
    }
    {
        // The widescreen mission image keeps the buttons.
        const auto old = info.width; info.width = 416; tick(150);
        CHECK(buttons::set(info) == std::optional<std::string_view>("game"));
        info.width = old;
    }
    {
        const auto old = info.ui_mode; info.ui_mode = "menu"; tick(150);
        CHECK(!buttons::set(info) && !pointer("1", down, 8, 8, true));
        info.ui_mode = old;
    }
    // A map gesture in flight keeps its contact; hover is only highlighted.
    clear(s); tick(160);
    sc64::input::pointer(s, "1", down, 100, 100, 160, true, &info);
    CHECK(!pointer("2", down, 8, 8, true) && !s.contacts.empty());
    clear(s);
    CHECK(pointer("mouse", move, 8, 8, false) && buttons::state(s, "menu") == "hover");
    CHECK(!buttons::state(s, "map"));
    CHECK(!pointer("mouse", move, 100, 100, false) && !buttons::state(s, "menu"));
    // Mouse click behaves like a tap and clears the hover highlight afterwards.
    stamp(169);
    CHECK(pointer("mouse", move, 8, 230, false) && buttons::state(s, "map") == "hover");
    CHECK(pointer("mouse", down, 8, 230, false) && buttons::state(s, "map") == "pressed");
    CHECK(pointer("mouse", up, 8, 230, false) && tick(170) == 0x2020);
    // Cancellation of the owning contact drops the press without an action.
    clear(s);
    CHECK(pointer("5", down, 8, 8, true)); CHECK(pointer("5", cancel, 8, 8, true));
    CHECK(!s.hud_button && tick(180) == 0);
    // Disabled gestures (diagnostic pad) leave the corner to the pad.
    s.enabled = false; CHECK(!pointer("1", down, 8, 8, true)); s.enabled = true;
    // Letterbox geometry: raw conversion is unclipped, the clipped one is not.
    const auto rect = viewport::rectangle(2400, 1080, 320, 240);
    CHECK(rect && rect->x == 480 && rect->width == 1440);
    const auto raw = viewport::to_native_raw(rect, rect->x - 10, 1079);
    CHECK(raw && raw->x < 0 && raw->y < 1 && !viewport::to_native(rect, rect->x - 10, 1079));
    const auto corner = viewport::to_native_raw(rect, rect->x + 4, 1079 - 4);
    CHECK(corner && std::abs(corner->x - 1) < 1 && std::abs(corner->y - 1) < 1);
    // Encyclopedia (menu 8, token 264, 640x480): back, previous and next article.
    clear(s);
    info = Info{};
    info.fields = 500; info.ui_field = 500; info.ui_token = 264; info.ui_mode = "menu";
    info.running = true; info.paused = false; info.width = 640; info.height = 480;
    const auto at = [&](double field) { info.fields = field; info.ui_field = field; };
    const auto enc_tick = [&](double field) {
        info.fields = field; info.ui_field = field;
        return read(s, info, false).buttons;
    };
    CHECK(buttons::set(info) == "encyclopedia");
    {
        Info main_menu;
        main_menu.running = true; main_menu.fields = 1; main_menu.ui_field = 1; main_menu.ui_mode = "menu";
        main_menu.ui_token = 256; main_menu.width = 640; main_menu.height = 480;
        CHECK(!buttons::set(main_menu));
    }
    // 640x480 coordinates are halved: next button sits at the bottom-right corner.
    CHECK(pointer("1", down, 2 * 312, 2 * 232, true) && buttons::state(s, "next") == "pressed");
    CHECK(pointer("1", up, 2 * 312, 2 * 232, true));
    CHECK(enc_tick(501) == 0x0100); // D-pad right = next article
    enc_tick(520); clear(s);
    CHECK(pointer("1", down, 2 * 296, 2 * 232, true) && pointer("1", up, 2 * 296, 2 * 232, true));
    CHECK(enc_tick(521) == 0x0200); // previous
    enc_tick(540); clear(s);
    CHECK(pointer("1", down, 2 * 8, 2 * 8, true) && pointer("1", up, 2 * 8, 2 * 8, true));
    CHECK(enc_tick(541) == 0x4000); // back to the main menu (original B)
    enc_tick(560); clear(s);
    // The articles carry no other targets: a tap elsewhere is not a button.
    CHECK(!pointer("1", down, 2 * 160, 2 * 120, true));
    // The hit areas reach the right letterbox beside the next button.
    CHECK(pointer("1", down, 2 * 330, 2 * 232, true) && buttons::state(s, "next") == "pressed");
    CHECK(pointer("1", up, 2 * 330, 2 * 232, true)); clear(s);
    // A finished swipe leaves its menu state behind; a button tap still works.
    at(579);
    CHECK(ui_pointer(s, "1", down, 300, 200, info, false, true));
    CHECK(ui_pointer(s, "1", move, 250, 205, info, false, true));
    CHECK(ui_pointer(s, "1", up, 250, 205, info, false, true)); // dx=-50: next article
    CHECK(enc_tick(580) == 0x0100 && enc_tick(590) == 0);
    at(599);
    CHECK(pointer("1", down, 2 * 8, 2 * 8, true) && pointer("1", up, 2 * 8, 2 * 8, true));
    CHECK(enc_tick(600) == 0x4000);
    clear(s); at(619);
    CHECK(ui_pointer(s, "1", down, 250, 200, info, false, true));
    CHECK(ui_pointer(s, "1", up, 320, 205, info, false, true)); // drag right: previous
    CHECK(enc_tick(620) == 0x0200);
    // A short tap or a vertical drag turns no page.
    clear(s); at(639);
    CHECK(ui_pointer(s, "1", down, 250, 200, info, false, true) && ui_pointer(s, "1", up, 252, 201, info, false, true));
    CHECK(enc_tick(640) == 0);
    clear(s); at(659);
    CHECK(ui_pointer(s, "1", down, 250, 200, info, false, true) && ui_pointer(s, "1", up, 255, 300, info, false, true));
    CHECK(enc_tick(660) == 0);
    // Desktop arrow keys are the original D-pad: they page the Encyclopedia too.
    pads::State state;
    pads::input(state, "dright", {.pressed = true}, 700, &info);
    CHECK(pads::read(state, 1, 700).buttons == 0x0100);
    pads::input(state, "dright", {.released = true}, 700, &info);
    pads::input(state, "dleft", {.pressed = true}, 710, &info);
    CHECK(pads::read(state, 1, 710).buttons == 0x0200);
    // Enter (keyboard Start) follows the highlighted article link; a gamepad's
    // Start keeps its original meaning, and Enter is plain Start elsewhere.
    info.fields = 800; info.ui_field = 800;
    state = pads::State{};
    pads::input(state, "start", {.pressed = true}, 800, &info);
    CHECK(pads::read(state, 1, 800).buttons == 0x8000);
    state = pads::State{};
    pads::input(state, "start", {.pressed = true, .gamepad = 0}, 800, &info);
    CHECK(pads::read(state, 1, 800).buttons == 0x1000);
    state = pads::State{};
    info.ui_token = 256;
    pads::input(state, "start", {.pressed = true}, 800, &info);
    CHECK(pads::read(state, 1, 800).buttons == 0x1000);
    std::puts("HUD corner buttons passed");
}
