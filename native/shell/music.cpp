#include "music.hpp"
#include "log.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace sc64::music {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int output_rate = 48000;
constexpr std::size_t ring_frames = output_rate * 3 / 2; // 1.5 s decoded ahead
constexpr std::int64_t silence_ns = 400'000'000;         // a stream not mixed for this long has stopped
constexpr float fade_step = 1.0f / (output_rate * 0.35f); // 350 ms fades
// The guest mixes its music at about this voice volume (16.16) at the default
// settings, where its title music reaches the output at a quarter of the PC
// title's level (RMS, before the shell's game gain). The PC track plays at that
// level and follows the guest's volume (options, fades) proportionally.
constexpr float reference_volume = 31334.0f / 65536.0f, reference_gain = 0.25f;
// The PC tracks are mastered near full scale (Vorbis peaks up to ~1.12): at the
// default game gain (4) they play at 0.85 so their peaks stay below the output's
// safety limiter instead of being bent or clipped.
constexpr float headroom = 0.85f;

enum class Kind : std::uint8_t { title, theme, jingle };
enum Race : std::uint8_t { none, terran, zerg, protoss };
// BOLT directory 015, StarCraft 64 USA: stream start (ROM, after the 12-byte
// header) and the PC track it was cut from (waveform-matched; the Brood War
// themes by chroma). Leaves 1, 14 and 15 have no PC counterpart and stay N64.
struct Track { std::uint32_t rom, bytes; const char* file; Kind kind; Race race; };
constexpr std::array<Track, 13> tracks{{
    {0x15d9012, 0x45a48, "title", Kind::title, none},
    {0x1633ff2, 0x13c00, "tvict", Kind::jingle, terran},
    {0x1647bfe, 0x15b00, "tdefeat", Kind::jingle, terran},
    {0x165d70a, 0x83332, "terran3", Kind::theme, terran},
    {0x16e0a48, 0xa9796, "bwterran", Kind::theme, terran},
    {0x178a1ea, 0x14380, "zvict", Kind::jingle, zerg},
    {0x179e576, 0x12e3c, "zdefeat", Kind::jingle, zerg},
    {0x17b13be, 0x8b132, "zerg1", Kind::theme, zerg},
    {0x183c4fc, 0x8c58a, "bwzerg", Kind::theme, zerg},
    {0x18c8a92, 0x12b9e, "pvict", Kind::jingle, protoss},
    {0x18db63c, 0x11a32, "pdefeat", Kind::jingle, protoss},
    {0x18ed07a, 0x86b10, "protoss1", Kind::theme, protoss},
    {0x1973b96, 0x98598, "protoss2", Kind::theme, protoss},
}};
// PC mission music per race, in the order StarCraft plays it. A Brood War theme
// leads its race's list, as Brood War's own terran1/zerg1/protoss1 do.
const char* const playlists[4][4] = {
    {}, {"terran1", "terran2", "terran3"}, {"zerg1", "zerg2", "zerg3"}, {"protoss1", "protoss2", "protoss3"}};
const char* const brood_war[4] = {nullptr, "bwterran", "bwzerg", "bwprotoss"};

std::string root;
bool enabled = false;
std::array<bool, tracks.size()> available{};

// Guest audio thread -> decoder thread.
std::atomic<int> heard{-1};             // track index of the latest muted stream
std::atomic<std::int64_t> heard_at{0};  // steady-clock ns
std::atomic<float> guest_gain{0};

// Decoder thread <-> audio thread.
std::mutex ring_mutex;
std::vector<float> ring(ring_frames * 2);
std::size_t ring_read = 0, ring_size = 0; // in frames
std::atomic<bool> fading_out{false}, silent{true};
float fade = 0, gain = 0;

std::thread worker;
std::atomic<bool> quit{false};

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
std::string path_of(const std::string& name) { return root + "/" + name + ".ogg"; }
bool exists(const std::string& name) {
    std::error_code error;
    return std::filesystem::is_regular_file(path_of(name), error);
}

class Decoder {
public:
    ~Decoder() { close(); }
    bool open(const std::string& path) {
        close();
        const std::string url = path.starts_with("file:") ? path : "file:" + path;
        if (avformat_open_input(&format_, url.c_str(), nullptr, nullptr) < 0
            || avformat_find_stream_info(format_, nullptr) < 0) return fail(path);
        stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (stream_ < 0) return fail(path);
        const AVCodecParameters* parameters = format_->streams[stream_]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec || !(codec_ = avcodec_alloc_context3(codec))) return fail(path);
        codec_->thread_count = 1;
        if (avcodec_parameters_to_context(codec_, parameters) < 0 || avcodec_open2(codec_, codec, nullptr) < 0)
            return fail(path);
        packet_ = av_packet_alloc(); frame_ = av_frame_alloc();
        eof_ = false;
        return packet_ && frame_;
    }
    void close() {
        swr_free(&resample_);
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&codec_);
        avformat_close_input(&format_);
    }
    bool is_open() const { return format_ != nullptr; }
    bool ended() const { return eof_; }
    // Decodes about one packet into out (stereo 48 kHz float). False at the end.
    bool decode(std::vector<float>& out) {
        out.clear();
        while (!eof_ && out.empty()) {
            if (av_read_frame(format_, packet_) < 0) { eof_ = true; avcodec_send_packet(codec_, nullptr); }
            else if (packet_->stream_index != stream_) { av_packet_unref(packet_); continue; }
            else { avcodec_send_packet(codec_, packet_); av_packet_unref(packet_); }
            while (avcodec_receive_frame(codec_, frame_) == 0) { convert(out); av_frame_unref(frame_); }
        }
        return !out.empty();
    }
private:
    bool fail(const std::string& path) {
        sc64::shell::log::warning("music: cannot play %s", path.c_str());
        close();
        return false;
    }
    void convert(std::vector<float>& out) {
        if (!resample_) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&resample_, &stereo, AV_SAMPLE_FMT_FLT, output_rate, &frame_->ch_layout,
                                    AVSampleFormat(frame_->format), frame_->sample_rate, 0, nullptr) < 0
                || swr_init(resample_) < 0) { swr_free(&resample_); eof_ = true; return; }
        }
        const int capacity = swr_get_out_samples(resample_, frame_->nb_samples);
        if (capacity <= 0) return;
        const std::size_t base = out.size();
        out.resize(base + std::size_t(capacity) * 2);
        std::uint8_t* planes[1] = {reinterpret_cast<std::uint8_t*>(out.data() + base)};
        const int got = swr_convert(resample_, planes, capacity, const_cast<const std::uint8_t**>(frame_->extended_data),
                                    frame_->nb_samples);
        out.resize(base + std::size_t(std::max(got, 0)) * 2);
    }
    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    SwrContext* resample_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int stream_ = -1;
    bool eof_ = false;
};

std::size_t ring_space() {
    std::lock_guard lock(ring_mutex);
    return ring_frames - ring_size;
}
void ring_push(const float* samples, std::size_t frames) {
    std::lock_guard lock(ring_mutex);
    for (std::size_t f = 0; f < frames && ring_size < ring_frames; ++f, ++ring_size) {
        const std::size_t at = (ring_read + ring_size) % ring_frames;
        ring[at * 2] = samples[f * 2]; ring[at * 2 + 1] = samples[f * 2 + 1];
    }
}
void ring_clear() {
    std::lock_guard lock(ring_mutex);
    ring_read = ring_size = 0;
}

// Fades the current track out (audio thread) and empties the ring.
void fade_out() {
    if (silent.load()) { ring_clear(); return; }
    fading_out = true;
    for (int i = 0; i < 60 && !silent.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ring_clear();
    fading_out = false;
}

void run() {
    Decoder decoder;
    std::vector<float> chunk;
    std::size_t chunk_at = 0;
    int guest_track = -1;      // the guest's stream we are standing in for
    std::string playing;       // file being decoded
    Kind kind = Kind::title;
    Race race = none;
    unsigned list_index = 0;   // position in the race playlist
    bool brood = false;
    auto start = [&](const std::string& name) {
        decoder.open(path_of(name));
        playing = decoder.is_open() ? name : std::string();
        chunk.clear(); chunk_at = 0;
        if (!playing.empty()) sc64::shell::log::info("music: %s", name.c_str());
    };
    auto stop = [&] {
        if (!playing.empty()) { fade_out(); sc64::shell::log::info("music: stopped %s", playing.c_str()); }
        decoder.close(); playing.clear(); guest_track = -1; chunk.clear(); chunk_at = 0;
    };
    // The race list after the theme the guest chose: Brood War theme first when chosen.
    auto next_theme = [&]() -> std::string {
        for (unsigned tries = 0; tries < 4; ++tries) {
            const unsigned size = brood ? 4 : 3;
            list_index = (list_index + 1) % size;
            const char* name = brood ? (list_index == 0 ? brood_war[race] : playlists[race][list_index - 1])
                                     : playlists[race][list_index];
            if (name && exists(name)) return name;
        }
        return playing;
    };
    while (!quit.load()) {
        const int seen = heard.load();
        const bool live = seen >= 0 && now_ns() - heard_at.load() < silence_ns;
        if (!live) {
            // A theme or the title stops with its stream; a jingle plays out once.
            if (!playing.empty() && kind != Kind::jingle) stop();
            guest_track = -1;
        } else if (seen != guest_track) {
            const Track& track = tracks[std::size_t(seen)];
            const bool same_list = kind == Kind::theme && track.kind == Kind::theme && track.race == race && !playing.empty();
            guest_track = seen;
            if (!same_list) {
                // A theme change within the race's list waits for the PC theme to end.
                if (!playing.empty()) fade_out();
                kind = track.kind; race = track.race;
                brood = std::string_view(track.file).starts_with("bw");
                list_index = 0;
                if (kind == Kind::theme && !brood)
                    for (unsigned i = 0; i < 3; ++i)
                        if (std::string_view(playlists[race][i]) == track.file) list_index = i;
                start(track.file);
            }
        }
        if (playing.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        // Keep the ring filled.
        bool progress = false;
        while (ring_space() > 4096 && !quit.load()) {
            if (chunk_at >= chunk.size()) {
                chunk_at = 0;
                if (!decoder.decode(chunk)) {
                    chunk.clear();
                    if (kind == Kind::jingle) {
                        if (ring_space() == ring_frames) { // played out
                            sc64::shell::log::info("music: %s ended", playing.c_str());
                            decoder.close(); playing.clear();
                        }
                        break;
                    }
                    start(kind == Kind::theme ? next_theme() : playing); // themes advance, the title loops
                    if (playing.empty()) break;
                    continue;
                }
            }
            const std::size_t frames = std::min((chunk.size() - chunk_at) / 2, ring_space());
            ring_push(chunk.data() + chunk_at, frames);
            chunk_at += frames * 2;
            progress = true;
        }
        if (!progress) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
}

void initialize(const std::string& directory) {
    root = directory;
    std::error_code error;
    const auto parent = std::filesystem::path(directory).parent_path();
    if (std::filesystem::exists(parent / "n64music", error) || std::filesystem::exists(parent / "n64music.txt", error)) {
        sc64::shell::log::info("music: n64music file present, N64 music kept");
        return;
    }
    unsigned count = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) count += available[i] = exists(tracks[i].file);
    if (!count) {
        sc64::shell::log::info("music: no PC music in %s, N64 music kept", directory.c_str());
        return;
    }
    av_log_set_level(AV_LOG_ERROR);
    enabled = true;
    worker = std::thread(run);
    sc64::shell::log::info("music: PC music for %u of %zu N64 tracks from %s", count, tracks.size(), directory.c_str());
}

void shutdown() {
    quit = true;
    if (worker.joinable()) worker.join();
}

bool guest_voice(std::uint32_t rom, std::int32_t left, std::int32_t right) {
    if (!enabled) return false;
    rom &= 0x0fffffff;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (rom < tracks[i].rom || rom >= tracks[i].rom + tracks[i].bytes) continue;
        if (!available[i]) return false;
        const float volume = float(std::max(std::abs(left), std::abs(right))) / 65536.0f;
        guest_gain.store(volume / reference_volume * reference_gain, std::memory_order_relaxed);
        heard.store(int(i), std::memory_order_relaxed);
        heard_at.store(now_ns(), std::memory_order_release);
        return true;
    }
    return false;
}

void mix(float* stereo, unsigned frames, float game_gain) {
    if (!enabled) return;
    std::lock_guard lock(ring_mutex);
    const float target_gain = guest_gain.load(std::memory_order_relaxed) * game_gain * headroom;
    const bool out = fading_out.load(std::memory_order_relaxed);
    for (unsigned f = 0; f < frames; ++f) {
        if (!ring_size) { fade = 0; break; }
        fade = out ? std::max(0.0f, fade - fade_step) : std::min(1.0f, fade + fade_step);
        gain += (target_gain - gain) * 0.0005f; // follows guest fades and volume changes
        const float g = fade * gain;
        stereo[f * 2] += ring[ring_read * 2] * g; // the output limits the whole mix
        stereo[f * 2 + 1] += ring[ring_read * 2 + 1] * g;
        ring_read = (ring_read + 1) % ring_frames;
        --ring_size;
    }
    silent.store(fade == 0, std::memory_order_relaxed);
}
}
