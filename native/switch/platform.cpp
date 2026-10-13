// Switch platform services: SD-card storage and an in-place ROM read.
#include "platform.hpp"
#include "mouse_cursor.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>

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
bool sc64_update_mouse_cursor(bool, const Sc64CursorRegion&, const Sc64CursorRegion*, const Sc64CursorRegion*) {
    return false; // no mouse pointer on Switch
}
void sc64_restore_mouse_cursor() {}
