#pragma once
// Full-screen movies from the user's own StarCraft PC files (Smacker .smk, or
// anything else FFmpeg decodes). The shell decodes on its main thread, paced by
// the wall clock, and the audio thread plays the soundtrack while the guest is
// suspended.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sc64::movie {
struct Frame { std::shared_ptr<const std::vector<std::uint8_t>> rgba; unsigned width = 0, height = 0; };
// Main thread. False (and logged) when the file cannot be played.
bool start(const std::string& path);
bool playing();
// Main thread: decodes up to the current playback time. Returns the newest frame
// when it changed; finished() turns true after the last frame and audio.
std::optional<Frame> update();
bool finished();
void stop();
// Audio thread: adds the soundtrack to interleaved stereo float (48 kHz).
void mix(float* stereo, unsigned frames);
}
