#pragma once
// HD missions from the user's own StarCraft PC graphics (tools/hd/extract_graphics.py).
// The 1P mission view is a CPU-drawn 8-bit canvas: PC terrain and each image's
// PC frame are drawn again into a canvas of twice the size, in the game's own
// palette and through the game's own render functions (shadows, remaps,
// cloaking), by hooks in the recompiled code (tools/recompile/apply_hd_hooks.py).
// Presentation replaces each canvas pixel of the finished frame with its 2x2
// HD block, keeping the RDP's darkening (fog) and everything drawn over it.
#include "gfx.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sc64::hd {
// One finished HD canvas: palette indices at twice the N64 canvas size.
struct Frame {
    std::uint32_t canvas = 0;            // physical address of the N64 canvas it doubles
    unsigned canvas_width = 0, canvas_height = 0;
    std::uint64_t canvas_hash = 0;       // the N64 canvas bytes when it was taken
    std::vector<std::uint8_t> pixels;    // (2 * canvas_width) x (2 * canvas_height)
};
using Frames = std::vector<std::shared_ptr<const Frame>>;

// <root> holds arr/, tileset/ and unit/. Missing files or <root>/../nohd keep the N64 graphics.
void initialize(const std::string& root);
bool enabled();
// Guest thread, at each graphics task submission: canvases finished since the last call.
Frames take(const std::uint8_t* rdram);
// Main thread: the 2x image of a presented target when it shows an HD canvas.
// rgba is the target as the RDP left it (width x height); nullopt otherwise.
std::optional<std::vector<std::uint8_t>> compose(const gfx::DrawList& list, const Frames& frames,
                                                 const std::vector<std::uint8_t>& rgba, const gfx::Image& target,
                                                 unsigned height);
}
