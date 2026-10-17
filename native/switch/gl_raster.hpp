#pragma once
// GPU rasterizer for gfx::DrawList (OpenGL 4.3, Mesa on the Switch).
// It reproduces native/gfx_core/software.cpp with the same integer rules:
// texture addressing is resolved on the CPU into per-axis lookup tables, and a
// fragment shader does sampling, combine, alpha tests, blend, dither and the
// RGBA16 store with image load/store, one rectangle at a time in list order.
// Anything the shader does not model makes render() return false, and the
// caller uses render_software (which also reports the real errors).
#include "gfx.hpp"
#include <memory>

namespace sc64::nx {
class GlRaster {
public:
    GlRaster();
    ~GlRaster();
    bool init();      // GL thread, context current
    void shutdown();
    bool render(const gfx::DrawList& list, gfx::SoftwareResult& out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
