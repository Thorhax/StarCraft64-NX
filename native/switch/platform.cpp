// Switch platform services: SD-card storage and an in-place ROM read.
#include "platform.hpp"
#include "mouse_cursor.hpp"
#include "pointer.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <switch.h>

namespace {
constexpr const char* storage_dir = "sdmc:/switch/StarCraft64";
constexpr const char* rom_path = "sdmc:/switch/StarCraft64/rom.z64";
}

bool sc64_choose_rom() { return false; } // no file picker: the ROM lives at rom_path
std::string sc64_storage_path() { return storage_dir; }
void sc64_platform_initialize(const std::string& storage) {
    std::error_code ignored;
    std::filesystem::create_directories(storage, ignored);
}
void sc64_platform_finalize() {}
Sc64RomSelection sc64_poll_rom() { return {}; }
std::vector<std::uint8_t> sc64_read_bundled_rom() {
    std::ifstream file(rom_path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Copy your StarCraft 64 (USA) z64 dump to sdmc:/switch/StarCraft64/rom.z64");
    std::vector<std::uint8_t> rom(std::size_t(file.tellg()));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(rom.data()), std::streamsize(rom.size())))
        throw std::runtime_error("ROM read failed: sdmc:/switch/StarCraft64/rom.z64");
    return rom;
}
void sc64_platform_vibrate(int, int) {}
SwitchPointer& switch_pointer() { static SwitchPointer pointer; return pointer; }
// The same rule as the SDL shell: hidden over the mission image, except the HUD and minimap.
bool sc64_update_mouse_cursor(bool requested, const Sc64CursorRegion& region,
                              const Sc64CursorRegion* visible_region, const Sc64CursorRegion* minimap_region) {
    auto& p = switch_pointer();
    const double x = p.x / 1280, y = 1 - p.y / 720;
    p.game_cursor = requested && region.contains(x, y) && !(visible_region && visible_region->contains(x, y))
                    && !(minimap_region && minimap_region->contains(x, y));
    return p.game_cursor;
}
void sc64_restore_mouse_cursor() { switch_pointer().game_cursor = false; }
// libnx threads: std::thread construction fails while the app starts (as in voices.cpp).
bool sc64_platform_thread(void (*entry)()) {
    static Thread threads[4];
    static unsigned used = 0;
    if (used == 4) return false;
    Thread& thread = threads[used];
    if (R_FAILED(threadCreate(&thread, [](void* arg) { reinterpret_cast<void (*)()>(arg)(); },
                              reinterpret_cast<void*>(entry), nullptr, 0x40000, 0x2C, -2))) return false;
    if (R_FAILED(threadStart(&thread))) { threadClose(&thread); return false; }
    ++used;
    return true;
}
// Core 3 when the launch grants it (4-core forwarder), else core 2 (the GL raster thread,
// mostly waiting on the GPU), instead of core 0, where the Gfx thread interprets and
// commits tasks below the main thread's priority.
void sc64_platform_heavy_begin() {
    static const bool core3 = R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, 1u << 3));
    if (!core3) svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1u << 2);
    else svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, 1u << 3);
}
void sc64_platform_heavy_end() { svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1u << 0); }
// Below the shell (0x2C) and graphics (0x2D) threads: it uses core 0's idle time only,
// so presentation keeps its pace; the guest copies whatever the helper has not taken.
double sc64_platform_thread_cpu_ms() {
    u64 ticks = 0;
    svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, -1); // this thread, all cores
    return double(armTicksToNs(ticks)) / 1e6;
}
void sc64_platform_helper_core() {
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1u << 0);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2E);
}
