#pragma once
// PC music from the user's own StarCraft CDs (tools/music/extract_music.py).
// The game streams each track from BOLT directory 015 as 11 kHz mono IMA ADPCM;
// its decoder asks guest_voice for every voice it mixes (apply_music_hook.py).
// A stream with a PC version is muted in the guest and the PC track (22 kHz
// stereo Ogg) plays instead, following the guest's track changes and volume.
// In missions the PC race playlist plays each theme to its end, as on PC.
#include <cstdint>
#include <string>

namespace sc64::music {
// <root> holds the .ogg files. Missing files, or <root>/../n64music, keep the N64 music.
void initialize(const std::string& root);
// Guest audio thread: rom is the voice's stream address (physical), left/right its
// 16.16 volumes. True when the shell plays this stream, so the guest mutes it.
bool guest_voice(std::uint32_t rom, std::int32_t left, std::int32_t right);
// Audio thread: adds the music to interleaved stereo float (48 kHz), scaled like
// the game's own PCM by game_gain.
void mix(float* stereo, unsigned frames, float game_gain);
void shutdown();
}
