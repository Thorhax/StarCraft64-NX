#include "portraits.hpp"
#include "log.hpp"
#include "platform.hpp"
#if defined(SC64_VOICES)
#include "voices.hpp"
#endif
#include <json/json.hpp>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

namespace sc64::portraits {
namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned n64_width = 64, n64_height = 56;   // the N64's portrait texture
constexpr int match_limit = 24;                       // mean RGB difference of a match, 0..255
// The PC idle videos are fidgets: one plays every few seconds, the first frame holds between.
constexpr double fidget_min = 3.0, fidget_max = 8.0;

// A decoded video: frames of packed RGBA (r | g << 8 | b << 16 | a << 24).
struct Clip { unsigned width = 0, height = 0; double fps = 15; std::vector<std::vector<std::uint32_t>> frames; };
struct Portrait { std::vector<Clip> idle, talk; bool alias = false; }; // alias: same videos as an earlier one

std::string root;
std::vector<Portrait> portraits; // by portdata.dat index; filled by the loader
std::atomic<bool> ready{false};

Clip decode(const std::string& path) {
    Clip clip;
    AVFormatContext* format = nullptr;
    // "sdmc:/..." would otherwise be taken for an FFmpeg protocol name.
    const std::string url = path.starts_with("file:") ? path : "file:" + path;
    if (avformat_open_input(&format, url.c_str(), nullptr, nullptr) < 0) return clip;
    AVCodecContext* context = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* scale = nullptr;
    const int stream = avformat_find_stream_info(format, nullptr) < 0 ? -1
                       : av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream >= 0) {
        const AVStream* video = format->streams[stream];
        if (const AVCodec* codec = avcodec_find_decoder(video->codecpar->codec_id)) {
            context = avcodec_alloc_context3(codec);
            if (context && (avcodec_parameters_to_context(context, video->codecpar) < 0
                            || avcodec_open2(context, codec, nullptr) < 0)) avcodec_free_context(&context);
        }
        if (video->avg_frame_rate.num > 0 && video->avg_frame_rate.den > 0) clip.fps = av_q2d(video->avg_frame_rate);
    }
    auto receive = [&] {
        while (avcodec_receive_frame(context, frame) == 0) {
            scale = sws_getCachedContext(scale, frame->width, frame->height, AVPixelFormat(frame->format), frame->width,
                                         frame->height, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
            if (!scale) continue;
            clip.width = unsigned(frame->width); clip.height = unsigned(frame->height);
            std::vector<std::uint32_t> pixels(std::size_t(clip.width) * clip.height);
            std::uint8_t* planes[4] = {reinterpret_cast<std::uint8_t*>(pixels.data()), nullptr, nullptr, nullptr};
            int strides[4] = {int(clip.width * 4), 0, 0, 0};
            sws_scale(scale, frame->data, frame->linesize, 0, frame->height, planes, strides);
            clip.frames.push_back(std::move(pixels));
        }
    };
    if (context && packet && frame) {
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == stream && avcodec_send_packet(context, packet) >= 0) receive();
            av_packet_unref(packet);
        }
        avcodec_send_packet(context, nullptr);
        receive();
    }
    sws_freeContext(scale);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    avformat_close_input(&format);
    return clip;
}

void load() {
    std::ifstream file(root + "/portrait/index.json");
    if (!file) return;
    nlohmann::json index;
    try { index = nlohmann::json::parse(file); } catch (const std::exception&) { return; }
    std::vector<Portrait> loaded;
    std::set<std::string> first_idle; // portdata lists some portraits twice
    unsigned clips = 0;
    for (const auto& entry : index) {
        Portrait portrait;
        const auto idle = entry.value("idle", nlohmann::json::array());
        if (!idle.empty() && !first_idle.insert(idle.front().get<std::string>()).second) {
            portrait.alias = true;
            loaded.push_back(std::move(portrait));
            continue;
        }
        for (const auto& [kind, list] : {std::pair{"idle", &portrait.idle}, std::pair{"talk", &portrait.talk}})
            for (const auto& name : entry.value(kind, nlohmann::json::array())) {
                Clip clip = decode(root + "/" + name.get<std::string>());
                if (clip.frames.empty()) continue;
                list->push_back(std::move(clip));
                ++clips;
            }
        loaded.push_back(std::move(portrait));
    }
    portraits = std::move(loaded);
    ready.store(clips > 0, std::memory_order_release);
    sc64::shell::log::info("portraits: %zu PC portraits, %u videos", portraits.size(), clips);
}

// The PC portrait a still N64 one shows (by its first idle frame), the column the
// 60-wide PC frame starts at in the 64-wide N64 texture, and whether the N64 one is
// mirrored (it faces the text box); -1 when none matches.
struct Match { int portrait = -1; unsigned offset = 0; bool mirrored = false; double difference = 1e9; bool matched = false; };
Match identify(const gfx::Texture& tex) {
    Match best;
    double best_difference = 1e9, runner_up = 1e9; // runner_up: the best of the other portraits
    for (std::size_t p = 0; p < portraits.size(); ++p) {
        double portrait_best = 1e9;
        if (portraits[p].idle.empty() || portraits[p].alias) continue;
        const Clip& clip = portraits[p].idle.front();
        if (clip.height != n64_height || clip.width > n64_width) continue;
        const auto& reference = clip.frames.front();
        for (bool mirrored : {false, true})
            for (unsigned offset = 0; offset + clip.width <= n64_width; ++offset) {
                std::uint64_t sum = 0;
                for (unsigned y = 0; y < n64_height; ++y)
                    for (unsigned x = 0; x < clip.width; ++x) {
                        const gfx::Color c = tex.texel(std::size_t(y) * n64_width + offset + x);
                        const std::uint32_t r = reference[std::size_t(y) * clip.width + (mirrored ? clip.width - 1 - x : x)];
                        sum += std::uint64_t(std::abs(c.r - int(r & 255)) + std::abs(c.g - int(r >> 8 & 255))
                                             + std::abs(c.b - int(r >> 16 & 255)));
                    }
                const double difference = double(sum) / (3.0 * n64_height * clip.width);
                portrait_best = std::min(portrait_best, difference);
                if (difference < best_difference) {
                    if (best.portrait != int(p)) runner_up = std::min(runner_up, best_difference);
                    best_difference = difference; best = {int(p), offset, mirrored, difference};
                }
            }
        if (best.portrait != int(p)) runner_up = std::min(runner_up, portrait_best);
    }
    // A match is close and clearly closer than any other portrait.
    best.matched = best_difference <= match_limit && runner_up >= best_difference * 1.3;
    return best;
}

// What plays: the clip of the current portrait and when it started.
struct Playing {
    int portrait = -1;
    bool talking = false;
    const Clip* clip = nullptr;
    Clock::time_point started{}, next_fidget{};
};
std::mutex mutex;
std::unordered_map<std::uint64_t, Match> matches; // by N64 texture key
Playing playing;
std::mt19937 random_engine{0x5c64};

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

// The frame shown now for a portrait (the Gfx thread calls this once per task).
const std::vector<std::uint32_t>* current(int portrait, unsigned& width, std::uint64_t& id) {
    const Portrait& p = portraits[std::size_t(portrait)];
    const auto now = Clock::now();
#if defined(SC64_VOICES)
    const bool talking = !p.talk.empty() && sc64::voices::speaking();
#else
    const bool talking = false;
#endif
    auto pick = [&](const std::vector<Clip>& clips) {
        return &clips[std::uniform_int_distribution<std::size_t>(0, clips.size() - 1)(random_engine)];
    };
    auto fidget_later = [&] {
        playing.next_fidget = now + std::chrono::milliseconds(
            int(1000 * std::uniform_real_distribution<double>(fidget_min, fidget_max)(random_engine)));
    };
    if (playing.portrait != portrait || playing.talking != talking) {
        playing = {portrait, talking, talking ? pick(p.talk) : &p.idle.front(), now, {}};
        if (!talking) fidget_later();
    }
    const Clip* clip = playing.clip;
    std::size_t frame = std::size_t(seconds(now - playing.started) * clip->fps);
    if (talking) {
        if (frame >= clip->frames.size()) { // the next talking loop, another variant
            playing.clip = clip = pick(p.talk);
            playing.started = now;
            frame = 0;
        }
    } else if (frame >= clip->frames.size()) {
        frame = clip == &p.idle.front() ? 0 : clip->frames.size() - 1;
        if (clip != &p.idle.front()) { playing.clip = clip = &p.idle.front(); frame = 0; }
        if (now >= playing.next_fidget) { // a fidget, then the still first frame again
            playing.clip = clip = pick(p.idle);
            playing.started = now;
            fidget_later();
            frame = 0;
        }
    }
    width = clip->width;
    id = std::uint64_t(reinterpret_cast<std::uintptr_t>(clip)) * 1315423911u + frame;
    return &clip->frames[frame];
}
}

void initialize(const std::string& hd_root) {
    root = hd_root;
    std::error_code error;
    const auto parent = std::filesystem::path(hd_root).parent_path();
    if (std::filesystem::exists(parent / "nohd", error) || std::filesystem::exists(parent / "nohd.txt", error)) return;
    if (!std::filesystem::exists(root + "/portrait/index.json", error)) {
        sc64::shell::log::info("portraits: no PC portraits in %s/portrait", root.c_str());
        return;
    }
    if (!sc64_platform_thread(load)) sc64::shell::log::warning("portraits: loader thread failed");
}

void apply(gfx::DrawList& list) {
    if (!ready.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex);
    std::map<unsigned, bool> replaced; // texture index -> done
    for (const auto& r : list.rectangles) {
        if (r.texture >= list.textures.size() || replaced.count(r.texture)) continue;
        gfx::Texture& tex = list.textures[r.texture];
        if (!tex.background || tex.width != n64_width || tex.height != n64_height
            || tex.texel_count() != std::size_t(n64_width) * n64_height) continue;
        auto found = matches.find(tex.key);
        if (found == matches.end()) {
            found = matches.emplace(tex.key, identify(tex)).first;
            sc64::shell::log::info("portraits: N64 portrait %s PC portrait %d (difference %.1f%s)",
                                   found->second.matched ? "shown as" : "kept, nearest", found->second.portrait,
                                   found->second.difference, found->second.mirrored ? ", mirrored" : "");
        }
        Match match = found->second;
        if (!match.matched) continue;
        unsigned width = 0;
        std::uint64_t id = 0;
        const auto* pixels = current(match.portrait, width, id);
        if (!pixels || width + match.offset > n64_width) continue;
        // The N64 texels with the PC frame over the columns it matched.
        std::vector<gfx::Color> texels(std::size_t(n64_width) * n64_height);
        for (std::size_t i = 0; i < texels.size(); ++i) texels[i] = tex.texel(i);
        for (unsigned y = 0; y < n64_height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                const std::uint32_t c = (*pixels)[std::size_t(y) * width + (match.mirrored ? width - 1 - x : x)];
                texels[std::size_t(y) * n64_width + match.offset + x] =
                    {int(c & 255), int(c >> 8 & 255), int(c >> 16 & 255), 255};
            }
        tex.pixels = std::move(texels);
        tex.packed.reset();
        tex.key = (tex.key ^ id) * 0x100000001b3ull; // a new image: GPU caches key by content
        replaced[r.texture] = true;
    }
}
}
