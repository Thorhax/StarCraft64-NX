#include "flash_device.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sc64::runtime {
namespace {
bool matches_file(const std::filesystem::path& path, std::span<const std::uint8_t> expected) {
    std::ifstream input(path, std::ios::binary);
    std::vector<char> actual(expected.size() + 1);
    input.read(actual.data(), static_cast<std::streamsize>(actual.size()));
    return static_cast<std::size_t>(input.gcount()) == expected.size() &&
           std::equal(expected.begin(), expected.end(), actual.begin(),
                      [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); });
}
#ifndef _WIN32
// fsync(2) on Apple platforms only reaches the drive; F_FULLFSYNC also flushes
// the drive's write cache, which the "synchronized" publish contract needs.
int durable_sync(int descriptor) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (::fcntl(descriptor, F_FULLFSYNC) == 0) return 0;
#endif
    return ::fsync(descriptor);
}
#endif
constexpr std::uint32_t ready_status = 0x008c;
constexpr std::array<std::uint8_t, 8> silicon_id{0x11, 0x11, 0x80, 0x01, 0x00, 0xc2, 0x00, 0x1d};
// Publish only after the complete image has been written, synchronized and
// verified. A failed write/verify/rename leaves the previous valid save and
// in-memory state intact.
void atomic_write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                  const std::function<void(const std::filesystem::path&)>& before_verify) {
    const auto temporary = std::filesystem::path(path.native() + std::filesystem::path(".pending").native());
#ifdef _WIN32
    FILE* stream = _wfopen(temporary.c_str(), L"wb");
#else
    FILE* stream = std::fopen(temporary.c_str(), "wb");
#endif
    if (!stream) throw std::system_error(errno, std::generic_category(), "open pending FlashRAM save");
    bool good = std::fwrite(bytes.data(), 1, bytes.size(), stream) == bytes.size();
    good = std::fflush(stream) == 0 && good;
#ifdef _WIN32
    good = _commit(_fileno(stream)) == 0 && good;
#else
    good = durable_sync(::fileno(stream)) == 0 && good;
#endif
    good = std::fclose(stream) == 0 && good;
    if (!good) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("cannot write complete FlashRAM save");
    }
    if (before_verify) before_verify(temporary);
    // Read back the synchronized image. This catches short or corrupted writes
    // the C library did not report; it may be served by the OS page cache.
    if (!matches_file(temporary, bytes)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("FlashRAM save verification failed");
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(GetLastError(), std::system_category(), "publish FlashRAM save");
    }
#else
#if defined(__SWITCH__)
    // libnx's SD-card rename does not replace an existing file; drop the old
    // image first (the verified temporary still holds the new one until renamed).
    std::error_code replace_error;
    std::filesystem::remove(path, replace_error);
#endif
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "publish FlashRAM save");
    }
#endif
}
// POSIX persists a rename only once its directory entry is synchronized.
// MoveFileEx(MOVEFILE_WRITE_THROUGH) already covers this on Windows.
void sync_parent_directory([[maybe_unused]] const std::filesystem::path& path) {
    // Switch: directories cannot be open()ed on libnx fsdev; the file itself was synced.
#if !defined(_WIN32) && !defined(__SWITCH__)
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) throw std::system_error(errno, std::generic_category(), "open FlashRAM save directory");
    const int synced = durable_sync(directory);
    const int sync_error = errno;
    ::close(directory);
    if (synced != 0) throw std::system_error(sync_error, std::generic_category(), "sync FlashRAM save directory");
#endif
}
}

void write_verified_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                         const std::function<void(const std::filesystem::path&)>& before_verify) {
    atomic_write(path, bytes, before_verify);
    sync_parent_directory(path);
}

void FlashDevice::require_initialized() const {
    if (!initialized_) throw std::logic_error("FlashRAM must be initialized before game startup");
}

void FlashDevice::initialize(const std::filesystem::path& path, Trace trace) {
    std::lock_guard lock(mutex_);
    if (path.empty()) throw std::invalid_argument("FlashRAM save path is empty");
    std::array<std::uint8_t, capacity> next;
    next.fill(0xff);
    if (std::filesystem::exists(path)) {
        if (std::filesystem::file_size(path) != capacity) throw std::runtime_error("FlashRAM save must contain exactly 131072 bytes");
        std::ifstream input(path, std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(next.data()), next.size())) throw std::runtime_error("cannot read complete FlashRAM save");
    }
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    data_ = next;
    page_.fill(0xff);
    path_ = path;
    trace_ = std::move(trace);
    mode_ = Mode::Array;
    erase_ = Erase::None;
    pending_status_ = false;
    initialized_ = true;
}

void FlashDevice::commit_locked(const std::array<std::uint8_t, capacity>& next) {
    atomic_write(path_, next, {});
    // The complete image is published; keep memory equal to it even if the
    // directory sync then reports that its durability is unconfirmed.
    data_ = next;
    sync_parent_directory(path_);
}

void FlashDevice::command_locked(std::uint32_t value) {
    const auto opcode = value >> 24;
    if (trace_) trace_("flash_command", 0x08010000, value, 4);
    // The reference Macronix device changes into Status after two consecutive
    // D2 commands. Preserve that latch while completing program/erase eagerly.
    if (opcode == 0xd2 && !pending_status_) { pending_status_ = true; return; }
    pending_status_ = false;
    switch (opcode) {
    case 0x3c: erase_ = Erase::Chip; break;
    case 0x4b: erase_ = Erase::Sector; sector_ = (value & 0x3ff) >> 7; break;
    case 0x78:
        if (erase_ != Erase::None) {
            auto next = data_;
            if (erase_ == Erase::Chip) next.fill(0xff);
            else std::fill_n(next.begin() + sector_ * sector_size, sector_size, 0xff);
            commit_locked(next);
            erase_ = Erase::None;
            mode_ = Mode::Status;
        }
        break;
    case 0xa5: {
        auto next = data_;
        const auto base = (value & 0x3ff) * page_size;
        for (std::size_t index = 0; index < page_size; ++index) next[base + index] &= page_[index];
        commit_locked(next);
        page_.fill(0xff);
        mode_ = Mode::Status;
        break;
    }
    case 0xb4: mode_ = Mode::Page; break;
    case 0xd2: mode_ = Mode::Status; break;
    case 0xe1: mode_ = Mode::Silicon; break;
    case 0xf0: mode_ = Mode::Array; break;
    default: throw std::runtime_error("unsupported FlashRAM command: " + std::to_string(opcode));
    }
}

void FlashDevice::command(std::uint32_t value) {
    std::lock_guard lock(mutex_);
    require_initialized();
    command_locked(value);
}

void FlashDevice::read_locked(std::uint32_t offset, std::span<std::uint8_t> output) {
    // Array PI bursts wrap in a 32KiB window while preserving the upper address
    // bits, as the byte-indexed Stage 0 device does. Silicon IDs start at ID[0]
    // for each burst, independently of the device address.
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto address = (offset & ~0x7fffu) | ((offset + static_cast<std::uint32_t>(index)) & 0x7fffu);
        switch (mode_) {
        case Mode::Array:
            output[index] = address < capacity ? data_[address] : 0;
            break;
        case Mode::Page: output[index] = page_[(offset + index) & 0x7f]; break;
        case Mode::Silicon: output[index] = silicon_id[index & 7]; break;
        case Mode::Status:
            output[index] = (offset & 0x20000) ? 0xff : ((index & 1) ? ready_status : 0);
            break;
        }
    }
}

void FlashDevice::write_locked(std::uint32_t offset, std::span<const std::uint8_t> input) {
    if (mode_ == Mode::Page) {
        for (std::size_t index = 0; index < input.size(); ++index) page_[(offset + index) & 0x7f] = input[index];
    } else if (mode_ != Mode::Status) {
        throw std::runtime_error("FlashRAM DMA write requires LoadBytePage mode");
    }
}

std::uint32_t FlashDevice::read_pio(std::uint32_t offset) {
    if (offset & 3) throw std::invalid_argument("unaligned FlashRAM PIO read");
    std::lock_guard lock(mutex_);
    require_initialized();
    std::array<std::uint8_t, 4> bytes;
    read_locked(offset, bytes);
    const auto value = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
    if (trace_) trace_("flash_pio_read", 0x08000000 + offset, value, 4);
    return value;
}

void FlashDevice::write_pio(std::uint32_t offset, std::uint32_t value) {
    if (offset & 3) throw std::invalid_argument("unaligned FlashRAM PIO write");
    std::lock_guard lock(mutex_);
    require_initialized();
    if (offset == 0x10000) command_locked(value);
    else {
        const std::array<std::uint8_t, 4> bytes{std::uint8_t(value >> 24), std::uint8_t(value >> 16), std::uint8_t(value >> 8), std::uint8_t(value)};
        write_locked(offset, bytes);
    }
}

void FlashDevice::read_dma(std::uint32_t offset, std::span<std::uint8_t> output) {
    std::lock_guard lock(mutex_);
    require_initialized();
    read_locked(offset, output);
    if (trace_) trace_("flash_dma_read", 0x08000000 + offset, 0, output.size());
}

void FlashDevice::write_dma(std::uint32_t offset, std::span<const std::uint8_t> input) {
    std::lock_guard lock(mutex_);
    require_initialized();
    write_locked(offset, input);
    if (trace_) trace_("flash_dma_write", 0x08000000 + offset, 0, input.size());
}

std::array<std::uint8_t, FlashDevice::capacity> FlashDevice::snapshot() const {
    std::lock_guard lock(mutex_);
    require_initialized();
    return data_;
}

FlashDevice& flash_device() { static FlashDevice instance; return instance; }
}

extern "C" std::uint32_t sc64_flash_pio_read(std::uint32_t physical) {
    if (physical < 0x08000000 || physical >= 0x10000000) throw std::out_of_range("FlashRAM physical address");
    return sc64::runtime::flash_device().read_pio(physical - 0x08000000);
}
extern "C" void sc64_flash_pio_write(std::uint32_t physical, std::uint32_t value) {
    if (physical < 0x08000000 || physical >= 0x10000000) throw std::out_of_range("FlashRAM physical address");
    sc64::runtime::flash_device().write_pio(physical - 0x08000000, value);
}
extern "C" void sc64_flash_dma(std::uint8_t* rdram, std::uint32_t guest,
    std::uint32_t physical, std::uint32_t size, std::uint32_t direction) {
    if (physical < 0x08000000 || physical >= 0x10000000) throw std::out_of_range("FlashRAM DMA physical address");
    const auto address = guest & 0x1fffffff;
    if (address > 0x800000 || size > 0x800000 - address || direction > 1) throw std::out_of_range("FlashRAM DMA guest range/direction");
    std::vector<std::uint8_t> bytes(size);
    if (direction == 0) {
        sc64::runtime::flash_device().read_dma(physical - 0x08000000, bytes);
        for (std::size_t index = 0; index < size; ++index) rdram[(address + index) ^ 3] = bytes[index];
    } else {
        for (std::size_t index = 0; index < size; ++index) bytes[index] = rdram[(address + index) ^ 3];
        sc64::runtime::flash_device().write_dma(physical - 0x08000000, bytes);
    }
}
