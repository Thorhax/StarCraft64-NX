#include "gl_raster.hpp"
#include "log.hpp"
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace sc64::nx {
namespace {
using gfx::Color;
using gfx::Rectangle;
using gfx::Texture;

// ---- Integer helpers: identical to native/gfx_core/software.cpp (keep in sync).
int floor_div(std::int64_t n, int d) { return n >= 0 ? n / d : -int((-n + d - 1) / d); }
int address(int value, unsigned mask, unsigned mode, int maximum, bool copy) {
    if (!copy && ((mode & 2) || mask == 0)) value = std::clamp(value, 0, maximum * 32);
    int coord = floor_div(value, 32);
    if (mask) {
        unsigned bit = std::min(mask, 10u);
        if ((mode & 1) && ((coord >> bit) & 1)) coord = ~coord;
        coord &= ((1u << mask) - 1) & 1023;
    }
    return coord;
}
int shifted(int v, unsigned shift) {
    v = std::int16_t(v);
    if (shift <= 10) return floor_div(v, 1 << shift);
    return std::int16_t(v * (1 << (16 - shift)));
}
constexpr std::uint64_t no_sample_combine = 0xfcfffffffffdf6fbULL;

const char* vertex_source = R"(#version 430 core
uniform vec4 u_quad; // x0, y0, x1, y1 in NDC
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(u_quad.xy, u_quad.zw, corner), 0.0, 1.0);
}
)";

// Mirrors raster_band(): sample -> combine -> alpha tests -> blend -> dither -> store.
const char* fragment_source = R"(#version 430 core
// Target rows from u_origin.z, row-major, one uint per pixel: raw RGBA16 in
// bits 0-15, bit 16 set where a store happened. A linear buffer, so upload and
// readback are plain copies (texture readback detiled through uncached memory).
layout(std430, binding = 0) buffer Framebuffer { uint fb[]; };
uniform int u_fb_width;
layout(binding = 0) uniform isampler2D u_lut;       // per-axis (coord, coord+32, frac, 0)
layout(binding = 1) uniform usampler2D u_grid;      // decoded texels
uniform ivec4 u_origin;      // bx0, by0, region first row, 0
uniform ivec4 u_lut_at;      // x-axis offset, y-axis offset, flip, 0
uniform ivec2 u_grid_origin;
uniform ivec4 u_mode;        // cycle, filter, no-sample, dither mode
uniform ivec4 u_alpha_mode;  // coverage test, compare, copy rule, compare threshold
uniform ivec2 u_rgb[12];     // combine RGB inputs [slot*3+lane]: (kind, constant)
uniform ivec2 u_alpha_in[4]; // combine alpha inputs
uniform ivec4 u_blend;       // enabled, p, a, m
uniform int u_blend_b;
uniform ivec4 u_blend_color;
uniform ivec4 u_fog_color;

const int magic[16] = int[16](0,6,1,7,4,2,5,3,3,5,2,4,7,1,6,0);
const int bayer[16] = int[16](0,4,1,5,4,0,5,1,3,7,2,6,7,3,6,2);

ivec4 lut(int e) { return texelFetch(u_lut, ivec2(e & 4095, e >> 12), 0); }
ivec4 fetch(int x, int y) { return ivec4(texelFetch(u_grid, ivec2(x - u_grid_origin.x, y - u_grid_origin.y), 0)); }
int in_rgb(int slot, int lane, ivec4 t) {
    ivec2 k = u_rgb[slot * 3 + lane];
    return k.x == 0 ? k.y : (k.x == 1 ? t[lane] : t.a);
}
int in_alpha(int slot, ivec4 t) { ivec2 k = u_alpha_in[slot]; return k.x == 0 ? k.y : t.a; }
int calc(int a, int b, int c, int d) { return clamp(((a - b) * c + d * 256 + 128) >> 8, 0, 255); }
ivec4 rgba16(uint v) {
    ivec3 c = ivec3(int(v >> 11) & 31, int(v >> 6) & 31, int(v >> 1) & 31);
    c = (c << 3) | (c >> 2);
    return ivec4(c, (v & 1u) != 0u ? 255 : 0);
}
uint encode16(ivec4 c) {
    ivec3 k = clamp(c.rgb, 0, 255) >> 3;
    return uint((k.r << 11) | (k.g << 6) | (k.b << 1) | (c.a != 0 ? 1 : 0));
}
ivec4 pick(int code, ivec4 color, ivec4 memory) {
    return code == 0 ? color : (code == 1 ? memory : (code == 2 ? u_blend_color : u_fog_color));
}
ivec4 blend(ivec4 color, ivec4 memory) {
    if (u_blend.x == 0) return color;
    ivec4 first = pick(u_blend.y, color, memory), second = pick(u_blend.w, color, memory);
    int a = u_blend.z, b = u_blend_b;
    int weight = a == 0 ? color.a : (a == 1 ? u_fog_color.a : 0);
    int weight2 = b == 0 ? 255 - weight : (b == 1 ? memory.a : (b == 2 ? 255 : 0));
    if (weight == 255) return first;
    ivec3 v = clamp((first.rgb * (weight >> 3) + second.rgb * ((weight2 >> 3) + 1)) >> 5, 0, 255);
    return ivec4(v, color.a);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int x = p.x, y = p.y + u_origin.z;
    ivec4 lx = lut(u_lut_at.x + (x - u_origin.x));
    ivec4 ly = lut(u_lut_at.y + (y - u_origin.y));
    ivec4 ls = u_lut_at.z != 0 ? ly : lx;
    ivec4 lt = u_lut_at.z != 0 ? lx : ly;
    int cycle = u_mode.x;
    ivec4 c = ivec4(0);
    if (u_mode.z == 0) {
        c = fetch(ls.x, lt.x);
        if (u_mode.y != 0) { // RDP three-point interpolation, 5-bit fractions
            int fs = ls.z, ft = lt.z;
            ivec4 a, b, d; int wa, wb, wd;
            if (fs + ft < 32) { a = c; b = fetch(ls.y, lt.x); d = fetch(ls.x, lt.y); wa = 32 - fs - ft; wb = fs; wd = ft; }
            else { a = fetch(ls.y, lt.y); b = fetch(ls.x, lt.y); d = fetch(ls.y, lt.x); wa = fs + ft - 32; wb = 32 - fs; wd = 32 - ft; }
            c = (a * wa + b * wb + d * wd + 16) / 32;
        }
    }
    if (cycle == 0) {
        ivec4 t = c, r;
        for (int lane = 0; lane < 3; ++lane) {
            int cv = in_rgb(2, lane, t);
            r[lane] = cv != 0 ? calc(in_rgb(0, lane, t), in_rgb(1, lane, t), cv, in_rgb(3, lane, t))
                              : clamp(in_rgb(3, lane, t), 0, 255);
        }
        int av = in_alpha(2, t);
        r.a = av != 0 ? calc(in_alpha(0, t), in_alpha(1, t), av, in_alpha(3, t)) : clamp(in_alpha(3, t), 0, 255);
        c = r;
        if (u_alpha_mode.x != 0 && c.a < 32) discard;
    }
    if (u_alpha_mode.y != 0) {
        if (cycle == 2) {
            bool bit = u_alpha_mode.z == 0 ? c.a != 0 : (c.a & 1) != 0;
            if (!bit) discard;
        } else if (c.a < u_alpha_mode.w) discard;
    }
    int index = p.y * u_fb_width + p.x;
    uint raw = fb[index] & 0xffffu;
    if (cycle == 0) {
        ivec4 memory = rgba16(raw);
        memory.rgb &= 248; // framebuffer reads zero-extend the five RGB bits
        c = blend(c, memory);
        int mode = u_mode.w;
        if (mode != 3) {
            int i = (y & 3) * 4 + (x & 3);
            int v = mode == 0 ? magic[i] : bayer[i];
            for (int lane = 0; lane < 3; ++lane)
                if ((c[lane] & 7) > v) c[lane] = min(255, (c[lane] & 248) + 8);
        }
        c.a = 255;
    }
    fb[index] = encode16(c) | 0x10000u;
}
)";

GLuint compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char text[2048] = {};
        glGetShaderInfoLog(shader, sizeof(text), nullptr, text);
        sc64::shell::log::error("GL raster shader: %s", text);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };

// Combine inputs resolved per rectangle: (0, constant) / (1, texel lane) / (2, texel alpha).
struct Input { int kind = 0, value = 0; };
Input rgb_input(unsigned code, unsigned lane, unsigned kind, const gfx::State& s) {
    auto channel = [](Color c, unsigned i) { return i == 0 ? c.r : i == 1 ? c.g : i == 2 ? c.b : c.a; };
    if (code == 1 || code == 2) return {1, 0};
    if (code == 3) return {0, channel(s.primitive, lane)};
    if (code == 4) return {0, 0};
    if (code == 5) return {0, channel(s.environment, lane)};
    if (code == 0) throw Unsupported("combined input");
    switch (kind) {
    case 0: if (code == 6) return {0, 256}; if (code == 7) throw Unsupported("noise"); return {0, 0};
    case 1: if (code == 6 || code == 7) throw Unsupported("key"); return {0, 0};
    case 2:
        if (code == 8 || code == 9) return {2, 0};
        if (code == 10) return {0, s.primitive.a};
        if (code == 11) return {0, 0};
        if (code == 12) return {0, s.environment.a};
        if (code <= 15) throw Unsupported("multiplier");
        return {0, 0};
    default: return {0, code == 6 ? 256 : 0};
    }
}
Input alpha_input(unsigned code, bool mul, const gfx::State& s) {
    if (code == 1 || code == 2) return {2, 0};
    if (code == 3) return {0, s.primitive.a};
    if (code == 5) return {0, s.environment.a};
    if (code == 6 && !mul) return {0, 256};
    if (code == 0 || (code == 6 && mul)) throw Unsupported("alpha input");
    return {0, 0};
}
int copy_alpha_rule(const Texture& tex, const Rectangle& r) {
    const unsigned tlut = (r.state.other_hi >> 14) & 3;
    const bool palette = tlut == 2 || tlut == 3;
    const auto& t = tex.tile;
    if (t.format == 2 && palette) return tlut == 2 ? 0 : 1;
    if (!palette && t.size == 2 && t.format == 0) return 0;
    if (!palette && t.size == 2 && t.format == 3) return 1;
    throw Unsupported("COPY alpha compare format");
}

struct Axis { int coord = 0, next = 0, frac = 0; };
struct GridKey {
    std::uint64_t key; bool background; unsigned tlut; gfx::Tile tile; int x0, y0, x1, y1;
    auto tie() const { return std::tie(key, background, tlut, tile.format, tile.size, tile.line, tile.tmem,
                                       tile.palette, x0, y0, x1, y1); }
    bool operator<(const GridKey& o) const { return tie() < o.tie(); }
};
struct Grid { GLuint texture = 0; int x0 = 0, y0 = 0; std::size_t bytes = 0; };
struct TargetRegion {
    gfx::Image image; int y0 = 0, y1 = 0;
    std::size_t offset = 0; // first pixel in the shared framebuffer buffer
};
struct Plan {
    unsigned target = 0;
    gfx::PixelBounds bounds;
    int lut_x = 0, lut_y = 0;
    const Grid* grid = nullptr;
    int cycle = 0, filter = 0, no_sample = 0, dither = 3;
    int coverage = 0, compare = 0, copy_rule = 0, threshold = 0;
    Input rgb[12], alpha[4];
    int blend_on = 0, p = 0, a = 0, m = 0, b = 0;
    int flip = 0;
    Color blend_color, fog_color;
};
}

struct GlRaster::Impl {
    GLuint program = 0, vao = 0, fbo = 0, lut = 0;
    // Time profile, logged every 120 frames: CPU planning (incl. new texel grids),
    // uploads, draw submission, readback (waits for the GPU) and RDRAM commit.
    struct Profile { double plan = 0, upload = 0, draw = 0, gpu = 0, readback = 0, commit = 0;
                     std::uint64_t frames = 0, rects = 0, barriers = 0; } profile;
    GLint u_quad = -1, u_origin = -1, u_lut_at = -1, u_grid_origin = -1, u_mode = -1, u_alpha_mode = -1,
          u_rgb = -1, u_alpha_in = -1, u_blend = -1, u_blend_b = -1, u_blend_color = -1, u_fog_color = -1,
          u_fb_width = -1;
    // fb_buffer: where the shader draws (GPU-friendly memory). read_buffer: CPU
    // readable staging copy (GL_STREAM_READ), filled by one GPU-side copy.
    GLuint fb_buffer = 0, read_buffer = 0;
    std::size_t fb_capacity = 0, ssbo_alignment = 256; // bytes, queried at init
    std::map<GridKey, Grid> grids;
    std::deque<GridKey> grid_order; // creation order, for eviction between frames
    std::size_t grid_bytes = 0;     // texel bytes held by the cache

    void clear_grids() {
        for (auto& [key, grid] : grids) glDeleteTextures(1, &grid.texture);
        grids.clear();
        grid_order.clear();
        grid_bytes = 0;
    }
    // Byte budget (a count limit let rectangle-sized grids exhaust memory).
    void trim_grids(std::size_t budget) {
        while (grid_bytes > budget && !grid_order.empty()) {
            auto it = grids.find(grid_order.front());
            grid_order.pop_front();
            if (it != grids.end()) {
                grid_bytes -= it->second.bytes;
                glDeleteTextures(1, &it->second.texture);
                grids.erase(it);
            }
        }
    }
    const Grid& grid_for(const Texture& tex, int x0, int y0, int x1, int y1) {
        GridKey key{tex.key, tex.background, tex.tlut_mode, tex.tile, x0, y0, x1, y1};
        if (auto it = grids.find(key); it != grids.end()) return it->second;
        const int w = x1 - x0 + 1, h = y1 - y0 + 1;
        if (w <= 0 || h <= 0 || std::size_t(w) * h * 4 > 16u * 1024 * 1024) throw Unsupported("texel grid size");
        std::vector<std::uint8_t> texels(std::size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const Color c = tex.background ? tex.pixels.at(std::size_t(y0 + y) * tex.width + (x0 + x))
                                               : tex.memory.fetch(tex.tile, x0 + x, y0 + y, tex.tlut_mode);
                auto* out = &texels[(std::size_t(y) * w + x) * 4];
                out[0] = std::uint8_t(std::clamp(c.r, 0, 255)); out[1] = std::uint8_t(std::clamp(c.g, 0, 255));
                out[2] = std::uint8_t(std::clamp(c.b, 0, 255)); out[3] = std::uint8_t(std::clamp(c.a, 0, 255));
                if (c.r < 0 || c.r > 255 || c.g < 0 || c.g > 255 || c.b < 0 || c.b > 255 || c.a < 0 || c.a > 255)
                    throw Unsupported("texel outside 8-bit range");
            }
        Grid grid{0, x0, y0, std::size_t(w) * h * 4};
        glGenTextures(1, &grid.texture);
        glBindTexture(GL_TEXTURE_2D, grid.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, w, h, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, texels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        grid_order.push_back(key);
        grid_bytes += grid.bytes;
        return grids.emplace(key, grid).first->second;
    }
};

GlRaster::GlRaster() : impl_(std::make_unique<Impl>()) {}
GlRaster::~GlRaster() = default;

bool GlRaster::init() {
    auto& g = *impl_;
    const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) return false;
    g.program = glCreateProgram();
    glAttachShader(g.program, vertex);
    glAttachShader(g.program, fragment);
    glLinkProgram(g.program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = 0;
    glGetProgramiv(g.program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char text[2048] = {};
        glGetProgramInfoLog(g.program, sizeof(text), nullptr, text);
        sc64::shell::log::error("GL raster link: %s", text);
        return false;
    }
    auto loc = [&](const char* name) { return glGetUniformLocation(g.program, name); };
    g.u_quad = loc("u_quad"); g.u_origin = loc("u_origin"); g.u_lut_at = loc("u_lut_at");
    g.u_grid_origin = loc("u_grid_origin"); g.u_mode = loc("u_mode"); g.u_alpha_mode = loc("u_alpha_mode");
    g.u_rgb = loc("u_rgb"); g.u_alpha_in = loc("u_alpha_in"); g.u_blend = loc("u_blend");
    g.u_blend_b = loc("u_blend_b"); g.u_blend_color = loc("u_blend_color"); g.u_fog_color = loc("u_fog_color");
    g.u_fb_width = loc("u_fb_width");
    glGenBuffers(1, &g.fb_buffer);
    glGenBuffers(1, &g.read_buffer);
    GLint alignment = 0;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment);
    if (alignment > 0) g.ssbo_alignment = std::size_t(alignment);
    glGenVertexArrays(1, &g.vao);
    glGenFramebuffers(1, &g.fbo);
    glGenTextures(1, &g.lut);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        sc64::shell::log::error("GL raster init error 0x%x", unsigned(error));
        return false;
    }
    sc64::shell::log::info("GL raster: ready");
    return true;
}

void GlRaster::shutdown() {
    auto& g = *impl_;
    g.clear_grids();
    if (g.lut) glDeleteTextures(1, &g.lut);
    if (g.fb_buffer) glDeleteBuffers(1, &g.fb_buffer);
    if (g.read_buffer) glDeleteBuffers(1, &g.read_buffer);
    g.fb_buffer = g.read_buffer = 0;
    if (g.fbo) glDeleteFramebuffers(1, &g.fbo);
    if (g.vao) glDeleteVertexArrays(1, &g.vao);
    if (g.program) glDeleteProgram(g.program);
    g.lut = g.fbo = g.vao = g.program = 0;
}

bool GlRaster::render(const gfx::DrawList& list, gfx::SoftwareResult& out) {
    auto& g = *impl_;
    std::vector<TargetRegion> regions;
    std::vector<Plan> plans;
    std::vector<GLint> lut_data; // RGBA32I entries
    // Evict only between frames (plans point into the map): oldest first,
    // down to 48 MiB of texels.
    g.trim_grids(48u * 1024 * 1024);
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto t_start = clock::now();
    try {
        // ---- Plan every rectangle on the CPU (exact addressing), or bail out.
        for (const auto& r : list.rectangles) {
            if (r.texture >= list.textures.size()) throw Unsupported("texture reference");
            const auto& tex = list.textures[r.texture];
            const int cycle = int(r.state.other_hi >> 20 & 3);
            if (cycle != 0 && cycle != 2) throw Unsupported("cycle type");
            if (r.state.other_lo & 0x30) throw Unsupported("depth");
            if (!tex.background && (tex.tile.sh < tex.tile.sl || tex.tile.th < tex.tile.tl)) throw Unsupported("tile limits");
            const auto b = gfx::raster_bounds(r);
            if (b.y1 > 1024 || b.y0 > 1024) throw Unsupported("raster height");
            if (b.empty()) continue;
            if (r.target.address % 2) throw Unsupported("odd target");
            Plan plan;
            plan.bounds = b;
            plan.cycle = cycle;
            plan.no_sample = r.state.combine == no_sample_combine && cycle == 0;
            plan.filter = cycle == 0 && (r.state.other_hi & (1u << 13)) ? 1 : 0;
            plan.flip = r.flip ? 1 : 0;
            plan.blend_color = r.state.blend;
            plan.fog_color = r.state.fog;
            const bool copy = cycle == 2;
            // Target region: rows per target image, grown to cover every rectangle.
            unsigned target = 0;
            for (; target < regions.size(); ++target) if (regions[target].image == r.target) break;
            if (target == regions.size()) regions.push_back({r.target, b.y0, b.y1});
            regions[target].y0 = std::min(regions[target].y0, b.y0);
            regions[target].y1 = std::max(regions[target].y1, b.y1);
            plan.target = target;
            // Per-axis lookup tables. The x axis drives S unless the rectangle is flipped.
            auto build_axis = [&](int count, int first, bool x_axis, int& min_c, int& max_c) {
                const bool s_axis = x_axis != r.flip;
                const int start = lut_data.size() / 4;
                for (int i = 0; i < count; ++i) {
                    const int d = x_axis ? first + i - r.x0 / 4 : first + i - r.y0 / 4;
                    int v = s_axis ? r.s + floor_div(std::int64_t(d) * r.dsdx, 32)
                                   : r.t + floor_div(std::int64_t(d) * r.dtdy, 32);
                    Axis axis;
                    auto coord = [&](int vv) {
                        if (tex.background) {
                            const int size = int(s_axis ? tex.width : tex.height);
                            int c = floor_div(vv, 32);
                            return (c % size + size) % size;
                        }
                        const auto& t = tex.tile;
                        return s_axis ? address(vv, t.masks, t.cms, (t.sh - t.sl) / 4, copy)
                                      : address(vv, t.maskt, t.cmt, (t.th - t.tl) / 4, copy);
                    };
                    if (!tex.background) v = shifted(v, s_axis ? tex.tile.shifts : tex.tile.shiftt)
                                             - (s_axis ? tex.tile.sl : tex.tile.tl) * 8;
                    if (!plan.no_sample) {
                        axis.coord = coord(v);
                        min_c = std::min(min_c, axis.coord); max_c = std::max(max_c, axis.coord);
                        if (plan.filter) {
                            axis.next = coord(v + 32);
                            min_c = std::min(min_c, axis.next); max_c = std::max(max_c, axis.next);
                        }
                        axis.frac = v - floor_div(v, 32) * 32;
                    }
                    lut_data.insert(lut_data.end(), {axis.coord, axis.next, axis.frac, 0});
                }
                return start;
            };
            int sx0, sx1, ty0, ty1;
            // x-axis coords feed S (or T when flipped); collect each texture axis' range.
            int ax0 = INT32_MAX, ax1 = INT32_MIN, ay0 = INT32_MAX, ay1 = INT32_MIN;
            plan.lut_x = build_axis(b.x1 - b.x0, b.x0, true, ax0, ax1);
            plan.lut_y = build_axis(b.y1 - b.y0, b.y0, false, ay0, ay1);
            if (r.flip) { sx0 = ay0; sx1 = ay1; ty0 = ax0; ty1 = ax1; }
            else { sx0 = ax0; sx1 = ax1; ty0 = ay0; ty1 = ay1; }
            // Decode a domain that depends on the texture only, so one grid
            // serves every rectangle and frame (a scrolling map otherwise made
            // a new rectangle-sized grid per frame until memory ran out):
            // masked axis -> the whole mask period; clamped axis -> the whole
            // tile; background -> the whole image. Only an unmasked COPY axis
            // (no clamp) keeps the coordinates this rectangle reaches.
            auto domain = [&](bool s_axis, int& lo, int& hi) {
                int d0 = lo, d1 = hi;
                if (tex.background) { d0 = 0; d1 = int(s_axis ? tex.width : tex.height) - 1; }
                else {
                    const auto& tile = tex.tile;
                    const unsigned mask = s_axis ? tile.masks : tile.maskt;
                    if (mask) { d0 = 0; d1 = int(std::min((1u << mask) - 1, 1023u)); }
                    else if (!copy) { d0 = 0; d1 = (s_axis ? tile.sh - tile.sl : tile.th - tile.tl) / 4; }
                }
                if (lo < d0 || hi > d1) return; // unexpected: keep the exact reachable range
                lo = d0; hi = d1;
            };
            if (!plan.no_sample) {
                domain(true, sx0, sx1);
                domain(false, ty0, ty1);
                plan.grid = &g.grid_for(tex, sx0, ty0, sx1, ty1);
            }
            // Combine, alpha tests, blend and dither state.
            const auto& s = r.state;
            if (cycle == 0) {
                const unsigned hi = s.combine >> 32, lo = unsigned(s.combine);
                const unsigned ca = hi >> 5 & 15, cb = lo >> 24 & 15, cc = hi & 31, cd = lo >> 6 & 7;
                const unsigned aa = lo >> 21 & 7, ab = lo >> 3 & 7, ac = lo >> 18 & 7, ad = lo & 7;
                for (unsigned lane = 0; lane < 3; ++lane) {
                    plan.rgb[0 * 3 + lane] = rgb_input(ca, lane, 0, s);
                    plan.rgb[1 * 3 + lane] = rgb_input(cb, lane, 1, s);
                    plan.rgb[2 * 3 + lane] = rgb_input(cc, lane, 2, s);
                    plan.rgb[3 * 3 + lane] = rgb_input(cd, lane, 3, s);
                }
                plan.alpha[0] = alpha_input(aa, false, s);
                plan.alpha[1] = alpha_input(ab, false, s);
                plan.alpha[2] = alpha_input(ac, true, s);
                plan.alpha[3] = alpha_input(ad, false, s);
                plan.coverage = (s.other_lo & (1u << 12)) ? 1 : 0;
                plan.dither = int(s.other_hi >> 6 & 3);
                if (plan.dither == 2) throw Unsupported("noise dither");
                const unsigned l = s.other_lo;
                plan.blend_on = (l & (1u << 14)) ? 1 : 0;
                plan.p = int(l >> 30 & 3); plan.a = int(l >> 26 & 3); plan.m = int(l >> 22 & 3); plan.b = int(l >> 18 & 3);
            }
            if (s.other_lo & 1) {
                plan.compare = 1;
                if (copy) plan.copy_rule = copy_alpha_rule(tex, r);
                else {
                    if (s.other_lo & 2) throw Unsupported("dithered alpha compare");
                    plan.threshold = s.blend.a;
                }
            }
            plans.push_back(plan);
        }
        // Targets: inside RDRAM, and no two targets sharing bytes.
        for (auto& region : regions) {
            const std::uint64_t first = region.image.address + std::uint64_t(region.y0) * region.image.width * 2;
            const std::uint64_t end = region.image.address + std::uint64_t(region.y1) * region.image.width * 2;
            if (end > list.initial_ram.size()) throw Unsupported("target outside RDRAM");
            for (const auto& other : regions) {
                if (&other == &region) continue;
                const std::uint64_t o0 = other.image.address + std::uint64_t(other.y0) * other.image.width * 2;
                const std::uint64_t o1 = other.image.address + std::uint64_t(other.y1) * other.image.width * 2;
                if (first < o1 && o0 < end) throw Unsupported("overlapping targets");
            }
        }
    } catch (const Unsupported&) {
        return false;
    } catch (const std::exception&) {
        return false; // e.g. a TMEM fetch error: the software path reports it
    }

    const auto t_planned = clock::now();
    // ---- Upload targets (one linear buffer, regions back to back) and lookup tables.
    std::size_t total = 0;
    const std::size_t align_pixels = std::max<std::size_t>(1, g.ssbo_alignment / 4);
    for (auto& region : regions) {
        total = (total + align_pixels - 1) / align_pixels * align_pixels; // SSBO range offset alignment
        region.offset = total;
        total += std::size_t(region.image.width) * (region.y1 - region.y0);
    }
    std::vector<std::uint32_t> pixels(std::max<std::size_t>(total, 1));
    for (const auto& region : regions) {
        const std::size_t count = std::size_t(region.image.width) * (region.y1 - region.y0);
        const std::uint8_t* src = list.initial_ram.data() + region.image.address
                                  + std::size_t(region.y0) * region.image.width * 2;
        for (std::size_t i = 0; i < count; ++i)
            pixels[region.offset + i] = std::uint32_t((src[i * 2] << 8) | src[i * 2 + 1]);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.fb_buffer);
    const std::size_t bytes = pixels.size() * sizeof(std::uint32_t);
    if (bytes > g.fb_capacity) {
        // Drawn by the GPU: GL_DYNAMIC_COPY keeps it in GPU-friendly memory
        // (GL_DYNAMIC_READ made every shader access slow: gpu 6-17 ms a frame).
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(bytes), nullptr, GL_DYNAMIC_COPY);
        glBindBuffer(GL_COPY_WRITE_BUFFER, g.read_buffer);
        glBufferData(GL_COPY_WRITE_BUFFER, GLsizeiptr(bytes), nullptr, GL_STREAM_READ);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.fb_buffer);
        g.fb_capacity = bytes;
    }
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(bytes), pixels.data());
    const int entries = int(lut_data.size() / 4);
    const int lut_rows = std::max(1, (entries + 4095) / 4096);
    lut_data.resize(std::size_t(lut_rows) * 4096 * 4, 0);
    glBindTexture(GL_TEXTURE_2D, g.lut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, 4096, lut_rows, 0, GL_RGBA_INTEGER, GL_INT, lut_data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    const auto t_uploaded = clock::now();
    // ---- Draw: one quad per rectangle, in list order. A later rectangle that
    // overlaps an earlier store must see it (blend reads memory), so an image
    // barrier is issued only when its bounds intersect a rectangle drawn since
    // the previous barrier. Disjoint rectangles (map tiles) share one batch;
    // distinct targets never share bytes (checked above).
    glUseProgram(g.program);
    glBindVertexArray(g.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.lut);
    int bound_target = -1;
    std::vector<gfx::PixelBounds> unbarriered;
    std::uint64_t barriers = 0;
    for (const auto& plan : plans) {
        const auto& region = regions[plan.target];
        const int w = int(region.image.width), h = region.y1 - region.y0;
        if (int(plan.target) != bound_target) {
            glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, w);
            glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT, h);
            glViewport(0, 0, w, h);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, g.fb_buffer, GLintptr(region.offset * 4),
                              GLsizeiptr(std::size_t(w) * h * 4));
            glUniform1i(g.u_fb_width, w);
            bound_target = int(plan.target);
            unbarriered.clear();
        }
        const auto& b = plan.bounds;
        for (const auto& o : unbarriered)
            if (b.x0 < o.x1 && o.x0 < b.x1 && b.y0 < o.y1 && o.y0 < b.y1) {
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                unbarriered.clear();
                ++barriers;
                break;
            }
        unbarriered.push_back(b);
        glUniform4f(g.u_quad, float(b.x0) / w * 2 - 1, float(b.y0 - region.y0) / h * 2 - 1,
                    float(b.x1) / w * 2 - 1, float(b.y1 - region.y0) / h * 2 - 1);
        glUniform4i(g.u_origin, b.x0, b.y0, region.y0, 0);
        glUniform4i(g.u_lut_at, plan.lut_x, plan.lut_y, plan.flip, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, plan.grid ? plan.grid->texture : 0);
        glActiveTexture(GL_TEXTURE0);
        glUniform2i(g.u_grid_origin, plan.grid ? plan.grid->x0 : 0, plan.grid ? plan.grid->y0 : 0);
        glUniform4i(g.u_mode, plan.cycle, plan.filter, plan.no_sample, plan.dither);
        glUniform4i(g.u_alpha_mode, plan.coverage, plan.compare, plan.copy_rule, plan.threshold);
        GLint rgb[24], alpha[8];
        for (int i = 0; i < 12; ++i) { rgb[i * 2] = plan.rgb[i].kind; rgb[i * 2 + 1] = plan.rgb[i].value; }
        for (int i = 0; i < 4; ++i) { alpha[i * 2] = plan.alpha[i].kind; alpha[i * 2 + 1] = plan.alpha[i].value; }
        glUniform2iv(g.u_rgb, 12, rgb);
        glUniform2iv(g.u_alpha_in, 4, alpha);
        glUniform4i(g.u_blend, plan.blend_on, plan.p, plan.a, plan.m);
        glUniform1i(g.u_blend_b, plan.b);
        glUniform4i(g.u_blend_color, plan.blend_color.r, plan.blend_color.g, plan.blend_color.b, plan.blend_color.a);
        glUniform4i(g.u_fog_color, plan.fog_color.r, plan.fog_color.g, plan.fog_color.b, plan.fog_color.a);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    glBindBuffer(GL_COPY_READ_BUFFER, g.fb_buffer);
    glBindBuffer(GL_COPY_WRITE_BUFFER, g.read_buffer);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, GLsizeiptr(bytes));
    const auto t_drawn = clock::now();
    // GPU time measured on its own, so readback is the pure transfer.
    glFinish();
    const auto t_finished = clock::now();
    double readback_ms = 0;

    // ---- Read back and commit exactly like render_software: RAM image plus
    // the address ranges of actual stores.
    out = gfx::SoftwareResult{list.initial_ram, 0, {}};
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const auto t_read = clock::now();
    const auto* mapped = static_cast<const std::uint32_t*>(
        glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, GLsizeiptr(bytes), GL_MAP_READ_BIT));
    if (!mapped) {
        sc64::shell::log::warning("GL raster: framebuffer map failed, frame rendered in software");
        return false;
    }
    std::memcpy(pixels.data(), mapped, bytes); // one sequential copy out of GPU memory
    glUnmapBuffer(GL_COPY_WRITE_BUFFER);
    readback_ms = ms(t_read, clock::now());
    for (const auto& region : regions) {
        const std::size_t w = region.image.width;
        for (int row = 0; row < region.y1 - region.y0; ++row)
            for (std::size_t x = 0; x < w; ++x) {
                const std::uint32_t value = pixels[region.offset + std::size_t(row) * w + x];
                if (!(value & 0x10000u)) continue;
                const std::uint32_t a = std::uint32_t(region.image.address + (std::size_t(region.y0 + row) * w + x) * 2);
                out.ram[a] = std::uint8_t(value >> 8);
                out.ram[a + 1] = std::uint8_t(value);
                if (!out.writes.empty() && std::uint64_t(out.writes.back().address) + out.writes.back().length == a)
                    out.writes.back().length += 2;
                else out.writes.push_back({a, 2});
                ++out.written_pixels;
            }
    }
    const auto by_address = [](auto x, auto y) { return x.address < y.address; };
    std::sort(out.writes.begin(), out.writes.end(), by_address);
    std::vector<gfx::SoftwareResult::Write> merged;
    for (const auto& w : out.writes) {
        if (!merged.empty() && std::uint64_t(merged.back().address) + merged.back().length >= w.address) {
            const auto end = std::max<std::uint64_t>(std::uint64_t(merged.back().address) + merged.back().length,
                                                     std::uint64_t(w.address) + w.length);
            merged.back().length = std::uint32_t(end - merged.back().address);
        } else merged.push_back(w);
    }
    out.writes = std::move(merged);
    const auto t_end = clock::now();
    auto& prof = g.profile;
    prof.plan += ms(t_start, t_planned); prof.upload += ms(t_planned, t_uploaded);
    prof.draw += ms(t_uploaded, t_drawn); prof.gpu += ms(t_drawn, t_finished); prof.readback += readback_ms;
    prof.commit += ms(t_finished, t_end) - readback_ms;
    prof.rects += plans.size(); prof.barriers += barriers;
    if (++prof.frames == 120) {
        const double n = double(prof.frames);
        sc64::shell::log::info("GL raster ms/frame: plan %.1f upload %.1f draw %.1f gpu %.1f readback %.1f commit %.1f | "
                               "%.0f rects, %.0f barriers per frame, %zu cached grids",
                               prof.plan / n, prof.upload / n, prof.draw / n, prof.gpu / n, prof.readback / n,
                               prof.commit / n,
                               prof.rects / n, prof.barriers / n, g.grids.size());
        prof = {};
    }
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        sc64::shell::log::warning("GL raster: GL error 0x%x, frame rendered in software", unsigned(error));
        return false;
    }
    return true;
}
}
