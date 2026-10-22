#include "session.hpp"
#include "log.hpp"
#include "audio.hpp"
#include "clock_bridge.hpp"
#include "rdp_writeback.hpp"
#include "flash_device.hpp"
#if defined(SC64_VOICES)
#include "voices.hpp"
#endif
#include "guest_fpu.hpp"
#include "../runtime/guest_poll.hpp"
#include "../runtime/rom_image.hpp"
#include "../runtime/minimap.hpp"
#include <librecomp/game.hpp>
#include <librecomp/overlays.hpp>
#include <librecomp/rsp.hpp>
#include <xxHash/xxh3.h>
#include "../runtime/sha.hpp"
#include <algorithm>
#include <deque>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>

extern "C" {
#include "funcs.h"
#include "recomp_overlays.inl"
}

namespace sc64::desktop {
namespace {
std::mutex pending_mutex;
std::atomic_flag preparing_task = ATOMIC_FLAG_INIT;
struct SubmissionGuard {
    SubmissionGuard() {
        if (preparing_task.test_and_set()) throw std::runtime_error("Overlapping guest graphics submission");
    }
    ~SubmissionGuard() { preparing_task.clear(); }
};
// Asynchronous graphics, as on the RCP: at osSpTaskStart the guest thread only
// snapshots RDRAM and continues; the Gfx thread interprets, rasterizes, commits
// the RDP stores and publishes the frame, and only then are SP/DP completion
// signalled. The game waits for DP completion before using the results.
struct SubmittedTask {
    std::vector<std::uint8_t> ram;   // canonical (big-endian) RDRAM at submission
    std::uint8_t* rdram = nullptr;   // live guest RAM, for the RDP stores
    std::uint32_t ucode = 0, data_ptr = 0;
    sc64::runtime::FrontendFeedback ui;
    // Synchronous mode (async_graphics off): already rendered on the guest thread.
    std::shared_ptr<gfx::DrawList> list;
    std::shared_ptr<const RenderedFrame> rendered;
};
std::mutex submitted_mutex;
std::deque<std::unique_ptr<SubmittedTask>> submitted;
std::vector<std::vector<std::uint8_t>> snapshot_pool; // recycled 8 MiB snapshot buffers
// Recycled 8 MiB render results: only their target extents are refreshed per
// frame (gfx::prepare_result_ram), instead of a full copy of the snapshot.
std::vector<std::vector<std::uint8_t>> result_pool;
void recycle(std::vector<std::vector<std::uint8_t>>& pool, std::vector<std::uint8_t>&& buffer) {
    if (buffer.size() != 0x800000) return;
    std::lock_guard lock(submitted_mutex);
    if (pool.size() < 4) pool.push_back(std::move(buffer));
}
std::vector<std::uint8_t> take(std::vector<std::vector<std::uint8_t>>& pool) {
    std::lock_guard lock(submitted_mutex);
    if (pool.empty()) return {};
    auto buffer = std::move(pool.back());
    pool.pop_back();
    return buffer;
}
std::shared_ptr<const RenderedFrame> render_task(SubmittedTask& job, std::shared_ptr<gfx::DrawList>& list,
                                                const std::function<void()>& service);
std::string sha1(std::span<const std::uint8_t> bytes) { return runtime::sha1_hex(bytes); }
class Renderer final : public ultramodern::renderer::RendererContext {
public:
    Renderer() {
        setup_result = ultramodern::renderer::SetupResult::Success; chosen_api = ultramodern::renderer::GraphicsApi::Auto;
    }
    std::uint64_t completed_task_=0;
    bool valid() override { return true; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
    void enable_instant_present() override {}
    // Gfx thread, in submission order; SP/DP completion follows this call.
    void send_dl(const OSTask*) override {
        std::unique_ptr<SubmittedTask> job;
        {
            std::lock_guard lock(submitted_mutex);
            if (submitted.empty()) return;
            job = std::move(submitted.front());
            submitted.pop_front();
        }
        try {
            if (!job->rendered) job->rendered = render_task(*job, job->list, [] {});
            completed_task_ = job->list->identity.task;
            session().mailbox.publish(std::move(job->list), std::move(job->ui), std::move(job->rendered));
        } catch (const std::exception& error) {
            session().fail(error.what());
        }
    }
    void send_dummy_workload(std::uint32_t) override {}
    void update_screen() override {
        const auto& v=*ultramodern::renderer::get_vi_regs();
        session().scanout.publish({v.VI_STATUS_REG,v.VI_ORIGIN_REG,v.VI_WIDTH_REG,v.VI_INTR_REG,
            v.VI_V_CURRENT_LINE_REG,v.VI_TIMING_REG,v.VI_V_SYNC_REG,v.VI_H_SYNC_REG,
            v.VI_LEAP_REG,v.VI_H_START_REG,v.VI_V_START_REG,v.VI_V_BURST_REG,
            v.VI_X_SCALE_REG,v.VI_Y_SCALE_REG},session().fields.load(),completed_task_);
    }
    void shutdown() override {}
    std::uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1; }
};
}
Session& session() { static Session value; return value; }
// Orderly shutdown belongs to the extension finalize callbacks (stop()).
// Static destruction can run from std::exit() on a guest thread, where
// quit()/join() would wait for that same thread, and other translation units'
// runtime statics have no ordering guarantee here. Only release our handle.
Session::~Session() { if (worker_.joinable()) worker_.detach(); }
void Session::load(const std::filesystem::path& path, const std::filesystem::path& storage) {
    if (launched_) throw std::runtime_error("Runtime supports one session per process; restart to change ROM");
    std::vector<std::uint8_t> rom(runtime::rom_size);
    {
        // Close the source before persisting: the path may be the stored copy
        // itself, and Windows cannot replace a file that is still open.
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() != std::streamoff(runtime::rom_size)) throw std::runtime_error("Expected a 32 MiB ROM dump");
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(rom.data()), rom.size())) throw std::runtime_error("ROM read failed");
    }
    launch_rom(std::move(rom), storage, true);
}
void Session::load_bundled(std::vector<std::uint8_t> rom, const std::filesystem::path& storage) {
    launch_rom(std::move(rom), storage, false);
}
void Session::launch_rom(std::vector<std::uint8_t> rom, const std::filesystem::path& storage, bool persist) {
    if (launched_) throw std::runtime_error("Runtime supports one session per process; restart to change ROM");
    runtime::validate_rom(rom, sha1);
    std::filesystem::create_directories(storage);
    // Validate every user-owned input before process-wide side effects. The
    // save image may be malformed; failing here leaves no audio device open.
    runtime::flash_device().initialize(storage / "sc64.fla");
#if defined(__SWITCH__)
    // A save is ~400 page programs; writing the SD file for each one froze the game for ~20 s.
    runtime::flash_device().defer_writes(std::chrono::milliseconds(500));
#endif
    if (persist) {
        // Picker imports survive cold starts. APK assets already have persistent
        // storage, so that path never creates a second ROM file.
        const auto temporary = storage / ".sc64-rom.tmp";
        std::ofstream output(temporary, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.write(reinterpret_cast<const char*>(rom.data()), rom.size());
        output.close();
        std::filesystem::rename(temporary, storage / "sc64-ntsc-u.z64");
    }
    audio::initialize();
    try {
        start_runtime(std::move(rom), storage);
    } catch (...) {
        // Registration is assignment/insert-only and idempotent for this
        // single validated game; only the audio device needs rollback.
        running = false; rom_preloaded = false;
        audio::shutdown();
        throw;
    }
}
void Session::start_runtime(std::vector<std::uint8_t> rom, const std::filesystem::path& storage) {
    raster = std::make_unique<gfx::RasterPool>(gfx::RasterPool::default_helpers());
    recomp::register_config_path(storage);
    recomp::overlays::register_overlays({section_table, ARRLEN(section_table), ARRLEN(section_table)}, {nullptr, 0});
    recomp::GameEntry game{};
    game.rom_hash = XXH3_64bits(rom.data(), rom.size());
    game.internal_name = "STARCRAFT64"; game.display_name = "StarCraft 64 NTSC-U";
    game.game_id = u8"sc64-ntsc-u"; game.is_enabled = true; game.save_type = recomp::SaveType::Flashram;
    game.entrypoint_address = std::int64_t(std::int32_t(0x80000400)); game.entrypoint = recomp_entrypoint;
    game.on_init_callback = [](std::uint8_t* rdram, recomp_context*) {
        runtime::initialize_guest_fpu();
        MEM_W(0, std::int64_t(std::int32_t(0x80000314))) = 1;
    };
    game.thread_create_callback = [](std::uint8_t*, recomp_context*) { runtime::initialize_guest_fpu(); };
    runtime::minimap_input().clear();
    runtime::set_minimap_handler([](std::uint8_t* ram, void* context, const runtime::MinimapTarget& target) {
        auto& s=session();
        const auto field=s.fields.load();
        if (s.paused || !s.running || field<target.field || field-target.field>12) return;
        // Use the original setter on its own guest thread and guest stack.
        // Preserve the interrupted input query's complete register context.
        auto* ctx=static_cast<recomp_context*>(context);
        const auto saved=*ctx;
        ctx->r4=target.x; ctx->r5=target.y;
        sc64_resident_8000B988(ram,ctx);
        *ctx=saved;
        s.minimap_field=field;
        ++s.minimap_jumps;
    });
    recomp::register_game(game);
    recomp::Configuration config{};
    config.project_version = {0, 2, 0};
#if defined(__APPLE__)
    config.window_handle = {reinterpret_cast<void*>(1), nullptr};
#elif defined(_WIN32)
    config.window_handle = {reinterpret_cast<HWND>(1), 0};
#elif defined(__SWITCH__)
    config.window_handle = reinterpret_cast<void*>(1);
#else
    config.window_handle = reinterpret_cast<SDL_Window*>(1);
#endif
    config.renderer_callbacks.create_render_context = [](std::uint8_t*, ultramodern::renderer::WindowHandle, bool) -> std::unique_ptr<ultramodern::renderer::RendererContext> {
        return std::make_unique<Renderer>();
    };
    config.rsp_callbacks.get_rsp_microcode = [](const OSTask*) -> RspUcodeFunc* {
        throw std::runtime_error("Unresearched non-graphics RSP task (D-020)");
    };
    config.audio_callbacks = {[](std::int16_t* data, std::size_t count) { audio::queue(data, count); }, audio::remaining, audio::frequency};
    config.input_callbacks.get_input = [](int player, std::uint16_t* buttons, float* x, float* y) {
        if (player < 0 || player >= 2) return false;
        auto& pad = session().pads[player];
        *buttons = std::uint16_t(pad.buttons.load()); *x = pad.x.load(); *y = pad.y.load(); return true;
    };
    config.input_callbacks.get_connected_device_info = [](int player) {
        using namespace ultramodern::input;
        return connected_device_info_t{player >= 0 && player < 2 ? Device::Controller : Device::None,
                                      player >= 0 && player < 2 ? Pak::RumblePak : Pak::None};
    };
    config.input_callbacks.set_rumble = [](int player, bool on) {
        if (player != 0) return;
        if (on && !session().rumble.exchange(true)) ++session().rumble_starts;
        if (!on) session().rumble = false;
    };
    config.events_callbacks.vi_callback = [] { ++session().fields; };
    config.error_handling_callbacks.message_box = [](const char* text) { session().fail(text); };
    recomp::set_rom_contents(std::move(rom));
    rom_preloaded = true;
    started = clock.now();
    running = true;
    recomp::start_game(game.game_id, "");
    worker_ = std::thread([this, config] {
        try { recomp::start(config); }
        catch (const ultramodern::thread_terminated&) {} // cooperative shutdown
        catch (const std::exception& error) { fail(error.what()); }
        running = false;
    });
    launched_ = true;
}
void Session::pause(bool value) {
    std::lock_guard lock(pause_mutex_);
    pause_sources_.requested = value;
    requested_paused = value;
    apply_pause_locked();
}
void Session::suspend(bool value) {
    std::lock_guard lock(pause_mutex_);
    pause_sources_.suspended = value;
    apply_pause_locked();
}
void Session::apply_pause_locked() {
    const bool value = pause_sources_.paused();
    if (value) {
        for (auto& pad : pads) { pad.buttons = 0; pad.x = 0; pad.y = 0; }
        runtime::minimap_input().clear();
    }
    if (value != paused.load()) audio::pause_changed();
    paused = value;
    clock.set(value);
}
void Session::stop() {
    clock.stop();
    if (worker_.joinable()) { ultramodern::quit(); worker_.join(); }
    raster.reset();
    audio::shutdown();
    runtime::flash_device().finish(); // publish a save the writer has not reached yet
    running = false;
}
void Session::fail(const std::string& message) {
    error_.record(message);
    clock.stop();
    ultramodern::quit();
}
std::string Session::error() { return error_.get(); }
}

bool sc64_rom_is_preloaded(const std::u8string& game_id) {
    return game_id == u8"sc64-ntsc-u" && sc64::desktop::session().rom_preloaded.load()
        && recomp::is_rom_loaded();
}
std::chrono::high_resolution_clock::time_point sc64_runtime_now() { return sc64::desktop::session().clock.now(); }
void sc64_pause_point() { sc64::desktop::session().clock.wait(); }
namespace {
// GPU validation mode: compare the GPU result with the software reference
// (write ranges and every RDRAM byte), log a summary every 60 frames plus the
// first differing pixels, and keep the software result.
void check_gpu_frame(const sc64::gfx::SoftwareResult& gpu, sc64::gfx::SoftwareResult software,
                     sc64::gfx::SoftwareResult& keep) {
    static std::uint64_t frames=0, identical=0, bad_pixels=0, worst=0, reported=0;
    std::uint64_t differing=0;
    std::size_t first=SIZE_MAX;
    for(std::size_t i=0;i<software.ram.size() && i<gpu.ram.size();i+=2)
        if(software.ram[i]!=gpu.ram[i] || software.ram[i+1]!=gpu.ram[i+1]) {
            if(first==SIZE_MAX)first=i;
            ++differing;
        }
    const bool same_writes=gpu.writes.size()==software.writes.size() && std::equal(gpu.writes.begin(),gpu.writes.end(),
        software.writes.begin(),[](auto a,auto b){return a.address==b.address && a.length==b.length;});
    ++frames;
    if(!differing && same_writes)++identical;
    bad_pixels+=differing;
    worst=std::max(worst,differing);
    if(first!=SIZE_MAX && reported<20) {
        ++reported;
        sc64::shell::log::warning("GL check: frame %llu, %llu pixels differ, first at 0x%06zx gpu %04x sw %04x",
            (unsigned long long)frames,(unsigned long long)differing,first,
            unsigned((gpu.ram[first]<<8)|gpu.ram[first+1]),unsigned((software.ram[first]<<8)|software.ram[first+1]));
    } else if(!same_writes && reported<20) {
        ++reported;
        sc64::shell::log::warning("GL check: frame %llu, same pixels but write ranges differ (gpu %zu, sw %zu)",
            (unsigned long long)frames,gpu.writes.size(),software.writes.size());
    }
    if(frames%60==0)
        sc64::shell::log::info("GL check: %llu frames, %llu identical, %llu differing pixels (worst frame %llu)",
            (unsigned long long)frames,(unsigned long long)identical,(unsigned long long)bad_pixels,
            (unsigned long long)worst);
    keep=std::move(software);
}
}

namespace sc64::desktop {
namespace {
// Gfx thread: everything that used to run on the guest thread after the
// snapshot. No guest servicing is needed here: the guest keeps running.
std::shared_ptr<const RenderedFrame> render_task(SubmittedTask& job, std::shared_ptr<gfx::DrawList>& list,
                                                const std::function<void()>& no_service) {
    const auto& canonical = job.ram;
    const unsigned ucode = job.ucode & 0x1fffffff;
    if (ucode > canonical.size() - 4096 || sha1({canonical.data() + ucode, 4096}) != "7d0f686719afde0703d377ccc7a492318c01eec0")
        throw std::runtime_error("Graphics microcode identity mismatch");
    const auto interpret_start = std::chrono::steady_clock::now();
    // The DrawList takes the snapshot itself (no copy) and hands it back to the
    // pool when the last owner releases it.
    list = std::shared_ptr<gfx::DrawList>(
        new gfx::DrawList(gfx::interpret_gbi(std::move(job.ram), job.data_ptr & 0x1fffffff, no_service)),
        [](gfx::DrawList* done) { recycle(snapshot_pool, std::move(done->initial_ram)); delete done; });
    session().capture_interpret_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - interpret_start).count();
    list->identity.task = ++session().tasks;
    list->identity.field = session().fields;
    for (const auto& rect : list->rectangles) session().cpu_scanouts.targeted(rect.target.address, list->identity.task);
    auto rendered = std::shared_ptr<RenderedFrame>(new RenderedFrame,
        [](RenderedFrame* done) { recycle(result_pool, std::move(done->pixels.ram)); delete done; });
    const auto start = std::chrono::steady_clock::now();
    // GL check compares every RDRAM byte, so it keeps full copies.
    std::vector<std::uint8_t> spare = session().gpu_check ? std::vector<std::uint8_t>{} : take(result_pool);
    bool gpu = false;
    if (session().gpu_render) {
        gfx::SoftwareResult gpu_result;
        gpu_result.ram = std::move(spare);
        if (session().gpu_render(*list, gpu_result, no_service)) {
            gpu = true;
            if (session().gpu_check)
                ::check_gpu_frame(gpu_result, gfx::render_software(*list, no_service, session().raster.get()), rendered->pixels);
            else rendered->pixels = std::move(gpu_result);
        } else spare = std::move(gpu_result.ram);
    }
    if (!gpu) rendered->pixels = gfx::render_software(*list, no_service, session().raster.get(), std::move(spare));
    ++(gpu ? session().gpu_frames : session().software_frames);
    rendered->software_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    rendered->software_service_ms = 0;
    // RDP stores into live guest RAM, before DP completion is signalled.
    commit_rdp_writes(rendered->pixels, {job.rdram, std::size_t(0x800000)});
    return rendered;
}
}
}

extern "C" void sc64_capture_submit(std::uint8_t* rdram, const OSTask* task) {
    using namespace sc64::desktop;
    sc64::runtime::report_host_failure<ultramodern::thread_terminated>([&] {
        sc64_pause_point();
        SubmissionGuard submission;
        if (session().unlock_cheats) // live RDRAM keeps host-order 32-bit words
            reinterpret_cast<std::uint32_t*>(rdram)[0xd13d0 / 4] |= 0x781e; // the eight cheat bits
#if defined(SC64_VOICES)
        sc64::voices::observe_messages(rdram);
#endif
        const auto t = task->t;
        if (t.type != M_GFXTASK || t.ucode_size != 4096) throw std::runtime_error("Unknown graphics task");
        const auto snapshot_start=std::chrono::steady_clock::now();
        auto job=std::make_unique<SubmittedTask>();
        {
            std::lock_guard lock(submitted_mutex);
            if(!snapshot_pool.empty()) { job->ram=std::move(snapshot_pool.back()); snapshot_pool.pop_back(); }
        }
        if(job->ram.size()!=0x800000)job->ram.resize(0x800000); // first use only; later buffers are recycled
        // Big-endian copy of the live RDRAM: rdram keeps host-order 32-bit words,
        // so canonical[i] = rdram[i ^ 3] is one byte swap per word.
        {
            const auto* words = reinterpret_cast<const std::uint32_t*>(rdram);
            auto* out = reinterpret_cast<std::uint32_t*>(job->ram.data());
            for (std::size_t i = 0; i < job->ram.size() / 4; ++i) out[i] = __builtin_bswap32(words[i]);
        }
        job->rdram=rdram;
        job->ucode=t.ucode;
        job->data_ptr=t.data_ptr;
        job->ui=sc64::runtime::take_frontend_feedback();
        session().capture_snapshot_ms=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-snapshot_start).count();
        if(!session().async_graphics) {
            // Synchronous (Android/desktop): render here, servicing higher guest
            // priorities every millisecond; the Gfx thread only publishes.
            auto last_poll=std::chrono::steady_clock::now();
            const std::function<void()> service_guest=[&] {
                const auto now=std::chrono::steady_clock::now();
                if(now-last_poll<std::chrono::milliseconds(1))return;
                ultramodern::wait_for_external_message_timed(rdram,0);
                ultramodern::check_running_queue(rdram);
                last_poll=std::chrono::steady_clock::now();
            };
            job->rendered=render_task(*job,job->list,service_guest);
        }
        std::lock_guard lock(submitted_mutex);
        submitted.push_back(std::move(job));
    }, [](const char* error) { session().fail(error); });
}
// osViSwapBuffer on the guest thread: the guest declares this framebuffer
// complete. Own its bytes only when no RDP task drew it since its last swap.
extern "C" void sc64_vi_swap(std::uint8_t* rdram, std::int32_t pointer) {
    using namespace sc64::desktop;
    sc64::runtime::report_host_failure<ultramodern::thread_terminated>([&] {
        auto& s=session();
        const std::uint32_t address=std::uint32_t(pointer)&0x1fffffff;
        if (address>=0x800000) return;
        const auto submitted=s.tasks.load();
        if (!s.cpu_scanouts.swapped(address,submitted)) return;
        const auto vi=s.scanout.latest();
        const sc64::gfx::Image image{address,vi.regs[2]&0xfff,0,vi.regs[0]&3};
        const unsigned height=vi_region(vi,image.width,1024).height;
        // Only the RGBA16 modes the software target export supports.
        if (vi.blank() || image.size!=2 || !image.width || image.width>1024
            || !(vi.regs[13]&0xfff) || !target_in_ram(image,height,0x800000)) return;
        auto cpu=std::make_shared<CpuScanout>();
        cpu->image=image; cpu->height=height; cpu->after_task=submitted;
        cpu->canonical.resize(std::size_t(image.width)*2*height);
        for (std::size_t i=0;i<cpu->canonical.size();++i) cpu->canonical[i]=rdram[(address+i)^3];
        s.cpu_scanouts.publish(std::move(cpu));
        ++s.cpu_swaps;
    }, [](const char* error) { session().fail(error); });
}
extern "C" void sc64_idle_poll(std::uint8_t* rdram) {
    sc64_pause_point();
    sc64::runtime::idle_boundary(rdram);
}
extern "C" void sc64_guest_poll(std::uint8_t* rdram) {
    sc64_pause_point();
    sc64::runtime::busy_wait_boundary(rdram);
}
extern "C" std::uint32_t sc64_ai_status(std::uint8_t* rdram) {
    sc64_pause_point();
    return sc64::runtime::ai_status_boundary(rdram, sc64::desktop::audio::status);
}
extern "C" void osViGetCurrentLine_recomp(std::uint8_t* rdram, recomp_context* ctx) {
    sc64_guest_poll(rdram);
    auto& s = sc64::desktop::session();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(s.clock.now() - s.started).count();
    ctx->r2 = sc64::runtime::vi_current_line(elapsed, s.fields.load());
}
