#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Platform services used by the shell main thread. Implementations:
// platform_sdl.cpp (all platforms) and platform_android.cpp (Android JNI/assets).
struct Sc64RomSelection {
    std::string path;
    std::string error;
};
// Starts an asynchronous ROM choice. The selected file is copied into an owned
// `sc64-import-*.tmp` file in storage; its result arrives through
// sc64_poll_rom(). Returns false when a choice is already in progress.
bool sc64_choose_rom();
std::string sc64_storage_path();
void sc64_platform_initialize(const std::string& storage);
void sc64_platform_finalize();
Sc64RomSelection sc64_poll_rom();
std::vector<std::uint8_t> sc64_read_bundled_rom();
// Vibrate for milliseconds at amplitude 1..255; milliseconds <= 0 stops.
// Hosts without a vibrator ignore it.
void sc64_platform_vibrate(int milliseconds, int amplitude);
// Starts a detached background worker (asset loading). False when no thread could be made.
bool sc64_platform_thread(void (*entry)());
// Brackets heavy main-thread work (HD composition): on Switch the main thread moves off
// core 0, which it shares with the graphics thread, for the duration. No-op elsewhere.
void sc64_platform_heavy_begin();
void sc64_platform_heavy_end();
// Called by a helper the guest thread waits on (the snapshot copy): on Switch, core 0
// below the shell and graphics threads (idle time only). No-op elsewhere.
void sc64_platform_helper_core();
