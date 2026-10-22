#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
namespace sc64::desktop::audio {
void trace(const std::filesystem::path& output, std::uint64_t from, std::uint64_t to);
void configure(const char* backend);
const char* backend();
struct Timing {
    double source_gap_ms, callback_gap_ms, callback_work_ms;
    std::uint64_t source_frames, source_nonzero_frames;
};
Timing take_timing();
void pause_changed();
// Gain of the game's own sound (1 = original); voices and movies keep their
// level, and peaks of the mix are soft-limited.
void set_game_gain(float gain);
void menu_profile(bool menu);
void initialize();
void shutdown();
void frequency(std::uint32_t rate);
void queue(const std::int16_t* samples, std::size_t count);
std::size_t remaining();
std::uint32_t status();
std::uint64_t underruns();
std::uint64_t dropped();
std::uint64_t played();
std::uint64_t nonzero();
std::uint64_t waits();
double buffered_ms();
bool rebuffering();
unsigned source_hz();
double dma_ms();
double target_ms();
int hardware_xruns();
int hardware_buffer_frames();
}
