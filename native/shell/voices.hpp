#pragma once
// Voiced briefings from the user's own StarCraft PC files (tools/voices).
// The N64 briefing script is read from RDRAM, matched to the PC campaign by
// its English text, and each PC transmission is played from its first N64
// page until the next transmission starts or the briefing ends.
#include <cstdint>
#include <string>

namespace sc64::voices {
// <root>/language selects the voice language (default enUS); <root>/enUS is the
// matching index. Missing files leave voices disabled. <root>/../novoices disables.
void initialize(const std::string& root);
// Guest thread: every briefing frame with active=true, once with false at exit.
void observe(const std::uint8_t* ram, bool active);
// Audio thread: adds the voice to interleaved stereo float (48 kHz) and ducks the rest.
void mix(float* stereo, unsigned frames);
}
