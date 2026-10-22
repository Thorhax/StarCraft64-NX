#pragma once
#include <array>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace sc64::gfx {
struct Color {
    int r = 0, g = 0, b = 0, a = 0;
    bool operator==(const Color&) const = default;
};
struct Tile {
    unsigned format = 0, size = 0, line = 0, tmem = 0, palette = 0;
    unsigned cms = 0, cmt = 0, masks = 0, maskt = 0, shifts = 0, shiftt = 0;
    int sl = 0, tl = 0, sh = 0, th = 0; // unsigned 10.2 tile limits
    bool operator==(const Tile&) const = default;
};
struct Image { unsigned address = 0, width = 0, format = 0, size = 0; bool operator==(const Image&) const = default; };
struct Scissor { int x0 = 0, y0 = 0, x1 = 4096, y1 = 4096; unsigned field = 0; bool operator==(const Scissor&) const = default; };
struct State {
    std::uint64_t combine = 0;
    std::uint32_t other_hi = 0, other_lo = 0;
    Color primitive{255,255,255,255}, environment{}, blend{}, fog{};
    std::uint32_t fill = 0; // SETFILLCOLOR word: two RGBA16 pixels (even x = high half)
    Scissor scissor;
    bool operator==(const State&) const = default;
};
class Tmem {
public:
    std::array<std::uint8_t,4096> bytes{};
    void load_tile(std::span<const std::uint8_t> ram, const Image& image,
                   const Tile& tile, int sl, int tl, int sh, int th);
    void load_tlut(std::span<const std::uint8_t> ram, const Image& image,
                   unsigned head, unsigned count, int sl = 0, int tl = 0);
    Color fetch(const Tile& tile, int s, int t, unsigned tlut_mode) const;
    std::uint16_t palette(unsigned index) const;
};

struct Texture {
    std::uint64_t key = 0;
    unsigned width = 0, height = 0;
    Tile tile;
    bool background = false;
    Tmem memory;
    unsigned tlut_mode = 0;
    std::vector<Color> pixels;
};
constexpr unsigned no_texture = 0xffffffffu;
struct Rectangle {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // screen signed 10.2, exclusive ends
    int s = 0, t = 0; // signed 10.5 texture coordinates
    int dsdx = 1024, dtdy = 1024; // signed 5.10, COPY already divided by four
    bool flip = false;
    unsigned texture = 0; // no_texture for FILL rectangles
    Image target;
    State state;
    std::uint32_t source_pc = 0;
};
// Integer pixels a rectangle may write: its edges intersected with the
// quarter-pixel scissor and the target width. Ends are exclusive.
struct PixelBounds {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
PixelBounds raster_bounds(const Rectangle& rect);
struct Batch { unsigned first = 0, count = 0; };
struct FrameIdentity {
    std::uint64_t task = 0, field = 0;
    std::array<std::uint32_t,14> vi{};
    std::string source_ram_sha256;
};
struct DrawList {
    static constexpr unsigned schema = 1;
    FrameIdentity identity;
    // Each packet owns all bytes; a submitter cannot recycle its RAM snapshot.
    std::vector<std::uint8_t> initial_ram;
    std::vector<Texture> textures;
    std::vector<Rectangle> rectangles;
    std::array<std::uint64_t,256> opcodes{};
    std::vector<std::string> limitations;
};
Color rgba16(std::uint16_t value);
std::uint16_t encode_rgba16(Color color);
Color source_texel(std::span<const std::uint8_t> ram, const Image& image,
                   unsigned x, unsigned y, unsigned palette, const Tmem& tmem, unsigned tlut_mode);
// The only accepted microcode is the Stage0 S2DEX2 xbus2.08 identity. The caller
// checks its captured SHA-1 before invoking this parser. Unknown operations fail.
DrawList interpret_gbi(std::span<const std::uint8_t> ram, unsigned root,
                       const std::function<void()>& checkpoint = {});
// Same, taking ownership of an 8 MiB snapshot as DrawList::initial_ram (no copy).
DrawList interpret_gbi(std::vector<std::uint8_t>&& ram, unsigned root,
                       const std::function<void()>& checkpoint = {});
DrawList interpret_rdp(std::span<const std::uint8_t> ram, std::span<const std::uint64_t> words);
struct SoftwareResult {
    std::vector<std::uint8_t> ram;
    std::uint64_t written_pixels = 0;
    struct Write { std::uint32_t address=0, length=0; };
    std::vector<Write> writes;
};
// Persistent helper threads for rasterization. Helpers only touch owned
// DrawList/SoftwareResult data; guest checkpoints stay on the calling thread.
class RasterPool {
public:
    explicit RasterPool(unsigned helpers);
    ~RasterPool();
    RasterPool(const RasterPool&) = delete;
    RasterPool& operator=(const RasterPool&) = delete;
    unsigned helpers() const { return unsigned(threads_.size()); }
    // Run job(0..count-1) on the helpers and the calling thread. The caller
    // invokes idle() between its jobs and while waiting. If idle() throws,
    // unstarted jobs are cancelled and running ones finish before rethrowing.
    // job must not throw.
    void run(unsigned count, const std::function<void(unsigned)>& job, const std::function<void()>& idle);
    // Helpers for this host: leave cores for the engine render and audio threads.
    static unsigned default_helpers();
private:
    void work();
    void drain(const std::function<void()>& idle);
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    const std::function<void(unsigned)>* job_ = nullptr;
    unsigned count_ = 0, busy_ = 0;
    std::atomic<unsigned> next_{0};
    std::uint64_t generation_ = 0;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};
// Raster work below this many candidate pixels stays on the calling thread.
inline constexpr std::uint64_t parallel_raster_min_pixels = 4096;
// Rasterize in command order. With a pool, work is split by RDRAM address
// ranges: every pixel reads and writes only its own address, so each range
// replays all rectangles in order and the result is byte-identical to the
// serial path. writes are canonical: sorted, merged byte ranges.
//
// recycled: an optional buffer of initial_ram's size from an earlier result.
// Then only target_extents() are refreshed from initial_ram (no full 8 MiB
// copy) and bytes outside them are stale; see target_extents for who may read.
SoftwareResult render_software(const DrawList& list, const std::function<void()>& checkpoint = {},
                               RasterPool* pool = nullptr, std::vector<std::uint8_t> recycled = {});
// RDRAM bytes of a result that consumers read: each distinct target from its
// address down to its lowest scissor row (the extent Native::tick exports),
// clipped to initial_ram. Every rasterized pixel and RDP store lies inside them.
std::vector<SoftwareResult::Write> target_extents(const DrawList& list);
// Fill ram as the starting image of a render result: a full copy of
// initial_ram, or, when ram already has its size (a recycled buffer), a copy of
// target_extents() only.
void prepare_result_ram(const DrawList& list, std::vector<std::uint8_t>& ram);
std::vector<Batch> adjacent_batches(const DrawList& list);
std::vector<std::uint8_t> target_rgba(std::span<const std::uint8_t> ram, Image image, unsigned height);
// Native target comparison, prior to VI filtering; alpha is guest coverage, not
// ordinary host transparency and is deliberately kept separate from RGB metrics.
}
