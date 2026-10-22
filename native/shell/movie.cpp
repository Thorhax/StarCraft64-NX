#include "movie.hpp"
#include "log.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>

namespace sc64::movie {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int output_rate = 48000;
constexpr double audio_ahead = 0.5, audio_limit = 2.0; // seconds of soundtrack buffered ahead / at most
constexpr std::size_t video_ahead = 8;                  // frames decoded ahead of playback
constexpr unsigned packets_per_update = 64;

struct Queued { double time; std::shared_ptr<const std::vector<std::uint8_t>> rgba; };

class Player {
public:
    ~Player() {
        sws_freeContext(scale_);
        swr_free(&resample_);
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&video_);
        avcodec_free_context(&audio_);
        avformat_close_input(&format_);
    }
    bool open(const std::string& path);
    void fill();
    std::optional<Frame> present(double now);
    bool done(double now) const;
    double length() const { return last_time_; }
    unsigned shown() const { return shown_; }
private:
    bool decode(AVCodecContext* context, const AVPacket* packet);
    void queue_video();
    void queue_audio();
    AVFormatContext* format_ = nullptr;
    AVCodecContext* video_ = nullptr;
    AVCodecContext* audio_ = nullptr;
    int video_stream_ = -1, audio_stream_ = -1;
    SwsContext* scale_ = nullptr;
    SwrContext* resample_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    unsigned width_ = 0, height_ = 0; // presented size
    std::deque<Queued> frames_;
    double last_time_ = 0, frame_step_ = 1.0 / 15;
    unsigned shown_ = 0;
    bool eof_ = false;
};
std::unique_ptr<Player> player;
Clock::time_point started;

// Soundtrack at 48 kHz stereo: filled on the main thread, drained by mix().
std::mutex audio_mutex;
std::deque<float> ring;
bool audio_active = false;

double buffered_audio() {
    std::lock_guard lock(audio_mutex);
    return double(ring.size()) / (2.0 * output_rate);
}

AVCodecContext* open_decoder(const AVStream* stream) {
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) return nullptr;
    context->thread_count = 1;
    if (avcodec_parameters_to_context(context, stream->codecpar) < 0 || avcodec_open2(context, codec, nullptr) < 0)
        avcodec_free_context(&context);
    return context;
}

bool Player::open(const std::string& path) {
    // "sdmc:/..." would otherwise be taken for an FFmpeg protocol name.
    const std::string url = path.starts_with("file:") ? path : "file:" + path;
    if (avformat_open_input(&format_, url.c_str(), nullptr, nullptr) < 0
        || avformat_find_stream_info(format_, nullptr) < 0) return false;
    video_stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_ < 0 || !(video_ = open_decoder(format_->streams[video_stream_]))) return false;
    audio_stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, video_stream_, nullptr, 0);
    if (audio_stream_ >= 0 && !(audio_ = open_decoder(format_->streams[audio_stream_]))) audio_stream_ = -1;
    if (!(packet_ = av_packet_alloc()) || !(frame_ = av_frame_alloc())) return false;
    const AVStream* stream = format_->streams[video_stream_];
    width_ = unsigned(video_->width); height_ = unsigned(video_->height);
    if (stream->sample_aspect_ratio.num > 0 && stream->sample_aspect_ratio.den > 0)
        width_ = unsigned(av_rescale(width_, stream->sample_aspect_ratio.num, stream->sample_aspect_ratio.den));
    // Classic StarCraft stores its cinematics at half width (320x308) and shows
    // them horizontally doubled in a 640x480 letterbox.
    if (video_->codec_id == AV_CODEC_ID_SMACKVIDEO && width_ <= 320) width_ *= 2;
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) frame_step_ = av_q2d(av_inv_q(stream->avg_frame_rate));
    return width_ && height_;
}

void Player::queue_video() {
    scale_ = sws_getCachedContext(scale_, frame_->width, frame_->height, AVPixelFormat(frame_->format),
                                  int(width_), int(height_), AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scale_) return;
    auto rgba = std::make_shared<std::vector<std::uint8_t>>(std::size_t(width_) * height_ * 4);
    std::uint8_t* planes[4] = {rgba->data(), nullptr, nullptr, nullptr};
    int strides[4] = {int(width_ * 4), 0, 0, 0};
    sws_scale(scale_, frame_->data, frame_->linesize, 0, frame_->height, planes, strides);
    const std::int64_t pts = frame_->best_effort_timestamp;
    const double time = pts == AV_NOPTS_VALUE ? (frames_.empty() ? last_time_ : frames_.back().time) + frame_step_
                                              : double(pts) * av_q2d(format_->streams[video_stream_]->time_base);
    last_time_ = std::max(last_time_, time);
    frames_.push_back({time, std::move(rgba)});
}

void Player::queue_audio() {
    if (!resample_) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&resample_, &stereo, AV_SAMPLE_FMT_FLT, output_rate, &frame_->ch_layout,
                                AVSampleFormat(frame_->format), frame_->sample_rate, 0, nullptr) < 0
            || swr_init(resample_) < 0) {
            sc64::shell::log::warning("movie: unsupported soundtrack format, playing silent");
            swr_free(&resample_);
            audio_stream_ = -1;
            return;
        }
    }
    const int capacity = swr_get_out_samples(resample_, frame_->nb_samples);
    if (capacity <= 0) return;
    std::vector<float> samples(std::size_t(capacity) * 2);
    std::uint8_t* out[1] = {reinterpret_cast<std::uint8_t*>(samples.data())};
    const int got = swr_convert(resample_, out, capacity, const_cast<const std::uint8_t**>(frame_->extended_data),
                                frame_->nb_samples);
    if (got <= 0) return;
    std::lock_guard lock(audio_mutex);
    ring.insert(ring.end(), samples.begin(), samples.begin() + std::ptrdiff_t(got) * 2);
    // A stalled output (or none) must not hold the picture back: the wall clock
    // paces the video, and sound the device has not taken in time is dropped.
    constexpr std::size_t limit = std::size_t(audio_limit * output_rate) * 2;
    if (ring.size() > limit) ring.erase(ring.begin(), ring.begin() + std::ptrdiff_t(ring.size() - limit));
}

bool Player::decode(AVCodecContext* context, const AVPacket* packet) {
    if (avcodec_send_packet(context, packet) < 0) return false;
    while (avcodec_receive_frame(context, frame_) == 0) {
        if (context == video_) queue_video(); else if (audio_stream_ >= 0) queue_audio();
        av_frame_unref(frame_);
    }
    return true;
}

void Player::fill() {
    for (unsigned n = 0; !eof_ && n < packets_per_update; ++n) {
        const double audio = audio_stream_ >= 0 ? buffered_audio() : audio_ahead;
        if (frames_.size() >= video_ahead && audio >= audio_ahead) break;
        if (av_read_frame(format_, packet_) < 0) {
            eof_ = true;
            decode(video_, nullptr);
            if (audio_stream_ >= 0) decode(audio_, nullptr);
            break;
        }
        if (packet_->stream_index == video_stream_) decode(video_, packet_);
        else if (packet_->stream_index == audio_stream_) decode(audio_, packet_);
        av_packet_unref(packet_);
    }
}

std::optional<Frame> Player::present(double now) {
    std::optional<Frame> newest;
    while (!frames_.empty() && frames_.front().time <= now) {
        newest = Frame{std::move(frames_.front().rgba), width_, height_};
        frames_.pop_front();
        ++shown_;
    }
    return newest;
}

bool Player::done(double now) const {
    if (!eof_ || !frames_.empty()) return false;
    // The soundtrack may run a little past the last picture; never wait on a stalled device.
    return buffered_audio() == 0 || now > last_time_ + audio_limit;
}
}

bool start(const std::string& path) {
    stop();
    av_log_set_level(AV_LOG_ERROR);
    auto next = std::make_unique<Player>();
    if (!next->open(path)) {
        sc64::shell::log::warning("movie: cannot play %s", path.c_str());
        return false;
    }
    next->fill(); // prime pictures and sound before the clock starts
    player = std::move(next);
    started = Clock::now();
    std::lock_guard lock(audio_mutex);
    audio_active = true;
    return true;
}

bool playing() { return bool(player); }

static double elapsed() { return std::chrono::duration<double>(Clock::now() - started).count(); }

std::optional<Frame> update() {
    if (!player) return std::nullopt;
    player->fill();
    return player->present(elapsed());
}

bool finished() { return !player || player->done(elapsed()); }

void stop() {
    if (player)
        sc64::shell::log::info("movie: stopped at %.1f s of %.1f s, %u frames shown", elapsed(), player->length(),
                               player->shown());
    player.reset();
    std::lock_guard lock(audio_mutex);
    audio_active = false;
    ring.clear();
}

void mix(float* stereo, unsigned frames) {
    std::lock_guard lock(audio_mutex);
    if (!audio_active) return;
    const std::size_t count = std::min<std::size_t>(std::size_t(frames) * 2, ring.size() & ~std::size_t(1));
    for (std::size_t i = 0; i < count; ++i) {
        stereo[i] = std::clamp(stereo[i] + ring.front(), -1.0f, 1.0f);
        ring.pop_front();
    }
}
}
