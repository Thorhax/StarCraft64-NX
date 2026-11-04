#include "hd.hpp"
#include "log.hpp"
#include "platform.hpp"
#include <recomp.h>
#include <librecomp/addresses.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace sc64::hd {
namespace {
// StarCraft 64 USA, 1P missions (docs/research.md, "HD missions").
constexpr std::uint32_t canvas_descriptor = 0x800A62F4; // u16 width, u16 height, u32 pixels
constexpr std::uint32_t camera_x = 0x800A2EE8, camera_y = 0x800A2EEC; // s32, PC pixels
constexpr std::uint32_t view_x = 0x800A5B0A, view_y = 0x800A5B0C;    // u16 canvas offset of the view
constexpr std::uint32_t map_size = 0x800B1D44 + 0xE4;                // u16 tiles wide, high
constexpr std::uint32_t tile_array = 0x800A2ED4;                     // the game's own (N64) tile array
constexpr std::uint32_t n64_megatiles = 0x800A2ED0, n64_minitiles = 0x800A2ED8; // its megatile and minitile tables
constexpr std::uint32_t live_tiles = 0x800A6150;                     // tile IDs as the game updates them (creep)
constexpr std::uint32_t creep_edges = 0x800EEE78, creep_edges_width = 0x800EEE7C; // u8 per tile: edge frame + 1
constexpr std::uint32_t mission_palette = 0x800A5F30;                // RGBA16, the tileset's palette
constexpr std::uint32_t render_canvas = 0x800AFD60;                  // canvas the render functions draw to
constexpr std::uint32_t hp_bar = 0x80029CC0;                         // render function of health bars (11, 18)
// The common render functions, drawn natively (normal: player colours through the
// remap table; shadow: darkened through the shadow table), each with its flipped twin.
constexpr std::uint32_t render_normal = 0x80048238, render_normal_flipped = 0x80049A10;
constexpr std::uint32_t render_shadow = 0x80048F7C, render_shadow_flipped = 0x80049D84;
constexpr std::uint32_t remap_table = 0x800B1784, shadow_table = 0x800AFA44; // shadow: [word] + 0x1200
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
struct GrpFrame { std::uint8_t x, y, w, h; std::uint32_t offset, size; std::vector<std::uint8_t> swapped; };
struct Grp { unsigned width = 0, height = 0; std::vector<GrpFrame> frames; std::vector<std::uint8_t> data; };
struct Tileset {
    std::vector<std::uint8_t> cv5, vr4, wpe;
    std::vector<std::uint16_t> vx4;
    std::shared_ptr<Grp> creep;  // <era>.grp: creep edge frames
    std::vector<float> features; // per minitile: 4x4 RGB box averages, made on first use
    std::vector<float> means;    // per minitile: mean RGB
};
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
        t.wpe = read_file(base.string() + ".wpe");
        t.creep = parse_grp(read_file(base.string() + ".grp"));
        const auto vx4 = read_file(base.string() + ".vx4");
        t.vx4.resize(vx4.size() / 2);
        for (std::size_t k = 0; k < t.vx4.size(); ++k) t.vx4[k] = std::uint16_t(vx4[k * 2] | vx4[k * 2 + 1] << 8);
        sets += !t.cv5.empty() && !t.vr4.empty() && !t.vx4.empty() && t.wpe.size() >= 1024;
    }
    sc64::shell::log::info("hd: %u PC graphics, %u tilesets from %s", loaded, sets, root.c_str());
    ready = loaded > 0 && sets > 0;
}

// ---- The map's terrain. The game draws from its own tile array: N64 megatiles
// (same 4x4 minitile layout and flips as the PC's, renumbered, half-size minitiles).
// Each N64 minitile is drawn as its PC minitile: learned from the map's PC MTXM
// (the loaded chk stays in RAM) where the two agree, otherwise the PC minitile
// that looks the same at half size. Creep follows the game's live tile IDs, which
// keep the PC's creep group (1), and its edge frames (docs/research.md).
struct MapTiles {
    std::uint32_t game_tiles = 0;          // the game's tile array (BE u16 per tile, N64 megatile | flags)
    std::uint32_t megas = 0, minis = 0;    // N64 megatiles (16 BE u16 refs) and minitiles (4x4 bytes)
    unsigned width = 0, height = 0, era = 0;
    bool found = false;                    // the chk (era, MTXM) was found
    std::vector<std::int32_t> mini_map;    // N64 minitile -> PC minitile, -1 unknown, -2 unmatched yet
    std::vector<std::int32_t> offsets;     // PC - N64 minitile index of the learned ones, most common first
};
MapTiles map;
unsigned match_budget = 0; // content matches allowed this frame

const std::vector<float>& features(Tileset& t) {
    if (t.features.empty()) {
        const std::size_t count = t.vr4.size() / 64;
        t.features.resize(count * 48);
        for (std::size_t m = 0; m < count; ++m)
            for (unsigned by = 0; by < 4; ++by)
                for (unsigned bx = 0; bx < 4; ++bx)
                    for (unsigned c = 0; c < 3; ++c) {
                        float sum = 0;
                        for (unsigned dy = 0; dy < 2; ++dy)
                            for (unsigned dx = 0; dx < 2; ++dx)
                                sum += t.wpe[t.vr4[m * 64 + (by * 2 + dy) * 8 + bx * 2 + dx] * 4u + c];
                        t.features[m * 48 + (by * 4 + bx) * 3 + c] = sum / 4;
                    }
        t.means.resize(count * 3);
        for (std::size_t m = 0; m < count; ++m)
            for (unsigned c = 0; c < 3; ++c) {
                float sum = 0;
                for (unsigned k = 0; k < 16; ++k) sum += t.features[m * 48 + k * 3 + c];
                t.means[m * 3 + c] = sum / 16;
            }
    }
    return t.features;
}
void n64_features(const std::uint8_t* rdram, const Tileset& t, unsigned mini, float* out) {
    const std::uint32_t base = map.minis + mini * 16;
    for (unsigned k = 0; k < 16; ++k)
        for (unsigned c = 0; c < 3; ++c) out[k * 3 + c] = t.wpe[gb(rdram, base + k) * 4u + c];
}
float distance(const float* a, const float* b) {
    float d = 0;
    for (unsigned k = 0; k < 48; ++k) d += (a[k] - b[k]) * (a[k] - b[k]);
    return d;
}
constexpr float same_look = 48 * 24 * 24; // largest distance of an N64 minitile and its PC original
// The PC minitile closest to an N64 one (cached in mini_map). Both tables keep the
// same order with gaps, so the offsets of the learned minitiles come first (creep
// and other tiles the map file lacks); then, within match_budget, every PC minitile.
// -2: not matched yet.
std::int32_t match_minitile(const std::uint8_t* rdram, unsigned mini) {
    Tileset& t = tileset_data[map.era];
    const auto& f = features(t);
    float n64[48], mean[3] = {0, 0, 0};
    n64_features(rdram, t, mini, n64);
    std::int32_t best = 0;
    float best_d = 1e30f;
    const std::int64_t pc_minis = std::int64_t(f.size() / 48);
    for (const std::int32_t o : map.offsets) {
        const std::int64_t m = std::int64_t(mini) + o;
        if (m < 0 || m >= pc_minis) continue;
        const float d = distance(n64, f.data() + m * 48);
        if (d < best_d) { best_d = d; best = std::int32_t(m); }
    }
    if (best_d <= same_look) return best;
    if (!match_budget) return -2;
    --match_budget;
    for (unsigned k = 0; k < 16; ++k) for (unsigned c = 0; c < 3; ++c) mean[c] += n64[k * 3 + c] / 16;
    for (std::size_t m = 0; m < f.size() / 48; ++m) {
        // 16 x the squared mean difference never exceeds the distance: most candidates stop here.
        const float* pm = t.means.data() + m * 3;
        const float bound = 16 * ((mean[0] - pm[0]) * (mean[0] - pm[0]) + (mean[1] - pm[1]) * (mean[1] - pm[1])
                                  + (mean[2] - pm[2]) * (mean[2] - pm[2]));
        if (bound >= best_d) continue;
        const float* pf = f.data() + m * 48;
        float d = 0;
        for (unsigned k = 0; k < 48 && d < best_d; ++k) d += (n64[k] - pf[k]) * (n64[k] - pf[k]);
        if (d < best_d) { best_d = d; best = std::int32_t(m); }
    }
    return best;
}
bool locate_map(const std::uint8_t* rdram, unsigned width, unsigned height) {
    map = {};
    map.game_tiles = gw(rdram, tile_array); map.megas = gw(rdram, n64_megatiles); map.minis = gw(rdram, n64_minitiles);
    map.width = width; map.height = height;
    if ((map.game_tiles >> 24) != 0x80 || (map.megas >> 24) != 0x80 || (map.minis >> 24) != 0x80) return false;
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
        map.era = (e[8] | e[9] << 8) & 7;
        map.found = true;
        // Learn N64 -> PC minitiles where the map's PC tile and the game's tile agree:
        // same flips, the PC minitile most often under the N64 one (creep laid at the
        // start covers many different tiles), same look at half size.
        Tileset& tileset = tileset_data[map.era];
        const auto& f = features(tileset);
        const std::size_t groups = tileset.cv5.size() / 52, pc_megas = tileset.vx4.size() / 16, pc_minis = f.size() / 48;
        std::size_t learned = 0;
        std::unordered_map<unsigned, std::unordered_map<unsigned, unsigned>> votes; // N64 minitile -> PC minitile -> count
        for (std::size_t pos = 0; pos < std::size_t(width) * height; ++pos) {
            const unsigned pc_tile = ram[at + 8 + pos * 2] | ram[at + 9 + pos * 2] << 8;
            const unsigned n = gh(rdram, map.game_tiles + std::uint32_t(pos) * 2) & 0x7fff;
            if ((pc_tile >> 4) >= groups) continue;
            const unsigned pc_mega = tileset.cv5[(pc_tile >> 4) * 52 + 20 + (pc_tile & 15) * 2]
                                     | tileset.cv5[(pc_tile >> 4) * 52 + 21 + (pc_tile & 15) * 2] << 8;
            if (pc_mega >= pc_megas) continue;
            for (unsigned k = 0; k < 16; ++k) {
                const unsigned rn = gh(rdram, map.megas + n * 32 + k * 2), rp = tileset.vx4[pc_mega * 16 + k];
                if ((rn & 1) != (rp & 1) || (rp >> 1) >= pc_minis) continue;
                ++votes[rn >> 1][rp >> 1];
            }
        }
        std::map<std::int32_t, unsigned> offsets;
        for (const auto& [mini, counts] : votes) {
            unsigned total = 0, most = 0, pc = 0;
            for (const auto& [candidate, count] : counts) {
                total += count;
                if (count > most) { most = count; pc = candidate; }
            }
            if (most * 2 <= total) continue;
            float n64[48];
            n64_features(rdram, tileset, mini, n64);
            if (distance(n64, f.data() + std::size_t(pc) * 48) > same_look) continue;
            if (mini >= map.mini_map.size()) map.mini_map.resize(mini + 1, -2);
            map.mini_map[mini] = std::int32_t(pc);
            ++offsets[std::int32_t(pc) - std::int32_t(mini)];
            ++learned;
        }
        std::vector<std::pair<unsigned, std::int32_t>> ranked;
        for (const auto& [o, count] : offsets) ranked.emplace_back(count, o);
        std::sort(ranked.rbegin(), ranked.rend());
        for (const auto& r : ranked) map.offsets.push_back(r.second);
        sc64::shell::log::info("hd: map %ux%u, %s, %zu minitiles from its PC tiles (%zu offsets)", width, height,
                               tilesets[map.era], learned, map.offsets.size());
        return true;
    }
    // No chk in memory: the tileset whose palette the game shows, every minitile by its look.
    unsigned best = 0, best_hits = 0;
    for (unsigned era = 0; era < 8; ++era) {
        const auto& wpe = tileset_data[era].wpe;
        if (wpe.size() < 1024) continue;
        unsigned hits = 0;
        for (unsigned i = 0; i < 256; ++i) {
            const unsigned c = gh(rdram, mission_palette + i * 2);
            hits += (c >> 11) == (wpe[i * 4] >> 3u) && ((c >> 6) & 31) == (wpe[i * 4 + 1] >> 3u) && ((c >> 1) & 31) == (wpe[i * 4 + 2] >> 3u);
        }
        if (hits > best_hits) { best_hits = hits; best = era; }
    }
    if (best_hits < 200) {
        sc64::shell::log::warning("hd: map %ux%u without its PC tiles or a known palette, N64 terrain kept", width, height);
        return false;
    }
    map.era = best;
    map.found = true;
    sc64::shell::log::info("hd: map %ux%u, %s by its palette, minitiles matched by their look", width, height, tilesets[best]);
    return true;
}

// 32x32 PC pixels of an N64 megatile; incomplete (some minitiles not matched yet)
// blocks are not kept and come back on a later frame.
struct MegaCache { std::vector<std::int32_t> slot; std::vector<std::uint8_t> pixels; std::uint32_t megas = 0; };
MegaCache mega_cache;
const std::uint8_t* megatile(const std::uint8_t* rdram, unsigned n, bool& complete) {
    static std::array<std::uint8_t, 1024> scratch_block;
    complete = true;
    if (mega_cache.megas != map.megas) { mega_cache = {}; mega_cache.megas = map.megas; }
    if (n < mega_cache.slot.size() && mega_cache.slot[n] >= 0)
        return mega_cache.pixels.data() + std::size_t(mega_cache.slot[n]) * 1024;
    const Tileset& t = tileset_data[map.era];
    const std::size_t pc_minis = t.vr4.size() / 64;
    std::uint8_t* out = scratch_block.data();
    for (unsigned k = 0; k < 16; ++k) {
        const unsigned ref = gh(rdram, map.megas + n * 32 + k * 2), mini = ref >> 1;
        if (mini >= map.mini_map.size()) map.mini_map.resize(mini + 1, -2);
        std::int32_t& pc = map.mini_map[mini];
        if (pc < 0) pc = match_minitile(rdram, mini);
        const unsigned bx = (k % 4) * 8, by = (k / 4) * 8;
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x)
                out[(by + y) * 32 + bx + x] = pc >= 0 && std::size_t(pc) < pc_minis
                    ? t.vr4[std::size_t(pc) * 64 + y * 8 + ((ref & 1) ? 7 - x : x)] : 0;
        complete &= pc >= 0;
    }
    if (!complete) return out;
    if (n >= mega_cache.slot.size()) mega_cache.slot.resize(n + 1, -1);
    mega_cache.slot[n] = std::int32_t(mega_cache.pixels.size() / 1024);
    mega_cache.pixels.insert(mega_cache.pixels.end(), out, out + 1024);
    return mega_cache.pixels.data() + std::size_t(mega_cache.slot[n]) * 1024;
}

// 32x32 PC pixels of a PC megatile (creep), cached.
struct PcMegaCache { std::vector<std::int32_t> slot; std::vector<std::uint8_t> pixels; unsigned era = ~0u; };
PcMegaCache pc_mega_cache;
const std::uint8_t* pc_megatile(unsigned m) {
    PcMegaCache& cache = pc_mega_cache;
    if (cache.era != map.era) cache = {{}, {}, map.era};
    if (m < cache.slot.size() && cache.slot[m] >= 0) return cache.pixels.data() + std::size_t(cache.slot[m]) * 1024;
    const Tileset& t = tileset_data[map.era];
    if (m >= t.vx4.size() / 16) return nullptr;
    std::array<std::uint8_t, 1024> out{};
    for (unsigned k = 0; k < 16; ++k) {
        const unsigned ref = t.vx4[m * 16 + k], mini = ref >> 1;
        if (std::size_t(mini) * 64 + 64 > t.vr4.size()) return nullptr;
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x)
                out[((k / 4) * 8 + y) * 32 + (k % 4) * 8 + x] = t.vr4[std::size_t(mini) * 64 + y * 8 + ((ref & 1) ? 7 - x : x)];
    }
    if (m >= cache.slot.size()) cache.slot.resize(m + 1, -1);
    cache.slot[m] = std::int32_t(cache.pixels.size() / 1024);
    cache.pixels.insert(cache.pixels.end(), out.begin(), out.end());
    return cache.pixels.data() + std::size_t(cache.slot[m]) * 1024;
}
// The PC megatile of a creep tile ID (cv5 group 1), or none.
const std::uint8_t* creep_megatile(unsigned id) {
    if ((id & 0x7fff) >> 4 != 1) return nullptr;
    const auto& cv5 = tileset_data[map.era].cv5;
    if (cv5.size() < 2 * 52) return nullptr;
    const unsigned at = 52 + 20 + (id & 15) * 2;
    const unsigned m = cv5[at] | cv5[at + 1] << 8;
    return m ? pc_megatile(m) : nullptr;
}
// The creep edge frame on a tile beside creep (game tile flag 0x8000; 0x8000A10C draws
// the same frame of the game's own edge graphic), its pixels over the terrain bytes:
// w x h, (x0, y0) the tile's top left, guest byte order.
void draw_creep_edge(std::uint8_t* bytes, unsigned w, unsigned h, int x0, int y0, unsigned frame) {
    const auto& grp = tileset_data[map.era].creep;
    if (!grp || frame >= grp->frames.size() || !grp->frames[frame].size) return;
    const GrpFrame& f = grp->frames[frame];
    const auto& d = grp->data;
    const int left = x0 + f.x - int(grp->width) / 2 + 16, top = y0 + f.y - int(grp->height) / 2 + 16;
    for (unsigned line = 0; line < f.h; ++line) {
        const int y = top + int(line);
        std::size_t p = f.offset + (d[f.offset + line * 2] | d[f.offset + line * 2 + 1] << 8);
        for (unsigned lx = 0; lx < f.w && p < d.size();) {
            const unsigned c = d[p++];
            unsigned run = c & 0x7f;
            if (c & 0x80) { lx += run; continue; }
            const bool fill = c & 0x40;
            if (fill) run = c & 0x3f;
            for (unsigned k = 0; k < run && p < d.size(); ++k) {
                const int x = left + int(lx + k);
                const std::uint8_t v = fill ? d[p] : d[p + k];
                if (y >= 0 && y < int(h) && x >= 0 && x < int(w)) bytes[(std::size_t(y) * w + unsigned(x)) ^ 3] = v;
            }
            p += fill ? 1 : run;
            lx += run;
        }
    }
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
double terrain_ms = 0, image_ms = 0, take_ms = 0, fill_ms = 0, compose_ms = 0, compose_cpu_ms = 0;
std::uint64_t composed = 0, fallbacks = 0;
// hdverify (next to the hd folder): every native image is also drawn by the game's
// function and the two compared; differences are logged.
bool verify = false;
std::uint64_t verified = 0, verify_mismatches = 0, verified_flipped = 0, verified_shadow = 0, verified_clipped = 0;
double since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

std::uint32_t guest_alloc(std::uint8_t* rdram, std::size_t size) {
    void* p = recomp::alloc(rdram, size);
    return p ? 0x80000000u + std::uint32_t(static_cast<std::uint8_t*>(p) - rdram) : 0;
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

// Index 0 marks pixels without a PC equivalent (beyond the map, minitiles not
// matched yet); presentation keeps the N64 pixels there. Rows are assembled in guest
// byte order (each 4-byte group reversed, as RDRAM holds them).
// The last terrain drawn, reused while the camera and the visible tiles stand still.
struct TerrainCache { int cam_x = 0, cam_y = 0, vx = 0, vy = 0; unsigned w = 0, h = 0; std::uint64_t tiles = 0;
                      std::uint32_t game_tiles = 0; std::vector<std::uint8_t> bytes; };
TerrainCache terrain_cache;
void draw_terrain(std::uint8_t* rdram, const Canvas& c, int cam_x, int cam_y, int vx, int vy) {
    const unsigned w = c.width * 2, h = c.height * 2;
    // The visible tiles' identity, so creep spreading redraws the terrain.
    const int tx0 = std::max(0, (cam_x - 2 * vx) / 32), ty0 = std::max(0, (cam_y - 2 * vy) / 32);
    const int tx1 = std::min(int(map.width), (cam_x - 2 * vx + int(w)) / 32 + 1);
    const int ty1 = std::min(int(map.height), (cam_y - 2 * vy + int(h)) / 32 + 1);
    const std::uint32_t live = gw(rdram, live_tiles), edges = gw(rdram, creep_edges);
    const bool creep = (live >> 24) == 0x80 && (edges >> 24) == 0x80 && gw(rdram, creep_edges_width) == map.width;
    std::uint64_t hash = 14695981039346656037ull;
    for (int ty = ty0; ty < ty1; ++ty)
        for (int tx = tx0; tx < tx1; ++tx) {
            const std::uint32_t pos = std::uint32_t(ty * int(map.width) + tx);
            hash = mix(hash, gh(rdram, map.game_tiles + pos * 2));
            if (creep) hash = mix(hash, std::uint32_t(gh(rdram, live + pos * 2)) << 8 | gb(rdram, edges + pos));
        }
    TerrainCache& cache = terrain_cache;
    if (cache.cam_x == cam_x && cache.cam_y == cam_y && cache.vx == vx && cache.vy == vy && cache.w == w && cache.h == h
        && cache.tiles == hash && cache.game_tiles == map.game_tiles && cache.bytes.size() == std::size_t(w) * h) {
        std::memcpy(rdram + offset(c.pixels), cache.bytes.data(), cache.bytes.size());
        return;
    }
    cache = {cam_x, cam_y, vx, vy, w, h, hash, map.game_tiles, std::move(cache.bytes)};
    cache.bytes.resize(std::size_t(w) * h);
    match_budget = 4; // full content matches per redraw (a few hundred microseconds each)
    bool complete = true;
    const std::uint32_t megas = (map.minis - map.megas) / 32;
    std::vector<std::uint8_t> row(w + 4);
    for (unsigned y = 0; y < h; ++y) {
        const int my = cam_y + int(y) - 2 * vy;
        std::fill(row.begin(), row.end(), 0);
        if (my >= 0 && my < int(map.height) * 32) {
            const unsigned ty = unsigned(my) / 32, py = unsigned(my) % 32;
            const int left = std::max(0, 2 * vx - cam_x), right = std::min(int(w), int(map.width) * 32 + 2 * vx - cam_x);
            for (int x = left; x < right;) {
                const unsigned mx = unsigned(cam_x + x - 2 * vx), tx = mx / 32, px = mx % 32;
                const unsigned n = gh(rdram, map.game_tiles + (ty * map.width + tx) * 2) & 0x7fff;
                const unsigned run = std::min(32 - px, unsigned(right - x));
                bool whole = true;
                const std::uint8_t* block = creep ? creep_megatile(gh(rdram, live + (ty * map.width + tx) * 2)) : nullptr;
                if (!block) block = n < megas ? megatile(rdram, n, whole) : nullptr;
                complete &= whole;
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
    if (creep)
        for (int ty = ty0; ty < ty1; ++ty)
            for (int tx = tx0; tx < tx1; ++tx) {
                const std::uint32_t pos = std::uint32_t(ty * int(map.width) + tx);
                const unsigned edge = gb(rdram, edges + pos);
                if (edge && (gh(rdram, map.game_tiles + pos * 2) & 0x8000))
                    draw_creep_edge(cache.bytes.data(), w, h, tx * 32 - cam_x + 2 * vx, ty * 32 - cam_y + 2 * vy, edge - 1);
            }
    if (!complete) cache.tiles = ~hash; // not kept: unmatched minitiles get matched over the next frames
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
    verify = std::filesystem::exists(parent / "hdverify", error);
    if (verify) sc64::shell::log::info("hd: hdverify, native images compared with the game's render functions");
    if (!sc64_platform_thread(load)) sc64::shell::log::warning("hd: loader thread failed, N64 graphics kept");
}

bool enabled() { return ready.load(); }

std::vector<Taken> take(const std::uint8_t*) {
    std::vector<Taken> out;
    if (!ready.load()) return out;
    std::lock_guard lock(mutex);
    const auto started = Clock::now();
    for (auto& [address, c] : canvases) {
        if (!c.dirty) continue;
        c.dirty = false;
        auto frame = std::make_shared<Frame>();
        frame->canvas = address; frame->canvas_width = c.width; frame->canvas_height = c.height;
        out.push_back({std::move(frame), c.pixels});
    }
    if (current && !current->dirty) current = nullptr; // images after the submission are not taken
    take_ms += since(started);
    return out;
}

Frames fill(const std::uint8_t* rdram, std::vector<Taken>&& taken) {
    Frames out;
    if (taken.empty()) return out;
    const auto started = Clock::now();
    for (auto& [frame, pixels] : taken) {
        const std::size_t n = std::size_t(frame->canvas_width) * frame->canvas_height * 4;
        frame->pixels.resize(n);
        std::memcpy(frame->pixels.data(), rdram + offset(pixels), n); // guest word order
        std::uint64_t hash = 14695981039346656037ull;
        for (std::uint32_t i = 0; i < frame->canvas_width * frame->canvas_height; i += 4)
            hash = mix(hash, gw(rdram, 0x80000000u + frame->canvas + i));
        frame->canvas_hash = hash;
        out.push_back(std::move(frame));
    }
    std::lock_guard lock(mutex);
    fill_ms += since(started);
    frames_taken += out.size();
    if (frames_taken % 1800 < out.size())
        sc64::shell::log::info("hd ms/frame: terrain %.2f images %.2f take %.2f fill %.2f compose %.2f (%.2f per image, %.2f CPU, %llu) | %.1f images per frame, %llu N64 fallbacks",
            terrain_ms / 1800, image_ms / 1800, take_ms / 1800, fill_ms / 1800, compose_ms / 1800,
            composed ? compose_ms / double(composed) : 0.0, composed ? compose_cpu_ms / double(composed) : 0.0,
            (unsigned long long)composed, double(images_drawn) / 1800, (unsigned long long)fallbacks),
        terrain_ms = image_ms = take_ms = fill_ms = compose_ms = compose_cpu_ms = 0, images_drawn = 0, composed = 0;
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
    const auto started = Clock::now();
    const double cpu_started = sc64_platform_thread_cpu_ms();
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
    // RDP rectangles after the background narrower than 3/4 of it (minimap, HUD,
    // cursor, dialogs; not the fog layer, drawn in full-width strips): there only
    // pixels left exactly as the canvas are HD, the rest stays as drawn even when dark.
    thread_local std::vector<std::uint8_t> overlay;
    overlay.assign(std::size_t(w) * h, 0);
    {
        bool after = false;
        for (const auto& r : list.rectangles) {
            if (&r == bg) { after = true; continue; }
            if (!after || r.target != target) continue;
            const auto rb = gfx::raster_bounds(r);
            if (rb.empty() || (rb.x1 - rb.x0) * 4 >= (bounds.x1 - bounds.x0) * 3) continue;
            // A copied background marks only its visible texels (dismissed boxes stay
            // drawn, fully transparent); anything else marks its whole rectangle.
            const gfx::Texture* source = r.texture != gfx::no_texture ? &list.textures[r.texture] : nullptr;
            const bool texels = source && source->background && r.dsdx == 1024 && r.dtdy == 1024
                                && source->pixels.size() == std::size_t(source->width) * source->height;
            for (int y = std::max(0, rb.y0); y < std::min(int(h), rb.y1); ++y)
                for (int x = std::max(0, rb.x0); x < std::min(int(w), rb.x1); ++x) {
                    if (texels) {
                        const int tu = (r.s >> 5) + x - r.x0 / 4, tv = (r.t >> 5) + y - r.y0 / 4;
                        if (tu < 0 || tv < 0 || tu >= int(source->width) || tv >= int(source->height)
                            || source->pixels[std::size_t(tv) * source->width + unsigned(tu)].a == 0) continue;
                    }
                    overlay[std::size_t(y) * w + unsigned(x)] = 1;
                }
        }
    }
    // Pass 1: each canvas pixel's fog alpha (0..256), or -1 where the RDP drew over
    // the canvas (HUD, cursor) or outside the background.
    thread_local std::vector<std::int16_t> alpha;
    alpha.assign(std::size_t(w) * h, -1);
    const int pc[3] = {fog_r, fog_g, fog_b};
    // Columns inside the background and the canvas, the same for every row.
    const int xa = std::max({0, bounds.x0, x0 - s0}), xb = std::min({int(w), bounds.x1, int(frame->canvas_width) - s0 + x0,
                                                                   int(tex.width) - s0 + x0});
    for (unsigned y = 0; y < h; ++y) {
        const int v = t0 + int(y) - y0;
        if (int(y) < bounds.y0 || int(y) >= bounds.y1 || v < 0 || v >= int(frame->canvas_height) || v >= int(tex.height))
            continue;
        const std::uint8_t* row = canvas + std::size_t(v) * frame->canvas_width + (s0 - x0);
        const std::uint32_t* drawn = in + std::size_t(y) * w;
        const std::uint8_t* over = overlay.data() + std::size_t(y) * w;
        std::int16_t* ar = alpha.data() + std::size_t(y) * w;
        for (int x = xa; x < xb; ++x) {
            const std::uint32_t pixel = drawn[x] | 0xff000000u, b = palette[row[x]];
            if (pixel == b) { ar[x] = 0; continue; }
            if (over[x]) continue;
            if (pixel == fog_pixel) { ar[x] = 256; continue; }
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
            if (fits) ar[x] = std::int16_t(value);
        }
    }
    // Pass 2: the RDP dithers the fog; a 3x3 average over the blended pixels
    // smooths its edges at twice the resolution: three-pixel row sums and counts,
    // then three rows. flat marks pixels whose whole neighbourhood has their alpha
    // (no blending needed). Branch-free so it vectorizes; the average is
    // sum * ceil(65536 / count) >> 16 (= sum / count), 7282 for a full neighbourhood,
    // and the rare partial ones are fixed after.
    const unsigned pw = w + 2;
    thread_local std::vector<std::int16_t> row_sums, smooth;
    thread_local std::vector<std::uint8_t> row_counts, flat;
    row_sums.assign(std::size_t(pw) * (h + 2), 0); row_counts.assign(std::size_t(pw) * (h + 2), 0);
    smooth.assign(std::size_t(pw) * (h + 2), -1); flat.assign(std::size_t(w) * h, 0);
    for (unsigned y = 0; y < h; ++y) {
        const std::int16_t* ar = alpha.data() + std::size_t(y) * w;
        std::int16_t* rs = row_sums.data() + std::size_t(y + 1) * pw + 1;
        std::uint8_t* rc = row_counts.data() + std::size_t(y + 1) * pw + 1;
        for (unsigned x = 0; x < w; ++x) { rs[x] = std::int16_t(std::max<int>(ar[x], 0)); rc[x] = ar[x] >= 0; }
        int previous_sum = 0, previous_count = 0; // the pixel left of x, before x is overwritten
        for (unsigned x = 0; x < w; ++x) {
            const int here_sum = rs[x], here_count = rc[x];
            const int next_sum = x + 1 < w ? std::max<int>(ar[x + 1], 0) : 0, next_count = x + 1 < w && ar[x + 1] >= 0;
            rs[x] = std::int16_t(previous_sum + here_sum + next_sum);
            rc[x] = std::uint8_t(previous_count + here_count + next_count);
            previous_sum = here_sum; previous_count = here_count;
        }
    }
    static const std::array<std::int32_t, 10> reciprocal = [] {
        std::array<std::int32_t, 10> r{};
        for (int i = 1; i < 10; ++i) r[std::size_t(i)] = (65536 + i - 1) / i;
        return r;
    }();
    for (unsigned y = 0; y < h; ++y) {
        const std::int16_t* ar = alpha.data() + std::size_t(y) * w;
        const std::int16_t* s0r = row_sums.data() + std::size_t(y) * pw + 1;
        const std::uint8_t* c0r = row_counts.data() + std::size_t(y) * pw + 1;
        std::int16_t* out_s = smooth.data() + std::size_t(y + 1) * pw + 1;
        std::uint8_t* out_f = flat.data() + std::size_t(y) * w;
        for (unsigned x = 0; x < w; ++x) {
            const std::int16_t a = ar[x];
            const std::uint16_t sum = std::uint16_t(s0r[x] + s0r[x + pw] + s0r[x + 2 * pw]);
            const std::uint8_t count = std::uint8_t(c0r[x] + c0r[x + pw] + c0r[x + 2 * pw]);
            out_s[x] = a < 0 ? std::int16_t(-1) : std::int16_t((std::uint32_t(sum) * 7282u) >> 16);
            out_f[x] = (count == 9) & (sum == 9 * a);
        }
        for (unsigned x = 0; x < w; ++x) {
            const int count = c0r[x] + c0r[x + pw] + c0r[x + 2 * pw];
            if (count == 9 || ar[x] < 0) continue;
            const int sum = s0r[x] + s0r[x + pw] + s0r[x + 2 * pw];
            out_s[x] = std::int16_t((sum * reciprocal[std::size_t(count)]) >> 16);
        }
    }
    // Pass 3: each HD pixel with the alpha interpolated toward its neighbours. The
    // two HD rows under a canvas row are read in host byte order.
    thread_local std::vector<std::uint8_t> hd_rows;
    hd_rows.resize(std::size_t(fw) * 2);
    for (unsigned y = 0; y < h; ++y) {
        std::uint32_t* o0 = out + std::size_t(y) * 2 * ow;
        std::uint32_t* o1 = o0 + ow;
        const std::uint32_t* f = in + std::size_t(y) * w;
        const int v = t0 + int(y) - y0;
        const std::int16_t* s = smooth.data() + std::size_t(y + 1) * pw + 1;
        const std::uint8_t* fl = flat.data() + std::size_t(y) * w;
        const bool canvas_row = v >= 0 && v < int(frame->canvas_height);
        if (canvas_row) {
            const auto* src = reinterpret_cast<const std::uint32_t*>(frame->pixels.data() + std::size_t(v) * 2 * fw);
            auto* dst = reinterpret_cast<std::uint32_t*>(hd_rows.data());
            for (unsigned i = 0; i < fw * 2 / 4; ++i) dst[i] = __builtin_bswap32(src[i]);
        }
        const std::uint8_t* h0 = hd_rows.data() + std::ptrdiff_t(s0 - x0) * 2;
        const std::uint8_t* h1 = h0 + fw;
        for (unsigned x = 0; x < w; ++x) {
            const std::uint32_t pixel = f[x] | 0xff000000u;
            const int a = s[x];
            if (a < 0 || !canvas_row) { o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = pixel; continue; }
            const std::uint8_t q[4] = {h0[2 * x], h0[2 * x + 1], h1[2 * x], h1[2 * x + 1]};
            std::uint32_t* const o[4] = {&o0[2 * x], &o0[2 * x + 1], &o1[2 * x], &o1[2 * x + 1]};
            if (fl[x] && a == 0) { // clear of fog: the HD pixels as they are
                for (int i = 0; i < 4; ++i) *o[i] = q[i] ? palette[q[i]] : pixel;
                continue;
            }
            if (fl[x] && a == 256) { // deep in the fog
                for (int i = 0; i < 4; ++i) *o[i] = q[i] ? fog_pixel : pixel;
                continue;
            }
            auto near = [&](int dx, int dy) { const int n = s[int(x) + dx + dy * int(pw)]; return n < 0 ? a : n; };
            const int l = near(-1, 0), r = near(1, 0), up = near(0, -1), dn = near(0, 1);
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
    {
        std::lock_guard lock(mutex);
        compose_ms += since(started);
        compose_cpu_ms += sc64_platform_thread_cpu_ms() - cpu_started;
        ++composed;
    }
    return bytes;
}
}

namespace sc64::hd {
namespace {
struct Job {
    std::shared_ptr<const gfx::DrawList> list;
    Frames frames;
    std::vector<std::uint8_t> rgba;
    gfx::Image target;
    unsigned height = 0;
    std::shared_ptr<Pending> pending;
};
std::mutex job_mutex;
std::condition_variable job_ready;
std::deque<Job> jobs;
bool worker_started = false;
void worker() {
    sc64_platform_heavy_begin(); // off core 0 (core 3 or 2 on the Switch), for good
    for (;;) {
        Job job;
        {
            std::unique_lock lock(job_mutex);
            job_ready.wait(lock, [] { return !jobs.empty(); });
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        if (auto image = compose(*job.list, job.frames, job.rgba, job.target, job.height))
            job.pending->result = std::make_shared<const std::vector<std::uint8_t>>(std::move(*image));
        job.pending->done.store(true, std::memory_order_release);
    }
}
}

std::shared_ptr<Pending> compose_async(std::shared_ptr<const gfx::DrawList> list, Frames frames,
                                       std::vector<std::uint8_t> rgba, gfx::Image target, unsigned height) {
    auto pending = std::make_shared<Pending>();
    std::lock_guard lock(job_mutex);
    if (!worker_started) worker_started = sc64_platform_thread(worker);
    if (!worker_started) { // no thread: compose here
        if (auto image = compose(*list, frames, rgba, target, height))
            pending->result = std::make_shared<const std::vector<std::uint8_t>>(std::move(*image));
        pending->done = true;
        return pending;
    }
    // A frame behind by two is not shown any more: drop it unfinished.
    while (jobs.size() >= 2) { jobs.front().pending->done = true; jobs.pop_front(); }
    jobs.push_back({std::move(list), std::move(frames), std::move(rgba), target, height, pending});
    job_ready.notify_one();
    return pending;
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
    if (!map_w || !map_h || map_w > 256 || map_h > 256) return;
    if (gw(rdram, tile_array) != map.game_tiles || gw(rdram, n64_megatiles) != map.megas || map_w != map.width
        || map_h != map.height)
        locate_map(rdram, map_w, map_h);
    if (!map.found) return;
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

namespace {
// A PC frame through one of the common render functions, as the game draws it:
// the clip rectangle (rx, ry, rw, rh) of the frame (columns counted from the screen
// left when flipped) at canvas position (x, y). pixel(col, row) is that canvas byte.
template <class Pixel>
void draw_native(const Grp& grp, const GrpFrame& f, bool flipped, bool shadow, const std::uint8_t* table,
                 int rx, int ry, int rw, int rh, Pixel&& pixel) {
    const auto& d = grp.data;
    for (int row = 0; row < rh; ++row) {
        const unsigned line = unsigned(ry + row);
        std::size_t p = f.offset + (d[f.offset + line * 2] | d[f.offset + line * 2 + 1] << 8);
        for (int lx = 0; lx < int(f.w) && p < d.size();) {
            const unsigned c = d[p++];
            int run = int(c & 0x7f);
            if (c & 0x80) { lx += run; continue; }
            const bool fill = c & 0x40;
            if (fill) run = int(c & 0x3f);
            for (int k = 0; k < run; ++k) {
                const int fx = lx + k, col = (flipped ? int(f.w) - 1 - fx : fx) - rx;
                if (col < 0 || col >= rw) continue;
                std::uint8_t& out = pixel(col, row);
                out = table[shadow ? out : (fill ? d[p] : d[p + std::size_t(k)])];
            }
            p += fill ? 1 : std::size_t(run);
            lx += run;
        }
    }
}
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
    Grp* grp = id < 999 && image_grp[id] && image_grp[id] < grps.size() ? grps[image_grp[id]].get() : nullptr;
    const unsigned frame_index = (gw(rdram, image + 0xC) >> 6) & 0xFFF;
    if (!grp || frame_index >= grp->frames.size() || (sprite >> 24) != 0x80) { begin_fallback(rdram, ctx); return; }
    GrpFrame& f = grp->frames[frame_index];
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
    const bool normal = render == render_normal || render == render_normal_flipped;
    const bool shadow = render == render_shadow || render == render_shadow_flipped;
    std::vector<std::uint8_t> expected;
    if (normal || shadow) {
        std::array<std::uint8_t, 256> table;
        const std::uint32_t source = shadow ? gw(rdram, shadow_table) + 0x1200 : remap_table;
        for (unsigned i = 0; i < 256; ++i) table[i] = gb(rdram, source + i);
        const bool mirrored = render == render_normal_flipped || render == render_shadow_flipped;
        const std::uint32_t base = c.pixels + std::uint32_t((y + ry) * cw + x + rx);
        if (verify) {
            expected.resize(std::size_t(rw) * rh);
            for (int row = 0; row < rh; ++row)
                for (int col = 0; col < rw; ++col)
                    expected[std::size_t(row) * rw + col] = gb(rdram, base + std::uint32_t(row * cw + col));
            draw_native(*grp, f, mirrored, shadow, table.data(), rx, ry, rw, rh,
                        [&](int col, int row) -> std::uint8_t& { return expected[std::size_t(row) * rw + col]; });
        } else {
            draw_native(*grp, f, mirrored, shadow, table.data(), rx, ry, rw, rh, [&](int col, int row) -> std::uint8_t& {
                return rdram[offset((base + std::uint32_t(row * cw + col)) ^ 3)];
            });
            ++images_drawn;
            image_ms += since(started);
            return;
        }
    }
    // The PC frame in the game's own layout: header, then its line table and lines.
    sw(rdram, scratch, std::uint32_t(f.x) << 24 | std::uint32_t(f.y) << 16 | std::uint32_t(f.w) << 8 | f.h);
    sw(rdram, scratch + 4, scratch + 8);
    // The frame's bytes in guest word order, made once per frame.
    if (f.swapped.empty()) {
        f.swapped.assign((f.size + 3) & ~3u, 0);
        for (std::uint32_t i = 0; i < f.size; ++i) f.swapped[i ^ 3] = grp->data[f.offset + i];
    }
    std::memcpy(rdram + offset(scratch + 8), f.swapped.data(), f.swapped.size());
    sh(rdram, rect, std::uint16_t(rx)); sh(rdram, rect + 2, std::uint16_t(ry));
    sh(rdram, rect + 4, std::uint16_t(rw)); sh(rdram, rect + 6, std::uint16_t(rh));
    call_render(rdram, ctx, c, render, x + rx, y + ry, scratch, gw(rdram, image + 0x2C));
    if (!expected.empty()) {
        std::uint64_t different = 0;
        for (int row = 0; row < rh; ++row)
            for (int col = 0; col < rw; ++col)
                different += gb(rdram, c.pixels + std::uint32_t((y + ry + row) * cw + x + rx + col))
                             != expected[std::size_t(row) * rw + col];
        ++verified;
        verified_flipped += render == render_normal_flipped || render == render_shadow_flipped;
        verified_shadow += shadow;
        verified_clipped += rx || ry || rw != int(f.w) || rh != int(f.h);
        if (different && verify_mismatches++ < 20)
            sc64::shell::log::warning("hdverify: render %08x image %u frame %u rect %d,%d %dx%d: %llu pixels differ",
                                      render, id, frame_index, rx, ry, rw, rh, (unsigned long long)different);
        if (verified % 5000 == 0)
            sc64::shell::log::info("hdverify: %llu images (%llu flipped, %llu shadows, %llu clipped), %llu with differences",
                                   (unsigned long long)verified, (unsigned long long)verified_flipped,
                                   (unsigned long long)verified_shadow, (unsigned long long)verified_clipped,
                                   (unsigned long long)verify_mismatches);
    }
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

