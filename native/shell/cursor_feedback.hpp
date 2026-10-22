#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include "../runtime/minimap.hpp"
#include "../runtime/guest_addresses.hpp"

namespace sc64::desktop {
struct CursorFeedback {
    bool valid = false;
    float x = 0, y = 0;
    float divisor = 0;
    std::uint64_t field = 0;
    bool camera_valid = false;
    float camera_x = 0, camera_y = 0;
    float camera_min_x = 0, camera_max_x = 0, camera_min_y = 0, camera_max_y = 0;
    float camera_divisor = 0;
    std::optional<runtime::MinimapProfile> minimap;
};
// NTSC-U profile only. Read the owned canonical task snapshot, never live guest
// RAM. Addresses and the 640x480 -> 320x240 transform are recorded in research.md.
inline CursorFeedback read_cursor(std::span<const std::uint8_t> ram, std::uint64_t field) {
    CursorFeedback result;
    result.field = field;
    result.minimap=runtime::read_minimap(ram);
    if (ram.size() < 0xafdfc) return result;
    auto word = [&](unsigned a) {
        return (std::uint32_t(ram[a]) << 24) | (std::uint32_t(ram[a+1]) << 16)
            | (std::uint32_t(ram[a+2]) << 8) | ram[a+3];
    };
    auto half = [&](unsigned a) { return std::int16_t((unsigned(ram[a]) << 8) | ram[a+1]); };
    float x = std::bit_cast<float>(word(0xafdbc)), y = std::bit_cast<float>(word(0xafdc4));
    // Up to 1023: the widescreen mission image is up to 512 wide (widescreen.hpp).
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1023 || y < 0 || y > 479
        || std::abs(x - half(0xafdcc)) > 1 || std::abs(y - half(0xafdce)) > 1) return result;
    result.valid = true;
    result.x = x * .5f; result.y = y * .5f;
    const float divisor=std::bit_cast<float>(word(0xafdf8));
    if (std::isfinite(divisor) && divisor>0) result.divisor=divisor;
    if (ram.size() >= 0xd13dc && half(runtime::guest::physical(runtime::guest::game_mode)) == 3 && ram[runtime::guest::physical(runtime::guest::split_screen)] == 0) {
        // See research.md: original camera helpers clamp these signed pixel
        // offsets. Expose half-size image units from the owned render snapshot.
        auto signed_word = [&](unsigned a) { return std::bit_cast<std::int32_t>(word(a)); };
        const auto xmin=signed_word(0xa2eb4), xmax=signed_word(0xa2ebc);
        const auto ymin=signed_word(0xa2eb8), ymax=signed_word(0xa2ec0);
        const auto cx=signed_word(0xa2ee8), cy=signed_word(0xa2eec);
        const float setting=std::bit_cast<float>(word(0xd13d8));
        const float divisor=6.f-(setting+120.f)/80.f;
        if (xmin>=-32768 && ymin>=-32768 && xmax<=32768 && ymax<=32768
            && xmax>xmin && ymax>ymin && cx>=xmin && cx<=xmax && cy>=ymin && cy<=ymax
            && std::isfinite(divisor) && divisor>=1 && divisor<=6) {
            result.camera_valid=true;
            result.camera_x=cx*.5f; result.camera_y=cy*.5f;
            result.camera_min_x=xmin*.5f; result.camera_max_x=xmax*.5f;
            result.camera_min_y=ymin*.5f; result.camera_max_y=ymax*.5f;
            result.camera_divisor=std::trunc(divisor);
        }
    }
    return result;
}
}
