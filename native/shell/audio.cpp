#include "audio.hpp"
#include "session.hpp"
#include "pcm_playback.hpp"
#include "pcm_inbox.hpp"
#include "pcm_trace.hpp"
#include "pcm_output.hpp"
#include "audio_timing.hpp"
#include "json/json.hpp"
#if defined(__ANDROID__)
#include <aaudio/AAudio.h>
#endif
#include <miniaudio.h>
#if defined(__SWITCH__)
#include <switch.h>
#include <cstdlib>
#include <cstring>
#include <span>
#include <thread>
#endif
#include "log.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <fstream>
#include <bit>
#include <string_view>

namespace sc64::desktop::audio {
namespace {
ma_device device{};
bool initialized = false;
bool compatibility_output = false;
std::atomic<int> active_backend{-1};
std::atomic<bool> menu_latency{true}, recover_menu{false};
unsigned latency_floor_ms() {
#if defined(__ANDROID__)
    if (compatibility_output && menu_latency.load()) return 160;
#endif
    return 60;
}
std::mutex mutex;
constexpr unsigned pcm_capacity = 28802;
PcmPlayback<pcm_capacity> pcm; // Device-thread-owned; no producer touches this state.
PcmInbox<pcm_capacity> inbox;
std::atomic<std::uint64_t> stream_state{(1ull << 32) | 48000u};
std::atomic<unsigned> pre_roll_frames{2880}, dma_frames{0};
std::atomic<int> device_xruns{-1}, device_buffer_frames{0};
std::uint64_t applied_stream = 0; // Device thread only.
std::deque<PauseClock::Clock::time_point> dma_ends;
unsigned source_rate = 48000;
std::filesystem::path trace_out;
std::uint64_t trace_from = 0, trace_to = 0;
PcmTraceLane source_trace, output_trace;
auto trace_start = std::chrono::steady_clock::now();
bool tracing(std::uint64_t field) { return trace_from && field >= trace_from && field <= trace_to; }
unsigned frame_limit(unsigned rate) {
    unsigned ms=200;
#if defined(__ANDROID__)
    if (compatibility_output) ms=300;
#endif
    // Reserve room for two late DMAs above the menu pre-roll. A 200 ms cap
    // alongside a 160 ms target discarded healthy late bursts on Android.
    return std::min(pcm_capacity,rate*ms/1000u);
}
double trace_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - trace_start).count();
}
void export_trace() {
    if (trace_out.empty()) return;
    static_assert(std::endian::native == std::endian::little);
    std::filesystem::create_directories(trace_out);
    nlohmann::json report{{"format", "f32le-stereo"}, {"from",trace_from}, {"to",trace_to}};
    for (auto [name,lane] : {std::pair{"source",&source_trace}, std::pair{"output",&output_trace}}) {
        std::ofstream data(trace_out/(std::string(name)+".f32"), std::ios::binary);
        data.exceptions(std::ios::failbit|std::ios::badbit);
        data.write(reinterpret_cast<const char*>(lane->samples().data()),lane->samples().size_bytes());
        auto& record=report[name]; record["truncated"]=lane->truncated();
        record["frames"]=lane->samples().size()/2; record["blocks"]=nlohmann::json::array();
        for (const auto& b:lane->blocks()) record["blocks"].push_back({
            {"field",b.field},{"stream",b.stream},{"seconds",b.seconds},
            {"offset",b.offset},{"frames",b.frames},{"rate",b.rate},
            {"played",b.played},{"starved",b.starved},{"correction",b.correction}});
    }
    std::ofstream meta(trace_out/"audio.json"); meta.exceptions(std::ios::failbit|std::ios::badbit);
    meta<<report.dump(2)<<'\n'; trace_out.clear();
}

std::atomic<std::uint64_t> underrun_count{0}, dropped_count{0};
std::atomic<std::uint64_t> played_count{0}, nonzero_count{0};
std::atomic<std::uint64_t> wait_count{0};
std::atomic<unsigned> buffered_frames{0}, reported_rate{48000};
std::atomic<bool> buffering{true};
std::atomic<std::uint64_t> timing_epoch{0}, source_frames{0}, source_nonzero_frames{0};
AudioIntervalPeak source_intervals, callback_intervals;
AudioPeak callback_work;
std::uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void retire() {
    auto now = session().clock.now();
    while (!dma_ends.empty() && dma_ends.front() <= now) dma_ends.pop_front();
}
void mix(float* dst, ma_uint32 frames) {
    std::fill_n(dst, frames * 2, 0.0f);
    if (session().paused.load()) return;
    auto apply = [&](std::uint64_t state) {
        if (state != applied_stream) { pcm.clear(); applied_stream = state; }
    };
    apply(stream_state.load(std::memory_order_acquire));
    PcmFrame frame;
    // A bounded inbox drain; a running producer cannot extend this callback
    // indefinitely. Rate generations prevent old PCM crossing a rate change.
    for (unsigned i = 0; i < pcm_capacity && inbox.pop(frame); ++i) {
        if (frame.stream < applied_stream) continue;
        apply(frame.stream);
        const auto rate = unsigned(applied_stream);
        if (pcm.size() >= frame_limit(rate)) { pcm.pop(); ++dropped_count; }
        pcm.push({frame.left, frame.right});
    }
    const auto rate = unsigned(applied_stream);
    if (recover_menu.exchange(false)) pcm.rebuffer();
    const auto target = std::clamp(pre_roll_frames.load(), rate * latency_floor_ms() / 1000u, rate * 160u / 1000u);
    const auto result = pcm.render(std::span<float>(dst, std::size_t(frames) * 2), rate, target);
    const auto field = session().fields.load();
    if (tracing(field)) output_trace.append({field,applied_stream,trace_seconds(),pcm.correction(),
        0,0,48000,result.played,result.starved},frames,[&](std::size_t i) { return dst[i]; });
    if (result.starved) ++underrun_count;
    if (result.waiting) ++wait_count;
    played_count += result.played; nonzero_count += result.nonzero;
    buffered_frames = unsigned(pcm.size()); buffering = pcm.buffering();
}
void callback(ma_device* sink, void* output, const void*, ma_uint32 frames) {
    const auto begin = monotonic_ns();
    callback_intervals.observe(begin, timing_epoch.load(std::memory_order_relaxed));
#if defined(__ANDROID__)
    if (sink->pContext->backend == ma_backend_aaudio && sink->aaudio.pStreamPlayback) {
        auto* stream = static_cast<AAudioStream*>(sink->aaudio.pStreamPlayback);
        device_xruns = AAudioStream_getXRunCount(stream);
        device_buffer_frames = AAudioStream_getBufferSizeInFrames(stream);
    }
#endif
    if (sink->playback.format == ma_format_f32) {
        mix(static_cast<float*>(output),frames);
    } else {
        // Signed stereo PCM, the format the compatibility OpenSL profile was measured with.
        // Bound scratch space independently of the backend's callback size.
        render_pcm16(std::span<std::int16_t>(static_cast<std::int16_t*>(output),std::size_t(frames)*2),mix);
    }
    callback_work.record(monotonic_ns() - begin);
}
#if defined(__SWITCH__)
// Switch output: libnx audout, 48 kHz stereo s16, three 1024-frame buffers
// (~64 ms queued). The thread refills each released buffer with the same mix
// the miniaudio callback uses elsewhere.
constexpr unsigned switch_frames = 1024, switch_buffers = 3;
std::thread switch_thread;
std::atomic<bool> switch_running{false};
void switch_loop() {
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2A); // ahead of the shell (0x2C) and raster helpers
    constexpr std::size_t bytes = switch_frames * 2 * sizeof(std::int16_t); // 4096 = audout's 0x1000 alignment
    std::array<AudioOutBuffer, switch_buffers> buffers{};
    std::array<void*, switch_buffers> memory{};
    for (unsigned i = 0; i < switch_buffers; ++i) {
        memory[i] = std::aligned_alloc(0x1000, bytes);
        std::memset(memory[i], 0, bytes);
        buffers[i].buffer = memory[i];
        buffers[i].buffer_size = bytes;
        buffers[i].data_size = bytes;
        audoutAppendAudioOutBuffer(&buffers[i]);
    }
    while (switch_running.load()) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100'000'000ULL)) || !released) continue;
        const auto begin = monotonic_ns();
        callback_intervals.observe(begin, timing_epoch.load(std::memory_order_relaxed));
        render_pcm16(std::span<std::int16_t>(static_cast<std::int16_t*>(released->buffer), switch_frames * 2), mix);
        armDCacheFlush(released->buffer, bytes);
        callback_work.record(monotonic_ns() - begin);
        audoutAppendAudioOutBuffer(released);
    }
    audoutStopAudioOut();
    for (void* block : memory) std::free(block);
}
#endif
}
Timing take_timing() {
    return {source_intervals.take_ms(), callback_intervals.take_ms(), callback_work.take_ms(),
        source_frames.load(), source_nonzero_frames.load()};
}
void pause_changed() { timing_epoch.fetch_add(1, std::memory_order_relaxed); }
void configure(const char* requested) {
    if (initialized) throw std::runtime_error("Configure the audio backend before starting the device");
    const std::string_view name=requested;
    if (name!="compatibility" && name!="aaudio")
        throw std::runtime_error("Audio backend must be compatibility or aaudio");
    compatibility_output=name=="compatibility";
}
const char* backend() {
#if defined(__SWITCH__)
    if (initialized) return "audout";
#endif
    const auto value=active_backend.load();
    return value<0 ? "none" : ma_get_backend_name(static_cast<ma_backend>(value));
}
void menu_profile(bool menu) {
#if defined(__ANDROID__)
    if (compatibility_output && menu_latency.exchange(menu)!=menu && menu) recover_menu=true;
#else
    (void)menu;
#endif
}
void trace(const std::filesystem::path& output, std::uint64_t from, std::uint64_t to) {
    if (initialized || !trace_out.empty() || !from || to<from || to-from>600)
        throw std::runtime_error("Audio trace requires an idle device and at most 600 fields");
    trace_out=output; trace_from=from; trace_to=to;
    source_trace.initialize(96000 * 12); output_trace.initialize(48000 * 12);
    trace_start=std::chrono::steady_clock::now();
}
void initialize() {
    if (initialized) return;
#if defined(__SWITCH__)
    if (R_FAILED(audoutInitialize())) throw std::runtime_error("audout initialization failed");
    if (R_FAILED(audoutStartAudioOut())) { audoutExit(); throw std::runtime_error("audout start failed"); }
    device_xruns = -1;
    device_buffer_frames = int(switch_frames * switch_buffers);
    switch_running = true;
    switch_thread = std::thread(switch_loop);
    initialized = true;
    sc64::shell::log::info("audout: %u Hz stereo s16, %u x %u frames", audoutGetSampleRate(), switch_buffers, switch_frames);
    return;
#endif
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 48000;
    config.periodSizeInFrames = 512;
    config.periods = 4;
    config.dataCallback = callback;
#if defined(__ANDROID__)
    if (compatibility_output) {
        config.playback.format = ma_format_s16;
        config.periodSizeInFrames = 1024;
        config.performanceProfile = ma_performance_profile_conservative;
        config.aaudio.usage = ma_aaudio_usage_game;
        config.aaudio.contentType = ma_aaudio_content_type_music;
    }
    config.opensl.streamType = ma_opensl_stream_type_media;
    const ma_backend compatible[] = {ma_backend_opensl,ma_backend_aaudio};
    const ma_backend low_latency[] = {ma_backend_aaudio};
    const auto opened = ma_device_init_ex(compatibility_output ? compatible : low_latency,
        compatibility_output ? 2 : 1,nullptr,&config,&device);
#else
    const auto opened = ma_device_init(nullptr, &config, &device);
#endif
    if (opened != MA_SUCCESS)
        throw std::runtime_error("Could not open a miniaudio playback device");
    active_backend = int(device.pContext->backend);
    device_xruns=-1;
    device_buffer_frames=int(device.playback.internalPeriodSizeInFrames*device.playback.internalPeriods);
#if defined(__ANDROID__)
    if (device.pContext->backend == ma_backend_aaudio && device.aaudio.pStreamPlayback) {
        auto* stream = static_cast<AAudioStream*>(device.aaudio.pStreamPlayback);
        const auto capacity = AAudioStream_getBufferCapacityInFrames(stream);
        const auto burst = AAudioStream_getFramesPerBurst(stream);
        const auto requested = std::min(capacity, std::max(4 * burst, 1536));
        const auto actual = AAudioStream_setBufferSizeInFrames(stream, requested);
        sc64::shell::log::info("AAudio reserve: %d frames, burst %d, capacity %d", actual, burst, capacity);
    }
#endif
    if (ma_device_start(&device) != MA_SUCCESS) {
        ma_device_uninit(&device);
        throw std::runtime_error("Could not start miniaudio playback");
    }
    initialized = true;
    sc64::shell::log::info("miniaudio %s: %s, host %u Hz stereo, device %u Hz, format %s, periods %u x %u",
        ma_get_backend_name(device.pContext->backend), device.playback.name,
        device.sampleRate, device.playback.internalSampleRate,
        ma_get_format_name(device.playback.format),
        device.playback.internalPeriodSizeInFrames,device.playback.internalPeriods);
}
void shutdown() {
#if defined(__SWITCH__)
    if (initialized) {
        switch_running = false;
        if (switch_thread.joinable()) switch_thread.join();
        audoutExit();
    }
#else
    if (initialized) ma_device_uninit(&device);
#endif
    initialized = false;
    active_backend=-1;
    try { export_trace(); }
    catch (const std::exception& error) { sc64::shell::log::error("Audio trace export: %s",error.what()); trace_out.clear(); }
}
void frequency(std::uint32_t rate) {
    if (rate < 1000 || rate > 96000) throw std::runtime_error("Invalid AI frequency");
    std::lock_guard lock(mutex);
    if (rate != source_rate) {
        pause_changed();
        dma_ends.clear(); pre_roll_frames = rate * 60u / 1000u;
        const auto generation = (stream_state.load() >> 32) + 1;
        stream_state.store((generation << 32) | rate, std::memory_order_release);
    }
    source_rate = rate; reported_rate = rate;
}
void queue(const std::int16_t* samples, std::size_t count) {
    if (count % 2) throw std::runtime_error("Invalid stereo AI buffer");
    std::lock_guard lock(mutex);
    source_intervals.observe(monotonic_ns(), timing_epoch.load(std::memory_order_relaxed));
    retire();
    if (dma_ends.size() >= 2) throw std::runtime_error("AI two-buffer FIFO overflow");
    auto begin = dma_ends.empty() ? session().clock.now() : dma_ends.back();
    dma_ends.push_back(begin + std::chrono::nanoseconds((count / 2) * 1'000'000'000ull / source_rate));
    dma_frames = unsigned(count / 2);
    // Two source DMA blocks cover their delivery cadence; short blocks
    // retain the 60 ms floor, large blocks can use up to 160 ms.
    pre_roll_frames = unsigned(std::min<std::size_t>(count >= 2 ? count - 2 : 0, source_rate * 160u / 1000u));
    const auto state = stream_state.load(std::memory_order_acquire);
    const auto field = session().fields.load();
    if (tracing(field)) source_trace.append({field,state,trace_seconds(),0,0,0,source_rate},
        count/2,[&](std::size_t i) { return samples[i^1]/32768.f; });
    std::uint64_t nonzero = 0;
    for (std::size_t i = 0; i < count; i += 2) {
        nonzero += samples[i ^ 1] != 0 || samples[(i + 1) ^ 1] != 0;
        if (inbox.pending() >= frame_limit(source_rate) || !inbox.push(
                {samples[i ^ 1] / 32768.0f, samples[(i + 1) ^ 1] / 32768.0f, state})) ++dropped_count;
    }
    source_frames += count / 2; source_nonzero_frames += nonzero;
    session().samples += count;
    // Device stalls never grow memory indefinitely. Account for every discarded
    // sample; acceptance requires zero sustained overruns after startup.
}
std::size_t remaining() {
    std::lock_guard lock(mutex);
    retire();
    if (dma_ends.empty()) return 0;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dma_ends.back() - session().clock.now()).count();
    return ns > 0 ? (std::uint64_t(ns) * source_rate + 999'999'999) / 1'000'000'000 : 0;
}
std::uint32_t status() {
    std::lock_guard lock(mutex);
    retire();
    return (dma_ends.empty() ? 0u : 0x40000000u) | (dma_ends.size() == 2 ? 0x80000000u : 0u);
}
std::uint64_t underruns() { return underrun_count.load(); }
std::uint64_t dropped() { return dropped_count.load(); }
std::uint64_t played() { return played_count.load(); }
std::uint64_t nonzero() { return nonzero_count.load(); }
std::uint64_t waits() { return wait_count.load(); }
double buffered_ms() { return 1000.0 * buffered_frames.load() / reported_rate.load(); }
bool rebuffering() { return buffering.load(); }
unsigned source_hz() { return reported_rate.load(); }
double dma_ms() { return 1000.0 * dma_frames.load() / reported_rate.load(); }
double target_ms() { return std::clamp(1000.0 * pre_roll_frames.load() / reported_rate.load(),double(latency_floor_ms()),160.0); }
int hardware_xruns() { return device_xruns.load(); }
int hardware_buffer_frames() { return device_buffer_frames.load(); }
}
