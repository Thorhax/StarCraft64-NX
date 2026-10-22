#pragma once
#include "../runtime/ui_feedback.hpp"
#include "../runtime/guest_addresses.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <string>

namespace sc64::desktop {
struct UiFeedback {
    std::string mode = "unknown";
    std::uint64_t token = 0;
    std::uint64_t field = 0;
    int guest_mode = -1;
    int message_id = -1;
    std::uint32_t context = 0;
    std::optional<runtime::UiBounds> bounds;
    std::optional<runtime::MainMenuFeedback> main;
    std::optional<runtime::UiBounds> carousel_bounds;
    std::vector<runtime::UiItem> items;
    bool pick_required = false;
    std::optional<runtime::CursorPick> pick;
    // Results screens only: the outcome bit 8012CA48 stores at 8011281C (1 when
    // the local player's game result is 1, a win), which also selects the
    // victory or defeat menu (13..18) and music.
    std::optional<bool> victory;
};
// Canonical, owned NTSC-U RDRAM only. Invalid pointers/cycles fail closed. No
// arbitrary object callback is called, and no guest memory is modified.
inline UiFeedback read_ui(std::span<const std::uint8_t> ram, std::uint64_t field,
                          const runtime::FrontendFeedback& frontend = {}) {
    UiFeedback result; result.field = field;
    if (ram.size() != 0x800000) return result;
    auto word = [&](unsigned a) { return (std::uint32_t(ram[a]) << 24) | (std::uint32_t(ram[a+1]) << 16)
        | (std::uint32_t(ram[a+2]) << 8) | ram[a+3]; };
    auto half = [&](unsigned a) { return std::int16_t((unsigned(ram[a]) << 8) | ram[a+1]); };
    auto pointer = [](std::uint32_t a) { return a >= 0x80000000 && a <= 0x807fffb8 && !(a & 3); };
    auto mode = half(runtime::guest::physical(runtime::guest::game_mode));
    result.guest_mode = mode;
    // Mode 12 covers the mission location card; the text briefing uses mode 4.
    // Its input-loop observer distinguishes it from pre-briefing loading frames.
    if ((mode == 12 || (mode == 4 && frontend.briefing))
        && word(0xd1170) == 7 && word(0x127db4) == 0x27bdf4b0) {
        result.mode = "briefing"; result.token = 263; return result;
    }
    if (mode == 4) {
        auto menu = word(0xd1170);
        result.token = menu + 0x100;
        result.mode = menu == 25 ? "intro" : menu == 7 ? "loading" : "menu";
        const auto score_root=word(0x112800);
        if (menu>=13 && menu<=18 && pointer(score_root)
            && word(0xa4ab0)==score_root && word((score_root&0x7fffff)+0x2c)==0x8012c90c
            && word(0x12c90c)==0x27bdffe0) {
            result.mode="results"; result.token=score_root;
            result.victory=ram[0x11281c]==1;
            const auto selected=word(0x112804);
            if (selected>3) return result;
            // Exact NTSC-U 640x480 strip painted by 8012B4A4 at (218,30).
            // The four 384x45 sprites contain shared artwork and quantization
            // differences, so pixel differences are not individual hit boxes.
            // Use the tab edges from this guarded original layout instead.
            if (word(0x12b4a4)!=0x27bdffc0) return result;
            for (unsigned id=0; id<4; ++id) {
                const unsigned sprite=0x112860+id*8;
                const auto pixels=word(sprite+4);
                if (half(sprite)!=384 || half(sprite+2)!=45 || pixels<0x80000000
                    || pixels>0x80800000-384*45) return result;
            }
            // The active tab expands; tabs to its left use the collapsed
            // edges. Preserve the ID while subsequent navigation moves them.
            constexpr std::array<std::array<int,5>,4> layouts{{
                {232,373,445,521,592}, {232,284,445,521,592},
                {232,284,361,521,592}, {232,284,361,443,592}}};
            const auto& edges=layouts[selected];
            for (unsigned id=0; id<4; ++id) {
                runtime::UiItem item;
                item.id=id; item.group=6; item.selected=id==selected;
                item.horizontal=true; item.activate=false;
                item.x0=edges[id]; item.y0=44;
                item.x1=edges[id+1]-1; item.y1=72;
                result.items.push_back(item);
            }
            return result;
        }
        result.items = frontend.items;
        std::erase_if(result.items,[&](const auto& item) { return item.group==5 || (item.group==7 && menu!=8); });
        if (menu==0 && frontend.main && frontend.main->campaign<=5) {
            result.main=frontend.main;
            auto valid=[](const runtime::UiBounds& b) {
                return b.x0>=0 && b.y0>=0 && b.x1<=640 && b.y1<=480 && b.x1>b.x0 && b.y1>b.y0;
            };
            if (frontend.main->warning!=-1) {
                result.mode=frontend.main->warning==0x217 ? "campaign_warning" : "menu_warning";
                result.token|=std::uint64_t(unsigned(frontend.main->warning)&0xffff)<<16;
                result.token|=std::uint64_t(frontend.main->campaign)<<32;
                result.items.clear();
                if (frontend.main->warning_bounds && valid(*frontend.main->warning_bounds))
                    result.bounds=frontend.main->warning_bounds;
                return result;
            }
            for (const auto& item:frontend.items) if (item.group==5
                && item.context==frontend.main->campaign && item.id<=5
                && item.selected==(item.id==frontend.main->campaign)) {
                const runtime::UiBounds b{item.x0,item.y0,item.x1,item.y1,item.context};
                if (!valid(b)) continue;
                result.items.push_back(item);
                if (!result.carousel_bounds) result.carousel_bounds=b;
                else {
                    auto& all=*result.carousel_bounds;
                    all.x0=std::min(all.x0,b.x0); all.y0=std::min(all.y0,b.y0);
                    all.x1=std::max(all.x1,b.x1); all.y1=std::max(all.y1,b.y1);
                }
            }
        }
        return result;
    }
    if (mode != 3) return result;
    result.mode = "game"; result.token = 3;
    // Mission canvas width (0x800A5AF0): 320, or wider with the widescreen option.
    const int image_width = std::clamp<int>(half(0xa5af0), 320, 512);
    const bool locked = half(0xf0c20) != 0;
    const bool build = !locked && ram[0xc6a64] != 0;
    if (!locked && !build) {
        if (ram[runtime::guest::physical(runtime::guest::split_screen)]) return result; // split-screen needs its own player/layout proof
        const auto context=runtime::hud_context_hash([&](std::uint32_t physical) { return ram[physical]; });
        result.context=context;
        result.pick_required=true;
        if (frontend.pick && frontend.pick->context==context
            && frontend.pick->x==half(0xafe18)*.5f && frontend.pick->y==half(0xafe1a)*.5f
            && frontend.pick->camera_x==std::int32_t(word(0xa2ee8))
            && frontend.pick->camera_y==std::int32_t(word(0xa2eec))) result.pick=frontend.pick;
        if (frontend.hud_bounds && frontend.hud_bounds->context==context) {
            const auto& b=*frontend.hud_bounds;
            if (b.x0>=0 && b.y0>=0 && b.x1<=image_width && b.y1<=240 && b.x1>b.x0 && b.y1>b.y0)
                result.bounds=b;
        }
        for (const auto& item : frontend.items)
            if (item.group==4 && item.context==context && item.id<=4
                && item.buttons==(item.id==4 ? 0x4000u : std::array<unsigned,4>{8,2,1,4}[item.id])
                && item.x0>=0 && item.y0>=0 && item.x1<=image_width && item.y1<=240
                && item.x1>item.x0 && item.y1>item.y0) result.items.push_back(item);
        return result;
    }
    result.mode = build ? "build" : "modal";
    const auto root = word(build ? 0xc6a68 : 0xa4ab0);
    if (!pointer(root)) return result;
    const auto parent = root & 0x7fffff;
    result.token = root;
    auto child = word(parent + 0x44), selected = word(parent + 0x40);
    const float x = half(parent+4), y = half(parent+6);
    const float x1 = half(parent+8), y1 = half(parent+10);
    if (build && x>=0 && y>=0 && x1<=image_width && y1<=240 && x1>x && y1>y)
        result.bounds=runtime::UiBounds{x,y,x1,y1,0};
    std::array<std::uint32_t, 64> visited{};
    unsigned count = 0;
    while (child) {
        if (!pointer(child) || count == visited.size()) { result.items.clear(); return result; }
        for (unsigned i=0; i<count; ++i) if (visited[i] == child) { result.items.clear(); return result; }
        visited[count++] = child;
        const auto p = child & 0x7fffff;
        // Ordinary buttons use the 8001843C predicate. The build grid instead
        // navigates by row/column and draws its highlight even on disabled
        // cells (80038334..80038384, 80072518..8007254C). Keep visible cells
        // so a disabled intermediate selection cannot lose acknowledgement.
        const auto flags = word(p+0x18);
        if (word(p+0x30) == (build ? 0x800723bcu : 0x8001ecb4u)
            && (build ? (flags & 8) != 0 : (flags & 0x1a) == 0x18)) {
            runtime::UiItem item;
            item.id = count-1; item.group = 1; item.selected = child == selected;
            if (build) {
                const auto button = half(p+0x20);
                if (button < 1 || button > 9) { result.items.clear(); return result; }
                item.id = button-1; item.columns = 3;
                item.selected = item.id == word(0xafe9c); // value used by the cell draw callback
                item.activate = (flags & 2) == 0;
            }
            item.x0 = x + half(p+4); item.y0 = y + half(p+6);
            item.x1 = x + half(p+8); item.y1 = y + half(p+10);
            if (item.x1 > item.x0 && item.y1 > item.y0) result.items.push_back(item);
        }
        child = word(p);
    }
    // Only the observed single-button advisor can accept a tap anywhere.
    // Pause/confirmation menus must select a specific button first.
    if (!build && word(parent+0x2c) == 0x8003faac && result.items.size() == 1) {
        result.mode = "message";
        result.message_id = word(0xb03d8) & 0xffff;
        result.token |= std::uint64_t(result.message_id) << 32;
    }
    return result;
}
}
