#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <thread>

namespace sc64::runtime {
// Publish bytes at path only after the pending image is written, synchronized
// and read back byte-identical; otherwise the previous file is left untouched.
// before_verify is a test seam that runs after synchronization.
void write_verified_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                         const std::function<void(const std::filesystem::path&)>& before_verify = {});
// Synchronous FlashRAM HLE for the byte-indexed Macronix profile used by the
// Stage 0 reference. Commands/storage are modeled; hardware busy durations are
// intentionally completed before returning, so polling sees completed status.
class FlashDevice {
public:
    static constexpr std::size_t capacity = 128 * 1024;
    static constexpr std::size_t page_size = 128;
    static constexpr std::size_t sector_size = 16 * 1024;
    using Trace = std::function<void(const char*, std::uint32_t, std::uint32_t, std::size_t)>;
    void initialize(const std::filesystem::path& path, Trace trace = {});
    void command(std::uint32_t value);
    std::uint32_t read_pio(std::uint32_t offset);
    void write_pio(std::uint32_t offset, std::uint32_t value);
    void read_dma(std::uint32_t offset, std::span<std::uint8_t> output);
    void write_dma(std::uint32_t offset, std::span<const std::uint8_t> input);
    std::array<std::uint8_t, capacity> snapshot() const;
    // Deferred persistence: program/erase still complete at once in memory, and
    // a writer thread publishes the whole image once commands have been quiet
    // for `quiet`. One save programs hundreds of pages, each a full verified
    // file write that costs tens of milliseconds on a Switch SD card.
    void defer_writes(std::chrono::milliseconds quiet);
    // Publishes any deferred image now and stops the writer.
    void finish();
    ~FlashDevice();
private:
    enum class Mode { Array, Status, Silicon, Page };
    enum class Erase { None, Sector, Chip };
    void command_locked(std::uint32_t value);
    void read_locked(std::uint32_t offset, std::span<std::uint8_t> output);
    void write_locked(std::uint32_t offset, std::span<const std::uint8_t> input);
    void commit_locked(const std::array<std::uint8_t, capacity>& next);
    void require_initialized() const;
    void writer_loop();
    bool publish_pending(); // takes file_mutex_, then mutex_
    mutable std::mutex mutex_;
    std::array<std::uint8_t, capacity> data_{};
    std::array<std::uint8_t, page_size> page_{};
    std::filesystem::path path_;
    Trace trace_;
    Mode mode_ = Mode::Array;
    Erase erase_ = Erase::None;
    std::uint32_t sector_ = 0;
    bool pending_status_ = false;
    bool initialized_ = false;
    // Deferred persistence (defer_writes): generations of the in-memory image.
    std::mutex file_mutex_;
    std::condition_variable writer_wake_;
    std::thread writer_;
    std::chrono::milliseconds quiet_{0};
    std::uint64_t changed_ = 0, published_ = 0;
    bool deferred_ = false, stopping_ = false;
};
FlashDevice& flash_device();
}

extern "C" std::uint32_t sc64_flash_pio_read(std::uint32_t physical);
extern "C" void sc64_flash_pio_write(std::uint32_t physical, std::uint32_t value);
extern "C" void sc64_flash_dma(std::uint8_t* rdram, std::uint32_t guest,
    std::uint32_t physical, std::uint32_t size, std::uint32_t direction);
