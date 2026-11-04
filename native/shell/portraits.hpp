#pragma once
// Animated unit portraits from the user's own StarCraft PC graphics
// (tools/hd/extract_graphics.py, <hd>/portrait). Briefings and transmissions show a
// still 64x56 N64 portrait; it is matched to the PC portrait it was made from, and
// the PC portrait's videos play in its place: talking while a voice plays,
// fidgeting now and then otherwise. Without the files nothing changes.
#include "gfx.hpp"
#include <string>

namespace sc64::portraits {
// <hd>/portrait/index.json and the .smk files beside it, decoded in the background.
void initialize(const std::string& hd_root);
// Gfx thread, before rasterizing a task: portrait textures replaced in place.
void apply(gfx::DrawList& list);
}
