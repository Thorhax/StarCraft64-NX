// libnx application shell: framebuffer video, HID pads and touch screen.
// Mirrors native/shell/app.cpp (SDL3) with the same Native/script objects; the
// guest runtime, raster helpers and audio keep their own threads.
#include "audio.hpp"
#include "log.hpp"
#include "native.hpp"
#include "platform.hpp"
#include "voices.hpp"
#include "hd.hpp"
#if defined(SC64_PORTRAITS)
#include "portraits.hpp"
#endif
#if defined(SC64_MUSIC)
#include "music.hpp"
#endif
#include "widescreen.hpp"
#if defined(SC64_MOVIES)
#include "movie.hpp"
#endif
#include "scripts.hpp"
#include "pointer.hpp"
#include <switch.h>
#if defined(SC64_SWITCH_GL)
#include "gl_device.hpp"
#endif
#include <pthread.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// libnx gives every std::thread a 128 KiB stack (newlib __syscall_thread_create);
// the recompiled game overflowed it (crash.txt: write fault at sp). Linker-wrapped
// so every pthread without an explicit size gets 2 MiB, like Android's ~1 MiB+.
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                     void* (*start)(void*), void* arg) {
    constexpr std::size_t stack = 2u << 20;
    std::size_t requested = 0;
    if (attr) pthread_attr_getstacksize(attr, &requested);
    if (requested >= stack) return __real_pthread_create(thread, attr, start, arg);
    pthread_attr_t local;
    if (attr) local = *attr; else pthread_attr_init(&local);
    pthread_attr_setstacksize(&local, stack);
    const int result = __real_pthread_create(thread, &local, start, arg);
    if (!attr) pthread_attr_destroy(&local);
    return result;
}

// Crash diagnostics written by the app itself (no dependency on Atmosphere's
// creport): CPU exceptions via the libnx hook, uncaught C++ exceptions via
// std::terminate. Offsets are relative to the module base = addresses in the ELF.
extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    MemoryInfo info{};
    u32 page = 0;
    svcQueryMemory(&info, &page, reinterpret_cast<u64>(&__libnx_exception_handler));
    const u64 base = info.addr;
    auto offset = [base](u64 address) { return address >= base ? address - base : 0; };
    if (FILE* f = std::fopen("sdmc:/switch/StarCraft64/crash.txt", "w")) {
        std::fprintf(f, "CPU exception desc=0x%x esr=0x%x\n", ctx->error_desc, ctx->esr);
        std::fprintf(f, "pc=0x%016lx (elf 0x%lx)\nlr=0x%016lx (elf 0x%lx)\n",
                     ctx->pc.x, offset(ctx->pc.x), ctx->lr.x, offset(ctx->lr.x));
        std::fprintf(f, "sp=0x%016lx fp=0x%016lx far=0x%016lx base=0x%016lx\n",
                     ctx->sp.x, ctx->fp.x, ctx->far.x, base);
        for (int i = 0; i < 29; ++i) std::fprintf(f, "x%d=0x%016lx\n", i, ctx->cpu_gprs[i].x);
        // Frame-pointer chain: [fp] = previous fp, [fp + 8] = return address.
        u64 fp = ctx->fp.x;
        for (int depth = 0; depth < 32 && fp && !(fp & 0xf); ++depth) {
            const u64* frame = reinterpret_cast<const u64*>(fp);
            const u64 next = frame[0], ret = frame[1];
            std::fprintf(f, "frame %d ret=0x%016lx (elf 0x%lx)\n", depth, ret, offset(ret));
            if (next <= fp || next - fp > 0x100000) break;
            fp = next;
        }
        std::fclose(f);
    }
    // Returning hands the exception back to the system (error screen / creport).
}
}

namespace {
using namespace sc64::shell;
constexpr unsigned screen_w = 1280, screen_h = 720;

void terminate_handler() {
    std::string what = "unknown";
    if (auto current = std::current_exception()) {
        try { std::rethrow_exception(current); }
        catch (const std::exception& error) { what = error.what(); }
        catch (...) { what = "non-std exception"; }
    }
    if (FILE* f = std::fopen("sdmc:/switch/StarCraft64/crash.txt", "w")) {
        std::fprintf(f, "Uncaught C++ exception: %s\n", what.c_str());
        // Call chain into std::terminate (frame pointers), as ELF offsets.
        MemoryInfo info{};
        u32 page = 0;
        svcQueryMemory(&info, &page, reinterpret_cast<u64>(&terminate_handler));
        const u64 base = info.addr;
        u64 fp = reinterpret_cast<u64>(__builtin_frame_address(0));
        for (int depth = 0; depth < 32 && fp && !(fp & 0xf); ++depth) {
            const u64* frame = reinterpret_cast<const u64*>(fp);
            const u64 next = frame[0], ret = frame[1];
            std::fprintf(f, "frame %d ret=0x%016lx (elf 0x%lx)\n", depth, ret, ret >= base ? ret - base : 0);
            if (next <= fp || next - fp > 0x100000) break;
            fp = next;
        }
        std::fclose(f);
    }
    std::fflush(stdout); std::fflush(stderr);
    std::abort();
}
constexpr const char* storage_dir = "sdmc:/switch/StarCraft64";
// The game's mix is quiet (menu music about -33 dBFS RMS, peaks -18 dBFS) next
// to the PC voices/movies (-17/-13 dBFS RMS) and other Switch software: x4
// (+12 dB) by default. sdmc:/switch/StarCraft64/volume holds another factor
// (1 = original level).
constexpr float default_volume = 4.0f;

// An empty switch file in the storage folder; Windows often saves it as name.txt.
bool option_file(const char* name) {
    std::error_code missing;
    const std::string path = std::string(storage_dir) + "/" + name;
    return std::filesystem::exists(path, missing) || std::filesystem::exists(path + ".txt", missing);
}

// Main loop timing, logged every 600 loops: Native::tick (presentation selection and
// HD composition), the framebuffer scaling, and the wait for a free framebuffer.
struct LoopProfile { u64 tick = 0, scale = 0, wait = 0, since = 0; unsigned loops = 0; };
LoopProfile loop_profile;

// The software mouse pointer, drawn 2x: '#' outline, '.' fill.
constexpr unsigned pointer_scale = 2;
constexpr std::array<const char*, 17> pointer_shape = {{
    "#", "##", "#.#", "#..#", "#...#", "#....#", "#.....#", "#......#", "#.......#",
    "#........#", "#.....#####", "#..#..#", "#.# #..#", "##  #..#", "#    #..#", "     #..#", "      ##",
}};

// Original raster fitted with nearest sampling (320x240 and 640x480 -> 960x720),
// centred: the same geometry as the shell's Fit viewport, so touch mapping matches.
// The framebuffer stays block linear: lines are scaled 8 at a time into a strip
// that stays in cache and swizzled straight into it, instead of a linear copy
// that framebufferEnd would read back and swizzle (3.7 MB more each way).
class FrameVideo {
public:
    void create() {
        if (R_FAILED(framebufferCreate(&fb_, nwindowGetDefault(), screen_w, screen_h, PIXEL_FORMAT_RGBA_8888, 2)))
            throw std::runtime_error("framebufferCreate failed");
        ready_ = true;
    }
    void destroy() { if (ready_) framebufferClose(&fb_); ready_ = false; }
    void upload(const Native::Upload& image) { image_ = image; }
    // Begin/End also presents and paces to the display (double buffered).
    void draw(bool visible) {
        u32 stride = 0;
        const u64 wait_start = armGetSystemTick();
        auto* base = static_cast<std::uint8_t*>(framebufferBegin(&fb_, &stride));
        const u64 scale_start = armGetSystemTick();
        loop_profile.wait += armTicksToNs(scale_start - wait_start);
        // The scaling runs off core 0, which the Gfx thread needs for the game's next task.
        sc64_platform_heavy_begin();
        struct Back { ~Back() { sc64_platform_heavy_end(); } } back;
        struct Scale { u64 start; ~Scale() { loop_profile.scale += armTicksToNs(armGetSystemTick() - start); } } scale{scale_start};
        const bool shown = visible && image_ && image_->rgba && image_->width && image_->height
                           && image_->rgba->size() >= std::size_t(image_->width) * image_->height * 4;
        unsigned w = 0, h = 0, dw = 0, dh = 0, ox = 0, oy = 0;
        const std::uint32_t* src = nullptr;
        bool linear = false;
        if (shown) {
            w = image_->width; h = image_->height;
            dw = screen_w; dh = unsigned(std::uint64_t(screen_w) * h / w);
            if (dh > screen_h) { dh = screen_h; dw = unsigned(std::uint64_t(screen_h) * w / h); }
            ox = (screen_w - dw) / 2; oy = (screen_h - dh) / 2;
            src = reinterpret_cast<const std::uint32_t*>(image_->rgba->data());
            linear = image_->smooth && dw * 2 == w * 3 && dh * 2 == h * 3; // HD missions: exact 3:2 linear
            if (!linear && (xmap_.size() != dw || xmap_source_ != w)) {
                xmap_.resize(dw);
                for (unsigned x = 0; x < dw; ++x) xmap_[x] = unsigned(std::uint64_t(x) * w / dw);
                xmap_source_ = w;
            }
            if (linear) { widened_[0].resize(dw); widened_[1].resize(dw); cached_[0] = cached_[1] = ~0u; }
        }
        unsigned previous = ~0u; // nearest: the source row of the line above
        for (unsigned y0 = 0; y0 < screen_h; y0 += 8) {
            for (unsigned k = 0; k < 8; ++k) {
                const unsigned y = y0 + k;
                std::uint32_t* line = strip_ + std::size_t(k) * screen_w;
                if (!shown || y < oy || y >= oy + dh) {
                    std::memset(line, 0, screen_w * 4); previous = ~0u; continue;
                }
                std::memset(line, 0, ox * 4);
                std::memset(line + ox + dw, 0, std::size_t(screen_w - ox - dw) * 4);
                std::uint32_t* out = line + ox;
                const unsigned ly = y - oy;
                if (linear) {
                    // Three output lines for every two source lines: the outer ones are the
                    // widened source lines, the middle one their mean.
                    const unsigned q = ly / 3, r = ly % 3;
                    if (r == 1) {
                        const std::uint32_t* a = widened(src, w, 2 * q, 0);
                        const std::uint32_t* b = widened(src, w, 2 * q + 1, 1);
                        for (unsigned x = 0; x < dw; ++x) out[x] = mean(a[x], b[x]);
                    } else {
                        const std::uint32_t* a = widened(src, w, 2 * q + (r == 2), r == 2);
                        std::memcpy(out, a, std::size_t(dw) * 4);
                    }
                    continue;
                }
                const unsigned sy = unsigned(std::uint64_t(ly) * h / dh);
                if (sy == previous && k) { // repeated source row: copy the line above
                    std::memcpy(out, line - screen_w + ox, std::size_t(dw) * 4);
                    continue;
                }
                previous = sy;
                const auto* in = src + std::size_t(sy) * w;
                for (unsigned x = 0; x < dw; ++x) out[x] = in[xmap_[x]] | 0xff000000u; // RGBA32, opaque
            }
            pointer(y0);
            swizzle(base, stride, y0);
        }
        framebufferEnd(&fb_);
    }
private:
    // The mouse pointer over the strip's 8 lines, unless the game cursor stands in for it.
    void pointer(unsigned y0) {
        const auto& p = switch_pointer();
        if (!p.shown || p.game_cursor) return;
        const int px = int(p.x), py = int(p.y);
        for (unsigned k = 0; k < 8; ++k) {
            const int row = (int(y0 + k) - py) / int(pointer_scale);
            if (int(y0 + k) < py || row >= int(pointer_shape.size())) continue;
            std::uint32_t* line = strip_ + std::size_t(k) * screen_w;
            const char* shape = pointer_shape[row];
            for (int i = 0; shape[i]; ++i) {
                if (shape[i] == ' ') continue;
                const std::uint32_t colour = shape[i] == '#' ? 0xff000000u : 0xffffffffu;
                for (unsigned d = 0; d < pointer_scale; ++d) {
                    const int x = px + i * int(pointer_scale) + int(d);
                    if (x >= 0 && x < int(screen_w)) line[x] = colour;
                }
            }
        }
    }
    static std::uint32_t mean(std::uint32_t a, std::uint32_t b) { return ((a & 0xfefefefeu) >> 1) + ((b & 0xfefefefeu) >> 1); }
    // Source line sy widened 2 -> 3 (outer pixels copied, the middle one their mean), cached per slot.
    const std::uint32_t* widened(const std::uint32_t* src, unsigned w, unsigned sy, unsigned slot) {
        std::uint32_t* out = widened_[slot].data();
        if (cached_[slot] == sy) return out;
        cached_[slot] = sy;
        const std::uint32_t* in = src + std::size_t(sy) * w;
        for (unsigned x = 0; x + 1 < w; x += 2) {
            const std::uint32_t a = in[x] | 0xff000000u, b = in[x + 1] | 0xff000000u;
            out[0] = a; out[1] = mean(a, b); out[2] = b; out += 3;
        }
        return widened_[slot].data();
    }
    // The 8 strip lines into their GOB row of the block-linear framebuffer: GOBs are
    // 64 bytes x 8 lines, 16 GOBs tall per block, 16-byte units in 16Bx2 order (libnx
    // framebuffer.c, _convertToBlocklinear).
    void swizzle(std::uint8_t* base, u32 stride, unsigned y0) const {
        const unsigned columns = stride / 64, block = y0 / 128, gob_y = (y0 / 8) % 16;
        const auto* in = reinterpret_cast<const std::uint8_t*>(strip_);
        for (unsigned column = 0; column < columns; ++column) {
            auto* gob = base + ((std::size_t(block) * columns + column) * 16 + gob_y) * 512;
            const std::uint8_t* at = in + column * 64;
            for (unsigned i = 0; i < 32; ++i) {
                const unsigned y = ((i >> 1) & 6) | (i & 1), x = ((i << 3) & 0x10) | ((i << 1) & 0x20);
                std::memcpy(gob + i * 16, at + y * screen_w * 4 + x, 16);
            }
        }
    }
    Framebuffer fb_{};
    bool ready_ = false;
    std::optional<Native::Upload> image_;
    std::vector<unsigned> xmap_;
    unsigned xmap_source_ = 0;
    std::vector<std::uint32_t> widened_[2];
    unsigned cached_[2] = {~0u, ~0u};
    alignas(64) std::uint32_t strip_[8 * screen_w] = {};
};

struct ButtonBinding { u64 button; const char* action; };
constexpr std::array<ButtonBinding, 12> button_bindings = {{
    {HidNpadButton_A, "a"}, {HidNpadButton_B, "b"}, {HidNpadButton_Plus, "start"},
    {HidNpadButton_L, "l"}, {HidNpadButton_R, "r"}, {HidNpadButton_ZL, "z"}, {HidNpadButton_ZR, "z"},
    {HidNpadButton_Left, "dleft"}, {HidNpadButton_Right, "dright"}, {HidNpadButton_Up, "dup"},
    {HidNpadButton_Down, "ddown"}, {HidNpadButton_Minus, "pointer_commands"},
}};
constexpr double analog_press = 0.5;
// A USB keyboard: the desktop shell's keys (native/shell/app.cpp), sent without a
// gamepad id so the arrows scroll the map as on PC; F10 opens the game menu.
struct KeyBinding { HidKeyboardKey key; const char* action; };
constexpr std::array<KeyBinding, 30> key_bindings = {{
    {HidKeyboardKey_X, "a"}, {HidKeyboardKey_C, "b"}, {HidKeyboardKey_Escape, "escape_back"},
    {HidKeyboardKey_Return, "start"}, {HidKeyboardKey_NumPadEnter, "start"}, {HidKeyboardKey_F10, "start"},
    {HidKeyboardKey_Z, "z"}, {HidKeyboardKey_Q, "l"}, {HidKeyboardKey_E, "r"},
    {HidKeyboardKey_I, "cup"}, {HidKeyboardKey_K, "cdown"}, {HidKeyboardKey_J, "cleft"}, {HidKeyboardKey_L, "cright"},
    {HidKeyboardKey_LeftArrow, "dleft"}, {HidKeyboardKey_RightArrow, "dright"}, {HidKeyboardKey_UpArrow, "dup"},
    {HidKeyboardKey_DownArrow, "ddown"}, {HidKeyboardKey_A, "left"}, {HidKeyboardKey_D, "right"}, {HidKeyboardKey_W, "up"},
    {HidKeyboardKey_S, "down"}, {HidKeyboardKey_P, "pause"},
    {HidKeyboardKey_F3, "touch_toggle"}, {HidKeyboardKey_F4, "touch_edit"}, {HidKeyboardKey_F6, "touch_player"},
    {HidKeyboardKey_Plus, "touch_larger"}, {HidKeyboardKey_NumPadAdd, "touch_larger"},
    {HidKeyboardKey_Minus, "touch_smaller"}, {HidKeyboardKey_OpenBracket, "touch_opacity_down"},
    {HidKeyboardKey_CloseBracket, "touch_opacity_up"},
}};
constexpr KeyBinding tab_binding{HidKeyboardKey_Tab, "pointer_commands"};

#if defined(SC64_MOVIES)
// PC cinematics (movies/*.smk, tools/voices/extract_movies.py from the user's own
// CampnSC.dat / CampnBW.dat), played full screen while the game is suspended.
constexpr const char* movie_dir = "sdmc:/switch/StarCraft64/movies/";
// The Blizzard logo and PC intro replace the N64 opening animation. That
// animation is the only scene scanned with VI X scale 512 / Y scale 716
// (docs/research.md); Start during it reaches the main menu.
constexpr std::array<const char*, 2> opening_movies = {"blizzard", "starintr"};
constexpr unsigned start_hold = 8, start_hold_limit = 120, start_tries = 4; // main-loop frames
// A repeated Start waits for the game itself to advance (graphics tasks), so a
// slow guest cannot queue presses that would then select Single-Player.
constexpr std::uint64_t start_seen_tasks = 2, start_retry_tasks = 60;
bool opening_animation() {
    const auto vi = sc64::desktop::session().scanout.latest();
    return !vi.blank() && (vi.regs[12] & 0xfff) == 512 && (vi.regs[13] & 0xfff) == 716;
}
// Where the PC campaign shows its cinematics. Each Pre* clip is the location
// title card that opens the cinematic after it. A "before" entry plays when the
// N64 briefing of that PC mission starts (matched by voices.cpp); an episode
// ending plays on the victory results screen of the episode's final mission.
struct Cinematic { const char* mission; bool ending; std::array<const char*, 4> movies; };
constexpr std::array<Cinematic, 15> cinematics = {{
    {"Terran/terran02", false, {"preterran_1", "terran1"}},            // Mar Sara perimeter patrol
    {"Terran/terran06", false, {"preterran_2", "terran2"}},            // the downing of Norad II
    {"Terran/terran11", false, {"preterran_3", "terran3"}},            // assault on the Tarsonis platform
    {"Terran/terran12", true, {"preterran_4", "terran4"}},             // coronation of Arcturus I
    {"Zerg/zerg01", false, {"zerg1"}},                                 // the Overmind's vision
    {"Zerg/zerg06", false, {"prezerg_2", "zerg2"}},                    // the Amerigo
    {"Zerg/zerg09", false, {"prezerg_3", "zerg3", "prezerg_4", "zerg4"}}, // the warp from Char; Aiur
    {"Protoss/protoss03", false, {"preprotoss_1", "protoss1"}},        // Fenix at Antioch
    {"Protoss/protoss05", false, {"preprotoss_2", "protoss2"}},        // the recon camp on Char
    {"Protoss/protoss07", false, {"preprotoss_3", "protoss3"}},        // the return to Aiur
    {"Protoss/protoss10", true, {"protoss4"}},                         // the Overmind's end
    {"EXPProtoss/protoss01", false, {"starxintr"}},                    // Brood War opening
    {"EXPProtoss/protoss08", true, {"xprotoss"}},
    {"EXPTerran/terran08", true, {"xterran"}},
    {"EXPZerg/zerg10", true, {"xzerg"}},
}};
struct Cinema {
    enum class State { idle, playing, leaving } state = State::idle;
    std::vector<std::string> queue; // movies still to play in this showing
    std::string name;
    bool opening = false;           // afterwards Start ends the opening animation
    bool armed = true, skip = false;
    std::set<std::string> shown;    // campaign cinematics already played this session
    std::uint64_t briefing_serial = 0, results_token = 0;
    unsigned frames = 0, tries = 0;
    std::uint64_t pressed_task = 0;
    bool held = false;
};
#endif

struct App {
    Config config;
    FrameVideo video;
#if defined(SC64_SWITCH_GL)
    sc64::nx::GlDevice gl;
#endif
    bool use_gl = false;
    std::unique_ptr<Native> native;
    Shared shared;
    std::unique_ptr<FileLayoutStore> layouts;
    std::unique_ptr<HostScript> host;
    std::unique_ptr<GuiScript> gui;
    std::unique_ptr<StatusScript> status;
    std::optional<int> exit_code;
    PadState pad{};
    u64 previous_buttons = 0;
    std::map<std::string, double> analog;
    std::map<s32, std::pair<double, double>> fingers; // id -> last screen position
    HidKeyboardState keyboard{};
    HidMouseState mouse{};
#if defined(SC64_MOVIES)
    Cinema cinema;
    bool movies = false;
    u64 swallowed = 0; // buttons held when the movie ended

    // Starts a showing of the movies that are installed; false when none plays.
    template <typename Names> bool show(const std::string& name, const Names& names, bool opening) {
        cinema.queue.clear();
        for (const char* movie : names)
            if (movie) cinema.queue.push_back(std::string(movie_dir) + movie + ".smk");
        cinema.name = name;
        if (!next_movie()) return false;
        sc64::shell::log::info("movie: %s", name.c_str());
        sc64::desktop::session().suspend(true);
        cinema.state = Cinema::State::playing;
        cinema.opening = opening;
        cinema.skip = false;
        return true;
    }
    bool next_movie() {
        while (!cinema.queue.empty()) {
            const std::string path = cinema.queue.front();
            cinema.queue.erase(cinema.queue.begin());
            std::error_code missing;
            if (std::filesystem::exists(path, missing) && sc64::movie::start(path)) return true;
            sc64::shell::log::warning("movie: %s not played", path.c_str());
        }
        return false;
    }
    // Main loop, after the game tick: starts, presents and ends cinematics.
    void update_movies() {
        using State = Cinema::State;
        const bool animation = opening_animation();
        switch (cinema.state) {
        case State::idle: {
            if (!animation) cinema.armed = true;
            if (!movies) return;
            if (animation && cinema.armed) {
                cinema.armed = false;
                if (show("opening replaces the N64 opening animation", opening_movies, true)) return;
            }
            const auto briefing = sc64::voices::last_briefing();
            const auto& ui = native->ui();
            const bool won = ui.mode == "results" && ui.victory.value_or(false);
            if (ui.mode != "results") cinema.results_token = 0;
            const bool new_briefing = briefing.serial != cinema.briefing_serial;
            const bool new_victory = won && ui.token != cinema.results_token;
            cinema.briefing_serial = briefing.serial;
            if (won) cinema.results_token = ui.token;
            for (const auto& entry : cinematics) {
                if (briefing.mission != entry.mission || !(entry.ending ? new_victory : new_briefing)) continue;
                const std::string name = std::string(entry.ending ? "after winning " : "before ") + entry.mission;
                if (!cinema.shown.insert(name).second) continue; // once per session
                if (show(name, entry.movies, false)) return;
            }
            return;
        }
        case State::playing:
            if (const auto frame = sc64::movie::update())
                video.upload(Native::Upload{frame->rgba, frame->width, frame->height});
            if (!cinema.skip && !sc64::movie::finished()) return;
            sc64::movie::stop();
            if (!cinema.skip && next_movie()) return; // the next movie of this showing
            cinema.queue.clear();
            sc64::desktop::session().suspend(false);
            swallowed = previous_buttons;
            if (!cinema.opening) { cinema.state = State::idle; return; }
            // Leave the animation the original way: Start reaches the main menu.
            cinema.state = State::leaving;
            cinema.frames = 0; cinema.tries = 0;
            cinema.pressed_task = sc64::desktop::session().tasks.load();
            cinema.held = true;
            action("start", true);
            return;
        case State::leaving: {
            ++cinema.frames;
            const std::uint64_t advanced = sc64::desktop::session().tasks.load() - cinema.pressed_task;
            // Held until the game has run long enough to poll it (or a stalled one gives up).
            if (cinema.held && cinema.frames >= start_hold
                && (advanced >= start_seen_tasks || cinema.frames >= start_hold_limit)) {
                action("start", false);
                cinema.held = false;
            }
            if (cinema.held) return;
            if (!animation) { cinema.state = State::idle; return; }
            if (advanced >= start_retry_tasks) {
                if (++cinema.tries >= start_tries) {
                    sc64::shell::log::warning("movie: the opening animation did not end on Start");
                    cinema.state = State::idle;
                    return;
                }
                cinema.frames = 0;
                cinema.pressed_task = sc64::desktop::session().tasks.load();
                cinema.held = true;
                action("start", true);
            }
            return;
        }
        }
    }
    bool movie_playing() const { return cinema.state == Cinema::State::playing; }
#endif

    void dispatch(const InputEvent& event) {
        if (!gui->on_input(event.id, event) && event.id) host->on_input(*event.id, event);
    }
    void action(const char* id, bool pressed) {
        InputEvent event;
        event.id = id; event.pressed = pressed; event.released = !pressed; event.gamepad = 0;
        dispatch(event);
    }
    void analog_action(const char* id, double value) {
        double& previous = analog[id];
        InputEvent event;
        event.id = id; event.gamepad = 0;
        event.pressed = previous <= analog_press && value > analog_press;
        if (value == 0 && previous != 0) event.released = true; else event.value = value;
        previous = value;
        dispatch(event);
    }
    Touch touch_at(s32 id, double px, double py) const {
        Touch touch;
        touch.id = std::to_string(id);
        touch.screen_x = px; touch.screen_y = screen_h - py;
        touch.x = px * design_width / screen_w; touch.y = (screen_h - py) * design_height / screen_h;
        return touch;
    }
    // Keyboard actions carry no gamepad id, like the desktop shell's.
    void key_action(const char* id, bool pressed) {
        InputEvent event;
        event.id = id; event.pressed = pressed; event.released = !pressed;
        dispatch(event);
    }
    InputEvent mouse_event(const char* id, const HidMouseState& m) const {
        InputEvent event;
        if (id) event.id = id;
        event.screen_x = m.x; event.screen_y = double(screen_h) - m.y;
        event.x = double(m.x) * design_width / screen_w; event.y = (double(screen_h) - m.y) * design_height / screen_h;
        event.confined_mouse = true;
        return event;
    }
    // Key presses and mouse buttons since the last poll (no gamepad, PC-style).
    void poll_keyboard_mouse() {
        HidKeyboardState k{};
        if (hidGetKeyboardStates(&k, 1)) {
            auto edge = [&](const KeyBinding& b) {
                const bool now = hidKeyboardStateGetKey(&k, b.key), before = hidKeyboardStateGetKey(&keyboard, b.key);
                if (now != before) key_action(b.action, now);
            };
            for (const auto& binding : key_bindings) edge(binding);
            edge(tab_binding);
            keyboard = k;
        }
        HidMouseState m{};
        if (!hidGetMouseStates(&m, 1) || !(m.attributes & HidMouseAttribute_IsConnected)) {
            switch_pointer().shown = false; mouse = {};
            return;
        }
        auto& p = switch_pointer();
        const bool moved = m.x != mouse.x || m.y != mouse.y;
        if (moved || m.buttons != mouse.buttons) p.shown = true;
        p.x = m.x; p.y = m.y;
        if (moved) dispatch(mouse_event(nullptr, m));
        constexpr std::array<std::pair<u32, const char*>, 3> buttons = {{
            {HidMouseButton_Left, "touch_mouse"}, {HidMouseButton_Right, "pointer_order"}, {HidMouseButton_Middle, "pointer_pan"}}};
        for (const auto& [bit, id] : buttons)
            if ((m.buttons ^ mouse.buttons) & bit) {
                auto event = mouse_event(id, m);
                event.pressed = (m.buttons & bit) != 0; event.released = !event.pressed;
                dispatch(event);
            }
        mouse = m;
    }
    void poll_input() {
        padUpdate(&pad);
        const u64 buttons = padGetButtons(&pad);
        const u64 changed = buttons ^ previous_buttons;
#if defined(SC64_MOVIES)
        if (movie_playing()) { // A, B, + or a touch skips; nothing reaches the game
            constexpr u64 skip = HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus;
            if (changed & buttons & skip) cinema.skip = true;
            previous_buttons = buttons;
            HidKeyboardState k{};
            if (hidGetKeyboardStates(&k, 1)) {
                for (auto key : {HidKeyboardKey_Escape, HidKeyboardKey_Return, HidKeyboardKey_Space})
                    if (hidKeyboardStateGetKey(&k, key) && !hidKeyboardStateGetKey(&keyboard, key)) cinema.skip = true;
                keyboard = k;
            }
            HidMouseState m{};
            if (hidGetMouseStates(&m, 1)) {
                if (m.buttons & ~mouse.buttons & (HidMouseButton_Left | HidMouseButton_Right)) cinema.skip = true;
                mouse = m;
            }
            HidTouchScreenState state{};
            std::map<s32, std::pair<double, double>> now;
            if (hidGetTouchScreenStates(&state, 1))
                for (s32 i = 0; i < state.count && i < 16; ++i)
                    now[s32(state.touches[i].finger_id)] = {double(state.touches[i].x), double(state.touches[i].y)};
            for (const auto& touch : now) if (!fingers.contains(touch.first)) cinema.skip = true;
            fingers = std::move(now);
            return;
        }
        // Buttons held through the end of the movie stay silent until released,
        // so a skip press cannot cut short the Start sent to the game.
        const u64 quiet = changed & swallowed;
        swallowed &= buttons;
#else
        constexpr u64 quiet = 0;
#endif
        for (const auto& binding : button_bindings)
            if (changed & ~quiet & binding.button) action(binding.action, (buttons & binding.button) != 0);
        if (changed & buttons) switch_pointer().shown = false; // the controller took over
        previous_buttons = buttons;
        poll_keyboard_mouse();
        auto axis = [&](const HidAnalogStickState& s, const char* left, const char* right,
                        const char* up, const char* down) {
            const double x = std::max(-1.0, s.x / 32767.0), y = std::max(-1.0, s.y / 32767.0);
            analog_action(left, x < 0 ? -x : 0); analog_action(right, x > 0 ? x : 0);
            analog_action(up, y > 0 ? y : 0); analog_action(down, y < 0 ? -y : 0); // HID: +y is up
        };
        axis(padGetStickPos(&pad, 0), "left", "right", "up", "down");
        axis(padGetStickPos(&pad, 1), "cleft", "cright", "cup", "cdown");

        HidTouchScreenState state{};
        std::map<s32, std::pair<double, double>> now;
        if (hidGetTouchScreenStates(&state, 1))
            for (s32 i = 0; i < state.count && i < 16; ++i)
                now[s32(state.touches[i].finger_id)] = {double(state.touches[i].x), double(state.touches[i].y)};
        InputEvent event;
        event.id = "touch_multi";
        for (const auto& [id, position] : now) {
            auto touch = touch_at(id, position.first, position.second);
            touch.pressed = !fingers.contains(id);
            event.touch.push_back(touch);
        }
        for (const auto& [id, position] : fingers)
            if (!now.contains(id)) {
                auto touch = touch_at(id, position.first, position.second);
                touch.released = true;
                event.touch.push_back(touch);
            }
        fingers = std::move(now);
        if (!event.touch.empty()) {
            switch_pointer().shown = false; // touch took over
            dispatch(event);
        }
    }
};
}

int main(int argc, char* argv[]) {
    std::set_terminate(terminate_handler);
    std::remove("sdmc:/switch/StarCraft64/crash.txt");
    std::error_code ignored;
    std::filesystem::create_directories(storage_dir, ignored);
    // stdout/stderr (shell log, runtime prints) go to the SD card.
    std::freopen("sdmc:/switch/StarCraft64/sc64.log", "w", stdout);
    std::freopen("sdmc:/switch/StarCraft64/sc64-err.log", "w", stderr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    hidInitializeTouchScreen();
    hidInitializeKeyboard();
    hidInitializeMouse();
    auto app = std::make_unique<App>();
    padInitializeDefault(&app->pad);
    int result = 0;
    try {
        app->config.parse_arguments(argc, argv);
        app->config.set("sc64.storage", storage_dir);
        // ROM read in place from the SD card (platform.cpp); no private copy.
        app->config.set("sc64.bundled_rom", "1");
#if defined(SC64_SWITCH_GL)
        app->use_gl = !option_file("nogl") && app->gl.create();
        // GPU rasterization (sdmc:/switch/StarCraft64/softraster keeps it on the CPU;
        // .../glcheck renders both ways and logs differences).
        if (app->use_gl && app->gl.raster_ready()
            && !option_file("softraster")) {
            auto* device = &app->gl;
            sc64::desktop::session().gpu_render = [device](const sc64::gfx::DrawList& list,
                                                          sc64::gfx::SoftwareResult& out,
                                                          const std::function<void()>& service) {
                return device->render(list, out, service);
            };
            sc64::desktop::session().gpu_check = option_file("glcheck");
        }
        // GL only rasterizes; presentation stays on the libnx framebuffer.
        app->use_gl = false;
#endif
        app->video.create();
        auto* raw = app.get();
        // Graphics tasks run on the Gfx thread while the game continues (like the RCP).
        sc64::desktop::session().async_graphics = true;
        // Every cheat on the Cheats page from the start (sdmc:/switch/StarCraft64/lockcheats keeps the original unlocks).
        sc64::desktop::session().unlock_cheats = !option_file("lockcheats");
        // Experimental 16:9 missions (416x240, 3x = 1248x720): sdmc:/switch/StarCraft64/widescreen.
        if (option_file("widescreen")) sc64::runtime::set_widescreen(416);
        sc64::shell::log::info(option_file("widescreen") ? "widescreen: 416x240 missions"
                                                         : "widescreen: off (no widescreen file)");
        app->native = std::make_unique<Native>(app->config, Native::Window{
            [] { return std::pair<double, double>(screen_w, screen_h); },
            [raw](int code) { raw->exit_code = code; },
            [raw] { return std::string(raw->use_gl ? "egl-opengl" : "libnx-framebuffer"); }});
        app->native->initialize();
        // Voiced briefings from the user's own PC files (tools/voices/extract_briefings.py).
        sc64::voices::initialize(std::string(storage_dir) + "/voices");
        // HD missions from the user's own PC graphics (tools/hd/extract_graphics.py); nohd keeps the N64 ones.
        sc64::hd::initialize(std::string(storage_dir) + "/hd");
#if defined(SC64_PORTRAITS)
        // Animated portraits from the same PC graphics (extract_graphics.py, hd/portrait).
        sc64::portraits::initialize(std::string(storage_dir) + "/hd");
#endif
#if defined(SC64_MUSIC)
        // PC music from the user's own CDs (tools/music/extract_music.py); n64music keeps the original.
        sc64::music::initialize(std::string(storage_dir) + "/music");
#endif
        {
            float volume = default_volume;
            if (FILE* f = std::fopen("sdmc:/switch/StarCraft64/volume", "r")) {
                float value = 0;
                if (std::fscanf(f, "%f", &value) == 1 && value >= 0) volume = value;
                std::fclose(f);
            }
            sc64::desktop::audio::set_game_gain(volume);
            sc64::shell::log::info("audio: game volume x%.2f", volume);
        }
#if defined(SC64_MOVIES)
        {
            std::error_code ignored;
            unsigned installed = 0;
            for (const auto& entry : std::filesystem::directory_iterator(movie_dir, ignored))
                if (entry.path().extension() == ".smk") ++installed;
            app->movies = installed && !option_file("nomovies");
            sc64::shell::log::info("movie: %u movies installed, %s", installed, app->movies ? "enabled" : "disabled");
        }
#endif
        app->layouts = std::make_unique<FileLayoutStore>(std::filesystem::path(app->native->storage())
                                                         / "touch-layout-v2.json");
        app->host = std::make_unique<HostScript>(app->shared, *app->native, app->config);
        app->gui = std::make_unique<GuiScript>(app->shared, *app->native, app->config, *app->layouts);
        app->status = std::make_unique<StatusScript>(app->shared, *app->native);
        app->host->init();
        app->gui->init();
        InputEvent connected;
        connected.id = "connected"; connected.pressed = true; connected.gamepad = 0;
        app->dispatch(connected);
        u64 last = armGetSystemTick();
        while (appletMainLoop() && !app->exit_code) {
            const u64 now = armGetSystemTick();
            const double dt = double(armTicksToNs(now - last)) / 1e9;
            last = now;
            app->poll_input();
            const u64 tick_start = armGetSystemTick();
            auto upload_now = app->native->tick();
            loop_profile.tick += armTicksToNs(armGetSystemTick() - tick_start);
            if (const auto& upload = upload_now) {
#if defined(SC64_SWITCH_GL)
                if (app->use_gl) app->gl.upload(*upload); else
#endif
#if defined(SC64_MOVIES)
                if (!app->movie_playing())
#endif
                app->video.upload(*upload);
            }
#if defined(SC64_MOVIES)
            app->update_movies();
            const bool visible = app->movie_playing() || app->native->visible();
#else
            const bool visible = app->native->visible();
#endif
            app->host->update(dt);
            app->gui->update(dt);
            app->status->update();
#if defined(SC64_SWITCH_GL)
            if (app->use_gl) app->gl.draw(visible); else
#endif
            app->video.draw(visible);
            app->native->after_render([] { return Readback{}; });
            if (++loop_profile.loops == 600) {
                const double ms = 1e-6 / 600;
                sc64::shell::log::info("loop ms: tick %.2f scale %.2f wait %.2f | %.1f loops/s",
                    double(loop_profile.tick) * ms, double(loop_profile.scale) * ms, double(loop_profile.wait) * ms,
                    600.0 / (double(armTicksToNs(armGetSystemTick() - loop_profile.since)) / 1e9));
                loop_profile = {}; loop_profile.since = armGetSystemTick();
            }
        }
        if (app->exit_code) result = *app->exit_code;
    } catch (const std::exception& error) {
        sc64::shell::log::error("SC64 Switch: %s", error.what());
        result = 1;
    }
#if defined(SC64_MOVIES)
    sc64::movie::stop();
#endif
    if (app->gui) app->gui->final();
    if (app->host) app->host->final();
#if defined(SC64_MUSIC)
    sc64::music::shutdown();
#endif
    if (app->native) app->native->finalize();
    sc64::desktop::session().gpu_render = nullptr;
#if defined(SC64_SWITCH_GL)
    app->gl.destroy();
#endif
    app->video.destroy();
    std::fflush(stdout); std::fflush(stderr);
    return result;
}
