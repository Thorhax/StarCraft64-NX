// libnx application shell: framebuffer video, HID pads and touch screen.
// Mirrors native/shell/app.cpp (SDL3) with the same Native/script objects; the
// guest runtime, raster helpers and audio keep their own threads.
#include "log.hpp"
#include "native.hpp"
#include "scripts.hpp"
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
        std::fclose(f);
    }
    std::fflush(stdout); std::fflush(stderr);
    std::abort();
}
constexpr const char* storage_dir = "sdmc:/switch/StarCraft64";

// Original raster fitted with nearest sampling (320x240 and 640x480 -> 960x720),
// centred: the same geometry as the shell's Fit viewport, so touch mapping matches.
class FrameVideo {
public:
    void create() {
        if (R_FAILED(framebufferCreate(&fb_, nwindowGetDefault(), screen_w, screen_h, PIXEL_FORMAT_RGBA_8888, 2)))
            throw std::runtime_error("framebufferCreate failed");
        if (R_FAILED(framebufferMakeLinear(&fb_))) throw std::runtime_error("framebufferMakeLinear failed");
        ready_ = true;
    }
    void destroy() { if (ready_) framebufferClose(&fb_); ready_ = false; }
    void upload(const Native::Upload& image) { image_ = image; }
    // Begin/End also presents and paces to the display (double buffered).
    void draw(bool visible) {
        u32 stride = 0;
        auto* base = static_cast<std::uint8_t*>(framebufferBegin(&fb_, &stride));
        for (unsigned y = 0; y < screen_h; ++y) std::memset(base + std::size_t(y) * stride, 0, screen_w * 4);
        if (visible && image_ && image_->rgba && image_->width && image_->height
            && image_->rgba->size() >= std::size_t(image_->width) * image_->height * 4) {
            const unsigned w = image_->width, h = image_->height;
            unsigned dw = screen_w, dh = unsigned(std::uint64_t(screen_w) * h / w);
            if (dh > screen_h) { dh = screen_h; dw = unsigned(std::uint64_t(screen_h) * w / h); }
            const unsigned ox = (screen_w - dw) / 2, oy = (screen_h - dh) / 2;
            if (xmap_.size() != dw || xmap_source_ != w) {
                xmap_.resize(dw);
                for (unsigned x = 0; x < dw; ++x) xmap_[x] = unsigned(std::uint64_t(x) * w / dw);
                xmap_source_ = w;
            }
            const auto* src = reinterpret_cast<const std::uint32_t*>(image_->rgba->data());
            unsigned previous = ~0u;
            for (unsigned y = 0; y < dh; ++y) {
                const unsigned sy = unsigned(std::uint64_t(y) * h / dh);
                auto* row = reinterpret_cast<std::uint32_t*>(base + std::size_t(oy + y) * stride) + ox;
                if (sy == previous) { // repeated source row: copy the line above
                    std::memcpy(row, reinterpret_cast<std::uint32_t*>(base + std::size_t(oy + y - 1) * stride) + ox,
                                std::size_t(dw) * 4);
                    continue;
                }
                previous = sy;
                const auto* in = src + std::size_t(sy) * w;
                for (unsigned x = 0; x < dw; ++x) row[x] = in[xmap_[x]] | 0xff000000u; // RGBA32, opaque
            }
        }
        framebufferEnd(&fb_);
    }
private:
    Framebuffer fb_{};
    bool ready_ = false;
    std::optional<Native::Upload> image_;
    std::vector<unsigned> xmap_;
    unsigned xmap_source_ = 0;
};

// Switch button -> shell action (N64 layout: A/B as on the original pad).
struct ButtonBinding { u64 button; const char* action; };
constexpr std::array<ButtonBinding, 12> button_bindings = {{
    {HidNpadButton_A, "a"}, {HidNpadButton_B, "b"}, {HidNpadButton_Plus, "start"},
    {HidNpadButton_L, "l"}, {HidNpadButton_R, "r"}, {HidNpadButton_ZL, "z"}, {HidNpadButton_ZR, "z"},
    {HidNpadButton_Left, "dleft"}, {HidNpadButton_Right, "dright"}, {HidNpadButton_Up, "dup"},
    {HidNpadButton_Down, "ddown"}, {HidNpadButton_Minus, "pointer_commands"},
}};
constexpr double analog_press = 0.5;

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
    void poll_input() {
        padUpdate(&pad);
        const u64 buttons = padGetButtons(&pad);
        const u64 changed = buttons ^ previous_buttons;
        for (const auto& binding : button_bindings)
            if (changed & binding.button) action(binding.action, (buttons & binding.button) != 0);
        previous_buttons = buttons;
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
        if (!event.touch.empty()) dispatch(event);
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
    auto app = std::make_unique<App>();
    padInitializeDefault(&app->pad);
    int result = 0;
    try {
        app->config.parse_arguments(argc, argv);
        app->config.set("sc64.storage", storage_dir);
        // ROM read in place from the SD card (platform.cpp); no private copy.
        app->config.set("sc64.bundled_rom", "1");
#if defined(SC64_SWITCH_GL)
        app->use_gl = !std::filesystem::exists("sdmc:/switch/StarCraft64/nogl") && app->gl.create();
        // GPU rasterization (sdmc:/switch/StarCraft64/softraster keeps it on the CPU;
        // .../glcheck renders both ways and logs differences).
        if (app->use_gl && app->gl.raster_ready()
            && !std::filesystem::exists("sdmc:/switch/StarCraft64/softraster")) {
            auto* device = &app->gl;
            sc64::desktop::session().gpu_render = [device](const sc64::gfx::DrawList& list,
                                                          sc64::gfx::SoftwareResult& out,
                                                          const std::function<void()>& service) {
                return device->render(list, out, service);
            };
            sc64::desktop::session().gpu_check = std::filesystem::exists("sdmc:/switch/StarCraft64/glcheck");
        }
        // GL only rasterizes; presentation stays on the libnx framebuffer.
        app->use_gl = false;
#endif
        app->video.create();
        auto* raw = app.get();
        app->native = std::make_unique<Native>(app->config, Native::Window{
            [] { return std::pair<double, double>(screen_w, screen_h); },
            [raw](int code) { raw->exit_code = code; },
            [raw] { return std::string(raw->use_gl ? "egl-opengl" : "libnx-framebuffer"); }});
        app->native->initialize();
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
            if (const auto upload = app->native->tick()) {
#if defined(SC64_SWITCH_GL)
                if (app->use_gl) app->gl.upload(*upload); else
#endif
                app->video.upload(*upload);
            }
            app->host->update(dt);
            app->gui->update(dt);
            app->status->update();
#if defined(SC64_SWITCH_GL)
            if (app->use_gl) app->gl.draw(app->native->visible()); else
#endif
            app->video.draw(app->native->visible());
            app->native->after_render([] { return Readback{}; });
        }
        if (app->exit_code) result = *app->exit_code;
    } catch (const std::exception& error) {
        sc64::shell::log::error("SC64 Switch: %s", error.what());
        result = 1;
    }
    if (app->gui) app->gui->final();
    if (app->host) app->host->final();
    if (app->native) app->native->finalize();
    sc64::desktop::session().gpu_render = nullptr;
#if defined(SC64_SWITCH_GL)
    app->gl.destroy();
#endif
    app->video.destroy();
    std::fflush(stdout); std::fflush(stderr);
    return result;
}
