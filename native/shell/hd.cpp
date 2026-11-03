#include "hd.hpp"
#include "log.hpp"
#include "platform.hpp"
#include <recomp.h>
#include <librecomp/addresses.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace sc64::hd {
namespace {
// StarCraft 64 USA, 1P missions (docs/research.md, "HD missions").
constexpr std::uint32_t canvas_descriptor = 0x800A62F4; // u16 width, u16 height, u32 pixels
constexpr std::uint32_t camera_x = 0x800A2EE8, camera_y = 0x800A2EEC; // s32, PC pixels
constexpr std::uint32_t view_x = 0x800A5B0A, view_y = 0x800A5B0C;    // u16 canvas offset of the view
constexpr std::uint32_t map_size = 0x800B1D44 + 0xE4;                // u16 tiles wide, high
constexpr std::uint32_t tile_array = 0x800A2ED4;                     // the game's own (N64) tile array
constexpr std::uint32_t render_canvas = 0x800AFD60;                  // canvas the render functions draw to
constexpr std::uint32_t hp_bar = 0x80029CC0;                         // render function of health bars (11, 18)
// Fog of war (and the dark beyond the map) blends the canvas toward this colour.
constexpr int fog_r = 16, fog_g = 0, fog_b = 16;
constexpr const char* tilesets[8] = {"badlands", "platform", "install", "ashworld", "jungle", "desert", "ice", "twilight"};

// ---- Guest memory (RDRAM keeps host-order 32-bit words; guest byte a is at a ^ 3).
std::uint32_t offset(std::uint32_t a) { return a - 0x80000000u; }
std::uint8_t gb(const std::uint8_t* rdram, std::uint32_t a) { return rdram[offset(a ^ 3)]; }
std::uint16_t gh(const std::uint8_t* rdram, std::uint32_t a) {
    std::uint16_t v; std::memcpy(&v, rdram + offset(a ^ 2), 2); return v;
}
std::uint32_t gw(const std::uint8_t* rdram, std::uint32_t a) {
    std::uint32_t v; std::memcpy(&v, rdram + offset(a), 4); return v;
}
void sh(std::uint8_t* rdram, std::uint32_t a, std::uint16_t v) { std::memcpy(rdram + offset(a ^ 2), &v, 2); }
void sw(std::uint8_t* rdram, std::uint32_t a, std::uint32_t v) { std::memcpy(rdram + offset(a), &v, 4); }
// Canonical bytes into guest memory at a 4-byte aligned address.
void put(std::uint8_t* rdram, std::uint32_t a, const std::uint8_t* src, std::size_t n) {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4)
        sw(rdram, a + std::uint32_t(i), std::uint32_t(src[i]) << 24 | std::uint32_t(src[i + 1]) << 16
                                        | std::uint32_t(src[i + 2]) << 8 | src[i + 3]);
    for (; i < n; ++i) rdram[offset((a + std::uint32_t(i)) ^ 3)] = src[i];
}
// Canvas identity: big-endian 32-bit words, as the canonical snapshot holds them.
std::uint64_t mix(std::uint64_t hash, std::uint32_t word) { return (hash ^ word) * 0x100000001b3ull + (hash >> 29); }

// ---- PC graphics
struct GrpFrame { std::uint8_t x, y, w, h; std::uint32_t offset, size; };
struct Grp { unsigned width = 0, height = 0; std::vector<GrpFrame> frames; std::vector<std::uint8_t> data; };
struct Tileset { std::vector<std::uint8_t> cv5, vr4; std::vector<std::uint16_t> vx4; };
std::string root;
std::atomic<bool> ready{false};
std::array<std::uint32_t, 999> image_grp{};
std::vector<std::shared_ptr<Grp>> grps; // by images.tbl index (1-based; [0] unused)
std::array<Tileset, 8> tileset_data;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), {}};
}
std::shared_ptr<Grp> parse_grp(std::vector<std::uint8_t> data) {
    if (data.size() < 6) return nullptr;
    auto u16 = [&](std::size_t o) { return unsigned(data[o] | data[o + 1] << 8); };
    auto grp = std::make_shared<Grp>();
    const unsigned count = u16(0);
    grp->width = u16(2); grp->height = u16(4);
    if (6 + std::size_t(count) * 8 > data.size()) return nullptr;
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t h = 6 + std::size_t(i) * 8;
        GrpFrame f{data[h], data[h + 1], data[h + 2], data[h + 3],
                   std::uint32_t(data[h + 4] | data[h + 5] << 8 | data[h + 6] << 16 | std::uint32_t(data[h + 7]) << 24), 0};
        // The frame's bytes: its line table and the furthest RLE line end.
        std::size_t end = f.offset + std::size_t(f.h) * 2;
        if (end > data.size()) { grp->frames.push_back({}); continue; }
        for (unsigned line = 0; line < f.h; ++line) {
            std::size_t p = f.offset + u16(f.offset + line * 2);
            for (unsigned x = 0; x < f.w && p < data.size();) {
                const unsigned c = data[p++];
                if (c & 0x80) x += c & 0x7f;
                else if (c & 0x40) { x += c & 0x3f; ++p; }
                else { x += c; p += c; }
            }
            end = std::max(end, p);
        }
        f.size = std::uint32_t(std::min(end, data.size()) - f.offset);
        grp->frames.push_back(f);
    }
    grp->data = std::move(data);
    return grp;
}
void load() {
    namespace fs = std::filesystem;
    const auto images = read_file(fs::path(root) / "arr/images.dat");
    const auto table = read_file(fs::path(root) / "arr/images.tbl");
    if (images.size() < 999 * 4 || table.size() < 2) {
        sc64::shell::log::warning("hd: %s/arr incomplete, N64 graphics kept", root.c_str());
        return;
    }
    for (unsigned i = 0; i < 999; ++i)
        image_grp[i] = images[i * 4] | images[i * 4 + 1] << 8 | images[i * 4 + 2] << 16 | std::uint32_t(images[i * 4 + 3]) << 24;
    const unsigned names = table[0] | table[1] << 8;
    grps.assign(names + 1, nullptr);
    unsigned loaded = 0;
    for (unsigned index : image_grp) {
        if (!index || index > names || grps[index] || 2 + index * 2 > table.size()) continue;
        const unsigned at = table[2 + (index - 1) * 2] | table[3 + (index - 1) * 2] << 8;
        std::string name;
        for (unsigned p = at; p < table.size() && table[p]; ++p)
            name += char(table[p] == '\\' ? '/' : std::tolower(table[p]));
        if ((grps[index] = parse_grp(read_file(fs::path(root) / "unit" / name)))) ++loaded;
    }
    unsigned sets = 0;
    for (unsigned i = 0; i < 8; ++i) {
        auto& t = tileset_data[i];
        const auto base = fs::path(root) / "tileset" / tilesets[i];
        t.cv5 = read_file(base.string() + ".cv5");
        t.vr4 = read_file(base.string() + ".vr4");
        const auto vx4 = read_file(base.string() + ".vx4");
        t.vx4.resize(vx4.size() / 2);
        for (std::size_t k = 0; k < t.vx4.size(); ++k) t.vx4[k] = std::uint16_t(vx4[k * 2] | vx4[k * 2 + 1] << 8);
        sets += !t.cv5.empty() && !t.vr4.empty() && !t.vx4.empty();
    }
    sc64::shell::log::info("hd: %u PC graphics, %u tilesets from %s", loaded, sets, root.c_str());
    ready = loaded > 0 && sets > 0;
}

// ---- The map's PC tiles: the chk the game loaded stays in RAM with its MTXM.
struct MapTiles {
    std::uint32_t tiles = 0;      // guest address of the MTXM data (little-endian u16 per tile)
    unsigned width = 0, height = 0, era = 0;
    std::uint32_t game_tiles = 0; // the game's own tile array address when found
};
MapTiles map;
bool locate_map(const std::uint8_t* rdram, unsigned width, unsigned height) {
    std::vector<std::uint8_t> ram(0x800000);
    for (std::size_t i = 0; i < ram.size(); i += 4) {
        std::uint32_t v; std::memcpy(&v, rdram + i, 4);
        ram[i] = std::uint8_t(v >> 24); ram[i + 1] = std::uint8_t(v >> 16); ram[i + 2] = std::uint8_t(v >> 8); ram[i + 3] = std::uint8_t(v);
    }
    const std::uint32_t bytes = width * height * 2;
    const std::uint8_t tag[8] = {'M', 'T', 'X', 'M', std::uint8_t(bytes), std::uint8_t(bytes >> 8),
                                 std::uint8_t(bytes >> 16), std::uint8_t(bytes >> 24)};
    for (auto it = ram.begin(); (it = std::search(it, ram.end(), tag, tag + 8)) != ram.end(); ++it) {
        const std::size_t at = std::size_t(it - ram.begin());
        if (at + 8 + bytes > ram.size()) break;
        const std::size_t from = at > 0x4000 ? at - 0x4000 : 0;
        const std::uint8_t era_tag[6] = {'E', 'R', 'A', ' ', 2, 0};
        auto e = std::find_end(ram.begin() + std::ptrdiff_t(from), it, era_tag, era_tag + 6);
        if (e == it) continue;
        map.tiles = 0x80000000u + std::uint32_t(at + 8);
        map.era = (e[8] | e[9] << 8) & 7;
        map.width = width; map.height = height;
        sc64::shell::log::info("hd: map %ux%u, %s, tiles at 0x%08x", width, height, tilesets[map.era], map.tiles);
        return true;
    }
    map = {};
    return false;
}
bool map_valid(const std::uint8_t* rdram) {
    if (!map.tiles) return false;
    const std::uint32_t tag = map.tiles - 8;
    return gb(rdram, tag) == 'M' && gb(rdram, tag + 1) == 'T' && gb(rdram, tag + 2) == 'X' && gb(rdram, tag + 3) == 'M';
}

// ---- HD canvases in guest memory (the render functions draw there)
struct Canvas {
    std::uint32_t pixels = 0;      // guest address, (2w x 2h) bytes
    std::uint32_t descriptor = 0;  // guest: s16 width, s16 height, u32 pixels
    unsigned width = 0, height = 0; // N64 canvas size
    bool dirty = false;
};
std::mutex mutex;
std::map<std::uint32_t, Canvas> canvases; // by N64 canvas physical address
Canvas* current = nullptr;
std::uint32_t current_canvas = 0;
std::uint32_t scratch = 0, rect = 0; // guest: one PC frame, its clip rectangle
constexpr std::uint32_t scratch_size = 256 * 1024;
std::uint64_t images_drawn = 0, frames_taken = 0;
// Time spent per part, logged every 1800 frames taken.
using Clock = std::chrono::steady_clock;
double terrain_ms = 0, image_ms = 0, take_ms = 0, compose_ms = 0;
std::uint64_t composed = 0, fallbacks = 0;
double since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

std::uint32_t guest_alloc(std::uint8_t* rdram, std::size_t size) {
    void* p = recomp::alloc(rdram, size);
    return p ? 0x80000000u + std::uint32_t(static_cast<std::uint8_t*>(p) - rdram) : 0;
}

// Decoded 32x32 megatiles of the map's tileset, made on first use.
struct MegaCache { unsigned era = ~0u; std::vector<std::int32_t> slot; std::vector<std::uint8_t> pixels; };
MegaCache mega_cache;
const std::uint8_t* megatile(unsigned mega) {
    const Tileset& t = tileset_data[map.era];
    if (mega_cache.era != map.era) {
        mega_cache.era = map.era;
        mega_cache.slot.assign(t.vx4.size() / 16, -1);
        mega_cache.pixels.clear();
    }
    if (mega >= mega_cache.slot.size()) return nullptr;
    if (mega_cache.slot[mega] < 0) {
        mega_cache.slot[mega] = std::int32_t(mega_cache.pixels.size() / 1024);
        mega_cache.pixels.resize(mega_cache.pixels.size() + 1024);
        std::uint8_t* out = mega_cache.pixels.data() + std::size_t(mega_cache.slot[mega]) * 1024;
        const std::size_t minitiles = t.vr4.size() / 64;
        for (unsigned y = 0; y < 32; ++y)
            for (unsigned x = 0; x < 32; ++x) {
                const unsigned v = t.vx4[mega * 16 + (y / 8) * 4 + x / 8];
                const std::size_t mini = v >> 1;
                out[y * 32 + x] = mini < minitiles ? t.vr4[mini * 64 + (y % 8) * 8 + ((v & 1) ? 7 - x % 8 : x % 8)] : 0;
            }
    }
    return mega_cache.pixels.data() + std::size_t(mega_cache.slot[mega]) * 1024;
}

// Calls a guest render function on an HD canvas: (x, y) in HD pixels, frame a guest
// frame header, the clip rectangle at rect, colouring data as the fifth argument.
void call_render(std::uint8_t* rdram, const recomp_context* ctx, const Canvas& c, std::uint32_t render, int x, int y,
                 std::uint32_t frame, std::uint32_t colour) {
    recomp_func_t* function = get_function(std::int32_t(render));
    if (!function) return;
    recomp_context call = *ctx;
    const std::uint32_t sp = (std::uint32_t(ctx->r29) - 0x400) & ~7u;
    call.r29 = gpr(std::int32_t(sp));
    call.r4 = gpr(std::int32_t(x)); call.r5 = gpr(std::int32_t(y));
    call.r6 = gpr(std::int32_t(frame)); call.r7 = gpr(std::int32_t(rect));
    sw(rdram, sp + 0x10, colour);
    const std::uint32_t saved = gw(rdram, render_canvas);
    sw(rdram, render_canvas, c.descriptor);
    function(rdram, &call);
    sw(rdram, render_canvas, saved);
}

// Index 0 (black) marks the dark beyond the map; presentation keeps the N64 pixels there.
// Rows are assembled in guest byte order (each 4-byte group reversed, as RDRAM holds
// them), so aligned megatile spans are plain copies.
// The last terrain drawn, reused while the camera stands still (most frames).
struct TerrainCache { int cam_x = 0, cam_y = 0, vx = 0, vy = 0; unsigned w = 0, h = 0, era = 0; std::uint32_t tiles = 0;
                      std::vector<std::uint8_t> bytes; };
TerrainCache terrain_cache;
void draw_terrain(std::uint8_t* rdram, const Canvas& c, int cam_x, int cam_y, int vx, int vy) {
    const Tileset& t = tileset_data[map.era];
    const unsigned w = c.width * 2, h = c.height * 2;
    TerrainCache& cache = terrain_cache;
    if (cache.cam_x == cam_x && cache.cam_y == cam_y && cache.vx == vx && cache.vy == vy && cache.w == w && cache.h == h
        && cache.era == map.era && cache.tiles == map.tiles && cache.bytes.size() == std::size_t(w) * h) {
        std::memcpy(rdram + offset(c.pixels), cache.bytes.data(), cache.bytes.size());
        return;
    }
    cache = {cam_x, cam_y, vx, vy, w, h, map.era, map.tiles, std::move(cache.bytes)};
    cache.bytes.resize(std::size_t(w) * h);
    const std::size_t groups = t.cv5.size() / 52;
    std::vector<std::uint8_t> row(w + 4);
    for (unsigned y = 0; y < h; ++y) {
        const int my = cam_y + int(y) - 2 * vy;
        std::fill(row.begin(), row.end(), 0);
        if (my >= 0 && my < int(map.height) * 32) {
            const unsigned ty = unsigned(my) / 32, py = unsigned(my) % 32;
            const int left = std::max(0, 2 * vx - cam_x), right = std::min(int(w), int(map.width) * 32 + 2 * vx - cam_x);
            for (int x = left; x < right;) {
                const unsigned mx = unsigned(cam_x + x - 2 * vx), tx = mx / 32, px = mx % 32;
                const std::uint32_t a = map.tiles + (ty * map.width + tx) * 2;
                const unsigned tile = gb(rdram, a) | gb(rdram, a + 1) << 8;
                const std::size_t group = tile >> 4;
                const unsigned run = std::min(32 - px, unsigned(right - x));
                const std::uint8_t* block = group < groups
                    ? megatile(t.cv5[group * 52 + 20 + (tile & 15) * 2] | t.cv5[group * 52 + 21 + (tile & 15) * 2] << 8)
                    : nullptr;
                if (block) {
                    const std::uint8_t* src = block + py * 32 + px;
                    if ((x & 3) == 0 && (px & 3) == 0 && (run & 3) == 0) {
                        for (unsigned k = 0; k < run; k += 4) {
                            row[unsigned(x) + k] = src[k + 3]; row[unsigned(x) + k + 1] = src[k + 2];
                            row[unsigned(x) + k + 2] = src[k + 1]; row[unsigned(x) + k + 3] = src[k];
                        }
                    } else
                        for (unsigned k = 0; k < run; ++k) row[(unsigned(x) + k) ^ 3] = src[k];
                }
                x += int(run);
            }
        }
        std::memcpy(cache.bytes.data() + std::size_t(y) * w, row.data(), w);
    }
    std::memcpy(rdram + offset(c.pixels), cache.bytes.data(), cache.bytes.size());
}
}

void initialize(const std::string& directory) {
    root = directory;
    std::error_code error;
    const auto parent = std::filesystem::path(directory).parent_path();
    if (std::filesystem::exists(parent / "nohd", error) || std::filesystem::exists(parent / "nohd.txt", error)) {
        sc64::shell::log::info("hd: nohd file present, N64 graphics kept");
        return;
    }
    if (!std::filesystem::exists(std::filesystem::path(directory) / "arr/images.dat", error)) {
        sc64::shell::log::info("hd: no PC graphics in %s, N64 graphics kept", directory.c_str());
        return;
    }
    if (!sc64_platform_thread(load)) sc64::shell::log::warning("hd: loader thread failed, N64 graphics kept");
}

bool enabled() { return ready.load(); }

Frames take(const std::uint8_t* rdram) {
    Frames out;
    if (!ready.load()) return out;
    std::lock_guard lock(mutex);
    const auto started = Clock::now();
    for (auto& [address, c] : canvases) {
        if (!c.dirty) continue;
        c.dirty = false;
        auto frame = std::make_shared<Frame>();
        frame->canvas = address; frame->canvas_width = c.width; frame->canvas_height = c.height;
        const std::size_t n = std::size_t(c.width) * c.height * 4;
        frame->pixels.resize(n);
        for (std::size_t i = 0; i < n; i += 4) {
            const std::uint32_t v = gw(rdram, c.pixels + std::uint32_t(i));
            frame->pixels[i] = std::uint8_t(v >> 24); frame->pixels[i + 1] = std::uint8_t(v >> 16);
            frame->pixels[i + 2] = std::uint8_t(v >> 8); frame->pixels[i + 3] = std::uint8_t(v);
        }
        std::uint64_t hash = 14695981039346656037ull;
        for (std::uint32_t i = 0; i < c.width * c.height; i += 4) hash = mix(hash, gw(rdram, 0x80000000u + address + i));
        frame->canvas_hash = hash;
        ++frames_taken;
        out.push_back(std::move(frame));
    }
    if (current && !current->dirty) current = nullptr; // images after the submission are not taken
    take_ms += since(started);
    if (!out.empty() && frames_taken % 1800 == 0)
        sc64::shell::log::info("hd ms/frame: terrain %.2f images %.2f take %.2f compose %.2f (%.2f per image, %llu) | %.1f images per frame, %llu N64 fallbacks",
            terrain_ms / 1800, image_ms / 1800, take_ms / 1800, compose_ms / 1800, composed ? compose_ms / double(composed) : 0.0,
            (unsigned long long)composed, double(images_drawn) / 1800, (unsigned long long)fallbacks),
        terrain_ms = image_ms = take_ms = compose_ms = 0, images_drawn = 0, composed = 0;
    return out;
}

std::optional<std::vector<std::uint8_t>> compose(const gfx::DrawList& list, const Frames& frames,
                                                 const std::vector<std::uint8_t>& rgba, const gfx::Image& target,
                                                 unsigned height) {
    if (frames.empty()) return std::nullopt;
    const gfx::Rectangle* bg = nullptr;
    const Frame* frame = nullptr;
    for (const auto& r : list.rectangles) {
        if (r.target != target || r.texture == gfx::no_texture || r.source == 0) continue;
        const auto& tex = list.textures[r.texture];
        if (!tex.background || tex.palette.size() != 256 || r.dsdx != 1024 || r.dtdy != 1024) continue;
        for (const auto& f : frames)
            if (f->canvas == r.source) { bg = &r; frame = f.get(); }
    }
    if (!bg) return std::nullopt;
    const auto& tex = list.textures[bg->texture];
    // The frame must show exactly the canvas this background presents.
    const std::size_t canvas_bytes = std::size_t(frame->canvas_width) * frame->canvas_height;
    if (frame->canvas + canvas_bytes > list.initial_ram.size()) return std::nullopt;
    struct Heavy { Heavy() { sc64_platform_heavy_begin(); } ~Heavy() { sc64_platform_heavy_end(); } } heavy;
    const auto started = Clock::now();
    std::uint64_t hash = 14695981039346656037ull;
    const std::uint8_t* canvas = list.initial_ram.data() + frame->canvas;
    for (std::size_t i = 0; i + 4 <= canvas_bytes; i += 4)
        hash = mix(hash, std::uint32_t(canvas[i]) << 24 | std::uint32_t(canvas[i + 1]) << 16
                         | std::uint32_t(canvas[i + 2]) << 8 | canvas[i + 3]);
    if (hash != frame->canvas_hash) return std::nullopt;

    const unsigned w = target.width, h = height, ow = w * 2;
    auto pack = [](int r, int g, int b) { return std::uint32_t(r) | std::uint32_t(g) << 8 | std::uint32_t(b) << 16 | 0xff000000u; };
    const std::uint32_t fog_pixel = pack(fog_r, fog_g, fog_b);
    std::array<std::uint32_t, 256> palette;
    for (unsigned i = 0; i < 256; ++i) palette[i] = pack(tex.palette[i].r, tex.palette[i].g, tex.palette[i].b);
    std::vector<std::uint8_t> bytes(std::size_t(ow) * h * 2 * 4);
    auto* out = reinterpret_cast<std::uint32_t*>(bytes.data());
    const auto* in = reinterpret_cast<const std::uint32_t*>(rgba.data());
    const auto bounds = gfx::raster_bounds(*bg);
    const int s0 = bg->s >> 5, t0 = bg->t >> 5, x0 = bg->x0 / 4, y0 = bg->y0 / 4;
    const unsigned fw = frame->canvas_width * 2;
    // Pass 1: each canvas pixel's fog alpha (0..256), or -1 where the RDP drew over
    // the canvas (HUD, cursor) or outside the background.
    std::vector<std::int16_t> alpha(std::size_t(w) * h, -1);
    const int pc[3] = {fog_r, fog_g, fog_b};
    for (unsigned y = 0; y < h; ++y) {
        const int v = t0 + int(y) - y0;
        if (int(y) < bounds.y0 || int(y) >= bounds.y1 || v < 0 || v >= int(frame->canvas_height) || v >= int(tex.height))
            continue;
        const std::uint8_t* row = canvas + std::size_t(v) * frame->canvas_width;
        for (unsigned x = 0; x < w; ++x) {
            const int u = s0 + int(x) - x0;
            if (int(x) < bounds.x0 || int(x) >= bounds.x1 || u < 0 || u >= int(frame->canvas_width) || u >= int(tex.width))
                continue;
            const std::uint32_t pixel = in[std::size_t(y) * w + x] | 0xff000000u, b = palette[row[u]];
            std::int16_t& a = alpha[std::size_t(y) * w + x];
            if (pixel == b) { a = 0; continue; }
            if (pixel == fog_pixel) { a = 256; continue; }
            // The RDP blends the canvas toward the fog colour (fog of war, the map's
            // edges): solve for that blend; anything else was drawn over the canvas.
            const int bc[3] = {int(b & 255), int(b >> 8 & 255), int(b >> 16 & 255)};
            const int fc[3] = {int(pixel & 255), int(pixel >> 8 & 255), int(pixel >> 16 & 255)};
            int k = 0;
            for (int c = 1; c < 3; ++c) if (std::abs(bc[c] - pc[c]) > std::abs(bc[k] - pc[k])) k = c;
            const int span = bc[k] - pc[k];
            const int value = std::abs(span) < 8 ? 256 : std::clamp((bc[k] - fc[k]) * 256 / span, 0, 256);
            bool fits = true;
            for (int c = 0; c < 3; ++c) fits &= std::abs((bc[c] - fc[c]) * 256 - value * (bc[c] - pc[c])) <= 12 * 256;
            if (fits) a = std::int16_t(value);
        }
    }
    // Pass 2: the RDP dithers the fog; a 3x3 average over the blended pixels
    // smooths its edges at twice the resolution. Padded by one pixel, separable.
    const unsigned pw = w + 2, ph = h + 2;
    std::vector<std::int32_t> sums(std::size_t(pw) * ph, 0), counts(std::size_t(pw) * ph, 0);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const int a = alpha[std::size_t(y) * w + x];
            if (a < 0) continue;
            const std::size_t i = std::size_t(y + 1) * pw + x + 1;
            sums[i - 1] += a; sums[i] += a; sums[i + 1] += a;
            counts[i - 1] += 1; counts[i] += 1; counts[i + 1] += 1;
        }
    std::vector<std::int16_t> smooth(std::size_t(pw) * ph, -1);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            if (alpha[std::size_t(y) * w + x] < 0) continue;
            const std::size_t i = std::size_t(y + 1) * pw + x + 1;
            const int sum = sums[i - pw] + sums[i] + sums[i + pw], count = counts[i - pw] + counts[i] + counts[i + pw];
            smooth[i] = std::int16_t(sum / count);
        }
    // Pass 3: each HD pixel with the alpha interpolated toward its neighbours.
    for (unsigned y = 0; y < h; ++y) {
        std::uint32_t* o0 = out + std::size_t(y) * 2 * ow;
        std::uint32_t* o1 = o0 + ow;
        const std::uint32_t* f = in + std::size_t(y) * w;
        const int v = t0 + int(y) - y0;
        const std::int16_t* s = smooth.data() + std::size_t(y + 1) * pw + 1;
        for (unsigned x = 0; x < w; ++x) {
            const std::uint32_t pixel = f[x] | 0xff000000u;
            const int a = s[x];
            if (a < 0) { o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = pixel; continue; }
            const int u = s0 + int(x) - x0;
            const std::uint8_t* hd0 = frame->pixels.data() + std::size_t(v) * 2 * fw + unsigned(u) * 2;
            const std::uint8_t* hd1 = hd0 + fw;
            const std::uint8_t q[4] = {hd0[0], hd0[1], hd1[0], hd1[1]};
            std::uint32_t* const o[4] = {&o0[2 * x], &o0[2 * x + 1], &o1[2 * x], &o1[2 * x + 1]};
            auto near = [&](int dx, int dy) { const int n = s[int(x) + dx + dy * int(pw)]; return n < 0 ? a : n; };
            const int l = near(-1, 0), r = near(1, 0), up = near(0, -1), dn = near(0, 1);
            if (a == 0 && l == 0 && r == 0 && up == 0 && dn == 0 && near(-1, -1) == 0 && near(1, -1) == 0
                && near(-1, 1) == 0 && near(1, 1) == 0) { // clear of fog: the HD pixels as they are
                for (int i = 0; i < 4; ++i) *o[i] = q[i] ? palette[q[i]] : pixel;
                continue;
            }
            for (int i = 0; i < 4; ++i) {
                if (!q[i]) { *o[i] = pixel; continue; } // beyond the map: as the N64 drew it
                const int dx = (i & 1) ? 1 : -1, dy = (i & 2) ? 1 : -1;
                const int sub = (9 * a + 3 * (dx > 0 ? r : l) + 3 * (dy > 0 ? dn : up) + near(dx, dy)) / 16;
                const std::uint32_t c = palette[q[i]];
                if (sub == 0) { *o[i] = c; continue; }
                const int keep = 256 - sub;
                *o[i] = pack((int(c & 255) * keep + fog_r * sub) >> 8, (int(c >> 8 & 255) * keep + fog_g * sub) >> 8,
                             (int(c >> 16 & 255) * keep + fog_b * sub) >> 8);
            }
        }
    }
    compose_ms += since(started);
    ++composed;
    return bytes;
}
}

using namespace sc64::hd;

namespace {
// An image the HD canvas has no PC frame for (or cannot draw): the N64 canvas
// region its render call may touch, before the call; after it, the pixels the call
// changed are copied at 2x into the HD canvas, in the same draw order.
struct Fallback { bool pending = false; std::uint32_t pixels = 0; unsigned stride = 0; int x = 0, y = 0, w = 0, h = 0;
                  std::vector<std::uint8_t> before; };
Fallback fallback;
void begin_fallback(std::uint8_t* rdram, const recomp_context* ctx) {
    const std::uint32_t descriptor = gw(rdram, render_canvas);
    if ((descriptor >> 24) != 0x80) return;
    const std::uint32_t r = std::uint32_t(ctx->r7);
    fallback.stride = gh(rdram, descriptor);
    fallback.pixels = gw(rdram, descriptor + 4);
    fallback.x = std::int32_t(ctx->r4); fallback.y = std::int32_t(ctx->r5);
    fallback.w = std::int16_t(gh(rdram, r + 4)); fallback.h = std::int16_t(gh(rdram, r + 6));
    if (fallback.x < 0 || fallback.y < 0 || fallback.w <= 0 || fallback.h <= 0 || fallback.w > 512 || fallback.h > 512
        || (fallback.pixels >> 24) != 0x80) return;
    fallback.before.resize(std::size_t(fallback.w) * fallback.h);
    for (int y = 0; y < fallback.h; ++y)
        for (int x = 0; x < fallback.w; ++x)
            fallback.before[std::size_t(y) * fallback.w + x] =
                gb(rdram, fallback.pixels + std::uint32_t((fallback.y + y) * int(fallback.stride) + fallback.x + x));
    fallback.pending = true;
}
}

// Before the camera renderer's terrain copy (0x8000AE44): a new frame of the
// current canvas, its PC terrain.
extern "C" void sc64_hd_terrain(std::uint8_t* rdram, recomp_context*) {
    if (!ready.load()) return;
    std::lock_guard lock(mutex);
    current = nullptr;
    const unsigned width = gh(rdram, canvas_descriptor), height = gh(rdram, canvas_descriptor + 2);
    const std::uint32_t canvas = gw(rdram, canvas_descriptor + 4) & 0x1fffffff;
    if (width < 320 || width > 512 || height != 240 || canvas == 0 || canvas + width * height > 0x800000) return;
    const unsigned map_w = gh(rdram, map_size), map_h = gh(rdram, map_size + 2);
    const std::uint32_t game_tiles = gw(rdram, tile_array);
    if (!map_w || !map_h || map_w > 256 || map_h > 256) return;
    if (game_tiles != map.game_tiles || map_w != map.width || map_h != map.height || !map_valid(rdram)) {
        if (!locate_map(rdram, map_w, map_h)) { map.game_tiles = game_tiles; return; }
        map.game_tiles = game_tiles;
    }
    if (!scratch) {
        scratch = guest_alloc(rdram, scratch_size + 16);
        rect = scratch + scratch_size;
        if (!scratch) { ready = false; sc64::shell::log::warning("hd: no guest heap, N64 graphics kept"); return; }
    }
    Canvas& c = canvases[canvas];
    if (!c.pixels || c.width != width || c.height != height) {
        if (c.pixels) recomp::free(rdram, rdram + offset(c.pixels));
        c.pixels = guest_alloc(rdram, std::size_t(width) * height * 4 + 16);
        if (!c.pixels) { canvases.erase(canvas); return; }
        c.descriptor = c.pixels + width * height * 4;
        c.width = width; c.height = height;
        sh(rdram, c.descriptor, std::uint16_t(width * 2)); sh(rdram, c.descriptor + 2, std::uint16_t(height * 2));
        sw(rdram, c.descriptor + 4, c.pixels);
    }
    const auto started = Clock::now();
    draw_terrain(rdram, c, std::int32_t(gw(rdram, camera_x)), std::int32_t(gw(rdram, camera_y)),
                 gh(rdram, view_x), gh(rdram, view_y));
    terrain_ms += since(started);
    c.dirty = true;
    current = &c;
    current_canvas = canvas;
}

// drawImage (0x8012011C) about to call the image's render function (s0 = image,
// v0 = function): the same function draws the image's PC frame at 2x.
extern "C" void sc64_hd_image(std::uint8_t* rdram, recomp_context* ctx) {
    if (!current) return;
    std::lock_guard lock(mutex);
    if (!current || !current->dirty) return;
    const std::uint32_t image = std::uint32_t(ctx->r16), render = std::uint32_t(ctx->r2);
    const Canvas& c = *current;
    if (render == hp_bar) {
        // Health bars are drawn from the unit and the clip rectangle, not from a
        // frame: the game's own placement and rectangle at twice the size.
        const std::uint32_t r = std::uint32_t(ctx->r7);
        sh(rdram, rect, std::uint16_t(2 * gh(rdram, r))); sh(rdram, rect + 2, std::uint16_t(2 * gh(rdram, r + 2)));
        sh(rdram, rect + 4, std::uint16_t(2 * gh(rdram, r + 4))); sh(rdram, rect + 6, std::uint16_t(2 * gh(rdram, r + 6)));
        call_render(rdram, ctx, c, render, 2 * std::int32_t(ctx->r4), 2 * std::int32_t(ctx->r5), std::uint32_t(ctx->r6),
                    gw(rdram, std::uint32_t(ctx->r29) + 0x10));
        return;
    }
    const unsigned id = gh(rdram, image + 8), flags = gh(rdram, image + 0xA);
    const std::uint32_t sprite = gw(rdram, image + 0x38);
    const Grp* grp = id < 999 && image_grp[id] && image_grp[id] < grps.size() ? grps[image_grp[id]].get() : nullptr;
    const unsigned frame_index = (gw(rdram, image + 0xC) >> 6) & 0xFFF;
    if (!grp || frame_index >= grp->frames.size() || (sprite >> 24) != 0x80) { begin_fallback(rdram, ctx); return; }
    const GrpFrame& f = grp->frames[frame_index];
    if (!f.w || !f.h || !f.size || f.size + 8 > scratch_size) { begin_fallback(rdram, ctx); return; }
    const bool flipped = flags & 0x4000;
    const int hoff = std::int8_t(gb(rdram, image + 0x10)), voff = std::int8_t(gb(rdram, image + 0x11));
    const int sx = std::int16_t(gh(rdram, sprite + 0x14)), sy = std::int16_t(gh(rdram, sprite + 0x16));
    int x = sx + hoff + (flipped ? int(grp->width / 2) - f.x - f.w : f.x - int(grp->width / 2));
    int y = sy + voff - int(grp->height / 2) + f.y;
    if (flags & 0x2000) y = std::int16_t(gh(rdram, image + 0x1E)); // positioned by the game
    x += 2 * gh(rdram, view_x) - std::int32_t(gw(rdram, camera_x));
    y += 2 * gh(rdram, view_y) - std::int32_t(gw(rdram, camera_y));
    const int cw = int(c.width) * 2, ch = int(c.height) * 2;
    const int rx = std::max(0, -x), ry = std::max(0, -y);
    const int rw = std::min(int(f.w), cw - x) - rx, rh = std::min(int(f.h), ch - y) - ry;
    if (rw <= 0 || rh <= 0) return;
    const auto started = Clock::now();
    // The PC frame in the game's own layout: header, then its line table and lines.
    sw(rdram, scratch, std::uint32_t(f.x) << 24 | std::uint32_t(f.y) << 16 | std::uint32_t(f.w) << 8 | f.h);
    sw(rdram, scratch + 4, scratch + 8);
    put(rdram, scratch + 8, grp->data.data() + f.offset, f.size);
    sh(rdram, rect, std::uint16_t(rx)); sh(rdram, rect + 2, std::uint16_t(ry));
    sh(rdram, rect + 4, std::uint16_t(rw)); sh(rdram, rect + 6, std::uint16_t(rh));
    call_render(rdram, ctx, c, render, x + rx, y + ry, scratch, gw(rdram, image + 0x2C));
    ++images_drawn;
    image_ms += since(started);
}

// After the image's render call: the N64 pixels of a fallback image, at 2x.
extern "C" void sc64_hd_image_done(std::uint8_t* rdram, recomp_context*) {
    if (!fallback.pending) return;
    fallback.pending = false;
    std::lock_guard lock(mutex);
    if (!current || !current->dirty) return;
    const Canvas& c = *current;
    const unsigned hw = c.width * 2, hh = c.height * 2;
    for (int y = 0; y < fallback.h; ++y)
        for (int x = 0; x < fallback.w; ++x) {
            const std::uint8_t value =
                gb(rdram, fallback.pixels + std::uint32_t((fallback.y + y) * int(fallback.stride) + fallback.x + x));
            if (value == fallback.before[std::size_t(y) * fallback.w + x]) continue;
            const unsigned hx = unsigned(fallback.x + x) * 2, hy = unsigned(fallback.y + y) * 2;
            if (hx + 1 >= hw || hy + 1 >= hh) continue;
            for (unsigned dy = 0; dy < 2; ++dy)
                for (unsigned dx = 0; dx < 2; ++dx)
                    rdram[offset((c.pixels + (hy + dy) * hw + hx + dx) ^ 3)] = value;
        }
    ++fallbacks;
}
